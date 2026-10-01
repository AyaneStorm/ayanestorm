/**
 * @file asDoFLiveGatherF.glsl
 * @author chanayane@firestorm
 * @brief Live DoF: area-tap scatter-as-gather of one bin at gather
 *        resolution.
 *
 * Every tap reads its bin's mip level matching the tap spacing, so it
 * integrates the area it stands for: no random phase, no noise, the same
 * result at any resolution (scripts/testing/dof_live_reference.py).
 *
 * A tap reads its layer "alone", completed where nearer bins hide it
 * (liveCompleted(), push-pull): the background behind a foreground rock is
 * the background around it, never nothing. Its sources spread energy
 * E = sum w / r^2 over their discs; the tap adds area / unit_area * E times
 * the share of its area the sources reach (liveReach()), r = sqrt(W / E).
 *
 * layer 0 (N2), 1 (N1), 3 (B1): veils, one kernel radius per tile
 *   (asDoFLiveTileF.glsl).
 * layer 2 (B2): the pixel's own mean radius M / W, completed: nearer, less
 *   blurred surfaces hide the spread of farther ones, and holes take the
 *   radius of the background around them.
 * Each layer is premultiplied colour and coverage. Layers 1 and 3 are drawn
 * after 0 and 2 and write the pair the composite reads: N1 is written over
 * N2 (one foreground veil), B1 over B2 normalized (the background).
 */

layout(location = 0) out vec4 frag_color;

uniform sampler2D diffuseRect;   // the bin's (S.rgb, W), mipmapped
uniform sampler2D specularRect;  // radius (E_N2, E_N1, E_B1, M_B2), mipmapped
uniform sampler2D emissiveRect;  // visibility (V_N1, V_F, V_B1, V_B2), mipmapped
uniform sampler2D bloomMap;      // F (S.rgb, W), mipmapped: skip in-focus pixels
uniform sampler2D noiseMap;      // tile kernel radii (N2, N1, B1)
uniform sampler2D lightMap;      // layer 1: the N2 veil; layer 3: B2
uniform vec2 target_res;
uniform int layer;
uniform int max_rings;
uniform int max_level;
// Composite debug views 9 and 10 (asDoFLiveCompositeF.glsl): the B1 pass
// writes B1 alone, or B2 alone, instead of B1 over B2.
uniform int debug_mode;

const int LAYER_N2 = 0;
const int LAYER_N1 = 1;
const int LAYER_B2 = 2;
const int LAYER_B1 = 3;
const int TILE = 8;
const int MAX_RINGS = 7;
const float LIVE_PI_G = 3.14159265358979323846;

uniform float unit_area;
uniform float anamorphic_ratio;
vec2 liveTapOffset(float angle, float distance, out float boundary);
float liveTapSectorArea(float angle, float half_angle);
float liveReach(float r, float d, float s);
bool liveCompleted(sampler2D layer, sampler2D energy_map, sampler2D vis_map,
                   int vis_channel, vec2 uv, float lod, float max_lod,
                   out vec4 value, out vec4 energy);
float liveFarEnergy(float weight, float moment);

// The layer alone at uv and lod, completed (liveCompleted()): visibility in
// front of N2 is 1, of N1 V.x, of B1 V.z, of B2 V.w.
bool readLayer(vec2 uv, float lod, out vec4 value, out vec4 energies)
{
    int vis_channel = layer == LAYER_N2 ? -1 :
        (layer == LAYER_N1 ? 0 : (layer == LAYER_B1 ? 2 : 3));
    return liveCompleted(diffuseRect, specularRect, emissiveRect, vis_channel,
                         uv, lod, float(max_level), value, energies);
}

bool readTap(vec2 uv, float lod, out vec4 layer_value, out float energy)
{
    vec4 energies;
    if (!readLayer(uv, lod, layer_value, energies))
    {
        return false;
    }
    energy = layer == LAYER_N2 ? energies.x :
        (layer == LAYER_N1 ? energies.y :
         (layer == LAYER_B1 ? energies.z : liveFarEnergy(layer_value.a, energies.w)));
    return layer_value.a > 0.00001 && energy > 0.0;
}

// One tap: adds its colour and weight. d and s are aperture-space distance
// and spacing (the reach test); spacing is the image-space tap spacing (the
// mip level). Taps past the frame read its edge: content continues there,
// so a foreground touching the frame keeps its coverage.
void addTap(vec2 center, vec2 offset, float d, float s, float spacing,
            float area, inout vec3 color_sum, inout float weight_sum)
{
    vec2 uv = clamp((center + offset) / target_res, vec2(0.0), vec2(1.0));
    float lod = log2(max(spacing, 1.0));
    vec4 value;
    float energy;
    if (!readTap(uv, lod, value, energy))
    {
        return;
    }
    float r = sqrt(value.a / energy);
    float weight = area / unit_area * energy * liveReach(r, d, s);
    color_sum += value.rgb / value.a * weight;
    weight_sum += weight;
}

