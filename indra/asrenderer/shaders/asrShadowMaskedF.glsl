/**
 * asrShadowMaskedF.glsl
 * A shadow caster's holes, masked and blended alike. A masked texel at or above the material's
 * cutoff counts as solid, as the surface itself is drawn; a blended one counts by its alpha.
 * Either is then taken with the colour's alpha and casts from 0.5 up. OpenGL's
 * shadowAlphaMaskF.glsl cast every other column below 0.88 instead: a half-dense pattern that
 * the filter smeared into grey blotches crawling over faces as hair moved (user's capture,
 * 26 November 2026).
 *
 * The alpha is read one level sharper than the shadow texel asks: a coarse cascade read at its
 * own level averaged leaves away. What it reads goes to GpuShadowViews::demand, so streaming
 * keeps the texture at that size while the cached map shows it.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "asrSceneCommon.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];

layout(location = 1) in vec2 vary_uv;
layout(location = 2) flat in uint vary_texture;
layout(location = 3) flat in vec2 vary_alpha;
layout(location = 4) flat in uint vary_layer;

const float SHADOW_ALPHA_CAST = 0.5;

void main()
{
    const uint slot = vary_texture & 0xFFFFFu;
    const uint sampler_index = (vary_texture >> 28) & 3u;
    const vec2 uvDx = dFdx(vary_uv) * 0.5; // one level sharper
    const vec2 uvDy = dFdy(vary_uv) * 0.5;
    const float alpha = textureGrad(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(sampler_index)]), vary_uv, uvDx, uvDy).a;

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

    const bool masked = vary_alpha.y > 0.0;
    if ((masked && alpha < vary_alpha.y) || (masked ? 1.0 : alpha) * vary_alpha.x < SHADOW_ALPHA_CAST)
    {
        discard;
    }
}
