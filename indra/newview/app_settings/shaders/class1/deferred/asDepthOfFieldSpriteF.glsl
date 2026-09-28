/**
 * @file asDepthOfFieldSpriteF.glsl
 * @author chanayane@firestorm
 * @brief Analytic aperture sprite for AyaneStorm DoF highlights.
 *
 * Additive (ONE, ONE) into the far target, or the near target's front layer
 * (attachment 1). Alpha stays 0 so layer coverage is unchanged: the sprite
 * adds the light removed from the gather input.
 */
layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;
// Radius moments attachment of the near target (postfilter input, already
// consumed): add nothing rather than leave it undefined.
layout(location = 2) out vec4 frag_data2;

uniform vec2 screen_res;
uniform vec2 target_res;
uniform int plane;
uniform int aperture_blades;
uniform float aperture_roundness;
uniform float aperture_rotation;
uniform float anamorphic_ratio;
uniform float cat_eye;

flat in vec3 vary_energy;
flat in vec2 vary_center;
flat in float vary_radius;
// Lens field of this sprite (asDepthOfFieldSpriteV.glsl).
flat in vec2 vary_field;
flat in vec2 vary_axis_scale;
flat in vec2 vary_barrel;
flat in float vary_delta;

#define AS_DOF_PI 3.14159265358979323846

// Axial chromatic aberration strata and channel weights, see
// asDepthOfFieldFarF.glsl.
const vec4 CA_STRATA = vec4(-0.75, -0.25, 0.25, 0.75);
const vec4 CA_RED = vec4(0.0625, 0.1875, 0.3125, 0.4375);
const vec4 CA_GREEN = vec4(0.1590909, 0.3409091, 0.3409091, 0.1590909);
const vec4 CA_BLUE = vec4(0.4375, 0.3125, 0.1875, 0.0625);

// Scales an offset along the radial and circumferential axes of the field
// position (astigmatism, see asDepthOfFieldFarF.glsl).
vec2 deform(vec2 offset, vec2 axis_scale)
{
    float len = length(vary_field);
    if (len < 0.0001 || axis_scale == vec2(1.0))
    {
        return offset;
    }
    vec2 radial = vary_field / len;
    vec2 circumferential = vec2(-radial.y, radial.x);
    return radial * (dot(offset, radial) * axis_scale.x) +
           circumferential * (dot(offset, circumferential) * axis_scale.y);
}

// Full-resolution pixels per unit aperture distance across an edge with
// aperture-space normal n: the deformation stretches the aperture by
// vary_radius * axis scale, so a gap f along n spans f * vary_radius /
// |D^-1 n| pixels.
float pixelsPerUnit(vec2 n)
{
    return vary_radius / length(deform(n, 1.0 / vary_axis_scale));
}

// Unit-circumradius edge radius at a polar angle before rotation; a blade
// vertex lies at angle 0, as in ASDoFAperture and the gathers. edge_scale
// converts a radial gap into the distance perpendicular to the edge.
float boundary(float phi, out float edge_scale)
{
    edge_scale = 1.0;
    if (aperture_blades < 3)
    {
        return 1.0;
    }
    float sector = 2.0 * AS_DOF_PI / float(aperture_blades);
    float local_angle = mod(phi, sector) - 0.5 * sector;
    float polygon = cos(0.5 * sector) / max(cos(local_angle), 0.001);
    edge_scale = mix(cos(local_angle), 1.0, aperture_roundness);
    return mix(polygon, 1.0, aperture_roundness);
}

// Signed distance inside the aperture edge, in full-resolution pixels (x),
// and its rate of change with the sprite radius (y, for axial CA).
vec2 edgeDistance(vec2 pixel)
{
    // Background points image as the upright aperture, foreground points as
    // the inverted one (see ASDoFCamera). u: unit aperture position,
    // anamorphic scale included, with the astigmatic deformation undone.
    vec2 q = (pixel - vary_center) * (plane > 0 ? 1.0 : -1.0);
    vec2 u = deform(q, 1.0 / vary_axis_scale) / vary_radius;
    vec2 v = vec2(u.x / anamorphic_ratio, u.y);
    float r = length(v);
    float phi = atan(v.y, v.x) - aperture_rotation;
    float edge_scale;
    float b = boundary(phi, edge_scale);
    float len = length(u);
    float to_pixels = pixelsPerUnit(len > 0.0001 ? u / len : vec2(1.0, 0.0));
    vec2 edge = vec2((b - r) * edge_scale, b * edge_scale) *
                vec2(to_pixels, to_pixels / vary_radius);
    // Cat's eye: also inside the barrel's unit circle; the nearer edge wins.
    if (cat_eye > 0.0)
    {
        vec2 w = u - vary_barrel;
        float wl = length(w);
        vec2 n = wl > 0.0001 ? w / wl : vec2(1.0, 0.0);
        float barrel_pixels = pixelsPerUnit(n);
        float barrel_edge = (1.0 - wl) * barrel_pixels;
        if (barrel_edge < edge.x)
        {
            edge = vec2(barrel_edge,
                        (1.0 + dot(n, vary_barrel)) * barrel_pixels / vary_radius);
        }
    }
    return edge;
}

// Coverage of one antialiasing sample: per channel with axial CA (each
// stratum's disc of radius R - sigma delta s carries (R / R_s)^2 of the
// radiance, so every channel keeps its energy), else the same for all.
vec3 sampleCoverage(vec2 pixel, float aa_width)
{
    vec2 edge = edgeDistance(pixel);
    if (vary_delta <= 0.01)
    {
        return vec3(clamp(edge.x / aa_width + 0.5, 0.0, 1.0));
    }
    float sigma = plane > 0 ? 1.0 : -1.0;
    vec4 radii = max(vec4(vary_radius) - sigma * vary_delta * CA_STRATA, vec4(1.0));
    vec4 cover = clamp((vec4(edge.x) + (radii - vec4(vary_radius)) * edge.y) / aa_width +
                       vec4(0.5), vec4(0.0), vec4(1.0));
    cover *= vec4(vary_radius * vary_radius) / (radii * radii);
    return vec3(dot(cover, CA_RED), dot(cover, CA_GREEN), dot(cover, CA_BLUE));
}

void main()
{
    float pixel_scale = max(screen_res.x / target_res.x, 1.0);
    vec2 pixel = gl_FragCoord.xy * screen_res / target_res;
    // Box-filter antialiasing over one target pixel. Sprites under 12 target
    // pixels in radius use a 4-sample rotated grid: a single sample leaves
    // over 1% energy error there (scripts/testing/dof_reference.py).
    vec3 coverage;
    if (vary_radius < 12.0 * pixel_scale)
    {
        const vec2 grid[4] = vec2[4](vec2(0.125, 0.375), vec2(-0.375, 0.125),
                                     vec2(-0.125, -0.375), vec2(0.375, -0.125));
        coverage = vec3(0.0);
        for (int i = 0; i < 4; ++i)
        {
            coverage += sampleCoverage(pixel + grid[i] * pixel_scale, 0.5 * pixel_scale);
        }
        coverage *= 0.25;
    }
    else
    {
        coverage = sampleCoverage(pixel, pixel_scale);
    }
    vec4 sprite = vec4(vary_energy * coverage, 0.0);
    frag_data0 = plane > 0 ? sprite : vec4(0.0);
    frag_data1 = plane > 0 ? vec4(0.0) : sprite;
    frag_data2 = vec4(0.0);
}
