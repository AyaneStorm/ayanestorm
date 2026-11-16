/**
 * @file ascelestialtwilight.h
 * @author chanayane@ayanestorm
 * @brief AyaneStorm below-horizon celestial influence helpers.
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
