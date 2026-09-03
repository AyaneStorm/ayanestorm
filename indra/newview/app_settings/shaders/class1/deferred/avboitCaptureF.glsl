/**
 * Shared AVBOIT direct-raster fragment output implementation.
 */

layout(early_fragment_tests) in;
uniform int avboitRasterPass;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform vec2 avboitDepthRange;
uniform float avboitLinearization;
uniform sampler3D avboitTransmittanceSampler;
const uint AVBOIT_DIRECT_SLICES = 128u;
// Must match the compaction search range in avboitVolumeC.glsl.
const uint AVBOIT_MAX_DIVIDER = uint(AVBOIT_MAX_DIVIDER_VALUE);
const uint AVBOIT_DIRECT_OCCUPANCY_WORDS = AVBOIT_DIRECT_SLICES / 32u;
const uint AVBOIT_WARP_FILTERABLE = 0x80000000u;
const uint AVBOIT_WARP_RANGE_BEGIN = 0x40000000u;
const uint AVBOIT_WARP_RANGE_END = 0x20000000u;
const uint AVBOIT_WARP_RANGE_MIDDLE = 0x10000000u;
const uint AVBOIT_WARP_COORDINATE_MASK = 0x00ffffffu;
// Effective-zero extinction. The wide layout matches the full-resolution
// accumulated-extinction endpoint in avboit_direct_store, so the ordering
// volume and the final opacity saturate the same fragment identically.
uniform int avboitWideExtinction;
uniform int avboitDebugMode;
// Self-occlusion bias in virtual slices. The PDF specifies 2.0 for linear
// filtering, which assumes a slice is thick relative to the spacing between
// distinct surfaces. Adaptive compaction removes empty world depth, so two
// layers of one garment can land in adjacent slices; backing off 2.0 then
// samples from before the foreground layer and stops it attenuating the layer
// behind it. Exposed live so the magnitude can be measured against content.
uniform float avboitSamplingBias;
// Enables per-tile depth ranging. The global depth curve is shared by the whole
// frame, so a distant surface anywhere on screen coarsens the slice spacing for
// every pixel; per-tile ranging gives each tile the slices its own content needs.
uniform int avboitTileRange;
const float AVBOIT_DIRECT_ZERO_EXTINCTION_NARROW = 5.54126355;  // -log(1 / 255)
const float AVBOIT_DIRECT_ZERO_EXTINCTION_WIDE = 11.09035489;   // -log(1 / 65536)

float avboit_zero_extinction()
{
    return avboitWideExtinction != 0 ?
        AVBOIT_DIRECT_ZERO_EXTINCTION_WIDE :
        AVBOIT_DIRECT_ZERO_EXTINCTION_NARROW;
}

layout(binding = 3, r32ui) uniform coherent uimage3D avboitExtinction;
layout(binding = 7, r32ui) uniform coherent uimage2D avboitExtinctionOverflowDepth;
layout(std430, binding = 4) buffer AVBOITOccupancy {
    uint avboitOccupancy[AVBOIT_VIRTUAL_SLICES]; };
layout(std430, binding = 5) buffer AVBOITWarp {
    uint avboitWarp[AVBOIT_VIRTUAL_SLICES]; };
layout(std430, binding = 6) buffer AVBOITTileOccupancy { uint avboitTileOccupancy[]; };
layout(std430, binding = 7) buffer AVBOITDiagnostics { uint avboitDiagnostic[16]; };
layout(std430, binding = 3) buffer AVBOITWork { uint avboitWork[]; };
layout(location = 1) out vec4 avboitAccumulatedColorGlow;
layout(location = 2) out float avboitAccumulatedWeight;
layout(location = 3) out float avboitAccumulatedExtinction;

// Unwarped normalized depth. This is the global log curve, unchanged, and is
// what the per-tile reduction stores so the rescale below is monotonic in it.
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

float avboit_virtual_depth(float window_depth);

