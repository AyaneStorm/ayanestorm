/**
 * @file alphaF.glsl
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

//class2/deferred/alphaF.glsl

/*[EXTRA_CODE_HERE]*/

#define INDEXED 1
#define NON_INDEXED 2
#define NON_INDEXED_NO_COLOR 3

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

uniform mat3 env_mat;
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int classic_mode;

#ifdef USE_DIFFUSE_TEX
uniform sampler2D diffuseMap;
#endif

in vec3 vary_fragcoord;
in vec3 vary_position;
in vec2 vary_texcoord0;
in vec3 vary_norm;

#ifdef USE_VERTEX_COLOR
in vec4 vertex_color; //vertex color should be treated as sRGB
#endif

uniform float minimum_alpha;

uniform mat4 proj_mat;
uniform mat4 inv_proj;
uniform vec2 screen_res;
// <AS:Chanayane> Cumulative foreground scatter for alpha transparency.
uniform sampler2D asVolumetricAtlas;
uniform int asVolumetricEnabled;
uniform int sun_up_factor;
uniform vec4 light_position[8];
uniform vec3 light_direction[8];
uniform vec4 light_attenuation[8];
uniform vec3 light_diffuse[8];

vec3 asVolumetricForeground(vec3 view_position)
{
    if (asVolumetricEnabled == 0)
    {
        return vec3(0.0);
    }

    const float max_distance = 128.0;
    float coordinate = sqrt(clamp(length(view_position) / max_distance, 0.0, 1.0)) * 16.0;
    float upper = clamp(floor(coordinate), 0.0, 15.0);
    float blend_weight = fract(coordinate);
    if (coordinate >= 16.0)
    {
        upper = 15.0;
        blend_weight = 1.0;
    }

    vec2 screen_uv = gl_FragCoord.xy / screen_res;
    vec2 inset = 2.0 / screen_res;
    screen_uv = clamp(screen_uv, inset, vec2(1.0) - inset);
    vec2 upper_tile = vec2(mod(upper, 4.0), floor(upper / 4.0));
    vec3 upper_value = texture(asVolumetricAtlas, (upper_tile + screen_uv) * 0.25).rgb;
    if (upper <= 0.0)
    {
        return upper_value * clamp(coordinate, 0.0, 1.0);
    }

    float lower = upper - 1.0;
    vec2 lower_tile = vec2(mod(lower, 4.0), floor(lower / 4.0));
    vec3 lower_value = texture(asVolumetricAtlas, (lower_tile + screen_uv) * 0.25).rgb;
    return mix(lower_value, upper_value, blend_weight);
}

// Scene transmittance T = exp(-extinction * distance), carried in the same
// atlas's alpha channel - mirrors asVolumetricForeground's own tile lookup
// exactly (see asVolumetricAtlasF.glsl) but reads .a instead of .rgb, and has
// no near-camera clamp-by-coordinate term since T is not a cumulative
// scatter quantity that fades in from zero at the camera; it is already 1.0
// there.
float asVolumetricTransmittance(vec3 view_position)
{
    if (asVolumetricEnabled == 0)
    {
        return 1.0;
    }

    const float max_distance = 128.0;
    float coordinate = sqrt(clamp(length(view_position) / max_distance, 0.0, 1.0)) * 16.0;
    float upper = clamp(floor(coordinate), 0.0, 15.0);
    float blend_weight = fract(coordinate);
    if (coordinate >= 16.0)
    {
        upper = 15.0;
        blend_weight = 1.0;
    }

    vec2 screen_uv = gl_FragCoord.xy / screen_res;
    vec2 inset = 2.0 / screen_res;
    screen_uv = clamp(screen_uv, inset, vec2(1.0) - inset);
    vec2 upper_tile = vec2(mod(upper, 4.0), floor(upper / 4.0));
    float upper_value = texture(asVolumetricAtlas, (upper_tile + screen_uv) * 0.25).a;
    if (upper <= 0.0)
    {
        return upper_value;
    }

    float lower = upper - 1.0;
    vec2 lower_tile = vec2(mod(lower, 4.0), floor(lower / 4.0));
    float lower_value = texture(asVolumetricAtlas, (lower_tile + screen_uv) * 0.25).a;
    return mix(lower_value, upper_value, blend_weight);
}
// </AS:Chanayane>

void waterClip(vec3 pos);

vec3 srgb_to_linear(vec3 c);
vec3 linear_to_srgb(vec3 c);

vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);

#ifdef HAS_SUN_SHADOW
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif

float getAmbientClamp();

void mirrorClip(vec3 pos);

void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);

vec3 calcPointLightOrSpotLight(vec3 light_col, vec3 diffuse, vec3 v, vec3 n, vec4 lp, vec3 ln, float la, float fa, float is_pointlight, float ambiance)
{
    // SL-14895 inverted attenuation work-around
    // This routine is tweaked to match deferred lighting, but previously used an inverted la value. To reconstruct
    // that previous value now that the inversion is corrected, we reverse the calculations in LLPipeline::setupHWLights()
    // to recover the `adjusted_radius` value previously being sent as la.
    float falloff_factor = (12.0 * fa) - 9.0;
    float inverted_la = falloff_factor / la;
    // Yes, it makes me want to cry as well. DJH

    vec3 col = vec3(0);

    //get light vector
    vec3 lv = lp.xyz-v;

    //get distance
    float dist = length(lv);
    float da = 1.0;

    /*if (dist > inverted_la)
    {
        return col;
    }

    clip to projector bounds
     vec4 proj_tc = proj_mat * lp;

    if (proj_tc.z < 0
     || proj_tc.z > 1
     || proj_tc.x < 0
     || proj_tc.x > 1
     || proj_tc.y < 0
     || proj_tc.y > 1)
    {
        return col;
    }*/

    if (dist > 0.0 && inverted_la > 0.0)
    {
        dist /= inverted_la;

        //normalize light vector
        lv = normalize(lv);

        //distance attenuation
        float dist_atten = clamp(1.0-(dist-1.0*(1.0-fa))/fa, 0.0, 1.0);
        dist_atten *= dist_atten;
        dist_atten *= 2.0f;

        if (dist_atten <= 0.0)
        {
           return col;
        }

        // spotlight coefficient.
        float spot = max(dot(-ln, lv), is_pointlight);
        da *= spot*spot; // GL_SPOT_EXPONENT=2

        //angular attenuation
        da *= dot(n, lv);
        da = max(0.0, da);

        float lit = 0.0f;

        float amb_da = 0.0;//ambiance;
        if (da > 0)
        {
            lit = clamp(da * dist_atten, 0.0, 1.0);
            col = lit * light_col * diffuse;
            amb_da += (da*0.5+0.5) * ambiance;
        }
        amb_da += (da*da*0.5 + 0.5) * ambiance;
        amb_da *= dist_atten;
        amb_da = min(amb_da, 1.0f - lit);

        // SL-10969 ... need to work out why this blows out in many setups...
        //col.rgb += amb_da * light_col * diffuse;

        // no spec for alpha shader...
    }
    float final_scale = 1.0;
    if (classic_mode > 0)
        final_scale = 0.9;
    col = max(col * final_scale, vec3(0));
    return col;
}

