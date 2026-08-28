/**
 * @file aslightrigrenderer.cpp
 * @author chanayane@firestorm
 * @brief See aslightrigrenderer.h
 */

#include "llviewerprecompiledheaders.h"

#include "aslightrigrenderer.h"

#include "llenvironment.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "llviewercontrol.h"
#include "llviewercamera.h"
#include "llvieweroctree.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

// Cube helper is implemented by llvieweroctree.cpp and historically declared
// locally by pipeline.cpp rather than in a shared header.
LLVertexBuffer* ll_create_cube_vb(U32 type_mask);

namespace
{
    std::vector<ASLightRigRenderer::Light> sLights;
    const F32 DEFERRED_RADIUS_SCALE = 1.5f;
    const F32 DEFERRED_FALLOFF_SCALE = 0.5f;
}

bool ASLightRigRenderer::usesShaderBackend()
{
    return gSavedSettings.getString("ASLightRigRenderBackend") == "Shader";
}

void ASLightRigRenderer::setLights(const std::vector<Light>& lights)
{
    sLights = lights;
}

void ASLightRigRenderer::render(LLPipeline& pipeline, F32 light_scale)
{
    if (!usesShaderBackend() || sLights.empty())
    {
        return;
    }

    static LLCachedControl<S32> local_light_count(gSavedSettings, "RenderLocalLightCount", 256);
    if (local_light_count <= 0)
    {
        return;
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const glm::mat4 modelview = get_current_modelview();
    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const S32 count_limit = llmin((S32)sLights.size(), (S32)local_light_count);

    if (pipeline.mCubeVB.isNull())
    {
        pipeline.mCubeVB = ll_create_cube_vb(LLVertexBuffer::MAP_VERTEX);
    }

    std::vector<LLVector4> fullscreen_lights;
    std::vector<LLVector4> fullscreen_colors;

    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    pipeline.bindDeferredShader(gDeferredLightProgram);
    pipeline.mCubeVB->setBuffer();

    for (S32 index = 0; index < count_limit; ++index)
    {
        const Light& light = sLights[index];
        LLVector4a center;
        center.load3(light.position_agent.mV);
        LLVector4a size;
        const F32 radius = light.radius * DEFERRED_RADIUS_SCALE;
        size.splat(radius);

        const LLColor3 color = linearColor3(light.color_srgb) * light.intensity * light_scale;
        if (radius <= 0.001f || color.magVecSquared() < 0.001f ||
            camera->AABBInFrustumNoFarClip(center, size) == 0)
        {
            continue;
        }

        LLPipeline::sVisibleLightCount++;
        const F32* c = center.getF32ptr();
        const LLVector3 camera_pos = camera->getOrigin();
        const bool camera_outside =
            camera_pos.mV[VX] > c[VX] + radius + 0.2f || camera_pos.mV[VX] < c[VX] - radius - 0.2f ||
            camera_pos.mV[VY] > c[VY] + radius + 0.2f || camera_pos.mV[VY] < c[VY] - radius - 0.2f ||
            camera_pos.mV[VZ] > c[VZ] + radius + 0.2f || camera_pos.mV[VZ] < c[VZ] - radius - 0.2f;

        if (camera_outside)
        {
            gDeferredLightProgram.uniform3fv(LLShaderMgr::LIGHT_CENTER, 1, c);
            gDeferredLightProgram.uniform1f(LLShaderMgr::LIGHT_SIZE, radius);
            gDeferredLightProgram.uniform3fv(LLShaderMgr::DIFFUSE_COLOR, 1, color.mV);
            gDeferredLightProgram.uniform1f(LLShaderMgr::LIGHT_FALLOFF,
                                             light.falloff * DEFERRED_FALLOFF_SCALE);
            gDeferredLightProgram.uniform1i(LLShaderMgr::CLASSIC_MODE,
                                             sky->canAutoAdjust() ? 1 : 0);
            gGL.syncMatrices();
            pipeline.mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8,
                                        get_box_fan_indices(camera, center));
        }
        else
        {
            glm::vec3 transformed(center);
            transformed = mul_mat4_vec3(modelview, transformed);
            fullscreen_lights.emplace_back(transformed.x, transformed.y, transformed.z, radius);
            fullscreen_colors.emplace_back(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE],
                                           light.falloff * DEFERRED_FALLOFF_SCALE);
        }
    }
    pipeline.unbindDeferredShader(gDeferredLightProgram);

    LLGLDepthTest fullscreen_depth(GL_FALSE);
    while (!fullscreen_lights.empty())
    {
        const U32 count = llmin((U32)fullscreen_lights.size(), (U32)LL_DEFERRED_MULTI_LIGHT_COUNT);
        F32 far_z = 0.f;
        for (U32 index = 0; index < count; ++index)
        {
            far_z = llmin(fullscreen_lights[index].mV[VZ] - fullscreen_lights[index].mV[VW], far_z);
        }

        LLGLSLShader& shader = gDeferredMultiLightProgram[count - 1];
        pipeline.bindDeferredShader(shader);
        shader.uniform1i(LLShaderMgr::MULTI_LIGHT_COUNT, count);
        shader.uniform4fv(LLShaderMgr::MULTI_LIGHT, count, fullscreen_lights.front().mV);
        shader.uniform4fv(LLShaderMgr::MULTI_LIGHT_COL, count, fullscreen_colors.front().mV);
        shader.uniform1f(LLShaderMgr::MULTI_LIGHT_FAR_Z, far_z);
        shader.uniform1i(LLShaderMgr::CLASSIC_MODE, sky->canAutoAdjust() ? 1 : 0);
        pipeline.mScreenTriangleVB->setBuffer();
        pipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        pipeline.unbindDeferredShader(shader);

        fullscreen_lights.erase(fullscreen_lights.begin(), fullscreen_lights.begin() + count);
        fullscreen_colors.erase(fullscreen_colors.begin(), fullscreen_colors.begin() + count);
    }
}

