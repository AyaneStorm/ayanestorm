/**
 * @file asVolumetricLightF.glsl
 * @brief AyaneStorm optional volumetric lighting - raymarched sun/moon shadow scatter.
 * @author chanayane@firestorm
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (c) 2026 Chanayane @ Second Life
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform int   sample_count;
uniform float scatter_albedo;
uniform float scatter_asymmetry;
uniform float scatter_density;
uniform vec3  as_active_light_dir;
uniform vec3  as_active_light_color;
uniform vec2  as_disc_sin_cos; // x = sin(radius), y = cos(radius)
uniform vec4  shadow_clip;

// TEMPORARY development aid - remove once the effect is confirmed working.
// 0: normal. 1: (unused here, composite pass handles the "replace screen"
// mode). 2: output raw mean occlusion in [0,1] as grayscale.
// 3: output raw mean visibility directly (pre-inversion). 4: output the
// reconstructed view-space ray_end position as RGB (abs(pos)/64, clamped),
// to sanity-check getPosition()/depth reconstruction independently of any
// shadow logic - should vary smoothly across the screen and roughly match
// scene depth (near geometry = small values/dark, far = larger/brighter,
// sky = clamped to the MAX_MARCH_DISTANCE cap). 5: output ray_len/128 as
// grayscale (raw distance, cheaper to read than mode 4's RGB). 6: output the
// raw device-depth proximity (1-getDepth), amplified for display, before any
// inv_proj transform at all - isolates whether the depth buffer read itself
// varies despite perspective depth clustering near 1. 7: sanity-checks
// getPositionWithNDC(vec3(0.5,0.25,0)) - a FIXED off-center NDC point at
// mid-depth, independent of pos_screen entirely. A green ramp means only that
// the result has a finite, nonzero magnitude; it does not prove the matrix is
// otherwise correct. Black means a genuinely near-zero length, while magenta
// means NaN/Inf (a NaN length fails every numeric comparison, so it must be
// checked explicitly rather than falling through to the zero case).
uniform int debug_mode;

vec4 getPosition(vec2 pos_screen);
float getDepth(vec2 pos_screen);
vec3 getPositionWithNDC(vec3 ndc);

// Defined in asVolumetricShadowUtil.glsl, which this program links in
// separately (see AS_VOL_SINGLE_CASCADE there for the current single-cascade,
// single-fetch selector).
float asVolumetricDirectionalShadow(vec3 sample_pos, vec2 pos_screen);

// Henyey-Greenstein phase function: biases in-scatter toward (g > 0) or away
// from (g < 0) the view direction, matching how sunbeams brighten as you
// look toward the light source.
float phaseHG(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

// 4x4 Bayer matrix: exactly stratified offsets over a 4x4 pixel block, so the
// composite's 4x4 depth-aware gather reconstructs 16x sample_count uniformly
// spaced samples per block with no lattice bands and no silhouette ghosts
// (see doc/volumetric_lighting_bugfix_and_speedup_plan.md section 3).
float volumetricJitter(vec2 screen_pos)
{
    const float bayer[16] = float[16](
         0.0,  8.0,  2.0, 10.0,
        12.0,  4.0, 14.0,  6.0,
         3.0, 11.0,  1.0,  9.0,
        15.0,  7.0, 13.0,  5.0);
    ivec2 p = ivec2(screen_pos) & 3;
    return (bayer[p.y * 4 + p.x] + 0.5) / 16.0;
}

// Real angular radius of the sun/moon as seen from a planetary surface is
// only ~0.53 degrees (0.0093 rad) - too small to visibly soften ray
// convergence or occlusion-edge penumbra at typical in-viewer distances.
// Exaggerated 4x here for a visible artistic effect (not physically
// accurate) so the disc-jitter fix (see the "sun/moon treated as a point"
// limitation this addresses: a single occluded direction, e.g. a roofline
// clipping the shadow-map sample point, was killing that sample's entire
// visibility contribution even when most of the physical disc was still
// unoccluded in the same frame) is actually noticeable. Revisit this
// multiplier if it reads as too soft/too sharp once tested in-viewer.
// Caps the march distance for sky/horizon pixels (effectively infinite depth)
// so the loop stays bounded and scatter does not blow out with distance.
const float MAX_MARCH_DISTANCE = 128.0;

// Fixed brightness normalization so that density*albedo (a physically
// bounded [0, ~0.25]*[0,1] product) produces roughly the same visible
// scatter brightness the old dimensionless scatter_intensity multiplier
// (default 0.8) used to. Derived from matching output at the original
// density=0.012, albedo=1.0 tuning point against the old formula's default
// output - NOT analytically exact, since it depends on phaseHG's existing
// normalization and this integral's existing scale. The shipped albedo
// default was subsequently tuned to 0.35. Must stay numerically identical
// to asVolumetricAtlasF.glsl's copy of the same constant.
const float BRIGHTNESS_SCALE = 64.0;

void main()
{
    vec2 pos_screen = vary_fragcoord.xy;
    vec4 pos = getPosition(pos_screen);

    // View space: the camera sits at the origin, so the ray is just the
    // fragment's own view-space position.
    vec3  ray_end   = pos.xyz;
    float endpoint_length = length(ray_end);
    float ray_len   = min(endpoint_length, MAX_MARCH_DISTANCE);
    vec3  ray_dir   = ray_end / max(endpoint_length, 1e-4);

    if (debug_mode == 4)
    {
        frag_color = vec4(clamp(abs(ray_end) / 64.0, 0.0, 1.0), 1.0);
        return;
    }

    if (debug_mode == 5)
    {
        frag_color = vec4(vec3(ray_len / MAX_MARCH_DISTANCE), 1.0);
        return;
    }

    if (debug_mode == 6)
    {
        // Perspective device depth is packed extremely close to 1 for most
        // scene distances. Reverse and amplify it so variation is visible.
        float depth_proximity = clamp((1.0 - getDepth(pos_screen)) * 1000.0, 0.0, 1.0);
        frag_color = vec4(vec3(depth_proximity), 1.0);
        return;
    }

    if (debug_mode == 7)
    {
        // An off-axis point exercises X/Y as well as Z. Screen-center would
        // legitimately normalize to almost pure blue and hide those terms.
        vec3 fixed_pos = getPositionWithNDC(vec3(0.5, 0.25, 0.0));
        float fixed_length = length(fixed_pos);
        // Magenta previously meant "length <= 1e-6", conflating a genuinely
        // near-zero vector with a NaN/Inf length (NaN > 1e-6 is false in
        // GLSL, so a broken matrix producing NaN would ALSO show magenta
        // here, indistinguishable from a real zero result). Encode the raw
        // magnitude as a log-scaled green ramp so the cases are
        // distinguishable: green = finite and nonzero, black = near-zero,
        // and magenta = NaN/Inf. A finite result can still come from a stale
        // or otherwise incorrect matrix, so green is not a correctness proof.
        bool is_finite = (fixed_length == fixed_length) && (fixed_length < 1e30);
        if (!is_finite)
        {
            frag_color = vec4(1.0, 0.0, 1.0, 1.0);
        }
        else if (fixed_length <= 1e-6)
        {
            frag_color = vec4(0.0, 0.0, 0.0, 1.0); // black: genuinely ~zero
        }
        else
        {
            float log_len = clamp(log2(fixed_length + 1.0) / 10.0, 0.0, 1.0);
            frag_color = vec4(0.0, log_len, 0.0, 1.0); // green ramp: real magnitude
        }
        return;
    }

    vec3 light_dir = as_active_light_dir;

    // Rays visually converged to a single point at the sun/moon's exact
    // center regardless of its true angular size, no matter how wide the
    // per-step shadow-sample disc jitter below was made - jittering
    // light_dir per-pixel only adds noise around the same fixed peak, it
    // does not widen it, since phaseHG's falloff (sharpened by a high
    // scatter_asymmetry) is angularly far narrower than the disc's actual
    // radius. Instead, clamp the angle fed into phaseHG so it can never read
    // sharper than the disc's own angular radius: any ray_dir within
    // SUN_MOON_ANGULAR_RADIUS of light_dir is treated as if it were exactly
    // at that radius (the disc's edge), giving every direction inside the
    // disc a comparably high phase value instead of only the exact center
    // direction - this is what actually gives the bright region real
    // angular width. Computed once per pixel (not per march step): it only
    // depends on the fixed light_dir/ray_dir pair for this pixel, not on
    // the per-step shadow-sampling jitter.
    float raw_cos_theta = clamp(dot(ray_dir, light_dir), -1.0, 1.0);
    float cos_theta = 1.0;
    if (raw_cos_theta < as_disc_sin_cos.y)
    {
        float sin_theta = sqrt(max(1.0 - raw_cos_theta * raw_cos_theta, 0.0));
        cos_theta = raw_cos_theta * as_disc_sin_cos.y +
                    sin_theta * as_disc_sin_cos.x;
    }
    float phase = phaseHG(cos_theta, scatter_asymmetry);

    // Preserve approximately the configured full-range sample spacing while
    // avoiding the full 16/32 shadow lookups for rays ending on nearby
    // geometry. Four samples is the conservative floor for stable near-field
    // shadow transitions; rays reaching MAX_MARCH_DISTANCE retain the exact
    // configured count and therefore their previous long-range quality.
    int max_steps = max(sample_count, 1);
    int min_steps = min(4, max_steps);
    int steps = clamp(int(ceil(float(max_steps) * ray_len /
                               MAX_MARCH_DISTANCE)),
                      min_steps, max_steps);
    float step_len = ray_len / float(steps);

    // Dither the ray's starting offset per-pixel so fixed-step banding turns
    // into fine grain instead of visible stepped rings at shadow boundaries.
    float jitter = volumetricJitter(gl_FragCoord.xy);

    // Integrate incident light along the ray. Lit air scatters light toward
    // the camera; shadowed air does not. Inverting this term would make
    // occluders glow and open shafts dark, which is the opposite of
    // volumetric single scattering.
    float accumulated_visibility = 0.0;
    float attenuated_visibility_integral = 0.0;

    float sample_distance = jitter * step_len;
    vec3 sample_pos = ray_dir * sample_distance;
    vec3 sample_step = ray_dir * step_len;
    float attenuation = scatter_density > 0.0
        ? exp(-scatter_density * sample_distance) : 1.0;
    float attenuation_decay = scatter_density > 0.0
        ? exp(-scatter_density * step_len) : 1.0;

    // Integrate Beer-Lambert transmittance over the complete represented
    // segment instead of evaluating it only at the jittered sample point.
    // Dividing by density preserves the existing outer density multiplier
    // and converges to step_len as density approaches zero. Use the series
    // form near zero to avoid cancellation in 1-exp(-x).
    float optical_step = scatter_density * step_len;
    float unattenuated_segment_integral;
    if (scatter_density <= 0.0)
    {
        unattenuated_segment_integral = step_len;
    }
    else if (optical_step < 1e-3)
    {
        unattenuated_segment_integral = step_len *
            (1.0 - 0.5 * optical_step +
             optical_step * optical_step * (1.0 / 6.0));
    }
    else
    {
        unattenuated_segment_integral =
            (1.0 - attenuation_decay) / scatter_density;
    }
    float segment_integral = attenuation * unattenuated_segment_integral;

    for (int i = 0; i < steps; ++i)
    {
        // The established shadow contract returns fully lit at and beyond
        // this boundary. Later samples on the same forward ray are farther
        // away, so their discrete contribution can be summed directly.
        if (ray_dir.z < 0.0 && sample_pos.z <= -shadow_clip.w)
        {
            int remaining_steps = steps - i;
            float remaining = float(remaining_steps);
            accumulated_visibility += remaining;

            float segment_sum;
            if (abs(1.0 - attenuation_decay) < 1e-6)
            {
                segment_sum = segment_integral * remaining;
            }
            else
            {
                segment_sum = segment_integral *
                    (1.0 - pow(attenuation_decay, remaining)) /
                    (1.0 - attenuation_decay);
            }
            attenuated_visibility_integral += segment_sum;
            break;
        }

        float visibility = asVolumetricDirectionalShadow(sample_pos, pos_screen);

        // Guard against a bad shadow sample poisoning the whole integral.
        if (visibility == visibility) // false only for NaN
        {
            visibility = clamp(visibility, 0.0, 1.0);
            accumulated_visibility += visibility;
            // Beer-Lambert view-path extinction prevents a long sequence of
            // weakly lit samples from remaining as prominent as nearby air.
            attenuated_visibility_integral += visibility * segment_integral;
        }

        sample_pos += sample_step;
        segment_integral *= attenuation_decay;
    }

    float mean_visibility = accumulated_visibility / float(steps);
    float occlusion = 1.0 - mean_visibility;

    if (debug_mode == 2)
    {
        frag_color = vec4(vec3(occlusion), 1.0);
        return;
    }

    if (debug_mode == 3)
    {
        frag_color = vec4(vec3(mean_visibility), 1.0);
        return;
    }

    float scatter = scatter_density * scatter_albedo * BRIGHTNESS_SCALE *
                    phase * (attenuated_visibility_integral / MAX_MARCH_DISTANCE);
    scatter = clamp(scatter, 0.0, 1.0);

    frag_color = vec4(as_active_light_color * scatter, 1.0);
}
