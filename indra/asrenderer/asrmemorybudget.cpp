/**
 * asrmemorybudget.cpp
 * See asrmemorybudget.h.
 */
#include "asrmemorybudget.h"

namespace asr
{

namespace
{

// Share of the budget in thousandths. Starting values: the tiers of section 6a
// replace them as each category gets real users, tested at 8, 6 and 4 GB.
// Textures have no share, because they take what is left.
// Uploads need a working minimum, however small the budget is.
constexpr uint64_t MINIMUM_STAGING_BYTES = 48ull * 1024 * 1024;
constexpr uint64_t MAXIMUM_UPLOAD_BYTES = 64ull * 1024 * 1024;
constexpr uint64_t SMALL_STAGING_BYTES = 16ull * 1024 * 1024; // kept for per-frame views and readbacks

// Of the whole budget, kept out of textures' reach for what the others may need at short
// notice. It keeps total use under the 85% of pressure(), which starts dropping mips.
constexpr uint64_t TEXTURE_RESERVE_PERCENT = 25;

constexpr uint64_t SHARE_PER_MILLE[] = {
    250, // RenderTargets
    100, // Shadows
    50,  // Probes
    150, // Geometry
    0,   // Textures
    50,  // OIT
    30,  // UI
    20,  // Staging
};
static_assert(sizeof(SHARE_PER_MILLE) / sizeof(SHARE_PER_MILLE[0]) == uint32_t(MemoryCategory::Count),
              "every memory category needs a share");

} // namespace

const char* memoryCategoryName(MemoryCategory category)
{
    static const char* const NAMES[] = { "render targets", "shadows", "probes", "geometry",
                                         "textures",       "OIT",     "UI",     "staging" };
    return NAMES[uint32_t(category)];
}

void MemoryBudget::setBudget(uint64_t bytes)
{
    uint64_t left = bytes;
    for (uint32_t i = 0; i < COUNT; ++i)
    {
        if (i != uint32_t(MemoryCategory::Textures))
        {
            uint64_t share = bytes / 1000 * SHARE_PER_MILLE[i];
            if (i == uint32_t(MemoryCategory::Staging) && share < MINIMUM_STAGING_BYTES)
            {
                share = bytes / 4 < MINIMUM_STAGING_BYTES ? bytes / 4 : MINIMUM_STAGING_BYTES;
            }
            mLimit[i].store(share, std::memory_order_relaxed);
            left -= share;
        }
    }
    mLimit[uint32_t(MemoryCategory::Textures)].store(left, std::memory_order_relaxed);
    mBudget.store(bytes, std::memory_order_relaxed);
}

uint64_t MemoryBudget::limit(MemoryCategory category) const
{
    return mLimit[uint32_t(category)].load(std::memory_order_relaxed);
}

uint64_t MemoryBudget::used(MemoryCategory category) const
{
    return mUsed[uint32_t(category)].load(std::memory_order_relaxed);
}

uint64_t MemoryBudget::textureLimit() const
{
    const uint64_t total = budget();
    const uint64_t others = totalUsed() - used(MemoryCategory::Textures);
    const uint64_t kept = others + total / 100 * TEXTURE_RESERVE_PERCENT;
    const uint64_t room = total > kept ? total - kept : 0;
    const uint64_t share = limit(MemoryCategory::Textures);
    return room > share ? room : share;
}

bool MemoryBudget::reserve(MemoryCategory category, uint64_t bytes)
{
    const uint32_t index = uint32_t(category);
    const uint64_t limit = mLimit[index].load(std::memory_order_relaxed);
    uint64_t used = mUsed[index].load(std::memory_order_relaxed);
    do
    {
        if (bytes > limit || used > limit - bytes)
        {
            return false;
        }
    } while (!mUsed[index].compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    mTotalUsed.fetch_add(bytes, std::memory_order_relaxed);
    return true;
}

void MemoryBudget::reserveAnyway(MemoryCategory category, uint64_t bytes)
{
    mUsed[uint32_t(category)].fetch_add(bytes, std::memory_order_relaxed);
    mTotalUsed.fetch_add(bytes, std::memory_order_relaxed);
}

void MemoryBudget::release(MemoryCategory category, uint64_t bytes)
{
    mUsed[uint32_t(category)].fetch_sub(bytes, std::memory_order_relaxed);
    mTotalUsed.fetch_sub(bytes, std::memory_order_relaxed);
}

uint64_t MemoryBudget::uploadBytesPerFrame(uint32_t framesInFlight) const
{
    const uint64_t share = (limit(MemoryCategory::Staging) - SMALL_STAGING_BYTES) / (uint64_t(framesInFlight) * 2);
    return share < MAXIMUM_UPLOAD_BYTES ? share : MAXIMUM_UPLOAD_BYTES;
}

MemoryPressure MemoryBudget::pressure() const
{
    const uint64_t total = budget();
    const uint64_t used = totalUsed();
    if (used >= total / 100 * 95)
    {
        return MemoryPressure::HoldLoads;
    }
    if (used >= total / 100 * 85)
    {
        return MemoryPressure::LowerMips;
    }
    return MemoryPressure::None;
}

} // namespace asr
