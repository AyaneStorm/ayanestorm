/**
 * AyaneStorm XeGTAO edge-aware denoiser adaptation. Author: chanayane@firestorm.
 * Derived from the Intel XeGTAO implementation (see LICENSE in that repository).
 */
#ifdef GTAO_BENT_NORMALS
out vec4 frag_visibility;
#else
out float frag_visibility;
#endif

vec4 gtao_denoise_pixel(ivec2 pixel);

void main()
{
    vec4 result = gtao_denoise_pixel(ivec2(gl_FragCoord.xy));
#ifdef GTAO_BENT_NORMALS
    frag_visibility = result;
#else
    frag_visibility = result.r;
#endif
}
