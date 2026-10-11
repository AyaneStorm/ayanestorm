/**
 * @file asVolumetricCloudApplyF.glsl
 * @brief AyaneStorm exact volumetric lighting - cloud shadows on the scene.
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

// Blended into the r channel (only) of the sun shadow map pass's target, the
// sun shadow every lit opaque surface reads:
//   r = shadow * T + diffuse * (1 - T) * ao   (blend ONE, SRC_ALPHA)
// A cloud dims the direct sun like a sun shadow. The light it still lets
// through (as_cloud_diffuse) comes from the whole bright cloud, like sky
// light: it ignores the sun shadow (an avatar's or a branch's shadow on open
// ground vanishes) but is occluded like ambient light, so enclosed places
// (canopy interiors, eaves, corners) stay dark instead of relighting.
// ao: GTAO when effective, else the legacy SSAO the sun pass also computes,
// else 1. Beyond the shadow cascades r is 1, so cloud shadows also cover the
// distant ground.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D depthMap;
uniform vec2 screen_res;
uniform float as_cloud_diffuse; // sunlight diffused through a full cloud
// 0: none, 1: legacy SSAO, 2: GTAO visibility (r), 3: GTAO with bent
// normals (a), as softenLightF.glsl reads it.
uniform int as_cloud_ao_source;
uniform sampler2D gtao_visibility_map;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNorm(vec2 pos_screen);
float calcAmbientOcclusion(vec4 pos, vec3 norm, vec2 pos_screen);

// asVolumetricCloudUtilF.glsl
float cloudTransmittance(vec3 view_pos, float lod);

// Map mip for the shadow edge: ~2 texels (~50 m) of softening, cloud
// shadows are much less defined than geometry shadows.
const float CLOUD_SHADOW_LOD = 1.0;

void main()
{
    float depth = texelFetch(depthMap, ivec2(gl_FragCoord.xy), 0).r;
    if (depth >= 1.0)
    {
        frag_color = vec4(0.0, 0.0, 0.0, 1.0); // sky: unchanged
        return;
    }
    vec2 pos_screen = gl_FragCoord.xy / screen_res;
    vec4 pos = getPositionWithDepth(pos_screen, depth);
    float transmittance = cloudTransmittance(pos.xyz, CLOUD_SHADOW_LOD);
    if (transmittance >= 1.0)
    {
        frag_color = vec4(0.0, 0.0, 0.0, 1.0); // no cloud: unchanged
        return;
    }

    float ao = 1.0;
    if (as_cloud_ao_source == 1)
    {
        ao = calcAmbientOcclusion(pos, getNorm(pos_screen).xyz, pos_screen);
    }
    else if (as_cloud_ao_source >= 2)
    {
        vec4 gtao = texture(gtao_visibility_map, pos_screen);
        ao = as_cloud_ao_source == 3 ? gtao.a : gtao.r;
    }
    frag_color = vec4(as_cloud_diffuse * (1.0 - transmittance) * clamp(ao, 0.0, 1.0),
                      0.0, 0.0, transmittance);
}
