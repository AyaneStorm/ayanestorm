/**
 * @file asrviewer.cpp
 * @brief See asrviewer.h.
 */
#include "llviewerprecompiledheaders.h"

#include "asrviewer.h"

#include "asrgltable.h"
#include "llappviewer.h"
#include "llcoord.h"
#include "lldir.h"
#include "llviewercontrol.h"
#include "llwindow.h"

// The Mac and Linux windows are still to do, so those builds stay on OpenGL.
#if defined(AS_RENDERER) && LL_WINDOWS
#define ASR_VIEWER_SUPPORTED 1
#include "asrdevice.h"
#include "asrrenderthread.h"
#include "llwin32headers.h"
#else
#define ASR_VIEWER_SUPPORTED 0
#endif

#include <memory>

namespace
{

constexpr char BACKEND_SETTING[] = "ASRendererBackend";
constexpr char BACKEND_OPENGL[] = "OpenGL";
constexpr char BACKEND_NATIVE[] = "Native";

bool sFellBack = false;

#if ASR_VIEWER_SUPPORTED

std::unique_ptr<asr::Device> sDevice;
std::unique_ptr<asr::RenderThread> sRenderThread;
void* sPlatformWindow = nullptr;

// A context would mean a GL path was missed. Loaded GL libraries prove nothing:
// the Vulkan driver loads them too.
void logGLContextCheck(const char* when)
{
    if (wglGetCurrentContext())
    {
        LL_WARNS("ASRenderer") << "a GL context exists " << when << LL_ENDL;
    }
    else
    {
        LL_INFOS("ASRenderer") << "no GL context " << when << LL_ENDL;
    }
}

// No GL context exists in this session, so OpenGL cannot take over now. The next
// launch uses it instead.
[[noreturn]] void failAndQuit(const std::string& reason)
{
    LL_WARNS("ASRenderer") << "stopping: " << reason << LL_ENDL;
    gSavedSettings.setString(BACKEND_SETTING, BACKEND_OPENGL);
    gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
    OSMessageBox("The new renderer stopped: " + reason + "\nThe next launch will use OpenGL.", "AyaneStorm", OSMB_OK);
    LLAppViewer::instance()->fastQuit(1);
    std::abort(); // fastQuit does not return
}

std::string startRenderer(LLWindow* window)
{
    asr::SurfaceTarget target;
    target.nativeWindow = window->getPlatformWindow();
    target.nativeInstance = GetModuleHandle(nullptr);

    asr::DeviceOptions options;
    options.shaderDir = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders_native");
    options.validation = gSavedSettings.getBOOL("ASRendererValidation");
    options.vsync = gSavedSettings.getBOOL("RenderVSyncEnable");
    options.memoryCapBytes = U64(gSavedSettings.getU32("ASRendererVRAMBudgetMB")) * 1024 * 1024;

    std::string error;
    sDevice = asr::Device::create(target, options, error);
    if (!sDevice)
    {
        return error;
    }

    sPlatformWindow = target.nativeWindow;
    sRenderThread = std::make_unique<asr::RenderThread>(*sDevice);
    sRenderThread->start();
    LL_INFOS("ASRenderer") << "running on " << sDevice->info().backend << ", " << sDevice->info().name << ", budget "
                           << sDevice->memoryBudget().budget() / (1024 * 1024) << " MB" << LL_ENDL;
    return std::string();
}

void stopRenderer()
{
    if (sRenderThread)
    {
        sRenderThread->stop();
        const asr::FrameStats stats = sRenderThread->takeStats();
        LL_INFOS("ASRenderer") << "stopped after " << stats.frames << " frames, worst frame "
                               << stats.worstFrameSeconds * 1000.0 << " ms, oldest state "
                               << stats.worstStateAgeSeconds * 1000.0 << " ms, "
                               << sDevice->validationErrors() << " validation errors" << LL_ENDL;
    }
    sRenderThread.reset();
    sDevice.reset();
    sPlatformWindow = nullptr;
}

#endif // ASR_VIEWER_SUPPORTED

} // namespace

void ASRViewer::selectBackend()
{
    const std::string backend = gSavedSettings.getString(BACKEND_SETTING);
    if (backend != BACKEND_NATIVE)
    {
        if (backend != BACKEND_OPENGL)
        {
            LL_WARNS("ASRenderer") << "unknown " << BACKEND_SETTING << " '" << backend << "', using OpenGL" << LL_ENDL;
        }
        return;
    }

#if ASR_VIEWER_SUPPORTED
    const asr::ProbeResult probe = asr::Device::probe();
    if (probe.supported)
    {
        LL_INFOS("ASRenderer") << "probe accepts " << probe.info.name << LL_ENDL;
        ASR::setActive(probe.info.vendor, probe.info.name);
        return;
    }
    LL_WARNS("ASRenderer") << "using OpenGL: " << probe.reason << LL_ENDL;
#else
    LL_WARNS("ASRenderer") << "using OpenGL: this build has no new renderer" << LL_ENDL;
#endif
    sFellBack = true;
}

void ASRViewer::start(LLWindow* window)
{
#if ASR_VIEWER_SUPPORTED
    if (ASR::active())
    {
        logGLContextCheck("after creating the window");
        const std::string error = startRenderer(window);
        if (!error.empty())
        {
            failAndQuit(error);
        }
        publishFrame(window);
    }
#endif
}

void ASRViewer::publishFrame(LLWindow* window)
{
#if ASR_VIEWER_SUPPORTED
    if (!ASR::active() || !sRenderThread)
    {
        return;
    }
    if (sRenderThread->failed())
    {
        failAndQuit(sRenderThread->error());
    }

    // Going fullscreen or back makes a new native window, which needs a new
    // surface. This is the only time the main thread waits on the renderer.
    if (window->getPlatformWindow() != sPlatformWindow)
    {
        stopRenderer();
        const std::string error = startRenderer(window);
        if (!error.empty())
        {
            failAndQuit(error);
        }
    }

    LLCoordWindow size;
    window->getSize(&size);
    asr::FrameState& state = sRenderThread->state();
    state.width = U32(llmax(size.mX, 0));
    state.height = U32(llmax(size.mY, 0));
    sRenderThread->publishState();
#endif
}

void ASRViewer::stop()
{
#if ASR_VIEWER_SUPPORTED
    if (ASR::active())
    {
        logGLContextCheck("at shutdown");
        stopRenderer();
    }
#endif
}

bool ASRViewer::fellBackToOpenGL()
{
    return sFellBack;
}
