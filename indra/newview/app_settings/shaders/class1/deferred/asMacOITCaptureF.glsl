// AyaneStorm OIT shader. Author: chanayane@firestorm.
/**
 * @file asMacOITCaptureF.glsl
 * @brief Mac OIT capture library (GLSL 4.10, no main()).
 *
 * Order-independent transparency built only from OpenGL 4.1 blending: no
 * atomics, images, or storage buffers. Linked into every Mac OIT material
 * program, where the shared material shaders call avboit_store() through the
 * weighted-OIT hook they already expose under the AVBOIT define, and into the
 * emissive and PBR glow programs, which call macoit_store_glow().
 *
 * Passes (avboitRasterPass; every pass but COLOR stops after alpha):
 *
 *   KEYS   MIN-blend the nearest layer key and the negated depth (so MIN
 *          yields the farthest depth). Stencil counts the pixel's fragments.
 *   PEEL   k = 1..K-1: MIN-blend the nearest key strictly behind layer k-1,
 *          read from macoitState, and carry the farthest depth forward.
 *          Stencil skips pixels with <= k fragments. The last peel pass also
 *          ADDs the optical depth behind layer K-2 and its power moments,
 *          warped to the pixel's own [layer K-2, farthest] depth range.
 *   COLOR  Weight each fragment by the transmittance in front of it: exact
 *          source-over for the K nearest layers, moment-reconstructed for
 *          the tail behind them. ADD color * weight, weight, optical depth.
 *
 * Anchoring the moments per pixel just ahead of the tail, instead of over
 * the whole view depth, is what lets float32 moments resolve hair strands a
 * few millimetres apart (scripts/testing/macoit_reference.py simulate).
 *
 * The resolve then normalizes the weighted color and applies the pixel's
 * exact total opacity (asMacOITResolveF.glsl).
 */

uniform int avboitRasterPass;
// PEEL: channel of the previous layer's key in macoitState, and the channel
// this pass writes in its own target.
uniform int macoitReadChannel;
uniform int macoitWriteChannel;
// Non-zero in the last peel pass, which accumulates the tail moments.
uniform int macoitMomentPass;
// K, the number of exact front layers (2..4).
uniform int macoitExactLayers;
// Rows between the stacked layers of the merged state texture.
uniform int macoitLayerRows;
// Near plane, far plane, 1 / log2(far / near).
uniform vec3 macoitDepth;
// PEEL: the previous pass's key target. COLOR: the merged per-pixel state,
// three layers stacked vertically (asMacOITMergeF.glsl). The only sampler
// this library adds, because macOS exposes 16 fragment texture units and
// the alpha material programs already use most of them.
uniform sampler2D macoitState;

layout(location = 0) out vec4 macoit_target0;
layout(location = 1) out vec4 macoit_target1;

#ifdef MACOIT_DOF
// Live DoF transparency bins (asdoflive.cpp; doc/ayanestorm-depth-of-field-
// live-plan.md, phase 2). In the COLOR pass, with macoitDofBins set, every
// fragment also adds its weighted colour to the blur bins its own depth
// falls in, front to back N2, N1, F, B1, B2, as (colour * w, w), plus the
// bins' energy and radius sums. The bins therefore partition exactly what this
// pass accumulates. Only programs that can draw blended alpha declare these:
// non-blend GLTF variants keep their implicit frag_data[4] locations.
uniform int macoitDofBins;
// focal_distance, blur_constant, tan_pixel_angle, magnification
uniform vec4 macoitDofLens;
// max_coc, near_radius, far_radius, split_radius (full-resolution pixels)
uniform vec4 macoitDofRadii;
// Full-resolution to gather pixels.
uniform float macoitDofGatherScale;
// B1 / B2 boundary (full-resolution pixels).
uniform float macoitDofFarSplit;
// Field position gl_FragCoord.xy * xy - zw, and field curvature
// (ASDepthOfField::LensField).
uniform vec4 macoitDofField;
uniform float macoitDofCurvature;

