/**
 * asrscene.h
 * The persistent GPU scene of section 4 of the renderer plan, as seen by the threads
 * that feed it. Nothing here touches a GPU API.
 *
 * Geometry is cut into clusters of at most 64 triangles, and clusters are what the
 * GPU culls and draws. A geometry is one face of one volume at one LOD, and is shared
 * by every instance that shows it, so a moved object updates one transform and nothing
 * else. Ids exist at once, as with textures, and the render thread does the real work.
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

constexpr uint32_t INSTANCE_FLIPPED = 1u; // a negative scale: winding is reversed

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
    uint32_t unused[2];
};
static_assert(sizeof(GpuGeometry) == 32, "shaders read this layout");

struct GpuInstance
{
    float rows[3][4]; // three rows of the matrix from packed positions to world
    uint32_t geometry;
    uint32_t material;
    uint32_t flags;
    uint32_t unused;
};
static_assert(sizeof(GpuInstance) == 64, "shaders read this layout");

struct GpuMaterial
{
    float color[4];
    float uvRow0[3];  // 2x3 matrix from face UVs to texture UVs
    uint32_t texture; // see uiTextureBits()
    float uvRow1[3];
    uint32_t flags;
};
static_assert(sizeof(GpuMaterial) == 48, "shaders read this layout");

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

// Cuts a face into clusters and packs it. Takes about a hundred nanoseconds per triangle,
// so it belongs on a worker. Triangles are reordered, and vertices stay where they are.
GeometryData buildGeometry(const SceneVertex* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount);

struct SceneCamera3D
{
    float viewProjection[16]; // column-major, Y down, reversed depth; see makeViewProjection()
    float position[3];
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
    uint32_t bucketBase[CLUSTER_BUCKETS]; // where each bucket's list starts
    uint32_t instanceEnd;
    uint32_t unused[3];
};
static_assert(sizeof(GpuSceneView) == 208, "shaders read this layout");

void fillSceneView(GpuSceneView& view, const SceneCamera3D& camera, uint32_t width, uint32_t height);

// Instances: which geometry and material, and where.
struct InstanceDesc
{
    uint32_t geometry = 0;
    uint32_t material = 0;
    float objectToWorld[12]; // three rows
};

struct MaterialDesc
{
    float color[4] = { 1.f, 1.f, 1.f, 1.f };
    float uvRow0[3] = { 1.f, 0.f, 0.f };
    float uvRow1[3] = { 0.f, 1.f, 0.f };
    uint32_t texture = 0;
    uint32_t flags = 0;
};

struct GeometryCommand
{
    uint32_t id = 0;
    GeometryData data;
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

    uint32_t addGeometry(GeometryData&& data);
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
