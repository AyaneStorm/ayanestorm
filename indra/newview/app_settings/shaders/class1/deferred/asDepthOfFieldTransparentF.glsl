/**
 * @file asDepthOfFieldTransparentF.glsl
 * @author chanayane@firestorm
 * @brief Premultiplied transparent-layer bokeh gather for AyaneStorm DoF.
 *
 * gather_pass 0 (gather resolution, four attachments, mips generated after):
 *   source pyramid of this gather's layer and plane, see buildSource().
 * gather_pass 1: the gather. With use_pyramid, taps read the pyramid at the
 *   mip level of the local tap spacing (area taps), as the opaque near
 *   gather does (asDepthOfFieldNearF.glsl, same bands): a thin defocused
 *   strand then contributes its share of every tap's area instead of
 *   flickering between hit and miss (fine pattern on defocused hair).
 *   Pyramid: 0 (sum rgb * hw / r^2, sum a / r^2) for the color per unit
 *   coverage, 1-3 the 11 reach bands of a * R^2 / r^2. Sources under
 *   split_radius (8 pi R / N, at least 1 px) stay point taps, as in the
 *   near gather.
 */
layout(location = 0) out vec4 frag_color;
// Pass 1: first and second moments of the accumulated source blur radius,
// weighted by coverage (asDepthOfFieldPostfilterF.glsl).
layout(location = 1) out vec4 frag_data1;
layout(location = 2) out vec4 frag_data2;
layout(location = 3) out vec4 frag_data3;

uniform sampler2D diffuseRect;
uniform sampler2D noiseMap;
uniform sampler2D lightMap;
uniform sampler2D bloomMap;
uniform sampler2D specularRect;
uniform sampler2D positionMap;
uniform sampler2D emissiveRect;
uniform sampler2D shadowMap0;
// Source pyramid attachments 0-3 (gather_pass 1 with use_pyramid).
uniform sampler2D shadowMap1;
uniform sampler2D shadowMap2;
uniform sampler2D shadowMap3;
uniform sampler2D shadowMap4;
uniform vec2 screen_res;
uniform vec2 target_res;
uniform int gather_pass;
uniform int use_pyramid;
uniform float split_radius;
uniform int sample_count;
uniform float max_radius;
uniform int aperture_blades;
uniform float aperture_roundness;
uniform float aperture_rotation;
uniform float anamorphic_ratio;
uniform float highlight_boost;
uniform int plane;
uniform int layer_mode;
uniform int use_occupancy;
// Lens field (asdepthoffield.cpp, setLensUniforms()); the same block is in
// the far and near gathers and the sprite shaders. Axial CA (ca_shift) is
// not applied to the transparent strata.
uniform vec2 field_scale;   // (uv - 0.5) * field_scale: field position, length 1 at the frame corner
uniform float cat_eye;      // cat's-eye barrel shift at the frame corner, aperture radii; 0 off
uniform float astigmatism;  // axis focus split at the frame corner, normalized CoC; 0 off

in vec2 vary_fragcoord;

#define AS_DOF_MAX_SAMPLES 96
#define AS_DOF_PI 3.14159265358979323846
#define BAND_COUNT 11

vec2 fieldPosition(vec2 uv)
{
    return (uv - 0.5) * field_scale;
}

// Cat's eye, see asDepthOfFieldFarF.glsl.
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

// Astigmatism, see asDepthOfFieldFarF.glsl.
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

float samplePhase()
{
    vec2 pixel = floor(gl_FragCoord.xy);
    return fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453) *
           2.0 * AS_DOF_PI;
}

vec2 apertureSample(int index, int count, float phase, out float area_weight)
{
    float fi = float(index) + 0.5;
    float radial_fraction = fi / float(max(count, 1));
    // Both gathers cover the maximum disc, including almost-focused sources.
    // Uniform radius retains central taps; 2*r compensates their area density.
    float radius = radial_fraction;
    // Polar angle before rotation; a blade vertex lies at angle 0 (ASDoFAperture).
    float phi = fi * 2.399963229728653 + phase;
    float boundary = 1.0;
    if (aperture_blades >= 3)
    {
        float sector = 2.0 * AS_DOF_PI / float(aperture_blades);
        float local_angle = mod(phi, sector) - 0.5 * sector;
        float polygon = cos(0.5 * sector) / max(cos(local_angle), 0.001);
        boundary = mix(polygon, 1.0, aperture_roundness);
    }
    area_weight = boundary * boundary * 2.0 * radius;
    float angle = phi + aperture_rotation;
    return vec2(cos(angle) * anamorphic_ratio, sin(angle)) * radius * boundary;
}

