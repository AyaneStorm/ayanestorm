/**
 * @file class1\deferred\moonF.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, 2020 Linden Research, Inc.
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

out vec4 frag_data[4];

uniform vec4 color;
uniform vec3 moon_dir;
uniform float moon_brightness;
// <AS:Chanayane> User-controlled lower bound for the legacy horizon fade.
uniform float moon_horizon_min_opacity;
uniform vec3 moon_horizon_tint;
uniform float moon_horizon_tint_strength;
uniform float moon_horizon_tint_height;
uniform int moon_render_partial;
uniform float moon_phase;
uniform float moon_phase_curvature;
uniform float moon_phase_softness;
uniform float moon_phase_tilt;
uniform float moon_earthshine_strength;
uniform float moon_terminator_relief_strength;
uniform float moon_terminator_relief_width;
uniform int moon_halo_pass;
uniform vec3 moon_halo_color;
uniform float moon_halo_strength;
uniform float moon_halo_radius;
uniform float moon_halo_softness;
uniform float moon_halo_illumination;
// </AS:Chanayane>
uniform sampler2D diffuseMap;

in vec2 vary_texcoord0;

// <AS:Chanayane> Sample the moon mask with a transparent border.
// The moon mask must behave as if it had a transparent border. The texture is
// configured clamp-to-edge, so sampling outside [0,1] would otherwise repeat
// nonzero edge alpha and create a discontinuity where mask filtering stops.
float moonMaskAlpha(vec2 uv, vec2 texel_size)
{
    // Emulate bilinear filtering against transparent border texels. Sampling
    // the clamped edge alone repeats its alpha; a hard bounds test would move
    // the same discontinuity to UV 0/1. At the boundary the edge texel carries
    // half weight, reaching full edge weight half a texel inward and zero half
    // a texel outward.
    vec2 lower_weight = clamp(uv / texel_size + vec2(0.5), 0.0, 1.0);
    vec2 upper_weight = clamp((vec2(1.0) - uv) / texel_size + vec2(0.5), 0.0, 1.0);
    float border_weight = lower_weight.x * lower_weight.y
                        * upper_weight.x * upper_weight.y;
    return texture(diffuseMap, clamp(uv, vec2(0.0), vec2(1.0))).a
         * border_weight;
}
// </AS:Chanayane>

void main()
{
    // <AS:Chanayane> Larger procedural billboard rendered behind the disc.
    if (moon_halo_pass != 0)
    {
        // FACE_BLOOM always spans four disc radii so radius changes are live.
        float distance_in_disc_radii = length(vary_texcoord0.xy * 2.0 - 1.0) * 4.0;
        float outside_disc = max(distance_in_disc_radii - 1.0, 0.0);
        float softness = max(moon_halo_softness, 0.01);
        float profile = exp(-0.5 * outside_disc * outside_disc / (softness * softness));
        // Use the texture's actual alpha boundary rather than an assumed
        // analytic circle. Moon textures can contain transparent padding, and
        // masking at radius 1 leaves a dark annulus between the two borders.
        vec2 halo_position = (vary_texcoord0.xy * 2.0 - 1.0) * 4.0;
        vec2 moon_mask_uv = vec2(0.5) + 0.5 * halo_position;
        float moon_coverage = 0.0;
        // Filter only near the disc, where the boundary can occur. Five
        // Gaussian-weighted samples along the radial edge turn the raw
        // one-texel alpha cutoff into a progressive inward halo reduction.
        if (distance_in_disc_radii < 1.25)
        {
            vec2 mask_direction = moon_mask_uv - vec2(0.5);
            float mask_direction_length = length(mask_direction);
            mask_direction = mask_direction_length > 0.0001
                           ? mask_direction / mask_direction_length
                           : vec2(1.0, 0.0);
            vec2 mask_texel = 1.0 / vec2(textureSize(diffuseMap, 0));
            float mask_spacing = mix(1.5, 4.0, clamp(softness * 0.5, 0.0, 1.0));
            vec2 mask_step = mask_direction * max(mask_texel.x, mask_texel.y)
                           * mask_spacing;
            moon_coverage = (moonMaskAlpha(moon_mask_uv - 2.0 * mask_step, mask_texel)
                           + 4.0 * moonMaskAlpha(moon_mask_uv - mask_step, mask_texel)
                           + 6.0 * moonMaskAlpha(moon_mask_uv, mask_texel)
                           + 4.0 * moonMaskAlpha(moon_mask_uv + mask_step, mask_texel)
                           + moonMaskAlpha(moon_mask_uv + 2.0 * mask_step, mask_texel))
                          / 16.0;
        }
        // Treat 50% filtered texture coverage as the visible moon border. Keep
        // the halo at full strength through that border, then feather it only
        // inward toward fully covered moon pixels. Starting attenuation at the
        // first nonzero coverage suppresses glow outside the limb and creates
        // a contrast ring even with correct additive blending.
        profile *= 1.0 - smoothstep(0.5, 1.0, moon_coverage);
        float outer_fade = 1.0 - smoothstep(max(moon_halo_radius - softness, 1.0),
                                            moon_halo_radius,
                                            distance_in_disc_radii);
        profile *= outer_fade;
        // Fully masked fragments must not reach any MRT.
        if (profile <= 0.0001)
        {
            discard;
        }

        // Keep a circular atmospheric base, but weight it toward the visibly
        // illuminated limb. Sampling slightly inside the spherical rim avoids
        // its zero-depth ambiguity. The 0.15 floor leaves the faint dark-side
        // glow seen in reference photography instead of cutting the halo out.
        float halo_position_length = length(halo_position);
        vec2 halo_disc_position = halo_position_length > 0.0001
                                ? halo_position * (0.85 / halo_position_length)
                                : vec2(0.0);
        float halo_surface_z = sqrt(max(1.0 - dot(halo_disc_position,
                                                   halo_disc_position), 0.0));
        halo_surface_z = pow(halo_surface_z, clamp(moon_phase_curvature, 0.25, 5.0));
        float halo_phase_cycle = clamp(moon_phase, 0.0, 1.0);
        float halo_lit_fraction = 1.0 - abs(2.0 * halo_phase_cycle - 1.0);
        float halo_light_z = 2.0 * halo_lit_fraction - 1.0;
        float halo_light_x = sqrt(max(1.0 - halo_light_z * halo_light_z, 0.0));
        halo_light_x *= halo_phase_cycle <= 0.5 ? 1.0 : -1.0;
        float halo_tilt = clamp(moon_phase_tilt, -180.0, 180.0) * 0.01745329252;
        vec2 halo_light_xy = halo_light_x * vec2(cos(halo_tilt), sin(halo_tilt));
        float halo_phase_light = dot(vec3(halo_disc_position, halo_surface_z),
                                     vec3(halo_light_xy, halo_light_z));
        float halo_phase_width = max(moon_phase_softness, 0.002) + 0.15 * softness;
        float halo_phase_transition = clamp((halo_phase_light + halo_phase_width)
                                          / (2.0 * halo_phase_width), 0.0, 1.0);
        float halo_limb_light = halo_phase_transition * halo_phase_transition
                              * (3.0 - 2.0 * halo_phase_transition);
        float halo_limb_weight = mix(0.15, 1.0, halo_limb_light);

        float energy = clamp(moon_halo_strength, 0.0, 10.0)
                     * clamp(moon_halo_illumination, 0.0, 1.0)
                     * halo_limb_weight;
        // Keep strength linear under the alpha-modulated additive blend.
        vec4 halo = vec4(clamp(moon_halo_color, 0.0, 1.0),
                         clamp(profile * energy, 0.0, 1.0));
        frag_data[0] = vec4(0.0);
        frag_data[1] = vec4(0.0);
        // Preserve the sky's existing G-buffer metadata. Writing a categorical
        // flag from a translucent emissive pass produces dark blended edges.
        frag_data[2] = vec4(0.0);
#if defined(HAS_EMISSIVE)
        frag_data[3] = halo;
#else
        frag_data[0] = halo;
#endif
        return;
    }
    // </AS:Chanayane>

    // <AS:Chanayane> Preserve the legacy fade shape but prevent the moon from
    // disappearing into horizon haze. A setting of zero reproduces upstream.
    // // Restore Pre-EEP alpha fade moon near horizon
    // float fade = 1.0;
    // if( moon_dir.z > 0 )
    //     fade = clamp( moon_dir.z*moon_dir.z*4.0, 0.0, 1.0 );
    float fade = 1.0;
    if (moon_render_partial != 0)
    {
        float legacy_fade = clamp(max(moon_dir.z, 0.0) * max(moon_dir.z, 0.0) * 4.0, 0.0, 1.0);
        fade = mix(clamp(moon_horizon_min_opacity, 0.0, 1.0), 1.0, legacy_fade);
    }
    else if (moon_dir.z > 0.0)
    {
        float legacy_fade = clamp(moon_dir.z * moon_dir.z * 4.0, 0.0, 1.0);
        fade = mix(clamp(moon_horizon_min_opacity, 0.0, 1.0), 1.0, legacy_fade);
    }
    // </AS:Chanayane>

    vec4 c      = texture(diffuseMap, vary_texcoord0.xy);

    // SL-14113 Don't write to depth; prevent moon's quad from hiding stars which should be visible
    // Moon texture has transparent pixels <0x55,0x55,0x55,0x00>
    if (c.a <= 2./255.) // 0.00784
    {
        discard;
    }

    // <AS:Chanayane> Reconstruct the front hemisphere from the disc UV and
    // intersect it with a rotating light direction. This produces a spherical
    // terminator for crescent, quarter, gibbous, and full phases while keeping
    // the environment's moon texture detail intact.
    vec2 phase_position = vary_texcoord0.xy * 2.0 - 1.0;
    float phase_surface_z = sqrt(max(1.0 - dot(phase_position, phase_position), 0.0));
    // A nonlinear depth warp genuinely changes the projected terminator
    // shape. Linear scaling here only renormalized the effective light angle.
    phase_surface_z = pow(phase_surface_z,
                          clamp(moon_phase_curvature, 0.25, 5.0));
    // Map the artistic control linearly by visible illuminated area rather
    // than orbital angle. The former cosine mapping compressed almost all
    // visible gibbous change into a few pixels around 0.45/0.55 and made
    // crescents unnecessarily thin on typical on-screen moon sizes.
    float phase_cycle = clamp(moon_phase, 0.0, 1.0);
    float illuminated_fraction = 1.0 - abs(2.0 * phase_cycle - 1.0);
    float phase_light_z = 2.0 * illuminated_fraction - 1.0;
    float phase_light_x = sqrt(max(1.0 - phase_light_z * phase_light_z, 0.0));
    phase_light_x *= phase_cycle <= 0.5 ? 1.0 : -1.0;
    float phase_tilt_radians = clamp(moon_phase_tilt, -180.0, 180.0) * 0.01745329252;
    vec2 phase_light_xy = phase_light_x * vec2(cos(phase_tilt_radians),
                                               sin(phase_tilt_radians));
    vec3 phase_light_dir = vec3(phase_light_xy, phase_light_z);
    float phase_light = dot(vec3(phase_position, phase_surface_z), phase_light_dir);

    // Accentuate texture detail across the incoming-light direction only in a
    // narrow terminator band. This suggests the long crater shadows seen under
    // grazing illumination without sharpening the rest of the moon.
    float relief_width = clamp(moon_terminator_relief_width, 0.01, 0.5);
    float relief_band = 1.0 - smoothstep(0.0, relief_width, abs(phase_light));
    vec2 relief_axis = phase_light_xy;
    float relief_axis_length = length(relief_axis);
    if (relief_axis_length > 0.001 && moon_terminator_relief_strength > 0.0)
    {
        relief_axis /= relief_axis_length;
        vec2 relief_offset = relief_axis * 2.0 / vec2(textureSize(diffuseMap, 0));
        vec3 relief_before = texture(diffuseMap, vary_texcoord0.xy - relief_offset).rgb;
        vec3 relief_after = texture(diffuseMap, vary_texcoord0.xy + relief_offset).rgb;
        const vec3 relief_luma = vec3(0.2126, 0.7152, 0.0722);
        float center_luma = dot(c.rgb, relief_luma);
        float neighbor_luma = 0.5 * dot(relief_before + relief_after, relief_luma);
        float relief_detail = center_luma - neighbor_luma;
        c.rgb = max(c.rgb + vec3(relief_detail * relief_band
                               * clamp(moon_terminator_relief_strength, 0.0, 2.0)),
                    vec3(0.0));
    }
    float phase_edge_width = max(fwidth(phase_light), 0.002)
                           + clamp(moon_phase_softness, 0.0, 0.30);
    // Keep the perceived boundary anchored to the geometric terminator: the
    // illumination is exactly 0.5 when phase_light is zero. Smoothstep eases
    // both plateaus while concentrating the strongest change around that line.
    // The result controls phase-surface visibility below.
    float phase_transition = clamp((phase_light + phase_edge_width)
                                 / (2.0 * phase_edge_width), 0.0, 1.0);
    float phase_illumination = phase_transition * phase_transition
                             * (3.0 - 2.0 * phase_transition);
    // Blend the shadowed hemisphere over the already rendered sky instead of
    // replacing it with an opaque black disc. Earthshine is the minimum
    // shadow-side visibility; halo and god-ray energy still use direct phase
    // illumination exclusively.
    float visible_phase_surface = mix(clamp(moon_earthshine_strength, 0.0, 0.30),
                                      1.0, phase_illumination);
    c.a *= visible_phase_surface;
    // </AS:Chanayane>

    c.rgb *= moon_brightness;
    // <AS:Chanayane> Warm only the visible moon disc near the horizon. The
    // user-selected elevation controls where the effect smoothly ends.
    float horizon_tint_amount = (1.0 - smoothstep(0.0, moon_horizon_tint_height, max(moon_dir.z, 0.0)))
                              * clamp(moon_horizon_tint_strength, 0.0, 1.0);
    c.rgb *= mix(vec3(1.0), clamp(moon_horizon_tint, 0.0, 1.0), horizon_tint_amount);
    // </AS:Chanayane>
    c.a   *= fade;

    frag_data[0] = vec4(0);
    frag_data[1] = vec4(0.0);
    // <AS:Chanayane> Preserve underlying sky metadata for phase transparency.
    // A categorical skip-atmosphere flag cannot be alpha-blended safely and
    // otherwise forms a dark outline around translucent moon texture edges.
    // frag_data[2] = vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
    frag_data[2] = vec4(0.0);
    // </AS:Chanayane>

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = vec4(c.rgb, c.a);
#else
    frag_data[0] = vec4(c.rgb, c.a);
#endif

    // Added and commented out for a ground truth.  Do not uncomment - Geenz
    //gl_FragDepth = 0.999985f;
}
