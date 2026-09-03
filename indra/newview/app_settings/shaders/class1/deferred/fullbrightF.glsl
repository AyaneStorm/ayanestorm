/**
 * @file deferred/fullbrightF.glsl
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

// <AS:Chanayane> Independent OIT output declarations
// out vec4 frag_color;
#ifdef EXACT_OIT
void exact_oit_store(vec4 color);
#elif defined(AVBOIT)
void avboit_store(vec4 color);
uniform int avboitRasterPass;
#else
out vec4 frag_color;
#endif
// </AS:Chanayane>

#if !defined(HAS_DIFFUSE_LOOKUP)
uniform sampler2D diffuseMap;
#endif

in vec3 vary_position;
in vec4 vertex_color;
in vec2 vary_texcoord0;

// <AS:Chanayane> Cumulative foreground scatter for alpha transparency.
uniform vec2 screen_res;
uniform sampler2D asVolumetricAtlas;
uniform int asVolumetricEnabled;

vec3 asVolumetricForeground(vec3 view_position)
{
    if (asVolumetricEnabled == 0) return vec3(0.0);
    float coordinate = sqrt(clamp(length(view_position) / 128.0, 0.0, 1.0)) * 16.0;
    float upper = clamp(floor(coordinate), 0.0, 15.0);
    float weight = fract(coordinate);
    if (coordinate >= 16.0) { upper = 15.0; weight = 1.0; }
    vec2 uv = clamp(gl_FragCoord.xy / screen_res, 2.0 / screen_res,
                    vec2(1.0) - 2.0 / screen_res);
    vec2 tile = vec2(mod(upper, 4.0), floor(upper / 4.0));
    vec3 hi = texture(asVolumetricAtlas, (tile + uv) * 0.25).rgb;
    if (upper <= 0.0) return hi * clamp(coordinate, 0.0, 1.0);
    float lower = upper - 1.0;
    tile = vec2(mod(lower, 4.0), floor(lower / 4.0));
    vec3 lo = texture(asVolumetricAtlas, (tile + uv) * 0.25).rgb;
    return mix(lo, hi, weight);
}

// Scene transmittance T, same atlas alpha channel - mirrors
// asVolumetricForeground's tile lookup, reading .a instead of .rgb, with no
// near-camera clamp-by-coordinate term (T is already 1.0 at the camera).
float asVolumetricTransmittance(vec3 view_position)
{
    if (asVolumetricEnabled == 0) return 1.0;
    float coordinate = sqrt(clamp(length(view_position) / 128.0, 0.0, 1.0)) * 16.0;
    float upper = clamp(floor(coordinate), 0.0, 15.0);
    float weight = fract(coordinate);
    if (coordinate >= 16.0) { upper = 15.0; weight = 1.0; }
    vec2 uv = clamp(gl_FragCoord.xy / screen_res, 2.0 / screen_res,
                    vec2(1.0) - 2.0 / screen_res);
    vec2 tile = vec2(mod(upper, 4.0), floor(upper / 4.0));
    float hi = texture(asVolumetricAtlas, (tile + uv) * 0.25).a;
    if (upper <= 0.0) return hi;
    float lower = upper - 1.0;
    tile = vec2(mod(lower, 4.0), floor(lower / 4.0));
    float lo = texture(asVolumetricAtlas, (tile + uv) * 0.25).a;
    return mix(lo, hi, weight);
}
// </AS:Chanayane>

vec3 srgb_to_linear(vec3 cs);
vec3 linear_to_srgb(vec3 cl);

#ifdef HAS_ALPHA_MASK
uniform float minimum_alpha;
#endif

#ifdef IS_ALPHA
uniform vec4 waterPlane;
void waterClip(vec3 pos);
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten);
vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
#endif

void mirrorClip(vec3 pos);

void main()
{
    mirrorClip(vary_position);
#ifdef IS_ALPHA
    waterClip(vary_position.xyz);
#endif

#ifdef HAS_DIFFUSE_LOOKUP
    vec4 color = diffuseLookup(vary_texcoord0.xy);
#else
    vec4 color = texture(diffuseMap, vary_texcoord0.xy);
#endif

    float final_alpha = color.a * vertex_color.a;

#ifdef HAS_ALPHA_MASK
    if (color.a < minimum_alpha)
    {
        discard;
    }
#endif

// <AS:Chanayane> AVBOIT prepasses stop after texture alpha and masking.
#if defined(AVBOIT)
    if (avboitRasterPass < 2)
    {
        avboit_store(vec4(0.0, 0.0, 0.0, final_alpha));
        return;
    }
#endif
// </AS:Chanayane>

    color.rgb *= vertex_color.rgb;

    vec3 pos = vary_position;

    color.a = final_alpha;
#ifndef IS_HUD
    color.rgb = srgb_to_linear(color.rgb);
#ifdef IS_ALPHA

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVars(pos.xyz, vec3(0), 1.0, sunlit, amblit, additive, atten);

    color.rgb = applySkyAndWaterFog(pos, additive, atten, color).rgb;

#endif

#endif

// <AS:Chanayane> Attenuate scene color by transmittance, then add
// camera-to-fragment scatter - IS_ALPHA only. The plain opaque/alpha-masked
// fullbright permutations (gDeferredFullbrightProgram,
// gDeferredFullbrightAlphaMaskProgram) write into the gbuffer/screen during
// renderGeomDeferred(), which runs BEFORE ASVolumetricLighting::renderPass()'s
// opaque composite - that composite already attenuates the entire screen by
// the same T, so applying it here too double-attenuates those pixels a
// second time (confirmed: fullbright opaque surfaces, e.g. a monitor/media
// screen, went dark far faster than surrounding lit geometry at the same
// extinction). Only the truly alpha-blended permutation
// (gDeferredFullbrightAlphaMaskAlphaProgram) forward-renders later, via
// renderGeomPostDeferred(), after the composite has already run - that one
// still needs to self-attenuate here, same as alphaF.glsl/pbralphaF.glsl.
// #if !defined(IS_HUD)
//     color.rgb += asVolumetricForeground(pos);
// #endif
#if defined(IS_ALPHA)
    // color.rgb = color.rgb * asVolumetricTransmittance(pos) + asVolumetricForeground(pos);
    color.rgb += asVolumetricForeground(pos);
#endif
// </AS:Chanayane>

// <AS:Chanayane> Replace the original framebuffer output only during OIT capture.
// frag_color = max(color, vec4(0));
#ifdef EXACT_OIT
    color = max(color, vec4(0));
    exact_oit_store(color);
#elif defined(AVBOIT)
    avboit_store(max(color, vec4(0)));
#else
    frag_color = max(color, vec4(0));
#endif
// </AS:Chanayane>
}
