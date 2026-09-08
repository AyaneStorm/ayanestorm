/**
 * @file asdiffuseglow.h
 * @author chanayane@firestorm
 * @brief Optional bright-surface bloom controls for the existing glow pass.
 */

#ifndef AS_DIFFUSE_GLOW_H
#define AS_DIFFUSE_GLOW_H

class LLGLSLShader;

namespace ASDiffuseGlow
{
    void registerUICallbacks();
    void appendShader(LLGLSLShader& shader);
    void bindExtractionUniforms(LLGLSLShader& shader);
}

#endif
