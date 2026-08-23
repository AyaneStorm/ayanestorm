/**
 * @file ascelestialtwilight.h
 * @author chanayane@firestorm
 * @brief AyaneStorm below-horizon celestial influence helpers.
 */

#ifndef AS_CELESTIAL_TWILIGHT_H
#define AS_CELESTIAL_TWILIGHT_H

#include "v3math.h"

class LLSettingsSky;

namespace ASCelestialTwilight
{
    // Base radii match LLVOSky's sun and moon billboard construction.
    constexpr F32 SUN_DISC_RADIUS = 0.5f;
    constexpr F32 MOON_DISC_RADIUS = SUN_DISC_RADIUS * 0.9f;

    F32 discEdgeElevation(F32 scale, F32 disc_radius);
    F32 influence(const LLVector3& direction, F32 scale, F32 disc_radius,
                  F32 fade_end_degrees);
    F32 sunInfluence(const LLSettingsSky* sky);
    F32 moonInfluence(const LLSettingsSky* sky);
    bool isSunSource(const LLSettingsSky* sky);
    F32 glowFactor(const LLSettingsSky* sky);
}

#endif
