/**
 * asrAtmosphereAerialC.glsl
 * The aerial perspective volume: for 32 by 32 view rays across the screen, the light the air
 * scatters towards the camera and the share it lets through, up to each of 32 distances
 * (asratmosphere.h). One thread marches one ray once, two steps a slice, storing as it
 * passes each slice's distance, so no stretch of air is marched twice. rgb: the light of sun
 * and moon at their own brightness; a: transmittance, the mean of the three.
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

    const vec3 lightDirections[2] = vec3[2](a.sunDirection, a.moonDirection);
    const vec3 lightColors[2] = vec3[2](a.sunIlluminance, a.moonSkyLight);
    vec2 miePhases;
    vec2 rayleighPhases;
    for (int l = 0; l < 2; ++l)
    {
        const float cosTheta = dot(lightDirections[l], direction);
        miePhases[l] = miePhase(a.mieG, -cosTheta);
        rayleighPhases[l] = rayleighPhase(cosTheta);
    }
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
            vec3 S = vec3(0.0);
            for (int l = 0; l < 2; ++l)
            {
                const float lightZenithCos = dot(lightDirections[l], up);
                const vec3 transmittanceToLight = sampleTransmittance(a, pHeight, lightZenithCos);
                const vec3 phaseTimesScattering = medium.scatteringMie * miePhases[l] + medium.scatteringRay * rayleighPhases[l];
                const float tEarth = raySphereIntersectNearest(p - PLANET_RADIUS_OFFSET * up, lightDirections[l], a.bottomRadius);
                const float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;
                const vec3 multiscattered = sampleMultiscatter(a, p, lightZenithCos) * a.multipleScattering;
                S += lightColors[l] * (earthShadow * transmittanceToLight * phaseTimesScattering + multiscattered * medium.scattering);
            }
            const vec3 extinction = max(medium.extinction, vec3(1e-9));
            luminance += throughput * (S - S * stepTransmittance) / extinction;
            throughput *= stepTransmittance;
        }
        t = tEnd;
        imageStore(aerialTarget, ivec3(texel, z), vec4(luminance, dot(throughput, vec3(1.0 / 3.0))));
    }
}