float highlightWeight(vec3 color)
{
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return 1.0 + highlight_boost * smoothstep(0.5, 1.5, luminance);
}

// Share of a source spread by this gather. The near plane leaves sources
// under 0.5-2 px of blur to the resolve, which keeps them at full resolution
// under the near spread (stratumBase() in asDepthOfFieldResolveF.glsl,
// same ramp); spreading them too would soften in-focus surfaces.
float spreadShare(float radius)
{
    return plane < 0 ? smoothstep(0.5, 2.0, radius) : 1.0;
}

vec2 sourcePixelCenter(vec2 uv)
{
    // Keep color, coverage and signed CoC on the same captured surface.
    // Interpolating CoC with an empty pixel's background can reverse its sign.
    vec2 size = vec2(textureSize(noiseMap, 0));
    return (clamp(floor(uv * size), vec2(0.0), size - 1.0) + 0.5) / size;
}

vec4 premultipliedSurface(vec2 uv)
{
    uv = sourcePixelCenter(uv);
    // Both scene and opaque snapshot are now linear HDR. Recover the visible
    // premultiplied transparent contribution from the selected compositor's
    // result. The rigged/world strata below retain their own focal depths.
    // Keep signed RGB: custom darkening blends can legitimately contribute a
    // negative residual relative to the opaque snapshot.
    float coverage = clamp(texture(lightMap, uv).a, 0.0, 1.0);
    if (coverage <= 0.0001)
    {
        return vec4(0.0);
    }
    // Rigged replay already contains the premultiplied color. It does not
    // need scene/opaque residual reconstruction or world depth ordering.
    if (layer_mode == 2)
    {
        return vec4(texture(specularRect, uv).rgb, texture(noiseMap, uv).b);
    }
    vec3 scene = texture(diffuseRect, uv).rgb;
    vec3 opaque = texture(bloomMap, uv).rgb;
    vec4 combined = vec4(scene - opaque * (1.0 - coverage), coverage);
    if (layer_mode == 0)
    {
        return combined;
    }
    vec4 rigged = texture(specularRect, uv);
    vec4 layers = texture(noiseMap, uv);
    if (texture(positionMap, uv).r > texture(emissiveRect, uv).r)
    {
        // The non-rigged layer is in front: subtract the transmitted rigged
        // contribution without dividing by the rigged transmittance.
        return vec4(combined.rgb - rigged.rgb * (1.0 - layers.a),
                    layers.a);
    }
    float transmission = 1.0 - layers.b;
    // Back-layer color is unavailable under fully opaque front fragments;
    // near foreground coverage conceals these pixels until neighboring
    // background samples fill the exposed silhouette.
    if (layers.a <= 0.0001 || transmission <= 0.05)
    {
        return vec4(0.0);
    }
    return vec4((combined.rgb - rigged.rgb) / transmission, layers.a);
}

float surfaceCoC(vec2 uv)
{
    vec4 data = texture(noiseMap, sourcePixelCenter(uv));
    return layer_mode == 2 ? data.r :
           layer_mode == 1 ? data.g : data.b;
}

bool nearbyLayer(vec2 uv)
{
    // One mip cell covers at least the full aperture radius. Its 3x3
    // neighborhood conservatively includes every possible source sample.
    ivec2 base_size = textureSize(shadowMap0, 0);
    int max_level = int(floor(log2(float(max(base_size.x, base_size.y)))));
    float reach = max_radius * max(anamorphic_ratio, 1.0);
    int level = clamp(int(ceil(log2(max(reach / 16.0, 1.0)))),
                      0, max_level);
    ivec2 size = textureSize(shadowMap0, level);
    ivec2 cell = clamp(ivec2(uv * vec2(size)), ivec2(0), size - ivec2(1));
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 neighbor = clamp(cell + ivec2(x, y), ivec2(0),
                                   size - ivec2(1));
            vec2 occupancy = texelFetch(shadowMap0, neighbor, level).rg;
            if ((layer_mode == 2 ? occupancy.r : occupancy.g) > 0.0)
            {
                return true;
            }
        }
    }
    return false;
}

