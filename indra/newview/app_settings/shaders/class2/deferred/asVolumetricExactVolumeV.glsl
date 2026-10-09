/**
 * @file asVolumetricExactVolumeV.glsl
 * @brief AyaneStorm exact volumetric lighting - sun shadow light-volume mesh.
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

// Builds the closed boundary of one cascade's shadowed volume
//   S = { p : uv(p) inside the texel-center hull, D(uv) < depth(p) < 1 }
// in shadow texture space (u, v, depth), where D is the shadow map depth
// surface triangulated through texel centers. Attribute-less: positions come
// from gl_VertexID / gl_InstanceID only. Parts (as_mesh_part):
//   0     top surface, one triangle strip per grid row (instance = row)
//   1..4  side walls (x = 0, x = last, y = 0, y = last) from D down to 1
//   5     bottom at depth 1, triangle fan over the hull perimeter
// Every triangle is wound counter-clockwise seen from outside S (right-hand
// normal points out of S) in texture space; the fragment shader turns that
// into enter/exit using gl_FrontFacing and the CPU orientation flip. Shared
// vertices are produced by one code path (single shadowToView call site), so
// adjacent parts meet bit-exactly and the surface is watertight.
// See doc/volumetric_lighting_exact_redesign_research.md.

/*[EXTRA_CODE_HERE]*/

invariant gl_Position;

uniform sampler2D as_shadow_depth; // raw depth; compare disabled by sampler object
uniform mat4 as_shadow_to_view;    // inverse(shadow_matrix[cascade])
uniform mat4 as_projection;        // camera projection (same as inv_proj's inverse)
uniform vec2 as_shadow_size;       // shadow map size in texels
uniform ivec2 as_grid_size;        // mesh vertices per axis
uniform int as_grid_stride;        // shadow texels per mesh cell
uniform int as_mesh_part;

out vec3 vary_view_pos;

// Bottom of the shadowed volume: the shadow frustum's far plane. A texel
// cleared to 1.0 (no caster) gives a zero-thickness volume there.
const float VOLUME_FAR_DEPTH = 1.0;

vec3 shadowToView(vec2 texel, float depth)
{
    vec2 uv = (texel + 0.5) / as_shadow_size;
    vec4 p = as_shadow_to_view * vec4(uv, depth, 1.0);
    return p.xyz / p.w;
}

// Perimeter of the grid hull, counter-clockwise in (x, y): k in [0, perimeter].
ivec2 perimeterVertex(int k, int last_x, int last_y)
{
    int perimeter = 2 * (last_x + last_y);
    k = (k >= perimeter) ? 0 : k;
    if (k < last_x)
    {
        return ivec2(k, 0);
    }
    k -= last_x;
    if (k < last_y)
    {
        return ivec2(last_x, k);
    }
    k -= last_y;
    if (k < last_x)
    {
        return ivec2(last_x - k, last_y);
    }
    k -= last_x;
    return ivec2(0, last_y - k);
}

void main()
{
    int n = gl_VertexID;
    int last_x = as_grid_size.x - 1;
    int last_y = as_grid_size.y - 1;

    ivec2 g = ivec2(0);
    bool far_vertex = false;
    bool fan_center = false;

    if (as_mesh_part == 0)
    {
        // Strip v0 = (0, row), v1 = (0, row + 1), v2 = (1, row): normal -depth.
        g = ivec2(n >> 1, gl_InstanceID + (n & 1));
    }
    else if (as_mesh_part == 1)
    {
        g = ivec2(0, n >> 1);
        far_vertex = (n & 1) == 1;
    }
    else if (as_mesh_part == 2)
    {
        g = ivec2(last_x, n >> 1);
        far_vertex = (n & 1) == 0;
    }
    else if (as_mesh_part == 3)
    {
        g = ivec2(n >> 1, 0);
        far_vertex = (n & 1) == 0;
    }
    else if (as_mesh_part == 4)
    {
        g = ivec2(n >> 1, last_y);
        far_vertex = (n & 1) == 1;
    }
    else
    {
        far_vertex = true;
        fan_center = n == 0;
        g = perimeterVertex(max(n - 1, 0), last_x, last_y);
    }

    ivec2 texel = g * as_grid_stride;
    vec2 texel_pos = fan_center ? vec2(float(last_x), float(last_y)) * (0.5 * float(as_grid_stride))
                                : vec2(texel);
    float depth = far_vertex ? VOLUME_FAR_DEPTH : texelFetch(as_shadow_depth, texel, 0).r;

    vec3 view_pos = shadowToView(texel_pos, depth);
    vary_view_pos = view_pos;
    gl_Position = as_projection * vec4(view_pos, 1.0);
}
