/**
 * asrmemorybudget.h
 * Owns the video memory total and splits it into fixed categories (section 6a of
 * the renderer plan). All allocations reserve here first. Any thread can call it,
 * and it takes no lock.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace asr
{

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
    uint64_t totalUsed() const { return mTotalUsed.load(std::memory_order_relaxed); }

    // Returns false, and changes nothing, if the category has no room left.
    bool reserve(MemoryCategory category, uint64_t bytes);
    // Counts the memory even past the limit. For a caller whose own policy decides,
    // and for what has to work when memory is short, such as shrinking a texture.
    void reserveAnyway(MemoryCategory category, uint64_t bytes);
    void release(MemoryCategory category, uint64_t bytes);

    MemoryPressure pressure() const;

    // The most one frame may send to the GPU, in textures and in scene data each. It is
    // set so that every frame in flight can stage both without leaving the staging
    // category, so a burst of loads can only take longer, never fail.
    uint64_t uploadBytesPerFrame(uint32_t framesInFlight) const;

private:
    static constexpr uint32_t COUNT = uint32_t(MemoryCategory::Count);

    std::atomic<uint64_t> mBudget{ 0 };
    std::atomic<uint64_t> mTotalUsed{ 0 };
    std::atomic<uint64_t> mLimit[COUNT]{};
    std::atomic<uint64_t> mUsed[COUNT]{};
};

} // namespace asr
