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

#ifdef WBOIT
const float WBOIT_MIN_ALPHA = 1.0 / 255.0;
out vec4 frag_data[2];
uniform int debugWBOITTint;
uniform int wboitAvatarLayer;
uniform sampler2D worldRevealTex;
float wboit_weight(float a, float depth) {
    return clamp(pow(clamp(a, 0.0, 1.0) + 0.01, 1.5) * 1e4 *
                 pow(1.0 - depth * 0.9, 12.0), 1e-2, 3e3);
}
float wboit_coverage_alpha(float a) {
    return mix(a, 1.0, smoothstep(0.995, 1.0, a));
}
float wboit_skinned_alpha(float a) {
#ifdef HAS_SKIN
    return mix(a, 1.0, smoothstep(0.55, 0.95, a));
#else
    return a;
#endif
}
float wboit_reveal_alpha(float a) {
    if (wboitAvatarLayer != 0) {
        float exponent = mix(1.65, 1.0, smoothstep(0.05, 0.25, a));
        float opacity = 1.0 - pow(max(1.0 - a, 0.0), exponent);
        return mix(opacity, 1.0, smoothstep(0.95, 1.0, a));
    } else {
        return mix(a, 1.0, smoothstep(0.95, 1.0, a));
    }
}
#else
out vec4 frag_color;
#endif

#if !defined(HAS_DIFFUSE_LOOKUP)
uniform sampler2D diffuseMap;
#endif

in vec3 vary_position;
in vec4 vertex_color;
in vec2 vary_texcoord0;

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

#ifdef WBOIT
    color = max(color, vec4(0));
    if (debugWBOITTint != 0)
    {
        color = vec4(1.0, 0.0, 1.0, 1.0);
    }
    if (color.a <= WBOIT_MIN_ALPHA)
    {
        discard;
    }
    float wboit_a = wboit_coverage_alpha(wboit_skinned_alpha(color.a));
    // <AS:Chanayane> Attenuate by world glass transmittance when in avatar layer
    if (wboitAvatarLayer != 0) {
        wboit_a *= texelFetch(worldRevealTex, ivec2(gl_FragCoord.xy), 0).r;
    }
    // </AS:Chanayane>
    float wboit_reveal = wboit_reveal_alpha(wboit_a);
    float wboit_w = wboit_weight(wboit_a, gl_FragCoord.z);
    frag_data[0] = vec4(color.rgb * wboit_a, wboit_a) * wboit_w;
    frag_data[1] = vec4(wboit_reveal);
#else
    frag_color = max(color, vec4(0));
#endif
}
