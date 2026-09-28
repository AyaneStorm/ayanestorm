/**
 * @file asDepthOfFieldTransparentF.glsl
 * @author chanayane@firestorm
 * @brief Premultiplied transparent-layer bokeh gather for AyaneStorm DoF.
 */
out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform sampler2D noiseMap;
uniform sampler2D lightMap;
uniform sampler2D bloomMap;
uniform sampler2D specularRect;
uniform sampler2D positionMap;
uniform sampler2D emissiveRect;
uniform sampler2D shadowMap0;
uniform vec2 screen_res;
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

in vec2 vary_fragcoord;

#define AS_DOF_MAX_SAMPLES 96
#define AS_DOF_PI 3.14159265358979323846

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

void main()
{
    vec2 uv = vary_fragcoord;
    float phase_angle = samplePhase();
    vec4 accumulated = vec4(0.0);

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
    for (int i = 0; i < AS_DOF_MAX_SAMPLES; ++i)
    {
        if (i >= sample_count) break;
        float aperture_weight;
        vec2 disk = apertureSample(i, sample_count, phase_angle,
                                   aperture_weight);
        kernel_area += aperture_weight;
        // Background points image as the upright aperture, foreground points
        // as the inverted one (see ASDoFCamera): the source reaching this
        // pixel lies at -disk for the far plane and +disk for the near plane.
        float source_side = plane > 0 ? -1.0 : 1.0;
        vec2 sample_uv = clamp(uv + source_side * disk * max_radius / screen_res,
                               0.5 / screen_res,
                               vec2(1.0) - 0.5 / screen_res);
        float sample_coc = surfaceCoC(sample_uv);
        float plane_coc = plane > 0 ? sample_coc : -sample_coc;
        float sample_radius = max(plane_coc, 0.0) * max_radius;
        float distance_pixels =
            (float(i) + 0.5) / float(sample_count) * max_radius;
        float support = 1.0 - smoothstep(sample_radius - 1.0,
                                         sample_radius + 1.0,
                                         distance_pixels);
        support *= plane_coc > 0.0 ? 1.0 : 0.0;
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
    }

    float coverage_scale = max_radius * max_radius /
                           max(kernel_area, 0.0001);
    frag_color = accumulated * coverage_scale;
    // Correct coverage overshoot without leaving excess premultiplied RGB.
    frag_color /= max(frag_color.a, 1.0);
}
