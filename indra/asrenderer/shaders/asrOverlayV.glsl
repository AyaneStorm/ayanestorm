/**
 * asrOverlayV.glsl
 * Vertices of the overlay draws: the selection mask, from the cull's selection list in
 * the scene's view, and the HUD pass, from the CPU's sorted HUD list in the HUD view. One
 * instance per list entry, from push.list on, so the list's order is the draw order.
 * Author: chanayane@firestorm
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

invariant gl_Position; // the mask must land on the scene's own depth, see scenePosition()

layout(location = 0) out vec2 vary_uv;
layout(location = 1) flat out uint vary_texture;
layout(location = 2) flat out vec4 vary_color;
layout(location = 3) flat out uint vary_flags;
layout(location = 4) flat out float vary_cutoff;

void main()
{
    ViewRef view = ViewRef(push.view);
    const uint triangle = uint(gl_VertexIndex) / 3u;
    const ListEntry entry = ListRef(push.visible).items[push.list + uint(gl_InstanceIndex)];
    const Cluster cluster = ClustersRef(push.clusters).items[entry.cluster];
    vary_uv = vec2(0.0);
    vary_texture = 0u;
    vary_color = vec4(0.0);
    vary_flags = 0u;
    vary_cutoff = 0.0;
    if (triangle >= cluster.triangles)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const Instance instance = InstancesRef(push.instances).items[entry.instance];
    uint corner = uint(gl_VertexIndex) % 3u;
    if ((instance.flags & INSTANCE_FLIPPED) != 0u && corner != 0u)
    {
        corner = 3u - corner;
    }
    const uint vertex = clusterIndex(cluster.indices, triangle * 3u + corner);
    const vec3 packed = packedPosition(cluster.positions, vertex);
    gl_Position = scenePosition(view, instance, packed);

    const Material material = MaterialsRef(push.materials).items[instance.material];
    WordsRef attributes = WordsRef(cluster.attributes);
    vec2 faceUV = vec2(uintBitsToFloat(attributes.words[vertex * 4u + 2u]), uintBitsToFloat(attributes.words[vertex * 4u + 3u]));
    if (material.mapping == MATERIAL_PLANAR)
    {
        faceUV = planarUV(attributeNormal(attributes.words[vertex * 4u]), packed * material.planarScale + material.planarOffset);
    }
    vary_uv = vec2(dot(material.uvRow0, vec3(faceUV, 1.0)), dot(material.uvRow1, vec3(faceUV, 1.0)));
    vary_texture = material.texture;
    vary_color = material.color;
    vary_flags = instance.flags;
    vary_cutoff = material.alphaCutoff;
}
