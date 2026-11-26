/**
 * asrShadowDemandC.glsl
 * What the cached shadow maps show of each texture, into this frame's texture feedback
 * (GpuShadowViews::demand, drawn by asrShadowMaskedF.glsl). Streaming then keeps a texture only
 * a shadow shows, such as a leaf canopy off screen, at the size its shadow was cut from. First
 * the bits of cascades the fit redraws whole, or dropped, are cleared (GpuShadowViews::keep).
 *
 * A thread per texture slot, over the whole GPU: in the fit's single workgroup this cost
 * 0.18 ms.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 256) in;

void main()
{
    const uint slot = gl_GlobalInvocationID.x;
    ViewRef view = ViewRef(push.view);
    ShadowViewsRef shadows = ShadowViewsRef(view.shadowViews);
    if (slot >= SHADOW_DEMAND_SLOTS)
    {
        return;
    }
    uvec2 bits = uvec2(shadows.demand[slot * 2u], shadows.demand[slot * 2u + 1u]);
    if (bits == uvec2(0u))
    {
        return;
    }
    const uint keep = shadows.keep;
    const uvec2 kept = bits & uvec2(((keep & 1u) != 0u ? 0xFFFFu : 0u) | ((keep & 2u) != 0u ? 0xFFFF0000u : 0u),
                                    ((keep & 4u) != 0u ? 0xFFFFu : 0u) | ((keep & 8u) != 0u ? 0xFFFF0000u : 0u));
    if (kept != bits)
    {
        bits = kept;
        shadows.demand[slot * 2u] = bits.x;
        shadows.demand[slot * 2u + 1u] = bits.y;
    }
    const uint feedback = view.feedback;
    if (feedback == 0u || bits == uvec2(0u))
    {
        return;
    }
    const int level = max(max(findMSB(bits.x & 0xFFFFu), findMSB(bits.x >> 16)), max(findMSB(bits.y & 0xFFFFu), findMSB(bits.y >> 16)));
    const uint word = ((feedback >> 4) << FEEDBACK_LEVEL_BITS) | uint(level + 1);
    FeedbackRef seen = FeedbackRef(push.feedback);
    if (seen.words[slot] < word)
    {
        atomicMax(seen.words[slot], word);
    }
}
