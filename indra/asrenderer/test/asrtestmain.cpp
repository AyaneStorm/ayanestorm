/**
 * asrtestmain.cpp
 * asrenderer_test: renderer checks that need no viewer build and no login.
 *
 *   asrenderer_test --selftest           portable checks, no window
 *   asrenderer_test [--seconds N]        draw the test triangle for N seconds (default 10)
 *                   [--ui]               also draw a busy test UI, rebuilt on the main thread
 *                   [--scene]            draw a city of 16,000 objects instead of the triangle
 *                   [--street]           walk through it at street level
 *                   [--audit]            count pixels occlusion culling should have kept
 *                   [--no-occlusion]     draw what is hidden too, to measure what culling saves
 *                   [--stall N]          freeze the main thread for N seconds (default 2, 0 for none)
 *                   [--size W H]         window size (default 1280 720)
 *                   [--keep-size]        skip the resize during the run, for speed readings
 *                   [--memory-cap-mb N]  lower the video memory budget
 *                   [--validate]         turn on the Vulkan validation layers
 *                   [--vsync]            wait for the display
 *                   [--shaders DIR]      compiled shader folder
 *   asrenderer_test --vram-stress        real VRAM overcommit, mip retirement and recovery
 *   asrenderer_test --vram-managed       same asset arrivals with bounded first-image mips
 *   asrenderer_test --avatar-sweep        crowd camera turn with and without VRAM pressure
 *   asrenderer_test --mouse-probe [--seconds N]  Mac: does mouse motion arrive while the main thread reads no events
 *
 * Exit code 0 means every check passed.
 */
#include "asrlatestvalue.h"
#include "asrmemorybudget.h"
#include "asrframegraph.h"
#include "asrscene.h"
#include "asrscenefile.h"
#include "asrscenegraph.h"
#include "asrscenestore.h"
#include "asrjointpose.h"
#include "asrskinweights.h"
#include "asrmeshlodresidency.h"
#include "asravatarresidency.h"
#include "asrtexturestreaming.h"
#include "asrtexturequeue.h"
#include "asruilist.h"

#ifdef ASR_HAS_DEVICE
#include "asrdevice.h"
#include "asrrenderthread.h"
#include "asrtestwindow.h"
#include "asrtestvram.h"
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
bool sOverlays = false; // --overlay

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
    constexpr uint64_t MB = 1024ull * 1024;
    static_assert(asr::workingVRAMBudget(10 * GB, 0, 0) == 9 * GB, "reserve space for the desktop and other apps");
    static_assert(asr::workingVRAMBudget(5 * GB, 4 * GB, 4 * GB) == 5 * GB - 512 * MB, "follow a lower driver allowance");
    static_assert(asr::workingVRAMBudget(10 * GB, 6 * GB, 5 * GB, 5 * GB) == 4 * GB, "account for driver overhead inside the user cap");
    static_assert(asr::workingVRAMBudget(5 * GB, 0, 0, 9 * GB) == 5 * GB - 512 * MB, "a higher user cap cannot consume headroom");
    static_assert(asr::workingVRAMBudget(256 * MB, 0, 0) == 128 * MB, "bound the reserve on small budgets");
    static_assert(asr::workingVRAMBudget(GB, 2 * GB, 0) == 0, "overhead cannot underflow the ceiling");

    // Crowd detail ranking: everyone full while memory allows, nearest first when it does not.
    {
        using Policy = asr::AvatarDetailPolicy;
        Policy detail;
        detail.update(30, 60, 100, 150, 0.0, false);
        expect(detail.fullDetail() == 150 && detail.lodCap(149) == Policy::FINEST, "a large card keeps every avatar at full detail");
        double now = 0.0;
        for (int step = 0; step < 40 && detail.fullDetail() > Policy::MIN_FULL; ++step) detail.update(60, 95, 100, 150, now += Policy::STEP_SECONDS, false);
        expect(detail.fullDetail() == Policy::MIN_FULL, "memory pressure lowers the full-detail count to its floor");
        expect(detail.lodCap(0) == Policy::FINEST && detail.lodCap(Policy::MIN_FULL) == Policy::FINEST - 1, "the nearest keep full detail, the next step down");
        expect(detail.lodCap(149) == Policy::COARSEST_CAP, "the farthest stop at the coarsest cap, never the often broken lowest LOD");
        detail.update(20, 60, 100, 150, now += 1.0, false);
        expect(detail.fullDetail() == Policy::MIN_FULL, "no step before the previous one had time to free memory");
        detail.update(20, 95, 100, 150, now += Policy::STEP_SECONDS, false);
        expect(detail.fullDetail() == Policy::MIN_FULL, "no detail returns while the total is near the budget");
        detail.update(20, 60, 100, 150, now += Policy::STEP_SECONDS, false);
        expect(detail.fullDetail() > Policy::MIN_FULL, "detail returns as memory frees");

        // A low mesh share, but past the budget with textures at their floor: mesh gives way.
        Policy crowd;
        crowd.update(40, 101, 100, 90, 0.0, false);
        expect(crowd.fullDetail() == 90, "under the high share and not overcommitted, mesh keeps full detail");
        crowd.update(40, 101, 100, 90, Policy::STEP_SECONDS, true);
        expect(crowd.fullDetail() < 90, "overcommitted, mesh gives way whatever its share");
    }

    asr::MemoryBudget budget;
    expect(!budget.reserve(Category::Textures, 1), "nothing fits before a budget is set");

    // Textures take what is really free, and hand it back as the others grow.
    {
        asr::MemoryBudget shared;
        shared.setBudget(8 * GB);
        const uint64_t share = shared.limit(Category::Textures);
        const uint64_t idle = shared.textureLimit();
        expect(idle > share && idle == 8 * GB / 100 * asr::TEXTURE_STEERING_PERCENT, "textures steer below the working budget, leaving room for the screen and replacements");
        shared.reserveAnyway(Category::Textures, 3 * GB);
        expect(shared.textureLimit() == idle, "their own use does not lower it");
        shared.reserveAnyway(Category::RenderTargets, GB);
        expect(shared.textureLimit() == idle - GB, "what others use comes off");
        shared.reserveAnyway(Category::Shadows, 7 * GB);
        expect(shared.textureLimit() == 0, "other categories can consume the remaining allowance");
        expect(!shared.reserve(Category::UI, 1), "a category with room cannot exceed the working budget");
    }

    {
        asr::MemoryBudget shared;
        shared.setBudget(asr::workingVRAMBudget(10 * GB, 0, 0, 7 * GB));
        expect(shared.reserve(Category::Geometry, 3 * GB), "geometry fits under the configured cap");
        shared.setBudget(asr::workingVRAMBudget(3 * GB, 3 * GB, 3 * GB, 7 * GB));
        expect(shared.textureLimit() == 0 && !shared.reserve(Category::UI, 1), "a falling driver budget stops new reservations");
        expect(shared.pressure() == asr::MemoryPressure::HoldLoads, "existing allocations trigger reclamation after a budget drop");
        shared.release(Category::Geometry, 3 * GB);
        expect(shared.reserve(Category::UI, 1), "admission resumes once memory is released");
    }

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
    budget.reserveAnyway(Category::UI, 1024);
    expect(budget.used(Category::UI) == budget.limit(Category::UI) + 1024, "memory that must exist is counted past the limit");
    expect(!budget.reserve(Category::UI, 1), "past the limit nothing else fits");
    budget.release(Category::UI, 1024);
    budget.release(Category::UI, budget.limit(Category::UI));

    // Geometry borrows past its share what is free, never the textures' share.
    const uint64_t borrowable = budget.budget() - budget.limit(Category::Textures);
    expect(budget.reserve(Category::Geometry, borrowable), "geometry borrows past its share");
    expect(!budget.reserve(Category::Geometry, 1), "geometry never borrows the textures' share");
    budget.release(Category::Geometry, borrowable);
    const uint64_t textures = budget.limit(Category::Textures) + 1024 * 1024; // textures past their share
    budget.reserveAnyway(Category::Textures, textures);
    expect(!budget.reserve(Category::Geometry, borrowable), "geometry borrows only what is free");
    expect(budget.reserve(Category::Geometry, borrowable - 1024 * 1024), "geometry borrows all that is free");
    budget.release(Category::Geometry, borrowable - 1024 * 1024);
    budget.release(Category::Textures, textures);
    expect(budget.totalUsed() == 0, "borrowing leaves nothing behind");

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

    // Depth: GL's compares reversed; world vertices end at nearClip / w, the rest at one
    // less GL's depth; asrUIV.glsl makes depth (z + w) / 2.
    list.clear();
    list.setSize(100, 100);
    asr::UIVertex deep[3] = { quad[0], quad[1], quad[2] };
    for (asr::UIVertex& v : deep)
    {
        v.position[2] = 0.5f;
        v.position[3] = 2.f;
    }
    list.beginWorld(0.25f);
    list.setDepthTest(true);
    list.setDepthCompare(asr::UICompare::LessOrEqual);
    list.append(UIGeometry::Triangles, deep, 3);
    expect(list.draws()[0].depth.test && list.draws()[0].depth.write && list.draws()[0].depth.compare == asr::UICompare::GreaterOrEqual,
           "GL's LEQUAL becomes GEQUAL in reversed depth, writing");
    expect(std::fabs((list.vertices()[0].position[2] + 2.f) * 0.5f / 2.f - 0.125f) < 1e-6f, "a world vertex lands at nearClip / w");
    list.markHud();
    list.append(UIGeometry::Triangles, deep, 3);
    expect(list.hudDraw() == 1 && list.draws().size() == 2 && list.worldDepthTested(), "the HUD mark splits the list");
    expect(std::fabs((list.vertices()[3].position[2] + 2.f) * 0.5f / 2.f - 0.375f) < 1e-6f, "after the HUD, depth is one less GL's");
    list.clearDepth();
    list.setDepthTest(false);
    list.setViewport(50, 0, 50, 100);
    list.append(UIGeometry::Triangles, deep, 3);
    expect(list.draws()[2].clearDepth && !list.draws()[3].depth.test, "a depth clear is its own draw");
    expect(std::fabs(list.vertices()[6].position[0] / 2.f - 0.5f) < 1e-6f, "the viewport folds into clip x");
    expect(list.draws()[3].scissor.enabled && list.draws()[3].scissor.x == 50 && list.draws()[3].scissor.width == 50 &&
           list.draws()[3].scissor.height == 100, "the viewport clips, as a scissor");
    list.clear();
    expect(list.hudDraw() == 0 && !list.worldDepthTested(), "clear forgets the HUD mark");

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
    expect(commands.size() == 1 && commands[0].slot == 0 && commands[0].type == Type::Create, "slot 0 is created at once");
    expect(queue.waitingBytes() == 4, "its white pixel waits for the ring");
    commands.clear();

    std::vector<uint8_t> ring(256);
    queue.setUploadRing(ring.data(), ring.size(), 64);
    queue.take(commands);
    expect(commands.size() == 1 && commands[0].type == Type::Upload && commands[0].ringOffset == 0 && commands[0].ringBytes == 4 &&
               commands[0].ringEnd == 4 && ring[3] == 0xFF && queue.waitingBytes() == 0,
           "the white pixel goes into the ring once there is one");
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
    expect(commands[2].type == Type::Upload && commands[2].level == 1 && commands[2].x == 2 && commands[2].y == 3 &&
               commands[2].ringOffset == asr::UPLOAD_RING_ALIGNMENT && ring[asr::UPLOAD_RING_ALIGNMENT + 3] == 4,
           "an upload's pixels are written into the ring, aligned");
    expect(commands[3].type == Type::Destroy && commands[3].lastList == 9, "destroy says which list may still use it");

    commands.clear();
    queue.upload(first, 0, 0, 0, 1, 1, pixels, 4);
    queue.generateMips(first);
    queue.generateMips(0);
    expect(queue.beginUpload(0, 0, 0, 0, 1, 1, 4) == nullptr, "slot 0 gives nowhere to write");
    queue.finishUpload();
    queue.take(commands);
    expect(commands.size() == 2 && commands[1].type == Type::GenerateMips && commands[1].slot == first, "mips are queued after the pixels they come from");

    commands.clear();
    asr::TextureDesc larger = desc;
    larger.width = 16;
    queue.replace(first, larger);
    queue.upload(first, 0, 0, 0, 1, 1, pixels, 4);
    queue.publish(first);
    queue.replace(first, desc, 2);
    queue.publish(first);
    queue.replace(0, desc);
    queue.publish(0);
    queue.take(commands);
    expect(commands.size() == 5 && commands[0].type == Type::Create && commands[0].replace && commands[0].slot == first &&
               commands[0].desc.width == 16 && commands[1].type == Type::Upload && commands[2].type == Type::Publish &&
               commands[2].slot == first,
           "a replacement keeps its slot, takes the pixels sent before publish, and white cannot be replaced");
    expect(commands[3].replace && commands[3].sourceSlot == first && commands[3].sourceMip == 2 && commands[4].type == Type::Publish,
           "a smaller replacement is made from the slot's own mips");

    // A ring of 96 bytes, pieces of 32: an upload of 8 rows of 16 bytes cannot fit at
    // once, so it goes in two rows at a time as the GPU frees room, with what follows it.
    {
        asr::TextureQueue small;
        std::vector<uint8_t> memory(96);
        small.setUploadRing(memory.data(), memory.size(), 32);
        std::vector<asr::TextureCommand> taken;
        small.take(taken);
        small.releaseRing(taken.back().ringEnd);
        taken.clear();

        asr::TextureDesc rowsDesc;
        rowsDesc.width = 16;
        rowsDesc.height = 8;
        rowsDesc.format = asr::TextureFormat::R8;
        const uint32_t tall = small.create(rowsDesc);
        std::vector<uint8_t> rows(16 * 8);
        for (size_t i = 0; i < rows.size(); ++i)
        {
            rows[i] = uint8_t(i / 16);
        }
        small.upload(tall, 0, 0, 0, 16, 8, rows.data(), rows.size());
        small.generateMips(tall);
        small.take(taken);
        expect(taken.size() == 3 && taken[1].type == Type::Upload && taken[1].y == 0 && taken[1].height == 2 && taken[1].ringOffset == 16 &&
                   taken[2].y == 2 && taken[2].ringOffset == 48 && taken[2].ringEnd == 80,
               "an upload goes in as pieces while there is room");
        expect(small.waitingBytes() == 64, "the rest waits, and what follows it too");
        taken.clear();

        small.flush();
        small.take(taken);
        expect(taken.empty(), "nothing more goes in before the GPU frees room");

        small.releaseRing(80);
        small.flush();
        small.take(taken);
        expect(taken.size() == 3 && taken[0].y == 4 && taken[0].ringOffset == 0 && taken[0].ringEnd == 128 && memory[0] == 4 &&
                   taken[1].y == 6 && taken[1].ringOffset == 32 && memory[32] == 6 && taken[2].type == Type::GenerateMips,
               "a piece that would wrap starts the ring again, and the mips still come after the pixels");
        expect(small.waitingBytes() == 0, "nothing waits once it has all gone in");
        small.releaseRing(40);
        small.releaseRing(160);
        small.releaseRing(100); // an older end never moves the tail back
        uint8_t* const direct = small.beginUpload(tall, 0, 0, 0, 16, 2, 16);
        expect(direct == memory.data() + 64, "an upload with room goes straight into the ring, where the last one ended");
        small.finishUpload();
    }

    expect(asr::textureCommandSeconds(0.0) == 0.001 && asr::textureCommandSeconds(1.0) == 0.004 &&
               std::abs(asr::textureCommandSeconds(0.025) - 0.0025) < 1e-9,
           "texture commands take a tenth of the frame, from 1 to 4 ms");

    expect(asr::textureBytesPerPixel(asr::TextureFormat::BGRA8) == 4, "BGRA takes four bytes");

    {
        asr::MemoryBudget textureBudget;
        asr::TextureQueue counted(&textureBudget);
        std::vector<uint8_t> countedRing(64);
        counted.setUploadRing(countedRing.data(), countedRing.size(), 64);
        asr::TextureDesc shrunkDesc;
        shrunkDesc.width = 2;
        shrunkDesc.height = 1;
        shrunkDesc.mipLevels = 2;
        const uint64_t white = textureBudget.used(asr::MemoryCategory::Textures);
        const uint32_t original = counted.create(desc);
        const uint32_t shrunk = counted.create(shrunkDesc, original, 2);
        expect(textureBudget.used(asr::MemoryCategory::Textures) == white + asr::textureBytes(desc) + asr::textureBytes(shrunkDesc),
               "a texture is counted as soon as it is asked for, with no budget set");
        std::vector<asr::TextureCommand> created;
        counted.take(created);
        expect(created.back().slot == shrunk && created.back().sourceSlot == original && created.back().sourceMip == 2, "a smaller texture says where its mips come from");
        expect(created[created.size() - 2].sourceSlot == 0, "a normal texture starts cleared");
    }

    expect(queue.create(desc) == 3, "a slot is not reused before it is released");
    queue.releaseSlot(second);
    expect(queue.create(desc) == second, "a released slot is reused");
    expect(queue.freeSlots() == asr::MAX_TEXTURES - 4, "free slots count what was never used");

    for (uint32_t i = 4; i < asr::MAX_TEXTURES; ++i)
    {
        queue.create(desc);
    }
    expect(queue.freeSlots() == 0, "every slot is taken");
    expect(queue.create(desc) == 0, "with no slot left, a texture falls back to white");
    queue.releaseSlot(second);
    expect(queue.freeSlots() == 1, "free slots count what was given back");
    expect(queue.create(desc) == second, "and that many creates succeed");

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

    const asr::GeometryData small = asrtest::build(asrtest::makeSphere(3, 4));
    expect(small.clusters.size() <= asr::CLUSTERS_WITHOUT_GROUPS && small.groups.empty(), "a small face has no groups");

    const asr::GeometryData empty = asr::buildGeometry(nullptr, 0, nullptr, 0);
    expect(empty.clusters.empty(), "an empty face builds to nothing");
}

