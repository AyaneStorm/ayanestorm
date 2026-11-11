/**
 * asrEncodeBC45C.glsl
 * BC4 (phase 1: red) and BC5 (phase 2: red, then green), a block a thread. Each channel
 * takes its block's lowest and highest values as endpoints, in the eight-value mode, and
 * each texel the nearest of the eight: for a single channel that is close to the best.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrEncodeCommon.glsl"

layout(local_size_x = ENCODE_GROUP_SIZE) in;

uvec2 encodeChannel(uint channel)
{
    float lowest = 255.0;
    float highest = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        lowest = min(lowest, texel(i)[channel]);
        highest = max(highest, texel(i)[channel]);
    }
    // Endpoint 0 above endpoint 1 selects the eight-value mode: indices 0 and 1 are the
    // endpoints, 2 to 7 the six steps from endpoint 0 to endpoint 1.
    const uint e0 = uint(highest);
    const uint e1 = uint(lowest);
    uint64_t bits = uint64_t(e0) | (uint64_t(e1) << 8);
    if (e0 > e1)
    {
        const float range = highest - lowest;
        for (uint i = 0u; i < 16u; ++i)
        {
            const uint step = uint(round((highest - texel(i)[channel]) / range * 7.0)); // 0 at e0, 7 at e1
            const uint index = step == 0u ? 0u : step == 7u ? 1u : step + 1u;
            bits |= uint64_t(index) << (16u + 3u * i);
        }
    }
    return uvec2(uint(bits), uint(bits >> 32));
}

void main()
{
    const uint block = loadBlock();
    if (block == ~0u)
    {
        return;
    }
    EncodeWordsRef words = EncodeWordsRef(push.instances);
    const uint channels = push.phase;
    for (uint channel = 0u; channel < channels; ++channel)
    {
        const uvec2 encoded = encodeChannel(channel);
        const uint at = (block * channels + channel) * 2u;
        words.words[at] = encoded.x;
        words.words[at + 1u] = encoded.y;
    }
}
