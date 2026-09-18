/**
 * @file asmotionblur.cpp
 * @author chanayane@firestorm
 * @brief Screen-space camera motion blur, reprojection-based (no per-object velocity buffer).
 */
#include "llviewerprecompiledheaders.h"
#include "asmotionblur.h"
#include "asbackgroundisolate.h"
#include "llcontrol.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "lluictrl.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"

namespace
{
    LLGLSLShader sProgram;
    const LLStaticHashedString sInvCurrProj("inv_curr_proj");
    const LLStaticHashedString sInvCurrModelview("inv_curr_modelview");
    const LLStaticHashedString sPrevModelviewProj("prev_modelview_proj");
    const LLStaticHashedString sStrength("motion_blur_strength");
    const LLStaticHashedString sMaxLength("motion_blur_max_length");
    const LLStaticHashedString sSamples("motion_blur_samples");
    const LLStaticHashedString sDebug("motion_blur_debug");
    constexpr S32 AS_MOTION_BLUR_MAX_SAMPLES = 24;

    // Consecutive main-view matrix snapshots. The pipeline updates gGLLastModelView and
    // gGLLastProjection before this capture, so render() cannot use get_last_*() as the
    // previous frame: by then those globals already contain this frame's matrices.
    glm::mat4 sCurrModelview(1.f);
    glm::mat4 sCurrProjection(1.f);
    glm::mat4 sPrevModelview(1.f);
    glm::mat4 sPrevProjection(1.f);
    bool sHaveCurrentFrameMatrices = false;
    bool sHaveFrameMatrices = false;
}

extern bool gCubeSnapshot;

void ASMotionBlur::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASMotionBlur.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            const std::string name = data.asString();
            if (name == "RenderMotionBlurStrength" || name == "RenderMotionBlurMaxLength" ||
                name == "RenderMotionBlurQuality" || name == "RenderMotionBlurDebug")
            {
                if (LLControlVariable* control = gSavedSettings.getControl(name))
                {
                    control->resetToDefault(true);
                }
            }
        });
}

void ASMotionBlur::registerShader(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sProgram);
}

bool ASMotionBlur::createShader(S32 shader_level)
{
    sProgram.mName = "AyaneStorm Motion Blur Shader";
    sProgram.mShaderFiles.clear();
    sProgram.clearPermutations();
    sProgram.mFeatures.isDeferred = true;
    sProgram.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    sProgram.mShaderFiles.emplace_back("deferred/asMotionBlurF.glsl", GL_FRAGMENT_SHADER);
    sProgram.mShaderLevel = shader_level;
    return sProgram.createShader();
}

void ASMotionBlur::unloadShader()
{
    sProgram.unload();
}

void ASMotionBlur::captureFrameMatrices()
{
    // Preserve our preceding capture before taking the current one. This call occurs
    // after the pipeline has overwritten its get_last_*() globals with the current frame.
    sPrevModelview = sCurrModelview;
    sPrevProjection = sCurrProjection;
    sHaveFrameMatrices = sHaveCurrentFrameMatrices;

    sCurrModelview = get_current_modelview();
    sCurrProjection = get_current_projection();
    sHaveCurrentFrameMatrices = true;
}

bool ASMotionBlur::render(LLRenderTarget& source, LLRenderTarget& destination, LLRenderTarget& depth,
                           LLVertexBuffer& screen_triangle)
{
    static LLCachedControl<bool> motion_blur_enabled(gSavedSettings, "RenderMotionBlur", false);
    if (!motion_blur_enabled || !sProgram.isComplete() || !sHaveFrameMatrices || gCubeSnapshot ||
        ASBackgroundIsolate::isActive() || &source == &destination ||
        source.getWidth() <= 0 || source.getHeight() <= 0 ||
        source.getWidth() != destination.getWidth() || source.getHeight() != destination.getHeight())
    {
        return false;
    }

    static LLCachedControl<F32> strength(gSavedSettings, "RenderMotionBlurStrength", 1.f);
    static LLCachedControl<F32> max_length(gSavedSettings, "RenderMotionBlurMaxLength", 32.f);
    static LLCachedControl<S32> quality(gSavedSettings, "RenderMotionBlurQuality", 1);
    static LLCachedControl<S32> debug_mode(gSavedSettings, "RenderMotionBlurDebug", 0);

    const F32 clamped_strength = llclamp((F32)strength, 0.f, 8.f);
    if (clamped_strength <= 0.f && debug_mode == 0)
    {
        return false;
    }

    // Low/medium/high sample-count tiers, matching the shader's fixed loop-unroll cap.
    static const S32 sample_counts[3] = { 8, 16, AS_MOTION_BLUR_MAX_SAMPLES };
    const S32 tier = llclamp((S32)quality, 0, 2);

    const glm::mat4 inv_curr_projection = glm::inverse(sCurrProjection);
    const glm::mat4 inv_curr_modelview = glm::inverse(sCurrModelview);
    const glm::mat4 prev_modelview_proj = sPrevProjection * sPrevModelview;

    LL_PROFILE_GPU_ZONE("Motion Blur");
    LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    destination.bindTarget();
    sProgram.bind();
    sProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &source);
    sProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth, true);
    sProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)source.getWidth(), (F32)source.getHeight());
    sProgram.uniformMatrix4fv(sInvCurrProj, 1, GL_FALSE, glm::value_ptr(inv_curr_projection));
    sProgram.uniformMatrix4fv(sInvCurrModelview, 1, GL_FALSE, glm::value_ptr(inv_curr_modelview));
    sProgram.uniformMatrix4fv(sPrevModelviewProj, 1, GL_FALSE, glm::value_ptr(prev_modelview_proj));
    sProgram.uniform1f(sStrength, clamped_strength);
    sProgram.uniform1f(sMaxLength, llclamp((F32)max_length, 1.f, 256.f));
    sProgram.uniform1i(sSamples, sample_counts[tier]);
    sProgram.uniform1i(sDebug, (S32)debug_mode);
    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    sProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, source.getUsage());
    sProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH, depth.getUsage());
    sProgram.unbind();
    destination.flush();
    return true;
}
