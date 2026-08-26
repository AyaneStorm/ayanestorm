/**
 * @file asmoonrendering.h
 * @author chanayane@firestorm
 * @brief Viewer-local moon disc and halo rendering integration.
 */

#ifndef AS_MOON_RENDERING_H
#define AS_MOON_RENDERING_H

class LLFace;
class LLGLSLShader;
class LLSettingsSky;

namespace ASMoonRendering
{
    // Registers reset buttons used by the standalone settings panel.
    void registerUICallbacks();

    void renderHalo(LLFace* halo_face, LLFace* moon_face, LLGLSLShader* shader);
    void configureDiscShader(LLGLSLShader* shader, const LLSettingsSky* sky);
}

#endif
