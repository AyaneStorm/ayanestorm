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

    // Live DoF (ASDepthOfFieldMode 3) transparency bins: the lens of the
    // frame, in the capture's full-resolution pixels (asdoflive.cpp).
    struct DoFLens
    {
        F32 mFocalDistance = 0.f;
        F32 mBlurConstant = 0.f;
        F32 mTanPixelAngle = 0.f;
        F32 mMagnification = 0.f;
        F32 mMaxCoC = 0.f;
        F32 mNearRadius = 0.f;
        F32 mFarRadius = 0.f;
        F32 mSplitRadius = 0.f;     // N1 / N2 boundary
        F32 mFarSplitRadius = 0.f;  // B1 / B2 boundary
        F32 mGatherScale = 0.5f;  // full-resolution to gather pixels
        // Lens field (ASDepthOfField::LensField): field position is
        // (uv - 0.5) * scale; curvature shifts the normalized CoC by
        // curvature * field^2.
        F32 mFieldScale[2] = { 0.f, 0.f };
        F32 mCurvature = 0.f;
    };
    // Number of bin textures: N2, N1, F, B1, B2 colour sums and their
    // energies. With the two accumulators, 8 draw buffers: OpenGL 4.1's
    // guaranteed maximum (and macOS's).
    static constexpr U32 DOF_BIN_TEXTURES = 6;

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

    // Bins requested for this frame's post-water capture (nullptr: none).
    // Mode 4 writes them in its own COLOR pass; any other mode runs
    // renderDoFCapture(), which captures without compositing anything.
    static void setDoFLens(const DoFLens* lens);
    static bool renderDoFCapture(LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign);
    // True when this frame's bins exist at width x height. The textures
    // (RGBA16F, full resolution, no mips) stay valid until the next capture.
    static bool dofBinsReady(U32 width, U32 height);
    static GLuint dofBinTexture(U32 index);
    // Sum of weights (x) and of optical depth (y) of the same capture.
    static GLuint dofWeightTexture();

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
    static bool capture(LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign,
                        LLGLSLShader*& emissive_shader, LLGLSLShader*& pbr_emissive_shader,
                        bool dof_only);
    static bool probeBlending();
};

#endif