float avboit_warped_slice(float depth)
{
    float virtual_coordinate =
        min(avboit_virtual_depth(depth) * float(AVBOIT_VIRTUAL_SLICES),
            float(AVBOIT_VIRTUAL_SLICES - 1u));
    uint lower_virtual = uint(floor(virtual_coordinate));
    uint upper_virtual =
        min(lower_virtual + 1u, uint(AVBOIT_VIRTUAL_SLICES) - 1u);
    uint lower_entry = avboitWarp[lower_virtual];
    uint upper_entry = avboitWarp[upper_virtual];
    float lower_coordinate =
        float(lower_entry & AVBOIT_WARP_COORDINATE_MASK);
    float upper_coordinate =
        float(upper_entry & AVBOIT_WARP_COORDINATE_MASK);
    bool lower_filterable = (lower_entry & AVBOIT_WARP_FILTERABLE) != 0u;
    bool upper_filterable = (upper_entry & AVBOIT_WARP_FILTERABLE) != 0u;
    bool lower_range_end = (lower_entry & AVBOIT_WARP_RANGE_END) != 0u;
    bool upper_range_begin = (upper_entry & AVBOIT_WARP_RANGE_BEGIN) != 0u;
    if (lower_filterable && upper_filterable)
    {
        return mix(lower_coordinate, upper_coordinate,
                   fract(virtual_coordinate)) / 65536.0;
    }
    // Empty ranges are invariant. Their boundary markers select the occupied
    // endpoint and discard the interpolation fraction across empty depth.
    if (lower_range_end)
    {
        return lower_coordinate / 65536.0;
    }
    if (upper_range_begin)
    {
        return upper_coordinate / 65536.0;
    }
    return (lower_filterable ? lower_coordinate : upper_coordinate) / 65536.0;
}

uint avboit_tile_index(ivec2 cell, uint word)
{
    return (uint(cell.y) * uint(avboitVolumeSize.x) + uint(cell.x)) *
        AVBOIT_DIRECT_OCCUPANCY_WORDS + word;
}

uint avboit_proxy_bounds_offset()
{
    ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
    return 8u + 128u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) +
        uint(tile_count.x * tile_count.y) * 4u;
}

uint avboit_dilated_proxy_bounds_offset()
{
    return avboit_proxy_bounds_offset() +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) * 2u;
}

// Screen-space tile grid used for per-tile depth ranging. Must match the grid
// in avboitVolumeC.glsl and the tile count allocated in fsavboit.cpp.
const int AVBOIT_RANGE_TILE = 16;

