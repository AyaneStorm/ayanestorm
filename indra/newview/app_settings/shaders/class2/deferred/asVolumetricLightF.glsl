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

uniform vec3  sun_dir;
uniform vec3  moon_dir;
uniform int   sun_up_factor;
uniform vec3  sunlight_color;
uniform vec3  moonlight_color;
uniform vec3  moon_horizon_tint;
uniform float moon_horizon_tint_strength;
uniform float moon_horizon_elevation;
uniform float moon_phase_illumination;

uniform int   sample_count;
uniform float scatter_albedo;
uniform float scatter_asymmetry;
uniform float scatter_density;

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

// The known-working, real shadow entry point used by every other caller in
// this codebase (sunLightF.glsl, alphaF.glsl, materialF.glsl, waterF.glsl,
// etc.) - defined in shadowUtil.glsl, forward-declared here the same way
// those callers do it. A custom single-cascade selector was tried here
// first and produced ~100% occlusion everywhere despite the cascade index
// mapping, matrix math, GL_TEXTURE_COMPARE_FUNC, and edge-clamp behavior
// all checking out individually under static review - rather than keep
// debugging a reimplementation, this calls the exact function every other
// shadow-consuming shader in the codebase already relies on.
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);

// Henyey-Greenstein phase function: biases in-scatter toward (g > 0) or away
// from (g < 0) the view direction, matching how sunbeams brighten as you
// look toward the light source.
float phaseHG(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

// Interleaved gradient noise (Jimenez 2014): a cheap, high-frequency
// per-pixel dither in [0,1). Offsetting each ray's first sample by this value
// (scaled by one step length) turns the fixed banding from a constant sample
// count into fine, far-less-objectionable grain, since neighboring pixels no
// longer land on the same shadow-transition step.
float interleavedGradientNoise(vec2 screen_pos)
{
    const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screen_pos, magic.xy)));
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
const float SUN_MOON_ANGULAR_RADIUS = 0.0372; // radians, ~2.1 degrees (4x real size)

// Caps the march distance for sky/horizon pixels (effectively infinite depth)
// so the loop stays bounded and scatter does not blow out with distance.
const float MAX_MARCH_DISTANCE = 128.0;

// Fixed brightness normalization so that density*albedo (a physically
// bounded [0, ~0.25]*[0,1] product) produces roughly the same visible
// scatter brightness the old dimensionless scatter_intensity multiplier
// (default 0.8) used to. Derived from matching output at density=0.012,
// albedo=1.0 (this feature's new defaults) against the old formula's
// default output - NOT analytically exact, since it depends on phaseHG's
// existing normalization and this integral's existing scale. Expect to
// re-tune this after first build/playtest. Must stay numerically identical
// to asVolumetricAtlasF.glsl's copy of the same constant.
const float BRIGHTNESS_SCALE = 64.0;

void main()
{
    vec2 pos_screen = vary_fragcoord.xy;
    vec4 pos = getPosition(pos_screen);

    // View space: the camera sits at the origin, so the ray is just the
    // fragment's own view-space position.
    vec3  ray_end   = pos.xyz;
    float ray_len   = min(length(ray_end), MAX_MARCH_DISTANCE);
    vec3  ray_dir   = ray_end / max(length(ray_end), 1e-4);

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

    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);

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
    float raw_cos_theta = dot(ray_dir, light_dir);
    float raw_angle = acos(clamp(raw_cos_theta, -1.0, 1.0));
    float disc_clamped_angle = max(raw_angle - SUN_MOON_ANGULAR_RADIUS, 0.0);
    float cos_theta = cos(disc_clamped_angle);
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
    float jitter = interleavedGradientNoise(gl_FragCoord.xy);

    // Integrate incident light along the ray. Lit air scatters light toward
    // the camera; shadowed air does not. Inverting this term would make
    // occluders glow and open shafts dark, which is the opposite of
    // volumetric single scattering.
    float accumulated_visibility = 0.0;
    float attenuated_visibility_integral = 0.0;

    for (int i = 0; i < steps; ++i)
    {
        float t = (float(i) + jitter) * step_len;
        vec3 sample_pos = ray_dir * t;

        // norm = light_dir makes sampleDirectionalShadow's surface-bias term
        // (dot(norm, light_dir)) evaluate to 1.0, i.e. no extra bias offset
        // - the correct choice for a sample in empty space, not on a surface.
        // A former per-step "disc jitter" perturbed this normal, but the
        // shared sampler does not accept a light direction and pcfShadow()
        // does not use its normal argument. That work never moved a shadow
        // lookup; its only effect was a tiny, inappropriate surface offset.
        float visibility = sampleDirectionalShadow(sample_pos, light_dir, pos_screen);

        // Guard against a bad shadow sample poisoning the whole integral.
        if (visibility == visibility) // false only for NaN
        {
            visibility = clamp(visibility, 0.0, 1.0);
            accumulated_visibility += visibility;
            // Beer-Lambert view-path extinction prevents a long sequence of
            // weakly lit samples from remaining as prominent as nearby air.
            attenuated_visibility_integral += visibility *
                exp(-scatter_density * t) * step_len;
        }
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

    vec3 light_color = (sun_up_factor == 1) ? sunlight_color : moonlight_color;
    // Match the moon disc's warm horizon tint without changing scene light.
    if (sun_up_factor != 1)
    {
        float horizon_tint_amount = (1.0 - smoothstep(0.0, 0.35, max(moon_horizon_elevation, 0.0)))
                                  * clamp(moon_horizon_tint_strength, 0.0, 1.0);
        light_color *= mix(vec3(1.0), clamp(moon_horizon_tint, 0.0, 1.0), horizon_tint_amount);
        light_color *= clamp(moon_phase_illumination, 0.0, 1.0);
    }
    frag_color = vec4(light_color * scatter, 1.0);
}
