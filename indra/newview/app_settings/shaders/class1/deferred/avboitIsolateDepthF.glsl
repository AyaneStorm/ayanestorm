/**
 * @file avboitIsolateDepthF.glsl
 * @author chanayane@firestorm
 * @brief Self-lighting floater isolate-background mode: writes a near-plane
 *        depth wherever AVBOIT actually accumulated captured alpha content,
 *        so a later depth-tested isolate backdrop pass correctly treats
 *        those pixels as "something was drawn" instead of painting over
 *        them. AVBOIT's own compute resolve writes color only (a compute
 *        shader cannot imageStore into a depth-format texture), so this
 *        runs as a small separate fragment pass right after that resolve,
 *        reading the same per-pixel coverage data. Only ever bound/drawn
 *        when isolate mode is active; has no effect on ordinary AVBOIT
 *        rendering, which never uses this shader.
 */

uniform sampler2D avboitIsolateWeight;
uniform sampler2D avboitIsolateColorGlow;

in vec2 vary_fragcoord;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float weight = texelFetch(avboitIsolateWeight, pixel, 0).r;
    float glow = texelFetch(avboitIsolateColorGlow, pixel, 0).a;

    if (weight <= 0.0 && glow <= 0.0)
    {
        discard;
    }

    gl_FragDepth = 0.0;
}
