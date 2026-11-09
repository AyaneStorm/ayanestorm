/**
 * asrscenestorememory.cpp
 * Reclaims interior holes in the deformation output cache. Parking alone releases
 * logical ranges; a few live allocations near the old end otherwise pin hundreds
 * of MB. Only transient skinned positions move: current-frame consumers regenerate
 * them before drawing, and the backend retires the previous physical buffer.
 * Author: chanayane@firestorm
 */
#include "asrscenestore.h"

#include <algorithm>
#include <utility>

namespace asr
{

void SceneStore::compactDeformedPositions()
{
    constexpr uint32_t TABLE = uint32_t(SceneTable::SkinnedPositions);
    constexpr uint64_t PAGE = 64 * 1024;
    const uint64_t stride = SCENE_TABLE_STRIDE[TABLE];
    const uint64_t capacity = mTableCapacity[TABLE];
    if (capacity < 16 * 1024 * 1024 || mRanges[TABLE].live() * stride > capacity / 2) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - mLastPositionCompaction < std::chrono::seconds(1)) return;

    uint64_t vertices = 0;
    uint64_t vertexBlocks = 0;
    uint64_t clusterBlocks = 0;
    for (const Binding& binding : mBindings)
    {
        if (binding.kind != BINDING_SKINNED || binding.parked) continue;
        vertices += binding.vertexCapacity;
        vertexBlocks += binding.blockCount;
        clusterBlocks += binding.clusterBlockCount;
    }
    const uint64_t bytes = std::max<uint64_t>(vertices * stride, PAGE);
    if (bytes > capacity / 2) return;

    // The old ranges belong to the old GPU buffer. Its retirement is handled by the
    // backend; freeing those offsets into the new allocator would corrupt live owners.
    mRetired.erase(std::remove_if(mRetired.begin(), mRetired.end(), [TABLE](const Retired& item)
    {
        return item.type == Retired::Type::TableRange && (item.index == TABLE ||
            item.index == uint32_t(SceneTable::SkinBlocks) || item.index == uint32_t(SceneTable::ClusterBlocks));
    }), mRetired.end());

    RangeAllocator compact(vertices);
    RangeAllocator compactVertexBlocks(vertexBlocks);
    RangeAllocator compactClusterBlocks(clusterBlocks);
    std::vector<uint32_t> vertexJobs, clusterJobs;
    vertexJobs.reserve(size_t(vertexBlocks));
    clusterJobs.reserve(size_t(clusterBlocks));
    for (uint32_t id = 1; id < mBindings.size(); ++id)
    {
        Binding& binding = mBindings[id];
        if (binding.kind != BINDING_SKINNED || binding.parked) continue;
        uint64_t offset = 0;
        compact.allocate(binding.vertexCapacity, 1, offset);
        binding.positions = uint32_t(offset);
        compactVertexBlocks.allocate(binding.blockCount, 1, offset);
        binding.firstBlock = uint32_t(offset);
        compactClusterBlocks.allocate(binding.clusterBlockCount, 1, offset);
        binding.firstClusterBlock = uint32_t(offset);
        vertexJobs.insert(vertexJobs.end(), binding.blockCount, id);
        clusterJobs.insert(clusterJobs.end(), binding.clusterBlockCount, id);
        writeSkinnedBinding(id);
    }
    mRanges[TABLE] = std::move(compact);
    mTableCapacity[TABLE] = (bytes + PAGE - 1) / PAGE * PAGE;
    const auto rewriteBlocks = [this, PAGE](SceneTable table, RangeAllocator&& ranges, const std::vector<uint32_t>& jobs)
    {
        const uint64_t size = std::max<uint64_t>(jobs.size() * sizeof(uint32_t), PAGE);
        mTableCapacity[uint32_t(table)] = (size + PAGE - 1) / PAGE * PAGE;
        mRanges[uint32_t(table)] = std::move(ranges);
        if (!jobs.empty()) writeTable(table, 0, jobs.data(), jobs.size() * sizeof(uint32_t));
    };
    // Compact dispatch maps too: freed interior blocks otherwise still launch empty
    // workgroups every frame, including both phases of demand-driven deformation.
    rewriteBlocks(SceneTable::SkinBlocks, std::move(compactVertexBlocks), vertexJobs);
    rewriteBlocks(SceneTable::ClusterBlocks, std::move(compactClusterBlocks), clusterJobs);
    mPositionsCompacted = true; // force a fresh physical buffer even if later writes regrow it
    mLastPositionCompaction = now;
}

} // namespace asr