// Per-tile depth range, two uint depth keys per tile, appended after the
// existing proxy-bounds region of the work buffer.
uint avboit_tile_range_offset()
{
    return avboit_proxy_bounds_offset() +
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

// Rescales the global normalized depth into the tile's own occupied range, so
// the 128 physical slices span only the depth actually present in that tile
// rather than the whole frame. This is the resolution fix: a tile containing two
// close clothing layers spends all of its slices between them.
float avboit_virtual_depth(float window_depth)
{
    float global_depth = avboit_global_normalized_depth(window_depth);
    if (avboitTileRange == 0)
    {
        return global_depth;
    }
    uint range = avboit_range_index(ivec2(gl_FragCoord.xy));
    uint stored_minimum = avboitWork[range];
    uint stored_maximum = avboitWork[range + 1u];
    // An unwritten tile keeps the global curve: pass 0 found no transparency
    // there, so nothing reads the rescaled coordinate anyway.
    if (stored_minimum > stored_maximum)
    {
        return global_depth;
    }
    float minimum_depth = float(stored_minimum) / 16777215.0;
    float maximum_depth = float(stored_maximum) / 16777215.0;
    // Pad the range so the frontmost and rearmost surfaces do not sit exactly on
    // the domain boundaries, where clamping would merge them with a neighbour.
    // The pad is relative, costing a fixed fraction of the resolution however
    // narrow the range is.
    float span = maximum_depth - minimum_depth;
    float pad = max(span * 0.0625, 1.0 / 16777215.0);
    minimum_depth -= pad;
    maximum_depth += pad;
    span = max(maximum_depth - minimum_depth, 1.0 / 16777215.0);
    return clamp((global_depth - minimum_depth) / span, 0.0, 1.0);
}

// Reduces one transparent fragment into its tile's depth range. Raster pass 0
// only, which visits every transparent fragment.
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

void avboit_compare_proxy_coverage(ivec2 cell, uint virtual_slice)
{
    uint linear_cell =
        uint(cell.y * avboitVolumeSize.x + cell.x);
    uint interval =
        avboit_dilated_proxy_bounds_offset() + linear_cell * 2u;
    uint minimum_slice = avboitWork[interval];
    uint maximum_slice = avboitWork[interval + 1u];
    atomicAdd(avboitDiagnostic[4], 1u);
    uint failure = minimum_slice == 0xffffffffu ? 1u :
        (virtual_slice < minimum_slice ? 2u :
         (virtual_slice > maximum_slice ? 4u : 0u));
    if (failure != 0u)
    {
        atomicAdd(avboitDiagnostic[5], 1u);
        uint miss_map = avboit_proxy_bounds_offset() +
            uint(avboitVolumeSize.x * avboitVolumeSize.y) * 4u;
        atomicOr(avboitWork[miss_map + linear_cell], failure);
    }
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
    bool wide = avboitWideExtinction != 0;
    // Lanes per word and their maximum representable value. The narrow layout
    // is the presentation's four 8-bit lanes; the wide layout trades scratch
    // memory for a quantum small enough that sheer viewer alpha survives the
    // two-slice splat that halves every contribution before rounding.
    uint lanes_per_word = wide ? 2u : 4u;
    uint lane_mask = wide ? 0xffffu : 0xffu;
    float lane_scale = float(lane_mask);

    uint value = uint(clamp(
        optical_depth / avboit_zero_extinction() * lane_scale,
        0.0, lane_scale) + 0.5);
    if (value == 0u)
    {
        return;
    }

    uint lane = slice_index % lanes_per_word;
    uint shift = lane * (32u / lanes_per_word);
    ivec3 coordinate = ivec3(cell, int(slice_index / lanes_per_word));

    // Saturating accumulation. A plain atomic add reads a stale previous value
    // under contention, so a lane could wrap past its maximum with no thread
    // observing the crossing: a dense surface then stored a near-zero value and
    // its carry corrupted the adjacent slice's lane. Because the winner depends
    // on rasterization order, that also made transmittance camera-dependent.
    // Compare-and-swap clamps at the maximum instead and records overflow from
    // whichever thread actually reaches it.
    uint expected = imageLoad(avboitExtinction, coordinate).r;
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        uint lane_value = (expected >> shift) & lane_mask;
        if (lane_value == lane_mask)
        {
            imageAtomicMin(avboitExtinctionOverflowDepth, cell, slice_index);
            return;
        }
        uint summed = min(lane_value + value, lane_mask);
        uint desired = (expected & ~(lane_mask << shift)) | (summed << shift);
        uint observed = imageAtomicCompSwap(
            avboitExtinction, coordinate, expected, desired);
        if (observed == expected)
        {
            if (summed == lane_mask)
            {
                imageAtomicMin(
                    avboitExtinctionOverflowDepth, cell, slice_index);
            }
            return;
        }
        expected = observed;
    }
    // Extreme contention on one lane means it is effectively saturated.
    imageAtomicMin(avboitExtinctionOverflowDepth, cell, slice_index);
}

