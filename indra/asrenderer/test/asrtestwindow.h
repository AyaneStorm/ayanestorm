/**
 * asrtestwindow.h
 * The small platform window of asrenderer_test, with one implementation per
 * platform. The viewer has its own windows and does not use this.
 */
#pragma once

#include "asrdevice.h"

#include <cstdint>

namespace asrtest
{

class TestWindow
{
public:
    bool create(uint32_t width, uint32_t height, const char* title);

    // Handles pending window events. Returns false once the window is closed.
    bool pumpEvents();

    void resize(uint32_t width, uint32_t height);

    // Current drawable size in pixels.
    void pixelSize(uint32_t& width, uint32_t& height) const;

    asr::SurfaceTarget surfaceTarget() const;

private:
    void* mWindow = nullptr;
    void* mView = nullptr;
};

// True if a GL context is current. The new renderer must never create one. GL
// libraries being loaded proves nothing: drivers and system frameworks load them.
bool glContextIsCurrent();

} // namespace asrtest
