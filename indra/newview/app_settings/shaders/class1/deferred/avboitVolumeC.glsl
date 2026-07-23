/**
 * Approximate adaptive voxel-based OIT built from captured transparency lists.
 * Passes build occupancy/warp/extinction/transmittance and resolve unsorted nodes.
 */

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0, r32ui) uniform readonly uimage2D oitHeadPointers;
layout(binding = 2, rgba16f) uniform writeonly image2D avboitOutput;
layout(binding = 3, r32ui) uniform coherent uimage3D avboitExtinction;
layout(binding = 4, r32f) uniform coherent image3D avboitTransmittance;
layout(binding = 5, r8ui) uniform coherent uimage2D avboitClassification;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
layout(binding = 7, r32f) uniform coherent image2D avboitTotalTransmittance;

struct OITNode
{
    vec4 color;
    float glow;
    float depth;
    uint next;
    uint blend;
};

layout(std430, binding = 0) readonly buffer OITNodes
{
    OITNode oitNodes[];
};

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

const uint OIT_NULL = 0xffffffffu;
const uint AVBOIT_SLICES = 192u;
const uint AVBOIT_OCCUPANCY_WORDS = AVBOIT_SLICES / 32u;
const uint AVBOIT_EXACT_SHALLOW_LIMIT = 32u;
const uint STANDARD_ALPHA_BLEND = 7u | (9u << 8u) | (1u << 16u) | (9u << 24u);
const uint AVBOIT_CLASS_EMPTY = 0u;
const uint AVBOIT_CLASS_EXACT = 1u;
const uint AVBOIT_CLASS_APPROXIMATE = 2u;
const float AVBOIT_ZERO_EXTINCTION = 11.0903549; // -log(1 / 65536)

uint virtual_slice(float depth)
{
    return min(uint(clamp(depth, 0.0, 1.0) * 8191.0), 8191u);
}

float warped_slice(float depth)
{
    float virtual_coordinate = clamp(depth, 0.0, 1.0) * 8191.0;
    uint lower_virtual = uint(floor(virtual_coordinate));
    uint upper_virtual = min(lower_virtual + 1u, 8191u);
    return mix(float(avboitWarp[lower_virtual]), float(avboitWarp[upper_virtual]),
               fract(virtual_coordinate));
}

ivec3 volume_coordinate(ivec2 pixel, uint slice)
{
    ivec2 cell = clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1));
    return ivec3(cell, int(slice));
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

void mark_filter_neighborhood(ivec2 cell,
                              uint slice_mask[AVBOIT_OCCUPANCY_WORDS])
{
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 neighbor = clamp(cell + ivec2(x, y), ivec2(0),
                                   avboitVolumeSize - ivec2(1));
            for (uint word = 0u; word < AVBOIT_OCCUPANCY_WORDS; ++word)
            {
                if (slice_mask[word] != 0u)
                {
                    atomicOr(avboitTileOccupancy[tile_occupancy_index(neighbor, word)],
                             slice_mask[word]);
                }
            }
        }
    }
}

bool node_comes_first(uint a, uint b)
{
    float a_depth = oitNodes[a].depth;
    float b_depth = oitNodes[b].depth;
    return a_depth > b_depth || (a_depth == b_depth && a < b);
}

