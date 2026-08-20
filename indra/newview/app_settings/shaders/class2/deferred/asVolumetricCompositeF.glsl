/**
 * @file asVolumetricCompositeF.glsl
 * @brief AyaneStorm optional volumetric lighting - upsample and additive composite.
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D emissiveRect; // half-res volumetric scatter target
uniform vec2 emissiveRectDelta;
uniform int depthAwareUpsample;

vec4 getPosition(vec2 pos_screen);

float depthSimilarity(vec2 uv, float center_depth)
{
    float tap_depth = abs(getPosition(uv).z);
    float relative_difference = abs(tap_depth - center_depth) /
                                max(max(tap_depth, center_depth), 1.0);
    return exp(-relative_difference * 64.0);
}

void main()
{
    if (depthAwareUpsample == 0)
    {
        frag_color = vec4(texture(emissiveRect, vary_fragcoord).rgb, 0.0);
        return;
    }

    // Reconstruct the four hardware-bilinear taps explicitly and reject taps
    // across opaque depth discontinuities. Transparency is rendered after this
    // composite and receives its own depth-resolved atlas contribution, so the
    // deferred depth buffer is now the correct guide for this opaque stage.
    vec2 source_position = vary_fragcoord / emissiveRectDelta - 0.5;
    vec2 source_base = floor(source_position);
    vec2 fraction = fract(source_position);
    vec2 min_uv = emissiveRectDelta * 0.5;
    vec2 max_uv = vec2(1.0) - min_uv;

    vec2 uv00 = clamp((source_base + vec2(0.5, 0.5)) * emissiveRectDelta, min_uv, max_uv);
    vec2 uv10 = clamp((source_base + vec2(1.5, 0.5)) * emissiveRectDelta, min_uv, max_uv);
    vec2 uv01 = clamp((source_base + vec2(0.5, 1.5)) * emissiveRectDelta, min_uv, max_uv);
    vec2 uv11 = clamp((source_base + vec2(1.5, 1.5)) * emissiveRectDelta, min_uv, max_uv);

    vec4 spatial = vec4((1.0 - fraction.x) * (1.0 - fraction.y),
                        fraction.x * (1.0 - fraction.y),
                        (1.0 - fraction.x) * fraction.y,
                        fraction.x * fraction.y);
    float center_depth = abs(getPosition(vary_fragcoord).z);
    vec4 weights = spatial * vec4(depthSimilarity(uv00, center_depth),
                                  depthSimilarity(uv10, center_depth),
                                  depthSimilarity(uv01, center_depth),
                                  depthSimilarity(uv11, center_depth));
    float weight_sum = dot(weights, vec4(1.0));
    if (weight_sum < 1e-6)
    {
        // A subpixel surface may have no representative half-resolution tap.
        // Preserve the former bilinear behavior instead of creating a hole.
        frag_color = vec4(texture(emissiveRect, vary_fragcoord).rgb, 0.0);
        return;
    }

    vec3 scatter = texture(emissiveRect, uv00).rgb * weights.x +
                   texture(emissiveRect, uv10).rgb * weights.y +
                   texture(emissiveRect, uv01).rgb * weights.z +
                   texture(emissiveRect, uv11).rgb * weights.w;
    frag_color = vec4(scatter / weight_sum, 0.0);
}
