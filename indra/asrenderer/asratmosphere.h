/**
 * asratmosphere.h
 * The sky's inputs and the constants its passes read. The model is Hillaire's 2020 sky
 * (see doc/ayanestorm-new-renderer-sky.md): lookup tables of how much light the air passes
 * and scatters, made every frame for a sun of brightness 1, so the sun's colour and
 * brightness are multiplied in when they are read and cost nothing to change.
 *
 * Units are the sample's: kilometres, and coefficients per kilometre. EEP keeps its density
 * profiles per metre; the viewer converts them before they get here.
 */
#pragma once

#include <cstdint>

namespace asr
{

// One layer of a density profile, Bruneton's format, which EEP's rayleigh_config,
// mie_config and absorption_config use too. The density at height h (km above the bottom
// radius) is expTerm * exp(expScale * h) + linearTerm * h + constantTerm, kept in [0, 1].
struct AtmosphereLayer
{
    float width = 0.f; // km; only the first layer's is used: below it, the first layer
    float expTerm = 0.f;
    float expScale = 0.f;   // per km
    float linearTerm = 0.f; // per km
    float constantTerm = 0.f;
};

// Two layers: the first below its width, the second above. A single-layer EEP profile is
// the second layer here, with a first layer of width 0.
struct AtmosphereProfile
{
    AtmosphereLayer layers[2];
};

// What a sky is, as the renderer takes it. The viewer fills it from EEP; earthSky() is the
// sample's Earth, which tests and a missing sky use.
struct SkyInput
{
    float bottomRadius = 0.f; // km, the planet
    float topRadius = 0.f;    // km, where the atmosphere ends
    AtmosphereProfile rayleigh;
    AtmosphereProfile mie;
    AtmosphereProfile absorption; // ozone: absorbs only
    float rayleighScattering[3]{};   // per km
    float mieScattering[3]{};        // per km
    float mieExtinction[3]{};        // per km: scattering plus absorption
    float absorptionExtinction[3]{}; // per km
    float mieG = 0.f;                // phase anisotropy
    float groundAlbedo[3]{};
    float multipleScattering = 1.f;  // 0 shows single scattering alone
    float sunDirection[3]{};         // towards the sun, world space (Z up), unit length
    float sunAngularRadius = 0.f;    // radians: the disc's size
    float sunIlluminance[3]{};       // the sun's colour times its brightness
    float seaLevel = 0.f;            // metres, world Z of altitude 0
};

SkyInput earthSky();

// Lookup table sizes, as the sample ships them. The transmittance table stays the same on
// every tier so the look matches; the others are what a low tier makes smaller.
constexpr uint32_t TRANSMITTANCE_LUT_WIDTH = 256;
constexpr uint32_t TRANSMITTANCE_LUT_HEIGHT = 64;
constexpr uint32_t MULTISCATTER_LUT_SIZE = 32;
constexpr uint32_t SKY_VIEW_LUT_WIDTH = 192;
constexpr uint32_t SKY_VIEW_LUT_HEIGHT = 108;

// What the atmosphere passes read, after the scene view (GpuSceneView::atmosphere points
// here). std430, mirrored by AtmosphereRef in asrAtmosphere.glsl.
struct GpuAtmosphere
{
    float rayleighScattering[3];
    float bottomRadius;
    float mieScattering[3];
    float topRadius;
    float mieExtinction[3];
    float mieG;
    float absorptionExtinction[3];
    float cameraHeight; // km above the bottom radius, kept just above the ground
    float groundAlbedo[3];
    float multipleScattering;
    float sunDirection[3];
    float sunCosRadius; // cosine of the disc's angular radius
    float sunIlluminance[3];
    float pad0;
    // Per profile (Rayleigh, Mie, absorption): the first layer's expTerm, expScale,
    // linearTerm, constantTerm, then the second's, then the first layer's width.
    float profiles[3][3][4];
};
static_assert(sizeof(GpuAtmosphere) == 256, "asrAtmosphere.glsl reads this layout");

// cameraZ: the camera's world height in metres.
void fillAtmosphere(GpuAtmosphere& out, const SkyInput& sky, float cameraZ);

} // namespace asr
