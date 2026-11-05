/**
 * asrrenderthread.cpp
 * See asrrenderthread.h.
 */
#include "asrrenderthread.h"

namespace asr
{

namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::chrono::seconds MEMORY_REFRESH_PERIOD(1);
constexpr std::chrono::milliseconds MINIMIZED_SLEEP(5);

uint64_t micros(Clock::duration duration)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

// Only the render thread raises the value, and takeStats() only zeroes it, so a
// lost update costs one reading at worst.
void raiseTo(std::atomic<uint64_t>& worst, uint64_t value)
{
    if (value > worst.load(std::memory_order_relaxed))
    {
        worst.store(value, std::memory_order_relaxed);
    }
}

} // namespace

void RenderThread::start()
{
    mStop.store(false, std::memory_order_relaxed);
    mThread = std::thread(&RenderThread::run, this);
}

void RenderThread::stop()
{
    if (mThread.joinable())
    {
        mStop.store(true, std::memory_order_relaxed);
        mThread.join();
    }
}

void RenderThread::publishState()
{
    mState.writeSlot().published = Clock::now();
    mState.publish();
}

void RenderThread::publishUI()
{
    mUI.writeSlot().generation = ++mUIGeneration;
    mUI.publish();
}

FrameStats RenderThread::takeStats()
{
    FrameStats stats;
    stats.frames = mFrames.exchange(0, std::memory_order_relaxed);
    stats.worstFrameSeconds = mWorstFrameMicros.exchange(0, std::memory_order_relaxed) * 1e-6;
    stats.worstStateAgeSeconds = mWorstAgeMicros.exchange(0, std::memory_order_relaxed) * 1e-6;
    return stats;
}

void RenderThread::run()
{
    const Clock::time_point start = Clock::now();
    Clock::time_point previousFrame = start;
    Clock::time_point lastMemoryRefresh = start;

    while (!mStop.load(std::memory_order_relaxed))
    {
        const FrameState& state = mState.read();
        if (state.width == 0 || state.height == 0)
        {
            // Nothing to present into: no state yet, or the window is minimized.
            std::this_thread::sleep_for(MINIMIZED_SLEEP);
            previousFrame = Clock::now();
            continue;
        }

        const Clock::time_point frameStart = Clock::now();
        FrameInput input;
        input.width = state.width;
        input.height = state.height;
        input.seconds = std::chrono::duration<float>(frameStart - start).count();
        input.testTriangle = state.testTriangle;
        input.ui = &mUI.read();
        input.camera = state.hasCamera ? &state.camera : nullptr;
        if (!mDevice.renderFrame(input))
        {
            mError = mDevice.lastError();
            mFailed.store(true, std::memory_order_release);
            return;
        }

        const Clock::time_point frameEnd = Clock::now();
        raiseTo(mWorstFrameMicros, micros(frameEnd - previousFrame));
        raiseTo(mWorstAgeMicros, micros(frameStart - state.published));
        previousFrame = frameEnd;
        mFrames.fetch_add(1, std::memory_order_relaxed);

        if (frameEnd - lastMemoryRefresh >= MEMORY_REFRESH_PERIOD)
        {
            lastMemoryRefresh = frameEnd;
            mDevice.refreshMemoryBudget();
        }
    }
}

} // namespace asr
