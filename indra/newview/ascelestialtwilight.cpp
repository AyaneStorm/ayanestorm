/**
 * @file ascelestialtwilight.cpp
 * @author chanayane@firestorm
 * @brief AyaneStorm below-horizon celestial influence helpers.
 */

#include "llviewerprecompiledheaders.h"

#include "ascelestialtwilight.h"

#include "llmath.h"
#include "llsettingssky.h"

namespace
{
    constexpr F32 HEAVENLY_BODY_FACTOR = 0.1f;
    constexpr F32 HORIZON_VERTICAL_ENLARGEMENT = 1.2f;
}

F32 ASCelestialTwilight::discEdgeElevation(F32 scale, F32 disc_radius)
{
    const F32 half_height = llclamp(HORIZON_VERTICAL_ENLARGEMENT * scale *
                                    HEAVENLY_BODY_FACTOR * disc_radius,
                                    0.f, 1.f);
    return -asinf(half_height);
}

F32 ASCelestialTwilight::influence(const LLVector3& direction, F32 scale,
                                   F32 disc_radius, F32 fade_end_degrees)
{
    const F32 elevation = asinf(llclamp(direction.mV[VZ], -1.f, 1.f));
    const F32 edge = discEdgeElevation(scale, disc_radius);
    const F32 fade_end = fade_end_degrees * DEG_TO_RAD;

    if (elevation >= edge)
    {
        return 1.f;
    }
    if (elevation <= fade_end || fade_end >= edge)
    {
        return 0.f;
    }

    F32 t = (elevation - fade_end) / (edge - fade_end);
    t = llclamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

F32 ASCelestialTwilight::sunInfluence(const LLSettingsSky* sky)
{
    return sky ? influence(sky->getSunDirection(), sky->getSunScale(),
                           SUN_DISC_RADIUS, -6.f) : 0.f;
}

F32 ASCelestialTwilight::moonInfluence(const LLSettingsSky* sky)
{
    return sky ? influence(sky->getMoonDirection(), sky->getMoonScale(),
                           MOON_DISC_RADIUS, -8.f) : 0.f;
}

bool ASCelestialTwilight::isSunSource(const LLSettingsSky* sky)
{
    return sunInfluence(sky) > 0.f;
}

F32 ASCelestialTwilight::glowFactor(const LLSettingsSky* sky)
{
    if (!sky)
    {
        return 0.f;
    }
    if (isSunSource(sky))
    {
        // Sky shaders use factor < 1 as a binary request to remove solar glow.
        // Solar energy itself is faded separately through sunlight_color.
        return 1.f;
    }
    return moonInfluence(sky) > 0.f ? sky->getMoonBrightness() * 0.25f : 0.f;
}
