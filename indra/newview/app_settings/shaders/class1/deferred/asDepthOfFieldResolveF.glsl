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
uniform sampler2D specularRect;
uniform sampler2D emissiveRect;
uniform sampler2D positionMap;
uniform sampler2D projectionMap;
uniform float max_radius;
uniform float near_max_radius;
uniform int debug_mode;
uniform int has_transparent_depth;

in vec2 vary_fragcoord;

void main()
{
    vec2 uv = vary_fragcoord;
    vec4 source = texture(projectionMap, uv);
    vec4 transparent_surface = has_transparent_depth != 0 ?
        texture(specularRect, uv) : vec4(0.0);
    vec3 opaque_source = texture(diffuseRect, uv).rgb;
    vec4 coc_data = texture(noiseMap, uv);
    float coc = coc_data.r;
    float opaque_coc = coc_data.g;
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
    if (debug_mode == 5)
    {
        frag_color = vec4(vec3(coc_data.a), 1.0);
        return;
    }
    if (debug_mode == 6)
    {
        float transparent_coc = coc_data.b;
        frag_color = coc_data.a <= 0.0 ? vec4(0.0, 0.0, 0.0, 1.0) :
            (transparent_coc < 0.0 ? vec4(-transparent_coc, 0.0, 0.0, 1.0) :
                                     vec4(vec3(transparent_coc), 1.0));
        return;
    }
    if (debug_mode == 7)
    {
        frag_color = opaque_coc < 0.0 ? vec4(-opaque_coc, 0.0, 0.0, 1.0)
                                      : vec4(vec3(opaque_coc), 1.0);
        return;
    }
    if (debug_mode == 8)
    {
        vec4 layer = has_transparent_depth != 0 ?
            texture(emissiveRect, uv) : vec4(0.0);
        frag_color = vec4(layer.a > 0.0001 ? layer.rgb / layer.a : vec3(0.0), 1.0);
        return;
    }
    if (debug_mode == 9)
    {
        vec4 layer = has_transparent_depth != 0 ?
            texture(positionMap, uv) : vec4(0.0);
        frag_color = vec4(layer.a > 0.0001 ? layer.rgb / layer.a : vec3(0.0), 1.0);
        return;
    }

    // Opaque background stays blurred behind in-focus transparent strands.
    // The effective CoC is useful for diagnostics, not opaque compositing.
    float pixel_blur = abs(opaque_coc) * max_radius;
    float far_blend = opaque_coc > 0.0 ? smoothstep(0.5, 2.0, pixel_blur) : 0.0;

    // Large discs undersample with a bounded gather budget. Neighboring
    // reduced-resolution pixels use decorrelated aperture phases, so a small
    // tent reconstruction removes coherent grids without softening low-radius
    // detail or changing the foreground layer.
    if (opaque_coc > 0.0 && pixel_blur > 6.0)
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

    vec3 resolved_far = far_color.a > 0.0001 ? far_color.rgb / far_color.a : opaque_source;
    vec3 color = mix(opaque_source, resolved_far, far_blend);
    float near_pixel_blur = max(-opaque_coc, 0.0) * near_max_radius;
    // The near gather guarantees full ownership of source foreground pixels
    // only once their blur reaches one pixel. Revealing synthesized background
    // before then causes camera-dependent seams near the focal plane.
    if (near_pixel_blur > 1.0 && far_color.a > 0.0001)
    {
        vec3 background_fill = far_color.rgb / far_color.a;
        color = mix(color, background_fill, far_color.a);
    }
    color = color * (1.0 - near_color.a) + near_color.rgb;

    if (has_transparent_depth != 0)
    {
        vec4 transparent_far = texture(emissiveRect, uv);
        vec4 transparent_near = texture(positionMap, uv);
        vec4 raw_layer = transparent_surface;
        float transparent_coc = coc_data.b;
        float transparent_radius = transparent_coc < 0.0 ?
            -transparent_coc * near_max_radius :
             transparent_coc * max_radius;
        float transparent_blend = smoothstep(0.5, 2.0, transparent_radius);
        vec4 transparent_layer;
        if (transparent_surface.a > 0.0001)
        {
            vec4 blurred_layer = transparent_coc < 0.0 ?
                transparent_near : transparent_far;
            transparent_layer = mix(raw_layer, blurred_layer,
                                    transparent_blend);
        }
        else
        {
            // Near coverage is in front of far coverage when independently
            // blurred transparent silhouettes overlap at this pixel.
            transparent_layer = transparent_near +
                transparent_far * (1.0 - transparent_near.a);
        }
        transparent_layer.a = clamp(transparent_layer.a, 0.0, 1.0);
        color = transparent_layer.rgb + color * (1.0 - transparent_layer.a);
        if (transparent_surface.a > 0.0001)
        {
            // Keep the selected alpha compositor exact when neither layer
            // needs defocus. Unoccupied pixels must still admit bokeh spread.
            float visible_blur = max(transparent_blend,
                                     max(far_blend, near_color.a));
            color = mix(source.rgb, color, clamp(visible_blur, 0.0, 1.0));
        }
    }

    frag_color = vec4(color, source.a);
}
