/**
 * asratmosphere.cpp
 * See asratmosphere.h. The Earth values are those of Hillaire's 2020 sample code
 * (SkyAtmosphereCommon.cpp, MIT licence, Copyright Epic Games, Inc.).
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
    // A square's corner reaches sqrt(2) times its half side: nothing of it lies outside that.
    out.moonCosRadius = std::cos(sky.moonTexture == NO_SKY_TEXTURE ? sky.moonAngularRadius : sky.moonAngularRadius * 1.41422f);
    out.aerialDistance = (drawDistance > 0.f ? drawDistance : AERIAL_DEFAULT_DISTANCE) * 0.001f;
}

} // namespace asr
