/**
 * @file asVolumetricExactVolumeF.glsl
 * @brief AyaneStorm exact volumetric lighting - signed light-volume crossings.
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

// Accumulates (additive blend into R32F) the exact transmittance-weighted
// shadowed length of the view ray inside this cascade's view-depth slab:
//
//   shadowed = integral over [a, b] of s(t) * exp(-density * t) dt
//
// where s(t) = 1 inside the closed shadowed volume S of the cascade, a/b are
// the slab entry/exit distances clamped to the scene depth. With
// G(t) = (1 - exp(-density * t)) / density, every boundary crossing of S
// contributes sign * (G(clamp(t, a, b)) - G(a)), sign = -1 entering S and +1
// leaving it. Summed over all crossings this telescopes to exactly the
// integral above: crossings before a give 0, crossings after b cancel in
// pairs, and subtracting G(a) removes any dependence on whether the camera
// starts inside S. No depth test and no lit/shadow lookup is involved, so
// the result depends only on the mesh topology. Faces are never culled or
// clipped (depth clamp on).
//
// Range: the medium density fades linearly to zero between as_fade_start and
// as_fade_end (radial metres, as_fade_end = last cascade far plane), so G is
// evaluated at the faded effective length D(t). G(D(t)) is non-decreasing,
// so the telescoping above stays exact.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec3 vary_view_pos;

uniform sampler2D depthMap;
uniform vec2 screen_res;
uniform float scatter_density;
uniform float as_fade_start;
uniform float as_fade_end;
uniform float as_slab_near;        // view-space distance along -z
uniform float as_slab_far;
uniform int as_orientation_flip;   // 1 when texture->screen mapping mirrors winding

vec4 getPositionWithDepth(vec2 pos_screen, float depth);

// Transmittance-weighted length from the camera: integral of exp(-density*s)
// over [0, t]. Series form near zero avoids cancellation in 1 - exp(-x).
float transmittedLength(float t)
{
    float x = scatter_density * t;
    if (x < 1e-3)
    {
        return t * (1.0 - 0.5 * x + x * x * (1.0 / 6.0));
    }
    return (1.0 - exp(-x)) / scatter_density;
}

// Effective medium length of [0, t]: density weight 1 up to as_fade_start,
// linear to 0 at as_fade_end, 0 beyond.
float fadedLength(float t)
{
    float ramp = max(as_fade_end - as_fade_start, 1e-3);
    float x = clamp(t - as_fade_start, 0.0, ramp);
    return min(t, as_fade_start) + x - 0.5 * x * x / ramp;
}

void main()
{
    // Per-pixel ray quantities come from the scene depth sample only, so
    // every fragment of this pixel uses bit-identical a, b and G(a).
    float depth = texelFetch(depthMap, ivec2(gl_FragCoord.xy), 0).r;
    vec3 scene_pos = getPositionWithDepth(gl_FragCoord.xy / screen_res, depth).xyz;
    float scene_dist = length(scene_pos);
    float ray_end = min(scene_dist, as_fade_end);
    float forward = max(-scene_pos.z / max(scene_dist, 1e-6), 1e-6);

    float a = min(as_slab_near / forward, ray_end);
    float b = min(as_slab_far / forward, ray_end);
    float t = clamp(length(vary_view_pos), a, b);

    float contribution = transmittedLength(fadedLength(t)) -
                         transmittedLength(fadedLength(a));
    bool entering = gl_FrontFacing != (as_orientation_flip != 0);
    frag_color = vec4(entering ? -contribution : contribution, 0.0, 0.0, 0.0);
}
