/**
 * AyaneStorm XeGTAO visibility and optional bent-normal adaptation.
 * Author: chanayane@firestorm
 *
 * Derived from the Intel XeGTAO implementation:
 * https://github.com/GameTechDev/XeGTAO (see LICENSE in that repository).
 */

uniform sampler2D gtao_depth;
uniform sampler2D gtao_normal;
uniform usampler2D gtao_hilbert;
uniform vec2 gtao_pixel_size;
uniform vec2 gtao_ndc_to_view_mul;
uniform vec2 gtao_ndc_to_view_add;
uniform float gtao_radius;
uniform float gtao_power;
uniform int gtao_orthographic_view;

const float GTAO_PI = 3.14159265358979323846;
const float GTAO_HALF_PI = 1.57079632679489661923;
const float GTAO_TERM_SCALE = 1.5;

float gtao_fast_sqrt(float x)
{
    return uintBitsToFloat(0x1fbd1df5u + (floatBitsToUint(x) >> 1u));
}

float gtao_fast_acos(float value)
{
    float x = abs(value);
    float result = (-0.156583 * x + GTAO_HALF_PI) * gtao_fast_sqrt(max(0.0, 1.0 - x));
    return value >= 0.0 ? result : GTAO_PI - result;
}

vec4 gtao_edges(float center, float left, float right, float top, float bottom)
{
    vec4 edges = vec4(left, right, top, bottom) - center;
    float slope_lr = (edges.y - edges.x) * 0.5;
    float slope_tb = (edges.w - edges.z) * 0.5;
    vec4 adjusted = edges + vec4(slope_lr, -slope_lr, slope_tb, -slope_tb);
    return clamp(1.25 - min(abs(edges), abs(adjusted)) / max(center * 0.011, 1e-6), 0.0, 1.0);
}

float gtao_pack_edges(vec4 edges)
{
    edges = round(clamp(edges, 0.0, 1.0) * 2.9);
    return dot(edges, vec4(64.0, 16.0, 4.0, 1.0) / 255.0);
}

vec3 gtao_decode_normal(vec4 encoded)
{
    vec2 fenc = encoded.xy * 4.0 - 2.0;
    float f = dot(fenc, fenc);
    float g = sqrt(max(0.0, 1.0 - f * 0.25));
    vec3 normal = vec3(fenc * g, 1.0 - f * 0.5);
    // Viewer view space -> XeGTAO convention.
    return normalize(vec3(normal.x, -normal.y, -normal.z));
}

vec3 gtao_view_position(vec2 uv, float depth)
{
    vec2 xy = gtao_ndc_to_view_mul * uv + gtao_ndc_to_view_add;
    return vec3(gtao_orthographic_view != 0 ? xy : xy * depth, depth);
}

vec2 gtao_noise(ivec2 pixel)
{
    uint index = texelFetch(gtao_hilbert, pixel & ivec2(63), 0).r;
    return fract(vec2(0.5) + float(index) * vec2(0.75487766624669276005,
                                                0.56984029099805326591));
}

vec3 gtao_rotate_from_minus_z(vec3 value, vec3 target)
{
    const vec3 source = vec3(0.0, 0.0, -1.0);
    float cosine = dot(source, target);
    if (cosine > 0.9997)
    {
        return value;
    }
    vec3 axis = cross(source, target);
    return value * cosine + cross(axis, value) +
        axis * (dot(axis, value) / max(1.0 + cosine, 1e-6));
}

