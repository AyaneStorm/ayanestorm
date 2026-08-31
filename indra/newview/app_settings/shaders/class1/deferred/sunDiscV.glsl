/**
 * @file sunDiscV.glsl
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
uniform mat4 modelview_matrix;
uniform mat4 modelview_projection_matrix;
// <AS:Chanayane> Procedural sun alignment and halo geometry controls.
uniform int procedural_sun_alignment_enabled;
uniform int procedural_sun_halo_pass;
uniform vec3 procedural_sun_center;
uniform float procedural_sun_halo_radius;
// </AS:Chanayane>

in vec3 position;
in vec2 texcoord0;

out vec2 vary_texcoord0;
out float sun_fade;

void calcAtmospherics(vec3 eye_pos);

void main()
{
    //transform vertex
    // <AS:Chanayane> Expand the existing celestial billboard for the
    // viewer-local halo pass and omit the legacy displacement when the
    // procedural sun uses the true EEP direction.
    vec3 vertex_position = position.xyz;
    if (procedural_sun_halo_pass != 0)
    {
        vertex_position = procedural_sun_center
                        + (vertex_position - procedural_sun_center) * procedural_sun_halo_radius;
    }
    vec3 offset = procedural_sun_alignment_enabled != 0
                ? vec3(0.0)
                : vec3(0, 0, 50);
    vec4 vert = vec4(vertex_position - offset, 1.0);
    // </AS:Chanayane>
    vec4 pos  = modelview_projection_matrix*vert;

    sun_fade = smoothstep(0.3, 1.0, (position.z + 50) / 512.0f);

    // smash to *almost* far clip plane -- behind clouds but in front of stars
    pos.z = pos.w*0.999999;
    gl_Position = pos;

    calcAtmospherics(pos.xyz);

    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;
}
