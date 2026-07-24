/**
 * Direct approximate adaptive voxel-based OIT volume construction and resolve.
 */

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 2, rgba16f) uniform writeonly image2D avboitOutput;
layout(binding = 3, r32ui) uniform coherent uimage3D avboitExtinction;
layout(binding = 4, r8) uniform coherent image3D avboitTransmittance;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
layout(binding = 7, r32ui) uniform coherent uimage2D avboitExtinctionOverflowDepth;
layout(binding = 0, rgba16f) uniform readonly image2D avboitAccumulatedColorGlow;
layout(binding = 1, r16f) uniform readonly image2D avboitAccumulatedWeight;
layout(binding = 5, r16f) uniform readonly image2D avboitAccumulatedExtinction;

layout(std430, binding = 4) buffer AVBOITOccupancy
{
    uint avboitOccupancy[8192];
};

layout(std430, binding = 5) buffer AVBOITWarp
{
    uint avboitWarp[8192];
};

layout(std430, binding = 6) buffer AVBOITTileOccupancy
{
    uint avboitTileOccupancy[];
};

layout(std430, binding = 7) buffer AVBOITDiagnostics
{
    uint avboitDiagnostic[4];
};

layout(std430, binding = 3) buffer AVBOITWork
{
    uint avboitWork[];
};

uniform sampler2D diffuseRect;
uniform sampler3D avboitTransmittanceSampler;
uniform int avboitPass;
uniform int avboitDebugMode;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform vec2 avboitDepthRange;

const uint AVBOIT_ENTITY_MASK_WORDS = 8u;

uint avboit_cell_offset()
{
    return 8u + 128u;
}

uint avboit_tile_offset()
{
    return avboit_cell_offset() +
        uint(avboitVolumeSize.x * avboitVolumeSize.y);
}

uint avboit_bounds_offset()
{
    ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
    return avboit_tile_offset() +
        uint(tile_count.x * tile_count.y) * 4u +
        8192u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) *
            AVBOIT_ENTITY_MASK_WORDS;
}

uint avboit_entity_mask_offset()
{
    ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
    return avboit_tile_offset() +
        uint(tile_count.x * tile_count.y) * 4u +
        8192u;
}

const uint AVBOIT_SLICES = 128u;
const uint AVBOIT_PACKED_SLICES = AVBOIT_SLICES / 4u;
const uint AVBOIT_OCCUPANCY_WORDS = AVBOIT_SLICES / 32u;
const uint AVBOIT_WARP_FILTERABLE = 0x80000000u;
const uint AVBOIT_WARP_RANGE_BEGIN = 0x40000000u;
const uint AVBOIT_WARP_RANGE_END = 0x20000000u;
const uint AVBOIT_WARP_RANGE_MIDDLE = 0x10000000u;
const float AVBOIT_ZERO_EXTINCTION = 5.54126355; // -log(1 / 255)

shared uint avboitWarpScan[8192];

float avboit_curve_coordinate(float linear_depth, uint divider)
{
    float scale = exp2(float(divider));
    float slice_count = 8192.0 / scale;
    float linearization = 16384.0 / scale;
    float far_depth = max(avboitDepthRange.y, 0.0001);
    return clamp(
        log2(linear_depth / linearization + 1.0) /
            log2(far_depth / linearization + 1.0) * slice_count,
        0.0, slice_count - 1.0);
}

float avboit_high_virtual_depth(float virtual_coordinate)
{
    float far_depth = max(avboitDepthRange.y, 0.0001);
    float normalized = clamp(virtual_coordinate / 8192.0, 0.0, 1.0);
    return 16384.0 *
        (exp2(normalized * log2(far_depth / 16384.0 + 1.0)) - 1.0);
}

float avboit_window_depth(float linear_depth)
{
    float near_depth = max(avboitDepthRange.x, 0.0001);
    float far_depth = max(avboitDepthRange.y, near_depth + 0.0001);
    float ndc_depth =
        (far_depth + near_depth -
         2.0 * near_depth * far_depth / max(linear_depth, near_depth)) /
        (far_depth - near_depth);
    return clamp(ndc_depth * 0.5 + 0.5, 0.0, 1.0);
}

uvec2 avboit_reparameterized_bin_range(uint virtual_index, uint divider)
{
    float lower_depth =
        avboit_high_virtual_depth(float(virtual_index));
    float upper_depth =
        avboit_high_virtual_depth(float(virtual_index + 1u));
    uint lower_bin = uint(floor(
        avboit_curve_coordinate(lower_depth, divider)));
    uint upper_bin = uint(floor(
        avboit_curve_coordinate(upper_depth, divider)));
    uint slice_count = 8192u >> divider;
    return min(uvec2(lower_bin, upper_bin),
               uvec2(slice_count - 1u));
}

