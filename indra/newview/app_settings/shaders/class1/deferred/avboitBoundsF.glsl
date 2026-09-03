/**
 * Reduce proxy-covered logarithmic depth intervals per 8x8 AVBOIT cell.
 */

layout(std430, binding = 3) buffer AVBOITWork
{
    uint avboitWork[];
};
layout(std430, binding = 4) buffer AVBOITOccupancy
{
    uint avboitOccupancy[AVBOIT_VIRTUAL_SLICES];
};

uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform vec2 avboitDepthRange;
uniform float avboitLinearization;
uniform sampler2D avboitOpaqueDepthSampler;
uniform vec2 avboitProxyDepthInterval;
uniform int avboitExactProxy;

uint avboit_bounds_offset()
{
    ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
    return 8u + 128u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) +
        uint(tile_count.x * tile_count.y) * 4u;
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
        log2(linear_depth / avboitLinearization + 1.0) /
        log2(far_depth / avboitLinearization + 1.0);
    return min(
        uint(clamp(coordinate, 0.0, 1.0) * float(AVBOIT_VIRTUAL_SLICES)),
        uint(AVBOIT_VIRTUAL_SLICES) - 1u);
}

uint avboit_virtual_bin_from_linear(float linear_depth)
{
    float far_depth = max(avboitDepthRange.y, 0.0001);
    float coordinate =
        log2(max(linear_depth, avboitDepthRange.x) /
             avboitLinearization + 1.0) /
        log2(far_depth / avboitLinearization + 1.0);
    return min(
        uint(clamp(coordinate, 0.0, 1.0) * float(AVBOIT_VIRTUAL_SLICES)),
        uint(AVBOIT_VIRTUAL_SLICES) - 1u);
}

void main()
{
    ivec2 cell = clamp(ivec2(gl_FragCoord.xy) / 8, ivec2(0),
                            avboitVolumeSize - ivec2(1));
    uint linear_cell =
        uint(cell.y * avboitVolumeSize.x + cell.x);
    uint interval = avboit_bounds_offset() + linear_cell * 2u;
    // gl_FragCoord.xy already addresses the pixel center. Adding another
    // half pixel shifts distant thin proxies onto neighboring opaque texels.
    vec2 opaque_uv = gl_FragCoord.xy / vec2(avboitViewport);
    float opaque_depth = texture(avboitOpaqueDepthSampler, opaque_uv).r;
    if (avboitExactProxy != 0 && gl_FragCoord.z > opaque_depth)
    {
        return;
    }
    // The proxy interval and material prepass share the same conservative
    // opaque-depth bound; proxy depth beyond it is harmlessly clamped.
    float bounded_window_depth = min(gl_FragCoord.z, opaque_depth);
    // Every touched cell receives the complete CPU AABB depth interval.
    // Surface-fragment depth is not conservative: only a far-facing cube
    // surface may cover a cell even though material exists near its front.
    uint exact_bin = avboit_virtual_bin(bounded_window_depth);
    if (avboitExactProxy != 0)
    {
        atomicOr(avboitOccupancy[exact_bin], 1u);
    }
    uint minimum_bin = avboitExactProxy != 0 ? exact_bin :
        avboit_virtual_bin_from_linear(avboitProxyDepthInterval.x);
    uint maximum_bin = avboitExactProxy != 0 ? exact_bin :
        avboit_virtual_bin_from_linear(avboitProxyDepthInterval.y);
    atomicMin(avboitWork[interval],
              minimum_bin > 0u ? minimum_bin - 1u : 0u);
    atomicMax(avboitWork[interval + 1u],
              min(maximum_bin + 1u, uint(AVBOIT_VIRTUAL_SLICES) - 1u));
}
