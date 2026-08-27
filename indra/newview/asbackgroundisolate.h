/**
 * @file asbackgroundisolate.h
 * @author chanayane@firestorm
 * @brief Viewer-local solid-color background isolate pass for the self-lighting
 *        floater's photography mode. Draws after the full post-process chain
 *        (tonemap, bloom, DoF, FXAA/SMAA, vignette) so the isolate color is
 *        immune to all of it, unlike a pass drawn earlier in the pipeline.
 */

#ifndef AS_BACKGROUNDISOLATE_H
#define AS_BACKGROUNDISOLATE_H

#include <vector>

#include "llglslshader.h"
#include "v4color.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASBackgroundIsolate
{
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();

    // No-op unless isolate mode is currently active (set via
    // ASFloaterMyLight::setActive()). depth_target supplies the scene depth
    // used to tell background pixels (nothing opaque drawn there) apart from
    // the avatar, which must stay untouched.
    void render(LLRenderTarget& depth_target, LLVertexBuffer& screen_triangle);

    // Called by ASFloaterMyLight when isolate mode is turned on/off/recolored.
    void setActive(bool active, const LLColor4& color);

    // Queried by other screen-space post effects (lens flare, vignette) that
    // must not draw over the solid isolate backdrop -- a lens flare "ghost"
    // or a vignette darkening the corners would both undermine the "only the
    // avatar, on a flat color" result isolate mode promises.
    bool isActive();
}

#endif
