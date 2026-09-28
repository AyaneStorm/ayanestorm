/**
 * @file asDepthOfFieldHighlightF.glsl
 * @author chanayane@firestorm
 * @brief Highlight extraction for AyaneStorm DoF aperture sprites.
 *
 * Isolated defocused highlights are removed from the gather input and drawn
 * as analytic aperture sprites (asDepthOfFieldSpriteV/F.glsl) carrying the
 * same energy, so small lights keep a coherent aperture outline instead of
 * a dotted gather.
 *
 * highlight_pass 0 (cell grid): per cell of CELL_SIZE^2 full-resolution
 * pixels, sum the extracted energy with its luminance-weighted centre and
 * signed CoC. highlight_pass 1 (full resolution): gather input = opaque
 * color minus the extraction of kept cells. Both passes evaluate the same
 * detect(), so the removed and the redrawn energy match exactly.
 */
layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;

// Original opaque linear-HDR color.
uniform sampler2D diffuseRect;
// Signed CoC target; g is the opaque CoC.
uniform sampler2D noiseMap;
// Cell energy (rgb) and occupancy (a) with mips (pass 1 only).
uniform sampler2D specularRect;
uniform vec2 screen_res;
uniform float max_radius;
uniform float near_max_radius;
uniform float isolation;
uniform int highlight_pass;
uniform ivec2 cell_grid;
uniform int cell_top_level;
uniform float sprite_budget;

#define CELL_SIZE 8

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// Energy of pixel p which belongs to an isolated defocused highlight: its
// excess over the surrounding ring. The ring is half the blur radius
// (6-32 px): a light qualifies when it is smaller than about half its own
// bokeh, however large it is on screen (stars on a zoomed view span many
// pixels). A light covering several cells gets one sprite per cell, which
// merge at these radii. Bright lines and areas large relative to their blur
// fail the ring test and remain in the gather; in-focus pixels are never
// touched.
vec3 detect(ivec2 p)
{
    ivec2 last = ivec2(screen_res) - 1;
    float coc = texelFetch(noiseMap, p, 0).g;
    float radius = coc < 0.0 ? -coc * near_max_radius : coc * max_radius;
    float gate = smoothstep(2.0, 4.0, radius);
    if (gate <= 0.0)
    {
        return vec3(0.0);
    }
    vec3 color = texelFetch(diffuseRect, p, 0).rgb;
    float ring_radius = clamp(0.5 * radius, 6.0, 32.0);
    vec3 ring_sum = vec3(0.0);
    float ring_max = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float angle = float(i) * 0.785398163;
        ivec2 q = clamp(p + ivec2(round(vec2(cos(angle), sin(angle)) * ring_radius)),
                        ivec2(0), last);
        vec3 ring = texelFetch(diffuseRect, q, 0).rgb;
        ring_sum += ring;
        ring_max = max(ring_max, luminance(ring));
    }
    float ratio = luminance(color) / max(ring_max, 0.0001);
    float isolated = smoothstep(isolation, 2.0 * isolation, ratio);
    return isolated * gate * max(color - ring_sum * 0.125, vec3(0.0));
}

// Cells over the sprite budget keep their highlight in the gather. The hash
// is stable per cell, so a static view never flickers.
bool keepCell(ivec2 cell)
{
    float occupied = texelFetch(specularRect, ivec2(0), cell_top_level).a *
                     float(cell_grid.x * cell_grid.y);
    if (occupied <= sprite_budget)
    {
        return true;
    }
    uint h = uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u;
    h *= 2654435761u;
    return float(h >> 8) / 16777216.0 < sprite_budget / occupied;
}

void main()
{
    if (highlight_pass == 0)
    {
        ivec2 cell = ivec2(gl_FragCoord.xy);
        ivec2 size = ivec2(screen_res);
        vec3 energy = vec3(0.0);
        vec2 center_sum = vec2(0.0);
        float coc_sum = 0.0;
        float weight = 0.0;
        for (int y = 0; y < CELL_SIZE; ++y)
        {
            for (int x = 0; x < CELL_SIZE; ++x)
            {
                ivec2 p = cell * CELL_SIZE + ivec2(x, y);
                if (p.x >= size.x || p.y >= size.y)
                {
                    continue;
                }
                vec3 excess = detect(p);
                float l = luminance(excess);
                if (l <= 0.0)
                {
                    continue;
                }
                energy += excess;
                center_sum += (vec2(p) + 0.5) * l;
                coc_sum += texelFetch(noiseMap, p, 0).g * l;
                weight += l;
            }
        }
        bool occupied = weight > 0.0001;
        frag_data0 = vec4(energy, occupied ? 1.0 : 0.0);
        frag_data1 = occupied
            ? vec4(center_sum / weight / screen_res, coc_sum / weight, weight)
            : vec4(0.0);
        return;
    }

    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 cell = p / CELL_SIZE;
    vec3 color = texelFetch(diffuseRect, p, 0).rgb;
    if (texelFetch(specularRect, cell, 0).a > 0.5 && keepCell(cell))
    {
        color -= detect(p);
    }
    frag_data0 = vec4(color, 1.0);
    frag_data1 = vec4(0.0);
}
