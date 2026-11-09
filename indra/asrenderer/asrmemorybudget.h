/**
 * asrmemorybudget.h
 * Owns the video memory total and splits it into fixed categories (section 6a of
 * the renderer plan). All allocations reserve here first. Any thread can call it,
 * and it takes no lock.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

namespace asr
{

// Leave space for the desktop and apps launched between driver budget samples.
// On small budgets, keep at least half available to the renderer.
inline constexpr uint64_t workingVRAMBudget(uint64_t driverBudget, uint64_t driverUsage,
                                            uint64_t residentBytes, uint64_t configuredCap = 0)
{
    constexpr uint64_t MIN_HEADROOM = 512ull * 1024 * 1024;
    const uint64_t headroom = std::min(driverBudget / 2, std::max(driverBudget / 10, MIN_HEADROOM));
    uint64_t ceiling = driverBudget - headroom;
    if (configuredCap != 0) ceiling = std::min(ceiling, configuredCap);
    const uint64_t overhead = driverUsage > residentBytes ? driverUsage - residentBytes : 0;
    return ceiling > overhead ? ceiling - overhead : 0;
}

// Textures steer to this share of the working budget; the rest absorbs replacements
// and what is counted past the budget (screen, scene tables, geometry).
inline constexpr uint64_t TEXTURE_STEERING_PERCENT = 85;

enum class MemoryCategory : uint32_t
{
    RenderTargets,
    Shadows,
    Probes,
    Geometry,
    Textures, // takes what the other categories leave
    OIT,
    UI,
    Staging,
    Count
};

enum class MemoryPressure
{
    None,
    LowerMips, // at 85% of the budget: textures drop mips, far and unseen first
    HoldLoads  // at 95% of the budget: new loads wait
};

const char* memoryCategoryName(MemoryCategory category);

class MemoryBudget
{
public:
    // Sets the total, and from it each category's limit. Called again whenever
    // the driver reports a new figure.
    void setBudget(uint64_t bytes);
    uint64_t budget() const { return mBudget.load(std::memory_order_relaxed); }

    uint64_t limit(MemoryCategory category) const;
    uint64_t used(MemoryCategory category) const;

    // What textures may take now. Other categories
    // leave their share unused until something needs it, so textures may use what is really
    // free inside the working budget. As the others grow this falls by itself, and textures shrink to
    // make room, so nothing depends on the size of the card.
    uint64_t textureLimit() const;
    uint64_t totalUsed() const { return mTotalUsed.load(std::memory_order_relaxed); }

    // Returns false, and changes nothing, if the category has no room left. Geometry is
    // the one category the world sizes rather than the screen (a crowd of avatars), so past
    // its share it borrows what is really free, never the textures' share. textureLimit()
    // counts the borrowed memory, so textures shrink to give the reserve back.
    bool reserve(MemoryCategory category, uint64_t bytes);
    // Counts the memory even past the limit. For a caller whose own policy decides,
    // and for what has to work when memory is short, such as shrinking a texture.
    void reserveAnyway(MemoryCategory category, uint64_t bytes);
    void release(MemoryCategory category, uint64_t bytes);

    // The error for a refused reserve(): what asked, how much, and the figures it was refused on.
    std::string refusal(const char* what, MemoryCategory category, uint64_t bytes) const;

    MemoryPressure pressure() const;

    // The most one frame may send to the GPU, in textures and in scene data each. It is
    // set so that every frame in flight can stage both without leaving the staging
    // category, so a burst of loads can only take longer, never fail.
    uint64_t uploadBytesPerFrame(uint32_t framesInFlight) const;

    // The texture upload ring: the textures' half of that staging. One frame copies at
    // most uploadRingPieceBytes() of it, so the main thread can write a frame's worth
    // ahead while every frame in flight still holds its own.
    uint64_t uploadRingBytes(uint32_t framesInFlight) const { return uploadBytesPerFrame(framesInFlight) * framesInFlight; }
    uint64_t uploadRingPieceBytes(uint32_t framesInFlight) const { return uploadRingBytes(framesInFlight) / (framesInFlight + 1); }

private:
    bool canBorrow(uint64_t bytes) const;

    static constexpr uint32_t COUNT = uint32_t(MemoryCategory::Count);

    std::atomic<uint64_t> mBudget{ 0 };
    std::atomic<uint64_t> mTotalUsed{ 0 };
    std::atomic<uint64_t> mLimit[COUNT]{};
    std::atomic<uint64_t> mUsed[COUNT]{};
};

} // namespace asr
