/**
 * Direct approximate adaptive voxel-based OIT volume construction and resolve.
 */

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 2, rgba16f) uniform writeonly image2D avboitOutput;
layout(binding = 3, r32ui) uniform coherent uimage3D avboitExtinction;
// R16F rather than the presentation's R8. This volume is the entire ordering
// weight for blended geometry, so its precision bounds how closely the
// approximate weight can match the exact aggregate extinction that attenuates
// opaque geometry. R8 quantizes the sheer range viewer clothing occupies to
// 1/255 steps and cannot represent a 1/65536 effective-zero endpoint at all.
layout(binding = 4, r16f) uniform coherent image3D avboitTransmittance;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
layout(binding = 7, r32ui) uniform coherent uimage2D avboitExtinctionOverflowDepth;
layout(binding = 0, rgba16f) uniform readonly image2D avboitAccumulatedColorGlow;
layout(binding = 1, r16f) uniform readonly image2D avboitAccumulatedWeight;
layout(binding = 5, r16f) uniform readonly image2D avboitAccumulatedExtinction;

// Transient prefix-scan working array. At the reference 8192-slice domain this
// fitted in shared memory, but a high-resolution domain exceeds
// GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, so it lives in shader storage. Image and
// buffer binding points are separate namespaces, so SSBO binding 2 is free.
layout(std430, binding = 2) buffer AVBOITWarpScan
{
    uint avboitWarpScan[AVBOIT_VIRTUAL_SLICES];
};

layout(std430, binding = 4) buffer AVBOITOccupancy
{
    uint avboitOccupancy[AVBOIT_VIRTUAL_SLICES];
};

layout(std430, binding = 5) buffer AVBOITWarp
{
    uint avboitWarp[AVBOIT_VIRTUAL_SLICES];
};

layout(std430, binding = 6) buffer AVBOITTileOccupancy
{
    uint avboitTileOccupancy[];
};

layout(std430, binding = 7) buffer AVBOITDiagnostics
{
    uint avboitDiagnostic[16];
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
uniform float avboitLinearization;
uniform vec2 avboitProxyDepthInterval;
uniform int avboitEntityID;

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

uint avboit_zbin_offset()
{
    ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
    return avboit_tile_offset() +
        uint(tile_count.x * tile_count.y) * 4u;
}

uint avboit_entity_mask_offset()
{
    return avboit_zbin_offset() +
        uint(AVBOIT_VIRTUAL_SLICES) * uint(AVBOIT_ZBIN_LEVELS);
}

uint avboit_bounds_offset()
{
    return avboit_entity_mask_offset() +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) *
            AVBOIT_ENTITY_MASK_WORDS;
}

uint avboit_proxy_miss_offset()
{
    return avboit_bounds_offset() +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) * 4u;
}

uint avboit_dilated_bounds_offset()
{
    return avboit_bounds_offset() +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) * 2u;
}

// Screen-space tile grid for per-tile depth ranging. Must match
// AVBOIT_RANGE_TILE in avboitCaptureF.glsl and the tile count in fsavboit.cpp.
const int AVBOIT_RANGE_TILE = 16;

// Per-tile depth range, two depth keys per tile, after the proxy-bounds region.
uint avboit_tile_range_offset()
{
    return avboit_bounds_offset() +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) * 5u;
}

ivec2 avboit_range_tile_count()
{
    return max((avboitViewport + ivec2(AVBOIT_RANGE_TILE - 1)) /
                   AVBOIT_RANGE_TILE,
               ivec2(1));
}

const uint AVBOIT_SLICES = 128u;
// Largest power-of-two divider the compaction search may use. It must be
// able to reduce the virtual domain to the physical budget, so it scales
// with the domain: 8192 needs 6, 65536 needs 9. A cap smaller than this
// pins the search below a fitting candidate and compaction never fits.
const uint AVBOIT_MAX_DIVIDER = uint(AVBOIT_MAX_DIVIDER_VALUE);
const uint AVBOIT_OCCUPANCY_WORDS = AVBOIT_SLICES / 32u;
const uint AVBOIT_WARP_FILTERABLE = 0x80000000u;
const uint AVBOIT_WARP_RANGE_BEGIN = 0x40000000u;
const uint AVBOIT_WARP_RANGE_END = 0x20000000u;
const uint AVBOIT_WARP_RANGE_MIDDLE = 0x10000000u;
const float AVBOIT_ZERO_EXTINCTION_NARROW = 5.54126355;  // -log(1 / 255)
const float AVBOIT_ZERO_EXTINCTION_WIDE = 11.09035489;   // -log(1 / 65536)

