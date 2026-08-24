/**
 * @file ascelestialtwilight.h
 * @author chanayane@firestorm
 * @brief AyaneStorm below-horizon celestial influence helpers.
 */

#ifndef AS_CELESTIAL_TWILIGHT_H
#define AS_CELESTIAL_TWILIGHT_H

#include "v3math.h"

class LLSettingsSky;
class LLHeavenBody;

namespace ASCelestialTwilight
{
    // Base radii match LLVOSky's sun and moon billboard construction.
    constexpr F32 SUN_DISC_RADIUS = 0.5f;
    constexpr F32 MOON_DISC_RADIUS = SUN_DISC_RADIUS * 0.9f;

    struct LegacyLightState
    {
        F32 sun_influence = 0.f;
        F32 moon_influence = 0.f;
        bool sun_active = false;
        bool moon_active = false;
        LLVector3 sun_direction;
        LLVector3 moon_direction;
    };

    F32 discEdgeElevation(F32 scale, F32 disc_radius);
    F32 influence(const LLVector3& direction, F32 scale, F32 disc_radius,
                  F32 fade_end_degrees);
    F32 sunInfluence(const LLSettingsSky* sky);
    F32 moonInfluence(const LLSettingsSky* sky);
    bool isSunSource(const LLSettingsSky* sky);
    F32 glowFactor(const LLSettingsSky* sky);
    bool shouldDrawDisc(bool geometry_valid, const LLHeavenBody& body,
                        bool center_visible);
    LegacyLightState legacyLightState(const LLSettingsSky* sky);
}

#endif