float unpack_extinction(uint packed_word, uint slice_index)
{
    uint shift = (slice_index & 3u) * 8u;
    return float((packed_word >> shift) & 255u) *
        (AVBOIT_ZERO_EXTINCTION / 255.0);
}

uint tile_occupancy_index(ivec2 cell, uint word)
{
    return (uint(cell.y) * uint(avboitVolumeSize.x) + uint(cell.x)) *
        AVBOIT_OCCUPANCY_WORDS + word;
}

bool tile_is_occupied(ivec2 cell)
{
    for (uint word = 0u; word < AVBOIT_OCCUPANCY_WORDS; ++word)
    {
        if (avboitTileOccupancy[tile_occupancy_index(cell, word)] != 0u)
        {
            return true;
        }
    }
    return false;
}

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);

    if (avboitPass == 9)
    {
        if (all(lessThan(pixel, avboitVolumeSize)))
        {
            uint linear_cell =
                uint(pixel.y * avboitVolumeSize.x + pixel.x);
            uint interval = avboit_bounds_offset() + linear_cell * 2u;
            avboitWork[interval] = 0xffffffffu;
            avboitWork[interval + 1u] = 0u;
            uint mask = avboit_entity_mask_offset() +
                linear_cell * AVBOIT_ENTITY_MASK_WORDS;
            for (uint word = 0u;
                 word < AVBOIT_ENTITY_MASK_WORDS; ++word)
            {
                avboitWork[mask + word] = 0u;
            }
        }
        return;
    }

    if (avboitPass == 8)
    {
        if (all(lessThan(pixel, avboitVolumeSize)))
        {
            uint linear_cell =
                uint(pixel.y * avboitVolumeSize.x + pixel.x);
            uint interval = avboit_bounds_offset() + linear_cell * 2u;
            uint minimum_bin = avboitWork[interval];
            uint mask = avboit_entity_mask_offset() +
                linear_cell * AVBOIT_ENTITY_MASK_WORDS;
            uint merged_mask = 0u;
            for (uint word = 0u;
                 word < AVBOIT_ENTITY_MASK_WORDS; ++word)
            {
                merged_mask |= avboitWork[mask + word];
            }
            if (minimum_bin != 0xffffffffu && merged_mask != 0u)
            {
                // The interval is retained for the future per-entity Z-bin
                // candidate stage. Coarse group bounds must not populate
                // global Z occupancy: doing so erases real empty ranges and
                // reduces adaptive precision.
                for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                {
                    ivec2 neighbor = clamp(
                        pixel + ivec2(x, y), ivec2(0),
                        avboitVolumeSize - ivec2(1));
                    atomicOr(avboitTileOccupancy[
                        tile_occupancy_index(neighbor, 0u)], 1u);
                }
            }
        }
        return;
    }

    if (avboitPass == 1)
    {
        uint thread_index = gl_LocalInvocationIndex;
        uint occupied_virtual = 0u;
        for (uint virtual_index = thread_index;
             virtual_index < 8192u; virtual_index += 256u)
        {
            occupied_virtual +=
                avboitOccupancy[virtual_index] != 0u ? 1u : 0u;
        }
        avboitWarpScan[thread_index] = occupied_virtual;
        barrier();
        for (uint stride = 128u; stride > 0u; stride >>= 1u)
        {
            if (thread_index < stride)
            {
                avboitWarpScan[thread_index] +=
                    avboitWarpScan[thread_index + stride];
            }
            barrier();
        }
        if (thread_index == 0u)
        {
            avboitDiagnostic[0] = avboitWarpScan[0];
            avboitDiagnostic[1] = 8192u;
            avboitDiagnostic[3] = 0u;
        }
        memoryBarrierBuffer();
        barrier();

        // Test successively halved, reparameterized virtual resolutions.
        for (uint candidate = 0u; candidate <= 6u; ++candidate)
        {
            uint candidate_count = 8192u >> candidate;
            for (uint index = thread_index;
                 index < 8192u; index += 256u)
            {
                avboitWarpScan[index] = 0u;
            }
            barrier();
            for (uint virtual_index = thread_index;
                 virtual_index < 8192u; virtual_index += 256u)
            {
                if (avboitOccupancy[virtual_index] != 0u)
                {
                    uvec2 bins = avboit_reparameterized_bin_range(
                        virtual_index, candidate);
                    for (uint bin_index = bins.x;
                         bin_index <= bins.y; ++bin_index)
                    {
                        atomicOr(avboitWarpScan[bin_index], 1u);
                    }
                }
            }
            barrier();
            uint local_count = 0u;
            for (uint index = thread_index;
                 index < candidate_count; index += 256u)
            {
                local_count += avboitWarpScan[index];
            }
            avboitWarpScan[thread_index] = local_count;
            barrier();
            for (uint stride = 128u; stride > 0u; stride >>= 1u)
            {
                if (thread_index < stride)
                {
                    avboitWarpScan[thread_index] +=
                        avboitWarpScan[thread_index + stride];
                }
                barrier();
            }
            if (thread_index == 0u &&
                avboitDiagnostic[1] > AVBOIT_SLICES)
            {
                avboitDiagnostic[3] = candidate;
                avboitDiagnostic[1] = avboitWarpScan[0];
            }
            memoryBarrierBuffer();
            barrier();
        }

        uint group_shift = avboitDiagnostic[3];
        uint group_count = 8192u >> group_shift;

        // Rebuild the selected conservative occupancy and preserve it in the
        // no-longer-needed high-resolution occupancy buffer during the scan.
        for (uint index = thread_index;
             index < 8192u; index += 256u)
        {
            avboitWarpScan[index] = 0u;
        }
        barrier();
        for (uint virtual_index = thread_index;
             virtual_index < 8192u; virtual_index += 256u)
        {
            if (avboitOccupancy[virtual_index] != 0u)
            {
                uvec2 bins = avboit_reparameterized_bin_range(
                    virtual_index, group_shift);
                for (uint bin_index = bins.x;
                     bin_index <= bins.y; ++bin_index)
                {
                    atomicOr(avboitWarpScan[bin_index], 1u);
                }
            }
        }
        barrier();
        for (uint index = thread_index;
             index < 8192u; index += 256u)
        {
            avboitOccupancy[index] =
                index < group_count ? avboitWarpScan[index] : 0u;
        }
        memoryBarrierBuffer();
        barrier();

        // In-place Blelloch exclusive prefix sum over group occupancy.
        for (uint stride = 1u; stride < 8192u; stride <<= 1u)
        {
            uint step = stride << 1u;
            for (uint index = (thread_index + 1u) * step - 1u;
                 index < 8192u; index += 256u * step)
            {
                avboitWarpScan[index] +=
                    avboitWarpScan[index - stride];
            }
            barrier();
        }
        if (thread_index == 0u)
        {
            avboitWarpScan[8191] = 0u;
        }
        barrier();
        for (uint stride = 4096u; stride > 0u; stride >>= 1u)
        {
            uint step = stride << 1u;
            for (uint index = (thread_index + 1u) * step - 1u;
                 index < 8192u; index += 256u * step)
            {
                uint left = index - stride;
                uint previous = avboitWarpScan[left];
                avboitWarpScan[left] = avboitWarpScan[index];
                avboitWarpScan[index] += previous;
            }
            barrier();
        }

        for (uint virtual_index = thread_index;
             virtual_index < 8192u; virtual_index += 256u)
        {
            float high_depth =
                avboit_high_virtual_depth(float(virtual_index));
            float reduced_coordinate =
                avboit_curve_coordinate(high_depth, group_shift);
            uint group = min(uint(floor(reduced_coordinate)),
                             group_count - 1u);
            bool occupied = avboitOccupancy[group] != 0u;
            bool previous_occupied =
                group > 0u && avboitOccupancy[group - 1u] != 0u;
            bool next_occupied =
                group + 1u < group_count &&
                avboitOccupancy[group + 1u] != 0u;
            uint previous_group = group;
            if (virtual_index > 0u)
            {
                previous_group = min(uint(floor(avboit_curve_coordinate(
                    avboit_high_virtual_depth(float(virtual_index - 1u)),
                    group_shift))), group_count - 1u);
            }
            uint next_group = group;
            if (virtual_index + 1u < 8192u)
            {
                next_group = min(uint(floor(avboit_curve_coordinate(
                    avboit_high_virtual_depth(float(virtual_index + 1u)),
                    group_shift))), group_count - 1u);
            }
            bool range_begin = occupied && !previous_occupied &&
                (virtual_index == 0u || previous_group != group);
            bool range_end = occupied && !next_occupied &&
                (virtual_index + 1u == 8192u || next_group != group);
            float coordinate = float(avboitWarpScan[group]);
            if (occupied)
            {
                coordinate += fract(reduced_coordinate);
                if (range_end)
                {
                    coordinate = float(avboitWarpScan[group] + 1u);
                }
            }
            uint encoded_coordinate = uint(
                clamp(coordinate, 0.0,
                      float(AVBOIT_SLICES - 1u)) * 65536.0 + 0.5);
            uint metadata = occupied ? AVBOIT_WARP_FILTERABLE : 0u;
            if (range_begin) metadata |= AVBOIT_WARP_RANGE_BEGIN;
            if (range_end) metadata |= AVBOIT_WARP_RANGE_END;
            if (occupied && !range_begin && !range_end)
            {
                metadata |= AVBOIT_WARP_RANGE_MIDDLE;
            }
            avboitWarp[virtual_index] = encoded_coordinate | metadata;
            if (occupied)
            {
                uint lower_slice = min(uint(floor(coordinate)),
                                       AVBOIT_SLICES - 1u);
                uint upper_slice = min(lower_slice + 1u,
                                       AVBOIT_SLICES - 1u);
                uint depth_bits = floatBitsToUint(
                    avboit_window_depth(avboit_high_virtual_depth(
                        float(virtual_index + 1u))));
                atomicMax(avboitWork[8u + lower_slice], depth_bits);
                atomicMax(avboitWork[8u + upper_slice], depth_bits);
            }
        }
        if (thread_index == 0u)
        {
            avboitDiagnostic[3] = 0u;
        }
        return;
    }

    if (avboitPass == 2)
    {
        if (all(lessThan(pixel, avboitVolumeSize)) &&
            tile_is_occupied(pixel))
        {
            uint work_index = atomicAdd(avboitDiagnostic[3], 1u);
            avboitWork[avboit_cell_offset() + work_index] =
                uint(pixel.y) * uint(avboitVolumeSize.x) + uint(pixel.x);
        }
        return;
    }

    if (avboitPass == 4)
    {
        if (gl_GlobalInvocationID == uvec3(0u))
        {
            avboitWork[0] =
                (avboitDiagnostic[3] + 255u) / 256u;
            avboitWork[1] = 1u;
            avboitWork[2] = 1u;
        }
        return;
    }

    if (avboitPass == 3)
    {
        uint work_index =
            gl_WorkGroupID.x * 256u + gl_LocalInvocationIndex;
        if (work_index < avboitDiagnostic[3])
        {
            uint linear_cell =
                avboitWork[avboit_cell_offset() + work_index];
            pixel = ivec2(
                int(linear_cell % uint(avboitVolumeSize.x)),
                int(linear_cell / uint(avboitVolumeSize.x)));
            imageStore(avboitExtinctionOverflowDepth, pixel, uvec4(255u));
            imageStore(avboitZeroTransmittanceDepth, pixel, uvec4(255u));
            for (uint word = 0u; word < AVBOIT_PACKED_SLICES; ++word)
            {
                ivec3 coordinate = ivec3(pixel, int(word));
                imageStore(avboitExtinction, coordinate, uvec4(0u));
            }
            for (uint slice_index = 0u;
                 slice_index < AVBOIT_SLICES; ++slice_index)
            {
                ivec3 coordinate = ivec3(pixel, int(slice_index));
                // Saturated integration stops early, so untouched tail
                // slices must represent zero rather than full transmission.
                imageStore(avboitTransmittance, coordinate, vec4(0.0));
            }
        }
        return;
    }

    if (avboitPass == 5)
    {
        uint work_index =
            gl_WorkGroupID.x * 256u + gl_LocalInvocationIndex;
        if (work_index < avboitDiagnostic[3])
        {
            uint linear_cell =
                avboitWork[avboit_cell_offset() + work_index];
            pixel = ivec2(
                int(linear_cell % uint(avboitVolumeSize.x)),
                int(linear_cell / uint(avboitVolumeSize.x)));
            float extinction = 0.0;
            uint zero_depth = 255u;
            uint overflow_depth =
                imageLoad(avboitExtinctionOverflowDepth, pixel).r;
            for (uint slice_index = 0u;
                 slice_index < AVBOIT_SLICES; ++slice_index)
            {
                imageStore(avboitTransmittance,
                           ivec3(pixel, int(slice_index)),
                           vec4(exp(-extinction)));
                if (slice_index >= overflow_depth)
                {
                    extinction = AVBOIT_ZERO_EXTINCTION;
                }
                else
                {
                    uint packed_word = imageLoad(
                        avboitExtinction,
                        ivec3(pixel, int(slice_index >> 2u))).r;
                    extinction += unpack_extinction(packed_word, slice_index);
                }
                if (zero_depth == 255u && extinction >= AVBOIT_ZERO_EXTINCTION)
                {
                    zero_depth = slice_index;
                    atomicAdd(avboitDiagnostic[2], 1u);
                    break;
                }
            }
            imageStore(avboitZeroTransmittanceDepth, pixel, uvec4(zero_depth));
        }
        return;
    }

    if (avboitPass == 6)
    {
        ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
        if (all(lessThan(pixel, tile_count)))
        {
            ivec2 base_cell = pixel * 2;
            uint zero_depth = 0u;
            for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
            {
                ivec2 cell = min(base_cell + ivec2(x, y),
                                 avboitVolumeSize - ivec2(1));
                zero_depth = max(
                    zero_depth,
                    imageLoad(avboitZeroTransmittanceDepth, cell).r);
            }
            if (zero_depth < AVBOIT_SLICES)
            {
                uint depth_bits =
                    avboitWork[8u + zero_depth];
                if (depth_bits != 0u)
                {
                    uint tile_index =
                        atomicAdd(avboitWork[5], 1u);
                    uint work_offset =
                        avboit_tile_offset() + tile_index * 4u;
                    avboitWork[work_offset] = uint(pixel.x);
                    avboitWork[work_offset + 1u] = uint(pixel.y);
                    avboitWork[work_offset + 2u] = depth_bits;
                    avboitWork[work_offset + 3u] = 0u;
                }
            }
        }
        return;
    }

    if (any(greaterThanEqual(pixel, avboitViewport)))
    {
        return;
    }

    if (avboitPass == 7)
    {
        vec4 color_glow = imageLoad(avboitAccumulatedColorGlow, pixel);
        float weight = imageLoad(avboitAccumulatedWeight, pixel).r;
        vec3 weighted_color = color_glow.rgb;
        float accumulated_glow = color_glow.a;
        float accumulated_extinction =
            imageLoad(avboitAccumulatedExtinction, pixel).r;
        if (avboitDebugMode == 1 &&
            (weight > 0.0 || accumulated_glow > 0.0))
        {
            imageStore(avboitOutput, pixel, vec4(1.0, 0.0, 1.0, 0.0));
            return;
        }
        float total_transmittance = exp(-accumulated_extinction);
        float aggregate_alpha = 1.0 - total_transmittance;
        ivec2 cell = clamp(pixel / 8, ivec2(0),
                           avboitVolumeSize - ivec2(1));
        vec3 transparent = weight > 0.0 ?
            weighted_color * (aggregate_alpha / weight) : vec3(0.0);
        vec4 opaque = texelFetch(diffuseRect, pixel, 0);
        if (avboitDebugMode == 2 && (weight > 0.0 || accumulated_glow > 0.0))
        {
            float occupancy = clamp(
                float(avboitDiagnostic[0]) / 8192.0, 0.0, 1.0);
            imageStore(avboitOutput, pixel,
                       vec4(occupancy, 1.0 - occupancy, 0.0, 0.0));
            return;
        }
        if (avboitDebugMode == 3 && (weight > 0.0 || accumulated_glow > 0.0))
        {
            float utilization = clamp(
                float(avboitDiagnostic[1]) / float(AVBOIT_SLICES),
                0.0, 1.0);
            imageStore(avboitOutput, pixel,
                       vec4(utilization, utilization * utilization,
                            1.0 - utilization, 0.0));
            return;
        }
        if (avboitDebugMode == 4 && (weight > 0.0 || accumulated_glow > 0.0))
        {
            imageStore(avboitOutput, pixel,
                       vec4(vec3(total_transmittance), 0.0));
            return;
        }
        if (avboitDebugMode == 5 && (weight > 0.0 || accumulated_glow > 0.0))
        {
            uint zero_depth =
                imageLoad(avboitZeroTransmittanceDepth, cell).r;
            vec3 diagnostic = zero_depth == 255u ?
                vec3(0.0, 0.25, 1.0) :
                vec3(1.0, float(zero_depth) /
                           float(AVBOIT_SLICES - 1u), 0.0);
            imageStore(avboitOutput, pixel, vec4(diagnostic, 0.0));
            return;
        }
        imageStore(avboitOutput, pixel,
                   max(vec4(transparent + opaque.rgb * total_transmittance,
                            accumulated_glow + opaque.a * total_transmittance),
                       vec4(0.0)));
    }
}
