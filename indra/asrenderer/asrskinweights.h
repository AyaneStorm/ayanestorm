/**
 * asrskinweights.h
 * Quantize four skin amounts without changing their total or activating empty slots.
 * Author: chanayane@firestorm
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
