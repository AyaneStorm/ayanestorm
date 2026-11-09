/**
 * asrSceneMacOIT.glsl
 * Mac OIT, the viewer's own order-independent transparency (asmacoit.cpp,
 * asMacOITCaptureF.glsl), rebuilt for the native renderer: the same keys, moments, weights
 * and resolve, with atomics where OpenGL 4.1 had none. Included by one fragment shader per
 * pass, which defines MACOIT_PASS:
 *
 *   KEYS   0: every blended fragment counts itself, widens the pixel's farthest depth, and
 *          inserts its key into the pixel's MACOIT_LAYERS sorted slots by a chain of
 *          atomicMin. A key equal in depth to a slot's is the same layer: the slot keeps the
 *          smaller and the other goes, as peeling strictly behind each layer gives. So one
 *          pass finds exactly the layers the K peel passes of the OpenGL version find.
 *   TAIL   1: only where a pixel has more than MACOIT_LAYERS fragments, and drawn at all only
 *          when one does: every layer fragment behind layer K-2 adds its optical depth and
 *          four power moments, over the pixel's own [layer K-2, farthest] range, by
 *          additive blending.
 *   COLOR  2: each fragment is weighted by the transmittance in front of it: exact for the
 *          K nearest layers, from the moments behind them. Adds colour times weight, the
 *          weight and the optical depth, by additive blending, and reports texture feedback.
 *
 * Fragments never write depth, and the opaque depth hides what is behind it before any
 * of this runs. The resolve (asrSceneMacOITResolveC.glsl) normalizes and composites.
 */
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "asrSceneCommon.glsl"

layout(early_fragment_tests) in;

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];

layout(location = 0) in vec2 vary_uv;
layout(location = 1) flat in uint vary_texture;
layout(location = 2) flat in vec4 vary_color;

#if MACOIT_PASS == 2
layout(set = 1, binding = 4, rgba32f) uniform readonly image2D macoitMoments; // optical depth * (z, z^2, z^3, z^4)
layout(set = 1, binding = 5, r32f) uniform readonly image2D macoitTailDepth;  // optical depth behind layer K-2
#endif
#if MACOIT_PASS != 0
layout(location = 0) out vec4 macoit_target0;
layout(location = 1) out vec4 macoit_target1;
#endif

// The offset of GpuSceneCounts::macoitTailSeen, which the gate reads.
const uint64_t MACOIT_TAIL_SEEN_OFFSET = 832ul;
layout(buffer_reference, std430) buffer TailSeenRef { uint value; };

// The moments are blended toward this bias vector, which keeps the Hankel matrix positive
// definite in finite precision: Mac OIT's ASRenderMacOITMomentBias default.
const float MACOIT_MOMENT_BIAS = 5.0e-7;
// Weight given to the query point itself in the moment bound: the paper's recommended value.
const float MACOIT_OVERESTIMATION = 0.25;

// Log-distance depth, 0 at the near plane and 1 at the far end of the view. Uniform
// relative precision. 1 / w is the distance along the view axis.
float macoitDepth01()
{
    ViewRef view = ViewRef(push.view);
    const float nearClip = view.projection.z;
    const float farClip = view.farClip > 0.0 ? view.farClip : 65536.0;
    const float distanceAlong = 1.0 / gl_FragCoord.w;
    return clamp(log2(distanceAlong / nearClip) / log2(max(farClip / nearClip, 2.0)), 0.0, 1.0);
}

uint macoitDepthBits(float depth01)
{
    return uint(depth01 * MACOIT_DEPTH_SCALE + 0.5);
}

uint macoitKey(uint depthBits, float alpha)
{
    return ((depthBits << 8u) | uint(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5)) + MACOIT_KEY_BIAS;
}

// The tail's moment coordinate: the pixel's [anchor, farthest] range mapped to [-1, 1].
float macoitTailCoordinate(float depth01, float anchor01, float far01)
{
    return 2.0 * clamp((depth01 - anchor01) / max(far01 - anchor01, 1.0e-7), 0.0, 1.0) - 1.0;
}

uint macoitPixel()
{
    return uint(gl_FragCoord.y) * uint(ViewRef(push.view).pixelSize.x) + uint(gl_FragCoord.x);
}

vec4 surfaceColor(out vec2 uvDx, out vec2 uvDy)
{
    uvDx = dFdx(vary_uv);
    uvDy = dFdy(vary_uv);
    const uint slot = vary_texture & 0xFFFFFu;
    const uint samplerIndex = (vary_texture >> 28) & 3u;
    return vary_color * texture(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(samplerIndex)]), vary_uv);
}

#if MACOIT_PASS == 2
// Moment-Based Order-Independent Transparency (Muenstermann, Krumpen, Klein, Peters, 2018),
// as asMacOITMomentsF.glsl: the Hamburger bound on the fraction of the optical depth lying in
// front of z, from the biased moments b1 = E[z], b2 = E[z^2] and the LDL^T factors.
float macoitAbsorbanceFraction(float z, float b1, float b2, float l21, float invD11, float invD22)
{
    vec3 c = vec3(1.0, z, z * z);
    c.y -= b1;
    c.z -= b2 + l21 * c.y;
    c.y *= invD11;
    c.z *= invD22;
    c.y -= l21 * c.z;
    c.x -= c.y * b1 + c.z * b2;

    const float invC2 = 1.0 / c.z;
    const float p = c.y * invC2;
    const float q = c.x * invC2;
    const float r = sqrt(max(0.25 * p * p - q, 0.0));
    const float z1 = -0.5 * p - r;
    const float z2 = -0.5 * p + r;

    const float f0 = MACOIT_OVERESTIMATION;
    const float f1 = z1 < z ? 1.0 : 0.0;
    const float f2 = z2 < z ? 1.0 : 0.0;
    const float f01 = (f1 - f0) / (z1 - z);
    const float f12 = (f2 - f1) / (z2 - z1);
    const float f012 = (f12 - f01) / (z2 - z);
    const float p2 = f012;
    const float p1 = f01 - f012 * (z + z1);
    const float p0 = f0 - f01 * z + f012 * z * z1;
    const float fraction = p0 + p1 * b1 + p2 * b2;
    // Coincident support points divide by zero; a NaN maps to nothing in front.
    return fraction > 0.0 ? min(fraction, 1.0) : 0.0;
}

