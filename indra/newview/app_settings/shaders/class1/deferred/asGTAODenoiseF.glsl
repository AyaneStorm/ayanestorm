/**
 * AyaneStorm XeGTAO edge-aware denoiser adaptation. Author: chanayane@firestorm.
 * Derived from the Intel XeGTAO implementation (see LICENSE in that repository).
 */
out float frag_visibility;

float gtao_denoise_pixel(ivec2 pixel);

void main()
{
    frag_visibility = gtao_denoise_pixel(ivec2(gl_FragCoord.xy));
}
