/**
 * AyaneStorm XeGTAO main fragment pass. Author: chanayane@firestorm.
 * Derived from the Intel XeGTAO implementation (see LICENSE in that repository).
 */
layout(location = 0) out float gtao_visibility;
layout(location = 1) out float gtao_packed_edges;

vec2 gtao_main_pass(ivec2 pixel, int slice_count, int steps_per_slice);

void main()
{
    vec2 result = gtao_main_pass(ivec2(gl_FragCoord.xy), GTAO_SLICES, GTAO_STEPS);
    gtao_visibility = result.x;
    gtao_packed_edges = result.y;
}
