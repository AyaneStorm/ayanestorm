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
// Near front layer (bloomMap holds the back layer) and the background
// completion (rgb, signed CoC; asDepthOfFieldBackgroundF.glsl), under free
// reserved names. With these the resolve uses 16 samplers, the OpenGL 4.1
// minimum.
uniform sampler2D exposureMap;
uniform sampler2D brdfLut;
uniform float max_radius;
uniform float near_max_radius;
uniform int debug_mode;
uniform int has_transparent_depth;
uniform int has_layers;
// 1 when asDepthOfFieldPostfilterF.glsl already smoothed every gathered
// layer: the fixed 3x3 tents below are then skipped.
uniform int postfiltered;

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

// What a transparent stratum shows at this pixel without its near spread:
// its own content in focus, its far blur, or, where the surface itself is a
// defocused foreground, behind in its place. The near spreads (opaque and
// transparent) are composited over every stratum's base afterwards: a
// defocused foreground veil lies in front of in-focus content whichever
// layer that content was captured in. Before, the opaque veil went under the
// transparent strata, so in-focus alpha hair painted over the blur of the
// alpha-masked strands in front of it (strand-shaped holes, dark strokes),
// and an in-focus pixel kept only its raw content, cutting off transparent
// spreads. The near gathers leave sources under 0.5-2 px of blur to raw, at
// full resolution (spreadShare() in asDepthOfFieldTransparentF.glsl, same
// ramp).
//
// Base of a pixel whose surface is a defocused foreground: its sharp share
// (1 - spread) plus behind, under a veil of coverage veil. Behind fills only
// what neither covers, max(spread - veil, 0): a solid surface's own veil
// covers about spread, so behind must vanish there. The plain mix
// self (1 - s) + behind s leaked behind through at s (1 - s) (up to 25 %):
// hair filled in behind a nearly focused arm showed through it.
vec4 exclusiveBase(vec4 self_color, vec4 behind, float spread, float veil)
{
    veil = clamp(veil, 0.0, 1.0);
    if (veil >= spread || veil > 0.999)
    {
        return self_color;
    }
    return (self_color * (1.0 - spread) + behind * (spread - veil)) / (1.0 - veil);
}

// Weight of a fill placed under a veil, exclusive in the same way.
float exclusiveFill(float spread, float veil)
{
    veil = clamp(veil, 0.0, 1.0);
    return veil >= spread || veil > 0.999 ? 0.0 : (spread - veil) / (1.0 - veil);
}

// Without a surface of this stratum here, only its far spread remains. That
// comes from surfaces behind the focal plane, so an opaque surface here that
// is in focus or in front of it hides it (far_visibility: the opaque far
// blend, as for the opaque plate). Before, a ponytail's blur spread over a
// nearly focused arm in front of it.
vec4 stratumBase(vec4 raw, float coverage, vec4 far_layer, float coc,
                 float blend, vec4 behind, float veil, float far_visibility)
{
    if (coverage <= 0.0001)
    {
        return far_layer * far_visibility;
    }
    return coc < 0.0 ? exclusiveBase(raw, behind, blend, veil)
                     : mix(raw, far_layer, blend);
}

vec3 over(vec4 front, vec3 back)
{
    return front.rgb + back * (1.0 - clamp(front.a, 0.0, 1.0));
}

// The behind fills below read rings at 2, 4 ... 64 px. Each ring weighs a
// quarter of the previous one, so the nearest known content dominates but
// the fill changes continuously between pixels. They used to stop at the
// first ring with enough taps: neighbouring pixels stopping at different
// rings copied different distant content, which drew hard-edged patches of
// hair texture once blurs grew past the 10 px default (radius x4).
float ringWeight(int ring)
{
    return exp2(-2.0 * float(ring));
}

// Stop once the rings left could change the fill by under 8 %: their total
// weight is at most 8 taps x 4^-j summed over the remaining j.
bool ringsDone(int ring, float weight_sum)
{
    float remaining = 8.0 * exp2(-2.0 * float(ring + 1)) * (4.0 / 3.0);
    return weight_sum > 0.0 && remaining <= 0.08 * weight_sum;
}

