/**
 * @file ascolorgradingF.glsl
 * @author chanayane@firestorm
 * @brief Perceptual scene color grading, mixer, grain, and final presentation.
 */
out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;
uniform sampler3D as_color_grade_lut;
uniform sampler2D as_color_grade_lut_1d;
uniform float as_color_grade_lut_strength;
uniform int as_color_grade_lut_type; // 0 disabled, 1 one-dimensional, 3 three-dimensional
uniform vec3 as_color_grade_lut_min;
uniform vec3 as_color_grade_lut_domain_scale;
uniform vec2 as_color_grade_lut_texel; // (size - 1) / size, 0.5 / size
uniform vec2 screen_res;
uniform vec4 as_color_grade_basic1; // brightness, contrast, highlights, shadows
uniform vec4 as_color_grade_basic2; // whites, blacks, saturation, vibrance
uniform float as_color_grade_basic3; // global hue radians
uniform vec3 as_color_grade_bands[24]; // hue radians, saturation, lightness
uniform vec3 as_color_grade_band_selection_colors[24]; // selection colors in OKLab
uniform vec3 as_color_grade_band_parameters[24]; // strength, hue range, softness
uniform vec2 as_color_grade_band_ranges[24]; // relative-chroma range, lightness range
uniform vec4 as_color_grade_colorize; // enabled, hue radians, saturation, lightness
uniform vec4 as_color_grade_split_toning1; // highlight hue/saturation, shadow hue/saturation
uniform vec2 as_color_grade_split_toning2; // enabled, balance
uniform int as_color_grade_negative; // invert the final display-referred scene RGB
uniform vec4 as_color_grade_grain; // amount, size, roughness, color
uniform float as_color_grade_grain_seed;
uniform vec3 as_color_grade_snapshot_tile; // zoom, tile x, tile y

in vec2 vary_fragcoord;

const float PI = 3.14159265358979323846;
const float TAU = 6.28318530717958647692;
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

float distanceWeight(float distance_from_center, float radius, float softness)
{
    if (radius <= 0.0001) return 1.0 - step(0.0001, distance_from_center);
    if (softness <= 0.0001) return 1.0 - step(radius, distance_from_center);
    // This curve makes UI softness 50 reproduce the former 0.42/0.72 feather.
    float feather = softness * (softness + 2.0) / 3.0;
    float weight = 1.0 - smoothstep(radius * (1.0 - feather), radius, distance_from_center);
    // Above the compatible midpoint, increasingly suppress colors farther
    // from the selection center; softness 100 uses a strong fourth-power falloff.
    float high_softness = smoothstep(0.5, 1.0, softness);
    return pow(weight, mix(1.0, 4.0, high_softness));
}

float independentRangeWeight(float distance_from_center, float range,
                             float softness, float curve)
{
    // Maximum range deliberately ignores this dimension, including softness.
    if (range >= 0.9999) return 1.0;
    return distanceWeight(distance_from_center, pow(range, curve), softness);
}

float colorBandWeight(vec3 lab, int index, float hue_range, float chroma_range,
                      float lightness_range, float softness)
{
    vec3 selection_color = as_color_grade_band_selection_colors[index];
    float selection_chroma = length(selection_color.yz);
    float pixel_chroma = length(lab.yz);
    float selection_relative_chroma = clamp(selection_chroma / max(selection_color.x, 0.02), 0.0, 1.0);
    float pixel_relative_chroma = clamp(pixel_chroma / max(lab.x, 0.02), 0.0, 1.0);

    // Lightness Range 100 ignores lightness. Relative chroma stays approximately
    // constant when illumination makes the same surface lighter or darker.
    float lightness_weight = independentRangeWeight(abs(lab.x - selection_color.x),
                                                    lightness_range, softness, 2.0);
    float gray_chroma_weight = independentRangeWeight(pixel_relative_chroma,
                                                      chroma_range, softness, 3.0);
    float gray_weight = lightness_weight * gray_chroma_weight;

    // Chromatic bands always require a hue match. Hue Range 100 is capped at
    // +/-60 degrees, so even its broad legacy-like mask cannot reach other hues.
    float hue_separation = PI;
    if (pixel_chroma > 0.000001 && selection_chroma > 0.000001)
    {
        float delta = abs(atan(lab.z, lab.y) - atan(selection_color.z, selection_color.y));
        hue_separation = min(delta, TAU - delta);
    }
    float hue_weight = distanceWeight(hue_separation,
                                      hue_range * PI / 3.0, softness);

    float chroma_weight = independentRangeWeight(
        abs(pixel_relative_chroma - selection_relative_chroma),
        chroma_range, softness, 3.0);
    float chromatic_weight = hue_weight * chroma_weight * lightness_weight;

    // Hue becomes perceptually undefined close to neutral. Blend smoothly to
    // the gray selector over that range instead of switching at one value.
    float hue_definition = smoothstep(0.0, 0.03, selection_chroma);
    return mix(gray_weight, chromatic_weight, hue_definition);
}

