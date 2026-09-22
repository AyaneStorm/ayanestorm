/**
 * AyaneStorm XeGTAO weighted depth mip adaptation. Author: chanayane@firestorm.
 * Derived from the Intel XeGTAO implementation (see LICENSE in that repository).
 */
out float frag_depth;
in vec2 vary_fragcoord;

uniform sampler2D gtao_source_depth;
uniform int gtao_source_mip;
uniform ivec2 gtao_source_size;
uniform float gtao_radius;

float gtao_depth_mip_filter(vec4 depth, float radius);

void main()
{
    ivec2 dst = ivec2(gl_FragCoord.xy);
    ivec2 base = dst * 2;
    ivec2 hi = gtao_source_size - 1;
    float d0 = texelFetch(gtao_source_depth, clamp(base, ivec2(0), hi), gtao_source_mip).r;
    float d1 = texelFetch(gtao_source_depth, clamp(base + ivec2(1, 0), ivec2(0), hi), gtao_source_mip).r;
    float d2 = texelFetch(gtao_source_depth, clamp(base + ivec2(0, 1), ivec2(0), hi), gtao_source_mip).r;
    float d3 = texelFetch(gtao_source_depth, clamp(base + ivec2(1, 1), ivec2(0), hi), gtao_source_mip).r;
    frag_depth = gtao_depth_mip_filter(vec4(d0, d1, d2, d3), gtao_radius);
}
