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
// Lens field (asdepthoffield.cpp, setLensUniforms()), as in the gathers.
uniform vec2 field_scale;
uniform float cat_eye;
uniform float astigmatism;
uniform float ca_shift;

flat out vec3 vary_energy;
flat out vec2 vary_center;
flat out float vary_radius;
// Field position, (radial, circumferential) aperture scales, barrel centre
// and axial CA shift (px) of this sprite (asDepthOfFieldSpriteF.glsl).
flat out vec2 vary_field;
flat out vec2 vary_axis_scale;
flat out vec2 vary_barrel;
flat out float vary_delta;

// Cat's eye and astigmatism, see asDepthOfFieldFarF.glsl.
vec2 barrelCenter(vec2 field)
{
    vec2 shift = cat_eye * field;
    float len = length(shift);
    return len > 1.6 ? shift * (1.6 / len) : shift;
}

vec2 astigmaticScale(vec2 field, float signed_radius, float plane_radius)
{
    float split = abs(astigmatism) * dot(field, field) * plane_radius;
    float radius = abs(signed_radius);
    if (split <= 0.0 || radius <= 0.0)
    {
        return vec2(1.0);
    }
    float t = clamp((radius - 2.0 * split) / radius, -1.0, 1.0);
    t = t < 0.0 ? min(t, -0.1) : max(t, 0.1);
    return (astigmatism > 0.0) == (signed_radius >= 0.0) ? vec2(t, 1.0) : vec2(1.0, t);
}

// Open fraction of a unit-circle aperture clipped by a unit circle at
// distance d: lens (vesica) area over pi (as in asDoFAccumulateF.glsl).
float catEyeFraction(float d)
{
    if (d >= 2.0)
    {
        return 0.0;
    }
    float h = 0.5 * d;
    return (2.0 * acos(h) - 2.0 * h * sqrt(1.0 - h * h)) / 3.14159265358979323846;
}

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
    vary_field = vec2(0.0);
    vary_axis_scale = vec2(1.0);
    vary_barrel = vec2(0.0);
    vary_delta = 0.0;
    bool this_plane = plane > 0 ? data.z > 0.0 : data.z < 0.0;
    if (energy.a < 0.5 || !this_plane || !keepCell(cell))
    {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }

    float plane_radius = plane > 0 ? max_radius : near_max_radius;
    float radius = max(plane > 0 ? data.z * max_radius : -data.z * near_max_radius, 1.0);
    vec2 center = data.xy * screen_res;
    vec2 field = (data.xy - 0.5) * field_scale;
    vec2 axis_scale = astigmaticScale(field, plane > 0 ? radius : -radius, plane_radius);
    vec2 barrel = barrelCenter(field);
    // Axial CA widens the bluest (far) or reddest (near) disc by delta.
    float delta = ca_shift * plane_radius;
    // Margin for the antialiased edge: 1.5 target pixels. The deformation
    // and the barrel only shrink the shape.
    float half_size = (radius + delta) * max(anamorphic_ratio, 1.0) +
                      1.5 * max(screen_res.x / target_res.x, 1.0);
    vec2 pixel = center + corners[(gl_InstanceID % 2) * 3 + gl_VertexID] * half_size;

    // Radiance per full-resolution pixel: the energy spread over the
    // aperture's area, so brightness is independent of shape and radius.
    // The astigmatic scales and the open share of the clipped aperture
    // (exact for a circle) shrink that area.
    float area = unit_area * radius * radius * abs(axis_scale.x * axis_scale.y);
    if (cat_eye > 0.0)
    {
        area *= max(catEyeFraction(length(barrel)), 0.05);
    }
    vary_energy = energy.rgb / area;
    vary_center = center;
    vary_radius = radius;
    vary_field = field;
    vary_axis_scale = axis_scale;
    vary_barrel = barrel;
    vary_delta = delta;
    gl_Position = vec4(pixel / screen_res * 2.0 - 1.0, 0.0, 1.0);
}
