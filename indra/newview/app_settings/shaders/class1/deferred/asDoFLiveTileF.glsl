/**
 * @file asDoFLiveTileF.glsl
 * @author chanayane@firestorm
 * @brief Live DoF: veil kernel radius per tile.
 *
 * A tile is TILE x TILE gather pixels. The veil gathers use, as their single
 * kernel radius, the largest radius of their bin whose disc can reach the
 * tile, separately for N2 (.x), N1 (.y) and B1 (.z), in gather pixels.
 * Tiles no source of a bin reaches stay 0, and its gather skips them.
 *   tile_pass 0: maximum source radius of each bin inside the tile;
 *   tile_pass 1, 2: dilation along x, then y: a neighbour tile k tiles away
 *   counts when its radius spans the k - 1 tiles between the two (a
 *   conservative square around each disc).
 */

layout(location = 0) out vec4 frag_color;

uniform sampler2D diffuseRect;   // pass 0: N2 (S.rgb, W)
uniform sampler2D specularRect;  // pass 0: N1 (S.rgb, W)
uniform sampler2D emissiveRect;  // pass 0: radius (E_N2, E_N1, E_B1, M_B2)
uniform sampler2D bloomMap;      // pass 0: B1 (S.rgb, W)
uniform sampler2D noiseMap;      // passes 1, 2: the previous tile result
uniform int tile_pass;
// Dilation reach in tiles: ceil(largest veil radius / TILE).
uniform int tile_reach;

const int TILE = 8;
const int MAX_REACH = 64;

// Radius of the sources in a texel of a veil bin: sqrt(W / E).
float binRadius(float weight, float energy)
{
    return weight > 0.001 && energy > 0.0 ? sqrt(weight / energy) : 0.0;
}

void main()
{
    ivec2 tile = ivec2(gl_FragCoord.xy);
    if (tile_pass == 0)
    {
        ivec2 size = textureSize(diffuseRect, 0);
        vec3 radius = vec3(0.0);
        for (int y = 0; y < TILE; ++y)
        {
            for (int x = 0; x < TILE; ++x)
            {
                ivec2 p = tile * TILE + ivec2(x, y);
                if (p.x >= size.x || p.y >= size.y)
                {
                    continue;
                }
                vec4 energy = texelFetch(emissiveRect, p, 0);
                radius = max(radius, vec3(binRadius(texelFetch(diffuseRect, p, 0).a, energy.x),
                                          binRadius(texelFetch(specularRect, p, 0).a, energy.y),
                                          binRadius(texelFetch(bloomMap, p, 0).a, energy.z)));
            }
        }
        frag_color = vec4(radius, 0.0);
        return;
    }

    ivec2 axis = tile_pass == 1 ? ivec2(1, 0) : ivec2(0, 1);
    ivec2 size = textureSize(noiseMap, 0);
    vec3 radius = texelFetch(noiseMap, tile, 0).xyz;
    for (int k = 1; k <= MAX_REACH; ++k)
    {
        if (k > tile_reach)
        {
            break;
        }
        // Gap between the two tiles' nearest edges.
        float gap = float((k - 1) * TILE);
        for (int side = -1; side <= 1; side += 2)
        {
            ivec2 n = tile + axis * (k * side);
            if (n.x < 0 || n.y < 0 || n.x >= size.x || n.y >= size.y)
            {
                continue;
            }
            vec3 other = texelFetch(noiseMap, n, 0).xyz;
            radius = max(radius, mix(vec3(0.0), other, greaterThanEqual(other, vec3(gap))));
        }
    }
    frag_color = vec4(radius, 0.0);
}
