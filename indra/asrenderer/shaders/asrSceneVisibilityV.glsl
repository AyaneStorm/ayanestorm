/**
 * asrSceneVisibilityV.glsl
 * Vertices of the visibility pass. An instance of the draw is one visible cluster, and
 * vertices are read from memory, so one draw covers everything of a bucket's size. A
 * cluster smaller than its bucket collapses its spare triangles to a point.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(location = 0) flat out uint vary_visibility;

const uint FLIPPED = 1u;

void main()
{
    ViewRef view = ViewRef(push.view);
    const uint entry = view.bucketBase[push.bucket] + uint(gl_InstanceIndex);
    const uint triangle = uint(gl_VertexIndex) / 3u;
    const VisibleCluster visible = VisibleRef(push.visible).items[entry];
    const Cluster cluster = ClustersRef(push.clusters).items[visible.cluster];
    vary_visibility = (entry << 6) | triangle;
    if (triangle >= cluster.triangles)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const Instance instance = InstancesRef(push.instances).items[visible.instance];
    uint corner = uint(gl_VertexIndex) % 3u;
    if ((instance.flags & FLIPPED) != 0u && corner != 0u)
    {
        corner = 3u - corner;
    }
    const uint vertex = clusterIndex(cluster.indices, triangle * 3u + corner);
    const vec3 world = transformPoint(instance.rows, packedPosition(cluster.positions, vertex));
    gl_Position = view.viewProjection * vec4(world, 1.0);
}
