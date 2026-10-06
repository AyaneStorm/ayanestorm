/**
 * @file asDepthOfFieldSpriteV.glsl
 * @author chanayane@firestorm
 * @brief Aperture sprite placement for AyaneStorm DoF highlights.
 *
 * Attribute-free instanced draw: two triangles per highlight cell
 * (asDepthOfFieldHighlightF.glsl). Empty cells, cells dropped by the sprite
 * budget and cells of the other plane collapse to a degenerate triangle.
 * Live DoF (asdoflive.cpp): the exact open fraction under cat's eye
 * (liveOpenFraction()) and one part of each cell's light per draw
 * (live_part).
 */
// Cell energy (rgb) and occupancy (a), with mips.
uniform sampler2D specularRect;
// Cell centroid uv (xy), signed CoC (z).
uniform sampler2D emissiveRect;
// Cell brightness levels 0-3 and 4-7 with mips (budget ranking).
uniform sampler2D lightMap;
uniform sampler2D bloomMap;
uniform vec2 screen_res;
uniform vec2 target_res;
uniform float max_radius;
uniform float near_max_radius;
uniform float anamorphic_ratio;
uniform float unit_area;
uniform int plane;
uniform ivec2 cell_grid;
uniform int cell_top_level;
uniform float sprite_budget;
// Bokeh shape saturation (ASDepthOfFieldHighlightSaturation): 1 unchanged,
// 0 grey, above 1 more vivid.
uniform float sprite_saturation;
// Lens field (asdoflive.cpp, setLensUniforms()), as in the gathers.
uniform vec2 field_scale;
uniform float cat_eye;
uniform float ca_shift;

flat out vec3 vary_energy;
flat out vec2 vary_center;
flat out float vary_radius;
// Barrel centre and axial CA shift (px) of this sprite
// (asDepthOfFieldSpriteF.glsl).
flat out vec2 vary_barrel;
flat out float vary_delta;

// Cat's eye: the barrel's centre at this field position, aperture radii.
vec2 barrelCenter(vec2 field)
{
    vec2 shift = cat_eye * field;
    float len = length(shift);
    return len > 1.6 ? shift * (1.6 / len) : shift;
}

// The exact open share of the aperture under cat's eye, spherical profile
// included, as Live's gathers renormalize every source to its exact open
// aperture (scripts/testing/dof_live_reference.py, sprite_open_fraction()).
uniform int aperture_blades;
uniform float aperture_roundness;
uniform float aperture_rotation;
uniform float sa_strength;
// 1: the front part of each cell's light (B1, N2), 0: the back part (B2,
// N1); the cell keeps their sum and the front fraction (centroid .w,
// asDepthOfFieldHighlightF.glsl).
uniform int live_part;

float liveSphericalNorm(float c)
{
    if (abs(c) <= 1.0)
    {
        return 1.0;
    }
    float q = (1.0 + c) * (1.0 + c) / (4.0 * abs(c));
    return c > 0.0 ? q : 1.0 + q;
}

// Integral of the cut spherical profile max(1 + c - 2 c rho^2, 0) times rho
// over [lo, hi], divided by its mean (asDepthOfFieldSpriteF.glsl,
// sphericalWeight()).
float liveProfileMoment(float c, float lo, float hi)
{
    if (c > 1.0)
    {
        hi = min(hi, sqrt((1.0 + c) / (2.0 * c)));
    }
    else if (c < -1.0)
    {
        lo = max(lo, sqrt((1.0 + c) / (2.0 * c)));
    }
    if (hi <= lo)
    {
        return 0.0;
    }
    float g_hi = 0.5 * (1.0 + c) * hi * hi - 0.5 * c * hi * hi * hi * hi;
    float g_lo = 0.5 * (1.0 + c) * lo * lo - 0.5 * c * lo * lo * lo * lo;
    return (g_hi - g_lo) / liveSphericalNorm(c);
}

// Edge radius at a polar angle before rotation (asDepthOfFieldSpriteF.glsl).
float liveBoundary(float phi)
{
    if (aperture_blades < 3)
    {
        return 1.0;
    }
    float sector = 2.0 * 3.14159265358979323846 / float(aperture_blades);
    float local_angle = mod(phi, sector) - 0.5 * sector;
    float polygon = cos(0.5 * sector) / max(cos(local_angle), 0.001);
    return mix(polygon, 1.0, aperture_roundness);
}