void gtao_main_pass(ivec2 pixel, int slice_count, int steps_per_slice,
                    out vec4 ao_term, out float packed_edges)
{
    ivec2 size = textureSize(gtao_depth, 0);
    ivec2 lo = ivec2(0);
    ivec2 hi = size - 1;
    float center = texelFetch(gtao_depth, pixel, 0).r;
    float left = texelFetch(gtao_depth, clamp(pixel + ivec2(-1, 0), lo, hi), 0).r;
    float right = texelFetch(gtao_depth, clamp(pixel + ivec2(1, 0), lo, hi), 0).r;
    float top = texelFetch(gtao_depth, clamp(pixel + ivec2(0, 1), lo, hi), 0).r;
    float bottom = texelFetch(gtao_depth, clamp(pixel + ivec2(0, -1), lo, hi), 0).r;
    vec4 edges = gtao_edges(center, left, right, top, bottom);
    packed_edges = gtao_pack_edges(edges);

    if (center >= 65500.0)
    {
#ifdef GTAO_BENT_NORMALS
        ao_term = vec4(0.5, 0.5, 0.0, 1.0 / GTAO_TERM_SCALE);
#else
        ao_term = vec4(1.0 / GTAO_TERM_SCALE);
#endif
        packed_edges = gtao_pack_edges(vec4(1.0));
        return;
    }

    vec2 uv = (vec2(pixel) + 0.5) * gtao_pixel_size;
    center *= 0.99920;
    vec3 center_pos = gtao_view_position(uv, center);
    vec3 view_vec = normalize(-center_pos);
    vec3 normal = gtao_decode_normal(texelFetch(gtao_normal, pixel, 0));
    vec2 noise = gtao_noise(pixel);

    const float radius_multiplier = 1.457;
    const float falloff_fraction = 0.615;
    const float distribution_power = 2.0;
    const float mip_offset = 3.30;
    float effect_radius = gtao_radius * radius_multiplier;
    float falloff_range = falloff_fraction * effect_radius;
    float falloff_from = effect_radius * (1.0 - falloff_fraction);
    float falloff_mul = -1.0 / max(falloff_range, 1e-6);
    float falloff_add = falloff_from / max(falloff_range, 1e-6) + 1.0;

    vec2 pixel_view_size = gtao_ndc_to_view_mul * gtao_pixel_size *
        (gtao_orthographic_view != 0 ? 1.0 : center);
    float screen_radius = effect_radius / max(abs(pixel_view_size.x), 1e-6);
    float visibility = clamp((10.0 - screen_radius) / 100.0, 0.0, 1.0) * 0.5;
#ifdef GTAO_BENT_NORMALS
    vec3 bent_normal = vec3(0.0);
#endif
    float min_s = 1.3 / max(screen_radius, 1e-6);

    for (int slice = 0; slice < 18; ++slice)
    {
        if (slice >= slice_count) break;
        float phi = (float(slice) + noise.x) * GTAO_PI / float(slice_count);
        float cos_phi = cos(phi);
        float sin_phi = sin(phi);
        vec2 omega = vec2(cos_phi, -sin_phi) * screen_radius;
        vec3 direction = vec3(cos_phi, sin_phi, 0.0);
        vec3 ortho_direction = direction - dot(direction, view_vec) * view_vec;
        vec3 axis = normalize(cross(ortho_direction, view_vec));
        vec3 projected_normal = normal - axis * dot(normal, axis);
        float projected_length = length(projected_normal);
        float sign_normal = sign(dot(ortho_direction, projected_normal));
        float cos_normal = clamp(dot(projected_normal, view_vec) /
                                 max(projected_length, 1e-6), 0.0, 1.0);
        float n = sign_normal * gtao_fast_acos(cos_normal);
        float low_horizon_0 = cos(n + GTAO_HALF_PI);
        float low_horizon_1 = cos(n - GTAO_HALF_PI);
        float horizon_0 = low_horizon_0;
        float horizon_1 = low_horizon_1;

        for (int step_index = 0; step_index < 4; ++step_index)
        {
            if (step_index >= steps_per_slice) break;
            float step_noise = fract(noise.y +
                float(slice + step_index * steps_per_slice) * 0.6180339887498948482);
            float sample_fraction = pow((float(step_index) + step_noise) /
                                        float(steps_per_slice), distribution_power) + min_s;
            vec2 sample_offset_pixels = round(sample_fraction * omega);
            float sample_offset_length = length(sample_offset_pixels);
            float mip = clamp(log2(max(sample_offset_length, 1.0)) - mip_offset, 0.0, 4.0);
            vec2 sample_offset = sample_offset_pixels * gtao_pixel_size;

            vec2 uv0 = clamp(uv + sample_offset, vec2(0.0), vec2(1.0));
            vec2 uv1 = clamp(uv - sample_offset, vec2(0.0), vec2(1.0));
            float depth0 = textureLod(gtao_depth, uv0, mip).r;
            float depth1 = textureLod(gtao_depth, uv1, mip).r;
            vec3 delta0 = gtao_view_position(uv0, depth0) - center_pos;
            vec3 delta1 = gtao_view_position(uv1, depth1) - center_pos;
            float distance0 = max(length(delta0), 1e-6);
            float distance1 = max(length(delta1), 1e-6);
            float weight0 = clamp(distance0 * falloff_mul + falloff_add, 0.0, 1.0);
            float weight1 = clamp(distance1 * falloff_mul + falloff_add, 0.0, 1.0);
            float sample_horizon_0 = dot(delta0 / distance0, view_vec);
            float sample_horizon_1 = dot(delta1 / distance1, view_vec);
            sample_horizon_0 = mix(low_horizon_0, sample_horizon_0, weight0);
            sample_horizon_1 = mix(low_horizon_1, sample_horizon_1, weight1);
            horizon_0 = max(horizon_0, sample_horizon_0);
            horizon_1 = max(horizon_1, sample_horizon_1);
        }

        projected_length = mix(projected_length, 1.0, 0.05);
        float h0 = -gtao_fast_acos(clamp(horizon_1, -1.0, 1.0));
        float h1 = gtao_fast_acos(clamp(horizon_0, -1.0, 1.0));
        float arc0 = (cos_normal + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) * 0.25;
        float arc1 = (cos_normal + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) * 0.25;
        visibility += projected_length * (arc0 + arc1);

#ifdef GTAO_BENT_NORMALS
        float t0 = (6.0 * sin(h0 - n) - sin(3.0 * h0 - n) +
                    6.0 * sin(h1 - n) - sin(3.0 * h1 - n) + 16.0 * sin(n) -
                    3.0 * (sin(h0 + n) + sin(h1 + n))) / 12.0;
        float t1 = (-cos(3.0 * h0 - n) - cos(3.0 * h1 - n) + 8.0 * cos(n) -
                    3.0 * (cos(h0 + n) + cos(h1 + n))) / 12.0;
        vec3 local_bent_normal = vec3(direction.x * t0, direction.y * t0, -t1);
        bent_normal += gtao_rotate_from_minus_z(local_bent_normal, view_vec) * projected_length;
#endif
    }

    visibility = pow(max(visibility / float(slice_count), 0.0), gtao_power);
    visibility = max(0.03, visibility);
#ifdef GTAO_BENT_NORMALS
    bent_normal = normalize(bent_normal);
    ao_term = vec4(bent_normal * 0.5 + 0.5,
                   clamp(visibility / GTAO_TERM_SCALE, 0.0, 1.0));
#else
    ao_term = vec4(clamp(visibility / GTAO_TERM_SCALE, 0.0, 1.0));
#endif
}

