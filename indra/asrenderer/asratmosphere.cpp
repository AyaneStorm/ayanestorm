/**
 * @file asratmosphere.cpp
 * asratmosphere.cpp
 * See asratmosphere.h. The Earth values are those of Hillaire's 2020 sample code
 * (SkyAtmosphereCommon.cpp, MIT licence, Copyright Epic Games, Inc.).
 * @author chanayane@ayanestorm
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
#include "asratmosphere.h"

#include <algorithm>
#include <cmath>

namespace asr
{

namespace
{
// Keeps the camera above the ground sphere, so rays never start inside the planet. The
// sample offsets by the same amount (PLANET_RADIUS_OFFSET).
constexpr float GROUND_OFFSET = 0.01f; // km

void set3(float out[3], float x, float y, float z)
{
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

void packProfile(float out[3][4], const AtmosphereProfile& profile)
{
    for (uint32_t i = 0; i < 2; ++i)
    {
        const AtmosphereLayer& layer = profile.layers[i];
        out[i][0] = layer.expTerm;
        out[i][1] = layer.expScale;
        out[i][2] = layer.linearTerm;
        out[i][3] = layer.constantTerm;
    }
    out[2][0] = profile.layers[0].width;
    out[2][1] = out[2][2] = out[2][3] = 0.f;
}
} // namespace

SkyInput earthSky()
{
    SkyInput sky;
    sky.bottomRadius = 6360.f;
    sky.topRadius = 6460.f;
    sky.rayleigh.layers[1].expTerm = 1.f;
    sky.rayleigh.layers[1].expScale = -1.f / 8.f;
    sky.mie.layers[1].expTerm = 1.f;
    sky.mie.layers[1].expScale = -1.f / 1.2f;
    // Ozone: a tent peaking at 25 km, rising from 10 km and gone at 40 km.
    sky.absorption.layers[0].width = 25.f;
    sky.absorption.layers[0].linearTerm = 1.f / 15.f;
    sky.absorption.layers[0].constantTerm = -2.f / 3.f;
    sky.absorption.layers[1].linearTerm = -1.f / 15.f;
    sky.absorption.layers[1].constantTerm = 8.f / 3.f;
    set3(sky.rayleighScattering, 0.005802f, 0.013558f, 0.033100f);
    set3(sky.mieScattering, 0.003996f, 0.003996f, 0.003996f);
    set3(sky.mieExtinction, 0.00444f, 0.00444f, 0.00444f);
    set3(sky.absorptionExtinction, 0.00065f, 0.001881f, 0.000085f);
    sky.mieG = 0.8f;
    set3(sky.groundAlbedo, 0.f, 0.f, 0.f);
    sky.multipleScattering = 1.f;
    // Mid-morning, so a test that sets nothing sees a lit sky.
    const float elevation = 0.6f;
    set3(sky.sunDirection, std::cos(elevation) * 0.6f, std::cos(elevation) * 0.8f, std::sin(elevation));
    sky.sunAngularRadius = 0.5f * 0.505f * 3.14159265f / 180.f; // the sample's disc
    set3(sky.sunIlluminance, 1.f, 1.f, 1.f);
    sky.seaLevel = 20.f; // SL's default water height
    return sky;
}

void fillAtmosphere(GpuAtmosphere& out, const SkyInput& sky, float cameraZ, float drawDistance)
{
    out = GpuAtmosphere{};
    std::copy(sky.rayleighScattering, sky.rayleighScattering + 3, out.rayleighScattering);
    std::copy(sky.mieScattering, sky.mieScattering + 3, out.mieScattering);
    std::copy(sky.mieExtinction, sky.mieExtinction + 3, out.mieExtinction);
    std::copy(sky.absorptionExtinction, sky.absorptionExtinction + 3, out.absorptionExtinction);
    std::copy(sky.groundAlbedo, sky.groundAlbedo + 3, out.groundAlbedo);
    std::copy(sky.sunDirection, sky.sunDirection + 3, out.sunDirection);
    std::copy(sky.sunIlluminance, sky.sunIlluminance + 3, out.sunIlluminance);
    out.bottomRadius = sky.bottomRadius;
    out.topRadius = std::max(sky.topRadius, sky.bottomRadius + 2.f * GROUND_OFFSET);
    out.mieG = std::clamp(sky.mieG, -0.999f, 0.999f);
    out.multipleScattering = sky.multipleScattering;
    out.sunCosRadius = std::cos(sky.sunAngularRadius);
    // Inside the atmosphere: SL cameras never leave it, and the tables assume it.
    const float height = (cameraZ - sky.seaLevel) * 0.001f;
    out.cameraHeight = std::clamp(height, GROUND_OFFSET, out.topRadius - out.bottomRadius - GROUND_OFFSET);
    packProfile(out.profiles[0], sky.rayleigh);
    packProfile(out.profiles[1], sky.mie);
    packProfile(out.profiles[2], sky.absorption);

    out.sunTexture = sky.sunTexture;
    out.sunTextureRadius = sky.sunTextureRadius;
    std::copy(sky.sunUp, sky.sunUp + 3, out.sunUp);
    out.moonTexture = sky.moonTexture;
    std::copy(sky.moonDirection, sky.moonDirection + 3, out.moonDirection);
    std::copy(sky.moonUp, sky.moonUp + 3, out.moonUp);
    std::copy(sky.moonIlluminance, sky.moonIlluminance + 3, out.moonIlluminance);
    out.moonAngularRadius = sky.moonAngularRadius;
    out.moonNormalTexture = sky.moonNormalTexture;
    out.moonSphere = sky.moonSphere ? 1u : 0u;
    // Where the sphere is lit from, by its phase: from behind the moon at new (theta pi), from
    // the viewer at full (0), from the disc's right at first quarter, turned by the tilt.
    const float* d = sky.moonDirection;
    float up[3];
    const float upAlong = sky.moonUp[0] * d[0] + sky.moonUp[1] * d[1] + sky.moonUp[2] * d[2];
    float upLength = 0.f;
    for (int i = 0; i < 3; ++i)
    {
        up[i] = sky.moonUp[i] - d[i] * upAlong;
        upLength += up[i] * up[i];
    }
    upLength = std::sqrt(std::max(upLength, 1e-12f));
    for (float& u : up)
    {
        u /= upLength;
    }
    // right = up x toViewer, toViewer = -d
    const float right[3] = { -(up[1] * d[2] - up[2] * d[1]), -(up[2] * d[0] - up[0] * d[2]), -(up[0] * d[1] - up[1] * d[0]) };
    const float theta = 3.14159265f * (1.f - 2.f * (sky.moonPhase - std::floor(sky.moonPhase)));
    const float tiltCos = std::cos(sky.moonPhaseTilt);
    const float tiltSin = std::sin(sky.moonPhaseTilt);
    for (int i = 0; i < 3; ++i)
    {
        const float side = tiltCos * right[i] + tiltSin * up[i];
        out.moonLightDirection[i] = std::cos(theta) * -d[i] + std::sin(theta) * side;
    }
    out.moonTerminatorSoftness = std::max(sky.moonTerminatorSoftness, 0.f);
    out.moonEarthshine = std::max(sky.moonEarthshine, 0.f);
    out.moonRelief = std::max(sky.moonRelief, 0.f);
    out.moonHaloStrength = std::max(sky.moonHaloStrength, 0.f);
    out.moonHaloRadius = std::max(sky.moonHaloRadius, 1.01f);
    out.moonHaloSoftness = std::max(sky.moonHaloSoftness, 0.05f);
    out.moonBrightness = std::max(sky.moonBrightness, 0.f);
    out.moonSkyScale = 1.f;
    if (sky.moonSphere)
    {
        // Lommel-Seeliger's sphere, as the moon's shading: its light at phase angle alpha
        // (light, moon, viewer) against full moon.
        const float* light = out.moonLightDirection;
        const float cosAlpha = -(light[0] * sky.moonDirection[0] + light[1] * sky.moonDirection[1] + light[2] * sky.moonDirection[2]);
        const float alpha = std::acos(std::clamp(cosAlpha, -1.f, 1.f));
        const float q = alpha * 0.25f;
        out.moonSkyScale = alpha < 1e-4f ? 1.f
                         : alpha > 3.1415f ? 0.f
                         : std::max(0.f, 1.f - std::sin(2.f * q) * std::tan(2.f * q) * std::log(1.f / std::tan(q)));
    }
    // The disc, or a square's corner (sqrt(2) times its half side): nothing lies outside. A
    // sphere's edge is smoothed over a pixel just past its radius.
    float reach = sky.moonSphere ? 1.05f : sky.moonTexture == NO_SKY_TEXTURE ? 1.f : 1.41422f;
    if (out.moonHaloStrength > 0.f)
    {
        reach = std::max(reach, out.moonHaloRadius);
    }
    out.moonCosRadius = std::cos(std::min(sky.moonAngularRadius * reach, 3.1f));
    out.aerialDistance = (drawDistance > 0.f ? drawDistance : AERIAL_DEFAULT_DISTANCE) * 0.001f;

    out.starBrightness = std::max(sky.starBrightness, 0.f);
    out.starTwinkle = std::clamp(sky.starTwinkle, 0.f, 1.f);
    const float rotationW[3] = { sky.starTwinklePhase, std::max(sky.milkyWayIntensity, 0.f), std::max(sky.deepSkyIntensity, 0.f) };
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            out.starRotation[column][row] = sky.starRotation[column * 3 + row];
        }
        out.starRotation[column][3] = rotationW[column];
    }
    // The device sets the list's address and count once it is on the GPU.
    out.stars = 0;
    out.starCount = 0;
    out.starTexture = sky.starTexture;
    // A map that is missing draws no glow: both levels must have their map.
    out.milkyWayTexture = sky.milkyWayTexture;
    out.deepSkyTexture = sky.deepSkyTexture;
    if (sky.milkyWayTexture == NO_SKY_TEXTURE)
    {
        out.starRotation[1][3] = 0.f;
    }
    if (sky.deepSkyTexture == NO_SKY_TEXTURE)
    {
        out.starRotation[2][3] = 0.f;
    }
    out.milkyWaySaturation = std::max(sky.milkyWaySaturation, 0.f);
    constexpr float MOON_RADIUS_KM = 1737.4f;
    out.moonHeightScale = std::max(sky.moonHeightRange, 0.f) / MOON_RADIUS_KM;
    for (int i = 0; i < 3; ++i)
    {
        out.moonEarthshineColor[i] = std::max(sky.moonEarthshineColor[i], 0.f);
    }
    out.pad = 0.f;
    for (int i = 0; i < 3; ++i)
    {
        out.moonTint[i] = std::max(sky.moonTint[i], 0.f);
    }
    out.pad2 = 0.f;
    out.sunGlow[0] = std::max(sky.sunGlowSize, 0.f);
    out.sunGlow[1] = sky.sunGlowFocus; // EEP: -10 to 10, negative for a glow at the sun
    out.sunGlow[2] = 0.f;
    out.sunGlow[3] = 0.f;
}

} // namespace asr
