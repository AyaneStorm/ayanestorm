/**
 * asrtestmain.cpp
 * asrenderer_test: renderer checks that need no viewer build and no login.
 *
 *   asrenderer_test --selftest           portable checks, no window
 *   asrenderer_test [--seconds N]        draw the test triangle for N seconds (default 10)
 *                   [--stall N]          freeze the main thread for N seconds (default 2, 0 for none)
 *                   [--memory-cap-mb N]  lower the video memory budget
 *                   [--validate]         turn on the Vulkan validation layers
 *                   [--vsync]            wait for the display
 *                   [--shaders DIR]      compiled shader folder
 *
 * Exit code 0 means every check passed.
 */
#include "asrlatestvalue.h"
#include "asrmemorybudget.h"
#include "asrscenefile.h"

#ifdef ASR_HAS_DEVICE
#include "asrdevice.h"
#include "asrrenderthread.h"
#include "asrtestwindow.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

int drawTriangle(double seconds, double stallSeconds, const asr::DeviceOptions& options)
{
    const asr::ProbeResult probe = asr::Device::probe();
    printf("probe before any window: %s\n", probe.supported ? probe.info.name.c_str() : probe.reason.c_str());
    expect(probe.supported, "probe accepts this machine");

    asrtest::TestWindow window;
    if (!window.create(1280, 720, "asrenderer_test"))
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
    const auto publishWindowSize = [&]()
    {
        asr::FrameState& state = renderThread.state();
        window.pixelSize(state.width, state.height);
        renderThread.publishState();
    };
    publishWindowSize();
    renderThread.start();

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

    window.resize(900, 600);
    ran = ran && runMainThread(restSeconds);

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
    }
    else
    {
#ifdef ASR_HAS_DEVICE
        if (drawTriangle(seconds, stallSeconds, options) != 0)
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
