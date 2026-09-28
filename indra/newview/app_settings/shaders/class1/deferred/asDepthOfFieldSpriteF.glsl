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

flat in vec3 vary_energy;
flat in vec2 vary_center;
flat in float vary_radius;

#define AS_DOF_PI 3.14159265358979323846

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

// Signed distance inside the aperture edge, in full-resolution pixels.
float edgeDistance(vec2 pixel)
{
    // Background points image as the upright aperture, foreground points as
    // the inverted one (see ASDoFCamera).
    vec2 q = (pixel - vary_center) * (plane > 0 ? 1.0 : -1.0);
    q.x /= anamorphic_ratio;
    float r = length(q) / vary_radius;
    float phi = atan(q.y, q.x) - aperture_rotation;
    float edge_scale;
    return (boundary(phi, edge_scale) - r) * edge_scale * vary_radius;
}

void main()
{
    float pixel_scale = max(screen_res.x / target_res.x, 1.0);
    vec2 pixel = gl_FragCoord.xy * screen_res / target_res;
    // Box-filter antialiasing over one target pixel. Sprites under 12 target
    // pixels in radius use a 4-sample rotated grid: a single sample leaves
    // over 1% energy error there (scripts/testing/dof_reference.py).
    float coverage;
    if (vary_radius < 12.0 * pixel_scale)
    {
        const vec2 grid[4] = vec2[4](vec2(0.125, 0.375), vec2(-0.375, 0.125),
                                     vec2(-0.125, -0.375), vec2(0.375, -0.125));
        coverage = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            coverage += clamp(edgeDistance(pixel + grid[i] * pixel_scale) /
                              (0.5 * pixel_scale) + 0.5, 0.0, 1.0);
        }
        coverage *= 0.25;
    }
    else
    {
        coverage = clamp(edgeDistance(pixel) / pixel_scale + 0.5, 0.0, 1.0);
    }
    vec4 sprite = vec4(vary_energy * coverage, 0.0);
    frag_data0 = plane > 0 ? sprite : vec4(0.0);
    frag_data1 = plane > 0 ? vec4(0.0) : sprite;
    frag_data2 = vec4(0.0);
}
