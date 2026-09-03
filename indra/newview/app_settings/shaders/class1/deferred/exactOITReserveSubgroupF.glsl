// <AS:Chanayane> Exact OIT wave-level node reservation (E7), shared by the
// three EXACT_OIT_SUBGROUP capture-family fragment shaders (capture, emissive,
// PBR glow). Linked as an extra object alongside each caller's own file, the
// same way exactOITCaptureF.glsl is shared today; see fsexactoit.cpp.
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
// Precondition: helper invocations have already returned before calling this
// (atomics issued by helper invocations are dropped by hardware, so a helper
// must never be the lane that performs the wave's atomicAdd), and every
// non-helper lane must reach this call (no early return before it) so the
// ballot sees the whole wave.
uint exact_oit_reserve(bool need)
{
    uvec4 ballot = subgroupBallot(need);
    uint  count  = subgroupBallotBitCount(ballot);
    uint  base   = 0u;
    if (count != 0u)                                   // `count` is wave-uniform
    {
        if (subgroupElect()) base = atomicAdd(oitNodeCount, count);   // lowest active lane
        base = subgroupBroadcastFirst(base);                           // reads that same lane
    }
    if (!need) return 0xffffffffu;
    uint index = base + subgroupBallotExclusiveBitCount(ballot);
    if (index >= oitNodeCapacity) { atomicOr(oitOverflow, 1u); return 0xffffffffu; }
    return index;
}

// Updates oitPad (the max list length seen so far, used to size sort passes)
// with the largest pixel_count in the wave, one atomic per wave instead of
// one per fragment. Every non-helper lane must call this (no early return
// before it) so the wave-wide max sees every active lane's value.
void exact_oit_wave_max_pad(uint pixel_count)
{
    uint wave_max = subgroupMax(pixel_count);
    if (subgroupElect()) atomicMax(oitPad, wave_max);
}
// </AS:Chanayane>