S32 ASLightRigRenderer::appendForwardLights(LLPipeline& pipeline, S32 first_slot,
                                             F32 light_scale)
{
    if (!usesShaderBackend() || sLights.empty())
    {
        return first_slot;
    }

    S32 slot = first_slot;
    for (const Light& light : sLights)
    {
        if (slot >= 8)
        {
            break;
        }

        LLColor4 color(linearColor3(light.color_srgb) * light.intensity * light_scale, 0.f);
        const F32 adjusted_radius = light.radius * (LLPipeline::sRenderDeferred ? 1.5f : 1.f);
        if (adjusted_radius <= 0.001f || color.magVecSquared() < 0.001f)
        {
            continue;
        }

        // Match LLPipeline::setupHWLights()'s LLVOVolume point-light setup.
        const F32 attenuation_factor = 3.f * (1.f + light.falloff * 2.f);
        const F32 linear_attenuation = attenuation_factor / adjusted_radius;
        const F32 deferred_falloff = light.falloff * DEFERRED_FALLOFF_SCALE;

        pipeline.mLightMovingMask |= (1 << slot);
        pipeline.mHWLightColors[slot] = color;

        LLLightState* state = gGL.getLight(slot);
        state->setPosition(LLVector4(light.position_agent, 1.f));
        state->setDiffuse(color);
        state->setAmbient(LLColor4::black);
        state->setConstantAttenuation(0.f);
        state->setLinearAttenuation(linear_attenuation);
        state->setQuadraticAttenuation(LLPipeline::sRenderDeferred ? deferred_falloff + 1.f : 0.f);
        state->setSize(light.radius * DEFERRED_RADIUS_SCALE);
        state->setFalloff(deferred_falloff);
        state->setSpotExponent(0.f);
        state->setSpotCutoff(180.f);
        state->setSpecular(LLColor4(0.f, 0.f, 1.f, 0.f));
        ++slot;
    }
    return slot;
}
