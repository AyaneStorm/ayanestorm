/**
 * @file asDepthOfFieldSpriteV.glsl
 * @author chanayane@firestorm
 * @brief Aperture sprite placement for AyaneStorm DoF highlights.
 *
 * Attribute-free instanced draw: two triangles per highlight cell
 * (asDepthOfFieldHighlightF.glsl). Empty cells, cells dropped by the sprite
 * budget and cells of the other plane collapse to a degenerate triangle.
 */
// Cell energy (rgb) and occupancy (a), with mips.
uniform sampler2D specularRect;
// Cell centroid uv (xy), signed CoC (z).
uniform sampler2D emissiveRect;
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

flat out vec3 vary_energy;
flat out vec2 vary_center;
flat out float vary_radius;

// Same rule as asDepthOfFieldHighlightF.glsl keepCell().
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
    bool this_plane = plane > 0 ? data.z > 0.0 : data.z < 0.0;
    if (energy.a < 0.5 || !this_plane || !keepCell(cell))
    {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }

    float radius = max(plane > 0 ? data.z * max_radius : -data.z * near_max_radius, 1.0);
    vec2 center = data.xy * screen_res;
    // Margin for the antialiased edge: 1.5 target pixels.
    float half_size = radius * max(anamorphic_ratio, 1.0) +
                      1.5 * max(screen_res.x / target_res.x, 1.0);
    vec2 pixel = center + corners[(gl_InstanceID % 2) * 3 + gl_VertexID] * half_size;

    // Radiance per full-resolution pixel: the energy spread over the
    // aperture's area, so brightness is independent of shape and radius.
    vary_energy = energy.rgb / (unit_area * radius * radius);
    vary_center = center;
    vary_radius = radius;
    gl_Position = vec4(pixel / screen_res * 2.0 - 1.0, 0.0, 1.0);
}