// Opaque content behind a defocused opaque foreground pixel: nearby pixels
// blurred at least ~2 px less, i.e. farther surfaces, whether background or
// a nearer-to-focus foreground. The background completion counts only
// non-foreground pixels: under a strand over a cheek that is itself slightly
// in front of focus it found none nearby and fell back to the sharp strand
// (or reached the sky past the head), which then showed through the thin
// veil as sharp dark strokes. Rings of 8 full-resolution taps at 2 to 64 px
// (see ringWeight()); background neighbours contribute their blurred plate,
// as the base does there. Alpha: how much was found (at least two taps'
// worth gives 1).
vec4 opaqueBehind(vec2 uv, float center_radius)
{
    ivec2 size = textureSize(diffuseRect, 0);
    vec2 center = uv * vec2(size);
    vec3 sum = vec3(0.0);
    float weight_sum = 0.0;
    float found = 0.0;
    float radius = 2.0;
    for (int ring = 0; ring < 6; ++ring)
    {
        for (int k = 0; k < 8; ++k)
        {
            float angle = (float(k) + 0.5 * float(ring & 1)) * 0.78539816339;
            ivec2 p = clamp(ivec2(center + vec2(cos(angle), sin(angle)) * radius),
                            ivec2(0), size - 1);
            float coc = texelFetch(noiseMap, p, 0).g;
            float weight = smoothstep(0.5, 2.0, center_radius -
                                      max(-coc, 0.0) * near_max_radius);
            if (weight <= 0.0)
            {
                continue;
            }
            vec3 color = texelFetch(diffuseRect, p, 0).rgb;
            if (coc > 0.0)
            {
                vec4 plate = texture(lightMap, (vec2(p) + 0.5) / vec2(size));
                if (plate.a > 0.0001)
                {
                    color = mix(color, plate.rgb / plate.a,
                                smoothstep(0.5, 2.0, coc * max_radius));
                }
            }
            sum += color * weight * ringWeight(ring);
            weight_sum += weight * ringWeight(ring);
            found += weight;
        }
        if (ringsDone(ring, weight_sum))
        {
            break;
        }
        radius *= 2.0;
    }
    return vec4(weight_sum > 0.0 ? sum / weight_sum : vec3(0.0),
                clamp(found * 0.5, 0.0, 1.0));
}

