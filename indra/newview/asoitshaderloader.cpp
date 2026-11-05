/**
 * @file asoitshaderloader.cpp
 * @brief AyaneStorm frame-budgeted loading of the OIT renderers' shaders.
 * @author chanayane@firestorm
 */

#include "llviewerprecompiledheaders.h"

#include "asoitshaderloader.h"

#include <functional>
#include <vector>

#include "asavboit.h"
#include "asexactoit.h"
#include "asmacoit.h"
#include "lltimer.h"
#include "pipeline.h"

namespace
{
enum Family : U32
{
    FAMILY_EXACT_OIT,
    FAMILY_AVBOIT,
    FAMILY_MAC_OIT,
    FAMILY_COUNT
};

enum class State
{
    UNLOADED,
    LOADING,
    READY,
    FAILED
};

struct Job
{
    State state = State::UNLOADED;
    std::vector<std::function<bool()>> steps;
    size_t next = 0;
};

// Compile time spent per frame once the frame's first step has run. One step
// is one program or one static/rigged pair: milliseconds from the shader
// cache, up to ~1 s for the largest AVBOIT material programs on a miss.
constexpr F64 FRAME_BUDGET_SECONDS = 0.02;

Job sJobs[FAMILY_COUNT];
bool sParametersValid = false;
S32 sShaderLevel = 0;
bool sUseSunShadow = false;
bool sGLTFEnabled = false;

// User intent AND hardware support; Mac OIT also serves Live DoF's
// transparency bins in every transparency mode.
bool wanted(U32 family)
{
    switch (family)
    {
    case FAMILY_EXACT_OIT: return ASExactOIT::isEnabled();
    case FAMILY_AVBOIT: return ASAVBOIT::requested();
    case FAMILY_MAC_OIT: return ASMacOIT::shadersWanted();
    default: return false;
    }
}

void unloadFamily(U32 family)
{
    switch (family)
    {
    case FAMILY_EXACT_OIT: ASExactOIT::unloadShaders(); break;
    case FAMILY_AVBOIT: ASAVBOIT::unloadShaders(); break;
    case FAMILY_MAC_OIT: ASMacOIT::unloadShaders(); break;
    default: break;
    }
}

void finish(U32 family, bool success)
{
    switch (family)
    {
    case FAMILY_EXACT_OIT: ASExactOIT::finishLoad(success); break;
    case FAMILY_AVBOIT: ASAVBOIT::finishLoad(success); break;
    case FAMILY_MAC_OIT: ASMacOIT::finishLoad(success); break;
    default: break;
    }
    Job& job = sJobs[family];
    job.state = success ? State::READY : State::FAILED;
    job.steps.clear();
    job.next = 0;
}

void start(U32 family)
{
    Job& job = sJobs[family];
    job.steps.clear();
    job.next = 0;
    switch (family)
    {
    case FAMILY_EXACT_OIT:
        ASExactOIT::appendLoadSteps(sShaderLevel, sUseSunShadow, sGLTFEnabled, job.steps);
        break;
    case FAMILY_AVBOIT:
        ASAVBOIT::appendLoadSteps(sShaderLevel, job.steps);
        break;
    case FAMILY_MAC_OIT:
        ASMacOIT::appendLoadSteps(sShaderLevel, job.steps);
        break;
    default:
        break;
    }
    job.state = State::LOADING;
    if (job.steps.empty())
    {
        finish(family, true);
    }
}

void cancel(U32 family)
{
    unloadFamily(family);
    Job& job = sJobs[family];
    job.state = State::UNLOADED;
    job.steps.clear();
    job.next = 0;
}
}

void ASOITShaderLoader::reset(S32 shader_level, bool use_sun_shadow, bool gltf_enabled)
{
    for (Job& job : sJobs)
    {
        job = Job();
    }
    sShaderLevel = shader_level;
    sUseSunShadow = use_sun_shadow;
    sGLTFEnabled = gltf_enabled;
    sParametersValid = true;
}

void ASOITShaderLoader::invalidate()
{
    for (Job& job : sJobs)
    {
        job = Job();
    }
    sParametersValid = false;
}

void ASOITShaderLoader::update()
{
    // The capture programs are cloned from the vanilla deferred programs, so
    // nothing may compile before a successful viewer-wide load.
    if (!sParametersValid || !gPipeline.mShadersLoaded)
    {
        return;
    }

    for (U32 family = 0; family < FAMILY_COUNT; ++family)
    {
        const bool needed = wanted(family);
        if (needed && sJobs[family].state == State::UNLOADED)
        {
            start(family);
        }
        else if (!needed && sJobs[family].state == State::LOADING)
        {
            cancel(family);
        }
        // A family no longer needed once READY stays loaded, so switching back
        // to it is instant.
    }

    LLTimer timer;
    bool stepped = false;
    for (U32 family = 0; family < FAMILY_COUNT; ++family)
    {
        Job& job = sJobs[family];
        while (job.state == State::LOADING &&
               (!stepped || timer.getElapsedTimeF64() < FRAME_BUDGET_SECONDS))
        {
            stepped = true;
            const bool success = job.steps[job.next++]();
            if (!success || job.next == job.steps.size())
            {
                finish(family, success);
            }
        }
    }

    if (stepped)
    {
        // Compiling binds programs; leave no capture program bound for the
        // frame that follows.
        LLGLSLShader::unbind();
    }
}

bool ASOITShaderLoader::loading()
{
    for (const Job& job : sJobs)
    {
        if (job.state == State::LOADING)
        {
            return true;
        }
    }
    return false;
}
