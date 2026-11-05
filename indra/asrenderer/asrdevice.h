/**
 * asrdevice.h
 * The renderer's connection to the GPU: one interface, with a Vulkan backend on
 * Windows and a native Metal backend on macOS. This first version only draws the
 * test triangle. P2 grows it into the full backend layer of section 2 of
 * doc/ayanestorm-new-renderer-plan.md.
 */
#pragma once

#include <cstdint>
#include <memory>
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
};

struct DeviceInfo
{
    std::string backend;
    std::string name;
    uint64_t memoryBudgetBytes = 0; // Vulkan device-local heap, or Metal working set
};

class Device
{
public:
    // Returns null and sets error when the device cannot be created, so the
    // caller can fall back to OpenGL.
    static std::unique_ptr<Device> create(const SurfaceTarget& target, const DeviceOptions& options,
                                          std::string& error);

    virtual ~Device() = default;

    virtual const DeviceInfo& info() const = 0;

    // Draws and presents one frame. Sizes are in pixels; a change of size
    // recreates what depends on it. Returns false on an unrecoverable error,
    // described by lastError().
    virtual bool renderTestFrame(uint32_t width, uint32_t height, float seconds) = 0;

    virtual const std::string& lastError() const = 0;

    // Errors reported by the validation layers. Always 0 on Metal.
    virtual uint32_t validationErrors() const = 0;
};

} // namespace asr
