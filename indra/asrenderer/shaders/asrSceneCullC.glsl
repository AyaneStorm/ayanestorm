/**
 * asrSceneCullC.glsl
 * Finds the clusters to draw. The draw counts are the list lengths, so the draws that
 * follow need nothing from the CPU. It does three jobs, chosen by push.phase:
 *
 * Early: one thread per instance. Picks an LOD, tests the instance against the view
 * and against last frame's depth pyramid, then does the same for each cluster, and also
 * tests whether the cluster faces away. What the pyramid hides goes on the retest list,
 * and the rest is drawn.
 *
 * Late: one thread per retest entry, tested against the pyramid of this frame's depth.
 *
 * Audit: as early, but with no occlusion test: a test's way of finding what occlusion
 * got wrong.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 64) in;

const uint CULL_EARLY = 0u;
const uint CULL_LATE = 1u;
const uint CULL_AUDIT = 2u;

const uint WHOLE_INSTANCE = 0xFFFFFFFFu;
const uint MAX_LODS = 4u;

struct DrawArguments
{
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};

layout(buffer_reference, std430) buffer CountsRef
{
    DrawArguments early[LISTS];
    DrawArguments late[LISTS];
    uint retestGroups[3];
    uint retestCount;
};
layout(buffer_reference, std430) writeonly buffer ListWriteRef { ListEntry items[]; };
layout(buffer_reference, std430) readonly buffer PyramidRef { float values[]; };

// Which camera the pyramid belongs to in this job.
vec4 occluderRows[3];
vec2 occluderFocal;
float occluderNear;
bool testOcclusion;

bool insideView(ViewRef view, vec3 center, float radius)
{
    for (int i = 0; i < 6; ++i)
    {
        if (dot(view.planes[i], vec4(center, 1.0)) < -radius)
        {
            return false;
        }
    }
    return view.farClip <= 0.0 || distance(center, view.camera.xyz) - radius <= view.farClip;
}

// True when every pixel the sphere covers already shows something nearer. The screen
// rectangle of a perspective sphere comes from the two lines through the eye that touch
// it, in each axis.
bool hiddenByPyramid(ViewRef view, vec3 center, float radius)
{
    if (!testOcclusion)
    {
        return false;
    }
    const vec3 eye = transformPoint(occluderRows, center);
    const float distanceAlong = -eye.z;
    if (distanceAlong - radius <= occluderNear)
    {
        return false; // reaches the near plane, or is behind it
    }

    const vec2 cx = vec2(-eye.x, -distanceAlong);
    const vec2 cy = vec2(-eye.y, -distanceAlong);
    const vec2 vx = vec2(sqrt(dot(cx, cx) - radius * radius), radius);
    const vec2 vy = vec2(sqrt(dot(cy, cy) - radius * radius), radius);
    const vec2 lowX = mat2(vx.x, vx.y, -vx.y, vx.x) * cx;
    const vec2 highX = mat2(vx.x, -vx.y, vx.y, vx.x) * cx;
    const vec2 lowY = mat2(vy.x, vy.y, -vy.y, vy.x) * cy;
    const vec2 highY = mat2(vy.x, -vy.y, vy.y, vy.x) * cy;

    // In pixels, where y runs down the screen.
    const vec2 size = view.pixelSize.xy;
    const vec2 lowest = vec2(0.5 + 0.5 * lowX.x / lowX.y * occluderFocal.x, 0.5 - 0.5 * highY.x / highY.y * occluderFocal.y) * size;
    const vec2 highest = vec2(0.5 + 0.5 * highX.x / highX.y * occluderFocal.x, 0.5 - 0.5 * lowY.x / lowY.y * occluderFocal.y) * size;
    if (highest.x <= 0.0 || highest.y <= 0.0 || lowest.x >= size.x || lowest.y >= size.y)
    {
        return false; // the pyramid says nothing about what was off screen
    }

    const uvec2 first = uvec2(max(floor(lowest), vec2(0.0)));
    const uvec2 last = uvec2(min(floor(highest), size - 1.0));

    // The level where the rectangle spans at most two texels each way.
    const uint span = max(last.x - first.x, last.y - first.y);
    const uint level = clamp(uint(findMSB(span) + 1), 1u, view.pyramidLevels);
    const uvec2 from = first >> level;
    const uvec2 to = last >> level;
    const uint row = pyramidLevelSize(uint(size.x), level);
    const uint offset = view.pyramidOffset[level - 1u];
    PyramidRef pyramid = PyramidRef(push.pyramid);
    const float farthest = min(min(pyramid.values[offset + from.y * row + from.x], pyramid.values[offset + from.y * row + to.x]),
                               min(pyramid.values[offset + to.y * row + from.x], pyramid.values[offset + to.y * row + to.x]));

    // Reversed depth with no far plane: depth is near / distance.
    return occluderNear / (distanceAlong - radius) < farthest;
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

uint listOf(uint flags, uint triangles)
{
    const uint kind = (flags >> 1) & 3u;
    return kind * 4u + (triangles <= 2u ? 0u : triangles <= 8u ? 1u : triangles <= 32u ? 2u : 3u);
}

// No direction of the matrix can stretch more than this, whatever its rotation and
// stretch, so a sphere scaled by it stays around the object.
float scaleBoundOf(Instance instance)
{
    const vec3 row0 = instance.rows[0].xyz;
    const vec3 row1 = instance.rows[1].xyz;
    const vec3 row2 = instance.rows[2].xyz;
    return sqrt(dot(row0, row0) + dot(row1, row1) + dot(row2, row2));
}

void draw(ViewRef view, Instance instance, uint instanceIndex, uint clusterId, uint triangles)
{
    const uint list = listOf(instance.flags, triangles);
    CountsRef counts = CountsRef(push.output_counts);
    ListWriteRef visible = ListWriteRef(push.visible);
    if (push.phase == CULL_LATE)
    {
        // The late list carries on where the early one ended, so a visibility pixel
        // numbers one entry of one list whichever draw made it.
        const uint earlyCount = counts.early[list].instanceCount;
        const uint slot = atomicAdd(counts.late[list].instanceCount, 1u);
        if (slot == 0u)
        {
            counts.late[list].firstInstance = earlyCount;
        }
        visible.items[view.listBase[list] + earlyCount + slot] = ListEntry(instanceIndex, clusterId);
    }
    else
    {
        const uint slot = atomicAdd(counts.early[list].instanceCount, 1u);
        visible.items[view.listBase[list] + slot] = ListEntry(instanceIndex, clusterId);
    }
}

void retest(uint instanceIndex, uint clusterId)
{
    CountsRef counts = CountsRef(push.output_counts);
    const uint slot = atomicAdd(counts.retestCount, 1u);
    if ((slot & 63u) == 0u)
    {
        atomicAdd(counts.retestGroups[0], 1u);
    }
    ListWriteRef(push.retest).items[slot] = ListEntry(instanceIndex, clusterId);
}

// From the finest LOD to the one wanted. An LOD still on its way is passed over.
uint chooseGeometry(ViewRef view, Instance instance, float scaleBound)
{
    GeometriesRef geometries = GeometriesRef(push.geometries);
    uint id = instance.geometry;
    Geometry geometry = geometries.items[id];
    if (geometry.coarser == 0u)
    {
        return id;
    }

    const bool own = instance.lod.w <= 0.0;
    const vec3 center = own ? vec3(instance.rows[0].w, instance.rows[1].w, instance.rows[2].w) : instance.lod.xyz;
    const float size = own ? geometry.radius * scaleBound : instance.lod.w;
    const float ratio = distance(center, view.camera.xyz) * view.projection.w / size;
    for (uint lod = 1u; lod < MAX_LODS && geometry.coarser != 0u; ++lod)
    {
        if (geometry.clusterCount != 0u && ratio <= geometry.coarserAt)
        {
            break;
        }
        const Geometry coarser = geometries.items[geometry.coarser];
        if (coarser.clusterCount == 0u && geometry.clusterCount != 0u)
        {
            break;
        }
        id = geometry.coarser;
        geometry = coarser;
    }
    return id;
}

void walkClusters(ViewRef view, Instance instance, uint instanceIndex, Geometry geometry, float scaleBound)
{
    const bool twoSided = (instance.flags & INSTANCE_DOUBLE_SIDED) != 0u;
    const vec3 camera = cameraInPacked(instance, view.camera.xyz);
    ClustersRef clusters = ClustersRef(push.clusters);
    for (uint i = 0u; i < geometry.clusterCount; ++i)
    {
        const uint clusterId = geometry.firstCluster + i;
        const Cluster cluster = clusters.items[clusterId];
        if (!twoSided && facesAway(cluster, camera))
        {
            continue;
        }
        const vec3 center = transformPoint(instance.rows, cluster.center);
        const float radius = cluster.radius * scaleBound;
        if (!insideView(view, center, radius))
        {
            continue;
        }
        if (!hiddenByPyramid(view, center, radius))
        {
            draw(view, instance, instanceIndex, clusterId, cluster.triangles);
        }
        else if (push.phase == CULL_EARLY)
        {
            retest(instanceIndex, clusterId);
        }
    }
}

void cullInstance(ViewRef view, uint instanceIndex)
{
    const Instance instance = InstancesRef(push.instances).items[instanceIndex];
    const float scaleBound = scaleBoundOf(instance);
    const Geometry geometry = GeometriesRef(push.geometries).items[chooseGeometry(view, instance, scaleBound)];
    if (geometry.clusterCount == 0u)
    {
        return;
    }

    const vec3 center = vec3(instance.rows[0].w, instance.rows[1].w, instance.rows[2].w);
    const float radius = geometry.radius * scaleBound;
    if (!insideView(view, center, radius))
    {
        return;
    }
    if (!hiddenByPyramid(view, center, radius))
    {
        walkClusters(view, instance, instanceIndex, geometry, scaleBound);
    }
    else if (push.phase == CULL_EARLY)
    {
        retest(instanceIndex, WHOLE_INSTANCE);
    }
}

void main()
{
    ViewRef view = ViewRef(push.view);
    const uint index = gl_GlobalInvocationID.x;
    if (push.phase == CULL_LATE)
    {
        if (index >= CountsRef(push.output_counts).retestCount)
        {
            return;
        }
        occluderRows = view.viewRows;
        occluderFocal = view.projection.xy;
        occluderNear = view.projection.z;
        testOcclusion = true;

        const ListEntry entry = ListRef(push.retest).items[index];
        if (entry.cluster == WHOLE_INSTANCE)
        {
            cullInstance(view, entry.instance);
            return;
        }
        const Instance instance = InstancesRef(push.instances).items[entry.instance];
        const Cluster cluster = ClustersRef(push.clusters).items[entry.cluster];
        if (!hiddenByPyramid(view, transformPoint(instance.rows, cluster.center), cluster.radius * scaleBoundOf(instance)))
        {
            draw(view, instance, entry.instance, entry.cluster, cluster.triangles);
        }
        return;
    }

    if (index < view.instanceEnd)
    {
        occluderRows = view.previousRows;
        occluderFocal = view.previousProjection.xy;
        occluderNear = view.previousProjection.z;
        testOcclusion = push.phase == CULL_EARLY && view.previousProjection.w != 0.0;
        cullInstance(view, index);
    }
}
