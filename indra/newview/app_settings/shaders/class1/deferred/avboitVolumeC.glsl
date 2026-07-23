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

uniform sampler2D diffuseRect;
uniform sampler3D avboitTransmittanceSampler;
uniform int avboitPass;
uniform int avboitDebugMode;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;

const uint AVBOIT_SLICES = 128u;
const uint AVBOIT_PACKED_SLICES = AVBOIT_SLICES / 4u;
const uint AVBOIT_OCCUPANCY_WORDS = AVBOIT_SLICES / 32u;
const uint AVBOIT_WARP_FILTERABLE = 0x80000000u;
const float AVBOIT_ZERO_EXTINCTION = 5.54126355; // -log(1 / 255)

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

    if (avboitPass == 1)
    {
        if (pixel == ivec2(0))
        {
            uint occupied_virtual = 0u;
            for (uint virtual_index = 0u;
                 virtual_index < 8192u; ++virtual_index)
            {
                occupied_virtual +=
                    avboitOccupancy[virtual_index] != 0u ? 1u : 0u;
            }
            avboitDiagnostic[0] = occupied_virtual;

            // Coarsen virtual depth until occupied groups fit the physical pool.
            uint group_shift = 0u;
            for (uint candidate = 0u; candidate <= 6u; ++candidate)
            {
                uint group_size = 1u << candidate;
                uint occupied_groups = 0u;
                for (uint start = 0u; start < 8192u; start += group_size)
                {
                    bool occupied = false;
                    for (uint offset = 0u; offset < group_size; ++offset)
                    {
                        occupied = occupied ||
                            avboitOccupancy[start + offset] != 0u;
                    }
                    occupied_groups += occupied ? 1u : 0u;
                }
                group_shift = candidate;
                if (occupied_groups <= AVBOIT_SLICES)
                {
                    break;
                }
            }

            uint group_size = 1u << group_shift;
            uint physical = 0u;
            for (uint start = 0u; start < 8192u; start += group_size)
            {
                bool occupied = false;
                for (uint offset = 0u; offset < group_size; ++offset)
                {
                    occupied = occupied ||
                        avboitOccupancy[start + offset] != 0u;
                }
                for (uint offset = 0u; offset < group_size; ++offset)
                {
                    float coordinate = float(physical);
                    if (occupied)
                    {
                        coordinate += float(offset) / float(group_size);
                    }
                    uint encoded_coordinate = uint(
                        clamp(coordinate, 0.0,
                              float(AVBOIT_SLICES - 1u)) * 65536.0 + 0.5);
                    avboitWarp[start + offset] = encoded_coordinate |
                        (occupied ? AVBOIT_WARP_FILTERABLE : 0u);
                }
                physical += occupied ? 1u : 0u;
            }
            avboitDiagnostic[1] = physical;
        }
        return;
    }

    if (avboitPass == 3)
    {
        if (all(lessThan(pixel, avboitVolumeSize)))
        {
            imageStore(avboitExtinctionOverflowDepth, pixel, uvec4(255u));
            imageStore(avboitZeroTransmittanceDepth, pixel, uvec4(255u));
            if (tile_is_occupied(pixel))
            {
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
        }
        return;
    }

    if (avboitPass == 5)
    {
        if (all(lessThan(pixel, avboitVolumeSize)) && tile_is_occupied(pixel))
        {
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
