/**
 * @file asVolumetricExactResolveF.glsl
 * @brief AyaneStorm exact volumetric lighting - lit integral to scatter.
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

// Full resolution, one pixel in, one pixel out (no gather, no jitter).
// lit = G(ray_end) - shadowed, where shadowed was accumulated exactly by
// asVolumetricExactVolumeF.glsl. Brightness uses the same formula and
// constants as asVolumetricLightF.glsl so both modes match visually.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D depthMap;
uniform sampler2D as_exact_accum; // R32F shadowed transmittance-weighted length
uniform vec2 screen_res;
uniform float scatter_albedo;
uniform float scatter_asymmetry;
uniform float scatter_density;
uniform float as_max_distance;
uniform vec3  as_active_light_dir;
uniform vec3  as_active_light_color;
uniform vec2  as_disc_sin_cos; // x = sin(radius), y = cos(radius)
uniform int   debug_mode;      // 2: occlusion, 3: visibility (grayscale)

vec4 getPositionWithDepth(vec2 pos_screen, float depth);

// Must match asVolumetricLightF.glsl: brightness normalisation, independent
// of as_max_distance so the march range never changes brightness.
const float BRIGHTNESS_SCALE = 64.0;
const float BRIGHTNESS_REFERENCE_DISTANCE = 128.0;

float transmittedLength(float t)
{
    float x = scatter_density * t;
    if (x < 1e-3)
    {
        return t * (1.0 - 0.5 * x + x * x * (1.0 / 6.0));
    }
    return (1.0 - exp(-x)) / scatter_density;
}

float phaseHG(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(depthMap, px, 0).r;
    vec3 scene_pos = getPositionWithDepth(gl_FragCoord.xy / screen_res, depth).xyz;
    float scene_dist = length(scene_pos);
    float ray_end = min(scene_dist, as_max_distance);
    vec3 ray_dir = scene_pos / max(scene_dist, 1e-6);

    float total = transmittedLength(ray_end);
    float shadowed = clamp(texelFetch(as_exact_accum, px, 0).r, 0.0, total);
    float lit = total - shadowed;

    if (debug_mode == 2 || debug_mode == 3)
    {
        float visibility = total > 0.0 ? lit / total : 1.0;
        frag_color = vec4(vec3(debug_mode == 2 ? 1.0 - visibility : visibility), 1.0);
        return;
    }

    // Same disc-clamped phase angle as asVolumetricLightF.glsl.
    float raw_cos_theta = clamp(dot(ray_dir, as_active_light_dir), -1.0, 1.0);
    float cos_theta = 1.0;
    if (raw_cos_theta < as_disc_sin_cos.y)
    {
        float sin_theta = sqrt(max(1.0 - raw_cos_theta * raw_cos_theta, 0.0));
        cos_theta = raw_cos_theta * as_disc_sin_cos.y + sin_theta * as_disc_sin_cos.x;
    }
    float phase = phaseHG(cos_theta, scatter_asymmetry);

    float scatter = scatter_density * scatter_albedo * BRIGHTNESS_SCALE *
                    phase * (lit / BRIGHTNESS_REFERENCE_DISTANCE);
    scatter = clamp(scatter, 0.0, 1.0);

    frag_color = vec4(as_active_light_color * scatter, 0.0);
}
