/**
 * asrSceneSkinC.glsl
 * Avatars, before anything is culled. Each frame, once for every view:
 *
 * Pose: one thread per joint, continuing rotation and translation to the frame's time
 * while preserving the latest scale/shear. Every skin and attachment shares the result.
 * Palette: one thread per palette entry, times the skin's inverse bind matrix.
 * One palette serves every face skinned to that skeleton and skin.
 *
 * Bind: one thread per binding. An attached instance gets its joint's pose times its own
 * matrix. A skinned one is placed in its skeleton's box, picks its LOD, and gets its own
 * geometry pointed at the LOD's room.
 *
 * Clusters: one thread per cluster, conservative bounds from its influencing joints.
 * Groups: spheres around their clusters, before culling decides deformation demand.
 * Vertices: a thread per selected-LOD vertex, only for bindings admitted by early culling
 * or newly exposed by current-depth late culling. Float position fidelity is unchanged.
 *
 * So culling, drawing and the resolve read skinned geometry like any other.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"
#include "asrSceneJointPose.glsl"
#include "asrSceneSkinDemand.glsl"

layout(local_size_x = 64) in;

const uint SKIN_POSE = 0u;
const uint SKIN_PALETTE = 1u;
const uint SKIN_BIND = 2u;
const uint SKIN_MORPH = 3u;
const uint SKIN_VERTICES = 4u;
const uint SKIN_CLUSTERS = 5u;
const uint SKIN_GROUPS = 6u;
const uint SKIN_VERTICES_LATE = 7u;
const uint SKIN_AUDIT = 8u;
const uint SKIN_BLOCK = 64u;
const uint MAX_LODS = 4u;
const uint MORPH_NO_MASK = 0xFFFFFFFFu;

struct MorphVertex
{
    vec3 position;
    uint firstDelta;
    vec3 normal;
    uint deltaCount;
    vec3 binormal;
    float u;
    float v;
    uint unused[3];
};

struct MorphDelta
{
    vec3 position;
    uint target;
    vec3 normal;
    uint mask;
    vec3 binormal;
    float u;
    float v;
    uint unused[3];
};

struct MorphWork
{
    uint morph;
    uint firstVertex;
};

layout(buffer_reference, std430) readonly buffer MorphsRef { Morph items[]; };
layout(buffer_reference, std430) readonly buffer MorphVerticesRef { MorphVertex items[]; };
layout(buffer_reference, std430) readonly buffer MorphDeltasRef { MorphDelta items[]; };
layout(buffer_reference, std430) readonly buffer MorphWorkRef { MorphWork items[]; };
layout(buffer_reference, std430) readonly buffer FloatsRef { float values[]; };
layout(buffer_reference, std430) buffer Vec4Ref { vec4 items[]; };

layout(buffer_reference, std430) readonly buffer SkeletonsRef { Skeleton items[]; };
layout(buffer_reference, std430) readonly buffer JointsRef { Joint items[]; };
layout(buffer_reference, std430) buffer JointsWriteRef { Joint items[]; };
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
// steps. Times wrap, so they are compared by difference. With minLead -1 the skeleton is
// drawn one step late: from the pose before (-1) to the newest (0), never past it.
void predictJoint(ViewRef view, uint joint)
{
    if (joint >= view.jointEnd) return;
    const Joint pose = JointsRef(view.skinTables[SKIN_TABLE_JOINTS]).items[joint];
    if (pose.skeleton == 0u) return;
    const Skeleton skeleton = SkeletonsRef(view.skinTables[SKIN_TABLE_SKELETONS]).items[pose.skeleton];
    if (joint < skeleton.firstJoint || joint - skeleton.firstJoint >= skeleton.jointCount) return;
    const uint step = skeleton.time - skeleton.previousTime;
    float lead = 0.0;
    if (step != 0u)
    {
        lead = clamp(float(int(view.time - skeleton.time)) / float(step) + skeleton.minLead, skeleton.minLead, skeleton.maxLead);
    }
    const Rows predicted = extrapolateJoint(pose, lead);
    for (int i = 0; i < 3; ++i)
    {
        JointsWriteRef(view.skinTables[SKIN_TABLE_JOINTS]).items[joint].predicted.r[i] = predicted.r[i];
    }
}

Rows predict(ViewRef view, uint joint)
{
    return JointsRef(view.skinTables[SKIN_TABLE_JOINTS]).items[joint].predicted;
}

Skeleton skeletonOf(ViewRef view, uint id)
{
    return SkeletonsRef(view.skinTables[SKIN_TABLE_SKELETONS]).items[id];
}

// Conservative fallback LOD size; deformed positions no longer use this packing grid.
float bindingBox(Skeleton skeleton, Binding binding)
{
    return (skeleton.boxScale + binding.reach * skeleton.stretch) * 1.001;
}

void makePalette(ViewRef view, uint entry)
{
    if (entry >= view.paletteEnd)
    {
        return;
    }
    const PaletteJob job = PaletteJobsRef(view.skinTables[SKIN_TABLE_PALETTE_JOBS]).items[entry];
    if (job.skeleton == 0u || (skeletonOf(view, job.skeleton).flags & SKELETON_PARKED) != 0u)
    {
        return; // free or parked: no resident skinned consumer needs this palette
    }
    const Rows matrix = RowsRef(view.skinTables[SKIN_TABLE_MATRICES]).items[job.matrix];
    // The joint is taken to the skeleton centre before the bind matrices multiply it: the
    // product's translation then stays metres, not region coordinates whose float steps are
    // microns, and the rounding left is the joint's own, shared by every layer it moves.
    // Layered meshes with different bind matrices keep their micron offsets this way.
    Rows joint = predict(view, job.joint);
    const vec3 center = skeletonOf(view, job.skeleton).boxCenter;
    for (int row = 0; row < 3; ++row) joint.r[row].w -= center[row];
    RowsWriteRef(view.skinTables[SKIN_TABLE_PALETTE]).items[entry] = multiply(joint, matrix);
}

// The finest LOD that is in and fine enough, as the cull shader picks for other instances.
uint chooseLod(ViewRef view, uint id, vec3 center, float size, uint paletteCount)
{
    GeometriesRef geometries = GeometriesRef(push.geometries);
    const float ratio = distance(center, view.camera.xyz) * view.projection.w / size;
    uint chosen = 0u;
    for (uint lod = 0u; lod < MAX_LODS && id != 0u; ++lod)
    {
        const Geometry geometry = geometries.items[id];
        if (geometry.clusterCount != 0u && geometry.skinJoints != 0u && geometry.skinJoints <= paletteCount)
        {
            chosen = id;
            if (ratio <= geometry.coarserAt) break;
        }
        id = geometry.coarser;
    }
    return chosen;
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
        const Rows pose = predict(view, binding.palette);
        const Rows placed = multiply(pose, RowsRef(view.skinTables[SKIN_TABLE_MATRICES]).items[binding.matrix]);
        // Row by row: a whole array written to device memory does not translate to Metal.
        instances.items[binding.instance].rows[0] = placed.r[0];
        instances.items[binding.instance].rows[1] = placed.r[1];
        instances.items[binding.instance].rows[2] = placed.r[2];
        instances.items[binding.instance].lod = vec4(rowsPoint(pose, binding.lod.xyz), binding.lod.w);
        return;
    }

    // Demand is per frame, reset before either culling phase can admit this binding.
    bindings.items[id].skinDemand = SKIN_DEMAND_NONE;
    // Deformed positions are float metres relative to the same skeleton centre.
    // No per-face grid or changing scale can move a shared boundary vertex.
    const vec3 center = skeleton.boxCenter;
    instances.items[binding.instance].rows[0] = vec4(1.0, 0.0, 0.0, center.x);
    instances.items[binding.instance].rows[1] = vec4(0.0, 1.0, 0.0, center.y);
    instances.items[binding.instance].rows[2] = vec4(0.0, 0.0, 1.0, center.z);
    const float size = binding.lod.w > 0.0 ? binding.lod.w : bindingBox(skeleton, binding);
    instances.items[binding.instance].lod = vec4(center, size);

    // Outside the view: nothing to skin or draw this frame. bindingBox() bounds every
    // deformed vertex per axis, so its sphere is the box's half diagonal. One that comes
    // into the view is skinned in that same frame.
    const bool seen = insideView(view, center, bindingBox(skeleton, binding) * 1.7321);
    const uint chosen = seen ? chooseLod(view, binding.source, center, size, binding.matrix) : 0u;
    const Geometry lod = GeometriesRef(push.geometries).items[chosen];
    Geometry own;
    own.firstGroup = binding.targetEntry;
    own.groupCount = lod.groupCount;
    own.skinJoints = 0u;
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
        bindings.items[id].weights = first.attributes + 16ul * vertexCountOf(first.positions, first.attributes);
    }
}

// The binding of this workgroup's block, and the block's place inside it. False when the
// block is free, or its binding draws nothing this frame.
// Vertex blocks come from SKIN_TABLE_BLOCKS, cluster blocks from SKIN_TABLE_CLUSTER_BLOCKS.
bool blockBinding(ViewRef view, bool clusterBlocks, out Binding binding, out uint block)
{
    const uint index = gl_WorkGroupID.x;
    if (index >= (clusterBlocks ? view.clusterBlockEnd : view.blockEnd))
    {
        return false;
    }
    const uint id = WordsRef(view.skinTables[clusterBlocks ? SKIN_TABLE_CLUSTER_BLOCKS : SKIN_TABLE_BLOCKS]).words[index];
    if (id == 0u)
    {
        return false;
    }
    binding = BindingsRef(view.skinTables[SKIN_TABLE_BINDINGS]).items[id];
    block = index - (clusterBlocks ? binding.firstClusterBlock : binding.firstBlock);
    return binding.kind == BINDING_SKINNED && binding.chosen != 0u;
}

uint64_t ownPositions(ViewRef view, Binding binding)
{
    return (view.skinTables[SKIN_TABLE_POSITIONS] + uint64_t(binding.positions) * 16ul) | 1ul;
}

int snorm16(float value)
{
    return int(round(clamp(value, -1.0, 1.0) * 32767.0));
}

// As buildGeometry() packs a normal: octahedral, two signed 16-bit halves.
uint packOctahedral(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-20);
    vec2 p = n.xy;
    if (n.z < 0.0)
    {
        p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    }
    return (uint(snorm16(p.x)) & 0xFFFFu) | (uint(snorm16(p.y)) << 16);
}

// A morphed vertex rebuilt from its base, as the viewer's sums give it: positions and UVs
// add their weighted deltas, normals and binormals add softened ones and are normalized,
// and the tangent is the binormal crossed with the normal.
void morphVertex(ViewRef view)
{
    const uint index = gl_WorkGroupID.x;
    if (index >= view.morphWorkEnd)
    {
        return;
    }
    const MorphWork work = MorphWorkRef(view.skinTables[SKIN_TABLE_MORPH_WORK]).items[index];
    const Morph morph = MorphsRef(view.skinTables[SKIN_TABLE_MORPHS]).items[work.morph];
    const uint vertex = work.firstVertex + gl_LocalInvocationID.x;
    if (vertex >= morph.vertexCount)
    {
        return;
    }
    const MorphVertex base = MorphVerticesRef(view.skinTables[SKIN_TABLE_MORPH_VERTICES]).items[morph.firstVertex + vertex];
    MorphDeltasRef deltas = MorphDeltasRef(view.skinTables[SKIN_TABLE_MORPH_DELTAS]);
    FloatsRef values = FloatsRef(view.skinTables[SKIN_TABLE_MORPH_WEIGHTS]);
    vec3 position = base.position;
    vec3 normal = base.normal;
    vec3 binormal = base.binormal;
    vec2 uv = vec2(base.u, base.v);
    for (uint i = 0u; i < base.deltaCount; ++i)
    {
        const MorphDelta delta = deltas.items[base.firstDelta + i];
        float weight = values.values[morph.weights + delta.target];
        if (delta.mask != MORPH_NO_MASK)
        {
            weight *= values.values[morph.masks + delta.mask];
        }
        position += weight * delta.position;
        normal += weight * morph.normalSoften * delta.normal;
        binormal += weight * morph.normalSoften * delta.binormal;
        uv += weight * vec2(delta.u, delta.v);
    }
    normal = dot(normal, normal) > 0.0 ? normalize(normal) : vec3(0.0, 0.0, 1.0);
    vec3 tangent = cross(binormal, normal);
    tangent = dot(tangent, tangent) > 0.0 ? normalize(tangent) : vec3(1.0, 0.0, 0.0);

    const uint at = morph.outputIndex + vertex;
    Vec4Ref(view.skinTables[SKIN_TABLE_MORPHED_POSITIONS]).items[at] = vec4(position, 1.0);
    PositionsWriteRef attributes = PositionsWriteRef(view.skinTables[SKIN_TABLE_MORPHED_ATTRIBUTES]);
    attributes.words[at * 4u] = packOctahedral(normal);
    attributes.words[at * 4u + 1u] = packOctahedral(tangent) & ~0x10000u; // handedness bit clear: positive
    attributes.words[at * 4u + 2u] = floatBitsToUint(uv.x);
    attributes.words[at * 4u + 3u] = floatBitsToUint(uv.y);
}

// Direct float result also serves as the audit reference, before output storage.
vec3 skinnedPoint(ViewRef view, Binding binding, uint vertex)
{
    const Geometry lod = GeometriesRef(push.geometries).items[binding.chosen];
    const Cluster first = ClustersRef(push.clusters).items[lod.firstCluster];
    vec3 mesh;
    if (binding.morph != 0u)
    {
        const Morph morph = MorphsRef(view.skinTables[SKIN_TABLE_MORPHS]).items[binding.morph];
        mesh = Vec4Ref(view.skinTables[SKIN_TABLE_MORPHED_POSITIONS]).items[morph.outputIndex + vertex].xyz;
    }
    else
    {
        mesh = packedPosition(first.positions, vertex) * binding.sourceBox.w + binding.sourceBox.xyz;
    }
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

    return skinned;
}

void skinVertex(ViewRef view)
{
    Binding binding;
    uint block;
    if (!blockBinding(view, false, binding, block)) return;
    // Audit mode deforms all chosen bindings early, so its no-occlusion reference
    // never reads old positions. Normal frames deform only actual draw consumers.
    if (view.skinAudit != 0u)
    {
        if (push.phase == SKIN_VERTICES_LATE) return;
    }
    else if (binding.skinDemand != (push.phase == SKIN_VERTICES_LATE ? SKIN_DEMAND_LATE : SKIN_DEMAND_EARLY)) return;
    const uint vertex = block * SKIN_BLOCK + gl_LocalInvocationID.x;
    const Geometry lod = GeometriesRef(push.geometries).items[binding.chosen];
    const Cluster first = ClustersRef(push.clusters).items[lod.firstCluster];
    if (vertex >= vertexCountOf(first.positions, first.attributes)) return;
    const vec3 skinned = skinnedPoint(view, binding, vertex);
    Vec4Ref(ownPositions(view, binding) & ~1ul).items[vertex] = vec4(skinned, 1.0);
}

#include "asrSceneSkinAudit.glsl"

// As buildGeometry() packs a cone: rounded so it never claims more than the real one.
uint coneBits(vec3 axis, float cutoff)
{
    const vec4 values = vec4(axis, min(1.0, cutoff + 4.0 / 127.0));
    const ivec4 bytes = ivec4(clamp(round(values * 127.0), -127.0, 127.0));
    return (uint(bytes.x) & 0xFFu) | ((uint(bytes.y) & 0xFFu) << 8) | ((uint(bytes.z) & 0xFFu) << 16) | ((uint(bytes.w) & 0xFFu) << 24);
}

// Stores one deformed cluster from its bounds: one thread per cluster calls this.
void writeSkinnedCluster(ViewRef view, Binding binding, Cluster source, uint64_t positions, uint local,
                         vec3 middle, float radius, uint cone)
{
    radius += max(1e-5, length(middle) * 1e-6); // conservative float roundoff

    Cluster own;
    own.center = middle;
    own.radius = radius;
    own.cone = cone;
    own.triangles = source.triangles;
    own.indices = source.indices;
    own.positions = positions;
    own.attributes = source.attributes;
    if (binding.morph != 0u)
    {
        // Normals and UVs as morphed: the resolve and masked surfaces read them from here.
        const Morph morph = MorphsRef(view.skinTables[SKIN_TABLE_MORPHS]).items[binding.morph];
        own.attributes = view.skinTables[SKIN_TABLE_MORPHED_ATTRIBUTES] + uint64_t(morph.outputIndex) * 16ul;
    }
    ClustersWriteRef(push.clusters).items[binding.targetEntry + binding.groupCapacity + local] = own;

    // The geometry's sphere around the packed origin. Positive floats order as their bits.
    GeometryWordsRef geometryWords = GeometryWordsRef(push.geometries);
    atomicMax(geometryWords.words[binding.target * 8u + 3u], floatBitsToUint(length(middle) + radius));
}

// An upper bound of how much the rows' linear part can stretch a length: the square root of
// the largest Gershgorin row sum of L Lt. Exact for rotations and uniform scales.
float stretchOf(Rows rows)
{
    const vec3 a = rows.r[0].xyz;
    const vec3 b = rows.r[1].xyz;
    const vec3 c = rows.r[2].xyz;
    const float ab = abs(dot(a, b));
    const float ac = abs(dot(a, c));
    const float bc = abs(dot(b, c));
    return sqrt(max(dot(a, a) + ab + ac, max(dot(b, b) + ab + bc, dot(c, c) + ac + bc)));
}

// Whether the rows only turn and scale evenly, so face normals turn as the rows do. A mirror
// fails: it flips them.
bool turnsOnly(Rows rows)
{
    const vec3 a = rows.r[0].xyz;
    const vec3 b = rows.r[1].xyz;
    const vec3 c = rows.r[2].xyz;
    const float scale = dot(a, a);
    const float tolerance = scale * 1e-3;
    return scale > 0.0 && dot(cross(a, b), c) > 0.0 && abs(dot(b, b) - scale) <= tolerance && abs(dot(c, c) - scale) <= tolerance
        && abs(dot(a, b)) <= tolerance && abs(dot(a, c)) <= tolerance && abs(dot(b, c)) <= tolerance;
}

// One thread per cluster, with no vertex read. A skinned vertex is the weighted sum of where
// each of its joints carries it, so with weights that sum to one it lies among those points,
// and the cluster's rest sphere (widened by the morph bound) carried through each of its
// joints holds it. buildGeometry() lists the joints after the cluster's indices: their
// count, how far a weight sum strays from one (in 4/65535ths), then the joints.
void skinCluster(ViewRef view)
{
    Binding binding;
    uint block;
    if (!blockBinding(view, true, binding, block)) return;
    const Geometry lod = GeometriesRef(push.geometries).items[binding.chosen];
    const uint local = block * SKIN_BLOCK + gl_LocalInvocationID.x;
    if (local >= lod.clusterCount) return;
    const Cluster source = ClustersRef(push.clusters).items[lod.firstCluster + local];

    const uint list = source.triangles * 3u;
    const uint jointCount = clusterIndex(source.indices, list);
    const float stray = float(clusterIndex(source.indices, list + 1u)) * (4.0 / 65535.0);
    const float bound = binding.morph != 0u ? MorphsRef(view.skinTables[SKIN_TABLE_MORPHS]).items[binding.morph].bound : 0.0;
    const vec3 restCenter = source.center * binding.sourceBox.w + binding.sourceBox.xyz;
    const float restRadius = source.radius * binding.sourceBox.w + bound;
    RowsRef palette = RowsRef(view.skinTables[SKIN_TABLE_PALETTE]);

    vec3 low = vec3(3.402823e38);
    vec3 high = -low;
    for (uint i = 0u; i < jointCount; ++i)
    {
        const vec3 carried = rowsPoint(palette.items[binding.palette + clusterIndex(source.indices, list + 2u + i)], restCenter);
        low = min(low, carried);
        high = max(high, carried);
    }
    const vec3 middle = jointCount != 0u ? (low + high) * 0.5 : vec3(0.0);
    float radius = 0.0;
    for (uint i = 0u; i < jointCount; ++i)
    {
        const Rows rows = palette.items[binding.palette + clusterIndex(source.indices, list + 2u + i)];
        radius = max(radius, distance(rowsPoint(rows, restCenter), middle) + restRadius * stretchOf(rows));
    }
    // A sum s times a point among them is off it by at most |s - 1| times its length.
    radius += stray * (length(middle) + radius);

    // The facing cone holds only when the whole cluster turns rigidly with one joint.
    uint cone = 127u << 24; // off
    if (jointCount == 1u && stray == 0.0 && binding.morph == 0u)
    {
        const Rows rows = palette.items[binding.palette + clusterIndex(source.indices, list + 2u)];
        const float sine = float(int(source.cone) >> 24) / 127.0;
        if (turnsOnly(rows) && sine < 1.0)
        {
            const ivec3 bytes = ivec3(int(source.cone << 24) >> 24, int(source.cone << 16) >> 24, int(source.cone << 8) >> 24);
            const vec3 axis = vec3(bytes) / 127.0;
            const vec3 turned = vec3(dot(rows.r[0].xyz, axis), dot(rows.r[1].xyz, axis), dot(rows.r[2].xyz, axis));
            if (dot(turned, turned) > 0.0)
            {
                cone = coneBits(normalize(turned), sine);
            }
        }
    }
    writeSkinnedCluster(view, binding, source, ownPositions(view, binding), local, middle, radius, cone);
}

// A workgroup per binding.
void skinGroups(ViewRef view)
{
    const uint id = gl_WorkGroupID.x;
    if (id >= view.bindingEnd)
    {
        return;
    }
    const Binding binding = BindingsRef(view.skinTables[SKIN_TABLE_BINDINGS]).items[id];
    if (binding.kind != BINDING_SKINNED || binding.chosen == 0u)
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
        vec3 low = vec3(3.402823e38);
        vec3 high = -low;
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

// Position fidelity is checked only after the vertex pass, independently of bounds.
void auditPositions(ViewRef view)
{
    if (view.skinAudit == 0u) return;
    Binding binding;
    uint block;
    if (!blockBinding(view, true, binding, block)) return;
    const Geometry lod = GeometriesRef(push.geometries).items[binding.chosen];
    const uint local = block * SKIN_BLOCK + gl_LocalInvocationID.x;
    if (local >= lod.clusterCount) return;
    auditSkinCluster(view, binding, ClustersRef(push.clusters).items[lod.firstCluster + local], ownPositions(view, binding));
}

void main()
{
    ViewRef view = ViewRef(push.view);
    switch (push.phase)
    {
        case SKIN_POSE:
            predictJoint(view, gl_GlobalInvocationID.x);
            break;
        case SKIN_PALETTE:
            makePalette(view, gl_GlobalInvocationID.x);
            break;
        case SKIN_BIND:
            bind(view, gl_GlobalInvocationID.x);
            break;
        case SKIN_MORPH:
            morphVertex(view);
            break;
        case SKIN_VERTICES:
        case SKIN_VERTICES_LATE:
            skinVertex(view);
            break;
        case SKIN_CLUSTERS:
            skinCluster(view);
            break;
        case SKIN_GROUPS:
            skinGroups(view);
            break;
        case SKIN_AUDIT:
            auditPositions(view);
            break;
    }
}
