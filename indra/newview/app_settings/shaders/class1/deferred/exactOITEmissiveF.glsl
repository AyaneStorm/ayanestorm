// <AS:Chanayane> Exact OIT ordered emissive capture
/*[EXTRA_CODE_HERE]*/

layout(early_fragment_tests) in;
layout(binding = 0, r32ui) uniform coherent uimage2D oitHeadPointers;
layout(binding = 1, r32ui) uniform coherent uimage2D oitListCounts;
// <AS:Chanayane> Lossless 32-byte node: scalar glow and index-derived sequence.
struct OITNode { vec4 color; float glow; float depth; uint next; uint blend; };
// </AS:Chanayane>
layout(std430, binding = 0) buffer OITNodes { OITNode oitNodes[]; };
layout(std430, binding = 1) buffer OITControl { uint oitNodeCount; uint oitNodeCapacity; uint oitOverflow; uint oitPad; };
void exact_oit_store_glow(float glow)
{
    uint index = atomicAdd(oitNodeCount, 1u);
    if (index >= oitNodeCapacity) { atomicOr(oitOverflow, 1u); return; }
    oitNodes[index].color = vec4(0.0);
    oitNodes[index].glow = glow;
    oitNodes[index].depth = gl_FragCoord.z;
    oitNodes[index].blend = 0xffffffffu;
    oitNodes[index].next = imageAtomicExchange(oitHeadPointers, ivec2(gl_FragCoord.xy), index);
    // <AS:Chanayane> Glow nodes participate in the same exact ordered list count.
    uint pixel_count = imageAtomicAdd(oitListCounts, ivec2(gl_FragCoord.xy), 1u) + 1u;
    atomicMax(oitPad, pixel_count);
    // </AS:Chanayane>
}

in vec4 vertex_color;
in vec2 vary_texcoord0;

void main()
{
    float glow = diffuseLookup(vary_texcoord0.xy).a * vertex_color.a;
    // A zero glow node adds exactly 0 in the composite (glow += node.glow):
    // skip allocating it. Exact; matches vanilla's additive (ONE, ONE) blend of 0.0.
    if (glow == 0.0) return;
    exact_oit_store_glow(glow);
}
// </AS:Chanayane>
