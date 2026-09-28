/**
 * @file asDepthOfFieldFarF.glsl
 * @author chanayane@firestorm
 * @brief Plane-discriminating far bokeh gather for AyaneStorm DoF.
 */
layout(location = 0) out vec4 frag_color;
// First and second moments of the accumulated source blur radius (full
// resolution px), same weights as the color: the postfilter's edge test
// (asDepthOfFieldPostfilterF.glsl).
layout(location = 1) out vec4 frag_moments;

// Gather input: opaque color with extracted highlight sprites removed.
uniform sampler2D diffuseRect;
// Reuse a viewer-reserved sampler name so LLGLSLShader assigns a texture unit.
uniform sampler2D noiseMap;
// Background completion (rgb, signed opaque CoC) behind foreground pixels.
uniform sampler2D lightMap;
uniform vec2 screen_res;
uniform int sample_count;
uniform float max_radius;
uniform int aperture_blades;
uniform float aperture_roundness;
uniform float aperture_rotation;
uniform float anamorphic_ratio;
uniform float highlight_boost;
// Lens field (asdepthoffield.cpp, setLensUniforms()); the same block is in
// the near and transparent gathers and the sprite shaders.
uniform vec2 field_scale;   // (uv - 0.5) * field_scale: field position, length 1 at the frame corner
uniform float cat_eye;      // cat's-eye barrel shift at the frame corner, aperture radii; 0 off
uniform float astigmatism;  // axis focus split at the frame corner, normalized CoC; 0 off
uniform float ca_shift;     // axial CA blur shift of the extreme wavelengths, normalized CoC; 0 off

in vec2 vary_fragcoord;

#define AS_DOF_MAX_SAMPLES 96
#define AS_DOF_PI 3.14159265358979323846

vec2 fieldPosition(vec2 uv)
{
    return (uv - 0.5) * field_scale;
}

// Cat's eye (mechanical vignetting), as in the aperture-sampled renderer
// (asDoFAccumulateF.glsl): a tap passes only inside the lens barrel, a unit
// circle centred at cat_eye * field in unit aperture coordinates (anamorphic
// scale included). The shift is capped at 1.6 radii (about 10 % of the
// aperture open) so that some taps always remain.
vec2 barrelCenter(vec2 field)
{
    vec2 shift = cat_eye * field;
    float len = length(shift);
    return len > 1.6 ? shift * (1.6 / len) : shift;
}

bool barrelOpen(vec2 disk, vec2 barrel)
{
    vec2 d = disk - barrel;
    return cat_eye <= 0.0 || dot(d, d) <= 1.0;
}

// Astigmatism: off-axis, the radial and circumferential image axes focus at
// different distances. The CoC pass stores |r| + split (split =
// |astigmatism| field^2 in plane pixels), so the aperture image keeps scale
// 1 on its long axis and (R - 2 split) / R on the other (negative between
// the focal lines: flipped). Long axis circumferential behind the focus for
// astigmatism > 0 (swirling background), radial in front; reversed for < 0.
// Returns the (radial, circumferential) scales for a source blurred
// signed_radius px (positive behind focus); plane_radius converts the
// normalized split to pixels.
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

// Scales an image offset along the radial and circumferential axes of field.
vec2 deform(vec2 offset, vec2 field, vec2 axis_scale)
{
    float len = length(field);
    if (len < 0.0001 || axis_scale == vec2(1.0))
    {
        return offset;
    }
    vec2 radial = field / len;
    vec2 circumferential = vec2(-radial.y, radial.x);
    return radial * (dot(offset, radial) * axis_scale.x) +
           circumferential * (dot(offset, circumferential) * axis_scale.y);
}

// Axial chromatic aberration with the aperture-sampled renderer's spectral
// model (ASDoFAperture::spectralWeights): wavelength s in [-1, 1] blurs to
// radius R - sigma delta s (sigma +1 behind focus, -1 in front; red, s = 1,
// focuses farther), channel weights red 1 + s, green 1.5 (1 - s^2), blue
// 1 - s. Four strata of s; a channel covers distance dist by the weighted
// share of the strata whose disc reaches it, with an edge half-width soft.
const vec4 CA_STRATA = vec4(-0.75, -0.25, 0.25, 0.75);
const vec4 CA_RED = vec4(0.0625, 0.1875, 0.3125, 0.4375);
const vec4 CA_GREEN = vec4(0.1590909, 0.3409091, 0.3409091, 0.1590909);
const vec4 CA_BLUE = vec4(0.4375, 0.3125, 0.1875, 0.0625);

vec3 channelCover(float radius, float dist, float sigma, float delta, float soft)
{
    vec4 radii = vec4(radius) - sigma * delta * CA_STRATA;
    vec4 cover = vec4(1.0) - smoothstep(radii - soft, radii + soft, vec4(dist));
    return vec3(dot(cover, CA_RED), dot(cover, CA_GREEN), dot(cover, CA_BLUE));
}

float samplePhase()
{
    vec2 pixel = floor(gl_FragCoord.xy);
    return fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453) * 2.0 * AS_DOF_PI;
}

