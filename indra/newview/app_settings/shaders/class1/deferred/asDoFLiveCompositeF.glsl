/**
 * @file asDoFLiveCompositeF.glsl
 * @author chanayane@firestorm
 * @brief Live DoF: full-resolution composite, N2 over N1 over F over B.
 *
 * - F (in focus) stays at full resolution. What this pixel shows of the
 *   focus bin is exact; what a nearer bin hides of it (1 - V_F of the pixel)
 *   is F completed at gather resolution (liveCompleted(), push-pull), the
 *   same read as the gathers'. The in-focus face under a defocused strand
 *   therefore comes from the face around it, never from the strand (mode
 *   1's sharp halos).
 * - B is the far gather (holes already filled); N1 and N2 are the near
 *   veils. All three are premultiplied and upsampled bilinearly (they are
 *   blurred); B is normalized after the read.
 * - In-focus content matches the source exactly whatever alpha mode
 *   composited it: the source's difference from the bins (zero with Mac OIT
 *   bins, which add up to its composite) is added for the pixel's unveiled
 *   focus share only. Blending the whole pixel toward the source instead
 *   brought back the sharp background seen through in-focus strands.
 * The pixel's own bins come from liveDecompose() (asDoFLiveCommonF.glsl):
 * with Mac OIT bins, a strand in front of the focus is its own N bin and the
 * face behind it its own F share, each at its own depth.
 */

layout(location = 0) out vec4 frag_color;

uniform sampler2D projectionMap; // composited linear HDR image (glow in alpha)
uniform sampler2D specularRect;  // F (S.rgb, W), mipmapped
uniform sampler2D emissiveRect;  // visibility (V_N1, V_F, V_B, 0), mipmapped
uniform sampler2D lightMap;      // far gather (premultiplied)
uniform sampler2D bloomMap;      // N1 veil (premultiplied)
uniform sampler2D exposureMap;   // N2 veil (premultiplied)
uniform sampler2D noiseMap;      // tile kernel radii (debug view)
uniform int max_level;
uniform int debug_mode;
uniform int bins_source;
uniform float near_radius;
uniform float far_radius;

in vec2 vary_fragcoord;

void liveDecompose(ivec2 p, out vec4 bins[4], out vec4 energy);
vec3 liveBinVisibility(vec4 bins[4]);
bool liveCompleted(sampler2D layer, sampler2D energy_map, sampler2D vis_map,
                   int vis_channel, vec2 uv, float lod, float max_lod,
                   out vec4 value, out vec4 energy);

// F alone at gather resolution, completed where nearer bins hide it.
vec4 focusFill(vec2 uv)
{
    vec4 value;
    vec4 unused;
    return liveCompleted(specularRect, specularRect, emissiveRect, 1, uv, 0.0,
                         float(max_level), value, unused) ? value : vec4(0.0);
}

vec4 over(vec4 front, vec4 back)
{
    return front + back * (1.0 - clamp(front.a, 0.0, 1.0));
}

void main()
{
    vec2 uv = vary_fragcoord;
    vec4 source = texture(projectionMap, uv);
    vec4 bins[4];
    vec4 energy;
    liveDecompose(ivec2(gl_FragCoord.xy), bins, energy);
    // Shares of the pixel's visible content per bin: N2, N1, F, B.
    vec4 w = vec4(bins[0].a, bins[1].a, bins[2].a, bins[3].a);
    float focus_visibility = liveBinVisibility(bins).y;

    // F: what the pixel shows of the focus bin (exact), plus the fill of
    // what nearer bins hide of it.
    vec4 focus = bins[2];
    if (focus_visibility < 0.999)
    {
        focus += (1.0 - focus_visibility) * focusFill(uv);
    }

    vec4 far_layer = texture(lightMap, uv);
    vec3 back = far_layer.a > 0.0001 ? far_layer.rgb / far_layer.a :
                (focus.a > 0.0001 ? focus.rgb / focus.a : source.rgb);
    vec4 near1 = texture(bloomMap, uv);
    vec4 near2 = texture(exposureMap, uv);

    vec4 layered = over(near2, over(near1, over(focus, vec4(back, 1.0))));
    // The pixel's own focus share, where no veil covers it, keeps the
    // source's exact compositing: add the source's difference from the bins
    // for that share only. A pixel all in focus and unveiled is then the
    // source itself; the background seen through an in-focus strand stays
    // blurred.
    float exact_share = clamp(w.z * (1.0 - near1.a) * (1.0 - near2.a), 0.0, 1.0);
    vec3 bins_sum = bins[0].rgb + bins[1].rgb + bins[2].rgb + bins[3].rgb;
    vec3 color = max(layered.rgb + exact_share * (source.rgb - bins_sum), vec3(0.0));

    // Debug views write zero glow: glow would bloom the whole frame.
    if (debug_mode == 1)
    {
        // Bin shares: N2 red, N1 orange, F green, B blue.
        color = w.x * vec3(1.0, 0.0, 0.0) + w.y * vec3(1.0, 0.5, 0.0) +
                w.z * vec3(0.0, 1.0, 0.0) + w.w * vec3(0.0, 0.0, 1.0);
    }
    else if (debug_mode == 2)
    {
        // Sum of the bin weights: white (1) everywhere.
        color = vec3(dot(w, vec4(1.0)));
    }
    else if (debug_mode == 3)
    {
        // Tile kernel radii relative to the foreground maximum: N2 red,
        // N1 green.
        vec2 radii = texture(noiseMap, uv).xy;
        color = vec3(radii / max(0.5 * near_radius, 0.5), 0.0);
    }
    else if (debug_mode == 4)
    {
        color = far_layer.a > 0.0001 ? far_layer.rgb / far_layer.a : vec3(0.0);
    }
    else if (debug_mode == 5)
    {
        color = over(near2, near1).rgb;
    }
    else if (debug_mode == 6)
    {
        color = focus.rgb;
    }
    else if (debug_mode == 7)
    {
        // White: the layered result; black: the source's exact compositing.
        color = vec3(1.0 - exact_share);
    }
    else if (debug_mode == 8)
    {
        // Bins source: green = Mac OIT transparency bins, grey = the
        // composited image (one layer, fallback).
        color = bins_source != 0 ? vec3(0.0, 0.6, 0.0) : vec3(0.5);
    }
    frag_color = vec4(color, debug_mode != 0 ? 0.0 : source.a);
}
