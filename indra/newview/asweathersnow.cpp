/**
 * @file asweathersnow.cpp
 * @author chanayane@firestorm
 * @brief CPU-stateful shelter-aware snow rendered through LLVertexBuffer.
 */

#include "llviewerprecompiledheaders.h"

#include "asweathersnow.h"

#include "asweather.h"
#include "llappviewer.h"
#include "llcontrol.h"
#include "llenvironment.h"
#include "llgl.h"
#include "llgltfmaterial.h"
#include "llmaterial.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "lltextureentry.h"
#include "pipeline.h"
#include "llviewercontrol.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llvertexbuffer.h"

extern bool gSnapshot;

namespace
{
    enum class ParticleState : U8
    {
        FALLING,
        LANDED
    };

    struct Particle
    {
        LLVector3 position;
        LLVector3 velocity;
        F32 seed{ 0.f };
        F32 size{ 1.f };
        F32 timer{ 0.f };
        U32 generation{ 0 };
        F32 blockerHeight{ 0.f };
        LLVector3 sampledPosition;
        LLUUID supportId;
        bool collisionValid{ false };
        bool retains{ false };
        ParticleState state{ ParticleState::FALLING };
    };

    LLGLSLShader sRenderProgram;
    LLPointer<LLVertexBuffer> sVertexBuffer;
    std::vector<Particle> sParticles;
    S32 sAllocatedQuality = -1;
    U32 sVisibleVertices = 0;
    bool sLoggedFirstRender = false;
    U32 sCollisionCursor = 0;

    const LLStaticHashedString sLightColor("snow_light_color");

    U32 countForQuality(S32 quality)
    {
        (void)quality;
        // Every quality shares one density ceiling. Quality controls shelter
        // refresh work; Intensity selects the active fraction of this reserve.
        return 48000;
    }

    F32 hash01(F32 value)
    {
        return value - floorf(value);
    }

    bool activeAtIntensity(const Particle& particle, F32 intensity)
    {
        return hash01(particle.seed * 97.13f) < intensity;
    }

    U32 hashBits(U32 value)
    {
        // Integer avalanche keeps successive particle IDs and generations
        // from forming correlated polar arcs in the precipitation volume.
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        value ^= value >> 16;
        return value;
    }

    F32 random01(U32& state)
    {
        state = hashBits(state + 0x9e3779b9u);
        return (F32)(state >> 8) * (1.f / 16777216.f);
    }

    void recycleParticle(Particle& particle, const ASWeather::FrameContext& context)
    {
        ++particle.generation;
        U32 random_state = hashBits((U32)(particle.seed * 16777215.f) ^
                                    particle.generation * 0x85ebca6bu);
        const F32 angle = random01(random_state) * F_TWO_PI;
        const F32 distance = sqrtf(random01(random_state)) * context.radius;
        particle.position.set(context.center.mV[VX] + cosf(angle) * distance,
                              context.center.mV[VY] + sinf(angle) * distance,
                              context.center.mV[VZ] + 20.f + random01(random_state) * 12.f);
        particle.velocity.set(context.drift.mV[VX], context.drift.mV[VY],
                              -(1.2f + random01(random_state) * 1.4f));
        particle.size = 0.65f + random01(random_state) * 0.70f;
        particle.timer = 0.f;
        particle.collisionValid = false;
        particle.retains = false;
        particle.supportId.setNull();
        particle.state = ParticleState::FALLING;
    }

    bool excludedBlocker(const LLViewerObject* object)
    {
        if (!object || object->isAvatar() || object->isAttachment()) return true;
        const LLPCode pcode = object->getPCode();
        return pcode == LL_PCODE_LEGACY_TREE || pcode == LL_PCODE_TREE_NEW ||
               pcode == LL_PCODE_LEGACY_GRASS ||
               pcode == LLViewerObject::LL_VO_PART_GROUP;
    }