// Every LOD of a ground patch keeps all points of its edges, so two patches side by side meet
// exactly whichever LODs they show, and its triangles cover the square once, all facing up.
void testTerrainLods()
{
    constexpr uint32_t SIZE = 16;
    constexpr uint32_t POINTS = SIZE + 1;
    std::vector<float> heights(POINTS * POINTS);
    std::vector<float> normals(POINTS * POINTS * 3);
    std::vector<float> blends(POINTS * POINTS * 2);
    for (uint32_t y = 0; y < POINTS; ++y)
    {
        for (uint32_t x = 0; x < POINTS; ++x)
        {
            const uint32_t at = y * POINTS + x;
            heights[at] = 20.f + 3.f * std::sin(x * 0.7f) * std::cos(y * 0.4f);
            normals[at * 3 + 2] = 1.f;
            blends[at * 2] = float(x) / SIZE * 3.f;
            blends[at * 2 + 1] = float(y) / SIZE;
        }
    }
    asr::TerrainGrid grid;
    grid.size = SIZE;
    grid.origin[0] = 32.f;
    grid.origin[1] = 48.f;
    grid.spacing = 1.f;
    grid.heights = heights.data();
    grid.normals = normals.data();
    grid.blends = blends.data();

    bool edgesWhole = true;
    bool covered = true;
    bool facingUp = true;
    bool fewer = true;
    size_t previousTriangles = 0;
    std::vector<asr::SceneVertex> vertices;
    std::vector<uint16_t> indices;
    for (uint32_t step = 1; step <= 8; step *= 2)
    {
        asr::buildTerrainLod(grid, step, vertices, indices);
        uint32_t edgePoints = 0;
        for (const asr::SceneVertex& v : vertices)
        {
            const float x = v.position[0] - grid.origin[0];
            const float y = v.position[1] - grid.origin[1];
            if (x == 0.f || y == 0.f || x == float(SIZE) || y == float(SIZE))
            {
                ++edgePoints;
                const uint32_t at = uint32_t(y) * POINTS + uint32_t(x);
                edgesWhole = edgesWhole && v.position[2] == heights[at] && v.uv[0] == blends[at * 2];
            }
        }
        edgesWhole = edgesWhole && edgePoints == SIZE * 4;

        double area = 0.0;
        for (size_t i = 0; i < indices.size(); i += 3)
        {
            const float* a = vertices[indices[i]].position;
            const float* b = vertices[indices[i + 1]].position;
            const float* c = vertices[indices[i + 2]].position;
            const double up = double(b[0] - a[0]) * (c[1] - a[1]) - double(b[1] - a[1]) * (c[0] - a[0]);
            facingUp = facingUp && up > 0.0;
            area += up * 0.5;
        }
        covered = covered && std::fabs(area - double(SIZE * SIZE)) < 1e-3;
        fewer = fewer && (step == 1 || indices.size() / 3 < previousTriangles);
        previousTriangles = indices.size() / 3;
    }
    expect(edgesWhole, "every terrain LOD keeps each edge point, at its exact height");
    expect(covered, "every terrain LOD covers its square once");
    expect(facingUp, "terrain triangles face up");
    expect(fewer, "coarser terrain LODs have fewer triangles");
}

void testClusterGroups()
{
    // Dense enough that each group covers a small patch of the sphere, so its normals agree.
    const asr::GeometryData geometry = asrtest::build(asrtest::makeSphere(120, 240));
    const uint16_t* indices = reinterpret_cast<const uint16_t*>(geometry.blob.data());
    const asr::PackedPosition* positions = reinterpret_cast<const asr::PackedPosition*>(geometry.blob.data() + geometry.positionsOffset);
    printf("%zu clusters in %zu groups\n", geometry.clusters.size(), geometry.groups.size());

    // Every cluster once, in order, with spheres around their clusters'.
    uint32_t nextCluster = 0;
    bool groupsFit = !geometry.groups.empty();
    bool groupSpheresHold = true;
    for (const asr::ClusterGroupData& group : geometry.groups)
    {
        groupsFit = groupsFit && group.firstCluster == nextCluster && group.clusterCount >= 1 && group.clusterCount <= asr::CLUSTER_GROUP_SIZE;
        nextCluster += group.clusterCount;
        for (uint32_t i = group.firstCluster; i < group.firstCluster + group.clusterCount && i < geometry.clusters.size(); ++i)
        {
            const asr::ClusterData& cluster = geometry.clusters[i];
            const float dx = cluster.center[0] - group.center[0];
            const float dy = cluster.center[1] - group.center[1];
            const float dz = cluster.center[2] - group.center[2];
            groupSpheresHold = groupSpheresHold && std::sqrt(dx * dx + dy * dy + dz * dz) + cluster.radius <= group.radius * 1.0001f;
        }
    }
    expect(groupsFit && nextCluster == geometry.clusters.size(), "groups hold every cluster once, in order, 32 at most");
    expect(groupSpheresHold, "a group's sphere holds its clusters' spheres");

    // The GPU's test of a group's normal cone, from the shader. It may only drop a group
    // whose every triangle faces away.
    const auto facesAway = [](uint32_t cone, const float* center, float radius, const float* camera)
    {
        const float cutoff = float(int8_t(cone >> 24)) / 127.f;
        if (cutoff >= 1.f)
        {
            return false;
        }
        const float axis[3] = { float(int8_t(cone)) / 127.f, float(int8_t(cone >> 8)) / 127.f, float(int8_t(cone >> 16)) / 127.f };
        const float to[3] = { center[0] - camera[0], center[1] - camera[1], center[2] - camera[2] };
        const float distance = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        return to[0] * axis[0] + to[1] * axis[1] + to[2] * axis[2] >= cutoff * distance + radius;
    };
    uint32_t dropped = 0;
    bool dropsAreRight = true;
    uint32_t seed = 12345;
    const auto random = [&seed]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1u << 24) * 2.f - 1.f; };
    for (int view = 0; view < 500; ++view)
    {
        float camera[3] = { random(), random(), random() };
        const float length = std::sqrt(camera[0] * camera[0] + camera[1] * camera[1] + camera[2] * camera[2]) + 1e-6f;
        const float distance = 1.2f + 2.f * (random() * 0.5f + 0.5f);
        for (float& value : camera)
        {
            value = value / length * distance;
        }
        for (const asr::ClusterGroupData& group : geometry.groups)
        {
            if (!facesAway(group.cone, group.center, group.radius, camera))
            {
                continue;
            }
            ++dropped;
            for (uint32_t i = group.firstCluster; i < group.firstCluster + group.clusterCount; ++i)
            {
                const uint16_t* first = indices + geometry.clusters[i].indexOffset / 2;
                for (uint32_t t = 0; t < geometry.clusters[i].triangles; ++t)
                {
                    float p[3][3];
                    for (int corner = 0; corner < 3; ++corner)
                    {
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            p[corner][axis] = positions[first[t * 3 + corner]].xyz[axis] / 32767.f;
                        }
                    }
                    const float e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
                    const float e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
                    const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                    const float toCamera[3] = { camera[0] - p[0][0], camera[1] - p[0][1], camera[2] - p[0][2] };
                    const float nLength = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    const float facing = (n[0] * toCamera[0] + n[1] * toCamera[1] + n[2] * toCamera[2]) / (nLength + 1e-12f);
                    dropsAreRight = dropsAreRight && facing <= 1e-3f;
                }
            }
        }
    }
    expect(dropped > 0, "groups facing away are dropped");
    expect(dropsAreRight, "a dropped group has no triangle facing the camera");
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

    // What a teleport does: many small ranges freed apart, then everything merging back.
    constexpr uint64_t PIECES = 20000;
    asr::RangeAllocator many(PIECES * 16);
    std::vector<uint64_t> offsets(PIECES);
    bool fitted = true;
    for (uint64_t& offset : offsets)
    {
        fitted = many.allocate(16, 16, offset) && fitted;
    }
    expect(fitted && many.freeSpace() == 0, "a span fills exactly");
    for (size_t i = 0; i < PIECES; i += 2)
    {
        many.release(offsets[i], 16);
    }
    expect(many.freeSpace() == PIECES * 8, "freed pieces are counted");
    uint64_t tooLarge = 0;
    expect(!many.allocate(32, 16, tooLarge), "scattered pieces do not add up to one range");
    for (size_t i = 1; i < PIECES; i += 2)
    {
        many.release(offsets[i], 16);
    }
    expect(many.allocate(PIECES * 16, 16, tooLarge) && tooLarge == 0, "all pieces merge back into one");

    // Freeing the tail must lower dispatch bounds without losing alignment holes.
    asr::RangeAllocator tail(128);
    expect(tail.allocate(3, 1, a) && tail.allocate(16, 16, b) && b == 16, "an aligned allocation leaves padding");
    tail.release(a, 3);
    expect(tail.used() == 32 && tail.live() == 16, "an interior hole keeps the live tail");
    tail.release(b, 16);
    expect(tail.used() == 0 && tail.live() == 0 && tail.freeSpace() == 128, "the free tail and its padding collapse together");
    expect(tail.allocate(128, 16, c) && c == 0, "the collapsed span can be reused completely");
}

