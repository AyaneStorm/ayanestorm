/**
 * asrAtmosphereMultiscatterC.glsl
 * The multiple scattering table: for each sun zenith angle and height, the light scattered
 * twice and more, summed as a power series (the paper's equations 5 to 10). One group per
 * texel, its 64 threads 64 directions over the sphere, 20 steps each. Ported from the
 * sample's NewMultiScattCS; licence in asrAtmosphere.glsl.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 1, local_size_y = 1, local_size_z = 64) in;

layout(set = 0, binding = 1) uniform sampler samplers[4];
layout(set = 1, binding = 12, rgba16f) uniform writeonly image2D multiscatterTarget;
#include "asrAtmosphere.glsl"

shared vec3 sharedMultiScatterAs1[64];
shared vec3 sharedLuminance[64];

void main()
{
    const ivec2 texel = ivec2(gl_WorkGroupID.xy);
    const uint direction = gl_LocalInvocationID.z;
    const Atmosphere a = loadAtmosphere(ViewRef(push.view).atmosphere);

    vec2 uv = (vec2(texel) + 0.5) / MULTISCATTER_LUT_SIZE;
    uv = vec2(fromSubUvsToUnit(uv.x, MULTISCATTER_LUT_SIZE), fromSubUvsToUnit(uv.y, MULTISCATTER_LUT_SIZE));
    const float sunZenithCos = uv.x * 2.0 - 1.0;
    const vec3 sunDirection = vec3(0.0, sqrt(clamp(1.0 - sunZenithCos * sunZenithCos, 0.0, 1.0)), sunZenithCos);
    const float viewHeight = a.bottomRadius + clamp(uv.y + PLANET_RADIUS_OFFSET, 0.0, 1.0) * (a.topRadius - a.bottomRadius - PLANET_RADIUS_OFFSET);
    const vec3 position = vec3(0.0, 0.0, viewHeight);

    // 8 by 8 directions spread evenly over the sphere.
    const float sqrtSamples = 8.0;
    const float i = 0.5 + float(direction / 8u);
    const float j = 0.5 + float(direction % 8u);
    const float theta = 2.0 * ATMOSPHERE_PI * i / sqrtSamples;
    const float phi = acos(1.0 - 2.0 * j / sqrtSamples);
    const vec3 ray = vec3(cos(theta) * sin(phi), sin(theta) * sin(phi), cos(phi));
    const Scattering result = integrateScattering(a, position, ray, sunDirection, true, 20.0, false, false, false, 9000000.0);

    const float sphereSolidAngle = 4.0 * ATMOSPHERE_PI;
    sharedMultiScatterAs1[direction] = result.multiScatterAs1 * sphereSolidAngle / (sqrtSamples * sqrtSamples);
    sharedLuminance[direction] = result.luminance * sphereSolidAngle / (sqrtSamples * sqrtSamples);
    barrier();
    for (uint half_ = 32u; half_ > 0u; half_ >>= 1u)
    {
        if (direction < half_)
        {
            sharedMultiScatterAs1[direction] += sharedMultiScatterAs1[direction + half_];
            sharedLuminance[direction] += sharedLuminance[direction + half_];
        }
        barrier();
    }
    if (direction != 0u)
    {
        return;
    }

    const float isotropicPhase = 1.0 / sphereSolidAngle;
    const vec3 r = sharedMultiScatterAs1[0] * isotropicPhase; // equation 7, f_ms
    const vec3 secondOrder = sharedLuminance[0] * isotropicPhase; // equation 5
    // All orders: 1 + r + r^2 + ... = 1 / (1 - r) (equation 10). The multiple scattering
    // factor is applied where the table is read.
    imageStore(multiscatterTarget, texel, vec4(secondOrder / max(vec3(1.0) - r, vec3(1e-4)), 1.0));
}
