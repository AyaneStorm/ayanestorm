/**
 * asrtestmain.cpp
 * asrenderer_test: renderer checks that need no viewer build and no login.
 *
 *   asrenderer_test --selftest           portable checks, no window
 *   asrenderer_test [--seconds N]        draw the test triangle for N seconds (default 10)
 *                   [--ui]               also draw a busy test UI, rebuilt on the main thread
 *                   [--scene]            draw a city of 16,000 objects instead of the triangle
 *                   [--stall N]          freeze the main thread for N seconds (default 2, 0 for none)
 *                   [--size W H]         window size (default 1280 720)
 *                   [--keep-size]        skip the resize during the run, for speed readings
 *                   [--memory-cap-mb N]  lower the video memory budget
 *                   [--validate]         turn on the Vulkan validation layers
 *                   [--vsync]            wait for the display
 *                   [--shaders DIR]      compiled shader folder
 *
 * Exit code 0 means every check passed.
 */
#include "asrlatestvalue.h"
#include "asrmemorybudget.h"
#include "asrscene.h"
#include "asrscenefile.h"
#include "asrscenestore.h"
#include "asrtexturequeue.h"
#include "asruilist.h"

#ifdef ASR_HAS_DEVICE
#include "asrdevice.h"
#include "asrrenderthread.h"
#include "asrtestwindow.h"
#endif
#include "asrtestscene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

int sFailures = 0;

void expect(bool condition, const char* what)
{
    printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
    sFailures += condition ? 0 : 1;
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes)
{
    FILE* file = fopen(path.c_str(), "wb");
    if (file)
    {
        fwrite(bytes.data(), 1, bytes.size(), file);
        fclose(file);
    }
}

std::vector<uint8_t> readFile(const std::string& path)
{
    std::vector<uint8_t> bytes;
    FILE* file = fopen(path.c_str(), "rb");
    if (file)
    {
        uint8_t buffer[4096];
        size_t got;
        while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        {
            bytes.insert(bytes.end(), buffer, buffer + got);
        }
        fclose(file);
    }
    return bytes;
}

// Round trip, plus damaged files: each must give an error, never a crash.
void testSceneFile(const std::string& path)
{
    asr::SceneCamera camera{};
    camera.position[0] = 128.f;
    camera.verticalFovRadians = 1.05f;
    camera.width = 1920;
    camera.height = 1080;

    asr::SceneChunk cameraChunk;
    cameraChunk.type = asr::SCENE_CHUNK_CAMERA;
    cameraChunk.data.resize(sizeof(camera));
    memcpy(cameraChunk.data.data(), &camera, sizeof(camera));

    asr::SceneChunk unknownChunk;
    unknownChunk.type = asr::fourCC('Z', 'Z', 'Z', 'Z');
    unknownChunk.version = 7;
    unknownChunk.data = { 1, 2, 3, 4, 5 };

    asr::SceneFile saved;
    saved.chunks.push_back(cameraChunk);
    saved.chunks.push_back(unknownChunk);
    saved.chunks.push_back(asr::SceneChunk{ asr::SCENE_CHUNK_GEOMETRY, 1, {} });

    std::string error;
    expect(saved.save(path, error), "scene file saves");

    asr::SceneFile loaded;
    expect(loaded.load(path, error), "scene file loads");
    expect(loaded.chunks.size() == 3, "all chunks come back");
    const asr::SceneChunk* found = loaded.find(asr::SCENE_CHUNK_CAMERA);
    expect(found && found->data == cameraChunk.data, "camera bytes are identical");
    if (found && found->data.size() == sizeof(camera))
    {
        asr::SceneCamera read;
        memcpy(&read, found->data.data(), sizeof(read));
        expect(read.position[0] == 128.f && read.width == 1920, "camera values are identical");
    }
    expect(loaded.find(unknownChunk.type) && loaded.find(unknownChunk.type)->version == 7, "unknown chunk keeps its version");
    expect(loaded.find(asr::SCENE_CHUNK_GEOMETRY) && loaded.find(asr::SCENE_CHUNK_GEOMETRY)->data.empty(), "empty chunk survives");
    expect(!loaded.find(asr::SCENE_CHUNK_LIGHTS), "missing chunk is not found");

    const std::vector<uint8_t> good = readFile(path);

    std::vector<uint8_t> bad = good;
    bad[0] ^= 0xff;
    writeFile(path, bad);
    expect(!loaded.load(path, error), "bad magic is refused");

    bad = good;
    bad[4] = 99;
    writeFile(path, bad);
    expect(!loaded.load(path, error), "unknown file version is refused");

    bad = good;
    memset(&bad[8], 0xff, 4); // huge chunk count
    writeFile(path, bad);
    expect(!loaded.load(path, error), "huge chunk count is refused");

    bad = good;
    memset(&bad[16 + 16], 0xff, 8); // first chunk size
    writeFile(path, bad);
    expect(!loaded.load(path, error), "huge chunk size is refused");

    bad = good;
    memset(&bad[16 + 8], 0xff, 8); // first chunk offset
    writeFile(path, bad);
    expect(!loaded.load(path, error), "bad chunk offset is refused");

    bad = good;
    bad.resize(good.size() - 3);
    writeFile(path, bad);
    expect(!loaded.load(path, error), "truncated file is refused");

    bad.assign(good.begin(), good.begin() + 10);
    writeFile(path, bad);
    expect(!loaded.load(path, error), "file shorter than its header is refused");

    remove(path.c_str());
    expect(!loaded.load(path, error), "missing file is refused");
}

// The fields only match if a value is never read while it is being written.
struct LatestValueTestData
{
    uint64_t a = 0;
    uint64_t padding[6] = {};
    uint64_t b = 0;
};

