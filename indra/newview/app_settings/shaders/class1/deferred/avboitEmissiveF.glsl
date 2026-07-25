// AVBOIT emissive capture
/*[EXTRA_CODE_HERE]*/

layout(early_fragment_tests) in;
uniform int avboitRasterPass;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform vec2 avboitDepthRange;
uniform sampler3D avboitTransmittanceSampler;
uniform sampler2D avboitOpaqueDepthSampler;
layout(std430, binding = 2) readonly buffer AVBOITNearestTransparent
{
    uint avboitNearestTransparent[];
};
const uint AVBOIT_DIRECT_SLICES = 128u;
const uint AVBOIT_DIRECT_OCCUPANCY_WORDS = AVBOIT_DIRECT_SLICES / 32u;
const uint AVBOIT_WARP_FILTERABLE = 0x80000000u;
const uint AVBOIT_WARP_RANGE_BEGIN = 0x40000000u;
const uint AVBOIT_WARP_RANGE_END = 0x20000000u;
const uint AVBOIT_WARP_RANGE_MIDDLE = 0x10000000u;
const uint AVBOIT_WARP_COORDINATE_MASK = 0x00ffffffu;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
layout(std430, binding = 5) buffer AVBOITWarp { uint avboitWarp[8192]; };
layout(std430, binding = 6) buffer AVBOITTileOccupancy { uint avboitTileOccupancy[]; };
layout(location = 1) out vec4 avboitAccumulatedColorGlow;
layout(location = 2) out float avboitAccumulatedWeight;
layout(location = 3) out float avboitAccumulatedExtinction;
float avboit_virtual_depth(float window_depth)
{
    float near_depth = max(avboitDepthRange.x, 0.0001);
    float far_depth = max(avboitDepthRange.y, near_depth + 0.0001);
    float ndc_depth = clamp(window_depth, 0.0, 1.0) * 2.0 - 1.0;
    float linear_depth = 2.0 * near_depth * far_depth /
        (far_depth + near_depth -
         ndc_depth * (far_depth - near_depth));
    const float linearization = 16384.0;
    return clamp(log2(linear_depth / linearization + 1.0) /
                 log2(far_depth / linearization + 1.0), 0.0, 1.0);
}
float avboit_warped_slice(float depth)
{
    float virtual_coordinate =
        min(avboit_virtual_depth(depth) * 8192.0, 8191.0);
    uint lower_virtual = uint(floor(virtual_coordinate));
    uint upper_virtual = min(lower_virtual + 1u, 8191u);
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
    if (lower_filterable && upper_filterable)
    {
        return mix(lower_coordinate, upper_coordinate,
                   fract(virtual_coordinate)) / 65536.0;
    }
    if (lower_range_end)
    {
        return lower_coordinate / 65536.0;
    }
    if (upper_range_begin)
    {
        return upper_coordinate / 65536.0;
    }
    return (lower_filterable ? lower_coordinate : upper_coordinate) /
        65536.0;
}
uint avboit_conservative_zero_depth(ivec2 pixel)
{
    ivec2 base_cell = (pixel / 16) * 2;
    uint zero_depth = 0u;
    for (int y = 0; y <= 1; ++y)
    for (int x = 0; x <= 1; ++x)
    {
        ivec2 sample_cell = clamp(base_cell + ivec2(x, y), ivec2(0),
                                  avboitVolumeSize - ivec2(1));
        zero_depth = max(
            zero_depth,
            imageLoad(avboitZeroTransmittanceDepth, sample_cell).r);
    }
    return zero_depth;
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
            avboit_virtual_depth(gl_FragCoord.z) * 8192.0, 8191.0);
        uint lower_virtual = uint(floor(virtual_coordinate));
        uint upper_virtual = min(lower_virtual + 1u, 8191u);
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
        // Match color capture at the emitting surface.
        float sample_slice = clamp(
            slice_coordinate, 0.0, float(AVBOIT_DIRECT_SLICES - 1u));
        float front = texture(avboitTransmittanceSampler,
                              vec3(sample_xy, (sample_slice + 0.5) /
                                  float(AVBOIT_DIRECT_SLICES))).r;
        uint nearest = avboitNearestTransparent[
            uint(pixel.y * avboitViewport.x + pixel.x)];
        uint surface_depth24 =
            uint(clamp(gl_FragCoord.z, 0.0, 1.0) * 16777215.0 + 0.5);
        uint nearest_depth24 = nearest >> 8u;
        if (nearest != 0xffffffffu &&
            surface_depth24 > nearest_depth24 + 1u)
        {
            float nearest_alpha =
                float(255u - (nearest & 255u)) / 255.0;
            front = min(front, 1.0 - nearest_alpha);
        }
        front = clamp(front, 0.0, 1.0);
        avboitAccumulatedColorGlow =
            vec4(0.0, 0.0, 0.0, max(glow, 0.0) * front);
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
