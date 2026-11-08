/**
 * asrscenestore.h
 * The render thread's side of the scene: turns SceneQueue commands into memory
 * placements and a list of writes for the GPU tables and geometry chunks. It is the
 * same on both backends, and touches no GPU API: a backend only supplies chunks, then
 * performs the writes each frame.
 *
 * Geometry goes in chunks that never move, so growing never copies it and clusters can
 * hold device addresses. The tables are small and do grow, by copy, on the GPU.
 */
#pragma once

#include "asrscene.h"

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace asr
{

enum class SceneTable : uint32_t
{
    Instances,
    Geometries,
    Clusters,
    Materials,
    Terrains,
    Count
};

constexpr uint32_t SCENE_TABLES = uint32_t(SceneTable::Count);
constexpr uint32_t SCENE_TABLE_STRIDE[SCENE_TABLES] = { sizeof(GpuInstance), sizeof(GpuGeometry), sizeof(GpuCluster), sizeof(GpuMaterial), sizeof(GpuTerrain) };
constexpr uint64_t SCENE_CHUNK_BYTES = 64ull * 1024 * 1024;

// Hands out ranges of a span that can grow. First fit, and freed neighbours merge.
class RangeAllocator
{
public:
    explicit RangeAllocator(uint64_t capacity = 0) : mEnd(capacity) {}

    void grow(uint64_t capacity);
    uint64_t capacity() const { return mEnd; }
    uint64_t freeSpace() const { return mEnd - mBump + mFreeBytes; }

    // Returns false when no range is large enough. Takes the smallest range that fits.
    // Both cost log n: a teleport can leave tens of thousands of free ranges.
    bool allocate(uint64_t size, uint64_t alignment, uint64_t& offset);
    void release(uint64_t offset, uint64_t size);

private:
    void addFree(uint64_t offset, uint64_t size);
    void removeFree(std::map<uint64_t, uint64_t>::iterator range);

    std::map<uint64_t, uint64_t> mFreeByOffset;           // to merge neighbours
    std::set<std::pair<uint64_t, uint64_t>> mFreeBySize;  // size, then offset
    uint64_t mFreeBytes = 0;
    uint64_t mEnd = 0;
    uint64_t mBump = 0;       // everything past this has never been used
};

class SceneChunks
{
public:
    virtual ~SceneChunks() = default;

    // Creates a chunk of SCENE_CHUNK_BYTES and returns its device address, or 0 when
    // the geometry budget has no room for it.
    virtual uint64_t createSceneChunk() = 0;
};

// One write. When data is null, source is an offset into SceneStore::tableBytes().
struct SceneWrite
{
    uint32_t chunk = 0;           // meaningful when table is SceneTable::Count
    SceneTable table = SceneTable::Count;
    uint64_t offset = 0;
    uint64_t bytes = 0;
    const void* data = nullptr;
    uint64_t source = 0;
};

class SceneStore
{
public:
    // Destroys nothing a frame can still be drawing: a freed range waits this many frames.
    explicit SceneStore(uint32_t framesInFlight) : mFreeDelay(framesInFlight + 1) {}

    // Applies what is queued, and fills writes(). uploadLimit bounds what one frame
    // sends, so a burst of loads spreads over several frames. Instances past half of it
    // wait for the next update, and the other half is for geometry.
    void update(SceneQueue& queue, SceneChunks& chunks, uint64_t frame, uint64_t uploadLimit);

    const std::vector<SceneWrite>& writes() const { return mWrites; }
    const std::vector<uint8_t>& tableBytes() const { return mTableBytes; }

    // What the tables must hold for writes() to fit.
    uint64_t tableCapacity(SceneTable table) const { return mTableCapacity[uint32_t(table)]; }

    // One past the highest instance in use.
    uint32_t instanceEnd() const { return mInstanceEnd; }

    // The room each list needs if every cluster of every instance is seen.
    uint32_t listCapacity(uint32_t list) const;
    uint64_t visibleCapacity() const; // of all lists together

    // The room the group list needs if every group of every instance passes.
    uint64_t groupCapacity() const;

    // Geometries waiting for upload room or for memory.
    size_t pendingGeometries() const { return mQueuedGeometries; }

    uint64_t triangles() const { return mTriangles; }   // in all instances
    uint32_t chunkCount() const { return uint32_t(mChunks.size()); }

    // Clusters' total triangle count, for tests.
    uint64_t freeChunkBytes() const;

private:
    struct Geometry
    {
        GeometryData data;     // emptied once it has been uploaded
        uint32_t instances[SCENE_KINDS]{}; // that can reach it, through finer LODs too
        uint32_t direct = 0;   // instances that name this one
        uint32_t finer = 0;    // LODs that name this one as coarser
        uint32_t coarser = 0;
        float coarserAt = 0.f;
        uint32_t firstEntry = 0;  // in the cluster table: its groups, then its clusters
        uint32_t entryCount = 0;
        uint32_t groupCount = 0;
        uint32_t chunk = 0;
        uint64_t chunkOffset = 0;
        uint64_t blobBytes = 0;
        uint32_t buckets[CLUSTER_BUCKETS]{};
        uint64_t triangles = 0;
        float center[3]{};
        float scale = 1.f;
        bool uploaded = false;
        bool removing = false; // asked to go, and waiting for its last user
        bool queued = false;   // waiting in mPending to be uploaded
    };

    struct InstanceUse
    {
        uint32_t geometry = 0; // 0 means free
        uint32_t kind = 0;
    };

    struct Chunk
    {
        uint64_t address = 0;
        RangeAllocator ranges;
    };

    struct EntryMark
    {
        uint64_t update = 0; // the update that last wrote this entry
        uint32_t write = 0;  // its place in mWrites
    };

    struct Retired
    {
        enum class Type : uint8_t
        {
            Chunk,
            Clusters,
            GeometryId,
            InstanceId,
            MaterialId,
            TerrainId
        };
        Type type;
        uint32_t index;   // chunk, or the id
        uint64_t offset;  // inside the chunk, or the first entry of the cluster table
        uint64_t size;
        uint64_t frame;   // free from this frame on
    };

    void applyMaterial(const MaterialCommand& command);
    void applyTerrain(const TerrainCommand& command);
    void applyGeometry(GeometryCommand& command);
    void applyInstance(const InstanceCommand& command, uint64_t frame);
    void removeGeometry(uint32_t id, uint64_t frame);
    void removeIfUnused(uint32_t id, uint64_t frame);
    bool upload(uint32_t id, SceneChunks& chunks);
    void runRetired(SceneQueue& queue, uint64_t frame);

    void writeTable(SceneTable table, uint32_t index, const void* data, uint64_t bytes);

    // For entries that can change often: however many changes one update sees, the
    // GPU gets one write with the newest value.
    void writeEntry(SceneTable table, uint32_t index, const void* data, uint64_t bytes);

    void ensureTable(SceneTable table, uint64_t entries);
    void countClusters(const Geometry& geometry, uint32_t kind, int64_t instances);
    void countInstance(uint32_t id, uint32_t kind, int sign);

    uint32_t mFreeDelay;
    SceneCommands mCommands;
    size_t mInstancesDone = 0; // how far into mCommands.instances this take has got
    std::vector<Geometry> mGeometries;
    std::vector<InstanceUse> mInstanceUses;
    std::deque<uint32_t> mPending;
    size_t mQueuedGeometries = 0; // mPending can also hold entries of removed geometries
    std::vector<Chunk> mChunks;
    RangeAllocator mClusterRanges;
    std::deque<Retired> mRetired;
    std::vector<SceneWrite> mWrites;
    std::vector<uint8_t> mTableBytes;
    std::vector<std::vector<uint8_t>> mWrittenBlobs; // alive until the next update
    std::vector<GpuCluster> mClusterScratch;
    std::vector<GpuClusterGroup> mGroupScratch;
    std::vector<EntryMark> mEntryMarks[SCENE_TABLES]; // by table
    uint64_t mUpdate = 0;
    uint64_t mTableCapacity[SCENE_TABLES]{};
    uint64_t mListTotals[SCENE_LISTS]{};
    uint64_t mGroupTotal = 0;
    uint64_t mTriangles = 0;
    uint32_t mInstanceEnd = 0;
    bool mStarted = false;
};

} // namespace asr