layout(location = 2) out vec4 macoit_dof_n2;
layout(location = 3) out vec4 macoit_dof_n1;
layout(location = 4) out vec4 macoit_dof_focus;
layout(location = 5) out vec4 macoit_dof_back1;
layout(location = 6) out vec4 macoit_dof_back2;
// (sum w / r^2 of N2, of N1, of B1, sum w r of B2), r in gather px.
layout(location = 7) out vec4 macoit_dof_energy;
#endif

float macoit_absorbance_fraction(float z, float b1, float b2, float l21,
                                 float inv_d11, float inv_d22);

const int MACOIT_PASS_KEYS = 0;
const int MACOIT_PASS_PEEL = 1;
const int MACOIT_PASS_COLOR = 2;

// Layer key: 22-bit log depth above 8-bit alpha, offset so every key is a
// positive normal float. For such floats IEEE ordering equals the ordering of
// their bit patterns, so MIN blending on the float is an exact atomicMin on
// the key: the nearest depth wins, and the key carries that layer's alpha.
// The largest key, 0x407fffff, stays far below infinity and NaN.
const uint MACOIT_KEY_BIAS = 0x00800000u;
const float MACOIT_DEPTH_SCALE = 4194303.0;
// Cleared value of every key slot, and the MIN-neutral output. Its decoded
// depth exceeds every real depth, so an empty slot is never "in front".
const float MACOIT_EMPTY = 3.4028234e38;
// Fragments whose alpha quantizes to zero cannot occlude and are not layers.
const float MACOIT_MIN_LAYER_ALPHA = 1.0 / 255.0;
// Glow is drawn by a different vertex program than its surface's color, so
// its depth may differ in the last bits. Allow this many depth steps (about
// 14 um at 2 m) when matching a glow fragment to its layer.
const uint MACOIT_GLOW_TOLERANCE = 4u;

// Eye-space distance of the fragment along the view axis.
float macoit_view_distance()
{
    float ndc = gl_FragCoord.z * 2.0 - 1.0;
    return 2.0 * macoitDepth.x * macoitDepth.y /
        (macoitDepth.y + macoitDepth.x - ndc * (macoitDepth.y - macoitDepth.x));
}

// Log-distance depth, 0 at the near plane and 1 at the far plane. Uniform
// relative precision: one 22-bit step is about 1.7e-6 of the distance.
float macoit_depth01()
{
    return clamp(log2(macoit_view_distance() / macoitDepth.x) * macoitDepth.z, 0.0, 1.0);
}

#ifdef MACOIT_DOF
// Signed blur radius (full px, negative in front of the focus) and bin
// weights (N2, N1, F, B1; B2 = 1 - their sum). Must match liveBlurRadius()
// and liveBinWeights() in asDoFLiveCommonF.glsl.
float macoit_dof_radius(float view_distance)
{
    float z = -view_distance;
    float coc = (z - macoitDofLens.x) / -z * macoitDofLens.y;
    coc /= macoitDofLens.w;
    coc = coc / (macoitDofLens.z * -macoitDofLens.x) * 1.41421356237;
    coc = -coc / max(macoitDofRadii.x, 0.0001);
    vec2 field = gl_FragCoord.xy * macoitDofField.xy - macoitDofField.zw;
    coc = clamp(coc + macoitDofCurvature * dot(field, field), -1.0, 1.0);
    return coc < 0.0 ? coc * macoitDofRadii.y : coc * macoitDofRadii.z;
}

vec4 macoit_dof_weights(float signed_radius)
{
    float a = abs(signed_radius);
    float focus = 1.0 - smoothstep(0.5, 2.0, a);
    bool behind = signed_radius >= 0.0;
    float split = behind ? macoitDofFarSplit : macoitDofRadii.w;
    float strong = (1.0 - focus) * smoothstep(0.8 * split, 1.25 * split, a);
    float weak = 1.0 - focus - strong;
    return behind ? vec4(0.0, 0.0, focus, weak) : vec4(strong, weak, focus, 0.0);
}

