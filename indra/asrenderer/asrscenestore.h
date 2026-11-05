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
#include <vector>

namespace asr
{

enum class SceneTable : uint32_t
{
    Instances,
    Geometries,
    Clusters,
    Materials,
    Count
};

constexpr uint32_t SCENE_TABLES = uint32_t(SceneTable::Count);
constexpr uint32_t SCENE_TABLE_STRIDE[SCENE_TABLES] = { sizeof(GpuInstance), sizeof(GpuGeometry), sizeof(GpuCluster), sizeof(GpuMaterial) };
constexpr uint64_t SCENE_CHUNK_BYTES = 64ull * 1024 * 1024;

// Hands out ranges of a span that can grow. First fit, and freed neighbours merge.
class RangeAllocator
{
public:
    explicit RangeAllocator(uint64_t capacity = 0) : mEnd(capacity) {}

    void grow(uint64_t capacity);
    uint64_t capacity() const { return mEnd; }
    uint64_t freeSpace() const;

    // Returns false when no range is large enough.
    bool allocate(uint64_t size, uint64_t alignment, uint64_t& offset);
    void release(uint64_t offset, uint64_t size);

private:
    struct Range
    {
        uint64_t offset;
        uint64_t size;
    };

    std::vector<Range> mFree; // sorted by offset
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

    // The room each cluster bucket needs if every cluster of every instance is seen.
    uint32_t bucketCapacity(uint32_t bucket) const;

    // Geometries waiting for upload room or for memory.
    size_t pendingGeometries() const { return mPending.size(); }

    uint64_t triangles() const { return mTriangles; }   // in all instances
    uint32_t chunkCount() const { return uint32_t(mChunks.size()); }

    // Clusters' total triangle count, for tests.
    uint64_t freeChunkBytes() const;

private:
    struct Geometry
    {
        GeometryData data;     // emptied once it has been uploaded
        uint32_t instances = 0;
        uint32_t firstCluster = 0;
        uint32_t clusterCount = 0;
        uint32_t chunk = 0;
        uint64_t chunkOffset = 0;
        uint64_t blobBytes = 0;
        uint32_t buckets[CLUSTER_BUCKETS]{};
        uint64_t triangles = 0;
        float center[3]{};
        float scale = 1.f;
        bool uploaded = false;
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
            InstanceId
        };
        Type type;
        uint32_t index;   // chunk, or the id
        uint64_t offset;  // inside the chunk, or the first cluster
        uint64_t size;
        uint64_t frame;   // free from this frame on
    };

    void applyMaterial(const MaterialCommand& command);
    void applyGeometry(GeometryCommand& command);
    void applyInstance(const InstanceCommand& command, uint64_t frame);
    void removeGeometry(uint32_t id, uint64_t frame);
    bool upload(uint32_t id, SceneChunks& chunks);
    void runRetired(SceneQueue& queue, uint64_t frame);

    void writeTable(SceneTable table, uint32_t index, const void* data, uint64_t bytes);

    // For entries that can change often: however many changes one update sees, the
    // GPU gets one write with the newest value.
    void writeEntry(SceneTable table, uint32_t index, const void* data, uint64_t bytes);

    void ensureTable(SceneTable table, uint64_t entries);
    void countInstance(const Geometry& geometry, int sign);

    uint32_t mFreeDelay;
    SceneCommands mCommands;
    size_t mInstancesDone = 0; // how far into mCommands.instances this take has got
    std::vector<Geometry> mGeometries;
    std::vector<uint32_t> mInstanceGeometry; // by instance; 0 means free
    std::deque<uint32_t> mPending;
    std::vector<Chunk> mChunks;
    RangeAllocator mClusterRanges;
    std::deque<Retired> mRetired;
    std::vector<SceneWrite> mWrites;
    std::vector<uint8_t> mTableBytes;
    std::vector<std::vector<uint8_t>> mWrittenBlobs; // alive until the next update
    std::vector<GpuCluster> mClusterScratch;
    std::vector<EntryMark> mEntryMarks[2]; // instances, materials
    uint64_t mUpdate = 0;
    uint64_t mTableCapacity[SCENE_TABLES]{};
    uint64_t mBucketTotals[CLUSTER_BUCKETS]{};
    uint64_t mTriangles = 0;
    uint32_t mInstanceEnd = 0;
    bool mStarted = false;
};

} // namespace asr
