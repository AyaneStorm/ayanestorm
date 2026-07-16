// <AS:Chanayane> Exact OIT ordered emissive capture
/*[EXTRA_CODE_HERE]*/

layout(early_fragment_tests) in;
layout(binding = 0, r32ui) uniform coherent uimage2D oitHeadPointers;
struct OITNode { vec4 color; vec4 glow; float depth; uint next; uint blend; uint sequence; };
layout(std430, binding = 0) buffer OITNodes { OITNode oitNodes[]; };
layout(std430, binding = 1) buffer OITControl { uint oitNodeCount; uint oitNodeCapacity; uint oitOverflow; uint oitPad; };
void exact_oit_store_glow(float glow)
{
    uint index = atomicAdd(oitNodeCount, 1u);
    if (index >= oitNodeCapacity) { atomicOr(oitOverflow, 1u); return; }
    oitNodes[index].color = vec4(0.0);
    oitNodes[index].glow = vec4(glow, 0.0, 0.0, 0.0);
    oitNodes[index].depth = gl_FragCoord.z;
    oitNodes[index].blend = 0xffffffffu;
    oitNodes[index].sequence = index;
    oitNodes[index].next = imageAtomicExchange(oitHeadPointers, ivec2(gl_FragCoord.xy), index);
}

in vec4 vertex_color;
in vec2 vary_texcoord0;

void main()
{
    exact_oit_store_glow(diffuseLookup(vary_texcoord0.xy).a * vertex_color.a);
}
// </AS:Chanayane>