void avboit_direct_store(vec4 color)
{
    float alpha = clamp(color.a, 0.0, 1.0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 cell = avboitRasterPass == 0 ?
        clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1)) :
        clamp(pixel, ivec2(0), avboitVolumeSize - ivec2(1));
    // A2: pass 1's hardware early_fragment_tests now rejects against the
    // correct per-cell farthest opaque depth (see avboitCellDepthF.glsl and
    // FSAVBOIT::finishDirectOccupancy()), so a fragment that reaches this
    // point in pass 1 has already survived that test -- no manual re-test
    // needed.
    if (avboitRasterPass == 0)
    {
        // avboit_direct_store never carries glow (glow accumulates through
        // avboit_store_glow() in the emissive/PBR-glow shaders instead), so
        // the original "alpha > 0.0 || oitGlow > 0.0" mark-tile test reduces
        // to alpha > 0.0 -- the same condition already gating the block
        // below, so both collapse into one.
        if (alpha > 0.0)
        {
            avboit_mark_tile(cell);
            // Reduce this fragment into its tile's depth range. The range is
            // only complete once every pass-0 fragment has been processed, so
            // occupancy below must use the global curve rather than the
            // per-tile rescale, which would read a partially reduced range.
            avboit_reduce_tile_range(gl_FragCoord.z);
            uint virtual_slice = min(uint(
                                         avboit_global_normalized_depth(
                                             gl_FragCoord.z) *
                                         float(AVBOIT_VIRTUAL_SLICES)),
                                     uint(AVBOIT_VIRTUAL_SLICES) - 1u);
            // Retain the material path while measuring whether conservative
            // proxy intervals cover every alpha-tested occupancy sample.
            // Diagnostic-only (feeds debug mode 6); skip the atomics otherwise.
            if (avboitDebugMode == 6)
            {
                avboit_compare_proxy_coverage(cell, virtual_slice);
            }
            atomicOr(avboitOccupancy[virtual_slice], 1u);
        }
        return;
    }

    float slice_coordinate = avboit_warped_slice(gl_FragCoord.z);
    if (avboitRasterPass == 1)
    {
        if (alpha > 0.0)
        {
            // Match the accumulated-extinction endpoint used by resolve so an
            // opaque fragment does not saturate the ordering volume 257 times
            // earlier than it saturates final opacity.
            float optical_depth = avboitWideExtinction != 0 ?
                -log(max(1.0 - alpha, 1.0 / 65536.0)) :
                -log(max(1.0 - alpha, 1.0 / 255.0));
            uint lower_slice = uint(floor(slice_coordinate));
            uint upper_slice = min(lower_slice + 1u, AVBOIT_DIRECT_SLICES - 1u);
            float upper_extinction = optical_depth * fract(slice_coordinate);
            float lower_extinction = optical_depth - upper_extinction;
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
        vec2 sample_xy = (vec2(pixel) + vec2(0.5)) / vec2(avboitViewport);
        uint curve_shift = min(avboitWork[7], AVBOIT_MAX_DIVIDER);
        float curve_scale = exp2(float(curve_shift));
        float reduced_count =
            float(AVBOIT_VIRTUAL_SLICES) / curve_scale;
        float reduced_linearization = avboitLinearization / curve_scale;
        float near_depth = max(avboitDepthRange.x, 0.0001);
        float far_depth = max(avboitDepthRange.y, near_depth + 0.0001);
        float ndc_depth = clamp(gl_FragCoord.z, 0.0, 1.0) * 2.0 - 1.0;
        float linear_depth = 2.0 * near_depth * far_depth /
            (far_depth + near_depth -
             ndc_depth * (far_depth - near_depth));
        float reduced_coordinate = clamp(
            log2(linear_depth / reduced_linearization + 1.0) /
                log2(far_depth / reduced_linearization + 1.0) *
                reduced_count - avboitSamplingBias,
            0.0, reduced_count - 1.0);
        float biased_depth = reduced_linearization *
            (exp2(reduced_coordinate / reduced_count *
                  log2(far_depth / reduced_linearization + 1.0)) - 1.0);
        float biased_window_depth = clamp(
            ((far_depth + near_depth -
              2.0 * near_depth * far_depth /
                  max(biased_depth, near_depth)) /
             (far_depth - near_depth)) * 0.5 + 0.5,
            0.0, 1.0);
        // Integration stores, for each physical slice, the transmittance after
        // that slice's extinction has been added (the post-slice phase the PDF
        // specifies), and the virtual-domain bias applied above is the offset
        // the specification defines against that phase.
        //
        // A one-physical-slice backoff was tried here and reverted. It did make
        // frontmost surfaces stop attenuating themselves, but it could not fix
        // layered garments: adaptive compaction assigns one physical slice per
        // occupancy group, so two surfaces millimetres apart differ only by
        // fract() within a single slice. Backing off a whole slice then samples
        // from before the foreground layer rather than between the two, which
        // is the same empty-space violation v119 removed.
        float sample_slice = avboit_warped_slice(biased_window_depth);
        float front_transmittance = texture(
            avboitTransmittanceSampler,
            vec3(sample_xy, (sample_slice + 0.5) /
                float(AVBOIT_DIRECT_SLICES))).r;

        // Remove this surface's own contribution from the transmittance it
        // reads. Pass 1 splatted optical_depth linearly across the slice pair
        // straddling slice_coordinate, giving the lower slice the fraction
        // (1 - fract). The integral sampled here therefore already contains
        // this surface's own extinction, so without correction every surface
        // attenuates itself.
        //
        // This is the discrete form of the specified -2 sampling bias. That
        // bias avoids self-occlusion by stepping back in depth, but adaptive
        // compaction can collapse the step to a fraction of a physical slice
        // (see the v128 result), at which point stepping back either fails to
        // clear the surface or clears the whole foreground layer. Subtracting
        // the known self-contribution achieves the intended result exactly and
        // is independent of how far apart two surfaces happen to be.
        //
        // Note this cannot recover the full ordering when two surfaces share a
        // slice: linear splatting also places a rear surface's extinction into
        // the texel a front surface reads, and the summed integral does not
        // record which part came from behind. The correction removes only the
        // self term, which is the part a fragment can know.
        // Only subtract while the sampled texel is still the one this surface
        // splatted into. The virtual-domain bias is normally a fraction of a
        // physical slice so the two coincide, but if it ever grows past a slice
        // the read moves in front of this surface, the self term is already
        // absent, and removing it again would brighten the surface incorrectly.
        float own_optical_depth =
            -log(max(1.0 - alpha, 1.0 / 65536.0));
        float read_overlap = clamp(
            1.0 - (floor(slice_coordinate) - floor(sample_slice)),
            0.0, 1.0);
        float own_share = own_optical_depth *
            (1.0 - fract(slice_coordinate)) * read_overlap;
        front_transmittance = clamp(
            front_transmittance * exp(own_share), 0.0, 1.0);

        float weight = alpha * front_transmittance;
        // Glow accumulates separately through avboit_store_glow() in the
        // emissive/PBR-glow shaders; this path never carries any.
        avboitAccumulatedColorGlow =
            vec4(max(color.rgb, vec3(0.0)) * weight, 0.0);
        avboitAccumulatedWeight = weight;
        avboitAccumulatedExtinction =
            -log(max(1.0 - alpha, 1.0 / 65536.0));
        // Diagnostic 14 replaces the color sum with the sampled front
        // transmittance itself, weighted the same way, so resolve can display
        // the average T_front actually used for ordering. A rear layer behind a
        // 0.95-alpha garment must report a low value; if it reports a high one,
        // the volume lookup is wrong rather than the averaging.
        if (avboitDebugMode == 14)
        {
            avboitAccumulatedColorGlow =
                vec4(vec3(front_transmittance) * weight, 0.0);
        }
    }
}

void avboit_store(vec4 color)
{
    avboit_direct_store(color);
}
