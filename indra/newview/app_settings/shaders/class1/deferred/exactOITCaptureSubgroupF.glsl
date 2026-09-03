// Shared Exact OIT fragment capture implementation: wave-level node allocation
// variant (E7). All direct subgroup calls, OITControl, and oitPad live in the
// linked exactOITReserveSubgroupF.glsl object instead of here (see
// fsexactoit.cpp), so this file needs no #extension of its own.
/*[EXTRA_CODE_HERE]*/

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

uniform uint oitBlendFactors;

// Both declared in the linked exactOITReserveSubgroupF.glsl object, which
// owns the only OITControl declaration (a binding declared in two linked
// objects is a link error). exact_oit_wave_max_pad() lets this file update
// oitPad without redeclaring the block itself.
uint exact_oit_reserve(bool need);
void exact_oit_wave_max_pad(uint pixel_count);

void exact_oit_store(vec4 color)
{
    // All texture sampling (which needs helpers for derivatives) happened
    // before this call, so helpers can leave now. They then count as inactive
    // in every subgroup operation inside exact_oit_reserve() and
    // exact_oit_wave_max_pad() below.
    if (gl_HelperInvocation) return;

    bool need = true;
#ifdef EXACT_OIT_DISCARD_NOOP
    // Standard alpha with exact zero source alpha is a complete no-op (glow
    // is always 0 on this path; the glow shaders use their own store
    // function and never reach here). Reject it before allocation so
    // invisible card texels create no list work.
    const uint standard_alpha_blend = 7u | (9u << 8u) | (1u << 16u) | (9u << 24u);
    need = !(oitBlendFactors == standard_alpha_blend && color.a == 0.0);
#endif

    uint index = exact_oit_reserve(need);   // every non-helper lane must reach this call (no early return before it)
    if (index == 0xffffffffu) return;

    oitNodes[index].color = color;
    oitNodes[index].glow = 0.0;
    oitNodes[index].depth = gl_FragCoord.z;
    oitNodes[index].blend = oitBlendFactors;
    oitNodes[index].next = imageAtomicExchange(oitHeadPointers, ivec2(gl_FragCoord.xy), index);

    uint pixel_count = imageAtomicAdd(oitListCounts, ivec2(gl_FragCoord.xy), 1u) + 1u;
    exact_oit_wave_max_pad(pixel_count);   // some lanes may have exited above; that's fine
}