// Keep time in a separate hash dimension so refreshing grain randomizes each
// cell instead of translating the pattern diagonally across the image.
float grainHash(vec2 cell, float seed)
{
    vec3 p3 = fract(vec3(cell, seed) * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
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

    float weights[24];
    float weight_sum = 0.0;
    vec3 mixer_lab = vec3(L, C * cos(hue), C * sin(hue));
    for (int i = 0; i < 24; ++i)
    {
        float selection = colorBandWeight(mixer_lab, i,
            as_color_grade_band_parameters[i].y, as_color_grade_band_ranges[i].x,
            as_color_grade_band_ranges[i].y, as_color_grade_band_parameters[i].z);
        // Bands with no actual adjustment must not dilute an overlapping band.
        float active = step(0.000001, dot(abs(as_color_grade_bands[i]), vec3(1.0))) *
            step(0.000001, as_color_grade_band_parameters[i].x);
        weights[i] = selection * active;
        weight_sum += weights[i];
    }
    float normalization = max(1.0, weight_sum);
    vec2 source_chroma = vec2(C * cos(hue), C * sin(hue));
    vec2 chroma_shift = vec2(0.0);
    float saturation_shift = 0.0;
    float lightness_shift = 0.0;
    for (int i = 0; i < 24; ++i)
    {
        // Apply strength after overlap normalization so 50 is exactly half of 100.
        float weight = weights[i] / normalization * as_color_grade_band_parameters[i].x;
        float shift_cos = cos(as_color_grade_bands[i].x);
        float shift_sin = sin(as_color_grade_bands[i].x);
        vec2 rotated_chroma = vec2(source_chroma.x * shift_cos - source_chroma.y * shift_sin,
                                   source_chroma.x * shift_sin + source_chroma.y * shift_cos);
        // Blend chroma vectors rather than signed angles so -180 and +180
        // remain identical even when selection weight is fractional.
        chroma_shift += (rotated_chroma - source_chroma) * weight;
        saturation_shift += as_color_grade_bands[i].y * weight;
        lightness_shift += as_color_grade_bands[i].z * weight;
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
        vec2 mixed_chroma = source_chroma + chroma_shift;
        C = length(mixed_chroma);
        hue = C > 0.000001 ? atan(mixed_chroma.y, mixed_chroma.x) : hue;
        hue = mod(hue + TAU, TAU);
        C *= max(0.0, 1.0 + saturation_shift);
        L = clamp(L + lightness_shift * 0.25 * 4.0 * L * (1.0 - L), 0.0, 1.0);
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

    // Display-referred LUT follows tonal/color adjustments and precedes grain/Negative.
    if (as_color_grade_lut_strength > 0.0)
    {
        vec3 coordinate = clamp((graded - as_color_grade_lut_min) * as_color_grade_lut_domain_scale, 0.0, 1.0);
        // Map domain endpoints to texel centers for correct hardware trilinear interpolation.
        coordinate = coordinate * as_color_grade_lut_texel.x + as_color_grade_lut_texel.y;
        vec3 lut_color;
        if (as_color_grade_lut_type == 1)
        {
            lut_color.r = texture(as_color_grade_lut_1d, vec2(coordinate.r, 0.5)).r;
            lut_color.g = texture(as_color_grade_lut_1d, vec2(coordinate.g, 0.5)).g;
            lut_color.b = texture(as_color_grade_lut_1d, vec2(coordinate.b, 0.5)).b;
        }
        else lut_color = texture(as_color_grade_lut, coordinate).rgb;
        graded = mix(graded, lut_color, as_color_grade_lut_strength);
    }

    float zoom = max(as_color_grade_snapshot_tile.x, 1.0);
    vec2 full_uv = (vary_fragcoord + as_color_grade_snapshot_tile.yz) / zoom;
    vec2 pixel = full_uv * screen_res * zoom;
    float grain_size = mix(1.0, 8.0, as_color_grade_grain.y);
    vec2 grain_pixel = floor(pixel / grain_size);
    float fine = grainHash(grain_pixel, as_color_grade_grain_seed);
    float coarse = grainHash(floor(grain_pixel * 0.35), as_color_grade_grain_seed + 11.0);
    float mono = mix(fine, coarse, as_color_grade_grain.z) - 0.5;
    vec3 colored = vec3(grainHash(grain_pixel, as_color_grade_grain_seed + 17.0),
                        grainHash(grain_pixel, as_color_grade_grain_seed + 43.0),
                        grainHash(grain_pixel, as_color_grade_grain_seed + 79.0)) - 0.5;
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
    // Invert after grading, grain, and dither; UI is composited after this pass.
    if (as_color_grade_negative != 0)
        graded = vec3(1.0) - clamp(graded, 0.0, 1.0);
    frag_color = vec4(clampHDRRange(graded), source.a);
    gl_FragDepth = texture(depthMap, vary_fragcoord).r;
}
