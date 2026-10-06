// AyaneStorm OIT shader. Author: chanayane@firestorm.
/**
 * @file asMacOITResolveF.glsl
 * @brief Composites Mac OIT's accumulated transparency over the opaque scene.
 *
 * With T = exp(-sum of optical depth), the exact aggregate transmittance:
 *
 *   rgb = (sum c*w / sum w) * (1 - T) + opaque.rgb * T
 *   a   = sum glow            + opaque.a * T
 *
 * The opaque term is applied by dual-source blending (ONE, SRC1_COLOR), so
 * the screen is never read as a texture. When at most K layers cover a
 * pixel, every weight is exact and the result equals sorted blending.
 *
 * Self-lighting floater isolate mode enables depth writes; covered pixels
 * then write a near-plane depth so the later backdrop pass keeps them.
 */

uniform sampler2D macoitColorGlow;     // sum c*w, sum glow
uniform sampler2D macoitWeightDepth;   // sum w, sum optical depth
uniform sampler2D macoitState;         // merged state; layer 0 = keys
uniform int macoitExactLayers;
uniform int macoitDebugMode;

layout(location = 0, index = 0) out vec4 macoit_color;
layout(location = 0, index = 1) out vec4 macoit_transmittance;

// Must match asMacOITCaptureF.glsl.
const float MACOIT_EMPTY = 3.4028234e38;
const uint MACOIT_KEY_BIAS = 0x00800000u;

// Debug mode 3: exact layer count, and whether a moment-weighted tail exists
// behind them (the pixel's optical depth exceeds the keyed layers' own).
vec3 macoit_layer_diagnostic(ivec2 pixel, float b0)
{
    vec4 keys = texelFetch(macoitState, pixel, 0);
    int count = 0;
    float keyed_optical_depth = 0.0;
    for (int k = 0; k < 4; ++k)
    {
        if (k < macoitExactLayers && keys[k] < MACOIT_EMPTY)
        {
            float alpha = float((floatBitsToUint(keys[k]) - MACOIT_KEY_BIAS) & 255u) / 255.0;
            keyed_optical_depth += -log(max(1.0 - alpha, 1.0 / 65536.0));
            ++count;
        }
    }
    if (count == macoitExactLayers && b0 > keyed_optical_depth * 1.02 + 0.002)
    {
        return vec3(1.0, 0.0, 0.0);  // tail behind every exact layer
    }
    const vec3 palette[5] = vec3[5](vec3(0.15), vec3(0.0, 0.3, 1.0),
                                    vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0),
                                    vec3(1.0, 0.5, 0.0));
    return palette[count];
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 color_glow = texelFetch(macoitColorGlow, pixel, 0);
    vec2 weight_depth = texelFetch(macoitWeightDepth, pixel, 0).rg;
    float weight = weight_depth.x;
    float b0 = weight_depth.y;
    if (b0 <= 0.0 && color_glow.a <= 0.0)
    {
        discard;  // nothing captured: the opaque pixel is already final
    }

    float transmittance = exp(-b0);
    vec3 transparent = weight > 0.0 ?
        color_glow.rgb * ((1.0 - transmittance) / weight) : vec3(0.0);

    macoit_color = vec4(max(transparent, vec3(0.0)), max(color_glow.a, 0.0));
    macoit_transmittance = vec4(transmittance);
    gl_FragDepth = 0.0;

    if (macoitDebugMode != 0)
    {
        vec3 diagnostic = vec3(1.0, 0.0, 1.0);  // 1: covered
        if (macoitDebugMode == 2)
        {
            diagnostic = vec3(transmittance);
        }
        else if (macoitDebugMode == 3)
        {
            diagnostic = macoit_layer_diagnostic(pixel, b0);
        }
        else if (macoitDebugMode == 4)
        {
            diagnostic = weight > 0.0 ? color_glow.rgb / weight : vec3(0.0, 0.0, 1.0);
        }
        macoit_color = vec4(diagnostic, 0.0);
        macoit_transmittance = vec4(0.0);
    }
}
