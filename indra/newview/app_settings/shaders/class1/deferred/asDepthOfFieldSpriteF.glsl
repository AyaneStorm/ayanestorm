/**
 * @file asDepthOfFieldSpriteF.glsl
 * @author chanayane@firestorm
 * @brief Analytic aperture sprite for AyaneStorm DoF highlights.
 *
 * Live DoF (asdoflive.cpp): additive (ONE, ONE) into the background or the
 * veil, location 0 for both planes. Alpha stays 0 so layer coverage is
 * unchanged: the sprite adds the light removed from the gather input. Exact
 * per-stratum coverage (liveCoverage()); the back part is drawn under the
 * front layer (liveBackVisibility()).
 */
layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;
// Further attachments of the bound target: add nothing rather than leave
// them undefined.
layout(location = 2) out vec4 frag_data2;

uniform vec2 screen_res;
uniform vec2 target_res;
uniform int plane;
uniform int aperture_blades;
uniform float aperture_roundness;
uniform float aperture_rotation;
uniform float anamorphic_ratio;
uniform float cat_eye;
uniform float sa_strength;  // spherical aberration, -5..5; 0 off

flat in vec3 vary_energy;
flat in vec2 vary_center;
flat in float vary_radius;
// Barrel centre and axial CA shift (px) of this sprite
// (asDepthOfFieldSpriteV.glsl).
flat in vec2 vary_barrel;
flat in float vary_delta;

#define AS_DOF_PI 3.14159265358979323846

// Axial chromatic aberration strata and channel weights, as in the Live
// gather (asDoFLiveGatherF.glsl).
const vec4 CA_STRATA = vec4(-0.75, -0.25, 0.25, 0.75);
const vec4 CA_RED = vec4(0.0625, 0.1875, 0.3125, 0.4375);
const vec4 CA_GREEN = vec4(0.1590909, 0.3409091, 0.3409091, 0.1590909);
const vec4 CA_BLUE = vec4(0.4375, 0.3125, 0.1875, 0.0625);

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

// Spherical aberration, the gathers' weight at the relative disc radii rho
// (one per axial CA stratum). It averages to 1
// over the aperture area, so the sprite keeps its energy.
// Mean of max(1 + c - 2 c u, 0) over u in [0, 1]: 1 for |c| <= 1.
float sphericalNorm(float c)
{
    if (abs(c) <= 1.0)
    {
        return 1.0;
    }
    float q = (1.0 + c) * (1.0 + c) / (4.0 * abs(c));
    return c > 0.0 ? q : 1.0 + q;
}

vec4 sphericalWeight(vec4 rho)
{
    if (sa_strength == 0.0)
    {
        return vec4(1.0);
    }
    float sigma = plane > 0 ? clamp(vary_radius / 3.0, 0.0, 1.0) :
                              -clamp(vary_radius / 3.0, 0.0, 1.0);
    vec4 pupil_r2 = clamp(rho * rho, vec4(0.0), vec4(1.0));
    return max(vec4(1.0) - sa_strength * sigma * (2.0 * pupil_r2 - vec4(1.0)), vec4(0.0)) /
           sphericalNorm(sa_strength * sigma);
}

// Live DoF (asdoflive.cpp) draws at its gather resolution, two full pixels
// per target pixel. Every stratum is an exact scaled aperture with its own
// antialiasing grid and keeps its energy, down to a 2 px triangle under
// strong axial CA (scripts/testing/dof_live_reference.py, live_sprite()).

// Signed distance inside the aperture of this radius, barrel included (the
// nearer edge wins), full-resolution pixels (x), and the pupil position over
// the edge radius (y, spherical aberration).
vec2 liveEdge(vec2 pixel, float radius)
{
    vec2 u = (pixel - vary_center) * (plane > 0 ? 1.0 : -1.0) / radius;
    vec2 v = vec2(u.x / anamorphic_ratio, u.y);
    float r = length(v);
    float edge_scale;
    float b = boundary(atan(v.y, v.x) - aperture_rotation, edge_scale);
    float edge = (b - r) * edge_scale * radius;
    if (cat_eye > 0.0)
    {
        edge = min(edge, (1.0 - length(u - vary_barrel)) * radius);
    }
    return vec2(edge, r / max(b, 0.0001));
}

