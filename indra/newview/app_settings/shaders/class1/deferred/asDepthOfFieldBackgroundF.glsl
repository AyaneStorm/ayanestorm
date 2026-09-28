/**
 * @file asDepthOfFieldBackgroundF.glsl
 * @author chanayane@firestorm
 * @brief Background completion behind foreground pixels for AyaneStorm DoF.
 *
 * A defocused foreground silhouette reveals background the central view
 * never saw. Push-pull estimates it from the nearest non-foreground pixels
 * on screen (finest mip level with enough of them). The far gather then
 * blurs the completion with its own CoC before the near layers composite
 * over it. This is an estimate: screen-space DoF cannot recover fully
 * hidden content.
 *
 * Unweighted by depth on purpose: a first version favoured farther
 * surfaces (exp2(3 * CoC)), which filled a hair lock over an in-focus cheek
 * with the distant sky seen past the head.
 *
 * bg_pass 0 (push, blur resolution, two attachments, mips generated after):
 *   0: (rgb * w, w), 1: (coc * w, 0, 0, valid fraction), averaged over the
 *   full-resolution footprint; w is 1 for non-foreground texels.
 * bg_pass 1 (pull, blur resolution): (rgb, signed CoC); holes take the
 *   finest mip level with enough valid background.
 */
layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;

// Gather input: opaque color with extracted highlight sprites removed.
uniform sampler2D diffuseRect;
// Signed CoC target; g is the opaque CoC.
uniform sampler2D noiseMap;
// Push attachments with mips (pass 1 only).
uniform sampler2D specularRect;
uniform sampler2D emissiveRect;
uniform vec2 screen_res;
uniform vec2 target_res;
uniform int bg_pass;
uniform int max_level;

// Keeps sun-level linear HDR well inside half-float range in the plate.
#define MAX_RADIANCE 4096.0

void main()
{
    if (bg_pass == 0)
    {
        // Point-fetch a 2x2 set of full-resolution texels: interpolating a
        // foreground and a background texel would mix colors and CoC signs.
        vec2 scale = screen_res / target_res;
        ivec2 last = ivec2(screen_res) - 1;
        vec4 color_sum = vec4(0.0);
        float coc_sum = 0.0;
        float valid = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            vec2 offset = vec2(float(i & 1), float(i >> 1)) * 0.5 + 0.25;
            ivec2 p = clamp(ivec2((floor(gl_FragCoord.xy) + offset) * scale),
                            ivec2(0), last);
            float coc = texelFetch(noiseMap, p, 0).g;
            if (coc < -0.0001)
            {
                continue;
            }
            vec3 color = min(texelFetch(diffuseRect, p, 0).rgb, vec3(MAX_RADIANCE));
            color_sum += vec4(color, 1.0);
            coc_sum += coc;
            valid += 1.0;
        }
        frag_data0 = color_sum * 0.25;
        frag_data1 = vec4(coc_sum * 0.25, 0.0, 0.0, valid * 0.25);
        return;
    }

    vec2 uv = gl_FragCoord.xy / target_res;
    vec4 own = texelFetch(specularRect, ivec2(gl_FragCoord.xy), 0);
    if (own.a > 0.0)
    {
        vec4 own_coc = texelFetch(emissiveRect, ivec2(gl_FragCoord.xy), 0);
        frag_data0 = vec4(own.rgb / own.a, own_coc.r / own.a);
        frag_data1 = vec4(0.0);
        return;
    }

    // Finest level with enough background, blended toward the next coarser
    // one while its valid fraction is small, so the fill has no mip blocks.
    vec4 result = vec4(texture(diffuseRect, uv).rgb, 0.0);
    for (int level = 1; level <= max_level; ++level)
    {
        vec4 fine = textureLod(specularRect, uv, float(level));
        float fine_valid = textureLod(emissiveRect, uv, float(level)).a;
        if (fine.a <= 0.0 || fine_valid < 0.05)
        {
            continue;
        }
        result = vec4(fine.rgb / fine.a,
                      textureLod(emissiveRect, uv, float(level)).r / fine.a);
        if (level < max_level)
        {
            vec4 coarse = textureLod(specularRect, uv, float(level + 1));
            if (coarse.a > 0.0)
            {
                vec4 coarse_result = vec4(coarse.rgb / coarse.a,
                    textureLod(emissiveRect, uv, float(level + 1)).r / coarse.a);
                result = mix(coarse_result, result, smoothstep(0.05, 0.5, fine_valid));
            }
        }
        break;
    }
    frag_data0 = result;
    frag_data1 = vec4(0.0);
}
