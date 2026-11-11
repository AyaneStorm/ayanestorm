/**
 * asrEncodeCommon.glsl
 * What the texture encoders share. A texture being compressed is a buffer of RGBA8 texels,
 * every mip one after the other; one thread encodes one 4x4 block of one mip.
 * ScenePush carries it: view is the mip's texels, instances where the output goes, list
 * the mip's width (low 16 bits) and height (high 16 bits), phase the encoder's option.
 * See doc/ayanestorm-new-renderer-texture-compression.md.
 */
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(buffer_reference, std430) readonly buffer EncodeTexelsRef { uint texels[]; };
layout(buffer_reference, std430) writeonly buffer EncodeWordsRef { uint words[]; };

const uint ENCODE_GROUP_SIZE = 64u;

uvec2 encodeSize()
{
    return uvec2(push.list & 0xFFFFu, push.list >> 16);
}

uvec2 encodeBlocks()
{
    return (encodeSize() + 3u) / 4u;
}

// The block's 16 texels, row by row, packed RGBA8: unpacked as they are used, so a thread
// keeps 16 registers for them, not 64. Arrays a thread indexes and too many registers
// spill to slow memory, which made BC7 ten times slower. Past the edge of a mip under 4
// texels across, the edge texel repeats: never sampled, and it adds no colour.
uint packedTexels[16];

vec4 texel(uint i)
{
    return vec4((uvec4(packedTexels[i]) >> uvec4(0u, 8u, 16u, 24u)) & 0xFFu);
}

// Returns the block's index, or ~0u past the last block.
uint loadBlock()
{
    const uvec2 size = encodeSize();
    const uvec2 blocks = encodeBlocks();
    const uint block = gl_GlobalInvocationID.x;
    if (block >= blocks.x * blocks.y)
    {
        return ~0u;
    }
    const uvec2 origin = uvec2(block % blocks.x, block / blocks.x) * 4u;
    EncodeTexelsRef texels = EncodeTexelsRef(push.view);
    for (uint i = 0u; i < 16u; ++i)
    {
        const uvec2 at = min(origin + uvec2(i & 3u, i >> 2), size - 1u);
        packedTexels[i] = texels.texels[at.y * size.x + at.x];
    }
    return block;
}

// The line through the texels of mask (a bit per texel) that fits them best, along the
// channels use selects: their mean and principal axis, by power iteration on the
// covariance. lo and hi are its ends over those texels, inside 0 to 255.
void fitLine(uint mask, vec4 use, out vec4 lo, out vec4 hi)
{
    vec4 mean = vec4(0.0);
    float count = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        if ((mask & (1u << i)) != 0u)
        {
            mean += texel(i);
            count += 1.0;
        }
    }
    mean = mean * use / max(count, 1.0);

    mat4 covariance = mat4(0.0);
    for (uint i = 0u; i < 16u; ++i)
    {
        if ((mask & (1u << i)) != 0u)
        {
            const vec4 d = (texel(i) - mean) * use;
            covariance += outerProduct(d, d);
        }
    }
    // Start from the channel that varies most, so the axis keeps its sign across channels.
    vec4 axis = covariance[0];
    float most = covariance[0][0];
    for (int c = 1; c < 4; ++c)
    {
        if (covariance[c][c] > most)
        {
            most = covariance[c][c];
            axis = covariance[c];
        }
    }
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        const vec4 next = covariance * axis;
        const float length2 = dot(next, next);
        axis = length2 > 1e-12 ? next * inversesqrt(length2) : axis;
    }
    const float axisLength2 = dot(axis, axis);
    if (most <= 1e-6 || axisLength2 <= 1e-12)
    {
        lo = mean;
        hi = mean;
        return;
    }
    axis *= inversesqrt(axisLength2);

    float tMin = 1e30;
    float tMax = -1e30;
    for (uint i = 0u; i < 16u; ++i)
    {
        if ((mask & (1u << i)) != 0u)
        {
            const float t = dot((texel(i) - mean) * use, axis);
            tMin = min(tMin, t);
            tMax = max(tMax, t);
        }
    }
    lo = clamp(mean + axis * tMin, 0.0, 255.0);
    hi = clamp(mean + axis * tMax, 0.0, 255.0);
}

// A 128-bit block as two halves, written least significant bit first, with no indexing.
struct Block128
{
    uint64_t lo;
    uint64_t hi;
};

void putBits(inout Block128 block, inout uint pos, uint value, uint bits)
{
    const uint64_t v = uint64_t(value & ((1u << bits) - 1u)); // bits is under 32
    if (pos >= 64u)
    {
        block.hi |= v << (pos - 64u);
    }
    else
    {
        block.lo |= v << pos;
        if (pos + bits > 64u)
        {
            block.hi |= v >> (64u - pos);
        }
    }
    pos += bits;
}

uvec4 blockWords(Block128 block)
{
    return uvec4(uint(block.lo), uint(block.lo >> 32), uint(block.hi), uint(block.hi >> 32));
}

// Sixteen indices of up to 4 bits, texel i at bits 4i to 4i+3.
uint indexOf(uint64_t indices, uint i)
{
    return uint(indices >> (4u * i)) & 15u;
}

// Texels of mask spread to a nibble each: a value of 0xF for each texel in it.
uint64_t nibbleMask(uint mask)
{
    uint64_t spread = 0ul;
    for (uint i = 0u; i < 16u; ++i)
    {
        if ((mask & (1u << i)) != 0u) spread |= 15ul << (4u * i);
    }
    return spread;
}
