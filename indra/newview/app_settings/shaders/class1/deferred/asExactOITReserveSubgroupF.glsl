// AyaneStorm OIT shader. Author: chanayane@firestorm.
// <AS:Chanayane> Exact OIT wave-level node reservation (E7), shared by the
// three EXACT_OIT_SUBGROUP capture-family fragment shaders (capture, emissive,
// PBR glow). Linked as an extra object alongside each caller's own file, the
// same way asExactOITCaptureF.glsl is shared today; see asexactoit.cpp.
//
// Declares only OITControl: OITNodes and the head/count images are declared
// by the caller, since a binding declared in two linked objects is a link
// error (this bit us for diffuseLookup earlier in E7 — see
// llglslshader.cpp's oit_capture_library special case).
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_arithmetic : require
/*[EXTRA_CODE_HERE]*/

layout(std430, binding = 1) buffer OITControl
{
    uint oitNodeCount;
    uint oitNodeCapacity;
    uint oitOverflow;
    uint oitPad;
};

// Returns 0xffffffffu when no node could be reserved (rejected or overflow).
// Precondition: every non-helper lane must reach this call (no early return
// before it) so the ballot sees the whole wave.
//
// The wave's single atomicAdd is performed by the LOWEST LANE THAT NEEDS A
// NODE (subgroupBallotFindLSB of the `need` ballot), and its result is
// published with subgroupMax() over lanes that otherwise hold 0. This is
// deliberately NOT subgroupElect() + subgroupBroadcastFirst(): those pick
// the lowest *active* lane, which is only a real, needing fragment if helper
// invocations have already left the wave -- and atomics issued by helper
// invocations are dropped by hardware. Whether a returned helper is still
// "active" for a subgroup op is implementation-defined; on a driver where
// it is, an elected helper's atomicAdd was dropped, `base` broadcast as 0,
// and every lane in the wave got index 0 + rank: duplicate node indices
// across all waves, i.e. cross-linked lists (corrupted transparency, cyclic
// traversals, TDR). Folding !gl_HelperInvocation into `need` makes the
// allocator correct whether or not helpers are still in the wave.
uint exact_oit_reserve(bool need)
{
    need = need && !gl_HelperInvocation;
    uvec4 ballot = subgroupBallot(need);
    uint  count  = subgroupBallotBitCount(ballot);
    uint  base   = 0u;
    if (need && subgroupBallotFindLSB(ballot) == gl_SubgroupInvocationID)
    {
        base = atomicAdd(oitNodeCount, count);     // exactly one real lane per wave
    }
    base = subgroupMax(base);                      // all other lanes hold 0
    if (!need) return 0xffffffffu;
    uint index = base + subgroupBallotExclusiveBitCount(ballot);
    if (index >= oitNodeCapacity) { atomicOr(oitOverflow, 1u); return 0xffffffffu; }
    return index;
}

// Updates oitPad (the max list length seen so far, used to size sort passes)
// with the largest pixel_count in the wave, one atomic per wave instead of
// one per fragment. Every non-helper lane must call this (no early return
// before it) so the wave-wide max sees every active lane's value. Same
// election rule as exact_oit_reserve(): the publishing lane is the lowest
// NON-HELPER lane, never subgroupElect(), so the atomicMax cannot be dropped.
void exact_oit_wave_max_pad(uint pixel_count)
{
    uint  wave_max = subgroupMax(pixel_count);
    uvec4 real     = subgroupBallot(!gl_HelperInvocation);
    if (!gl_HelperInvocation && subgroupBallotFindLSB(real) == gl_SubgroupInvocationID)
    {
        atomicMax(oitPad, wave_max);
    }
}
// </AS:Chanayane>
