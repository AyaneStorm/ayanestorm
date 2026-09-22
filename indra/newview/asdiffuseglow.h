/**
 * @file asdiffuseglow.h
 * @author chanayane@firestorm
 * @brief Optional bright-surface bloom controls for the existing glow pass.
 */

#ifndef AS_DIFFUSE_GLOW_H
#define AS_DIFFUSE_GLOW_H

#include <vector>

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderTarget;
class LLVertexBuffer;

namespace ASDiffuseGlow
{
    void registerUICallbacks();
    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool createHDRShaders(S32 shader_level);
    void unloadHDRShaders();
    void releaseHDRResources();
    void appendShader(LLGLSLShader& shader);
    void bindExtractionUniforms(LLGLSLShader& shader);

    // Returns the scene-linear composite when HDR bloom ran, otherwise null.
    LLRenderTarget* renderHDR(LLRenderTarget& scene, LLRenderTarget& exposure_map,
                              LLVertexBuffer& screen_triangle);
}

#endif
