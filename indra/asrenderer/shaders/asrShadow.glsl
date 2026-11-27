/**
 * asrShadow.glsl
 * Sun and moon shadows, for every shader that lights a surface: one function, so the resolve
 * and Mac OIT shadow alike (doc/ayanestorm-new-renderer-shadows.md). Include after
 * asrSceneCommon.glsl, with set 0's samplers declared.
 *
 * The finest cascade holding the point is read with 2 by 2 bilinear taps a texel apart, made
 * of four gathers, the hardware's 2 by 2 reads, compared here, so no comparison sampler is needed and
 * PCSS can later read the same depths. The point moves along its normal by a texel and a half
 * of that cascade, and its depth by a texel more, so a lit surface does not shadow itself.
 */
layout(set = 1, binding = 20) uniform texture2DArray sunShadow;
// Partly covering casters (hair, lace, leaf edges): optical depth and optical depth times
// depth, summed per texel (asrShadowAlpha.glsl).
layout(set = 1, binding = 22) uniform texture2DArray sunOpacity;

const uint SHADOW_GATHER_SAMPLER = 0u; // UISampler::LinearClamp: gathers read whole texels
const float SHADOW_NORMAL_OFFSET = 1.5; // texels
const float SHADOW_DEPTH_BIAS = 1.0;    // texels, as depth
const float SHADOW_FADE = 0.1;          // the share of the reach over which shadows fade out
const float SHADOW_BLEND = 0.08;        // the share of a cascade's width, at its edge, blended into the next
// PCSS: an edge widens with its blocker's height above the receiver, as the sun's disc makes
// it. The real sun (0.27 degrees, near enough the moon's too), not EEP's drawn size, which
// can be many times larger. Blockers higher than SHADOW_PCSS_REACH widen it no more.
const float SHADOW_LIGHT_RADIUS = 0.00465; // radians
const float SHADOW_PCSS_REACH = 20.0;      // metres
const float SHADOW_BOX_RADIUS = 2.0;       // texels: the 4 by 4 filter, the narrowest edge
const float GOLDEN_ANGLE = 2.39996323;

// One cascade's share of light at texel position `at`, receiver depth `depth`. A light is 1
// where the receiver is at or above the stored depth (depth 1 lies towards the light).
//
// The 4 by 4 box always gives the answer, unless blockers high enough to widen the edge past
// it are found, among its own reads and 4 more on a circle as wide as the widest edge this
// cascade can show: their mean height then sets a radius, and 16 taps on a disc of that
// radius blend in. Every read is in a fixed pattern, so neighbouring texels never disagree.
// Early-outs read from a smaller footprint than the box, and a pattern turned per texel,
// lit squares inside shadows (user's capture, 26 November 2026).
//
// Every tap compares with the receiver's own plane at that tap (`slope`, depth per texel):
// against the centre's depth alone, flat ground sloping in the light's view blocked its own
// wide taps, took the disc and darkened itself in a texel grid (user's capture, 26 November
// 2026).
uint gShadowPath = 0u; // debug view 11: the last cascade read's path, 0 the box, 1 the box with high blockers, 2 the disc

