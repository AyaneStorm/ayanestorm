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
 * layer 0 (N2), 1 (N1): one kernel radius per tile (asDoFLiveTileF.glsl).
 * layer 2 (B): the pixel's own mean radius M / W, completed: nearer, less
 *   blurred surfaces hide the spread of farther ones, and holes take the
 *   radius of the background around them.
 * Output: premultiplied colour and coverage.
 */

layout(location = 0) out vec4 frag_color;

uniform sampler2D diffuseRect;   // the bin's (S.rgb, W), mipmapped
uniform sampler2D specularRect;  // radius (E_N2, E_N1, M_B, E_B), mipmapped
uniform sampler2D emissiveRect;  // visibility (V_N1, V_F, V_B, 0), mipmapped
uniform sampler2D bloomMap;      // F (S.rgb, W), mipmapped: skip in-focus pixels
uniform sampler2D noiseMap;      // tile kernel radii (N2, N1)
uniform vec2 target_res;
uniform int layer;
uniform int max_rings;
uniform int max_level;

const int TILE = 8;
const int MAX_RINGS = 7;
const float LIVE_PI_G = 3.14159265358979323846;

uniform float unit_area;
uniform float anamorphic_ratio;
vec2 liveTapOffset(float angle, float distance, out float boundary);
float liveReach(float r, float d, float s);
bool liveCompleted(sampler2D layer, sampler2D energy_map, sampler2D vis_map,
                   int vis_channel, vec2 uv, float lod, float max_lod,
                   out vec4 value, out vec4 energy);

// The layer alone at uv and lod, completed (liveCompleted()): visibility in
// front of N2 is 1, of N1 V.x, of B V.z. energies: (E_N2, E_N1, M_B, E_B).
bool readLayer(vec2 uv, float lod, out vec4 value, out vec4 energies)
{
    int vis_channel = layer == 0 ? -1 : (layer == 1 ? 0 : 2);
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
    energy = layer == 0 ? energies.x : (layer == 1 ? energies.y : energies.w);
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

// Far kernel radius: the completed mean radius M / W here; 0 where no
// background exists at all.
float farKernel(vec2 uv)
{
    vec4 value;
    vec4 energies;
    if (!readLayer(uv, 0.0, value, energies))
    {
        return 0.0;
    }
    return energies.z / value.a;
}

void main()
{
    vec2 center = gl_FragCoord.xy;
    vec2 uv = center / target_res;
    float kernel;
    if (layer == 2)
    {
        // Fully in focus here: F hides whatever B would hold.
        if (textureLod(bloomMap, uv, 0.0).a >= 0.999)
        {
            frag_color = vec4(0.0);
            return;
        }
        // A background blurred under half a gather pixel is still read
        // (centre taps, nearly sharp): returning nothing left the holes it
        // hides black.
        kernel = farKernel(uv);
        if (kernel <= 0.0)
        {
            frag_color = vec4(0.0);
            return;
        }
        kernel = max(kernel, 0.5);
    }
    else
    {
        vec2 radii = texelFetch(noiseMap, ivec2(center) / TILE, 0).xy;
        kernel = layer == 0 ? radii.x : radii.y;
    }
    if (kernel < 0.5)
    {
        frag_color = vec4(0.0);
        return;
    }

    int rings = clamp(int(ceil(kernel - 0.5)), 1, min(max_rings, MAX_RINGS));
    float s = kernel / (float(rings) + 0.5);
    // Foreground sources image as the inverted aperture (the source reaching
    // this pixel lies at +offset), background ones upright (-offset), as in
    // the aperture-sampled renderer (ASDoFCamera).
    float direction = layer == 2 ? -1.0 : 1.0;
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
            // Ring tap area pi s^2 / 3 in aperture space, scaled to the
            // image by the polygon boundary squared and the anamorphic
            // squeeze, as unit_area is for the whole aperture.
            float area = anamorphic_ratio * (LIVE_PI_G * s * s / 3.0) * boundary * boundary;
            addTap(center, tap * direction, d, s, s * boundary * squeeze, area,
                   color_sum, weight_sum);
        }
    }

    if (weight_sum <= 0.0)
    {
        frag_color = vec4(0.0);
        return;
    }
    // Premultiplied for every layer: skipped pixels are (0, 0, 0, 0), and the
    // composite's bilinear upsampling must not mix that black into the
    // colour of their neighbours (dark fringes along in-focus strands).
    float coverage = clamp(weight_sum, 0.0, 1.0);
    frag_color = vec4(color_sum / weight_sum * coverage, coverage);
}
