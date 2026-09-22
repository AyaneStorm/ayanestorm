/**
 * @file asDepthOfFieldResolveF.glsl
 * @author chanayane@firestorm
 * @brief Ordered far-then-near resolve for AyaneStorm DoF.
 */
out vec4 frag_color;

uniform sampler2D diffuseRect;
// Viewer-reserved sampler names receive distinct texture units automatically.
uniform sampler2D noiseMap;
uniform sampler2D lightMap;
uniform sampler2D bloomMap;
uniform float max_radius;
uniform float near_max_radius;
uniform int debug_mode;

in vec2 vary_fragcoord;

void main()
{
    vec2 uv = vary_fragcoord;
    vec4 original = texture(diffuseRect, uv);
    float coc = texture(noiseMap, uv).r;
    vec4 far_color = texture(lightMap, uv);
    vec4 near_color = texture(bloomMap, uv);

    // Near data is premultiplied. Reconstruct neighboring independently phased
    // gathers to turn sparse coverage into a stable fractional silhouette.
    if (near_max_radius > 6.0)
    {
        vec2 texel = 1.0 / vec2(textureSize(bloomMap, 0));
        vec2 lo = texel * 0.5;
        vec2 hi = vec2(1.0) - lo;
        vec4 reconstructed = near_color * 4.0;
        reconstructed += texture(bloomMap, clamp(uv + texel * vec2(-1.0, -1.0), lo, hi));
        reconstructed += texture(bloomMap, clamp(uv + texel * vec2( 1.0, -1.0), lo, hi));
        reconstructed += texture(bloomMap, clamp(uv + texel * vec2(-1.0,  1.0), lo, hi));
        reconstructed += texture(bloomMap, clamp(uv + texel * vec2( 1.0,  1.0), lo, hi));
        reconstructed *= 0.125;
        near_color = mix(near_color, reconstructed,
                         smoothstep(6.0, 18.0, near_max_radius));
    }

    if (debug_mode == 1)
    {
        frag_color = coc < 0.0 ? vec4(-coc, 0.0, 0.0, 1.0)
                               : vec4(vec3(coc), 1.0);
        return;
    }
    if (debug_mode == 2)
    {
        frag_color = vec4(vec3(near_color.a), 1.0);
        return;
    }
    if (debug_mode == 3)
    {
        vec3 debug_far = far_color.a > 0.0001 ? far_color.rgb / far_color.a : vec3(0.0);
        frag_color = vec4(debug_far, 1.0);
        return;
    }
    if (debug_mode == 4)
    {
        vec3 debug_near = near_color.a > 0.0001 ? near_color.rgb / near_color.a : vec3(0.0);
        frag_color = vec4(debug_near, 1.0);
        return;
    }

    float pixel_blur = abs(coc) * max_radius;
    float far_blend = coc > 0.0 ? smoothstep(0.5, 2.0, pixel_blur) : 0.0;

    // Large discs undersample with a bounded gather budget. Neighboring
    // reduced-resolution pixels use decorrelated aperture phases, so a small
    // tent reconstruction removes coherent grids without softening low-radius
    // detail or changing the foreground layer.
    if (coc > 0.0 && pixel_blur > 6.0)
    {
        vec2 texel = 1.0 / vec2(textureSize(lightMap, 0));
        vec2 lo = texel * 0.5;
        vec2 hi = vec2(1.0) - lo;
        vec4 reconstructed = far_color * 4.0;
        reconstructed += texture(lightMap, clamp(uv + texel * vec2(-1.0, -1.0), lo, hi));
        reconstructed += texture(lightMap, clamp(uv + texel * vec2( 1.0, -1.0), lo, hi));
        reconstructed += texture(lightMap, clamp(uv + texel * vec2(-1.0,  1.0), lo, hi));
        reconstructed += texture(lightMap, clamp(uv + texel * vec2( 1.0,  1.0), lo, hi));
        reconstructed *= 0.125;
        far_color = mix(far_color, reconstructed,
                        smoothstep(6.0, 18.0, pixel_blur));
    }

    vec3 resolved_far = far_color.a > 0.0001 ? far_color.rgb / far_color.a : original.rgb;
    vec3 color = mix(original.rgb, resolved_far, far_blend);
    float near_pixel_blur = max(-coc, 0.0) * near_max_radius;
    // The near gather guarantees full ownership of source foreground pixels
    // only once their blur reaches one pixel. Revealing synthesized background
    // before then causes camera-dependent seams near the focal plane.
    if (near_pixel_blur > 1.0 && far_color.a > 0.0001)
    {
        vec3 background_fill = far_color.rgb / far_color.a;
        color = mix(color, background_fill, far_color.a);
    }
    color = color * (1.0 - near_color.a) + near_color.rgb;
    frag_color = vec4(color, original.a);
}
