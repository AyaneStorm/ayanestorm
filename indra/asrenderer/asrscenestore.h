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

// The avatar tables, from Skeletons on, are in GpuSceneView::skinTables in this order.
enum class SceneTable : uint32_t
{
    Instances,
    Geometries,
    Clusters,
    Materials,
    Terrains,
    Skeletons,
    Joints,
    SkinMatrices,
    PaletteJobs,
    Palette,          // written by the GPU, entry for entry with PaletteJobs
    Bindings,
    SkinBlocks,       // a binding id per block of SKIN_BLOCK vertices, 0 for none
    SkinnedPositions, // written by the GPU
    Morphs,
    MorphVertices,
    MorphDeltas,
    MorphWeights,      // a morph's target weights, then its masks
    MorphedPositions,  // written by the GPU, entry for entry with MorphedAttributes
    MorphedAttributes,
    MorphWork,         // rewritten each update: the morphs to rebuild this frame
    ClusterBlocks,     // a binding id per block of SKIN_BLOCK clusters, 0 for none
    Count
};

constexpr uint32_t SCENE_TABLES = uint32_t(SceneTable::Count);
constexpr uint32_t SCENE_FIRST_SKIN_TABLE = uint32_t(SceneTable::Skeletons);
constexpr uint32_t SCENE_TABLE_STRIDE[SCENE_TABLES] = { sizeof(GpuInstance), sizeof(GpuGeometry), sizeof(GpuCluster), sizeof(GpuMaterial),
                                                        sizeof(GpuTerrain), sizeof(GpuSkeleton), sizeof(GpuJoint), 48,
                                                        sizeof(GpuPaletteJob), 48, sizeof(GpuBinding), sizeof(uint32_t),
                                                        16, sizeof(GpuMorph), sizeof(GpuMorphVertex), // skinned positions: float4, not SNORM16
                                                        sizeof(GpuMorphDelta), sizeof(float), 16, sizeof(PackedAttributes),
                                                        sizeof(GpuMorphWork), sizeof(uint32_t) };
static_assert(SCENE_TABLES - SCENE_FIRST_SKIN_TABLE == sizeof(GpuSceneView::skinTables) / sizeof(uint64_t), "one address per avatar table");
constexpr uint64_t SCENE_CHUNK_BYTES = 64ull * 1024 * 1024;

// Hands out ranges of a span that can grow. First fit, and freed neighbours merge.
class RangeAllocator
{
public:
    explicit RangeAllocator(uint64_t capacity = 0) : mEnd(capacity) {}

    void grow(uint64_t capacity);
    uint64_t capacity() const { return mEnd; }
    uint64_t used() const { return mBump; } // live or still awaiting retirement; free tails collapse
    uint64_t freeSpace() const { return mEnd - mBump + mFreeBytes; }
    uint64_t live() const { return mBump - mFreeBytes; } // handed out and not released

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
    virtual uint64_t createSceneChunk(uint32_t index) = 0;
    // Called only after every range has passed the frames-in-flight retirement delay.
    virtual void releaseSceneChunk(uint32_t index) = 0;
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
    bool positionsCompacted() const { return mPositionsCompacted; }

    // One past the highest instance in use.
    uint32_t instanceEnd() const { return mInstanceEnd; }

    // Everything in a view that comes from the store: list places, table ends and what
    // the skin passes cover. A backend adds table addresses, time and texture feedback.
    void fillView(GpuSceneView& view) const;

    // Threads each skin pass needs: palette entries, bindings, and blocks for the rest.
    uint32_t skinThreads(uint32_t phase) const;

    uint32_t skinnedInstances() const { return mSkinnedBindings; }

    // The room each list needs if every cluster of every instance is seen.
    uint32_t listCapacity(uint32_t list) const;
    uint64_t visibleCapacity() const; // of all lists together

    // The room the group list needs if every group of every instance passes.
    uint64_t groupCapacity() const;

    // The room the selection list needs: every cluster of every selected instance's finest
    // LOD. 0 when nothing is selected, and the selection passes are skipped.
    uint32_t selectionCapacity() const { return mSelectedClusters; }
    bool hasSelection() const { return mSelectedClusters != 0 || mHudSelected != 0; }

    // The HUD pass's draw order, as OpenGL's: opaque instances, the first `opaque` entries,
    // then blended ones back to front along the HUD camera's axis; every cluster of each
    // one's finest LOD, as HUDs are near and few. After them, the clusters of selected HUD
    // instances again, `selected` of them, for the HUD's selection mask. Empty when there
    // are no HUD instances.
    void hudList(std::vector<GpuListEntry>& entries, uint32_t& opaque, uint32_t& selected) const;

