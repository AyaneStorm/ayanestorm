/**
 * AyaneStorm XeGTAO main compute pass. Author: chanayane@firestorm.
 * Derived from the Intel XeGTAO implementation (see LICENSE in that repository).
 */
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0, r8) uniform writeonly image2D gtao_visibility_output;
layout(binding = 1, r8) uniform writeonly image2D gtao_edges_output;
uniform ivec2 gtao_viewport;

vec2 gtao_main_pass(ivec2 pixel, int slice_count, int steps_per_slice);

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(pixel, gtao_viewport))) return;
    vec2 result = gtao_main_pass(pixel, GTAO_SLICES, GTAO_STEPS);
    imageStore(gtao_visibility_output, pixel, vec4(result.x));
    imageStore(gtao_edges_output, pixel, vec4(result.y));
}
