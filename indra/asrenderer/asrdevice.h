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
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

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
    bool validation = false; // Vulkan validation layers, when installed
    bool vsync = false;      // off by default: the renderer has no built-in cap
    uint64_t memoryCapBytes = 0; // lowers the memory budget; 0 means no cap
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
};

// The newest figures the scene has reported. GPU times are 0 where the backend has none.
struct SceneStats
{
    uint32_t instances = 0;           // instance slots the culling walks
    uint64_t triangles = 0;           // in all instances, before culling
    uint64_t visibleClusters = 0;
    uint64_t pendingGeometries = 0;
    uint32_t chunks = 0;              // geometry chunks in use
    double cullMilliseconds = 0.0;
    double visibilityMilliseconds = 0.0;
    double resolveMilliseconds = 0.0;
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

    // Draws and presents one frame. Returns false on an unrecoverable error,
    // described by lastError().
    virtual bool renderFrame(const FrameInput& input) = 0;

    virtual const std::string& lastError() const = 0;

    // Errors reported by the validation layers. Always 0 on Metal.
    virtual uint32_t validationErrors() const = 0;

protected:
    void setDriverBudget(uint64_t bytes)
    {
        mMemoryBudget.setBudget(mMemoryCapBytes ? std::min(bytes, mMemoryCapBytes) : bytes);
    }

    void publishSceneStats(const SceneStats& stats)
    {
        std::lock_guard<std::mutex> lock(mSceneStatsMutex);
        mSceneStats = stats;
    }

    MemoryBudget mMemoryBudget;
    TextureQueue mTextures;
    SceneQueue mScene;
    std::mutex mSceneStatsMutex;
    SceneStats mSceneStats;
    uint64_t mMemoryCapBytes = 0;
};

} // namespace asr