// The scratch volume is always allocated for the wide two-lane layout. The
// narrow four-lane layout simply leaves the upper half of its slices unused,
// so the representation is switchable at runtime without reallocation.
uniform int avboitWideExtinction;

uint avboit_lanes_per_word()
{
    return avboitWideExtinction != 0 ? 2u : 4u;
}

uint avboit_packed_slices()
{
    return AVBOIT_SLICES / avboit_lanes_per_word();
}

float avboit_zero_extinction()
{
    return avboitWideExtinction != 0 ?
        AVBOIT_ZERO_EXTINCTION_WIDE : AVBOIT_ZERO_EXTINCTION_NARROW;
}

// avboitWarpScan is a shader-storage buffer (declared above) rather than shared
// memory, so every barrier() guarding it must be paired with
// memoryBarrierBuffer() to make the writes visible across the workgroup.
void avboit_scan_barrier()
{
    memoryBarrierBuffer();
    barrier();
}

float avboit_curve_coordinate(float linear_depth, uint divider)
{
    float scale = exp2(float(divider));
    float slice_count = float(AVBOIT_VIRTUAL_SLICES) / scale;
    float linearization = avboitLinearization / scale;
    float far_depth = max(avboitDepthRange.y, 0.0001);
    return clamp(
        log2(linear_depth / linearization + 1.0) /
            log2(far_depth / linearization + 1.0) * slice_count,
        0.0, slice_count - 1.0);
}