    bool nonRetainingFace(const LLViewerObject* object, S32 face)
    {
        if (!object || face < 0 || face >= object->getNumTEs()) return false;
        const LLTextureEntry* te = object->getTE((U8)face);
        if (!te) return false;
        if (te->getAlpha() < 0.999f || object->isImageAlphaBlended((U8)face)) return true;
        if (const LLMaterialPtr material = te->getMaterialParams())
        {
            if (material->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
                return true;
        }
        const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial();
        return gltf && gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND;
    }

    void sampleCollision(Particle& particle, const ASWeather::FrameContext& context,
                         LLPipeline& pipeline)
    {
        const LLUUID previous_support = particle.supportId;
        const bool validating_landed = particle.state == ParticleState::LANDED &&
                                       previous_support.notNull();
        LLVector3 start3(particle.position.mV[VX], particle.position.mV[VY], context.top);
        LLVector3 end3(particle.position.mV[VX], particle.position.mV[VY], context.bottom);
        LLVector4a start;
        LLVector4a end;
        start.load3(start3.mV);
        end.load3(end3.mV);
        particle.collisionValid = true;
        particle.blockerHeight = context.bottom;
        particle.retains = false;
        particle.supportId.setNull();
        particle.sampledPosition = particle.position;

        // Skip non-weather geometry by continuing immediately below each hit.
        for (U32 attempt = 0; attempt < 8; ++attempt)
        {
            S32 face = -1;
            LLVector4a intersection;
            LLVector4a normal;
            LLViewerObject* object = pipeline.lineSegmentIntersectInWorld(
                start, end, true, false, true, false, &face, nullptr, nullptr,
                &intersection, nullptr, &normal, nullptr);
            if (!object)
            {
                return;
            }
            if (excludedBlocker(object))
            {
                LLVector3 next(intersection.getF32ptr());
                next.mV[VZ] -= 0.05f;
                start.load3(next.mV);
                continue;
            }

            particle.blockerHeight = intersection.getF32ptr()[VZ];
            particle.supportId = object->getID();
            const LLPCode pcode = object->getPCode();
            const bool water = pcode == LLViewerObject::LL_VO_WATER ||
                               pcode == LLViewerObject::LL_VO_VOID_WATER;
            particle.retains = !water && !nonRetainingFace(object, face) &&
                               normal.getF32ptr()[VZ] >= 0.906307787f;
            if (validating_landed && particle.supportId != previous_support)
            {
                particle.collisionValid = false;
                particle.retains = false;
            }
            return;
        }
    }

    void refreshCollisionCache(const ASWeather::FrameContext& context, LLPipeline& pipeline,
                               S32 quality)
    {
        if (sParticles.empty()) return;
        const F32 intensity = llclamp(gSavedSettings.getF32("ASWeatherSnowIntensity"), 0.f, 1.f);
        if (intensity <= 0.f) return;
        const U32 budget = quality <= 0 ? 192 : quality == 1 ? 384 : 768;
        const U32 active_budget = llmax(1u, (U32)llceil((F32)budget * intensity));
        U32 queried = 0;
        U32 examined = 0;
        while (queried < active_budget && examined++ < sParticles.size())
        {
            // 7919 is coprime with every supported particle count. This walks
            // the whole buffer without exposing sequential spawn IDs as bands.
            const U32 index = ((sCollisionCursor++ % sParticles.size()) * 7919u) %
                              (U32)sParticles.size();
            Particle& particle = sParticles[index];
            if (!activeAtIntensity(particle, intensity))
            {
                particle.collisionValid = false;
                continue;
            }
            sampleCollision(particle, context, pipeline);
            ++queried;
        }
    }

    void allocateParticles(S32 quality, const ASWeather::FrameContext& context)
    {
        const U32 count = countForQuality(quality);
        sParticles.assign(count, Particle());
        for (U32 index = 0; index < count; ++index)
        {
            Particle& particle = sParticles[index];
            particle.seed = hash01((F32)index * 0.754877666f + 0.17f);
            particle.generation = index % 17;
            recycleParticle(particle, context);
            particle.position.mV[VZ] = context.center.mV[VZ] - 16.f + ll_frand() * 48.f;
        }

        sVertexBuffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX |
                                           LLVertexBuffer::MAP_TEXCOORD0 |
                                           LLVertexBuffer::MAP_COLOR);
        if (!sVertexBuffer->allocateBuffer(count * 6, 0))
        {
            LL_WARNS("Weather") << "Unable to allocate native Snow vertex buffer" << LL_ENDL;
            sVertexBuffer = nullptr;
            sParticles.clear();
            sAllocatedQuality = -1;
            return;
        }
        sAllocatedQuality = quality;
        LL_INFOS("Weather") << "Allocated " << count
                            << " CPU Snow particles and native billboard vertices" << LL_ENDL;
    }

