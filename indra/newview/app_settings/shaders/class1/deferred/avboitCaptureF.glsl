/**
 * Shared AVBOIT direct-raster fragment output implementation.
 */

layout(early_fragment_tests) in;
uniform float oitGlow;
uniform int avboitRasterPass;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform vec2 avboitDepthRange;
uniform sampler3D avboitTransmittanceSampler;
uniform sampler2D avboitOpaqueDepthSampler;
const uint AVBOIT_DIRECT_SLICES = 128u;
const uint AVBOIT_DIRECT_OCCUPANCY_WORDS = AVBOIT_DIRECT_SLICES / 32u;
const uint AVBOIT_WARP_FILTERABLE = 0x80000000u;
const uint AVBOIT_WARP_RANGE_BEGIN = 0x40000000u;
const uint AVBOIT_WARP_RANGE_END = 0x20000000u;
const uint AVBOIT_WARP_RANGE_MIDDLE = 0x10000000u;
const uint AVBOIT_WARP_COORDINATE_MASK = 0x00ffffffu;
const float AVBOIT_DIRECT_ZERO_EXTINCTION = 11.0903549; // -log(1 / 65536)

layout(binding = 3, r32ui) uniform coherent uimage3D avboitExtinction;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
layout(binding = 7, r32ui) uniform coherent uimage2D avboitExtinctionOverflowDepth;
layout(std430, binding = 2) buffer AVBOITNearestTransparent
{
    uint avboitNearestTransparent[];
};
layout(std430, binding = 4) buffer AVBOITOccupancy { uint avboitOccupancy[8192]; };
layout(std430, binding = 5) buffer AVBOITWarp { uint avboitWarp[8192]; };
layout(std430, binding = 6) buffer AVBOITTileOccupancy { uint avboitTileOccupancy[]; };
layout(std430, binding = 7) buffer AVBOITDiagnostics { uint avboitDiagnostic[8]; };
layout(std430, binding = 3) buffer AVBOITWork { uint avboitWork[]; };
layout(location = 1) out vec4 avboitAccumulatedColorGlow;
layout(location = 2) out float avboitAccumulatedWeight;
layout(location = 3) out float avboitAccumulatedExtinction;

float avboit_virtual_depth(float window_depth)
{
    float near_depth = max(avboitDepthRange.x, 0.0001);
    float far_depth = max(avboitDepthRange.y, near_depth + 0.0001);
    float ndc_depth = clamp(window_depth, 0.0, 1.0) * 2.0 - 1.0;
    float linear_depth = 2.0 * near_depth * far_depth /
        (far_depth + near_depth -
         ndc_depth * (far_depth - near_depth));
    // PDF slide 49: n=8192 and a=16384 at the proposed high virtual
    // resolution. The near plane is implicit in this parametrization.
    const float linearization = 16384.0;
    return clamp(log2(linear_depth / linearization + 1.0) /
                 log2(far_depth / linearization + 1.0), 0.0, 1.0);
}

float avboit_warped_slice(float depth)
{
    float virtual_coordinate =
        min(avboit_virtual_depth(depth) * 8192.0, 8191.0);
    uint lower_virtual = uint(floor(virtual_coordinate));
    uint upper_virtual = min(lower_virtual + 1u, 8191u);
    uint lower_entry = avboitWarp[lower_virtual];
    uint upper_entry = avboitWarp[upper_virtual];
    float lower_coordinate =
        float(lower_entry & AVBOIT_WARP_COORDINATE_MASK);
    float upper_coordinate =
        float(upper_entry & AVBOIT_WARP_COORDINATE_MASK);
    bool lower_filterable = (lower_entry & AVBOIT_WARP_FILTERABLE) != 0u;
    bool upper_filterable = (upper_entry & AVBOIT_WARP_FILTERABLE) != 0u;
    bool lower_range_end = (lower_entry & AVBOIT_WARP_RANGE_END) != 0u;
    bool upper_range_begin = (upper_entry & AVBOIT_WARP_RANGE_BEGIN) != 0u;
    if (lower_filterable && upper_filterable)
    {
        return mix(lower_coordinate, upper_coordinate,
                   fract(virtual_coordinate)) / 65536.0;
    }
    // Empty ranges are invariant. Their boundary markers select the occupied
    // endpoint and discard the interpolation fraction across empty depth.
    if (lower_range_end)
    {
        return lower_coordinate / 65536.0;
    }
    if (upper_range_begin)
    {
        return upper_coordinate / 65536.0;
    }
    return (lower_filterable ? lower_coordinate : upper_coordinate) / 65536.0;
}

uint avboit_pack_rgb10(vec3 color)
{
    uvec3 encoded = uvec3(
        clamp(color, vec3(0.0), vec3(1.0)) * 1023.0 + 0.5);
    return encoded.r | (encoded.g << 10u) | (encoded.b << 20u);
}

