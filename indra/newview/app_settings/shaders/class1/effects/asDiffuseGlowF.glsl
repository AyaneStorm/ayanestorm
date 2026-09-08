/**
 * @file asDiffuseGlowF.glsl
 * @author chanayane@firestorm
 * @brief Bright-surface selection for the existing glow extraction pass.
 */

uniform float asDiffuseGlowThreshold;
uniform float asDiffuseGlowSoftness;
uniform float asDiffuseGlowStrength;

float asDiffuseGlowMask(vec3 color)
{
    const vec3 AS_LUMINANCE_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);
    float luminance = dot(color, AS_LUMINANCE_WEIGHTS);
    float half_softness = asDiffuseGlowSoftness * 0.5;
    float selection = asDiffuseGlowSoftness > 0.0001
        ? smoothstep(asDiffuseGlowThreshold - half_softness,
                     asDiffuseGlowThreshold + half_softness,
                     luminance)
        : step(asDiffuseGlowThreshold, luminance);
    return selection * asDiffuseGlowStrength;
}