void testTextureStreamingPolicy()
{
    // These regressions also execute during syntax-only checking, without a test build.
    static_assert(asr::firstTextureMip(2048, 2048, 4, true, 32 * 1024 * 1024) == 0, "roomy first images retain requested detail");
    static_assert(asr::firstTextureMip(2048, 2048, 4, true, 2 * 1024 * 1024) == 2, "first images fit remaining room before allocation");
    static_assert(asr::firstTextureMip(2048, 2048, 4, true, 0) == 4, "pressure still admits a 128-pixel first image");
    static_assert(asr::firstTextureMip(1024, 512, 4, true, 0) == 3, "rectangular images preserve full-size discard metadata");
    static_assert(asr::firstTextureMip(1, 4096, 4, true, 0) == 0, "thin images cannot change their logical full width");
    static_assert(asr::firstTextureMip(2048, 2048, 4, false, 0) == 0, "nonmipmapped images retain their existing semantics");
    static_assert(asr::textureFeedbackSeen(10.0, 10.0, 4.0), "unchanged completed feedback retains demand");
    static_assert(asr::textureFeedbackSeen(12.0, 10.0, 4.0), "one missed two-second window retains demand");
    static_assert(!asr::textureFeedbackSeen(14.0, 10.0, 4.0), "completed negative evidence expires demand");
    static_assert(!asr::textureFeedbackSeen(10.0, -1000.0, 4.0), "first images need bootstrap demand");
    static_assert(asr::avatarDetailTexelLimit(1.f) == 512 && asr::avatarDetailTexelLimit(2.f) == 256 &&
                  asr::avatarDetailTexelLimit(4.f) == 128, "small details reduce with distance");
    static_assert(asr::avatarDetailTexelLimit(0.5f) == 1024 && asr::avatarDetailTexelLimit(0.25f) == 2048,
                  "camera close-ups get finer small details");
    static_assert(asr::textureDiscardForLimit(2048, 512, 5) == 2 && asr::textureDiscardForLimit(256, 512, 5) == 0,
                  "limit the longest side without refining an already small asset");
    constexpr bool copyRecovery = []()
    {
        uint32_t level = 99;
        return asr::textureMipCopyLevel(1024, 1024, 11, 256, 256, 9, level) && level == 2 &&
               !asr::textureMipCopyLevel(128, 128, 8, 512, 512, 10, level) &&
               !asr::textureMipCopyLevel(1024, 1024, 3, 256, 256, 9, level) &&
               !asr::textureMipCopyLevel(0, 0, 0, 1, 1, 1, level);
    }();
    static_assert(copyRecovery, "actual mip recovery and invalid-copy rejection");
    constexpr bool replacementIsolation = []()
    {
        asr::TextureReplacementState<4> state;
        state.begin(1);
        state.fail(1);
        if (!state.failed(1) || state.failed(2)) return false;
        state.begin(2); // unrelated creates must not unblock a failed slot's uploads
        if (!state.failed(1)) return false;
        state.begin(1); // only a new transaction for this slot permits uploads again
        return !state.failed(1);
    }();
    static_assert(replacementIsolation, "failed replacements cannot fall back to uploading into the displayed image");
    // At eight GPU FPS a 16-frame window takes two seconds. No readback between
    // those windows is not evidence that a previously sampled texture disappeared.
    expect(asr::textureFeedbackSeen(10.0, 10.0, 4.0), "an unchanged completed-window clock retains demand while waiting for the GPU");
    expect(asr::textureFeedbackSeen(12.0, 10.0, 4.0), "one missed slow window retains the existing demand");
    expect(!asr::textureFeedbackSeen(14.0, 10.0, 4.0), "completed windows can establish that a texture is no longer sampled");
    expect(!asr::textureFeedbackSeen(10.0, -1000.0, 4.0), "never-sampled textures retain the separate bootstrap path");

    uint32_t mip = 99;
    // CPU believed a 2048 image had published, but allocation failed and the GPU
    // retained 1024. A requested 256 image needs actual mip 2, not queued mip 3.
    expect(asr::textureMipCopyLevel(1024, 1024, 11, 256, 256, 9, mip) && mip == 2,
           "a shrink uses the actual retained image after a failed grow");
    expect(asr::textureMipCopyLevel(1024, 512, 11, 128, 64, 8, mip) && mip == 3,
           "rectangular images select matching mip extents");
    expect(!asr::textureMipCopyLevel(128, 128, 8, 512, 512, 10, mip), "a missing larger source must retain the old image, never publish a clear");
    expect(!asr::textureMipCopyLevel(1024, 1024, 11, 256, 128, 9, mip), "mismatched source aspect cannot be copied");
    expect(!asr::textureMipCopyLevel(1024, 1024, 3, 256, 256, 9, mip), "an incomplete mip chain cannot fill a replacement");
    expect(!asr::textureMipCopyLevel(0, 0, 0, 1, 1, 1, mip), "a missing source cannot fill a replacement");
}

void testMeshLodResidency()
{
    asr::MeshLodResidency residency;
    expect(!residency.holds(3, 0.0, false), "an unrequested fine LOD is not admitted");
    residency.want(3, 1.0);
    residency.want(0, 5.0);
    expect(residency.holds(3, 5.5, true), "a camera glance does not immediately evict detail");
    expect(!residency.holds(3, 6.0, true) && residency.holds(0, 6.0, true), "under pressure only obsolete fine detail expires");
    expect(residency.holds(3, 30.0, false) && !residency.holds(3, 31.0, false), "normal retention has a bounded lifetime");
    expect(!residency.holds(3, 32.0, false), "a late callback cannot re-admit an evicted level");
    residency.want(3, 33.0); // actual finer fallback when the requested coarse asset is absent
    expect(residency.holds(3, 34.0, true) && residency.holds(0, 34.0, true), "current demand or an asset fallback admits the whole required chain again");
}

void testCameraPredictor()
{
    using Clock = asr::CameraPredictor::Clock;
    const Clock::time_point start = Clock::now();
    const auto at = [start](float seconds) { return start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(seconds)); };
    const float forward[3] = { 1.f, 0.f, 0.f };
    const float up[3] = { 0.f, 0.f, 1.f };
    const auto cameraAt = [&](float x)
    {
        const float eye[3] = { x, 2.f, 3.f };
        return asr::makeCameraFromAxes(eye, forward, up, 1.f, 1.5f, 0.1f, 200.f);
    };
    const auto close = [](float a, float b, float tolerance) { return std::fabs(a - b) < tolerance; };

    // Moving at 10 m/s, with main-thread steps 100 ms apart.
    asr::CameraPredictor moving;
    asr::SceneCamera3D first = moving.predict(cameraAt(0.f), at(0.f), at(0.f));
    expect(close(first.position[0], 0.f, 1e-5f), "the first camera is shown as it is");
    moving.predict(cameraAt(1.f), at(0.1f), at(0.1f));
    moving.predict(cameraAt(2.f), at(0.2f), at(0.2f));
    moving.predict(cameraAt(2.f), at(0.2f), at(0.5f)); // lets the correction of the start fade
    moving.predict(cameraAt(3.f), at(0.3f), at(0.3f));
    asr::SceneCamera3D ahead = moving.predict(cameraAt(3.f), at(0.3f), at(0.35f));
    expect(ahead.position[0] > 3.f, "between two steps the camera goes on moving");
    expect(close(ahead.position[1], 2.f, 1e-3f) && close(ahead.position[2], 3.f, 1e-3f), "it moves along its own path");
    asr::SceneCamera3D held = moving.predict(cameraAt(3.f), at(0.3f), at(5.f));
    asr::SceneCamera3D heldLater = moving.predict(cameraAt(3.f), at(0.3f), at(6.f));
    expect(close(held.position[0], heldLater.position[0], 1e-4f) && held.position[0] < 3.f + 10.f * 0.26f, "a stalled main thread leaves the camera held, not drifting");

    // Stopping: what was predicted past the stop fades back, with no jump.
    asr::SceneCamera3D stop = moving.predict(cameraAt(3.f), at(5.1f), at(5.1f));
    expect(stop.position[0] > 3.f, "a correction starts from what was shown");
    asr::SceneCamera3D settled = moving.predict(cameraAt(3.f), at(5.1f), at(7.f));
    expect(close(settled.position[0], 3.f, 1e-3f), "a correction fades away");

    // A cut, such as a teleport, is not motion.
    asr::CameraPredictor cut;
    cut.predict(cameraAt(0.f), at(0.f), at(0.f));
    cut.predict(cameraAt(0.5f), at(0.1f), at(0.1f));
    asr::SceneCamera3D teleported = cut.predict(cameraAt(5000.f), at(0.2f), at(0.25f));
    expect(close(teleported.position[0], 5000.f, 1e-2f), "a cut lands at once, with nothing carried over");

    // Turning a quarter of the way in two steps keeps a real, upright camera.
    asr::CameraPredictor still;
    const asr::SceneCamera3D same = cameraAt(7.f);
    still.predict(same, at(0.f), at(0.f));
    still.predict(same, at(0.1f), at(0.1f));
    const asr::SceneCamera3D unchanged = still.predict(same, at(0.2f), at(0.3f));
    expect(memcmp(&unchanged, &same, sizeof(same)) == 0, "a still camera comes through exactly");

    asr::CameraPredictor turning;
    const float turnedForward[3] = { std::cos(0.2f), std::sin(0.2f), 0.f };
    const float turnedTwice[3] = { std::cos(0.4f), std::sin(0.4f), 0.f };
    const float eye[3] = { 0.f, 0.f, 0.f };
    turning.predict(asr::makeCameraFromAxes(eye, forward, up, 1.f, 1.5f, 0.1f, 200.f), at(0.f), at(0.f));
    const asr::SceneCamera3D level = turning.predict(asr::makeCameraFromAxes(eye, forward, up, 1.f, 1.5f, 0.1f, 200.f), at(0.1f), at(0.1f));
    turning.predict(asr::makeCameraFromAxes(eye, turnedForward, up, 1.f, 1.5f, 0.1f, 200.f), at(0.2f), at(0.2f));
    turning.predict(asr::makeCameraFromAxes(eye, turnedTwice, up, 1.f, 1.5f, 0.1f, 200.f), at(0.3f), at(0.3f));
    const asr::SceneCamera3D turned = turning.predict(asr::makeCameraFromAxes(eye, turnedTwice, up, 1.f, 1.5f, 0.1f, 200.f), at(0.3f), at(5.f));
    expect(close(level.focal[1], turned.focal[1], 1e-4f) && close(level.farClip, turned.farClip, 1e-3f), "the lens stays the same");
    const float lookX = -turned.viewRows[2][0];
    const float lookY = -turned.viewRows[2][1];
    const float lookZ = -turned.viewRows[2][2];
    expect(close(lookX * lookX + lookY * lookY + lookZ * lookZ, 1.f, 1e-3f) && close(lookZ, 0.f, 1e-3f) && lookY > std::sin(0.4f), "a turn carries on in its own plane");

    // A zoom eases out: each step is half the one before, ending at 2 m. It must never pass
    // the end and come back, which is what a bump is.
    asr::CameraPredictor zoom;
    float position = 0.f;
    float stepSize = 1.f;
    float furthest = 0.f;
    float previousShown = 0.f;
    bool neverBack = true;
    zoom.predict(cameraAt(0.f), at(0.f), at(0.f));
    for (int step = 1; step <= 12; ++step)
    {
        position += stepSize;
        stepSize *= 0.5f;
        const asr::SceneCamera3D stepCamera = cameraAt(position);
        for (int frame = 0; frame < 10; ++frame)
        {
            const float frameTime = 0.1f * float(step) + 0.01f * float(frame);
            const float shown = zoom.predict(stepCamera, at(0.1f * float(step)), at(frameTime)).position[0];
            furthest = std::max(furthest, shown);
            neverBack = neverBack && shown >= previousShown - 0.01f;
            previousShown = shown;
        }
    }
    expect(furthest < 2.02f, "an easing camera does not pass where it is going");
    expect(neverBack, "an easing camera does not go backwards");

    // The first step of a motion has nothing to say it will go on.
    asr::CameraPredictor starting;
    starting.predict(cameraAt(0.f), at(0.f), at(0.f));
    starting.predict(cameraAt(0.f), at(0.1f), at(0.1f));
    expect(starting.predict(cameraAt(1.f), at(0.2f), at(0.29f)).position[0] < 1.01f, "a first step is not carried on");
}

void testMouseMotion()
{
    asr::MouseMotion motion;
    const asr::MouseMotion::Total start = motion.total();
    motion.add(1.5f, -2.25f);
    float dx = 0.f;
    float dy = 0.f;
    asr::MouseMotion::between(start, motion.total(), dx, dy);
    expect(dx == 1.5f && dy == -2.25f, "motion adds up on both axes");

    asr::MouseMotion fractions;
    for (int i = 0; i < 1000; ++i)
    {
        fractions.add(0.1f, -0.1f);
    }
    asr::MouseMotion::between(0, fractions.total(), dx, dy);
    expect(std::fabs(dx - 100.f) < 0.02f && std::fabs(dy + 100.f) < 0.02f, "fractions of a unit do not get lost");

    // Past 2^31 substeps the counts wrap, and the motion between two totals still holds.
    asr::MouseMotion far;
    far.add(40000000.f, -40000000.f);
    const asr::MouseMotion::Total before = far.total();
    far.add(3.f, 4.f);
    asr::MouseMotion::between(before, far.total(), dx, dy);
    expect(dx == 3.f && dy == 4.f, "motion across a wrap is right");
}

