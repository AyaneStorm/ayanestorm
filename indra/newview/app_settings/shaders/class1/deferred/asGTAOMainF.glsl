/**
 * AyaneStorm XeGTAO main fragment pass. Author: chanayane@firestorm.
 * Derived from the Intel XeGTAO implementation (see LICENSE in that repository).
 */
#ifdef GTAO_BENT_NORMALS
layout(location = 0) out vec4 gtao_visibility;
#else
layout(location = 0) out float gtao_visibility;
#endif
layout(location = 1) out float gtao_packed_edges;

void gtao_main_pass(ivec2 pixel, int slice_count, int steps_per_slice,
                    out vec4 ao_term, out float packed_edges);

void main()
{
    vec4 ao_term;
    gtao_main_pass(ivec2(gl_FragCoord.xy), GTAO_SLICES, GTAO_STEPS,
                   ao_term, gtao_packed_edges);
#ifdef GTAO_BENT_NORMALS
    gtao_visibility = ao_term;
#else
    gtao_visibility = ao_term.r;
#endif
}