void macoit_dof_clear()
{
    macoit_dof_n2 = vec4(0.0);
    macoit_dof_n1 = vec4(0.0);
    macoit_dof_focus = vec4(0.0);
    macoit_dof_back1 = vec4(0.0);
    macoit_dof_back2 = vec4(0.0);
    macoit_dof_energy = vec4(0.0);
}

void macoit_dof_store(vec3 color, float weight)
{
    if (macoitDofBins == 0)
    {
        macoit_dof_clear();
        return;
    }
    float signed_radius = macoit_dof_radius(macoit_view_distance());
    vec4 shares = macoit_dof_weights(signed_radius);
    vec4 w = shares * weight;
    float far_weight = max(1.0 - dot(shares, vec4(1.0)), 0.0) * weight;
    float r = max(abs(signed_radius) * macoitDofGatherScale, 0.5);
    float inv_r2 = 1.0 / (r * r);
    macoit_dof_n2 = vec4(color * w.x, w.x);
    macoit_dof_n1 = vec4(color * w.y, w.y);
    macoit_dof_focus = vec4(color * w.z, w.z);
    macoit_dof_back1 = vec4(color * w.w, w.w);
    macoit_dof_back2 = vec4(color * far_weight, far_weight);
    macoit_dof_energy = vec4(w.x * inv_r2, w.y * inv_r2, w.w * inv_r2, far_weight * r);
}
#endif

uint macoit_depth_bits(float depth01)
{
    return uint(depth01 * MACOIT_DEPTH_SCALE + 0.5);
}

