/**
 * Shared Exact OIT fragment capture implementation.
 */

layout(early_fragment_tests) in;
layout(binding = 0, r32ui) uniform coherent uimage2D oitHeadPointers;
layout(binding = 1, r32ui) uniform coherent uimage2D oitListCounts;

struct OITNode
{
    vec4 color;
    float glow;
    float depth;
    uint next;
    uint blend;
};

layout(std430, binding = 0) buffer OITNodes
{
    OITNode oitNodes[];
};

layout(std430, binding = 1) buffer OITControl
{
    uint oitNodeCount;
    uint oitNodeCapacity;
    uint oitOverflow;
    uint oitPad;
};

uniform uint oitBlendFactors;
uniform float oitGlow;
uniform int oitDiscardNoOp;

void exact_oit_store(vec4 color)
{
    // Standard alpha with exact zero source alpha and glow is a complete no-op.
    // Reject it before allocation so invisible card texels create no list work.
    const uint standard_alpha_blend = 7u | (9u << 8u) | (1u << 16u) | (9u << 24u);
    if (oitDiscardNoOp != 0 &&
        oitBlendFactors == standard_alpha_blend &&
        color.a == 0.0 &&
        oitGlow == 0.0)
    {
        return;
    }

    uint index = atomicAdd(oitNodeCount, 1u);
    if (index >= oitNodeCapacity)
    {
        atomicOr(oitOverflow, 1u);
        return;
    }

    oitNodes[index].color = color;
    oitNodes[index].glow = oitGlow;
    oitNodes[index].depth = gl_FragCoord.z;
    oitNodes[index].blend = oitBlendFactors;
    oitNodes[index].next = imageAtomicExchange(oitHeadPointers, ivec2(gl_FragCoord.xy), index);

    uint pixel_count = imageAtomicAdd(oitListCounts, ivec2(gl_FragCoord.xy), 1u) + 1u;
    atomicMax(oitPad, pixel_count);
}
