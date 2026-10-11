/**
 * @file asVolumetricCloudShadowF.glsl
 * @brief AyaneStorm exact volumetric lighting - cloud shadow map bake.
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

// SL clouds are painted on the sky dome: a sphere whose centre lies
// DOME_OFFSET (0.96) radii below the camera (LLEnvironment::getCamHeight()),
// i.e. a shallow cap above the viewer. Modelled at physical scale as a sphere
// of radius as_cloud_radius whose centre lies as_cloud_centre_depth below the
// cloud anchor. Every point on one light ray leaves the sphere at the same
// point, so the cloud shadow is a 2D map over the plane through the anchor
// perpendicular to the light (extent 2 * as_cloud_extent). Each texel stores
// the cloud transmittance (1 - alpha1) along the light ray through that
// plane point. All vectors are in sky dome space (Y up: x = north, y = up,
// z = east), the space of lightnorm and the dome UVs, relative to the anchor.
// See doc/volumetric_lighting_exact_redesign_research.md, phase 4.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

// Cloud inputs, as set for the cloud pass (LLDrawPoolWLSky::renderSkyCloudsDeferred()).
uniform sampler2D cloud_noise_texture;
uniform sampler2D cloud_noise_texture_next;
uniform float blend_factor;
uniform vec3 cloud_pos_density1;
uniform vec3 cloud_pos_density2;
uniform float cloud_scale;
uniform float cloud_variance;
uniform float cloud_shadow;
uniform vec3 lightnorm;
uniform float max_y;

uniform vec3 as_cloud_e1;          // plane basis, perpendicular to the light
uniform vec3 as_cloud_e2;
uniform vec3 as_cloud_light;       // unit direction toward the light
uniform float as_cloud_radius;       // sphere radius (metres)
uniform float as_cloud_centre_depth; // sphere centre below the anchor (metres)
uniform float as_cloud_extent;       // map half extent (metres)
uniform float as_cam_height;         // LLEnvironment::getCamHeight(), as camPosLocal.y

// LLVOWLSky::buildStripsBuffer() dome radius (LLSettingsSky::DOME_RADIUS);
// cloudsV.glsl measures altitude on the unscaled dome vertices.
const float DOME_RADIUS = 15000.0;

vec4 cloudNoise(vec2 uv)
{
    vec4 a = texture(cloud_noise_texture, uv);
    vec4 b = texture(cloud_noise_texture_next, uv);
    return mix(a, b, blend_factor);
}

// Cloud opacity alpha1 for a unit dome direction: cloudsV.glsl + cloudsF.glsl
// (also asHorizonCloudTintF.glsl). Must stay in sync with them.
float cloudAlpha(vec3 dir)
{
    // LLVOWLSky::buildStripsBuffer() planar UV of the dome direction.
    vec2 texcoord0 = vec2((-dir.z + 1.0) * 0.5, (-dir.x + 1.0) * 0.5);

    // cloudsV.glsl
    vec2 uv1 = vec2(-texcoord0.x, texcoord0.y);
    uv1 -= 0.5;
    uv1 /= cloud_scale;
    uv1 += 0.5;
    vec2 uv2 = uv1 + vec2(lightnorm.x, lightnorm.z) * 0.0125;
    vec2 uv3 = uv1 * 16.;
    vec2 uv4 = uv2 * 16.;

    float rel_y = dir.y * DOME_RADIUS - as_cam_height + 50.0;
    float altitude_blend_factor = rel_y < 0.0 ? 0.0 : clamp((rel_y + 512.0) / max_y, 0.0, 1.0);
    float cloudDensity = 2. * (cloud_shadow - 0.25);

    // cloudsF.glsl
    vec2 disturbance  = vec2(cloudNoise(uv1 / 8.0f).x, cloudNoise((uv3 + uv1) / 16.0f).x) * cloud_variance * (1.0f - cloud_scale * 0.25f);
    vec2 disturbance2 = vec2(cloudNoise((uv1 + uv3) / 4.0f).x, cloudNoise((uv4 + uv2) / 8.0f).x) * cloud_variance * (1.0f - cloud_scale * 0.25f);

    uv1 += cloud_pos_density1.xy + (disturbance * 0.2);
    uv3 += cloud_pos_density2.xy;

    float density_variance = min(1.0, (disturbance.x* 2.0 + disturbance.y* 2.0 + disturbance2.x + disturbance2.y) * 4.0);
    cloudDensity *= 1.0 - (density_variance * density_variance);

    float alpha1 = (cloudNoise(uv1).x - 0.5) + (cloudNoise(uv3).x - 0.5) * cloud_pos_density2.z;
    alpha1 = min(max(alpha1 + cloudDensity, 0.) * 10 * cloud_pos_density1.z, 1.);
    alpha1 = 1. - alpha1 * alpha1;
    alpha1 = 1. - alpha1 * alpha1;
    alpha1 *= altitude_blend_factor;
    return clamp(alpha1, 0.0, 1.0);
}

void main()
{
    // Texel -> point on the plane through the anchor.
    vec2 ab = (vary_fragcoord - 0.5) * (2.0 * as_cloud_extent);
    vec3 p = ab.x * as_cloud_e1 + ab.y * as_cloud_e2;

    // Relative to the sphere centre; outside the sphere (above the clouds)
    // there is no cloud shadow.
    vec3 pc = p + vec3(0.0, as_cloud_centre_depth, 0.0);
    float radius2 = as_cloud_radius * as_cloud_radius;
    float c = dot(pc, pc) - radius2;
    if (cloud_scale < 0.001 || c >= 0.0)
    {
        frag_color = vec4(1.0); // no clouds / outside the sphere: lit
        return;
    }

    // The light ray from p leaves the sphere at q; the dome direction of q
    // is its unit vector from the centre (as the dome vertices).
    float b = dot(pc, as_cloud_light);
    float s = -b + sqrt(b * b - c);
    vec3 dir = (pc + s * as_cloud_light) / as_cloud_radius;
    frag_color = vec4(vec3(1.0 - cloudAlpha(dir)), 1.0);
}
