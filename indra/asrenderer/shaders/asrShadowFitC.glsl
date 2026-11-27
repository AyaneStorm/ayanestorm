/**
 * asrShadowFitC.glsl
 * Places the sun's or moon's cascades for this frame and marks what their static cache must
 * redraw (doc/ayanestorm-new-renderer-shadows.md, The design, items 1 and 2). One workgroup:
 * first a thread per cascade places it, then a thread per cascade and tile row marks its
 * dirty tiles (GpuShadowViews::dirty).
 *
 * Reach: view.shadowDistance, the draw distance up to 512 m, whatever the screen shows, so
 * the cascades never change size as the camera turns. Each cascade is a box around a sphere centred on the camera: a fixed area whichever
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


const float SPLIT_LOG_SHARE = 1.0;
const float SHADOW_NEAREST = 1.0; // metres where the logarithmic splits start
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

vec3 uAcross(vec3 toLight)
{
    const vec3 helper = abs(toLight.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    return normalize(cross(helper, toLight));
}

// The matrix and planes of a window at shadow.origin and shadow.depthOrigin, for readers
// relative to this frame's camera. Returns the camera's place in the window across the light.
vec2 placeView(inout ShadowView shadow, vec3 u, vec3 v, vec3 toLight, float radius, vec3 camera)
{
    const float texel = 2.0 * radius / ViewRef(push.view).shadowResolution;
    const vec2 offset = vec2(dot(camera, u), dot(camera, v)) - vec2(shadow.origin) * texel;
    const float offsetL = dot(camera, toLight) - shadow.depthOrigin;
    const float depthHalf = radius * 1.5; // the sphere, wherever the camera is in its depth step

    // Camera-relative: x = (dot(p, u) + offset.x) / radius, and depth 1 towards the light.
    const float s = 1.0 / radius;
    const float d = 0.5 / depthHalf;
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
    return offset;
}

void main()
{
    ViewRef view = ViewRef(push.view);
    ShadowViewsRef shadows = ShadowViewsRef(view.shadowViews);
    // Every frame: the receiver tiles start empty (asrShadowReceiversC.glsl marks them).
    for (uint word = gl_LocalInvocationID.x; word < MAX_SHADOW_VIEWS * SHADOW_TILES; word += gl_WorkGroupSize.x)
    {
        shadows.receivers[word] = 0u;
    }
    if (gl_LocalInvocationID.x == 0u)
    {
        shadows.deep = 0u;
    }
    if (view.shadowUpdate == 0u)
    {
        // Between updates (ASRendererShadowInterval) the windows stay where the maps were drawn,
        // re-placed for this frame's camera: every reader is camera-relative, and views kept
        // as they were slid the cached shadows by the camera's motion until the next update.
        const uint k = gl_LocalInvocationID.x;
        if (k < shadows.count && shadows.views[k].valid != 0u)
        {
            ShadowView shadow = shadows.views[k];
            const vec3 toLight = shadow.lightDirection.xyz;
            const vec3 u = uAcross(toLight);
            placeView(shadow, u, cross(toLight, u), toLight, shadow.lightDirection.w * view.shadowResolution * 0.5, view.camera.xyz);
            shadows.views[k] = shadow;
        }
        if (k == 0u)
        {
            shadows.keep = 0xFu; // asrShadowDemandC.glsl: nothing drawn whole
        }
        return;
    }
    const uint cascades = min(view.shadowCascades, SHADOW_CASCADES);
    const uint t = gl_LocalInvocationID.x;
    const float size = view.shadowResolution;

    // A fixed reach: sized from what the screen shows, the cascades jumped as the camera turned
    // or a head moved, and self-shadowing flickered with them (user's capture, 26 November 2026).
    // Logarithmic from 1 m: 4.8, 23, 108 and 512 m at the full reach, so the nearest cascade
    // has texels of a few millimetres (an even share left it 26 m wide, and small moving
    // things such as earrings shadowed themselves in a pattern that flickered).
    const float near = SHADOW_NEAREST;
    const float far = max(view.shadowDistance, near * 4.0);

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
        const vec3 u = uAcross(toLight);
        const vec3 v = cross(toLight, u);
        const vec3 camera = view.camera.xyz;
        const ivec2 origin = ivec2(floor(vec2(dot(camera, u), dot(camera, v)) / texel + 0.5));
        const float depthStep = radius * 0.25;
        const float depthOrigin = floor(dot(camera, toLight) / depthStep + 0.5) * depthStep;

        const ivec2 shift = origin - old.origin;
        const bool full = !keep || old.valid == 0u || depthOrigin != old.depthOrigin || old.lodScale != view.projection.w ||
                          abs(shift.x) >= int(size) || abs(shift.y) >= int(size);

        ShadowView shadow;
        shadow.origin = origin;
        shadow.depthOrigin = depthOrigin;
        const vec2 offset = placeView(shadow, u, v, toLight, radius, camera); // under half a texel
        shadow.layer = t;
        shadow.previousOrigin = full ? origin : old.origin;
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

    // A cascade drawn whole, or gone, holds none of what it held: its demand starts again
    // (asrShadowDemandC.glsl).
    if (t == 0u)
    {
        uint keep = 0u;
        for (uint k = 0u; k < 4u; ++k)
        {
            keep |= lit && k < cascades && !sFull[k] ? 1u << k : 0u;
        }
        shadows.keep = keep;
    }

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
    // Added to the LOD marks gathered since the last update; the store pass clears them.
    shadows.dirty[c * SHADOW_TILES + row] |= mask;
}
