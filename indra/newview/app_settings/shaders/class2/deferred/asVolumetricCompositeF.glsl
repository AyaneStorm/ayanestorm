/**
 * @file asVolumetricCompositeF.glsl
 * @brief AyaneStorm optional volumetric lighting - upsample and transmittance composite.
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
// Debug-only: show emissiveRect's alpha channel (e.g. the transparency
// atlas's raw transmittance) as grayscale RGB instead of its normal RGB
// content. Never set outside a diagnostic debug mode.
uniform int showAlphaChannel;
// Opaque scene attenuation: attenuateScene is only set for the real (debug
// mode 0) composite. RGB contains scatter and alpha contains transmittance;
// destination blending evaluates scene * transmittance + scatter without
// sampling or copying the live destination. sceneDensity is the same
// Beer-Lambert coefficient the raymarch/atlas passes already use
// (RenderVolumetricLightingDensity) - T is derived directly from this
// pass's own full-resolution depth sample rather than from the transparency
// atlas, since that atlas is keyed to per-object view_position lookups from
// alpha/material shaders, not to this full-screen opaque pass.
uniform float sceneDensity;
uniform int attenuateScene;
// Debug-only: postDeferredTonemap.glsl (upstream, runs after this composite)
// unconditionally multiplies whatever lands in "screen" by exposure *
// exposureMap's sampled scale before tonemapping - it has no concept of
// debug modes. A plain [0,1] scalar like transmittance gets pushed toward
// solid white by that multiply on any reasonably-exposed scene, well before
// tonemap curves even come into play. Pre-dividing by the same factor here
// cancels it out so the displayed value approximates the raw stored scalar.
uniform sampler2D exposureMap;
uniform float debugExposure;

vec4 getPosition(vec2 pos_screen);
vec4 getNorm(vec2 pos_screen);

float depthSimilarity(vec2 uv, float center_depth)
{
    float tap_depth = abs(getPosition(uv).z);
    float relative_difference = abs(tap_depth - center_depth) /
                                max(max(tap_depth, center_depth), 1.0);
    return exp(-relative_difference * 64.0);
}

float normalSimilarity(vec2 uv, vec3 center_normal)
{
    vec3 tap_normal = getNorm(uv).xyz;
    // Invalid/background normals should not suppress sky taps; opaque depth
    // remains the complete guide in that case. For real surfaces, smoothly
    // reject differently oriented geometry even when its depth is similar.
    float center_length_squared = dot(center_normal, center_normal);
    float tap_length_squared = dot(tap_normal, tap_normal);
    // Express validity positively and negate the whole predicate: comparisons
    // against NaN are false, so this also catches the NaN produced when the
    // packed-normal decoder sees an empty background texel.
    if (!(center_length_squared > 0.25 && center_length_squared < 1.5) ||
        !(tap_length_squared > 0.25 && tap_length_squared < 1.5))
    {
        return 1.0;
    }
    return smoothstep(0.5, 0.9,
                      dot(normalize(center_normal), normalize(tap_normal)));
}

// Same sky/horizon boundary the raymarch shader uses (MAX_MARCH_DISTANCE in
// asVolumetricLightF.glsl). Beyond this distance is treated as sky/atmosphere,
// not fogged ground - the sky is not a physical surface sitting some finite
// distance away that this feature's ground fog should attenuate a second
// time; the sky's own extinction/scattering is already handled entirely by
// its own WLSky rendering upstream of this composite. An earlier version of
// this function clamped dist to this boundary and still attenuated at the
// clamped value - that applied a uniform, non-negligible dimming to the
// ENTIRE sky at once for any nonzero extinction (confirmed: extinction as
// low as 0.001 visibly flattened the whole sky, while attenuateScene was
// otherwise verified correct since it exactly matched the disabled case at
// extinction=0). Skip attenuation entirely at/beyond the boundary instead.
const float SKY_DISTANCE = 128.0;
// Start of the fade-to-1.0 band, ending at SKY_DISTANCE. A hard cutoff at
// SKY_DISTANCE alone produces a visible seam: geometry a hair closer than the
// boundary is attenuated, geometry a hair beyond it is not. Fading T back to
// 1.0 across this band removes that pop while still landing on exactly 1.0
// at SKY_DISTANCE, matching the sky-side assumption below unchanged.
const float SKY_FADE_START = 100.0;

// Beer-Lambert transmittance at a given view-space distance. Matches the
// raymarch/atlas passes' exp(-extinction * distance) for real, finite-depth
// scene geometry; smoothly fades to 1.0 (no attenuation) over
// [SKY_FADE_START, SKY_DISTANCE), and is exactly 1.0 at or beyond
// SKY_DISTANCE, since that range is sky/atmosphere, not ground fog.
float sceneTransmittance(float dist)
{
    if (dist >= SKY_DISTANCE)
    {
        return 1.0;
    }
    float beer_lambert = exp(-sceneDensity * dist);
    float fade = smoothstep(SKY_FADE_START, SKY_DISTANCE, dist);
    return mix(beer_lambert, 1.0, fade);
}

float compositeTransmittance(float dist)
{
    if (attenuateScene == 0)
    {
        return 0.0;
    }
    return sceneTransmittance(dist);
}

void main()
{
    if (depthAwareUpsample == 0)
    {
        vec4 sampled = texture(emissiveRect, vary_fragcoord);
        if (showAlphaChannel != 0)
        {
            float exp_scale = texture(exposureMap, vec2(0.5)).r;
            float inv_exposure = 1.0 / max(debugExposure * exp_scale, 1e-4);
            frag_color = vec4(vec3(sampled.a) * inv_exposure, 0.0);
            return;
        }
        float dist = abs(getPosition(vary_fragcoord).z);
        frag_color = vec4(sampled.rgb, compositeTransmittance(dist));
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
    vec3 center_normal = getNorm(vary_fragcoord).xyz;
    vec4 weights = spatial *
        vec4(depthSimilarity(uv00, center_depth) * normalSimilarity(uv00, center_normal),
             depthSimilarity(uv10, center_depth) * normalSimilarity(uv10, center_normal),
             depthSimilarity(uv01, center_depth) * normalSimilarity(uv01, center_normal),
             depthSimilarity(uv11, center_depth) * normalSimilarity(uv11, center_normal));
    float weight_sum = dot(weights, vec4(1.0));
    if (weight_sum < 1e-6)
    {
        // A subpixel surface may have no representative half-resolution tap.
        // Preserve the former bilinear behavior instead of creating a hole.
        vec3 scatter = texture(emissiveRect, vary_fragcoord).rgb;
        frag_color = vec4(scatter, compositeTransmittance(center_depth));
        return;
    }

    vec3 scatter = texture(emissiveRect, uv00).rgb * weights.x +
                   texture(emissiveRect, uv10).rgb * weights.y +
                   texture(emissiveRect, uv01).rgb * weights.z +
                   texture(emissiveRect, uv11).rgb * weights.w;
    scatter /= weight_sum;
    frag_color = vec4(scatter, compositeTransmittance(center_depth));
}