float avboit_high_virtual_depth(float virtual_coordinate)
{
    float far_depth = max(avboitDepthRange.y, 0.0001);
    float normalized =
        clamp(virtual_coordinate / float(AVBOIT_VIRTUAL_SLICES), 0.0, 1.0);
    return avboitLinearization *
        (exp2(normalized *
              log2(far_depth / avboitLinearization + 1.0)) - 1.0);
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

uint avboit_uniform_zbin(float linear_depth)
{
    float coordinate =
        (linear_depth - avboitDepthRange.x) /
        max(avboitDepthRange.y - avboitDepthRange.x, 0.0001);
    return min(
        uint(clamp(coordinate, 0.0, 0.99999994) *
            float(AVBOIT_VIRTUAL_SLICES)),
        uint(AVBOIT_VIRTUAL_SLICES) - 1u);
}

uvec2 avboit_zbin_range(uint first_bin, uint last_bin)
{
    uint range_length = last_bin - first_bin + 1u;
    uint level = uint(findMSB(range_length));
    uint span = 1u << level;
    uint level_offset =
        avboit_zbin_offset() + level * uint(AVBOIT_VIRTUAL_SLICES);
    uint left = avboitWork[level_offset + first_bin];
    uint right = avboitWork[
        level_offset + last_bin - span + 1u];
    uint minimum_id = 0xffffu;
    uint maximum_id = 0u;
    if ((left & 0xffffu) != 0xffffu)
    {
        minimum_id = left & 0xffffu;
        maximum_id = left >> 16u;
    }
    if ((right & 0xffffu) != 0xffffu)
    {
        minimum_id = minimum_id == 0xffffu ?
            (right & 0xffffu) :
            min(minimum_id, right & 0xffffu);
        maximum_id = max(maximum_id, right >> 16u);
    }
    // An empty query must remain conservative rather than erase proxy work.
    return minimum_id == 0xffffu ?
        uvec2(0u, 0xfffeu) : uvec2(minimum_id, maximum_id);
}

uint avboit_mask_for_id_range(uint word, uvec2 id_range)
{
    uint word_begin = word * 32u;
    uint bits = 0u;
    if (id_range.x <= 254u)
    {
        uint normal_end = min(id_range.y, 254u);
        uint first_bit = clamp(id_range.x, word_begin,
                               word_begin + 32u) - word_begin;
        uint end_bit = clamp(normal_end + 1u, word_begin,
                             word_begin + 32u) - word_begin;
        if (end_bit > first_bit)
        {
            uint width = end_bit - first_bit;
            bits = width == 32u ? 0xffffffffu :
                ((1u << width) - 1u) << first_bit;
        }
    }
    if (word == 7u && id_range.y >= 255u)
    {
        bits |= 0x80000000u;
    }
    return bits;
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
    uint slice_count = uint(AVBOIT_VIRTUAL_SLICES) >> divider;
    return min(uvec2(lower_bin, upper_bin),
               uvec2(slice_count - 1u));
}

float unpack_extinction(uint packed_word, uint slice_index)
{
    uint lanes_per_word = avboit_lanes_per_word();
    uint lane_mask = avboitWideExtinction != 0 ? 0xffffu : 0xffu;
    uint shift = (slice_index % lanes_per_word) * (32u / lanes_per_word);
    return float((packed_word >> shift) & lane_mask) *
        (avboit_zero_extinction() / float(lane_mask));
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

    if (avboitPass == 12)
    {
        if (all(lessThan(pixel, avboitVolumeSize)) &&
            tile_is_occupied(pixel))
        {
            float previous = 1.0;
            uint zero_depth =
                imageLoad(avboitZeroTransmittanceDepth, pixel).r;
            bool invalid = false;
            for (uint slice_index = 0u;
                 slice_index < AVBOIT_SLICES; ++slice_index)
            {
                float current = imageLoad(
                    avboitTransmittance,
                    ivec3(pixel, int(slice_index))).r;
                invalid = invalid ||
                    current > previous + (1.0 / 255.0) ||
                    (zero_depth < AVBOIT_SLICES &&
                     slice_index > zero_depth &&
                     current > (1.0 / 255.0));
                previous = current;
            }
            if (invalid)
            {
                atomicAdd(avboitDiagnostic[7], 1u);
            }
        }
        return;
    }

    if (avboitPass == 11)
    {
        uint thread_index = gl_LocalInvocationIndex;
        for (uint index = thread_index;
             index < uint(AVBOIT_VIRTUAL_SLICES); index += 256u)
        {
            uint entry = avboitWarp[index];
            uint coordinate = entry & 0x00ffffffu;
            bool filterable =
                (entry & AVBOIT_WARP_FILTERABLE) != 0u;
            bool range_begin =
                (entry & AVBOIT_WARP_RANGE_BEGIN) != 0u;
            bool range_end =
                (entry & AVBOIT_WARP_RANGE_END) != 0u;
            bool range_middle =
                (entry & AVBOIT_WARP_RANGE_MIDDLE) != 0u;
            bool invalid = coordinate >
                (AVBOIT_SLICES - 1u) * 65536u;
            if (filterable)
            {
                invalid = invalid ||
                    (range_middle && (range_begin || range_end)) ||
                    (!range_begin && !range_end && !range_middle);
            }
            else
            {
                invalid = invalid ||
                    range_begin || range_end || range_middle;
            }
            if (index > 0u)
            {
                uint previous = avboitWarp[index - 1u];
                uint previous_coordinate =
                    previous & 0x00ffffffu;
                bool previous_filterable =
                    (previous & AVBOIT_WARP_FILTERABLE) != 0u;
                invalid = invalid ||
                    coordinate < previous_coordinate ||
                    (!filterable && !previous_filterable &&
                     coordinate != previous_coordinate);
            }
            if (invalid)
            {
                atomicAdd(avboitDiagnostic[6], 1u);
            }
        }
        return;
    }

    if (avboitPass == 10)
    {
        if (all(lessThan(pixel, avboitVolumeSize)))
        {
            uint linear_cell =
                uint(pixel.y * avboitVolumeSize.x + pixel.x);
            uint interval = avboit_bounds_offset() + linear_cell * 2u;
            uint minimum_bin = uint(floor(
                avboit_curve_coordinate(
                    avboitProxyDepthInterval.x, 0u)));
            uint maximum_bin = uint(ceil(
                avboit_curve_coordinate(
                    avboitProxyDepthInterval.y, 0u)));
            atomicMin(avboitWork[interval],
                      minimum_bin > 0u ? minimum_bin - 1u : 0u);
            atomicMax(avboitWork[interval + 1u],
                      min(maximum_bin + 1u,
                          uint(AVBOIT_VIRTUAL_SLICES) - 1u));
            uint mask_entity =
                min(uint(max(avboitEntityID, 0)), 255u);
            uint mask = avboit_entity_mask_offset() +
                linear_cell * AVBOIT_ENTITY_MASK_WORDS +
                (mask_entity >> 5u);
            atomicOr(avboitWork[mask],
                     1u << (mask_entity & 31u));
        }
        return;
    }

    // Resets the per-tile depth range to an empty interval. atomicMin and
    // atomicMax in raster pass 0 close it around the transparency actually
    // present, and an untouched tile keeps minimum > maximum so the capture
    // shader falls back to the global curve.
    if (avboitPass == 12)
    {
        ivec2 tile_count = avboit_range_tile_count();
        if (all(lessThan(pixel, tile_count)))
        {
            uint range = avboit_tile_range_offset() +
                (uint(pixel.y) * uint(tile_count.x) + uint(pixel.x)) * 2u;
            avboitWork[range] = 0xffffffffu;
            avboitWork[range + 1u] = 0u;
        }
        return;
    }

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
            uint minimum_bin = 0xffffffffu;
            uint maximum_bin = 0u;
            // Match the neighbor-cell dilation required by trilinear
            // filtering without racing writes into the raw proxy intervals.
            for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                ivec2 neighbor = clamp(
                    pixel + ivec2(x, y), ivec2(0),
                    avboitVolumeSize - ivec2(1));
                uint neighbor_cell =
                    uint(neighbor.y * avboitVolumeSize.x + neighbor.x);
                uint neighbor_interval =
                    avboit_bounds_offset() + neighbor_cell * 2u;
                uint neighbor_minimum =
                    avboitWork[neighbor_interval];
                if (neighbor_minimum != 0xffffffffu)
                {
                    minimum_bin = min(minimum_bin, neighbor_minimum);
                    maximum_bin = max(
                        maximum_bin,
                        avboitWork[neighbor_interval + 1u]);
                }
            }
            uint dilated_interval =
                avboit_dilated_bounds_offset() + linear_cell * 2u;
            avboitWork[dilated_interval] = minimum_bin;
            avboitWork[dilated_interval + 1u] = maximum_bin;
            uint mask = avboit_entity_mask_offset() +
                linear_cell * AVBOIT_ENTITY_MASK_WORDS;
            uint merged_mask = 0u;
            if (minimum_bin != 0xffffffffu)
            {
                uint first_zbin = avboit_uniform_zbin(
                    avboit_high_virtual_depth(float(minimum_bin)));
                uint last_zbin = avboit_uniform_zbin(
                    avboit_high_virtual_depth(
                        float(min(maximum_bin + 1u,
                                  uint(AVBOIT_VIRTUAL_SLICES)))));
                uvec2 id_range = avboit_zbin_range(
                    min(first_zbin, last_zbin),
                    max(first_zbin, last_zbin));
                uint word_min = min(id_range.x >> 5u,
                                    AVBOIT_ENTITY_MASK_WORDS - 1u);
                uint word_max = min(id_range.y >> 5u,
                                    AVBOIT_ENTITY_MASK_WORDS - 1u);
                for (uint word = word_min; word <= word_max; ++word)
                {
                    uint candidates = avboitWork[mask + word] &
                        avboit_mask_for_id_range(word, id_range);
                    // Portable scalar bit iteration corresponding to DRO17's
                    // wave-uniform merged-mask loop.
                    while (candidates != 0u)
                    {
                        uint bit = uint(findLSB(candidates));
                        merged_mask |= 1u;
                        candidates ^= 1u << bit;
                    }
                }
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
             virtual_index < uint(AVBOIT_VIRTUAL_SLICES); virtual_index += 256u)
        {
            occupied_virtual +=
                avboitOccupancy[virtual_index] != 0u ? 1u : 0u;
        }
        avboitWarpScan[thread_index] = occupied_virtual;
        avboit_scan_barrier();
        for (uint stride = 128u; stride > 0u; stride >>= 1u)
        {
            if (thread_index < stride)
            {
                avboitWarpScan[thread_index] +=
                    avboitWarpScan[thread_index + stride];
            }
            avboit_scan_barrier();
        }
        if (thread_index == 0u)
        {
            avboitDiagnostic[0] = avboitWarpScan[0];
            avboitDiagnostic[1] = uint(AVBOIT_VIRTUAL_SLICES);
            avboitDiagnostic[3] = 0u;
        }
        avboit_scan_barrier();

        // Test successively halved, reparameterized virtual resolutions.
        for (uint candidate = 0u;
             candidate <= AVBOIT_MAX_DIVIDER; ++candidate)
        {
            uint candidate_count = uint(AVBOIT_VIRTUAL_SLICES) >> candidate;
            for (uint index = thread_index;
                 index < uint(AVBOIT_VIRTUAL_SLICES); index += 256u)
            {
                avboitWarpScan[index] = 0u;
            }
            avboit_scan_barrier();
            for (uint virtual_index = thread_index;
                 virtual_index < uint(AVBOIT_VIRTUAL_SLICES); virtual_index += 256u)
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
            avboit_scan_barrier();
            uint local_count = 0u;
            for (uint index = thread_index;
                 index < candidate_count; index += 256u)
            {
                local_count += avboitWarpScan[index];
            }
            avboitWarpScan[thread_index] = local_count;
            avboit_scan_barrier();
            for (uint stride = 128u; stride > 0u; stride >>= 1u)
            {
                if (thread_index < stride)
                {
                    avboitWarpScan[thread_index] +=
                        avboitWarpScan[thread_index + stride];
                }
                avboit_scan_barrier();
            }
            if (thread_index == 0u &&
                avboitDiagnostic[1] > AVBOIT_SLICES)
            {
                avboitDiagnostic[3] = candidate;
                avboitDiagnostic[1] = avboitWarpScan[0];
            }
            memoryBarrierBuffer();
            avboit_scan_barrier();
        }

        uint group_shift = avboitDiagnostic[3];
        avboitWork[7] = group_shift;
        // Record whether the search actually found a fitting candidate. If the
        // final occupied-group count still exceeds the physical budget, the
        // divider is pinned at its maximum and the warp cannot represent the
        // scene: compaction has failed rather than merely coarsened.
        if (thread_index == 0u)
        {
            avboitDiagnostic[8] = group_shift;
        }
        uint group_count = uint(AVBOIT_VIRTUAL_SLICES) >> group_shift;

        // Rebuild the selected conservative occupancy and preserve it in the
        // no-longer-needed high-resolution occupancy buffer during the scan.
        for (uint index = thread_index;
             index < uint(AVBOIT_VIRTUAL_SLICES); index += 256u)
        {
            avboitWarpScan[index] = 0u;
        }
        avboit_scan_barrier();
        for (uint virtual_index = thread_index;
             virtual_index < uint(AVBOIT_VIRTUAL_SLICES); virtual_index += 256u)
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
        avboit_scan_barrier();
        for (uint index = thread_index;
             index < uint(AVBOIT_VIRTUAL_SLICES); index += 256u)
        {
            avboitOccupancy[index] =
                index < group_count ? avboitWarpScan[index] : 0u;
        }
        avboit_scan_barrier();

        // In-place Blelloch exclusive prefix sum over group occupancy.
        for (uint stride = 1u;
             stride < uint(AVBOIT_VIRTUAL_SLICES); stride <<= 1u)
        {
            uint step = stride << 1u;
            for (uint index = (thread_index + 1u) * step - 1u;
                 index < uint(AVBOIT_VIRTUAL_SLICES); index += 256u * step)
            {
                avboitWarpScan[index] +=
                    avboitWarpScan[index - stride];
            }
            avboit_scan_barrier();
        }
        if (thread_index == 0u)
        {
            avboitWarpScan[uint(AVBOIT_VIRTUAL_SLICES) - 1u] = 0u;
        }
        avboit_scan_barrier();
        for (uint stride = uint(AVBOIT_VIRTUAL_SLICES) >> 1u;
             stride > 0u; stride >>= 1u)
        {
            uint step = stride << 1u;
            for (uint index = (thread_index + 1u) * step - 1u;
                 index < uint(AVBOIT_VIRTUAL_SLICES); index += 256u * step)
            {
                uint left = index - stride;
                uint previous = avboitWarpScan[left];
                avboitWarpScan[left] = avboitWarpScan[index];
                avboitWarpScan[index] += previous;
            }
            avboit_scan_barrier();
        }

        for (uint virtual_index = thread_index;
             virtual_index < uint(AVBOIT_VIRTUAL_SLICES); virtual_index += 256u)
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
            if (virtual_index + 1u < uint(AVBOIT_VIRTUAL_SLICES))
            {
                next_group = min(uint(floor(avboit_curve_coordinate(
                    avboit_high_virtual_depth(float(virtual_index + 1u)),
                    group_shift))), group_count - 1u);
            }
            bool range_begin = occupied && !previous_occupied &&
                (virtual_index == 0u || previous_group != group);
            bool range_end = occupied && !next_occupied &&
                (virtual_index + 1u == uint(AVBOIT_VIRTUAL_SLICES) ||
                 next_group != group);
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
            for (uint word = 0u; word < avboit_packed_slices(); ++word)
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
                // The PDF integration sequence adds this slice's extinction
                // before storing its integral sample. The -2 sampling bias is
                // defined against this post-slice phase.
                if (slice_index >= overflow_depth)
                {
                    extinction = avboit_zero_extinction();
                }
                else
                {
                    uint packed_word = imageLoad(
                        avboitExtinction,
                        ivec3(pixel,
                              int(slice_index / avboit_lanes_per_word()))).r;
                    extinction += unpack_extinction(packed_word, slice_index);
                }
                imageStore(avboitTransmittance,
                           ivec3(pixel, int(slice_index)),
                           vec4(exp(-extinction)));
                if (zero_depth == 255u &&
                    extinction >= avboit_zero_extinction())
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
        if (avboitDebugMode == 2 &&
            (weight > 0.0 || accumulated_glow > 0.0))
        {
            float occupancy = clamp(
                float(avboitDiagnostic[0]) /
                    float(AVBOIT_VIRTUAL_SLICES), 0.0, 1.0);
            imageStore(avboitOutput, pixel,
                       vec4(occupancy, 1.0 - occupancy, 0.0, 0.0));
            return;
        }
        if (avboitDebugMode == 3 &&
            (weight > 0.0 || accumulated_glow > 0.0))
        {
            float utilization = clamp(
                float(avboitDiagnostic[1]) / float(AVBOIT_SLICES),
                0.0, 1.0);
            imageStore(avboitOutput, pixel,
                       vec4(utilization, utilization * utilization,
                            1.0 - utilization, 0.0));
            return;
        }
        if (avboitDebugMode == 4 &&
            (weight > 0.0 || accumulated_glow > 0.0))
        {
            imageStore(avboitOutput, pixel,
                       vec4(vec3(total_transmittance), 0.0));
            return;
        }
        if (avboitDebugMode == 5 &&
            (weight > 0.0 || accumulated_glow > 0.0))
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
        if (avboitDebugMode == 6 &&
            (weight > 0.0 || accumulated_glow > 0.0))
        {
            uint failure = avboitWork[
                avboit_proxy_miss_offset() +
                uint(cell.y * avboitVolumeSize.x + cell.x)];
            vec3 coverage = vec3(0.0, 1.0, 0.0);
            if ((failure & 1u) != 0u)
            {
                coverage = vec3(1.0, 0.0, 0.0);
            }
            else if ((failure & 2u) != 0u)
            {
                coverage = vec3(1.0, 1.0, 0.0);
            }
            else if ((failure & 4u) != 0u)
            {
                coverage = vec3(1.0, 0.0, 1.0);
            }
            imageStore(avboitOutput, pixel,
                       vec4(coverage, 0.0));
            return;
        }
        if (avboitDebugMode == 7 &&
            (weight > 0.0 || accumulated_glow > 0.0))
        {
            vec3 result = avboitDiagnostic[6] == 0u ?
                vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            imageStore(avboitOutput, pixel, vec4(result, 0.0));
            return;
        }
        if (avboitDebugMode == 8 &&
            (weight > 0.0 || accumulated_glow > 0.0))
        {
            vec3 result = avboitDiagnostic[7] == 0u ?
                vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            imageStore(avboitOutput, pixel, vec4(result, 0.0));
            return;
        }
        // Separate the two resolve terms so a wrong result can be attributed to
        // aggregate opacity or to the normalized average color, rather than
        // inferred from the composited output. Mode 9 shows aggregate alpha as
        // greyscale; mode 10 shows the normalized average transparent color
        // without applying that opacity or the opaque background.
        // Unconditional so every pixel has a defined diagnostic value. Gating
        // these on weight > 0.0 let alpha-zero pixels fall through to normal
        // rendering, producing an image that mixed diagnostic and composited
        // output and could not be read reliably.
        if (avboitDebugMode == 9)
        {
            imageStore(avboitOutput, pixel,
                       vec4(vec3(aggregate_alpha), 0.0));
            return;
        }
        if (avboitDebugMode == 10)
        {
            imageStore(avboitOutput, pixel,
                       weight > 0.0 ?
                           vec4(max(weighted_color / weight, vec3(0.0)), 0.0) :
                           vec4(0.0, 0.0, 1.0, 0.0));
            return;
        }
        // Selected compaction divider, as a distinct colour per value. This
        // reports what the adaptive search actually chose, rather than the
        // occupied-group ratio that mode 3 shows. A divider pinned at
        // AVBOIT_MAX_DIVIDER means the search never found a fitting candidate.
        //   black  0    grey 1   blue 2   cyan 3   green 4
        //   yellow 5    orange 6  red 7   magenta 8   white 9 or more
        if (avboitDebugMode == 15)
        {
            uint d = avboitDiagnostic[8];
            vec3 c = vec3(1.0);
            if (d == 0u)      c = vec3(0.05);
            else if (d == 1u) c = vec3(0.4);
            else if (d == 2u) c = vec3(0.0, 0.2, 1.0);
            else if (d == 3u) c = vec3(0.0, 0.8, 1.0);
            else if (d == 4u) c = vec3(0.0, 1.0, 0.0);
            else if (d == 5u) c = vec3(1.0, 1.0, 0.0);
            else if (d == 6u) c = vec3(1.0, 0.5, 0.0);
            else if (d == 7u) c = vec3(1.0, 0.0, 0.0);
            else if (d == 8u) c = vec3(1.0, 0.0, 1.0);
            imageStore(avboitOutput, pixel, vec4(c, 0.0));
            return;
        }
        // Raw accumulated optical depth, banded. aggregate_alpha = 1-exp(-x)
        // compresses everything above about x=3 into visually identical white,
        // so it cannot distinguish a correct single layer from many-times
        // over-accumulation. Each band is one -log(1-alpha) unit:
        //   black   x<0.1   (nothing)
        //   blue    x~0.69  (one layer at alpha 0.5)
        //   green   x~1.39  (two layers at alpha 0.5)
        //   yellow  x~2.1   (three layers)
        //   red     x>3     (over-accumulated or near-opaque)
        if (avboitDebugMode == 11)
        {
            float x = accumulated_extinction;
            vec3 band = vec3(0.0);
            if (x >= 3.0)            band = vec3(1.0, 0.0, 0.0);
            else if (x >= 1.75)      band = vec3(1.0, 1.0, 0.0);
            else if (x >= 1.05)      band = vec3(0.0, 1.0, 0.0);
            else if (x >= 0.4)       band = vec3(0.0, 0.4, 1.0);
            else if (x >= 0.1)       band = vec3(0.3, 0.3, 0.3);
            imageStore(avboitOutput, pixel, vec4(band, 0.0));
            return;
        }
        // Average front transmittance actually used for ordering, banded. The
        // capture pass substitutes T_front for color under this mode. Behind a
        // 0.95-alpha garment a rear layer must report a low value; a high value
        // means the volume lookup returns the wrong depth rather than the
        // averaging being at fault.
        //   grey   no coverage
        //   green  T_front < 0.15   rear layers correctly suppressed
        //   blue   0.15 - 0.5
        //   yellow 0.5 - 0.85
        //   red    T_front > 0.85   rear layers not suppressed at all
        if (avboitDebugMode == 14)
        {
            if (weight <= 0.0)
            {
                imageStore(avboitOutput, pixel, vec4(0.15, 0.15, 0.15, 0.0));
                return;
            }
            float average_front = clamp(weighted_color.r / weight, 0.0, 1.0);
            vec3 band = vec3(0.0, 1.0, 0.0);
            if (average_front > 0.85)      band = vec3(1.0, 0.0, 0.0);
            else if (average_front > 0.5)  band = vec3(1.0, 1.0, 0.0);
            else if (average_front > 0.15) band = vec3(0.0, 0.4, 1.0);
            imageStore(avboitOutput, pixel, vec4(band, 0.0));
            return;
        }
        // Compare the low-resolution volume against the exact full-resolution
        // accumulation. Both measure how much light survives everything at this
        // pixel, so they must agree. The volume's deepest slice is total
        // transmittance; exp(-accumulated_extinction) is the exact value.
        // Disagreement means the volume is not recording the extinction that
        // the weighted-color pass relies on for ordering, which is the only
        // remaining way a surface behind a near-opaque layer can stay visible.
        //   green  volume matches exact within 0.05
        //   blue   volume transmits MORE than exact (under-recorded occlusion)
        //   red    volume transmits LESS than exact (over-recorded occlusion)
        //   grey   no transparent coverage at this pixel
        if (avboitDebugMode == 13)
        {
            if (weight <= 0.0)
            {
                imageStore(avboitOutput, pixel, vec4(0.15, 0.15, 0.15, 0.0));
                return;
            }
            vec2 sample_xy = (vec2(pixel) + vec2(0.5)) / vec2(avboitViewport);
            float volume_tail = texture(
                avboitTransmittanceSampler,
                vec3(sample_xy,
                     (float(AVBOIT_SLICES) - 0.5) /
                         float(AVBOIT_SLICES))).r;
            float difference = volume_tail - total_transmittance;
            vec3 band = vec3(0.0, 1.0, 0.0);
            if (difference > 0.05)       band = vec3(0.0, 0.4, 1.0);
            else if (difference < -0.05) band = vec3(1.0, 0.0, 0.0);
            imageStore(avboitOutput, pixel, vec4(band, 0.0));
            return;
        }
        // Distinguish many correct layers from few layers with inflated alpha.
        // weight is sum(alpha * T_front); for the frontmost layers T_front is
        // near one, so weight approximates sum(alpha). Comparing that against
        // accumulated optical depth separates the two causes of a red mode-11
        // reading:
        //   grey  weight < 0.1        no meaningful coverage
        //   blue  weight ~ 0.5-1.5    one or two ordinary layers: if mode 11 is
        //                             red here, individual alpha is inflated
        //   green weight ~ 1.5-3      genuinely several overlapping layers
        //   red   weight > 3          very dense real geometry
        if (avboitDebugMode == 12)
        {
            float w = weight;
            vec3 band = vec3(0.0);
            if (w >= 3.0)            band = vec3(1.0, 0.0, 0.0);
            else if (w >= 1.5)       band = vec3(0.0, 1.0, 0.0);
            else if (w >= 0.4)       band = vec3(0.0, 0.4, 1.0);
            else if (w >= 0.1)       band = vec3(0.3, 0.3, 0.3);
            imageStore(avboitOutput, pixel, vec4(band, 0.0));
            return;
        }
        imageStore(avboitOutput, pixel,
                   max(vec4(transparent + opaque.rgb * total_transmittance,
                            accumulated_glow +
                                opaque.a * total_transmittance),
                       vec4(0.0)));
    }
}
