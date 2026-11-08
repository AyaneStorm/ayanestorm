/**
 * asrtexturefeedback.cpp
 * TextureFeedback, from asrdevice.h: what the resolve pass saw of textures.
 */
#include "asrdevice.h"

namespace asr
{

namespace
{

// The place in each 4 by 4 block for each frame of a window, as x | (y << 2), in the
// order of a 4 by 4 Bayer matrix: any run of frames spreads its samples evenly.
constexpr uint32_t PLACES[FEEDBACK_SAMPLES] = { 0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12 };

} // namespace

bool TextureFeedback::Collector::frame(Clock::time_point now, uint32_t& value, Window& ended)
{
    if (!mStarted)
    {
        mStarted = true;
        mWindow.start = now;
    }
    value = (mWindow.number << 4) | PLACES[mFrames % FEEDBACK_SAMPLES];
    ++mFrames;
    if (mFrames < FEEDBACK_SAMPLES || now - mWindow.start < FEEDBACK_WINDOW)
    {
        return false;
    }

    ended = mWindow;
    mFrames = 0;
    mWindow.start = now;
    if (++mWindow.number == FEEDBACK_WINDOW_LIMIT)
    {
        // Old words would outrank new ones, so they go.
        mWindow.number = 1;
        mClear = true;
    }
    return true;
}

bool TextureFeedback::Collector::takeClear()
{
    const bool clear = mClear;
    mClear = false;
    return clear;
}

void TextureFeedback::add(const uint32_t* words, const Window& window)
{
    if (mPosition.empty())
    {
        mPosition.assign(MAX_TEXTURES, 0);
    }
    if (windows == 0)
    {
        since = window.start;
    }
    ++windows;

    // Slot 0 is the white texture, which stands for no texture.
    for (uint32_t slot = 1; slot < MAX_TEXTURES; ++slot)
    {
        const uint32_t word = words[slot];
        if ((word >> FEEDBACK_LEVEL_BITS) != window.number)
        {
            continue;
        }
        const uint8_t level = uint8_t((word & ((1u << FEEDBACK_LEVEL_BITS) - 1)) - 1);
        uint32_t& position = mPosition[slot];
        if (position == 0)
        {
            slots.push_back(slot);
            levels.push_back(level);
            position = uint32_t(slots.size());
        }
        else
        {
            levels[position - 1] = std::max(levels[position - 1], level);
        }
    }
}

void TextureFeedback::clear()
{
    for (uint32_t slot : slots)
    {
        mPosition[slot] = 0;
    }
    slots.clear();
    levels.clear();
    windows = 0;
    since = {};
}

} // namespace asr
