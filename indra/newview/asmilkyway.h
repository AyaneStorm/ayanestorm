/**
 * @file asmilkyway.h
 * @author chanayane@firestorm
 * @brief Viewer-local real-sky Milky Way and deep-sky glow, drawn on the sky
 * dome from a catalogue-derived equirectangular texture.
 */

#ifndef AS_MILKY_WAY_H
#define AS_MILKY_WAY_H

#include <vector>

#include "llglslshader.h"

namespace ASMilkyWay
{
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    // Also releases the GL texture (shader reload / GL restore).
    void unloadShader();
    // Binds the program and texture and sets uniforms; false when the glow
    // must not be drawn (stars off, procedural mode, disabled, HDRI sky,
    // daylight, missing texture).
    bool configureShader(bool hdri_sky);
    LLGLSLShader& getShader();
}

#endif
