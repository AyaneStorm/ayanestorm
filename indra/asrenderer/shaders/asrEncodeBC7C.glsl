/**
 * asrEncodeBC7C.glsl
 * BC7, a block a thread. Every block tries mode 6 (one subset, RGBA, 4-bit indices). A block
 * mode 6 leaves visibly wrong tries more: an opaque one mode 1 (two subsets, RGB, 3-bit
 * indices) on the partition of the 64 a quick estimate ranks best, one with alpha mode 5
 * (RGB and alpha with indices of their own). The smallest squared error wins. Endpoints
 * come from the principal axis, then once more from least squares on the indices they gave.
 * Indices live in 64-bit fields, not arrays, so a thread stays in its registers.
 * Bit layouts: the BC7 format specification, least significant bit first.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrEncodeCommon.glsl"

layout(local_size_x = ENCODE_GROUP_SIZE) in;

// Two-subset partitions: bit i set puts texel i in subset 1.
const uint PARTITIONS2[64] = uint[64](
    0xCCCCu, 0x8888u, 0xEEEEu, 0xECC8u, 0xC880u, 0xFEECu, 0xFEC8u, 0xEC80u,
    0xC800u, 0xFFECu, 0xFE80u, 0xE800u, 0xFFE8u, 0xFF00u, 0xFFF0u, 0xF000u,
    0xF710u, 0x008Eu, 0x7100u, 0x08CEu, 0x008Cu, 0x7310u, 0x3100u, 0x8CCEu,
    0x088Cu, 0x3110u, 0x6666u, 0x366Cu, 0x17E8u, 0x0FF0u, 0x718Eu, 0x399Cu,
    0xAAAAu, 0xF0F0u, 0x5A5Au, 0x33CCu, 0x3C3Cu, 0x55AAu, 0x9696u, 0xA55Au,
    0x73CEu, 0x13C8u, 0x324Cu, 0x3BDCu, 0x6996u, 0xC33Cu, 0x9966u, 0x0660u,
    0x0272u, 0x04E4u, 0x4E40u, 0x2720u, 0xC936u, 0x936Cu, 0x39C6u, 0x639Cu,
    0x9336u, 0x9CC6u, 0x817Eu, 0xE718u, 0xCCF0u, 0x0FCCu, 0x7744u, 0xEE22u);

// The texel whose index drops its top bit in subset 1; subset 0's is always texel 0.
const uint ANCHOR2[64] = uint[64](
    15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u,
    15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u,
    15u,  2u,  8u,  2u,  2u,  8u,  8u, 15u,
     2u,  8u,  2u,  2u,  8u,  8u,  2u,  2u,
    15u, 15u,  6u,  8u,  2u,  8u, 15u, 15u,
     2u,  8u,  2u,  2u,  2u, 15u, 15u,  6u,
     6u,  2u,  6u,  8u, 15u, 15u,  2u,  2u,
    15u, 15u, 15u, 15u, 15u,  2u,  2u, 15u);

const uint WEIGHTS2[4] = uint[4](0u, 21u, 43u, 64u);
const uint WEIGHTS3[8] = uint[8](0u, 9u, 18u, 27u, 37u, 46u, 55u, 64u);
const uint WEIGHTS4[16] = uint[16](0u, 4u, 9u, 13u, 17u, 21u, 26u, 30u, 34u, 38u, 43u, 47u, 51u, 55u, 60u, 64u);

const vec4 RGB = vec4(1.0, 1.0, 1.0, 0.0);

uint weightOf(uint indexBits, uint index)
{
    return indexBits == 2u ? WEIGHTS2[index] : indexBits == 3u ? WEIGHTS3[index] : WEIGHTS4[index];
}

// For the texels of mask, the nearest of the values between the decoded endpoints, as the
// hardware interpolates them, into indices (other texels' nibbles are kept), and the
// squared error over the channels in use.
float chooseIndices(uint mask, uvec4 e0, uvec4 e1, uint indexBits, vec4 use, inout uint64_t indices)
{
    const uint count = 1u << indexBits;
    const vec4 a = vec4(e0);
    const vec4 b = vec4(e1);
    float error = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        if ((mask & (1u << i)) == 0u)
        {
            continue;
        }
        const vec4 x = texel(i);
        uint best = 0u;
        float bestError = 1e30;
        for (uint k = 0u; k < count; ++k)
        {
            const uint w = weightOf(indexBits, k);
            const vec4 d = (floor(((64.0 - float(w)) * a + float(w) * b + 32.0) / 64.0) - x) * use;
            const float e = dot(d, d);
            if (e < bestError)
            {
                bestError = e;
                best = k;
            }
        }
        indices = (indices & ~(15ul << (4u * i))) | (uint64_t(best) << (4u * i));
        error += bestError;
    }
    return error;
}

// The endpoints that best fit these indices for the texels of mask, by least squares.
bool refit(uint mask, uint indexBits, uint64_t indices, inout vec4 lo, inout vec4 hi)
{
    float aa = 0.0, ab = 0.0, bb = 0.0;
    vec4 ax = vec4(0.0), bx = vec4(0.0);
    for (uint i = 0u; i < 16u; ++i)
    {
        if ((mask & (1u << i)) != 0u)
        {
            const float t = float(weightOf(indexBits, indexOf(indices, i))) / 64.0;
            const vec4 x = texel(i);
            aa += (1.0 - t) * (1.0 - t);
            ab += (1.0 - t) * t;
            bb += t * t;
            ax += (1.0 - t) * x;
            bx += t * x;
        }
    }
    const float det = aa * bb - ab * ab;
    if (abs(det) < 1e-6)
    {
        return false;
    }
    lo = clamp((ax * bb - bx * ab) / det, 0.0, 255.0);
    hi = clamp((bx * aa - ax * ab) / det, 0.0, 255.0);
    return true;
}

// Mode 6 endpoint: 7 bits a channel and a p-bit for all four; decoded as q * 2 + p.
uvec4 quantize7p(vec4 e, bool opaque, out uint p)
{
    if (opaque)
    {
        p = 1u; // so alpha decodes to exactly 255
        return uvec4(clamp(round((e - 1.0) * 0.5), 0.0, 127.0));
    }
    const uvec4 q0 = uvec4(clamp(round(e * 0.5), 0.0, 127.0));
    const uvec4 q1 = uvec4(clamp(round((e - 1.0) * 0.5), 0.0, 127.0));
    const vec4 d0 = vec4(q0 * 2u) - e;
    const vec4 d1 = vec4(q1 * 2u + 1u) - e;
    p = dot(d1, d1) < dot(d0, d0) ? 1u : 0u;
    return p == 1u ? q1 : q0;
}

float mode6(bool opaque, out uvec4 words)
{
    vec4 lo, hi;
    fitLine(0xFFFFu, opaque ? RGB : vec4(1.0), lo, hi);
    float bestError = 1e30;
    uvec4 best0 = uvec4(0u), best1 = uvec4(0u);
    uint bestP0 = 0u, bestP1 = 0u;
    uint64_t bestIndices = 0ul;
    for (int pass = 0; pass < 2; ++pass)
    {
        if (opaque)
        {
            lo.a = 255.0;
            hi.a = 255.0;
        }
        uint p0, p1;
        const uvec4 q0 = quantize7p(lo, opaque, p0);
        const uvec4 q1 = quantize7p(hi, opaque, p1);
        uint64_t indices = 0ul;
        const float error = chooseIndices(0xFFFFu, q0 * 2u + p0, q1 * 2u + p1, 4u, vec4(1.0), indices);
        if (error < bestError)
        {
            bestError = error;
            best0 = q0;
            best1 = q1;
            bestP0 = p0;
            bestP1 = p1;
            bestIndices = indices;
        }
        if (error == 0.0 || !refit(0xFFFFu, 4u, indices, lo, hi))
        {
            break;
        }
    }
    // Texel 0's index has no top bit: if it needs one, the endpoints trade places.
    if (indexOf(bestIndices, 0u) >= 8u)
    {
        const uvec4 q = best0;
        best0 = best1;
        best1 = q;
        const uint p = bestP0;
        bestP0 = bestP1;
        bestP1 = p;
        bestIndices = ~bestIndices; // 15 - i for every 4-bit index
    }
    Block128 block = Block128(0ul, 0ul);
    uint pos = 0u;
    putBits(block, pos, 1u << 6, 7u);
    for (uint c = 0u; c < 4u; ++c)
    {
        putBits(block, pos, best0[c], 7u);
        putBits(block, pos, best1[c], 7u);
    }
    putBits(block, pos, bestP0, 1u);
    putBits(block, pos, bestP1, 1u);
    for (uint i = 0u; i < 16u; ++i)
    {
        putBits(block, pos, indexOf(bestIndices, i), i == 0u ? 3u : 4u);
    }
    words = blockWords(block);
    return bestError;
}

// Mode 5: colour of 7 bits and alpha of 8, each with 2-bit indices of its own.
float mode5(out uvec4 words)
{
    float alphaLo = 255.0;
    float alphaHi = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        alphaLo = min(alphaLo, texel(i).a);
        alphaHi = max(alphaHi, texel(i).a);
    }
    uint a0 = uint(alphaLo);
    uint a1 = uint(alphaHi);
    uint64_t alphaIndices = 0ul;
    const float alphaError = chooseIndices(0xFFFFu, uvec4(0u, 0u, 0u, a0), uvec4(0u, 0u, 0u, a1), 2u, vec4(0.0, 0.0, 0.0, 1.0), alphaIndices);

    vec4 lo, hi;
    fitLine(0xFFFFu, RGB, lo, hi);
    float bestError = 1e30;
    uvec3 best0 = uvec3(0u), best1 = uvec3(0u);
    uint64_t bestIndices = 0ul;
    for (int pass = 0; pass < 2; ++pass)
    {
        const uvec3 c0 = uvec3(clamp(round(lo.rgb * (127.0 / 255.0)), 0.0, 127.0));
        const uvec3 c1 = uvec3(clamp(round(hi.rgb * (127.0 / 255.0)), 0.0, 127.0));
        uint64_t indices = 0ul;
        const float error = chooseIndices(0xFFFFu, uvec4((c0 << 1u) | (c0 >> 6u), 0u), uvec4((c1 << 1u) | (c1 >> 6u), 0u), 2u, RGB, indices);
        if (error < bestError)
        {
            bestError = error;
            best0 = c0;
            best1 = c1;
            bestIndices = indices;
        }
        if (error == 0.0 || !refit(0xFFFFu, 2u, indices, lo, hi))
        {
            break;
        }
    }
    const uint64_t ALL_THREES = 0x3333333333333333ul; // 3 - i for every 2-bit index
    if (indexOf(bestIndices, 0u) >= 2u)
    {
        const uvec3 c = best0;
        best0 = best1;
        best1 = c;
        bestIndices ^= ALL_THREES;
    }
    if (indexOf(alphaIndices, 0u) >= 2u)
    {
        const uint a = a0;
        a0 = a1;
        a1 = a;
        alphaIndices ^= ALL_THREES;
    }
    Block128 block = Block128(0ul, 0ul);
    uint pos = 0u;
    putBits(block, pos, 1u << 5, 6u);
    putBits(block, pos, 0u, 2u); // no channel rotation
    for (uint c = 0u; c < 3u; ++c)
    {
        putBits(block, pos, best0[c], 7u);
        putBits(block, pos, best1[c], 7u);
    }
    putBits(block, pos, a0, 8u);
    putBits(block, pos, a1, 8u);
    for (uint i = 0u; i < 16u; ++i)
    {
        putBits(block, pos, indexOf(bestIndices, i), i == 0u ? 1u : 2u);
    }
    for (uint i = 0u; i < 16u; ++i)
    {
        putBits(block, pos, indexOf(alphaIndices, i), i == 0u ? 1u : 2u);
    }
    words = blockWords(block);
    return bestError + alphaError;
}

// A quick estimate of how well one line with 8 steps fits the texels of mask: the line
// through their mean along the whole block's axis, its distance to them, and the step
// error of its length. Ranking the 64 partitions this way, rather than fitting each
// subset's own line, keeps mode 1 affordable.
float lineEstimate(uint mask, vec3 axis)
{
    vec3 mean = vec3(0.0);
    float count = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        if ((mask & (1u << i)) != 0u)
        {
            mean += texel(i).rgb;
            count += 1.0;
        }
    }
    mean /= max(count, 1.0);
    float off = 0.0;
    float tMin = 1e30;
    float tMax = -1e30;
    for (uint i = 0u; i < 16u; ++i)
    {
        if ((mask & (1u << i)) != 0u)
        {
            const vec3 d = texel(i).rgb - mean;
            const float t = dot(d, axis);
            off += dot(d, d) - t * t;
            tMin = min(tMin, t);
            tMax = max(tMax, t);
        }
    }
    const float step = (tMax - tMin) / 7.0;
    return off + count * step * step / 12.0;
}

// Mode 1 endpoint: 6 bits a channel and a p-bit shared by the subset's two endpoints.
uvec3 quantize6(vec3 e, uint p)
{
    return uvec3(clamp(round((e * (127.0 / 255.0) - float(p)) * 0.5), 0.0, 63.0));
}

uvec4 decode6(uvec3 q, uint p)
{
    const uvec3 v7 = (q << 1u) | p;
    return uvec4((v7 << 1u) | (v7 >> 6u), 0u);
}

float mode1(out uvec4 words)
{
    vec4 blockLo, blockHi;
    fitLine(0xFFFFu, RGB, blockLo, blockHi);
    const vec3 span = blockHi.rgb - blockLo.rgb;
    const vec3 axis = dot(span, span) > 0.0 ? normalize(span) : vec3(0.57735);
    uint shape = 0u; // the partition ('partition' is reserved in GLSL)
    float bestEstimate = 1e30;
    for (uint p = 0u; p < 64u; ++p)
    {
        const uint mask1 = PARTITIONS2[p];
        const float estimate = lineEstimate(~mask1 & 0xFFFFu, axis) + lineEstimate(mask1, axis);
        if (estimate < bestEstimate)
        {
            bestEstimate = estimate;
            shape = p;
        }
    }
    const uint mask1 = PARTITIONS2[shape];
    uvec3 ends0 = uvec3(0u), ends1 = uvec3(0u), ends2 = uvec3(0u), ends3 = uvec3(0u);
    uint pBit0 = 0u, pBit1 = 0u;
    uint64_t finalIndices = 0ul;
    float total = 0.0;
    for (uint s = 0u; s < 2u; ++s)
    {
        const uint mask = s == 0u ? ~mask1 & 0xFFFFu : mask1;
        vec4 lo, hi;
        fitLine(mask, RGB, lo, hi);
        float best = 1e30;
        uvec3 bestLo = uvec3(0u), bestHi = uvec3(0u);
        uint bestP = 0u;
        uint64_t bestIndices = 0ul;
        for (int pass = 0; pass < 2; ++pass)
        {
            for (uint p = 0u; p < 2u; ++p)
            {
                const uvec3 q0 = quantize6(lo.rgb, p);
                const uvec3 q1 = quantize6(hi.rgb, p);
                uint64_t indices = 0ul;
                const float error = chooseIndices(mask, decode6(q0, p), decode6(q1, p), 3u, RGB, indices);
                if (error < best)
                {
                    best = error;
                    bestLo = q0;
                    bestHi = q1;
                    bestP = p;
                    bestIndices = indices;
                }
            }
            if (best == 0.0 || !refit(mask, 3u, bestIndices, lo, hi))
            {
                break;
            }
        }
        // Texel 0 anchors subset 0, ANCHOR2 subset 1: their index has no top bit.
        const uint anchor = s == 0u ? 0u : ANCHOR2[shape];
        if (indexOf(bestIndices, anchor) >= 4u)
        {
            const uvec3 e = bestLo;
            bestLo = bestHi;
            bestHi = e;
            bestIndices ^= nibbleMask(mask) & 0x7777777777777777ul; // 7 - i for the subset's 3-bit indices
        }
        finalIndices |= bestIndices & nibbleMask(mask);
        if (s == 0u)
        {
            ends0 = bestLo;
            ends1 = bestHi;
            pBit0 = bestP;
        }
        else
        {
            ends2 = bestLo;
            ends3 = bestHi;
            pBit1 = bestP;
        }
        total += best;
    }
    const uint anchor1 = ANCHOR2[shape];
    Block128 block = Block128(0ul, 0ul);
    uint pos = 0u;
    putBits(block, pos, 1u << 1, 2u);
    putBits(block, pos, shape, 6u);
    for (uint c = 0u; c < 3u; ++c)
    {
        putBits(block, pos, ends0[c], 6u);
        putBits(block, pos, ends1[c], 6u);
        putBits(block, pos, ends2[c], 6u);
        putBits(block, pos, ends3[c], 6u);
    }
    putBits(block, pos, pBit0, 1u);
    putBits(block, pos, pBit1, 1u);
    for (uint i = 0u; i < 16u; ++i)
    {
        putBits(block, pos, indexOf(finalIndices, i), i == 0u || i == anchor1 ? 2u : 3u);
    }
    words = blockWords(block);
    return total;
}

void main()
{
    const uint block = loadBlock();
    if (block == ~0u)
    {
        return;
    }
    bool opaque = true;
    for (uint i = 0u; i < 16u; ++i)
    {
        opaque = opaque && (packedTexels[i] >> 24) == 255u;
    }
    uvec4 best;
    const float bestError = mode6(opaque, best);
    // Under about 1.5 levels of error a channel, mode 6 is already invisible: the others
    // cost far more to try than they could gain.
    const float GOOD_ENOUGH = 16.0 * 4.0 * 1.5 * 1.5;
    if (bestError > GOOD_ENOUGH)
    {
        uvec4 other;
        const float otherError = opaque ? mode1(other) : mode5(other);
        if (otherError < bestError)
        {
            best = other;
        }
    }
    EncodeWordsRef words = EncodeWordsRef(push.instances);
    words.words[block * 4u] = best.x;
    words.words[block * 4u + 1u] = best.y;
    words.words[block * 4u + 2u] = best.z;
    words.words[block * 4u + 3u] = best.w;
}
