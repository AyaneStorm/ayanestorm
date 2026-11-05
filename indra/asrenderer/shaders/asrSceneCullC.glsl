/**
 * asrSceneCullC.glsl
 * Finds the clusters to draw. One thread per instance tests the instance against the
 * view, then each of its clusters against the view and against facing away, and appends
 * what survives to the list of its size. The draw counts are the list lengths, so the
 * draws that follow need nothing from the CPU.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 64) in;

struct DrawArguments
{
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};

layout(buffer_reference, std430) buffer CountsRef { DrawArguments draws[BUCKETS]; };
layout(buffer_reference, std430) writeonly buffer VisibleWriteRef { VisibleCluster items[]; };

bool insideView(ViewRef view, vec3 center, float radius)
{
    for (int i = 0; i < 5; ++i)
    {
        if (dot(view.planes[i], vec4(center, 1.0)) < -radius)
        {
            return false;
        }
    }
    return true;
}

// The camera in packed space. Which side of a triangle faces the camera survives any
// affine transform, so the normal cone stays valid for stretched prims this way.
vec3 cameraInPacked(Instance instance, vec3 camera)
{
    const mat3 linear = mat3(instance.rows[0].xyz, instance.rows[1].xyz, instance.rows[2].xyz); // transposed
    const vec3 offset = camera - vec3(instance.rows[0].w, instance.rows[1].w, instance.rows[2].w);
    return inverse(transpose(linear)) * offset;
}

bool facesAway(Cluster cluster, vec3 camera)
{
    const float cutoff = float(int(cluster.cone) >> 24) / 127.0;
    if (cutoff >= 1.0)
    {
        return false;
    }
    const vec3 axis = vec3(float(int(cluster.cone << 24) >> 24), float(int(cluster.cone << 16) >> 24), float(int(cluster.cone << 8) >> 24)) / 127.0;
    const vec3 toCluster = cluster.center - camera;
    return dot(toCluster, axis) >= cutoff * length(toCluster) + cluster.radius;
}

uint bucketOf(uint triangles)
{
    return triangles <= 2u ? 0u : triangles <= 8u ? 1u : triangles <= 32u ? 2u : 3u;
}

void main()
{
    ViewRef view = ViewRef(push.view);
    const uint index = gl_GlobalInvocationID.x;
    if (index >= view.instanceEnd)
    {
        return;
    }

    const Instance instance = InstancesRef(push.instances).items[index];
    const Geometry geometry = GeometriesRef(push.geometries).items[instance.geometry];
    if (geometry.clusterCount == 0u)
    {
        return;
    }

    // No direction of the matrix can stretch more than this, whatever its rotation and
    // stretch, so a sphere scaled by it stays around the object.
    const vec3 row0 = instance.rows[0].xyz;
    const vec3 row1 = instance.rows[1].xyz;
    const vec3 row2 = instance.rows[2].xyz;
    const float scaleBound = sqrt(dot(row0, row0) + dot(row1, row1) + dot(row2, row2));
    const vec3 worldCenter = vec3(instance.rows[0].w, instance.rows[1].w, instance.rows[2].w);
    if (!insideView(view, worldCenter, geometry.radius * scaleBound))
    {
        return;
    }

    const vec3 camera = cameraInPacked(instance, view.camera.xyz);
    ClustersRef clusters = ClustersRef(push.clusters);
    VisibleWriteRef visible = VisibleWriteRef(push.visible);
    CountsRef counts = CountsRef(push.output_counts);
    for (uint i = 0u; i < geometry.clusterCount; ++i)
    {
        const uint clusterId = geometry.firstCluster + i;
        const Cluster cluster = clusters.items[clusterId];
        if (facesAway(cluster, camera))
        {
            continue;
        }
        if (!insideView(view, transformPoint(instance.rows, cluster.center), cluster.radius * scaleBound))
        {
            continue;
        }
        const uint bucket = bucketOf(cluster.triangles);
        const uint slot = atomicAdd(counts.draws[bucket].instanceCount, 1u);
        visible.items[view.bucketBase[bucket] + slot] = VisibleCluster(index, clusterId);
    }
}
