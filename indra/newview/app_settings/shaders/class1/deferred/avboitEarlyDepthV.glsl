/**
 * Rasterizes conservative zero-transmittance tiles into private AVBOIT depth.
 */

layout(std430, binding = 3) readonly buffer AVBOITWork
{
    uint avboitWork[];
};

uniform ivec2 avboitViewport;
uniform ivec2 avboitVolumeSize;

void main()
{
    const vec2 corners[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    uint tile_offset = 8u + 128u +
        uint(avboitVolumeSize.x * avboitVolumeSize.y) +
        uint(gl_InstanceID) * 4u;
    uvec4 tile = uvec4(
        avboitWork[tile_offset],
        avboitWork[tile_offset + 1u],
        avboitWork[tile_offset + 2u],
        avboitWork[tile_offset + 3u]);
    vec2 pixel = vec2(tile.xy * 16u) +
        corners[gl_VertexID] * 16.0;
    vec2 ndc_xy = pixel / vec2(avboitViewport) * 2.0 - 1.0;
    float window_depth = uintBitsToFloat(tile.z);
    gl_Position = vec4(ndc_xy, window_depth * 2.0 - 1.0, 1.0);
}