vec2 apertureSample(int index, int count, float phase, out float area_weight)
{
    float fi = float(index) + 0.5;
    float radius = sqrt(fi / float(max(count, 1)));
    // Polar angle before rotation. A blade vertex lies at angle 0, as in the
    // aperture-sampled renderer (ASDoFAperture), so both modes share one shape.
    float phi = fi * 2.399963229728653 + phase;
    float boundary = 1.0;
    if (aperture_blades >= 3)
    {
        float sector = 2.0 * AS_DOF_PI / float(aperture_blades);
        float local_angle = mod(phi, sector) - 0.5 * sector;
        float polygon = cos(0.5 * sector) / max(cos(local_angle), 0.001);
        boundary = mix(polygon, 1.0, aperture_roundness);
    }
    // Uniform angle and sqrt-radius sampling has an area Jacobian proportional
    // to boundary squared. Weighting by it makes polygonal apertures integrate
    // uniformly without a lookup table or a platform-specific compute pass.
    area_weight = boundary * boundary;
    float angle = phi + aperture_rotation;
    return vec2(cos(angle) * anamorphic_ratio, sin(angle)) * radius * boundary;
}

float highlightWeight(vec3 color)
{
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return 1.0 + highlight_boost * smoothstep(0.5, 1.5, luminance);
}

// Color and signed CoC of a tap. Foreground taps are replaced by the
// background completion: the far blur behind a silhouette integrates what the
// foreground hides instead of renormalizing over the remaining taps.
vec4 farSource(vec2 sample_uv, bool completion_only)
{
    float coc = texture(noiseMap, sample_uv).g;
    if (completion_only || coc < -0.0001)
    {
        return texture(lightMap, sample_uv);
    }
    return vec4(texture(diffuseRect, sample_uv).rgb, coc);
}

vec4 gatherFar(vec2 uv, vec3 center_color, float center_radius, float phase,
               bool completion_only, out vec2 moments)
{
    vec3 sum = center_color * highlightWeight(center_color);
    vec3 weight_sum = vec3(highlightWeight(center_color));
    vec2 moment_sum = weight_sum.g * vec2(center_radius, center_radius * center_radius);
    vec2 field = fieldPosition(uv);
    vec2 barrel = barrelCenter(field);
    vec2 axis_scale = astigmaticScale(field, center_radius, max_radius);
    // Axial CA: each channel averages the sources its own discs reach, over
    // its own disc of this pixel; the taps span the widest (blue) one.
    float delta = ca_shift * max_radius;
    bool chroma = delta > 0.01;
    float reach_radius = chroma ? center_radius + delta : center_radius;

    for (int i = 0; i < AS_DOF_MAX_SAMPLES; ++i)
    {
        if (i >= sample_count)
        {
            break;
        }
        float aperture_weight;
        vec2 disk = apertureSample(i, sample_count, phase, aperture_weight);
        if (!barrelOpen(disk, barrel))
        {
            continue;
        }
        // A background point images as the upright aperture (see
        // ASDoFCamera): the source reaching this pixel lies at -disk.
        vec2 offset_pixels = deform(disk * reach_radius, field, axis_scale);
        vec2 sample_uv = clamp(uv - offset_pixels / screen_res,
                               0.5 / screen_res, vec2(1.0) - 0.5 / screen_res);
        vec4 source = farSource(sample_uv, completion_only);
        float sample_radius = max(source.a, 0.0) * max_radius;
        // Compare radii in aperture space so anamorphic and polygonal kernels
        // retain their intended coverage instead of being clipped as circles.
        float distance_pixels = sqrt((float(i) + 0.5) / float(sample_count)) * reach_radius;
        vec3 coverage;
        if (chroma)
        {
            coverage = channelCover(sample_radius, distance_pixels, 1.0, delta,
                                    max(1.0, 0.25 * delta)) *
                       channelCover(center_radius, distance_pixels, 1.0, delta,
                                    max(0.001, 0.25 * delta));
        }
        else
        {
            coverage = vec3(1.0 - smoothstep(sample_radius - 1.0,
                                             sample_radius + 1.0,
                                             distance_pixels));
        }
        coverage *= source.a > 0.0 ? 1.0 : 0.0;
        vec3 weight = coverage * aperture_weight * highlightWeight(source.rgb);
        sum += source.rgb * weight;
        weight_sum += weight;
        moment_sum += weight.g * vec2(sample_radius, sample_radius * sample_radius);
    }

    moments = moment_sum / max(weight_sum.g, 0.0001);
    return vec4(sum / max(weight_sum, vec3(0.0001)), 1.0);
}

void main()
{
    vec2 uv = vary_fragcoord;
    float center_coc = texture(noiseMap, uv).g;
    float phase = samplePhase();
    vec2 moments = vec2(0.0);

    // Foreground center: blur the depth-biased background completion with its
    // own CoC. The near layers own these pixels; this plate is revealed only by
    // their fractional silhouette coverage.
    if (center_coc < -0.0001)
    {
        vec4 background = texture(lightMap, uv);
        float background_radius = max(background.a, 0.0) * max_radius;
        moments = vec2(background_radius, background_radius * background_radius);
        frag_color = background_radius < 0.5
            ? vec4(background.rgb, 1.0)
            : gatherFar(uv, background.rgb, background_radius, phase, true, moments);
        frag_moments = vec4(moments, 0.0, 0.0);
        return;
    }

    if (center_coc <= 0.0001 || max_radius <= 0.0)
    {
        frag_color = vec4(0.0);
        frag_moments = vec4(0.0);
        return;
    }

    frag_color = gatherFar(uv, texture(diffuseRect, uv).rgb,
                           center_coc * max_radius, phase, false, moments);
    frag_moments = vec4(moments, 0.0, 0.0);
}
