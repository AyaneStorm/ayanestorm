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
 * signed CoC. highlight_pass 1 (full resolution): gather input = bin colour
 * minus the extraction of kept cells. Both passes evaluate the same
 * detect(), so the removed and the redrawn energy match exactly.
 *
 * Live DoF (asdoflive.cpp), linked with asDoFLiveCommonF.glsl: the radius
 * comes from the depth (highlightCoC()) and each cell keeps the light alone
 * in the front and the back bin of its side (highlightParts()): their sum,
 * and the front fraction in frag_data1.w; the bright bokeh highlights gain
 * applies to that energy (highlightGain()).
 */
layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;
// Pass 0: the cell's brightness levels (budget ranking, see keepCell()).
layout(location = 2) out vec4 frag_data2;
layout(location = 3) out vec4 frag_data3;

// Live's bin colour (the opaque surface, or the composited image in its
// one-layer fallback), linear HDR.
uniform sampler2D diffuseRect;
// Cell energy (rgb) and occupancy (a) with mips (pass 1 only).
uniform sampler2D specularRect;
// Cell brightness levels 0-3 and 4-7 with mips (pass 1 only).
uniform sampler2D lightMap;
uniform sampler2D bloomMap;
uniform vec2 screen_res;
uniform float max_radius;
uniform float near_max_radius;
uniform float isolation;
uniform int highlight_pass;
uniform ivec2 cell_grid;
uniform int cell_top_level;
uniform float sprite_budget;

#define CELL_SIZE 8

// The blur radius comes from depthMap (asDoFLiveCommonF.glsl).
uniform sampler2D depthMap;
uniform sampler2D positionMap;  // Mac OIT sum of weights, sum of optical depth
uniform int bins_source;
float liveBlurRadius(float device_depth, vec2 uv);
void liveDecompose(ivec2 p, out vec4 bins[5], out vec4 energy);
vec4 liveBinVisibility(vec4 bins[5]);

// Signed CoC normalized as the sprites decode it (asDepthOfFieldSpriteV.glsl):
// the radius over the side's largest radius.
float highlightCoC(ivec2 p)
{
    vec2 uv = (vec2(p) + 0.5) / screen_res;
    float radius = liveBlurRadius(texture(depthMap, uv).r, uv);
    return radius < 0.0 ? radius / max(near_max_radius, 0.0001) :
                          radius / max(max_radius, 0.0001);
}

// The opaque light alone in the front and the back bin of its side (B1 and
// B2 behind the focus, N2 and N1 in front): each bin is read alone (S / V),
// so the surface's share b shows a_b = share_b T / V_b of its light (T: the
// transmittance in front of it). The sprites draw the front part over its
// layer pair and the back part into the back layer, under what the front
// holds there (asdoflive.cpp): a light behind a B1 hair strand stays behind
// it. A light behind a strand in a nearer bin keeps its full excess, behind
// glass in its own bin T. One layer (bins_source 0): (0, 1). Mirrored by
// dof_live_reference.py (sprite_layer_parts()).
vec4 liveBinWeights(float signed_radius);

vec2 highlightParts(ivec2 p)
{
    vec2 uv = (vec2(p) + 0.5) / screen_res;
    float radius = liveBlurRadius(texture(depthMap, uv).r, uv);
    vec4 shares = liveBinWeights(radius);       // N2, N1, F, B1
    float far_share = max(1.0 - dot(shares, vec4(1.0)), 0.0);
    vec2 split = radius < 0.0 ? shares.xy : vec2(shares.w, far_share);
    if (bins_source == 0)
    {
        // One surface per pixel: nothing in front of it, V_back = 1 - front.
        return vec2(split.x, split.y / max(1.0 - split.x, 0.0001));
    }
    vec4 bins[5];
    vec4 energy;
    liveDecompose(p, bins, energy);
    vec4 visibility = liveBinVisibility(bins);  // in front of N1, F, B1, B2
    float transmittance = exp(-texelFetch(positionMap, p, 0).y);
    vec2 front_back = radius < 0.0 ? vec2(1.0, visibility.x) : visibility.zw;
    return min(split * transmittance / max(front_back, vec2(0.0001)), vec2(1.0));
}

// Bright bokeh highlights (ASDepthOfFieldApertureHighlights, artistic, not
// energy preserving): the Aperture-sampled renderer's gain
// (asDoFAccumulateF.glsl) on the sprites' light, 1 + strength * bright *
// min((radius / 4)^2, 1024), radius the full-resolution blur. The extraction
// has already found the light isolated, so mode 2's ring test is left out;
// the gather input loses only the light itself. hl_strength 0: off. Mirrored
// by dof_live_reference.py (live_highlight_gain()).
uniform float hl_strength;
uniform float hl_threshold;