float macoit_key(uint depth_bits, float alpha)
{
    uint alpha_bits = uint(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
    return uintBitsToFloat(((depth_bits << 8u) | alpha_bits) + MACOIT_KEY_BIAS);
}

uint macoit_key_depth(float key)
{
    return (floatBitsToUint(key) - MACOIT_KEY_BIAS) >> 8u;
}

float macoit_key_transmittance(float key)
{
    return 1.0 - float((floatBitsToUint(key) - MACOIT_KEY_BIAS) & 255u) / 255.0;
}

float macoit_key_depth01(float key)
{
    return float(macoit_key_depth(key)) / MACOIT_DEPTH_SCALE;
}

// Same effective-opaque endpoint as the resolve's total opacity.
float macoit_optical_depth(float alpha)
{
    return -log(max(1.0 - alpha, 1.0 / 65536.0));
}

// Tail moment coordinate: the pixel's [anchor, farthest] depth range, where
// the anchor is exact layer K-2, mapped to [-1, 1].
float macoit_tail_coordinate(float depth01, float anchor01, float far01)
{
    return 2.0 * clamp((depth01 - anchor01) / max(far01 - anchor01, 1.0e-7),
                       0.0, 1.0) - 1.0;
}

vec4 macoit_state(int layer)
{
    return texelFetch(macoitState,
                      ivec2(gl_FragCoord.xy) + ivec2(0, layer * macoitLayerRows), 0);
}

// Source-over transmittance of everything in front of a surface. A surface
// at or in front of exact layer j is attenuated exactly by layers 0..j-1.
// Behind all K layers, the tail is attenuated exactly by the K layers and
// approximately by what lies between the last layer and the surface: the
// moment estimate relative to the last layer, so the layers' own optical
// depth is never counted twice.
float macoit_front_transmittance(float depth01, uint depth_bits, uint tolerance)
{
    vec4 keys = macoit_state(0);
    float transmittance = 1.0;
    for (int k = 0; k < 4; ++k)
    {
        if (k >= macoitExactLayers)
        {
            break;
        }
        if (depth_bits <= macoit_key_depth(keys[k]) + tolerance)
        {
            return transmittance;
        }
        transmittance *= macoit_key_transmittance(keys[k]);
    }
    // x = optical depth behind layer K-2, y = b1, z = b2, w = L21
    vec4 moments = macoit_state(1);
    // x = fraction in front of layer K-1, y = 1/D11, z = 1/D22,
    // w = farthest depth
    vec4 factors = macoit_state(2);
    float z = macoit_tail_coordinate(
        depth01, macoit_key_depth01(keys[macoitExactLayers - 2]), factors.w);
    float fraction = macoit_absorbance_fraction(
        z, moments.y, moments.z, moments.w, factors.y, factors.z);
    return transmittance * exp(-moments.x * max(fraction - factors.x, 0.0));
}

void avboit_store(vec4 color)
{
    float alpha = clamp(color.a, 0.0, 1.0);
    float depth01 = macoit_depth01();
    uint depth_bits = macoit_depth_bits(depth01);
#ifdef MACOIT_DOF
    // Draw buffers 2..7 exist only in the COLOR pass with bins; no output
    // stays undefined in any pass.
    macoit_dof_clear();
#endif

    if (alpha <= 0.0)
    {
        discard;  // not a fragment: no stencil count, no optical depth
    }

    if (avboitRasterPass == MACOIT_PASS_KEYS)
    {
        float key = alpha >= MACOIT_MIN_LAYER_ALPHA ?
            macoit_key(depth_bits, alpha) : MACOIT_EMPTY;
        macoit_target0 = vec4(key, MACOIT_EMPTY, -depth01, 0.0);
        macoit_target1 = vec4(0.0);
        return;
    }

    if (avboitRasterPass == MACOIT_PASS_PEEL)
    {
        // x/y = keys, z = -farthest depth, w = tail optical depth
        vec4 previous = macoit_state(0);
        float previous_key = previous[macoitReadChannel];
        if (alpha < MACOIT_MIN_LAYER_ALPHA ||
            depth_bits <= macoit_key_depth(previous_key))
        {
            discard;  // not a layer, or a layer already keyed
        }
        vec4 target = vec4(MACOIT_EMPTY, MACOIT_EMPTY, -depth01, 0.0);
        target[macoitWriteChannel] = macoit_key(depth_bits, alpha);
        macoit_target1 = vec4(0.0);
        if (macoitMomentPass != 0)
        {
            // Every fragment behind the anchor reaches this point, so the
            // moments describe exactly the layers K-1.. and the tail.
            float optical_depth = macoit_optical_depth(alpha);
            float z = macoit_tail_coordinate(
                depth01, macoit_key_depth01(previous_key), -previous.z);
            float z2 = z * z;
            target.w = optical_depth;
            macoit_target1 = optical_depth * vec4(z, z2, z2 * z, z2 * z2);
        }
        macoit_target0 = target;
        return;
    }

    float weight = alpha * macoit_front_transmittance(depth01, depth_bits, 0u);
    macoit_target0 = vec4(max(color.rgb, vec3(0.0)) * weight, 0.0);
    macoit_target1 = vec4(weight, macoit_optical_depth(alpha), 0.0, 0.0);
#ifdef MACOIT_DOF
    macoit_dof_store(max(color.rgb, vec3(0.0)), weight);
#endif
}

// Glow adds to the pixel's glow, attenuated like any other contribution by
// the layers in front of it, as vanilla's sorted glow suppression does.
void macoit_store_glow(float glow)
{
#ifdef MACOIT_DOF
    // Glow is not part of the blur bins.
    macoit_dof_clear();
#endif
    if (avboitRasterPass != MACOIT_PASS_COLOR || glow <= 0.0)
    {
        discard;
    }
    float depth01 = macoit_depth01();
    float front = macoit_front_transmittance(
        depth01, macoit_depth_bits(depth01), MACOIT_GLOW_TOLERANCE);
    macoit_target0 = vec4(0.0, 0.0, 0.0, glow * front);
    macoit_target1 = vec4(0.0);
}
