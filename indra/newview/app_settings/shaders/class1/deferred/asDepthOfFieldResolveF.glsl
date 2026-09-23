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
uniform sampler2D shadowMap0;
uniform sampler2D shadowMap1;
uniform sampler2D shadowMap2;
uniform sampler2D shadowMap3;
uniform sampler2D shadowMap4;
uniform sampler2D shadowMap5;
uniform float max_radius;
uniform float near_max_radius;
uniform int debug_mode;
uniform int has_transparent_depth;
uniform int has_layers;

in vec2 vary_fragcoord;

vec4 reconstructTransparent(sampler2D layer, vec2 uv, float radius)
{
    vec2 texel = 1.0 / vec2(textureSize(layer, 0));
    vec2 step_uv = texel * mix(1.0, 1.5,
                               smoothstep(12.0, 32.0, radius));
    vec2 lo = texel * 0.5;
    vec2 hi = vec2(1.0) - lo;
    vec4 sum = texture(layer, uv) * 4.0;
    sum += 2.0 * texture(layer, clamp(uv + step_uv * vec2(-1.0,  0.0), lo, hi));
    sum += 2.0 * texture(layer, clamp(uv + step_uv * vec2( 1.0,  0.0), lo, hi));
    sum += 2.0 * texture(layer, clamp(uv + step_uv * vec2( 0.0, -1.0), lo, hi));
    sum += 2.0 * texture(layer, clamp(uv + step_uv * vec2( 0.0,  1.0), lo, hi));
    sum += texture(layer, clamp(uv + step_uv * vec2(-1.0, -1.0), lo, hi));
    sum += texture(layer, clamp(uv + step_uv * vec2( 1.0, -1.0), lo, hi));
    sum += texture(layer, clamp(uv + step_uv * vec2(-1.0,  1.0), lo, hi));
    sum += texture(layer, clamp(uv + step_uv * vec2( 1.0,  1.0), lo, hi));
    return sum * 0.0625;
}

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
        vec2 step_uv = texel * mix(1.0, 1.5,
                                   smoothstep(12.0, 32.0, near_max_radius));
        vec2 lo = texel * 0.5;
        vec2 hi = vec2(1.0) - lo;
        vec4 reconstructed = near_color * 4.0;
        reconstructed += 2.0 * texture(bloomMap, clamp(uv + step_uv * vec2(-1.0,  0.0), lo, hi));
        reconstructed += 2.0 * texture(bloomMap, clamp(uv + step_uv * vec2( 1.0,  0.0), lo, hi));
        reconstructed += 2.0 * texture(bloomMap, clamp(uv + step_uv * vec2( 0.0, -1.0), lo, hi));
        reconstructed += 2.0 * texture(bloomMap, clamp(uv + step_uv * vec2( 0.0,  1.0), lo, hi));
        reconstructed += texture(bloomMap, clamp(uv + step_uv * vec2(-1.0, -1.0), lo, hi));
        reconstructed += texture(bloomMap, clamp(uv + step_uv * vec2( 1.0, -1.0), lo, hi));
        reconstructed += texture(bloomMap, clamp(uv + step_uv * vec2(-1.0,  1.0), lo, hi));
        reconstructed += texture(bloomMap, clamp(uv + step_uv * vec2( 1.0,  1.0), lo, hi));
        reconstructed *= 0.0625;
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
    if (debug_mode >= 10 && debug_mode <= 15)
    {
        vec4 layers = has_layers != 0 ? texture(shadowMap0, uv) : vec4(0.0);
        if (debug_mode == 10 || debug_mode == 11)
        {
            float value = debug_mode == 10 ? layers.r : layers.g;
            frag_color = value < 0.0 ? vec4(-value, 0.0, 0.0, 1.0)
                                     : vec4(vec3(value), 1.0);
        }
        else if (debug_mode == 12 || debug_mode == 13)
        {
            frag_color = vec4(vec3(debug_mode == 12 ? layers.b : layers.a), 1.0);
        }
        else
        {
            vec4 layer = vec4(0.0);
            if (has_layers != 0)
            {
                layer = debug_mode == 14 ? texture(shadowMap1, uv) :
                                           texture(shadowMap2, uv);
            }
            frag_color = vec4(layer.a > 0.0001 ? layer.rgb / layer.a :
                             vec3(0.0), 1.0);
        }
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
        vec2 step_uv = texel * mix(1.0, 1.5,
                                   smoothstep(12.0, 32.0, pixel_blur));
        vec2 lo = texel * 0.5;
        vec2 hi = vec2(1.0) - lo;
        vec4 reconstructed = far_color * 4.0;
        reconstructed += 2.0 * texture(lightMap, clamp(uv + step_uv * vec2(-1.0,  0.0), lo, hi));
        reconstructed += 2.0 * texture(lightMap, clamp(uv + step_uv * vec2( 1.0,  0.0), lo, hi));
        reconstructed += 2.0 * texture(lightMap, clamp(uv + step_uv * vec2( 0.0, -1.0), lo, hi));
        reconstructed += 2.0 * texture(lightMap, clamp(uv + step_uv * vec2( 0.0,  1.0), lo, hi));
        reconstructed += texture(lightMap, clamp(uv + step_uv * vec2(-1.0, -1.0), lo, hi));
        reconstructed += texture(lightMap, clamp(uv + step_uv * vec2( 1.0, -1.0), lo, hi));
        reconstructed += texture(lightMap, clamp(uv + step_uv * vec2(-1.0,  1.0), lo, hi));
        reconstructed += texture(lightMap, clamp(uv + step_uv * vec2( 1.0,  1.0), lo, hi));
        reconstructed *= 0.0625;
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
        // Reconstruct sparse aperture samples in premultiplied space so
        // foreground hair and transparent light discs retain smooth coverage.
        if (max_radius > 6.0)
        {
            transparent_far = mix(transparent_far,
                                  reconstructTransparent(emissiveRect, uv,
                                                         max_radius),
                                  smoothstep(6.0, 18.0, max_radius));
        }
        if (near_max_radius > 6.0)
        {
            transparent_near = mix(transparent_near,
                                   reconstructTransparent(positionMap, uv,
                                                          near_max_radius),
                                   smoothstep(6.0, 18.0, near_max_radius));
        }
        if (has_layers != 0)
        {
            vec4 layers = texture(shadowMap0, uv);
            vec4 rigged_raw = texture(shadowMap3, uv);
            rigged_raw.a = layers.b;
            vec4 combined_raw = vec4(
                source.rgb - opaque_source * (1.0 - transparent_surface.a),
                transparent_surface.a);
            float rigged_transmission = 1.0 - layers.b;
            vec4 world_raw = vec4(0.0);
            bool world_in_front = texture(shadowMap4, uv).r >
                                  texture(shadowMap5, uv).r;
            if (layers.a > 0.0001 && world_in_front)
            {
                world_raw = vec4(combined_raw.rgb - rigged_raw.rgb *
                                 (1.0 - layers.a), layers.a);
            }
            else if (layers.a > 0.0001 && rigged_transmission > 0.05)
            {
                world_raw = vec4((combined_raw.rgb - rigged_raw.rgb) /
                                     rigged_transmission, layers.a);
            }
            vec4 rigged_far = texture(shadowMap1, uv);
            vec4 rigged_near = texture(shadowMap2, uv);
            if (max_radius > 6.0)
            {
                rigged_far = mix(rigged_far,
                                 reconstructTransparent(shadowMap1, uv,
                                                        max_radius),
                                 smoothstep(6.0, 18.0, max_radius));
            }
            if (near_max_radius > 6.0)
            {
                rigged_near = mix(rigged_near,
                                  reconstructTransparent(shadowMap2, uv,
                                                         near_max_radius),
                                  smoothstep(6.0, 18.0, near_max_radius));
            }
            float world_radius = layers.g < 0.0 ?
                -layers.g * near_max_radius : layers.g * max_radius;
            float rigged_radius = layers.r < 0.0 ?
                -layers.r * near_max_radius : layers.r * max_radius;
            float world_blend = smoothstep(0.5, 2.0, world_radius);
            float rigged_blend = smoothstep(0.5, 2.0, rigged_radius);
            vec4 world_layer = layers.a > 0.0001 ?
                mix(world_raw, layers.g < 0.0 ?
                     transparent_near : transparent_far, world_blend) :
                transparent_near +
                transparent_far * (1.0 - transparent_near.a);
            vec4 rigged_layer = layers.b > 0.0001 ?
                mix(rigged_raw, layers.r < 0.0 ?
                     rigged_near : rigged_far, rigged_blend) :
                rigged_near + rigged_far * (1.0 - rigged_near.a);
            world_layer.a = clamp(world_layer.a, 0.0, 1.0);
            rigged_layer.a = clamp(rigged_layer.a, 0.0, 1.0);
            if (!world_in_front)
            {
                color = world_layer.rgb + color * (1.0 - world_layer.a);
                color = rigged_layer.rgb + color * (1.0 - rigged_layer.a);
            }
            else
            {
                color = rigged_layer.rgb + color * (1.0 - rigged_layer.a);
                color = world_layer.rgb + color * (1.0 - world_layer.a);
            }
            if (transparent_surface.a > 0.0001)
            {
                float visible_blur = max(max(world_blend, rigged_blend),
                                         max(far_blend, near_color.a));
                color = mix(source.rgb, color,
                            clamp(visible_blur, 0.0, 1.0));
            }
            frag_color = vec4(color, source.a);
            return;
        }
        // Reconstruct from the actual alpha-mode resolve, not the separately
        // ordered replay. All inputs are linear HDR at this stage.
        vec4 raw_layer = vec4(
            source.rgb - opaque_source * (1.0 - transparent_surface.a),
            transparent_surface.a);
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
