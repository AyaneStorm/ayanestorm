/**
 * asrShadowAlpha.glsl
 * A shadow caster with holes, masked and blended alike, included twice (doc/ayanestorm-new-
 * renderer-shadows.md, hair opacity maps):
 *
 * asrShadowMaskedF.glsl, into the depth map: only where the texture is solid at the level the
 * shadow texel covers (SHADOW_ALPHA_SOLID: a leaf's centre). It also adds the level read to
 * GpuShadowViews::demand, so streaming keeps the texture at that size while the cached map
 * shows it.
 *
 * asrShadowOpacityF.glsl (OPACITY), into the opacity map, depth tested but not written: the
 * rest, by its coverage c, as optical depth t = -log(1 - c), summed with t times its depth,
 * so a reader finds the light let through, exp(-sum t), and the casters' mean depth. Behind
 * the first solid caster nothing is added (the depth test): hair at the back of a head does
 * not move the mean behind the face.
 *
 * Hair strands are far thinner than a texel: cut at 0.5 into the depth map they became an
 * on/off pattern that pulsed over faces as the avatar moved (user's capture, 26 November
 * 2026), and OpenGL's every-other-column cut below 0.88 smeared into grey blotches.
 */
#extension GL_EXT_nonuniform_qualifier : require
#include "asrSceneCommon.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];

layout(location = 1) in vec2 vary_uv;
layout(location = 2) flat in uint vary_texture;
layout(location = 3) flat in vec2 vary_alpha;
layout(location = 4) flat in uint vary_layer;

#ifdef OPACITY
layout(location = 0) out vec2 frag_opacity;
#endif

const float SHADOW_ALPHA_SOLID = 0.9;      // coverage written as depth
const float SHADOW_ALPHA_FAINT = 1.0 / 255.0; // coverage too small to add
const float SHADOW_OPTICAL_MAX = 8.0;       // a fully covered texel's optical depth: 0.03 % let through

void main()
{
#ifdef OPACITY
    if (push.phase == SHADOW_DRAW_STATIC)
    {
        // Optical depth adds up, so a static caster reaching from a dirty tile into a clean one
        // must not add itself again over what the cache kept there (depth is idempotent).
        ViewRef view = ViewRef(push.view);
        ShadowViewsRef shadows = ShadowViewsRef(view.shadowViews);
        const uvec2 tile = uvec2(gl_FragCoord.xy * (float(SHADOW_TILES) / view.shadowResolution));
        if ((shadows.dirty[vary_layer * SHADOW_TILES + tile.y] & (1u << tile.x)) == 0u)
        {
            discard;
        }
    }
#endif
    const uint slot = vary_texture & 0xFFFFFu;
    const uint sampler_index = (vary_texture >> 28) & 3u;
    const vec2 uvDx = dFdx(vary_uv);
    const vec2 uvDy = dFdy(vary_uv);
    float coverage = textureGrad(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(sampler_index)]), vary_uv, uvDx, uvDy).a;

#ifndef OPACITY
    if (slot != 0u && vary_layer < 4u)
    {
        // The level read, as giveFeedback() counts it: texels across the texture.
        const float step = max(max(dot(uvDx, uvDx), dot(uvDy, uvDy)), 1e-30);
        const uint level = uint(clamp(ceil(-0.5 * log2(step)), 0.0, float(FEEDBACK_MAX_LEVEL)));
        const uint bit = 1u << ((vary_layer & 1u) * 16u + level);
        ShadowViewsRef shadows = ShadowViewsRef(ViewRef(push.view).shadowViews);
        const uint word = slot * 2u + (vary_layer >> 1);
        if ((shadows.demand[word] & bit) == 0u)
        {
            atomicOr(shadows.demand[word], bit);
        }
    }
#endif

    // Blended: the filtered alpha times the colour's. Masked (vary_alpha.y, the texture alpha
    // a shown texel reaches): its alpha is near 0 or 1 per texel, so the filtered alpha is the
    // share of shown texels the shadow texel covers; none past 1, all at 0. A texture of even
    // partial alpha is still read as that share.
    const float threshold = vary_alpha.y;
    if (threshold > 1.0)
    {
        discard;
    }
    coverage = threshold < 0.0 ? coverage * vary_alpha.x : threshold == 0.0 ? 1.0 : coverage;
#ifdef OPACITY
    if (coverage >= SHADOW_ALPHA_SOLID || coverage < SHADOW_ALPHA_FAINT)
    {
        discard;
    }
    const float optical = min(-log(1.0 - coverage), SHADOW_OPTICAL_MAX);
    frag_opacity = vec2(optical, optical * gl_FragCoord.z);
#else
    if (coverage < SHADOW_ALPHA_SOLID)
    {
        discard;
    }
#endif
}
