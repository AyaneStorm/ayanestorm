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
uniform sampler3D avboitTransmittanceSampler;
const uint AVBOIT_DIRECT_SLICES = 128u;
const uint AVBOIT_DIRECT_OCCUPANCY_WORDS = AVBOIT_DIRECT_SLICES / 32u;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
layout(std430, binding = 5) buffer AVBOITWarp { uint avboitWarp[8192]; };
layout(std430, binding = 6) buffer AVBOITTileOccupancy { uint avboitTileOccupancy[]; };
layout(std430, binding = 7) buffer AVBOITDirectAccumulation { uint avboitDirectAccumulation[]; };
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
        float virtual_coordinate = clamp(gl_FragCoord.z, 0.0, 1.0) * 8191.0;
        uint lower_virtual = uint(floor(virtual_coordinate));
        uint upper_virtual = min(lower_virtual + 1u, 8191u);
        float slice_coordinate = mix(float(avboitWarp[lower_virtual]),
                                     float(avboitWarp[upper_virtual]),
                                     fract(virtual_coordinate)) / 65536.0;
        ivec2 cell = clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1));
        uint zero_depth = imageLoad(avboitZeroTransmittanceDepth, cell).r;
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

void main()
{
    vec4 basecolor = texture(diffuseMap, base_color_texcoord);
    if (basecolor.a < minimum_alpha) discard;
    vec3 emissive = emissiveColor * srgb_to_linear(texture(emissiveMap, emissive_texcoord).rgb);
    avboit_store_glow(max(max(emissive.r, emissive.g), emissive.b) * vertex_emissive.a);
}
