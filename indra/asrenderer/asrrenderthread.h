/**
 * asrrenderthread.h
 * The free-running render thread of section 0 of the renderer plan. It renders
 * from the newest published state and never waits for the main thread, so a
 * main-thread stall cannot freeze the view. After start(), only this thread
 * touches the device, until stop().
 */
#pragma once

#include "asrdevice.h"
#include "asrlatestvalue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace asr
{

// What the main thread hands over. Later phases add the camera, motion and UI.
struct FrameState
{
    std::chrono::steady_clock::time_point published{};
    uint32_t width = 0; // drawable size in pixels
    uint32_t height = 0;
};

struct FrameStats
{
    uint64_t frames = 0;
    double worstFrameSeconds = 0.0;
    double worstStateAgeSeconds = 0.0; // how old the newest state was, at its worst
};

class RenderThread
{
public:
    explicit RenderThread(Device& device) : mDevice(device) {}
    ~RenderThread() { stop(); }

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    void start();
    void stop();

    // Main thread: fill in state(), then publish it. Neither call ever waits.
    FrameState& state() { return mState.writeSlot(); }
    void publishState();

    // Main thread: stats since the previous call.
    FrameStats takeStats();

    // True once rendering has stopped on an unrecoverable error.
    bool failed() const { return mFailed.load(std::memory_order_acquire); }

    // Valid once failed() is true.
    const std::string& error() const { return mError; }

private:
    void run();

    Device& mDevice;
    LatestValue<FrameState> mState;
    std::thread mThread;
    std::atomic<bool> mStop{ false };
    std::atomic<bool> mFailed{ false };
    std::string mError;

    std::atomic<uint64_t> mFrames{ 0 };
    std::atomic<uint64_t> mWorstFrameMicros{ 0 };
    std::atomic<uint64_t> mWorstAgeMicros{ 0 };
};

} // namespace asr
