// AVBOIT emissive capture
/*[EXTRA_CODE_HERE]*/

layout(early_fragment_tests) in;
uniform int avboitRasterPass;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform vec2 avboitDepthRange;
uniform float avboitLinearization;
uniform float avboitSamplingBias;
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
layout(std430, binding = 5) buffer AVBOITWarp {
    uint avboitWarp[AVBOIT_VIRTUAL_SLICES]; };
layout(std430, binding = 6) buffer AVBOITTileOccupancy { uint avboitTileOccupancy[]; };
layout(std430, binding = 3) readonly buffer AVBOITWork { uint avboitWork[]; };
layout(location = 1) out vec4 avboitAccumulatedColorGlow;
layout(location = 2) out float avboitAccumulatedWeight;
layout(location = 3) out float avboitAccumulatedExtinction;
// Per-tile depth ranging. Must match avboitCaptureF.glsl exactly: emissive glow
// has to land in the same slices as the colour it belongs to.
uniform int avboitTileRange;
const int AVBOIT_RANGE_TILE = 16;
// Round 3: pass 1's sub-cell sample count (see FSAVBOIT::
// AVBOIT_PASS1_SUBSAMPLE). This shader's pass-1 branch returns immediately
// without using it -- declared for uniform-set consistency with
// avboitCaptureF.glsl, which every raster program shares uniform uploads
// with (FSAVBOIT::configureDirectRasterShader()).
uniform int avboitPass1Subsample;
// A9: per-pixel exact front-two-layer key. Must match avboitCaptureF.glsl's
// declarations exactly -- see doc/ayanestorm-oit-performance-audit-plan.md's
// A9 section for the full design.
uniform int avboitFrontLayers;
layout(binding = 0, r32ui) uniform coherent uimage2D avboitFrontKey0;
layout(binding = 1, r32ui) uniform coherent uimage2D avboitFrontKey1;
// Glass darkening fix: third and fourth nearest, distinct depth. See
// doc/ayanestorm-oit-avboit-glass-darkening.md.
layout(binding = 2, r32ui) uniform coherent uimage2D avboitFrontKey2;
layout(binding = 5, r32ui) uniform coherent uimage2D avboitFrontKey3;

uint avboit_emissive_proxy_bounds_offset()
{
    ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
    return 8u + 128u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) +
        uint(tile_count.x * tile_count.y) * 4u;
}

uint avboit_range_index(ivec2 full_res_pixel)
{
    ivec2 tile_count =
        max((avboitViewport + ivec2(AVBOIT_RANGE_TILE - 1)) /
                AVBOIT_RANGE_TILE,
            ivec2(1));
    ivec2 tile = clamp(full_res_pixel / AVBOIT_RANGE_TILE, ivec2(0),
                       tile_count - ivec2(1));
    return avboit_emissive_proxy_bounds_offset() +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) * 5u +
        (uint(tile.y) * uint(tile_count.x) + uint(tile.x)) * 2u;
}

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

// Round 9: never spread the 127 physical slices over less depth than the
// pass-1/pass-2 rasterizers can agree on -- see avboitCaptureF.glsl's
// identical constant for the full rationale.
const float AVBOIT_TILE_MIN_SPAN = 6.0e-4;

// True, with the padded [minimum_depth, minimum_depth + span] global-
// normalized range, when ranging is on and pass 0 wrote the tile containing
// full-resolution `pixel`. Must match avboitCaptureF.glsl's function of the
// same name exactly, including the padding.
bool avboit_tile_range(ivec2 pixel, out float minimum_depth, out float span)
{
    if (avboitTileRange == 0)
    {
        return false;
    }
    uint range = avboit_range_index(pixel);
    uint stored_minimum = avboitWork[range];
    uint stored_maximum = avboitWork[range + 1u];
    if (stored_minimum > stored_maximum)
    {
        return false;
    }
    minimum_depth = float(stored_minimum) / 16777215.0;
    float maximum_depth = float(stored_maximum) / 16777215.0;
    float pad = max((maximum_depth - minimum_depth) * 0.0625,
                    1.0 / 16777215.0);
    minimum_depth -= pad;
    maximum_depth += pad;
    span = max(maximum_depth - minimum_depth, 1.0 / 16777215.0);
    if (span < AVBOIT_TILE_MIN_SPAN)
    {
        minimum_depth -= (AVBOIT_TILE_MIN_SPAN - span) * 0.5;
        span = AVBOIT_TILE_MIN_SPAN;
    }
    return true;
}

