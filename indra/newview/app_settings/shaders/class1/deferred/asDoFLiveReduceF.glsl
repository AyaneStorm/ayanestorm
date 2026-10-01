/**
 * @file asDoFLiveReduceF.glsl
 * @author chanayane@firestorm
 * @brief Live DoF: splits the image into CoC bins at gather resolution.
 *
 * One gather pixel averages its 2x2 full-resolution pixels as premultiplied
 * sums. Each pixel is decomposed by liveDecompose() (asDoFLiveCommonF.glsl):
 * the transparency bins of the Mac OIT capture plus the opaque surface, or,
 * as the fallback, the composited image as one surface at the depth buffer's
 * depth.
 *
 * Two passes (reduce_pass), up to four attachments (LLRenderTarget's limit):
 *   0: N2 (S.rgb, W), N1 (S.rgb, W), radius (E_N2, E_N1, E_B1, M_B2)
 *   1: F (S.rgb, W), B1 (S.rgb, W), B2 (S.rgb, W),
 *      visibility (V_N1, V_F, V_B1, V_B2)
 * with S = sum colour w, W = sum w, E = sum w / r^2 (r in gather pixels, the
 * energy the gathers spread), M = sum w r (the far kernel radius).
 */

layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;
layout(location = 2) out vec4 frag_data2;
layout(location = 3) out vec4 frag_data3;

uniform int reduce_pass;

void liveDecompose(ivec2 p, out vec4 bins[5], out vec4 energy);
vec4 liveBinVisibility(vec4 bins[5]);

uniform vec2 screen_res;

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * 2;
    ivec2 last = ivec2(screen_res) - 1;
    vec4 n2 = vec4(0.0);
    vec4 n1 = vec4(0.0);
    vec4 radius = vec4(0.0);
    vec4 focus = vec4(0.0);
    vec4 back1 = vec4(0.0);
    vec4 back2 = vec4(0.0);
    vec4 visibility = vec4(0.0);
    for (int i = 0; i < 4; ++i)
    {
        vec4 bins[5];
        vec4 energy;
        liveDecompose(min(base + ivec2(i & 1, i >> 1), last), bins, energy);
        n2 += bins[0];
        n1 += bins[1];
        focus += bins[2];
        back1 += bins[3];
        back2 += bins[4];
        radius += energy;
        visibility += liveBinVisibility(bins);
    }
    if (reduce_pass == 0)
    {
        frag_data0 = n2 * 0.25;
        frag_data1 = n1 * 0.25;
        frag_data2 = radius * 0.25;
        frag_data3 = vec4(0.0);
    }
    else
    {
        frag_data0 = focus * 0.25;
        frag_data1 = back1 * 0.25;
        frag_data2 = back2 * 0.25;
        frag_data3 = visibility * 0.25;
    }
}