float gtao_depth_mip_filter(vec4 depth, float radius)
{
    float max_depth = max(max(depth.x, depth.y), max(depth.z, depth.w));
    float effect_radius = 0.75 * radius * 1.457;
    float falloff_range = 0.615 * effect_radius;
    float falloff_from = effect_radius * (1.0 - 0.615);
    float falloff_mul = -1.0 / max(falloff_range, 1e-6);
    float falloff_add = falloff_from / max(falloff_range, 1e-6) + 1.0;
    vec4 weights = clamp((vec4(max_depth) - depth) * falloff_mul + falloff_add, 0.0, 1.0);
    return dot(weights, depth) / max(dot(weights, vec4(1.0)), 1e-6);
}

uniform sampler2D gtao_visibility_source;
uniform sampler2D gtao_edges_source;
uniform float gtao_blur_beta;
uniform int gtao_final_pass;

vec4 gtao_unpack_edges(ivec2 pixel, ivec2 hi)
{
    uint packed_value = uint(texelFetch(gtao_edges_source,
        clamp(pixel, ivec2(0), hi), 0).r * 255.5);
    return clamp(vec4((packed_value >> 6u) & 3u, (packed_value >> 4u) & 3u,
                      (packed_value >> 2u) & 3u, packed_value & 3u) / 3.0,
                 0.0, 1.0);
}

