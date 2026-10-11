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
// lit = G(D(ray_end)) - shadowed, where shadowed was accumulated exactly by
// asVolumetricExactVolumeF.glsl (D = faded medium length, same function).
// Physical single scattering, HDR: scatter = albedo * density * phase * lit,
// no clamp, no range normalisation (scene is pre-tonemap here).
//
// Clouds (phase 4, asVolumetricCloudUtilF.glsl):
// - near shafts: the exact lit integral times the mean cloud transmittance
//   along the ray, weighted like the medium (e^-sigma t);
// - cloud rays: beyond the exact range a thin far medium (own density
//   as_cloud_ray_density, like clear air; the near density is fog-like and
//   would absorb everything past a few hundred metres) continues, lit only
//   by the cloud shadow map. Its density ramps in where the exact medium
//   fades out, up to the scene or the cloud layer, and it is not dimmed by
//   the near medium (as the composite stops attenuating the scene there).
//   Added light only: SL's sky, clouds and haze are left as they are.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D depthMap;
uniform sampler2D as_exact_accum; // R32F shadowed transmittance-weighted length
uniform vec2 screen_res;
uniform float scatter_albedo;
uniform float scatter_asymmetry;
uniform float scatter_density;
uniform float as_fade_start;
uniform float as_fade_end;
uniform vec3  as_active_light_dir;
uniform vec3  as_active_light_color;
uniform vec2  as_disc_sin_cos; // x = sin(radius), y = cos(radius)
uniform float as_cloud_ray_density;  // far medium extinction (per metre)
uniform float as_cloud_ray_strength; // far medium brightness multiplier
// 2: occlusion, 3: visibility, 4: cloud shadow, 5: near shaft dimming,
// 6: cloud-ray light (lit share of the far medium).
uniform int   debug_mode;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);

// asVolumetricCloudUtilF.glsl
bool cloudsEnabled();
float cloudTransmittance(vec3 view_pos, float lod);
float meanCloudTransmittance(vec3 ray_dir, float t_end, float sigma, int steps);
float cloudLayerDistance(vec3 ray_dir);
float cloudRayLength(vec3 ray_dir, float t0, float t1, float sigma, float ramp_len,
                     int steps, out float lit_share);
float cloudSkyDebug(vec3 ray_dir);

const int SHAFT_CLOUD_STEPS = 8;
const int CLOUD_RAY_STEPS = 16;
// Cloud rays end at the scene or the cloud layer, at most this far.
const float CLOUD_RAY_MAX_DISTANCE = 20000.0;
// Far medium phase asymmetry: broad, so the rays show across the sky
// instead of piling up as a glow around the sun (the near shafts keep the
// user's strongly forward scatter_asymmetry).
const float CLOUD_RAY_ASYMMETRY = 0.2;

float transmittedLength(float t)
{
    float x = scatter_density * t;
    if (x < 1e-3)
    {
        return t * (1.0 - 0.5 * x + x * x * (1.0 / 6.0));
    }
    return (1.0 - exp(-x)) / scatter_density;
}

// Must match asVolumetricExactVolumeF.glsl.
float fadedLength(float t)
{
    float ramp = max(as_fade_end - as_fade_start, 1e-3);
    float x = clamp(t - as_fade_start, 0.0, ramp);
    return min(t, as_fade_start) + x - 0.5 * x * x / ramp;
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
    float ray_end = min(scene_dist, as_fade_end);
    vec3 ray_dir = scene_pos / max(scene_dist, 1e-6);

    float total = transmittedLength(fadedLength(ray_end));
    float shadowed = clamp(texelFetch(as_exact_accum, px, 0).r, 0.0, total);
    float lit = total - shadowed;

    if (debug_mode == 2 || debug_mode == 3)
    {
        float visibility = total > 0.0 ? lit / total : 1.0;
        frag_color = vec4(vec3(debug_mode == 2 ? 1.0 - visibility : visibility), 1.0);
        return;
    }

    float cloud = meanCloudTransmittance(ray_dir, ray_end, scatter_density, SHAFT_CLOUD_STEPS);

    if (debug_mode == 4 || debug_mode == 5)
    {
        // 4: cloud shadow on the scene; on sky pixels, the baked map seen
        //    from the camera (must reproduce the drawn clouds toward the
        //    light, grey elsewhere). 5: near shaft dimming.
        float value = debug_mode == 5 ? cloud :
                      (depth < 1.0 ? cloudTransmittance(scene_pos, 0.0) : cloudSkyDebug(ray_dir));
        frag_color = vec4(vec3(value), 1.0);
        return;
    }

    // Cloud rays: the far medium from the start of the exact fade to the
    // scene (or the cloud layer for the sky).
    float far_lit = 0.0;
    float far_share = 1.0;
    if (cloudsEnabled())
    {
        float far_end = min(depth < 1.0 ? scene_dist : cloudLayerDistance(ray_dir),
                            CLOUD_RAY_MAX_DISTANCE);
        far_lit = cloudRayLength(ray_dir, as_fade_start, far_end, as_cloud_ray_density,
                                 as_fade_end - as_fade_start, CLOUD_RAY_STEPS, far_share);
    }

    if (debug_mode == 6)
    {
        frag_color = vec4(vec3(far_share), 1.0);
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

    float far_phase = phaseHG(cos_theta, CLOUD_RAY_ASYMMETRY);
    float scatter = scatter_albedo *
                    (phase * scatter_density * lit * cloud +
                     far_phase * as_cloud_ray_strength * as_cloud_ray_density * far_lit);

    frag_color = vec4(as_active_light_color * scatter, 0.0);
}
