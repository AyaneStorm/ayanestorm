/**
 * @file starsF.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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

in vec4 vertex_color;
in vec2 vary_texcoord0;
in vec2 screenpos;
// <AS:Chanayane> Real-sky horizon fade from starsV.glsl (1 = none)
in float vary_as_horizon;
// </AS:Chanayane>

uniform sampler2D diffuseMap;
uniform float blend_factor;
uniform float custom_alpha;
uniform float time;
// <AS:Chanayane> Aperture DoF: > 0 replaces twinkle by this constant (its
// mean) while lens samples accumulate; 0 keeps the vanilla twinkle.
uniform float as_twinkle_mean;
// > 0: drawing the aperture DoF star mask (one attachment): colour in
// frag_data[0] even with the emissive buffer.
uniform float as_star_mask;
// Viewer-local twinkle depth: 0 steady, 1 stock.
uniform float as_twinkle_amount;
// Viewer-local brightness multiplier (1 = stock), after the smoothstep.
uniform float as_star_brightness;
// Encoded stars: > 0 decodes per-star brightness from vertex alpha, stored
// as log2 over as_star_log_range octaves, as_star_log_offset of them below
// the stock level (so alpha above offset/range is brighter than stock). 0 =
// stock, which ignores vertex alpha.
uniform float as_star_log_range;
uniform float as_star_log_offset;
// </AS:Chanayane>

float twinkle(){
    float d = fract(screenpos.x + screenpos.y);
    return abs(d);
}

// See:
// ALM off: class1/environment/starsF.glsl
// ALM on : class1/deferred/starsF.glsl
void main()
{
    // camera above water: class1\deferred\starsF.glsl
    // camera below water: class1\environment\starsF.glsl
    vec4 col_a = texture(diffuseMap, vary_texcoord0.xy);
    vec4 col_b = texture(diffuseMap, vary_texcoord0.xy);
    vec4 col = mix(col_b, col_a, blend_factor);
    col.rgb *= vertex_color.rgb;

    float factor = smoothstep(0.0f, 0.9f, custom_alpha);

    // <AS:Chanayane> Viewer-local brightness multiplier
    //col.a = (col.a * factor) * 32.0f;
    col.a = (col.a * factor) * 32.0f * as_star_brightness * vary_as_horizon;
    // and real-sky per-star brightness
    if (as_star_log_range > 0.0)
    {
        col.a *= exp2(vertex_color.a * as_star_log_range - as_star_log_offset);
    }
    // </AS:Chanayane>
    // <AS:Chanayane> Aperture DoF: constant mean twinkle while accumulating
    // and viewer-local twinkle amount
    //col.a *= twinkle();
    col.a *= mix(1.0, as_twinkle_mean > 0.0 ? as_twinkle_mean : twinkle(), as_twinkle_amount);
    // </AS:Chanayane>

    frag_data[1] = vec4(0.0f);
    frag_data[2] = vec4(0.0, 1.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = col;
#else
    frag_data[0] = col;
#endif
    // <AS:Chanayane> Aperture DoF star mask
    if (as_star_mask > 0.0)
    {
        frag_data[0] = col;
    }
    // </AS:Chanayane>
}

