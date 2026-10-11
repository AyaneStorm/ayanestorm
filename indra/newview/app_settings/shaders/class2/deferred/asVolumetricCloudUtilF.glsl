/**
 * @file asVolumetricCloudUtilF.glsl
 * @brief AyaneStorm exact volumetric lighting - cloud shadow map lookups.
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

// Shared by the exact resolves: lookups of the baked cloud shadow map
// (asVolumetricCloudShadowF.glsl, same cloud model) and prefiltered marches
// of it along view rays. Uniforms set by ASVolumetricLighting::applyCloudUniforms().

uniform int   as_cloud_enabled;
uniform sampler2D as_cloud_map;       // R16F cloud transmittance, mipmapped
uniform mat4  as_cloud_from_view;     // view position -> anchor-relative dome position
uniform vec3  as_cloud_e1;            // map plane basis (dome space)
uniform vec3  as_cloud_e2;
uniform vec3  as_cloud_light;         // unit direction toward the light
uniform float as_cloud_radius;        // sphere radius
uniform float as_cloud_centre_depth;  // sphere centre below the anchor
uniform float as_cloud_extent;        // map half extent
uniform float as_cloud_texels_per_metre;

bool cloudsEnabled()
{
    return as_cloud_enabled != 0;
}

// View-space direction -> sky dome direction.
vec3 cloudDomeDir(vec3 view_dir)
{
    return mat3(as_cloud_from_view) * view_dir;
}

// Map lookup for an anchor-relative dome position: lit outside the cloud
// sphere (above the clouds) or outside the map.
float cloudTransmittanceDome(vec3 p, float lod)
{
    vec3 pc = p + vec3(0.0, as_cloud_centre_depth, 0.0);
    vec2 uv = vec2(dot(p, as_cloud_e1), dot(p, as_cloud_e2)) / (2.0 * as_cloud_extent) + 0.5;
    if (dot(pc, pc) >= as_cloud_radius * as_cloud_radius ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        return 1.0;
    }
    return textureLod(as_cloud_map, uv, lod).r;
}

// Cloud transmittance toward the light at a view-space point.
float cloudTransmittance(vec3 view_pos, float lod)
{
    if (!cloudsEnabled())
    {
        return 1.0;
    }
    return cloudTransmittanceDome((as_cloud_from_view * vec4(view_pos, 1.0)).xyz, lod);
}

// Map texels crossed per metre along a view ray: the map is constant along
// the light, so only the step's component across the light moves on it (a
// ray toward the light barely moves: crepuscular rays stay sharp).
float cloudTexelsPerMetre(vec3 ray_dir)
{
    float along = dot(cloudDomeDir(ray_dir), as_cloud_light);
    return as_cloud_texels_per_metre * sqrt(max(1.0 - along * along, 0.0));
}

// e^(-sigma t)-weighted mean cloud transmittance over [0, t_end]. With
// u = 1 - e^(-sigma t) the weight is uniform in u, so `steps` midpoints in u
// give the weighted mean as a plain average; each sample reads the mip
// matching the map distance between samples (band limited, no jitter).
float meanCloudTransmittance(vec3 ray_dir, float t_end, float sigma, int steps)
{
    if (!cloudsEnabled())
    {
        return 1.0;
    }
    float texels_per_metre = cloudTexelsPerMetre(ray_dir);
    float u_total = 1.0 - exp(-sigma * t_end);
    bool uniform_spacing = sigma * t_end < 1e-3;
    float sum = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        float s = (float(i) + 0.5) / float(steps);
        float t;
        float spacing;
        if (uniform_spacing)
        {
            t = s * t_end;
            spacing = t_end / float(steps);
        }
        else
        {
            float u = s * u_total;
            t = -log(1.0 - u) / sigma;
            spacing = u_total / (float(steps) * sigma * (1.0 - u));
        }
        float lod = log2(max(spacing * texels_per_metre, 1.0));
        sum += cloudTransmittance(ray_dir * t, lod);
    }
    return sum / float(steps);
}

// View distance along ray_dir to the cloud layer (the sphere's far side seen
// from inside; 0 when the camera is above the clouds and looking away).
float cloudLayerDistance(vec3 ray_dir)
{
    vec3 o = (as_cloud_from_view * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    vec3 d = cloudDomeDir(ray_dir);
    vec3 oc = o + vec3(0.0, as_cloud_centre_depth, 0.0);
    float b = dot(oc, d);
    float c = dot(oc, oc) - as_cloud_radius * as_cloud_radius;
    return max(-b + sqrt(max(b * b - c, 0.0)), 0.0);
}

// Cloud rays: the cloud-lit in-scatter length of the thin far medium (its
// own extinction sigma, starting at t0; not dimmed by the near medium),
//   integral over [t0, t1] of e^(-sigma (t - t0)) * ramp(t) * T_cloud(t) dt,
// ramp(t) = clamp((t - t0) / ramp_len, 0, 1) (density taking over where the
// exact medium fades out). Importance-spaced in e^(-sigma (t - t0)) (relative
// to t0: no underflow at any distance), prefiltered like
// meanCloudTransmittance(). lit_share = ramp-weighted mean T_cloud.
float cloudRayLength(vec3 ray_dir, float t0, float t1, float sigma, float ramp_len,
                     int steps, out float lit_share)
{
    lit_share = 1.0;
    if (!cloudsEnabled() || t1 <= t0)
    {
        return 0.0;
    }
    float texels_per_metre = cloudTexelsPerMetre(ray_dir);
    float ramp_inv = 1.0 / max(ramp_len, 1e-3);
    float span = t1 - t0;
    float u_total = 1.0 - exp(-sigma * span);
    bool uniform_spacing = sigma * span < 1e-3;
    float path_length = uniform_spacing ? span : u_total / sigma;
    float lit = 0.0;
    float weight = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        float s = (float(i) + 0.5) / float(steps);
        float t;
        float spacing;
        if (uniform_spacing)
        {
            t = t0 + s * span;
            spacing = span / float(steps);
        }
        else
        {
            float e = 1.0 - s * u_total;
            t = t0 - log(e) / sigma;
            spacing = u_total / (float(steps) * sigma * e);
        }
        float ramp = clamp((t - t0) * ramp_inv, 0.0, 1.0);
        float lod = log2(max(spacing * texels_per_metre, 1.0));
        lit += ramp * cloudTransmittance(ray_dir * t, lod);
        weight += ramp;
    }
    lit_share = weight > 0.0 ? lit / weight : 1.0;
    return path_length * lit / float(steps);
}

// Debug: the baked map seen from the camera along a sky view direction, where
// the view ray meets the cloud layer (reproduces the drawn clouds toward the
// light), 0.5 on the half sky facing away from it or beyond the map.
float cloudSkyDebug(vec3 ray_dir)
{
    if (!cloudsEnabled())
    {
        return 1.0;
    }
    vec3 o = (as_cloud_from_view * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    vec3 q = o + cloudLayerDistance(ray_dir) * cloudDomeDir(ray_dir);
    vec2 uv = vec2(dot(q, as_cloud_e1), dot(q, as_cloud_e2)) / (2.0 * as_cloud_extent) + 0.5;
    bool covered = dot(q, as_cloud_light) > 0.0 &&
                   all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    return covered ? textureLod(as_cloud_map, uv, 0.0).r : 0.5;
}
