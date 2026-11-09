/**
 * asrdevice.h
 * The renderer's connection to the GPU: one interface, with a Vulkan backend on
 * Windows and a native Metal backend on macOS. It draws the scene, the UI and the
 * test triangle so far, and grows into the full backend layer of section 2 of
 * doc/ayanestorm-new-renderer-plan.md. Create it on one thread and use it on
 * another if needed, but only one thread may use it at a time, apart from
 * textures(), scene() and sceneStats().
 */
#pragma once

#include "asrmemorybudget.h"
#include "asrscene.h"
#include "asrtexturequeue.h"
#include "asruilist.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace asr
{

// The platform window to present into.
struct SurfaceTarget
{
    void* nativeWindow = nullptr;   // HWND on Windows, NSView* on macOS
    void* nativeInstance = nullptr; // HINSTANCE on Windows, unused on macOS
};

struct DeviceOptions
{
    std::string shaderDir;   // folder holding the compiled shaders
    bool validation = false; // Vulkan validation layers, when installed, GPU-assisted checks included
    void (*report)(const char* message) = nullptr; // where validation errors go; stderr when null
    bool vsync = false;      // off by default: the renderer has no built-in cap
    uint64_t memoryCapBytes = 0; // lowers the memory budget; 0 means no cap
    bool occlusion = true;   // off: for measuring what occlusion culling saves
};

struct DeviceInfo
{
    std::string backend;
    std::string vendor; // named as GL drivers name it, which the viewer's GPU checks expect
    std::string name;
    uint64_t memoryBudgetBytes = 0; // Vulkan device-local heap, or Metal working set
};

// Whether this machine can run the renderer, known before any window exists.
struct ProbeResult
{
    bool supported = false;
    std::string reason; // why not, when supported is false
    DeviceInfo info;
};

// One frame's work.
struct FrameInput
{
    uint32_t width = 0; // in pixels; a change of size recreates what depends on it
    uint32_t height = 0;
    float seconds = 0.f;
    bool testTriangle = false;
    const UIList* ui = nullptr; // drawn on top; stays valid during the call
    const SceneCamera3D* camera = nullptr; // the scene is drawn when this is set
    bool audit = false; // also looks for clusters occlusion culling lost
};

constexpr uint32_t MAX_TIMED_PASSES = 32; // every scene pass, audit and feedback included

// One pass of the frame graph. On Vulkan it runs from the end of the last pass to its
// own end, so barriers count too and the passes add up to the frame's GPU time. On Metal
// it is the pass's own span, and passes can overlap, because a tile GPU splits vertices
// and fragments and each runs when it can.
struct PassTime
{
    const char* name = nullptr;
    double milliseconds = 0.0;
    double vertexMilliseconds = 0.0; // Metal draws: the part before fragments start
};

// The newest figures the scene has reported.
struct SceneStats
{
    uint32_t instances = 0;           // instance slots the culling walks
    uint64_t triangles = 0;           // in all instances, before culling
    uint64_t earlyClusters = 0;       // drawn by the first phase
    uint64_t lateClusters = 0;        // found by the second
    uint64_t retestEntries = 0;       // what the first phase left for the second
    uint64_t groupEntries = 0;        // cluster groups tested a thread per cluster, both phases
    uint32_t guardBits = 0;           // temporary: the resolve's failed checks since the start, GUARD_*
    uint64_t guardPixels = 0;         // pixels they stopped, since the start
    uint32_t guardValue = 0;          // the largest bad value seen
    uint64_t droppedEntries = 0;      // turned away by a full cull list, since the start
    uint64_t auditErrorPixels = 0;    // pixels that a wrongly hidden cluster should have covered
    uint64_t auditedFrames = 0;
    uint64_t skinAuditSamples = 0;
    uint64_t skinAuditErrors = 0;
    float skinAuditMaxError = 0.f;
    uint64_t pendingGeometries = 0;
    uint32_t chunks = 0;              // geometry chunks in use
    uint64_t paletteEnd = 0;          // entries a skin job runs over, and those in use
    uint64_t paletteLive = 0;
    uint64_t vertexBlockEnd = 0;
    uint64_t vertexBlockLive = 0;
    uint64_t clusterBlockEnd = 0;
    uint64_t clusterBlockLive = 0;
    // Temporary, for the log: where geometry memory goes. The rest of it is lists.
    uint64_t chunkBytes = 0;              // every geometry chunk
    uint64_t chunkFreeBytes = 0;          // free inside them
    uint64_t tableBytes = 0;              // every scene table's capacity
    uint64_t skinnedPositionBytes = 0;    // of which deformed positions
    uint64_t clusterTableBytes = 0;       // and clusters, sources' and deformed copies'
    uint64_t textureCopiesRebased = 0;    // queued shrink used a different actual resident mip
    uint64_t textureCopiesRejected = 0;   // invalid copies kept their previous published image
    uint32_t passCount = 0;
    PassTime passes[MAX_TIMED_PASSES];
};

// What the GPU saw of textures since the last take: for each slot seen, log2 of the texels
// across it needed at its sharpest. See FEEDBACK_SAMPLES in asrscene.h.
struct TextureFeedback
{
    using Clock = std::chrono::steady_clock;

    Clock::time_point since{}; // when the oldest window in it began
    std::vector<uint32_t> slots;
    std::vector<uint8_t> levels; // one per slot
    uint32_t windows = 0;        // how many windows it holds; 0 is none

    struct Window
    {
        uint32_t number = 0;
        Clock::time_point start{};
    };

    // Render thread: picks the windows. Shared by the backends.
    class Collector
    {
    public:
        // Once a frame with a scene: the value for GpuSceneView::feedback. True when this
        // frame is the last of its window, given in `ended`, whose words are then read back
        // after this frame's resolve.
        bool frame(Clock::time_point now, uint32_t& value, Window& ended);

        // True once each time the words have to be cleared before the next resolve: at
        // the start, and when window numbers wrap.
        bool takeClear();

    private:
        Window mWindow{ 1, {} };
        uint32_t mFrames = 0;
        bool mStarted = false;
        bool mClear = true;
    };

    // Adds what one window saw, from all MAX_TEXTURES words read back: a word belongs to
    // the window when its upper part is the window's number. A slot seen in several
    // windows keeps its largest level.
    void add(const uint32_t* words, const Window& window);

    void clear();

private:
    std::vector<uint32_t> mPosition; // per slot: its index in slots, plus 1, or 0
};

// A window lasts this long, and at least FEEDBACK_SAMPLES frames, so every pixel is seen.
constexpr std::chrono::milliseconds FEEDBACK_WINDOW(100);
constexpr uint64_t FEEDBACK_BYTES = uint64_t(MAX_TEXTURES) * sizeof(uint32_t);

// Where the render thread's time went inside one frame, in seconds. The parts between record
// and submit are inside record. Every figure stays 0 on a backend that does not report them.
struct FrameParts
{
    double waitForGpu = 0.0;
    double acquire = 0.0;
    double prepareUI = 0.0;
    double record = 0.0;
    double retireTextures = 0.0;  // images whose last frame is done, destroyed
    double textureCommands = 0.0; // creates, uploads and mips from the texture queue
    double readResults = 0.0;     // pass times, counts and texture feedback of a finished frame
    double sceneUpdate = 0.0;
    double sceneGrowth = 0.0;     // scene tables and lists made larger
    double sceneStaging = 0.0;    // scene writes copied to the staging buffer
    double submit = 0.0;
    double present = 0.0;
    uint32_t texturesRetired = 0;
    uint32_t texturesCreated = 0;
    uint64_t textureUploadBytes = 0;
    uint32_t textureCommandsLeft = 0; // for later frames, once this one's time or share of the ring ran out
    uint64_t sceneStagingBytes = 0;
};

class Device
{
public:
    // Needs no window, and leaves nothing behind. The viewer calls it before
    // creating its window, to choose between this renderer and OpenGL.
    static ProbeResult probe();

    // Returns null and sets error when the device cannot be created, so the
    // caller can fall back to OpenGL.
    static std::unique_ptr<Device> create(const SurfaceTarget& target, const DeviceOptions& options,
                                          std::string& error);

    virtual ~Device() = default;

    virtual const DeviceInfo& info() const = 0;

    MemoryBudget& memoryBudget() { return mMemoryBudget; }

    // Reads the driver's current figure into memoryBudget(), so the budget
    // follows what other applications are using.
    virtual void refreshMemoryBudget() = 0;

    // The one part of a device the main thread may use while the render thread runs.
    TextureQueue& textures() { return mTextures; }
    SceneQueue& scene() { return mScene; }

    SceneStats sceneStats()
    {
        std::lock_guard<std::mutex> lock(mSceneStatsMutex);
        return mSceneStats;
    }

    // Main thread: what the GPU saw of textures since the last call. False when nothing
    // new came. The vectors are reused, so pass the same one each time.
    bool takeTextureFeedback(TextureFeedback& feedback)
    {
        std::lock_guard<std::mutex> lock(mFeedbackMutex);
        if (mFeedback.windows == 0)
        {
            return false;
        }
        std::swap(feedback, mFeedback);
        mFeedback.clear();
        return true;
    }

    // Draws and presents one frame. Returns false on an unrecoverable error,
    // described by lastError().
    virtual bool renderFrame(const FrameInput& input) = 0;

    // Render thread: the parts of the frame renderFrame() just drew.
    virtual FrameParts frameParts() const { return {}; }

    virtual const std::string& lastError() const = 0;

    // Errors reported by the validation layers. Always 0 on Metal.
    virtual uint32_t validationErrors() const = 0;

    // Any thread: GPU work that ended in error since the last call, with the first few
    // word for word, or empty. Metal only: Vulkan loses the device instead, and says so.
    virtual std::string takeGpuErrors() { return std::string(); }

protected:
    void setDriverBudget(uint64_t bytes)
    {
        const uint64_t allocatorUsed = allocatorUsedBytes.load();
        const uint64_t resident = allocatorUsed != 0 ? allocatorUsed : mMemoryBudget.totalUsed();
        mMemoryBudget.setBudget(workingVRAMBudget(bytes, driverUsageBytes.load(), resident, mMemoryCapBytes));
    }

    // Passes vary from frame to frame, so one frame's time says little. A pass that
    // comes or goes when the graph changes starts again from its own figure.
    static void recordPassTime(PassTime& pass, const char* name, double milliseconds, double vertexMilliseconds)
    {
        constexpr double NEW_WEIGHT = 1.0 / 64.0;
        if (pass.name != name)
        {
            pass.name = name;
            pass.milliseconds = milliseconds;
            pass.vertexMilliseconds = vertexMilliseconds;
            return;
        }
        pass.milliseconds += (milliseconds - pass.milliseconds) * NEW_WEIGHT;
        pass.vertexMilliseconds += (vertexMilliseconds - pass.vertexMilliseconds) * NEW_WEIGHT;
    }

    void publishSceneStats(const SceneStats& stats)
    {
        std::lock_guard<std::mutex> lock(mSceneStatsMutex);
        mSceneStats = stats;
    }

    // Render thread: a window's words have come back.
    void publishTextureFeedback(const uint32_t* words, const TextureFeedback::Window& window)
    {
        std::lock_guard<std::mutex> lock(mFeedbackMutex);
        mFeedback.add(words, window);
    }

    TextureFeedback::Collector mFeedbackCollector;

    MemoryBudget mMemoryBudget;
    TextureQueue mTextures{ &mMemoryBudget };
    SceneQueue mScene;
    std::mutex mSceneStatsMutex;
    SceneStats mSceneStats;
    std::mutex mFeedbackMutex;
    TextureFeedback mFeedback;
    uint64_t mMemoryCapBytes = 0;

public:
    // Temporary, for the log: what the driver says this process uses and may use. Over its
    // budget, the system moves memory out of VRAM and passes on big buffers slow down.
    std::atomic<uint64_t> driverBudgetBytes{ 0 };
    std::atomic<uint64_t> driverUsageBytes{ 0 };
    // Vulkan: what the allocator holds from the driver, and what of it is handed out. The
    // difference is free space inside its blocks, which no limit of ours sees.
    std::atomic<uint64_t> allocatorBlockBytes{ 0 };
    std::atomic<uint64_t> allocatorUsedBytes{ 0 };
};

} // namespace asr