uint avboit_tile_index(ivec2 cell, uint word)
{
    return (uint(cell.y) * uint(avboitVolumeSize.x) + uint(cell.x)) *
        AVBOIT_DIRECT_OCCUPANCY_WORDS + word;
}

uint avboit_proxy_bounds_offset()
{
    ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
    return 8u + 128u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) +
        uint(tile_count.x * tile_count.y) * 4u +
        8192u * 14u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) * 8u;
}

uint avboit_dilated_proxy_bounds_offset()
{
    return avboit_proxy_bounds_offset() +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) * 2u;
}

void avboit_compare_proxy_coverage(ivec2 cell, uint virtual_slice)
{
    uint linear_cell =
        uint(cell.y * avboitVolumeSize.x + cell.x);
    uint interval =
        avboit_dilated_proxy_bounds_offset() + linear_cell * 2u;
    uint minimum_slice = avboitWork[interval];
    uint maximum_slice = avboitWork[interval + 1u];
    atomicAdd(avboitDiagnostic[4], 1u);
    uint failure = minimum_slice == 0xffffffffu ? 1u :
        (virtual_slice < minimum_slice ? 2u :
         (virtual_slice > maximum_slice ? 4u : 0u));
    if (failure != 0u)
    {
        atomicAdd(avboitDiagnostic[5], 1u);
        uint miss_map = avboit_proxy_bounds_offset() +
            uint(avboitVolumeSize.x * avboitVolumeSize.y) * 4u;
        atomicOr(avboitWork[miss_map + linear_cell], failure);
    }
}

void avboit_mark_tile(ivec2 cell)
{
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 neighbor = clamp(cell + ivec2(x, y), ivec2(0),
                                   avboitVolumeSize - ivec2(1));
            atomicOr(avboitTileOccupancy[avboit_tile_index(neighbor, 0u)], 1u);
        }
    }
}

uint avboit_conservative_zero_depth(ivec2 pixel)
{
    // One early-depth tile covers 16x16 full-resolution pixels, or 2x2
    // extinction cells. A tile can reject only when every covered cell has
    // reached effective zero, so the farthest zero depth is conservative.
    ivec2 base_cell = (pixel / 16) * 2;
    uint zero_depth = 0u;
    for (int y = 0; y <= 1; ++y)
    {
        for (int x = 0; x <= 1; ++x)
        {
            ivec2 sample_cell = clamp(base_cell + ivec2(x, y), ivec2(0),
                                      avboitVolumeSize - ivec2(1));
            zero_depth = max(
                zero_depth,
                imageLoad(avboitZeroTransmittanceDepth, sample_cell).r);
        }
    }
    return zero_depth;
}

bool avboit_cull_fragment()
{
    if (avboitRasterPass != 2)
    {
        return false;
    }
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float slice_coordinate = avboit_warped_slice(gl_FragCoord.z);
    uint zero_depth = avboit_conservative_zero_depth(pixel);
    // Preserve the two-slice self-occlusion margin. Removing it exposes the
    // 16x16 rejection granularity as visible blocks in layered hair.
    return zero_depth < AVBOIT_DIRECT_SLICES &&
        slice_coordinate > float(zero_depth) + 2.0;
}

bool avboit_behind_opaque_bounds(ivec2 cell)
{
    float farthest_depth = 0.0;
    ivec2 base_pixel = cell * 8;
    for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x)
    {
        ivec2 sample_pixel = min(
            base_pixel + ivec2(x, y), avboitViewport - ivec2(1));
        farthest_depth = max(
            farthest_depth,
            texelFetch(avboitOpaqueDepthSampler, sample_pixel, 0).r);
    }
    return gl_FragCoord.z > farthest_depth;
}

void avboit_add_extinction(ivec2 cell, uint slice_index, float optical_depth)
{
    uint value = uint(clamp(
        optical_depth / AVBOIT_DIRECT_ZERO_EXTINCTION * 65535.0,
        0.0, 65535.0) + 0.5);
    if (value == 0u)
    {
        return;
    }

    uint shift = (slice_index & 1u) * 16u;
    ivec3 coordinate = ivec3(cell, int(slice_index >> 1u));
    // Packed atomic addition deliberately permits carry into later slices.
    // Once this lane overflows, integration saturates at the recorded minimum
    // depth, so data in this and all later slices is irrelevant.
    uint previous = imageAtomicAdd(
        avboitExtinction, coordinate, value << shift);
    uint previous_value = (previous >> shift) & 65535u;
    if (previous_value + value > 65535u)
    {
        imageAtomicMin(avboitExtinctionOverflowDepth, cell, slice_index);
    }
}

