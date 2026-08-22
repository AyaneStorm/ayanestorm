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
// Debug-only: show emissiveRect's alpha channel (e.g. the transparency
// atlas's raw transmittance) as grayscale RGB instead of its normal RGB
// content. Never set outside a diagnostic debug mode.
uniform int showAlphaChannel;
// Opaque scene attenuation: attenuateScene is only set for the real (debug
// mode 0) composite. sceneCopy is a copy of "screen" taken just before this
// draw (see ASVolumetricLighting::renderPass()'s use of sSceneCopyTarget) -
// a texture cannot be sampled while simultaneously bound as this draw's
// destination, so the existing scene color has to be read from a separate
// copy rather than the live destination. sceneExtinction is the same
// Beer-Lambert coefficient the raymarch/atlas passes already use
// (RenderVolumetricLightingExtinction) - T is derived directly from this
// pass's own full-resolution depth sample rather than from the transparency
// atlas, since that atlas is keyed to per-object view_position lookups from
// alpha/material shaders, not to this full-screen opaque pass.
uniform sampler2D sceneCopy;
uniform float sceneExtinction;
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

float depthSimilarity(vec2 uv, float center_depth)
{
    float tap_depth = abs(getPosition(uv).z);
    float relative_difference = abs(tap_depth - center_depth) /
                                max(max(tap_depth, center_depth), 1.0);
    return exp(-relative_difference * 64.0);
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

// Beer-Lambert transmittance at a given view-space distance. Matches the
// raymarch/atlas passes' exp(-extinction * distance) exactly for real,
// finite-depth scene geometry; returns 1.0 (no attenuation) for anything at
// or beyond SKY_DISTANCE, since that range is sky/atmosphere, not ground fog.
float sceneTransmittance(float dist)
{
    if (dist >= SKY_DISTANCE)
    {
        return 1.0;
    }
    return exp(-sceneExtinction * dist);
}

vec3 attenuateAndComposite(vec2 pos_screen, float dist, vec3 scatter)
{
    if (attenuateScene == 0)
    {
        return scatter;
    }
    vec3 scene_color = texture(sceneCopy, pos_screen).rgb;
    return scene_color * sceneTransmittance(dist) + scatter;
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
        frag_color = vec4(attenuateAndComposite(vary_fragcoord, dist, sampled.rgb), 0.0);
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
        vec3 scatter = texture(emissiveRect, vary_fragcoord).rgb;
        frag_color = vec4(attenuateAndComposite(vary_fragcoord, center_depth, scatter), 0.0);
        return;
    }

    vec3 scatter = texture(emissiveRect, uv00).rgb * weights.x +
                   texture(emissiveRect, uv10).rgb * weights.y +
                   texture(emissiveRect, uv01).rgb * weights.z +
                   texture(emissiveRect, uv11).rgb * weights.w;
    scatter /= weight_sum;
    frag_color = vec4(attenuateAndComposite(vary_fragcoord, center_depth, scatter), 0.0);
}
