/**
 * asrSceneSkinDemand.glsl
 * Culling admits deformation work only after a cluster gets a draw-list entry.
 * The first consumer wins: a late cluster can reuse an early deformation, while
 * a binding hidden by previous depth gets another chance against current depth.
 * Author: chanayane@firestorm
 */
const uint SKIN_DEMAND_NONE = 0u;
const uint SKIN_DEMAND_EARLY = 1u;
const uint SKIN_DEMAND_LATE = 2u;

layout(buffer_reference, std430) buffer SkinDemandRef { Binding items[]; };

void requestSkin(ViewRef view, Instance instance, bool late)
{
    if ((instance.flags & INSTANCE_SKINNED) == 0u) return;
    atomicCompSwap(SkinDemandRef(view.skinTables[SKIN_TABLE_BINDINGS]).items[instance.binding].skinDemand,
                   SKIN_DEMAND_NONE, late ? SKIN_DEMAND_LATE : SKIN_DEMAND_EARLY);
}