float shadowCascade(ShadowView shadow, vec2 at, float depth, vec2 steepness, float resolution)
{
    const float texel = shadow.lightDirection.w;
    const float depthPerTexel = texel / shadow.depthRange;
    // The wide taps follow the receiver's plane, clamped to 4 texels of depth per texel: they
    // look for blockers metres above, and at 32 a curved arm the light grazed swung it by metres.
    const vec2 slope = clamp(steepness, vec2(-4.0), vec2(4.0)) * depthPerTexel;
    // The box allows each read the plane's rise to it as a tolerance, with no sign: flat ground
    // under a sunset sun climbs 11 texels of depth per texel, and its reads 2.5 texels uphill
    // found the ground above the point and shadowed it, flickering (user's capture, 27 November
    // 2026); followed with its sign, a curved arm's plane broke its edges into texel blocks.
    const vec2 tolerance = abs(steepness) * depthPerTexel;
    const float layer = float(shadow.layer);
    const float search = clamp(SHADOW_PCSS_REACH * SHADOW_LIGHT_RADIUS / texel, SHADOW_BOX_RADIUS, SHADOW_FILTER_TEXELS);
    // Blockers lower than this above the receiver's plane give an edge no wider than the box:
    // they are the box's alone, and never pick the disc (a curved face's own cheek and nose).
    const float wide = SHADOW_BOX_RADIUS * texel / SHADOW_LIGHT_RADIUS / shadow.depthRange; // as depth
    // The plane at a gather's four texel centres, half a texel around its point, in gather
    // order: (low x, high y), (high x, high y), (high x, low y), (low x, low y). At the point
    // alone, ground under a low sun (3 to 4 texels of depth per texel) was off by more than
    // the bias at the centres and shadowed itself in a texel grid (user's capture, 26 November 2026).
    const vec4 corners = vec4(dot(vec2(-0.5, 0.5), slope), dot(vec2(0.5, 0.5), slope), dot(vec2(0.5, -0.5), slope), dot(vec2(-0.5, -0.5), slope));

    // The 4 by 4 box, always: four bilinear taps a texel apart each way, summed: texels t to
    // t + 4 weighted 1 - f, 1, 1, 1, f, from 3 by 3 gathers. Its width keeps casters as fine as
    // a texel, such as hair in the nearest cascade, from flickering as they move. Its reads
    // are also the search's middle.
    float box = 0.0;
    float blockers = 0.0;
    float blockerHeight = 0.0;
    {
        const vec2 from = at - 2.0;
        const vec2 base = floor(from);
        const vec2 f = from - base;
        const vec2 wx[3] = vec2[](vec2(1.0 - f.x, 1.0), vec2(1.0, 1.0), vec2(f.x, 0.0));
        const vec2 wy[3] = vec2[](vec2(1.0 - f.y, 1.0), vec2(1.0, 1.0), vec2(f.y, 0.0));
        for (int y = 0; y < 3; ++y)
        {
            for (int x = 0; x < 3; ++x)
            {
                const vec2 corner = base + vec2(float(2 * x + 1), float(2 * y + 1));
                const float plane = depth + dot(corner - at, slope);
                const vec4 stored = textureGather(sampler2DArray(sunShadow, samplers[SHADOW_GATHER_SAMPLER]), vec3(corner / resolution, layer));
                // Gather order: (low x, high y), (high x, high y), (high x, low y), (low x, low y).
                const vec2 d = corner - at;
                const vec4 allowed = abs(vec4(d.x - 0.5, d.x + 0.5, d.x + 0.5, d.x - 0.5)) * tolerance.x +
                                     abs(vec4(d.y + 0.5, d.y + 0.5, d.y - 0.5, d.y - 0.5)) * tolerance.y;
                box += dot(step(stored, vec4(depth) + allowed), vec4(wx[x].x * wy[y].y, wx[x].y * wy[y].y, wx[x].y * wy[y].x, wx[x].x * wy[y].x));
                const vec4 high = step((vec4(plane + wide) + corners), stored);
                blockers += dot(high, vec4(1.0));
                blockerHeight += dot(high, stored - (vec4(plane) + corners));
            }
        }
        box *= 1.0 / 16.0;
    }
    // The search's edge: 4 taps on a circle as wide as the widest edge this cascade can show.
    for (int k = 0; k < 4; ++k)
    {
        const vec2 tap = floor(at + vec2(float(k & 1) * 2.0 - 1.0, float((k >> 1) & 1) * 2.0 - 1.0) * (0.70710678 * search) - 0.5) + 1.0; // between 4 texel centres
        const float plane = depth + dot(tap - at, slope);
        const vec4 stored = textureGather(sampler2DArray(sunShadow, samplers[SHADOW_GATHER_SAMPLER]), vec3(tap / resolution, layer));
        const vec4 high = step((vec4(plane + wide) + corners), stored);
        blockers += dot(high, vec4(1.0));
        blockerHeight += dot(high, stored - (vec4(plane) + corners));
    }
    if (blockers == 0.0)
    {
        gShadowPath = 0u;
        return box; // nothing high enough to widen the edge
    }
    const float height = blockerHeight / blockers * shadow.depthRange; // metres above the receiver
    const float radius = clamp(height * SHADOW_LIGHT_RADIUS / texel, 0.0, SHADOW_FILTER_TEXELS);
    gShadowPath = radius <= SHADOW_BOX_RADIUS ? 1u : 2u;
    if (radius <= SHADOW_BOX_RADIUS)
    {
        return box;
    }
    const vec2 spin = vec2(1.0, 0.0); // fixed: a pattern turned per texel made neighbours disagree, in squares

    // The disc: 16 gathers on a Vogel spiral, each the mean of its 2 by 2.
    float disc = 0.0;
    vec2 direction = spin;
    const vec2 golden = vec2(cos(GOLDEN_ANGLE), sin(GOLDEN_ANGLE));
    for (int k = 0; k < 16; ++k)
    {
        const vec2 tap = floor(at + direction * (radius * sqrt((float(k) + 0.5) / 16.0)) - 0.5) + 1.0; // between 4 texel centres
        const vec4 stored = textureGather(sampler2DArray(sunShadow, samplers[SHADOW_GATHER_SAMPLER]), vec3(tap / resolution, layer));
        disc += dot(step(stored, vec4(depth + dot(tap - at, slope)) + corners), vec4(0.25));
        direction = vec2(direction.x * golden.x - direction.y * golden.y, direction.x * golden.y + direction.y * golden.x);
    }
    disc *= 1.0 / 16.0;
    // From the box to the disc over half a box radius, so no edge jumps between them.
    return mix(box, disc, clamp((radius - SHADOW_BOX_RADIUS) / (SHADOW_BOX_RADIUS * 0.5), 0.0, 1.0));
}

