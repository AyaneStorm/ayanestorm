/**
 * asrAtmosphereAerialC.glsl
 * The aerial perspective volume: for 32 by 32 view rays across the screen, the light the air
 * scatters towards the camera and the share it lets through, up to each of 32 distances
 * (asratmosphere.h). One thread marches one ray once, two steps a slice, storing as it
 * passes each slice's distance, so no stretch of air is marched twice. rgb: light for a sun
 * of brightness 1; a: transmittance, the mean of the three.
 * The march is the sample's (RenderSkyRayMarching.hlsl, its camera volumes); licence in
 * asrAtmosphere.glsl.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 1) uniform sampler samplers[4];
layout(set = 1, binding = 14, rgba16f) uniform writeonly image3D aerialTarget;
#include "asrAtmosphere.glsl"

const float STEPS_PER_SLICE = 2.0;

void main()
{
    const ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    if (texel.x >= int(AERIAL_LUT_SIZE) || texel.y >= int(AERIAL_LUT_SIZE))
    {
        return;
    }
    ViewRef view = ViewRef(push.view);
    const Atmosphere a = loadAtmosphere(view.atmosphere);
    const vec2 ndc = (vec2(texel) + 0.5) * (2.0 / AERIAL_LUT_SIZE) - 1.0;
    const vec3 direction = viewDirection(view, ndc);
    const vec3 origin = atmosphereCamera(a);

    const float cosTheta = dot(a.sunDirection, direction);
    const float miePhaseValue = miePhase(a.mieG, -cosTheta);
    const float rayleighPhaseValue = rayleighPhase(cosTheta);
    // The ray goes on through the planet: SL ground can lie below altitude 0 (the water
    // height), and anything drawn there still has the air in front of it. Density keeps
    // its altitude 0 value below the ground, and earth shadow and the sun's transmittance
    // read as at the surface.
    vec3 luminance = vec3(0.0);
    vec3 throughput = vec3(1.0);
    float t = 0.0;
    for (int z = 0; z < int(AERIAL_LUT_SIZE); ++z)
    {
        const float tEnd = aerialSliceDistance(a, float(z));
        const float dt = (tEnd - t) / STEPS_PER_SLICE;
        for (float s = 0.0; s < STEPS_PER_SLICE; s += 1.0)
        {
            vec3 p = origin + (t + (s + 0.5) * dt) * direction;
            p *= max(a.bottomRadius + PLANET_RADIUS_OFFSET, length(p)) / length(p); // up onto the ground
            const Medium medium = sampleMedium(a, p);
            const vec3 stepTransmittance = exp(-medium.extinction * dt);

            const float pHeight = length(p);
            const vec3 up = p / pHeight;
            const float sunZenithCos = dot(a.sunDirection, up);
            const vec3 transmittanceToSun = sampleTransmittance(a, pHeight, sunZenithCos);
            const vec3 phaseTimesScattering = medium.scatteringMie * miePhaseValue + medium.scatteringRay * rayleighPhaseValue;
            const float tEarth = raySphereIntersectNearest(p - PLANET_RADIUS_OFFSET * up, a.sunDirection, a.bottomRadius);
            const float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;
            const vec3 multiscattered = sampleMultiscatter(a, p, sunZenithCos) * a.multipleScattering;

            const vec3 S = earthShadow * transmittanceToSun * phaseTimesScattering + multiscattered * medium.scattering;
            const vec3 extinction = max(medium.extinction, vec3(1e-9));
            luminance += throughput * (S - S * stepTransmittance) / extinction;
            throughput *= stepTransmittance;
        }
        t = tEnd;
        imageStore(aerialTarget, ivec3(texel, z), vec4(luminance, dot(throughput, vec3(1.0 / 3.0))));
    }
}
