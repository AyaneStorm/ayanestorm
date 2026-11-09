/**
 * asrscene.h
 * The persistent GPU scene of section 4 of the renderer plan, as seen by the threads
 * that feed it. Nothing here touches a GPU API.
 *
 * Geometry is cut into clusters of at most 64 triangles, and clusters are what the
 * GPU culls and draws. A geometry is one face of one volume at one LOD, and is shared
 * by every instance that shows it, so a moved object updates one transform and nothing
 * else. Ids exist at once, as with textures, and the render thread does the real work.
 * Each geometry can name a coarser one, and the GPU walks that chain to pick an LOD.
 *
 * The Gpu* structs and the constants are mirrored in shaders/asrSceneCommon.glsl.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace asr
{

constexpr uint32_t CLUSTER_MAX_TRIANGLES = 64;

// Clusters near each other form a group, with bounds and a normal cone of its own, so one
// test can drop them all. Culling tests an instance's groups, then gives every group that
// passes one thread per cluster, so a dense mesh is not walked by a single thread. A face
// is at most 65,535 vertices, so groups of an instance's geometry never number more than a
// few dozen. A geometry this small or smaller has its clusters tested directly.
constexpr uint32_t CLUSTER_GROUP_SIZE = 32;
constexpr uint32_t CLUSTERS_WITHOUT_GROUPS = 4;

// Clusters are drawn in instanced draws of a fixed size, which needs no index buffer,
// no per-object draw and no command list, on either API. A cluster goes in the smallest
// size that holds it, so a two-triangle prim face does not pay for 64.
constexpr uint32_t CLUSTER_BUCKETS = 4;
constexpr uint32_t CLUSTER_BUCKET_TRIANGLES[CLUSTER_BUCKETS] = { 2, 8, 32, 64 };

// A pixel of the visibility buffer is (visible cluster << 6) | triangle.
constexpr uint32_t MAX_VISIBLE_CLUSTERS = 1u << 26;
constexpr uint32_t VISIBILITY_EMPTY = 0xFFFFFFFFu;

constexpr uint32_t INSTANCE_FLIPPED = 1u;      // a negative scale: winding is reversed
constexpr uint32_t INSTANCE_DOUBLE_SIDED = 2u; // drawn from both sides
constexpr uint32_t INSTANCE_MASKED = 4u;       // holes, from the texture's alpha
constexpr uint32_t INSTANCE_SKINNED = 8u;      // set by the store: its positions follow joints, see GpuBinding
constexpr uint32_t INSTANCE_BOUND = 16u;       // set by the store: on a skeleton, and GpuInstance::binding names how

// Surfaces in the same plane would tie in depth, and the order of a list changes from frame
// to frame, so which one shows would flicker. Each rank sits a fixed share of its distance
// nearer, see the vertex shader, so the higher one always wins, at any camera position.
constexpr uint32_t INSTANCE_RANK_SHIFT = 8;
constexpr uint32_t INSTANCE_RANK_MAX = 1023;
constexpr uint32_t INSTANCE_DESC_FLAGS = INSTANCE_DOUBLE_SIDED | INSTANCE_MASKED | (INSTANCE_RANK_MAX << INSTANCE_RANK_SHIFT);

// Each kind of surface has its own draw state, so each has its own lists: double-sided
// is bit 0 of the kind, and masked is bit 1. The list a cluster goes in comes from its
// instance's kind and its own size.
constexpr uint32_t SCENE_KINDS = 4;
constexpr uint32_t SCENE_LISTS = SCENE_KINDS * CLUSTER_BUCKETS;

constexpr uint32_t sceneKind(uint32_t instanceFlags)
{
    return (instanceFlags >> 1) & (SCENE_KINDS - 1);
}

constexpr uint32_t sceneList(uint32_t kind, uint32_t bucket)
{
    return kind * CLUSTER_BUCKETS + bucket;
}

constexpr uint32_t MAX_LODS = 4;           // the longest chain of coarser geometries the GPU walks
constexpr uint32_t PYRAMID_MAX_LEVELS = 16;
constexpr uint32_t RETEST_WHOLE_INSTANCE = 0xFFFFFFFFu; // in GpuListEntry::cluster
constexpr uint32_t RETEST_GROUP = 0x80000000u;          // in GpuListEntry::cluster: the rest is a group

constexpr uint32_t CULL_GROUP_SIZE = 64;
constexpr uint32_t PYRAMID_GROUP_SIZE = 16;
constexpr uint32_t PYRAMID_LEVELS_PER_DISPATCH = 3;
constexpr uint32_t RESOLVE_GROUP_SIZE = 8;

// Texture feedback: the resolve pass records, for each texture slot, the sharpest size a
// sampled pixel needed, as the GPU really sampled it: after occlusion, repeats and every
// UV transform. It looks at one pixel in each block of 4 by 4, at a place that moves every
// frame, so a window of 16 frames or more sees every pixel. A slot's word is
// (window << FEEDBACK_LEVEL_BITS) | (level + 1): atomicMax makes a newer window win, so the
// buffer is never cleared, and level is log2 of the texels across the texture needs. The
// window number in GpuSceneView::feedback is (window << 4) | the place in the block, and
// 0 there turns feedback off.
constexpr uint32_t FEEDBACK_SAMPLES = 16;
constexpr uint32_t FEEDBACK_LEVEL_BITS = 8;
constexpr uint32_t FEEDBACK_WINDOW_LIMIT = 1u << (32 - FEEDBACK_LEVEL_BITS); // windows wrap here, and the buffer is cleared
constexpr uint32_t FEEDBACK_MAX_LEVEL = 15; // 32,768 texels across

// Which job the cull shader does. The first three cull instances, or retest entries; the
// others test the clusters of the groups those passed, one thread per cluster.
constexpr uint32_t CULL_EARLY = 0;
constexpr uint32_t CULL_LATE = 1;
constexpr uint32_t CULL_AUDIT = 2; // everything in view, with no occlusion test
constexpr uint32_t CULL_GROUPS_EARLY = 3;
constexpr uint32_t CULL_GROUPS_LATE = 4;
constexpr uint32_t CULL_GROUPS_AUDIT = 5;
constexpr uint32_t CULL_GROUPS_PER_WORKGROUP = CULL_GROUP_SIZE / CLUSTER_GROUP_SIZE;

// What a volume face provides.
struct SceneVertex
{
    float position[3];
    float normal[3];
    float tangent[4]; // w is the handedness
    float uv[2];
};

struct PackedPosition
{
    int16_t xyz[3]; // inside the geometry's bounds, see GeometryData::scale
    int16_t unused;
};
static_assert(sizeof(PackedPosition) == 8, "shaders read this layout");

struct PackedAttributes
{
    int16_t normal[2];  // octahedral
    int16_t tangent[2]; // octahedral; the lowest bit of tangent[1] is the handedness
    float uv[2];
};
static_assert(sizeof(PackedAttributes) == 16, "shaders read this layout");

struct GpuCluster
{
    float center[3];     // bounding sphere, in the positions' own scale
    float radius;
    uint32_t cone;       // normals: axis and cutoff as four signed bytes
    uint32_t triangles;
    uint64_t indices;    // device addresses
    uint64_t positions;
    uint64_t attributes;
};
static_assert(sizeof(GpuCluster) == 48, "shaders read this layout");

// A geometry's groups sit in the cluster table just before its clusters, so they come and
// go with them as one range.
struct GpuClusterGroup
{
    float center[3]; // bounding sphere of its clusters, in the positions' own scale
    float radius;
    uint32_t cone;   // as a cluster's, holding the normals of all its triangles
    uint32_t firstCluster;
    uint32_t clusterCount;
    uint32_t unused[5];
};
static_assert(sizeof(GpuClusterGroup) == sizeof(GpuCluster), "shares the cluster table");

// A skinned vertex's joints and weights. Weights add up to 65535. The blob keeps them right
// after the attributes, so a shader finds them from a cluster's two addresses alone.
struct SkinWeights
{
    uint8_t joints[4];   // of the skin, not of the skeleton
    uint16_t weights[4];
};
static_assert(sizeof(SkinWeights) == 12, "shaders read this layout");

struct GpuGeometry
{
    uint32_t firstGroup;
    uint32_t groupCount;   // 0 when its clusters are few enough to test directly
    uint32_t unused;
    float radius;          // bounding sphere around the packed origin
    uint32_t firstCluster;
    uint32_t clusterCount; // 0 until its data has reached the GPU
    uint32_t coarser;      // the next LOD, or 0
    float coarserAt;       // distance / object size from which that LOD is used
};
static_assert(sizeof(GpuGeometry) == 32, "shaders read this layout");

struct GpuInstance
{
    float rows[3][4]; // three rows of the matrix from packed positions to world
    float lodCenter[3]; // what LOD measures from: the same for every face of an object
    float lodSize;      // 0 means this instance's own bounding sphere
    uint32_t geometry;
    uint32_t material;
    uint32_t flags;
    uint32_t binding; // with INSTANCE_BOUND: its GpuBinding
};
static_assert(sizeof(GpuInstance) == 80, "shaders read this layout");

// Avatars. A skeleton's joints carry the two newest poses the main thread gave, and the GPU
// carries the motion on from them to the frame's own time, so avatars move at render rate.
// A skin's palette is its joints' poses times their inverse bind matrices, made once per
// frame per skeleton and skin, and shared by every face and every view. A skinned instance
// draws through a geometry of its own, whose positions and bounds the skin passes rewrite
// each frame, packed in its skeleton's box, so culling, drawing and the resolve treat it as
// any other. An attached instance follows one joint. See asrSceneSkinC.glsl.
constexpr uint32_t SKIN_BLOCK = 64;          // vertices or clusters per workgroup of the skin passes
constexpr uint32_t SKIN_PALETTE = 0;         // the skin passes, in order
constexpr uint32_t SKIN_BIND = 1;
constexpr uint32_t SKIN_VERTICES = 2;
constexpr uint32_t SKIN_CLUSTERS = 3;
constexpr uint32_t SKIN_GROUPS = 4;
constexpr uint32_t SKIN_PHASES = 5;
constexpr uint32_t BINDING_ATTACHED = 1;
constexpr uint32_t BINDING_SKINNED = 2;
constexpr float SKELETON_MAX_LEAD = 1.1f;    // main-thread steps the motion is carried on, at most
constexpr float SKELETON_MAX_LEAD_SECONDS = 0.25f;

struct GpuJoint
{
    float rows[3][4];     // the newest pose, in render space
    float previous[3][4]; // the one before
};
static_assert(sizeof(GpuJoint) == 96, "shaders read this layout");

struct GpuSkeleton
{
    float boxCenter[3]; // holds every vertex bound to it at any lead it can draw: packing for its skinned instances
    float boxScale;
    uint32_t firstJoint;
    uint32_t jointCount;
    uint32_t time;         // of the newest pose, and of the one before: microseconds, wrapping
    uint32_t previousTime;
    float maxLead;         // steps of motion carried on past the newest pose, at most
    uint32_t unused[3];
};
static_assert(sizeof(GpuSkeleton) == 48, "shaders read this layout");

struct GpuPaletteJob
{
    uint32_t joint;    // in the joint table
    uint32_t matrix;   // in the skin matrix table
    uint32_t skeleton;
    uint32_t unused;
};
static_assert(sizeof(GpuPaletteJob) == 16, "shaders read this layout");

struct GpuBinding
{
    uint32_t kind;        // 0 for a free entry, BINDING_ATTACHED or BINDING_SKINNED
    uint32_t instance;
    uint32_t skeleton;
    uint32_t palette;     // skinned: its first palette entry; attached: its joint, in the joint table
    uint32_t matrix;      // attached: object to joint, packing included, in the skin matrix table
    uint32_t source;      // skinned: the finest geometry of the chain it draws from
    uint32_t target;      // skinned: its own geometry
    uint32_t targetEntry; // skinned: its first entry in the cluster table: groups, then clusters
    float lod[4];         // what LOD measures from, in joint space for an attachment, and the object's size
    float sourceBox[4];   // skinned: the chain's packing box, centre and scale
    uint32_t positions;   // skinned: its first entry in the skinned position table
    uint32_t chosen;      // skinned, written by the GPU: the LOD drawn this frame
    uint32_t firstBlock;  // in the skin block table
    uint32_t groupCapacity; // groups room before its clusters
    uint64_t weights;     // skinned, written by the GPU: the chosen LOD's SkinWeights
    uint32_t morphed;     // reserved for morphs: their output, 0 for none
    uint32_t unused;
};
static_assert(sizeof(GpuBinding) == 96, "shaders read this layout");

struct GpuMaterial
{
    float color[4];
    float uvRow0[3];  // 2x3 matrix from face UVs to texture UVs
    uint32_t texture; // see uiTextureBits()
    float uvRow1[3];
    float alphaCutoff; // masked instances keep pixels at or above this alpha
    float planarScale[3];  // planar mapping: packed position to scaled object position; terrain: to region metres
    uint32_t mapping;      // MATERIAL_*: where UVs come from
    float planarOffset[3];
    uint32_t unused;
};
static_assert(sizeof(GpuMaterial) == 80, "shaders read this layout");

constexpr uint32_t MATERIAL_FACE_UV = 0; // the face's own UVs
constexpr uint32_t MATERIAL_PLANAR = 1;  // from position and normal, as the viewer's planar mapping
constexpr uint32_t MATERIAL_TERRAIN = 2; // texture is a terrain's id: its layers blend, from region metres

// Ground blends four layers, as the viewer's terrain shaders do. A layer's UVs come from the
// position in region metres, through its 2x3 matrix: from x and y, or with three planes also
// from y and z and from x and z, weighted by the normal.
constexpr uint32_t TERRAIN_LAYERS = 4;
constexpr uint32_t TERRAIN_TEXTURES = 0;  // four detail textures, mixed through the alpha ramp
constexpr uint32_t TERRAIN_MATERIALS = 1; // four PBR materials, weighted from the same ramp

struct GpuTerrainLayer
{
    float uvRow0[3];
    uint32_t texture; // see uiTextureBits()
    float uvRow1[3];
    uint32_t unused;
    float color[4];   // the material's base colour factor; white for a texture
};
static_assert(sizeof(GpuTerrainLayer) == 48, "shaders read this layout");

struct GpuTerrain
{
    GpuTerrainLayer layers[TERRAIN_LAYERS];
    uint32_t ramp;    // the alpha ramp's texture bits, read at the composition and its noise
    uint32_t kind;    // TERRAIN_TEXTURES or TERRAIN_MATERIALS
    uint32_t planes;  // 1, or 3 for triplanar
    float sharpness;  // triplanar: how sharply the planes give way to each other
};
static_assert(sizeof(GpuTerrain) == 208, "shaders read this layout");

// A VkDrawIndirectCommand, and also Metal's MTLDrawPrimitivesIndirectArguments.
struct GpuDrawArguments
{
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;
};
static_assert(sizeof(GpuDrawArguments) == 16, "APIs read this layout");

// An indirect dispatch, and how many entries it has to cover.
struct GpuWorkCount
{
    uint32_t groups[3];
    uint32_t entries;
};
static_assert(sizeof(GpuWorkCount) == 16, "shaders read this layout");

// What culling fills in. The late lists continue where the early ones ended, and so does
// the late part of the group list.
struct GpuSceneCounts
{
    GpuDrawArguments early[SCENE_LISTS];
    GpuDrawArguments late[SCENE_LISTS];
    GpuWorkCount retest;    // the late cull's entries
    GpuWorkCount groups[2]; // groups whose clusters are tested, early and late

    // Temporary, while the Mac's GPU page faults in the resolve are found: which of its
    // checks failed (GUARD_*), on how many pixels, and the largest bad value. A pixel that
    // fails one is drawn magenta and read no further, so it cannot fault.
    uint32_t guardBits;
    uint32_t guardPixels;
    uint32_t guardValue;

    // Entries a full list turned away this frame. The lists are sized for every cluster of
    // every instance, so this stays 0 below the MAX_VISIBLE_CLUSTERS cap.
    uint32_t dropped;
};
static_assert(sizeof(GpuSceneCounts) == 576, "shaders read this layout");

constexpr uint32_t GUARD_ENTRY = 1;     // a visibility pixel names an entry past the visible list
constexpr uint32_t GUARD_INSTANCE = 2;  // an entry names an instance past instanceEnd
constexpr uint32_t GUARD_CLUSTER = 4;   // an entry names a cluster past the cluster table
constexpr uint32_t GUARD_MATERIAL = 8;  // an instance names a material past the material table
constexpr uint32_t GUARD_ADDRESSES = 16; // a cluster's three addresses are not in one chunk, in order

// How the counts start each frame: only the draws' sizes are set.
GpuSceneCounts sceneCountsTemplate();

// An entry of the visible list, the retest list and the group list.
struct GpuListEntry
{
    uint32_t instance;
    uint32_t cluster;
};
static_assert(sizeof(GpuListEntry) == 8, "shaders read this layout");

// Push constants of every scene shader.
struct ScenePush
{
    uint64_t view;
    uint64_t instances;
    uint64_t geometries;
    uint64_t clusters;
    uint64_t materials;
    uint64_t visible;
    uint64_t counts;
    uint64_t retest;
    uint64_t pyramid;
    uint64_t groups; // the group list
    uint64_t feedback; // a word per texture slot: see FEEDBACK_SAMPLES
    uint64_t terrains;
    uint32_t list;
    uint32_t phase; // one of CULL_*, or the pyramid level being read
};
static_assert(sizeof(ScenePush) == 104, "shaders read this layout");

// One cluster of a built geometry. Offsets count from the start of GeometryData::blob.
struct ClusterData
{
    float center[3];
    float radius;
    uint32_t cone;
    uint32_t triangles;
    uint32_t indexOffset; // in bytes, and four-byte aligned
};

// Clusters [firstCluster, firstCluster + clusterCount) of a built geometry.
struct ClusterGroupData
{
    float center[3];
    float radius;
    uint32_t cone;
    uint32_t firstCluster;
    uint32_t clusterCount;
};

// A geometry ready for the GPU: built on any thread, and moved into the queue. The blob
// is indices, then positions, then attributes, and goes up in one copy.
struct GeometryData
{
    std::vector<uint8_t> blob;
    std::vector<ClusterData> clusters;   // in group order
    std::vector<ClusterGroupData> groups; // empty when there are CLUSTERS_WITHOUT_GROUPS or fewer
    uint32_t positionsOffset = 0;
    uint32_t attributesOffset = 0;
    uint32_t vertexCount = 0;
    bool skinned = false; // SkinWeights follow the attributes
    float center[3] = { 0.f, 0.f, 0.f }; // packed = (object - center) / scale
    float scale = 1.f;
    float radius = 0.f;                  // in packed units, around the packed origin
};

// A box to pack into. The LODs of one face must share one, because an instance's
// matrix holds it and the GPU picks the LOD after that.
struct GeometryBox
{
    float center[3];
    float scale;
};

// Cuts a face into clusters and packs it. Takes about a hundred nanoseconds per triangle,
// so it belongs on a worker. Triangles are reordered, and vertices stay where they are.
// With no box given, the face's own bounds are used. weights, one per vertex, make a
// geometry that skinned instances can draw.
GeometryData buildGeometry(const SceneVertex* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount,
                           const GeometryBox* box = nullptr, const SkinWeights* weights = nullptr);

// A square of ground: a grid of (size + 1) by (size + 1) points, row after row, x first.
// size is a power of two.
struct TerrainGrid
{
    uint32_t size = 16;
    float origin[2] = { 0.f, 0.f }; // where the first point is, in region metres
    float spacing = 1.f;            // metres between points
    const float* heights = nullptr; // one per point
    const float* normals = nullptr; // three per point
    const float* blends = nullptr;  // two per point, the vertex UV: composition and noise
};

// One LOD of a patch of ground: every step-th point inside, and at every step all points of
// its four edges, so patches next to each other share their edges exactly at any LODs and
// never crack. A cell touching an edge fans out from its centre to the edge's points. Step 1
// is the full grid; a larger step must divide size and leave at least two cells per side.
void buildTerrainLod(const TerrainGrid& grid, uint32_t step, std::vector<SceneVertex>& vertices, std::vector<uint16_t>& indices);

struct SceneCamera3D
{
    float viewProjection[16]; // column-major, Y down, reversed depth; see makeCamera()
    float position[3];
    float viewRows[3][4];     // world to camera, looking down -Z
    float focal[2];           // the projection's x and y scales
    float nearClip;
    float lodScale;           // distances count this much more: above 1 lowers detail
    float farClip;            // nothing past this is drawn; 0 means no limit
};

// A look-at camera with reversed depth and no far plane. World space is the viewer's:
// right-handed, Z up. Y comes out flipped, as Vulkan wants it, and asr_spirv2msl turns
// that back for Metal, so one matrix suits both.
SceneCamera3D makeCamera(const float eye[3], const float target[3], float verticalFovRadians, float aspect, float nearClip);

// The same from the camera's own axes, for a camera that can roll. Both are unit length.
SceneCamera3D makeCameraFromAxes(const float eye[3], const float forward[3], const float up[3], float verticalFovRadians, float aspect,
                                 float nearClip, float farClip);

// Per view constants read by every scene shader.
struct GpuSceneView
{
    float viewProjection[16];
    float camera[4];
    float planes[6][4];       // pointing inwards: left, right, top, bottom, near, and one nothing fails
    float pixelSize[4];       // width, height, 1 / width, 1 / height
    float viewRows[3][4];
    float previousRows[3][4]; // the view the depth pyramid was made from
    float projection[4];      // focal x, focal y, near, LOD scale
    float previousProjection[4]; // focal x, focal y, near, and 0 when the pyramid cannot be used
    uint32_t listBase[SCENE_LISTS]; // where each list starts in the visible list
    uint32_t pyramidOffset[PYRAMID_MAX_LEVELS]; // where each level starts, in floats; level 1 is first
    uint32_t instanceEnd;
    uint32_t pyramidLevels;
    float farClip;            // a true distance from the camera, so turning changes nothing; 0 is no limit
    uint32_t feedback;        // the texture feedback window and place: see FEEDBACK_SAMPLES
    uint32_t visibleEnd;      // entries in the visible list, and of the two tables, which the
    uint32_t clusterEnd;      // resolve checks before it reads through them: see GpuSceneCounts::guard
    uint32_t materialEnd;
    uint32_t groupEnd;        // entries in the group list; the retest list holds visibleEnd + groupEnd + instanceEnd

    // The avatar tables, in SceneTable order from Skeletons, and what the skin passes cover.
    uint64_t skinTables[8];
    uint64_t drawOnly;        // not 0: a bit per skeleton, and only instances bound to one set are drawn
    uint32_t time;            // the frame's own time, microseconds, wrapping as GpuSkeleton::time does
    uint32_t paletteEnd;
    uint32_t bindingEnd;
    uint32_t blockEnd;
};
static_assert(sizeof(GpuSceneView) == 568, "shaders read this layout");

// The time skeletons and views are stamped with.
uint32_t sceneTime(std::chrono::steady_clock::time_point time);

// previous is the camera of the frame that left the depth pyramid, or null when there is none.
void fillSceneView(GpuSceneView& view, const SceneCamera3D& camera, const SceneCamera3D* previous, uint32_t width, uint32_t height);

// A depth pyramid is a plain array of floats. Level L holds, for each block of 2^L by 2^L
// pixels, the farthest depth in it. Level 0 is the depth buffer itself. Sizes round up,
// so a texel always covers exactly its 2x2 below, and no level needs filtering. Fills
// in where each level starts, and returns the number of floats.
uint32_t pyramidLayout(uint32_t width, uint32_t height, uint32_t offsets[PYRAMID_MAX_LEVELS], uint32_t& levels);

inline uint32_t pyramidLevelSize(uint32_t pixels, uint32_t level)
{
    return (pixels + (1u << level) - 1) >> level;
}

// Instances: which geometry and material, and where.
struct InstanceDesc
{
    uint32_t geometry = 0;
    uint32_t material = 0;
    uint32_t flags = 0;      // INSTANCE_DOUBLE_SIDED, INSTANCE_MASKED, a rank
    float objectToWorld[12]; // three rows
    float lodCenter[3] = { 0.f, 0.f, 0.f }; // shared by an object's faces, so they agree on an LOD
    float lodSize = 0.f;     // 0 uses the instance's own bounding sphere

    // On a skeleton: with a skin, the geometry is skinned, objectToWorld and lodCenter are
    // unused, and LOD measures from the skeleton. Without one, the instance follows joint:
    // objectToWorld and lodCenter are then in that joint's space.
    uint32_t skeleton = 0;
    uint32_t skin = 0;
    uint32_t joint = 0;
};

// A rigged mesh's skin: for each of its joints, inverse bind times bind shape as three rows,
// and which joint of a skeleton it is. All skeletons number their joints alike.
struct SkinDesc
{
    std::vector<float> matrices; // 12 per joint
    std::vector<uint16_t> joints;
};

struct MaterialDesc
{
    float color[4] = { 1.f, 1.f, 1.f, 1.f };
    float uvRow0[3] = { 1.f, 0.f, 0.f };
    float uvRow1[3] = { 0.f, 1.f, 0.f };
    uint32_t texture = 0;
    float alphaCutoff = 0.5f;
    bool planar = false;
    float planarScale[3] = { 0.f, 0.f, 0.f };
    float planarOffset[3] = { 0.f, 0.f, 0.f };

    // Not 0: a terrain from addTerrain(), whose layers this blends, and texture is unused. The
    // planar scale and offset then take packed positions to region metres, and a vertex's
    // UV is its composition and noise.
    uint32_t terrain = 0;
};

struct TerrainLayerDesc
{
    uint32_t texture = 0;
    float uvRow0[3] = { 1.f, 0.f, 0.f };
    float uvRow1[3] = { 0.f, 1.f, 0.f };
    float color[4] = { 1.f, 1.f, 1.f, 1.f };
};

struct TerrainDesc
{
    TerrainLayerDesc layers[TERRAIN_LAYERS];
    uint32_t ramp = 0;
    uint32_t kind = TERRAIN_TEXTURES;
    uint32_t planes = 1;
    float sharpness = 8.f;
};

struct TerrainCommand
{
    uint32_t id = 0;
    TerrainDesc desc;
};

struct GeometryCommand
{
    uint32_t id = 0;
    GeometryData data;
    uint32_t coarser = 0;
    float coarserAt = 0.f;
};

struct InstanceCommand
{
    enum class Type : uint8_t
    {
        Set,
        Remove
    };
    Type type = Type::Set;
    uint32_t id = 0;
    InstanceDesc desc;
};

struct MaterialCommand
{
    uint32_t id = 0;
    MaterialDesc desc;
};

struct SkeletonCommand
{
    uint32_t id = 0;
    uint32_t jointCount = 0;  // not 0: the skeleton is new
    std::vector<float> rows;  // a pose, 12 per joint, or empty
    uint32_t time = 0;
};

struct SkinCommand
{
    uint32_t id = 0;
    SkinDesc desc;
};

// Handed over in one piece by SceneQueue::take().
struct SceneCommands
{
    std::vector<TerrainCommand> terrains;
    std::vector<MaterialCommand> materials;
    std::vector<GeometryCommand> geometries;
    std::vector<SkeletonCommand> skeletons;
    std::vector<SkinCommand> skins;
    std::vector<InstanceCommand> instances;
    std::vector<uint32_t> removedGeometries;
    std::vector<uint32_t> removedMaterials;
    std::vector<uint32_t> removedTerrains;
    std::vector<uint32_t> removedSkeletons;
    std::vector<uint32_t> removedSkins;

    bool empty() const
    {
        return terrains.empty() && materials.empty() && geometries.empty() && skeletons.empty() && skins.empty() && instances.empty() &&
               removedGeometries.empty() && removedMaterials.empty() && removedTerrains.empty() && removedSkeletons.empty() &&
               removedSkins.empty();
    }
};

// What the main thread and workers call while the render thread runs. Instances
// can name a geometry before its data has reached the GPU, and appear once it has.
class SceneQueue
{
public:
    SceneQueue();

    // coarser is a geometry added earlier, packed in the same box, that takes over
    // once distance / object size passes coarserAt. Instances name the finest LOD.
    uint32_t addGeometry(GeometryData&& data, uint32_t coarser = 0, float coarserAt = 0.f);

    // Takes effect once no instance, and no finer LOD, still uses it.
    void removeGeometry(uint32_t id);

    uint32_t addMaterial(const MaterialDesc& desc);
    void setMaterial(uint32_t id, const MaterialDesc& desc);
    void removeMaterial(uint32_t id);

    // A ground blend, which materials name by MaterialDesc::terrain. Ids start at 1.
    uint32_t addTerrain(const TerrainDesc& desc);
    void setTerrain(uint32_t id, const TerrainDesc& desc);
    void removeTerrain(uint32_t id);

    uint32_t addInstance(const InstanceDesc& desc);
    void setInstance(uint32_t id, const InstanceDesc& desc);
    void removeInstance(uint32_t id);

    // An avatar's or animated object's joints. Ids start at 1. A pose is jointCount
    // matrices of three rows, joint to render space, as of time; the GPU carries the motion
    // between the last two on to each frame's time. Removal waits for its last instance.
    uint32_t addSkeleton(uint32_t jointCount);
    void poseSkeleton(uint32_t id, const float* rows, uint32_t jointCount, std::chrono::steady_clock::time_point time);
    void removeSkeleton(uint32_t id);

    // Ids start at 1. Removal waits for its last instance.
    uint32_t addSkin(SkinDesc&& desc);
    void removeSkin(uint32_t id);

    // Render thread.
    void take(SceneCommands& commands);
    uint32_t reserveGeometry(); // for the store's own geometries
    void releaseGeometry(uint32_t id);
    void releaseInstance(uint32_t id);
    void releaseMaterial(uint32_t id);
    void releaseTerrain(uint32_t id);
    void releaseSkeleton(uint32_t id);
    void releaseSkin(uint32_t id);

private:
    std::mutex mMutex; // held only to move a command, so neither thread really waits
    SceneCommands mCommands;
    std::vector<uint32_t> mFreeGeometries;
    std::vector<uint32_t> mFreeInstances;
    std::vector<uint32_t> mFreeMaterials;
    std::vector<uint32_t> mFreeTerrains;
    std::vector<uint32_t> mFreeSkeletons;
    std::vector<uint32_t> mFreeSkins;
    uint32_t mNextGeometry = 1; // 0 is the empty geometry
    uint32_t mNextInstance = 0;
    uint32_t mNextMaterial = 1; // 0 is plain white
    uint32_t mNextTerrain = 1;  // 0 means none
    uint32_t mNextSkeleton = 1;
    uint32_t mNextSkin = 1;
};

} // namespace asr
