/**
 * @file aschromaticaberration.h
 * @author chanayane@firestorm
 * @brief Optional radial camera chromatic aberration.
 */
#ifndef AS_CHROMATIC_ABERRATION_H
#define AS_CHROMATIC_ABERRATION_H

#include <vector>
#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASChromaticAberration
{
    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();
    // Returns true only when a distinct destination received the processed image.
    bool render(LLRenderTarget& source, LLRenderTarget& destination, LLVertexBuffer& screen_triangle);
    // Draws a screen-space crosshair at the configured center when the beacon is enabled.
    void renderCenterBeacon();
}
#endif
