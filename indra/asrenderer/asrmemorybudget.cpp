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
    // Steered below the working budget: a texture replacement holds both images for a while,
    // and the screen, scene tables and geometry are counted even past the budget. Steering
    // to all of it left nothing for them, and the total sat over the budget.
    const uint64_t total = budget() / 100 * TEXTURE_STEERING_PERCENT;
    const uint64_t all = totalUsed();
    const uint64_t textures = used(MemoryCategory::Textures);
    const uint64_t others = all > textures ? all - textures : 0;
    return total > others ? total - others : 0;
}

bool MemoryBudget::canBorrow(uint64_t bytes) const
{
    const uint64_t total = budget();
    const uint64_t all = totalUsed();
    const uint64_t textureUse = used(MemoryCategory::Textures);
    const uint64_t others = all > textureUse ? all - textureUse : 0;
    const uint64_t textures = limit(MemoryCategory::Textures);
    // Within what is free now, and never into the textures' own share.
    return textures <= total && bytes <= total - textures && all <= total - bytes && others <= total - textures - bytes;
}

bool MemoryBudget::reserve(MemoryCategory category, uint64_t bytes)
{
    const uint32_t index = uint32_t(category);
    const uint64_t limit = mLimit[index].load(std::memory_order_relaxed);
    uint64_t used = mUsed[index].load(std::memory_order_relaxed);
    do
    {
        if ((bytes > limit || used > limit - bytes) && (category != MemoryCategory::Geometry || !canBorrow(bytes)))
        {
            return false;
        }
    } while (!mUsed[index].compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    uint64_t all = totalUsed();
    do
    {
        const uint64_t total = budget();
        if (bytes > total || all > total - bytes)
        {
            mUsed[index].fetch_sub(bytes, std::memory_order_relaxed);
            return false;
        }
    } while (!mTotalUsed.compare_exchange_weak(all, all + bytes, std::memory_order_relaxed));
    return true;
}

void MemoryBudget::reserveAnyway(MemoryCategory category, uint64_t bytes)
{
    mTotalUsed.fetch_add(bytes, std::memory_order_relaxed);
    mUsed[uint32_t(category)].fetch_add(bytes, std::memory_order_relaxed);
}

void MemoryBudget::release(MemoryCategory category, uint64_t bytes)
{
    mUsed[uint32_t(category)].fetch_sub(bytes, std::memory_order_relaxed);
    mTotalUsed.fetch_sub(bytes, std::memory_order_relaxed);
}

std::string MemoryBudget::refusal(const char* what, MemoryCategory category, uint64_t bytes) const
{
    const auto mb = [](uint64_t value) { return std::to_string((value + (1 << 20) - 1) >> 20); };
    return std::string(what) + " does not fit in its memory budget: " + mb(bytes) + " MB asked, " +
           memoryCategoryName(category) + " " + mb(used(category)) + "/" + mb(limit(category)) + " MB, all " +
           mb(totalUsed()) + "/" + mb(budget()) + " MB, textures " + mb(used(MemoryCategory::Textures)) + " MB";
}

uint64_t MemoryBudget::uploadBytesPerFrame(uint32_t framesInFlight) const
{
    const uint64_t staging = limit(MemoryCategory::Staging);
    const uint64_t share = staging > SMALL_STAGING_BYTES ? (staging - SMALL_STAGING_BYTES) / (uint64_t(framesInFlight) * 2) : 0;
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
