/**
 * @file asVolumetricAtlasF.glsl
 * @brief Cumulative depth atlas for transparency-correct volumetric lighting.
 * @author chanayane@firestorm
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;
in vec2 vary_fragcoord;

uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int sun_up_factor;
uniform vec3 sunlight_color;
uniform vec3 moonlight_color;
uniform float scatter_intensity;
uniform float scatter_asymmetry;
uniform float scatter_extinction;
uniform int atlas_debug;

vec3 getPositionWithNDC(vec3 ndc);
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);

const float MAX_MARCH_DISTANCE = 128.0;
const float ATLAS_GRID = 4.0;
const float ATLAS_SLICES = 16.0;

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
    vec2 atlas_pos = clamp(vary_fragcoord, vec2(0.0), vec2(0.999999)) * ATLAS_GRID;
    vec2 tile = floor(atlas_pos);
    vec2 screen_uv = fract(atlas_pos);
    float slice = tile.y * ATLAS_GRID + tile.x;

    // Quadratic slice placement concentrates precision around avatars and
    // nearby transparent geometry while still reaching the 128 m march cap.
    vec3 far_position = getPositionWithNDC(vec3(screen_uv * 2.0 - 1.0, 1.0));
    vec3 ray_dir = normalize(far_position);
    vec3 light_dir = normalize(sun_up_factor == 1 ? sun_dir : moon_dir);
    float phase = phaseHG(dot(ray_dir, light_dir), scatter_asymmetry);

    // Every slice reuses exactly the same preceding depth segments and adds
    // one new nonnegative segment. Independent fixed-count marches could make
    // a farther slice darker by missing lit samples seen by a nearer slice,
    // which violates the definition of a cumulative integral.
    float visibility_integral = 0.0;
    int slice_index = int(slice);
    for (int i = 0; i < 16; ++i)
    {
        if (i > slice_index) break;
        float near_fraction = float(i) / ATLAS_SLICES;
        float far_fraction = float(i + 1) / ATLAS_SLICES;
        float segment_near = near_fraction * near_fraction * MAX_MARCH_DISTANCE;
        float segment_far = far_fraction * far_fraction * MAX_MARCH_DISTANCE;
        float segment_length = segment_far - segment_near;
        float jitter = interleavedGradientNoise(
            screen_uv * vec2(4096.0, 2160.0) + vec2(float(i) * 17.0));
        float sample_distance = mix(segment_near, segment_far, jitter);
        vec3 sample_pos = ray_dir * sample_distance;
        float visibility = sampleDirectionalShadow(sample_pos, light_dir, screen_uv);
        if (visibility == visibility)
        {
            visibility_integral += clamp(visibility, 0.0, 1.0) *
                                   exp(-scatter_extinction * sample_distance) *
                                   segment_length;
        }
    }

    float scatter = clamp((visibility_integral / MAX_MARCH_DISTANCE) *
                          phase * scatter_intensity, 0.0, 1.0);
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
    frag_color = vec4(light_color * scatter, 1.0);
}