// Reach bands, identical to asDepthOfFieldNearF.glsl.
float bandEdge(int k)
{
    return k == 0 ? 0.0 :
        1.0 + (max_radius - 1.0) * pow(float(k - 1) / 10.0, 1.5);
}

int bandIndex(float distance)
{
    if (distance < 1.0)
    {
        return 0;
    }
    float t = pow(clamp((distance - 1.0) / max(max_radius - 1.0, 0.0001), 0.0, 1.0),
                  2.0 / 3.0);
    return min(1 + int(floor(10.0 * t)), BAND_COUNT - 1);
}

// Pyramid level 0 for this plane: the gather-resolution texel averages a 2x2
// set of full-resolution sources, each read exactly as a point tap reads it.
void buildSource()
{
    vec2 scale = screen_res / target_res;
    ivec2 last = ivec2(screen_res) - 1;
    vec4 color_sum = vec4(0.0);
    float bands[BAND_COUNT];
    for (int k = 0; k < BAND_COUNT; ++k)
    {
        bands[k] = 0.0;
    }
    for (int i = 0; i < 4; ++i)
    {
        vec2 offset = vec2(float(i & 1), float(i >> 1)) * 0.5 + 0.25;
        ivec2 p = clamp(ivec2((floor(gl_FragCoord.xy) + offset) * scale),
                        ivec2(0), last);
        vec2 source_uv = (vec2(p) + 0.5) / screen_res;
        float coc = surfaceCoC(source_uv);
        float r = (plane > 0 ? coc : -coc) * max_radius;
        if (r < split_radius)
        {
            continue;
        }
        vec4 layer = premultipliedSurface(source_uv);
        layer.rgb = clamp(layer.rgb, vec3(-60000.0), vec3(60000.0));
        layer.rgb *= highlightWeight(layer.rgb);
        layer *= spreadShare(r);
        // Color per unit coverage, weighted like the point gather (1 / r^2);
        // bands scaled by R^2 for half-float range.
        color_sum += layer / (r * r);
        float w = layer.a * max_radius * max_radius / (r * r);
        for (int k = 0; k < BAND_COUNT; ++k)
        {
            float a = bandEdge(k);
            float b = bandEdge(k + 1);
            float rc = clamp(r, a, b);
            bands[k] += w * (rc * rc - a * a) / (b * b - a * a);
        }
    }
    frag_color = color_sum * 0.25;
    frag_data1 = vec4(bands[0], bands[1], bands[2], bands[3]) * 0.25;
    frag_data2 = vec4(bands[4], bands[5], bands[6], bands[7]) * 0.25;
    frag_data3 = vec4(bands[8], bands[9], bands[10], 0.0) * 0.25;
}

float bandValue(int k, vec2 uv, float lod)
{
    vec4 bands;
    if (k < 4)
    {
        bands = textureLod(shadowMap2, uv, lod);
        return bands[k];
    }
    if (k < 8)
    {
        bands = textureLod(shadowMap3, uv, lod);
        return bands[k - 4];
    }
    bands = textureLod(shadowMap4, uv, lod);
    return bands[k - 8];
}