void avboit_direct_store(vec4 color)
{
    float alpha = clamp(color.a, 0.0, 1.0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 cell = avboitRasterPass == 0 ?
        clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1)) :
        clamp(pixel, ivec2(0), avboitVolumeSize - ivec2(1));
    if (avboitRasterPass == 1 && avboit_behind_opaque_bounds(cell))
    {
        return;
    }
    if (avboitRasterPass == 0)
    {
        if (alpha > 0.0 || oitGlow > 0.0)
        {
            avboit_mark_tile(cell);
        }
        if (alpha > 0.0)
        {
            // Pack monotonically ordered 24-bit window depth and inverted
            // alpha. Atomic min selects the nearest layer and, for coincident
            // samples, the greatest coverage.
            uint depth24 = uint(clamp(gl_FragCoord.z, 0.0, 1.0) *
                                16777215.0 + 0.5);
            uint alpha8 = uint(alpha * 255.0 + 0.5);
            uint nearest_index =
                uint(pixel.y * avboitViewport.x + pixel.x);
            atomicMin(avboitNearestTransparent[nearest_index],
                      (depth24 << 8u) | (255u - alpha8));
            uint virtual_slice = min(uint(
                                         avboit_virtual_depth(gl_FragCoord.z) *
                                         8192.0),
                                     8191u);
            // Retain the material path while measuring whether conservative
            // proxy intervals cover every alpha-tested occupancy sample.
            avboit_compare_proxy_coverage(cell, virtual_slice);
            atomicOr(avboitOccupancy[virtual_slice], 1u);
        }
        return;
    }

    float slice_coordinate = avboit_warped_slice(gl_FragCoord.z);
    if (avboitRasterPass == 1)
    {
        if (alpha > 0.0)
        {
            float optical_depth =
                -log(max(1.0 - alpha, 1.0 / 65536.0));
            uint lower_slice = uint(floor(slice_coordinate));
            uint upper_slice = min(lower_slice + 1u, AVBOIT_DIRECT_SLICES - 1u);
            float upper_extinction = optical_depth * fract(slice_coordinate);
            float lower_extinction = optical_depth - upper_extinction;
            if (upper_slice == lower_slice)
            {
                avboit_add_extinction(cell, lower_slice, optical_depth);
            }
            else
            {
                avboit_add_extinction(cell, lower_slice, lower_extinction);
                avboit_add_extinction(cell, upper_slice, upper_extinction);
            }
        }
        return;
    }

    if (avboitRasterPass == 2)
    {
        if (avboit_cull_fragment())
        {
            return;
        }

        vec2 sample_xy = (vec2(pixel) + vec2(0.5)) / vec2(avboitViewport);
        // Sample at the surface. The two-slice safety margin belongs only to
        // conservative early-depth rejection; using it here overweights rear
        // layer color.
        float sample_slice = clamp(
            slice_coordinate, 0.0, float(AVBOIT_DIRECT_SLICES - 1u));
        float front_transmittance = texture(
            avboitTransmittanceSampler,
            vec3(sample_xy, (sample_slice + 0.5) /
                float(AVBOIT_DIRECT_SLICES))).r;
        uint nearest = avboitNearestTransparent[
            uint(pixel.y * avboitViewport.x + pixel.x)];
        uint surface_depth24 =
            uint(clamp(gl_FragCoord.z, 0.0, 1.0) * 16777215.0 + 0.5);
        uint nearest_depth24 = nearest >> 8u;
        if (nearest != 0xffffffffu &&
            surface_depth24 <= nearest_depth24 + 1u)
        {
            uint nearest_index =
                uint(pixel.y * avboitViewport.x + pixel.x);
            uint color_offset =
                uint(avboitViewport.x * avboitViewport.y);
            avboitNearestTransparent[color_offset + nearest_index] =
                avboit_pack_rgb10(color.rgb);
            // Resolve composites this exact nearest layer separately.
            avboitAccumulatedColorGlow =
                vec4(0.0, 0.0, 0.0,
                     max(oitGlow, 0.0) * front_transmittance);
            avboitAccumulatedWeight = 0.0;
            avboitAccumulatedExtinction = 0.0;
            return;
        }
        front_transmittance =
            clamp(front_transmittance, 0.0, 1.0);
        float weight = alpha * front_transmittance;
        avboitAccumulatedColorGlow =
            vec4(max(color.rgb, vec3(0.0)) * weight,
                 max(oitGlow, 0.0) * front_transmittance);
        avboitAccumulatedWeight = weight;
        avboitAccumulatedExtinction =
            -log(max(1.0 - alpha, 1.0 / 65536.0));
    }
}

void avboit_store(vec4 color)
{
    avboit_direct_store(color);
}
