/**
 * @file asaurora.h
 * @author chanayane@firestorm
 * @brief Viewer-local configurable procedural aurora rendering.
 */

#ifndef AS_AURORA_H
#define AS_AURORA_H

#include <vector>

#include "llglslshader.h"

namespace ASAurora
{
    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();
    bool configureShader();
    LLGLSLShader& getShader();
}

#endif
