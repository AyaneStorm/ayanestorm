/**
 * @file asvignette.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local configurable screen-space vignette.
 */

#include "llviewerprecompiledheaders.h"

#include "asvignette.h"

#include "asbackgroundisolate.h"
#include "llcontrol.h"
#include "llgl.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "lluictrl.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"

namespace
{
    LLGLSLShader sVignetteProgram;
    const LLStaticHashedString sScreenResolution("screen_res");
    const LLStaticHashedString sStrength("vignette_strength");
    const LLStaticHashedString sRadius("vignette_radius");
    const LLStaticHashedString sSmoothness("vignette_smoothness");
    const LLStaticHashedString sShape("vignette_shape");
}

extern bool gCubeSnapshot;

void ASVignette::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASVignette.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            const std::string control_name = data.asString();
            if (control_name == "ASVignetteStrength" ||
                control_name == "ASVignetteRadius" ||
                control_name == "ASVignetteSmoothness" ||
                control_name == "ASVignetteShape")
            {
                if (LLControlVariable* control = gSavedSettings.getControl(control_name))
                {
                    control->resetToDefault(true);
                }
            }
        });
}

void ASVignette::registerShader(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sVignetteProgram);
}

bool ASVignette::createShader(S32 shader_level)
{
    sVignetteProgram.mName = "AyaneStorm Vignette Shader";
    sVignetteProgram.mShaderFiles.clear();
    sVignetteProgram.clearPermutations();
    sVignetteProgram.mFeatures.isDeferred = true;
    sVignetteProgram.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    sVignetteProgram.mShaderFiles.emplace_back("deferred/asvignetteF.glsl", GL_FRAGMENT_SHADER);
    sVignetteProgram.mShaderLevel = shader_level;
    return sVignetteProgram.createShader();
}

void ASVignette::unloadShader()
{
    sVignetteProgram.unload();
}

void ASVignette::render(S32 width, S32 height, LLVertexBuffer& screen_triangle)
{
    if (!gSavedSettings.getBOOL("ASVignetteEnabled") ||
        !sVignetteProgram.isComplete() || gCubeSnapshot || width <= 0 || height <= 0 ||
        ASBackgroundIsolate::isActive())
    {
        return;
    }

    const F32 strength = llclamp(gSavedSettings.getF32("ASVignetteStrength"), 0.f, 1.f);
    if (strength <= 0.f)
    {
        return;
    }

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_MULT);

    sVignetteProgram.bind();
    sVignetteProgram.uniform2f(sScreenResolution, (F32)width, (F32)height);
    sVignetteProgram.uniform1f(sStrength, strength);
    sVignetteProgram.uniform1f(sRadius, llclamp(gSavedSettings.getF32("ASVignetteRadius"), 0.f, 10.f));
    sVignetteProgram.uniform1f(sSmoothness, llclamp(gSavedSettings.getF32("ASVignetteSmoothness"), 0.f, 1.f));
    sVignetteProgram.uniform1f(sShape, llclamp(gSavedSettings.getF32("ASVignetteShape"), -1.f, 1.f));

    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);

    sVignetteProgram.unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}
