/**
 * asrSceneCullC.glsl
 * Finds the clusters to draw. The draw counts are the list lengths, so the draws that
 * follow need nothing from the CPU. It does six jobs, chosen by push.phase:
 *
 * Early: one thread per instance. Picks an LOD, and tests the instance against the view
 * and against last frame's depth pyramid. A small geometry has its clusters tested right
 * there, each also for facing away. A larger one has its groups tested the same way, and
 * each group that passes goes on the group list. What the pyramid hides goes on the
 * retest list, and the rest is drawn.
 *
 * Groups early: one thread per cluster of each group on the list, tested as above. So a
 * dense mesh close up is spread over many threads, not walked by one.
 *
 * Late: one thread per retest entry, tested against the pyramid of this frame's depth. A
 * group that passes goes on the late part of the group list, for groups late.
 *
 * Audit, and groups audit: as early, but with no occlusion test: a test's way of finding
 * what occlusion got wrong.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 64) in;

const uint CULL_EARLY = 0u;
const uint CULL_LATE = 1u;
const uint CULL_AUDIT = 2u;
const uint CULL_GROUPS_EARLY = 3u;
const uint CULL_GROUPS_LATE = 4u;
const uint CULL_GROUPS_AUDIT = 5u;

const uint WHOLE_INSTANCE = 0xFFFFFFFFu;
const uint RETEST_GROUP = 0x80000000u;
const uint MAX_LODS = 4u;
const uint CLUSTER_GROUP_SIZE = 32u;
const uint GROUPS_PER_WORKGROUP = 2u; // 64 threads, one per cluster of a group

struct DrawArguments
{
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};

struct WorkCount
{
    uint groups[3]; // an indirect dispatch
    uint entries;
};

layout(buffer_reference, std430) buffer CountsRef
{
    DrawArguments early[LISTS];
    DrawArguments late[LISTS];
    WorkCount retest;
    WorkCount groupWork[2]; // early, late
    uint guard[3]; // the resolve's, see GpuSceneCounts::guard
    uint dropped;  // entries a full list turned away
    uint macoitTailSeen;
    uint countsUnused[3];
    DrawArguments macoitTail[2u * BLENDED_LISTS];
};

const uint MACOIT_GATE = 6u;
layout(buffer_reference, std430) writeonly buffer ListWriteRef { ListEntry items[]; };
layout(buffer_reference, std430) readonly buffer PyramidRef { float values[]; };

// Which camera the pyramid belongs to in this job.
vec4 occluderRows[3];
vec2 occluderFocal;
float occluderNear;
bool testOcclusion;

bool latePhase()
{
    return push.phase == CULL_LATE || push.phase == CULL_GROUPS_LATE;
}

bool earlyPhase()
{
    return push.phase == CULL_EARLY || push.phase == CULL_GROUPS_EARLY;
}

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

// For a cluster or a group: true when every triangle inside the sphere faces away.
bool facesAway(uint cone, vec3 center, float radius, vec3 camera)
{
    const float cutoff = float(int(cone) >> 24) / 127.0;
    if (cutoff >= 1.0)
    {
        return false;
    }
    const vec3 axis = vec3(float(int(cone << 24) >> 24), float(int(cone << 16) >> 24), float(int(cone << 8) >> 24)) / 127.0;
    const vec3 toSphere = center - camera;
    return dot(toSphere, axis) >= cutoff * length(toSphere) + radius;
}

uint listOf(uint flags, uint triangles)
{
    const uint kind = (flags & INSTANCE_BLENDED) != 0u ? BLENDED_KIND + ((flags >> 1) & 1u) : (flags >> 1) & 3u; // sceneKind()
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

// Every list write takes its place with an atomic add, checked against the room left. A
// thread that finds the list full takes its add back, so the count ends at the entries
// written and is what the draw or dispatch that reads it covers. No place is handed out
// twice: the count is at the room or above while any thread is taking an add back.
const uint TAKE_BACK = 0xFFFFFFFFu; // adds as minus one

void turnAway(CountsRef counts)
{
    atomicAdd(counts.dropped, 1u);
}

// Where a list's room ends: where the next one starts.
uint listEnd(ViewRef view, uint list)
{
    return list + 1u < LISTS ? view.listBase[list + 1u] : view.visibleEnd;
}

void draw(ViewRef view, Instance instance, uint instanceIndex, uint clusterId, uint triangles)
{
    const uint list = listOf(instance.flags, triangles);
    const uint room = listEnd(view, list) - view.listBase[list];
    CountsRef counts = CountsRef(push.output_counts);
    ListWriteRef visible = ListWriteRef(push.visible);
    if (latePhase())
    {
        // The late list carries on where the early one ended, so a visibility pixel
        // numbers one entry of one list whichever draw made it.
        const uint earlyCount = counts.early[list].instanceCount;
        const uint slot = atomicAdd(counts.late[list].instanceCount, 1u);
        if (slot >= room - earlyCount)
        {
            atomicAdd(counts.late[list].instanceCount, TAKE_BACK);
            turnAway(counts);
            return;
        }
        if (slot == 0u)
        {
            counts.late[list].firstInstance = earlyCount;
        }
        visible.items[view.listBase[list] + earlyCount + slot] = ListEntry(instanceIndex, clusterId);
    }
    else
    {
        const uint slot = atomicAdd(counts.early[list].instanceCount, 1u);
        if (slot >= room)
        {
            atomicAdd(counts.early[list].instanceCount, TAKE_BACK);
            turnAway(counts);
            return;
        }
        visible.items[view.listBase[list] + slot] = ListEntry(instanceIndex, clusterId);
    }
}

void retest(ViewRef view, uint instanceIndex, uint clusterId)
{
    CountsRef counts = CountsRef(push.output_counts);
    const uint slot = atomicAdd(counts.retest.entries, 1u);
    if (slot >= view.visibleEnd + view.groupEnd + view.instanceEnd)
    {
        atomicAdd(counts.retest.entries, TAKE_BACK);
        turnAway(counts);
        return;
    }
    if ((slot & 63u) == 0u)
    {
        atomicAdd(counts.retest.groups[0], 1u);
    }
    ListWriteRef(push.retest).items[slot] = ListEntry(instanceIndex, clusterId);
}

// A group whose clusters are to be tested, by the groups job of the same phase. The late
// part of the list carries on where the early part ended.
void addGroup(ViewRef view, uint instanceIndex, uint groupId)
{
    CountsRef counts = CountsRef(push.output_counts);
    const uint work = latePhase() ? 1u : 0u;
    const uint base = work == 1u ? counts.groupWork[0].entries : 0u;
    const uint slot = atomicAdd(counts.groupWork[work].entries, 1u);
    if (slot >= view.groupEnd - base)
    {
        atomicAdd(counts.groupWork[work].entries, TAKE_BACK);
        turnAway(counts);
        return;
    }
    if (slot % GROUPS_PER_WORKGROUP == 0u)
    {
        atomicAdd(counts.groupWork[work].groups[0], 1u);
    }
    ListWriteRef(push.groups).items[base + slot] = ListEntry(instanceIndex, groupId);
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

void testCluster(ViewRef view, Instance instance, uint instanceIndex, uint clusterId, vec3 camera, float scaleBound)
{
    const Cluster cluster = ClustersRef(push.clusters).items[clusterId];
    if ((instance.flags & INSTANCE_DOUBLE_SIDED) == 0u && facesAway(cluster.cone, cluster.center, cluster.radius, camera))
    {
        return;
    }
    const vec3 center = transformPoint(instance.rows, cluster.center);
    const float radius = cluster.radius * scaleBound;
    if (!insideView(view, center, radius))
    {
        return;
    }
    if (!hiddenByPyramid(view, center, radius))
    {
        draw(view, instance, instanceIndex, clusterId, cluster.triangles);
    }
    else if (earlyPhase())
    {
        retest(view, instanceIndex, clusterId);
    }
}

void testGroup(ViewRef view, Instance instance, uint instanceIndex, uint groupId, vec3 camera, float scaleBound)
{
    const ClusterGroup group = ClusterGroupsRef(push.clusters).items[groupId];
    if ((instance.flags & INSTANCE_DOUBLE_SIDED) == 0u && facesAway(group.cone, group.center, group.radius, camera))
    {
        return;
    }
    const vec3 center = transformPoint(instance.rows, group.center);
    const float radius = group.radius * scaleBound;
    if (!insideView(view, center, radius))
    {
        return;
    }
    if (!hiddenByPyramid(view, center, radius))
    {
        addGroup(view, instanceIndex, groupId);
    }
    else if (earlyPhase())
    {
        retest(view, instanceIndex, RETEST_GROUP | groupId);
    }
}

// A view can draw only the instances bound to chosen skeletons: avatars over a plain colour.
bool chosenByView(ViewRef view, Instance instance)
{
    if (view.drawOnly == 0ul)
    {
        return true;
    }
    if ((instance.flags & INSTANCE_BOUND) == 0u)
    {
        return false;
    }
    const uint skeleton = BindingsRef(view.skinTables[SKIN_TABLE_BINDINGS]).items[instance.binding].skeleton;
    return (WordsRef(view.drawOnly).words[skeleton >> 5] & (1u << (skeleton & 31u))) != 0u;
}

void cullInstance(ViewRef view, uint instanceIndex)
{
    const Instance instance = InstancesRef(push.instances).items[instanceIndex];
    if (!chosenByView(view, instance))
    {
        return;
    }
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
    if (hiddenByPyramid(view, center, radius))
    {
        if (push.phase == CULL_EARLY)
        {
            retest(view, instanceIndex, WHOLE_INSTANCE);
        }
        return;
    }

    const vec3 camera = cameraInPacked(instance, view.camera.xyz);
    if (geometry.groupCount != 0u)
    {
        for (uint i = 0u; i < geometry.groupCount; ++i)
        {
            testGroup(view, instance, instanceIndex, geometry.firstGroup + i, camera, scaleBound);
        }
    }
    else
    {
        for (uint i = 0u; i < geometry.clusterCount; ++i)
        {
            testCluster(view, instance, instanceIndex, geometry.firstCluster + i, camera, scaleBound);
        }
    }
}

// Groups jobs: each half of the workgroup takes one group, a thread per cluster.
void cullGroupClusters(ViewRef view)
{
    CountsRef counts = CountsRef(push.output_counts);
    const uint work = push.phase == CULL_GROUPS_LATE ? 1u : 0u;
    const uint entryIndex = gl_WorkGroupID.x * GROUPS_PER_WORKGROUP + gl_LocalInvocationID.x / CLUSTER_GROUP_SIZE;
    if (entryIndex >= counts.groupWork[work].entries)
    {
        return;
    }
    const uint base = work == 1u ? counts.groupWork[0].entries : 0u;
    const ListEntry entry = ListRef(push.groups).items[base + entryIndex];
    const ClusterGroup group = ClusterGroupsRef(push.clusters).items[entry.cluster];
    const uint lane = gl_LocalInvocationID.x % CLUSTER_GROUP_SIZE;
    if (lane >= group.clusterCount)
    {
        return;
    }
    const Instance instance = InstancesRef(push.instances).items[entry.instance];
    testCluster(view, instance, entry.instance, group.firstCluster + lane, cameraInPacked(instance, view.camera.xyz), scaleBoundOf(instance));
}

// Mac OIT's tail pass draws the blended lists only when some pixel has a tail; otherwise its
// draws are empty and cost nothing. One thread per draw: early lists, then late.
void gateMacOITTail()
{
    const uint draw = gl_GlobalInvocationID.x;
    if (draw >= 2u * BLENDED_LISTS)
    {
        return;
    }
    CountsRef counts = CountsRef(push.output_counts);
    const uint list = BLENDED_KIND * 4u + (draw % BLENDED_LISTS);
    DrawArguments arguments = draw < BLENDED_LISTS ? counts.early[list] : counts.late[list];
    if (counts.macoitTailSeen == 0u)
    {
        arguments.instanceCount = 0u;
    }
    counts.macoitTail[draw] = arguments;
}

void main()
{
    ViewRef view = ViewRef(push.view);
    const uint index = gl_GlobalInvocationID.x;
    if (push.phase == MACOIT_GATE)
    {
        gateMacOITTail();
        return;
    }
    const bool late = latePhase();
    occluderRows = late ? view.viewRows : view.previousRows;
    occluderFocal = late ? view.projection.xy : view.previousProjection.xy;
    occluderNear = late ? view.projection.z : view.previousProjection.z;
    testOcclusion = late || (earlyPhase() && view.previousProjection.w != 0.0);

    if (push.phase >= CULL_GROUPS_EARLY)
    {
        cullGroupClusters(view);
        return;
    }

    if (push.phase == CULL_LATE)
    {
        if (index >= CountsRef(push.output_counts).retest.entries)
        {
            return;
        }
        const ListEntry entry = ListRef(push.retest).items[index];
        if (entry.cluster == WHOLE_INSTANCE)
        {
            cullInstance(view, entry.instance);
            return;
        }
        const Instance instance = InstancesRef(push.instances).items[entry.instance];
        const float scaleBound = scaleBoundOf(instance);
        if ((entry.cluster & RETEST_GROUP) != 0u)
        {
            // Facing and view were tested early, with this same camera.
            const uint groupId = entry.cluster & ~RETEST_GROUP;
            const ClusterGroup group = ClusterGroupsRef(push.clusters).items[groupId];
            if (!hiddenByPyramid(view, transformPoint(instance.rows, group.center), group.radius * scaleBound))
            {
                addGroup(view, entry.instance, groupId);
            }
            return;
        }
        const Cluster cluster = ClustersRef(push.clusters).items[entry.cluster];
        if (!hiddenByPyramid(view, transformPoint(instance.rows, cluster.center), cluster.radius * scaleBound))
        {
            draw(view, instance, entry.instance, entry.cluster, cluster.triangles);
        }
        return;
    }

    if (index < view.instanceEnd)
    {
        cullInstance(view, index);
    }
}
