/**
 * @file fsoitdispatcher.h
 * @brief Neutral selection between AVBOIT, Exact OIT, and vanilla transparency.
 * @author chanayane@firestorm
 */

#ifndef FS_OIT_DISPATCHER_H
#define FS_OIT_DISPATCHER_H

#include <vector>

#include "llglslshader.h"

class LLDrawInfo;
class LLDrawPoolAlpha;
class LLPipeline;
class LLRenderTarget;
class LLVertexBuffer;

class FSOITDispatcher
{
public:
    using PrepareShader = void (*)(LLGLSLShader*, bool, F32);

    static void beginFrame();
    // Order-independent transparency makes alpha submission order irrelevant to
    // the resolved image. Published as a per-frame snapshot refreshed before
    // culling, so hot paths that only need to know whether that ordering still
    // matters can read it without a settings lookup. beginFrame() runs after
    // culling and is therefore too late to publish it.
    static void refreshOrderIndependentAlphaState();
    static bool orderIndependentAlphaActive();
    static bool captureActive();
    static bool captureCompleted();
    static bool renderPostDeferredCapture(
        LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign,
        LLGLSLShader*& emissive_shader, LLGLSLShader*& pbr_emissive_shader);
    static bool configureCapturedDrawIfActive(
        LLGLSLShader* shader, U32 color_source, U32 color_destination,
        U32 alpha_source, U32 alpha_destination);
    static bool handleCapturedEmissives(
        LLDrawPoolAlpha& pool, bool depth_only,
        std::vector<LLDrawInfo*>& emissives,
        std::vector<LLDrawInfo*>& pbr_emissives,
        std::vector<LLDrawInfo*>& rigged_emissives,
        std::vector<LLDrawInfo*>& pbr_rigged_emissives);
    static void configureGLTFCapturedDraw(LLGLSLShader& shader);
    static LLGLSLShader& gltfProgram(LLGLSLShader& ordinary);
    static LLGLSLShader* alphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* pbrAlphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* fullbrightAlphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* materialAlphaShader(U32 mask, LLGLSLShader* ordinary);
    static void finishFrame(
        LLPipeline& pipeline, LLRenderTarget& screen,
        LLVertexBuffer& screen_triangle, bool cube_snapshot,
        bool impostor_render, bool mouselook);
};

#endif
