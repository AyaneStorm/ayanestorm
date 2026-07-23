/**
 * Direct approximate adaptive voxel-based OIT volume construction and resolve.
 */

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 2, rgba16f) uniform writeonly image2D avboitOutput;
layout(binding = 3, r32ui) uniform coherent uimage3D avboitExtinction;
layout(binding = 4, r32f) uniform coherent image3D avboitTransmittance;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
layout(binding = 7, r32f) uniform coherent image2D avboitTotalTransmittance;

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

layout(std430, binding = 7) buffer AVBOITDirectAccumulation
{
    uint avboitDirectAccumulation[];
};

uniform sampler2D diffuseRect;
uniform sampler3D avboitTransmittanceSampler;
uniform int avboitPass;
uniform int avboitDebugMode;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;

const uint AVBOIT_SLICES = 128u;
const uint AVBOIT_OCCUPANCY_WORDS = AVBOIT_SLICES / 32u;
const float AVBOIT_ZERO_EXTINCTION = 11.0903549; // -log(1 / 65536)

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
                    avboitWarp[start + offset] = uint(
                        clamp(coordinate, 0.0,
                              float(AVBOIT_SLICES - 1u)) * 65536.0 + 0.5);
                }
                physical += occupied ? 1u : 0u;
            }
        }
        return;
    }

    if (avboitPass == 3)
    {
        if (all(lessThan(pixel, avboitVolumeSize)))
        {
            imageStore(avboitTotalTransmittance, pixel, vec4(1.0));
            if (tile_is_occupied(pixel))
            {
                for (uint slice = 0u; slice < AVBOIT_SLICES; ++slice)
                {
                    ivec3 coordinate = ivec3(pixel, int(slice));
                    imageStore(avboitExtinction, coordinate, uvec4(0u));
                    imageStore(avboitTransmittance, coordinate, vec4(1.0));
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
            for (uint slice = 0u; slice < AVBOIT_SLICES; ++slice)
            {
                imageStore(avboitTransmittance, ivec3(pixel, int(slice)),
                           vec4(exp(-extinction)));
                extinction += float(imageLoad(
                    avboitExtinction, ivec3(pixel, int(slice))).r) / 65536.0;
                if (zero_depth == 255u && extinction >= AVBOIT_ZERO_EXTINCTION)
                {
                    zero_depth = slice;
                    break;
                }
            }
            imageStore(avboitZeroTransmittanceDepth, pixel, uvec4(zero_depth));
            imageStore(avboitTotalTransmittance, pixel, vec4(exp(-extinction)));
        }
        return;
    }

    if (any(greaterThanEqual(pixel, avboitViewport)))
    {
        return;
    }

    if (avboitPass == 7)
    {
        uint index =
            (uint(pixel.y) * uint(avboitViewport.x) + uint(pixel.x)) * 6u;
        float weight =
            float(avboitDirectAccumulation[index + 3u]) / 4096.0;
        vec3 weighted_color = vec3(
            avboitDirectAccumulation[index],
            avboitDirectAccumulation[index + 1u],
            avboitDirectAccumulation[index + 2u]) / 4096.0;
        float accumulated_glow =
            float(avboitDirectAccumulation[index + 4u]) / 4096.0;
        float accumulated_extinction =
            float(avboitDirectAccumulation[index + 5u]) / 4096.0;
        if (avboitDebugMode == 1 &&
            (weight > 0.0 || accumulated_glow > 0.0))
        {
            imageStore(avboitOutput, pixel, vec4(1.0, 0.0, 1.0, 0.0));
            return;
        }
        float total_transmittance = exp(-accumulated_extinction);
        float aggregate_alpha = 1.0 - total_transmittance;
        vec3 transparent = weight > 0.0 ?
            weighted_color * (aggregate_alpha / weight) : vec3(0.0);
        vec4 opaque = texelFetch(diffuseRect, pixel, 0);
        imageStore(avboitOutput, pixel,
                   max(vec4(transparent + opaque.rgb * total_transmittance,
                            accumulated_glow + opaque.a * total_transmittance),
                       vec4(0.0)));
    }
}
