/**
 * @file asoitdispatcher.cpp
 * @brief AyaneStorm selection between independent OIT renderers.
 * @author chanayane@firestorm
 */

#include "llviewerprecompiledheaders.h"

#include "asoitdispatcher.h"

#include "asavboit.h"
#include "asexactoit.h"
#include "lldrawpoolalpha.h"
#include "lldrawpoolwater.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>

extern bool gCubeSnapshot;

namespace
{
enum class TransparencyMode : S32
{
    STANDARD = 0,
    EXACT_OIT = 1,
    AVBOIT = 2,
    AYASTORM = 3
};

TransparencyMode sTransparencyMode = TransparencyMode::STANDARD;

struct ASAlphaGroupEntry
{
    LLSpatialGroup* group = nullptr;
    bool rigged = false;
    F32 depth = 0.f;
};

std::vector<ASAlphaGroupEntry> sMergedAlphaGroups;
bool sMergedAlphaActive = false;

class MergedAlphaScope
{
public:
    MergedAlphaScope()
    {
        std::vector<ASAlphaGroupEntry> world_groups;
        std::vector<ASAlphaGroupEntry> rigged_groups;

        for (LLCullResult::sg_iterator iter = gPipeline.beginAlphaGroups();
             iter != gPipeline.endAlphaGroups(); ++iter)
        {
            world_groups.push_back({ *iter, false, (*iter)->mDepth });
        }

        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3& camera_origin = camera->getOrigin();
        const LLVector3& camera_at = camera->getAtAxis();
        for (LLCullResult::sg_iterator iter = gPipeline.beginRiggedAlphaGroups();
             iter != gPipeline.endRiggedAlphaGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            F32 depth = group->mDepth;
            LLVOAvatar* avatar = group->mAvatarp;
            if (avatar && avatar->mDrawable)
            {
                const LLVector3* extents = avatar->getLastAnimExtents();
                const LLVector3 center =
                    (extents[0] + extents[1]) * 0.5f - camera_origin;
                const LLVector3 half =
                    (extents[1] - extents[0]) * 0.5f;
                depth = center * camera_at
                    - (std::fabs(camera_at.mV[0]) * half.mV[0]
                       + std::fabs(camera_at.mV[1]) * half.mV[1]
                       + std::fabs(camera_at.mV[2]) * half.mV[2]);
            }
            rigged_groups.push_back({ group, true, depth });
        }

        std::sort(rigged_groups.begin(), rigged_groups.end(),
            [](const ASAlphaGroupEntry& lhs, const ASAlphaGroupEntry& rhs)
            {
                if (lhs.group->mAvatarp != rhs.group->mAvatarp)
                {
                    if (lhs.depth != rhs.depth)
                    {
                        return lhs.depth > rhs.depth;
                    }
                    return lhs.group->mAvatarp < rhs.group->mAvatarp;
                }
                return lhs.group->mRenderOrder > rhs.group->mRenderOrder;
            });

        sMergedAlphaGroups.clear();
        sMergedAlphaGroups.reserve(world_groups.size() + rigged_groups.size());
        auto world = world_groups.begin();
        auto rigged = rigged_groups.begin();
        while (world != world_groups.end() || rigged != rigged_groups.end())
        {
            if (rigged == rigged_groups.end() ||
                (world != world_groups.end() && world->depth >= rigged->depth))
            {
                sMergedAlphaGroups.push_back(*world++);
            }
            else
            {
                sMergedAlphaGroups.push_back(*rigged++);
            }
        }
        sMergedAlphaActive = true;
    }

