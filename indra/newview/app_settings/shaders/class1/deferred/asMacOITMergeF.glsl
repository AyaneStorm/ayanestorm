// AyaneStorm OIT shader. Author: chanayane@firestorm.
/**
 * @file asMacOITMergeF.glsl
 * @brief Packs Mac OIT's per-pixel capture results into the single state
 *        texture the color pass samples, and precomputes everything about the
 *        pixel that its fragments would otherwise each recompute.
 *
 * Drawn once over a W x 3H target. Row block `layer` of the output holds:
 *
 *   0  key0, key1, key2, key3      exact layer keys, nearest first
 *   1  m0, b1, b2, L21             optical depth behind layer K-2, its
 *                                  normalized biased moments, Cholesky factor
 *   2  f_K, 1/D11, 1/D22, far      moment fraction in front of layer K-1,
 *                                  inverse pivots, farthest depth
 *
 * Moments use the pixel's own [layer K-2, farthest] warp; see
 * asMacOITCaptureF.glsl and asMacOITMomentsF.glsl.
 */

uniform sampler2D macoitKeysEven;   // key0, key2, -farthest depth, (m0)
uniform sampler2D macoitKeysOdd;    // key1, key3, -farthest depth, (m0)
uniform sampler2D macoitMoments;    // m0 * E[z, z^2, z^3, z^4]
uniform int macoitExactLayers;
uniform int macoitLayerRows;
// Non-zero when the last peel pass wrote macoitKeysOdd, which then holds m0.
uniform int macoitMomentsInOdd;
// Blend weight toward the bias vector that keeps the Hankel matrix positive
// definite in finite precision (MBOIT's moment bias).
uniform float macoitMomentBias;

layout(location = 0) out vec4 macoit_state;

float macoit_absorbance_fraction(float z, float b1, float b2, float l21,
                                 float inv_d11, float inv_d22);

// Must match asMacOITCaptureF.glsl.
const float MACOIT_EMPTY = 3.4028234e38;
const uint MACOIT_KEY_BIAS = 0x00800000u;
const float MACOIT_DEPTH_SCALE = 4194303.0;

float macoit_key_depth01(float key)
{
    return float((floatBitsToUint(key) - MACOIT_KEY_BIAS) >> 8u) / MACOIT_DEPTH_SCALE;
}

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    int layer = texel.y / macoitLayerRows;
    ivec2 pixel = ivec2(texel.x, texel.y - layer * macoitLayerRows);
    vec4 even = texelFetch(macoitKeysEven, pixel, 0);
    vec4 odd = texelFetch(macoitKeysOdd, pixel, 0);
    vec4 keys = vec4(even.x, odd.x, even.y, odd.y);

    if (layer == 0)
    {
        macoit_state = keys;
        return;
    }

    // Pass 0 saw every fragment; its MIN of -depth is the farthest depth.
    float far01 = -even.z;
    float m0 = macoitMomentsInOdd != 0 ? odd.w : even.w;
    // Only a pixel whose K slots are all filled has a tail to weight.
    float last_key = keys[macoitExactLayers - 1];
    if (m0 <= 0.0 || !(last_key < MACOIT_EMPTY))
    {
        macoit_state = layer == 1 ? vec4(0.0) : vec4(0.0, 0.0, 0.0, far01);
        return;
    }

    vec4 b = mix(texelFetch(macoitMoments, pixel, 0) / m0,
                 vec4(0.0, 0.375, 0.0, 0.375), macoitMomentBias);
    float d11 = b.y - b.x * b.x;
    float inv_d11 = 1.0 / d11;
    float l21_d11 = b.z - b.x * b.y;
    float l21 = l21_d11 * inv_d11;
    float d22 = (b.w - b.y * b.y) - l21_d11 * l21;
    float inv_d22 = 1.0 / d22;

    if (layer == 1)
    {
        macoit_state = vec4(m0, b.x, b.y, l21);
        return;
    }

    float anchor01 = macoit_key_depth01(keys[macoitExactLayers - 2]);
    float z = 2.0 * clamp((macoit_key_depth01(last_key) - anchor01) /
                          max(far01 - anchor01, 1.0e-7), 0.0, 1.0) - 1.0;
    macoit_state = vec4(macoit_absorbance_fraction(z, b.x, b.y, l21, inv_d11, inv_d22),
                        inv_d11, inv_d22, far01);
}
