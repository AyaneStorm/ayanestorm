/**
 * @file asDepthOfFieldOccupancyF.glsl
 * @author chanayane@firestorm
 * @brief Conservative transparent-layer occupancy for bokeh gathering.
 */
out vec4 frag_color;

uniform sampler2D noiseMap;

void main()
{
    ivec2 source_size = textureSize(noiseMap, 0);
    ivec2 origin = ivec2(gl_FragCoord.xy) * 16;
    vec2 occupied = vec2(0.0);
    for (int y = 0; y < 16; ++y)
    {
        for (int x = 0; x < 16; ++x)
        {
            ivec2 pixel = origin + ivec2(x, y);
            if (pixel.x < source_size.x && pixel.y < source_size.y)
            {
                vec4 layers = texelFetch(noiseMap, pixel, 0);
                occupied = max(occupied,
                               vec2(layers.b > 0.0001 ? 1.0 : 0.0,
                                    layers.a > 0.0001 ? 1.0 : 0.0));
            }
        }
    }
    // Mip averaging keeps every occupied source tile nonzero at coarser levels.
    frag_color = vec4(occupied, 0.0, 1.0);
}
