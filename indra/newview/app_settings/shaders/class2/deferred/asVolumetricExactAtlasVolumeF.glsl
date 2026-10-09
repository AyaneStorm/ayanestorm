/**
 * @file asVolumetricExactAtlasVolumeF.glsl
 * @brief AyaneStorm exact volumetric lighting - light-volume crossings for the
 *        16 transparency atlas slices.
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

// Same telescoping accumulation as asVolumetricExactVolumeF.glsl, but for
// 16 ray end distances at once (the atlas slice far distances, radial, from
// the camera) instead of the scene depth. Rendered at atlas tile resolution
// into 4 RGBA32F attachments: slice k lands in attachment k / 4, channel
// k % 4. G is monotonic, so with A/B = slab entry/exit and e = slice end:
//   G(clamp(t, min(A, e), min(B, e))) - G(min(A, e))
//     = e <= A ? 0 : clamp(G(t), G(A), min(G(B), G(e))) - G(A)
// which needs only three exp() per fragment for all 16 slices.

/*[EXTRA_CODE_HERE]*/

layout(location = 0) out vec4 frag_slices0;
layout(location = 1) out vec4 frag_slices1;
layout(location = 2) out vec4 frag_slices2;
layout(location = 3) out vec4 frag_slices3;

in vec3 vary_view_pos;

uniform vec2 as_target_size;       // atlas tile size in texels
uniform float scatter_density;
uniform float as_slab_near;
uniform float as_slab_far;
uniform int as_orientation_flip;
uniform float as_slice_g[16];      // G(slice end distance), from the CPU

vec3 getPositionWithNDC(vec3 ndc);

const float MAX_MARCH_DISTANCE = 128.0;
const float ATLAS_SLICES = 16.0;

float transmittedLength(float t)
{
    float x = scatter_density * t;
    if (x < 1e-3)
    {
        return t * (1.0 - 0.5 * x + x * x * (1.0 / 6.0));
    }
    return (1.0 - exp(-x)) / scatter_density;
}

void main()
{
    // Per-pixel ray from the texel center only, so every fragment of this
    // texel uses bit-identical slab distances.
    vec2 uv = gl_FragCoord.xy / as_target_size;
    vec3 ray_dir = normalize(getPositionWithNDC(vec3(uv * 2.0 - 1.0, 1.0)));
    float forward = max(-ray_dir.z, 1e-6);
    float slab_a = as_slab_near / forward;
    float g_a = transmittedLength(slab_a);
    float g_b = transmittedLength(as_slab_far / forward);
    float g_t = transmittedLength(length(vary_view_pos));

    bool entering = gl_FrontFacing != (as_orientation_flip != 0);
    float sign_factor = entering ? -1.0 : 1.0;

    float slices[16];
    for (int k = 0; k < 16; ++k)
    {
        float fraction = float(k + 1) / ATLAS_SLICES;
        float slice_end = fraction * fraction * MAX_MARCH_DISTANCE;
        slices[k] = slice_end <= slab_a ? 0.0 :
            sign_factor * (clamp(g_t, g_a, min(g_b, as_slice_g[k])) - g_a);
    }

    frag_slices0 = vec4(slices[0], slices[1], slices[2], slices[3]);
    frag_slices1 = vec4(slices[4], slices[5], slices[6], slices[7]);
    frag_slices2 = vec4(slices[8], slices[9], slices[10], slices[11]);
    frag_slices3 = vec4(slices[12], slices[13], slices[14], slices[15]);
}
