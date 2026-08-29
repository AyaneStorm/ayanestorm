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
#include "lldrawable.h"
#include "llenvironment.h"
#include "llface.h"
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
#include "llviewerregion.h"
#include "llvertexbuffer.h"
#include "llworld.h"

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
        F32 lateralPhase{ 0.f };
        F32 lateralRate{ 1.f };
        F32 targetDriftX{ 0.f };
        F32 targetDriftY{ 0.f };
        F32 nextDirectionHeight{ 0.f };
        U32 directionStep{ 0 };
        bool collisionValid{ false };
        bool retains{ false };
        ParticleState state{ ParticleState::FALLING };
    };

    LLGLSLShader sRenderProgram;
    LLPointer<LLVertexBuffer> sVertexBuffer;
    std::vector<Particle> sParticles;
    U32 sVisibleVertices = 0;
    bool sLoggedFirstRender = false;
    U32 sCollisionCursor = 0;
    U32 sNearCollisionCursor = 0;
    F32 sAllocatedRadius = 0.f;

    const LLStaticHashedString sLightColor("snow_light_color");
    const LLStaticHashedString sShape("snow_shape");

    F32 distanceAreaScale(const ASWeather::FrameContext& context)
    {
        const F32 relative_radius = context.radius / 32.f;
        return relative_radius * relative_radius;
    }

    U32 particleCount(const ASWeather::FrameContext& context)
    {
        // Preserve flakes per square metre as Distance changes. Shape does not
        // alter density; Intensity selects the active fraction of this reserve.
        return llmax(1u, (U32)ll_round(48000.f * distanceAreaScale(context)));
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

    void chooseNextDirection(Particle& particle, const ASWeather::FrameContext& context)
    {
        U32 state = hashBits((U32)(particle.seed * 16777215.f) ^
                             particle.generation * 0x85ebca6bu ^
                             ++particle.directionStep * 0xc2b2ae35u);
        particle.targetDriftX = context.drift.mV[VX] +
                                (random01(state) - 0.5f) * 0.28f;
        particle.targetDriftY = context.drift.mV[VY] +
                                (random01(state) - 0.5f) * 0.28f;
        particle.nextDirectionHeight = particle.position.mV[VZ] -
                                       (1.75f + random01(state) * 0.50f);
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
        particle.velocity.set(context.drift.mV[VX] + (random01(random_state) - 0.5f) * 0.22f,
                              context.drift.mV[VY] + (random01(random_state) - 0.5f) * 0.22f,
                              -(1.2f + random01(random_state) * 1.4f));
        particle.size = 0.65f + random01(random_state) * 0.70f;
        particle.lateralPhase = random01(random_state) * F_TWO_PI;
        particle.lateralRate = 0.45f + random01(random_state) * 0.85f;
        particle.directionStep = 0;
        particle.targetDriftX = particle.velocity.mV[VX];
        particle.targetDriftY = particle.velocity.mV[VY];
        particle.nextDirectionHeight = particle.position.mV[VZ] - 2.f;
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
        if (te->getAlpha() < 0.999f) return true;
        if (const LLMaterialPtr material = te->getMaterialParams())
        {
            if (material->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
                return true;
        }
        const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial();
        if (gltf && gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND) return true;

        // The rendered face pool already resolves legacy texture alpha and
        // material mode without querying the texture's raw GL format. The
        // latter logs for compressed/modern formats and is unsuitable here.
        if (object->mDrawable.notNull() && face < object->mDrawable->getNumFaces())
        {
            const LLFace* render_face = object->mDrawable->getFace(face);
            return render_face && render_face->isInAlphaPool();
        }
        return false;
    }

    bool sweptSideBlocked(const Particle& particle, LLPipeline& pipeline,
                          bool bidirectional = false)
    {
        if (!particle.collisionValid || particle.state != ParticleState::FALLING ||
            (particle.position - particle.sampledPosition).lengthSquared() < 0.0001f)
        {
            return false;
        }

        auto blocked_between = [&](const LLVector3& from, const LLVector3& to)
        {
            LLVector4a start;
            LLVector4a end;
            start.load3(from.mV);
            end.load3(to.mV);
            for (U32 attempt = 0; attempt < 8; ++attempt)
            {
                S32 face = -1;
                LLVector4a intersection;
                LLVector4a normal;
                LLViewerObject* object = pipeline.lineSegmentIntersectInWorld(
                    start, end, true, false, true, false, &face, nullptr, nullptr,
                    &intersection, nullptr, &normal, nullptr);
                if (!object) return false;
                if (excludedBlocker(object))
                {
                    LLVector3 next(intersection.getF32ptr());
                    LLVector3 direction = to - next;
                    if (direction.normVec() == 0.f) return false;
                    next += direction * 0.01f;
                    start.load3(next.mV);
                    continue;
                }

                return nonRetainingFace(object, face) ||
                       normal.getF32ptr()[VZ] < 0.642787610f;
            }
            return false;
        };

        // Test both directions because thin mesh walls can be one-sided. A
        // retaining near-horizontal hit remains the ordinary landing path.
        return blocked_between(particle.sampledPosition, particle.position) ||
               (bidirectional &&
                blocked_between(particle.position, particle.sampledPosition));
    }

    void sampleTerrainFallback(Particle& particle)
    {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosAgent(particle.position);
        if (!region) return;

        LLVector3 region_position = particle.position - region->getOriginAgent();
        const F32 height = region->getLandHeightRegion(region_position);
        const F32 step = 0.25f;
        LLVector3 east = region_position;
        LLVector3 west = region_position;
        LLVector3 north = region_position;
        LLVector3 south = region_position;
        east.mV[VX] += step;
        west.mV[VX] -= step;
        north.mV[VY] += step;
        south.mV[VY] -= step;
        LLVector3 normal(region->getLandHeightRegion(west) - region->getLandHeightRegion(east),
                         region->getLandHeightRegion(south) - region->getLandHeightRegion(north),
                         step * 2.f);
        normal.normVec();

        particle.blockerHeight = height + region->getOriginAgent().mV[VZ];
        particle.supportId = region->getRegionID();
        particle.retains = normal.mV[VZ] >= 0.642787610f;
    }

    bool sampleOverheadBlocker(Particle& particle,
                               const ASWeather::FrameContext& context,
                               LLPipeline& pipeline)
    {
        LLVector3 start3 = particle.position;
        start3.mV[VZ] += 0.02f;
        LLVector3 end3(particle.position.mV[VX], particle.position.mV[VY], context.top);
        LLVector4a start;
        LLVector4a end;
        start.load3(start3.mV);
        end.load3(end3.mV);
        for (U32 attempt = 0; attempt < 8; ++attempt)
        {
            S32 face = -1;
            LLVector4a intersection;
            LLViewerObject* object = pipeline.lineSegmentIntersectInWorld(
                start, end, true, false, true, false, &face, nullptr, nullptr,
                &intersection, nullptr, nullptr, nullptr);
            if (!object) return false;
            if (excludedBlocker(object))
            {
                LLVector3 next(intersection.getF32ptr());
                next.mV[VZ] += 0.05f;
                start.load3(next.mV);
                continue;
            }
            particle.blockerHeight = intersection.getF32ptr()[VZ];
            particle.supportId = object->getID();
            particle.retains = false;
            return true;
        }
        return false;
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

        if (particle.state == ParticleState::FALLING &&
            sampleOverheadBlocker(particle, context, pipeline))
        {
            return;
        }

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
                sampleTerrainFallback(particle);
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
                               normal.getF32ptr()[VZ] >= 0.642787610f;
            if (validating_landed && particle.supportId != previous_support)
            {
                particle.collisionValid = false;
                particle.retains = false;
            }
            return;
        }
        sampleTerrainFallback(particle);
    }

    void refreshCollisionCache(const ASWeather::FrameContext& context, LLPipeline& pipeline)
    {
        if (sParticles.empty()) return;
        const F32 intensity = llclamp(gSavedSettings.getF32("ASWeatherSnowIntensity"), 0.f, 1.f);
        if (intensity <= 0.f) return;

        const U32 budget = llmax(1u, (U32)ll_round(
            1536.f * distanceAreaScale(context)));
        const U32 active_budget = llmax(1u, (U32)llceil((F32)budget * intensity));
        auto refresh_particle = [&](Particle& particle)
        {
            if (sweptSideBlocked(particle, pipeline))
            {
                recycleParticle(particle, context);
            }
            else
            {
                sampleCollision(particle, context, pipeline);
            }
        };

        // Spend most queries near the camera, where a wall crossing is visible.
        const U32 near_budget = llmax(1u, active_budget * 3u / 4u);
        U32 near_queried = 0;
        U32 examined = 0;
        while (near_queried < near_budget && examined++ < sParticles.size())
        {
            const U32 index = ((sNearCollisionCursor++ % sParticles.size()) * 7919u) %
                              (U32)sParticles.size();
            Particle& particle = sParticles[index];
            const LLVector3 offset = particle.position - context.center;
            if (!activeAtIntensity(particle, intensity) ||
                offset.mV[VX] * offset.mV[VX] + offset.mV[VY] * offset.mV[VY] > 144.f)
            {
                continue;
            }
            refresh_particle(particle);
            ++near_queried;
        }

        U32 queried = near_queried;
        examined = 0;
        while (queried < active_budget && examined++ < sParticles.size())
        {
            // 7919 is coprime with the particle count. This walks the whole
            // buffer without exposing sequential spawn IDs as bands.
            const U32 index = ((sCollisionCursor++ % sParticles.size()) * 7919u) %
                              (U32)sParticles.size();
            Particle& particle = sParticles[index];
            if (!activeAtIntensity(particle, intensity))
            {
                particle.collisionValid = false;
                continue;
            }
            refresh_particle(particle);
            ++queried;
        }
    }

    void sweepIndoorNearby(const ASWeather::FrameContext& context, LLPipeline& pipeline)
    {
        const F32 intensity = llclamp(gSavedSettings.getF32("ASWeatherSnowIntensity"), 0.f, 1.f);
        if (intensity <= 0.f) return;

        Particle camera_probe;
        camera_probe.position = context.center;
        sampleCollision(camera_probe, context, pipeline);
        if (camera_probe.blockerHeight <= context.center.mV[VZ] + 0.10f) return;

        // Indoor leakage is a correctness failure, not a quality tradeoff.
        // This runs after simulation so a flake crossing a nearby wall in the
        // current frame is recycled before geometry is submitted.
        for (Particle& particle : sParticles)
        {
            if (!activeAtIntensity(particle, intensity) ||
                !particle.collisionValid || particle.state != ParticleState::FALLING)
            {
                continue;
            }
            const LLVector3 offset = particle.position - context.center;
            if (offset.lengthSquared() > 256.f) continue;
            if (sweptSideBlocked(particle, pipeline, true))
            {
                recycleParticle(particle, context);
            }
            else
            {
                particle.sampledPosition = particle.position;
            }
        }
    }

    void allocateParticles(const ASWeather::FrameContext& context)
    {
        const U32 count = particleCount(context);
        sParticles.assign(count, Particle());
        for (U32 index = 0; index < count; ++index)
        {
            Particle& particle = sParticles[index];
            particle.seed = hash01((F32)index * 0.754877666f + 0.17f);
            particle.generation = index % 17;
            recycleParticle(particle, context);
            // Pre-distribute initial flakes through the visible height so
            // enabling Snow or walking outdoors does not require a 20-metre
            // fall-in delay. They remain hidden until a valid shelter query;
            // indoor candidates are recycled before geometry submission.
            particle.position.mV[VZ] = context.center.mV[VZ] - 16.f + ll_frand() * 48.f;
            particle.nextDirectionHeight = particle.position.mV[VZ] - 2.f;
        }

        sVertexBuffer = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX |
                                           LLVertexBuffer::MAP_TEXCOORD0 |
                                           LLVertexBuffer::MAP_COLOR);
        if (!sVertexBuffer->allocateBuffer(count * 6, 0))
        {
            LL_WARNS("Weather") << "Unable to allocate native Snow vertex buffer" << LL_ENDL;
            sVertexBuffer = nullptr;
            sParticles.clear();
            return;
        }
        LL_INFOS("Weather") << "Allocated " << count
                            << " CPU Snow particles across a " << context.radius
                            << " metre radius and native billboard vertices" << LL_ENDL;
        sAllocatedRadius = context.radius;
    }

    void simulate(const ASWeather::FrameContext& context)
    {
        if (gSnapshot)
        {
            return;
        }

        const F32 delta = llclamp(gFrameIntervalSeconds.value(), 0.f, 0.1f);
        const F32 speed = llclamp(gSavedSettings.getF32("ASWeatherSnowSpeed"), 0.25f, 2.f);
        const F32 hold = llclamp(gSavedSettings.getF32("ASWeatherSnowLandedHold"), 0.f, 60.f);
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
                if (particle.position.mV[VZ] <= particle.nextDirectionHeight)
                {
                    chooseNextDirection(particle, context);
                }
                const F32 steering = llclamp(delta * 1.8f, 0.f, 1.f);
                particle.velocity.mV[VX] +=
                    (particle.targetDriftX - particle.velocity.mV[VX]) * steering;
                particle.velocity.mV[VY] +=
                    (particle.targetDriftY - particle.velocity.mV[VY]) * steering;
                const F32 meander = sinf(particle.lateralPhase +
                                         particle.timer * particle.lateralRate) * 0.09f;
                particle.position.mV[VX] += (particle.velocity.mV[VX] + meander) * delta;
                particle.position.mV[VY] += (particle.velocity.mV[VY] - meander * 0.65f) * delta;
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
        const F32 hold = llclamp(gSavedSettings.getF32("ASWeatherSnowLandedHold"), 0.f, 60.f);
        const F32 fade = llclamp(gSavedSettings.getF32("ASWeatherSnowLandedFade"), 0.1f, 10.f);
        const F32 size_scale = llclamp(gSavedSettings.getF32("ASWeatherSnowSize"), 0.1f, 2.f);
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
            F32 local_intensity = intensity;
            if (context.radius > context.fullDensityRadius)
            {
                const LLVector3 offset = particle.position - context.center;
                const F32 horizontal_distance = sqrtf(
                    offset.mV[VX] * offset.mV[VX] +
                    offset.mV[VY] * offset.mV[VY]);
                const F32 falloff = llclamp(
                    (context.radius - horizontal_distance) /
                    (context.radius - context.fullDensityRadius), 0.f, 1.f);
                local_intensity *= falloff * falloff * (3.f - 2.f * falloff);
            }
            if (!activeAtIntensity(particle, local_intensity))
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
    sVisibleVertices = 0;
    sLoggedFirstRender = false;
    sCollisionCursor = 0;
    sNearCollisionCursor = 0;
    sAllocatedRadius = 0.f;
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

    const S32 shape = llclamp(gSavedSettings.getS32("ASWeatherSnowShape"), 0, 2);
    if (sParticles.empty() || fabsf(sAllocatedRadius - context.radius) > 0.01f)
    {
        allocateParticles(context);
    }
    if (sParticles.empty())
    {
        return;
    }
    refreshCollisionCache(context, pipeline);
    simulate(context);
    sweepIndoorNearby(context, pipeline);
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
    sRenderProgram.uniform1i(sShape, shape);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    // Exact OIT stores its resolved glow mask in the scene target's alpha
    // channel. Snow is a lit translucent overlay, not emissive geometry, so
    // preserve destination alpha and blend colour only.
    gGL.setColorMask(true, false);
    sVertexBuffer->setBuffer();
    sVertexBuffer->drawArrays(LLRender::TRIANGLES, 0, sVisibleVertices);
    gGL.setColorMask(true, true);
    LLGLSLShader::unbind();
}
