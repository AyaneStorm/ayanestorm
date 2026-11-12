/**
 * asrscenestore.cpp
 * See asrscenestore.h.
 */
#include "asrscenestore.h"
#include "asrjointpose.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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

void RangeAllocator::addFree(uint64_t offset, uint64_t size)
{
    mFreeByOffset.emplace(offset, size);
    mFreeBySize.emplace(size, offset);
    mFreeBytes += size;
}

void RangeAllocator::removeFree(std::map<uint64_t, uint64_t>::iterator range)
{
    mFreeBytes -= range->second;
    mFreeBySize.erase({ range->second, range->first });
    mFreeByOffset.erase(range);
}

bool RangeAllocator::allocate(uint64_t size, uint64_t alignment, uint64_t& offset)
{
    const auto aligned = [alignment](uint64_t value) { return (value + alignment - 1) / alignment * alignment; };

    // The smallest range of this size usually fits. If aligning its start rules it out, a
    // range this much larger always fits, so no search through ranges is ever needed.
    auto found = mFreeBySize.lower_bound({ size, 0 });
    if (found != mFreeBySize.end() && aligned(found->second) + size > found->second + found->first)
    {
        found = mFreeBySize.lower_bound({ size + alignment - 1, 0 });
    }
    if (found != mFreeBySize.end())
    {
        const uint64_t rangeSize = found->first;
        const uint64_t rangeOffset = found->second;
        const uint64_t start = aligned(rangeOffset);
        removeFree(mFreeByOffset.find(rangeOffset));
        if (start != rangeOffset)
        {
            addFree(rangeOffset, start - rangeOffset);
        }
        const uint64_t tail = rangeOffset + rangeSize - (start + size);
        if (tail != 0)
        {
            addFree(start + size, tail);
        }
        offset = start;
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
    const auto next = mFreeByOffset.lower_bound(offset);
    if (next != mFreeByOffset.end() && offset + size == next->first)
    {
        size += next->second;
        removeFree(next);
    }
    auto previous = mFreeByOffset.lower_bound(offset);
    if (previous != mFreeByOffset.begin())
    {
        --previous;
        if (previous->first + previous->second == offset)
        {
            offset = previous->first;
            size += previous->second;
            removeFree(previous);
        }
    }
    // Return the free tail to the bump span too. Otherwise parking/removal leaves the
    // GPU dispatching over historical allocations, and tables can never shrink.
    if (offset + size == mBump)
    {
        mBump = offset;
    }
    else
    {
        addFree(offset, size);
    }
}

void SceneStore::ensureTable(SceneTable table, uint64_t entries)
{
    uint64_t& capacity = mTableCapacity[uint32_t(table)];
    const uint64_t bytes = entries * SCENE_TABLE_STRIDE[uint32_t(table)];
    if (capacity < bytes)
    {
        // A quarter more, not double: a doubled table held twice its live bytes, and the
        // old buffer stays while frames in flight read it. Growth still costs log n copies.
        const uint64_t grown = std::max<uint64_t>({ bytes, capacity + capacity / 4, 64 * 1024 });
        capacity = (grown + 15) & ~uint64_t(15); // Vulkan fill/copy sizes stay word aligned
    }
}

void SceneStore::trimTables()
{
    // Only range-owned tables: ID tables have independent lifetimes. Retirement has
    // finished before this runs, and no writes for this update have been recorded yet.
    for (uint32_t i = 0; i < SCENE_TABLES; ++i)
    {
        SceneTable owner = SceneTable(i);
        if (owner == SceneTable::PaletteJobs) owner = SceneTable::Palette;
        if (owner == SceneTable::MorphedAttributes) owner = SceneTable::MorphedPositions;
        const RangeAllocator& ranges = owner == SceneTable::Clusters ? mClusterRanges : mRanges[uint32_t(owner)];
        if (ranges.capacity() == 0) continue;
        constexpr uint64_t PAGE = 64 * 1024;
        const uint64_t needed = std::max<uint64_t>(ranges.used() * SCENE_TABLE_STRIDE[i], PAGE);
        if (needed <= mTableCapacity[i] / 2)
        {
            mTableCapacity[i] = (needed + PAGE - 1) / PAGE * PAGE;
        }
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
    std::vector<EntryMark>& marks = mEntryMarks[uint32_t(table)];
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
    mGroupTotal += uint64_t(instances * int64_t(geometry.groupCount));
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

void SceneStore::hudList(std::vector<GpuListEntry>& entries, uint32_t& opaque, uint32_t& selected) const
{
    entries.clear();
    opaque = 0;
    selected = 0;
    if (mHudInstances.empty())
    {
        return;
    }
    // Opaque first in id order, so the order never flickers; then blended, farthest first.
    std::vector<std::pair<float, uint32_t>> order;
    order.reserve(mHudInstances.size());
    for (uint32_t id : mHudInstances)
    {
        const InstanceUse& use = mInstanceUses[id];
        const bool blended = use.kind >= SCENE_BLENDED_KIND;
        order.push_back({ blended ? -use.hudDepth : -std::numeric_limits<float>::infinity(), id });
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto add = [&](uint32_t id)
    {
        const Geometry& geometry = mGeometries[mInstanceUses[id].geometry];
        if (!geometry.uploaded)
        {
            return uint32_t(0);
        }
        const uint32_t firstCluster = geometry.firstEntry + geometry.groupCount;
        for (uint32_t i = 0; i < geometry.clusterCount; ++i)
        {
            entries.push_back({ id, firstCluster + i });
        }
        return geometry.clusterCount;
    };
    for (const auto& item : order)
    {
        const uint32_t added = add(item.second);
        if (mInstanceUses[item.second].kind < SCENE_BLENDED_KIND)
        {
            opaque += added; // opaque items sort first, so these stay a prefix
        }
    }
    for (uint32_t id : mHudInstances)
    {
        if (mInstanceUses[id].hudSelected)
        {
            selected += add(id);
        }
    }
}

uint64_t SceneStore::groupCapacity() const
{
    // An instance's group goes in the list once, early or late, never both, as one that
    // passes early is not retested. Doubling steps, as for the lists.
    uint64_t capacity = MINIMUM_LIST_CAPACITY;
    while (capacity < mGroupTotal)
    {
        capacity *= 2;
    }
    return std::min<uint64_t>(capacity, MAX_VISIBLE_CLUSTERS);
}

uint64_t SceneStore::freeChunkBytes() const
{
    uint64_t total = 0;
    for (const Chunk& chunk : mChunks)
    {
        if (chunk.address != 0) total += chunk.ranges.freeSpace();
    }
    return total;
}

uint32_t SceneStore::chunkCount() const
{
    return uint32_t(std::count_if(mChunks.begin(), mChunks.end(), [](const Chunk& chunk) { return chunk.address != 0; }));
}

void SceneStore::applyMaterial(const MaterialCommand& command)
{
    GpuMaterial material{};
    memcpy(material.color, command.desc.color, sizeof(material.color));
    memcpy(material.uvRow0, command.desc.uvRow0, sizeof(material.uvRow0));
    memcpy(material.uvRow1, command.desc.uvRow1, sizeof(material.uvRow1));
    material.texture = command.desc.terrain != 0 ? command.desc.terrain : command.desc.texture;
    material.alphaCutoff = command.desc.alphaCutoff;
    material.mapping = command.desc.terrain != 0 ? MATERIAL_TERRAIN : command.desc.planar ? MATERIAL_PLANAR : MATERIAL_FACE_UV;
    memcpy(material.planarScale, command.desc.planarScale, sizeof(material.planarScale));
    memcpy(material.planarOffset, command.desc.planarOffset, sizeof(material.planarOffset));
    const bool animated = (command.desc.anim.mode & TEXTURE_ANIM_ON) != 0 && command.desc.terrain == 0;
    setAnimated(command.id, animated ? &material : nullptr, command.desc.anim);
    writeEntry(SceneTable::Materials, command.id, &material, sizeof(material));
}

void SceneStore::setAnimated(uint32_t id, const GpuMaterial* material, const TextureAnimDesc& anim)
{
    const auto found = mAnimatedIndex.find(id);
    if (material)
    {
        if (found != mAnimatedIndex.end())
        {
            mAnimatedMaterials[found->second] = { id, *material, anim };
        }
        else
        {
            mAnimatedIndex.emplace(id, uint32_t(mAnimatedMaterials.size()));
            mAnimatedMaterials.push_back({ id, *material, anim });
        }
    }
    else if (found != mAnimatedIndex.end())
    {
        const uint32_t place = found->second;
        mAnimatedIndex.erase(found);
        if (place + 1 != mAnimatedMaterials.size())
        {
            mAnimatedMaterials[place] = mAnimatedMaterials.back();
            mAnimatedIndex[mAnimatedMaterials[place].id] = place;
        }
        mAnimatedMaterials.pop_back();
    }
}

// Each animated material once a frame, written only when its frame changed.
void SceneStore::animateMaterials(std::chrono::steady_clock::time_point time)
{
    for (AnimatedMaterial& animated : mAnimatedMaterials)
    {
        float row0[3];
        float row1[3];
        textureAnimRows(animated.anim, time, row0, row1);
        if (memcmp(row0, animated.material.uvRow0, sizeof(row0)) != 0 || memcmp(row1, animated.material.uvRow1, sizeof(row1)) != 0)
        {
            memcpy(animated.material.uvRow0, row0, sizeof(row0));
            memcpy(animated.material.uvRow1, row1, sizeof(row1));
            writeEntry(SceneTable::Materials, animated.id, &animated.material, sizeof(animated.material));
        }
    }
}

void SceneStore::applyTerrain(const TerrainCommand& command)
{
    GpuTerrain terrain{};
    for (uint32_t i = 0; i < TERRAIN_LAYERS; ++i)
    {
        const TerrainLayerDesc& from = command.desc.layers[i];
        GpuTerrainLayer& to = terrain.layers[i];
        memcpy(to.uvRow0, from.uvRow0, sizeof(to.uvRow0));
        memcpy(to.uvRow1, from.uvRow1, sizeof(to.uvRow1));
        memcpy(to.color, from.color, sizeof(to.color));
        to.texture = from.texture;
    }
    terrain.ramp = command.desc.ramp;
    terrain.kind = command.desc.kind;
    terrain.planes = command.desc.planes == 3 ? 3 : 1;
    terrain.sharpness = command.desc.sharpness;
    writeEntry(SceneTable::Terrains, command.id, &terrain, sizeof(terrain));
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
    geometry.radius = command.data.radius;
    geometry.vertexCount = command.data.vertexCount;
    geometry.clusterCount = uint32_t(command.data.clusters.size());
    geometry.groupCount = uint32_t(command.data.groups.size());
    geometry.skinned = command.data.skinned;
    geometry.jointSpheres = std::move(command.data.jointSpheres);
    geometry.skinJoints = 0;
    for (const GeometryData::JointSphere& sphere : geometry.jointSpheres)
    {
        geometry.skinJoints = std::max(geometry.skinJoints, uint32_t(sphere.joint) + 1);
    }
    geometry.data = std::move(command.data);
    geometry.queued = true;
    ++mQueuedGeometries;
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
    const InstanceDesc& desc = command.desc;

    // A skinned instance set again on the same skeleton, skin and geometry keeps its own
    // geometry and ranges: a new material, say, costs one binding write.
    uint32_t kept = 0;
    if (current.binding != 0)
    {
        const Binding& binding = mBindings[current.binding];
        if (command.type == InstanceCommand::Type::Set && binding.kind == BINDING_SKINNED && desc.skin == binding.skin &&
            desc.skeleton == binding.skeleton && desc.geometry == binding.source && desc.morph == binding.morph)
        {
            kept = current.binding;
        }
        else
        {
            unbind(current.binding, frame);
        }
        current.binding = 0;
    }

    mSelectedClusters -= current.selectedClusters;
    current.selectedClusters = 0;
    mHudSelected -= current.hudSelected ? 1 : 0;
    current.hudSelected = false;
    mHudInstances.erase(command.id);
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

        // On a skeleton, the skin passes write the matrix and LOD centre every frame.
        uint32_t binding = 0;
        bool drawn = true;
        if (desc.skeleton != 0 && desc.skin != 0)
        {
            binding = kept != 0 ? kept : bindSkinned(command.id, desc);
            if (binding != 0)
            {
                if (kept != 0)
                {
                    writeSkinnedBinding(kept, command.id, desc);
                }
                instance.geometry = mBindings[binding].target;
                instance.flags = INSTANCE_SKINNED | INSTANCE_BOUND | (desc.flags & INSTANCE_DESC_FLAGS);
            }
            drawn = binding != 0;
        }
        else if (desc.skeleton != 0)
        {
            binding = bindAttached(command.id, desc, instance);
            instance.flags |= INSTANCE_BOUND;
            drawn = binding != 0;
        }
        instance.binding = binding;
        current.binding = binding;

        if (drawn)
        {
            current.geometry = desc.geometry;
            current.kind = sceneKind(instance.flags);
            countInstance(current.geometry, current.kind, 1);
            if ((instance.flags & INSTANCE_SELECTION) != 0 && (instance.flags & INSTANCE_HUD) == 0)
            {
                current.selectedClusters = mGeometries[desc.geometry].clusterCount;
                mSelectedClusters += current.selectedClusters;
            }
            if ((instance.flags & INSTANCE_HUD) != 0)
            {
                current.hudDepth = desc.hudDepth; // the HUD camera looks along +X
                current.hudSelected = (instance.flags & INSTANCE_SELECTION) != 0;
                mHudSelected += current.hudSelected ? 1 : 0;
                mHudInstances.insert(command.id);
            }
        }
        else
        {
            instance = GpuInstance{}; // its skeleton, skin or weights are missing: nothing to draw
        }
    }
    writeEntry(SceneTable::Instances, command.id, &instance, sizeof(instance));
    mInstanceEnd = std::max(mInstanceEnd, command.id + 1);
}

uint32_t SceneStore::allocateRange(SceneTable table, uint64_t size)
{
    RangeAllocator& ranges = mRanges[uint32_t(table)];
    uint64_t offset = 0;
    while (!ranges.allocate(size, 1, offset))
    {
        ranges.grow(ranges.capacity() + std::max<uint64_t>({ ranges.capacity() / 4, 1024, size })); // see ensureTable()
    }
    ensureTable(table, ranges.used());
    if (table == SceneTable::Palette)
    {
        ensureTable(SceneTable::PaletteJobs, ranges.used()); // entry for entry
    }
    if (table == SceneTable::MorphedPositions)
    {
        ensureTable(SceneTable::MorphedAttributes, ranges.used());
    }
    return uint32_t(offset);
}

// The set's deltas go in vertex order, so the GPU gathers each vertex's sum in one thread,
// with no atomics: a vertex names its run of deltas.
void SceneStore::applyMorphSet(const MorphSetCommand& command)
{
    if (mMorphSets.size() <= command.id)
    {
        mMorphSets.resize(size_t(command.id) + 1);
    }
    MorphSet& set = mMorphSets[command.id];
    set = MorphSet();
    const MorphSetDesc& desc = command.desc;
    const uint32_t vertexCount = uint32_t(std::min({ desc.positions.size() / 3, desc.normals.size() / 3, desc.binormals.size() / 3, desc.uvs.size() / 2 }));
    if (vertexCount == 0)
    {
        return;
    }

    struct Entry
    {
        uint32_t vertex;
        uint32_t target;
        uint32_t index; // inside the target
    };
    std::vector<Entry> entries;
    set.targetMasks.assign(desc.targets.size(), MORPH_NO_MASK);
    set.targetSizes.assign(desc.targets.size(), 0);
    for (uint32_t target = 0; target < desc.targets.size(); ++target)
    {
        const MorphTargetDesc& morph = desc.targets[target];
        const uint32_t count = uint32_t(std::min({ morph.vertices.size(), morph.positions.size() / 3, morph.normals.size() / 3,
                                                   morph.binormals.size() / 3, morph.uvs.size() / 2 }));
        set.targetSizes[target] = count;
        float longest = 0.f;
        for (uint32_t i = 0; i < count; ++i)
        {
            const float* d = &morph.positions[i * 3];
            longest = std::max(longest, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        }
        set.targetReach.push_back(longest);
        if (morph.masked)
        {
            set.targetMasks[target] = set.maskCount;
            set.maskCount += count;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            if (morph.vertices[i] < vertexCount)
            {
                entries.push_back({ morph.vertices[i], target, i });
            }
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.vertex < b.vertex; });

    std::vector<GpuMorphDelta> deltas(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const MorphTargetDesc& morph = desc.targets[entries[i].target];
        const uint32_t at = entries[i].index;
        GpuMorphDelta& delta = deltas[i];
        memcpy(delta.position, &morph.positions[at * 3], sizeof(delta.position));
        memcpy(delta.normal, &morph.normals[at * 3], sizeof(delta.normal));
        memcpy(delta.binormal, &morph.binormals[at * 3], sizeof(delta.binormal));
        memcpy(delta.uv, &morph.uvs[at * 2], sizeof(delta.uv));
        delta.target = entries[i].target;
        delta.mask = morph.masked ? set.targetMasks[entries[i].target] + at : MORPH_NO_MASK;
    }

    set.vertexCount = vertexCount;
    set.deltaCount = uint32_t(deltas.size());
    set.normalSoften = desc.normalSoften;
    set.firstVertex = allocateRange(SceneTable::MorphVertices, vertexCount);
    set.firstDelta = deltas.empty() ? 0 : allocateRange(SceneTable::MorphDeltas, deltas.size());

    std::vector<GpuMorphVertex> vertices(vertexCount);
    size_t next = 0;
    for (uint32_t v = 0; v < vertexCount; ++v)
    {
        GpuMorphVertex& vertex = vertices[v];
        memcpy(vertex.position, &desc.positions[v * 3], sizeof(vertex.position));
        memcpy(vertex.normal, &desc.normals[v * 3], sizeof(vertex.normal));
        memcpy(vertex.binormal, &desc.binormals[v * 3], sizeof(vertex.binormal));
        memcpy(vertex.uv, &desc.uvs[v * 2], sizeof(vertex.uv));
        vertex.firstDelta = set.firstDelta + uint32_t(next);
        while (next < entries.size() && entries[next].vertex == v)
        {
            ++next;
        }
        vertex.deltaCount = set.firstDelta + uint32_t(next) - vertex.firstDelta;
    }
    writeTable(SceneTable::MorphVertices, set.firstVertex, vertices.data(), vertices.size() * sizeof(GpuMorphVertex));
    if (!deltas.empty())
    {
        writeTable(SceneTable::MorphDeltas, set.firstDelta, deltas.data(), deltas.size() * sizeof(GpuMorphDelta));
    }
}

void SceneStore::applyMorph(const MorphCommand& command)
{
    if (mMorphs.size() <= command.id)
    {
        mMorphs.resize(size_t(command.id) + 1);
    }
    Morph& morph = mMorphs[command.id];
    if (command.set != 0)
    {
        morph = Morph();
        if (command.set >= mMorphSets.size() || mMorphSets[command.set].vertexCount == 0 || mMorphSets[command.set].removing)
        {
            return; // never usable
        }
        MorphSet& set = mMorphSets[command.set];
        ++set.users;
        morph.set = command.set;
        morph.weightsAndMasks.assign(set.targetSizes.size() + set.maskCount, 0.f);
        morph.values = allocateRange(SceneTable::MorphWeights, std::max<size_t>(morph.weightsAndMasks.size(), 1));
        morph.output = allocateRange(SceneTable::MorphedPositions, set.vertexCount);
        writeMorphEntry(command.id);
        writeMorphValues(command.id);
        return;
    }
    if (morph.set == 0)
    {
        return;
    }
    const MorphSet& set = mMorphSets[morph.set];
    if (command.target == 0xFFFFFFFFu)
    {
        const size_t count = std::min(command.values.size(), set.targetSizes.size());
        std::copy(command.values.begin(), command.values.begin() + count, morph.weightsAndMasks.begin());
    }
    else if (command.target < set.targetSizes.size() && set.targetMasks[command.target] != MORPH_NO_MASK)
    {
        const size_t count = std::min<size_t>(command.values.size(), set.targetSizes[command.target]);
        std::copy(command.values.begin(), command.values.begin() + count,
                  morph.weightsAndMasks.begin() + set.targetSizes.size() + set.targetMasks[command.target]);
    }
    writeMorphValues(command.id);
}

// Its values in one write per update, however many changes came, and a rebuild this frame.
void SceneStore::writeMorphValues(uint32_t id)
{
    Morph& morph = mMorphs[id];
    if (morph.weightsAndMasks.empty())
    {
        morph.weightsAndMasks.push_back(0.f); // a set with no targets still has its base to show
    }
    const uint64_t bytes = morph.weightsAndMasks.size() * sizeof(float);
    if (morph.valuesUpdate == mUpdate)
    {
        memcpy(mTableBytes.data() + mWrites[morph.valuesWrite].source, morph.weightsAndMasks.data(), bytes);
    }
    else
    {
        morph.valuesUpdate = mUpdate;
        morph.valuesWrite = uint32_t(mWrites.size());
        writeTable(SceneTable::MorphWeights, morph.values, morph.weightsAndMasks.data(), bytes);
    }
    if (morph.dirtyUpdate != mUpdate)
    {
        morph.dirtyUpdate = mUpdate;
        mDirtyMorphs.push_back(id);
    }

    // No vertex moves further than the weighted sum of each target's longest delta: masks
    // only scale down. The bindings' reach widens to it, and never narrows.
    const MorphSet& set = mMorphSets[morph.set];
    float bound = 0.f;
    for (size_t target = 0; target < set.targetReach.size(); ++target)
    {
        bound += std::fabs(morph.weightsAndMasks[target]) * set.targetReach[target];
    }
    if (bound > morph.bound)
    {
        morph.bound = bound;
        writeMorphEntry(id);
        for (uint32_t binding : morph.bindings)
        {
            widenReach(binding);
        }
    }
}

void SceneStore::writeMorphEntry(uint32_t id)
{
    const Morph& morph = mMorphs[id];
    const MorphSet& set = mMorphSets[morph.set];
    GpuMorph entry{};
    entry.firstVertex = set.firstVertex;
    entry.vertexCount = set.vertexCount;
    entry.weights = morph.values;
    entry.masks = morph.values + uint32_t(set.targetSizes.size());
    entry.output = morph.output;
    entry.normalSoften = set.normalSoften;
    entry.bound = morph.bound;
    writeEntry(SceneTable::Morphs, id, &entry, sizeof(entry));
}

void SceneStore::removeMorphSetIfUnused(uint32_t id, uint64_t frame)
{
    MorphSet& set = mMorphSets[id];
    if (!set.removing || set.users != 0)
    {
        return;
    }
    retireRange(SceneTable::MorphVertices, set.firstVertex, set.vertexCount, frame);
    retireRange(SceneTable::MorphDeltas, set.firstDelta, set.deltaCount, frame);
    set = MorphSet();
    mRetired.push_back({ Retired::Type::MorphSetId, id, 0, 0, frame + mFreeDelay });
}

void SceneStore::removeMorphIfUnused(uint32_t id, uint64_t frame)
{
    Morph& morph = mMorphs[id];
    if (!morph.removing || morph.users != 0)
    {
        return;
    }
    if (morph.set != 0)
    {
        MorphSet& set = mMorphSets[morph.set];
        retireRange(SceneTable::MorphWeights, morph.values, uint32_t(morph.weightsAndMasks.size()), frame);
        retireRange(SceneTable::MorphedPositions, morph.output, set.vertexCount, frame);
        --set.users;
        removeMorphSetIfUnused(morph.set, frame);
    }
    morph = Morph();
    mRetired.push_back({ Retired::Type::MorphId, id, 0, 0, frame + mFreeDelay });
}

// The morphs whose values changed this update, in blocks of SKIN_BLOCK vertices.
void SceneStore::writeMorphWork()
{
    std::vector<GpuMorphWork> work;
    for (uint32_t id : mDirtyMorphs)
    {
        const Morph& morph = mMorphs[id];
        if (morph.set == 0)
        {
            continue; // removed in the same update
        }
        const uint32_t vertices = mMorphSets[morph.set].vertexCount;
        for (uint32_t first = 0; first < vertices; first += SKIN_BLOCK)
        {
            work.push_back({ id, first });
        }
    }
    mDirtyMorphs.clear();
    mMorphWorkCount = uint32_t(work.size());
    if (!work.empty())
    {
        writeTable(SceneTable::MorphWork, 0, work.data(), work.size() * sizeof(GpuMorphWork));
    }
}

void SceneStore::retireRange(SceneTable table, uint32_t first, uint32_t count, uint64_t frame)
{
    if (count != 0)
    {
        mRetired.push_back({ Retired::Type::TableRange, uint32_t(table), first, count, frame + mFreeDelay });
    }
}

void SceneStore::applySkeleton(const SkeletonCommand& command)
{
    if (mSkeletons.size() <= command.id)
    {
        mSkeletons.resize(size_t(command.id) + 1);
    }
    Skeleton& skeleton = mSkeletons[command.id];
    if (command.jointCount != 0)
    {
        skeleton = Skeleton();
        skeleton.jointCount = command.jointCount;
        skeleton.firstJoint = allocateRange(SceneTable::Joints, command.jointCount);
        GpuJoint identity{};
        identity.skeleton = command.id;
        identity.rotationDelta[3] = 1.f;
        for (int row = 0; row < 3; ++row)
        {
            identity.rows[row][row] = 1.f;
            identity.previous[row][row] = 1.f;
        }
        skeleton.joints.assign(command.jointCount, identity);
        writeSkeleton(command.id, true);
    }
    if (command.rows.empty() || skeleton.jointCount == 0)
    {
        return;
    }

    const size_t count = std::min<size_t>(skeleton.jointCount, command.rows.size() / 12);
    for (size_t i = 0; i < count; ++i)
    {
        GpuJoint& joint = skeleton.joints[i];
        if (skeleton.posed)
        {
            memcpy(joint.previous, joint.rows, sizeof(joint.rows));
            memcpy(joint.rows, &command.rows[i * 12], sizeof(joint.rows));
        }
        else
        {
            memcpy(joint.rows, &command.rows[i * 12], sizeof(joint.rows));
            memcpy(joint.previous, joint.rows, sizeof(joint.rows));
        }
        jointRotationDelta(joint.rows, joint.previous, joint.rotationDelta);
    }
    skeleton.previousTime = skeleton.posed ? skeleton.time : command.time;
    if (command.interpolate != 0 && command.time - skeleton.previousTime > command.interpolate)
    {
        skeleton.previousTime = command.time - command.interpolate; // after a rest: blend over one gap, not the whole rest
    }
    skeleton.time = command.time;
    skeleton.posed = true;
    skeleton.interpolate = command.interpolate != 0;
    writeSkeleton(command.id, true);
}

// The cube the joints stay in, at any lead the GPU can draw: around their places from the
// newest pose to the furthest lead, as motion is linear in the lead and its two ends hold the
// rest. Each binding widens it by its own reach times the largest stretch: see bindingBox().
void SceneStore::writeSkeleton(uint32_t id, bool joints)
{
    Skeleton& skeleton = mSkeletons[id];
    const uint32_t step = skeleton.time - skeleton.previousTime; // wraps like the times
    float lead = step == 0 ? 0.f : std::min(SKELETON_MAX_LEAD, SKELETON_MAX_LEAD_SECONDS * 1e6f / float(step));
    float leastLead = 0.f;
    if (skeleton.interpolate)
    {
        leastLead = -1.f; // the pose before, and nothing past the newest
        lead = 0.f;
    }

    float low[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
    float high[3] = { -low[0], -low[1], -low[2] };
    float scale = 0.f;
    for (const GpuJoint& joint : skeleton.joints)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const float now = joint.rows[axis][3];
            const float ahead = now + (now - joint.previous[axis][3]) * lead;
            const float behind = now + (now - joint.previous[axis][3]) * leastLead;
            low[axis] = std::min({ low[axis], now, ahead, behind });
            high[axis] = std::max({ high[axis], now, ahead, behind });
        }
        // Prediction rotates the newest affine shape: its Frobenius norm stays constant.
        float now = 0.f;
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                const float value = joint.rows[row][column];
                now += value * value;
            }
        }
        scale = std::max(scale, std::sqrt(now));
    }
    GpuSkeleton entry{};
    float half = 1e-3f;
    for (int axis = 0; axis < 3; ++axis)
    {
        entry.boxCenter[axis] = (low[axis] + high[axis]) * 0.5f;
        half = std::max(half, (high[axis] - low[axis]) * 0.5f);
    }
    entry.boxScale = half;
    entry.stretch = scale;
    entry.firstJoint = skeleton.firstJoint;
    entry.jointCount = skeleton.jointCount;
    entry.time = skeleton.time;
    entry.previousTime = skeleton.previousTime;
    entry.maxLead = lead;
    entry.minLead = leastLead;
    entry.flags = skeleton.parked ? SKELETON_PARKED : 0;
    writeEntry(SceneTable::Skeletons, id, &entry, sizeof(entry));

    if (!joints)
    {
        return;
    }
    const uint64_t bytes = skeleton.joints.size() * sizeof(GpuJoint);
    if (skeleton.jointUpdate == mUpdate)
    {
        // Overlapping copies in one batch are undefined, so a second pose writes over the first.
        memcpy(mTableBytes.data() + mWrites[skeleton.jointWrite].source, skeleton.joints.data(), bytes);
        return;
    }
    skeleton.jointUpdate = mUpdate;
    skeleton.jointWrite = uint32_t(mWrites.size());
    writeTable(SceneTable::Joints, skeleton.firstJoint, skeleton.joints.data(), bytes);
}

void SceneStore::applySkin(const SkinCommand& command)
{
    if (mSkins.size() <= command.id)
    {
        mSkins.resize(size_t(command.id) + 1);
    }
    Skin& skin = mSkins[command.id];
    skin = Skin();
    const size_t count = std::min(command.desc.joints.size(), command.desc.matrices.size() / 12);
    if (count == 0)
    {
        return; // nothing can be skinned to it
    }
    skin.matrices.assign(command.desc.matrices.begin(), command.desc.matrices.begin() + count * 12);
    skin.joints.assign(command.desc.joints.begin(), command.desc.joints.begin() + count);
    skin.firstMatrix = allocateRange(SceneTable::SkinMatrices, count);
    writeTable(SceneTable::SkinMatrices, skin.firstMatrix, skin.matrices.data(), count * 48);
}

void SceneStore::removeSkeletonIfUnused(uint32_t id, uint64_t frame)
{
    Skeleton& skeleton = mSkeletons[id];
    if (!skeleton.removing || skeleton.users != 0)
    {
        return;
    }
    const GpuSkeleton empty{};
    writeEntry(SceneTable::Skeletons, id, &empty, sizeof(empty));
    retireRange(SceneTable::Joints, skeleton.firstJoint, skeleton.jointCount, frame);
    skeleton = Skeleton();
    mRetired.push_back({ Retired::Type::SkeletonId, id, 0, 0, frame + mFreeDelay });
}

void SceneStore::removeSkinIfUnused(uint32_t id, uint64_t frame)
{
    Skin& skin = mSkins[id];
    if (!skin.removing || skin.users != 0)
    {
        return;
    }
    retireRange(SceneTable::SkinMatrices, skin.firstMatrix, uint32_t(skin.joints.size()), frame);
    skin = Skin();
    mRetired.push_back({ Retired::Type::SkinId, id, 0, 0, frame + mFreeDelay });
}

// One palette per skeleton and skin, whatever number of faces and views use it.
uint32_t SceneStore::usePalette(uint32_t skeletonId, uint32_t skinId, uint32_t binding)
{
    const uint64_t key = uint64_t(skeletonId) << 32 | skinId;
    auto found = mPalettes.find(key);
    if (found != mPalettes.end())
    {
        ++found->second.users;
        found->second.bindings.push_back(binding);
        return found->second.first;
    }

    Skeleton& skeleton = mSkeletons[skeletonId];
    Skin& skin = mSkins[skinId];
    Palette palette;
    palette.count = uint32_t(skin.joints.size());
    palette.first = allocateRange(SceneTable::Palette, palette.count);
    palette.users = 1;
    palette.bindings.push_back(binding);
    std::vector<GpuPaletteJob> jobs(palette.count);
    for (uint32_t i = 0; i < palette.count; ++i)
    {
        jobs[i].joint = skeleton.firstJoint + std::min<uint32_t>(skin.joints[i], skeleton.jointCount - 1);
        jobs[i].matrix = skin.firstMatrix + i;
        jobs[i].skeleton = skeletonId;
    }
    writeTable(SceneTable::PaletteJobs, palette.first, jobs.data(), jobs.size() * sizeof(GpuPaletteJob));
    ++skeleton.users;
    ++skin.users;
    const uint32_t first = palette.first;
    mPalettes.emplace(key, std::move(palette));
    return first;
}

void SceneStore::releasePalette(uint32_t skeletonId, uint32_t skinId, uint32_t binding, uint64_t frame)
{
    const auto found = mPalettes.find(uint64_t(skeletonId) << 32 | skinId);
    if (found == mPalettes.end())
    {
        return;
    }
    auto& bindings = found->second.bindings;
    bindings.erase(std::remove(bindings.begin(), bindings.end(), binding), bindings.end());
    if (--found->second.users != 0)
    {
        return;
    }
    const Palette palette = std::move(found->second);
    mPalettes.erase(found);
    const std::vector<GpuPaletteJob> empty(palette.count);
    writeTable(SceneTable::PaletteJobs, palette.first, empty.data(), empty.size() * sizeof(GpuPaletteJob));
    retireRange(SceneTable::Palette, palette.first, palette.count, frame);
    --mSkeletons[skeletonId].users;
    --mSkins[skinId].users;
    removeSkeletonIfUnused(skeletonId, frame);
    removeSkinIfUnused(skinId, frame);
}

void SceneStore::writeSkinnedBinding(uint32_t id, uint32_t instance, const InstanceDesc& desc)
{
    mBindings[id].instance = instance;
    mBindings[id].lodSize = desc.lodSize;
    writeSkinnedBinding(id);
}

// A skinned binding's own storage: its deformed positions, its clusters and groups, and the
// blocks the skin jobs run over. Only while its skeleton is resident: see setResident().
void SceneStore::allocateDeformed(uint32_t id)
{
    Binding& binding = mBindings[id];
    const uint32_t clusters = binding.entryCount - binding.groupCapacity;
    uint64_t firstEntry = 0;
    while (!mClusterRanges.allocate(binding.entryCount, 1, firstEntry))
    {
        mClusterRanges.grow(mClusterRanges.capacity() + std::max<uint64_t>({ mClusterRanges.capacity() / 4, 4096, binding.entryCount })); // see ensureTable()
    }
    binding.targetEntry = uint32_t(firstEntry);
    binding.positions = allocateRange(SceneTable::SkinnedPositions, binding.vertexCapacity);
    // Vertex and cluster jobs each run over their own blocks: clusters are far fewer.
    binding.blockCount = std::max((binding.vertexCapacity + SKIN_BLOCK - 1) / SKIN_BLOCK, 1u);
    binding.firstBlock = allocateRange(SceneTable::SkinBlocks, binding.blockCount);
    const std::vector<uint32_t> blocks(binding.blockCount, id);
    writeTable(SceneTable::SkinBlocks, binding.firstBlock, blocks.data(), blocks.size() * sizeof(uint32_t));
    binding.clusterBlockCount = std::max((clusters + SKIN_BLOCK - 1) / SKIN_BLOCK, 1u);
    binding.firstClusterBlock = allocateRange(SceneTable::ClusterBlocks, binding.clusterBlockCount);
    const std::vector<uint32_t> clusterBlocks(binding.clusterBlockCount, id);
    writeTable(SceneTable::ClusterBlocks, binding.firstClusterBlock, clusterBlocks.data(), clusterBlocks.size() * sizeof(uint32_t));
}

// Its geometry entry goes empty, so its instance draws nothing until it is skinned again.
void SceneStore::releaseDeformed(uint32_t id, uint64_t frame)
{
    const Binding& binding = mBindings[id];
    const std::vector<uint32_t> blocks(binding.blockCount, 0u);
    writeTable(SceneTable::SkinBlocks, binding.firstBlock, blocks.data(), blocks.size() * sizeof(uint32_t));
    retireRange(SceneTable::SkinBlocks, binding.firstBlock, binding.blockCount, frame);
    const std::vector<uint32_t> clusterBlocks(binding.clusterBlockCount, 0u);
    writeTable(SceneTable::ClusterBlocks, binding.firstClusterBlock, clusterBlocks.data(), clusterBlocks.size() * sizeof(uint32_t));
    retireRange(SceneTable::ClusterBlocks, binding.firstClusterBlock, binding.clusterBlockCount, frame);
    retireRange(SceneTable::SkinnedPositions, binding.positions, binding.vertexCapacity, frame);
    const GpuGeometry emptyGeometry{};
    writeEntry(SceneTable::Geometries, binding.target, &emptyGeometry, sizeof(emptyGeometry));
    mRetired.push_back({ Retired::Type::Clusters, 0, binding.targetEntry, binding.entryCount, frame + mFreeDelay });
}

// An avatar out of view gives back every skinned copy it has; back in view, they are made
// again in the same update, and the skin jobs fill them before anything draws them.
void SceneStore::setResident(uint32_t skeletonId, bool resident, uint64_t frame)
{
    if (skeletonId >= mSkeletons.size() || mSkeletons[skeletonId].jointCount == 0 || mSkeletons[skeletonId].parked == !resident)
    {
        return;
    }
    mSkeletons[skeletonId].parked = !resident;
    writeSkeleton(skeletonId, false); // palettes can skip parked skins; rigid attachments still pose
    for (uint32_t id = 1; id < mBindings.size(); ++id)
    {
        Binding& binding = mBindings[id];
        if (binding.kind != BINDING_SKINNED || binding.skeleton != skeletonId || binding.parked == !resident)
        {
            continue;
        }
        binding.parked = !resident;
        if (resident)
        {
            allocateDeformed(id);
        }
        else
        {
            releaseDeformed(id, frame);
        }
        writeSkinnedBinding(id);
    }
}

void SceneStore::writeSkinnedBinding(uint32_t id)
{
    const Binding& binding = mBindings[id];
    if (binding.parked)
    {
        const GpuBinding empty{}; // the skin jobs pass it by
        writeEntry(SceneTable::Bindings, id, &empty, sizeof(empty));
        return;
    }
    const Geometry& source = mGeometries[binding.source];
    const Palette& palette = mPalettes[uint64_t(binding.skeleton) << 32 | binding.skin];
    GpuBinding entry{};
    entry.kind = BINDING_SKINNED;
    entry.instance = binding.instance;
    entry.skeleton = binding.skeleton;
    entry.palette = palette.first;
    entry.matrix = uint32_t(mSkins[binding.skin].joints.size());
    entry.source = binding.source;
    entry.target = binding.target;
    entry.targetEntry = binding.targetEntry;
    entry.lod[3] = binding.lodSize;
    entry.reach = palette.reach;
    memcpy(entry.sourceBox, source.center, sizeof(source.center));
    entry.sourceBox[3] = source.scale;
    entry.positions = binding.positions;
    entry.firstBlock = binding.firstBlock;
    entry.firstClusterBlock = binding.firstClusterBlock;
    entry.groupCapacity = binding.groupCapacity;
    entry.morph = binding.morph;
    writeEntry(SceneTable::Bindings, id, &entry, sizeof(entry));
}

uint32_t SceneStore::bindSkinned(uint32_t instance, const InstanceDesc& desc)
{
    if (desc.skeleton >= mSkeletons.size() || mSkeletons[desc.skeleton].jointCount == 0 || mSkeletons[desc.skeleton].removing ||
        desc.skin >= mSkins.size() || mSkins[desc.skin].joints.empty() || mSkins[desc.skin].removing)
    {
        return 0;
    }

    // An unweighted/invalid coarse LOD must not reject valid weighted finer LODs.
    // The GPU walks the same chain, retaining only LODs this palette can skin.
    uint32_t vertices = 0;
    uint32_t groups = 0;
    uint32_t clusters = 0;
    uint32_t at = desc.geometry;
    for (uint32_t lod = 0; lod < MAX_LODS && at != 0; ++lod)
    {
        const Geometry& geometry = mGeometries[at];
        at = geometry.coarser;
        if (!geometry.skinned || geometry.skinJoints == 0 || geometry.skinJoints > mSkins[desc.skin].joints.size())
        {
            continue;
        }
        vertices = std::max(vertices, geometry.vertexCount);
        groups = std::max(groups, geometry.groupCount);
        clusters = std::max(clusters, geometry.clusterCount);
    }
    if (vertices == 0 || clusters == 0)
    {
        return 0;
    }
    // A morph must cover every vertex any LOD of the chain has.
    if (desc.morph != 0 && (desc.morph >= mMorphs.size() || mMorphs[desc.morph].set == 0 || mMorphs[desc.morph].removing ||
                            mMorphSets[mMorphs[desc.morph].set].vertexCount < vertices))
    {
        return 0;
    }

    uint32_t id = 0;
    if (mBindings.empty())
    {
        mBindings.resize(1); // 0 is never a binding
    }
    if (!mFreeBindings.empty())
    {
        id = mFreeBindings.back();
        mFreeBindings.pop_back();
    }
    else
    {
        id = uint32_t(mBindings.size());
        mBindings.emplace_back();
    }
    ensureTable(SceneTable::Bindings, uint64_t(mBindings.size()));

    Binding& binding = mBindings[id];
    binding = Binding();
    binding.kind = BINDING_SKINNED;
    binding.skeleton = desc.skeleton;
    binding.skin = desc.skin;
    binding.source = desc.geometry;
    binding.morph = desc.morph;
    if (desc.morph != 0)
    {
        ++mMorphs[desc.morph].users;
    }
    binding.target = mQueue->reserveGeometry();
    ensureTable(SceneTable::Geometries, uint64_t(binding.target) + 1);
    const GpuGeometry empty{};
    writeEntry(SceneTable::Geometries, binding.target, &empty, sizeof(empty));

    binding.groupCapacity = groups;
    binding.entryCount = groups + clusters;
    binding.vertexCapacity = vertices;
    binding.parked = mSkeletons[desc.skeleton].parked;
    if (!binding.parked)
    {
        allocateDeformed(id);
    }
    usePalette(desc.skeleton, desc.skin, id);

    // How far its vertices can be from the joints that move them, in the joints' own space:
    // for each joint some LOD's weights name, the sphere around its vertices carried through
    // that joint's skin matrix. Joints the skin lists but no vertex uses add nothing.
    const Skin& skin = mSkins[desc.skin];
    for (uint32_t at = desc.geometry, lod = 0; lod < MAX_LODS && at != 0; ++lod, at = mGeometries[at].coarser)
    {
        for (const GeometryData::JointSphere& sphere : mGeometries[at].jointSpheres)
        {
            if (sphere.joint >= skin.joints.size())
            {
                continue; // the instance cannot be drawn right anyway: the viewer leaves such faces out
            }
            const float* m = &skin.matrices[size_t(sphere.joint) * 12];
            float center = 0.f;
            float stretch = 0.f;
            for (int row = 0; row < 3; ++row)
            {
                const float* r = m + row * 4;
                const float c = r[0] * sphere.center[0] + r[1] * sphere.center[1] + r[2] * sphere.center[2] + r[3];
                center += c * c;
                stretch += r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
            }
            binding.reachCenter = std::max(binding.reachCenter, std::sqrt(center) + sphere.radius * std::sqrt(stretch));
            binding.reachStretch = std::max(binding.reachStretch, std::sqrt(stretch));
        }
    }
    ++mSkeletons[desc.skeleton].users;
    if (desc.morph != 0)
    {
        mMorphs[desc.morph].bindings.push_back(id);
    }
    binding.instance = instance;
    binding.lodSize = desc.lodSize;
    widenReach(id);
    ++mSkinnedBindings;
    return id;
}

void SceneStore::widenReach(uint32_t id)
{
    Binding& binding = mBindings[id];
    const float bound = binding.morph != 0 ? mMorphs[binding.morph].bound : 0.f;
    const float reach = binding.reachCenter + bound * binding.reachStretch;
    binding.reach = std::max(binding.reach, reach);
    Palette& palette = mPalettes[uint64_t(binding.skeleton) << 32 | binding.skin];
    if (binding.reach > palette.reach)
    {
        // A new face, finer LOD or morph can widen the mesh. Publish one grid for all
        // its faces in the same update, including faces that were already bound.
        palette.reach = binding.reach;
        for (uint32_t face : palette.bindings)
        {
            writeSkinnedBinding(face);
        }
    }
    else
    {
        writeSkinnedBinding(id);
    }
}

uint32_t SceneStore::bindAttached(uint32_t instance, const InstanceDesc& desc, const GpuInstance& placed)
{
    if (desc.skeleton >= mSkeletons.size() || mSkeletons[desc.skeleton].jointCount == 0 || mSkeletons[desc.skeleton].removing ||
        desc.joint >= mSkeletons[desc.skeleton].jointCount)
    {
        return 0;
    }
    uint32_t id = 0;
    if (mBindings.empty())
    {
        mBindings.resize(1);
    }
    if (!mFreeBindings.empty())
    {
        id = mFreeBindings.back();
        mFreeBindings.pop_back();
    }
    else
    {
        id = uint32_t(mBindings.size());
        mBindings.emplace_back();
    }
    ensureTable(SceneTable::Bindings, uint64_t(mBindings.size()));

    Skeleton& skeleton = mSkeletons[desc.skeleton];
    ++skeleton.users;
    Binding& binding = mBindings[id];
    binding = Binding();
    binding.kind = BINDING_ATTACHED;
    binding.skeleton = desc.skeleton;
    binding.matrix = allocateRange(SceneTable::SkinMatrices, 1);
    writeTable(SceneTable::SkinMatrices, binding.matrix, placed.rows, sizeof(placed.rows)); // packing to joint space

    GpuBinding entry{};
    entry.kind = BINDING_ATTACHED;
    entry.instance = instance;
    entry.skeleton = desc.skeleton;
    entry.palette = skeleton.firstJoint + desc.joint;
    entry.matrix = binding.matrix;
    memcpy(entry.lod, desc.lodCenter, sizeof(desc.lodCenter));
    entry.lod[3] = desc.lodSize;
    writeEntry(SceneTable::Bindings, id, &entry, sizeof(entry));
    return id;
}

void SceneStore::unbind(uint32_t id, uint64_t frame)
{
    Binding& binding = mBindings[id];
    const GpuBinding empty{};
    writeEntry(SceneTable::Bindings, id, &empty, sizeof(empty));
    if (binding.kind == BINDING_SKINNED)
    {
        if (!binding.parked)
        {
            releaseDeformed(id, frame);
        }
        mRetired.push_back({ Retired::Type::GeometryId, binding.target, 0, 0, frame + mFreeDelay });
        releasePalette(binding.skeleton, binding.skin, id, frame);
        --mSkinnedBindings;
        if (binding.morph != 0)
        {
            Morph& morph = mMorphs[binding.morph];
            morph.bindings.erase(std::remove(morph.bindings.begin(), morph.bindings.end(), id), morph.bindings.end());
            --morph.users;
            removeMorphIfUnused(binding.morph, frame);
        }
    }
    else if (binding.kind == BINDING_ATTACHED)
    {
        retireRange(SceneTable::SkinMatrices, binding.matrix, 1, frame);
    }
    const uint32_t skeleton = binding.skeleton;
    binding = Binding();
    mRetired.push_back({ Retired::Type::BindingId, id, 0, 0, frame + mFreeDelay });
    if (skeleton != 0)
    {
        --mSkeletons[skeleton].users;
        removeSkeletonIfUnused(skeleton, frame);
    }
}

void SceneStore::fillView(GpuSceneView& view) const
{
    uint32_t base = 0;
    for (uint32_t list = 0; list < SCENE_LISTS; ++list)
    {
        view.listBase[list] = base;
        base += listCapacity(list);
    }
    view.instanceEnd = mInstanceEnd;
    view.visibleEnd = base;
    view.clusterEnd = uint32_t(tableCapacity(SceneTable::Clusters) / sizeof(GpuCluster));
    view.materialEnd = uint32_t(tableCapacity(SceneTable::Materials) / sizeof(GpuMaterial));
    view.groupEnd = uint32_t(groupCapacity());
    view.selectionEnd = mSelectedClusters; // the backend sets view.selection
    view.paletteEnd = uint32_t(mRanges[uint32_t(SceneTable::Palette)].used());
    view.bindingEnd = uint32_t(mBindings.size());
    view.blockEnd = uint32_t(mRanges[uint32_t(SceneTable::SkinBlocks)].used());
    view.morphWorkEnd = mMorphWorkCount;
    view.jointEnd = uint32_t(mRanges[uint32_t(SceneTable::Joints)].used());
    view.clusterBlockEnd = uint32_t(mRanges[uint32_t(SceneTable::ClusterBlocks)].used());
}

uint32_t SceneStore::skinThreads(uint32_t phase) const
{
    switch (phase)
    {
        case SKIN_POSE:
            return uint32_t(mRanges[uint32_t(SceneTable::Joints)].used());
        case SKIN_PALETTE:
            return uint32_t(mRanges[uint32_t(SceneTable::Palette)].used());
        case SKIN_BIND:
            return uint32_t(mBindings.size());
        case SKIN_MORPH:
            return mMorphWorkCount * SKIN_BLOCK;
        case SKIN_VERTICES:
        case SKIN_VERTICES_LATE:
            return mSkinnedBindings == 0 ? 0 : uint32_t(mRanges[uint32_t(SceneTable::SkinBlocks)].used()) * SKIN_BLOCK;
        case SKIN_CLUSTERS:
        case SKIN_AUDIT:
            return mSkinnedBindings == 0 ? 0 : uint32_t(mRanges[uint32_t(SceneTable::ClusterBlocks)].used()) * SKIN_BLOCK;
        default: // groups: a workgroup per binding
            return mSkinnedBindings == 0 ? 0 : uint32_t(mBindings.size()) * SKIN_BLOCK;
    }
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

    // Its entry in mPending is skipped when it reaches the front. Taking it out here would
    // mean searching the queue, once for each of the thousands removed by a teleport.
    if (geometry.queued)
    {
        geometry.queued = false;
        --mQueuedGeometries;
    }
    const GpuGeometry empty{};
    writeEntry(SceneTable::Geometries, id, &empty, sizeof(empty));
    if (geometry.uploaded)
    {
        mRetired.push_back({ Retired::Type::Clusters, 0, geometry.firstEntry, geometry.entryCount, frame + mFreeDelay });
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
        // Reuse an empty slot without moving any live chunk's index or device address.
        chunkIndex = 0;
        while (chunkIndex < mChunks.size() && mChunks[chunkIndex].address != 0) ++chunkIndex;
        const uint64_t address = chunks.createSceneChunk(chunkIndex);
        if (address == 0)
        {
            return false; // no memory for it: it waits, and the budget's pressure rules take over
        }
        Chunk chunk;
        chunk.address = address;
        chunk.ranges.grow(SCENE_CHUNK_BYTES);
        if (chunkIndex == mChunks.size()) mChunks.emplace_back();
        mChunks[chunkIndex] = std::move(chunk);
        mChunks[chunkIndex].ranges.allocate(blobBytes, BLOB_ALIGNMENT, chunkOffset);
    }

    const uint64_t groupCount = data.groups.size();
    const uint64_t clusterCount = data.clusters.size();
    uint64_t firstEntry = 0;
    while (!mClusterRanges.allocate(groupCount + clusterCount, 1, firstEntry))
    {
        mClusterRanges.grow(std::max<uint64_t>(mClusterRanges.capacity() * 2, 4096));
    }
    const uint64_t firstCluster = firstEntry + groupCount;

    mGroupScratch.resize(groupCount);
    for (size_t i = 0; i < groupCount; ++i)
    {
        const ClusterGroupData& from = data.groups[i];
        GpuClusterGroup& to = mGroupScratch[i];
        to = GpuClusterGroup{};
        memcpy(to.center, from.center, sizeof(to.center));
        to.radius = from.radius;
        to.cone = from.cone;
        to.firstCluster = uint32_t(firstCluster + from.firstCluster);
        to.clusterCount = from.clusterCount;
    }
    if (groupCount != 0)
    {
        writeTable(SceneTable::Clusters, uint32_t(firstEntry), mGroupScratch.data(), groupCount * sizeof(GpuClusterGroup));
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
        to.positions = base + data.positionsOffset + (data.skinned ? 1u : 0u); // float positions
        to.attributes = base + data.attributesOffset;

        const uint32_t bucket = bucketOf(from.triangles);
        ++geometry.buckets[bucket];
        geometry.triangles += from.triangles;
    }
    writeTable(SceneTable::Clusters, uint32_t(firstCluster), mClusterScratch.data(), clusterCount * sizeof(GpuCluster));

    GpuGeometry entry{};
    entry.firstGroup = uint32_t(firstEntry);
    entry.groupCount = uint32_t(groupCount);
    entry.skinJoints = geometry.skinJoints;
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
    geometry.firstEntry = uint32_t(firstEntry);
    geometry.entryCount = uint32_t(groupCount + clusterCount);
    geometry.groupCount = uint32_t(groupCount);
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
            case Retired::Type::TerrainId:
                queue.releaseTerrain(retired.index);
                break;
            case Retired::Type::TableRange:
                mRanges[retired.index].release(retired.offset, retired.size);
                break;
            case Retired::Type::BindingId:
                mFreeBindings.push_back(retired.index);
                break;
            case Retired::Type::SkeletonId:
                queue.releaseSkeleton(retired.index);
                break;
            case Retired::Type::SkinId:
                queue.releaseSkin(retired.index);
                break;
            case Retired::Type::MorphSetId:
                queue.releaseMorphSet(retired.index);
                break;
            case Retired::Type::MorphId:
                queue.releaseMorph(retired.index);
                break;
        }
    }
}

