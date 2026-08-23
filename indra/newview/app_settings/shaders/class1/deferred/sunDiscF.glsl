/**
 * @file sunDiscF.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_data[4];

vec3 srgb_to_linear(vec3 c);

uniform sampler2D diffuseMap;
uniform sampler2D altDiffuseMap;
uniform float blend_factor; // interp factor between sunDisc A/B
// <AS:Chanayane> Optional viewer-local fallback beneath transparent EEP suns.
uniform int sun_texture_available;
uniform int procedural_sun_enabled;
uniform float procedural_sun_opacity;
uniform float procedural_sun_feather;
uniform float procedural_sun_shimmer;
uniform float procedural_sun_horizon_factor;
uniform float procedural_sun_time;
uniform vec3 procedural_sun_color;
uniform vec3 procedural_sun_limb_color;
// </AS:Chanayane>
in vec2 vary_texcoord0;
in float sun_fade;

void main()
{
    // <AS:Chanayane> Preserve the original texture result exactly while the
    // fallback is disabled, and avoid sampling unbound textures when absent.
    vec4 c = vec4(0.0);
    if (sun_texture_available != 0)
    {
        vec4 sunDiscA = texture(diffuseMap, vary_texcoord0.xy);
        vec4 sunDiscB = texture(altDiffuseMap, vary_texcoord0.xy);
        c = mix(sunDiscA, sunDiscB, blend_factor);
    }

    if (procedural_sun_enabled != 0 && procedural_sun_opacity > 0.0)
    {
        vec2 disc_pos = vary_texcoord0 * 2.0 - 1.0;
        // Atmospheric refraction makes the low solar limb drift and ripple.
        // Keep amplitudes small and use two incommensurate waves to avoid an
        // obviously mechanical oscillation.
        float shimmer = procedural_sun_shimmer * procedural_sun_horizon_factor;
        disc_pos.x += shimmer * (0.55 * sin(procedural_sun_time * 0.73 + disc_pos.y * 10.0)
                               + 0.25 * sin(procedural_sun_time * 1.37 - disc_pos.y * 17.0));
        disc_pos.y += shimmer * 0.18 * sin(procedural_sun_time * 0.41);
        float radius = length(disc_pos);
        float feather = max(fwidth(radius) * 1.5, procedural_sun_feather);
        // The billboard ends at radius 1 along its cardinal axes. Keep the
        // complete soft transition and shimmer displacement inside that quad
        // or rasterization clips them back into a hard edge.
        float outer_radius = 1.0 - shimmer * 0.8;
        float inner_radius = max(0.0, outer_radius - feather);
        float disc_coverage = 1.0 - smoothstep(inner_radius, outer_radius, radius);
        float procedural_alpha = procedural_sun_opacity;
        float limb_mix = smoothstep(max(0.0, 0.72 - feather), 1.0, radius);
        vec3 procedural_color = mix(procedural_sun_color,
                                    procedural_sun_limb_color,
                                    limb_mix);
        // An opaque EEP sun texture otherwise hides every procedural edge
        // control. Preserve its color/detail but share the analytic silhouette
        // so feathering and horizon refraction remain effective.
        float combined_alpha = c.a + procedural_alpha * (1.0 - c.a);
        if (combined_alpha > 0.0)
        {
            vec3 combined_premultiplied = c.rgb * c.a
                                        + procedural_color * procedural_alpha * (1.0 - c.a);
            c = vec4(combined_premultiplied / combined_alpha,
                     combined_alpha * disc_coverage);
        }
    }
    // </AS:Chanayane>

    // SL-9806 stars poke through
    //c.a *= sun_fade;

    frag_data[0] = vec4(0);
    frag_data[1] = vec4(0.0f);
    frag_data[2] = vec4(0.0, 1.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = c;
#else
    frag_data[0] = c;
#endif
}
