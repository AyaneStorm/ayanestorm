/**
 * asrSceneVisibilityVertex.glsl
 * Vertices of the visibility pass. An instance of the draw is one visible cluster, and
 * vertices are read from memory, so one draw covers everything in a list. A cluster
 * smaller than its list's size collapses its spare triangles to a point. Masked surfaces
 * define MASKED, and also pass on what their fragments need to find holes.
 *
 * SHADOW: the shadow draw. An entry of a shadow list is one cluster in one shadow view, its
 * view above SHADOW_VIEW_SHIFT, and the vertex goes to that view's layer, so one draw fills
 * every cascade. Nothing but depth is written.
 */
#extension GL_GOOGLE_include_directive : require
#ifdef SHADOW
#extension GL_ARB_shader_viewport_layer_array : require
#endif
#include "asrSceneCommon.glsl"

invariant gl_Position; // see scenePosition()

#ifndef SHADOW
layout(location = 0) flat out uint vary_visibility;
#endif
#ifdef MASKED
layout(location = 1) out vec2 vary_uv;
layout(location = 2) flat out uint vary_texture;
layout(location = 3) flat out vec2 vary_alpha; // the material's alpha, and its cutoff
#endif

void main()
{
    ViewRef view = ViewRef(push.view);
    const uint triangle = uint(gl_VertexIndex) / 3u;
#ifdef SHADOW
    // Static entries from the list's start, moving ones from its end backwards (drawShadow()).
    const uint base = view.shadowListBase[push.list];
    const uint end = push.list + 1u < LISTS ? view.shadowListBase[push.list + 1u] : view.shadowListEnd;
    const uint entry = push.phase == SHADOW_DRAW_MOVING ? end - 1u - uint(gl_InstanceIndex) : base + uint(gl_InstanceIndex);
    const ListEntry visible = ListRef(view.shadowLists).items[entry];
    const uint instanceIndex = visible.instance & SHADOW_INSTANCE_MASK;
    const ShadowView shadow = ShadowViewsRef(view.shadowViews).views[visible.instance >> SHADOW_VIEW_SHIFT];
    gl_Layer = int(shadow.layer);
#else
    const uint entry = view.listBase[push.list] + uint(gl_InstanceIndex);
    const ListEntry visible = ListRef(push.visible).items[entry];
    const uint instanceIndex = visible.instance;
    vary_visibility = (entry << 6) | triangle;
#endif
    const Cluster cluster = ClustersRef(push.clusters).items[visible.cluster];
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

    const Instance instance = InstancesRef(push.instances).items[instanceIndex];
    uint corner = uint(gl_VertexIndex) % 3u;
    if ((instance.flags & INSTANCE_FLIPPED) != 0u && corner != 0u)
    {
        corner = 3u - corner;
    }
    const uint vertex = clusterIndex(cluster.indices, triangle * 3u + corner);
#ifdef SHADOW
    gl_Position = shadowPosition(view, shadow.viewProjection, instance, packedPosition(cluster.positions, vertex));
#else
    gl_Position = scenePosition(view, instance, packedPosition(cluster.positions, vertex));
#endif

#ifdef MASKED
    const Material material = MaterialsRef(push.materials).items[instance.material];
    WordsRef attributes = WordsRef(cluster.attributes);
    const vec3 faceUV = vec3(uintBitsToFloat(attributes.words[vertex * 4u + 2u]), uintBitsToFloat(attributes.words[vertex * 4u + 3u]), 1.0);
    vary_uv = vec2(dot(material.uvRow0, faceUV), dot(material.uvRow1, faceUV));
    vary_texture = material.texture;
    vary_alpha = vec2(material.color.a, material.alphaCutoff);
#ifdef SHADOW
    // shadowAlphaMaskF.glsl: a fullbright face's colour alpha does not thin its shadow.
    vary_alpha.x = (material.flags & MATERIAL_FLAG_FULLBRIGHT) != 0u ? 1.0 : vary_alpha.x;
#endif
#endif
}
