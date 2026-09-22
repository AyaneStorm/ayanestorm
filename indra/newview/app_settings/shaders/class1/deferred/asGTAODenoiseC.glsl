/**
 * AyaneStorm XeGTAO compute denoiser. Author: chanayane@firestorm.
 * Derived from the Intel XeGTAO implementation (see LICENSE in that repository).
 */
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0, r8) uniform writeonly image2D gtao_visibility_output;
uniform ivec2 gtao_viewport;

float gtao_denoise_pixel(ivec2 pixel);

void main()
{
    ivec2 base = ivec2(gl_GlobalInvocationID.xy) * ivec2(2, 1);
    if (base.y >= gtao_viewport.y) return;
    if (base.x < gtao_viewport.x)
        imageStore(gtao_visibility_output, base, vec4(gtao_denoise_pixel(base)));
    ivec2 second = base + ivec2(1, 0);
    if (second.x < gtao_viewport.x)
        imageStore(gtao_visibility_output, second, vec4(gtao_denoise_pixel(second)));
}