void main()
{
    ivec3 invocation = ivec3(gl_GlobalInvocationID);
    ivec2 pixel = invocation.xy;

    if (avboitPass == 1)
    {
        if (pixel == ivec2(0))
        {
            uint occupied = 0u;
            for (uint i = 0u; i < 8192u; ++i)
            {
                occupied += avboitOccupancy[i] != 0u ? 1u : 0u;
            }
            uint ordinal = 0u;
            for (uint i = 0u; i < 8192u; ++i)
            {
                if (avboitOccupancy[i] != 0u)
                {
                    avboitWarp[i] = occupied == 0u ? 0u :
                        min((ordinal * AVBOIT_SLICES) / occupied, AVBOIT_SLICES - 1u);
                    ++ordinal;
                }
                else
                {
                    avboitWarp[i] = ordinal == 0u ? 0u :
                        min(((ordinal - 1u) * AVBOIT_SLICES) / max(occupied, 1u),
                            AVBOIT_SLICES - 1u);
                }
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
        uint accumulation_index =
            (uint(pixel.y) * uint(avboitViewport.x) + uint(pixel.x)) * 6u;
        float weight = float(avboitDirectAccumulation[accumulation_index + 3u]) / 4096.0;
        vec3 weighted_color = vec3(
            avboitDirectAccumulation[accumulation_index],
            avboitDirectAccumulation[accumulation_index + 1u],
            avboitDirectAccumulation[accumulation_index + 2u]) / 4096.0;
        float accumulated_glow =
            float(avboitDirectAccumulation[accumulation_index + 4u]) / 4096.0;
        float accumulated_extinction =
            float(avboitDirectAccumulation[accumulation_index + 5u]) / 4096.0;
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
        return;
    }

    uint head = imageLoad(oitHeadPointers, pixel).r;
    if (avboitPass == 0)
    {
        uint classification = head == OIT_NULL ? AVBOIT_CLASS_EMPTY : AVBOIT_CLASS_EXACT;
        uint count = 0u;
        for (uint node = head; node != OIT_NULL; node = oitNodes[node].next)
        {
            if (count == AVBOIT_EXACT_SHALLOW_LIMIT ||
                (oitNodes[node].blend != OIT_NULL &&
                 oitNodes[node].blend != STANDARD_ALPHA_BLEND))
            {
                classification = AVBOIT_CLASS_APPROXIMATE;
                break;
            }
            ++count;
        }
        imageStore(avboitClassification, pixel, uvec4(classification));

        if (classification == AVBOIT_CLASS_APPROXIMATE)
        {
            for (uint node = head; node != OIT_NULL; node = oitNodes[node].next)
            {
                if (oitNodes[node].blend != OIT_NULL && oitNodes[node].color.a > 0.0)
                {
                    atomicOr(avboitOccupancy[virtual_slice(oitNodes[node].depth)], 1u);
                }
            }
        }
        return;
    }

    if (avboitPass == 2)
    {
        if (imageLoad(avboitClassification, pixel).r != AVBOIT_CLASS_APPROXIMATE)
        {
            return;
        }
        ivec2 cell = clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1));
        uint slice_mask[AVBOIT_OCCUPANCY_WORDS];
        for (uint word = 0u; word < AVBOIT_OCCUPANCY_WORDS; ++word)
        {
            slice_mask[word] = 0u;
        }
        for (uint node = head; node != OIT_NULL; node = oitNodes[node].next)
        {
            float alpha = clamp(oitNodes[node].color.a, 0.0, 1.0);
            if (oitNodes[node].blend != OIT_NULL && alpha > 0.0)
            {
                float slice_coordinate = warped_slice(oitNodes[node].depth);
                uint lower_slice = uint(floor(slice_coordinate));
                uint upper_slice = min(lower_slice + 1u, AVBOIT_SLICES - 1u);
                slice_mask[lower_slice >> 5u] |= 1u << (lower_slice & 31u);
                slice_mask[upper_slice >> 5u] |= 1u << (upper_slice & 31u);
            }
        }
        // Keep custom zero-alpha/glow pixels on initialized identity
        // transmittance even when they contribute no extinction.
        bool any_slice = false;
        for (uint word = 0u; word < AVBOIT_OCCUPANCY_WORDS; ++word)
        {
            any_slice = any_slice || slice_mask[word] != 0u;
        }
        if (!any_slice)
        {
            slice_mask[0] = 1u;
        }
        mark_filter_neighborhood(cell, slice_mask);
        return;
    }

    if (avboitPass == 4)
    {
        if (imageLoad(avboitClassification, pixel).r != AVBOIT_CLASS_APPROXIMATE)
        {
            return;
        }
        for (uint node = head; node != OIT_NULL; node = oitNodes[node].next)
        {
            float alpha = clamp(oitNodes[node].color.a, 0.0, 1.0);
            if (oitNodes[node].blend != OIT_NULL && alpha > 0.0)
            {
                float optical_depth = -log(max(1.0 - alpha, 1.0 / 65536.0)) / 64.0;
                uint fixed_extinction = uint(optical_depth * 65536.0 + 0.5);
                float slice_coordinate = warped_slice(oitNodes[node].depth);
                uint lower_slice = uint(floor(slice_coordinate));
                uint upper_slice = min(lower_slice + 1u, AVBOIT_SLICES - 1u);
                uint upper_extinction = uint(
                    float(fixed_extinction) * fract(slice_coordinate) + 0.5);
                uint lower_extinction = fixed_extinction - upper_extinction;
                if (upper_slice == lower_slice)
                {
                    imageAtomicAdd(avboitExtinction, volume_coordinate(pixel, lower_slice),
                                   fixed_extinction);
                }
                else
                {
                    imageAtomicAdd(avboitExtinction, volume_coordinate(pixel, lower_slice),
                                   lower_extinction);
                    imageAtomicAdd(avboitExtinction, volume_coordinate(pixel, upper_slice),
                                   upper_extinction);
                }
            }
        }
        return;
    }

    vec4 opaque = texelFetch(diffuseRect, pixel, 0);
    uint classification = imageLoad(avboitClassification, pixel).r;
    uint shallow_nodes[AVBOIT_EXACT_SHALLOW_LIMIT];
    uint shallow_count = 0u;
    bool shallow_exact = classification != AVBOIT_CLASS_APPROXIMATE;
    if (shallow_exact)
    {
        for (uint node = head; node != OIT_NULL; node = oitNodes[node].next)
        {
            shallow_nodes[shallow_count++] = node;
        }
    }

    if (shallow_exact)
    {
        if (avboitDebugMode == 1 && head != OIT_NULL)
        {
            imageStore(avboitOutput, pixel, vec4(0.0, 1.0, 0.0, 0.0));
            return;
        }

        // Exact ordering is inexpensive for ordinary shallow lists and avoids
        // voxel quantization on hair cards, clothing, and similar surfaces.
        for (uint i = 1u; i < shallow_count; ++i)
        {
            uint value = shallow_nodes[i];
            uint position = i;
            while (position > 0u && node_comes_first(value, shallow_nodes[position - 1u]))
            {
                shallow_nodes[position] = shallow_nodes[position - 1u];
                --position;
            }
            shallow_nodes[position] = value;
        }

        vec4 exact_color = opaque;
        float exact_glow = opaque.a;
        for (uint i = 0u; i < shallow_count; ++i)
        {
            OITNode fragment = oitNodes[shallow_nodes[i]];
            if (fragment.blend == OIT_NULL)
            {
                exact_glow += fragment.glow;
            }
            else
            {
                float alpha = clamp(fragment.color.a, 0.0, 1.0);
                exact_color.rgb = fragment.color.rgb * alpha +
                    exact_color.rgb * (1.0 - alpha);
                exact_glow = fragment.glow + exact_glow * (1.0 - alpha);
            }
        }
        // Screen alpha is glow, not accumulated display opacity. In
        // particular, do not restore opaque glow after glass attenuates it.
        exact_color.a = exact_glow;
        imageStore(avboitOutput, pixel, max(exact_color, vec4(0.0)));
        return;
    }

    if (avboitDebugMode == 1)
    {
        imageStore(avboitOutput, pixel, vec4(1.0, 0.0, 1.0, 0.0));
        return;
    }

    vec3 weighted_color = vec3(0.0);
    float color_weight = 0.0;
    float glow = 0.0;
    float pixel_transmittance = 1.0;
    ivec2 volume_cell = clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1));
    uint zero_depth = imageLoad(avboitZeroTransmittanceDepth, volume_cell).r;
    for (uint node = head; node != OIT_NULL; node = oitNodes[node].next)
    {
        OITNode fragment = oitNodes[node];
        float fragment_slice = warped_slice(fragment.depth);
        if (zero_depth != 255u && fragment_slice > float(zero_depth))
        {
            if (fragment.blend != OIT_NULL)
            {
                pixel_transmittance *= 1.0 - clamp(fragment.color.a, 0.0, 1.0);
            }
            continue;
        }
        vec2 sample_xy = (vec2(pixel) + vec2(0.5)) / vec2(avboitViewport);
        // Linear sampling over a linear extinction splat needs a two-slice
        // camera-side bias so a surface does not attenuate itself.
        float sample_slice = clamp(
            fragment_slice - 2.0, 0.0, float(AVBOIT_SLICES - 1u));
        float sample_z = (sample_slice + 0.5) / float(AVBOIT_SLICES);
        float front_transmittance = texture(
            avboitTransmittanceSampler, vec3(sample_xy, sample_z)).r;
        if (fragment.blend == OIT_NULL)
        {
            glow += fragment.glow * front_transmittance;
        }
        else
        {
            float alpha = clamp(fragment.color.a, 0.0, 1.0);
            float weight = alpha * front_transmittance;
            weighted_color += fragment.color.rgb * weight;
            color_weight += weight;
            pixel_transmittance *= 1.0 - alpha;
            glow += fragment.glow * front_transmittance;
        }
    }

    float aggregate_alpha = 1.0 - pixel_transmittance;
    vec3 transparent = color_weight > 0.0 ?
        weighted_color * (aggregate_alpha / color_weight) : vec3(0.0);
    // Screen alpha carries glow. Opaque-scene glow is behind every captured
    // transparent fragment and must be attenuated like opaque color.
    glow += opaque.a * pixel_transmittance;
    imageStore(avboitOutput, pixel,
               max(vec4(transparent + opaque.rgb * pixel_transmittance, glow), vec4(0.0)));
}
