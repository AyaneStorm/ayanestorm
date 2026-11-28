/**
 * asrContactShadowsC.glsl
 * Contact shadows into the sun shadow mask: for each pixel, whether something in the final
 * depth hides it from the shadowing light within contactReach(), at screen resolution: the gap
 * the maps' bias leaves above every receiver (asrShadow.glsl), where small things on a table
 * lose their shadow's foot. Farther blockers are the maps' alone, with PCSS edges: a fixed
 * 6 cm reach (once the hair push) shadowed faces in a high sun with hard patches under the
 * brows and nose (user's capture, 27 November 2026). Hair casts through the opacity maps.
 * Only what is in the depth casts: blended surfaces (Mac OIT) do not.
 *
 * One group follows one ray across one band (asrContactShadows.glsl). The ray's own depth
 * changes linearly in 1 / distance along its line on screen, so each pixel needs only where
 * its reach's end lands: its rows until there are compared, at most CONTACT_SAMPLES.
 * 1: lit, 0: shadowed. Pixels beyond CONTACT_RANGE and the sky are 1.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#include "asrSceneCommon.glsl"
#include "asrContactShadows.glsl"

layout(local_size_x = 128) in; // CONTACT_GROUP

layout(set = 1, binding = 1, r8) uniform writeonly image2D contactMask;
layout(set = 1, binding = 3) uniform texture2D depthImage;

// 1 / distance along the view of each row the group reads: [0] the row before thread 0's,
// [1 + i] row i. 0: the sky or off screen, nothing to compare. Inverse, as the reversed depth
// stores it (near / distance), so the samples need no division.
shared float sharedInverse[2u * CONTACT_GROUP + 1u];
shared uint sharedNear; // not 0: some pixel of the group is within CONTACT_RANGE

float inverseAt(ViewRef view, ivec2 pixel)
{
    if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, ivec2(view.pixelSize.xy))))
    {
        return 0.0;
    }
    return max(texelFetch(depthImage, pixel, 0).r, 0.0) / view.projection.z;
}

float distanceOf(float inverse)
{
    return inverse > 0.0 ? 1.0 / inverse : 0.0;
}

// The smaller step of the depth to either neighbour: a slope that ignores an edge on one side.
float smallerStep(float before, float here, float after)
{
    const float a = before > 0.0 ? abs(here - before) : 1e9;
    const float b = after > 0.0 ? abs(after - here) : 1e9;
    const float s = min(a, b);
    return s < 1e9 ? s : 0.0;
}

void main()
{
    ViewRef view = ViewRef(push.view);
    ContactShadowsRef work = ContactShadowsRef(view.contactShadows);
    // A row of the dispatch per band, a group per ray.
    const ContactEntry entry = work.entries[gl_WorkGroupID.y];
    if (gl_WorkGroupID.x >= entry.rayCount)
    {
        return; // the whole group
    }
    const int ray = entry.firstRay + int(gl_WorkGroupID.x);
    const int outer = entry.firstRow + int(CONTACT_GROUP) - 1;
    const ivec2 lightPixel = work.lightPixel;
    const bool reverse = work.reverse != 0u;
    // Index i of the shared rows: towards the light as i grows. Each thread's own row first.
    const int t = int(gl_LocalInvocationID.x);
    const int row = reverse ? entry.firstRow + t : outer - t;
    const int across = contactAcross(ray, row, outer);
    const ivec2 pixel = contactPixel(lightPixel, entry.sector, row, across);
    if (t == 0)
    {
        sharedNear = 0u;
    }
    const float ownInverse = inverseAt(view, pixel);
    sharedInverse[t + 1] = ownInverse;
    barrier();
    if (contactInRange(ownInverse))
    {
        atomicOr(sharedNear, 1u);
    }
    barrier();
    if (sharedNear == 0u)
    {
        return; // the whole group is sky or past the range (most of a landscape): nothing to read or write
    }
    // Then the rows after the group's, towards the light; thread 0 also the row before its own.
    for (int i = t == 0 ? -1 : t + int(CONTACT_GROUP); i < 2 * int(CONTACT_GROUP); i += i < 0 ? int(CONTACT_GROUP) + 1 : int(CONTACT_GROUP))
    {
        const int at = reverse ? entry.firstRow + i : outer - i;
        sharedInverse[i + 1] = at >= 1 ? inverseAt(view, contactPixel(lightPixel, entry.sector, at, contactAcross(ray, at, outer))) : 0.0;
    }
    barrier();

    // Pixels past the range are not written: the resolve reads the mask only within it
    // (contactInRange()), from the same depth texel, so it never reads one left out. Only
    // the ray owning a pixel writes it; horizontal sectors leave the diagonals to the vertical ones.
    if (!contactInRange(ownInverse) || !(entry.sector < 2u ? abs(across) < row : abs(across) <= row) ||
        contactOwner(across, row, outer) != ray)
    {
        return;
    }
    const float here = 1.0 / ownInverse;

    // The point and its reach's end, camera-relative.
    const vec2 ndc = (vec2(pixel) + 0.5) * view.pixelSize.zw * 2.0 - 1.0;
    const vec3 eye = vec3(ndc.x * here / view.projection.x, -ndc.y * here / view.projection.y, -here);
    const vec3 relative = eye.x * view.viewRows[0].xyz + eye.y * view.viewRows[1].xyz + eye.z * view.viewRows[2].xyz;
    ExposureRef e = ExposureRef(view.exposure);
    const vec3 toLight = work.light == SHADOW_LIGHT_SUN ? e.sunDirection : e.moonDirection;
    // The bias of the cascade the resolve reads here (asr_shadow_sun(): the first holding the
    // point, and in its edge band the next one too), as it moves a receiver (SHADOW_NORMAL_OFFSET
    // texels along its normal, SHADOW_DEPTH_BIAS in depth), and a texel more for its filter.
    ShadowViewsRef shadows = ShadowViewsRef(view.shadowViews);
    float texel = CONTACT_REACH_MAX / CONTACT_BIAS_TEXELS;
    for (uint i = 0u; i < shadows.count; ++i)
    {
        const vec4 clip = shadows.views[i].viewProjection * vec4(relative, 1.0);
        const float edge = 1.0 - max(abs(clip.x), abs(clip.y)); // share of the half width left
        if (edge >= 0.0 && clip.z >= 0.0)
        {
            const bool blended = edge * 0.5 < SHADOW_BLEND && i + 1u < shadows.count;
            texel = shadows.views[blended ? i + 1u : i].lightDirection.w;
            break;
        }
    }
    float reach = min(CONTACT_BIAS_TEXELS * texel, CONTACT_REACH_MAX);
    const float endAlong = here - reach * dot(toLight, view.viewRows[2].xyz);
    // A ray towards the camera stops halfway to it.
    if (endAlong < 0.5 * here)
    {
        reach *= 0.5 * here / (here - endAlong);
    }
    const vec3 end = relative + toLight * reach;
    const vec3 endEye = vec3(dot(end, view.viewRows[0].xyz), dot(end, view.viewRows[1].xyz), dot(end, view.viewRows[2].xyz));
    const float endDistance = -endEye.z;
    const vec2 endPixel = (vec2(endEye.x * view.projection.x, -endEye.y * view.projection.y) / endDistance * 0.5 + 0.5) * view.pixelSize.xy;
    const uint axis = entry.sector < 2u ? 0u : 1u;
    const float endRows = abs(endPixel[axis] - (float(pixel[axis]) + 0.5));
    const int samples = min(CONTACT_SAMPLES, int(endRows));
    if (samples < 2)
    {
        imageStore(contactMask, pixel, vec4(1.0));
        return;
    }
    const float stepInverse = (1.0 / endDistance - ownInverse) / endRows;

    // How much the receiver's own depth changes over a pixel and a half, along the ray and
    // across it (a sample may be half a pixel off the line): seen at a grazing angle the
    // ground steps by centimetres between rows, and a fixed tolerance found the ground
    // itself, in stripes (user's capture, 26 November 2026).
    const ivec2 side = entry.sector < 2u ? ivec2(0, 1) : ivec2(1, 0);
    const float alongStep = smallerStep(distanceOf(sharedInverse[t]), here, distanceOf(sharedInverse[t + 2]));
    const float acrossStep = smallerStep(distanceOf(inverseAt(view, pixel - side)), here, distanceOf(inverseAt(view, pixel + side)));
    const float tolerance = max(0.002, 1.5 * max(alongStep, acrossStep));

    float lit = 1.0;
    // Row 1 is the receiver's neighbour: looking towards the light every step lands there,
    // and the surface hid itself in a pattern seen only at that angle (user, 26 November 2026).
    // With a the ray's inverse distance and b the sample's, the ray is behind the sample by
    // 1/a - 1/b = (b - a) / (a b): compared without dividing.
    for (int k = 2; k <= samples; ++k)
    {
        const float b = sharedInverse[t + 1 + k];
        if (b <= 0.0)
        {
            continue;
        }
        const float a = ownInverse + stepInverse * float(k);
        const float gap = b - a;
        const float scale = a * b;
        if (gap > tolerance * scale && gap < CONTACT_THICKNESS * scale)
        {
            lit = 0.0;
            break;
        }
    }
    imageStore(contactMask, pixel, vec4(lit));
}
