/**
 * @file asmilkywayF.glsl
 * @author chanayane@firestorm
 * @brief AyaneStorm real-sky Milky Way and deep-sky glow, sky-dome fragment
 * shader. Samples two equirectangular J2000 maps (the Milky Way band and the
 * deep-sky nebulae, gamma-encoded RGB) in the direction rotated into the
 * equatorial frame of the real-sky stars.
 */

in vec3 vary_mw_direction;

uniform sampler2D as_milky_way_map;
uniform sampler2D as_deep_sky_map;
// Local (x east, y north, z up) -> equatorial rotation, as mat3 columns.
uniform vec3 mw_rot0;
uniform vec3 mw_rot1;
uniform vec3 mw_rot2;
uniform float mw_intensity;      // Milky Way level, night factor included
uniform float mw_dso_intensity;  // deep-sky level, night factor included
uniform float mw_saturation;

out vec4 frag_data[4];

const float AS_MW_PI = 3.14159265359;

void main()
{
    vec3 local_dir = normalize(vary_mw_direction);
    vec3 eq = mat3(mw_rot0, mw_rot1, mw_rot2) * local_dir;

    // u = (RA + 180) / 360 with RA in [-180, 180]; v = (dec + 90) / 180.
    vec2 uv = vec2(atan(eq.y, eq.x) / (2.0 * AS_MW_PI) + 0.5,
                   asin(clamp(eq.z, -1.0, 1.0)) / AS_MW_PI + 0.5);
    // Stored as value^(1/2.2) so 8 bits do not band in the faint glow.
    vec3 band = pow(textureLod(as_milky_way_map, uv, 0.0).rgb, vec3(2.2));
    vec3 deep_sky = pow(textureLod(as_deep_sky_map, uv, 0.0).rgb, vec3(2.2));

    vec3 color = band * mw_intensity + deep_sky * mw_dso_intensity;
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = max(vec3(lum) + (color - vec3(lum)) * mw_saturation, vec3(0.0));

    // Atmospheric extinction toward the horizon; nothing below it.
    color *= smoothstep(0.0, 0.15, local_dir.z);

    frag_data[1] = vec4(0.0);
    frag_data[2] = vec4(0.0);
#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0.0);
    frag_data[3] = vec4(color, 0.0);
#else
    frag_data[0] = vec4(color, 0.0);
    frag_data[3] = vec4(0.0);
#endif
}