// Round 8: window depth of this fragment's surface extrapolated to the
// centre of its own 8x8 volume cell, so a tilted surface cannot occlude
// itself within one cell once slices are thin (per-tile ranging). Must
// match avboitCaptureF.glsl's function of the same name. This pass is
// always full resolution (pass 1 returns immediately below without
// reaching pass 2), so `pixel` needs no cell-to-pixel scaling here.
float avboit_cell_centre_depth(vec2 cell_centre_fragcoord, float z,
                               float dz_dx, float dz_dy, float slope_limit)
{
    vec2 d = cell_centre_fragcoord - gl_FragCoord.xy;
    float dz = dz_dx * d.x + dz_dy * d.y;
    return clamp(z + clamp(dz, -slope_limit, slope_limit), 0.0, 1.0);
}

// Physical slices to back off in tile mode -- see avboitCaptureF.glsl's
// AVBOIT_TILE_BIAS_SLICES for the rationale (round 8).
const float AVBOIT_TILE_BIAS_SLICES = 2.0;

// Physical slice coordinate of a window depth for the tile containing
// `pixel`. This pass is always full resolution (pass 1 returns immediately
// below without reaching pass 2), so `pixel` needs no cell-to-pixel scaling.
// `window_depth` should already be the caller's cell-centre-projected depth
// when the tile is ranged.
float avboit_slice_for_pixel(ivec2 pixel, float window_depth)
{
    float minimum_depth, span;
    if (avboit_tile_range(pixel, minimum_depth, span))
    {
        float global_depth = avboit_global_normalized_depth(window_depth);
        return clamp((global_depth - minimum_depth) / span, 0.0, 1.0) *
            float(AVBOIT_DIRECT_SLICES - 1u);
    }
    return -1.0;
}
float avboit_biased_depth(float window_depth)
{
    float near_depth = max(avboitDepthRange.x, 0.0001);
    float far_depth = max(avboitDepthRange.y, near_depth + 0.0001);
    float ndc = clamp(window_depth, 0.0, 1.0) * 2.0 - 1.0;
    float depth = 2.0 * near_depth * far_depth /
        (far_depth + near_depth - ndc * (far_depth - near_depth));
    float scale = exp2(float(min(avboitWork[7], AVBOIT_MAX_DIVIDER)));
    float count = float(AVBOIT_VIRTUAL_SLICES) / scale;
    float a = avboitLinearization / scale;
    float coordinate = clamp(
        log2(depth / a + 1.0) / log2(far_depth / a + 1.0) * count -
            avboitSamplingBias,
        0.0, count - 1.0);
    float biased = a *
        (exp2(coordinate / count * log2(far_depth / a + 1.0)) - 1.0);
    float biased_ndc = (far_depth + near_depth -
        2.0 * near_depth * far_depth / max(biased, near_depth)) /
        (far_depth - near_depth);
    return clamp(biased_ndc * 0.5 + 0.5, 0.0, 1.0);
}
void avboit_store_glow(float glow)
{
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
        for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 neighbor = clamp(cell + ivec2(x, y), ivec2(0),
                                   avboitVolumeSize - ivec2(1));
            uint tile = (uint(neighbor.y) * uint(avboitVolumeSize.x) +
                         uint(neighbor.x)) * AVBOIT_DIRECT_OCCUPANCY_WORDS;
            atomicOr(avboitTileOccupancy[tile], 1u);
        }
        return;
    }
    if (avboitRasterPass == 1) return;
    if (avboitRasterPass == 2)
    {
        // Pass 2 is full resolution; `cell` above is unscaled (`clamp(pixel,
        // ...)`) since this shader's pass-1 branch below returns without
        // using it, so a texel fetch into the volume-resolution
        // transmittance texture needs its own /8 scale down to cell space.
        ivec2 transmittance_cell = clamp(pixel / 8, ivec2(0),
                                         avboitVolumeSize - ivec2(1));
        // Round 8: project to this cell's own centre depth before biasing,
        // so a tilted surface's own sub-samples elsewhere in the cell
        // cannot occlude this fragment; derivatives computed once in
        // uniform control flow.
        float fragment_z = gl_FragCoord.z;
        float dz_dx = dFdx(fragment_z);
        float dz_dy = dFdy(fragment_z);
        float slope_limit = fwidth(fragment_z) * 8.0;
        vec2 cell_centre_fragcoord = vec2(pixel / 8) * 8.0 + 4.0;
        float cell_centre_depth = avboit_cell_centre_depth(
            cell_centre_fragcoord, fragment_z, dz_dx, dz_dy, slope_limit);
        float front;
        float tile_slice = avboit_slice_for_pixel(pixel, cell_centre_depth);
        if (tile_slice >= 0.0)
        {
            tile_slice = max(tile_slice - AVBOIT_TILE_BIAS_SLICES, 0.0);
            uint lower = uint(floor(tile_slice));
            uint upper = min(lower + 1u, AVBOIT_DIRECT_SLICES - 1u);
            float a = texelFetch(avboitTransmittanceSampler,
                                 ivec3(transmittance_cell, int(lower)), 0).r;
            float b = texelFetch(avboitTransmittanceSampler,
                                 ivec3(transmittance_cell, int(upper)), 0).r;
            front = mix(a, b, fract(tile_slice));
            // See avboitCaptureF.glsl's identical floor: pass 1's one-sample-
            // per-cell extinction can saturate a fine per-tile slice range
            // exactly, reading 0 for glow that is not actually behind
            // anything the sampled core covered.
            front = max(front, 1.0 / 16384.0);
        }
        else
        {
        float virtual_coordinate = min(
            avboit_global_normalized_depth(avboit_biased_depth(gl_FragCoord.z)) *
                float(AVBOIT_VIRTUAL_SLICES),
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
        bool lower_filterable =
            (lower_entry & AVBOIT_WARP_FILTERABLE) != 0u;
        bool upper_filterable =
            (upper_entry & AVBOIT_WARP_FILTERABLE) != 0u;
        bool lower_range_end =
            (lower_entry & AVBOIT_WARP_RANGE_END) != 0u;
        bool upper_range_begin =
            (upper_entry & AVBOIT_WARP_RANGE_BEGIN) != 0u;
        float encoded_slice;
        if (lower_filterable && upper_filterable)
        {
            encoded_slice = mix(lower_coordinate, upper_coordinate,
                                fract(virtual_coordinate));
        }
        else if (lower_range_end)
        {
            encoded_slice = lower_coordinate;
        }
        else if (upper_range_begin)
        {
            encoded_slice = upper_coordinate;
        }
        else
        {
            encoded_slice =
                lower_filterable ? lower_coordinate : upper_coordinate;
        }
        float slice_coordinate = encoded_slice / 65536.0;
        vec2 sample_xy = (vec2(pixel) + vec2(0.5)) / vec2(avboitViewport);
        float sample_slice = slice_coordinate;
        front = texture(avboitTransmittanceSampler,
                        vec3(sample_xy, (sample_slice + 0.5) /
                            float(AVBOIT_DIRECT_SLICES))).r;
        }
        // A9: same front-key bound as avboitCaptureF.glsl's colour path.
        // Glow-only fragments (this shader) share their surface's depth
        // with its colour fragment, so a front-surface glow texel's
        // gl_FragCoord.z matches key0 exactly and gets front_factor = 1,
        // same as vanilla's unattenuated emissive.
        uint my_depth = uint(
            clamp(gl_FragCoord.z, 0.0, 1.0) * 16777215.0 + 0.5);
        uint key0 = imageLoad(avboitFrontKey0, pixel).r;
        uint key1 = imageLoad(avboitFrontKey1, pixel).r;
        uint key2 = imageLoad(avboitFrontKey2, pixel).r;
        uint key3 = imageLoad(avboitFrontKey3, pixel).r;
        float key_alpha0 = key0 == 0xffffffffu ?
            0.0 : float(key0 & 255u) / 255.0;
        float key_alpha1 = key1 == 0xffffffffu ?
            0.0 : float(key1 & 255u) / 255.0;
        float key_alpha2 = key2 == 0xffffffffu ?
            0.0 : float(key2 & 255u) / 255.0;
        float key_alpha3 = key3 == 0xffffffffu ?
            0.0 : float(key3 & 255u) / 255.0;
        float front_factor;
        if (avboitFrontLayers != 0 && key0 != 0xffffffffu &&
            my_depth == (key0 >> 8u))
        {
            front_factor = 1.0;
        }
        else if (avboitFrontLayers != 0 && key1 != 0xffffffffu &&
                 my_depth == (key1 >> 8u))
        {
            front_factor = 1.0 - key_alpha0;
        }
        else if (avboitFrontLayers != 0 && key2 != 0xffffffffu &&
                 my_depth == (key2 >> 8u))
        {
            front_factor = (1.0 - key_alpha0) * (1.0 - key_alpha1);
        }
        else if (avboitFrontLayers != 0 && key3 != 0xffffffffu &&
                 my_depth == (key3 >> 8u))
        {
            front_factor =
                (1.0 - key_alpha0) * (1.0 - key_alpha1) * (1.0 - key_alpha2);
        }
        else if (avboitFrontLayers != 0)
        {
            // Glow keeps the plain bound (no relative-volume-weight
            // refinement) -- see doc/ayanestorm-oit-avboit-glass-
            // darkening.md: not worth the extra reads for glow.
            front_factor = min(front, (1.0 - key_alpha0) * (1.0 - key_alpha1) *
                               (1.0 - key_alpha2) * (1.0 - key_alpha3));
        }
        else
        {
            front_factor = front;
        }
        avboitAccumulatedColorGlow =
            vec4(0.0, 0.0, 0.0, max(glow, 0.0) * front_factor);
        avboitAccumulatedWeight = 0.0;
        avboitAccumulatedExtinction = 0.0;
        return;
    }

}

in vec4 vertex_color;
in vec2 vary_texcoord0;

void main()
{
    avboit_store_glow(diffuseLookup(vary_texcoord0.xy).a * vertex_color.a);
}