void main()
{
    frag_data1 = vec4(0.0);
    frag_data2 = vec4(0.0);
    frag_data3 = vec4(0.0);
    if (gather_pass == 0)
    {
        buildSource();
        return;
    }

    vec2 uv = vary_fragcoord;
    float phase_angle = samplePhase();
    vec4 accumulated = vec4(0.0);
    vec3 moment_sum = vec3(0.0);   // sum w r, sum w r^2, sum w
    bool pyramid = use_pyramid != 0;
    float pixel_scale = max(screen_res.x / target_res.x, 1.0);
    float inverse_scale = 1.0 / max(max_radius * max_radius, 0.0001);

    if (max_radius <= 0.0)
    {
        frag_color = vec4(0.0);
        return;
    }

    if (use_occupancy != 0 && layer_mode != 0 && !nearbyLayer(uv))
    {
        frag_color = vec4(0.0);
        return;
    }

    // Gather source footprints for either plane. A destination's background
    // depth must not shrink or suppress a transparent strand's outgoing blur.
    // Normalize by source area so increasing its radius spreads its coverage.
    float kernel_area = 0.0;
    vec2 field = fieldPosition(uv);
    vec2 barrel = barrelCenter(field);
    // One aperture deformation for all taps: this pixel's own blur on this
    // plane, else half the maximum (see asDepthOfFieldNearF.glsl).
    float center_plane_coc = float(plane) * surfaceCoC(uv);
    float shape_radius = center_plane_coc > 0.0 ? center_plane_coc * max_radius :
                                                  0.5 * max_radius;
    vec2 axis_scale = astigmaticScale(field, float(plane) * shape_radius, max_radius);
    for (int i = 0; i < AS_DOF_MAX_SAMPLES; ++i)
    {
        if (i >= sample_count) break;
        float aperture_weight;
        vec2 disk = apertureSample(i, sample_count, phase_angle,
                                   aperture_weight);
        // Clipped by the barrel: outside the kernel (no kernel area).
        if (!barrelOpen(disk, barrel))
        {
            continue;
        }
        kernel_area += aperture_weight;
        // Background points image as the upright aperture, foreground points
        // as the inverted one (see ASDoFCamera): the source reaching this
        // pixel lies at -disk for the far plane and +disk for the near plane.
        float source_side = plane > 0 ? -1.0 : 1.0;
        vec2 sample_uv = clamp(uv + source_side * deform(disk * max_radius, field, axis_scale) / screen_res,
                               0.5 / screen_res,
                               vec2(1.0) - 0.5 / screen_res);
        float distance_pixels =
            (float(i) + 0.5) / float(sample_count) * max_radius;
        if (pyramid)
        {
            // Mip level matching the local tap spacing: uniform-radius taps
            // put 2 pi d R / N px^2 around each tap at distance d.
            float spacing = sqrt(2.0 * AS_DOF_PI * max(distance_pixels, 0.5) *
                                 max_radius / float(sample_count));
            float lod = log2(max(spacing / pixel_scale, 1.0));
            float reach = bandValue(bandIndex(distance_pixels), sample_uv, lod);
            if (reach > 0.0)
            {
                vec4 source = textureLod(shadowMap1, sample_uv, lod);
                vec3 color_per_coverage = source.rgb / max(source.a, 0.000001);
                float tap_coverage = reach * inverse_scale * aperture_weight;
                accumulated += vec4(color_per_coverage, 1.0) * tap_coverage;
                // The pyramid keeps no per-source radius; every source
                // reaching this tap is blurred at least this much.
                float r = max(distance_pixels, split_radius);
                moment_sum += tap_coverage * vec3(r, r * r, 1.0);
            }
        }
        float sample_coc = surfaceCoC(sample_uv);
        float plane_coc = plane > 0 ? sample_coc : -sample_coc;
        float sample_radius = max(plane_coc, 0.0) * max_radius;
        // Point tap: every source without the pyramid, otherwise only
        // sources under split_radius, which the pyramid leaves out.
        if (pyramid && sample_radius >= split_radius)
        {
            continue;
        }
        float support = 1.0 - smoothstep(sample_radius - 1.0,
                                         sample_radius + 1.0,
                                         distance_pixels);
        support *= plane_coc > 0.0 ? spreadShare(sample_radius) : 0.0;
        // Rejected taps contribute exactly zero. Keep their aperture area in
        // kernel_area, but avoid color reconstruction and highlight work.
        if (support <= 0.0)
        {
            continue;
        }
        float weight = support * aperture_weight /
                       max(sample_radius * sample_radius, 1.0);
        vec4 layer = premultipliedSurface(sample_uv);
        layer.rgb *= highlightWeight(layer.rgb);
        accumulated += layer * weight;
        moment_sum += weight * layer.a *
                      vec3(sample_radius, sample_radius * sample_radius, 1.0);
    }

    float coverage_scale = max_radius * max_radius /
                           max(kernel_area, 0.0001);
    frag_color = accumulated * coverage_scale;
    // Correct coverage overshoot without leaving excess premultiplied RGB.
    frag_color /= max(frag_color.a, 1.0);
    frag_data1 = vec4(moment_sum.xy / max(moment_sum.z, 0.000001), 0.0, 0.0);
}