void testCameraTurn()
{
    const auto close = [](float a, float b, float tolerance) { return std::fabs(a - b) < tolerance; };
    const auto forwardOf = [](const asr::SceneCamera3D& camera, float out[3])
    {
        for (int i = 0; i < 3; ++i)
        {
            out[i] = -camera.viewRows[2][i];
        }
    };
    const float noShift[3] = {};
    const float pi = 3.14159265f;

    // Orbiting a point 10 m ahead, as the camera tool does.
    const float eye[3] = { 0.f, 0.f, 0.f };
    const float forward[3] = { 1.f, 0.f, 0.f };
    const float up[3] = { 0.f, 0.f, 1.f };
    const asr::SceneCamera3D published = asr::makeCameraFromAxes(eye, forward, up, 1.f, 1.5f, 0.1f, 200.f);
    asr::CameraTurn orbit;
    orbit.active = true;
    orbit.yawPerUnit = 0.01f;
    orbit.pitchPerUnit = 0.01f;
    orbit.yawAxis[2] = 1.f;
    orbit.pitchAxis[1] = 1.f; // the camera's left
    orbit.largestPitch = 0.1f;
    orbit.smallestPitch = -0.2f;
    orbit.eyePivot[0] = 10.f;
    orbit.eyeArm[0] = -10.f;
    orbit.focusPivot[0] = 10.f;
    orbit.up[2] = 1.f;

    const asr::SceneCamera3D unturned = asr::turnCamera(published, orbit, 0.f, 0.f, noShift);
    expect(memcmp(&unturned, &published, sizeof(published)) == 0, "with no motion the published camera comes through exactly");

    const asr::SceneCamera3D quarter = asr::turnCamera(published, orbit, pi / 2.f / 0.01f, 0.f, noShift);
    float look[3];
    forwardOf(quarter, look);
    expect(close(quarter.position[0], 10.f, 1e-3f) && close(quarter.position[1], -10.f, 1e-3f) && close(quarter.position[2], 0.f, 1e-3f),
           "a quarter yaw takes the eye a quarter of the way round the pivot");
    expect(close(look[0], 0.f, 1e-4f) && close(look[1], 1.f, 1e-4f) && close(look[2], 0.f, 1e-4f), "the turned eye still looks at the point");
    expect(close(quarter.focal[1], published.focal[1], 1e-5f) && close(quarter.lodScale, published.lodScale, 1e-6f), "the lens stays the same");

    // Pitch stops where the main thread would stop it, in both directions.
    const asr::SceneCamera3D over = asr::turnCamera(published, orbit, 0.f, 100.f, noShift);
    const asr::SceneCamera3D under = asr::turnCamera(published, orbit, 0.f, -1000.f, noShift);
    // By the right-hand rule about the left axis, a positive pitch lifts an eye behind the pivot.
    expect(close(std::atan2(over.position[2], 10.f - over.position[0]), 0.1f, 1e-4f), "pitch above 0 stops at the largest");
    expect(close(std::atan2(under.position[2], 10.f - under.position[0]), -0.2f, 1e-4f), "pitch below 0 stops at the smallest");

    // A full turn comes back to the published camera, roll included.
    const float rollAngle = 0.3f;
    const float rolledUp[3] = { 0.f, -std::sin(rollAngle), std::cos(rollAngle) };
    const asr::SceneCamera3D rolled = asr::makeCameraFromAxes(eye, forward, rolledUp, 1.f, 1.5f, 0.1f, 200.f);
    asr::CameraTurn rolling = orbit;
    rolling.roll = rollAngle;
    const asr::SceneCamera3D round = asr::turnCamera(rolled, rolling, 2.f * pi / 0.01f, 0.f, noShift);
    bool same = true;
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            same = same && close(round.viewRows[row][column], rolled.viewRows[row][column], 1e-3f);
        }
    }
    expect(same, "a full turn comes back to the published view, rolled as it was");

    // Mouselook turns the view about the eye, and a moved pivot moves the eye with it.
    asr::CameraTurn look360 = orbit;
    look360.eyePivot[0] = 0.f;
    look360.eyeArm[0] = 0.f;
    look360.focusPivot[0] = 0.f;
    look360.focusArm[0] = 1.f;
    const float shift[3] = { 1.f, 2.f, 3.f };
    const asr::SceneCamera3D looked = asr::turnCamera(published, look360, pi / 2.f / 0.01f, 0.f, shift);
    forwardOf(looked, look);
    expect(close(looked.position[0], 1.f, 1e-4f) && close(looked.position[1], 2.f, 1e-4f) && close(looked.position[2], 3.f, 1e-4f),
           "the eye only moves with its pivot");
    expect(close(look[0], 0.f, 1e-4f) && close(look[1], 1.f, 1e-4f), "the view turns about the eye");
}

void testCameraZoom()
{
    using Clock = asr::ZoomFollower::Clock;
    const auto close = [](float a, float b, float tolerance) { return std::fabs(a - b) < tolerance; };
    const float target[3] = { 10.f, 0.f, 0.f };

    // A step is LLAgentCamera's: the eye lerps first, then the shown eye towards the new eye.
    float eyeA[3] = { 2.f, 0.f, 0.f };
    float shownA[3] = { 1.f, 0.f, 0.f };
    asr::stepZoom(eyeA, shownA, target, 0.07f, 0.07f, 0.035f);
    expect(close(eyeA[0], 6.f, 1e-4f) && close(shownA[0], 6.f - 5.f * 0.25f, 1e-4f), "a zoom step lerps as the main thread does");

    // No smoothing: the shown eye is the eye. No zoom ease: the eye is at the target at once.
    float eyeD[3] = { 2.f, 0.f, 0.f };
    float shownD[3] = { 2.f, 0.f, 0.f };
    asr::stepZoom(eyeD, shownD, target, 0.07f, 0.07f, 0.f);
    expect(close(eyeD[0], 6.f, 1e-4f) && shownD[0] == eyeD[0], "with no smoothing the eye halves its way in a half-life");
    float eyeE[3] = { 2.f, 0.f, 0.f };
    float shownE[3] = { 2.f, 0.f, 0.f };
    asr::stepZoom(eyeE, shownE, target, 0.02f, 0.f, 0.02f);
    expect(eyeE[0] == target[0] && close(shownE[0], 6.f, 1e-4f), "with no zoom ease only the smoothing eases");

    // The follower: an eye 4 m behind a pivot at 10 m, zooming out to 8 m behind it, in
    // main-thread steps of 70 ms.
    asr::CameraTurn turn;
    turn.eyePivot[0] = 10.f;
    turn.zoom.active = true;
    turn.zoom.eyeNow[0] = 6.f;
    turn.zoom.eyeTarget[0] = 2.f;
    turn.zoom.zoomHalfLife = 0.07f;
    turn.zoom.stepSeconds = 0.07f;
    turn.zoom.origin[0] = 10.f;
    turn.zoom.targetDistance = 8.f;
    turn.zoom.perClick = 2.f;
    turn.zoom.nearest = 1.f;
    turn.zoom.farthest = 20.f;
    turn.zoom.included = 5;
    const float publishedEye[3] = { 6.f, 0.f, 0.f };
    const Clock::time_point start = Clock::now();
    asr::ZoomFollower follower;
    float arm[3];
    follower.follow(turn, publishedEye, 5, start, start, arm);
    expect(close(arm[0], -4.f, 1e-5f) && arm[1] == 0.f, "the follower starts at the published eye");
    follower.follow(turn, publishedEye, 5, start, start + std::chrono::microseconds(35000), arm);
    expect(close(arm[0], -5.f, 1e-3f), "half a step on, the follower is half way to the main thread's next eye");
    follower.follow(turn, publishedEye, 5, start, start + std::chrono::microseconds(70000), arm);
    expect(close(arm[0], -6.f, 1e-3f), "a step on, it is where the main thread's next eye will be");
    follower.follow(turn, publishedEye, 5, start, start + std::chrono::microseconds(140000), arm);
    expect(close(arm[0], -7.f, 1e-3f), "and so on, step by step");

    // Clicks the main thread has not handled count only when the wheel zooms the world.
    follower.follow(turn, publishedEye, 6, start, start + std::chrono::seconds(5), arm);
    expect(close(arm[0], -8.f, 1e-3f), "clicks are ignored while the wheel does not zoom the world");
    turn.zoom.followClicks = true;
    asr::ZoomFollower clicking;
    clicking.follow(turn, publishedEye, 6, start, start + std::chrono::seconds(5), arm);
    expect(close(arm[0], -16.f, 1e-3f), "a click not yet handled scales the target");

    // A click after the publish is handled by the main thread a step later than one before it.
    asr::ZoomFollower later1;
    later1.follow(turn, publishedEye, 5, start, start, arm);
    later1.follow(turn, publishedEye, 6, start, start + std::chrono::microseconds(10000), arm);
    later1.follow(turn, publishedEye, 6, start, start + std::chrono::microseconds(70000), arm);
    const float firstStep = arm[0];
    later1.follow(turn, publishedEye, 6, start, start + std::chrono::microseconds(140000), arm);
    expect(close(firstStep, -6.f, 1e-3f) && close(arm[0], -6.f + (-16.f + 6.f) * 0.5f, 1e-3f),
           "a click after the publish turns the ease from the step after the next");
    clicking.reset();
    clicking.follow(turn, publishedEye, 9, start, start + std::chrono::seconds(5), arm);
    expect(close(arm[0], -20.f, 1e-3f), "the target stays within the limits");

    // A new state that disagrees fades in, and does not jump.
    asr::CameraTurn moved = turn;
    moved.zoom.followClicks = false;
    moved.zoom.eyeNow[0] = 0.f;
    moved.zoom.eyeTarget[0] = 0.f;
    const float movedEye[3] = { 0.f, 0.f, 0.f };
    const Clock::time_point later = start + std::chrono::seconds(5); // published as the last frame was shown
    clicking.follow(moved, movedEye, 9, later, later, arm);
    expect(close(arm[0], -20.f, 1e-3f), "a new state's difference starts where the last frame was");
    clicking.follow(moved, movedEye, 9, later, later + std::chrono::milliseconds(60), arm);
    expect(close(arm[0], -15.f, 1e-2f), "and fades by half in its half-life");
}

void testTextureFeedback()
{
    using Clock = asr::TextureFeedback::Clock;
    const Clock::time_point start = Clock::now();
    asr::TextureFeedback::Collector collector;
    expect(collector.takeClear() && !collector.takeClear(), "the words are cleared once before first use");

    uint32_t value = 0;
    asr::TextureFeedback::Window ended;
    uint32_t places = 0;
    bool endedEarly = false;
    bool firstIsOne = true;
    for (uint32_t frame = 0; frame < asr::FEEDBACK_SAMPLES; ++frame)
    {
        // Long frames: only the frame count holds the window open.
        const bool last = collector.frame(start + std::chrono::seconds(frame), value, ended);
        endedEarly = endedEarly || (last && frame + 1 < asr::FEEDBACK_SAMPLES);
        firstIsOne = firstIsOne && (value >> 4) == 1;
        places |= 1u << (value & 15u);
    }
    expect(firstIsOne, "the first window is 1, as 0 means off");
    expect(!endedEarly && ended.number == 1 && ended.start == start, "a window ends after its 16th frame and says which it was");
    expect(places == 0xFFFFu, "a window samples every place in the block");

    // The second window began with the 16th frame, at 15 s.
    const Clock::time_point second = start + std::chrono::seconds(15);
    bool endedShort = false;
    for (uint32_t frame = 1; frame < 40; ++frame)
    {
        endedShort = endedShort || collector.frame(second + std::chrono::milliseconds(frame), value, ended);
    }
    expect((value >> 4) == 2, "the next window follows");
    expect(!endedShort, "fast frames keep a window open for its whole time");
    expect(collector.frame(second + std::chrono::milliseconds(150), value, ended) && ended.number == 2 && ended.start == second,
           "and it ends once its time is up");

    // Words from two windows: a slot keeps its largest level, and other windows' words do not count.
    std::vector<uint32_t> words(asr::MAX_TEXTURES, 0);
    const auto word = [](uint32_t window, uint32_t level) { return (window << asr::FEEDBACK_LEVEL_BITS) | (level + 1); };
    words[0] = word(5, 3);  // the white texture
    words[7] = word(5, 9);
    words[8] = word(4, 12); // an older window
    words[9] = word(5, 0);
    asr::TextureFeedback feedback;
    feedback.add(words.data(), { 5, start });
    words[7] = word(6, 11);
    words[9] = word(6, 0);
    words[10] = word(6, 2);
    feedback.add(words.data(), { 6, start + std::chrono::seconds(1) });
    const auto levelOf = [&feedback](uint32_t slot) -> int
    {
        for (size_t i = 0; i < feedback.slots.size(); ++i)
        {
            if (feedback.slots[i] == slot)
            {
                return feedback.levels[i];
            }
        }
        return -1;
    };
    expect(feedback.windows == 2 && feedback.since == start, "feedback counts its windows from the oldest");
    expect(feedback.slots.size() == 3 && levelOf(7) == 11 && levelOf(9) == 0 && levelOf(10) == 2, "each slot is listed once, at its largest level");
    expect(levelOf(0) < 0 && levelOf(8) < 0, "the white texture and other windows are left out");
    feedback.clear();
    feedback.add(words.data(), { 6, start });
    expect(feedback.windows == 1 && feedback.slots.size() == 3, "a cleared feedback starts again");
}

class FakeChunks final : public asr::SceneChunks
{
public:
    uint64_t createSceneChunk(uint32_t index) override
    {
        if (limit == 0)
        {
            return 0;
        }
        --limit;
        lastIndex = index;
        return next += asr::SCENE_CHUNK_BYTES;
    }

    void releaseSceneChunk(uint32_t index) override
    {
        released.push_back(index);
    }

    uint32_t limit = 100;
    uint32_t lastIndex = 0;
    uint64_t next = 0;
    std::vector<uint32_t> released;
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

    // A rank reaches the shader, and says nothing about which list a surface goes in.
    desc.flags = asr::INSTANCE_RANK_MAX << asr::INSTANCE_RANK_SHIFT;
    queue.setInstance(first, desc);
    store.update(queue, chunks, ++frame, LIMIT);
    const asr::GpuInstance* ranked = reinterpret_cast<const asr::GpuInstance*>(store.tableBytes().data());
    expect(((ranked->flags >> asr::INSTANCE_RANK_SHIFT) & asr::INSTANCE_RANK_MAX) == asr::INSTANCE_RANK_MAX, "a rank is kept");
    expect(asr::sceneKind(ranked->flags) == 0, "a rank does not change the list");
    desc.flags = 0;

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
    expect(store.chunkCount() == 0 && chunks.released.size() == 1 && chunks.released.back() == 0,
           "an empty chunk is returned to the backend after frames in flight finish");
    expect(queue.addGeometry(asrtest::build(mesh)) == geometry, "a freed id is reused");
    store.update(queue, chunks, ++frame, LIMIT);
    expect(store.chunkCount() == 1 && chunks.lastIndex == 0, "a released chunk slot is reusable without moving live indices");

    // Ranged tables shrink after retirement and do not regrow to historical capacity
    // when a much smaller avatar arrives. Neither shrink nor ID reuse is premature.
    asr::SceneQueue trimQueue;
    asr::SceneStore trim(2);
    const uint32_t largeSkeleton = trimQueue.addSkeleton(2048);
    trim.update(trimQueue, chunks, 1, LIMIT);
    const uint64_t largeTable = trim.tableCapacity(asr::SceneTable::Joints);
    expect(largeTable > 64 * 1024, "the reclamation test grows a joint table");
    trimQueue.removeSkeleton(largeSkeleton);
    trim.update(trimQueue, chunks, 2, LIMIT);
    expect(trim.tableCapacity(asr::SceneTable::Joints) == largeTable, "a table cannot shrink while retired joints are in flight");
    trim.update(trimQueue, chunks, 6, LIMIT);
    expect(trim.tableCapacity(asr::SceneTable::Joints) == 64 * 1024 && trim.rangeEnd(asr::SceneTable::Joints) == 0,
           "retired joint storage and dispatch extent shrink");
    trimQueue.addSkeleton(2);
    trim.update(trimQueue, chunks, 7, LIMIT);
    expect(trim.tableCapacity(asr::SceneTable::Joints) == 64 * 1024, "a small allocation does not restore the historical table size");