// Share of the profiled unit aperture inside the barrel's unit circle: a
// polar integral over 64 aperture angles, each with the exact ray and circle
// intersection (0.4% from a brute-force area for a triangle at shift 1.4).
float liveOpenFraction(vec2 barrel, float c)
{
    float inside = 0.0;
    float full = 0.0;
    for (int i = 0; i < 64; ++i)
    {
        float phi = 2.0 * 3.14159265358979323846 * (float(i) + 0.5) / 64.0;
        float b = liveBoundary(phi);
        vec2 dir = vec2(anamorphic_ratio * cos(phi + aperture_rotation),
                        sin(phi + aperture_rotation));
        float qa = dot(dir, dir);
        float qb = -2.0 * dot(dir, barrel);
        float qc = dot(barrel, barrel) - 1.0;
        float disc = qb * qb - 4.0 * qa * qc;
        if (disc > 0.0)
        {
            float root = sqrt(disc);
            float lo = max((-qb - root) / (2.0 * qa), 0.0);
            float hi = min((-qb + root) / (2.0 * qa), b);
            inside += b * b * liveProfileMoment(c, lo / b, hi / b);
        }
        full += b * b * liveProfileMoment(c, 0.0, 1.0);
    }
    return full > 0.0 ? inside / full : 1.0;
}

// Same rule as asDepthOfFieldHighlightF.glsl keepCell() (brightest cells
// first over the budget); keep the two copies identical.
float levelThreshold(int k)
{
    return exp2(2.0 * float(k) - 8.0);
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
    float above = occupied;
    float floor_level = 0.0;
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
    return cell_luminance >= floor_level && cellHash(cell) < sprite_budget / max(above, 1.0);
}

void main()
{
    const vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    int cell_index = gl_InstanceID / 2;
    ivec2 cell = ivec2(cell_index % cell_grid.x, cell_index / cell_grid.x);
    vec4 energy = texelFetch(specularRect, cell, 0);
    vec4 data = texelFetch(emissiveRect, cell, 0);

    vary_energy = vec3(0.0);
    vary_center = vec2(0.0);
    vary_radius = 1.0;
    vary_barrel = vec2(0.0);
    vary_delta = 0.0;
    bool this_plane = plane > 0 ? data.z > 0.0 : data.z < 0.0;
    if (energy.a < 0.5 || !this_plane ||
        !keepCell(cell, dot(energy.rgb, vec3(0.2126, 0.7152, 0.0722))))
    {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }

    float plane_radius = plane > 0 ? max_radius : near_max_radius;
    float radius = max(plane > 0 ? data.z * max_radius : -data.z * near_max_radius, 1.0);
    vec2 center = data.xy * screen_res;
    vec2 field = (data.xy - 0.5) * field_scale;
    vec2 barrel = barrelCenter(field);
    // Axial CA widens the bluest (far) or reddest (near) disc by delta.
    float delta = ca_shift * plane_radius;
    // Margin for the antialiased edge: 1.5 target pixels. The barrel only
    // shrinks the shape.
    float half_size = (radius + delta) * max(anamorphic_ratio, 1.0) +
                      1.5 * max(screen_res.x / target_res.x, 1.0);
    vec2 pixel = center + corners[(gl_InstanceID % 2) * 3 + gl_VertexID] * half_size;

    // Radiance per full-resolution pixel: the energy spread over the
    // aperture's area, so brightness is independent of shape and radius.
    // The open share of the clipped aperture shrinks that area.
    float area = unit_area * radius * radius;
    if (cat_eye > 0.0)
    {
        float sigma = (plane > 0 ? 1.0 : -1.0) * clamp(radius / 3.0, 0.0, 1.0);
        area *= max(liveOpenFraction(barrel, sa_strength * sigma), 0.01);
    }
    float part = live_part != 0 ? data.w : 1.0 - data.w;
    if (part <= 0.0001)
    {
        vary_energy = vec3(0.0);
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }
    vary_energy = energy.rgb * part / area;
    // Saturation around the luminance, which it keeps (the gather lost
    // exactly this luminance); channels pushed below 0 are clipped.
    float luma = dot(vary_energy, vec3(0.2126, 0.7152, 0.0722));
    vary_energy = max(mix(vec3(luma), vary_energy, sprite_saturation), vec3(0.0));
    vary_center = center;
    vary_radius = radius;
    vary_barrel = barrel;
    vary_delta = delta;
    gl_Position = vec4(pixel / screen_res * 2.0 - 1.0, 0.0, 1.0);
}
