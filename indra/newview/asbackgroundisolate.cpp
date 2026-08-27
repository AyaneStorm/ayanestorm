/**
 * @file asbackgroundisolate.cpp
 * @author chanayane@firestorm
 * @brief See asbackgroundisolate.h
 */

#include "llviewerprecompiledheaders.h"

#include "asbackgroundisolate.h"

#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"

namespace
{
    LLGLSLShader sBackgroundIsolateProgram;
    const LLStaticHashedString sIsolateColor("isolate_color");

    bool sActive = false;
    LLColor4 sColor(0.f, 0.f, 0.f, 1.f);
}

extern bool gCubeSnapshot;

void ASBackgroundIsolate::setActive(bool active, const LLColor4& color)
{
    sActive = active;
    sColor = color;
}

bool ASBackgroundIsolate::isActive()
{
    return sActive;
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
    sBackgroundIsolateProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth_target, true, LLTexUnit::TFO_POINT);
    sBackgroundIsolateProgram.uniform4fv(sIsolateColor, 1, sColor.mV);

    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);

    sBackgroundIsolateProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    sBackgroundIsolateProgram.unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}
