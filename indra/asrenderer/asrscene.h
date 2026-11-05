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

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace asr
{

constexpr uint32_t CLUSTER_MAX_TRIANGLES = 64;

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
constexpr uint32_t INSTANCE_DESC_FLAGS = INSTANCE_DOUBLE_SIDED | INSTANCE_MASKED;

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

constexpr uint32_t CULL_GROUP_SIZE = 64;
constexpr uint32_t PYRAMID_GROUP_SIZE = 16;
constexpr uint32_t PYRAMID_LEVELS_PER_DISPATCH = 3;
constexpr uint32_t RESOLVE_GROUP_SIZE = 8;

// Which job the cull shader does.
constexpr uint32_t CULL_EARLY = 0;
constexpr uint32_t CULL_LATE = 1;
constexpr uint32_t CULL_AUDIT = 2; // everything in view, with no occlusion test

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

struct GpuGeometry
{
    float center[3]; // bounding sphere of the whole geometry
    float radius;
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
    uint32_t unused;
};
static_assert(sizeof(GpuInstance) == 80, "shaders read this layout");

struct GpuMaterial
{
    float color[4];
    float uvRow0[3];  // 2x3 matrix from face UVs to texture UVs
    uint32_t texture; // see uiTextureBits()
    float uvRow1[3];
    float alphaCutoff; // masked instances keep pixels at or above this alpha
};
static_assert(sizeof(GpuMaterial) == 48, "shaders read this layout");

// A VkDrawIndirectCommand, and also Metal's MTLDrawPrimitivesIndirectArguments.
struct GpuDrawArguments
{
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;
};
static_assert(sizeof(GpuDrawArguments) == 16, "APIs read this layout");

// What culling fills in. The late lists continue where the early ones ended.
struct GpuSceneCounts
{
    GpuDrawArguments early[SCENE_LISTS];
    GpuDrawArguments late[SCENE_LISTS];
    uint32_t retestGroups[3]; // arguments of the indirect dispatch of the late cull
    uint32_t retestCount;
};
static_assert(sizeof(GpuSceneCounts) == 528, "shaders read this layout");

// How the counts start each frame: only the draws' sizes are set.
GpuSceneCounts sceneCountsTemplate();

// An entry of the visible list and of the retest list.
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
    uint32_t list;
    uint32_t phase; // one of CULL_*, or the pyramid level being read
};
static_assert(sizeof(ScenePush) == 80, "shaders read this layout");

// One cluster of a built geometry. Offsets count from the start of GeometryData::blob.
struct ClusterData
{
    float center[3];
    float radius;
    uint32_t cone;
    uint32_t triangles;
    uint32_t indexOffset; // in bytes, and four-byte aligned
};

// A geometry ready for the GPU: built on any thread, and moved into the queue. The blob
// is indices, then positions, then attributes, and goes up in one copy.
struct GeometryData
{
    std::vector<uint8_t> blob;
    std::vector<ClusterData> clusters;
    uint32_t positionsOffset = 0;
    uint32_t attributesOffset = 0;
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
// With no box given, the face's own bounds are used.
GeometryData buildGeometry(const SceneVertex* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount,
                           const GeometryBox* box = nullptr);

struct SceneCamera3D
{
    float viewProjection[16]; // column-major, Y down, reversed depth; see makeCamera()
    float position[3];
    float viewRows[3][4];     // world to camera, looking down -Z
    float focal[2];           // the projection's x and y scales
    float nearClip;
    float lodScale;           // distances count this much more: above 1 lowers detail
};

// A look-at camera with reversed depth and no far plane. World space is the viewer's:
// right-handed, Z up. Y comes out flipped, as Vulkan wants it, and asr_spirv2msl turns
// that back for Metal, so one matrix suits both.
SceneCamera3D makeCamera(const float eye[3], const float target[3], float verticalFovRadians, float aspect, float nearClip);

// Per view constants read by every scene shader.
struct GpuSceneView
{
    float viewProjection[16];
    float camera[4];
    float planes[5][4];       // pointing inwards: left, right, top, bottom, near
    float pixelSize[4];       // width, height, 1 / width, 1 / height
    float viewRows[3][4];
    float previousRows[3][4]; // the view the depth pyramid was made from
    float projection[4];      // focal x, focal y, near, LOD scale
    float previousProjection[4]; // focal x, focal y, near, and 0 when the pyramid cannot be used
    uint32_t listBase[SCENE_LISTS]; // where each list starts in the visible list
    uint32_t pyramidOffset[PYRAMID_MAX_LEVELS]; // where each level starts, in floats; level 1 is first
    uint32_t instanceEnd;
    uint32_t pyramidLevels;
    uint32_t unused[2];
};
static_assert(sizeof(GpuSceneView) == 448, "shaders read this layout");

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
    uint32_t flags = 0;      // INSTANCE_DOUBLE_SIDED, INSTANCE_MASKED
    float objectToWorld[12]; // three rows
    float lodCenter[3] = { 0.f, 0.f, 0.f }; // shared by an object's faces, so they agree on an LOD
    float lodSize = 0.f;     // 0 uses the instance's own bounding sphere
};

struct MaterialDesc
{
    float color[4] = { 1.f, 1.f, 1.f, 1.f };
    float uvRow0[3] = { 1.f, 0.f, 0.f };
    float uvRow1[3] = { 0.f, 1.f, 0.f };
    uint32_t texture = 0;
    float alphaCutoff = 0.5f;
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

// Handed over in one piece by SceneQueue::take().
struct SceneCommands
{
    std::vector<MaterialCommand> materials;
    std::vector<GeometryCommand> geometries;
    std::vector<InstanceCommand> instances;
    std::vector<uint32_t> removedGeometries;

    bool empty() const
    {
        return materials.empty() && geometries.empty() && instances.empty() && removedGeometries.empty();
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

    uint32_t addInstance(const InstanceDesc& desc);
    void setInstance(uint32_t id, const InstanceDesc& desc);
    void removeInstance(uint32_t id);

    // Render thread.
    void take(SceneCommands& commands);
    void releaseGeometry(uint32_t id);
    void releaseInstance(uint32_t id);

private:
    std::mutex mMutex; // held only to move a command, so neither thread really waits
    SceneCommands mCommands;
    std::vector<uint32_t> mFreeGeometries;
    std::vector<uint32_t> mFreeInstances;
    uint32_t mNextGeometry = 1; // 0 is the empty geometry
    uint32_t mNextInstance = 0;
    uint32_t mNextMaterial = 1; // 0 is plain white
};

} // namespace asr
