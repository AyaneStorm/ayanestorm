/**
 * @file starsV.glsl
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

uniform mat4 texture_matrix0;
uniform mat4 modelview_projection_matrix;
uniform float time;

in vec3 position;
in vec4 diffuse_color;
in vec2 texcoord0;

out vec4 vertex_color;
out vec2 vary_texcoord0;
out vec2 screenpos;

// <AS:Chanayane> Real-sky stars: local zenith in the star frame and fade
// switch (0 = stock, no fade); stars below the horizon fade out.
uniform vec3 as_star_up;
uniform float as_star_horizon_fade;
out float vary_as_horizon;
// </AS:Chanayane>

void main()
{
    //transform vertex
    vec4 pos = modelview_projection_matrix * vec4(position, 1.0);


    // smash to far clip plane to
    // avoid rendering on top of moon (do NOT write to gl_FragDepth, it's slow)
    pos.z = pos.w;

    gl_Position = pos;

    float t = mod(time, 1.25f);
    screenpos = position.xy * vec2(t, t);
    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;
    vertex_color = diffuse_color;
    // <AS:Chanayane> Real-sky horizon fade (sine of the altitude)
    float altitude = dot(normalize(position), as_star_up);
    vary_as_horizon = as_star_horizon_fade > 0.0 ? smoothstep(-0.01, 0.06, altitude) : 1.0;
    // </AS:Chanayane>
}
