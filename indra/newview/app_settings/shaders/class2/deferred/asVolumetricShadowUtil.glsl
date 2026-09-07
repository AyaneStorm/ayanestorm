/**
 * @file asVolumetricShadowUtil.glsl
 * @brief Surface-free directional shadow sampling for volumetric integration.
 * @author chanayane@firestorm
 *
 * This is linked only into the AyaneStorm volumetric programs. It preserves
 * the viewer's directional cascade selection and five-comparison PCF kernel,
 * but omits surface-normal work whose volumetric inputs make it a constant
 * no-op.
 */

uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform sampler2DShadow shadowMap2;
uniform sampler2DShadow shadowMap3;

uniform vec2 shadow_res;
uniform mat4 shadow_matrix[6];
uniform vec4 shadow_clip;
uniform float shadow_bias;

#define AS_VOL_SINGLE_CASCADE 1

float asVolumetricShadowFetch(sampler2DShadow shadow_map, vec4 stc)
{
    stc.xyz /= stc.w;               // orthographic: w == 1, kept for safety
    stc.z += shadow_bias * 2.0;
    return texture(shadow_map, stc.xyz); // hardware bilinear 2x2 compare
}

float asVolumetricDirectionalShadow(vec3 sample_pos, vec2 pos_screen)
{
    // Volumetric callers previously supplied the normalized light direction
    // as `norm`. The shared surface sampler normalized that same direction,
    // computed dot(norm, light_dir) == 1, and consequently applied a zero
    // surface offset. Start directly from the unchanged sample position.
    vec4 spos = vec4(sample_pos, 1.0);
    if (spos.z <= -shadow_clip.w)
    {
        return 1.0;
    }

#if AS_VOL_SINGLE_CASCADE
    // Hard cascade split (no cross-fade): the volumetric integral plus the
    // interleaved reconstruction filter already blends across depth, so a
    // single texel-perfect cascade avoids the four-comparison weighted
    // blend entirely. See doc/volumetric_lighting_bugfix_and_speedup_plan.md
    // section 4.2.
    if (spos.z < -shadow_clip.z)
    {
        float shadow = asVolumetricShadowFetch(shadowMap3, shadow_matrix[3] * spos);
        // Same far fade as upstream so visibility reaches 1 at shadow_clip.w.
        shadow += max((spos.z + shadow_clip.z) /
                      (shadow_clip.z - shadow_clip.w) * 2.0 - 1.0, 0.0);
        return clamp(shadow, 0.0, 1.0);
    }
    if (spos.z < -shadow_clip.y)
    {
        return asVolumetricShadowFetch(shadowMap2, shadow_matrix[2] * spos);
    }
    if (spos.z < -shadow_clip.x)
    {
        return asVolumetricShadowFetch(shadowMap1, shadow_matrix[1] * spos);
    }
    return asVolumetricShadowFetch(shadowMap0, shadow_matrix[0] * spos);
#else
    vec4 near_split = shadow_clip * -0.75;
    vec4 far_split = shadow_clip * -1.25;
    vec4 transition_domain = near_split - far_split;
    float shadow = 0.0;
    float weight = 0.0;

    if (spos.z < near_split.z)
    {
        float w = 1.0;
        w -= max(spos.z - far_split.z, 0.0) / transition_domain.z;
        shadow += asVolumetricShadowFetch(shadowMap3, shadow_matrix[3] * spos) * w;
        weight += w;
        // Preserve the viewer's far-cascade fade exactly.
        shadow += max((sample_pos.z + shadow_clip.z) /
                      (shadow_clip.z - shadow_clip.w) * 2.0 - 1.0, 0.0);
    }

    if (spos.z < near_split.y && spos.z > far_split.z)
    {
        float w = 1.0;
        w -= max(spos.z - far_split.y, 0.0) / transition_domain.y;
        w -= max(near_split.z - spos.z, 0.0) / transition_domain.z;
        shadow += asVolumetricShadowFetch(shadowMap2, shadow_matrix[2] * spos) * w;
        weight += w;
    }

    if (spos.z < near_split.x && spos.z > far_split.y)
    {
        float w = 1.0;
        w -= max(spos.z - far_split.x, 0.0) / transition_domain.x;
        w -= max(near_split.y - spos.z, 0.0) / transition_domain.y;
        shadow += asVolumetricShadowFetch(shadowMap1, shadow_matrix[1] * spos) * w;
        weight += w;
    }

    if (spos.z > far_split.x)
    {
        float w = 1.0;
        w -= max(near_split.x - spos.z, 0.0) / transition_domain.x;
        shadow += asVolumetricShadowFetch(shadowMap0, shadow_matrix[0] * spos) * w;
        weight += w;
    }

    return shadow / weight;
#endif
}
