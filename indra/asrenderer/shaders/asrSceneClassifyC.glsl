/**
 * asrSceneClassifyC.glsl
 * Marks which material kinds each 8 by 8 tile of the visibility buffer holds, and adds the
 * tile to each of those kinds' lists. The resolve then runs once per kind, each over only its
 * tiles with a shader made for that kind, so one GPU wave never mixes the sky, terrain and
 * surfaces' code (Nanite's material classification, SIGGRAPH 2021). A mixed tile is resolved
 * once per kind it holds, each pass taking only its pixels. The dispatch counts start at 0
 * (filled before this pass).
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 0, r32ui) uniform readonly uimage2D visibility;

shared uint sharedKinds;

// The kind the resolve gives this pixel. Entries it rejects go to surfaces, whose pass reports them.
uint pixelKind(ViewRef view, ivec2 pixel)
{
    const uint id = imageLoad(visibility, pixel).r;
    if (id == 0xFFFFFFFFu)
    {
        return RESOLVE_KIND_SKY;
    }
    const uint entry = id >> 6;
    if (entry >= view.visibleEnd)
    {
        return RESOLVE_KIND_SURFACE;
    }
    const ListEntry visible = ListRef(push.visible).items[entry];
    if (visible.instance >= view.instanceEnd || visible.cluster >= view.clusterEnd)
    {
        return RESOLVE_KIND_SURFACE;
    }
    const uint material = InstancesRef(push.instances).items[visible.instance].material;
    if (material >= view.materialEnd)
    {
        return RESOLVE_KIND_SURFACE;
    }
    return MaterialsRef(push.materials).items[material].mapping == MATERIAL_TERRAIN ? RESOLVE_KIND_TERRAIN : RESOLVE_KIND_SURFACE;
}

void main()
{
    ViewRef view = ViewRef(push.view);
    if (gl_LocalInvocationIndex == 0u)
    {
        sharedKinds = 0u;
    }
    barrier();
    const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x < int(view.pixelSize.x) && pixel.y < int(view.pixelSize.y))
    {
        atomicOr(sharedKinds, 1u << pixelKind(view, pixel));
    }
    barrier();
    if (gl_LocalInvocationIndex != 0u)
    {
        return;
    }
    ResolveTilesRef tiles = ResolveTilesRef(view.resolveTiles);
    const uint capacity = tileCapacity(view);
    const uint packed = gl_WorkGroupID.x | (gl_WorkGroupID.y << 16);
    for (uint kind = 0u; kind < RESOLVE_KINDS; ++kind)
    {
        if ((sharedKinds & (1u << kind)) != 0u)
        {
            const uint slot = atomicAdd(tiles.dispatches[kind * 4u], 1u);
            tiles.tiles[kind * capacity + slot] = packed;
            // Every tile writes the same 1s, so the order does not matter.
            tiles.dispatches[kind * 4u + 1u] = 1u;
            tiles.dispatches[kind * 4u + 2u] = 1u;
        }
    }
}
