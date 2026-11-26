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

const uint SHADOW_GATHER_SAMPLER = 0u; // UISampler::LinearClamp: gathers read whole texels
const float SHADOW_NORMAL_OFFSET = 1.5; // texels
const float SHADOW_DEPTH_BIAS = 1.0;    // texels, as depth
const float SHADOW_FADE = 0.1;          // the share of the reach over which shadows fade out

// Two by two texels from `corner` (in texels), weighted: 1 where the receiver is at or above
// the stored depth (depth 1 lies towards the light).
float shadowGather(vec2 corner, float layer, float depth, vec2 wx, vec2 wy, float resolution)
{
    const vec4 stored = textureGather(sampler2DArray(sunShadow, samplers[SHADOW_GATHER_SAMPLER]), vec3(corner / resolution, layer));
    // Gather order: (low x, high y), (high x, high y), (high x, low y), (low x, low y).
    return dot(step(stored, vec4(depth)), vec4(wx.x * wy.y, wx.y * wy.y, wx.y * wy.x, wx.x * wy.x));
}

// The share of the shadowing light (GpuShadowViews::light) that reaches a point `relative` to
// the camera, with this normal.
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
    const float margin = 2.0 / resolution; // the filter's own reach, in uv
    for (uint i = 0u; i < count; ++i)
    {
        const float texel = shadows.views[i].lightDirection.w;
        const vec4 clip = shadows.views[i].viewProjection * vec4(relative + normal * (texel * SHADOW_NORMAL_OFFSET), 1.0);
        const vec2 uv = clip.xy * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(margin))) || any(greaterThan(uv, vec2(1.0 - margin))) || clip.z < 0.0)
        {
            continue;
        }
        const float depth = clip.z + SHADOW_DEPTH_BIAS * texel / shadows.views[i].depthRange;

        // Two bilinear taps a texel apart each way, summed: texels t to t + 2 weighted 1 - f,
        // 1, f. Odd and even columns always weigh the same, so OpenGL's half-density dither of
        // semi-transparent casters (every other column) reads as an even half shadow.
        const vec2 at = uv * resolution - 1.0;
        const vec2 base = floor(at);
        const vec2 f = at - base;
        const float layer = float(shadows.views[i].layer);
        float lit = shadowGather(base + 1.0, layer, depth, vec2(1.0 - f.x, 1.0), vec2(1.0 - f.y, 1.0), resolution)
                  + shadowGather(base + vec2(3.0, 1.0), layer, depth, vec2(f.x, 0.0), vec2(1.0 - f.y, 1.0), resolution)
                  + shadowGather(base + vec2(1.0, 3.0), layer, depth, vec2(1.0 - f.x, 1.0), vec2(f.y, 0.0), resolution)
                  + shadowGather(base + 3.0, layer, depth, vec2(f.x, 0.0), vec2(f.y, 0.0), resolution);
        lit *= 0.25;
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
vec2 sunMoonShadow(vec3 relative, vec3 normal)
{
    const float shadow = asr_shadow_sun(relative, normal);
    const uint light = ShadowViewsRef(ViewRef(push.view).shadowViews).light;
    return light == SHADOW_LIGHT_SUN ? vec2(shadow, 1.0) : light == SHADOW_LIGHT_MOON ? vec2(1.0, shadow) : vec2(1.0);
}
