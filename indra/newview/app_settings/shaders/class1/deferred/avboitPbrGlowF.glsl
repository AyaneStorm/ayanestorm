// AVBOIT PBR glow capture
/*[EXTRA_CODE_HERE]*/

uniform sampler2D diffuseMap;
uniform vec3 emissiveColor;
uniform sampler2D emissiveMap;
uniform float minimum_alpha;
in vec4 vertex_emissive;
in vec2 base_color_texcoord;
in vec2 emissive_texcoord;
vec3 srgb_to_linear(vec3 c);

layout(early_fragment_tests) in;
uniform int avboitRasterPass;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform vec2 avboitDepthRange;
uniform float avboitLinearization;
uniform float avboitSamplingBias;
uniform sampler3D avboitTransmittanceSampler;
uniform sampler2D avboitOpaqueDepthSampler;
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
// Per-tile depth ranging. Must match avboitCaptureF.glsl exactly: glow has to
// land in the same slices as the colour it belongs to.
uniform int avboitTileRange;
const int AVBOIT_RANGE_TILE = 16;

uint avboit_glow_proxy_bounds_offset()
{
    ivec2 tile_count = (avboitViewport + ivec2(15)) / 16;
    return 8u + 128u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) +
        uint(tile_count.x * tile_count.y) * 4u +
        uint(AVBOIT_VIRTUAL_SLICES) * uint(AVBOIT_ZBIN_LEVELS) +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) * 8u;
}

uint avboit_range_index(ivec2 full_res_pixel)
{
    ivec2 tile_count =
        max((avboitViewport + ivec2(AVBOIT_RANGE_TILE - 1)) /
                AVBOIT_RANGE_TILE,
            ivec2(1));
    ivec2 tile = clamp(full_res_pixel / AVBOIT_RANGE_TILE, ivec2(0),
                       tile_count - ivec2(1));
    return avboit_glow_proxy_bounds_offset() +
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
    if (stored_minimum > stored_maximum)
    {
        return global_depth;
    }
    float minimum_depth = float(stored_minimum) / 16777215.0;
    float maximum_depth = float(stored_maximum) / 16777215.0;
    float span = maximum_depth - minimum_depth;
    float pad = max(span * 0.0625, 1.0 / 16777215.0);
    minimum_depth -= pad;
    maximum_depth += pad;
    span = max(maximum_depth - minimum_depth, 1.0 / 16777215.0);
    return clamp((global_depth - minimum_depth) / span, 0.0, 1.0);
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
    if (avboitRasterPass == 1)
    {
        float farthest_depth = 0.0;
        ivec2 base_pixel = cell * 8;
        for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            ivec2 sample_pixel = min(
                base_pixel + ivec2(x, y), avboitViewport - ivec2(1));
            farthest_depth = max(
                farthest_depth,
                texelFetch(avboitOpaqueDepthSampler, sample_pixel, 0).r);
        }
        if (gl_FragCoord.z > farthest_depth) return;
    }
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
        float virtual_coordinate = min(
            avboit_virtual_depth(avboit_biased_depth(gl_FragCoord.z)) *
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
        float front = texture(avboitTransmittanceSampler,
                              vec3(sample_xy, (sample_slice + 0.5) /
                                  float(AVBOIT_DIRECT_SLICES))).r;
        avboitAccumulatedColorGlow =
            vec4(0.0, 0.0, 0.0, max(glow, 0.0) * front);
        avboitAccumulatedWeight = 0.0;
        avboitAccumulatedExtinction = 0.0;
        return;
    }

}

void main()
{
    vec4 basecolor = texture(diffuseMap, base_color_texcoord);
    if (basecolor.a < minimum_alpha) discard;
    vec3 emissive = emissiveColor * srgb_to_linear(texture(emissiveMap, emissive_texcoord).rgb);
    avboit_store_glow(max(max(emissive.r, emissive.g), emissive.b) * vertex_emissive.a);
}
