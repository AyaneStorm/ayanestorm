/**
 * @file asDoFLiveCompositeF.glsl
 * @author chanayane@firestorm
 * @brief Live DoF: full-resolution composite, the foreground veil over F
 *        over the background.
 *
 * - F (in focus) stays at full resolution. What this pixel shows of the
 *   focus bin is exact; what a nearer bin hides of it (1 - V_F of the pixel)
 *   is F completed at gather resolution (asDoFLiveCompleteF.glsl,
 *   push-pull), the same read as the gathers'. The in-focus face under a defocused strand
 *   therefore comes from the face around it, never from the strand (mode
 *   1's sharp halos).
 * - The background is B1 over B2 (holes already filled), the foreground
 *   veil N2 over N1, both from the gathers (asDoFLiveGatherF.glsl), both
 *   premultiplied and upsampled bilinearly (they are blurred); the
 *   background is normalized after the read.
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
uniform sampler2D specularRect;  // F (S.rgb, W), completed
uniform sampler2D lightMap;      // background: B1 over B2 (premultiplied)
uniform sampler2D bloomMap;      // foreground veil: N2 over N1 (premultiplied)
uniform sampler2D normalMap;     // axial CA: the veil's per-channel coverage
uniform int debug_mode;
uniform int bins_source;
// Axial CA (asDoFLiveGatherF.glsl): the veil is laid per channel.
uniform float ca_shift;
// Optical vignetting: the light the cat's-eye barrel clips, kept when
// darkening is on (barrel shift at the frame corner, aperture radii; 0
// off). The gathers renormalize every source to its open aperture; the
// whole image darkens here.
uniform float vignette_shift;

in vec2 vary_fragcoord;

void liveDecompose(ivec2 p, out vec4 bins[5], out vec4 energy);
vec4 liveBinVisibility(vec4 bins[5]);
vec2 liveFieldPosition(vec2 uv);

// Open fraction of a unit-circle aperture clipped by a unit circle at
// distance d: lens (vesica) area over pi, with a 5 % floor.
float vignette(vec2 uv)
{
    if (vignette_shift <= 0.0)
    {
        return 1.0;
    }
    float d = vignette_shift * length(liveFieldPosition(uv));
    if (d >= 2.0)
    {
        return 0.05;
    }
    float h = 0.5 * d;
    return max((2.0 * acos(h) - 2.0 * h * sqrt(1.0 - h * h)) / 3.14159265358979323846, 0.05);
}

// F alone at gather resolution, completed where nearer bins hide it.
vec4 focusFill(vec2 uv)
{
    vec4 value = textureLod(specularRect, uv, 0.0);
    return value.a > 0.000001 ? value : vec4(0.0);
}

vec4 over(vec4 front, vec4 back)
{
    return front + back * (1.0 - clamp(front.a, 0.0, 1.0));
}

void main()
{
    vec2 uv = vary_fragcoord;
    vec4 source = texture(projectionMap, uv);
    vec4 bins[5];
    vec4 energy;
    liveDecompose(ivec2(gl_FragCoord.xy), bins, energy);
    float focus_share = bins[2].a;
    float focus_visibility = liveBinVisibility(bins).y;

    // F: what the pixel shows of the focus bin (exact), plus the fill of
    // what nearer bins hide of it.
    vec4 focus = bins[2];
    if (focus_visibility < 0.999)
    {
        focus += (1.0 - focus_visibility) * focusFill(uv);
    }

    vec4 background = texture(lightMap, uv);
    vec3 back = background.a > 0.0001 ? background.rgb / background.a :
                (focus.a > 0.0001 ? focus.rgb / focus.a : source.rgb);
    vec4 veil = texture(bloomMap, uv);

    vec4 under = over(focus, vec4(back, 1.0));
    vec4 layered = over(veil, under);
    if (ca_shift > 0.0)
    {
        // One alpha lost the fringes of dark edges over bright areas.
        vec3 veil_alpha = clamp(texture(normalMap, uv).rgb, 0.0, 1.0);
        layered.rgb = veil.rgb + under.rgb * (1.0 - veil_alpha);
    }
    // The pixel's own focus share, where no veil covers it, keeps the
    // source's exact compositing: add the source's difference from the bins
    // for that share only. A pixel all in focus and unveiled is then the
    // source itself; the background seen through an in-focus strand stays
    // blurred.
    float exact_share = clamp(focus_share * (1.0 - veil.a), 0.0, 1.0);
    vec3 bins_sum = bins[0].rgb + bins[1].rgb + bins[2].rgb + bins[3].rgb + bins[4].rgb;
    vec3 color = max(layered.rgb + exact_share * (source.rgb - bins_sum), vec3(0.0)) *
                 vignette(uv);

    // Debug views write zero glow: glow would bloom the whole frame.
    if (debug_mode == 1)
    {
        // Bin shares: N2 red, N1 orange, F green, B1 purple, B2 blue.
        color = bins[0].a * vec3(1.0, 0.0, 0.0) + bins[1].a * vec3(1.0, 0.5, 0.0) +
                bins[2].a * vec3(0.0, 1.0, 0.0) + bins[3].a * vec3(0.7, 0.0, 1.0) +
                bins[4].a * vec3(0.0, 0.0, 1.0);
    }
    else if (debug_mode == 2)
    {
        // Sum of the bin weights: white (1) everywhere.
        color = vec3(bins[0].a + bins[1].a + bins[2].a + bins[3].a + bins[4].a);
    }
    else if (debug_mode == 3)
    {
        // Tile kernel radii, drawn by the B1 gather (asDoFLiveGatherF.glsl).
        color = background.a > 0.0001 ? background.rgb / background.a : vec3(0.0);
    }
    else if (debug_mode == 4)
    {
        // The background, B1 over B2.
        color = background.a > 0.0001 ? background.rgb / background.a : vec3(0.0);
    }
    else if (debug_mode == 5)
    {
        color = veil.rgb;
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
    else if (debug_mode == 9)
    {
        // B1 alone (the gather skips B2): magenta where it does not cover.
        color = background.rgb + (1.0 - clamp(background.a, 0.0, 1.0)) * vec3(1.0, 0.0, 1.0);
    }
    else if (debug_mode == 10)
    {
        // B2 alone, normalized; black where it holds nothing.
        color = background.a > 0.0001 ? background.rgb / background.a : vec3(0.0);
    }
    else if (debug_mode == 8)
    {
        // Bins source: green = Mac OIT transparency bins, grey = the
        // composited image (one layer, fallback).
        color = bins_source != 0 ? vec3(0.0, 0.6, 0.0) : vec3(0.5);
    }
    frag_color = vec4(color, debug_mode != 0 ? 0.0 : source.a);
}