    // LODs: lists keep room for every LOD an instance could show, and a coarse LOD
    // stays for as long as a finer one names it.
    asr::SceneQueue lodQueue;
    asr::SceneStore lods(2);
    const asr::GeometryBox box{ { 0.f, 0.f, 0.f }, 1.f };
    const asrtest::Mesh fine = asrtest::makeSphere(32, 64);
    const asrtest::Mesh rough = asrtest::makeSphere(8, 16);
    const uint32_t roughId = lodQueue.addGeometry(asrtest::build(rough, &box));
    const uint32_t fineId = lodQueue.addGeometry(asrtest::build(fine, &box), roughId, 10.f);
    const asr::GeometryBox otherBox{ { 0.25f, 0.f, 0.f }, 2.f };
    const uint32_t otherBoxId = lodQueue.addGeometry(asrtest::build(fine, &otherBox), roughId, 10.f);
    desc.geometry = fineId;
    desc.flags = asr::INSTANCE_MASKED;
    const uint32_t lodInstance = lodQueue.addInstance(desc);
    lods.update(lodQueue, chunks, 1, LIMIT);
    expect(lods.triangles() == fine.indices.size() / 3, "only the finest LOD counts as the object's triangles");
    uint64_t fineRoom = 0;
    for (uint32_t bucket = 0; bucket < asr::CLUSTER_BUCKETS; ++bucket)
    {
        fineRoom += lods.listCapacity(asr::sceneList(asr::sceneKind(asr::INSTANCE_MASKED), bucket));
    }
    expect(fineRoom >= asrtest::build(fine, &box).clusters.size() + asrtest::build(rough, &box).clusters.size(), "lists of the instance's kind hold both LODs");

    bool sawChain = false;
    bool otherBoxLinked = false;
    for (const asr::SceneWrite& write : lods.writes())
    {
        if (write.table == asr::SceneTable::Geometries)
        {
            const asr::GpuGeometry* entry = reinterpret_cast<const asr::GpuGeometry*>(lods.tableBytes().data() + write.source);
            const uint32_t id = uint32_t(write.offset / sizeof(asr::GpuGeometry));
            sawChain = sawChain || (id == fineId && entry->coarser == roughId && entry->coarserAt == 10.f && entry->clusterCount != 0);
            otherBoxLinked = otherBoxLinked || (id == otherBoxId && entry->coarser != 0);
        }
    }
    expect(sawChain, "the GPU sees the chain");
    expect(!otherBoxLinked, "an LOD in another box is not linked");

