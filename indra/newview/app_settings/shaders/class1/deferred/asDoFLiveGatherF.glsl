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
 * A tap's layer is read "alone": its sums divided by the visibility in
 * front of the bin, V. Its sources spread energy E = sum w / r^2 over their
 * discs; the tap adds area / unit_area * E times the share of its area the
 * sources reach (liveReach()), with r = sqrt(W / E).
 *
 * layer 0 (N2), 1 (N1): one kernel radius per tile (asDoFLiveTileF.glsl).
 *   Output: premultiplied colour and coverage (a veil).
 * layer 2 (B): the pixel's own mean radius M / W, from the finest mip level
 *   holding enough background: nearer, less blurred surfaces hide the
 *   spread of farther ones, and holes (in-focus or foreground pixels) take
 *   the radius of the background around them and fill from it.
 *   Output: premultiplied colour and coverage, normalized by the composite.
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
// Finest-level search for the far kernel: enough background weight.
const float FAR_RADIUS_MIN_WEIGHT = 0.25;

uniform float unit_area;
uniform float anamorphic_ratio;
vec2 liveTapOffset(float angle, float distance, out float boundary);
float liveReach(float r, float d, float s);

// The layer alone at uv and lod: (S.rgb, W) / V and E / V. Returns false
// where nothing of the bin is visible.
bool readTap(vec2 uv, float lod, out vec4 layer_value, out float energy)
{
    vec4 value = textureLod(diffuseRect, uv, lod);
    vec4 radius = textureLod(specularRect, uv, lod);
    float visibility = 1.0;
    if (layer == 1)
    {
        visibility = textureLod(emissiveRect, uv, lod).x;
    }
    else if (layer == 2)
    {
        visibility = textureLod(emissiveRect, uv, lod).z;
    }
    energy = layer == 0 ? radius.x : (layer == 1 ? radius.y : radius.w);
    if (visibility < 0.001 || value.a < 0.00001 || energy <= 0.0)
    {
        return false;
    }
    layer_value = value / visibility;
    energy /= visibility;
    return true;
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

// Far kernel radius M / W at the finest level with enough background.
float farKernel(vec2 uv)
{
    for (int level = 0; level <= 16; ++level)
    {
        if (level > max_level)
        {
            break;
        }
        float weight = textureLod(diffuseRect, uv, float(level)).a;
        if (weight >= FAR_RADIUS_MIN_WEIGHT)
        {
            return textureLod(specularRect, uv, float(level)).z / weight;
        }
    }
    return 0.0;
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
        kernel = farKernel(uv);
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
