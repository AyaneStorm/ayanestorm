/**
 * asrSceneSkinC.glsl
 * Avatars, before anything is culled. Each frame, once for every view:
 *
 * Palette: one thread per palette entry. A joint's pose is carried on from its two newest
 * main-thread poses to the frame's own time, then times the skin's inverse bind matrix.
 * One palette serves every face skinned to that skeleton and skin.
 *
 * Bind: one thread per binding. An attached instance gets its joint's pose times its own
 * matrix. A skinned one is placed in its skeleton's box, picks its LOD, and gets its own
 * geometry pointed at the LOD's room.
 *
 * Vertices: a thread per vertex of the LOD picked, skinned and packed in the box.
 * Clusters: a thread per cluster, with bounds and a normal cone from the skinned triangles.
 * Groups: spheres around their clusters.
 *
 * So culling, drawing and the resolve read skinned geometry like any other.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 64) in;

const uint SKIN_PALETTE = 0u;
const uint SKIN_BIND = 1u;
const uint SKIN_VERTICES = 2u;
const uint SKIN_CLUSTERS = 3u;
const uint SKIN_GROUPS = 4u;
const uint SKIN_BLOCK = 64u;
const uint MAX_LODS = 4u;

layout(buffer_reference, std430) readonly buffer SkeletonsRef { Skeleton items[]; };
layout(buffer_reference, std430) readonly buffer JointsRef { Joint items[]; };
layout(buffer_reference, std430) readonly buffer PaletteJobsRef { PaletteJob items[]; };
layout(buffer_reference, std430) writeonly buffer RowsWriteRef { Rows items[]; };
layout(buffer_reference, std430) buffer BindingsWriteRef { Binding items[]; };
layout(buffer_reference, std430) buffer InstancesWriteRef { Instance items[]; };
layout(buffer_reference, std430) buffer GeometriesWriteRef { Geometry items[]; };
layout(buffer_reference, std430) buffer GeometryWordsRef { uint words[]; };
layout(buffer_reference, std430) writeonly buffer ClustersWriteRef { Cluster items[]; };
layout(buffer_reference, std430) writeonly buffer GroupsWriteRef { ClusterGroup items[]; };
layout(buffer_reference, std430) writeonly buffer PositionsWriteRef { uint words[]; };

Rows multiply(Rows a, Rows b)
{
    Rows m;
    for (int i = 0; i < 3; ++i)
    {
        m.r[i] = a.r[i].x * b.r[0] + a.r[i].y * b.r[1] + a.r[i].z * b.r[2] + vec4(0.0, 0.0, 0.0, a.r[i].w);
    }
    return m;
}

// The motion between the two newest poses, carried on to the frame's time, at most maxLead
// steps. Times wrap, so they are compared by difference.
Rows predict(ViewRef view, Skeleton skeleton, uint joint)
{
    const Joint pose = JointsRef(view.skinTables[SKIN_TABLE_JOINTS]).items[joint];
    const uint step = skeleton.time - skeleton.previousTime;
    float lead = 0.0;
    if (step != 0u)
    {
        lead = clamp(float(int(view.time - skeleton.time)) / float(step), 0.0, skeleton.maxLead);
    }
    Rows m;
    for (int i = 0; i < 3; ++i)
    {
        m.r[i] = pose.newest.r[i] + (pose.newest.r[i] - pose.previous.r[i]) * lead;
    }
    return m;
}

Skeleton skeletonOf(ViewRef view, uint id)
{
    return SkeletonsRef(view.skinTables[SKIN_TABLE_SKELETONS]).items[id];
}

void makePalette(ViewRef view, uint entry)
{
    if (entry >= view.paletteEnd)
    {
        return;
    }
    const PaletteJob job = PaletteJobsRef(view.skinTables[SKIN_TABLE_PALETTE_JOBS]).items[entry];
    if (job.skeleton == 0u)
    {
        return; // a free entry
    }
    const Rows matrix = RowsRef(view.skinTables[SKIN_TABLE_MATRICES]).items[job.matrix];
    RowsWriteRef(view.skinTables[SKIN_TABLE_PALETTE]).items[entry] = multiply(predict(view, skeletonOf(view, job.skeleton), job.joint), matrix);
}

// The finest LOD that is in and fine enough, as the cull shader picks for other instances.
uint chooseLod(ViewRef view, uint id, vec3 center, float size)
{
    GeometriesRef geometries = GeometriesRef(push.geometries);
    Geometry geometry = geometries.items[id];
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

void bind(ViewRef view, uint id)
{
    if (id >= view.bindingEnd)
    {
        return;
    }
    BindingsWriteRef bindings = BindingsWriteRef(view.skinTables[SKIN_TABLE_BINDINGS]);
    const Binding binding = bindings.items[id];
    if (binding.kind == 0u)
    {
        return;
    }
    const Skeleton skeleton = skeletonOf(view, binding.skeleton);
    InstancesWriteRef instances = InstancesWriteRef(push.instances);

    if (binding.kind == BINDING_ATTACHED)
    {
        const Rows pose = predict(view, skeleton, binding.palette);
        const Rows placed = multiply(pose, RowsRef(view.skinTables[SKIN_TABLE_MATRICES]).items[binding.matrix]);
        // Row by row: a whole array written to device memory does not translate to Metal.
        instances.items[binding.instance].rows[0] = placed.r[0];
        instances.items[binding.instance].rows[1] = placed.r[1];
        instances.items[binding.instance].rows[2] = placed.r[2];
        instances.items[binding.instance].lod = vec4(rowsPoint(pose, binding.lod.xyz), binding.lod.w);
        return;
    }

    // Packed positions are box units, so the matrix is the box.
    const vec3 center = skeleton.boxCenter;
    const float scale = skeleton.boxScale;
    instances.items[binding.instance].rows[0] = vec4(scale, 0.0, 0.0, center.x);
    instances.items[binding.instance].rows[1] = vec4(0.0, scale, 0.0, center.y);
    instances.items[binding.instance].rows[2] = vec4(0.0, 0.0, scale, center.z);
    const float size = binding.lod.w > 0.0 ? binding.lod.w : scale;
    instances.items[binding.instance].lod = vec4(center, size);

    const uint chosen = chooseLod(view, binding.source, center, size);
    const Geometry lod = GeometriesRef(push.geometries).items[chosen];
    Geometry own;
    own.firstGroup = binding.targetEntry;
    own.groupCount = lod.groupCount;
    own.unused = 0u;
    own.radius = 0.0; // the clusters job widens it
    own.firstCluster = binding.targetEntry + binding.groupCapacity;
    own.clusterCount = lod.clusterCount;
    own.coarser = 0u;
    own.coarserAt = 0.0;
    GeometriesWriteRef(push.geometries).items[binding.target] = own;

    bindings.items[id].chosen = lod.clusterCount != 0u ? chosen : 0u;
    if (lod.clusterCount != 0u)
    {
        const Cluster first = ClustersRef(push.clusters).items[lod.firstCluster];
        bindings.items[id].weights = first.attributes + 2ul * (first.attributes - first.positions);
    }
}

// The binding of this workgroup's block, and the block's place inside it. False when the
// block is free, or its binding draws nothing this frame.
bool blockBinding(ViewRef view, out Binding binding, out uint block)
{
    const uint index = gl_WorkGroupID.x;
    if (index >= view.blockEnd)
    {
        return false;
    }
    const uint id = WordsRef(view.skinTables[SKIN_TABLE_BLOCKS]).words[index];
    if (id == 0u)
    {
        return false;
    }
    binding = BindingsRef(view.skinTables[SKIN_TABLE_BINDINGS]).items[id];
    block = index - binding.firstBlock;
    return binding.kind == BINDING_SKINNED && binding.chosen != 0u;
}

uint64_t ownPositions(ViewRef view, Binding binding)
{
    return view.skinTables[SKIN_TABLE_POSITIONS] + uint64_t(binding.positions) * 8ul;
}

int snorm16(float value)
{
    return int(round(clamp(value, -1.0, 1.0) * 32767.0));
}

void skinVertex(ViewRef view)
{
    Binding binding;
    uint block;
    if (!blockBinding(view, binding, block))
    {
        return;
    }
    const uint vertex = block * SKIN_BLOCK + gl_LocalInvocationID.x;
    const Geometry lod = GeometriesRef(push.geometries).items[binding.chosen];
    const Cluster first = ClustersRef(push.clusters).items[lod.firstCluster];
    if (vertex >= uint((first.attributes - first.positions) / 8ul))
    {
        return;
    }

    const vec3 mesh = packedPosition(first.positions, vertex) * binding.sourceBox.w + binding.sourceBox.xyz;
    uvec4 joints;
    vec4 amounts;
    skinWeights(binding.weights, vertex, joints, amounts);
    RowsRef palette = RowsRef(view.skinTables[SKIN_TABLE_PALETTE]);
    vec3 skinned = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        if (amounts[i] > 0.0)
        {
            skinned += amounts[i] * rowsPoint(palette.items[binding.palette + joints[i]], mesh);
        }
    }

    const Skeleton skeleton = skeletonOf(view, binding.skeleton);
    const vec3 packed = (skinned - skeleton.boxCenter) / skeleton.boxScale;
    PositionsWriteRef out_positions = PositionsWriteRef(ownPositions(view, binding));
    out_positions.words[vertex * 2u] = (uint(snorm16(packed.x)) & 0xFFFFu) | (uint(snorm16(packed.y)) << 16);
    out_positions.words[vertex * 2u + 1u] = uint(snorm16(packed.z)) & 0xFFFFu;
}

// As buildGeometry() packs a cone: rounded so it never claims more than the real one.
uint coneBits(vec3 axis, float cutoff)
{
    const vec4 values = vec4(axis, min(1.0, cutoff + 4.0 / 127.0));
    const ivec4 bytes = ivec4(clamp(round(values * 127.0), -127.0, 127.0));
    return (uint(bytes.x) & 0xFFu) | ((uint(bytes.y) & 0xFFu) << 8) | ((uint(bytes.z) & 0xFFu) << 16) | ((uint(bytes.w) & 0xFFu) << 24);
}

void skinCluster(ViewRef view)
{
    Binding binding;
    uint block;
    if (!blockBinding(view, binding, block))
    {
        return;
    }
    const uint local = block * SKIN_BLOCK + gl_LocalInvocationID.x;
    const Geometry lod = GeometriesRef(push.geometries).items[binding.chosen];
    if (local >= lod.clusterCount)
    {
        return;
    }
    const Cluster source = ClustersRef(push.clusters).items[lod.firstCluster + local];
    const uint64_t positions = ownPositions(view, binding);

    vec3 low = vec3(2.0);
    vec3 high = vec3(-2.0);
    vec3 normalSum = vec3(0.0);
    for (uint triangle = 0u; triangle < source.triangles; ++triangle)
    {
        const vec3 a = packedPosition(positions, clusterIndex(source.indices, triangle * 3u));
        const vec3 b = packedPosition(positions, clusterIndex(source.indices, triangle * 3u + 1u));
        const vec3 c = packedPosition(positions, clusterIndex(source.indices, triangle * 3u + 2u));
        low = min(low, min(a, min(b, c)));
        high = max(high, max(a, max(b, c)));
        const vec3 facing = cross(b - a, c - a);
        if (dot(facing, facing) > 0.0)
        {
            normalSum += normalize(facing);
        }
    }
    const vec3 middle = (low + high) * 0.5;
    const vec3 axis = dot(normalSum, normalSum) > 0.0 ? normalize(normalSum) : vec3(0.0, 0.0, 1.0);
    float radius = 0.0;
    float cutoff = 1.0;
    bool anyNormal = false;
    for (uint triangle = 0u; triangle < source.triangles; ++triangle)
    {
        const vec3 a = packedPosition(positions, clusterIndex(source.indices, triangle * 3u));
        const vec3 b = packedPosition(positions, clusterIndex(source.indices, triangle * 3u + 1u));
        const vec3 c = packedPosition(positions, clusterIndex(source.indices, triangle * 3u + 2u));
        radius = max(radius, max(distance(a, middle), max(distance(b, middle), distance(c, middle))));
        const vec3 facing = cross(b - a, c - a);
        if (dot(facing, facing) > 0.0)
        {
            cutoff = min(cutoff, dot(normalize(facing), axis));
            anyNormal = true;
        }
    }
    radius += 2.0 / 32767.0; // packing moves vertices a little, as in buildGeometry()

    // The sine of how far normals stray past the plane: 1 turns the facing test off.
    const float sine = (cutoff <= 0.0 || !anyNormal) ? 1.0 : sqrt(max(1.0 - cutoff * cutoff, 0.0));

    Cluster own;
    own.center = middle;
    own.radius = radius;
    own.cone = coneBits(axis, sine);
    own.triangles = source.triangles;
    own.indices = source.indices;
    own.positions = positions;
    own.attributes = source.attributes;
    ClustersWriteRef(push.clusters).items[binding.targetEntry + binding.groupCapacity + local] = own;

    // The geometry's sphere around the packed origin. Positive floats order as their bits.
    GeometryWordsRef geometryWords = GeometryWordsRef(push.geometries);
    atomicMax(geometryWords.words[binding.target * 8u + 3u], floatBitsToUint(length(middle) + radius));
}

void skinGroups(ViewRef view)
{
    Binding binding;
    uint block;
    if (!blockBinding(view, binding, block) || block != 0u)
    {
        return;
    }
    const Geometry lod = GeometriesRef(push.geometries).items[binding.chosen];
    ClusterGroupsRef table = ClusterGroupsRef(push.clusters);
    ClustersRef clusters = ClustersRef(push.clusters);
    const uint firstOwnCluster = binding.targetEntry + binding.groupCapacity;
    for (uint group = gl_LocalInvocationID.x; group < lod.groupCount; group += SKIN_BLOCK)
    {
        const ClusterGroup source = table.items[lod.firstGroup + group];
        const uint first = firstOwnCluster + (source.firstCluster - lod.firstCluster);
        vec3 low = vec3(3.0);
        vec3 high = vec3(-3.0);
        for (uint i = 0u; i < source.clusterCount; ++i)
        {
            const Cluster cluster = clusters.items[first + i];
            low = min(low, cluster.center - cluster.radius);
            high = max(high, cluster.center + cluster.radius);
        }
        const vec3 middle = (low + high) * 0.5;
        float radius = 0.0;
        for (uint i = 0u; i < source.clusterCount; ++i)
        {
            const Cluster cluster = clusters.items[first + i];
            radius = max(radius, distance(cluster.center, middle) + cluster.radius);
        }

        ClusterGroup own;
        own.center = middle;
        own.radius = radius;
        own.cone = 127u << 24; // a group bends with the body: its clusters' cones do the facing test
        own.firstCluster = first;
        own.clusterCount = source.clusterCount;
        for (int i = 0; i < 5; ++i)
        {
            own.unused[i] = 0u;
        }
        GroupsWriteRef(push.clusters).items[binding.targetEntry + group] = own;
    }
}

void main()
{
    ViewRef view = ViewRef(push.view);
    switch (push.phase)
    {
        case SKIN_PALETTE:
            makePalette(view, gl_GlobalInvocationID.x);
            break;
        case SKIN_BIND:
            bind(view, gl_GlobalInvocationID.x);
            break;
        case SKIN_VERTICES:
            skinVertex(view);
            break;
        case SKIN_CLUSTERS:
            skinCluster(view);
            break;
        default:
            skinGroups(view);
            break;
    }
}
