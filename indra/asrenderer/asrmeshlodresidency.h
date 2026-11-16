/**
 * @file asrmeshlodresidency.h
 * asrmeshlodresidency.h
 * Admission and expiry share one demand window, so a late mesh callback cannot
 * immediately rebuild a LOD that was just evicted. Call want() for each live user,
 * including the asset's actual fallback LOD when its requested level is absent.
 * Author: chanayane@ayanestorm
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (C) 2026, Ayane Lyla
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * $/LicenseInfo$
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
