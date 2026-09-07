/**
 * @file ascolorgradingF.glsl
 * @author chanayane@firestorm
 * @brief Perceptual scene color grading, mixer, grain, and final presentation.
 */
out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;
uniform vec2 screen_res;
uniform vec4 as_color_grade_basic1; // brightness, contrast, highlights, shadows
uniform vec4 as_color_grade_basic2; // whites, blacks, saturation, vibrance
uniform float as_color_grade_basic3; // global hue radians
uniform vec3 as_color_grade_bands[8]; // hue radians, saturation, luminance
uniform vec4 as_color_grade_colorize; // enabled, hue radians, saturation, luminance
uniform vec4 as_color_grade_split_toning1; // highlight hue/saturation, shadow hue/saturation
uniform vec2 as_color_grade_split_toning2; // enabled, balance
uniform vec4 as_color_grade_grain; // amount, size, roughness, color
uniform float as_color_grade_grain_seed;
uniform vec3 as_color_grade_snapshot_tile; // zoom, tile x, tile y

in vec2 vary_fragcoord;

const float PI = 3.14159265358979323846;
const float TAU = 6.28318530717958647692;
const float BAND_CENTERS[8] = float[8](0.0, 30.0, 60.0, 120.0, 180.0, 240.0, 280.0, 320.0);

vec3 srgbToLinear(vec3 c)
{
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 low = c / 12.92;
    vec3 high = pow(max((c + 0.055) / 1.055, vec3(0.0)), vec3(2.4));
    return mix(high, low, cutoff);
}

vec3 linearToSrgb(vec3 c)
{
    c = max(c, vec3(0.0));
    bvec3 cutoff = lessThanEqual(c, vec3(0.0031308));
    vec3 low = c * 12.92;
    vec3 high = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, cutoff);
}

vec3 linearToOklab(vec3 c)
{
    vec3 lms = mat3(0.4122214708, 0.2119034982, 0.0883024619,
                    0.5363325363, 0.6806995451, 0.2817188376,
                    0.0514459929, 0.1073969566, 0.6299787005) * c;
    lms = sign(lms) * pow(abs(lms), vec3(1.0 / 3.0));
    return mat3(0.2104542553, 1.9779984951, 0.0259040371,
                0.7936177850, -2.4285922050, 0.7827717662,
               -0.0040720468, 0.4505937099, -0.8086757660) * lms;
}

vec3 oklabToLinear(vec3 c)
{
    vec3 lms = mat3(1.0, 1.0, 1.0,
                    0.3963377774, -0.1055613458, -0.0894841775,
                    0.2158037573, -0.0638541728, -1.2914855480) * c;
    lms = lms * lms * lms;
    return mat3(4.0767416621, -1.2684380046, -0.0041960863,
               -3.3077115913, 2.6097574011, -0.7034186147,
                0.2309699292, -0.3413193965, 1.7076147010) * lms;
}

float hueDistance(float a, float b)
{
    float d = abs(a - b);
    return min(d, 360.0 - d);
}

