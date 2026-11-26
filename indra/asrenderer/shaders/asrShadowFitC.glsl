/**
 * asrShadowFitC.glsl
 * Places the sun's or moon's cascades for this frame and marks what their static cache must
 * redraw (doc/ayanestorm-new-renderer-shadows.md, The design, items 1 and 2). One workgroup:
 * first a thread per cascade places it, then a thread per cascade and tile row marks its
 * dirty tiles (GpuShadowViews::dirty).
 *
 * Reach: twice the farthest depth on screen last frame, as OpenGL's, read from the depth
 * pyramid's last 1 by 1 level, at most view.shadowDistance, which it reaches when the sky
 * shows. Each cascade is a box around a sphere centred on the camera: a fixed area whichever
 * way the camera looks. Its radius moves in quarter octave steps and keeps its step until the
 * reach leaves it. The window sits on whole texels of a grid anchored in the world across the
 * light, and depth is measured from a world origin along the light stepped by a quarter of
 * the radius, so the cache only scrolls. The light is the one the cache was drawn for, moved
 * when the real one leaves it by more than view.shadowLightStep.
 *
 * Dirty: the whole cascade on a new radius, light, depth origin, LOD scale or a first frame;
 * otherwise the strips that scrolled into view and the footprints of the world spheres the
 * store reported (a negative radius: everything).
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "asrSceneCommon.glsl"

layout(set = 0, binding = 1) uniform sampler samplers[4];
#include "asrAtmosphere.glsl"

layout(local_size_x = 128) in;

layout(buffer_reference, std430) readonly buffer PyramidRef { float values[]; };

const float SPLIT_LOG_SHARE = 0.8;
const float QUARTER_OCTAVE = 1.18920712; // 2^(1/4)

shared vec3 sLight;
shared bool sLightMoved;
shared uint sLightKind;
shared vec3 sU[SHADOW_CASCADES];
shared vec3 sV[SHADOW_CASCADES];
shared vec2 sOffset[SHADOW_CASCADES]; // the camera's place in its window, metres from the window's centre
shared float sTexel[SHADOW_CASCADES];
shared ivec2 sShift[SHADOW_CASCADES];
shared bool sFull[SHADOW_CASCADES];

float splitAt(uint k, uint cascades, float near, float far)
{
    const float t = float(k) / float(cascades);
    return mix(near + (far - near) * t, near * pow(far / near, t), SPLIT_LOG_SHARE);
}

void main()
{
    ViewRef view = ViewRef(push.view);
    ShadowViewsRef shadows = ShadowViewsRef(view.shadowViews);
    const uint cascades = min(view.shadowCascades, SHADOW_CASCADES);
    const uint t = gl_LocalInvocationID.x;
    const float size = view.shadowResolution;

    const float near = view.projection.z;
    float far = view.shadowDistance;
    if (view.previousProjection.w != 0.0 && view.pyramidLevels != 0u)
    {
        const float farthest = PyramidRef(push.pyramid).values[view.pyramidOffset[view.pyramidLevels - 1u]];
        if (farthest > 0.0)
        {
            // As OpenGL: twice the farthest depth seen, from 16 m. Reversed depth is near / distance.
            far = min(far, max(2.0 * view.previousProjection.z / farthest, 16.0));
        }
    }
    far = max(far, near * 4.0);

    if (t == 0u)
    {
        const Atmosphere a = loadAtmosphere(view.atmosphere);
        const uint light = cascades == 0u ? SHADOW_LIGHT_NONE
                         : a.sunDirection.z > 0.0 ? SHADOW_LIGHT_SUN : a.moonDirection.z > 0.0 ? SHADOW_LIGHT_MOON : SHADOW_LIGHT_NONE;
        const vec3 real = normalize(light == SHADOW_LIGHT_MOON ? a.moonDirection : a.sunDirection);
        const vec4 cached = shadows.cachedLight;
        const bool moved = uint(cached.w) != light || dot(real, cached.xyz) < view.shadowLightStep;
        if (moved)
        {
            shadows.cachedLight = vec4(real, float(light));
        }
        sLight = moved ? real : cached.xyz;
        sLightMoved = moved;
        sLightKind = light;
        shadows.count = light == SHADOW_LIGHT_NONE ? 0u : cascades;
        shadows.light = light;
    }
    barrier();
    const bool lit = sLightKind != SHADOW_LIGHT_NONE;

    if (lit && t < cascades)
    {
        const ShadowView old = shadows.views[t];
        const float needed = splitAt(t + 1u, cascades, near, far);
        const float oldRadius = old.lightDirection.w * size * 0.5;
        const bool keep = old.valid != 0u && !sLightMoved && oldRadius >= needed && oldRadius <= needed * QUARTER_OCTAVE * QUARTER_OCTAVE;
        const float radius = keep ? oldRadius : exp2(ceil(log2(needed) * 4.0) * 0.25);
        const float texel = 2.0 * radius / size;

        const vec3 toLight = sLight;
        const vec3 helper = abs(toLight.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
        const vec3 u = normalize(cross(helper, toLight));
        const vec3 v = cross(toLight, u);
        const vec3 camera = view.camera.xyz;
        const ivec2 origin = ivec2(floor(vec2(dot(camera, u), dot(camera, v)) / texel + 0.5));
        const vec2 offset = vec2(dot(camera, u), dot(camera, v)) - vec2(origin) * texel; // under half a texel
        const float depthStep = radius * 0.25;
        const float depthOrigin = floor(dot(camera, toLight) / depthStep + 0.5) * depthStep;
        const float offsetL = dot(camera, toLight) - depthOrigin;
        const float depthHalf = radius * 1.5; // the sphere, wherever the camera is in its depth step

        const ivec2 shift = origin - old.origin;
        const bool full = !keep || old.valid == 0u || depthOrigin != old.depthOrigin || old.lodScale != view.projection.w ||
                          abs(shift.x) >= int(size) || abs(shift.y) >= int(size);

        // Camera-relative: x = (dot(p, u) + offset.x) / radius, and depth 1 towards the light.
        const float s = 1.0 / radius;
        const float d = 0.5 / depthHalf;
        ShadowView shadow;
        shadow.viewProjection = mat4(vec4(u.x * s, v.x * s, toLight.x * d, 0.0),
                                     vec4(u.y * s, v.y * s, toLight.y * d, 0.0),
                                     vec4(u.z * s, v.z * s, toLight.z * d, 0.0),
                                     vec4(offset.x * s, offset.y * s, 0.5 + offsetL * d, 1.0));
        shadow.planes[0] = vec4(u, radius + offset.x);
        shadow.planes[1] = vec4(-u, radius - offset.x);
        shadow.planes[2] = vec4(v, radius + offset.y);
        shadow.planes[3] = vec4(-v, radius - offset.y);
        shadow.planes[4] = vec4(toLight, depthHalf + offsetL); // the side away from the light
        shadow.planes[5] = vec4(0.0, 0.0, 0.0, 1.0);
        shadow.lightDirection = vec4(toLight, texel);
        shadow.depthRange = 2.0 * depthHalf;
        shadow.layer = t;
        shadow.origin = origin;
        shadow.previousOrigin = full ? origin : old.origin;
        shadow.depthOrigin = depthOrigin;
        shadow.lodScale = view.projection.w;
        shadow.valid = 1u;
        shadow.pad[0] = 0.0;
        shadow.pad[1] = 0.0;
        shadow.pad[2] = 0.0;
        shadows.views[t] = shadow;
        if (t == cascades - 1u)
        {
            shadows.splits = vec4(radius); // the reach, read by the fade; inner splits are the radii below
        }

        sU[t] = u;
        sV[t] = v;
        sOffset[t] = offset;
        sTexel[t] = texel;
        sShift[t] = full ? ivec2(0) : shift;
        sFull[t] = full;
    }
    barrier();

    if (!lit || t >= cascades * SHADOW_TILES)
    {
        return;
    }
    const uint c = t / SHADOW_TILES;
    const uint row = t % SHADOW_TILES;
    const float tileTexels = size / float(SHADOW_TILES);
    uint mask = 0u;
    if (sFull[c])
    {
        mask = 0xFFFFFFFFu;
    }
    else
    {
        // Strips: texels whose world texel was outside last frame's window.
        const ivec2 shift = sShift[c];
        const float rowLow = float(row) * tileTexels;
        const float rowHigh = rowLow + tileTexels;
        const bool rowNew = (shift.y > 0 && rowHigh > size - float(shift.y)) || (shift.y < 0 && rowLow < float(-shift.y));
        mask = rowNew ? 0xFFFFFFFFu
             : shift.x > 0 ? shadowTileBits(size - float(shift.x), size, tileTexels)
             : shift.x < 0 ? shadowTileBits(0.0, float(-shift.x), tileTexels) : 0u;

        // The store's spheres: what was added, removed, changed, or started or stopped moving.
        SpheresRef spheres = SpheresRef(view.shadowDirty);
        const vec3 u = sU[c];
        const vec3 v = sV[c];
        const float texel = sTexel[c];
        const vec2 offset = sOffset[c];
        for (uint i = 0u; i < view.shadowDirtyCount && mask != 0xFFFFFFFFu; ++i)
        {
            const vec4 sphere = spheres.items[i];
            if (sphere.w < 0.0)
            {
                mask = 0xFFFFFFFFu;
                break;
            }
            const vec3 relative = sphere.xyz - view.camera.xyz;
            const vec2 at = (vec2(dot(relative, u), dot(relative, v)) + offset) / texel + size * 0.5;
            const float reach = sphere.w / texel;
            if (at.y + reach > rowLow && at.y - reach < rowHigh)
            {
                mask |= shadowTileBits(at.x - reach, at.x + reach, tileTexels);
            }
        }
    }
    shadows.dirty[c * SHADOW_TILES + row] = mask;
}
