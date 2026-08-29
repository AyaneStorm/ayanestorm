/**
 * @file asweathersnow.cpp
 * @author chanayane@firestorm
 * @brief CPU-stateful shelter-aware snow rendered through LLVertexBuffer.
 */

#include "llviewerprecompiledheaders.h"

#include <chrono>
#include <unordered_map>

#include "asweathersnow.h"

#include "asweather.h"
#include "llappviewer.h"
#include "llcontrol.h"
#include "llenvironment.h"
#include "llgl.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "lltimer.h"
#include "pipeline.h"
#include "llviewercontrol.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
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
    struct VisibleParticle
    {
        U32 index;
        LLColor4U color;
    };
    std::vector<VisibleParticle> sVisibleParticles;
    U32 sVisibleVertices = 0;
    bool sLoggedFirstRender = false;
    U32 sCollisionCursor = 0;
    U32 sNearCollisionCursor = 0;
    U32 sIndoorSweepFrame = 0;
    F32 sAllocatedRadius = 0.f;
    U32 sTimingFrames = 0;
    F64 sCollisionMs = 0.0;
    F64 sSimulationMs = 0.0;
    F64 sIndoorMs = 0.0;
    F64 sGeometryMs = 0.0;
    F64 sDrawMs = 0.0;
    U64 sCollisionCacheHits = 0;
    U64 sCollisionCacheMisses = 0;

    struct CollisionCell
    {
        LLVector3 normal;
        LLVector3 supportPosition;
        LLQuaternion supportRotation;
        LLUUID supportId;
        F32 sampleX{ 0.f };
        F32 sampleY{ 0.f };
        F32 blockerHeight{ 0.f };
        F64 updatedAt{ 0.0 };
        bool retains{ false };
        bool terrainSupport{ false };
        bool trackedSupport{ false };
    };

    std::unordered_map<U64, CollisionCell> sCollisionCells;
    LLVector3 sCollisionCacheCenter;
    bool sCollisionCacheCenterValid = false;
    F64 sCollisionNow = 0.0;

    const LLStaticHashedString sLightColor("snow_light_color");
    const LLStaticHashedString sShape("snow_shape");

    F64 milliseconds(const std::chrono::steady_clock::time_point& begin,
                     const std::chrono::steady_clock::time_point& end)
    {
        return std::chrono::duration<F64, std::milli>(end - begin).count();
    }

    U64 collisionCellKey(const LLVector3& position)
    {
        // Half-metre cells share a surface query while the cached plane normal
        // preserves accurate landing height across sloped geometry.
        const S32 x = (S32)floorf(position.mV[VX] * 2.f);
        const S32 y = (S32)floorf(position.mV[VY] * 2.f);
        return ((U64)(U32)x << 32) | (U32)y;
    }

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
        // Ordered uniform thresholds make Intensity a compact prefix; spawn
        // attributes use an independent integer avalanche, so membership
        // remains spatially decorrelated.
        return particle.seed < intensity;
    }

    U32 activeParticleCount(F32 intensity)
    {
        return llmin((U32)sParticles.size(),
                     (U32)llceil(llclamp(intensity, 0.f, 1.f) *
                                 (F32)sParticles.size()));
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
        // Advance a full-width stream state. A small generation counter makes
        // large particle populations recycle in visible cohorts, especially
        // when landed flakes remain long enough to expose their distribution.
        particle.generation = hashBits(particle.generation + 0x9e3779b9u);
        U32 random_state = particle.generation;
        const F32 angle = random01(random_state) * F_TWO_PI;
        const F32 distance = sqrtf(random01(random_state)) * context.radius;
        particle.position.set(context.center.mV[VX] + cosf(angle) * distance,
                              context.center.mV[VY] + sinf(angle) * distance,
                              context.center.mV[VZ] + 20.f + random01(random_state) * 12.f);
        particle.velocity.set(context.drift.mV[VX] + (random01(random_state) - 0.5f) * 0.22f,
                              context.drift.mV[VY] + (random01(random_state) - 0.5f) * 0.22f,
                              -(1.2f + random01(random_state) * 1.4f));
        particle.size = 0.65f + random01(random_state) * 0.70f;
        particle.directionStep = 0;
        particle.targetDriftX = particle.velocity.mV[VX];
        particle.targetDriftY = particle.velocity.mV[VY];
        particle.nextDirectionHeight = particle.position.mV[VZ] - 2.f;
        particle.sampledPosition = particle.position;
        particle.timer = 0.f;
        particle.collisionValid = false;
        particle.retains = false;
        particle.supportId.setNull();
        particle.state = ParticleState::FALLING;
    }

    bool expensiveDynamicBlocker(const LLViewerObject* object)
    {
        // Avatar picking also includes name-tag geometry and rigged triangle
        // traversal. It cannot participate in the per-flake ray path without
        // a dedicated broad-phase cache.
        return object && object->isAvatar();
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
                if (expensiveDynamicBlocker(object))
                {
                    LLVector3 next(intersection.getF32ptr());
                    LLVector3 direction = to - next;
                    if (direction.normVec() == 0.f) return false;
                    next += direction * 0.01f;
                    start.load3(next.mV);
                    continue;
                }
                return normal.getF32ptr()[VZ] < 0.642787610f;
            }
            return false;
        };

        // Test both directions because thin mesh walls can be one-sided. A
        // retaining near-horizontal hit remains the ordinary landing path.
        return blocked_between(particle.sampledPosition, particle.position) ||
               (bidirectional &&
                blocked_between(particle.position, particle.sampledPosition));
    }

    void sampleTerrainFallback(Particle& particle, LLVector3* blocker_normal = nullptr)
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
        if (blocker_normal) *blocker_normal = normal;
        particle.supportId = region->getRegionID();
        particle.retains = normal.mV[VZ] >= 0.642787610f;
    }

    bool sampleOverheadBlocker(Particle& particle,
                               const ASWeather::FrameContext& context,
                               LLPipeline& pipeline, LLVector3* blocker_normal)
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
            if (expensiveDynamicBlocker(object))
            {
                LLVector3 next(intersection.getF32ptr());
                next.mV[VZ] += 0.05f;
                start.load3(next.mV);
                continue;
            }
            particle.blockerHeight = intersection.getF32ptr()[VZ];
            if (blocker_normal) blocker_normal->set(0.f, 0.f, -1.f);
            particle.supportId = object->getID();
            particle.retains = false;
            return true;
        }
        return false;
    }

    void sampleCollision(Particle& particle, const ASWeather::FrameContext& context,
                         LLPipeline& pipeline, LLVector3* blocker_normal = nullptr)
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
        if (blocker_normal) blocker_normal->set(0.f, 0.f, 1.f);
        particle.retains = false;
        particle.supportId.setNull();

        if (particle.state == ParticleState::FALLING &&
            sampleOverheadBlocker(particle, context, pipeline, blocker_normal))
        {
            return;
        }

        // Every world surface is weather geometry; only water has distinct
        // impact behaviour, and retention otherwise depends solely on slope.
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
                sampleTerrainFallback(particle, blocker_normal);
                return;
            }
            if (expensiveDynamicBlocker(object))
            {
                LLVector3 next(intersection.getF32ptr());
                next.mV[VZ] -= 0.05f;
                start.load3(next.mV);
                continue;
            }
            particle.blockerHeight = intersection.getF32ptr()[VZ];
            if (blocker_normal) blocker_normal->set(normal.getF32ptr());
            particle.supportId = object->getID();
            const LLPCode pcode = object->getPCode();
            const bool water = pcode == LLViewerObject::LL_VO_WATER ||
                               pcode == LLViewerObject::LL_VO_VOID_WATER;
            particle.retains = !water && normal.getF32ptr()[VZ] >= 0.642787610f;
            if (validating_landed && particle.supportId != previous_support)
            {
                particle.collisionValid = false;
                particle.retains = false;
            }
            return;
        }
        sampleTerrainFallback(particle, blocker_normal);
    }

    void prepareCollisionCache(const ASWeather::FrameContext& context)
    {
        sCollisionNow = LLTimer::getTotalSeconds();
        const F32 reset_distance = llmax(16.f, context.radius * 0.5f);
        if (!sCollisionCacheCenterValid ||
            (context.center - sCollisionCacheCenter).lengthSquared() >
                reset_distance * reset_distance ||
            sCollisionCells.size() > 400000)
        {
            sCollisionCells.clear();
            sCollisionCacheCenter = context.center;
            sCollisionCacheCenterValid = true;
        }
    }

    void sampleCollisionCached(Particle& particle,
                               const ASWeather::FrameContext& context,
                               LLPipeline& pipeline)
    {
        const U64 key = collisionCellKey(particle.position);
        auto found = sCollisionCells.find(key);
        bool reusable = found != sCollisionCells.end() &&
                        sCollisionNow - found->second.updatedAt <= 5.0;
        if (reusable && particle.state == ParticleState::LANDED &&
            found->second.supportId.notNull() && !found->second.terrainSupport)
        {
            // Validate a supporting object by identity and transform instead
            // of repeating the detailed surface ray every half-second.
            LLViewerObject* support = found->second.trackedSupport
                ? gObjectList.findObject(found->second.supportId) : nullptr;
            reusable = support &&
                (support->getPositionAgent() - found->second.supportPosition).lengthSquared() <
                    0.000001f &&
                fabsf(dot(support->getRotation(), found->second.supportRotation)) > 0.999999f;
        }
        if (reusable)
        {
            ++sCollisionCacheHits;
            const CollisionCell& cell = found->second;
            const LLUUID previous_support = particle.supportId;
            particle.collisionValid = true;
            particle.blockerHeight = cell.blockerHeight;
            particle.supportId = cell.supportId;
            particle.retains = cell.retains;

            // Reconstruct the local height from the cached surface plane so a
            // half-metre cell does not quantize landed flakes on slopes.
            if (fabsf(cell.normal.mV[VZ]) > 0.05f)
            {
                particle.blockerHeight -=
                    (cell.normal.mV[VX] * (particle.position.mV[VX] - cell.sampleX) +
                     cell.normal.mV[VY] * (particle.position.mV[VY] - cell.sampleY)) /
                    cell.normal.mV[VZ];
            }
            if (particle.state == ParticleState::LANDED &&
                previous_support.notNull() && particle.supportId != previous_support)
            {
                particle.collisionValid = false;
                particle.retains = false;
            }
            return;
        }

        ++sCollisionCacheMisses;
        LLVector3 blocker_normal;
        sampleCollision(particle, context, pipeline, &blocker_normal);
        if (particle.collisionValid)
        {
            CollisionCell& cell = sCollisionCells[key];
            cell.normal = blocker_normal;
            cell.supportId = particle.supportId;
            cell.sampleX = particle.position.mV[VX];
            cell.sampleY = particle.position.mV[VY];
            cell.blockerHeight = particle.blockerHeight;
            cell.updatedAt = sCollisionNow;
            cell.retains = particle.retains;
            cell.terrainSupport = false;
            cell.trackedSupport = false;
            if (particle.supportId.notNull())
            {
                LLViewerRegion* region =
                    LLWorld::getInstance()->getRegionFromPosAgent(particle.position);
                cell.terrainSupport = region && region->getRegionID() == particle.supportId;
                if (!cell.terrainSupport)
                {
                    if (LLViewerObject* support = gObjectList.findObject(particle.supportId))
                    {
                        cell.supportPosition = support->getPositionAgent();
                        cell.supportRotation = support->getRotation();
                        cell.trackedSupport = true;
                    }
                }
            }
        }
    }

    void refreshCollisionCache(const ASWeather::FrameContext& context, LLPipeline& pipeline)
    {
        if (sParticles.empty()) return;
        const F32 intensity = llclamp(gSavedSettings.getF32("ASWeatherSnowIntensity"), 0.f, 1.f);
        if (intensity <= 0.f) return;

        const U32 budget = llmax(1u, (U32)ll_round(
            1536.f * distanceAreaScale(context)));
        const U32 active_budget = llmax(1u, (U32)llceil((F32)budget * intensity));
        const U32 active_count = activeParticleCount(intensity);
        prepareCollisionCache(context);
        auto refresh_particle = [&](Particle& particle)
        {
            // The cached vertical blocker rejects a flake after it enters a
            // roofed cell. Nearby indoor wall crossings retain their stricter
            // per-frame swept test in sweepIndoorNearby().
            sampleCollisionCached(particle, context, pipeline);
        };

        // Spend most queries near the camera, where a wall crossing is visible.
        const U32 near_budget = llmax(1u, active_budget * 3u / 4u);
        U32 near_queried = 0;
        U32 examined = 0;
        while (near_queried < near_budget && examined++ < active_count)
        {
            // Spawn streams are independently hashed, so sequential logical
            // indices remain spatially random without searching inactive data.
            const U32 index = sNearCollisionCursor++ % active_count;
            Particle& particle = sParticles[index];
            const LLVector3 offset = particle.position - context.center;
            if (offset.mV[VX] * offset.mV[VX] + offset.mV[VY] * offset.mV[VY] > 144.f)
            {
                continue;
            }
            refresh_particle(particle);
            ++near_queried;
        }

        U32 queried = near_queried;
        examined = 0;
        while (queried < active_budget && examined++ < active_count)
        {
            const U32 index = sCollisionCursor++ % active_count;
            Particle& particle = sParticles[index];
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
        // The validated four-frame cadence bounds indoor cost while the
        // complete unswept path preserves continuous wall-crossing coverage.
        if (++sIndoorSweepFrame % 4u != 0u) return;
        // Indoor leakage is a correctness failure, not a quality tradeoff.
        // This runs after simulation so a flake crossing a nearby wall in the
        // current frame is recycled before geometry is submitted.
        const U32 active_count = activeParticleCount(intensity);
        for (U32 index = 0; index < active_count; ++index)
        {
            Particle& particle = sParticles[index];
            if (!particle.collisionValid || particle.state != ParticleState::FALLING)
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
        sVisibleParticles.clear();
        sVisibleParticles.reserve(count / 4u);
        sCollisionCells.reserve(llmin(count, 300000u));
        for (U32 index = 0; index < count; ++index)
        {
            Particle& particle = sParticles[index];
            // Ordered thresholds make Intensity an active prefix. Spawn and
            // motion remain spatially independent through the 32-bit stream.
            particle.seed = ((F32)index + 0.5f) / (F32)count;
            // Give every particle an independently avalanched 32-bit stream;
            // seed remains reserved for the stable Intensity selection.
            particle.generation = hashBits(index ^ 0xa511e9b3u);
            recycleParticle(particle, context);
            // Pre-distribute initial flakes through the visible height so
            // enabling Snow or walking outdoors does not require a 20-metre
            // fall-in delay. They remain hidden until a valid shelter query;
            // indoor candidates are recycled before geometry submission.
            particle.position.mV[VZ] = context.center.mV[VZ] - 16.f + ll_frand() * 48.f;
            particle.nextDirectionHeight = particle.position.mV[VZ] - 2.f;
            particle.sampledPosition = particle.position;
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

        const U32 active_count = activeParticleCount(intensity);
        for (U32 index = 0; index < active_count; ++index)
        {
            Particle& particle = sParticles[index];
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

        const F32 intensity = llclamp(gSavedSettings.getF32("ASWeatherSnowIntensity"), 0.f, 1.f);
        const F32 hold = llclamp(gSavedSettings.getF32("ASWeatherSnowLandedHold"), 0.f, 60.f);
        const F32 fade = llclamp(gSavedSettings.getF32("ASWeatherSnowLandedFade"), 0.1f, 10.f);
        const F32 size_scale = llclamp(gSavedSettings.getF32("ASWeatherSnowSize"), 0.1f, 2.f);
        sVisibleParticles.clear();
        const U32 active_count = activeParticleCount(intensity);
        for (U32 index = 0; index < active_count; ++index)
        {
            const Particle& particle = sParticles[index];
            // Do not display a flake until its vertical shelter query is valid.
            // This prevents indoor leakage while the bounded cache warms up.
            if (!particle.collisionValid)
            {
                continue;
            }
            const F32 metres = particle.size * size_scale * 0.045f;
            if (!camera.sphereInFrustum(particle.position, metres * 1.5f))
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

            const bool near_shape =
                (particle.position - camera.getOrigin()).lengthSquared() <= 256.f;
            // RGB was uniformly white. Reuse red as a normalized near/far bit;
            // the fragment shader uses EEP light directly for actual colour.
            const LLColor4U color(near_shape ? 255 : 0, 255, 255,
                                  (U8)ll_round(alpha * 209.f));
            sVisibleParticles.push_back({ index, color });
        }

        const U32 visible_count = (U32)sVisibleParticles.size();
        sVisibleVertices = visible_count * 6u;
        if (!sVisibleVertices)
        {
            return true;
        }

        LLStrider<LLVector3> vertices;
        LLStrider<LLVector2> texcoords;
        LLStrider<LLColor4U> colors;
        if (!sVertexBuffer->getVertexStrider(vertices, 0, sVisibleVertices) ||
            !sVertexBuffer->getTexCoord0Strider(texcoords, 0, sVisibleVertices) ||
            !sVertexBuffer->getColorStrider(colors, 0, sVisibleVertices))
        {
            sVertexBuffer->unmapBuffer();
            LL_WARNS("Weather") << "Unable to map native Snow vertex buffer" << LL_ENDL;
            return false;
        }

        const LLVector3 right = camera.getLeftAxis();
        const LLVector3 up = camera.getUpAxis();
        static const LLVector2 uv[6] = {
            LLVector2(0.f, 0.f), LLVector2(1.f, 0.f), LLVector2(0.f, 1.f),
            LLVector2(0.f, 1.f), LLVector2(1.f, 0.f), LLVector2(1.f, 1.f)
        };
        static const F32 corner_x[6] = { -1.f, 1.f, -1.f, -1.f, 1.f, 1.f };
        static const F32 corner_y[6] = { -1.f, -1.f, 1.f, 1.f, -1.f, 1.f };

        U32 vertex = 0;
        for (U32 visible = 0; visible < visible_count; ++visible)
        {
            const Particle& particle = sParticles[sVisibleParticles[visible].index];
            const F32 metres = particle.size * size_scale * 0.045f;
            const LLColor4U color = sVisibleParticles[visible].color;
            for (U32 corner = 0; corner < 6; ++corner)
            {
                vertices[vertex] = particle.position +
                    right * (corner_x[corner] * metres) +
                    up * (corner_y[corner] * metres);
                texcoords[vertex] = uv[corner];
                colors[vertex] = color;
                ++vertex;
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
    std::vector<VisibleParticle>().swap(sVisibleParticles);
    std::unordered_map<U64, CollisionCell>().swap(sCollisionCells);
    sVisibleVertices = 0;
    sLoggedFirstRender = false;
    sCollisionCursor = 0;
    sNearCollisionCursor = 0;
    sIndoorSweepFrame = 0;
    sAllocatedRadius = 0.f;
    sCollisionCacheCenterValid = false;
    sTimingFrames = 0;
    sCollisionMs = sSimulationMs = sIndoorMs = sGeometryMs = sDrawMs = 0.0;
    sCollisionCacheHits = sCollisionCacheMisses = 0;
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
    if (gSavedSettings.getF32("ASWeatherSnowIntensity") <= 0.f)
    {
        if (!sParticles.empty())
        {
            releaseResources();
        }
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
    const auto collision_begin = std::chrono::steady_clock::now();
    refreshCollisionCache(context, pipeline);
    const auto simulation_begin = std::chrono::steady_clock::now();
    simulate(context);
    const auto indoor_begin = std::chrono::steady_clock::now();
    sweepIndoorNearby(context, pipeline);
    const auto geometry_begin = std::chrono::steady_clock::now();
    const bool geometry_ready = updateGeometry(context, camera);
    const auto geometry_end = std::chrono::steady_clock::now();
    if (!geometry_ready || !sVisibleVertices)
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
    const auto draw_begin = std::chrono::steady_clock::now();
    sVertexBuffer->setBuffer();
    sVertexBuffer->drawArrays(LLRender::TRIANGLES, 0, sVisibleVertices);
    const auto draw_end = std::chrono::steady_clock::now();
    gGL.setColorMask(true, true);
    LLGLSLShader::unbind();

    sCollisionMs += milliseconds(collision_begin, simulation_begin);
    sSimulationMs += milliseconds(simulation_begin, indoor_begin);
    sIndoorMs += milliseconds(indoor_begin, geometry_begin);
    sGeometryMs += milliseconds(geometry_begin, geometry_end);
    sDrawMs += milliseconds(draw_begin, draw_end);
    if (++sTimingFrames >= 120)
    {
        const F64 divisor = 1.0 / (F64)sTimingFrames;
        LL_INFOS("Weather") << "Snow timing average over " << sTimingFrames
                            << " frames: collision=" << sCollisionMs * divisor
                            << "ms simulation=" << sSimulationMs * divisor
                            << "ms indoor=" << sIndoorMs * divisor
                            << "ms geometry/map=" << sGeometryMs * divisor
                            << "ms draw-submit=" << sDrawMs * divisor
                            << "ms particles=" << sParticles.size()
                            << " visible=" << sVisibleVertices / 6
                            << " cache=" << sCollisionCells.size()
                            << " hits=" << sCollisionCacheHits
                            << " misses=" << sCollisionCacheMisses << LL_ENDL;
        sTimingFrames = 0;
        sCollisionMs = sSimulationMs = sIndoorMs = sGeometryMs = sDrawMs = 0.0;
        sCollisionCacheHits = sCollisionCacheMisses = 0;
    }
}
