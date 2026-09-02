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

uniform sampler2D emissiveRect; // volumetric scatter target: half-res (Normal) or full-res (High)
uniform vec2 emissiveRectDelta; // 1 / sourceSize (source-texel units, NOT screen-texel units)
uniform int depthAwareUpsample; // debug-only: raw unfiltered sample, no gather
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
// Not standard deferred uniforms; uploaded by draw_composite() from
// LLViewerCamera::getInstance()->getNear()/getFar(), same pattern as
// screenSpaceReflPostF.glsl.
uniform float zNear;
uniform float zFar;

vec4 getPosition(vec2 pos_screen);
float getDepth(vec2 pos_screen);
float linearDepth(float d, float znear, float zfar);

// Same sky-ray cap the raymarch shaders use (MAX_MARCH_DISTANCE in
// asVolumetricLightF.glsl/asVolumetricAtlasF.glsl). Depths beyond this are
// indistinguishable for scatter purposes; without the clamp below, sky vs.
// a hill at 200m would be wrongly depth-rejected in the gather.
const float MAX_MARCH_DISTANCE = 128.0;

float linearViewDepth(vec2 uv)
{
    return linearDepth(getDepth(uv), zNear, zFar);
}

float depthWeight(float tap_depth, float center_depth)
{
    tap_depth    = min(tap_depth, MAX_MARCH_DISTANCE);
    center_depth = min(center_depth, MAX_MARCH_DISTANCE);
    float rel = abs(tap_depth - center_depth) / max(max(tap_depth, center_depth), 1.0);
    return exp(-rel * 8.0);
}

// Single depth-aware gather over the 4x4 source-texel window around this
// display pixel. Box weight: Bayer is 4-periodic, so any 4x4 window holds
// each of the 16 phases once and a sliding box is the exact
// reconstruction. Edge-class texels (alpha 1, diagnostic only) march a
// phase-refined multiple of the flat count, so mixing them here keeps the
// phase weights balanced; no class split is needed (see
// doc/volumetric_lighting_sample_count_question.md, round 4).
vec3 gatherScatter(vec2 uv, float center_depth)
{
    vec2 src = uv / emissiveRectDelta - 0.5;
    vec2 base = floor(src + 0.5);
    vec2 min_uv = emissiveRectDelta * 0.5;
    vec2 max_uv = vec2(1.0) - min_uv;
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int y = -2; y <= 1; ++y)
    {
        for (int x = -2; x <= 1; ++x)
        {
            vec2 tap_uv = clamp((base + vec2(float(x), float(y)) + 0.5) * emissiveRectDelta,
                                min_uv, max_uv);
            float w = depthWeight(linearViewDepth(tap_uv), center_depth);
            sum += texture(emissiveRect, tap_uv).rgb * w;
            wsum += w;
        }
    }
    if (wsum < 1e-6)
    {
        return texture(emissiveRect, uv).rgb; // subpixel surface fallback
    }
    return sum / wsum;
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
// The directional model contains only single scattering. In a shadowed ray
// its scatter can approach zero while unconstrained Beer-Lambert extinction
// also removes essentially all scene light, producing an unphysical black
// mass because ambient/multiple scattering is absent. Smoothly saturating
// optical depth preserves the configured response near zero (unit slope) but
// leaves roughly 10% direct scene transmittance at the extreme instead of
// converging to black. Keep this value and formula identical to
// asVolumetricAtlasF.glsl so opaque and transparent surfaces remain matched.
const float MAX_SCENE_OPTICAL_DEPTH = 2.3;

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
    float optical_depth = sceneDensity * dist;
    float limited_depth = MAX_SCENE_OPTICAL_DEPTH *
                          (1.0 - exp(-optical_depth / MAX_SCENE_OPTICAL_DEPTH));
    float beer_lambert = exp(-limited_depth);
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
    if (depthAwareUpsample != 0)
    {
        // Debug modes only: plain unfiltered sample, no gather.
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

    float d = linearViewDepth(vary_fragcoord);
    vec3 s = gatherScatter(vary_fragcoord, d);
    frag_color = vec4(s, compositeTransmittance(d));
}
