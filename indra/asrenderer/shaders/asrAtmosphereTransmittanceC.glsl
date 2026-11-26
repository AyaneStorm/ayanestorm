/**
 * asrAtmosphereTransmittanceC.glsl
 * The transmittance table: how much light passes from a height, along a zenith angle, to
 * the top of the atmosphere. 40 steps a texel, as the sample (fewer loses visible energy).
 * Same size on every tier. Ported from the sample's RenderTransmittanceLutPS; licence in
 * asrAtmosphere.glsl.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 1) uniform sampler samplers[4];
layout(set = 1, binding = 11, rgba32f) uniform writeonly image2D transmittanceTarget;
#include "asrAtmosphere.glsl"

const float STEPS = 40.0;

void main()
{
    const ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    if (texel.x >= int(TRANSMITTANCE_LUT_SIZE.x) || texel.y >= int(TRANSMITTANCE_LUT_SIZE.y))
    {
        return;
    }
    const Atmosphere a = loadAtmosphere(ViewRef(push.view).atmosphere);
    float viewHeight;
    float viewZenithCos;
    uvToTransmittanceParams(a, (vec2(texel) + 0.5) / TRANSMITTANCE_LUT_SIZE, viewHeight, viewZenithCos);
    const vec3 position = vec3(0.0, 0.0, viewHeight);
    const vec3 direction = vec3(0.0, sqrt(max(1.0 - viewZenithCos * viewZenithCos, 0.0)), viewZenithCos);

    // Only the optical depth: what the sample's integrator gives with these settings.
    const float tBottom = raySphereIntersectNearest(position, direction, a.bottomRadius);
    const float tTop = raySphereIntersectNearest(position, direction, a.topRadius);
    const float tMax = tBottom < 0.0 ? max(tTop, 0.0) : (tTop > 0.0 ? min(tTop, tBottom) : 0.0);
    vec3 opticalDepth = vec3(0.0);
    float t = 0.0;
    for (float s = 0.0; s < STEPS; s += 1.0)
    {
        const float newT = tMax * (s + 0.3) / STEPS;
        const float dt = newT - t;
        t = newT;
        opticalDepth += sampleMedium(a, position + t * direction).extinction * dt;
    }
    imageStore(transmittanceTarget, texel, vec4(exp(-opticalDepth), 1.0));
}
