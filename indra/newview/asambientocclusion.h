/**
 * @file asambientocclusion.h
 * @author chanayane@firestorm
 * @brief Optional XeGTAO-derived ambient-occlusion backend.
 */
#ifndef AS_AMBIENT_OCCLUSION_H
#define AS_AMBIENT_OCCLUSION_H

#include <vector>

#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASAmbientOcclusion
{
    enum Technique
    {
        LEGACY_SSAO = 0,
        GTAO = 1
    };

    enum Backend
    {
        AUTO = 0,
        FRAGMENT = 1,
        COMPUTE = 2
    };

    const char* shaderCacheRevision();
    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool loadShaders(S32 shader_level);
    void unloadShaders();

    void allocateResources(U32 width, U32 height);
    void releaseResources();

    // Produces GTAO for the current deferred G-buffer. False selects legacy SSAO.
    bool render(LLRenderTarget& deferred_screen, LLVertexBuffer& screen_triangle);

    // Called while the deferred composite shader is bound.
    bool bindResult(LLGLSLShader& shader);
    void unbindResult(LLGLSLShader& shader);

    Technique requestedTechnique();
    Technique effectiveTechnique();
    Backend requestedBackend();
    Backend effectiveBackend();
    bool bentNormalsEffective();
    bool debugWhiteEnabled();
}

#endif
