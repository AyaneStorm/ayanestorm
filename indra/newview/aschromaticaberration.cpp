/**
 * @file aschromaticaberration.cpp
 * @author chanayane@firestorm
 * @brief Optional radial camera chromatic aberration.
 */
#include "llviewerprecompiledheaders.h"
#include "aschromaticaberration.h"
#include "asbackgroundisolate.h"
#include "llcontrol.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "lluictrl.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

namespace
{
    LLGLSLShader sProgram;
    const LLStaticHashedString sStrength("aberration_strength");
    const LLStaticHashedString sFalloff("aberration_falloff");
    const LLStaticHashedString sCenter("aberration_center");
}

extern bool gCubeSnapshot;

void ASChromaticAberration::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASChromaticAberration.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            const std::string name = data.asString();
            if (name == "ASChromaticAberrationStrength" || name == "ASChromaticAberrationFalloff" ||
                name == "ASChromaticAberrationCenterX" || name == "ASChromaticAberrationCenterY")
            {
                if (LLControlVariable* control = gSavedSettings.getControl(name))
                {
                    control->resetToDefault(true);
                }
            }
        });
}

void ASChromaticAberration::registerShader(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sProgram);
}

bool ASChromaticAberration::createShader(S32 shader_level)
{
    sProgram.mName = "AyaneStorm Chromatic Aberration Shader";
    sProgram.mShaderFiles.clear();
    sProgram.clearPermutations();
    sProgram.mFeatures.isDeferred = true;
    sProgram.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    sProgram.mShaderFiles.emplace_back("deferred/aschromaticaberrationF.glsl", GL_FRAGMENT_SHADER);
    sProgram.mShaderLevel = shader_level;
    return sProgram.createShader();
}

void ASChromaticAberration::unloadShader()
{
    sProgram.unload();
}

bool ASChromaticAberration::render(LLRenderTarget& source, LLRenderTarget& destination,
                                   LLVertexBuffer& screen_triangle)
{
    if (!gSavedSettings.getBOOL("ASChromaticAberrationEnabled") || !sProgram.isComplete() ||
        gCubeSnapshot || ASBackgroundIsolate::isActive() || &source == &destination ||
        source.getWidth() <= 0 || source.getHeight() <= 0 ||
        source.getWidth() != destination.getWidth() || source.getHeight() != destination.getHeight())
    {
        return false;
    }

    const F32 strength = llclamp(gSavedSettings.getF32("ASChromaticAberrationStrength"), 0.f, 20.f);
    if (strength <= 0.f)
    {
        return false;
    }

    LL_PROFILE_GPU_ZONE("Chromatic Aberration");
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    destination.bindTarget();
    sProgram.bind();
    sProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &source);
    sProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)source.getWidth(), (F32)source.getHeight());
    sProgram.uniform1f(sStrength, strength);
    sProgram.uniform1f(sFalloff, llclamp(gSavedSettings.getF32("ASChromaticAberrationFalloff"), 0.001f, 4.f));
    sProgram.uniform2f(sCenter, llclamp(gSavedSettings.getF32("ASChromaticAberrationCenterX"), 0.f, 1.f),
                        llclamp(gSavedSettings.getF32("ASChromaticAberrationCenterY"), 0.f, 1.f));
    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    sProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, source.getUsage());
    sProgram.unbind();
    destination.flush();
    return true;
}

void ASChromaticAberration::renderCenterBeacon()
{
    if (!gSavedSettings.getBOOL("ASChromaticAberrationShowBeacon") ||
        !gSavedSettings.getBOOL("ASChromaticAberrationEnabled"))
    {
        return;
    }

    static const S32 ARM_LENGTH = 12;
    const S32 width = gViewerWindow->getWorldViewWidthScaled();
    const S32 height = gViewerWindow->getWorldViewHeightScaled();
    const F32 center_x = llclamp(gSavedSettings.getF32("ASChromaticAberrationCenterX"), 0.f, 1.f) * (F32)width;
    const F32 center_y = llclamp(gSavedSettings.getF32("ASChromaticAberrationCenterY"), 0.f, 1.f) * (F32)height;

    gUIProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.setLineWidth(2.f);
    gGL.color4f(1.f, 1.f, 1.f, 0.8f);
    gGL.begin(LLRender::LINES);
    gGL.vertex2f(center_x - ARM_LENGTH, center_y);
    gGL.vertex2f(center_x + ARM_LENGTH, center_y);
    gGL.vertex2f(center_x, center_y - ARM_LENGTH);
    gGL.vertex2f(center_x, center_y + ARM_LENGTH);
    gGL.end();
    gGL.setLineWidth(1.f);
    gUIProgram.unbind();
}
