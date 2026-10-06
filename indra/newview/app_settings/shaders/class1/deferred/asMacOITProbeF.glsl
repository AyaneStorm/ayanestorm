// AyaneStorm OIT shader. Author: chanayane@firestorm.
/**
 * @file asMacOITProbeF.glsl
 * @brief One-shot driver self-test for Mac OIT: writes a constant to two
 *        render targets so the CPU can verify MIN blending on R32F is
 *        bit-exact and ADD blending on RGBA32F accumulates.
 */

uniform vec4 macoitProbeValue;

layout(location = 0) out vec4 macoit_probe0;
layout(location = 1) out vec4 macoit_probe1;

void main()
{
    macoit_probe0 = macoitProbeValue;
    macoit_probe1 = macoitProbeValue;
}