float highlightGain(ivec2 p)
{
    if (hl_strength <= 0.0)
    {
        return 1.0;
    }
    float coc = highlightCoC(p);
    float radius = coc < 0.0 ? -coc * near_max_radius : coc * max_radius;
    if (radius <= 1.0)
    {
        return 1.0;
    }
    float luma = dot(texelFetch(diffuseRect, p, 0).rgb, vec3(0.2126, 0.7152, 0.0722));
    float bright = smoothstep(0.5 * hl_threshold, 1.5 * hl_threshold, luma);
    return 1.0 + hl_strength * bright * min(radius * radius / 16.0, 1024.0);
}

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
    float coc = highlightCoC(p);
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
    // Isolation of the light this pixel belongs to: the brightest pixel
    // within 2 px. Judged alone, the dimmer antialiased edge pixels of a
    // light failed the test while its core passed; the core became a sprite
    // and its edges stayed in the gather as a sparse ring of 1 px sources,
    // which the gather turns into dotted bokeh (more so at low isolation).
    // Each pixel still gives only its own excess over the ring, so sky next
    // to a light loses nothing and the energy balance is unchanged.
    const ivec2 neighbours[12] = ivec2[12](
        ivec2(-1, -1), ivec2(0, -1), ivec2(1, -1), ivec2(-1, 0), ivec2(1, 0),
        ivec2(-1, 1), ivec2(0, 1), ivec2(1, 1),
        ivec2(-2, 0), ivec2(2, 0), ivec2(0, -2), ivec2(0, 2));
    float peak = luminance(color);
    for (int i = 0; i < 12; ++i)
    {
        peak = max(peak, luminance(texelFetch(diffuseRect,
                                              clamp(p + neighbours[i], ivec2(0), last), 0).rgb));
    }
    float ratio = peak / max(ring_max, 0.0001);
    float isolated = smoothstep(isolation, 2.0 * isolation, ratio);
    return isolated * gate * max(color - ring_sum * 0.125, vec3(0.0));
}

// Budget ranking. Pass 0 flags, per cell, whether its extracted luminance
// reaches each of 8 levels (factor 4 apart, 2^-8 to 2^6); the top mips count
// the cells at each level. Over the budget, the brightest cells keep their
// sprite: every cell at the first level that fits, and the band just below
// it fills the rest by a stable per-cell hash. A random pick over all cells
// let leaf glints (low isolation) crowd out stars, whose dropped cells went
// back to the gather as dotted bokeh. Same function in
// asDepthOfFieldSpriteV.glsl.
float levelThreshold(int k)
{
    return exp2(2.0 * float(k) - 8.0);
}

vec4 cellLevels(float cell_luminance, int first)
{
    return step(vec4(levelThreshold(first), levelThreshold(first + 1),
                     levelThreshold(first + 2), levelThreshold(first + 3)),
                vec4(cell_luminance));
}

float cellHash(ivec2 cell)
{
    uint h = uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u;
    h *= 2654435761u;
    return float(h >> 8) / 16777216.0;
}

bool keepCell(ivec2 cell, float cell_luminance)
{
    float cells = float(cell_grid.x * cell_grid.y);
    float occupied = texelFetch(specularRect, ivec2(0), cell_top_level).a * cells;
    if (occupied <= sprite_budget)
    {
        return true;
    }
    vec4 low = texelFetch(lightMap, ivec2(0), cell_top_level) * cells;
    vec4 high = texelFetch(bloomMap, ivec2(0), cell_top_level) * cells;
    float counts[8] = float[8](low.x, low.y, low.z, low.w, high.x, high.y, high.z, high.w);
    float above = occupied;   // cells at the level below (all occupied at first)
    float floor_level = 0.0;  // its threshold
    for (int k = 0; k < 8; ++k)
    {
        float threshold = levelThreshold(k);
        if (counts[k] <= sprite_budget)
        {
            if (cell_luminance >= threshold)
            {
                return true;
            }
            if (cell_luminance < floor_level)
            {
                return false;
            }
            return cellHash(cell) < (sprite_budget - counts[k]) / max(above - counts[k], 1.0);
        }
        above = counts[k];
        floor_level = threshold;
    }
    // Even the top level exceeds the budget: a stable share of it.
    return cell_luminance >= floor_level && cellHash(cell) < sprite_budget / max(above, 1.0);
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
        float front_sum = 0.0;  // luminance of the front parts
        float part_sum = 0.0;
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
                vec2 parts = highlightParts(p);
                energy += excess * (parts.x + parts.y) * highlightGain(p);
                front_sum += l * parts.x;
                part_sum += l * (parts.x + parts.y);
                center_sum += (vec2(p) + 0.5) * l;
                coc_sum += highlightCoC(p) * l;
                weight += l;
            }
        }
        bool occupied = weight > 0.0001;
        frag_data0 = vec4(energy, occupied ? 1.0 : 0.0);
        // .w: the share of the cell's energy in the front bin.
        frag_data1 = occupied
            ? vec4(center_sum / weight / screen_res, coc_sum / weight,
                   part_sum > 0.0 ? front_sum / part_sum : 0.0)
            : vec4(0.0);
        // Counts only: keepCell() reads the stored energy in both pass 1 and
        // the sprites, so they always agree on which cells are kept.
        frag_data2 = occupied ? cellLevels(luminance(energy), 0) : vec4(0.0);
        frag_data3 = occupied ? cellLevels(luminance(energy), 4) : vec4(0.0);
        return;
    }

    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 cell = p / CELL_SIZE;
    vec3 color = texelFetch(diffuseRect, p, 0).rgb;
    vec4 cell_energy = texelFetch(specularRect, cell, 0);
    if (cell_energy.a > 0.5 && keepCell(cell, luminance(cell_energy.rgb)))
    {
        color -= detect(p);
    }
    frag_data0 = vec4(color, 1.0);
    frag_data1 = vec4(0.0);
    frag_data2 = vec4(0.0);
    frag_data3 = vec4(0.0);
}