vec4 gtao_term_at(ivec2 pixel, ivec2 hi)
{
    vec4 encoded = texelFetch(gtao_visibility_source, clamp(pixel, ivec2(0), hi), 0);
#ifdef GTAO_BENT_NORMALS
    return vec4(encoded.rgb * 2.0 - 1.0, encoded.a);
#else
    return vec4(encoded.r);
#endif
}

void gtao_add_denoise_sample(ivec2 pixel, float weight, ivec2 hi,
                             inout vec4 sum, inout float sum_weight)
{
    sum += gtao_term_at(pixel, hi) * weight;
    sum_weight += weight;
}

vec4 gtao_denoise_pixel(ivec2 pixel)
{
    ivec2 hi = textureSize(gtao_visibility_source, 0) - 1;
    vec4 center = gtao_unpack_edges(pixel, hi);
    vec4 left = gtao_unpack_edges(pixel + ivec2(-1, 0), hi);
    vec4 right = gtao_unpack_edges(pixel + ivec2(1, 0), hi);
    vec4 top = gtao_unpack_edges(pixel + ivec2(0, 1), hi);
    vec4 bottom = gtao_unpack_edges(pixel + ivec2(0, -1), hi);
    center *= vec4(left.y, right.x, top.w, bottom.z);

    float edginess = clamp(4.0 - 2.5 - dot(center, vec4(1.0)), 0.0, 1.5) / 1.5 * 0.5;
    center = clamp(center + edginess, 0.0, 1.0);
    float weight_tl = 0.425 * (center.x * left.z + center.z * top.x);
    float weight_tr = 0.425 * (center.z * top.y + center.y * right.z);
    float weight_bl = 0.425 * (center.w * bottom.x + center.x * left.w);
    float weight_br = 0.425 * (center.y * right.w + center.w * bottom.y);

    float beta = gtao_final_pass != 0 ? gtao_blur_beta : gtao_blur_beta / 5.0;
    vec4 sum = gtao_term_at(pixel, hi) * beta;
    float sum_weight = beta;
    gtao_add_denoise_sample(pixel + ivec2(-1, 0), center.x, hi, sum, sum_weight);
    gtao_add_denoise_sample(pixel + ivec2(1, 0), center.y, hi, sum, sum_weight);
    gtao_add_denoise_sample(pixel + ivec2(0, 1), center.z, hi, sum, sum_weight);
    gtao_add_denoise_sample(pixel + ivec2(0, -1), center.w, hi, sum, sum_weight);
    gtao_add_denoise_sample(pixel + ivec2(-1, 1), weight_tl, hi, sum, sum_weight);
    gtao_add_denoise_sample(pixel + ivec2(1, 1), weight_tr, hi, sum, sum_weight);
    gtao_add_denoise_sample(pixel + ivec2(-1, -1), weight_bl, hi, sum, sum_weight);
    gtao_add_denoise_sample(pixel + ivec2(1, -1), weight_br, hi, sum, sum_weight);
    vec4 result = sum / max(sum_weight, 1e-6);
#ifdef GTAO_BENT_NORMALS
    vec3 filtered_normal = normalize(result.xyz);
    float visibility = clamp(result.a * (gtao_final_pass != 0 ? GTAO_TERM_SCALE : 1.0),
                             0.0, 1.0);
    return vec4(filtered_normal * 0.5 + 0.5, visibility);
#else
    float visibility = clamp(result.r * (gtao_final_pass != 0 ? GTAO_TERM_SCALE : 1.0),
                             0.0, 1.0);
    return vec4(visibility);
#endif
}
