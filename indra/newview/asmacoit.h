/**
 * @file asmacoit.h
 * @brief AyaneStorm Mac OIT: order-independent transparency built only from
 *        OpenGL 4.1 blending, for macOS and every other platform.
 * @author chanayane@firestorm
 *
 * K exact front layers per pixel, found by MIN-blend peeling of packed
 * depth/alpha keys, plus a per-pixel moment-reconstructed tail behind them.
 * See doc/ayanestorm-mac-oit-plan.md and asMacOITCaptureF.glsl.
 */

#ifndef AS_MACOIT_H
#define AS_MACOIT_H

#include "llgl.h"

#include <vector>

class LLDrawInfo;
class LLDrawPoolAlpha;
class LLGLSLShader;
class LLPipeline;
class LLRenderTarget;
class LLSD;

class ASMacOIT
{
public:
    using PrepareShader = void (*)(LLGLSLShader*, bool, F32);

    static const char* shaderCacheRevision();
    // User intent (ASRenderOITMode 4) and hardware support.
    static bool requested();
    static void loadShaders(S32 shader_level);
    static void registerShaders(std::vector<LLGLSLShader*>& shader_list);
    static void unloadShaders();

    static void beginFrame();
    static bool captureActive();
    static bool captureCompleted();
    static bool renderPostDeferredCapture(
        LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign,
        LLGLSLShader*& emissive_shader, LLGLSLShader*& pbr_emissive_shader);
    static bool configureCapturedDrawIfActive(LLGLSLShader* shader);
    static bool handleCapturedEmissives(
        LLDrawPoolAlpha& pool, bool depth_only,
        std::vector<LLDrawInfo*>& emissives,
        std::vector<LLDrawInfo*>& pbr_emissives,
        std::vector<LLDrawInfo*>& rigged_emissives,
        std::vector<LLDrawInfo*>& pbr_rigged_emissives);
    static void configureGLTFCapturedDraw(LLGLSLShader& shader);
    static bool finishFrame(LLPipeline& pipeline, LLRenderTarget& screen);

    static LLGLSLShader& gltfProgram(LLGLSLShader& ordinary_program);
    static LLGLSLShader* alphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* pbrAlphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* fullbrightAlphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* materialAlphaShader(U32 mask, LLGLSLShader* ordinary);

    static void allocateResources(U32 width, U32 height);
    static void releaseResources();
    static void appendDiagnostics(LLSD& info);

private:
    static bool supported();
    static bool shadersReady();
    static bool allocate(U32 width, U32 height);
    static bool probeBlending();
};

#endif
