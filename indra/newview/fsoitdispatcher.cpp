/**
 * @file fsoitdispatcher.cpp
 * @brief Neutral selection between independent OIT renderers.
 * @author chanayane@firestorm
 */

#include "llviewerprecompiledheaders.h"

#include "fsoitdispatcher.h"

#include "fsavboit.h"
#include "fsexactoit.h"
#include "llspatialpartition.h"
#include "llviewercontrol.h"
#include "pipeline.h"

namespace
{
enum class TransparencyMode : S32
{
    STANDARD = 0,
    EXACT_OIT = 1,
    AVBOIT = 2
};

TransparencyMode synchronizeModeSettings()
{
    S32 mode = gSavedSettings.getS32("RenderOITMode");
    if (mode < static_cast<S32>(TransparencyMode::STANDARD) ||
        mode > static_cast<S32>(TransparencyMode::AVBOIT))
    {
        // Migrate existing installations before making the selector authoritative.
        mode = gSavedSettings.getBOOL("RenderAVBOIT") ?
            static_cast<S32>(TransparencyMode::AVBOIT) :
            (gSavedSettings.getBOOL("RenderExactOIT") ?
                static_cast<S32>(TransparencyMode::EXACT_OIT) :
                static_cast<S32>(TransparencyMode::STANDARD));
        gSavedSettings.setS32("RenderOITMode", mode);
    }

    const bool exact = mode == static_cast<S32>(TransparencyMode::EXACT_OIT);
    const bool avboit = mode == static_cast<S32>(TransparencyMode::AVBOIT);
    if (gSavedSettings.getBOOL("RenderExactOIT") != exact)
    {
        gSavedSettings.setBOOL("RenderExactOIT", exact);
    }
    if (gSavedSettings.getBOOL("RenderAVBOIT") != avboit)
    {
        gSavedSettings.setBOOL("RenderAVBOIT", avboit);
    }
    return static_cast<TransparencyMode>(mode);
}

void invalidateVanillaAlphaOrdering()
{
    const auto invalidate_visible =
        [](LLCullResult::sg_iterator begin, LLCullResult::sg_iterator end)
    {
        for (LLCullResult::sg_iterator iter = begin; iter != end; ++iter)
        {
            LLSpatialGroup* group = *iter;
            if (group && !group->isDead())
            {
                group->setState(LLSpatialGroup::ALPHA_DIRTY);
                gPipeline.markRebuild(group);
            }
        }
    };
    invalidate_visible(
        gPipeline.beginAlphaGroups(), gPipeline.endAlphaGroups());
    invalidate_visible(
        gPipeline.beginRiggedAlphaGroups(),
        gPipeline.endRiggedAlphaGroups());

    // The current cull result is insufficient: groups outside it can later
    // enter view carrying OIT-era alpha ordering. Traverse all region
    // volume/bridge octrees so Standard eventually sees only rebuilt data.
    gPipeline.rebuildDrawInfo();
}
}

// Snapshot of "an order-independent renderer will handle this frame's alpha".
// Refreshed once per frame before culling; read per spatial group during
// culling, so it must stay a plain load with no settings lookup behind it.
static bool sOrderIndependentAlpha = false;

void FSOITDispatcher::refreshOrderIndependentAlphaState()
{
    // Both accessors are user intent AND hardware support. Deliberately not
    // FSAVBOIT::available(), which additionally requires allocated resources:
    // those are created lazily at the first alpha pass, after culling, so it
    // would read false on the frame the mode is enabled.
    sOrderIndependentAlpha = FSExactOIT::isEnabled() || FSAVBOIT::requested();
}

bool FSOITDispatcher::orderIndependentAlphaActive()
{
    return sOrderIndependentAlpha;
}

void FSOITDispatcher::beginFrame()
{
    // Translate the single live UI choice without coupling either renderer module
    // to the other renderer or to preferences.
    static TransparencyMode previous_mode = TransparencyMode::STANDARD;
    static U32 vanilla_rebuild_frames = 0u;
    const TransparencyMode mode = synchronizeModeSettings();
    if (previous_mode != mode)
    {
        invalidateVanillaAlphaOrdering();
        if (mode == TransparencyMode::STANDARD)
        {
            // Repeat after the first refreshed Standard cull result.
            vanilla_rebuild_frames = 1u;
        }
    }
    else if (mode == TransparencyMode::STANDARD &&
             vanilla_rebuild_frames > 0u)
    {
        invalidateVanillaAlphaOrdering();
        --vanilla_rebuild_frames;
    }
    previous_mode = mode;
    FSAVBOIT::beginFrame();
    FSExactOIT::beginFrame();
}

