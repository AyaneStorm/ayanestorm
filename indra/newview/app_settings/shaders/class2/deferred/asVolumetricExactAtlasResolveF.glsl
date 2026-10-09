/**
 * @file asVolumetricExactAtlasResolveF.glsl
 * @brief AyaneStorm exact volumetric lighting - transparency atlas resolve.
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

// Writes the 4x4 transparency atlas (same layout and meaning as
// asVolumetricAtlasF.glsl, which alpha/material shaders sample) from the
// exact per-slice shadowed integrals of asVolumetricExactAtlasVolumeF.glsl:
// RGB = light color * scatter accumulated from the camera to the slice far
// distance, A = scene transmittance at that distance. No sampling, no dither.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D as_atlas_slices0; // slices 0-3 (RGBA32F, tile resolution)
uniform sampler2D as_atlas_slices1; // slices 4-7
uniform sampler2D as_atlas_slices2; // slices 8-11
uniform sampler2D as_atlas_slices3; // slices 12-15
uniform ivec2 as_tile_size;
uniform float as_slice_g[16];       // G(slice end distance), from the CPU
uniform float scatter_albedo;
uniform float scatter_asymmetry;
uniform float scatter_density;
uniform float transmittance_density;
uniform int atlas_debug;
uniform vec3 as_active_light_dir;
uniform vec3 as_active_light_color;
uniform vec2 as_disc_sin_cos; // x = sin(radius), y = cos(radius)

vec3 getPositionWithNDC(vec3 ndc);

const float MAX_MARCH_DISTANCE = 128.0;
const float ATLAS_SLICES = 16.0;
// Must match asVolumetricLightF.glsl / asVolumetricExactResolveF.glsl.
const float BRIGHTNESS_SCALE = 64.0;
// Must match asVolumetricCompositeF.glsl / asVolumetricAtlasF.glsl.
const float MAX_SCENE_OPTICAL_DEPTH = 2.3;

float phaseHG(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 tile = min(px / as_tile_size, ivec2(3));
    ivec2 local = px - tile * as_tile_size;
    int slice = tile.y * 4 + tile.x;

    vec4 group;
    if (slice < 4)
    {
        group = texelFetch(as_atlas_slices0, local, 0);
    }
    else if (slice < 8)
    {
        group = texelFetch(as_atlas_slices1, local, 0);
    }
    else if (slice < 12)
    {
        group = texelFetch(as_atlas_slices2, local, 0);
    }
    else
    {
        group = texelFetch(as_atlas_slices3, local, 0);
    }
    float total = as_slice_g[slice];
    float shadowed = clamp(group[slice & 3], 0.0, total);
    float lit = total - shadowed;

    // Same per-texel ray as the accumulation pass (texel center).
    vec2 local_uv = (vec2(local) + 0.5) / vec2(as_tile_size);
    vec3 ray_dir = normalize(getPositionWithNDC(vec3(local_uv * 2.0 - 1.0, 1.0)));

    // Disc-clamped phase, as in asVolumetricExactResolveF.glsl.
    float raw_cos_theta = clamp(dot(ray_dir, as_active_light_dir), -1.0, 1.0);
    float cos_theta = 1.0;
    if (raw_cos_theta < as_disc_sin_cos.y)
    {
        float sin_theta = sqrt(max(1.0 - raw_cos_theta * raw_cos_theta, 0.0));
        cos_theta = raw_cos_theta * as_disc_sin_cos.y + sin_theta * as_disc_sin_cos.x;
    }
    float phase = phaseHG(cos_theta, scatter_asymmetry);

    float scatter = clamp(scatter_density * scatter_albedo * BRIGHTNESS_SCALE *
                          phase * (lit / MAX_MARCH_DISTANCE), 0.0, 1.0);

    // Scene transmittance, identical to asVolumetricAtlasF.glsl (view-depth
    // metric, smooth optical-depth ceiling, 100-128 m sky fade).
    float fraction = float(slice + 1) / ATLAS_SLICES;
    float slice_end = fraction * fraction * MAX_MARCH_DISTANCE;
    float view_depth = slice_end * abs(ray_dir.z);
    float optical_depth = transmittance_density * view_depth;
    float limited_depth = MAX_SCENE_OPTICAL_DEPTH *
                          (1.0 - exp(-optical_depth / MAX_SCENE_OPTICAL_DEPTH));
    float beer_lambert = exp(-limited_depth);
    float sky_fade = smoothstep(100.0, MAX_MARCH_DISTANCE, view_depth);
    float transmittance = mix(beer_lambert, 1.0, sky_fade);

    if (atlas_debug != 0)
    {
        // Same diagnostic encoding as asVolumetricAtlasF.glsl (debug 10).
        vec3 diagnostic = clamp(scatter * vec3(1.0, 16.0, 256.0), 0.0, 1.0);
        vec2 edge = min(local_uv, vec2(1.0) - local_uv);
        if (min(edge.x, edge.y) < 0.006)
        {
            diagnostic = vec3(1.0, 0.0, 0.0);
        }
        frag_color = vec4(diagnostic, 1.0);
        return;
    }
    frag_color = vec4(as_active_light_color * scatter, transmittance);
}
