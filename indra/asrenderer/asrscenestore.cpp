/**
 * asrscenestore.cpp
 * See asrscenestore.h.
 */
#include "asrscenestore.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace asr
{

namespace
{

constexpr uint64_t BLOB_ALIGNMENT = 16;
constexpr uint32_t MINIMUM_LIST_CAPACITY = 1024;

uint32_t bucketOf(uint32_t triangles)
{
    uint32_t bucket = 0;
    while (bucket + 1 < CLUSTER_BUCKETS && triangles > CLUSTER_BUCKET_TRIANGLES[bucket])
    {
        ++bucket;
    }
    return bucket;
}

} // namespace

void RangeAllocator::grow(uint64_t capacity)
{
    mEnd = std::max(mEnd, capacity);
}

uint64_t RangeAllocator::freeSpace() const
{
    uint64_t total = mEnd - mBump;
    for (const Range& range : mFree)
    {
        total += range.size;
    }
    return total;
}

bool RangeAllocator::allocate(uint64_t size, uint64_t alignment, uint64_t& offset)
{
    const auto aligned = [alignment](uint64_t value) { return (value + alignment - 1) / alignment * alignment; };
    for (size_t i = 0; i < mFree.size(); ++i)
    {
        const Range range = mFree[i];
        const uint64_t start = aligned(range.offset);
        if (start + size > range.offset + range.size)
        {
            continue;
        }
        offset = start;
        const uint64_t tail = range.offset + range.size - (start + size);
        if (start != range.offset)
        {
            mFree[i].size = start - range.offset;
            if (tail != 0)
            {
                mFree.insert(mFree.begin() + i + 1, Range{ start + size, tail });
            }
        }
        else if (tail != 0)
        {
            mFree[i] = Range{ start + size, tail };
        }
        else
        {
            mFree.erase(mFree.begin() + i);
        }
        return true;
    }

    const uint64_t start = aligned(mBump);
    if (start + size > mEnd)
    {
        return false;
    }
    if (start != mBump)
    {
        release(mBump, start - mBump);
    }
    offset = start;
    mBump = start + size;
    return true;
}

void RangeAllocator::release(uint64_t offset, uint64_t size)
{
    const auto next = std::lower_bound(mFree.begin(), mFree.end(), offset,
                                       [](const Range& range, uint64_t value) { return range.offset < value; });
    size_t index = size_t(next - mFree.begin());
    mFree.insert(next, Range{ offset, size });
    if (index + 1 < mFree.size() && mFree[index].offset + mFree[index].size == mFree[index + 1].offset)
    {
        mFree[index].size += mFree[index + 1].size;
        mFree.erase(mFree.begin() + index + 1);
    }
    if (index > 0 && mFree[index - 1].offset + mFree[index - 1].size == mFree[index].offset)
    {
        mFree[index - 1].size += mFree[index].size;
        mFree.erase(mFree.begin() + index);
    }
}

void SceneStore::ensureTable(SceneTable table, uint64_t entries)
{
    uint64_t& capacity = mTableCapacity[uint32_t(table)];
    const uint64_t bytes = entries * SCENE_TABLE_STRIDE[uint32_t(table)];
    if (capacity < bytes)
    {
        capacity = std::max<uint64_t>({ bytes, capacity * 2, 64 * 1024 });
    }
}

void SceneStore::writeTable(SceneTable table, uint32_t index, const void* data, uint64_t bytes)
{
    const uint64_t stride = SCENE_TABLE_STRIDE[uint32_t(table)];
    ensureTable(table, uint64_t(index) + (bytes + stride - 1) / stride);

    SceneWrite write;
    write.table = table;
    write.offset = index * stride;
    write.bytes = bytes;
    write.source = mTableBytes.size();
    mTableBytes.resize(mTableBytes.size() + bytes);
    memcpy(mTableBytes.data() + write.source, data, bytes);
    mWrites.push_back(write);
}

void SceneStore::writeEntry(SceneTable table, uint32_t index, const void* data, uint64_t bytes)
{
    std::vector<EntryMark>& marks = mEntryMarks[table == SceneTable::Instances ? 0 : table == SceneTable::Materials ? 1 : 2];
    if (marks.size() <= index)
    {
        marks.resize(size_t(index) + 1);
    }
    EntryMark& mark = marks[index];
    if (mark.update == mUpdate)
    {
        memcpy(mTableBytes.data() + mWrites[mark.write].source, data, bytes);
        return;
    }
    mark.update = mUpdate;
    mark.write = uint32_t(mWrites.size());
    writeTable(table, index, data, bytes);
}

void SceneStore::countClusters(const Geometry& geometry, uint32_t kind, int64_t instances)
{
    for (uint32_t bucket = 0; bucket < CLUSTER_BUCKETS; ++bucket)
    {
        mListTotals[sceneList(kind, bucket)] += uint64_t(instances * int64_t(geometry.buckets[bucket]));
    }
}

// An instance can draw any LOD of its chain, so every LOD keeps room for it. That is a
// little more than the largest one alone, and the GPU can never run out.
void SceneStore::countInstance(uint32_t id, uint32_t kind, int sign)
{
    Geometry& finest = mGeometries[id];
    finest.direct = uint32_t(int64_t(finest.direct) + sign);
    if (finest.uploaded)
    {
        mTriangles += uint64_t(int64_t(sign) * int64_t(finest.triangles));
    }
    uint32_t at = id;
    for (uint32_t lod = 0; lod < MAX_LODS && at != 0; ++lod)
    {
        Geometry& geometry = mGeometries[at];
        geometry.instances[kind] = uint32_t(int64_t(geometry.instances[kind]) + sign);
        if (geometry.uploaded)
        {
            countClusters(geometry, kind, sign);
        }
        at = geometry.coarser;
    }
}

uint32_t SceneStore::listCapacity(uint32_t list) const
{
    // Doubling steps, so the list is not recreated for every instance that arrives.
    uint64_t capacity = MINIMUM_LIST_CAPACITY;
    while (capacity < mListTotals[list])
    {
        capacity *= 2;
    }
    return uint32_t(std::min<uint64_t>(capacity, MAX_VISIBLE_CLUSTERS / SCENE_LISTS));
}

uint64_t SceneStore::visibleCapacity() const
{
    uint64_t total = 0;
    for (uint32_t list = 0; list < SCENE_LISTS; ++list)
    {
        total += listCapacity(list);
    }
    return total;
}

uint64_t SceneStore::freeChunkBytes() const
{
    uint64_t total = 0;
    for (const Chunk& chunk : mChunks)
    {
        total += chunk.ranges.freeSpace();
    }
    return total;
}

void SceneStore::applyMaterial(const MaterialCommand& command)
{
    GpuMaterial material{};
    memcpy(material.color, command.desc.color, sizeof(material.color));
    memcpy(material.uvRow0, command.desc.uvRow0, sizeof(material.uvRow0));
    memcpy(material.uvRow1, command.desc.uvRow1, sizeof(material.uvRow1));
    material.texture = command.desc.texture;
    material.alphaCutoff = command.desc.alphaCutoff;
    material.planar = command.desc.planar ? 1 : 0;
    memcpy(material.planarScale, command.desc.planarScale, sizeof(material.planarScale));
    memcpy(material.planarOffset, command.desc.planarOffset, sizeof(material.planarOffset));
    writeEntry(SceneTable::Materials, command.id, &material, sizeof(material));
}

void SceneStore::applyGeometry(GeometryCommand& command)
{
    if (mGeometries.size() <= command.id)
    {
        mGeometries.resize(size_t(command.id) + 1);
    }
    Geometry& geometry = mGeometries[command.id];
    geometry = Geometry();
    memcpy(geometry.center, command.data.center, sizeof(geometry.center));
    geometry.scale = command.data.scale;
    geometry.data = std::move(command.data);
    mPending.push_back(command.id);

    // Instances hold the box they were packed in, so an LOD in another box cannot be used.
    if (command.coarser != 0 && command.coarser < mGeometries.size())
    {
        Geometry& coarser = mGeometries[command.coarser];
        if (memcmp(coarser.center, geometry.center, sizeof(geometry.center)) == 0 && coarser.scale == geometry.scale)
        {
            geometry.coarser = command.coarser;
            geometry.coarserAt = command.coarserAt;
            ++coarser.finer;

            // Instances on this one can already show the coarser one while it waits.
            GpuGeometry entry{};
            entry.coarser = geometry.coarser;
            entry.coarserAt = geometry.coarserAt;
            writeEntry(SceneTable::Geometries, command.id, &entry, sizeof(entry));
        }
    }
}

void SceneStore::applyInstance(const InstanceCommand& command, uint64_t frame)
{
    if (mInstanceUses.size() <= command.id)
    {
        mInstanceUses.resize(size_t(command.id) + 1);
    }
    InstanceUse& current = mInstanceUses[command.id];
    if (current.geometry != 0)
    {
        const uint32_t old = current.geometry;
        countInstance(old, current.kind, -1);
        current.geometry = 0;
        uint32_t at = old;
        for (uint32_t lod = 0; lod < MAX_LODS && at != 0; ++lod)
        {
            const uint32_t next = mGeometries[at].coarser;
            removeIfUnused(at, frame);
            at = next;
        }
    }

    GpuInstance instance{};
    if (command.type == InstanceCommand::Type::Remove)
    {
        writeEntry(SceneTable::Instances, command.id, &instance, sizeof(instance));
        mRetired.push_back({ Retired::Type::InstanceId, command.id, 0, 0, frame + mFreeDelay });
        return;
    }

    const InstanceDesc& desc = command.desc;
    if (desc.geometry != 0 && desc.geometry < mGeometries.size())
    {
        Geometry& geometry = mGeometries[desc.geometry];

        // The stored matrix starts from packed positions, so shaders do one transform.
        for (int row = 0; row < 3; ++row)
        {
            const float* from = &desc.objectToWorld[row * 4];
            float* to = instance.rows[row];
            to[0] = from[0] * geometry.scale;
            to[1] = from[1] * geometry.scale;
            to[2] = from[2] * geometry.scale;
            to[3] = from[0] * geometry.center[0] + from[1] * geometry.center[1] + from[2] * geometry.center[2] + from[3];
        }
        const float* m = desc.objectToWorld;
        const float determinant = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
                                  m[2] * (m[4] * m[9] - m[5] * m[8]);
        instance.flags = (determinant < 0.f ? INSTANCE_FLIPPED : 0) | (desc.flags & INSTANCE_DESC_FLAGS);
        instance.geometry = desc.geometry;
        instance.material = desc.material;
        memcpy(instance.lodCenter, desc.lodCenter, sizeof(instance.lodCenter));
        instance.lodSize = desc.lodSize;

        current.geometry = desc.geometry;
        current.kind = sceneKind(instance.flags);
        countInstance(current.geometry, current.kind, 1);
    }
    writeEntry(SceneTable::Instances, command.id, &instance, sizeof(instance));
    mInstanceEnd = std::max(mInstanceEnd, command.id + 1);
}

void SceneStore::removeGeometry(uint32_t id, uint64_t frame)
{
    if (id != 0 && id < mGeometries.size())
    {
        mGeometries[id].removing = true;
        removeIfUnused(id, frame);
    }
}

void SceneStore::removeIfUnused(uint32_t id, uint64_t frame)
{
    Geometry& geometry = mGeometries[id];
    const bool used = geometry.finer != 0 ||
                      std::any_of(std::begin(geometry.instances), std::end(geometry.instances), [](uint32_t count) { return count != 0; });
    if (!geometry.removing || used)
    {
        return;
    }
    geometry.removing = false;

    const auto pending = std::find(mPending.begin(), mPending.end(), id);
    if (pending != mPending.end())
    {
        mPending.erase(pending);
    }
    const GpuGeometry empty{};
    writeEntry(SceneTable::Geometries, id, &empty, sizeof(empty));
    if (geometry.uploaded)
    {
        mRetired.push_back({ Retired::Type::Clusters, 0, geometry.firstCluster, geometry.clusterCount, frame + mFreeDelay });
        mRetired.push_back({ Retired::Type::Chunk, geometry.chunk, geometry.chunkOffset, geometry.blobBytes, frame + mFreeDelay });
    }
    geometry.uploaded = false;
    geometry.data = GeometryData();
    mRetired.push_back({ Retired::Type::GeometryId, id, 0, 0, frame + mFreeDelay });

    if (geometry.coarser != 0)
    {
        const uint32_t coarser = geometry.coarser;
        geometry.coarser = 0;
        --mGeometries[coarser].finer;
        removeIfUnused(coarser, frame);
    }
}

bool SceneStore::upload(uint32_t id, SceneChunks& chunks)
{
    Geometry& geometry = mGeometries[id];
    GeometryData& data = geometry.data;
    const uint64_t blobBytes = data.blob.size();
    if (blobBytes > SCENE_CHUNK_BYTES)
    {
        // No face can be this large: a face is at most 65,535 vertices. Dropped, not retried.
        geometry.data = GeometryData();
        return true;
    }

    uint32_t chunkIndex = 0;
    uint64_t chunkOffset = 0;
    bool placed = false;
    for (; chunkIndex < mChunks.size() && !placed; ++chunkIndex)
    {
        placed = mChunks[chunkIndex].ranges.allocate(blobBytes, BLOB_ALIGNMENT, chunkOffset);
    }
    if (placed)
    {
        --chunkIndex;
    }
    else
    {
        const uint64_t address = chunks.createSceneChunk();
        if (address == 0)
        {
            return false; // no memory for it: it waits, and the budget's pressure rules take over
        }
        Chunk chunk;
        chunk.address = address;
        chunk.ranges.grow(SCENE_CHUNK_BYTES);
        mChunks.push_back(std::move(chunk));
        chunkIndex = uint32_t(mChunks.size() - 1);
        mChunks.back().ranges.allocate(blobBytes, BLOB_ALIGNMENT, chunkOffset);
    }

    uint64_t firstCluster = 0;
    const uint64_t clusterCount = data.clusters.size();
    while (!mClusterRanges.allocate(clusterCount, 1, firstCluster))
    {
        mClusterRanges.grow(std::max<uint64_t>(mClusterRanges.capacity() * 2, 4096));
    }

    const uint64_t base = mChunks[chunkIndex].address + chunkOffset;
    mClusterScratch.resize(clusterCount);
    for (size_t i = 0; i < clusterCount; ++i)
    {
        const ClusterData& from = data.clusters[i];
        GpuCluster& to = mClusterScratch[i];
        memcpy(to.center, from.center, sizeof(to.center));
        to.radius = from.radius;
        to.cone = from.cone;
        to.triangles = from.triangles;
        to.indices = base + from.indexOffset;
        to.positions = base + data.positionsOffset;
        to.attributes = base + data.attributesOffset;

        const uint32_t bucket = bucketOf(from.triangles);
        ++geometry.buckets[bucket];
        geometry.triangles += from.triangles;
    }
    writeTable(SceneTable::Clusters, uint32_t(firstCluster), mClusterScratch.data(), clusterCount * sizeof(GpuCluster));

    GpuGeometry entry{};
    entry.radius = data.radius;
    entry.firstCluster = uint32_t(firstCluster);
    entry.clusterCount = uint32_t(clusterCount);
    entry.coarser = geometry.coarser;
    entry.coarserAt = geometry.coarserAt;
    writeEntry(SceneTable::Geometries, id, &entry, sizeof(entry));

    SceneWrite write;
    write.chunk = chunkIndex;
    write.offset = chunkOffset;
    write.bytes = blobBytes;
    write.data = data.blob.data();
    mWrites.push_back(write);
    mWrittenBlobs.push_back(std::move(data.blob)); // moving a vector keeps its storage

    geometry.chunk = chunkIndex;
    geometry.chunkOffset = chunkOffset;
    geometry.blobBytes = blobBytes;
    geometry.firstCluster = uint32_t(firstCluster);
    geometry.clusterCount = uint32_t(clusterCount);
    geometry.uploaded = true;
    geometry.data = GeometryData();
    for (uint32_t kind = 0; kind < SCENE_KINDS; ++kind)
    {
        countClusters(geometry, kind, geometry.instances[kind]);
    }
    mTriangles += geometry.direct * geometry.triangles;
    return true;
}

void SceneStore::runRetired(SceneQueue& queue, uint64_t frame)
{
    while (!mRetired.empty() && mRetired.front().frame <= frame)
    {
        const Retired retired = mRetired.front();
        mRetired.pop_front();
        switch (retired.type)
        {
            case Retired::Type::Chunk:
                mChunks[retired.index].ranges.release(retired.offset, retired.size);
                break;
            case Retired::Type::Clusters:
                mClusterRanges.release(retired.offset, retired.size);
                break;
            case Retired::Type::GeometryId:
                queue.releaseGeometry(retired.index);
                break;
            case Retired::Type::InstanceId:
                queue.releaseInstance(retired.index);
                break;
            case Retired::Type::MaterialId:
                queue.releaseMaterial(retired.index);
                break;
        }
    }
}

void SceneStore::update(SceneQueue& queue, SceneChunks& chunks, uint64_t frame, uint64_t uploadLimit)
{
    mWrites.clear();
    mTableBytes.clear();
    mWrittenBlobs.clear();
    ++mUpdate;

    if (!mStarted)
    {
        // Entries 0 are written once. Tables start zeroed, and the first material is white.
        mStarted = true;
        applyMaterial(MaterialCommand());
        ensureTable(SceneTable::Geometries, 1);
        ensureTable(SceneTable::Instances, 1);
        ensureTable(SceneTable::Clusters, 1);
    }

    runRetired(queue, frame);

    // A take finishes before the next one starts, which keeps commands in order.
    if (mInstancesDone == mCommands.instances.size())
    {
        mCommands.instances.clear();
        mInstancesDone = 0;
        queue.take(mCommands);
        for (const MaterialCommand& command : mCommands.materials)
        {
            applyMaterial(command);
        }
        for (GeometryCommand& command : mCommands.geometries)
        {
            applyGeometry(command);
        }
        mCommands.materials.clear();
        mCommands.geometries.clear();
    }

    while (mInstancesDone < mCommands.instances.size() && mTableBytes.size() < uploadLimit / 2)
    {
        applyInstance(mCommands.instances[mInstancesDone++], frame);
    }
    if (mInstancesDone == mCommands.instances.size())
    {
        for (uint32_t id : mCommands.removedGeometries)
        {
            removeGeometry(id, frame);
        }
        mCommands.removedGeometries.clear();
        for (uint32_t id : mCommands.removedMaterials)
        {
            mRetired.push_back({ Retired::Type::MaterialId, id, 0, 0, frame + mFreeDelay });
        }
        mCommands.removedMaterials.clear();
    }

    uint64_t uploaded = 0;
    while (!mPending.empty())
    {
        const uint32_t id = mPending.front();
        const GeometryData& data = mGeometries[id].data;
        const uint64_t bytes = data.blob.size() + data.clusters.size() * sizeof(GpuCluster);
        if ((uploaded != 0 && mTableBytes.size() + uploaded + bytes > uploadLimit) || !upload(id, chunks))
        {
            break;
        }
        uploaded += bytes;
        mPending.pop_front();
    }
    ensureTable(SceneTable::Clusters, mClusterRanges.capacity());
}

} // namespace asr