    void simulate(const ASWeather::FrameContext& context)
    {
        if (gSnapshot)
        {
            return;
        }

        const F32 delta = llclamp(gFrameIntervalSeconds.value(), 0.f, 0.1f);
        const F32 speed = llclamp(gSavedSettings.getF32("ASWeatherSnowSpeed"), 0.25f, 2.f);
        const F32 hold = llclamp(gSavedSettings.getF32("ASWeatherSnowLandedHold"), 0.f, 30.f);
        const F32 fade = llclamp(gSavedSettings.getF32("ASWeatherSnowLandedFade"), 0.1f, 10.f);
        const F32 intensity = llclamp(gSavedSettings.getF32("ASWeatherSnowIntensity"), 0.f, 1.f);

        for (Particle& particle : sParticles)
        {
            if (!activeAtIntensity(particle, intensity))
            {
                continue;
            }
            particle.timer += delta;
            if (particle.state == ParticleState::FALLING)
            {
                const LLVector3 previous = particle.position;
                particle.velocity.mV[VX] = context.drift.mV[VX];
                particle.velocity.mV[VY] = context.drift.mV[VY];
                particle.position.mV[VX] += particle.velocity.mV[VX] * delta;
                particle.position.mV[VY] += particle.velocity.mV[VY] * delta;
                particle.position.mV[VZ] += particle.velocity.mV[VZ] * speed * delta;

                const LLVector3 offset = particle.position - context.center;
                if (fabsf(offset.mV[VX]) > context.radius ||
                    fabsf(offset.mV[VY]) > context.radius ||
                    particle.position.mV[VZ] < context.center.mV[VZ] - 18.f ||
                    particle.position.mV[VZ] <= context.waterHeight)
                {
                    recycleParticle(particle, context);
                    continue;
                }

                if (particle.collisionValid &&
                    particle.position.mV[VZ] <= particle.blockerHeight)
                {
                    if (previous.mV[VZ] > particle.blockerHeight)
                    {
                        if (particle.retains)
                        {
                            particle.position.mV[VZ] = particle.blockerHeight + 0.012f;
                            particle.state = ParticleState::LANDED;
                            particle.timer = 0.f;
                        }
                        else
                        {
                            recycleParticle(particle, context);
                        }
                    }
                    else
                    {
                        recycleParticle(particle, context);
                    }
                }
            }
            else
            {
                const bool invalid = !particle.collisionValid || !particle.retains ||
                    fabsf(particle.blockerHeight + 0.012f -
                          particle.position.mV[VZ]) > 0.20f;
                if (invalid || particle.timer >= hold + fade)
                {
                    recycleParticle(particle, context);
                }
            }
        }
    }

    bool updateGeometry(const ASWeather::FrameContext& context, LLCamera& camera)
    {
        if (sVertexBuffer.isNull())
        {
            return false;
        }

        LLStrider<LLVector3> vertices;
        LLStrider<LLVector2> texcoords;
        LLStrider<LLColor4U> colors;
        if (!sVertexBuffer->getVertexStrider(vertices) ||
            !sVertexBuffer->getTexCoord0Strider(texcoords) ||
            !sVertexBuffer->getColorStrider(colors))
        {
            sVertexBuffer->unmapBuffer();
            LL_WARNS("Weather") << "Unable to map native Snow vertex buffer" << LL_ENDL;
            return false;
        }

        const F32 intensity = llclamp(gSavedSettings.getF32("ASWeatherSnowIntensity"), 0.f, 1.f);
        const F32 hold = llclamp(gSavedSettings.getF32("ASWeatherSnowLandedHold"), 0.f, 30.f);
        const F32 fade = llclamp(gSavedSettings.getF32("ASWeatherSnowLandedFade"), 0.1f, 10.f);
        const F32 size_scale = llclamp(gSavedSettings.getF32("ASWeatherSnowSize"), 0.5f, 2.f);
        const LLVector3 right = camera.getLeftAxis();
        const LLVector3 up = camera.getUpAxis();
        static const LLVector2 uv[6] = {
            LLVector2(0.f, 0.f), LLVector2(1.f, 0.f), LLVector2(0.f, 1.f),
            LLVector2(0.f, 1.f), LLVector2(1.f, 0.f), LLVector2(1.f, 1.f)
        };
        static const F32 corner_x[6] = { -1.f, 1.f, -1.f, -1.f, 1.f, 1.f };
        static const F32 corner_y[6] = { -1.f, -1.f, 1.f, 1.f, -1.f, 1.f };

        sVisibleVertices = 0;
        for (const Particle& particle : sParticles)
        {
            // Do not display a flake until its vertical shelter query is valid.
            // This prevents indoor leakage while the bounded cache warms up.
            if (!particle.collisionValid)
            {
                continue;
            }
            if (!activeAtIntensity(particle, intensity))
            {
                continue;
            }
            F32 alpha = 1.f;
            if (particle.state == ParticleState::LANDED && particle.timer > hold)
            {
                alpha = llclamp(1.f - (particle.timer - hold) / fade, 0.f, 1.f);
            }
            if (alpha <= 0.f)
            {
                continue;
            }

            const F32 metres = particle.size * size_scale * 0.045f;
            const LLColor4U color(255, 255, 255, (U8)ll_round(alpha * 209.f));
            for (U32 corner = 0; corner < 6; ++corner)
            {
                vertices[sVisibleVertices] = particle.position +
                    right * (corner_x[corner] * metres) +
                    up * (corner_y[corner] * metres);
                texcoords[sVisibleVertices] = uv[corner];
                colors[sVisibleVertices] = color;
                ++sVisibleVertices;
            }
        }
        sVertexBuffer->unmapBuffer();
        return true;
    }
}

