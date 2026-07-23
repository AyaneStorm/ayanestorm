/**
 * Shared AVBOIT direct-raster fragment output implementation.
 */

layout(early_fragment_tests) in;
uniform float oitGlow;
uniform int avboitRasterPass;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform sampler3D avboitTransmittanceSampler;
const uint AVBOIT_DIRECT_SLICES = 128u;
const uint AVBOIT_DIRECT_OCCUPANCY_WORDS = AVBOIT_DIRECT_SLICES / 32u;

layout(binding = 3, r32ui) uniform coherent uimage3D avboitExtinction;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
layout(binding = 7, r32ui) uniform coherent uimage2D avboitExtinctionOverflowDepth;
layout(std430, binding = 4) buffer AVBOITOccupancy { uint avboitOccupancy[8192]; };
layout(std430, binding = 5) buffer AVBOITWarp { uint avboitWarp[8192]; };
layout(std430, binding = 6) buffer AVBOITTileOccupancy { uint avboitTileOccupancy[]; };
layout(std430, binding = 7) buffer AVBOITDirectAccumulation
{
    uint avboitDirectAccumulation[];
};

float avboit_warped_slice(float depth)
{
    float virtual_coordinate = clamp(depth, 0.0, 1.0) * 8191.0;
    uint lower_virtual = uint(floor(virtual_coordinate));
    uint upper_virtual = min(lower_virtual + 1u, 8191u);
    return mix(float(avboitWarp[lower_virtual]),
               float(avboitWarp[upper_virtual]),
               fract(virtual_coordinate)) / 65536.0;
}

uint avboit_tile_index(ivec2 cell, uint word)
{
    return (uint(cell.y) * uint(avboitVolumeSize.x) + uint(cell.x)) *
        AVBOIT_DIRECT_OCCUPANCY_WORDS + word;
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

void avboit_add_extinction(ivec2 cell, uint slice_index, float optical_depth)
{
    uint value = uint(clamp(
        optical_depth / -log(1.0 / 65536.0) * 255.0, 0.0, 255.0) + 0.5);
    if (value == 0u)
    {
        return;
    }

    uint shift = (slice_index & 3u) * 8u;
    uint old_word = imageAtomicAdd(
        avboitExtinction, ivec3(cell, int(slice_index >> 2u)), value << shift);
    uint old_value = (old_word >> shift) & 255u;
    if (old_value + value > 255u)
    {
        imageAtomicMin(avboitExtinctionOverflowDepth, cell, slice_index);
    }
}

void avboit_direct_store(vec4 color)
{
    float alpha = clamp(color.a, 0.0, 1.0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (avboitRasterPass == 0)
    {
        if (alpha > 0.0 || oitGlow > 0.0)
        {
            ivec2 cell = clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1));
            avboit_mark_tile(cell);
        }
        if (alpha > 0.0)
        {
            uint virtual_slice = min(uint(clamp(gl_FragCoord.z, 0.0, 1.0) * 8191.0),
                                     8191u);
            atomicOr(avboitOccupancy[virtual_slice], 1u);
        }
        return;
    }

    float slice_coordinate = avboit_warped_slice(gl_FragCoord.z);
    if (avboitRasterPass == 1)
    {
        if (alpha > 0.0)
        {
            float optical_depth = -log(max(1.0 - alpha, 1.0 / 65536.0)) / 64.0;
            uint lower_slice = uint(floor(slice_coordinate));
            uint upper_slice = min(lower_slice + 1u, AVBOIT_DIRECT_SLICES - 1u);
            float upper_extinction = optical_depth * fract(slice_coordinate);
            float lower_extinction = optical_depth - upper_extinction;
            ivec2 cell = clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1));
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
        ivec2 cell = clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1));
        uint zero_depth = imageLoad(avboitZeroTransmittanceDepth, cell).r;
        if (zero_depth != 255u && slice_coordinate > float(zero_depth))
        {
            return;
        }

        vec2 sample_xy = (vec2(pixel) + vec2(0.5)) / vec2(avboitViewport);
        float sample_slice = clamp(
            slice_coordinate - 2.0, 0.0, float(AVBOIT_DIRECT_SLICES - 1u));
        float front_transmittance = texture(
            avboitTransmittanceSampler,
            vec3(sample_xy, (sample_slice + 0.5) /
                float(AVBOIT_DIRECT_SLICES))).r;
        float weight = alpha * front_transmittance;
        uint index = (uint(pixel.y) * uint(avboitViewport.x) + uint(pixel.x)) * 6u;
        atomicAdd(avboitDirectAccumulation[index],
                  uint(clamp(color.r * weight * 4096.0, 0.0, 16777215.0) + 0.5));
        atomicAdd(avboitDirectAccumulation[index + 1u],
                  uint(clamp(color.g * weight * 4096.0, 0.0, 16777215.0) + 0.5));
        atomicAdd(avboitDirectAccumulation[index + 2u],
                  uint(clamp(color.b * weight * 4096.0, 0.0, 16777215.0) + 0.5));
        atomicAdd(avboitDirectAccumulation[index + 3u],
                  uint(clamp(weight * 4096.0, 0.0, 16777215.0) + 0.5));
        atomicAdd(avboitDirectAccumulation[index + 4u],
                  uint(clamp(oitGlow * front_transmittance * 4096.0,
                             0.0, 16777215.0) + 0.5));
        atomicAdd(avboitDirectAccumulation[index + 5u],
                  uint(clamp(-log(max(1.0 - alpha, 1.0 / 65536.0)) * 4096.0,
                             0.0, 16777215.0) + 0.5));
    }
}

void avboit_store(vec4 color)
{
    avboit_direct_store(color);
}
