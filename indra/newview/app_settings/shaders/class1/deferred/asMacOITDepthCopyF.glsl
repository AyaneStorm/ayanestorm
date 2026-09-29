// AyaneStorm OIT shader. Author: chanayane@firestorm.
/**
 * @file asMacOITDepthCopyF.glsl
 * @brief Copies opaque scene depth into Mac OIT's private depth-stencil
 *        target. The scene depth is DEPTH_COMPONENT24 without stencil, and
 *        glBlitFramebuffer requires matching depth formats, so the copy is a
 *        fullscreen depth write. The stencil half is cleared separately.
 */

uniform sampler2D macoitOpaqueDepth;

void main()
{
    gl_FragDepth = texelFetch(macoitOpaqueDepth, ivec2(gl_FragCoord.xy), 0).r;
}