void main()
{
    mirrorClip(vary_position);

    vec2 frag = vary_fragcoord.xy/vary_fragcoord.z*0.5+0.5;

    vec4 pos = vec4(vary_position, 1.0);
#ifndef IS_AVATAR_SKIN
    // clip against water plane unless this is a legacy avatar skin
    waterClip(pos.xyz);
#endif
    vec3 norm = vary_norm;

    float shadow = 1.0f;

#ifdef USE_DIFFUSE_TEX
    vec4 diffuse_tap = texture(diffuseMap,vary_texcoord0.xy);
#endif

#ifdef USE_INDEXED_TEX
    vec4 diffuse_tap = diffuseLookup(vary_texcoord0.xy);
#endif

    vec4 diffuse_srgb = diffuse_tap;

#ifdef FOR_IMPOSTOR
    vec4 color;
    color.rgb = diffuse_srgb.rgb;
    color.a = 1.0;

    float final_alpha = diffuse_srgb.a * vertex_color.a;
    diffuse_srgb.rgb *= vertex_color.rgb;

    // Insure we don't pollute depth with invis pixels in impostor rendering
    //
    if (final_alpha < minimum_alpha)
    {
        discard;
    }

    color.rgb = diffuse_srgb.rgb;
    color.a = final_alpha;

#else // FOR_IMPOSTOR

    vec4 diffuse_linear = vec4(srgb_to_linear(diffuse_srgb.rgb), diffuse_srgb.a);

    vec3 light_dir = (sun_up_factor == 1) ? sun_dir: moon_dir; // TODO -- factor out "sun_up_factor" and just send in the appropriate light vector

    float final_alpha = diffuse_linear.a;

#ifdef IS_AVATAR_SKIN
    if(final_alpha < minimum_alpha)
    {
        discard;
    }
#endif

#ifdef USE_VERTEX_COLOR
    final_alpha *= vertex_color.a;

    if (final_alpha < minimum_alpha)
    { // TODO: figure out how to get invisible faces out of
        // render batches without breaking glow
        discard;
    }

    diffuse_srgb.rgb *= vertex_color.rgb;
    diffuse_linear.rgb = srgb_to_linear(diffuse_srgb.rgb);
#endif // USE_VERTEX_COLOR

// <AS:Chanayane> AVBOIT occupancy/extinction need only post-mask opacity.
#if defined(AVBOIT)
    if (avboitRasterPass < 2)
    {
        avboit_store(vec4(0.0, 0.0, 0.0, final_alpha));
        return;
    }
#endif
// </AS:Chanayane>

// <AS:Chanayane> Delay the original shadow sample until after AVBOIT early culling.
// #ifdef HAS_SUN_SHADOW
//     shadow = sampleDirectionalShadow(pos.xyz, norm.xyz, frag);
// #endif
#ifdef HAS_SUN_SHADOW
    shadow = sampleDirectionalShadow(pos.xyz, norm.xyz, frag);
#endif
// </AS:Chanayane>

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;

    calcAtmosphericVarsLinear(pos.xyz, norm, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
        sunlit *= 1.35;
    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec3 irradiance = amblit;
    vec3 glossenv;
    vec3 legacyenv;
    sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, frag, pos.xyz, norm.xyz, 0.0, 0.0, true, amblit_linear);


    float da = dot(norm.xyz, light_dir.xyz);
          da = clamp(da, -1.0, 1.0);

    float final_da = da;
          final_da = clamp(final_da, 0.0f, 1.0f);

    vec4 color = vec4(0.0);

    color.a   = final_alpha;

    color.rgb = irradiance;
    if (classic_mode > 0)
    {
        final_da = pow(final_da,1.2);
        vec3 sun_contrib = vec3(min(final_da, shadow));

        color.rgb = srgb_to_linear(color.rgb * 0.9 + linear_to_srgb(sun_contrib) * sunlit_linear * 0.7);
        sunlit_linear = srgb_to_linear(sunlit_linear);
    }
    else
    {
        vec3 sun_contrib = min(final_da, shadow) * sunlit_linear;
        color.rgb += sun_contrib;
    }

    color.rgb *= diffuse_linear.rgb;

    vec4 light = vec4(0,0,0,0);

   #define LIGHT_LOOP(i) light.rgb += calcPointLightOrSpotLight(light_diffuse[i].rgb, diffuse_linear.rgb, pos.xyz, norm, light_position[i], light_direction[i].xyz, light_attenuation[i].x, light_attenuation[i].y, light_attenuation[i].z, light_attenuation[i].w);

    LIGHT_LOOP(1)
    LIGHT_LOOP(2)
    LIGHT_LOOP(3)
    LIGHT_LOOP(4)
    LIGHT_LOOP(5)
    LIGHT_LOOP(6)
    LIGHT_LOOP(7)

    // sum local light contrib in linear colorspace
    color.rgb += light.rgb;

    color.rgb = applySkyAndWaterFog(pos.xyz, additive, atten, color).rgb;

#endif // #else // FOR_IMPOSTOR
    float final_scale = 1;
    if (classic_mode > 0)
        final_scale = 1.1;
#ifdef IS_HUD
    color.rgb = linear_to_srgb(color.rgb);
    final_scale = 1;
#endif

    color.rgb *= final_scale;
// <AS:Chanayane> Add camera-to-fragment scatter without applying the atlas
// transmittance to the forward alpha surface itself. Multiplying these
// minified alpha cards by T made the red forward-alpha LOD turn black inside
// shadowed volumetric regions, while the same asset's blue deferred alpha-mask
// LOD remained normal. The opaque scene behind it is already attenuated by the
// main composite, so ordinary alpha blending still places the transparent
// surface into the fogged scene without the red/blue discontinuity.
// color.rgb = color.rgb * asVolumetricTransmittance(vary_position) +
//             asVolumetricForeground(vary_position);
    color.rgb += asVolumetricForeground(vary_position);
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
