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
// Per-tile depth ranging. Must match avboitCaptureF.glsl exactly: this pass's
// atomicMin/atomicMax and the capture shaders' read of the same words have to
// agree on both the offset formula and the depth encoding.
uniform int avboitTileRange;
const int AVBOIT_RANGE_TILE = 16;

uint avboit_bounds_offset()
{
    ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
    return 8u + 128u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) +
        uint(tile_count.x * tile_count.y) * 4u;
}

// Unwarped normalized depth, identical to avboitCaptureF.glsl's function of
// the same name -- the per-tile reduction stores this curve's coordinate, not
// the linear or virtual-bin one this file otherwise uses.
float avboit_global_normalized_depth(float window_depth)
{
    float near_depth = max(avboitDepthRange.x, 0.0001);
    float far_depth = max(avboitDepthRange.y, near_depth + 0.0001);
    float ndc_depth = clamp(window_depth, 0.0, 1.0) * 2.0 - 1.0;
    float linear_depth = 2.0 * near_depth * far_depth /
        (far_depth + near_depth -
         ndc_depth * (far_depth - near_depth));
    return clamp(log2(linear_depth / avboitLinearization + 1.0) /
                 log2(far_depth / avboitLinearization + 1.0), 0.0, 1.0);
}

// Per-tile depth range, two uint depth keys per tile, appended after the
// proxy-bounds region of the work buffer. Must match avboitCaptureF.glsl and
// avboitVolumeC.glsl's own copies of this offset formula exactly.
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

uint avboit_range_index(ivec2 full_res_pixel)
{
    ivec2 tile_count = avboit_range_tile_count();
    ivec2 tile = clamp(full_res_pixel / AVBOIT_RANGE_TILE, ivec2(0),
                       tile_count - ivec2(1));
    return avboit_tile_range_offset() +
        (uint(tile.y) * uint(tile_count.x) + uint(tile.x)) * 2u;
}

// Reduces one covered fragment into its tile's depth range. gl_FragCoord.xy
// is always a full-resolution pixel in this pass (unlike avboitCaptureF.glsl's
// pass 1, which rasterizes at volume/cell resolution).
void avboit_reduce_tile_range(float window_depth)
{
    if (avboitTileRange == 0)
    {
        return;
    }
    uint key = uint(clamp(avboit_global_normalized_depth(window_depth),
                          0.0, 1.0) * 16777215.0);
    uint range = avboit_range_index(ivec2(gl_FragCoord.xy));
    atomicMin(avboitWork[range], key);
    atomicMax(avboitWork[range + 1u], key);
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
        // Exact-proxy geometry has no alpha test, so its per-pixel coverage
        // is a superset of what the material-tested capture passes will
        // actually draw -- a tile's range only ever comes out wider than
        // strictly necessary here, never wrong.
        avboit_reduce_tile_range(bounded_window_depth);
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
