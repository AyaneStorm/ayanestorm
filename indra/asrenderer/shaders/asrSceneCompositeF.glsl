/**
 * asrSceneCompositeF.glsl
 * The scene colour into the window, at the world view's place in it: push.list holds that
 * offset, x low, y high, as the viewport does.
 *
 * Scene colour is linear light. It is exposed (the sky's exposure times RenderExposure),
 * tone mapped by RenderTonemapType, mixed back with the exposed colour by RenderTonemapMix
 * as LL does, and encoded to sRGB for the window. Debug colour views go through untouched.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(set = 1, binding = 9, rgba16f) uniform readonly image2D sceneColor;
layout(set = 1, binding = 19, rgba16f) uniform readonly image2D bloom;

#include "asrBloom.glsl"

layout(location = 0) out vec4 frag_color;

// ACES, Hill's fit (MJP's BakingLab ACES.hlsl), as LL's tonemapUtilF.glsl has it.
// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
const mat3 ACES_INPUT = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
// ODT_SAT => XYZ => D60_2_D65 => sRGB
const mat3 ACES_OUTPUT = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);

vec3 toneMapACES(vec3 color)
{
    color = ACES_INPUT * color;
    const vec3 a = color * (color + 0.0245786) - 0.000090537;
    const vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return clamp(ACES_OUTPUT * (a / b), 0.0, 1.0);
}

// Khronos PBR Neutral (github.com/KhronosGroup/ToneMapping), as LL has it.
vec3 toneMapPBRNeutral(vec3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
    {
        return color;
    }
    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

// AgX (Troy Sobotka), base look, with the published polynomial fit of its contrast curve
// (Benjamin Wrensch's minimal AgX). Bright saturated colours keep their hue as they reach
// white, where ACES shifts them.
vec3 toneMapAgX(vec3 color)
{
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    vec3 x = inset * color;
    x = clamp(log2(max(x, vec3(1e-10))), minEv, maxEv);
    x = (x - minEv) / (maxEv - minEv);
    const vec3 x2 = x * x;
    const vec3 x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    // The curve gives display values: back to linear light, as the other mappers return.
    x = outset * x;
    return pow(clamp(x, 0.0, 1.0), vec3(2.2));
}

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy) - ivec2(push.list & 0xFFFFu, push.list >> 16);
    vec3 color = imageLoad(sceneColor, pixel).rgb;
    ViewRef view = ViewRef(push.view);
    const uint debugView = view.debugView;
    if (debugView != 0u && debugView != DEBUG_VIEW_NO_OCCLUSION)
    {
        frag_color = vec4(color, 1.0); // colours meant as they are
        return;
    }
    if (view.bloomStrength > 0.0 || view.glowStrength > 0.0)
    {
        // Bloom moves bloomStrength of the light into the chain's mean (asrSceneBloomC.glsl);
        // glow comes on top.
        const ivec2 scene = ivec2(view.pixelSize);
        const vec3 spread = bloomBilinear(ivec2(0), bloomLevelSize(scene, 0u), (vec2(pixel) + 0.5) * 0.5);
        color = color * (1.0 - view.bloomStrength) + spread * (1.0 / float(BLOOM_LEVELS));
    }
    // Scene colour is pre-exposed by the sky's exposure where it is drawn; RenderExposure on top.
    const vec3 exposed = max(color, vec3(0.0)) * view.userExposure;
    vec3 mapped = exposed;
    if (view.toneMapper == TONE_MAPPER_PBR_NEUTRAL)
    {
        mapped = toneMapPBRNeutral(exposed);
    }
    else if (view.toneMapper == TONE_MAPPER_ACES)
    {
        mapped = toneMapACES(exposed);
    }
    else if (view.toneMapper == TONE_MAPPER_AGX)
    {
        mapped = toneMapAgX(exposed);
    }
    frag_color = vec4(linearToSrgb(mix(exposed, mapped, view.toneMix)), 1.0);
}
