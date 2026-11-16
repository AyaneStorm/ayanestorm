/**
 * @file asrskinweights.h
 * asrskinweights.h
 * Quantize four skin amounts without changing their total or activating empty slots.
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

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace asr
{

inline void packSkinAmounts(const float shares[4], uint16_t amounts[4])
{
    float values[4], total = 0.f;
    int largest = 0;
    for (int i = 0; i < 4; ++i)
    {
        values[i] = std::max(0.f, shares[i]);
        total += values[i];
        if (values[i] > values[largest]) largest = i;
    }
    if (!(total > 0.f) || !std::isfinite(total))
    {
        amounts[0] = 65535;
        amounts[1] = amounts[2] = amounts[3] = 0;
        return;
    }
    int given = 0;
    for (int i = 0; i < 4; ++i)
    {
        amounts[i] = uint16_t(std::lround(values[i] / total * 65535.f));
        given += amounts[i];
    }
    // The largest share can absorb the signed rounding residual. Slot zero may be
    // empty, so clamping a negative residual there would leave the total too large.
    amounts[largest] = uint16_t(int(amounts[largest]) + 65535 - given);
}

} // namespace asr