// Far kernel radius: the completed mean radius M / W here; 0 where no far
// background exists at all.
float farKernel(vec2 uv)
{
    vec4 value;
    vec4 energies;
    if (!readLayer(uv, 0.0, value, energies))
    {
        return 0.0;
    }
    return energies.w / value.a;
}

// This layer at this pixel: premultiplied colour and coverage.
vec4 gatherLayer(vec2 center)
{
    vec2 uv = center / target_res;
    float kernel;
    bool background = layer == LAYER_B2 || layer == LAYER_B1;
    // Fully in focus here: F hides whatever the background would hold.
    if (background && textureLod(bloomMap, uv, 0.0).a >= 0.999)
    {
        return vec4(0.0);
    }
    if (layer == LAYER_B2)
    {
        kernel = farKernel(uv);
    }
    else
    {
        vec3 radii = texelFetch(noiseMap, ivec2(center) / TILE, 0).xyz;
        kernel = layer == LAYER_N2 ? radii.x : (layer == LAYER_N1 ? radii.y : radii.z);
    }
    // Skip only where the bin holds nothing (kernel 0). Every source's
    // radius is at least half a gather pixel (liveDecompose()), so content
    // just off the focus sits exactly at 0.5: a "kernel < 0.5" test skipped
    // it whenever 16-bit rounding of W / E fell a hair below, tile by tile
    // (the far background showed through in blocks). Such content is read
    // nearly sharp, by the centre taps.
    if (kernel <= 0.0)
    {
        return vec4(0.0);
    }
    kernel = max(kernel, 0.5);

    int rings = clamp(int(ceil(kernel - 0.5)), 1, min(max_rings, MAX_RINGS));
    float s = kernel / (float(rings) + 0.5);
    // Foreground sources image as the inverted aperture (the source reaching
    // this pixel lies at +offset), background ones upright (-offset), as in
    // the aperture-sampled renderer (ASDoFCamera).
    float direction = background ? -1.0 : 1.0;
    vec3 color_sum = vec3(0.0);
    float weight_sum = 0.0;
    float squeeze = max(anamorphic_ratio, 1.0);
    addTap(center, vec2(0.0), 0.0, s, s * squeeze, unit_area * 0.25 * s * s,
           color_sum, weight_sum);
    for (int k = 1; k <= MAX_RINGS; ++k)
    {
        if (k > rings)
        {
            break;
        }
        int count = 6 * k;
        float offset = (k & 1) != 0 ? 0.5 : 0.0;
        float d = float(k) * s;
        for (int j = 0; j < 6 * MAX_RINGS; ++j)
        {
            if (j >= count)
            {
                break;
            }
            float angle = 2.0 * LIVE_PI_G * (float(j) + offset) / float(count);
            float boundary;
            vec2 tap = liveTapOffset(angle, d, boundary);
            // Integrate the whole angular sector, not boundary^2 at its
            // midpoint: every ring must partition its annulus exactly.
            // The radial squared-width is 2*d*s; squeeze is in the area.
            float area = 2.0 * d * s * liveTapSectorArea(angle, LIVE_PI_G / float(count));
            addTap(center, tap * direction, d, s, s * boundary * squeeze, area,
                   color_sum, weight_sum);
        }
    }

    if (weight_sum <= 0.0)
    {
        return vec4(0.0);
    }
    // Premultiplied for every layer: skipped pixels are (0, 0, 0, 0), and the
    // composite's bilinear upsampling must not mix that black into the
    // colour of their neighbours (dark fringes along in-focus strands).
    float coverage = clamp(weight_sum, 0.0, 1.0);
    return vec4(color_sum / weight_sum * coverage, coverage);
}

void main()
{
    vec2 center = gl_FragCoord.xy;
    vec4 result = gatherLayer(center);
    if (layer == LAYER_N1)
    {
        // N2 in front of N1.
        vec4 near2 = texelFetch(lightMap, ivec2(center), 0);
        result = near2 + result * (1.0 - near2.a);
    }
    else if (layer == LAYER_B1)
    {
        // B1 in front of B2, which is the farthest content: normalized, it
        // fills whatever B1 leaves uncovered.
        vec4 back2 = texelFetch(lightMap, ivec2(center), 0);
        if (debug_mode == 10)
        {
            result = back2;
        }
        else if (back2.a > 0.0001 && debug_mode != 9)
        {
            result += vec4(back2.rgb / back2.a, 1.0) * (1.0 - result.a);
        }
    }
    frag_color = result;
}
