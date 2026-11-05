/**
 * @file asshaderreload.cpp
 * @brief AyaneStorm: one viewer-wide shader reload per frame at most.
 * @author chanayane@firestorm
 */

#include "llviewerprecompiledheaders.h"

#include "asshaderreload.h"

#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

namespace
{
U32 sRequests = 0;
bool sFlushing = false;
}

namespace ASShaderReload
{
bool defer()
{
    if (sFlushing || !gPipeline.mShadersLoaded)
    {
        return false;
    }
    ++sRequests;
    return true;
}

void flush()
{
    if (!sRequests)
    {
        return;
    }
    // The settings that feed the global shader defines, so a log shows what
    // each reload compiled for (a new combination misses the shader cache).
    LL_INFOS("ShaderLoading") << "Shader reload for " << sRequests << " request(s):"
                              << " shadows " << gSavedSettings.getS32("RenderShadowDetail")
                              << " ssao " << gSavedSettings.getBOOL("RenderDeferredSSAO")
                              << " ssr " << gSavedSettings.getBOOL("RenderScreenSpaceReflections")
                              << " probes " << gSavedSettings.getBOOL("RenderReflectionsEnabled")
                              << "/" << gSavedSettings.getS32("RenderReflectionProbeLevel")
                              << " mirrors " << gSavedSettings.getBOOL("RenderMirrors")
                              << " glow " << gSavedSettings.getBOOL("RenderGlow")
                              << " emissive " << gSavedSettings.getBOOL("RenderEnableEmissiveBuffer")
                              << " hdr " << gSavedSettings.getBOOL("RenderHDREnabled")
                              << LL_ENDL;
    sRequests = 0;
    sFlushing = true;
    LLViewerShaderMgr::instance()->setShaders();
    sFlushing = false;
}
}
