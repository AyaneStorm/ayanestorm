/**
 * Reduce proxy-covered logarithmic depth intervals per 8x8 AVBOIT cell.
 */

layout(std430, binding = 3) buffer AVBOITWork
{
    uint avboitWork[];
};

uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform vec2 avboitDepthRange;
uniform sampler2D avboitOpaqueDepthSampler;
uniform int avboitEntityID;

const uint AVBOIT_ENTITY_MASK_WORDS = 8u;

uint avboit_zbin_offset()
{
    ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
    return 8u + 128u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) +
        uint(tile_count.x * tile_count.y) * 4u;
}

uint avboit_bounds_offset()
{
    return avboit_zbin_offset() + 8192u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) *
            AVBOIT_ENTITY_MASK_WORDS;
}

uint avboit_entity_mask_offset()
{
    return avboit_zbin_offset() + 8192u;
}

float avboit_linear_depth(float window_depth)
{
    float near_depth = max(avboitDepthRange.x, 0.0001);
    float far_depth = max(avboitDepthRange.y, near_depth + 0.0001);
    float ndc_depth = window_depth * 2.0 - 1.0;
    return (2.0 * near_depth * far_depth) /
        max(far_depth + near_depth -
            ndc_depth * (far_depth - near_depth), 0.0001);
}

uint avboit_virtual_bin(float window_depth)
{
    float linear_depth = avboit_linear_depth(window_depth);
    float far_depth = max(avboitDepthRange.y, 0.0001);
    float coordinate =
        log2(linear_depth / 16384.0 + 1.0) /
        log2(far_depth / 16384.0 + 1.0);
    return min(uint(clamp(coordinate, 0.0, 1.0) * 8191.0), 8191u);
}

void main()
{
    ivec2 cell = clamp(ivec2(gl_FragCoord.xy) / 8, ivec2(0),
                            avboitVolumeSize - ivec2(1));
    uint linear_cell =
        uint(cell.y * avboitVolumeSize.x + cell.x);
    uint interval = avboit_bounds_offset() + linear_cell * 2u;
    vec2 opaque_uv =
        (gl_FragCoord.xy + vec2(0.5)) / vec2(avboitViewport);
    float opaque_depth = texture(avboitOpaqueDepthSampler, opaque_uv).r;
    // The proxy interval and material prepass share the same conservative
    // opaque-depth bound; proxy depth beyond it is harmlessly clamped.
    float bounded_window_depth = min(gl_FragCoord.z, opaque_depth);
    float linear_depth = avboit_linear_depth(bounded_window_depth);
    float zbin_coordinate =
        (linear_depth - avboitDepthRange.x) /
        max(avboitDepthRange.y - avboitDepthRange.x, 0.0001);
    uint zbin = min(
        uint(clamp(zbin_coordinate, 0.0, 0.99999994) * 8192.0),
        8191u);
    uint packed_range = avboitWork[avboit_zbin_offset() + zbin];
    uint entity_id = uint(max(avboitEntityID, 0));
    uint minimum_id = packed_range & 0xffffu;
    uint maximum_id = packed_range >> 16u;
    if (minimum_id == 0xffffu ||
        entity_id < minimum_id || entity_id > maximum_id)
    {
        return;
    }
    // Bit 255 is the conservative overflow bucket for IDs beyond the fixed
    // 256-bit portable mask budget.
    uint mask_entity = min(entity_id, 255u);
    uint mask_address = avboit_entity_mask_offset() +
        linear_cell * AVBOIT_ENTITY_MASK_WORDS +
        (mask_entity >> 5u);
    atomicOr(avboitWork[mask_address],
             1u << (mask_entity & 31u));
    uint depth_bin = avboit_virtual_bin(bounded_window_depth);
    atomicMin(avboitWork[interval], depth_bin);
    atomicMax(avboitWork[interval + 1u], depth_bin);
}