void testLatestValue()
{
    asr::LatestValue<LatestValueTestData> latest;
    expect(latest.read().a == 0, "reads a default value before the first publish");

    latest.writeSlot().a = 1;
    latest.writeSlot().b = 1;
    expect(latest.read().a == 0, "unpublished value stays hidden");
    latest.publish();
    expect(latest.read().a == 1, "published value arrives");
    expect(latest.read().a == 1, "newest value can be read again");

    for (uint64_t i = 2; i <= 5; ++i)
    {
        latest.writeSlot().a = i;
        latest.writeSlot().b = i;
        latest.publish();
    }
    expect(latest.read().a == 5, "a slow reader skips to the newest value");

    std::atomic<bool> stop{ false };
    uint64_t published = 5;
    std::thread writer([&]()
    {
        while (!stop.load(std::memory_order_relaxed))
        {
            ++published;
            LatestValueTestData& slot = latest.writeSlot();
            slot.a = published;
            slot.b = published;
            latest.publish();
        }
    });

    uint64_t reads = 0;
    uint64_t previous = 0;
    uint64_t changes = 0;
    bool torn = false;
    bool backwards = false;
    const Clock::time_point start = Clock::now();
    while (secondsSince(start) < 0.5)
    {
        const LatestValueTestData& value = latest.read();
        torn = torn || value.a != value.b;
        backwards = backwards || value.a < previous;
        changes += value.a != previous ? 1 : 0;
        previous = value.a;
        ++reads;
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    printf("      %llu reads, %llu new values, %llu published\n", (unsigned long long)reads,
           (unsigned long long)changes, (unsigned long long)published);
    expect(!torn, "no value is read half written, across threads");
    expect(!backwards, "values never go back in time");
    expect(changes > 100, "the reader keeps seeing new values");
}

void testMemoryBudget()
{
    using Category = asr::MemoryCategory;
    constexpr uint64_t GB = 1024ull * 1024 * 1024;

    asr::MemoryBudget budget;
    expect(!budget.reserve(Category::Textures, 1), "nothing fits before a budget is set");

    budget.setBudget(4 * GB);
    uint64_t total = 0;
    for (uint32_t i = 0; i < uint32_t(Category::Count); ++i)
    {
        total += budget.limit(Category(i));
    }
    expect(total == 4 * GB, "category limits add up to the budget exactly");
    expect(budget.limit(Category::Textures) > budget.limit(Category::RenderTargets), "textures take what is left");

    expect(budget.reserve(Category::UI, 1024), "a small reservation fits");
    expect(budget.used(Category::UI) == 1024 && budget.totalUsed() == 1024, "reservation is counted");
    expect(!budget.reserve(Category::UI, budget.limit(Category::UI)), "reservation past the limit is refused");
    expect(budget.used(Category::UI) == 1024, "a refused reservation changes nothing");
    expect(!budget.reserve(Category::UI, ~0ull), "huge reservation does not wrap around");
    budget.release(Category::UI, 1024);
    expect(budget.used(Category::UI) == 0 && budget.totalUsed() == 0, "release gives the memory back");
    expect(budget.reserve(Category::UI, budget.limit(Category::UI)), "a category can use all of its limit");
    budget.release(Category::UI, budget.limit(Category::UI));

    // One category cannot reach 85% of the total, so fill all of them equally.
    const auto fill = [&](uint64_t percent)
    {
        for (uint32_t i = 0; i < uint32_t(Category::Count); ++i)
        {
            budget.reserve(Category(i), budget.limit(Category(i)) / 100 * percent);
        }
    };
    const auto empty = [&](uint64_t percent)
    {
        for (uint32_t i = 0; i < uint32_t(Category::Count); ++i)
        {
            budget.release(Category(i), budget.limit(Category(i)) / 100 * percent);
        }
    };
    expect(budget.pressure() == asr::MemoryPressure::None, "no pressure when empty");
    fill(90);
    expect(budget.pressure() == asr::MemoryPressure::LowerMips, "lowers mips at 90%");
    empty(90);
    fill(96);
    expect(budget.pressure() == asr::MemoryPressure::HoldLoads, "holds loads at 96%");
    empty(96);
    expect(budget.totalUsed() == 0, "budget is empty again");

    constexpr int THREADS = 4;
    constexpr int ROUNDS = 100000;
    const uint64_t limit = budget.limit(Category::Staging);
    std::atomic<bool> overLimit{ false };
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back([&]()
        {
            const uint64_t size = limit / 3;
            for (int i = 0; i < ROUNDS; ++i)
            {
                if (budget.reserve(Category::Staging, size))
                {
                    if (budget.used(Category::Staging) > limit)
                    {
                        overLimit.store(true);
                    }
                    budget.release(Category::Staging, size);
                }
            }
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    expect(!overLimit.load(), "threads racing for memory never pass the limit");
    expect(budget.used(Category::Staging) == 0 && budget.totalUsed() == 0, "threads racing leave the count exact");
}

asr::UIVertex uiVertex(float x, float y, uint32_t texture)
{
    asr::UIVertex vertex{};
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[3] = 1.f;
    vertex.color = 0xFFFFFFFF;
    vertex.texture = texture;
    return vertex;
}

void testUIList()
{
    using asr::UIGeometry;
    asr::UIList list;
    const asr::UIVertex quad[4] = { uiVertex(0, 0, 1), uiVertex(1, 0, 1), uiVertex(0, 1, 2), uiVertex(1, 1, 2) };

    list.append(UIGeometry::TriangleStrip, quad, 4);
    list.append(UIGeometry::TriangleStrip, quad, 4);
    expect(list.draws().size() == 1, "a texture change does not split a draw");
    expect(list.vertices().size() == 12, "a strip of 4 becomes 6 triangle vertices, twice");
    expect(list.vertices()[3].position[0] == 1.f && list.vertices()[5].position[0] == 1.f && list.vertices()[5].position[1] == 1.f,
           "strip triangles keep their vertices");

    list.clear();
    list.append(UIGeometry::TriangleFan, quad, 4);
    expect(list.vertices().size() == 6 && list.vertices()[3].position[1] == 0.f && list.vertices()[3].position[0] == 0.f,
           "fan triangles all start at the first vertex");

    list.clear();
    list.append(UIGeometry::LineLoop, quad, 3);
    expect(list.vertices().size() == 6 && list.draws()[0].primitive == asr::UIPrimitive::Lines, "a loop becomes closed lines");
    expect(list.vertices()[5].position[0] == 0.f && list.vertices()[5].position[1] == 0.f, "the loop ends on its first vertex");

    list.clear();
    list.append(UIGeometry::TriangleStrip, quad, 2);
    list.append(UIGeometry::Triangles, quad, 4);
    list.append(UIGeometry::Lines, quad, 3);
    expect(list.vertices().size() == 5, "incomplete primitives are dropped, as GL does");

    list.clear();
    expect(list.draws().empty() && list.vertices().empty(), "clear empties the list");
    asr::UIBlend additive;
    additive.colorDest = asr::UIBlendFactor::One;
    list.setBlend(asr::UIBlend());
    list.append(UIGeometry::Triangles, quad, 3);
    list.setBlend(additive);
    list.append(UIGeometry::Triangles, quad, 3);
    list.setBlend(asr::UIBlend());
    list.append(UIGeometry::Triangles, quad, 3);
    expect(list.draws().size() == 3, "a blend change splits a draw");
    expect(list.blends().size() == 2 && list.draws()[2].blend == 0, "a blend used twice is stored once");

    asr::UIScissor scissor;
    scissor.enabled = true;
    scissor.width = 10;
    scissor.height = 10;
    list.setScissor(scissor);
    list.append(UIGeometry::Triangles, quad, 3);
    list.append(UIGeometry::Triangles, quad, 3);
    expect(list.draws().size() == 4 && list.draws()[3].vertexCount == 6, "the same scissor keeps merging");

    // Doubles X, moves Y by 3.
    const float matrix[16] = { 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 3, 0, 1 };
    const float uvMatrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1 };
    const float positions[8] = { 1, 1, 0, 0, 2, 2, 0, 0 };
    const float uvs[4] = { 0.25f, 0.5f, 0.75f, 1.f };
    asr::UIStreams streams;
    streams.positions = positions;
    streams.uvs = uvs;
    streams.matrix = matrix;
    streams.singleColor = 0x11223344;
    streams.texture = asr::uiTextureBits(7, asr::UISampler::NearestWrap, true);
    list.clear();
    list.append(UIGeometry::Lines, streams, 2);
    const asr::UIVertex& second = list.vertices()[1];
    expect(second.position[0] == 4.f && second.position[1] == 5.f && second.position[3] == 1.f, "positions are transformed");
    expect(second.uv[0] == 0.75f && second.color == 0x11223344, "without a texture matrix the UV is kept");
    expect((second.texture & asr::UI_TEXTURE_SLOT_MASK) == 7 && (second.texture & asr::UI_SOLID_COLOR_BIT),
           "the texture bits are stored");
    streams.uvMatrix = uvMatrix;
    list.clear();
    list.append(UIGeometry::Lines, streams, 2);
    expect(list.vertices()[1].uv[0] == 1.25f, "the texture matrix is applied");

    asr::UIRect rect;
    asr::UIScissor test;
    expect(asr::resolveScissor(test, 100, 50, 100, 50, rect) && rect.width == 100 && rect.height == 50, "no scissor means the whole target");
    expect(asr::resolveScissor(test, 100, 50, 200, 40, rect) && rect.width == 100 && rect.height == 40,
           "no scissor covers what the list and the target share");
    test.enabled = true;
    test.x = 10;
    test.y = 5;
    test.width = 20;
    test.height = 15;
    expect(asr::resolveScissor(test, 100, 50, 100, 50, rect) && rect.x == 10 && rect.y == 30 && rect.width == 20 && rect.height == 15,
           "the scissor origin moves to the top left");
    expect(asr::resolveScissor(test, 100, 50, 300, 200, rect) && rect.x == 10 && rect.y == 30 && rect.width == 20 && rect.height == 15,
           "a window grown since the list was made leaves the scissor where it was");
    expect(asr::resolveScissor(test, 100, 50, 25, 40, rect) && rect.x == 10 && rect.y == 30 && rect.width == 15 && rect.height == 10,
           "a window shrunk since the list was made cuts the scissor");
    expect(!asr::resolveScissor(test, 100, 50, 25, 30, rect), "a scissor past a shrunk window draws nothing");
    test.x = -10;
    test.y = 40;
    test.width = 1000;
    test.height = 1000;
    expect(asr::resolveScissor(test, 100, 50, 100, 50, rect) && rect.x == 0 && rect.y == 0 && rect.width == 100 && rect.height == 10,
           "a scissor past the edges is clamped");
    test.x = 200;
    expect(!asr::resolveScissor(test, 100, 50, 100, 50, rect), "a scissor outside the target draws nothing");
}

void testTextureQueue()
{
    using Type = asr::TextureCommand::Type;
    asr::TextureQueue queue;
    std::vector<asr::TextureCommand> commands;

    queue.take(commands);
    expect(commands.size() == 2 && commands[0].slot == 0 && commands[0].type == Type::Create && commands[1].type == Type::Upload,
           "slot 0 starts as a queued white pixel");
    commands.clear();

    asr::TextureDesc desc;
    desc.width = 8;
    desc.height = 4;
    desc.mipLevels = 4;
    expect(asr::textureBytes(desc) == (32 + 8 + 2 + 1) * 4, "memory counts every mip");

    const uint8_t pixels[4] = { 1, 2, 3, 4 };
    const uint32_t first = queue.create(desc);
    const uint32_t second = queue.create(desc);
    expect(first == 1 && second == 2, "slots are handed out in order");
    queue.upload(first, 1, 2, 3, 1, 1, pixels, 4);
    queue.destroy(second, 9);
    queue.upload(0, 0, 0, 0, 1, 1, pixels, 4);
    queue.destroy(0, 9);
    queue.take(commands);
    expect(commands.size() == 4, "the white pixel cannot be changed or destroyed");
    expect(commands[2].type == Type::Upload && commands[2].level == 1 && commands[2].pixels[3] == 4, "upload keeps its own pixels");
    expect(commands[3].type == Type::Destroy && commands[3].lastList == 9, "destroy says which list may still use it");

    commands.clear();
    std::vector<uint8_t> owned = { 9, 8, 7, 6 };
    const uint8_t* const ownedData = owned.data();
    queue.upload(first, 0, 0, 0, 1, 1, std::move(owned));
    queue.generateMips(first);
    queue.generateMips(0);
    queue.take(commands);
    expect(commands.size() == 2 && commands[0].pixels.data() == ownedData, "pixels built for an upload are moved, not copied");
    expect(commands[1].type == Type::GenerateMips && commands[1].slot == first, "mips are queued after the pixels they come from");

    expect(asr::textureBytesPerPixel(asr::TextureFormat::BGRA8) == 4, "BGRA takes four bytes");

    expect(queue.create(desc) == 3, "a slot is not reused before it is released");
    queue.releaseSlot(second);
    expect(queue.create(desc) == second, "a released slot is reused");

    for (uint32_t i = 4; i < asr::MAX_TEXTURES; ++i)
    {
        queue.create(desc);
    }
    expect(queue.create(desc) == 0, "with no slot left, a texture falls back to white");

    asr::RetiredTextures retired;
    std::vector<uint32_t> destroyed;
    const auto run = [&](uint64_t list, uint64_t frame)
    {
        retired.run(list, frame, 2, [&](uint32_t slot) { destroyed.push_back(slot); });
    };
    retired.add(5, 10);
    run(10, 100);
    expect(destroyed.empty(), "a texture stays while the list that may use it is drawn");
    run(11, 101);
    run(11, 103);
    expect(destroyed.empty(), "it stays until frames using that list have finished");
    run(11, 104);
    expect(destroyed.size() == 1 && destroyed[0] == 5 && retired.size() == 0, "then it goes, once");
}

void testSceneGeometry()
{
    const asrtest::Mesh mesh = asrtest::makeSphere(40, 80);
    const uint32_t triangles = uint32_t(mesh.indices.size() / 3);
    const Clock::time_point start = Clock::now();
    const asr::GeometryData geometry = asrtest::build(mesh);
    printf("built %u triangles into %zu clusters in %.2f ms\n", triangles, geometry.clusters.size(), secondsSince(start) * 1000.0);

    uint32_t total = 0;
    uint32_t largest = 0;
    bool insideSphere = true;
    std::vector<uint8_t> seen(mesh.vertices.size(), 0);
    const uint16_t* indices = reinterpret_cast<const uint16_t*>(geometry.blob.data());
    const asr::PackedPosition* positions = reinterpret_cast<const asr::PackedPosition*>(geometry.blob.data() + geometry.positionsOffset);
    for (const asr::ClusterData& cluster : geometry.clusters)
    {
        total += cluster.triangles;
        largest = std::max(largest, cluster.triangles);
        const uint16_t* first = indices + cluster.indexOffset / 2;
        for (uint32_t i = 0; i < cluster.triangles * 3; ++i)
        {
            seen[first[i]] = 1;
            const asr::PackedPosition& p = positions[first[i]];
            const float dx = p.xyz[0] / 32767.f - cluster.center[0];
            const float dy = p.xyz[1] / 32767.f - cluster.center[1];
            const float dz = p.xyz[2] / 32767.f - cluster.center[2];
            insideSphere = insideSphere && std::sqrt(dx * dx + dy * dy + dz * dz) <= cluster.radius;
        }
    }
    expect(total == triangles, "clusters hold every triangle once");
    expect(largest <= asr::CLUSTER_MAX_TRIANGLES, "no cluster is over 64 triangles");
    expect(geometry.clusters.size() < size_t(triangles / asr::CLUSTER_MAX_TRIANGLES) * 3 / 2, "clusters are nearly full");
    expect(std::find(seen.begin(), seen.end(), uint8_t(0)) == seen.end(), "every vertex is used");
    expect(insideSphere, "bounding spheres hold their vertices");

    const asr::GeometryData empty = asr::buildGeometry(nullptr, 0, nullptr, 0);
    expect(empty.clusters.empty(), "an empty face builds to nothing");
}

void testRangeAllocator()
{
    asr::RangeAllocator ranges(100);
    uint64_t a = 0, b = 0, c = 0, d = 0;
    expect(ranges.allocate(30, 1, a) && ranges.allocate(30, 1, b) && ranges.allocate(30, 1, c), "ranges fit");
    expect(!ranges.allocate(30, 1, d), "a full span refuses");
    ranges.release(a, 30);
    ranges.release(b, 30);
    expect(ranges.allocate(50, 1, d) && d == 0, "freed neighbours merge");
    uint64_t aligned = 0;
    expect(ranges.allocate(4, 16, aligned) && aligned % 16 == 0, "alignment holds");
    ranges.grow(200);
    expect(ranges.allocate(100, 1, d), "a grown span has room");
}

class FakeChunks final : public asr::SceneChunks
{
public:
    uint64_t createSceneChunk() override
    {
        if (limit == 0)
        {
            return 0;
        }
        --limit;
        return next += asr::SCENE_CHUNK_BYTES;
    }

    uint32_t limit = 100;
    uint64_t next = 0;
};

void testSceneStore()
{
    asr::SceneQueue queue;
    asr::SceneStore store(2);
    FakeChunks chunks;
    constexpr uint64_t LIMIT = 1024 * 1024;

    const asrtest::Mesh mesh = asrtest::makeSphere(16, 32);

    asr::InstanceDesc desc;
    memset(desc.objectToWorld, 0, sizeof(desc.objectToWorld));
    desc.objectToWorld[0] = desc.objectToWorld[5] = desc.objectToWorld[10] = 2.f;
    desc.objectToWorld[3] = 7.f;

    const uint32_t geometry = queue.addGeometry(asrtest::build(mesh));
    desc.geometry = geometry;
    const uint32_t first = queue.addInstance(desc);
    const uint32_t second = queue.addInstance(desc);
    uint64_t frame = 10;
    store.update(queue, chunks, frame, LIMIT);
    expect(store.instanceEnd() == 2, "instances are counted");
    expect(store.triangles() == 2 * mesh.indices.size() / 3, "both instances count their triangles");

    bool sawChunkWrite = false;
    bool sawInstance = false;
    for (const asr::SceneWrite& write : store.writes())
    {
        sawChunkWrite = sawChunkWrite || (write.table == asr::SceneTable::Count && write.data != nullptr);
        if (write.table == asr::SceneTable::Instances && write.offset == sizeof(asr::GpuInstance) * second)
        {
            const asr::GpuInstance* instance = reinterpret_cast<const asr::GpuInstance*>(store.tableBytes().data() + write.source);
            sawInstance = instance->geometry == geometry && instance->rows[0][3] == 7.f;
        }
    }
    expect(sawChunkWrite, "geometry goes into a chunk");
    expect(sawInstance, "instances reach the table");

    // A flipped object is told apart, and a move writes one entry.
    desc.objectToWorld[0] = -2.f;
    queue.setInstance(first, desc);
    store.update(queue, chunks, ++frame, LIMIT);
    expect(store.writes().size() == 1, "a moved object writes one entry and no geometry");
    const asr::GpuInstance* moved = reinterpret_cast<const asr::GpuInstance*>(store.tableBytes().data());
    expect((moved->flags & asr::INSTANCE_FLIPPED) != 0, "a negative scale flips the winding");

    // Nothing is reused until frames in flight can no longer draw it.
    queue.removeInstance(first);
    queue.removeInstance(second);
    queue.removeGeometry(geometry);
    store.update(queue, chunks, ++frame, LIMIT);
    expect(store.triangles() == 0, "removed instances stop counting");
    const uint64_t freeBefore = store.freeChunkBytes();
    store.update(queue, chunks, ++frame, LIMIT);
    expect(store.freeChunkBytes() == freeBefore, "freed geometry waits for frames in flight");
    store.update(queue, chunks, frame += 4, LIMIT);
    expect(store.freeChunkBytes() > freeBefore, "freed geometry comes back");
    expect(queue.addGeometry(asrtest::build(mesh)) == geometry, "a freed id is reused");

    // Out of memory: geometry waits, instances can already name it, and nothing is lost.
    chunks.limit = 0;
    asr::SceneQueue tightQueue;
    asr::SceneStore tight(2);
    desc.geometry = tightQueue.addGeometry(asrtest::build(mesh));
    tightQueue.addInstance(desc);
    tight.update(tightQueue, chunks, 1, LIMIT);
    expect(tight.pendingGeometries() == 1 && tight.triangles() == 0, "no memory: the geometry waits");
    chunks.limit = 1;
    tight.update(tightQueue, chunks, 2, LIMIT);
    expect(tight.pendingGeometries() == 0 && tight.triangles() == mesh.indices.size() / 3, "it arrives when memory does");

    // Upload limit: a burst spreads over several frames.
    chunks.limit = 100;
    asr::SceneQueue burstQueue;
    asr::SceneStore burst(2);
    for (int i = 0; i < 40; ++i)
    {
        burstQueue.addGeometry(asrtest::build(mesh));
    }
    burst.update(burstQueue, chunks, 1, 1);
    expect(burst.pendingGeometries() == 39, "the upload limit holds the rest back");

    // The same for instances: they wait in order, and the last value wins.
    asr::SceneQueue manyQueue;
    asr::SceneStore many(2);
    constexpr uint32_t COUNT = 1000;
    for (uint32_t i = 0; i < COUNT; ++i)
    {
        manyQueue.addInstance(desc);
    }
    many.update(manyQueue, chunks, 1, COUNT * sizeof(asr::GpuInstance) / 2);
    expect(many.instanceEnd() == COUNT / 4, "a burst of instances spreads over frames");
    for (uint64_t next = 2; next < 8; ++next)
    {
        many.update(manyQueue, chunks, next, COUNT * sizeof(asr::GpuInstance) / 2);
    }
    expect(many.instanceEnd() == COUNT, "all of them arrive");

    // Every frame in flight can stage both kinds of upload inside the staging category.
    asr::MemoryBudget budget;
    constexpr uint64_t GIGABYTE = 1024ull * 1024 * 1024;
    for (uint64_t gigabytes : { 12, 8, 6, 4 })
    {
        budget.setBudget(gigabytes * GIGABYTE);
        expect(budget.uploadBytesPerFrame(3) * 3 * 2 <= budget.limit(asr::MemoryCategory::Staging), "uploads fit in the staging budget");
        expect(budget.uploadBytesPerFrame(3) >= 8 * 1024 * 1024, "uploads keep a working size");
    }
}

#ifdef ASR_HAS_DEVICE
void printStats(const char* name, const asr::FrameStats& stats, double seconds)
{
    printf("%s: %.0f fps, worst frame %.1f ms, oldest state %.1f ms\n", name, stats.frames / seconds,
           stats.worstFrameSeconds * 1000.0, stats.worstStateAgeSeconds * 1000.0);
}

void printMemory(asr::MemoryBudget& budget)
{
    printf("memory budget %.2f GB, used %.1f MB\n", budget.budget() / (1024.0 * 1024.0 * 1024.0),
           budget.totalUsed() / (1024.0 * 1024.0));
    for (uint32_t i = 0; i < uint32_t(asr::MemoryCategory::Count); ++i)
    {
        const asr::MemoryCategory category = asr::MemoryCategory(i);
        printf("  %-15s %8.1f MB of %8.1f MB\n", asr::memoryCategoryName(category),
               budget.used(category) / (1024.0 * 1024.0), budget.limit(category) / (1024.0 * 1024.0));
    }
}

// A UI with the habits of the real one: many small textured quads, text in an
// alpha-only texture, a clipped panel, and an additive overlay.
class TestUI
{
public:
    explicit TestUI(asr::TextureQueue& textures) : mTextures(textures)
    {
        constexpr uint32_t SIZE = 16;
        asr::TextureDesc desc;
        desc.width = SIZE;
        desc.height = SIZE;
        for (uint32_t i = 0; i < TEXTURES; ++i)
        {
            std::vector<uint32_t> pixels(SIZE * SIZE);
            const uint32_t tint = 0xFF000000 | ((i * 37 + 64) & 0xFF) | ((i * 91 + 64) & 0xFF) << 8 | ((i * 53 + 64) & 0xFF) << 16;
            for (uint32_t y = 0; y < SIZE; ++y)
            {
                for (uint32_t x = 0; x < SIZE; ++x)
                {
                    pixels[y * SIZE + x] = ((x / 4 + y / 4) & 1) ? tint : 0xFFFFFFFF;
                }
            }
            mSlots[i] = textures.create(desc);
            textures.upload(mSlots[i], 0, 0, 0, SIZE, SIZE, pixels.data(), pixels.size() * sizeof(uint32_t));
        }

        desc.format = asr::TextureFormat::R8;
        desc.swizzle = asr::TextureSwizzle::AlphaOnly;
        std::vector<uint8_t> glyph(SIZE * SIZE);
        for (uint32_t y = 0; y < SIZE; ++y)
        {
            for (uint32_t x = 0; x < SIZE; ++x)
            {
                glyph[y * SIZE + x] = (x == 8 || y == 8) ? 255 : 0;
            }
        }
        mGlyph = textures.create(desc);
        textures.upload(mGlyph, 0, 0, 0, SIZE, SIZE, glyph.data(), glyph.size());

        // Red in BGRA byte order, in one-pixel stripes with black. Drawn small with
        // working mips it looks evenly dark red, with broken mips it shimmers.
        constexpr uint32_t MIPPED_SIZE = 64;
        asr::TextureDesc mippedDesc;
        mippedDesc.width = MIPPED_SIZE;
        mippedDesc.height = MIPPED_SIZE;
        mippedDesc.mipLevels = 7;
        mippedDesc.format = asr::TextureFormat::BGRA8;
        std::vector<uint8_t> stripes(MIPPED_SIZE * MIPPED_SIZE * 4);
        for (uint32_t y = 0; y < MIPPED_SIZE; ++y)
        {
            for (uint32_t x = 0; x < MIPPED_SIZE; ++x)
            {
                uint8_t* pixel = &stripes[(y * MIPPED_SIZE + x) * 4];
                pixel[2] = (x & 1) ? 255 : 0;
                pixel[3] = 255;
            }
        }
        mMipped = textures.create(mippedDesc);
        textures.upload(mMipped, 0, 0, 0, MIPPED_SIZE, MIPPED_SIZE, std::move(stripes));
        textures.generateMips(mMipped);
    }

    void build(asr::UIList& list, uint32_t width, uint32_t height, float seconds, uint64_t listGeneration)
    {
        list.clear();
        list.setSize(width, height);
        mFlushes = 0;

        // Pixels to GL clip space, as the viewer's 2D setup does.
        const float matrix[16] = { 2.f / width, 0, 0, 0, 0, 2.f / height, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1 };

        const int columns = 120;
        const int rows = 60;
        const float size = 9.f;
        for (int row = 0; row < rows; ++row)
        {
            for (int column = 0; column < columns; ++column)
            {
                const float x = 20.f + column * 10.f + 4.f * sinf(seconds * 2.f + row * 0.3f);
                const float y = 20.f + row * 10.f;
                const uint32_t slot = mSlots[(row + column) % TEXTURES];
                quad(list, x, y, size, size, asr::uiTextureBits(slot), 0xFFFFFFFF, matrix);
            }
        }

        asr::UIScissor scissor;
        scissor.enabled = true;
        scissor.x = 100;
        scissor.y = 100;
        scissor.width = 300;
        scissor.height = 200;
        list.setScissor(scissor);
        for (int i = 0; i < 400; ++i)
        {
            const float x = 40.f + (i % 40) * 12.f + 60.f * sinf(seconds);
            const float y = 80.f + (i / 40) * 24.f;
            quad(list, x, y, 12.f, 16.f, asr::uiTextureBits(mGlyph, asr::UISampler::NearestClamp), 0xFF40F0FF, matrix);
        }
        list.setScissor(asr::UIScissor());

        asr::UIBlend additive;
        additive.colorDest = asr::UIBlendFactor::One;
        list.setBlend(additive);
        quad(list, 500.f, 300.f, 300.f, 150.f, asr::uiTextureBits(0), 0x60202040, matrix);
        list.setBlend(asr::UIBlend());
        quad(list, 40.f, 500.f, 300.f, 60.f, asr::uiTextureBits(mGlyph, asr::UISampler::LinearClamp, true), 0xFF3040F0, matrix);
        quad(list, 400.f, 500.f, 16.f + 8.f * sinf(seconds), 60.f, asr::uiTextureBits(mMipped), 0xFFFFFFFF, matrix);

        // A texture replaced now and then, so destruction runs under a live list.
        if (++mBuilds % 30 == 0)
        {
            const uint32_t index = (mBuilds / 30) % TEXTURES;
            mTextures.destroy(mSlots[index], listGeneration);
            asr::TextureDesc desc;
            desc.width = 16;
            desc.height = 16;
            mSlots[index] = mTextures.create(desc);
            const std::vector<uint32_t> pixels(16 * 16, 0xFF00C0FF);
            mTextures.upload(mSlots[index], 0, 0, 0, 16, 16, pixels.data(), pixels.size() * sizeof(uint32_t));
            ++mReplaced;
        }
    }

    uint32_t flushes() const { return mFlushes; } // the draws GL would have made
    uint32_t replaced() const { return mReplaced; }

private:
    static constexpr uint32_t TEXTURES = 64;

    void quad(asr::UIList& list, float x, float y, float width, float height, uint32_t texture, uint32_t color, const float* matrix)
    {
        const float positions[16] = { x, y, 0, 0, x + width, y, 0, 0, x, y + height, 0, 0, x + width, y + height, 0, 0 };
        const float uvs[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
        asr::UIStreams streams;
        streams.positions = positions;
        streams.uvs = uvs;
        streams.singleColor = color;
        streams.matrix = matrix;
        streams.texture = texture;
        list.append(asr::UIGeometry::TriangleStrip, streams, 4);
        ++mFlushes;
    }

    asr::TextureQueue& mTextures;
    uint32_t mSlots[TEXTURES]{};
    uint32_t mGlyph = 0;
    uint32_t mMipped = 0;
    uint32_t mFlushes = 0;
    uint32_t mBuilds = 0;
    uint32_t mReplaced = 0;
};

int drawTriangle(double seconds, double stallSeconds, bool drawUI, bool drawScene, bool keepSize, uint32_t windowWidth, uint32_t windowHeight,
                 const asr::DeviceOptions& options)
{
    const asr::ProbeResult probe = asr::Device::probe();
    printf("probe before any window: %s\n", probe.supported ? probe.info.name.c_str() : probe.reason.c_str());
    expect(probe.supported, "probe accepts this machine");

    asrtest::TestWindow window;
    if (!window.create(windowWidth, windowHeight, "asrenderer_test"))
    {
        printf("FAIL  cannot open a window\n");
        return 1;
    }

    std::string error;
    std::unique_ptr<asr::Device> device = asr::Device::create(window.surfaceTarget(), options, error);
    if (!device)
    {
        printf("FAIL  no device: %s\n", error.c_str());
        return 1;
    }
    printf("backend %s, device %s, memory %.1f GB\n", device->info().backend.c_str(), device->info().name.c_str(),
           device->info().memoryBudgetBytes / (1024.0 * 1024.0 * 1024.0));
    expect(!asrtest::glContextIsCurrent(), "no GL context after device creation");
    expect(probe.info.name == device->info().name, "probe and device agree on the GPU");
    if (options.memoryCapBytes != 0)
    {
        expect(device->memoryBudget().budget() <= options.memoryCapBytes, "memory cap lowers the budget");
    }

    asr::RenderThread renderThread(*device);
    std::unique_ptr<asrtest::TestCity> city;
    const Clock::time_point sceneStart = Clock::now();
    const auto publishWindowSize = [&]()
    {
        asr::FrameState& state = renderThread.state();
        window.pixelSize(state.width, state.height);
        state.testTriangle = !drawScene;
        if (drawScene)
        {
            state.hasCamera = true;
            state.camera = city->camera(float(secondsSince(sceneStart)), state.width, state.height);
        }
        renderThread.publishState();
    };
    if (drawScene)
    {
        city = std::make_unique<asrtest::TestCity>(device->scene(), device->textures());
        printf("city: %u objects, %u instances, built in %.0f ms\n", city->objects(), city->instances(), secondsSince(sceneStart) * 1000.0);
    }
    publishWindowSize();

    TestUI testUI(device->textures());
    const Clock::time_point uiStart = Clock::now();
    Clock::time_point lastUI = uiStart - std::chrono::seconds(1);
    double worstBuildSeconds = 0.0;
    uint32_t uiBuilds = 0;
    uint32_t lastDraws = 0;
    uint32_t lastVertices = 0;
    const auto buildUI = [&]()
    {
        // The capped UI rate of section 1 of the plan, here fixed at 60 Hz.
        if (!drawUI || secondsSince(lastUI) < 1.0 / 60.0)
        {
            return;
        }
        lastUI = Clock::now();
        uint32_t width = 0;
        uint32_t height = 0;
        window.pixelSize(width, height);
        // Lists larger and smaller than the window in turn, as during a resize that
        // the main thread has not caught up with.
        const uint32_t phase = (uiBuilds / 60) % 3;
        width = phase == 1 ? width + 200 : phase == 2 ? width / 2 : width;
        height = phase == 1 ? height + 100 : phase == 2 ? height / 2 : height;
        asr::UIList& list = renderThread.ui();
        testUI.build(list, width, height, float(secondsSince(uiStart)), renderThread.currentUIGeneration());
        lastDraws = uint32_t(list.draws().size());
        lastVertices = uint32_t(list.vertices().size());
        renderThread.publishUI();
        worstBuildSeconds = std::max(worstBuildSeconds, secondsSince(lastUI));
        ++uiBuilds;
    };
    buildUI();
    renderThread.start();

    // The first frame takes the whole scene at once, and is not what is being measured.
    const Clock::time_point firstFrameStart = Clock::now();
    while (drawScene && device->sceneStats().pendingGeometries + device->sceneStats().instances == 0 && secondsSince(firstFrameStart) < 60.0)
    {
        window.pumpEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Runs the main thread normally for a while. False if the test must stop.
    const auto runMainThread = [&](double duration)
    {
        const Clock::time_point start = Clock::now();
        while (secondsSince(start) < duration)
        {
            if (!window.pumpEvents())
            {
                return false;
            }
            if (renderThread.failed())
            {
                printf("FAIL  render thread: %s\n", renderThread.error().c_str());
                return false;
            }
            publishWindowSize();
            buildUI();
            if (city)
            {
                city->update(float(secondsSince(sceneStart)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    };

    constexpr double WARM_UP_SECONDS = 1.0;
    const double normalSeconds = std::max(stallSeconds, 1.0);
    const double restSeconds = std::max(seconds - WARM_UP_SECONDS - normalSeconds - stallSeconds, 1.0);

    bool ran = runMainThread(WARM_UP_SECONDS);
    renderThread.takeStats();

    ran = ran && runMainThread(normalSeconds);
    const asr::FrameStats normal = renderThread.takeStats();
    printStats("main thread running", normal, normalSeconds);

    if (ran && stallSeconds > 0.0)
    {
        std::this_thread::sleep_for(std::chrono::duration<double>(stallSeconds));
        const asr::FrameStats stalled = renderThread.takeStats();
        printStats("main thread stalled", stalled, stallSeconds);

        const double normalFps = normal.frames / normalSeconds;
        const double stalledFps = stalled.frames / stallSeconds;
        expect(stalled.worstStateAgeSeconds >= stallSeconds * 0.9, "render thread really ran on old state");
        expect(stalledFps >= normalFps * 0.8, "render rate holds during the stall");
        expect(stalled.worstFrameSeconds < 0.1, "no frame took 100 ms during the stall");
    }

    // Proves resizing while drawing. A speed reading passes --keep-size, so it stays at one size.
    if (!keepSize)
    {
        window.resize(900, 600);
    }
    ran = ran && runMainThread(restSeconds);

    if (drawUI)
    {
        printf("UI: %u lists built, worst %.2f ms, %u vertices, %u flushes merged into %u draws, %u textures replaced\n",
               uiBuilds, worstBuildSeconds * 1000.0, lastVertices, testUI.flushes(), lastDraws, testUI.replaced());
        expect(lastDraws <= 4, "UI flushes merge into a few draws");
        expect(testUI.replaced() > 0, "textures were replaced while lists were drawing");
    }

    if (city)
    {
        const asr::SceneStats stats = device->sceneStats();
        printf("scene: %u instances, %.1f M triangles, %llu visible clusters, %u chunks, %llu geometries waiting\n", stats.instances,
               stats.triangles / 1e6, (unsigned long long)stats.visibleClusters, stats.chunks, (unsigned long long)stats.pendingGeometries);
        printf("GPU: cull %.2f ms, visibility %.2f ms, resolve %.2f ms\n", stats.cullMilliseconds, stats.visibilityMilliseconds, stats.resolveMilliseconds);
        printf("moved %u objects, replaced %u geometries\n", city->moved(), city->replaced());
        expect(stats.visibleClusters > 0, "the camera sees part of the scene");
        expect(stats.pendingGeometries == 0, "all geometry reached the GPU");
        expect(city->replaced() > 0, "geometry was replaced while drawing");
    }

    printMemory(device->memoryBudget());
    const bool countsSwapchain = device->memoryBudget().used(asr::MemoryCategory::RenderTargets) > 0;
    renderThread.stop();

    expect(ran, "ran to the end");
    expect(countsSwapchain, "budget counts the window's images");
    expect(!asrtest::glContextIsCurrent(), "no GL context after rendering");
    expect(device->validationErrors() == 0, "no validation errors");
    device.reset();
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    bool selftest = false;
    double seconds = 10.0;
#ifdef ASR_HAS_DEVICE
    double stallSeconds = 2.0;
    bool drawUI = false;
    bool drawScene = false;
    bool keepSize = false;
    uint32_t windowWidth = 1280;
    uint32_t windowHeight = 720;
    asr::DeviceOptions options;
    options.shaderDir = ASR_SHADER_DIR;
#endif
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--selftest")
        {
            selftest = true;
        }
        else if (arg == "--seconds" && i + 1 < argc)
        {
            seconds = atof(argv[++i]);
        }
#ifdef ASR_HAS_DEVICE
        else if (arg == "--stall" && i + 1 < argc)
        {
            stallSeconds = atof(argv[++i]);
        }
        else if (arg == "--ui")
        {
            drawUI = true;
        }
        else if (arg == "--scene")
        {
            drawScene = true;
        }
        else if (arg == "--keep-size")
        {
            keepSize = true;
        }
        else if (arg == "--size" && i + 2 < argc)
        {
            windowWidth = uint32_t(atoi(argv[++i]));
            windowHeight = uint32_t(atoi(argv[++i]));
        }
        else if (arg == "--memory-cap-mb" && i + 1 < argc)
        {
            options.memoryCapBytes = uint64_t(atof(argv[++i])) * 1024 * 1024;
        }
        else if (arg == "--validate")
        {
            options.validation = true;
        }
        else if (arg == "--vsync")
        {
            options.vsync = true;
        }
        else if (arg == "--shaders" && i + 1 < argc)
        {
            options.shaderDir = argv[++i];
        }
#endif
        else
        {
            printf("unknown option %s\n", arg.c_str());
            return 2;
        }
    }

    if (selftest)
    {
        testSceneFile("asrenderer_test_scene.tmp");
        testLatestValue();
        testMemoryBudget();
        testUIList();
        testTextureQueue();
        testSceneGeometry();
        testRangeAllocator();
        testSceneStore();
    }
    else
    {
#ifdef ASR_HAS_DEVICE
        if (drawTriangle(seconds, stallSeconds, drawUI, drawScene, keepSize, windowWidth, windowHeight, options) != 0)
        {
            return 1;
        }
#else
        printf("no window code on this platform yet: use --selftest\n");
        return 2;
#endif
    }

    printf("%s\n", sFailures ? "FAILED" : "ALL PASSED");
    return sFailures ? 1 : 0;
}