    lodQueue.removeGeometry(roughId);
    lods.update(lodQueue, chunks, 2, LIMIT);
    lods.update(lodQueue, chunks, 10, LIMIT);
    const size_t releasesWithRough = chunks.released.size();
    lodQueue.removeInstance(lodInstance);
    lodQueue.removeGeometry(otherBoxId);
    lodQueue.removeGeometry(fineId);
    lods.update(lodQueue, chunks, 11, LIMIT);
    lods.update(lodQueue, chunks, 20, LIMIT);
    // A completely empty chunk is destroyed, rather than retained as free space.
    expect(lods.chunkCount() == 0 && chunks.released.size() > releasesWithRough, "the coarse LOD went with its last user");
    expect(lods.triangles() == 0, "nothing is left of the chain");
    desc.flags = 0;

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
    expect(many.instanceEnd() > COUNT / 8 && many.instanceEnd() <= COUNT / 4, "a burst of instances spreads over frames");
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

// Mac OIT's keys pass, as asrSceneMacOIT.glsl runs it, against the OpenGL version's peeling:
// layer 0 is the smallest key, and layer k the smallest key strictly behind layer k-1 in
// depth. Both must give the same layers whatever order fragments arrive in. A key's depth is
// its bits above the lowest 8.
void testMacOITKeys()
{
    const auto depthOf = [](uint32_t key) { return key >> 8; };
    const auto insert = [&](uint32_t slots[asr::MACOIT_LAYERS], uint32_t key)
    {
        for (uint32_t k = 0; k < asr::MACOIT_LAYERS; ++k)
        {
            const uint32_t previous = slots[k];
            slots[k] = std::min(previous, key); // atomicMin
            if (previous == asr::MACOIT_EMPTY_KEY || depthOf(previous) == depthOf(key))
            {
                return;
            }
            key = std::max(previous, key);
        }
    };

    uint32_t random = 777;
    const auto next = [&random]() { random = random * 1664525u + 1013904223u; return random >> 8; };
    bool same = true;
    for (int trial = 0; trial < 2000 && same; ++trial)
    {
        // Few depths, so many fragments share one, as two faces of one layer would.
        std::vector<uint32_t> keys(1 + next() % 12);
        for (uint32_t& key : keys)
        {
            key = (1 + next() % 8) << 8 | (next() & 255);
        }
        uint32_t peeled[asr::MACOIT_LAYERS];
        uint32_t behind = 0;
        for (uint32_t k = 0; k < asr::MACOIT_LAYERS; ++k)
        {
            peeled[k] = asr::MACOIT_EMPTY_KEY;
            for (uint32_t key : keys)
            {
                if (depthOf(key) > behind && key < peeled[k])
                {
                    peeled[k] = key;
                }
            }
            behind = peeled[k] == asr::MACOIT_EMPTY_KEY ? behind : depthOf(peeled[k]);
        }
        for (int order = 0; order < 6 && same; ++order)
        {
            for (size_t i = keys.size(); i > 1; --i)
            {
                std::swap(keys[i - 1], keys[next() % i]);
            }
            uint32_t slots[asr::MACOIT_LAYERS];
            std::fill(slots, slots + asr::MACOIT_LAYERS, asr::MACOIT_EMPTY_KEY);
            for (uint32_t key : keys)
            {
                insert(slots, key);
            }
            same = std::equal(slots, slots + asr::MACOIT_LAYERS, peeled);
        }
    }
    expect(same, "Mac OIT's one-pass keys are the layers peeling finds, in any order");
}

// The newest bytes this update sent for one table entry, or null.
template <typename T>
const T* writtenEntry(const asr::SceneStore& store, asr::SceneTable table, uint64_t index)
{
    const uint64_t at = index * asr::SCENE_TABLE_STRIDE[uint32_t(table)];
    const uint8_t* found = nullptr;
    for (const asr::SceneWrite& write : store.writes())
    {
        if (write.table == table && write.offset <= at && at < write.offset + write.bytes)
        {
            const uint8_t* bytes = write.data ? static_cast<const uint8_t*>(write.data) : store.tableBytes().data() + write.source;
            found = bytes + (at - write.offset);
        }
    }
    return reinterpret_cast<const T*>(found);
}

void testSkinAmounts()
{
    const float cases[][4] = { { 0.f, .5f, .5f, 0.f }, { 0.f, 1.f/6.f, 1.f/6.f, 2.f/3.f },
                              { .2f, .3f, .1f, .4f }, { 0.f, 0.f, 0.f, 1.f } };
    for (const auto& shares : cases)
    {
        uint16_t amounts[4];
        asr::packSkinAmounts(shares, amounts);
        uint32_t total = 0;
        bool emptyStaysEmpty = true;
        for (int i = 0; i < 4; ++i)
        {
            total += amounts[i];
            emptyStaysEmpty = emptyStaysEmpty && (shares[i] != 0.f || amounts[i] == 0);
        }
        expect(total == 65535 && emptyStaysEmpty, "rounded skin weights total exactly one without activating empty joints");
    }
    const float zero[4]{};
    uint16_t amounts[4];
    asr::packSkinAmounts(zero, amounts);
    expect(amounts[0] == 65535 && amounts[1] == 0 && amounts[2] == 0 && amounts[3] == 0,
           "a vertex with no skin amounts follows its first joint");
}

void testJointPrediction()
{
    // Independent analytic rotations of a sheared, nonuniformly scaled shape.
    // Matrix-element extrapolation fails these checks even for an unscaled joint.
    const float shape[3][4] = { { 1.7f, .3f, .1f, 1.f }, { 0.f, .8f, .2f, 2.f }, { 0.f, 0.f, 1.2f, 3.f } };
    auto rotated = [&](int axis, float angle, float out[3][4])
    {
        const int a = (axis + 1) % 3, b = (axis + 2) % 3;
        memcpy(out, shape, sizeof(shape));
        for (int column = 0; column < 3; ++column)
        {
            out[a][column] = std::cos(angle)*shape[a][column] - std::sin(angle)*shape[b][column];
            out[b][column] = std::sin(angle)*shape[a][column] + std::cos(angle)*shape[b][column];
        }
    };
    for (int axis = 0; axis < 3; ++axis)
    {
        bool followsRotation = true, preservesShape = true, followsTranslation = true;
        for (float degrees : { 5.f, 60.f, 170.f, -170.f })
        {
            const float angle = degrees * 3.14159265358979323846f / 180.f;
            float newest[3][4], delta[4];
            rotated(axis, angle, newest);
            newest[0][3] += .5f;
            asr::jointRotationDelta(newest, shape, delta);
            for (float lead : { 0.f, .25f, .5f, asr::SKELETON_MAX_LEAD })
            {
                float predicted[3][4], expected[3][4];
                asr::extrapolateJointRows(newest, shape, delta, lead, predicted);
                rotated(axis, angle * (1.f + lead), expected);
                for (int row = 0; row < 3; ++row)
                {
                    followsTranslation = followsTranslation && std::fabs(predicted[row][3] -
                        (shape[row][3] + (row == 0 ? .5f * (1.f + lead) : 0.f))) < 1e-5f;
                    for (int column = 0; column < 3; ++column)
                    {
                        followsRotation = followsRotation && std::fabs(predicted[row][column] - expected[row][column]) < 2e-5f;
                        float before = 0.f, after = 0.f;
                        for (int k = 0; k < 3; ++k)
                        {
                            before += newest[k][row] * newest[k][column];
                            after += predicted[k][row] * predicted[k][column];
                        }
                        preservesShape = preservesShape && std::fabs(before - after) < 2e-5f;
                    }
                }
            }
        }
        expect(followsRotation, "joint prediction follows analytic rotation about each axis, including large turns");
        expect(preservesShape, "joint prediction preserves column lengths and angles with nonuniform scale and shear");
        expect(followsTranslation, "joint prediction retains linear translation");
    }
    float singular[3][4]{}, delta[4], result[3][4];
    asr::jointRotationDelta(shape, singular, delta);
    asr::extrapolateJointRows(shape, singular, delta, 1.f, result);
    expect(delta[3] == 1.f && result[0][0] == shape[0][0] && std::isfinite(result[2][3]),
           "a singular previous pose keeps the latest shape finite");
    float reflected[3][4];
    memcpy(reflected, shape, sizeof(shape));
    for (int column = 0; column < 3; ++column) reflected[0][column] *= -1.f;
    asr::jointRotationDelta(reflected, shape, delta);
    expect(delta[0] == 0.f && delta[1] == 0.f && delta[2] == 0.f && delta[3] == 1.f,
           "a reflection change is not treated as a rotation");
    asr::jointRotationDelta(shape, shape, delta);
    asr::extrapolateJointRows(shape, shape, delta, asr::SKELETON_MAX_LEAD, result);
    bool unchanged = true;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
            unchanged = unchanged && std::fabs(result[row][column] - shape[row][column]) < 1e-6f;
    expect(unchanged, "a stopped pose does not drift");
}

void testSkinStore()
{
    asr::SceneQueue queue;
    asr::SceneStore store(2);
    FakeChunks chunks;
    constexpr uint64_t LIMIT = 4 * 1024 * 1024;

    std::vector<asr::SkinWeights> fineWeights;
    const asrtest::Mesh fine = asrtest::makeTube(64, 24, fineWeights);
    asrtest::Mesh rough = fine;
    rough.indices = asrtest::coarseTubeIndices(64, 24, 8, 3);
    const uint32_t roughId = queue.addGeometry(asrtest::build(rough, &asrtest::CREATURE_BOX, fineWeights.data()));
    const uint32_t fineId = queue.addGeometry(asrtest::build(fine, &asrtest::CREATURE_BOX, fineWeights.data()), roughId, 30.f);
    const uint32_t plain = queue.addGeometry(asrtest::build(asrtest::makeBoxFace(0, 1.f)));
    const uint32_t skin = queue.addSkin(asrtest::creatureSkin());

    // Morphs at full weight, the swell unmasked: the furthest the vertices go.
    const asr::MorphSetDesc morphDesc = asrtest::creatureMorphs(fine);
    asr::MorphSetDesc morphCopy = morphDesc;
    const uint32_t morphSet = queue.addMorphSet(std::move(morphCopy));
    const uint32_t morph = queue.addMorph(morphSet);
    const float fullWeights[2] = { 1.f, 1.f };
    queue.setMorphWeights(morph, fullWeights, 2);
    const std::vector<float> fullMask(morphDesc.targets[1].vertices.size(), 1.f);
    queue.setMorphMask(morph, 1, fullMask.data(), uint32_t(fullMask.size()));
    std::vector<float> morphed = morphDesc.positions;
    for (const asr::MorphTargetDesc& target : morphDesc.targets)
    {
        for (size_t i = 0; i < target.vertices.size(); ++i)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                morphed[target.vertices[i] * 3 + axis] += target.positions[i * 3 + axis];
            }
        }
    }
    const uint32_t skeleton = queue.addSkeleton(asrtest::CREATURE_JOINTS);
    float rows[asrtest::CREATURE_JOINTS * 12];
    asrtest::creaturePose(0.f, 10.f, 20.f, 0.5f, 0.f, rows);
    const auto start = std::chrono::steady_clock::now();
    queue.poseSkeleton(skeleton, rows, asrtest::CREATURE_JOINTS, start);

    asr::InstanceDesc tube;
    tube.geometry = fineId;
    tube.skeleton = skeleton;
    tube.skin = skin;
    tube.morph = morph;
    tube.lodSize = asrtest::CREATURE_HEIGHT;
    const uint32_t tubeId = queue.addInstance(tube);
    asr::InstanceDesc held;
    memset(held.objectToWorld, 0, sizeof(held.objectToWorld));
    held.objectToWorld[0] = held.objectToWorld[5] = held.objectToWorld[10] = 1.f;
    held.objectToWorld[11] = 1.25f;
    held.geometry = plain;
    held.skeleton = skeleton;
    held.joint = asrtest::CREATURE_JOINTS - 1;
    const uint32_t heldId = queue.addInstance(held);
    asr::InstanceDesc loose = tube;
    loose.geometry = plain; // no weights: nothing to skin
    loose.morph = 0;
    const uint32_t looseId = queue.addInstance(loose);

    uint64_t frame = 10;
    store.update(queue, chunks, frame, LIMIT);
    expect(store.skinnedInstances() == 1, "a skinned instance is bound, and one with no weights is not");
    const asr::GpuInstance* tubeEntry = writtenEntry<asr::GpuInstance>(store, asr::SceneTable::Instances, tubeId);
    expect(tubeEntry && (tubeEntry->flags & (asr::INSTANCE_SKINNED | asr::INSTANCE_BOUND)) == (asr::INSTANCE_SKINNED | asr::INSTANCE_BOUND),
           "a skinned instance is marked");
    expect(tubeEntry && tubeEntry->geometry != fineId && tubeEntry->geometry != roughId && tubeEntry->geometry != 0,
           "a skinned instance draws through a geometry of its own");
    const uint32_t bindingId = tubeEntry ? tubeEntry->binding : 0;
    const asr::GpuBinding* binding = writtenEntry<asr::GpuBinding>(store, asr::SceneTable::Bindings, bindingId);
    expect(binding && binding->kind == asr::BINDING_SKINNED && binding->source == fineId && binding->instance == tubeId &&
               tubeEntry && binding->target == tubeEntry->geometry,
           "its binding names the source chain and its own geometry");
    expect(binding && binding->morph == morph, "a morphed instance names its morph");
    expect(store.skinThreads(asr::SKIN_MORPH) == (fine.vertices.size() + asr::SKIN_BLOCK - 1) / asr::SKIN_BLOCK * asr::SKIN_BLOCK,
           "a new morph is rebuilt, every vertex once");

    // Each vertex names its run of deltas, and the runs hold every delta once.
    const asr::GpuMorph* morphEntry = writtenEntry<asr::GpuMorph>(store, asr::SceneTable::Morphs, morph);
    const asr::GpuMorphVertex* morphVertices =
        morphEntry ? writtenEntry<asr::GpuMorphVertex>(store, asr::SceneTable::MorphVertices, morphEntry->firstVertex) : nullptr;
    uint32_t deltaTotal = 0;
    bool runsInOrder = morphVertices != nullptr;
    for (uint32_t v = 0; morphVertices && v < morphEntry->vertexCount; ++v)
    {
        runsInOrder = runsInOrder && (v == 0 || morphVertices[v].firstDelta == morphVertices[v - 1].firstDelta + morphVertices[v - 1].deltaCount);
        deltaTotal += morphVertices[v].deltaCount;
    }
    expect(runsInOrder && deltaTotal == morphDesc.targets[0].vertices.size() + morphDesc.targets[1].vertices.size(),
           "morph deltas are sorted by vertex, each once");
    const asr::GpuInstance* heldEntry = writtenEntry<asr::GpuInstance>(store, asr::SceneTable::Instances, heldId);
    const asr::GpuSkeleton* skeletonEntry = writtenEntry<asr::GpuSkeleton>(store, asr::SceneTable::Skeletons, skeleton);
    const asr::GpuBinding* heldBinding = heldEntry ? writtenEntry<asr::GpuBinding>(store, asr::SceneTable::Bindings, heldEntry->binding) : nullptr;
    expect(heldBinding && skeletonEntry && heldBinding->kind == asr::BINDING_ATTACHED &&
               heldBinding->palette == skeletonEntry->firstJoint + asrtest::CREATURE_JOINTS - 1,
           "an attached instance follows its joint");
    const asr::GpuInstance* looseEntry = writtenEntry<asr::GpuInstance>(store, asr::SceneTable::Instances, looseId);
    expect(looseEntry && looseEntry->geometry == 0, "an instance that cannot be skinned draws nothing");
    expect(store.skinThreads(asr::SKIN_VERTICES) >= fine.vertices.size() && store.skinThreads(asr::SKIN_PALETTE) >= asrtest::CREATURE_JOINTS,
           "the skin passes cover every vertex and palette entry");

    // Every vertex, skinned on the CPU, must land in the box its positions are packed in.
    const float bindingReach = binding ? binding->reach : 0.f; // held now: a later update need not write the entry again
    const auto insideBox = [&](const asr::GpuSkeleton& entry, const float* pose)
    {
        bool inside = true;
        for (size_t i = 0; i < fine.vertices.size(); ++i)
        {
            const float* p = &morphed[i * 3];
            float skinned[3] = { 0.f, 0.f, 0.f };
            for (int k = 0; k < 2; ++k)
            {
                const uint32_t joint = fineWeights[i].joints[k];
                const float amount = fineWeights[i].weights[k] / 65535.f;
                const float lowered[12] = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, -float(joint) };
                float palette[12];
                asrtest::multiplyRows(pose + joint * 12, lowered, palette);
                for (int row = 0; row < 3; ++row)
                {
                    skinned[row] += amount * (palette[row * 4] * p[0] + palette[row * 4 + 1] * p[1] + palette[row * 4 + 2] * p[2] + palette[row * 4 + 3]);
                }
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                inside = inside && std::fabs(skinned[axis] - entry.boxCenter[axis]) <= entry.boxScale + bindingReach * entry.stretch;
            }
        }
        return inside;
    };
    expect(skeletonEntry && insideBox(*skeletonEntry, rows), "the binding's box holds every morphed and skinned vertex");

    // A second pose: the one before is kept for the GPU to carry the motion on from.
    float later[asrtest::CREATURE_JOINTS * 12];
    asrtest::creaturePose(0.5f, 10.5f, 20.f, 0.6f, 0.f, later);
    queue.poseSkeleton(skeleton, later, asrtest::CREATURE_JOINTS, start + std::chrono::milliseconds(20));
    store.update(queue, chunks, ++frame, LIMIT);
    skeletonEntry = writtenEntry<asr::GpuSkeleton>(store, asr::SceneTable::Skeletons, skeleton);
    const asr::GpuJoint* joints = skeletonEntry ? writtenEntry<asr::GpuJoint>(store, asr::SceneTable::Joints, skeletonEntry->firstJoint) : nullptr;
    expect(skeletonEntry && skeletonEntry->time - skeletonEntry->previousTime == 20000 && skeletonEntry->maxLead == asr::SKELETON_MAX_LEAD,
           "two poses give a step and a lead");
    expect(joints && joints[1].rows[0][3] == later[12 + 3] && joints[1].previous[0][3] == rows[12 + 3], "joints carry both poses");
    expect(store.skinThreads(asr::SKIN_MORPH) == 0, "a morph whose weights did not change is not rebuilt");

    // A far avatar's skeleton: drawn one gap late, between its last two poses, and a gap
    // after a rest counts as at most the one given.
    const uint32_t farSkeleton = queue.addSkeleton(asrtest::CREATURE_JOINTS);
    queue.poseSkeleton(farSkeleton, rows, asrtest::CREATURE_JOINTS, start, std::chrono::microseconds(50000));
    queue.poseSkeleton(farSkeleton, later, asrtest::CREATURE_JOINTS, start + std::chrono::seconds(5), std::chrono::microseconds(50000));
    store.update(queue, chunks, ++frame, LIMIT);
    const asr::GpuSkeleton* farEntry = writtenEntry<asr::GpuSkeleton>(store, asr::SceneTable::Skeletons, farSkeleton);
    expect(farEntry && farEntry->minLead == -1.f && farEntry->maxLead == 0.f && farEntry->time - farEntry->previousTime == 50000,
           "an interpolated skeleton blends one capped gap late, never past its newest pose");
    float ahead[asrtest::CREATURE_JOINTS * 12];
    for (size_t i = 0; i < asrtest::CREATURE_JOINTS; ++i)
    {
        float newest[3][4], previous[3][4], delta[4], predicted[3][4];
        memcpy(newest, later + i * 12, sizeof(newest));
        memcpy(previous, rows + i * 12, sizeof(previous));
        asr::jointRotationDelta(newest, previous, delta);
        asr::extrapolateJointRows(newest, previous, delta, asr::SKELETON_MAX_LEAD, predicted);
        memcpy(ahead + i * 12, predicted, sizeof(predicted));
    }
    expect(skeletonEntry && insideBox(*skeletonEntry, later) && insideBox(*skeletonEntry, ahead), "the box holds the furthest lead too");

    // A new material keeps the skinned geometry: only its instance and binding are written.
    tube.material = 3;
    queue.setInstance(tubeId, tube);
    store.update(queue, chunks, ++frame, LIMIT);
    tubeEntry = writtenEntry<asr::GpuInstance>(store, asr::SceneTable::Instances, tubeId);
    expect(tubeEntry && tubeEntry->binding == bindingId && tubeEntry->material == 3, "a skinned instance set again keeps its binding");
    expect(store.writes().size() == 2, "and writes only its instance and binding");

    // Out of view, an avatar gives back its skinned copies; back in view it gets them again.
    const uint64_t liveBefore = store.rangeLive(asr::SceneTable::SkinnedPositions);
    queue.setSkeletonResident(skeleton, false);
    store.update(queue, chunks, ++frame, LIMIT);
    const asr::GpuBinding* parked = writtenEntry<asr::GpuBinding>(store, asr::SceneTable::Bindings, bindingId);
    expect(parked && parked->kind == 0, "a parked avatar's bindings are skipped");
    const asr::GpuSkeleton* parkedSkeleton = writtenEntry<asr::GpuSkeleton>(store, asr::SceneTable::Skeletons, skeleton);
    expect(parkedSkeleton && (parkedSkeleton->flags & asr::SKELETON_PARKED), "parked skins also stop generating unused palettes");
    for (int i = 0; i < 8; ++i)
    {
        store.update(queue, chunks, ++frame, LIMIT); // past the free delay
    }
    expect(store.rangeLive(asr::SceneTable::SkinnedPositions) < liveBefore, "and its deformed positions are released");
    queue.setSkeletonResident(skeleton, true);
    store.update(queue, chunks, ++frame, LIMIT);
    const asr::GpuBinding* back = writtenEntry<asr::GpuBinding>(store, asr::SceneTable::Bindings, bindingId);
    const asr::GpuSkeleton* residentSkeleton = writtenEntry<asr::GpuSkeleton>(store, asr::SceneTable::Skeletons, skeleton);
    expect(residentSkeleton && (residentSkeleton->flags & asr::SKELETON_PARKED) == 0, "un-parking restores palette generation in the same update");
    expect(back && back->kind == asr::BINDING_SKINNED && store.rangeLive(asr::SceneTable::SkinnedPositions) == liveBefore,
           "back in view, its bindings are skinned again with storage of their own");

    // Faces can have different morph bounds, but shared edges still need one packing
    // grid for their skeleton/skin. A later widening must reach already-bound faces too.
    asr::InstanceDesc sibling = tube;
    sibling.morph = 0;
    const uint32_t siblingId = queue.addInstance(sibling);
    store.update(queue, chunks, ++frame, LIMIT);
    const asr::GpuInstance* siblingEntry = writtenEntry<asr::GpuInstance>(store, asr::SceneTable::Instances, siblingId);
    const uint32_t siblingBindingId = siblingEntry ? siblingEntry->binding : 0;
    const asr::GpuBinding* siblingBinding = writtenEntry<asr::GpuBinding>(store, asr::SceneTable::Bindings, siblingBindingId);
    expect(siblingBinding && siblingBinding->reach == bindingReach,
           "another face of the same skin keeps the shared packing grid despite a smaller morph bound");
    const float largerMorph[2] = { 2.f, 2.f };
    queue.setMorphWeights(morph, largerMorph, 2);
    store.update(queue, chunks, ++frame, LIMIT);
    binding = writtenEntry<asr::GpuBinding>(store, asr::SceneTable::Bindings, bindingId);
    siblingBinding = writtenEntry<asr::GpuBinding>(store, asr::SceneTable::Bindings, siblingBindingId);
    expect(binding && siblingBinding && binding->reach > bindingReach && binding->reach == siblingBinding->reach,
           "a larger morph updates the packing grid of every face sharing that skin");

    // An absent/coarse face has no weights. Either neighbour can still supply a
    // valid weighted surface; a bad palette index in one LOD is isolated too.
    const uint32_t unweightedCoarse = queue.addGeometry(asrtest::build(rough, &asrtest::CREATURE_BOX));
    const uint32_t weightedFine = queue.addGeometry(asrtest::build(fine, &asrtest::CREATURE_BOX, fineWeights.data()), unweightedCoarse, 30.f);
    const uint32_t unweightedFine = queue.addGeometry(asrtest::build(fine, &asrtest::CREATURE_BOX), roughId, 30.f);
    std::vector<asr::SkinWeights> invalidWeights = fineWeights;
    for (asr::SkinWeights& weights : invalidWeights)
        for (int k = 0; k < 4; ++k) weights.joints[k] = 255;
    const uint32_t invalidCoarse = queue.addGeometry(asrtest::build(rough, &asrtest::CREATURE_BOX, invalidWeights.data()));
    const uint32_t goodFine = queue.addGeometry(asrtest::build(fine, &asrtest::CREATURE_BOX, fineWeights.data()), invalidCoarse, 30.f);
    uint32_t mixedInstances[3];
    size_t mixed = 0;
    for (uint32_t geometry : { weightedFine, unweightedFine, goodFine })
    {
        asr::InstanceDesc desc = sibling;
        desc.geometry = geometry;
        mixedInstances[mixed++] = queue.addInstance(desc);
    }
    store.update(queue, chunks, ++frame, LIMIT);
    for (uint32_t instance : mixedInstances)
    {
        const asr::GpuInstance* entry = writtenEntry<asr::GpuInstance>(store, asr::SceneTable::Instances, instance);
        expect(entry && entry->binding != 0 && (entry->flags & asr::INSTANCE_SKINNED) != 0,
               "a valid weighted LOD survives an unweighted or invalid neighbour");
    }
    const asr::GpuGeometry* coarseEntry = writtenEntry<asr::GpuGeometry>(store, asr::SceneTable::Geometries, unweightedCoarse);
    const asr::GpuGeometry* invalidEntry = writtenEntry<asr::GpuGeometry>(store, asr::SceneTable::Geometries, invalidCoarse);
    expect(coarseEntry && coarseEntry->skinJoints == 0 && invalidEntry && invalidEntry->skinJoints == 256,
           "the GPU receives each LOD's own palette requirements");

    // Everything goes once frames in flight are done with it, and ids come back.
    for (uint32_t instance : mixedInstances) queue.removeInstance(instance);
    queue.removeInstance(siblingId);
    queue.removeInstance(tubeId);
    queue.removeInstance(heldId);
    queue.removeInstance(looseId);
    queue.removeSkeleton(skeleton);
    queue.removeSkin(skin);
    queue.removeMorph(morph);
    queue.removeMorphSet(morphSet);
    store.update(queue, chunks, ++frame, LIMIT);
    expect(store.skinnedInstances() == 0, "a removed skinned instance is unbound");
    store.update(queue, chunks, frame += 4, LIMIT);
    expect(queue.addSkeleton(2) == skeleton, "a freed skeleton id is reused");
    expect(queue.addSkin(asrtest::creatureSkin()) == skin, "a freed skin id is reused");
    expect(queue.addMorph(1) == morph && queue.addMorphSet(asr::MorphSetDesc()) == morphSet, "freed morph ids are reused");
}

