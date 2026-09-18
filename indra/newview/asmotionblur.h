/**
 * @file asmotionblur.h
 * @author chanayane@firestorm
 * @brief Screen-space camera motion blur, reprojection-based (no per-object velocity buffer).
 */
#ifndef AS_MOTION_BLUR_H
#define AS_MOTION_BLUR_H

#include <vector>
#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASMotionBlur
{
    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();
    // Advances the module's previous/current main-view matrix snapshots. Must be called
    // once per frame at the end of the 3D scene render, before render_ui() substitutes its
    // HUD modelview. The module retains its own previous snapshot because the pipeline's
    // get_last_*() globals have already advanced to the current frame at this call site.
    void captureFrameMatrices();
    // Returns true only when a distinct destination received the processed image.
    // depth is the shared deferred depth buffer (e.g. mRT->deferredScreen), read for
    // depth reconstruction and per-sample depth-aware rejection.
    bool render(LLRenderTarget& source, LLRenderTarget& destination, LLRenderTarget& depth,
                LLVertexBuffer& screen_triangle);
}
#endif
