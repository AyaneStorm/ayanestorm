/**
 * @file asdiffuseglow.cpp
 * @author chanayane@firestorm
 * @brief Optional bright-surface bloom controls for the existing glow pass.
 */

#include "llviewerprecompiledheaders.h"

#include "asdiffuseglow.h"

#include "llcontrol.h"
#include "llglslshader.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

namespace
{
    const LLStaticHashedString sThreshold("asDiffuseGlowThreshold");
    const LLStaticHashedString sSoftness("asDiffuseGlowSoftness");
    const LLStaticHashedString sStrength("asDiffuseGlowStrength");
}

void ASDiffuseGlow::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASDiffuseGlow.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            const std::string name = data.asString();
            if (name == "ASDiffuseGlowThreshold" || name == "ASDiffuseGlowSoftness" ||
                name == "ASDiffuseGlowStrength")
            {
                if (LLControlVariable* control = gSavedSettings.getControl(name))
                {
                    control->resetToDefault(true);
                }
            }
        });
}

void ASDiffuseGlow::appendShader(LLGLSLShader& shader)
{
    shader.mShaderFiles.emplace_back("effects/asDiffuseGlowF.glsl", GL_FRAGMENT_SHADER);
}

void ASDiffuseGlow::bindExtractionUniforms(LLGLSLShader& shader)
{
    const bool enabled = gSavedSettings.getBOOL("ASDiffuseGlowEnabled");
    shader.uniform1f(sThreshold, llclamp(gSavedSettings.getF32("ASDiffuseGlowThreshold"), 0.f, 1.f));
    shader.uniform1f(sSoftness, llclamp(gSavedSettings.getF32("ASDiffuseGlowSoftness"), 0.f, 1.f));
    shader.uniform1f(sStrength,
                     enabled ? llclamp(gSavedSettings.getF32("ASDiffuseGlowStrength"), 0.f, 1.f) : 0.f);
}
