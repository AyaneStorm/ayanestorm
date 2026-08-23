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
