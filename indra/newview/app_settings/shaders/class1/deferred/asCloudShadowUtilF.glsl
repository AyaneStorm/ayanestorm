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

// shadowUtil.glsl (always attached with this file, hasShadows).
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int sun_up_factor;
uniform vec2 screen_res;
uniform mat4 shadow_matrix[6];
uniform vec4 shadow_clip;

// A sun cascade covers this view position: in front of the camera, before
// the last split, inside the selected cascade's map. Outside, the lookup
// returns "lit" whatever is there (camera close to a wall or an avatar
// indoors: openness points behind the camera or off screen).
bool asSunShadowCovered(vec3 view_pos)
{
    if (view_pos.z >= 0.0 || view_pos.z <= -shadow_clip.w)
    {
        return false;
    }
    int cascade = view_pos.z > -shadow_clip.x ? 0 :
                  view_pos.z > -shadow_clip.y ? 1 :
                  view_pos.z > -shadow_clip.z ? 2 : 3;
    vec4 lpos = shadow_matrix[cascade] * vec4(view_pos, 1.0);
    vec2 uv = lpos.xy / lpos.w;
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}

// Radius (metres) of the disc whose sun visibility stands for how open a
// point is to the cloud-lit sky: wider than an avatar's or a branch's
// shadow, smaller than a room.
const float AS_SKY_OPENNESS_RADIUS = 3.0;

// Mean sun visibility over a disc around view_pos, perpendicular to the
// light (8 points on two rings, the point itself excluded). Under a roof or
// a dense canopy the whole disc is shadowed (0); on open ground a small
// shadow covers little of it (~1). norm = light: no normal offset. Points
// no cascade covers are skipped; none covered: the point's own shadow.
float asSkyOpenness(vec3 view_pos)
{
    vec3 light = normalize(sun_up_factor == 1 ? sun_dir : moon_dir);
    vec3 helper = abs(light.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 axis_a = normalize(cross(light, helper));
    vec3 axis_b = cross(light, axis_a);
    vec2 pos_screen = gl_FragCoord.xy / screen_res;
    float sum = 0.0;
    float count = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float angle = float(i) * 0.78539816;
        float radius = (i & 1) == 0 ? AS_SKY_OPENNESS_RADIUS : 0.5 * AS_SKY_OPENNESS_RADIUS;
        vec3 sample_pos = view_pos + radius * (cos(angle) * axis_a + sin(angle) * axis_b);
        if (asSunShadowCovered(sample_pos))
        {
            sum += sampleDirectionalShadow(sample_pos, light, pos_screen);
            count += 1.0;
        }
    }
    return count > 0.0 ? sum / count : sampleDirectionalShadow(view_pos, light, pos_screen);
}

// Sun shadow under the clouds: a cloud dims the direct sun like a sun
// shadow; the light it still lets through (as_cloud_diffuse) comes from the
// whole cloud and ignores the sun shadow, but not a roof: it is scaled by
// asSkyOpenness() (indoors it relit everything with direct sun). The opaque
// scene also occludes it with ambient occlusion
// (asVolumetricCloudApplyF.glsl); these forward surfaces have none.
float asCloudSunShadow(float shadow, vec3 view_pos)
{
    float transmittance = asCloudTransmittance(view_pos);
    if (transmittance >= 1.0)
    {
        return shadow;
    }
    return shadow * transmittance +
           as_cloud_diffuse * (1.0 - transmittance) * asSkyOpenness(view_pos);
}
