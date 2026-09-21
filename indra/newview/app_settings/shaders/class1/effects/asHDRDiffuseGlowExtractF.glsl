/**
 * @file asHDRDiffuseGlowExtractF.glsl
 * @author chanayane@firestorm
 * @brief Scene-linear bright-surface bloom extraction with a scalar RGB knee.
 */

out vec4 frag_color;

uniform sampler2D diffuseMap;
uniform sampler2D exposureMap;
uniform float exposure;
uniform float asDiffuseGlowThreshold;
uniform float asDiffuseGlowSoftness;
uniform float asDiffuseGlowStrength;
uniform float minLuminance;
uniform float maxExtractAlpha;
uniform vec3 lumWeights;
uniform vec3 warmthWeights;
uniform float warmthAmount;

in vec2 vary_texcoord0;

vec3 asApplyLinearColorGrade(vec3 color);

void main()
{
    const vec3 AS_LUMINANCE_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);
    vec3 scene_color = max(texture(diffuseMap, vary_texcoord0).rgb, vec3(0.0));

    // Match the exposure and scene-linear grading used by the following
    // tonemap pass for selection, while preserving unexposed radiance for blur.
    float final_exposure = max(exposure * texture(exposureMap, vec2(0.5)).r, 0.0);
    vec3 selection_color = asApplyLinearColorGrade(scene_color) * final_exposure;
    float luminance = dot(selection_color, AS_LUMINANCE_WEIGHTS);
    float half_softness = asDiffuseGlowSoftness * 0.5;
    float selection = asDiffuseGlowSoftness > 0.0001
        ? smoothstep(asDiffuseGlowThreshold - half_softness,
                     asDiffuseGlowThreshold + half_softness,
                     luminance)
        : step(asDiffuseGlowThreshold, luminance);
    float legacy_luminance = smoothstep(minLuminance, minLuminance + 1.0,
                                        dot(selection_color, lumWeights));
    float legacy_warmth = smoothstep(minLuminance, minLuminance + 1.0,
                                     max(selection_color.r * warmthWeights.r,
                                         max(selection_color.g * warmthWeights.g,
                                             selection_color.b * warmthWeights.b)));
    float legacy_contribution = mix(legacy_luminance, legacy_warmth, warmthAmount) *
                                maxExtractAlpha;
    float contribution = max(selection * asDiffuseGlowStrength, legacy_contribution);

    // One scalar multiplies the RGB triplet, so extraction cannot rotate hue.
    frag_color = vec4(scene_color * contribution, contribution);
}
