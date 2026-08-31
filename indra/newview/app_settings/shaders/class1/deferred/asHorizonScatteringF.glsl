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
uniform int as_horizon_pass;
uniform float as_horizon_sky_haze_strength;
uniform float as_horizon_sky_haze_intensity;

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
    if (as_horizon_blend_mode == 1)
    {
        radiance *= as_horizon_override_opacity;
    }
    radiance = min(radiance, vec3(16.0));

    float coverage = clamp(activation * as_horizon_override_opacity, 0.0, 1.0);
    // In Additive mode (as_horizon_blend_mode == 0) the extinction draw is
    // normally never issued at all -- additive mode only adds light. It is
    // now ALSO issued in that mode, but ONLY to carry sky horizon haze's
    // own darkening (see below); the base scattering band's own
    // transmittance-based extinction must stay a no-op here so enabling
    // haze doesn't quietly introduce a second, unrelated darkening effect
    // into additive mode's established brighten-only look.
    vec3 extinction = as_horizon_blend_mode == 0
                     ? vec3(1.0)
                     : mix(vec3(1.0), transmittance, coverage);

    // Sky-side companion to ASWaterHorizonFogStrength: the reference look
    // (a pale, warm haze band the sky and water both fade into at the
    // horizon) comes from lightening BOTH sides of the line, not from
    // blurring the seam between them (that approach was tried as a
    // screen-space post-process and abandoned -- see the doc). Reuse a
    // narrower core of the existing band/activation falloff so the haze
    // concentrates right at the horizon rather than spreading across the
    // whole existing scattering band.
    if (as_horizon_sky_haze_strength > 0.0001)
    {
        // haze_core's reach must scale with as_horizon_sky_haze_strength,
        // not be a fixed fraction of as_horizon_band_height (an earlier
        // version used `as_horizon_band_height * 0.6`, a constant ~7.2
        // degrees at the default band height, completely independent of
        // the strength slider -- so 0.1 and 1.0 produced the identical
        // vertical extent, only differing in opacity, which read as "the
        // gradient is always too tall regardless of the slider").
        //
        // Mirrors water horizon fog's own fix and the lessons learned
        // there, including one found only after a real screenshot test: a
        // "fixed-width window whose START moves outward with strength"
        // shape (`smoothstep(reach, reach + width, elevation)`) is a FLAT,
        // fully-opaque plateau from the horizon out to `reach`, fading
        // only at its far edge -- not a gradient peaking at the horizon,
        // and not genuinely subtle at low reach (a small reach still
        // produces a fully-opaque, if narrow, band). Fixed the same way as
        // water: a single smoothstep spanning the ENTIRE reach distance,
        // 1.0 (peak) exactly at the horizon (`absolute_elevation == 0`),
        // fading smoothly to 0.0 by `absolute_elevation == haze_reach`.
        // Reach still grows with strength; a separate intensity multiplier
        // fades the whole haze toward fully invisible independent of
        // reach.
        // Reach (as_horizon_sky_haze_strength, 0..2) and intensity
        // (as_horizon_sky_haze_intensity, 0..1, separate uniform/slider)
        // are independently controllable, mirroring water horizon fog's
        // own reach/intensity split per explicit user request.
        //
        // An fwidth(absolute_elevation)-based pixel-space reach was tried
        // here to match water horizon fog's own attempt at the same fix,
        // but produced visible horizontal banding across the WHOLE sky
        // (not just near the horizon) -- fwidth() on the sky dome's
        // triangulated mesh geometry evaluates inconsistently across
        // triangle/seam boundaries, and that inconsistency became visible
        // as discrete stripes rather than a smooth reach. Reverted to a
        // plain angular reach (same conclusion reached independently on
        // the water side after its own derivative attempts proved
        // unreliable): a small floor and a moderate ceiling, calibrated by
        // eye against real screenshots rather than derivative machinery.
        float haze_reach = mix(0.001, 0.12, clamp(as_horizon_sky_haze_strength / 2.0, 0.0, 1.0));
        float haze_core_raw = 1.0 - smoothstep(0.0, haze_reach, absolute_elevation);
        // Squaring a decreasing falloff narrows it; it does not broaden it.
        // Keep the smoothstep result itself so the middle of the gradient
        // remains visible instead of collapsing into a hard horizon strip.
        float haze_core = haze_core_raw;
        float haze_intensity = clamp(as_horizon_sky_haze_intensity, 0.0, 1.0);
        // Keep the haze horizontal. Combining elevation with an azimuth
        // cone bent the gradient down into the horizon around the sun.
        float haze_gate = clamp(haze_core * as_horizon_sun_fade, 0.0, 1.0);
        float haze_amount = haze_gate * haze_intensity;
        // Desaturate and mute toward a pale, dim haze tone rather than
        // brightening -- the reference look darkens/desaturates the sky
        // right at the line (mirroring water horizon fog's own darkening of
        // the water), not an added glow. haze_color's own darkness lowered
        // further still (* 0.05, was * 0.15, before that * 0.4) per
        // explicit user direction that intensity=1.0 still wasn't dark
        // enough -- this is another largely unverified guess at the right
        // absolute darkness, not measured against a debug view.
        float haze_luma = dot(base_color, vec3(0.299, 0.587, 0.114));
        vec3 haze_color = mix(vec3(haze_luma), base_color, 0.4) * 0.05;
        radiance = mix(radiance, haze_color, clamp(haze_amount, 0.0, 1.0));

        // Darkening ONLY `radiance` (this shader's own small additive
        // contribution on top of EEP) left the underlying EEP sky's own
        // brightness completely untouched, so the visible effect was
        // dwarfed by however bright EEP already was -- "close to nothing"
        // even at half strength. `extinction` (computed above) is the
        // MULTIPLICATIVE term that actually attenuates the EEP sky color
        // itself; darkening it too is what water horizon fog's equivalent
        // does structurally (it darkens the full composited `color`, not
        // an additive delta on top of it). Pull extinction darker/toward
        // the haze tone's luminance by the same haze_amount so the whole
        // visible sky pixel dims near the horizon, not just this
        // feature's own small contribution to it.
        extinction *= mix(vec3(1.0), vec3(haze_luma * 0.05), clamp(haze_amount, 0.0, 1.0));
    }

    if (as_horizon_pass == 0)
    {
        // The extinction draw uses (ZERO, SOURCE_COLOR), so white preserves
        // every non-radiance MRT while RGB transmittance attenuates only the
        // active sky radiance target. Alpha is preserved by separate factors.
        frag_data[0] = vec4(1.0);
        frag_data[1] = vec4(1.0);
        frag_data[2] = vec4(1.0);
        frag_data[3] = vec4(1.0);
#if defined(HAS_EMISSIVE)
        frag_data[3] = vec4(extinction, 1.0);
#else
        frag_data[0] = vec4(extinction, 1.0);
#endif
        return;
    }

    frag_data[0] = vec4(0.0);
    frag_data[1] = vec4(0.0);
    // In replace mode, per-target source alpha keeps categorical metadata
    // unchanged while the radiance target uses generated band coverage.
    frag_data[2] = as_horizon_blend_mode == 2
                 ? vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS)
                 : vec4(0.0);
#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(radiance, as_horizon_blend_mode == 2 ? coverage : 0.0);
#else
    frag_data[0] = vec4(radiance, as_horizon_blend_mode == 2 ? coverage : 0.0);
#endif
}