// In-focus rigged content behind a defocused foreground pixel: either the
// rigged surface itself is the foreground (the replay keeps one depth per
// pixel, so a strand over an alpha-blended face carries the face with the
// strand's blur), or an opaque foreground (alpha-masked lock strands) hides
// it and its coverage was zeroed. Both are unknown, not empty: under a thin
// veil a hole there showed the face where the neighbours show hair
// (jagged strand-shaped strokes). Rings of 8 full-resolution taps at 2 to
// 64 px (see ringWeight()); the nearest known pixels dominate. Known is
// relative, as in opaqueBehind(): a front surface (rigged if present, else
// opaque) blurred at least ~2 px less than this pixel's, so a cheek slightly
// in front of focus still counts behind a strand.
vec4 riggedBehind(vec2 uv, float center_radius)
{
    ivec2 size = textureSize(shadowMap3, 0);
    vec2 center = uv * vec2(size);
    vec4 sum = vec4(0.0);
    float weight_sum = 0.0;
    float radius = 2.0;
    for (int ring = 0; ring < 6; ++ring)
    {
        for (int k = 0; k < 8; ++k)
        {
            float angle = (float(k) + 0.5 * float(ring & 1)) * 0.78539816339;
            ivec2 p = clamp(ivec2(center + vec2(cos(angle), sin(angle)) * radius),
                            ivec2(0), size - 1);
            vec4 layers = texelFetch(shadowMap0, p, 0);
            float present = layers.b > 0.0001 ? 1.0 : 0.0;
            float front_coc = present > 0.0 ? layers.r : texelFetch(noiseMap, p, 0).g;
            float focus = smoothstep(0.5, 2.0, center_radius -
                max(-front_coc, 0.0) * near_max_radius);
            if (focus <= 0.0)
            {
                continue;
            }
            float weight = focus * ringWeight(ring);
            sum += weight * present * vec4(texelFetch(shadowMap3, p, 0).rgb, layers.b);
            weight_sum += weight;
        }
        if (ringsDone(ring, weight_sum))
        {
            break;
        }
        radius *= 2.0;
    }
    return weight_sum > 0.0 ? sum / weight_sum : vec4(0.0);
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
    vec4 near_back = texture(bloomMap, uv);
    vec4 near_front = texture(exposureMap, uv);

    // Near data is premultiplied. Reconstruct neighboring independently phased
    // gathers to turn sparse coverage into a stable fractional silhouette.
    if (postfiltered == 0 && near_max_radius > 6.0)
    {
        float amount = smoothstep(6.0, 18.0, near_max_radius);
        near_back = mix(near_back,
                        reconstructTransparent(bloomMap, uv, near_max_radius),
                        amount);
        near_front = mix(near_front,
                         reconstructTransparent(exposureMap, uv, near_max_radius),
                         amount);
    }
    // Both layers as one premultiplied layer. Color: front over back, so a
    // nearer foreground occludes a farther one. Coverage: their clamped sum,
    // the single-layer estimate. Over-compositing alone would lose coverage
    // where one surface is split across both layers (b = f = 0.5 gives 0.75)
    // and expose the background plate through it. The rescale is at most
    // 4/3 and only where both layers are partial.
    vec4 near_over = near_front + near_back * (1.0 - near_front.a);
    float near_alpha = min(near_back.a + near_front.a, 1.0);
    vec4 near_color = vec4(near_over.rgb * (near_over.a > 0.0001 ?
                                            near_alpha / near_over.a : 1.0),
                           near_alpha);

    // Debug views write zero glow (alpha): glow 1 bloomed the whole frame.
    if (debug_mode == 1)
    {
        frag_color = coc < 0.0 ? vec4(-coc, 0.0, 0.0, 0.0)
                               : vec4(vec3(coc), 0.0);
        return;
    }
    if (debug_mode == 2)
    {
        frag_color = vec4(vec3(near_color.a), 0.0);
        return;
    }
    if (debug_mode == 3)
    {
        vec3 debug_far = far_color.a > 0.0001 ? far_color.rgb / far_color.a : vec3(0.0);
        frag_color = vec4(debug_far, 0.0);
        return;
    }
    if (debug_mode == 4)
    {
        vec3 debug_near = near_color.a > 0.0001 ? near_color.rgb / near_color.a : vec3(0.0);
        frag_color = vec4(debug_near, 0.0);
        return;
    }
    if (debug_mode == 5)
    {
        frag_color = vec4(vec3(coc_data.a), 0.0);
        return;
    }
    if (debug_mode == 6)
    {
        float transparent_coc = coc_data.b;
        frag_color = coc_data.a <= 0.0 ? vec4(0.0, 0.0, 0.0, 0.0) :
            (transparent_coc < 0.0 ? vec4(-transparent_coc, 0.0, 0.0, 0.0) :
                                     vec4(vec3(transparent_coc), 0.0));
        return;
    }
    if (debug_mode == 7)
    {
        frag_color = opaque_coc < 0.0 ? vec4(-opaque_coc, 0.0, 0.0, 0.0)
                                      : vec4(vec3(opaque_coc), 0.0);
        return;
    }
    if (debug_mode == 8)
    {
        vec4 layer = has_transparent_depth != 0 ?
            texture(emissiveRect, uv) : vec4(0.0);
        frag_color = vec4(layer.a > 0.0001 ? layer.rgb / layer.a : vec3(0.0), 0.0);
        return;
    }
    if (debug_mode == 9)
    {
        vec4 layer = has_transparent_depth != 0 ?
            texture(positionMap, uv) : vec4(0.0);
        frag_color = vec4(layer.a > 0.0001 ? layer.rgb / layer.a : vec3(0.0), 0.0);
        return;
    }
    if (debug_mode >= 10 && debug_mode <= 15)
    {
        vec4 layers = has_layers != 0 ? texture(shadowMap0, uv) : vec4(0.0);
        if (debug_mode == 10 || debug_mode == 11)
        {
            float value = debug_mode == 10 ? layers.r : layers.g;
            frag_color = value < 0.0 ? vec4(-value, 0.0, 0.0, 0.0)
                                     : vec4(vec3(value), 0.0);
        }
        else if (debug_mode == 12 || debug_mode == 13)
        {
            frag_color = vec4(vec3(debug_mode == 12 ? layers.b : layers.a), 0.0);
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
                             vec3(0.0), 0.0);
        }
        return;
    }
    if (debug_mode == 25)
    {
        // Radius spread sqrt(var) / R written by the postfilter into the far
        // and near layers in this mode (grey: mixed blur radii); near where
        // covered, far elsewhere.
        vec3 spread = near_color.a > 0.0001 ? near_color.rgb / near_color.a :
                      (far_color.a > 0.0001 ? far_color.rgb / far_color.a : vec3(0.0));
        frag_color = vec4(spread, 0.0);
        return;
    }
    if (debug_mode == 16)
    {
        // Background completion (unblurred): what the resolve puts behind a
        // defocused foreground pixel. The sprite-energy view it replaced
        // needed this sampler slot.
        frag_color = vec4(texture(brdfLut, uv).rgb, 0.0);
        return;
    }
    if (debug_mode == 17 || debug_mode == 18)
    {
        vec4 layer = debug_mode == 17 ? near_back : near_front;
        frag_color = vec4(layer.a > 0.0001 ? layer.rgb / layer.a : vec3(0.0), 0.0);
        return;
    }
    if (debug_mode == 20 || debug_mode == 21)
    {
        // Spread coverage of the rigged / world near transparent gathers
        // (15 and 9 show their color divided by it).
        vec4 layer = debug_mode == 20 ?
            (has_layers != 0 ? texture(shadowMap2, uv) : vec4(0.0)) :
            (has_transparent_depth != 0 ? texture(positionMap, uv) : vec4(0.0));
        frag_color = vec4(vec3(layer.a), 0.0);
        return;
    }
    if (debug_mode == 19)
    {
        // Blurred background completion under foreground pixels; the rest
        // of the image in grey at its own luminance. Dimming it would make
        // auto-exposure brighten the whole view until the plate burns out.
        float context = dot(opaque_source, vec3(0.2126, 0.7152, 0.0722));
        frag_color = vec4(opaque_coc < 0.0 && far_color.a > 0.0001 ?
                          far_color.rgb / far_color.a : vec3(context), 0.0);
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
    if (postfiltered == 0 && opaque_coc > 0.0 && pixel_blur > 6.0)
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
    // A defocused foreground pixel reaches the image only through the near
    // layers; underneath them lies what is behind it, never the pixel
    // itself. The far target alone cannot supply that: it classifies
    // foreground at gather resolution, where a strand 1-2 px wide blends
    // with its surroundings and gets no plate (alpha 0), so the sharp strand
    // stayed as the base and showed through its partial near coverage (dark
    // flecks on defocused hair). The completion covers every foreground
    // pixel; the far target's blurred plate replaces it where the
    // background behind is itself defocused. Between 0.5 and 2 px of blur
    // the pixel fades from its sharp self to that, the complement of the
    // near gather's spreadShare() (no seams at the focal plane).
    if (near_pixel_blur > 0.5)
    {
        vec4 completion = texture(brdfLut, uv);
        vec3 behind = completion.rgb;
        if (far_color.a > 0.0001)
        {
            float behind_blur = smoothstep(0.5, 2.0, max(completion.a, 0.0) * max_radius);
            behind = mix(behind, far_color.rgb / far_color.a, far_color.a * behind_blur);
        }
        // Nearby farther surfaces first; the completion only where none
        // lies within reach (see opaqueBehind()).
        vec4 local_behind = opaqueBehind(uv, near_pixel_blur);
        behind = mix(behind, local_behind.rgb, local_behind.a);
        color = exclusiveBase(vec4(color, 1.0), vec4(behind, 1.0),
                              smoothstep(0.5, 2.0, near_pixel_blur),
                              near_color.a).rgb;
    }
    // Stage views for locating artifacts: 22 opaque result before the
    // transparent layers, 23 rigged layer as composited (premultiplied, over
    // black), 24 final blend toward the blurred result (white) versus the
    // sharp compositor output (black). 23 and 24 need the layered path.
    if (debug_mode == 22)
    {
        frag_color = vec4(over(near_color, color), 0.0);
        return;
    }
    if (has_transparent_depth == 0)
    {
        color = over(near_color, color);
    }

    if (has_transparent_depth != 0)
    {
        vec4 transparent_far = texture(emissiveRect, uv);
        vec4 transparent_near = texture(positionMap, uv);
        // Reconstruct sparse aperture samples in premultiplied space so
        // foreground hair and transparent light discs retain smooth coverage.
        if (postfiltered == 0 && max_radius > 6.0)
        {
            transparent_far = mix(transparent_far,
                                  reconstructTransparent(emissiveRect, uv,
                                                         max_radius),
                                  smoothstep(6.0, 18.0, max_radius));
        }
        if (postfiltered == 0 && near_max_radius > 6.0)
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
            if (postfiltered == 0 && max_radius > 6.0)
            {
                rigged_far = mix(rigged_far,
                                 reconstructTransparent(shadowMap1, uv,
                                                        max_radius),
                                 smoothstep(6.0, 18.0, max_radius));
            }
            if (postfiltered == 0 && near_max_radius > 6.0)
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
            bool rigged_present = layers.b > 0.0001;
            // Rigged content hidden by a defocused opaque foreground.
            float hidden_blend = rigged_present ? 0.0 :
                smoothstep(0.5, 2.0, near_pixel_blur);
            vec4 rigged_behind = (rigged_present && layers.r < 0.0 &&
                                  rigged_blend > 0.0) || hidden_blend > 0.0 ?
                riggedBehind(uv, rigged_present ? rigged_radius : near_pixel_blur) :
                vec4(0.0);
            // Every veil composited over the strata bases below.
            float veil = 1.0 - (1.0 - clamp(near_color.a, 0.0, 1.0)) *
                               (1.0 - clamp(transparent_near.a, 0.0, 1.0)) *
                               (1.0 - clamp(rigged_near.a, 0.0, 1.0));
            vec4 world_base = stratumBase(world_raw, layers.a, transparent_far,
                                          layers.g, world_blend, vec4(0.0), veil,
                                          far_blend);
            vec4 rigged_base = stratumBase(rigged_raw, layers.b, rigged_far,
                                           layers.r, rigged_blend, rigged_behind, veil,
                                           far_blend);
            rigged_base = mix(rigged_base, rigged_behind,
                              exclusiveFill(hidden_blend, veil));
            if (debug_mode == 23)
            {
                frag_color = vec4(over(rigged_near, rigged_base.rgb), 0.0);
                return;
            }
            // Bases in depth order, then the foreground veils over all of them.
            if (!world_in_front)
            {
                color = over(world_base, color);
                color = over(rigged_base, color);
                color = over(near_color, color);
                color = over(transparent_near, color);
                color = over(rigged_near, color);
            }
            else
            {
                color = over(rigged_base, color);
                color = over(world_base, color);
                color = over(near_color, color);
                color = over(rigged_near, color);
                color = over(transparent_near, color);
            }
            if (transparent_surface.a > 0.0001)
            {
                // Includes the transparent near spread: an in-focus pixel
                // under a defocused strand's veil is no longer sharp.
                float visible_blur = max(max(world_blend, rigged_blend),
                                         max(far_blend, near_color.a));
                visible_blur = max(visible_blur,
                                   max(rigged_near.a, transparent_near.a));
                color = mix(source.rgb, color,
                            clamp(visible_blur, 0.0, 1.0));
                if (debug_mode == 24)
                {
                    frag_color = vec4(vec3(clamp(visible_blur, 0.0, 1.0)), 0.0);
                    return;
                }
            }
            else if (debug_mode == 24)
            {
                frag_color = vec4(1.0, 1.0, 1.0, 0.0);
                return;
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
        // Base, then the foreground veils (see stratumBase()).
        vec4 transparent_base = stratumBase(raw_layer, transparent_surface.a,
                                            transparent_far, transparent_coc,
                                            transparent_blend, vec4(0.0),
                                            1.0 - (1.0 - clamp(near_color.a, 0.0, 1.0)) *
                                                  (1.0 - clamp(transparent_near.a, 0.0, 1.0)),
                                            far_blend);
        color = over(transparent_base, color);
        color = over(near_color, color);
        color = over(transparent_near, color);
        if (transparent_surface.a > 0.0001)
        {
            // Keep the selected alpha compositor exact when neither layer
            // needs defocus. Unoccupied pixels must still admit bokeh spread.
            float visible_blur = max(max(transparent_blend, transparent_near.a),
                                     max(far_blend, near_color.a));
            color = mix(source.rgb, color, clamp(visible_blur, 0.0, 1.0));
        }
    }

    frag_color = vec4(color, source.a);
}
