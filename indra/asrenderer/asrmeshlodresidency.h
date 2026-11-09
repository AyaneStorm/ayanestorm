/**
 * asrmeshlodresidency.h
 * Admission and expiry share one demand window, so a late mesh callback cannot
 * immediately rebuild a LOD that was just evicted. Call want() for each live user,
 * including the asset's actual fallback LOD when its requested level is absent.
 * Author: chanayane@firestorm
 */
#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace asr
{

class MeshLodResidency
{
public:
    static constexpr uint32_t LEVELS = 4;

    MeshLodResidency() { mWantedAt.fill(-std::numeric_limits<double>::infinity()); }

    void want(uint32_t level, double now)
    {
        for (uint32_t i = 0; i < LEVELS && i <= level; ++i) mWantedAt[i] = now;
    }

    bool holds(uint32_t level, double now, bool pressure) const
    {
        // Retain useful detail across short camera motion; reclaim sooner under pressure.
        return level < LEVELS && now - mWantedAt[level] < (pressure ? 5.0 : 30.0);
    }

private:
    std::array<double, LEVELS> mWantedAt;
};

} // namespace asr
