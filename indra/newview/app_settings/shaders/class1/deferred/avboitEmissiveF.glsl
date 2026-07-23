// AVBOIT emissive capture
/*[EXTRA_CODE_HERE]*/

layout(early_fragment_tests) in;
uniform int avboitRasterPass;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform vec2 avboitDepthRange;
uniform sampler3D avboitTransmittanceSampler;
const uint AVBOIT_DIRECT_SLICES = 128u;
const uint AVBOIT_DIRECT_OCCUPANCY_WORDS = AVBOIT_DIRECT_SLICES / 32u;
const uint AVBOIT_WARP_FILTERABLE = 0x80000000u;
const uint AVBOIT_WARP_COORDINATE_MASK = 0x00ffffffu;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
layout(std430, binding = 5) buffer AVBOITWarp { uint avboitWarp[8192]; };
layout(std430, binding = 6) buffer AVBOITTileOccupancy { uint avboitTileOccupancy[]; };
layout(std430, binding = 7) buffer AVBOITDirectAccumulation { uint avboitDirectAccumulation[]; };
float avboit_virtual_depth(float window_depth)
{
    float near_depth = max(avboitDepthRange.x, 0.0001);
    float far_depth = max(avboitDepthRange.y, near_depth + 0.0001);
    float ndc_depth = clamp(window_depth, 0.0, 1.0) * 2.0 - 1.0;
    float linear_depth = 2.0 * near_depth * far_depth /
        (far_depth + near_depth -
         ndc_depth * (far_depth - near_depth));
    return clamp(log(max(linear_depth / near_depth, 1.0)) /
                 log(far_depth / near_depth), 0.0, 1.0);
}
uint avboit_conservative_zero_depth(ivec2 pixel)
{
    vec2 volume_position =
        ((vec2(pixel) + vec2(0.5)) / vec2(avboitViewport)) *
        vec2(avboitVolumeSize) - vec2(0.5);
    ivec2 base_cell = ivec2(floor(volume_position));
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
    if (avboitRasterPass == 0)
    {
        ivec2 cell = clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1));
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
        float virtual_coordinate =
            avboit_virtual_depth(gl_FragCoord.z) * 8191.0;
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
        float slice_coordinate =
            (lower_filterable && upper_filterable ?
                mix(lower_coordinate, upper_coordinate,
                    fract(virtual_coordinate)) :
                (lower_filterable ? lower_coordinate : upper_coordinate)) /
            65536.0;
        uint zero_depth = avboit_conservative_zero_depth(pixel);
        if (zero_depth != 255u && slice_coordinate > float(zero_depth)) return;
        vec2 sample_xy = (vec2(pixel) + vec2(0.5)) / vec2(avboitViewport);
        float sample_slice = clamp(
            slice_coordinate - 2.0, 0.0, float(AVBOIT_DIRECT_SLICES - 1u));
        float front = texture(avboitTransmittanceSampler,
                              vec3(sample_xy, (sample_slice + 0.5) /
                                  float(AVBOIT_DIRECT_SLICES))).r;
        uint index = (uint(pixel.y) * uint(avboitViewport.x) + uint(pixel.x)) * 6u;
        atomicAdd(avboitDirectAccumulation[index + 4u],
                  uint(clamp(glow * front * 4096.0, 0.0, 16777215.0) + 0.5));
        return;
    }

}

in vec4 vertex_color;
in vec2 vary_texcoord0;

void main()
{
    avboit_store_glow(diffuseLookup(vary_texcoord0.xy).a * vertex_color.a);
}
