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

class LLGLSLShader;

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

    // True when the last generate() built the real-sky catalogue (stars in
    // the equatorial frame, whole sphere).
    bool realSkyActive();
    // True when the last generate() stored log-encoded per-star brightness
    // in vertex alpha (real sky, or procedural brightness contrast > 0).
    bool encodedBrightness();
    // Applies the star dome rotation to the current GL matrix (stock zenith
    // spin, or real-sky sidereal spin + latitude tilt) and sets the bound
    // star shader's horizon-fade uniforms. star_time: seconds, DoF-frozen.
    void applySkyTransform(F32 star_time, LLGLSLShader& shader);
    // Real-sky rotation from the local frame (x east, y north, z up) to the
    // J2000 equatorial frame, as a GL column-major mat3; same orientation as
    // applySkyTransform() at star_time.
    void localToEquatorial(F32 star_time, F32 out[9]);
}

#endif
