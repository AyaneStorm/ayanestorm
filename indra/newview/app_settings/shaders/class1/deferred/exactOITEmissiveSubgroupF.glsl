// <AS:Chanayane> Exact OIT ordered emissive capture: wave-level node allocation
// variant. exact_oit_reserve() and OITControl are declared in the linked
// exactOITReserveSubgroupF.glsl object instead of here; see fsexactoit.cpp.
/*[EXTRA_CODE_HERE]*/

layout(early_fragment_tests) in;
layout(binding = 0, r32ui) uniform coherent uimage2D oitHeadPointers;
layout(binding = 1, r32ui) uniform coherent uimage2D oitListCounts;
// <AS:Chanayane> Lossless 32-byte node: scalar glow and index-derived sequence.
struct OITNode { vec4 color; float glow; float depth; uint next; uint blend; };
// </AS:Chanayane>
layout(std430, binding = 0) buffer OITNodes { OITNode oitNodes[]; };
// Both declared in the linked exactOITReserveSubgroupF.glsl object, which owns
// the only OITControl declaration (a binding declared in two linked objects
// is a link error).
uint exact_oit_reserve(bool need);
void exact_oit_wave_max_pad(uint pixel_count);
// <AS:Chanayane> The caller (main(), below) already filters glow==0 before
// reaching here, so every lane that calls this function needs a node.
void exact_oit_store_glow(float glow)
{
    if (gl_HelperInvocation) return;
    uint index = exact_oit_reserve(true);
    if (index == 0xffffffffu) return;
    oitNodes[index].color = vec4(0.0);
    oitNodes[index].glow = glow;
    oitNodes[index].depth = gl_FragCoord.z;
    oitNodes[index].blend = 0xffffffffu;
    oitNodes[index].next = imageAtomicExchange(oitHeadPointers, ivec2(gl_FragCoord.xy), index);
    // Glow nodes participate in the same exact ordered list count.
    uint pixel_count = imageAtomicAdd(oitListCounts, ivec2(gl_FragCoord.xy), 1u) + 1u;
    exact_oit_wave_max_pad(pixel_count);
}
// </AS:Chanayane>

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