// The share of light partly covering casters let through to a receiver at texel position
// `at`, depth `depth`. Each texel holds their summed optical depth and its mean depth: a
// receiver behind that mean takes it, one in front does not, over SHADOW_OPACITY_SPREAD (the
// casters' own depth inside a head of hair). 4 bilinear taps on a fixed diagonal, a texel out
// at least (the depth map's box: with one tap a sheet's edge showed its texel stairs), wider
// with the casters' height above the receiver as PCSS's edges, so neighbours agree.
const float SHADOW_OPACITY_SPREAD = 0.02; // metres
float shadowOpacity(ShadowView shadow, vec2 at, float depth, float resolution)
{
    const float layer = float(shadow.layer);
    const float spread = SHADOW_OPACITY_SPREAD / shadow.depthRange;
    const vec2 centre = textureLod(sampler2DArray(sunOpacity, samplers[SHADOW_GATHER_SAMPLER]), vec3(at / resolution, layer), 0.0).rg;
    const float height = centre.x > 0.0 ? max(centre.y / centre.x - depth, 0.0) * shadow.depthRange : 0.0;
    const float radius = clamp(height * SHADOW_LIGHT_RADIUS / shadow.lightDirection.w, 1.0, SHADOW_FILTER_TEXELS) * 0.7071;
    float optical = 0.0;
    for (uint i = 0u; i < 4u; ++i)
    {
        const vec2 offset = vec2((i & 1u) != 0u ? radius : -radius, (i & 2u) != 0u ? radius : -radius);
        const vec2 tap = textureLod(sampler2DArray(sunOpacity, samplers[SHADOW_GATHER_SAMPLER]), vec3((at + offset) / resolution, layer), 0.0).rg;
        const float tapMean = tap.x > 0.0 ? tap.y / tap.x : 0.0;
        optical += 0.25 * tap.x * clamp((tapMean - depth) / spread + 0.5, 0.0, 1.0);
    }
    return exp(-optical);
}

// Where a point falls in cascade i: x, y in texels; z its biased depth; w how far inside the
// usable window it is, in texels (negative: outside, or past the light's side).
vec4 shadowPlace(ShadowView shadow, vec3 relative, vec3 normal, float resolution)
{
    const float texel = shadow.lightDirection.w;
    const vec4 clip = shadow.viewProjection * vec4(relative + normal * (texel * SHADOW_NORMAL_OFFSET), 1.0);
    const vec2 at = (clip.xy * 0.5 + 0.5) * resolution;
    const vec2 inside = min(at, vec2(resolution) - at) - SHADOW_FILTER_MARGIN;
    return vec4(at, clip.z + SHADOW_DEPTH_BIAS * texel / shadow.depthRange, clip.z < 0.0 ? -1.0 : min(inside.x, inside.y));
}

// The receiver's slope in the light's view: texels of depth per texel along x and y, from
// its normal in the light's frame (the matrix's rows are the window's axes over the radius,
// and the light over the depth range), up to SHADOW_SLOPE_STEEP (a sun about 2 degrees over
// flat ground).
const float SHADOW_SLOPE_STEEP = 32.0;
vec2 shadowSlope(ShadowView shadow, vec3 normal)
{
    const mat4 m = shadow.viewProjection;
    const vec3 u = normalize(vec3(m[0].x, m[1].x, m[2].x));
    const vec3 v = normalize(vec3(m[0].y, m[1].y, m[2].y));
    const vec3 toLight = normalize(vec3(m[0].z, m[1].z, m[2].z));
    const vec2 slope = -vec2(dot(normal, u), dot(normal, v)) / max(dot(normal, toLight), 1.0 / SHADOW_SLOPE_STEEP);
    return clamp(slope, vec2(-SHADOW_SLOPE_STEEP), vec2(SHADOW_SLOPE_STEEP));
}