// Source-over transmittance of everything in front of a surface: exact through the layers in
// front of it, and behind all of them, the moment estimate relative to the last layer, so the
// layers' own optical depth is never counted twice. The OpenGL version's merge pass did the
// per-pixel part once; here only tail fragments, which are few, pay for it.
float macoitFrontTransmittance(MacOITStateRef state, uint pixel, float depth01, uint depthBits)
{
    float transmittance = 1.0;
    uint keys[MACOIT_LAYERS];
    for (uint k = 0u; k < MACOIT_LAYERS; ++k)
    {
        keys[k] = state.words[macoitKeyWord(pixel, k)];
        if (depthBits <= macoitKeyDepth(keys[k]))
        {
            return transmittance;
        }
        transmittance *= macoitKeyTransmittance(keys[k]);
    }

    const ivec2 texel = ivec2(gl_FragCoord.xy);
    const float m0 = imageLoad(macoitTailDepth, texel).r;
    if (m0 <= 0.0)
    {
        return transmittance;
    }
    const vec4 b = mix(imageLoad(macoitMoments, texel) / m0, vec4(0.0, 0.375, 0.0, 0.375), MACOIT_MOMENT_BIAS);
    const float d11 = b.y - b.x * b.x;
    const float invD11 = 1.0 / d11;
    const float l21D11 = b.z - b.x * b.y;
    const float l21 = l21D11 * invD11;
    const float d22 = (b.w - b.y * b.y) - l21D11 * l21;
    const float invD22 = 1.0 / d22;

    const float anchor01 = macoitKeyDepth01(keys[MACOIT_LAYERS - 2u]);
    const float far01 = uintBitsToFloat(state.words[macoitFarthestWord(pixel)]);
    const float lastFraction = macoitAbsorbanceFraction(macoitTailCoordinate(macoitKeyDepth01(keys[MACOIT_LAYERS - 1u]), anchor01, far01),
                                                        b.x, b.y, l21, invD11, invD22);
    const float fraction = macoitAbsorbanceFraction(macoitTailCoordinate(depth01, anchor01, far01), b.x, b.y, l21, invD11, invD22);
    return transmittance * exp(-m0 * max(fraction - lastFraction, 0.0));
}
#endif

void main()
{
    vec2 uvDx;
    vec2 uvDy;
    const vec4 color = surfaceColor(uvDx, uvDy);
    const float alpha = clamp(color.a, 0.0, 1.0);
    if (alpha <= 0.0)
    {
        discard; // not a fragment: no count, no optical depth
    }
    const float depth01 = macoitDepth01();
    const uint depthBits = macoitDepthBits(depth01);
    const uint pixel = macoitPixel();
    MacOITStateRef state = MacOITStateRef(push.macoit);

#if MACOIT_PASS == 0
    const uint before = atomicAdd(state.words[macoitCountWord(pixel)], 1u);
    if (before == MACOIT_LAYERS)
    {
        TailSeenRef(push.output_counts + MACOIT_TAIL_SEEN_OFFSET).value = 1u; // this pixel has a tail
    }
    atomicMax(state.words[macoitFarthestWord(pixel)], floatBitsToUint(depth01));
    if (alpha < MACOIT_MIN_LAYER_ALPHA)
    {
        return; // counted, but too faint to be a layer
    }
    uint key = macoitKey(depthBits, alpha);
    for (uint k = 0u; k < MACOIT_LAYERS; ++k)
    {
        const uint previous = atomicMin(state.words[macoitKeyWord(pixel, k)], key);
        if (previous == MACOIT_EMPTY_KEY || macoitKeyDepth(previous) == macoitKeyDepth(key))
        {
            return; // an empty slot taken, or the same layer: the smaller key stays
        }
        key = max(previous, key); // the one this slot let go goes on behind it
    }
#elif MACOIT_PASS == 1
    if (state.words[macoitCountWord(pixel)] <= MACOIT_LAYERS || alpha < MACOIT_MIN_LAYER_ALPHA)
    {
        discard; // no tail here, or not a layer
    }
    const uint anchor = state.words[macoitKeyWord(pixel, MACOIT_LAYERS - 2u)];
    if (depthBits <= macoitKeyDepth(anchor))
    {
        discard; // at or in front of layer K-2: exact, not in the moments
    }
    const float opticalDepth = macoitOpticalDepth(alpha);
    const float z = macoitTailCoordinate(depth01, macoitKeyDepth01(anchor), uintBitsToFloat(state.words[macoitFarthestWord(pixel)]));
    const float z2 = z * z;
    macoit_target0 = opticalDepth * vec4(z, z2, z2 * z, z2 * z2);
    macoit_target1 = vec4(opticalDepth, 0.0, 0.0, 0.0);
#else
    const float weight = alpha * macoitFrontTransmittance(state, pixel, depth01, depthBits);
    macoit_target0 = vec4(max(color.rgb, vec3(0.0)) * weight, 0.0);
    macoit_target1 = vec4(weight, macoitOpticalDepth(alpha), 0.0, 0.0);
    giveFeedback(ivec2(gl_FragCoord.xy), vary_texture & 0xFFFFFu, uvDx, uvDy);
#endif
}