float bandWeight(float hue, int index)
{
    float center = BAND_CENTERS[index];
    float previous = BAND_CENTERS[(index + 7) % 8];
    float next = BAND_CENTERS[(index + 1) % 8];
    float left_span = mod(center - previous + 360.0, 360.0);
    float right_span = mod(next - center + 360.0, 360.0);
    float signed_delta = mod(hue - center + 540.0, 360.0) - 180.0;
    float span = signed_delta < 0.0 ? left_span : right_span;
    return 1.0 - smoothstep(span * 0.42, span * 0.72, abs(signed_delta));
}

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Match the viewer's existing final presentation dither.
float presentationHash(float n) { return fract(sin(n) * 1e4); }
float presentationHash(vec2 p)
{
    return fract(1e4 * sin(17.0 * p.x + p.y * 0.1) * (0.1 + abs(sin(p.y * 13.0 + p.x))));
}
float presentationNoise(vec2 x)
{
    vec2 i = floor(x);
    vec2 f = fract(x);
    float a = presentationHash(i);
    float b = presentationHash(i + vec2(1.0, 0.0));
    float c = presentationHash(i + vec2(0.0, 1.0));
    float d = presentationHash(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

vec3 clampHDRRange(vec3 color);

vec3 gamutCompress(vec3 rgb)
{
    float minimum = min(rgb.r, min(rgb.g, rgb.b));
    if (minimum < 0.0) rgb -= minimum;
    float maximum = max(rgb.r, max(rgb.g, rgb.b));
    if (maximum > 1.0) rgb /= maximum;
    return rgb;
}

void main()
{
    vec4 source = texture(diffuseRect, vary_fragcoord);
    vec3 lab = linearToOklab(srgbToLinear(clamp(source.rgb, 0.0, 1.0)));
    float L = lab.x;

    L += as_color_grade_basic1.x * 0.25 * (1.0 - abs(2.0 * L - 1.0));
    L = 0.5 + (L - 0.5) * exp2(as_color_grade_basic1.y * 2.0);
    L += as_color_grade_basic1.z * 0.20 * smoothstep(0.45, 0.85, L);
    L += as_color_grade_basic1.w * 0.20 * (1.0 - smoothstep(0.15, 0.55, L));
    L += as_color_grade_basic2.x * 0.15 * smoothstep(0.75, 1.0, L);
    L += as_color_grade_basic2.y * 0.15 * (1.0 - smoothstep(0.0, 0.25, L));
    L = clamp(L, 0.0, 1.0);

    float C = length(lab.yz);
    float hue = C > 1e-6 ? atan(lab.z, lab.y) : 0.0;
    hue = mod(hue + as_color_grade_basic3 + TAU, TAU);
    C *= max(0.0, 1.0 + as_color_grade_basic2.z);

    float hue_degrees = degrees(hue);
    float warm = max(1.0 - smoothstep(25.0, 65.0, hueDistance(hue_degrees, 35.0)), 0.0);
    float vibrance = as_color_grade_basic2.w;
    if (vibrance >= 0.0)
    {
        float headroom = 1.0 - smoothstep(0.05, 0.30, C);
        C *= 1.0 + vibrance * headroom * mix(1.0, 0.65, warm);
    }
    else
    {
        C *= 1.0 + vibrance;
    }

    float weights[8];
    float weight_sum = 0.0;
    float neutral_guard = smoothstep(0.01, 0.05, C);
    for (int i = 0; i < 8; ++i)
    {
        weights[i] = bandWeight(hue_degrees, i) * neutral_guard;
        weight_sum += weights[i];
    }
    float normalization = max(1.0, weight_sum);
    float hue_shift = 0.0;
    float saturation_shift = 0.0;
    float luminance_shift = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float weight = weights[i] / normalization;
        hue_shift += as_color_grade_bands[i].x * weight;
        saturation_shift += as_color_grade_bands[i].y * weight;
        luminance_shift += as_color_grade_bands[i].z * weight;
    }
    if (as_color_grade_colorize.x > 0.5)
    {
        hue = as_color_grade_colorize.y;
        // Replace source chroma so the result reads as a tinted monochrome image.
        C = as_color_grade_colorize.z * 0.22 * (0.35 + 0.65 * 4.0 * L * (1.0 - L));
        L = clamp(L + as_color_grade_colorize.w * 0.25 * 4.0 * L * (1.0 - L), 0.0, 1.0);
    }
    else
    {
        hue = mod(hue + hue_shift + TAU, TAU);
        C *= max(0.0, 1.0 + saturation_shift);
        L = clamp(L + luminance_shift * 0.25 * 4.0 * L * (1.0 - L), 0.0, 1.0);
    }

    lab = vec3(L, C * cos(hue), C * sin(hue));

    // Split toning follows Basic and Mixer/Colorize and precedes grain.
    if (as_color_grade_split_toning2.x > 0.5)
    {
        float split_pivot = 0.5 - as_color_grade_split_toning2.y * 0.30;
        float highlight_tone = smoothstep(split_pivot - 0.25, split_pivot + 0.25, L);
        float shadow_tone = 1.0 - highlight_tone;
        vec2 highlight_color = vec2(cos(as_color_grade_split_toning1.x), sin(as_color_grade_split_toning1.x));
        vec2 shadow_color = vec2(cos(as_color_grade_split_toning1.z), sin(as_color_grade_split_toning1.z));
        lab.yz += highlight_color * as_color_grade_split_toning1.y * highlight_tone * 0.12;
        lab.yz += shadow_color * as_color_grade_split_toning1.w * shadow_tone * 0.12;
    }
    vec3 graded = linearToSrgb(gamutCompress(oklabToLinear(lab)));

    float zoom = max(as_color_grade_snapshot_tile.x, 1.0);
    vec2 full_uv = (vary_fragcoord + as_color_grade_snapshot_tile.yz) / zoom;
    vec2 pixel = full_uv * screen_res * zoom;
    float grain_size = mix(1.0, 8.0, as_color_grade_grain.y);
    vec2 grain_pixel = floor(pixel / grain_size);
    float fine = hash12(grain_pixel + as_color_grade_grain_seed);
    float coarse = hash12(floor(grain_pixel * 0.35) + as_color_grade_grain_seed * 1.37);
    float mono = mix(fine, coarse, as_color_grade_grain.z) - 0.5;
    vec3 colored = vec3(hash12(grain_pixel + as_color_grade_grain_seed + 17.0),
                        hash12(grain_pixel + as_color_grade_grain_seed + 43.0),
                        hash12(grain_pixel + as_color_grade_grain_seed + 79.0)) - 0.5;
    vec3 grain_noise = mix(vec3(mono), colored, as_color_grade_grain.w);
    float grain_envelope = 0.35 + 0.65 * 4.0 * L * (1.0 - L);
    graded += grain_noise * as_color_grade_grain.x * 0.08 * grain_envelope;

    // Preserve the viewer's existing small final dither separately from artistic grain.
#ifdef HAS_NOISE
    vec2 tc = vary_fragcoord * screen_res * 4.0;
    vec3 seed = (graded + vec3(1.0)) * vec3(tc.xy, tc.x + tc.y);
    vec3 nz = vec3(presentationNoise(seed.rg), presentationNoise(seed.gb), presentationNoise(seed.rb));
    graded += nz * 0.003;
#endif
    frag_color = vec4(clampHDRRange(graded), source.a);
    gl_FragDepth = texture(depthMap, vary_fragcoord).r;
}
