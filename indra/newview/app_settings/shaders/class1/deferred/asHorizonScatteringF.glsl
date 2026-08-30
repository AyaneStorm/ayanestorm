/**
 * @file asHorizonScatteringF.glsl
 * @author chanayane@firestorm
 * @brief AyaneStorm analytic broad horizon illumination.
 */

in vec3 vary_horizon_direction;

uniform vec3 as_horizon_sun_direction;
uniform vec3 as_horizon_sun_color;
uniform vec3 as_horizon_tint;
uniform float as_horizon_strength;
uniform float as_horizon_override_opacity;
uniform float as_horizon_band_height;
uniform float as_horizon_band_softness;
uniform float as_horizon_rayleigh_strength;
uniform float as_horizon_aerosol_strength;
uniform float as_horizon_mie_anisotropy;
uniform float as_horizon_azimuth_spread;
uniform float as_horizon_tint_mix;
uniform float as_horizon_sun_fade;
uniform float as_horizon_dip;
uniform int as_horizon_blend_mode;

out vec4 frag_data[4];

const float AS_PI = 3.14159265358979323846;

vec3 safeNormalize(vec3 value, vec3 fallback)
{
    float magnitude_squared = dot(value, value);
    return magnitude_squared > 1e-8
         ? value * inversesqrt(magnitude_squared)
         : fallback;
}

vec2 safeNormalize(vec2 value, vec2 fallback)
{
    float magnitude_squared = dot(value, value);
    return magnitude_squared > 1e-8
         ? value * inversesqrt(magnitude_squared)
         : fallback;
}

void main()
{
    vec3 ray = safeNormalize(vary_horizon_direction, vec3(0.0, 1.0, 0.0));
    vec3 sun = safeNormalize(as_horizon_sun_direction, vec3(0.0, 1.0, 0.0));
    float view_elevation = asin(clamp(ray.y, -1.0, 1.0));
    float horizon_elevation = view_elevation + as_horizon_dip;
    float absolute_elevation = abs(horizon_elevation);
    float positive_elevation = max(horizon_elevation, 0.0);

    float band_end = as_horizon_band_height + as_horizon_band_softness;
    // A symmetric soft band avoids the hard lower cutoff that could bisect
    // the sun when the camera was above the local horizon.
    float band = 1.0 - smoothstep(as_horizon_band_height, band_end, absolute_elevation);

    float vertical = max(sin(positive_elevation), 0.0125);
    float optical_depth = min(24.0, 0.30 / pow(vertical, 0.72));
    vec3 beta_rayleigh = vec3(0.18, 0.42, 1.0);
    vec3 transmittance = exp(-beta_rayleigh * optical_depth * 0.34);
    vec3 removed_light = vec3(1.0) - transmittance;

    float mu = clamp(dot(ray, sun), -1.0, 1.0);
    float g = as_horizon_mie_anisotropy;
    float hg_denominator = max(0.0005, 1.0 + g * g - 2.0 * g * mu);
    float mie_phase = (1.0 - g * g) / pow(hg_denominator, 1.5);
    float mie_forward_normalization = (1.0 - g * g) / pow(max(0.0005, (1.0 - g) * (1.0 - g)), 1.5);
    mie_phase = clamp(mie_phase / max(mie_forward_normalization, 0.0005), 0.0, 1.0);

    vec2 ray_horizontal = safeNormalize(ray.xz, vec2(1.0, 0.0));
    vec2 sun_horizontal = safeNormalize(sun.xz, vec2(1.0, 0.0));
    float azimuth_angle = acos(clamp(dot(ray_horizontal, sun_horizontal), -1.0, 1.0));
    float azimuth = 1.0 - smoothstep(as_horizon_azimuth_spread * 0.45,
                                     as_horizon_azimuth_spread, azimuth_angle);

    float rayleigh_phase = 0.75 * (1.0 + mu * mu);
    vec3 spectral_warmth = max(removed_light * vec3(1.15, 0.72, 0.20), vec3(0.015));
    vec3 base_color = mix(as_horizon_sun_color,
                          as_horizon_sun_color * as_horizon_tint,
                          as_horizon_tint_mix);
    vec3 broad = spectral_warmth * rayleigh_phase * as_horizon_rayleigh_strength *
                 mix(0.32, 1.0, azimuth);
    vec3 aureole = vec3(1.0, 0.72, 0.38) * mie_phase * azimuth *
                   as_horizon_aerosol_strength * (0.45 + optical_depth * 0.045);
    float activation = band * as_horizon_sun_fade;
    vec3 radiance = base_color * (broad + aureole) * as_horizon_strength * activation;
    radiance = min(radiance, vec3(16.0));

    float coverage = clamp(activation * as_horizon_override_opacity, 0.0, 1.0);
    float output_alpha = as_horizon_blend_mode == 1 ? coverage : 0.0;

    frag_data[0] = vec4(0.0);
    frag_data[1] = vec4(0.0);
    // Match the already-rendered EEP sky metadata in replacement mode and add
    // zero in additive mode, avoiding interpolation of categorical flags.
    frag_data[2] = as_horizon_blend_mode == 1
                 ? vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS)
                 : vec4(0.0);
#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(radiance, output_alpha);
#else
    frag_data[0] = vec4(radiance, output_alpha);
#endif
}