void ASWeatherSnow::registerShaders(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sRenderProgram);
}

bool ASWeatherSnow::createShaders(S32 shader_level)
{
    sRenderProgram.mName = "AyaneStorm Native Snow Render";
    sRenderProgram.mShaderFiles.clear();
    sRenderProgram.mShaderFiles.emplace_back("deferred/asweatherSnowRenderV.glsl", GL_VERTEX_SHADER);
    sRenderProgram.mShaderFiles.emplace_back("deferred/asweatherSnowRenderF.glsl", GL_FRAGMENT_SHADER);
    sRenderProgram.mShaderLevel = shader_level;
    return sRenderProgram.createShader();
}

void ASWeatherSnow::unloadShaders()
{
    releaseResources();
    sRenderProgram.unload();
}

void ASWeatherSnow::releaseResources()
{
    sVertexBuffer = nullptr;
    sParticles.clear();
    sAllocatedQuality = -1;
    sVisibleVertices = 0;
    sLoggedFirstRender = false;
    sCollisionCursor = 0;
}

bool ASWeatherSnow::isSupported()
{
    return sRenderProgram.isComplete();
}

void ASWeatherSnow::updateAndRender(const ASWeather::FrameContext& context,
                                    LLPipeline& pipeline, LLCamera& camera)
{
    if (!isSupported())
    {
        return;
    }

    const S32 quality = llclamp(gSavedSettings.getS32("ASWeatherSnowQuality"), 0, 2);
    if (sParticles.empty())
    {
        allocateParticles(quality, context);
    }
    if (sParticles.empty())
    {
        return;
    }
    // Quality only changes the live shelter-query budget; the shared density
    // reserve no longer needs reallocation when the user changes quality.
    sAllocatedQuality = quality;

    refreshCollisionCache(context, pipeline, quality);
    simulate(context);
    if (!updateGeometry(context, camera) || !sVisibleVertices)
    {
        return;
    }

    if (!sLoggedFirstRender)
    {
        LL_INFOS("Weather") << "Beginning first native Snow billboard draw" << LL_ENDL;
        sLoggedFirstRender = true;
    }

    LLColor3 snow_light(0.72f, 0.78f, 0.88f);
    if (const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky())
    {
        const LLColor3 ambient = sky->getAmbientColorClamped();
        const LLColor3 direct = LLEnvironment::instance().getIsSunUp()
            ? sky->getSunlightColorClamped() : sky->getMoonlightColor();
        snow_light = ambient * 0.65f + direct * 0.35f;
        const F32 peak = llmax(snow_light.mV[VX],
                               llmax(snow_light.mV[VY], snow_light.mV[VZ]));
        if (peak > 1.f)
        {
            snow_light *= 1.f / peak;
        }
        snow_light *= LLEnvironment::instance().getIsSunUp() ? 1.f : 0.62f;
    }

    sRenderProgram.bind();
    sRenderProgram.uniform3fv(sLightColor, 1, snow_light.mV);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    sVertexBuffer->setBuffer();
    sVertexBuffer->drawArrays(LLRender::TRIANGLES, 0, sVisibleVertices);
    LLGLSLShader::unbind();
}