// The share of the shadowing light (GpuShadowViews::light) that reaches a point `relative` to
// the camera, with this normal. The finest cascade holding it is read; near that cascade's
// edge the next one is read too and blended in, so the step between them shows no line.
float asr_shadow_sun(vec3 relative, vec3 normal)
{
    ViewRef view = ViewRef(push.view);
    if (view.shadowCascades == 0u)
    {
        return 1.0;
    }
    ShadowViewsRef shadows = ShadowViewsRef(view.shadowViews);
    const uint count = shadows.count;
    if (count == 0u)
    {
        return 1.0;
    }
    const float reach = shadows.splits[count - 1u];
    const float distanceToCamera = length(relative);
    if (distanceToCamera >= reach)
    {
        return 1.0;
    }
    const float resolution = view.shadowResolution;
    const float band = SHADOW_BLEND * resolution;
    for (uint i = 0u; i < count; ++i)
    {
        const ShadowView shadow = shadows.views[i];
        const vec4 place = shadowPlace(shadow, relative, normal, resolution);
        if (place.w < 0.0)
        {
            continue;
        }
        float lit = shadowCascade(shadow, place.xy, place.z, shadowSlope(shadow, normal), resolution) *
                    shadowOpacity(shadow, place.xy, place.z, resolution);
        if (place.w < band && i + 1u < count)
        {
            const ShadowView next = shadows.views[i + 1u];
            const vec4 outer = shadowPlace(next, relative, normal, resolution);
            if (outer.w >= 0.0)
            {
                const float nextLit = shadowCascade(next, outer.xy, outer.z, shadowSlope(next, normal), resolution) *
                                      shadowOpacity(next, outer.xy, outer.z, resolution);
                lit = mix(nextLit, lit, smoothstep(0.0, band, place.w));
            }
        }
        return mix(lit, 1.0, smoothstep(reach * (1.0 - SHADOW_FADE), reach, distanceToCamera));
    }
    return 1.0;
}

// Debug view 8: the first cascade whose map holds the point, SHADOW_NO_CASCADE past them all.
const uint SHADOW_NO_CASCADE = 0xFFFFFFFFu;
uint shadowCascadeAt(vec3 relative)
{
    ShadowViewsRef shadows = ShadowViewsRef(ViewRef(push.view).shadowViews);
    for (uint i = 0u; i < shadows.count; ++i)
    {
        const vec4 clip = shadows.views[i].viewProjection * vec4(relative, 1.0);
        if (all(lessThanEqual(abs(clip.xy), vec2(1.0))) && clip.z >= 0.0)
        {
            return i;
        }
    }
    return SHADOW_NO_CASCADE;
}

// The sun's and the moon's share: only the light the maps were drawn from is shadowed.
vec2 sunMoonShadow(vec3 relative, vec3 normal, vec3 seen)
{
    const uint light = ShadowViewsRef(ViewRef(push.view).shadowViews).light;
    // In shadow without a lookup: a surface whose own normal faces away from the light
    // (OpenGL's rule, sampleDirectionalShadow), or whose side seen does (`seen`: the
    // triangle's face turned to the camera). The map cannot tell for either: a ceiling with
    // its normals pointing up, seen from below, is its own topmost caster and read as lit,
    // and its normal map put sun highlights indoors (user's capture, 26 November 2026).
    ExposureRef e = ExposureRef(ViewRef(push.view).exposure);
    const vec3 toLight = light == SHADOW_LIGHT_MOON ? e.moonDirection : e.sunDirection;
    // The side seen decides only where the normals contradict it (inverted normals): tested on
    // every surface, its flat triangles cut a zigzag along a curved arm's terminator, where the
    // smooth normal still let in a little light (user's capture, 26 November 2026).
    const bool inverted = dot(normal, seen) < 0.0;
    const bool away = dot(normal, toLight) <= 0.0 || (inverted && dot(seen, toLight) <= 0.0);
    const float shadow = light != SHADOW_LIGHT_NONE && away ? 0.0 : asr_shadow_sun(relative, normal);
    return light == SHADOW_LIGHT_SUN ? vec2(shadow, 1.0) : light == SHADOW_LIGHT_MOON ? vec2(1.0, shadow) : vec2(1.0);
}
