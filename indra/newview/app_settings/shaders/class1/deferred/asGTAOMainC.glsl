/**
 * AyaneStorm XeGTAO main compute pass. Author: chanayane@firestorm.
 * Derived from the Intel XeGTAO implementation (see LICENSE in that repository).
 */
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#ifdef GTAO_BENT_NORMALS
layout(binding = 0, rgba8) uniform writeonly image2D gtao_visibility_output;
#else
layout(binding = 0, r8) uniform writeonly image2D gtao_visibility_output;
#endif
layout(binding = 1, r8) uniform writeonly image2D gtao_edges_output;
uniform ivec2 gtao_viewport;

void gtao_main_pass(ivec2 pixel, int slice_count, int steps_per_slice,
                    out vec4 ao_term, out float packed_edges);

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(pixel, gtao_viewport))) return;
    vec4 ao_term;
    float packed_edges;
    gtao_main_pass(pixel, GTAO_SLICES, GTAO_STEPS, ao_term, packed_edges);
    imageStore(gtao_visibility_output, pixel, ao_term);
    imageStore(gtao_edges_output, pixel, vec4(packed_edges));
}
