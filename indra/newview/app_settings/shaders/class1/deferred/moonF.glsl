/**
 * @file class1\deferred\moonF.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, 2020 Linden Research, Inc.
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

uniform vec4 color;
uniform vec3 moon_dir;
uniform float moon_brightness;
// <AS:Chanayane> User-controlled lower bound for the legacy horizon fade.
uniform float moon_horizon_min_opacity;
uniform vec3 moon_horizon_tint;
uniform float moon_horizon_tint_strength;
uniform int moon_render_partial;
uniform float moon_phase;
uniform float moon_phase_curvature;
uniform float moon_phase_softness;
uniform float moon_phase_tilt;
// </AS:Chanayane>
uniform sampler2D diffuseMap;

in vec2 vary_texcoord0;

void main()
{
    // <AS:Chanayane> Preserve the legacy fade shape but prevent the moon from
    // disappearing into horizon haze. A setting of zero reproduces upstream.
    // // Restore Pre-EEP alpha fade moon near horizon
    // float fade = 1.0;
    // if( moon_dir.z > 0 )
    //     fade = clamp( moon_dir.z*moon_dir.z*4.0, 0.0, 1.0 );
    float fade = 1.0;
    if (moon_render_partial != 0)
    {
        float legacy_fade = clamp(max(moon_dir.z, 0.0) * max(moon_dir.z, 0.0) * 4.0, 0.0, 1.0);
        fade = mix(clamp(moon_horizon_min_opacity, 0.0, 1.0), 1.0, legacy_fade);
    }
    else if (moon_dir.z > 0.0)
    {
        float legacy_fade = clamp(moon_dir.z * moon_dir.z * 4.0, 0.0, 1.0);
        fade = mix(clamp(moon_horizon_min_opacity, 0.0, 1.0), 1.0, legacy_fade);
    }
    // </AS:Chanayane>

    vec4 c      = texture(diffuseMap, vary_texcoord0.xy);

    // SL-14113 Don't write to depth; prevent moon's quad from hiding stars which should be visible
    // Moon texture has transparent pixels <0x55,0x55,0x55,0x00>
    if (c.a <= 2./255.) // 0.00784
    {
        discard;
    }

    // <AS:Chanayane> Reconstruct the front hemisphere from the disc UV and
    // intersect it with a rotating light direction. This produces a spherical
    // terminator for crescent, quarter, gibbous, and full phases while keeping
    // the environment's moon texture detail intact.
    vec2 phase_position = vary_texcoord0.xy * 2.0 - 1.0;
    float phase_surface_z = sqrt(max(1.0 - dot(phase_position, phase_position), 0.0));
    // A nonlinear depth warp genuinely changes the projected terminator
    // shape. Linear scaling here only renormalized the effective light angle.
    phase_surface_z = pow(phase_surface_z,
                          clamp(moon_phase_curvature, 0.25, 5.0));
    // Map the artistic control linearly by visible illuminated area rather
    // than orbital angle. The former cosine mapping compressed almost all
    // visible gibbous change into a few pixels around 0.45/0.55 and made
    // crescents unnecessarily thin on typical on-screen moon sizes.
    float phase_cycle = clamp(moon_phase, 0.0, 1.0);
    float illuminated_fraction = 1.0 - abs(2.0 * phase_cycle - 1.0);
    float phase_light_z = 2.0 * illuminated_fraction - 1.0;
    float phase_light_x = sqrt(max(1.0 - phase_light_z * phase_light_z, 0.0));
    phase_light_x *= phase_cycle <= 0.5 ? 1.0 : -1.0;
    float phase_tilt_radians = clamp(moon_phase_tilt, -180.0, 180.0) * 0.01745329252;
    vec2 phase_light_xy = phase_light_x * vec2(cos(phase_tilt_radians),
                                               sin(phase_tilt_radians));
    vec3 phase_light_dir = vec3(phase_light_xy, phase_light_z);
    float phase_light = dot(vec3(phase_position, phase_surface_z), phase_light_dir);
    float phase_edge_width = max(fwidth(phase_light), 0.002)
                           + clamp(moon_phase_softness, 0.0, 0.15);
    c.a *= smoothstep(-phase_edge_width, phase_edge_width, phase_light);
    // </AS:Chanayane>

    c.rgb *= moon_brightness;
    // <AS:Chanayane> Warm only the visible moon disc near the horizon. The
    // effect smoothly ends by moon_dir.z 0.35 (approximately 20 degrees).
    float horizon_tint_amount = (1.0 - smoothstep(0.0, 0.35, max(moon_dir.z, 0.0)))
                              * clamp(moon_horizon_tint_strength, 0.0, 1.0);
    c.rgb *= mix(vec3(1.0), clamp(moon_horizon_tint, 0.0, 1.0), horizon_tint_amount);
    // </AS:Chanayane>
    c.a   *= fade;

    frag_data[0] = vec4(0);
    frag_data[1] = vec4(0.0);
    frag_data[2] = vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = vec4(c.rgb, c.a);
#else
    frag_data[0] = vec4(c.rgb, c.a);
#endif

    // Added and commented out for a ground truth.  Do not uncomment - Geenz
    //gl_FragDepth = 0.999985f;
}
