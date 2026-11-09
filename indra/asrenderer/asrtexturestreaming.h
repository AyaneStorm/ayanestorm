/**
 * asrtexturestreaming.h
 * Texture streaming decisions shared by the viewer and both GPU backends.
 * Compare feedback observations, not time spent waiting for another GPU window;
 * never publish an unfilled mip replacement.
 * Author: chanayane@firestorm
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace asr
{

// A first image must arrive even when full-size decodes were queued before pressure
// began. Select a smaller upload before allocating GPU storage, retaining at least
// a 128-pixel image. Exact halving preserves the viewer's full-size/discard metadata.
inline constexpr uint32_t firstTextureMip(uint32_t width, uint32_t height, uint32_t channels, bool mips, uint64_t available)
{
    if (!mips) return 0;
    uint32_t discard = 0;
    for (;;)
    {
        uint64_t bytes = 0;
        uint32_t w = width, h = height;
        for (;;)
        {
            bytes += uint64_t(w) * h * channels;
            if (w <= 1 && h <= 1) break;
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
        }
        if (bytes <= available || std::max(width, height) <= 128 || width <= 1 || height <= 1 || (width & 1u) || (height & 1u)) return discard;
        width /= 2;
        height /= 2;
        ++discard;
    }
}

// Render-thread transaction state. A failed replacement must not route its upload
// commands into the still-visible old image. Only another create or destruction resets it.
template <uint32_t Slots>
class TextureReplacementState
{
public:
    constexpr void begin(uint32_t slot) { if (slot < Slots) mFailed[slot] = false; }
    constexpr void fail(uint32_t slot) { if (slot < Slots) mFailed[slot] = true; }
    constexpr bool failed(uint32_t slot) const { return slot < Slots && mFailed[slot]; }

private:
    std::array<bool, Slots> mFailed{};
};

inline constexpr bool textureFeedbackSeen(double latestWindow, double sampledWindow, double holdSeconds)
{
    return latestWindow >= 0.0 && sampledWindow >= 0.0 && latestWindow >= sampledWindow && latestWindow - sampledWindow < holdSeconds;
}

// Small avatar details gain one mip each time the camera distance halves.
inline constexpr uint32_t avatarDetailTexelLimit(float distance)
{
    uint32_t texels = 512;
    float boundary = 1.f;
    while (distance >= boundary * 2.f && texels > 64)
    {
        boundary *= 2.f;
        texels /= 2;
    }
    while (distance < boundary && texels < 2048)
    {
        boundary *= 0.5f;
        texels *= 2;
    }
    return texels;
}

inline constexpr int textureDiscardForLimit(uint32_t longest, uint32_t limit, int maxDiscard)
{
    int discard = 0;
    while (longest > limit && discard < maxDiscard)
    {
        longest /= 2;
        ++discard;
    }
    return discard;
}

inline constexpr bool textureMipCopyLevel(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t sourceLevels,
                                uint32_t width, uint32_t height, uint32_t levels, uint32_t& mip)
{
    if (sourceWidth == 0 || sourceHeight == 0 || sourceLevels == 0 || width == 0 || height == 0 || levels == 0) return false;
    // Use the image that actually survived allocation/publication. A queued CPU-relative
    // mip offset can be obsolete after a prior replacement failed under memory pressure.
    for (uint32_t level = 0; level < sourceLevels; ++level)
    {
        if (sourceWidth == width && sourceHeight == height && levels <= sourceLevels - level)
        {
            mip = level;
            return true;
        }
        sourceWidth = std::max(sourceWidth / 2, 1u);
        sourceHeight = std::max(sourceHeight / 2, 1u);
    }
    return false;
}

} // namespace asr