void testDeformationCompaction()
{
    asr::SceneQueue queue;
    asr::SceneStore store(2);
    FakeChunks chunks;
    constexpr uint64_t LIMIT = 64 * 1024 * 1024;
    constexpr uint32_t AVATARS = 80;
    std::vector<asr::SkinWeights> weights;
    const asrtest::Mesh mesh = asrtest::makeTube(256, 64, weights);
    const uint32_t geometry = queue.addGeometry(asrtest::build(mesh, &asrtest::CREATURE_BOX, weights.data()));
    const uint32_t skin = queue.addSkin(asrtest::creatureSkin());
    std::vector<uint32_t> skeletons, bindingIds;
    for (uint32_t i = 0; i < AVATARS; ++i)
    {
        asr::InstanceDesc desc;
        desc.geometry = geometry;
        desc.skin = skin;
        desc.skeleton = queue.addSkeleton(asrtest::CREATURE_JOINTS);
        skeletons.push_back(desc.skeleton);
        queue.addInstance(desc);
    }
    store.update(queue, chunks, 1, LIMIT);
    const uint64_t originalCapacity = store.tableCapacity(asr::SceneTable::SkinnedPositions);
    expect(originalCapacity >= 16 * 1024 * 1024, "the compaction test creates a large output cache");
    for (const asr::SceneWrite& write : store.writes())
        if (write.table == asr::SceneTable::Bindings && write.bytes == sizeof(asr::GpuBinding))
            bindingIds.push_back(uint32_t(write.offset / sizeof(asr::GpuBinding)));
    for (uint32_t i = 0; i < 64; ++i) queue.setSkeletonResident(skeletons[i], false);
    store.update(queue, chunks, 2, LIMIT);
    queue.setSkeletonResident(skeletons[64], false);
    store.update(queue, chunks, 4, LIMIT); // this avatar's old range retires after compaction
    store.update(queue, chunks, 5, LIMIT);
    expect(store.positionsCompacted() && store.tableCapacity(asr::SceneTable::SkinnedPositions) < originalCapacity / 2,
           "parked interior holes force a fresh, smaller physical position buffer");
    const uint64_t vertices = mesh.vertices.size();
    expect(store.rangeLive(asr::SceneTable::SkinnedPositions) == 15 * vertices &&
           store.rangeEnd(asr::SceneTable::SkinnedPositions) == 15 * vertices,
           "only resident bindings occupy the compacted cache");
    expect(store.rangeEnd(asr::SceneTable::SkinBlocks) == store.rangeLive(asr::SceneTable::SkinBlocks) &&
           store.rangeEnd(asr::SceneTable::ClusterBlocks) == store.rangeLive(asr::SceneTable::ClusterBlocks),
           "compaction removes empty dispatch-map holes too");
    uint64_t expectedOffset = 0;
    for (uint32_t i = 65; i < AVATARS; ++i)
    {
        const asr::GpuBinding* binding = writtenEntry<asr::GpuBinding>(store, asr::SceneTable::Bindings, bindingIds[i]);
        expect(binding && binding->positions == expectedOffset, "every surviving binding receives its own dense output span");
        expectedOffset += vertices;
    }
    queue.setSkeletonResident(skeletons[0], true);
    store.update(queue, chunks, 6, LIMIT);
    store.update(queue, chunks, 8, LIMIT);
    expect(store.rangeLive(asr::SceneTable::SkinnedPositions) == 16 * vertices,
           "old-generation retirement cannot free a new allocation after un-parking");
}

void testFrameGraph()
{
    using namespace asr::GraphAccess;

    std::vector<std::string> ran;
    const auto body = [&ran](const char* name) { return [&ran, name]() { ran.push_back(name); return true; }; };
    std::string error;

    // A -> B -> sink, with C making something nobody reads.
    asr::FrameGraph graph;
    const asr::ResultId a = graph.addResult("a", asr::ResultScope::Frame);
    const asr::ResultId b = graph.addResult("b", asr::ResultScope::Frame);
    const asr::ResultId unused = graph.addResult("unused", asr::ResultScope::Frame);
    const asr::PassId makeA = graph.addPass("make a", { { a, ComputeWrite } });
    const asr::PassId makeB = graph.addPass("make b", { { a, ComputeRead }, { b, ComputeWrite } });
    const asr::PassId makeUnused = graph.addPass("make unused", { { b, ComputeRead }, { unused, ComputeWrite } });
    graph.setBody(makeA, body("a"));
    graph.setBody(makeB, body("b"));
    graph.setBody(makeUnused, body("unused"));
    graph.setSink(b, FragmentRead, true);
    expect(graph.compile(error), "a graph compiles");
    expect(graph.passCount() == 2 && !graph.isRunning(makeUnused), "a pass nothing needs does not run");

    graph.setSink(unused, CopyRead, true);
    expect(graph.compile(error) && graph.isRunning(makeUnused), "asking for its result brings it back");
    graph.setSink(unused, CopyRead, false);

    // The barrier into a pass says what it waits for.
    expect(graph.compile(error), "it compiles again");
    uint32_t count = 0;
    const asr::GraphTransition* transitions = graph.transitions(1, count);
    const bool sawWait = std::any_of(transitions, transitions + count, [a](const asr::GraphTransition& t)
                                     { return t.result == a && t.source == ComputeWrite && t.destination == ComputeRead && !t.discard; });
    expect(sawWait, "a read waits for the write before it");
    transitions = graph.transitions(0, count);
    expect(count == 1 && transitions[0].discard, "a result made fresh does not keep its old contents");
    expect((transitions[0].source & ComputeRead) != 0, "and it still waits for the last frame's use of it");

    // Overrides: the replaced pass stops, additions run in order, and readers are unchanged.
    const asr::PassId better = graph.replace(makeA, "better a", { { a, ComputeWrite } });
    const asr::PassId first = graph.insertAfter(better, "first addition", { { a, ComputeWrite | Keep } });
    const asr::PassId second = graph.insertAfter(better, "second addition", { { a, ComputeWrite | Keep } });
    graph.setBody(better, body("better"));
    graph.setBody(first, body("first"));
    graph.setBody(second, body("second"));
    expect(graph.compile(error), "overrides compile");
    for (uint32_t i = 0; i < graph.passCount(); ++i)
    {
        expect(graph.run(i), "passes run");
    }
    const std::vector<std::string> wanted = { "better", "first", "second", "b" };
    expect(ran == wanted, "a replaced pass stops, and additions stack in order");

    // A result with two producers is an error, and so is one with none.
    asr::FrameGraph doubled;
    const asr::ResultId twice = doubled.addResult("twice", asr::ResultScope::Frame);
    const asr::ResultId between = doubled.addResult("between", asr::ResultScope::Frame);
    const asr::ResultId end = doubled.addResult("end", asr::ResultScope::Frame);
    doubled.addPass("one", { { twice, ComputeWrite } });
    doubled.addPass("reader", { { twice, ComputeRead }, { between, ComputeWrite } });
    doubled.addPass("two", { { twice, ComputeWrite } });
    doubled.addPass("finish", { { twice, ComputeRead }, { between, ComputeRead }, { end, ComputeWrite } });
    doubled.setSink(end, CopyRead, true);
    expect(!doubled.compile(error), "two producers of one result are refused");

    asr::FrameGraph orphan;
    const asr::ResultId missing = orphan.addResult("missing", asr::ResultScope::Frame);
    const asr::ResultId made = orphan.addResult("made", asr::ResultScope::Frame);
    orphan.addPass("reader", { { missing, ComputeRead }, { made, ComputeWrite } });
    orphan.setSink(made, CopyRead, true);
    expect(!orphan.compile(error), "reading what nothing makes is refused");

    // What only the next frame reads is still made, and the reader's wait comes from it.
    asr::FrameGraph loop;
    const asr::ResultId kept = loop.addResult("kept", asr::ResultScope::Persistent);
    const asr::ResultId shown = loop.addResult("shown", asr::ResultScope::Frame);
    loop.addPass("use last frame", { { kept, ComputeRead | PreviousFrame }, { shown, ComputeWrite } });
    const asr::PassId keep = loop.addPass("make for next frame", { { shown, ComputeRead }, { kept, ComputeWrite } });
    loop.setSink(shown, FragmentRead, true);
    expect(loop.compile(error) && loop.isRunning(keep), "a result for the next frame is made");
    transitions = loop.transitions(0, count);
    const bool sawLoop = std::any_of(transitions, transitions + count, [kept](const asr::GraphTransition& t)
                                     { return t.result == kept && t.source == ComputeWrite; });
    expect(sawLoop, "the first pass waits for the last frame's write");

    // A kept result is made once, until something changes it.
    asr::FrameGraph cached;
    const asr::ResultId shadow = cached.addResult("shadow", asr::ResultScope::UntilChanged);
    const asr::ResultId lit = cached.addResult("lit", asr::ResultScope::Frame);
    const asr::PassId drawShadow = cached.addPass("shadow", { { shadow, ComputeWrite } });
    const asr::PassId light = cached.addPass("light", { { shadow, ComputeRead }, { lit, ComputeWrite } });
    uint32_t shadowRuns = 0;
    uint32_t lightRuns = 0;
    cached.setBody(drawShadow, [&shadowRuns]() { ++shadowRuns; return true; });
    cached.setBody(light, [&lightRuns]() { ++lightRuns; return true; });
    cached.setSink(lit, FragmentRead, true);
    expect(cached.compile(error), "a graph with a kept result compiles");
    const auto frame = [&cached]()
    {
        for (uint32_t i = 0; i < cached.passCount(); ++i)
        {
            cached.run(i);
        }
    };
    frame();
    frame();
    expect(shadowRuns == 1 && lightRuns == 2, "a kept result is not made again");
    cached.invalidate(shadow);
    frame();
    expect(shadowRuns == 2, "until it changes");

    // Scene passes as declared: a normal frame leaves the audit out, and asking brings it in.
    asr::FrameGraph scene;
    asr::declareSceneGraph(scene);
    expect(scene.compile(error), "the scene graph compiles");
    if (!error.empty())
    {
        printf("      %s\n", error.c_str());
    }
    expect(scene.isRunning(asr::PassPyramidFinal), "the final pyramid runs, for the next frame");
    expect(!scene.isRunning(asr::PassAuditDraw), "the audit costs nothing unless asked for");
    const auto orderOf = [&scene](asr::PassId pass)
    {
        for (uint32_t i = 0; i < scene.passCount(); ++i) if (scene.passId(i) == pass) return i;
        return scene.passCount();
    };
    expect(orderOf(asr::PassSkinClusters) < orderOf(asr::PassCullEarly) &&
           orderOf(asr::PassGroupsEarly) < orderOf(asr::PassSkinVertices) &&
           orderOf(asr::PassSkinVertices) < orderOf(asr::PassDrawEarly),
           "conservative bounds and culling precede early deformation and drawing");
    expect(orderOf(asr::PassPyramidEarly) < orderOf(asr::PassGroupsLate) &&
           orderOf(asr::PassGroupsLate) < orderOf(asr::PassSkinVerticesLate) &&
           orderOf(asr::PassSkinVerticesLate) < orderOf(asr::PassDrawLate),
           "current-depth recovery deforms newly exposed geometry before its same-frame draw");
    transitions = scene.transitions(orderOf(asr::PassSkinVertices), count);
    expect(std::any_of(transitions, transitions + count, [](const asr::GraphTransition& transition)
    {
        return transition.result == asr::ResultSceneData && (transition.source & ComputeWrite) && (transition.destination & ComputeRead);
    }), "culling demand writes are synchronized before deformation reads them");
    scene.setSink(asr::ResultAuditPixels, CopyRead, true);
    expect(scene.compile(error) && scene.isRunning(asr::PassAuditDraw), "asking for the audit runs it");
    expect(scene.passCount() <= asr::MAX_TIMED_PASSES, "every scene pass can be timed");
}

