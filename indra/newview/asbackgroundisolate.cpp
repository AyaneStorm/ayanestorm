/**
 * @file asbackgroundisolate.cpp
 * @author chanayane@firestorm
 * @brief See asbackgroundisolate.h
 */

#include "llviewerprecompiledheaders.h"

#include "asbackgroundisolate.h"

#include "lldrawable.h"
#include "llgl.h"
#include "pipeline.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"

namespace
{
    LLGLSLShader sBackgroundIsolateProgram;
    const LLStaticHashedString sIsolateColor("isolate_color");
    const LLStaticHashedString sBaseLayer("base_layer");
    const LLStaticHashedString sExposure("exposure");

    bool sActive = false;
    LLColor4 sColor(0.f, 0.f, 0.f, 1.f);
    std::set<LLUUID> sLightRigIds;
    // Objects updateDrawableHiddenState() has set FORCE_INVISIBLE on,
    // tracked by object id so restoreAllHiddenDrawables() can explicitly
    // un-hide every one of them rather than waiting for incidental
    // spatial-group traversal to revisit them.
    std::set<LLUUID> sHiddenObjectIds;
}

extern bool gCubeSnapshot;
extern LLPointer<LLVOAvatarSelf> gAgentAvatarp;
extern LLPipeline gPipeline;

namespace
{
    // Terrain, water, clouds, and grass render through their own geometry
    // managers (LLSurfacePatch/LLVOSurfacePatch for terrain, LLVOSky/
    // LLVOWLSky for sky/clouds, LLVOWater, LLVOGrass) that never check
    // LLDrawable::FORCE_INVISIBLE -- unlike ordinary volume geometry, no
    // amount of setting that flag hides them. None of these can ever be
    // "mine" though (there's no such thing as a self-owned patch of
    // terrain or a self-owned cloud), so unlike RENDER_TYPE_VOLUME/
    // RENDER_TYPE_AVATAR (which are shared by self and non-self content
    // and can't be disabled wholesale), it's always safe to disable these
    // render types outright for the whole duration of isolate mode.
    // RENDER_TYPE_SKY/RENDER_TYPE_WL_SKY are deliberately left out: doing
    // so previously broke LLVOSky::updateSky()'s atmospherics refresh and
    // corrupted LLReflectionMapManager's baked ambient/irradiance cubemaps
    // (the global render-type mask also suppresses sky during the probe
    // manager's own internal capture passes), producing a camera-reactive
    // magenta tint on every surface -- the sky dome is instead left
    // rendering normally and simply painted over by the solid-color
    // backdrop pass (render() below), which does not have this problem.
    const U32 sIsolateRenderTypes[] = {
        LLPipeline::RENDER_TYPE_TERRAIN,
        LLPipeline::RENDER_TYPE_WATER,
        LLPipeline::RENDER_TYPE_VOIDWATER,
        LLPipeline::RENDER_TYPE_WATEREXCLUSION,
        LLPipeline::RENDER_TYPE_CLOUDS,
        LLPipeline::RENDER_TYPE_GRASS,
        LLPipeline::RENDER_TYPE_PARTICLES,
    };
}

void ASBackgroundIsolate::setActive(bool active, const LLColor4& color)
{
    if (active != sActive)
    {
        for (U32 type : sIsolateRenderTypes)
        {
            if (gPipeline.hasRenderType(type) == active)
            {
                gPipeline.toggleRenderType(type);
            }
        }
    }

    sActive = active;
    sColor = color;
}

void ASBackgroundIsolate::setLightRigIds(const std::set<LLUUID>& ids)
{
    sLightRigIds = ids;
}

bool ASBackgroundIsolate::isActive()
{
    return sActive;
}

bool ASBackgroundIsolate::shouldHideDrawable(LLDrawable* drawable)
{
    if (!sActive || !drawable)
    {
        return false;
    }

    LLViewerObject* obj = drawable->getVObj();
    if (!obj)
    {
        return false;
    }

    if (obj->asAvatar())
    {
        return !obj->asAvatar()->isSelf();
    }

    if (sLightRigIds.count(obj->getID()))
    {
        return false;
    }

    LLVOAvatar* ancestor = obj->getAvatarAncestor();
    if (ancestor && ancestor->isSelf())
    {
        return false;
    }

    if (gAgentAvatarp && obj->getID() == gAgentAvatarp->getID())
    {
        return false;
    }

    return true;
}

