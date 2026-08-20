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

uniform int   sample_count;
uniform float scatter_intensity;
uniform float scatter_asymmetry;

// TEMPORARY development aid - remove once the effect is confirmed working.
// 0: normal. 1: (unused here, composite pass handles the "replace screen"
// mode). 2: output raw occlusion (1-min_visibility) in [0,1] as grayscale.
// 3: output raw min_visibility directly (pre-inversion). 4: output the
// reconstructed view-space ray_end position as RGB (abs(pos)/64, clamped),
// to sanity-check getPosition()/depth reconstruction independently of any
// shadow logic - should vary smoothly across the screen and roughly match
// scene depth (near geometry = small values/dark, far = larger/brighter,
// sky = clamped to the MAX_MARCH_DISTANCE cap). 5: output ray_len/128 as
// grayscale (raw distance, cheaper to read than mode 4's RGB). 6: output the
// raw linear depth buffer value (getDepth()) as grayscale, before any
// inv_proj transform at all - isolates whether the depth buffer read itself
// is sane (should vary with scene depth, not be flat). 7: output
// getPositionWithNDC(vec3(0,0,0)) as RGB (abs/64, clamped) - a FIXED NDC
// point at screen center, mid-depth, independent of pos_screen entirely.
// If this differs from what mode 4 shows at screen center, or reads as a
// suspiciously round/zero value, inv_proj itself (not the depth read or the
// screen-coordinate math) is the broken link.
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

// Caps the march distance for sky/horizon pixels (effectively infinite depth)
// so the loop stays bounded and scatter does not blow out with distance.
const float MAX_MARCH_DISTANCE = 128.0;

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
        frag_color = vec4(vec3(getDepth(pos_screen)), 1.0);
        return;
    }

    if (debug_mode == 7)
    {
        vec3 fixed_pos = getPositionWithNDC(vec3(0.0, 0.0, 0.0));
        frag_color = vec4(clamp(abs(fixed_pos) / 64.0, 0.0, 1.0), 1.0);
        return;
    }

    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);

    float cos_theta = dot(ray_dir, light_dir);
    float phase = phaseHG(cos_theta, scatter_asymmetry);

    int   steps    = max(sample_count, 1);
    float step_len = ray_len / float(steps);

    // Track the darkest point along the ray, not the average visibility.
    // Open sky/unoccluded rays are lit almost everywhere (visibility ~1
    // along the whole ray), so averaging visibility - as an earlier version
    // of this shader did - adds a large, nearly uniform wash across the
    // whole frame instead of a spatially-varying shaft effect. A real god
    // ray is defined by rays that pass through at least one occluder; rays
    // that never enter shadow should contribute ~nothing.
    float min_visibility = 1.0;

    for (int i = 0; i < steps; ++i)
    {
        float t = (float(i) + 0.5) * step_len;
        vec3 sample_pos = ray_dir * t;

        // norm = light_dir makes sampleDirectionalShadow's surface-bias term
        // (dot(norm, light_dir)) evaluate to 1.0, i.e. no extra bias offset
        // - the correct choice for a sample in empty space, not on a surface.
        float visibility = sampleDirectionalShadow(sample_pos, light_dir, pos_screen);

        // Guard against NaN poisoning the whole ray via min() - clamp to a
        // valid [0,1] visibility and skip anything non-finite.
        if (visibility == visibility) // false only for NaN
        {
            min_visibility = min(min_visibility, clamp(visibility, 0.0, 1.0));
        }
    }

    // Occlusion in [0,1]: 0 for rays that never touched shadow, up to 1 for
    // rays that were fully shadowed somewhere along their length.
    float occlusion = 1.0 - min_visibility;

    if (debug_mode == 2)
    {
        frag_color = vec4(vec3(occlusion), 1.0);
        return;
    }

    if (debug_mode == 3)
    {
        frag_color = vec4(vec3(min_visibility), 1.0);
        return;
    }

    float distance_factor = ray_len / MAX_MARCH_DISTANCE;
    float scatter = occlusion * phase * scatter_intensity * distance_factor;
    scatter = clamp(scatter, 0.0, 1.0);

    frag_color = vec4(sunlight_color * scatter, 1.0);
}