    // Geometries waiting for upload room or for memory.
    size_t pendingGeometries() const { return mQueuedGeometries; }

    uint64_t triangles() const { return mTriangles; }   // in all instances
    uint32_t chunkCount() const;
    // Temporary, for the log: skin jobs run over a table up to its end; live is what is in use.
    uint64_t rangeEnd(SceneTable table) const { return mRanges[uint32_t(table)].used(); }
    uint64_t rangeLive(SceneTable table) const { return mRanges[uint32_t(table)].live(); }
    void memoryStats(uint64_t& chunkBytes, uint64_t& chunkFree, uint64_t& tables, uint64_t& skinnedPositions, uint64_t& clusters) const
    {
        chunkBytes = uint64_t(chunkCount()) * SCENE_CHUNK_BYTES;
        chunkFree = freeChunkBytes();
        tables = 0;
        for (uint32_t i = 0; i < SCENE_TABLES; ++i) tables += mTableCapacity[i];
        skinnedPositions = mTableCapacity[uint32_t(SceneTable::SkinnedPositions)];
        clusters = mTableCapacity[uint32_t(SceneTable::Clusters)];
    }

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
        float radius = 0.f;      // in packed units
        uint32_t vertexCount = 0;
        uint32_t clusterCount = 0;
        bool skinned = false;    // has weights
        uint32_t skinJoints = 0; // validate each LOD independently against a skin
        std::vector<GeometryData::JointSphere> jointSpheres; // kept after upload, for skinned bindings' reach
        bool uploaded = false;
        bool removing = false; // asked to go, and waiting for its last user
        bool queued = false;   // waiting in mPending to be uploaded
    };

    struct InstanceUse
    {
        uint32_t geometry = 0; // 0 means free
        uint32_t kind = 0;
        uint32_t binding = 0;
        uint32_t selectedClusters = 0; // selected: what it may add to the selection list
        float hudDepth = 0.f;          // a HUD instance: its centre along the HUD camera's axis
        bool hudSelected = false;      // a HUD instance with an outline or a face tint
    };

    struct Skeleton
    {
        uint32_t jointCount = 0; // 0 means free
        uint32_t firstJoint = 0;
        std::vector<GpuJoint> joints;
        uint32_t time = 0;
        uint32_t previousTime = 0;
        bool posed = false;
        bool interpolate = false; // drawn one gap late, between its last two poses
        bool removing = false;
        bool parked = false;     // out of view: its skinned bindings hold no storage
        uint32_t users = 0;      // bindings and palettes
        uint64_t jointUpdate = 0; // the update that last wrote its joints, and where
        uint32_t jointWrite = 0;
    };

    struct Skin
    {
        std::vector<float> matrices; // empty means free
        std::vector<uint16_t> joints;
        uint32_t firstMatrix = 0;
        bool removing = false;
        uint32_t users = 0;
    };

    struct Palette
    {
        uint32_t first = 0;
        uint32_t count = 0;
        uint32_t users = 0;
        // Faces of one mesh on one skeleton must use the same position grid, or shared
        // edges round apart. Keep their maximum reach here, without widening other skins.
        float reach = 0.f;
        std::vector<uint32_t> bindings;
    };

    struct Binding
    {
        uint32_t kind = 0;
        uint32_t skeleton = 0;
        uint32_t skin = 0;
        uint32_t source = 0;
        uint32_t target = 0;      // skinned
        uint32_t targetEntry = 0;
        uint32_t entryCount = 0;
        uint32_t groupCapacity = 0;
        uint32_t positions = 0;
        uint32_t vertexCapacity = 0;
        uint32_t firstBlock = 0;
        uint32_t blockCount = 0;
        uint32_t firstClusterBlock = 0;
        uint32_t clusterBlockCount = 0;
        uint32_t matrix = 0;      // attached
        uint32_t morph = 0;       // skinned
        uint32_t instance = 0;    // skinned
        bool parked = false;      // skinned, with no storage: see SceneStore::setResident()
        float lodSize = 0.f;
        // How far its vertices reach from their joints: centre + morph bound * stretch, and
        // that reach as written, which only grows.
        float reachCenter = 0.f;
        float reachStretch = 0.f;
        float reach = 0.f;
    };

    struct MorphSet
    {
        uint32_t vertexCount = 0; // 0 means free
        uint32_t firstVertex = 0;
        uint32_t deltaCount = 0;
        uint32_t firstDelta = 0;
        uint32_t maskCount = 0;
        std::vector<uint32_t> targetMasks; // per target: its first mask, or MORPH_NO_MASK
        std::vector<uint32_t> targetSizes; // per target: its vertices
        std::vector<float> targetReach;    // per target: its longest position delta
        float normalSoften = 0.65f;
        bool removing = false;
        uint32_t users = 0;
    };

    struct Morph
    {
        uint32_t set = 0; // 0 means free
        uint32_t values = 0;   // in MorphWeights
        uint32_t output = 0;   // in MorphedPositions and MorphedAttributes
        std::vector<float> weightsAndMasks;
        std::vector<uint32_t> bindings; // that skin from it
        float bound = 0.f;              // no vertex moves further than this, at any weights it had
        bool removing = false;
        uint32_t users = 0;
        uint64_t valuesUpdate = 0; // the update that last wrote its values, and where
        uint32_t valuesWrite = 0;
        uint64_t dirtyUpdate = 0;  // the update that listed it for rebuilding
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
            TerrainId,
            TableRange, // index is the table
            BindingId,
            SkeletonId,
            SkinId,
            MorphSetId,
            MorphId
        };
        Type type;
        uint32_t index;   // chunk, table, or the id
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

    void applySkeleton(const SkeletonCommand& command);
    void applySkin(const SkinCommand& command);
    void writeSkeleton(uint32_t id, bool joints);
    void removeSkeletonIfUnused(uint32_t id, uint64_t frame);
    void removeSkinIfUnused(uint32_t id, uint64_t frame);

    // Returns the binding, or 0 when the instance cannot be bound yet and draws nothing.
    uint32_t bindSkinned(uint32_t instance, const InstanceDesc& desc);
    uint32_t bindAttached(uint32_t instance, const InstanceDesc& desc, const GpuInstance& placed);
    void unbind(uint32_t binding, uint64_t frame);
    uint32_t usePalette(uint32_t skeleton, uint32_t skin, uint32_t binding);
    void releasePalette(uint32_t skeleton, uint32_t skin, uint32_t binding, uint64_t frame);
    uint32_t allocateRange(SceneTable table, uint64_t size);
    void retireRange(SceneTable table, uint32_t first, uint32_t count, uint64_t frame);
    void writeSkinnedBinding(uint32_t id, uint32_t instance, const InstanceDesc& desc);
    void writeSkinnedBinding(uint32_t id); // from what the binding holds
    void allocateDeformed(uint32_t id);
    void releaseDeformed(uint32_t id, uint64_t frame);
    void setResident(uint32_t skeleton, bool resident, uint64_t frame);

    void applyMorphSet(const MorphSetCommand& command);
    void applyMorph(const MorphCommand& command);
    void writeMorphValues(uint32_t id);
    void removeMorphSetIfUnused(uint32_t id, uint64_t frame);
    void removeMorphIfUnused(uint32_t id, uint64_t frame);
    void writeMorphWork();
    void widenReach(uint32_t binding); // to the binding's reach, if that is further
    void writeMorphEntry(uint32_t morph); // its GPU entry, bound included

    void writeTable(SceneTable table, uint32_t index, const void* data, uint64_t bytes);

    // For entries that can change often: however many changes one update sees, the
    // GPU gets one write with the newest value.
    void writeEntry(SceneTable table, uint32_t index, const void* data, uint64_t bytes);

    void ensureTable(SceneTable table, uint64_t entries);
    void trimTables(); // reclaim unused tails, preserving stable indices and live contents
    void compactDeformedPositions(); // fresh GPU output storage; no live source geometry moves
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
    uint32_t mSelectedClusters = 0;
    uint32_t mHudSelected = 0; // HUD instances with an outline or a tint
    std::set<uint32_t> mHudInstances; // ids drawn by the HUD pass
    bool mStarted = false;
    bool mPositionsCompacted = false;
    std::chrono::steady_clock::time_point mLastPositionCompaction{};

    std::vector<Skeleton> mSkeletons;
    std::vector<Skin> mSkins;
    std::map<uint64_t, Palette> mPalettes; // by skeleton << 32 | skin
    std::vector<Binding> mBindings;        // 0 is never used
    std::vector<uint32_t> mFreeBindings;
    uint32_t mSkinnedBindings = 0;
    std::vector<MorphSet> mMorphSets;
    std::vector<Morph> mMorphs;
    std::vector<uint32_t> mDirtyMorphs;   // this update's morphs to rebuild
    uint32_t mMorphWorkCount = 0;
    RangeAllocator mRanges[SCENE_TABLES];  // for the avatar tables that hold ranges
    SceneQueue* mQueue = nullptr;          // during update(), for the store's own geometries
};

} // namespace asr