void ASBackgroundIsolate::updateDrawableHiddenState(LLDrawable* drawable)
{
    if (!drawable || drawable->isDead())
    {
        return;
    }

    // Shadow maps must retain the isolated scene as occluding geometry. If
    // the room is removed from the shadow draw maps along with the visible
    // scene, walls and roofs stop blocking the sun and a sun-direction-reactive
    // highlight appears on the avatar. Temporarily restore those drawables
    // during shadow state sorting; the main-camera pass hides them again.
    const bool should_hide = !LLPipeline::sShadowRender && shouldHideDrawable(drawable);
    const bool currently_hidden = drawable->isState(LLDrawable::FORCE_INVISIBLE);

    if (should_hide == currently_hidden)
    {
        return;
    }

    LLViewerObject* obj = drawable->getVObj();

    if (should_hide)
    {
        drawable->setState(LLDrawable::FORCE_INVISIBLE);
        if (obj)
        {
            sHiddenObjectIds.insert(obj->getID());
        }
    }
    else
    {
        drawable->clearState(LLDrawable::FORCE_INVISIBLE);
        if (obj)
        {
            sHiddenObjectIds.erase(obj->getID());
        }
    }

    // Force the owning spatial group to rebuild its geometry batch so the
    // state change is reflected immediately. markRebuild(drawable, ...)
    // alone is NOT sufficient here: it only queues the drawable's own
    // updateGeometry() (a no-op for a static prim) and never touches the
    // group. LLVolumeGeometryManager::rebuildGeom() -- the function that
    // actually checks FORCE_INVISIBLE when building LLSpatialGroup::mDrawMap
    // -- only runs when the GROUP itself carries LLSpatialGroup::GEOM_DIRTY
    // (checked in LLSpatialPartition::rebuildGeom). Without explicitly
    // dirtying the group and queuing it via markRebuild(LLSpatialGroup*),
    // a group that isn't otherwise becoming geometry-dirty on its own (a
    // static, unmoving build) keeps drawing from its previously-built
    // mDrawMap forever, regardless of how many times FORCE_INVISIBLE is
    // set/cleared or the drawable-level rebuild is requested.
    gPipeline.markRebuild(drawable, LLDrawable::REBUILD_ALL);
    LLSpatialGroup* group = drawable->getSpatialGroup();
    if (group)
    {
        group->dirtyGeom();
        gPipeline.markRebuild(group);
    }
}

void ASBackgroundIsolate::restoreAllHiddenDrawables()
{
    for (const LLUUID& id : sHiddenObjectIds)
    {
        LLViewerObject* obj = gObjectList.findObject(id);
        if (!obj)
        {
            continue;
        }
        LLDrawable* drawable = obj->mDrawable;
        if (!drawable || drawable->isDead())
        {
            continue;
        }
        drawable->clearState(LLDrawable::FORCE_INVISIBLE);
        gPipeline.markRebuild(drawable, LLDrawable::REBUILD_ALL);
        LLSpatialGroup* group = drawable->getSpatialGroup();
        if (group)
        {
            group->dirtyGeom();
            gPipeline.markRebuild(group);
        }
    }
    sHiddenObjectIds.clear();
}

void ASBackgroundIsolate::registerShader(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sBackgroundIsolateProgram);
}

bool ASBackgroundIsolate::createShader(S32 shader_level)
{
    sBackgroundIsolateProgram.mName = "AyaneStorm Background Isolate Shader";
    sBackgroundIsolateProgram.mShaderFiles.clear();
    sBackgroundIsolateProgram.clearPermutations();
    sBackgroundIsolateProgram.mFeatures.isDeferred = true;
    sBackgroundIsolateProgram.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    sBackgroundIsolateProgram.mShaderFiles.emplace_back("deferred/asBackgroundIsolateF.glsl", GL_FRAGMENT_SHADER);
    sBackgroundIsolateProgram.mShaderLevel = shader_level;
    return sBackgroundIsolateProgram.createShader();
}

void ASBackgroundIsolate::unloadShader()
{
    sBackgroundIsolateProgram.unload();
}

void ASBackgroundIsolate::render(LLRenderTarget& depth_target, LLVertexBuffer& screen_triangle)
{
    if (!sActive || !sBackgroundIsolateProgram.isComplete() || gCubeSnapshot)
    {
        return;
    }

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    sBackgroundIsolateProgram.bind();
    sBackgroundIsolateProgram.uniform1i(sBaseLayer, 0);
    sBackgroundIsolateProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth_target, true, LLTexUnit::TFO_POINT);
    sBackgroundIsolateProgram.uniform4fv(sIsolateColor, 1, sColor.mV);

    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);

    sBackgroundIsolateProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    sBackgroundIsolateProgram.unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

void ASBackgroundIsolate::renderBaseLayer(LLRenderTarget& depth_target, LLRenderTarget& exposure_target,
                                          LLVertexBuffer& screen_triangle)
{
    if (!sActive || !sBackgroundIsolateProgram.isComplete() || gCubeSnapshot)
    {
        return;
    }

    // Opaque depth is complete here, but alpha/OIT has not rendered. The
    // shader's binary background mask replaces only far-depth atmospherics;
    // opaque avatar pixels remain untouched and hair blends over this color.
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    // mRT->screen alpha is the glow/bloom accumulator, not scene coverage.
    // Writing the isolate mask's alpha here makes every background pixel emit
    // maximum glow. Seed only RGB and preserve the existing zero glow channel.
    gGL.setColorMask(true, false);

    sBackgroundIsolateProgram.bind();
    sBackgroundIsolateProgram.uniform1i(sBaseLayer, 1);
    sBackgroundIsolateProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth_target, true, LLTexUnit::TFO_POINT);
    sBackgroundIsolateProgram.bindTexture(LLShaderMgr::EXPOSURE_MAP, &exposure_target, false, LLTexUnit::TFO_POINT);
    static LLCachedControl<F32> render_exposure(gSavedSettings, "RenderExposure", 1.f);
    sBackgroundIsolateProgram.uniform1f(sExposure, llclamp(render_exposure(), 0.5f, 4.f));
    sBackgroundIsolateProgram.uniform4fv(sIsolateColor, 1, sColor.mV);

    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);

    sBackgroundIsolateProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    sBackgroundIsolateProgram.unbindTexture(LLShaderMgr::EXPOSURE_MAP);
    sBackgroundIsolateProgram.unbind();
    gGL.setColorMask(true, true);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}
