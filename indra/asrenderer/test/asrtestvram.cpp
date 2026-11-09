/**
 * Real Vulkan/Metal allocation pressure, without a viewer build or login.
 * Hold a dense rigged crowd constant, fill VRAM with sampled mipmapped textures,
 * then discard their top mips and verify physical retirement and frame recovery.
 * Author: chanayane@firestorm
 */
#include "asrtestvram.h"
#include "asrtestscene.h"
#include "asrtestwindow.h"
#include "asravatarresidency.h"
#include "asrtexturestreaming.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace asrtest
{
int runVramStress(const asr::DeviceOptions& options, double holdSeconds, uint32_t width, uint32_t height, VramTestMode mode)
{
    using Clock = std::chrono::steady_clock;
    constexpr uint64_t MB = 1024ull * 1024;
    constexpr uint32_t AVATARS = 80;
    constexpr uint32_t FACES = 16;
    TestWindow window;
    if (!window.create(width, height, "asrenderer_test: VRAM fill / discard / recovery")) return 1;
    std::string error;
    std::unique_ptr<asr::Device> device = asr::Device::create(window.surfaceTarget(), options, error);
    if (!device)
    {
        printf("FAIL device: %s\n", error.c_str());
        return 1;
    }
    printf("VRAM stress on %s: %u avatars, %u separate mesh faces each\n", device->info().name.c_str(), AVATARS, FACES);
    const bool managed = mode == VramTestMode::ManagedTextures;
    printf("mode: %s\n", mode == VramTestMode::AvatarSweep ? "crowd camera sweep with simulated memory pressure" :
           managed ? "charge queued work; admit smaller first images under pressure" : "unbounded residency reproduction");
    asr::SceneQueue& scene = device->scene();
    asr::TextureQueue& textures = device->textures();
    std::vector<asr::SkinWeights> weights;
    const Mesh mesh = makeTube(128, 128, weights);
    const asr::GeometryData geometry = build(mesh, &CREATURE_BOX, weights.data());
    const uint32_t skin = scene.addSkin(creatureSkin());
    std::vector<uint32_t> skeletons, materials;
    for (uint32_t avatar = 0; avatar < AVATARS; ++avatar)
    {
        const uint32_t skeleton = scene.addSkeleton(CREATURE_JOINTS);
        skeletons.push_back(skeleton);
        for (uint32_t face = 0; face < FACES; ++face)
        {
            // Unique source storage, as different mesh outfits do not share geometry.
            const uint32_t ownGeometry = scene.addGeometry(asr::GeometryData(geometry));
            asr::MaterialDesc material;
            material.color[0] = 0.4f + float(avatar % 5) * 0.12f;
            const uint32_t id = scene.addMaterial(material);
            materials.push_back(id);
            asr::InstanceDesc instance{};
            instance.geometry = ownGeometry;
            instance.material = id;
            instance.skeleton = skeleton;
            instance.skin = skin;
            instance.lodSize = CREATURE_HEIGHT;
            scene.addInstance(instance);
        }
    }
    const float eye[3] = { 0.f, -12.f, 7.f };
    const float target[3] = { 0.f, 12.f, 2.f };
    const float aspect = float(width) / height;
    constexpr float FOV = 1.2f;
    const asr::SceneCamera3D front = asr::makeCamera(eye, target, FOV, aspect, 0.1f);
    asr::SceneCamera3D camera = front;
    const auto avatarX = [](uint32_t i) { return (float(i % 8) - 3.5f) * 2.f; };
    const auto avatarY = [](uint32_t i) { return float(i / 8) * 2.f; };
    asr::UIList ui;
    ui.setSize(width, height);
    ui.generation = 1;
    const auto start = Clock::now();
    auto refreshAt = start;
    auto poseAt = start - std::chrono::seconds(1);
    const auto elapsed = [&](Clock::time_point from) { return std::chrono::duration<double>(Clock::now() - from).count(); };
    bool okay = true;
    const auto frame = [&]()
    {
        if (!window.pumpEvents()) return false;
        if (elapsed(poseAt) >= 1.0 / 30.0)
        {
            poseAt = Clock::now();
            for (uint32_t avatar = 0; avatar < AVATARS; ++avatar)
            {
                float rows[CREATURE_JOINTS * 12];
                creaturePose(float(elapsed(start)), avatarX(avatar), avatarY(avatar), 0.f, float(avatar), rows);
                scene.poseSkeleton(skeletons[avatar], rows, CREATURE_JOINTS, poseAt);
            }
        }
        textures.flush();
        asr::FrameInput input;
        input.width = width;
        input.height = height;
        input.seconds = float(elapsed(start));
        input.camera = &camera;
        input.ui = &ui;
        if (!device->renderFrame(input))
        {
            printf("FAIL render: %s\n", device->lastError().c_str());
            return false;
        }
        if (elapsed(refreshAt) >= 0.2)
        {
            device->refreshMemoryBudget();
            refreshAt = Clock::now();
        }
        return true;
    };
    const auto report = [&](const char* phase, uint64_t frames, double seconds)
    {
        const asr::SceneStats stats = device->sceneStats();
        double gpu = 0.0;
        for (uint32_t i = 0; i < stats.passCount; ++i) gpu += stats.passes[i].milliseconds;
        printf("%s: %.1f fps, GPU %.2f ms; driver %llu/%llu MB, allocator %llu/%llu MB, tracked %llu MB, textures %llu MB, geometry %llu MB, pending %llu\n",
               phase, frames / std::max(seconds, 0.001), gpu,
               (unsigned long long)(device->driverUsageBytes.load() / MB), (unsigned long long)(device->driverBudgetBytes.load() / MB),
               (unsigned long long)(device->allocatorUsedBytes.load() / MB), (unsigned long long)(device->allocatorBlockBytes.load() / MB),
               (unsigned long long)(device->memoryBudget().totalUsed() / MB),
               (unsigned long long)(device->memoryBudget().used(asr::MemoryCategory::Textures) / MB),
               (unsigned long long)(device->memoryBudget().used(asr::MemoryCategory::Geometry) / MB),
               (unsigned long long)stats.pendingGeometries);
        const asr::FrameParts parts = device->frameParts();
        printf("  last frame: GPU wait %.2f ms, record %.2f ms, present %.2f ms\n", parts.waitForGpu * 1000.0, parts.record * 1000.0, parts.present * 1000.0);
    };
    // Drain the initial crowd before comparing any frame rates.
    uint64_t frames = 0;
    auto since = Clock::now();
    do
    {
        okay = frame();
        ++frames;
    } while (okay && elapsed(since) < 60.0 && (device->sceneStats().pendingGeometries != 0 || elapsed(since) < 5.0));
    report("crowd loaded", frames, elapsed(since));
    if (!okay || device->sceneStats().pendingGeometries != 0) return 1;
    frames = 0;
    since = Clock::now();
    while (okay && elapsed(since) < 5.0) { okay = frame(); ++frames; }
    const double baselineFps = frames / elapsed(since);
    report("baseline", frames, elapsed(since));
    if (!okay) return 1;

    if (mode == VramTestMode::AvatarSweep)
    {
        const float sideTarget[3] = { -24.f, -14.f, 2.f };
        const asr::SceneCamera3D side = asr::makeCamera(eye, sideTarget, FOV, aspect, 0.1f);
        const float halfCorner = std::atan(std::tan(FOV * 0.5f) * std::sqrt(1.f + aspect * aspect));
        const asr::SceneStats baseline = device->sceneStats();
        uint64_t blocks[2]{};
        uint64_t positionBytes[2]{};
        for (uint32_t sweep = 0; sweep < 2 && okay; ++sweep)
        {
            asr::AvatarResidencyPolicy residency;
            double lastWanted[AVATARS]{};
            bool resident[AVATARS];
            std::fill_n(resident, AVATARS, true);
            const auto sample = [&](const asr::SceneCamera3D& view, double now)
            {
                // Hold a fixed synthetic charge so this compares retention at equal load
                // without making the driver page real allocations out of VRAM.
                residency.update(sweep ? 96 : 85, 100, now);
                asr::GpuSceneView gpuView{};
                asr::fillSceneView(gpuView, view, nullptr, width, height);
                for (uint32_t i = 0; i < AVATARS; ++i)
                {
                    const float center[3] = { avatarX(i), avatarY(i), CREATURE_HEIGHT * 0.5f };
                    constexpr float radius = CREATURE_HEIGHT * 0.5f;
                    bool inView = true;
                    for (const auto& p : gpuView.planes)
                    {
                        inView &= p[0] * center[0] + p[1] * center[1] + p[2] * center[2] + p[3] >= -radius;
                    }
                    const float x = center[0] - eye[0], y = center[1] - eye[1], z = center[2] - eye[2];
                    const float distance = std::sqrt(x * x + y * y + z * z);
                    const auto& back = view.viewRows[2];
                    const float forwardDot = -(back[0] * x + back[1] * y + back[2] * z) / distance;
                    const bool wanted = inView || residency.nearView(forwardDot, radius / distance, halfCorner);
                    const bool keep = residency.keepResident(wanted, now, lastWanted[i]);
                    if (keep != resident[i])
                    {
                        resident[i] = keep;
                        scene.setSkeletonResident(skeletons[i], keep);
                    }
                }
            };
            // The same two-second look aside in each run. Retired ranges have time
            // to drain and existing GPU counters measure the resulting scene work.
            camera = side;
            frames = 0;
            since = Clock::now();
            while (okay && elapsed(since) < 2.0)
            {
                sample(camera, std::min(elapsed(since), 2.0));
                okay = frame();
                ++frames;
            }
            report(sweep ? "tight budget, looking aside" : "room in budget, looking aside", frames, elapsed(since));
            const asr::SceneStats aside = device->sceneStats();
            blocks[sweep] = aside.vertexBlockLive;
            positionBytes[sweep] = aside.skinnedPositionBytes;
            printf("  live vertex blocks %llu, deformed position capacity %llu MB\n",
                   (unsigned long long)aside.vertexBlockLive, (unsigned long long)(aside.skinnedPositionBytes / MB));

            camera = front;
            sample(camera, 2.0);
            frames = 0;
            since = Clock::now();
            double slowest = 0.0;
            while (okay && elapsed(since) < 2.0)
            {
                const auto frameStart = Clock::now();
                okay = frame();
                slowest = std::max(slowest, elapsed(frameStart));
                ++frames;
            }
            report("back on crowd", frames, elapsed(since));
            printf("  slowest return frame %.2f ms\n", slowest * 1000.0);
            const asr::SceneStats returned = device->sceneStats();
            const bool restored = returned.vertexBlockLive == baseline.vertexBlockLive &&
                returned.chunkBytes == baseline.chunkBytes && returned.pendingGeometries == 0;
            if (!restored) printf("FAIL returning to the crowd did not restore the original bindings from resident geometry\n");
            okay &= restored;
        }
        const bool reduced = blocks[0] == baseline.vertexBlockLive && blocks[1] < blocks[0] &&
            positionBytes[1] < positionBytes[0];
        printf("%s tighter headroom reduces off-screen skinning allocations during the same camera turn\n", reduced ? "ok" : "FAIL");
        return okay && reduced && device->validationErrors() == 0 ? 0 : 1;
    }

    asr::TextureDesc full;
    full.width = full.height = 2048;
    full.mipLevels = 12;
    asr::TextureDesc small = full;
    small.width = small.height = 128;
    small.mipLevels = 8;
    std::vector<uint32_t> slots;
    auto loadAt = Clock::now() - std::chrono::seconds(1);
    auto loggedAt = Clock::now();
    // Intentionally exceed the driver's residency budget for a bounded experiment.
    const uint64_t fillTarget = device->driverBudgetBytes.load() * 110 / 100;
    const uint64_t imageCount = (fillTarget - std::min(fillTarget, device->memoryBudget().totalUsed())) / asr::textureBytes(full) + 1;
    frames = 0;
    since = Clock::now();
    while (okay && elapsed(since) < 60.0 && slots.size() < imageCount)
    {
        if (elapsed(loadAt) >= 0.1)
        {
            loadAt = Clock::now();
            for (uint32_t n = 0; n < 2 && slots.size() < imageCount; ++n)
            {
                const asr::MemoryBudget& budget = device->memoryBudget();
                const uint64_t limit = std::min(device->info().memoryBudgetBytes / 2, budget.textureLimit());
                // Same asset count and source crowd in both runs. The managed run
                // keeps first images loading instead of exempting them from residency.
                const uint64_t target = limit / 100 * 85;
                const uint64_t used = budget.used(asr::MemoryCategory::Textures);
                const uint32_t drop = managed ? asr::firstTextureMip(full.width, full.height, 4, true, target > used ? target - used : 0) : 0;
                asr::TextureDesc admitted = full;
                admitted.width >>= drop;
                admitted.height >>= drop;
                admitted.mipLevels -= drop;
                const uint32_t slot = textures.create(admitted);
                if (slot == 0) { okay = false; break; }
                asr::MaterialDesc material;
                material.texture = asr::uiTextureBits(slot, asr::UISampler::LinearWrap);
                scene.setMaterial(materials[slots.size() % materials.size()], material);
                slots.push_back(slot);
            }
        }
        okay = okay && frame();
        ++frames;
        if (elapsed(loggedAt) >= 5.0)
        {
            report("filling", frames, elapsed(since));
            loggedAt = Clock::now();
        }
    }
    report("filled", frames, elapsed(since));
    const bool loaded = slots.size() == imageCount;
    if (!loaded) printf("FAIL loaded %zu of %llu requested textures\n", slots.size(), (unsigned long long)imageCount);
    frames = 0;
    since = Clock::now();
    while (okay && elapsed(since) < std::max(holdSeconds, 5.0)) { okay = frame(); ++frames; }
    const uint64_t peakUsage = device->driverUsageBytes.load();
    report("pressure held", frames, elapsed(since));
    const bool keptRate = !managed || frames / elapsed(since) >= baselineFps * 0.5;
    if (!keptRate) printf("FAIL managed loading reduced the frame rate by more than half\n");

    // Real image replacement/copy/publication and in-flight retirement, not a counter reset.
    for (uint32_t slot : slots)
    {
        textures.replace(slot, small, 4);
        textures.publish(slot);
    }
    frames = 0;
    since = Clock::now();
    // Measure drain latency separately: averaging it into the subsequent steady
    // rate would hide whether performance actually recovered once retirement finished.
    const uint64_t expectedSmall = slots.size() * asr::textureBytes(small) + 4;
    while (okay && elapsed(since) < 45.0 && device->memoryBudget().used(asr::MemoryCategory::Textures) > expectedSmall)
    {
        okay = frame();
        ++frames;
    }
    const bool drained = device->memoryBudget().used(asr::MemoryCategory::Textures) <= expectedSmall;
    report("discarded to 128", frames, elapsed(since));
    printf("%s all old texture allocations retired in %.2f seconds\n", drained ? "ok" : "FAIL", elapsed(since));
    frames = 0;
    since = Clock::now();
    while (okay && elapsed(since) < 5.0) { okay = frame(); ++frames; }
    double recoveredFps = frames / elapsed(since);
    report("steady after retirement", frames, elapsed(since));
    const uint64_t recoveredUsage = device->driverUsageBytes.load();
    const bool released = peakUsage != 0 && recoveredUsage < peakUsage * 3 / 4;
    bool recovered = recoveredFps >= baselineFps * 0.5;
    if (okay && !recovered)
    {
        // An overcommitted driver can leave allocations in system memory. Do not
        // mistake its usage counter falling for the hot working set returning to VRAM.
        frames = 0;
        since = Clock::now();
        while (okay && elapsed(since) < 20.0) { okay = frame(); ++frames; }
        report("settled after retirement", frames, elapsed(since));
        recoveredFps = frames / elapsed(since);
        recovered = recoveredFps >= baselineFps * 0.5;
    }
    printf("%s driver memory falls after mip retirement; %s frame rate recovers (baseline %.1f, recovered %.1f fps)\n",
           released ? "ok" : "FAIL", recovered ? "ok" : "FAIL", baselineFps, recoveredFps);
    for (uint32_t slot : slots) textures.destroy(slot, ui.generation);
    ++ui.generation;
    since = Clock::now();
    frames = 0;
    while (okay && elapsed(since) < 5.0) { okay = frame(); ++frames; }
    report("textures destroyed", frames, elapsed(since));
    return okay && loaded && keptRate && drained && released && recovered && device->validationErrors() == 0 ? 0 : 1;
}
} // namespace asrtest