void testPyramidLayout()
{
    uint32_t offsets[asr::PYRAMID_MAX_LEVELS];
    uint32_t levels = 0;
    expect(asr::pyramidLayout(5, 3, offsets, levels) == 6 + 2 + 1 && levels == 3, "pyramid sizes round up to one texel");
    expect(offsets[1] == 6 && offsets[2] == 8, "levels follow each other");
    expect(asr::pyramidLevelSize(5, 1) == 3 && asr::pyramidLevelSize(5, 3) == 1 && asr::pyramidLevelSize(1440, 4) == 90, "level sizes are exact");
    expect(asr::pyramidLayout(11500, 11500, offsets, levels) > 0 && levels == 14, "the largest snapshot fits");
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
        textures.upload(mMipped, 0, 0, 0, MIPPED_SIZE, MIPPED_SIZE, stripes.data(), stripes.size());
        textures.generateMips(mMipped);

        // The same image made smaller from its own mips, and drawn right after them:
        // it reads evenly dark red, and black would mean the copy did not happen.
        asr::TextureDesc shrunkDesc = mippedDesc;
        shrunkDesc.width = MIPPED_SIZE / 4;
        shrunkDesc.height = MIPPED_SIZE / 4;
        shrunkDesc.mipLevels = 5;
        mShrunk = textures.create(shrunkDesc, mMipped, 2);
    }

    // As render_hud_elements(): a 50 m wire box 60 m ahead of the camera, cutting through
    // the city, in the world view's
    // viewport, drawn twice as a manipulator is: where it shows (GL's LEQUAL) solid,
    // where the scene hides it (GL_GREATER) faint. Then the HUD mark.
    void buildWorld(asr::UIList& list, uint32_t height, const asr::SceneCamera3D& camera, const uint32_t worldView[4])
    {
        list.beginWorld(camera.nearClip);
        list.setViewport(int32_t(worldView[0]), int32_t(height) - int32_t(worldView[1] + worldView[3]), int32_t(worldView[2]), int32_t(worldView[3]));
        const float* r = camera.viewRows[2];
        const float centre[3] = { camera.position[0] - r[0] * 60.f, camera.position[1] - r[1] * 60.f, camera.position[2] - r[2] * 60.f };
        asr::UIVertex lines[24];
        for (uint32_t edge = 0; edge < 12; ++edge)
        {
            // Each edge: an axis, and the two other coordinates' signs.
            const uint32_t axis = edge / 4;
            for (uint32_t end = 0; end < 2; ++end)
            {
                float p[3];
                p[axis] = end ? 25.f : -25.f;
                p[(axis + 1) % 3] = (edge & 1) ? 25.f : -25.f;
                p[(axis + 2) % 3] = (edge & 2) ? 25.f : -25.f;
                const float* m = camera.viewProjection;
                const float x = centre[0] + p[0], y = centre[1] + p[1], z = centre[2] + p[2];
                asr::UIVertex& v = lines[edge * 2 + end];
                v = uiVertex(0, 0, 0);
                v.position[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
                v.position[1] = -(m[1] * x + m[5] * y + m[9] * z + m[13]); // the renderer's Y down to GL's up
                v.position[2] = 0.f;                                      // rewritten: depth tested
                v.position[3] = m[3] * x + m[7] * y + m[11] * z + m[15];
                v.texture = asr::uiTextureBits(0);
            }
        }
        list.setDepthTest(true);
        list.setDepthWrite(false);
        list.setDepthCompare(asr::UICompare::LessOrEqual);
        for (asr::UIVertex& v : lines)
        {
            v.color = 0xFF00FFFF; // yellow, RGBA8 little-endian
        }
        list.append(asr::UIGeometry::Lines, lines, 24);
        list.setDepthCompare(asr::UICompare::Greater);
        for (asr::UIVertex& v : lines)
        {
            v.color = 0x5000FFFF;
        }
        list.append(asr::UIGeometry::Lines, lines, 24);
        list.setDepthTest(false);
        list.setDepthWrite(true);
        list.setViewport(0, 0, 0, 0);
        list.markHud();
    }

    void build(asr::UIList& list, uint32_t width, uint32_t height, float seconds, uint64_t listGeneration,
               const asr::SceneCamera3D* world = nullptr, const uint32_t* worldView = nullptr)
    {
        list.clear();
        list.setSize(width, height);
        mFlushes = 0;
        if (world)
        {
            buildWorld(list, height, *world, worldView);
        }

        // Pixels to GL clip space, as the viewer's 2D setup does.
        const float matrix[16] = { 2.f / width, 0, 0, 0, 0, 2.f / height, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1 };

        // The scene shows through where the world part is drawn: no dense grid over it.
        const int columns = world ? 0 : 120;
        const int rows = world ? 0 : 60;
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
        quad(list, 440.f, 500.f, 16.f + 8.f * sinf(seconds), 60.f, asr::uiTextureBits(mShrunk), 0xFFFFFFFF, matrix);

        // A texture replaced now and then, so destruction runs under a live list, and every
        // other time in place, another size behind the same slot, as the viewer resizes.
        if (++mBuilds % 30 == 0)
        {
            const uint32_t index = (mBuilds / 30) % TEXTURES;
            const bool inPlace = (mBuilds / 30) % 2 == 1;
            asr::TextureDesc desc;
            desc.width = inPlace && (mBuilds / 60) % 2 == 0 ? 32 : 16;
            desc.height = desc.width;
            if (inPlace)
            {
                mTextures.replace(mSlots[index], desc);
            }
            else
            {
                mTextures.destroy(mSlots[index], listGeneration);
                mSlots[index] = mTextures.create(desc);
            }
            const std::vector<uint32_t> pixels(desc.width * desc.height, 0xFF00C0FF);
            mTextures.upload(mSlots[index], 0, 0, 0, desc.width, desc.height, pixels.data(), pixels.size() * sizeof(uint32_t));
            if (inPlace)
            {
                mTextures.publish(mSlots[index]);
            }
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
    uint32_t mShrunk = 0;
    uint32_t mFlushes = 0;
    uint32_t mBuilds = 0;
    uint32_t mReplaced = 0;
};

int drawTriangle(double seconds, double stallSeconds, bool drawUI, bool drawScene, bool street, bool audit, bool keepSize, uint32_t windowWidth, uint32_t windowHeight,
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

    asr::RenderThread renderThread(*device, nullptr);
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
            state.audit = audit;
            if (sOverlays)
            {
                const float colors[3][4] = { { 1.f, 1.f, 0.f, 1.f }, { 0.f, 0.6f, 1.f, 1.f }, { 1.f, 1.f, 1.f, 0.35f } };
                // A world view short of the window, as a toolbar keeping its space leaves it.
                state.worldView[0] = 40;
                state.worldView[1] = 30;
                state.worldView[2] = state.width > 120 ? state.width - 80 : state.width;
                state.worldView[3] = state.height > 140 ? state.height - 100 : state.height;
                state.overlay.hud = true;
                state.overlay.outlines = true;
                state.overlay.faceHighlights = true;
                state.overlay.hiddenSelections = true;
                std::memcpy(state.overlay.selectionColors, colors, sizeof(colors));
                asrtest::TestCity::hudViewProjection(float(state.width) / float(std::max(state.height, 1u)), state.overlay.hudViewProjection);
            }
        }
        renderThread.publishState();
    };
    if (drawScene)
    {
        city = std::make_unique<asrtest::TestCity>(device->scene(), device->textures(), street);
        if (sOverlays)
        {
            city->showOverlays();
        }
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
        // With overlays, the world part as the viewer draws it, at the window's size only.
        const asr::FrameState& state = renderThread.state();
        const bool world = sOverlays && drawScene && state.hasCamera && phase == 0;
        testUI.build(list, width, height, float(secondsSince(uiStart)), renderThread.currentUIGeneration(),
                     world ? &state.camera : nullptr, state.worldView);
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

    // What texture feedback brought back, taken as the viewer takes it.
    asr::TextureFeedback feedback;
    uint32_t feedbackWindows = 0;
    size_t mostSlotsSeen = 0;
    uint32_t highestLevel = 0;
    bool groundSeen = false;

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
                if (device->takeTextureFeedback(feedback))
                {
                    feedbackWindows += feedback.windows;
                    mostSlotsSeen = std::max(mostSlotsSeen, feedback.slots.size());
                    for (size_t i = 0; i < feedback.slots.size(); ++i)
                    {
                        groundSeen = groundSeen || feedback.slots[i] == city->groundTexture();
                        highestLevel = std::max(highestLevel, uint32_t(feedback.levels[i]));
                    }
                }
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
        printf("scene: %u instances, %.1f M triangles, %llu clusters drawn early, %llu late, %llu retested, %llu groups, %u chunks, %llu geometries waiting\n",
               stats.instances, stats.triangles / 1e6, (unsigned long long)stats.earlyClusters, (unsigned long long)stats.lateClusters,
               (unsigned long long)stats.retestEntries, (unsigned long long)stats.groupEntries, stats.chunks, (unsigned long long)stats.pendingGeometries);
        if (stats.guardPixels != 0)
        {
            printf("resolve guards: bits %u, %llu pixels, largest bad value %u\n", stats.guardBits, (unsigned long long)stats.guardPixels, stats.guardValue);
        }
        expect(stats.guardPixels == 0, "the resolve reads nothing out of place");
        expect(stats.droppedEntries == 0, "no cull list was full");
        double total = 0.0;
        for (uint32_t i = 0; i < stats.passCount; ++i)
        {
            const asr::PassTime& pass = stats.passes[i];
            printf("  %-16s %6.2f ms", pass.name, pass.milliseconds);
            if (pass.vertexMilliseconds > 0.0)
            {
                printf("  (vertices %.2f ms)", pass.vertexMilliseconds);
            }
            printf("\n");
            total += pass.milliseconds;
        }
        printf("  %-16s %6.2f ms\n", "GPU total", total);
        printf("moved %u objects, replaced %u geometries\n", city->moved(), city->replaced());
        printf("texture feedback: %u windows, at most %zu textures seen at once, sharpest level %u\n", feedbackWindows, mostSlotsSeen, highestLevel);
        expect(feedbackWindows > 0, "texture feedback comes back");
        expect(groundSeen, "the ground's texture is seen");
        expect(highestLevel <= asr::FEEDBACK_MAX_LEVEL, "feedback levels stay in range");
        expect(stats.earlyClusters > 0, "the camera sees part of the scene");
        expect(stats.pendingGeometries == 0, "all geometry reached the GPU");
        expect(city->replaced() > 0, "geometry was replaced while drawing");
        if (audit)
        {
            printf("audit: %llu frames, %llu pixels lost\n", (unsigned long long)stats.auditedFrames, (unsigned long long)stats.auditErrorPixels);
            expect(stats.auditedFrames > 0, "the audit ran");
            expect(stats.auditErrorPixels == 0, "occlusion culling lost nothing");
            printf("skin audit: %llu samples, %llu errors, maximum %.9g metres\n",
                   (unsigned long long)stats.skinAuditSamples, (unsigned long long)stats.skinAuditErrors, stats.skinAuditMaxError);
            expect(stats.skinAuditSamples > 0, "actual GPU skinned positions were checked");
            expect(stats.skinAuditErrors == 0, "stored skinned positions match direct float skinning");
        }
    }

    printMemory(device->memoryBudget());
    const bool countsSwapchain = device->memoryBudget().used(asr::MemoryCategory::RenderTargets) > 0;
    renderThread.stop();

    expect(ran, "ran to the end");
    expect(countsSwapchain, "budget counts the window's images");
    expect(!asrtest::glContextIsCurrent(), "no GL context after rendering");
    expect(device->validationErrors() == 0, "no validation errors");
    const std::string gpuErrors = device->takeGpuErrors();
    if (!gpuErrors.empty())
    {
        printf("%s\n", gpuErrors.c_str());
    }
    expect(gpuErrors.empty(), "no GPU work ended in error");
    device.reset();
    return 0;
}
#endif

} // namespace

#ifdef __APPLE__
int runMouseProbe(double seconds); // asrtestmouseprobe.mm
#endif

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    bool selftest = false;
    double seconds = 10.0;
#ifdef ASR_HAS_DEVICE
    double stallSeconds = 2.0;
    bool drawUI = false;
    bool drawScene = false;
    bool vramStress = false;
    bool encodeTest = false;
    asrtest::VramTestMode vramMode = asrtest::VramTestMode::Overcommit;
    bool street = false;
    bool audit = false;
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
        else if (arg == "--vram-stress")
        {
            vramStress = true;
            vramMode = asrtest::VramTestMode::Overcommit;
        }
        else if (arg == "--vram-managed")
        {
            vramStress = true;
            vramMode = asrtest::VramTestMode::ManagedTextures;
        }
        else if (arg == "--avatar-sweep")
        {
            vramStress = true;
            vramMode = asrtest::VramTestMode::AvatarSweep;
        }
        else if (arg == "--encode")
        {
            encodeTest = true;
        }
        else if (arg == "--scene")
        {
            drawScene = true;
        }
        else if (arg == "--street")
        {
            street = true;
        }
        else if (arg == "--overlay")
        {
            sOverlays = true; // selections and HUDs, see TestCity::showOverlays()
        }
        else if (arg == "--audit")
        {
            audit = true;
        }
        else if (arg == "--no-occlusion")
        {
            options.occlusion = false;
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
#ifdef __APPLE__
        else if (arg == "--mouse-probe")
        {
            return runMouseProbe(seconds);
        }
#endif
        else
        {
            printf("unknown option %s\n", arg.c_str());
            return 2;
        }
    }

#ifdef ASR_HAS_DEVICE
    if (vramStress) return asrtest::runVramStress(options, seconds, windowWidth, windowHeight, vramMode);
    if (encodeTest) return asrtest::runEncodeTest(options);
#endif
    if (selftest)
    {
        testSceneFile("asrenderer_test_scene.tmp");
        testLatestValue();
        testMemoryBudget();
        testUIList();
        testTextureQueue();
        testSceneGeometry();
        testTerrainLods();
        testClusterGroups();
        testRangeAllocator();
        testMeshLodResidency();
        testTextureStreamingPolicy();
        testCameraPredictor();
        testMouseMotion();
        testCameraTurn();
        testCameraZoom();
        testTextureFeedback();
        testSceneStore();
        testJointPrediction();
        testSkinAmounts();
        testSkinStore();
        testDeformationCompaction();
        testMacOITKeys();
        testFrameGraph();
        testPyramidLayout();
    }
    else
    {
#ifdef ASR_HAS_DEVICE
        if (drawTriangle(seconds, stallSeconds, drawUI, drawScene, street, audit, keepSize, windowWidth, windowHeight, options) != 0)
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
