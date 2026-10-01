/**
 * @file asDoFLiveTileF.glsl
 * @author chanayane@firestorm
 * @brief Live DoF: veil kernel radius per tile.
 *
 * A tile is TILE x TILE gather pixels. The veil gathers use, as their single
 * kernel radius, the largest radius of their bin whose disc can reach the
 * tile, separately for N2 (.x), N1 (.y) and B1 (.z), in gather pixels.
 * Tiles no source of a bin reaches stay 0, and its gather skips them.
 *   tile_pass 0: maximum source radius of each bin inside the tile, read
 *   as the gathers read it: completed (asDoFLiveCompleteF.glsl). From the raw bin,
 *   a tile where nearer bins hide all of it got 0 while its neighbour's
 *   gather ran and read the fill: the fill stopped at tile edges (grey
 *   blocks seen through semi-transparent in-focus hair);
 *   with axial CA, a bin's radius grows by its widest stratum (ca_reach);
 *   tile_pass 1, 2: dilation along x, then y: a neighbour tile k tiles away
 *   counts when its radius spans the k - 1 tiles between the two (a
 *   conservative square around each disc).
 */

layout(location = 0) out vec4 frag_color;

uniform sampler2D diffuseRect;   // pass 0: N2 (S.rgb, W), raw
uniform sampler2D specularRect;  // pass 0: N1 (S.rgb, W), completed
uniform sampler2D emissiveRect;  // pass 0: raw energies (E_N2, ...)
uniform sampler2D bloomMap;      // pass 0: B1 (S.rgb, W), completed
uniform sampler2D lightMap;      // pass 0: completed energies (E_N1, E_B1, M_B2)
uniform sampler2D noiseMap;      // passes 1, 2: the previous tile result
uniform int tile_pass;
// Dilation reach in tiles: ceil(largest veil radius / TILE).
uniform int tile_reach;
// Axial CA: widest stratum's radius increase in front of and behind the
// focus, gather pixels (asDoFLiveGatherF.glsl); 0 off.
uniform vec2 ca_reach;

const int TILE = 8;
const int MAX_REACH = 64;

// Radius of the sources a veil gather reads at uv: sqrt(W / E) of the bin
// as the gather reads it; energy is its E.
float binRadius(vec4 value, float energy)
{
    return value.a > 0.001 && energy > 0.0 ? sqrt(value.a / energy) : 0.0;
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
                float raw_energy = texelFetch(emissiveRect, p, 0).x;
                vec2 done_energy = texelFetch(lightMap, p, 0).xy;
                radius = max(radius, vec3(binRadius(texelFetch(diffuseRect, p, 0), raw_energy),
                                          binRadius(texelFetch(specularRect, p, 0), done_energy.x),
                                          binRadius(texelFetch(bloomMap, p, 0), done_energy.y)));
            }
        }
        radius += mix(vec3(0.0), ca_reach.xxy, greaterThan(radius, vec3(0.0)));
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
