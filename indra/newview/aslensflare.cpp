/**
 * @file aslensflare.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local screen-space sun and moon lens flares.
 */

#include "llviewerprecompiledheaders.h"

#include "aslensflare.h"

#include "llenvironment.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llsettingssky.h"
#include "llshadermgr.h"
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
    bool projectDirection(const LLVector3& direction, LLVector2& position)
    {
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        LLCoordGL screen;
        const LLVector3 source = camera->getOrigin() + direction * 4096.f;
        if (!camera->projectPosAgentToScreen(source, screen, false))
        {
            return false;
        }

        const LLRect viewport = gViewerWindow->getWorldViewRectScaled();
        if (viewport.getWidth() <= 0 || viewport.getHeight() <= 0)
        {
            return false;
        }

        position.set((F32)(screen.mX - viewport.mLeft) / (F32)viewport.getWidth(),
                     (F32)(screen.mY - viewport.mBottom) / (F32)viewport.getHeight());
        return position.mV[VX] >= 0.f && position.mV[VX] <= 1.f &&
               position.mV[VY] >= 0.f && position.mV[VY] <= 1.f;
    }
}

extern bool gCubeSnapshot;

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
        !sLensFlareProgram.isComplete() || gCubeSnapshot)
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
    sLensFlareProgram.uniform2f(sScreenResolution, (F32)depth_target.getWidth(), (F32)depth_target.getHeight());

    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);

    sLensFlareProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    sLensFlareProgram.unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}
