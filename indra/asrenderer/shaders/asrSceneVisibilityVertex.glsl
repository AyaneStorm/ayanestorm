/**
 * asrSceneVisibilityVertex.glsl
 * Vertices of the visibility pass. An instance of the draw is one visible cluster, and
 * vertices are read from memory, so one draw covers everything in a list. A cluster
 * smaller than its list's size collapses its spare triangles to a point. Masked surfaces
 * define MASKED, and also pass on what their fragments need to find holes.
 */
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

invariant gl_Position; // see scenePosition()

layout(location = 0) flat out uint vary_visibility;
#ifdef MASKED
layout(location = 1) out vec2 vary_uv;
layout(location = 2) flat out uint vary_texture;
layout(location = 3) flat out vec2 vary_alpha; // the material's alpha, and its cutoff
#endif

void main()
{
    ViewRef view = ViewRef(push.view);
    const uint entry = view.listBase[push.list] + uint(gl_InstanceIndex);
    const uint triangle = uint(gl_VertexIndex) / 3u;
    const ListEntry visible = ListRef(push.visible).items[entry];
    const Cluster cluster = ClustersRef(push.clusters).items[visible.cluster];
    vary_visibility = (entry << 6) | triangle;
#ifdef MASKED
    vary_uv = vec2(0.0);
    vary_texture = 0u;
    vary_alpha = vec2(0.0);
#endif
    if (triangle >= cluster.triangles)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const Instance instance = InstancesRef(push.instances).items[visible.instance];
    uint corner = uint(gl_VertexIndex) % 3u;
    if ((instance.flags & INSTANCE_FLIPPED) != 0u && corner != 0u)
    {
        corner = 3u - corner;
    }
    const uint vertex = clusterIndex(cluster.indices, triangle * 3u + corner);
    gl_Position = scenePosition(view, instance, packedPosition(cluster.positions, vertex));

#ifdef MASKED
    const Material material = MaterialsRef(push.materials).items[instance.material];
    WordsRef attributes = WordsRef(cluster.attributes);
    const vec3 faceUV = vec3(uintBitsToFloat(attributes.words[vertex * 4u + 2u]), uintBitsToFloat(attributes.words[vertex * 4u + 3u]), 1.0);
    vary_uv = vec2(dot(material.uvRow0, faceUV), dot(material.uvRow1, faceUV));
    vary_texture = material.texture;
    vary_alpha = vec2(material.color.a, material.alphaCutoff);
#endif
}
