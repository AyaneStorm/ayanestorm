/**
 * asrSceneMacOITV.glsl
 * Vertices of Mac OIT's three passes: blended clusters from the cull's blended lists, drawn as
 * the visibility pass draws its own, with what the fragments need to find their colour.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

invariant gl_Position; // see scenePosition()

layout(location = 0) out vec2 vary_uv;
layout(location = 1) flat out uint vary_texture;
layout(location = 2) flat out vec4 vary_color;
layout(location = 3) out vec3 vary_normal;   // world
layout(location = 4) out vec3 vary_toCamera; // world, for two-sided faces

void main()
{
    ViewRef view = ViewRef(push.view);
    const uint entry = view.listBase[push.list] + uint(gl_InstanceIndex);
    const uint triangle = uint(gl_VertexIndex) / 3u;
    const ListEntry visible = ListRef(push.visible).items[entry];
    const Cluster cluster = ClustersRef(push.clusters).items[visible.cluster];
    vary_uv = vec2(0.0);
    vary_texture = 0u;
    vary_color = vec4(0.0);
    vary_normal = vec3(0.0, 0.0, 1.0);
    vary_toCamera = vec3(0.0, 0.0, 1.0);
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
    const vec3 packed = packedPosition(cluster.positions, vertex);
    // The same position and rank as the visibility pass, so coplanar surfaces order the same way.
    gl_Position = scenePosition(view, instance, packed);
    vary_toCamera = view.camera.xyz - transformPoint(instance.rows, packed);

    const Material material = MaterialsRef(push.materials).items[instance.material];
    WordsRef attributes = WordsRef(cluster.attributes);
    // Lit as the resolve lights opaque surfaces: skinned normals turn with the palette.
    vec3 objectNormal = attributeNormal(attributes.words[vertex * 4u]);
    if ((instance.flags & INSTANCE_SKINNED) != 0u)
    {
        const Binding binding = BindingsRef(view.skinTables[SKIN_TABLE_BINDINGS]).items[instance.binding];
        objectNormal = skinNormal(view, binding, vertex, objectNormal);
    }
    vary_normal = worldNormalOf(instance, objectNormal);
    vec2 faceUV = vec2(uintBitsToFloat(attributes.words[vertex * 4u + 2u]), uintBitsToFloat(attributes.words[vertex * 4u + 3u]));
    if (material.mapping == MATERIAL_PLANAR)
    {
        faceUV = planarUV(attributeNormal(attributes.words[vertex * 4u]), packed * material.planarScale + material.planarOffset);
    }
    vary_uv = vec2(dot(material.uvRow0, vec3(faceUV, 1.0)), dot(material.uvRow1, vec3(faceUV, 1.0)));
    vary_texture = material.texture;
    vary_color = material.color;
}
