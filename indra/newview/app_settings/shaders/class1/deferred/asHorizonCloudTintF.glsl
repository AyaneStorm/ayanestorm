/**
 * @file asHorizonCloudTintF.glsl
 * @author chanayane@firestorm
 * @brief Sunset3-inspired warm scattering tint for existing EEP clouds.
 *
 * Reuses the EEP cloud density field but changes neither cloud opacity nor
 * noise. Sun-facing, less self-shadowed cloud regions receive the strongest
 * horizon-scattering colour; dense interiors retain more of their EEP shade.
 */

out vec4 frag_data[4];

in vec3 vary_CloudColorSun;
in vec3 vary_CloudColorAmbient;
in float vary_CloudDensity;
in vec2 vary_texcoord0;
in vec2 vary_texcoord1;
in vec2 vary_texcoord2;
in vec2 vary_texcoord3;
in float altitude_blend_factor;
in float as_horizon_view_elevation;
in float as_horizon_sun_alignment;
in float as_horizon_sun_elevation;

uniform sampler2D cloud_noise_texture;
uniform sampler2D cloud_noise_texture_next;
uniform float blend_factor;
uniform vec3 cloud_pos_density1;
uniform vec3 cloud_pos_density2;
uniform float cloud_scale;
uniform float cloud_variance;
uniform vec3 as_horizon_cloud_tint;
uniform float as_horizon_cloud_strength;

vec4 cloudNoise(vec2 uv)
{
    return mix(texture(cloud_noise_texture, uv),
               texture(cloud_noise_texture_next, uv), blend_factor);
}

void main()
{
    if (cloud_scale < 0.001)
    {
        discard;
    }

    vec2 uv1 = vary_texcoord0;
    vec2 uv2 = vary_texcoord1;
    vec2 uv3 = vary_texcoord2;
    vec2 uv4 = vary_texcoord3;
    vec2 disturbance = vec2(cloudNoise(uv1 / 8.0).x,
                             cloudNoise((uv3 + uv1) / 16.0).x)
                      * cloud_variance * (1.0 - cloud_scale * 0.25);
    vec2 disturbance2 = vec2(cloudNoise((uv1 + uv3) / 4.0).x,
                              cloudNoise((uv4 + uv2) / 8.0).x)
                       * cloud_variance * (1.0 - cloud_scale * 0.25);

    uv1 += cloud_pos_density1.xy + disturbance * 0.2;
    uv2 += cloud_pos_density1.xy;
    uv3 += cloud_pos_density2.xy;
    uv4 += cloud_pos_density2.xy;

    float density_variance = min(1.0,
        (disturbance.x * 2.0 + disturbance.y * 2.0
         + disturbance2.x + disturbance2.y) * 4.0);
    float cloud_density = vary_CloudDensity
                        * (1.0 - density_variance * density_variance);

    float alpha = (cloudNoise(uv1).x - 0.5)
                + (cloudNoise(uv3).x - 0.5) * cloud_pos_density2.z;
    alpha = clamp((alpha + cloud_density) * 10.0 * cloud_pos_density1.z, 0.0, 1.0);
    alpha = 1.0 - alpha * alpha;
    alpha = 1.0 - alpha * alpha;
    alpha = clamp(alpha * altitude_blend_factor, 0.0, 1.0);

    float self_shadow = clamp((cloudNoise(uv2).x - 0.5 + cloud_density)
                              * 2.5 * cloud_pos_density1.z, 0.0, 1.0);
    self_shadow = 1.0 - self_shadow;
    self_shadow = 1.0 - self_shadow * self_shadow;

    // Sunset3-style forward illumination: use the EEP vertex-computed solar
    // signal as the directional term, then let cloud self-shadow retain dark
    // interiors while exposed edges receive warm scattered light.
    float solar_signal = smoothstep(0.0, 0.92, as_horizon_sun_alignment);
    // The sunward illumination field continues through the zenith. Azimuth
    // becomes undefined directly overhead, so smoothly converge there on the
    // lit side instead of producing an arbitrary gray hole. Near the horizon,
    // the anti-solar side retains the ordinary gray EEP cloud appearance.
    float overhead = smoothstep(0.610865, 1.308997,
                                max(as_horizon_view_elevation, 0.0));
    float directional_alignment = mix(as_horizon_sun_alignment, 1.0, overhead);
    // Begin fading only at the exact anti-solar direction and use nearly the
    // full hemisphere for the transition. This avoids a visible cutoff while
    // preserving a natural gray destination behind the viewer.
    float solar_reach = smoothstep(-1.0, 0.65, directional_alignment);

    // Opaque low-cloud interiors receive a bounded multiple-scattering
    // contribution; exposed regions receive the stronger forward component.
    // This changes illumination, not cloud density or alpha.
    float forward_light = (1.0 - self_shadow) * (0.3 + 0.7 * solar_signal);
    float cloud_light = clamp(0.38 + 0.62 * forward_light, 0.0, 1.0);
    float tint_amount = solar_reach * cloud_light
                      * clamp(as_horizon_cloud_strength * 0.90, 0.0, 0.90);

    // Reproduce the viewer cloud base colour, then apply extinction-coloured
    // illumination in the same draw. This retains the exact EEP opacity and
    // avoids a depth-equal overlay pass being discarded after cloud rendering.
    vec3 base_color = clamp(vary_CloudColorSun * (1.0 - self_shadow)
                            + vary_CloudColorAmbient, vec3(0.0), vec3(1.0)) * 2.0;
    vec3 extincted_color = base_color
                         * mix(vec3(1.0), as_horizon_cloud_tint, 0.78 * tint_amount);

    // In-scattering is incoming radiance, so it must not be multiplied by the
    // already-dark cloud base. That previous dependency made dense clouds
    // almost immune to sunset light. Self-shadow still limits the direct
    // forward term, while a smaller multiple-scattering floor reaches opaque
    // low-horizon interiors.
    float scattering_strength = clamp(as_horizon_cloud_strength, 0.0, 2.0);
    float multiple_scattering = 0.32 + 0.68 * (1.0 - self_shadow);
    float forward_scattering = (0.25 + 0.75 * solar_signal)
                             * (1.0 - self_shadow);
    // Low-angle sunlight remains visible beneath the cloud layer after the
    // disc reaches the horizon. Spread it by solar azimuth and favor the
    // self-shadowed (visually lower) parts of the cloud instead of recoloring
    // the complete cloud sheet. The common twilight fade still bounds its
    // lifetime and the existing strength control bounds its energy.
    float sunset_height = 1.0 - smoothstep(0.0349066, 0.209440,
                                           abs(as_horizon_sun_elevation));
    float underside = 0.25 + 0.75 * self_shadow;
    float underside_scattering = sunset_height * solar_reach * underside;
    vec3 in_scattering = as_horizon_cloud_tint * solar_reach
                       * scattering_strength
                       * (0.48 * multiple_scattering + 0.72 * forward_scattering
                          + 0.42 * underside_scattering);
    vec3 color = min(extincted_color + in_scattering, vec3(4.0));

    frag_data[0] = vec4(0.0);
    frag_data[1] = vec4(0.0);
    frag_data[2] = vec4(0.0);
#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(color, alpha);
#else
    frag_data[0] = vec4(color, alpha);
#endif
}
