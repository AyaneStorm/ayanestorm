/**
 * @file asstars.h
 * @author chanayane@firestorm
 * @brief Viewer-local star catalogue: density, blackbody colors, size and
 * twinkle. Defaults reproduce the stock LLVOWLSky stars.
 */

#ifndef AS_STARS_H
#define AS_STARS_H

#include <vector>

#include "v3math.h"
#include "v4color.h"

namespace ASStars
{
    void registerUICallbacks();

    // Builds the star catalogue on a dome of the given radius and latches
    // its count, so geometry never reads a count newer than the vectors.
    void generate(F32 radius, std::vector<LLVector3>& positions,
                  std::vector<LLColor4>& colors, std::vector<F32>& intensities);

    // Star count of the last generate() call.
    U32 starCount();
    // Sprite size of star i from the last generate() call.
    F32 starSize(U32 i);

    // Live multipliers (no rebuild needed).
    F32 brightness();
    F32 twinkleAmount();
}

#endif
