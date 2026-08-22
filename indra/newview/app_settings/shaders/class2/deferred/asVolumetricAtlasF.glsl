/**
 * @file asVolumetricAtlasF.glsl
 * @brief Cumulative depth atlas for transparency-correct volumetric lighting.
 * @author chanayane@firestorm
 *
 * Builds one atlas tile (one of 16 cumulative camera-to-depth integrals) per
 * draw call. Each slice adds only its own new depth segment's raw integral
 * to the previous slice's already-resolved RAW integral (sampled from
 * previous_slice_integral, a single-tile-sized scratch copy carried between
 * slices) instead of re-summing every earlier segment's shadow sampling from
 * scratch - the earlier single-draw, 16-tile version did that
 * unconditionally, so the deepest tile alone repeated all 16 segments' work,
 * at full screen resolution, every frame.
 *
 * Every tile written to the real atlas (frag_color.rgb) is still the exact
 * same final, independently valid light_color * clamped-scatter value the
 * old version produced for that slice - consumers (alphaF.glsl,
 * pbralphaF.glsl, materialF.glsl, fullbrightF.glsl, waterF.glsl) sample and
 * blend between arbitrary tiles based on distance, not just the deepest
 * one, so every tile must remain self-contained in that same format.
 * previous_slice_integral is a SEPARATE scratch texture carrying only the
 * raw, unclamped, uncolored running sum between slices - it is never
 * sampled by any alpha/material shader and never substitutes for a real
 * atlas tile.
 *
 * frag_color.a carries scene transmittance T = exp(-extinction * distance)
 * at this tile's far boundary, per the Beer-Lambert law. Unlike the RGB
 * scatter value, T needs no cumulative ping-pong: extinction is a constant
 * coefficient and each tile already knows its own far-boundary distance
 * from slice_index alone, so T is computed directly per-tile with no
 * dependency on shadow visibility or the previous slice's result.
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;
// Raw, unbounded, uncolored cumulative integral written alongside the real
// atlas tile in the same draw (see file header) - carried into the next
// slice's previous_slice_integral input via a full-atlas-sized scratch
// render target attached as this program's second color attachment.
layout(location = 1) out float integral_out;
in vec2 vary_fragcoord;

uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int sun_up_factor;
uniform vec3 sunlight_color;
uniform vec3 moonlight_color;
uniform float scatter_albedo;
uniform float scatter_asymmetry;
uniform float scatter_density;
uniform int atlas_debug;

// Raw cumulative integral (R channel, unbounded, pre-color, pre-clamp)
// carried forward from the previous slice's draw. Unused/unbound when
// slice_index == 0.
uniform sampler2D previous_slice_integral;
uniform int slice_index;

vec3 getPositionWithNDC(vec3 ndc);
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);

const float MAX_MARCH_DISTANCE = 128.0;
const float ATLAS_SLICES = 16.0;
const float ATLAS_GRID = 4.0;

// Keep numerically identical to asVolumetricLightF.glsl's constant of the
// same name - both shaders must agree on the same brightness normalization
// so the raymarch and atlas paths produce matching scatter for the same
// density/albedo settings. See that file for the derivation/tuning note.
const float BRIGHTNESS_SCALE = 64.0;

float phaseHG(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

float interleavedGradientNoise(vec2 p)
{
    const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(p, magic.xy)));
}

void main()
{
    // The scissor rect confines which fragments this draw call actually
    // rasterizes to this slice's tile rectangle, but vary_fragcoord is
    // ALWAYS interpolated across the full [0,1] range of the bound
    // framebuffer (scissoring does not remap varyings) - vary_fragcoord at
    // a fragment inside tile (tile_x, tile_y) is therefore already
    // (tile_x + local_uv.x, tile_y + local_uv.y) / ATLAS_GRID, not a
    // standalone [0,1] UV. Recover this tile's own local UV explicitly.
    float this_tile_x = mod(float(slice_index), ATLAS_GRID);
    float this_tile_y = floor(float(slice_index) / ATLAS_GRID);
    vec2 screen_uv = clamp(vary_fragcoord * ATLAS_GRID - vec2(this_tile_x, this_tile_y),
                            vec2(0.0), vec2(0.999999));

    vec3 far_position = getPositionWithNDC(vec3(screen_uv * 2.0 - 1.0, 1.0));
    vec3 ray_dir = normalize(far_position);
    vec3 light_dir = normalize(sun_up_factor == 1 ? sun_dir : moon_dir);
    float phase = phaseHG(dot(ray_dir, light_dir), scatter_asymmetry);

    // This slice's own new segment only.
    float near_fraction = float(slice_index) / ATLAS_SLICES;
    float far_fraction = float(slice_index + 1) / ATLAS_SLICES;
    float segment_near = near_fraction * near_fraction * MAX_MARCH_DISTANCE;
    float segment_far = far_fraction * far_fraction * MAX_MARCH_DISTANCE;
    float segment_length = segment_far - segment_near;
    float jitter = interleavedGradientNoise(
        screen_uv * vec2(4096.0, 2160.0) + vec2(float(slice_index) * 17.0));
    float sample_distance = mix(segment_near, segment_far, jitter);
    vec3 sample_pos = ray_dir * sample_distance;
    float visibility = sampleDirectionalShadow(sample_pos, light_dir, screen_uv);

    float new_segment_integral = 0.0;
    if (visibility == visibility)
    {
        new_segment_integral = clamp(visibility, 0.0, 1.0) *
                               exp(-scatter_density * sample_distance) *
                               segment_length;
    }

    float visibility_integral = new_segment_integral;
    if (slice_index > 0)
    {
        // The previous slice's draw wrote its result into ITS OWN tile's
        // region of the ping-pong texture (that draw was scissored to
        // slice_index - 1's rectangle, at THAT slice's tile offset) - not
        // into this fragment's own tile region. Sample the previous tile's
        // offset with this fragment's local UV, matching where that data
        // actually landed, rather than this tile's own coordinates.
        float prev_tile_x = mod(float(slice_index - 1), ATLAS_GRID);
        float prev_tile_y = floor(float(slice_index - 1) / ATLAS_GRID);
        vec2 prev_uv = (vec2(prev_tile_x, prev_tile_y) + screen_uv) / ATLAS_GRID;
        visibility_integral += texture(previous_slice_integral, prev_uv).r;
    }

    // Carry the raw, unbounded integral forward for the next slice
    // regardless of debug mode - the scratch buffer must always reflect the
    // true running sum, never a debug-visualization value.
    integral_out = visibility_integral;

    // Real atlas tile: identical format/derivation to the original
    // single-draw version's per-slice output - every tile is independently
    // a full, valid, final light_color * clamped-scatter value.
    float scatter = clamp(scatter_density * scatter_albedo * BRIGHTNESS_SCALE *
                          phase * (visibility_integral / MAX_MARCH_DISTANCE), 0.0, 1.0);

    // Beer-Lambert transmittance at this tile's far boundary. A deterministic
    // function of slice_index and the constant density coefficient only -
    // no dependency on shadow visibility, so (unlike visibility_integral)
    // this needs no cumulative ping-pong across slices.
    float transmittance = exp(-scatter_density * segment_far);

    if (atlas_debug != 0)
    {
        // Exposure-safe magnitude encoding: R is the raw [0,1] range, G is
        // amplified 16x, and B 256x. Near slices should therefore begin blue,
        // then progress through cyan/white as cumulative scatter increases.
        vec3 diagnostic = clamp(scatter * vec3(1.0, 16.0, 256.0), 0.0, 1.0);
        vec2 edge = min(screen_uv, vec2(1.0) - screen_uv);
        if (min(edge.x, edge.y) < 0.006)
        {
            diagnostic = vec3(1.0, 0.0, 0.0);
        }
        frag_color = vec4(diagnostic, 1.0);
        return;
    }
    vec3 light_color = sun_up_factor == 1 ? sunlight_color : moonlight_color;
    frag_color = vec4(light_color * scatter, transmittance);
}
