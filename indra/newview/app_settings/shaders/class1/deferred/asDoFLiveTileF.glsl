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
 *   as the gathers read it: completed (liveCompleted()). From the raw bin,
 *   a tile where nearer bins hide all of it got 0 while its neighbour's
 *   gather ran and read the fill: the fill stopped at tile edges (grey
 *   blocks seen through semi-transparent in-focus hair);
 *   with axial CA, a bin's radius grows by its widest stratum (ca_reach);
 *   tile_pass 1, 2: dilation along x, then y: a neighbour tile k tiles away
 *   counts when its radius spans the k - 1 tiles between the two (a
 *   conservative square around each disc).
 */

layout(location = 0) out vec4 frag_color;

uniform sampler2D diffuseRect;   // pass 0: N2 (S.rgb, W), mipmapped
uniform sampler2D specularRect;  // pass 0: N1 (S.rgb, W), mipmapped
uniform sampler2D emissiveRect;  // pass 0: radius (E_N2, E_N1, E_B1, M_B2), mipmapped
uniform sampler2D bloomMap;      // pass 0: B1 (S.rgb, W), mipmapped
uniform sampler2D lightMap;      // pass 0: visibility (V_N1, V_F, V_B1, V_B2), mipmapped
uniform sampler2D noiseMap;      // passes 1, 2: the previous tile result
uniform int tile_pass;
uniform int max_level;
// Dilation reach in tiles: ceil(largest veil radius / TILE).
uniform int tile_reach;
// Axial CA: widest stratum's radius increase in front of and behind the
// focus, gather pixels (asDoFLiveGatherF.glsl); 0 off.
uniform vec2 ca_reach;

const int TILE = 8;
const int MAX_REACH = 64;

bool liveCompleted(sampler2D layer, sampler2D energy_map, sampler2D vis_map,
                   int vis_channel, vec2 uv, float lod, float max_lod,
                   out vec4 value, out vec4 energy);

// Radius of the sources a veil gather reads at uv: sqrt(W / E) of the
// completed bin. vis_channel as in asDoFLiveGatherF.glsl; energy_channel
// picks E of the bin.
float binRadius(sampler2D bin, int vis_channel, int energy_channel, vec2 uv)
{
    vec4 value;
    vec4 energies;
    if (!liveCompleted(bin, emissiveRect, lightMap, vis_channel, uv, 0.0,
                       float(max_level), value, energies))
    {
        return 0.0;
    }
    float energy = energies[energy_channel];
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
                vec2 uv = (vec2(p) + 0.5) / vec2(size);
                radius = max(radius, vec3(binRadius(diffuseRect, -1, 0, uv),
                                          binRadius(specularRect, 0, 1, uv),
                                          binRadius(bloomMap, 2, 2, uv)));
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