    ~MergedAlphaScope()
    {
        sMergedAlphaActive = false;
        sMergedAlphaGroups.clear();
    }
};

TransparencyMode synchronizeModeSettings()
{
    S32 mode = gSavedSettings.getS32("ASRenderOITMode");
    if (mode < static_cast<S32>(TransparencyMode::STANDARD) ||
        mode > static_cast<S32>(TransparencyMode::AYASTORM))
    {
        // Migrate existing installations before making the selector authoritative.
        mode = gSavedSettings.getBOOL("ASRenderAVBOIT") ?
            static_cast<S32>(TransparencyMode::AVBOIT) :
            (gSavedSettings.getBOOL("ASRenderExactOIT") ?
                static_cast<S32>(TransparencyMode::EXACT_OIT) :
                static_cast<S32>(TransparencyMode::STANDARD));
        gSavedSettings.setS32("ASRenderOITMode", mode);
    }

    const bool exact = mode == static_cast<S32>(TransparencyMode::EXACT_OIT);
    const bool avboit = mode == static_cast<S32>(TransparencyMode::AVBOIT);
    if (gSavedSettings.getBOOL("ASRenderExactOIT") != exact)
    {
        gSavedSettings.setBOOL("ASRenderExactOIT", exact);
    }
    if (gSavedSettings.getBOOL("ASRenderAVBOIT") != avboit)
    {
        gSavedSettings.setBOOL("ASRenderAVBOIT", avboit);
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

void ASOITDispatcher::refreshOrderIndependentAlphaState()
{
    // Both accessors are user intent AND hardware support. Deliberately not
    // ASAVBOIT::available(), which additionally requires allocated resources:
    // those are created lazily at the first alpha pass, after culling, so it
    // would read false on the frame the mode is enabled.
    sOrderIndependentAlpha = ASExactOIT::isEnabled() || ASAVBOIT::requested();
}

bool ASOITDispatcher::orderIndependentAlphaActive()
{
    return sOrderIndependentAlpha;
}

void ASOITDispatcher::beginFrame()
{
    // Translate the single live UI choice without coupling either renderer module
    // to the other renderer or to preferences.
    static TransparencyMode previous_mode = TransparencyMode::STANDARD;
    static U32 vanilla_rebuild_frames = 0u;
    const TransparencyMode mode = synchronizeModeSettings();
    sTransparencyMode = mode;
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
    ASAVBOIT::beginFrame();
    ASExactOIT::beginFrame();
}

bool ASOITDispatcher::captureActive()
{
    return ASAVBOIT::captureActive() || ASExactOIT::captureActive();
}

bool ASOITDispatcher::captureCompleted()
{
    return ASAVBOIT::captureCompleted() || ASExactOIT::captureCompleted();
}

ASAlphaGroupTraversal::ASAlphaGroupTraversal(
    Iterator begin, Iterator end, bool rigged) :
    mCurrent(begin),
    mEnd(end),
    mRigged(rigged),
    mMerged(sMergedAlphaActive)
{
}

bool ASAlphaGroupTraversal::next(LLSpatialGroup*& group, bool& rigged)
{
    if (mMerged)
    {
        if (mMergedIndex == sMergedAlphaGroups.size())
        {
            return false;
        }
        const ASAlphaGroupEntry& entry = sMergedAlphaGroups[mMergedIndex++];
        group = entry.group;
        rigged = entry.rigged;
        return true;
    }

    if (mCurrent == mEnd)
    {
        return false;
    }
    group = *mCurrent++;
    rigged = mRigged;
    return true;
}

ASAlphaGroupDepthScope::ASAlphaGroupDepthScope(
    bool merged, bool rigged, S32 pool_type)
{
    if (merged)
    {
        const bool write_depth = rigged ||
            LLDrawPoolWater::sSkipScreenCopy ||
            LLPipeline::sImpostorRenderAlphaDepthPass ||
            pool_type == LLDrawPool::POOL_ALPHA_PRE_WATER;
        mDepth.emplace(GL_TRUE, write_depth ? GL_TRUE : GL_FALSE);
    }
}

bool ASOITDispatcher::mergedAlphaActive()
{
    return sMergedAlphaActive;
}

bool ASOITDispatcher::renderPostDeferredCapture(
    LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign,
    LLGLSLShader*& emissive_shader, LLGLSLShader*& pbr_emissive_shader)
{
    if (ASAVBOIT::renderPostDeferredCapture(
            pool, prepare, water_sign, emissive_shader, pbr_emissive_shader))
    {
        return true;
    }
    return ASExactOIT::renderPostDeferredCapture(
        pool, prepare, water_sign, emissive_shader, pbr_emissive_shader);
}

void ASOITDispatcher::renderNonOITPostDeferred(LLDrawPoolAlpha& pool)
{
    LLGLSLShader::unbind();

    if (sTransparencyMode == TransparencyMode::AYASTORM &&
        !LLPipeline::sRenderingHUDs &&
        !LLPipeline::sImpostorRender &&
        !LLPipeline::sReflectionRender &&
        !gCubeSnapshot &&
        LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD &&
        pool.getType() == LLDrawPool::POOL_ALPHA_POST_WATER)
    {
        // CPU-only adaptation of AYAstorm's newer unified group-depth merge.
        MergedAlphaScope merged_alpha;
        pool.forwardRender(false);
        return;
    }

    // Standard is the original Firestorm traversal. AYAstorm also preserves
    // this ordering for pre-water alpha and HUD rendering.
    if (!LLPipeline::sRenderingHUDs)
    {
        pool.forwardRender(true);
    }
    pool.forwardRender();
}

bool ASOITDispatcher::configureCapturedDrawIfActive(
    LLGLSLShader* shader, U32 color_source, U32 color_destination,
    U32 alpha_source, U32 alpha_destination)
{
    if (ASAVBOIT::captureActive())
    {
        return ASAVBOIT::configureCapturedDrawIfActive(shader);
    }
    return ASExactOIT::configureCapturedDrawIfActive(
        shader, color_source, color_destination, alpha_source,
        alpha_destination);
}

bool ASOITDispatcher::handleCapturedEmissives(
    LLDrawPoolAlpha& pool, bool depth_only,
    std::vector<LLDrawInfo*>& emissives,
    std::vector<LLDrawInfo*>& pbr_emissives,
    std::vector<LLDrawInfo*>& rigged_emissives,
    std::vector<LLDrawInfo*>& pbr_rigged_emissives)
{
    if (ASAVBOIT::captureActive())
    {
        return ASAVBOIT::handleCapturedEmissives(
            pool, depth_only, emissives, pbr_emissives, rigged_emissives,
            pbr_rigged_emissives);
    }
    return ASExactOIT::handleCapturedEmissives(
        pool, depth_only, emissives, pbr_emissives, rigged_emissives,
        pbr_rigged_emissives);
}

void ASOITDispatcher::configureGLTFCapturedDraw(LLGLSLShader& shader)
{
    if (ASAVBOIT::captureActive())
    {
        ASAVBOIT::configureGLTFCapturedDraw(shader);
    }
    else
    {
        ASExactOIT::configureGLTFCapturedDraw(shader);
    }
}

LLGLSLShader& ASOITDispatcher::gltfProgram(LLGLSLShader& ordinary)
{
    return ASAVBOIT::captureActive() ? ASAVBOIT::gltfProgram(ordinary) :
        ASExactOIT::gltfProgram(ordinary);
}

LLGLSLShader* ASOITDispatcher::alphaShader(LLGLSLShader* ordinary)
{
    return ASAVBOIT::captureActive() ? ASAVBOIT::alphaShader(ordinary) :
        ASExactOIT::alphaShader(ordinary);
}

LLGLSLShader* ASOITDispatcher::pbrAlphaShader(LLGLSLShader* ordinary)
{
    return ASAVBOIT::captureActive() ? ASAVBOIT::pbrAlphaShader(ordinary) :
        ASExactOIT::pbrAlphaShader(ordinary);
}

LLGLSLShader* ASOITDispatcher::fullbrightAlphaShader(LLGLSLShader* ordinary)
{
    return ASAVBOIT::captureActive() ?
        ASAVBOIT::fullbrightAlphaShader(ordinary) :
        ASExactOIT::fullbrightAlphaShader(ordinary);
}

LLGLSLShader* ASOITDispatcher::materialAlphaShader(
    U32 mask, LLGLSLShader* ordinary)
{
    return ASAVBOIT::captureActive() ?
        ASAVBOIT::materialAlphaShader(mask, ordinary) :
        ASExactOIT::materialAlphaShader(mask, ordinary);
}

void ASOITDispatcher::finishFrame(
    LLPipeline& pipeline, LLRenderTarget& screen,
    LLVertexBuffer& screen_triangle, bool cube_snapshot,
    bool impostor_render, bool mouselook)
{
    if (!ASAVBOIT::finishFrame(pipeline, screen))
    {
        ASExactOIT::finishFrame(
            pipeline, screen, screen_triangle, cube_snapshot,
            impostor_render, mouselook);
    }
}