bool FSOITDispatcher::captureActive()
{
    return FSAVBOIT::captureActive() || FSExactOIT::captureActive();
}

bool FSOITDispatcher::captureCompleted()
{
    return FSAVBOIT::captureCompleted() || FSExactOIT::captureCompleted();
}

bool FSOITDispatcher::renderPostDeferredCapture(
    LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign,
    LLGLSLShader*& emissive_shader, LLGLSLShader*& pbr_emissive_shader)
{
    if (FSAVBOIT::renderPostDeferredCapture(
            pool, prepare, water_sign, emissive_shader, pbr_emissive_shader))
    {
        return true;
    }
    return FSExactOIT::renderPostDeferredCapture(
        pool, prepare, water_sign, emissive_shader, pbr_emissive_shader);
}

bool FSOITDispatcher::configureCapturedDrawIfActive(
    LLGLSLShader* shader, U32 color_source, U32 color_destination,
    U32 alpha_source, U32 alpha_destination)
{
    if (FSAVBOIT::captureActive())
    {
        return FSAVBOIT::configureCapturedDrawIfActive(shader);
    }
    return FSExactOIT::configureCapturedDrawIfActive(
        shader, color_source, color_destination, alpha_source,
        alpha_destination);
}

bool FSOITDispatcher::handleCapturedEmissives(
    LLDrawPoolAlpha& pool, bool depth_only,
    std::vector<LLDrawInfo*>& emissives,
    std::vector<LLDrawInfo*>& pbr_emissives,
    std::vector<LLDrawInfo*>& rigged_emissives,
    std::vector<LLDrawInfo*>& pbr_rigged_emissives)
{
    if (FSAVBOIT::captureActive())
    {
        return FSAVBOIT::handleCapturedEmissives(
            pool, depth_only, emissives, pbr_emissives, rigged_emissives,
            pbr_rigged_emissives);
    }
    return FSExactOIT::handleCapturedEmissives(
        pool, depth_only, emissives, pbr_emissives, rigged_emissives,
        pbr_rigged_emissives);
}

void FSOITDispatcher::configureGLTFCapturedDraw(LLGLSLShader& shader)
{
    if (FSAVBOIT::captureActive())
    {
        FSAVBOIT::configureGLTFCapturedDraw(shader);
    }
    else
    {
        FSExactOIT::configureGLTFCapturedDraw(shader);
    }
}

LLGLSLShader& FSOITDispatcher::gltfProgram(LLGLSLShader& ordinary)
{
    return FSAVBOIT::captureActive() ? FSAVBOIT::gltfProgram(ordinary) :
        FSExactOIT::gltfProgram(ordinary);
}

LLGLSLShader* FSOITDispatcher::alphaShader(LLGLSLShader* ordinary)
{
    return FSAVBOIT::captureActive() ? FSAVBOIT::alphaShader(ordinary) :
        FSExactOIT::alphaShader(ordinary);
}

LLGLSLShader* FSOITDispatcher::pbrAlphaShader(LLGLSLShader* ordinary)
{
    return FSAVBOIT::captureActive() ? FSAVBOIT::pbrAlphaShader(ordinary) :
        FSExactOIT::pbrAlphaShader(ordinary);
}

LLGLSLShader* FSOITDispatcher::fullbrightAlphaShader(LLGLSLShader* ordinary)
{
    return FSAVBOIT::captureActive() ?
        FSAVBOIT::fullbrightAlphaShader(ordinary) :
        FSExactOIT::fullbrightAlphaShader(ordinary);
}

LLGLSLShader* FSOITDispatcher::materialAlphaShader(
    U32 mask, LLGLSLShader* ordinary)
{
    return FSAVBOIT::captureActive() ?
        FSAVBOIT::materialAlphaShader(mask, ordinary) :
        FSExactOIT::materialAlphaShader(mask, ordinary);
}

void FSOITDispatcher::finishFrame(
    LLPipeline& pipeline, LLRenderTarget& screen,
    LLVertexBuffer& screen_triangle, bool cube_snapshot,
    bool impostor_render, bool mouselook)
{
    if (!FSAVBOIT::finishFrame(pipeline, screen))
    {
        FSExactOIT::finishFrame(
            pipeline, screen, screen_triangle, cube_snapshot,
            impostor_render, mouselook);
    }
}
