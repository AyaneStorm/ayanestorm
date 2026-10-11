/**
 * @file asCloudShadowUtilF.glsl
 * @brief AyaneStorm cloud shadows for surfaces that sample the sun shadow.
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

// Attached with deferred/shadowUtil.glsl to every program with shadows
// (LLShaderMgr::attachShaderFeatures(), hasShadows); unused (and its
// uniforms optimized out) unless a shader calls asCloudSunShadow(). Used by
// the surfaces that sample the sun shadow themselves (alpha, PBR alpha,
// alpha materials, water); the opaque scene gets the same result from
// asVolumetricCloudApplyF.glsl. Uniforms set by
// ASVolumetricLighting::bindTransparencyAtlas() (Exact volumetric mode).
// Same map and lookup as asVolumetricCloudUtilF.glsl cloudTransmittance().

uniform int as_cloud_enabled;
uniform sampler2D as_cloud_map;       // R16F cloud transmittance, mipmapped
uniform mat4 as_cloud_from_view;      // view position -> anchor-relative dome position
uniform vec3 as_cloud_e1;             // map plane basis (dome space)
uniform vec3 as_cloud_e2;
uniform float as_cloud_radius;        // cloud sphere radius
uniform float as_cloud_centre_depth;  // sphere centre below the anchor
uniform float as_cloud_extent;        // map half extent
uniform float as_cloud_diffuse;       // sunlight diffused through the clouds (no shadows)

// Map mip for the shadow edge: ~2 texels (~50 m) of softening, as the
// opaque scene (asVolumetricCloudApplyF.glsl CLOUD_SHADOW_LOD).
const float AS_CLOUD_SHADOW_LOD = 1.0;

// Cloud transmittance toward the sun at a view-space point (1 = no cloud).
float asCloudTransmittance(vec3 view_pos)
{
    if (as_cloud_enabled == 0)
    {
        return 1.0;
    }
    vec3 p = (as_cloud_from_view * vec4(view_pos, 1.0)).xyz;
    vec3 pc = p + vec3(0.0, as_cloud_centre_depth, 0.0);
    vec2 uv = vec2(dot(p, as_cloud_e1), dot(p, as_cloud_e2)) / (2.0 * as_cloud_extent) + 0.5;
    if (dot(pc, pc) >= as_cloud_radius * as_cloud_radius ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        return 1.0;
    }
    return textureLod(as_cloud_map, uv, AS_CLOUD_SHADOW_LOD).r;
}

// Sun shadow under the clouds: a cloud dims the direct sun like a sun
// shadow; the light it still lets through (as_cloud_diffuse) comes from the
// whole cloud and ignores the sun shadow. The opaque scene also occludes it
// with ambient occlusion (asVolumetricCloudApplyF.glsl); these forward
// surfaces have none, so ao = 1.
float asCloudSunShadow(float shadow, vec3 view_pos)
{
    return mix(as_cloud_diffuse, shadow, asCloudTransmittance(view_pos));
}
