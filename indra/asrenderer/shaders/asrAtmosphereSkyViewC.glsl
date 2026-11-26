/**
 * asrAtmosphereSkyViewC.glsl
 * The sky view table: the sky seen from the camera's height, for every direction, by the
 * view's zenith angle and its angle to the sun around the zenith. 4 to 14 steps a texel,
 * more for longer rays. Read for the background, and later reflections and ambient light.
 * Three tables, one above the other, each for a light of brightness 1 (see sampleSkyView()):
 * lit by the sun without its Mie single scattering, by the moon, then the sun's Mie single
 * scattering with no phase, shaped per pixel by skyRadiance().
 * Ported from the sample's SkyViewLutPS; licence in asrAtmosphere.glsl.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 1) uniform sampler samplers[4];
layout(set = 1, binding = 13, rgba32f) uniform writeonly image2D skyViewTarget;
#include "asrAtmosphere.glsl"

void main()
{
    const ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    if (texel.x >= int(SKY_VIEW_LUT_SIZE.x) || texel.y >= int(SKY_VIEW_LUT_LIGHTS) * int(SKY_VIEW_LUT_SIZE.y))
    {
        return;
    }
    const uint light = uint(texel.y) / uint(SKY_VIEW_LUT_SIZE.y);
    const ivec2 inTable = ivec2(texel.x, texel.y - int(light) * int(SKY_VIEW_LUT_SIZE.y));
    const Atmosphere a = loadAtmosphere(ViewRef(push.view).atmosphere);
    const float viewHeight = length(atmosphereCamera(a));
    float viewZenithCos;
    float lightViewCos;
    uvToSkyViewParams(a, (vec2(inTable) + 0.5) / SKY_VIEW_LUT_SIZE, viewHeight, viewZenithCos, lightViewCos);

    // The light in the table's own frame: its zenith angle kept, its azimuth along X.
    const float sunZenithCos = light == SKY_LIGHT_MOON ? a.moonDirection.z : a.sunDirection.z;
    const vec3 sunDirection = normalize(vec3(sqrt(max(1.0 - sunZenithCos * sunZenithCos, 0.0)), 0.0, sunZenithCos));
    const float viewZenithSin = sqrt(max(1.0 - viewZenithCos * viewZenithCos, 0.0));
    const vec3 direction = vec3(viewZenithSin * lightViewCos, viewZenithSin * sqrt(max(1.0 - lightViewCos * lightViewCos, 0.0)), viewZenithCos);

    vec3 position = vec3(0.0, 0.0, viewHeight);
    vec3 luminance = vec3(0.0);
    if (moveToTopAtmosphere(position, direction, a.topRadius))
    {
        const Scattering result = integrateScattering(a, position, direction, sunDirection, false, 30.0, true, true, true, 9000000.0);
        // The phase is one value along the ray, so the sun's Mie single scattering comes out whole.
        const float mieAlong = miePhase(a.mieG, -dot(sunDirection, direction));
        luminance = light == SKY_LIGHT_SUN_MIE ? result.mieSingle
                  : light == SKY_LIGHT_SUN     ? max(result.luminance - result.mieSingle * mieAlong, vec3(0.0))
                                               : result.luminance;
    }
    imageStore(skyViewTarget, texel, vec4(luminance, 1.0));
}
