/**
 * @file aslensflare.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local screen-space sun and moon lens flares.
 */

#include "llviewerprecompiledheaders.h"

#include "aslensflare.h"

#include "asbackgroundisolate.h"
#include "llcontrol.h"
#include "llenvironment.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llsettingssky.h"
#include "llshadermgr.h"
#include "lluictrl.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

namespace
{
    LLGLSLShader sLensFlareProgram;
    const LLStaticHashedString sSourcePosition("flare_source_position");
    const LLStaticHashedString sSourceColor("flare_source_color");
    const LLStaticHashedString sSourceStrength("flare_source_strength");
    const LLStaticHashedString sSaturation("flare_saturation");
    const LLStaticHashedString sScreenResolution("screen_res");
    const LLStaticHashedString sSnapshotTile("flare_snapshot_tile");
    const LLStaticHashedString sSnapshotRender("flare_snapshot_render");
    bool projectDirection(const LLVector3& direction, LLVector2& position)
    {
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        if (direction * camera->getAtAxis() <= 0.f)
        {
            return false;
        }

        LLCoordGL screen;
        const LLVector3 source = camera->getOrigin() + direction * 4096.f;
        // A tiled snapshot deliberately projects most of the full image outside
        // the current viewport, so retain the coordinates even when this reports
        // that the point is outside the current tile.
        camera->projectPosAgentToScreen(source, screen, false);

        // rawSnapshot() can resize mWorldViewRectRaw directly without updating
        // the cached scaled rectangle. Derive scaled bounds from the live raw
        // rectangle so celestial projection follows the actual snapshot target.
        const LLRect viewport = gViewerWindow->getWorldViewRectRaw();
        const LLVector2 display_scale = gViewerWindow->getDisplayScale();
        if (viewport.getWidth() <= 0 || viewport.getHeight() <= 0 ||
            display_scale.mV[VX] <= 0.f || display_scale.mV[VY] <= 0.f)
        {
            return false;
        }

        const F32 scaled_left = (F32)viewport.mLeft / display_scale.mV[VX];
        const F32 scaled_bottom = (F32)viewport.mBottom / display_scale.mV[VY];
        const F32 scaled_width = (F32)viewport.getWidth() / display_scale.mV[VX];
        const F32 scaled_height = (F32)viewport.getHeight() / display_scale.mV[VY];
        position.set(((F32)screen.mX - scaled_left) / scaled_width,
                     ((F32)screen.mY - scaled_bottom) / scaled_height);

        const F32 zoom = camera->getZoomFactor();
        if (zoom > 1.f)
        {
            const S32 tiles_per_row = llceil(zoom);
            const S32 tile = camera->getZoomSubRegion();
            position.mV[VX] = (position.mV[VX] + (F32)(tile % tiles_per_row)) / zoom;
            position.mV[VY] = (position.mV[VY] + (F32)(tile / tiles_per_row)) / zoom;
        }

        return position.mV[VX] >= 0.f && position.mV[VX] <= 1.f &&
               position.mV[VY] >= 0.f && position.mV[VY] <= 1.f;
    }
}

extern bool gCubeSnapshot;
extern bool gSnapshot;

void ASLensFlare::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASLensFlare.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            const std::string control_name = data.asString();
            if (control_name == "ASLensFlareStrength" ||
                control_name == "ASLensFlareSaturation")
            {
                if (LLControlVariable* control = gSavedSettings.getControl(control_name))
                {
                    control->resetToDefault(true);
                }
            }
        });
}

void ASLensFlare::registerShader(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sLensFlareProgram);
}

bool ASLensFlare::createShader(S32 shader_level)
{
    sLensFlareProgram.mName = "AyaneStorm Lens Flare Shader";
    sLensFlareProgram.mShaderFiles.clear();
    sLensFlareProgram.clearPermutations();
    sLensFlareProgram.mFeatures.isDeferred = true;
    sLensFlareProgram.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    sLensFlareProgram.mShaderFiles.emplace_back("deferred/aslensflareF.glsl", GL_FRAGMENT_SHADER);
    sLensFlareProgram.mShaderLevel = shader_level;
    return sLensFlareProgram.createShader();
}

void ASLensFlare::unloadShader()
{
    sLensFlareProgram.unload();
}

void ASLensFlare::render(LLRenderTarget& depth_target, LLVertexBuffer& screen_triangle)
{
    if (!gSavedSettings.getBOOL("ASLensFlareEnabled") ||
        !sLensFlareProgram.isComplete() || gCubeSnapshot ||
        ASBackgroundIsolate::isActive())
    {
        return;
    }

    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    if (!sky)
    {
        return;
    }

    LLVector2 source_position[2] = { LLVector2::zero, LLVector2::zero };
    LLColor3 source_color[2] = { LLColor3::black, LLColor3::black };
    F32 source_strength[2] = { 0.f, 0.f };
    const F32 strength = llclamp(gSavedSettings.getF32("ASLensFlareStrength"), 0.f, 1.f);
    const F32 saturation = llclamp(gSavedSettings.getF32("ASLensFlareSaturation"), 0.f, 1.f);
    const F32 zoom = llmax(LLViewerCamera::getInstance()->getZoomFactor(), 1.f);
    const S32 tiles_per_row = llceil(zoom);
    const S32 tile = LLViewerCamera::getInstance()->getZoomSubRegion();
    const F32 tile_x = zoom > 1.f ? (F32)(tile % tiles_per_row) : 0.f;
    const F32 tile_y = zoom > 1.f ? (F32)(tile / tiles_per_row) : 0.f;

    if (sky->getSunDirection().mV[VZ] > -0.05f &&
        projectDirection(sky->getSunDirection(), source_position[0]))
    {
        source_color[0] = sky->getSunlightColorClamped();
        source_strength[0] = 0.70f * strength;
    }
    if (sky->getMoonDirection().mV[VZ] > -0.05f &&
        projectDirection(sky->getMoonDirection(), source_position[1]))
    {
        source_color[1] = sky->getMoonlightColor();
        source_color[1].clamp();
        source_strength[1] = llclamp(sky->getMoonBrightness() * 0.30f, 0.08f, 0.35f) * strength;
    }

    if (source_strength[0] <= 0.f && source_strength[1] <= 0.f)
    {
        return;
    }

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ADD);

    sLensFlareProgram.bind();
    sLensFlareProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth_target, true, LLTexUnit::TFO_POINT);
    sLensFlareProgram.uniform2fv(sSourcePosition, 2, source_position[0].mV);
    sLensFlareProgram.uniform3fv(sSourceColor, 2, source_color[0].mV);
    sLensFlareProgram.uniform1fv(sSourceStrength, 2, source_strength);
    sLensFlareProgram.uniform1f(sSaturation, saturation);
    sLensFlareProgram.uniform2f(sScreenResolution,
                                (F32)depth_target.getWidth() * zoom,
                                (F32)depth_target.getHeight() * zoom);
    sLensFlareProgram.uniform3f(sSnapshotTile, zoom, tile_x, tile_y);
    // Snapshot depth does not preserve the viewport's sky sentinel reliably;
    // applying the live-view 0.999 test suppresses the complete flare. Snapshot
    // source occlusion must be supplied separately rather than inferred here.
    sLensFlareProgram.uniform1i(sSnapshotRender, gSnapshot ? 1 : 0);

    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);

    sLensFlareProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    sLensFlareProgram.unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}
