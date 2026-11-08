/**
 * asrmousemotion.h
 * The mouse's motion added up as it arrives, on an input thread, so that the main
 * thread and the render thread can both read it and agree on it exactly. The main
 * thread turns its camera by the motion up to some total, publishes that total with
 * the camera, and the render thread turns the published camera by the motion since.
 * See doc/ayanestorm-new-renderer-plan.md, 'Camera input on the render thread'.
 */
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace asr
{

class MouseMotion
{
public:
    // Both axes of a total in one word, so a reader never sees x and y from two
    // different moments. Each is a wrapping count of 1/SUBSTEPS of a unit.
    using Total = uint64_t;
    static constexpr float SUBSTEPS = 64.f;

    // Motion in the units the window reports, x to the right and y up. Only one
    // thread at a time may call this: the platform's input thread.
    void add(float dx, float dy)
    {
        mX += dx;
        mY += dy;
        const uint32_t x = uint32_t(int64_t(std::llround(mX * SUBSTEPS)));
        const uint32_t y = uint32_t(int64_t(std::llround(mY * SUBSTEPS)));
        mTotal.store(Total(x) | (Total(y) << 32), std::memory_order_release);
    }

    // Any thread.
    Total total() const { return mTotal.load(std::memory_order_acquire); }

    // The motion from one total to a later one. The counts wrap, which only matters
    // past millions of units between the two.
    static void between(Total from, Total to, float& dx, float& dy)
    {
        dx = float(int32_t(uint32_t(to) - uint32_t(from))) / SUBSTEPS;
        dy = float(int32_t(uint32_t(to >> 32) - uint32_t(from >> 32))) / SUBSTEPS;
    }

private:
    std::atomic<Total> mTotal{ 0 };
    double mX = 0.0; // input thread only: the exact sums, so rounding never adds up
    double mY = 0.0;
};

#if defined(__APPLE__)
// The Mac's raw motion, from every mouse the GameController framework sees, now or plugged
// in later, added to `motion` on one serial queue. Main thread. nullptr stops it.
void listenToMice(MouseMotion* motion);
#endif

} // namespace asr
