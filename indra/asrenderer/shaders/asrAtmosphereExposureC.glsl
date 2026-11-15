/**
 * asrAtmosphereExposureC.glsl
 * The frame's exposure, from its sky, as an incident light meter reads it: the light falling
 * on a horizontal surface at the camera, from the sky over the whole upper hemisphere and
 * from the sun after the air. A white surface lit by all of it comes out at 1 before tone
 * mapping. No history and nothing measured on screen, so every view and every snapshot tile
 * of the frame gets the same value. One group of 64 threads.
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

shared float sharedIlluminance[64];

void main()
{
    const uint thread = gl_LocalInvocationID.x;
    ViewRef view = ViewRef(push.view);
    const Atmosphere a = loadAtmosphere(view.atmosphere);

    // Cosine-weighted directions over the upper hemisphere, one per cell of the grid: the
    // irradiance is then pi times the mean radiance.
    vec3 sum = vec3(0.0);
    for (uint i = 0u; i < SAMPLES_PER_THREAD; ++i)
    {
        const uint cell = thread * SAMPLES_PER_THREAD + i;
        const vec2 u = (vec2(cell % 32u, cell / 32u) + 0.5) / 32.0;
        const float r = sqrt(u.x);
        const float phi = 2.0 * ATMOSPHERE_PI * u.y;
        const vec3 direction = vec3(r * cos(phi), r * sin(phi), sqrt(max(1.0 - u.x, 0.0)));
        sum += sampleSkyView(a, direction);
    }
    sharedIlluminance[thread] = dot(sum * a.sunIlluminance, vec3(0.2126, 0.7152, 0.0722)); // luminance, Rec. 709
    barrier();
    for (uint half_ = 32u; half_ > 0u; half_ >>= 1u)
    {
        if (thread < half_)
        {
            sharedIlluminance[thread] += sharedIlluminance[thread + half_];
        }
        barrier();
    }
    if (thread != 0u)
    {
        return;
    }

    const float skyIlluminance = ATMOSPHERE_PI * sharedIlluminance[0] / float(64u * SAMPLES_PER_THREAD);
    const vec3 position = atmosphereCamera(a);
    float sunIlluminance = 0.0;
    if (a.sunDirection.z > 0.0 && raySphereIntersectNearest(position, a.sunDirection, a.bottomRadius) < 0.0)
    {
        const vec3 transmittance = sampleTransmittance(a, length(position), a.sunDirection.z);
        sunIlluminance = dot(a.sunIlluminance * transmittance, vec3(0.2126, 0.7152, 0.0722)) * a.sunDirection.z;
    }
    const float illuminance = max(skyIlluminance + sunIlluminance, ILLUMINANCE_FLOOR);
    ExposureRef result = ExposureRef(view.exposure);
    result.exposure = ATMOSPHERE_PI / illuminance;
    result.illuminance = illuminance;
}
