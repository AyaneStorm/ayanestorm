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

constexpr uint32_t NO_SKY_TEXTURE = 0xFFFFFFFFu;

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

    // The sun and moon as images, each a square facing the camera, `up` its top edge in the
    // world. The air in front dims them and the planet hides them. Without an image a disc
    // is drawn: the sun's limb darkened, the moon's flat.
    uint32_t sunTexture = NO_SKY_TEXTURE; // texture bits, as materials carry them
    float sunTextureRadius = 0.f;    // radians, half the square's side
    float sunUp[3]{};
    uint32_t moonTexture = NO_SKY_TEXTURE;
    float moonDirection[3]{};        // towards the moon, unit length
    float moonUp[3]{};
    float moonAngularRadius = 0.f;   // radians, half the square's side, or the disc's radius
    float moonIlluminance[3]{};      // what the whole moon gives, in the sun's units
};

SkyInput earthSky();

// Lookup table sizes, as the sample ships them. The transmittance table stays the same on
// every tier so the look matches; the others are what a low tier makes smaller.
constexpr uint32_t TRANSMITTANCE_LUT_WIDTH = 256;
constexpr uint32_t TRANSMITTANCE_LUT_HEIGHT = 64;
constexpr uint32_t MULTISCATTER_LUT_SIZE = 32;
constexpr uint32_t SKY_VIEW_LUT_WIDTH = 192;
constexpr uint32_t SKY_VIEW_LUT_HEIGHT = 108;
// The aerial perspective volume: the screen in 32 by 32 columns, each the air along one view
// ray, in 32 slices spread as the square of their number out to the draw distance, so near
// slices are thin (sample: 32 slices of 4 km, made for flight; SL sees at most 1 km).
constexpr uint32_t AERIAL_LUT_SIZE = 32;
constexpr float AERIAL_DEFAULT_DISTANCE = 1024.f; // metres, for a view with no draw distance

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
    uint32_t sunTexture;
    // Per profile (Rayleigh, Mie, absorption): the first layer's expTerm, expScale,
    // linearTerm, constantTerm, then the second's, then the first layer's width.
    float profiles[3][3][4];
    float sunUp[3];
    float sunTextureRadius;
    float moonDirection[3];
    float moonCosRadius;
    float moonUp[3];
    float moonAngularRadius;
    float moonIlluminance[3];
    uint32_t moonTexture;
    float aerialDistance; // km: the far edge of the aerial perspective volume
    float pad[3];
};
static_assert(sizeof(GpuAtmosphere) == 336, "asrAtmosphere.glsl reads this layout");

// cameraZ: the camera's world height in metres. drawDistance: the view's, in metres; 0 is
// none, and the aerial perspective volume then reaches AERIAL_DEFAULT_DISTANCE.
void fillAtmosphere(GpuAtmosphere& out, const SkyInput& sky, float cameraZ, float drawDistance);

} // namespace asr