// Grids in target pixels: 8x8, 4x4 (rows offset by 1/8) and a rotated 4 by
// stratum size; sub-pixel features (a 3 px triangle, a thin
// spherical ring) need the finer ones.
vec2 liveGridOffset(int count, int i)
{
    if (count == 64)
    {
        return (vec2(float(i % 8), float(i / 8)) + 0.5) / 8.0 - 0.5;
    }
    if (count == 16)
    {
        float x = float(i % 4);
        float y = float(i / 4);
        return (vec2(x, y) + 0.5) / 4.0 - 0.5 +
               (vec2(mod(y, 2.0), mod(x, 2.0)) - 0.5) * 0.125;
    }
    if (count == 4)
    {
        const vec2 grid[4] = vec2[4](vec2(0.125, 0.375), vec2(-0.375, 0.125),
                                     vec2(-0.125, -0.375), vec2(0.375, -0.125));
        return grid[i];
    }
    return vec2(0.0);
}

// Part drawn (asDepthOfFieldSpriteV.glsl): 0 the back part, 1 the front.
uniform int live_part;
// Back part, far: B2 completed (S.rgb, W) and energies (.z = M), at the
// target's resolution. A pixel shows the light only where its own B2
// kernel M / W reaches it, as the far gather sees it: nearer, sharper B2
// surfaces (palm leaves over a lit backdrop) hide it.
uniform sampler2D diffuseRect;
uniform sampler2D normalMap;
// Back part, near: N2 (premultiplied), whose coverage lies over N1.
uniform sampler2D noiseMap;

float liveBackVisibility(vec2 pixel, float pixel_scale)
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    if (plane < 0)
    {
        return 1.0 - clamp(texelFetch(noiseMap, texel, 0).a, 0.0, 1.0);
    }
    float weight = texelFetch(diffuseRect, texel, 0).a;
    if (weight <= 0.000001)
    {
        // No far background here: the blend (DST_ALPHA) adds nothing.
        return 1.0;
    }
    float kernel = texelFetch(normalMap, texel, 0).z / weight;
    // Aperture-space distance from the light, target pixels; full up to
    // half a pixel past the kernel (the antialiased rim), then over one.
    float distance = liveEdge(pixel, vary_radius).y * vary_radius / pixel_scale;
    return clamp(kernel - distance + 1.5, 0.0, 1.0);
}

vec3 liveCoverage(vec2 pixel, float pixel_scale)
{
    float sigma = plane > 0 ? 1.0 : -1.0;
    bool ca = vary_delta > 0.01;
    vec3 total = vec3(0.0);
    for (int k = 0; k < 4; ++k)
    {
        if (!ca && k > 0)
        {
            break;
        }
        float radius = ca ? max(vary_radius - sigma * vary_delta * CA_STRATA[k], 1.0) :
                            vary_radius;
        int count = radius < 3.0 * pixel_scale ? 64 :
                    (radius < 6.0 * pixel_scale ? 16 : (radius < 12.0 * pixel_scale ? 4 : 1));
        float aa = (count == 64 ? 0.125 : (count == 16 ? 0.25 : (count == 4 ? 0.5 : 1.0))) *
                   pixel_scale;
        float cover = 0.0;
        for (int i = 0; i < 64; ++i)
        {
            if (i >= count)
            {
                break;
            }
            vec2 edge = liveEdge(pixel + liveGridOffset(count, i) * pixel_scale, radius);
            cover += clamp(edge.x / aa + 0.5, 0.0, 1.0) * sphericalWeight(vec4(edge.y)).x;
        }
        // Each stratum's disc carries (R / R_k)^2 of the radiance.
        cover *= vary_radius * vary_radius / (radius * radius * float(count));
        total += ca ? cover * vec3(CA_RED[k], CA_GREEN[k], CA_BLUE[k]) : vec3(cover);
    }
    return total;
}

void main()
{
    float pixel_scale = max(screen_res.x / target_res.x, 1.0);
    vec2 pixel = gl_FragCoord.xy * screen_res / target_res;
    vec3 coverage = liveCoverage(pixel, pixel_scale);
    if (live_part == 0)
    {
        coverage *= liveBackVisibility(pixel, pixel_scale);
    }
    // Both planes into location 0: Live binds the background or the veil,
    // whose attachment 1 is its per-channel coverage.
    frag_data0 = vec4(vary_energy * coverage, 0.0);
    frag_data1 = vec4(0.0);
    frag_data2 = vec4(0.0);
}
