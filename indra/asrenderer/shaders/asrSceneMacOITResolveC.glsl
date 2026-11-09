/**
 * asrSceneMacOITResolveC.glsl
 * Mac OIT's resolve, as asMacOITResolveF.glsl: with T = exp(-sum of optical depth), the
 * exact total transmittance,
 *
 *   rgb = (sum c*w / sum w) * (1 - T) + lit.rgb * T
 *   a   = sum glow + lit.a * T
 *
 * When at most MACOIT_LAYERS layers cover a pixel, every weight is exact and this equals
 * sorted blending. It writes a new result, so the opaque image stays for whoever reads it,
 * and puts back the per-pixel state of every pixel the frame touched, so that state is
 * never cleared whole.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 6, rgba16f) uniform readonly image2D macoitColor;   // sum c*w, sum glow
layout(set = 1, binding = 7, rg32f) uniform readonly image2D macoitWeights;   // sum w, sum optical depth
layout(set = 1, binding = 8, rgba16f) uniform readonly image2D lit;           // the opaque image
layout(set = 1, binding = 9, rgba16f) uniform writeonly image2D sceneColor;

void main()
{
    const ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    ViewRef view = ViewRef(push.view);
    if (texel.x >= int(view.pixelSize.x) || texel.y >= int(view.pixelSize.y))
    {
        return;
    }
    const vec4 opaque = imageLoad(lit, texel);
    const uint pixel = uint(texel.y) * uint(view.pixelSize.x) + uint(texel.x);
    MacOITStateRef state = MacOITStateRef(push.macoit);
    if (state.words[macoitCountWord(pixel)] == 0u)
    {
        imageStore(sceneColor, texel, opaque);
        return;
    }

    const vec4 colorGlow = imageLoad(macoitColor, texel);
    const vec2 weightDepth = imageLoad(macoitWeights, texel).rg;
    const float transmittance = exp(-weightDepth.y);
    const vec3 transparent = weightDepth.x > 0.0 ? colorGlow.rgb * ((1.0 - transmittance) / weightDepth.x) : vec3(0.0);
    imageStore(sceneColor, texel, vec4(max(transparent, vec3(0.0)) + opaque.rgb * transmittance, max(colorGlow.a, 0.0) + opaque.a * transmittance));

    for (uint k = 0u; k < MACOIT_LAYERS; ++k)
    {
        state.words[macoitKeyWord(pixel, k)] = MACOIT_EMPTY_KEY;
    }
    state.words[macoitFarthestWord(pixel)] = 0u;
    state.words[macoitCountWord(pixel)] = 0u;
}
