/**
 * @file asDepthOfFieldCoCF.glsl
 * @author chanayane@firestorm
 * @brief Signed physical circle-of-confusion preparation for AyaneStorm DoF.
 */
// Effective, opaque, transparent CoC and transparent coverage. Keeping the
// components separate allows later passes to evolve without recapturing the
// scene and makes transparent-depth failures directly diagnosable.
layout(location = 0) out vec4 frag_coc;
layout(location = 1) out vec4 frag_layers;

uniform sampler2D depthMap;
// Viewer-reserved slots carry the private transparent coverage and depth.
uniform sampler2D noiseMap;
uniform sampler2D lightMap;
uniform sampler2D bloomMap;
uniform sampler2D positionMap;
uniform sampler2D emissiveRect;
uniform mat4 inv_proj;
uniform float focal_distance;
uniform float blur_constant;
uniform float tan_pixel_angle;
uniform float magnification;
uniform float max_coc;
uniform int has_transparent_depth;
uniform int has_layers;
// Off-axis focus (asdepthoffield.cpp, lens field), in normalized CoC at the
// frame corner; the field position is (uv - 0.5) * field_scale, length 1 at
// the corner, so both terms grow with field^2.
uniform vec2 field_scale;
// Field curvature: the in-focus surface bends toward the camera (> 0, as in
// most lenses) or away from it (< 0), a signed shift of every CoC.
uniform float field_curvature;
// Astigmatism: the radial and circumferential image axes focus apart; the
// blur magnitude grows by the split (the gathers stretch the aperture).
uniform float astigmatism;

in vec2 vary_fragcoord;

float calculateCoC(float depth)
{
    float coc = (depth - focal_distance) / -depth * blur_constant;
    coc /= magnification;
    float pixel_length = tan_pixel_angle * -focal_distance;
    coc = coc / pixel_length;
    return coc * 1.41421356237;
}

// Normalized signed CoC (negative foreground, positive background) with the
// off-axis focus terms. Past the focal lines the larger axis blur is kept:
// |coc| + split; at the focal plane the sign is positive (far plane).
float normalizedCoC(float depth)
{
    float coc = -calculateCoC(depth) / max(max_coc, 0.0001);
    vec2 field = (vary_fragcoord - 0.5) * field_scale;
    float field2 = dot(field, field);
    coc += field_curvature * field2;
    float split = abs(astigmatism) * field2;
    if (split > 0.0)
    {
        coc = coc < 0.0 ? coc - split : coc + split;
    }
    return clamp(coc, -1.0, 1.0);
}

void main()
{
    float device_depth = texture(depthMap, vary_fragcoord).r;
    float ndc_depth = device_depth * 2.0 - 1.0;
    vec4 view_position = inv_proj * vec4(0.0, 0.0, ndc_depth, 1.0);
    float view_depth = view_position.z / view_position.w;
    // Firestorm's camera-space formula is positive in front of focus and
    // negative behind it. Store the module's explicit convention instead:
    // negative foreground, positive background.
    float opaque_coc = normalizedCoC(view_depth);
    float effective_coc = opaque_coc;
    float transparent_coc = opaque_coc;
    float transparent_coverage = 0.0;

    if (has_transparent_depth != 0)
    {
        transparent_coverage = texture(noiseMap, vary_fragcoord).a;
        float transparent_device_depth = texture(lightMap, vary_fragcoord).r;
        // Ignore cleared pixels and transparent fragments hidden by opaque
        // geometry. Coverage smoothly controls how much the visible color is
        // associated with the transparent surface rather than what lies behind.
        if (transparent_coverage > 0.0 && transparent_device_depth < device_depth - 0.0000001)
        {
            float transparent_ndc_depth = transparent_device_depth * 2.0 - 1.0;
            vec4 transparent_position = inv_proj * vec4(0.0, 0.0, transparent_ndc_depth, 1.0);
            float transparent_view_depth = transparent_position.z / transparent_position.w;
            transparent_coc = normalizedCoC(transparent_view_depth);
            float influence = smoothstep(0.05, 0.65, transparent_coverage);
            effective_coc = mix(opaque_coc, transparent_coc, influence);
        }
        else
        {
            transparent_coverage = 0.0;
        }
    }

    frag_coc = vec4(effective_coc, opaque_coc,
                    transparent_coc, transparent_coverage);

    frag_layers = vec4(0.0);
    if (has_layers != 0)
    {
        float rigged_coverage = clamp(texture(bloomMap, vary_fragcoord).a,
                                      0.0, 1.0);
        float world_coverage = rigged_coverage < 0.99 ? clamp(
            (transparent_coverage - rigged_coverage) /
            (1.0 - rigged_coverage), 0.0, 1.0) : 0.0;
        float rigged_coc = opaque_coc;
        float world_coc = opaque_coc;
        float rigged_depth = texture(positionMap, vary_fragcoord).r;
        float world_depth = texture(emissiveRect, vary_fragcoord).r;
        // Under near-opaque rigged coverage the world share cannot be
        // separated from the combined coverage. When the world layer is in
        // front (window shades over hair), it owns the whole combined
        // contribution; dropping it would expose the rigged layer behind it.
        if (rigged_coverage >= 0.99 && world_depth < rigged_depth)
        {
            world_coverage = transparent_coverage;
        }
        if (rigged_coverage > 0.0 && rigged_depth < device_depth - 0.0000001)
        {
            vec4 p = inv_proj * vec4(0.0, 0.0,
                                    rigged_depth * 2.0 - 1.0, 1.0);
            rigged_coc = normalizedCoC(p.z / p.w);
        }
        else
        {
            rigged_coverage = 0.0;
        }
        if (world_coverage > 0.0 && world_depth < device_depth - 0.0000001)
        {
            vec4 p = inv_proj * vec4(0.0, 0.0,
                                    world_depth * 2.0 - 1.0, 1.0);
            world_coc = normalizedCoC(p.z / p.w);
        }
        else
        {
            world_coverage = 0.0;
        }
        frag_layers = vec4(rigged_coc, world_coc,
                           rigged_coverage, world_coverage);
    }
}
