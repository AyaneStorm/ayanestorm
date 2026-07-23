// <AS:Chanayane> Exact OIT ordered emissive capture
/*[EXTRA_CODE_HERE]*/

layout(early_fragment_tests) in;
layout(binding = 0, r32ui) uniform coherent uimage2D oitHeadPointers;
layout(binding = 1, r32ui) uniform coherent uimage2D oitListCounts;
// <AS:Chanayane> Lossless 32-byte node: scalar glow and index-derived sequence.
struct OITNode { vec4 color; float glow; float depth; uint next; uint blend; };
// </AS:Chanayane>
layout(std430, binding = 0) buffer OITNodes { OITNode oitNodes[]; };
layout(std430, binding = 1) buffer OITControl { uint oitNodeCount; uint oitNodeCapacity; uint oitOverflow; uint oitPad; };
uniform int avboitRasterPass;
uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;
uniform sampler3D avboitTransmittanceSampler;
layout(binding = 6, r8ui) uniform coherent uimage2D avboitZeroTransmittanceDepth;
layout(std430, binding = 5) buffer AVBOITWarp { uint avboitWarp[8192]; };
layout(std430, binding = 6) buffer AVBOITTileOccupancy { uint avboitTileOccupancy[]; };
layout(std430, binding = 7) buffer AVBOITDirectAccumulation { uint avboitDirectAccumulation[]; };
void exact_oit_store_glow(float glow)
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
                         uint(neighbor.x)) * 4u;
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
                                     fract(virtual_coordinate));
        ivec2 cell = clamp(pixel / 8, ivec2(0), avboitVolumeSize - ivec2(1));
        uint zero_depth = imageLoad(avboitZeroTransmittanceDepth, cell).r;
        if (zero_depth != 255u && slice_coordinate > float(zero_depth)) return;
        vec2 sample_xy = (vec2(pixel) + vec2(0.5)) / vec2(avboitViewport);
        float sample_slice = clamp(slice_coordinate - 2.0, 0.0, 127.0);
        float front = texture(avboitTransmittanceSampler,
                              vec3(sample_xy, (sample_slice + 0.5) / 128.0)).r;
        uint index = (uint(pixel.y) * uint(avboitViewport.x) + uint(pixel.x)) * 6u;
        atomicAdd(avboitDirectAccumulation[index + 4u],
                  uint(clamp(glow * front * 4096.0, 0.0, 16777215.0) + 0.5));
        return;
    }

    uint index = atomicAdd(oitNodeCount, 1u);
    if (index >= oitNodeCapacity) { atomicOr(oitOverflow, 1u); return; }
    oitNodes[index].color = vec4(0.0);
    oitNodes[index].glow = glow;
    oitNodes[index].depth = gl_FragCoord.z;
    oitNodes[index].blend = 0xffffffffu;
    oitNodes[index].next = imageAtomicExchange(oitHeadPointers, ivec2(gl_FragCoord.xy), index);
    // <AS:Chanayane> Glow nodes participate in the same exact ordered list count.
    uint pixel_count = imageAtomicAdd(oitListCounts, ivec2(gl_FragCoord.xy), 1u) + 1u;
    atomicMax(oitPad, pixel_count);
    // </AS:Chanayane>
}

in vec4 vertex_color;
in vec2 vary_texcoord0;

void main()
{
    exact_oit_store_glow(diffuseLookup(vary_texcoord0.xy).a * vertex_color.a);
}
// </AS:Chanayane>