void SceneStore::update(SceneQueue& queue, SceneChunks& chunks, uint64_t frame, uint64_t uploadLimit, std::chrono::steady_clock::time_point time)
{
    mWrites.clear();
    mTableBytes.clear();
    mWrittenBlobs.clear();
    mPositionsCompacted = false;
    ++mUpdate;
    mQueue = &queue;

    if (!mStarted)
    {
        // Entries 0 are written once. Tables start zeroed, and the first material is white.
        // Every table exists from the start, so each has an address.
        mStarted = true;
        applyMaterial(MaterialCommand());
        for (uint32_t table = 0; table < SCENE_TABLES; ++table)
        {
            ensureTable(SceneTable(table), 1);
        }
    }

    runRetired(queue, frame);
    trimTables();
    compactDeformedPositions();

    // A take finishes before the next one starts, which keeps commands in order.
    if (mInstancesDone == mCommands.instances.size())
    {
        mCommands.instances.clear();
        mInstancesDone = 0;
        queue.take(mCommands);
        for (const TerrainCommand& command : mCommands.terrains)
        {
            applyTerrain(command);
        }
        for (const MaterialCommand& command : mCommands.materials)
        {
            applyMaterial(command);
        }
        for (GeometryCommand& command : mCommands.geometries)
        {
            applyGeometry(command);
        }
        for (const SkeletonCommand& command : mCommands.skeletons)
        {
            applySkeleton(command);
        }
        for (const SkinCommand& command : mCommands.skins)
        {
            applySkin(command);
        }
        for (const MorphSetCommand& command : mCommands.morphSets)
        {
            applyMorphSet(command);
        }
        for (const MorphCommand& command : mCommands.morphs)
        {
            applyMorph(command);
        }
        mCommands.terrains.clear();
        mCommands.materials.clear();
        mCommands.geometries.clear();
        mCommands.skeletons.clear();
        mCommands.skins.clear();
        mCommands.morphSets.clear();
        mCommands.morphs.clear();
    }

    while (mInstancesDone < mCommands.instances.size() && mTableBytes.size() < uploadLimit / 2)
    {
        applyInstance(mCommands.instances[mInstancesDone++], frame);
    }
    if (mInstancesDone == mCommands.instances.size())
    {
        // After the instances, so bindings made in this update follow too.
        for (const auto& resident : mCommands.residentSkeletons)
        {
            setResident(resident.first, resident.second, frame);
        }
        mCommands.residentSkeletons.clear();
        for (uint32_t id : mCommands.removedGeometries)
        {
            removeGeometry(id, frame);
        }
        mCommands.removedGeometries.clear();
        for (uint32_t id : mCommands.removedMaterials)
        {
            setAnimated(id, nullptr, TextureAnimDesc());
            mRetired.push_back({ Retired::Type::MaterialId, id, 0, 0, frame + mFreeDelay });
        }
        mCommands.removedMaterials.clear();
        for (uint32_t id : mCommands.removedTerrains)
        {
            mRetired.push_back({ Retired::Type::TerrainId, id, 0, 0, frame + mFreeDelay });
        }
        mCommands.removedTerrains.clear();
        for (uint32_t id : mCommands.removedSkeletons)
        {
            if (id < mSkeletons.size() && mSkeletons[id].jointCount != 0)
            {
                mSkeletons[id].removing = true;
                removeSkeletonIfUnused(id, frame);
            }
        }
        mCommands.removedSkeletons.clear();
        for (uint32_t id : mCommands.removedSkins)
        {
            if (id < mSkins.size() && !mSkins[id].joints.empty())
            {
                mSkins[id].removing = true;
                removeSkinIfUnused(id, frame);
            }
            else if (id < mSkins.size())
            {
                mRetired.push_back({ Retired::Type::SkinId, id, 0, 0, frame + mFreeDelay }); // never held anything
            }
        }
        mCommands.removedSkins.clear();
        for (uint32_t id : mCommands.removedMorphs)
        {
            if (id < mMorphs.size())
            {
                mMorphs[id].removing = true;
                removeMorphIfUnused(id, frame);
            }
        }
        mCommands.removedMorphs.clear();
        for (uint32_t id : mCommands.removedMorphSets)
        {
            if (id < mMorphSets.size() && mMorphSets[id].vertexCount != 0)
            {
                mMorphSets[id].removing = true;
                removeMorphSetIfUnused(id, frame);
            }
            else if (id < mMorphSets.size())
            {
                mRetired.push_back({ Retired::Type::MorphSetId, id, 0, 0, frame + mFreeDelay });
            }
        }
        mCommands.removedMorphSets.clear();
    }
    animateMaterials(time);

    uint64_t uploaded = 0;
    while (!mPending.empty())
    {
        const uint32_t id = mPending.front();
        if (!mGeometries[id].queued)
        {
            mPending.pop_front(); // removed while it waited
            continue;
        }
        const GeometryData& data = mGeometries[id].data;
        const uint64_t bytes = data.blob.size() + (data.groups.size() + data.clusters.size()) * sizeof(GpuCluster);
        if ((uploaded != 0 && mTableBytes.size() + uploaded + bytes > uploadLimit) || !upload(id, chunks))
        {
            break;
        }
        uploaded += bytes;
        mGeometries[id].queued = false;
        --mQueuedGeometries;
        mPending.pop_front();
    }
    // Empty chunks have no live or in-flight users. Destroy the actual backend allocation,
    // rather than counting its bytes as free while keeping the driver's memory forever.
    for (uint32_t i = 0; i < mChunks.size(); ++i)
    {
        if (mChunks[i].address != 0 && mChunks[i].ranges.live() == 0)
        {
            chunks.releaseSceneChunk(i);
            mChunks[i] = Chunk();
        }
    }
    ensureTable(SceneTable::Clusters, mClusterRanges.used());
    writeMorphWork();
    ensureTable(SceneTable::MorphWork, mMorphWorkCount);
}

} // namespace asr
