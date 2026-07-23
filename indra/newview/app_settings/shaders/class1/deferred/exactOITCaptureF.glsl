/**
 * Shared Exact OIT fragment capture implementation.
 */

layout(early_fragment_tests) in;
layout(binding = 0, r32ui) uniform coherent uimage2D oitHeadPointers;
layout(binding = 1, r32ui) uniform coherent uimage2D oitListCounts;

struct OITNode
{
    vec4 color;
    float glow;
    float depth;
    uint next;
    uint blend;
};

layout(std430, binding = 0) buffer OITNodes
{
    OITNode oitNodes[];
};

layout(std430, binding = 1) buffer OITControl
{
    uint oitNodeCount;
    uint oitNodeCapacity;
    uint oitOverflow;
    uint oitPad;
};

uniform uint oitBlendFactors;
uniform float oitGlow;
uniform int oitDiscardNoOp;
uniform int avboitRasterPass;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform sampler3D avboitTransmittanceSampler;

layout(binding = 3, r32ui) uniform coherent uimage3D avboitExtinction;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
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
    return mix(float(avboitWarp[lower_virtual]), float(avboitWarp[upper_virtual]),
               fract(virtual_coordinate));
}

uint avboit_tile_index(ivec2 cell, uint word)
{
    return (uint(cell.y) * uint(avboitVolumeSize.x) + uint(cell.x)) * 4u + word;
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
            uint fixed_extinction = uint(optical_depth * 65536.0 + 0.5);
            uint lower_slice = uint(floor(slice_coordinate));
            uint upper_slice = min(lower_slice + 1u, 127u);
            uint upper_extinction = uint(
                float(fixed_extinction) * fract(slice_coordinate) + 0.5);
            uint lower_extinction = fixed_extinction - upper_extinction;
            ivec2 cell = clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1));
            if (upper_slice == lower_slice)
            {
                imageAtomicAdd(avboitExtinction, ivec3(cell, int(lower_slice)),
                               fixed_extinction);
            }
            else
            {
                imageAtomicAdd(avboitExtinction, ivec3(cell, int(lower_slice)),
                               lower_extinction);
                imageAtomicAdd(avboitExtinction, ivec3(cell, int(upper_slice)),
                               upper_extinction);
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
        float sample_slice = clamp(slice_coordinate - 2.0, 0.0, 127.0);
        float front_transmittance = texture(
            avboitTransmittanceSampler,
            vec3(sample_xy, (sample_slice + 0.5) / 128.0)).r;
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

void exact_oit_store(vec4 color)
{
    // Standard alpha with exact zero source alpha and glow is a complete no-op.
    // Reject it before allocation so invisible card texels create no list work.
    const uint standard_alpha_blend = 7u | (9u << 8u) | (1u << 16u) | (9u << 24u);
    if (oitDiscardNoOp != 0 &&
        oitBlendFactors == standard_alpha_blend &&
        color.a == 0.0 &&
        oitGlow == 0.0)
    {
        return;
    }

    if (avboitRasterPass >= 0)
    {
        avboit_direct_store(color);
        return;
    }

    uint index = atomicAdd(oitNodeCount, 1u);
    if (index >= oitNodeCapacity)
    {
        atomicOr(oitOverflow, 1u);
        return;
    }

    oitNodes[index].color = color;
    oitNodes[index].glow = oitGlow;
    oitNodes[index].depth = gl_FragCoord.z;
    oitNodes[index].blend = oitBlendFactors;
    oitNodes[index].next = imageAtomicExchange(oitHeadPointers, ivec2(gl_FragCoord.xy), index);

    uint pixel_count = imageAtomicAdd(oitListCounts, ivec2(gl_FragCoord.xy), 1u) + 1u;
    atomicMax(oitPad, pixel_count);
}
