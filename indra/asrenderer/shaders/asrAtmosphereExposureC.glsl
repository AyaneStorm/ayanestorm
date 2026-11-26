/**
 * asrAtmosphereExposureC.glsl
 * The frame's exposure, from its sky, as an incident light meter reads it: the light falling
 * on a horizontal surface at the camera, from the sky over the whole upper hemisphere and
 * from the sun after the air, as a dome meter takes it. A white surface lit by all of it
 * comes out at 1 before tone mapping. A guard then measures the sky itself, its mean luminance over the upper
 * hemisphere, and lowers the exposure so that mean stays at SKY_KEY or below: at sunset the
 * card gets little light while the sky stays bright, and the sky keeps its colour where an
 * incident meter alone blows it out. No history and nothing measured on screen, so every view
 * and every snapshot tile of the frame gets the same value. One group of 64 threads.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 64) in;

layout(set = 0, binding = 1) uniform sampler samplers[4];
#include "asrAtmosphere.glsl"

// The least illuminance the exposure follows, in the sky's units (the sun's illuminance
// at 1 gives about 1 at noon): darker skies stay darker on screen, as night should.
const float ILLUMINANCE_FLOOR = 0.01;
const uint SAMPLES_PER_THREAD = 16u; // 1024 directions in all, on a 32 by 32 grid
// The most the sky's mean luminance may be after exposure, before tone mapping.
const float SKY_KEY = 0.5;

const vec3 LUMINANCE = vec3(0.2126, 0.7152, 0.0722); // Rec. 709
shared vec4 sharedSums[64]; // rgb: cosine-weighted, a: uniform over the hemisphere's luminance

void main()
{
    const uint thread = gl_LocalInvocationID.x;
    ViewRef view = ViewRef(push.view);
    const Atmosphere a = loadAtmosphere(view.atmosphere);

    // Per cell of the grid, a cosine-weighted direction over the upper hemisphere (the
    // irradiance is then pi times the mean radiance) and a uniform one (the sky's mean).
    vec3 cosineSum = vec3(0.0);
    vec3 uniformSum = vec3(0.0);
    for (uint i = 0u; i < SAMPLES_PER_THREAD; ++i)
    {
        const uint cell = thread * SAMPLES_PER_THREAD + i;
        const vec2 u = (vec2(cell % 32u, cell / 32u) + 0.5) / 32.0;
        const float phi = 2.0 * ATMOSPHERE_PI * u.y;
        const float r = sqrt(u.x);
        cosineSum += skyRadiance(a, vec3(r * cos(phi), r * sin(phi), sqrt(max(1.0 - u.x, 0.0))));
        const float z = u.x;
        const float side = sqrt(max(1.0 - z * z, 0.0));
        uniformSum += skyRadiance(a, vec3(side * cos(phi), side * sin(phi), z));
    }
    sharedSums[thread] = vec4(cosineSum, dot(uniformSum, LUMINANCE));
    barrier();
    for (uint half_ = 32u; half_ > 0u; half_ >>= 1u)
    {
        if (thread < half_)
        {
            sharedSums[thread] += sharedSums[thread + half_];
        }
        barrier();
    }
    if (thread != 0u)
    {
        return;
    }

    const vec3 skyIrradiance = ATMOSPHERE_PI * sharedSums[0].rgb / float(64u * SAMPLES_PER_THREAD);
    const float skyIlluminance = dot(skyIrradiance, LUMINANCE);
    const float skyMean = sharedSums[0].a / float(64u * SAMPLES_PER_THREAD);
    const vec3 position = atmosphereCamera(a);
    // The sun sets over its disc's width (the EEP sun scale: about 3 degrees either side of
    // the horizon at scale 1), not at its centre, which switched the light off between
    // elevation 0 and -0.01.
    const vec3 transmittance = lightThroughAir(a, position, a.sunDirection, acos(a.sunCosRadius));
    // A dome meter's response, as photographers meter 3D subjects: (1 + sin elevation) / 2,
    // the dome's projected area towards the sun. A flat card's sin(elevation) raised the
    // exposure so much at low sun that the sun, its bloom and whatever faces it peaked
    // near 9 degrees; at noon the two read alike. The sky stays read by the card.
    const float sunIlluminance = dot(a.sunIlluminance * transmittance, LUMINANCE) * max(0.5 + 0.5 * a.sunDirection.z, 0.0);
    const float illuminance = max(skyIlluminance + sunIlluminance, ILLUMINANCE_FLOOR);
    ExposureRef result = ExposureRef(view.exposure);
    // Partial adaptation, as the eye's: against the sun's own illuminance above the air (a
    // noon of this EEP), the exposure follows the light by exposureAdaptation only, so a low
    // sun, dusk and night stay dimmer than noon. Fully adapted, the sun on screen came out
    // about 1 / the meter's reading, brighter the lower it was until the air won near 2.5
    // degrees.
    const float noon = max(dot(a.sunIlluminance, LUMINANCE), ILLUMINANCE_FLOOR);
    const float adaptation = clamp(view.exposureAdaptation, 0.0, 1.0);
    const float adapted = ATMOSPHERE_PI / illuminance * pow(illuminance / noon, 1.0 - adaptation);
    // The guard only lowers: at night the floor above still decides.
    result.exposure = min(adapted, SKY_KEY / max(skyMean, 1e-9));
    result.illuminance = illuminance;
    result.skyIrradiance = skyIrradiance;
}
