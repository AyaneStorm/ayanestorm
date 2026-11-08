/**
 * @file asrviewer.cpp
 * @brief See asrviewer.h.
 */
#include "llviewerprecompiledheaders.h"

#include "asrviewer.h"

#include "asrbridge.h"
#include "asrcameraturn.h"
#include "asrgltable.h"
#include "asrworld.h"
#include "llappviewer.h"
#include "llcoord.h"
#include "lldir.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llwindow.h"

// The Linux window is still to do, so that build stays on OpenGL.
#if defined(AS_RENDERER) && (LL_WINDOWS || LL_DARWIN)
#define ASR_VIEWER_SUPPORTED 1
#include "asrdevice.h"
#include "asrrenderthread.h"
#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <OpenGL/OpenGL.h>
#endif
#else
#define ASR_VIEWER_SUPPORTED 0
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace
{

constexpr char BACKEND_SETTING[] = "ASRendererBackend";
constexpr char BACKEND_OPENGL[] = "OpenGL";
constexpr char BACKEND_NATIVE[] = "Native";

bool sFellBack = false;

#if ASR_VIEWER_SUPPORTED

using Clock = std::chrono::steady_clock;

constexpr char STALL_SETTING[] = "ASRendererStallTestAt";
constexpr F64 STALL_SECONDS = 5.0; // well short of the 30 s that would log the agent out
constexpr auto STATS_PERIOD = std::chrono::seconds(60);

std::unique_ptr<asr::Device> sDevice;
std::unique_ptr<asr::RenderThread> sRenderThread;
Clock::time_point sStarted;
Clock::time_point sStatsLogged;
Clock::time_point sNextUI;
U32 sWidth = 0;
U32 sHeight = 0;
U32 sUIWidth = 0; // the size of the newest UI list
U32 sUIHeight = 0;
asr::SceneCamera3D sCamera{};
asr::CameraTurn sCameraTurn{};
bool sHasCamera = false;
Clock::time_point sShownAt{};   // see takeShownFrames()
Clock::time_point sRateStart{};
uint64_t sRateFrames = 0;
F64 sShownRate = 0.0;

F64 secondsSince(Clock::time_point start)
{
    return std::chrono::duration<F64>(Clock::now() - start).count();
}

// True once, when the seconds in this setting have passed since the renderer
// started. Setting it later than that, in a running viewer, fires it at once.
bool dueOnce(LLCachedControl<F32>& at, const char* setting)
{
    if (at() <= 0.f || secondsSince(sStarted) < at())
    {
        return false;
    }
    gSavedSettings.setF32(setting, 0.f);
    return true;
}

void logFrameStats(const char* what, const asr::FrameStats& stats)
{
    LL_INFOS("ASRenderer") << what << ": " << stats.frames << " frames, worst frame " << stats.worstFrameSeconds * 1000.0
                           << " ms, oldest state " << stats.worstStateAgeSeconds * 1000.0 << " ms, average state " << stats.averageStateAgeSeconds * 1000.0
                           << " ms, frames with a new state " << stats.newStates << ", frames the mouse turned ahead of the main thread "
                           << stats.turnedFrames << " of " << stats.activeTurnFrames << " with an active turn, frames a zoom eased on " << stats.zoomedFrames << ", of which with clicks not handled yet "
                           << stats.clickFrames << LL_ENDL;
    const asr::FrameParts& parts = stats.worstFrameParts;
    const double recordParts = parts.retireTextures + parts.textureCommands + parts.readResults + parts.sceneUpdate + parts.sceneGrowth + parts.sceneStaging;
    // Temporary through step d: finds what the render thread's record spikes are.
    LL_INFOS("ASRenderer") << "worst frame, ms: wait for GPU " << parts.waitForGpu * 1000.0 << ", acquire " << parts.acquire * 1000.0 << ", UI " << parts.prepareUI * 1000.0
                           << ", record " << parts.record * 1000.0 << " of which retire " << parts.texturesRetired << " textures " << parts.retireTextures * 1000.0
                           << ", texture commands " << parts.textureCommands * 1000.0 << " (" << parts.texturesCreated << " created, " << parts.textureUploadBytes / 1024
                           << " KB uploaded, " << parts.textureCommandsLeft << " left for later), read results " << parts.readResults * 1000.0 << ", scene update " << parts.sceneUpdate * 1000.0 << ", scene growth "
                           << parts.sceneGrowth * 1000.0 << ", scene staging " << parts.sceneStaging * 1000.0 << " (" << parts.sceneStagingBytes / 1024
                           << " KB), the rest " << std::max(0.0, parts.record - recordParts) * 1000.0 << ", submit " << parts.submit * 1000.0
                           << ", present " << parts.present * 1000.0 << "; worst memory refresh " << stats.worstRefreshSeconds * 1000.0
                           << "; uploads waiting for room in the ring now " << (sDevice ? sDevice->textures().waitingBytes() / 1024 : 0) << " KB" << LL_ENDL;
}

// The P2 gate: a stopped main thread must not reach the render rate.
void runStallTest()
{
    logFrameStats("before the stall", sRenderThread->takeStats());
    const Clock::time_point start = Clock::now();
    ms_sleep(U32(STALL_SECONDS * 1000.0));
    const F64 seconds = secondsSince(start);
    const asr::FrameStats stats = sRenderThread->takeStats();
    logFrameStats("during the stall", stats);
    LL_INFOS("ASRenderer") << "main thread stopped for " << seconds << " s, render rate " << stats.frames / seconds << " fps" << LL_ENDL;
}

U32 sourcePixelBytes(ASR::PixelLayout layout)
{
    switch (layout)
    {
        case ASR::PixelLayout::RGBA:
        case ASR::PixelLayout::BGRA:           return 4;
        case ASR::PixelLayout::RGB:
        case ASR::PixelLayout::BGR:            return 3;
        case ASR::PixelLayout::RG:
        case ASR::PixelLayout::LuminanceAlpha: return 2;
        default:                               return 1;
    }
}

asr::TextureDesc textureDesc(U32 width, U32 height, U32 mipLevels, ASR::PixelLayout layout)
{
    asr::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.mipLevels = mipLevels;
    switch (layout)
    {
        case ASR::PixelLayout::RGBA:
        case ASR::PixelLayout::RGB:
            desc.format = asr::TextureFormat::RGBA8;
            break;
        case ASR::PixelLayout::BGRA:
        case ASR::PixelLayout::BGR:
            desc.format = asr::TextureFormat::BGRA8;
            break;
        case ASR::PixelLayout::Red:
            desc.format = asr::TextureFormat::R8;
            break;
        case ASR::PixelLayout::RG:
            desc.format = asr::TextureFormat::RG8;
            break;
        case ASR::PixelLayout::Alpha:
            desc.format = asr::TextureFormat::R8;
            desc.swizzle = asr::TextureSwizzle::AlphaOnly;
            break;
        case ASR::PixelLayout::Luminance:
            desc.format = asr::TextureFormat::R8;
            desc.swizzle = asr::TextureSwizzle::Luminance;
            break;
        case ASR::PixelLayout::LuminanceAlpha:
            desc.format = asr::TextureFormat::RG8;
            desc.swizzle = asr::TextureSwizzle::LuminanceAlpha;
            break;
    }
    return desc;
}

class ViewerLink final : public ASR::RendererLink
{
public:
    U32 createTexture(U32 width, U32 height, U32 mipLevels, ASR::PixelLayout layout) override
    {
        return sDevice->textures().create(textureDesc(width, height, mipLevels, layout));
    }

    void uploadTexture(U32 slot, U32 x, U32 y, U32 width, U32 height, ASR::PixelLayout layout, const void* pixels, U32 rowPixels) override
    {
        // GPUs have no three-channel format, so those rows gain an opaque alpha.
        const U32 sourceBytes = sourcePixelBytes(layout);
        const U32 targetBytes = sourceBytes == 3 ? 4 : sourceBytes;
        const size_t sourceRow = size_t(rowPixels) * sourceBytes;
        const size_t targetRow = size_t(width) * targetBytes;

        // Written once, usually straight into the memory the GPU copies from.
        asr::TextureQueue& textures = sDevice->textures();
        U8* target = textures.beginUpload(slot, 0, x, y, width, height, U32(targetRow));
        const U8* source = static_cast<const U8*>(pixels);
        for (U32 row = 0; target && row < height; ++row)
        {
            if (sourceBytes == targetBytes)
            {
                memcpy(target, source, targetRow);
            }
            else
            {
                for (U32 column = 0; column < width; ++column)
                {
                    target[column * 4] = source[column * 3];
                    target[column * 4 + 1] = source[column * 3 + 1];
                    target[column * 4 + 2] = source[column * 3 + 2];
                    target[column * 4 + 3] = 255;
                }
            }
            source += sourceRow;
            target += targetRow;
        }
        textures.finishUpload();
    }

    void generateMips(U32 slot) override
    {
        sDevice->textures().generateMips(slot);
    }

    U32 createSmallerTexture(U32 sourceSlot, U32 sourceMip, U32 width, U32 height, U32 mipLevels, ASR::PixelLayout layout) override
    {
        return sDevice->textures().create(textureDesc(width, height, mipLevels, layout), sourceSlot, sourceMip);
    }

    void destroyTexture(U32 slot) override
    {
        sDevice->textures().destroy(slot, sRenderThread->currentUIGeneration());
    }

    void getTextureMemory(U64& used, U64& limit) override
    {
        const asr::MemoryBudget& budget = sDevice->memoryBudget();
        used = budget.used(asr::MemoryCategory::Textures);
        limit = budget.textureLimit();
    }

    U32 freeTextureSlots() override
    {
        return sDevice->textures().freeSlots();
    }

    void beginUI() override
    {
        mList = &sRenderThread->ui();
        mList->clear();
        mList->setSize(sWidth, sHeight);
    }

    void setUIBlend(const U8 factors[4]) override
    {
        asr::UIBlend blend;
        blend.colorSource = asr::UIBlendFactor(factors[0]);
        blend.colorDest = asr::UIBlendFactor(factors[1]);
        blend.alphaSource = asr::UIBlendFactor(factors[2]);
        blend.alphaDest = asr::UIBlendFactor(factors[3]);
        mList->setBlend(blend);
    }

    void setUIScissor(bool enabled, S32 x, S32 y, S32 width, S32 height) override
    {
        asr::UIScissor scissor;
        scissor.enabled = enabled;
        scissor.x = x;
        scissor.y = y;
        scissor.width = width;
        scissor.height = height;
        mList->setScissor(scissor);
    }

    void drawUI(const ASR::UIDrawData& data) override
    {
        asr::UIStreams streams;
        streams.positions = data.positions;
        streams.uvs = data.uvs;
        streams.colors = data.colors;
        streams.matrix = data.matrix;
        streams.uvMatrix = data.uvMatrix;
        streams.texture = asr::uiTextureBits(data.slot, asr::UISampler(data.sampler), data.solidColor);
        mList->append(asr::UIGeometry(data.mode), streams, data.count);
    }

    void publishUI() override
    {
        sRenderThread->publishUI();
        mList = nullptr;
    }

private:
    asr::UIList* mList = nullptr;
};

ViewerLink sViewerLink;

// A context would mean a GL path was missed. Loaded GL libraries prove nothing:
// the Vulkan driver loads them too.
void logGLContextCheck(const char* when)
{
#if LL_WINDOWS
    const bool hasContext = wglGetCurrentContext() != nullptr;
#else
    const bool hasContext = CGLGetCurrentContext() != nullptr;
#endif
    if (hasContext)
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
    target.nativeWindow = window->getRendererSurface();
#if LL_WINDOWS
    target.nativeInstance = GetModuleHandle(nullptr);
#endif
    if (!target.nativeWindow)
    {
        return "the window has no surface for the renderer";
    }

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

    sRenderThread = std::make_unique<asr::RenderThread>(*sDevice, ASRCameraTurn::motion());
    sRenderThread->start();
    ASRCameraTurn::start(window);
    sStarted = Clock::now();
    sStatsLogged = sStarted;
    ASR::setRendererLink(&sViewerLink);
    ASRWorld::start();
    LL_INFOS("ASRenderer") << "running on " << sDevice->info().backend << ", " << sDevice->info().name << ", budget "
                           << sDevice->memoryBudget().budget() / (1024 * 1024) << " MB" << LL_ENDL;
    return std::string();
}

void stopRenderer()
{
    ASRCameraTurn::stop();
    ASRWorld::stop();
    ASR::setRendererLink(nullptr);
    if (sRenderThread)
    {
        sRenderThread->stop();
        logFrameStats("last period", sRenderThread->takeStats());
        LL_INFOS("ASRenderer") << "stopped with " << sDevice->validationErrors() << " validation errors" << LL_ENDL;
    }
    sRenderThread.reset();
    sDevice.reset();
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

    LLCoordWindow size;
    window->getSize(&size);
    sWidth = U32(llmax(size.mX, 0));
    sHeight = U32(llmax(size.mY, 0));
    asr::FrameState& state = sRenderThread->state();
    state.width = sWidth;
    state.height = sHeight;
    state.hasCamera = sHasCamera;
    state.camera = sCamera;
    state.turn = sCameraTurn;
    static LLCachedControl<bool> predict_camera(gSavedSettings, "ASRendererCameraPrediction", true);
    state.predictCamera = predict_camera;
    static LLCachedControl<bool> limit_framerate(gSavedSettings, "FSLimitFramerate", false);
    static LLCachedControl<U32> max_fps(gSavedSettings, "FramePerSecondLimit", 0);
    state.maxFramesPerSecond = limit_framerate() ? F32(max_fps()) : 0.f;
    sRenderThread->publishState();
    sDevice->textures().flush(); // uploads that found the ring full

    if (Clock::now() - sStatsLogged >= STATS_PERIOD)
    {
        sStatsLogged = Clock::now();
        logFrameStats("last minute", sRenderThread->takeStats());
        ASRCameraTurn::logCounts();
    }

    static LLCachedControl<F32> stall_at(gSavedSettings, STALL_SETTING, 0.f);
    if (dueOnce(stall_at, STALL_SETTING))
    {
        runStallTest();
    }
#endif
}

bool ASRViewer::beginUI()
{
#if ASR_VIEWER_SUPPORTED
    static LLCachedControl<U32> ui_hz(gSavedSettings, "ASRendererUIHz", 60);
    // A list of the wrong size leaves part of the window black, so it does not wait.
    const bool resized = sWidth != sUIWidth || sHeight != sUIHeight;
    if (ASR::active() && ui_hz() != 0 && !resized)
    {
        const Clock::time_point now = Clock::now();
        if (now < sNextUI)
        {
            return false;
        }
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<F64>(1.0 / ui_hz()));
        sNextUI = std::max(sNextUI + period, now);
    }
    sUIWidth = sWidth;
    sUIHeight = sHeight;
#endif
    ASR::beginUI();
    return true;
}

void ASRViewer::publishUI()
{
    ASR::endUI();
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

asr::Device* ASRViewer::device()
{
#if ASR_VIEWER_SUPPORTED
    return sDevice.get();
#else
    return nullptr;
#endif
}

void ASRViewer::setCamera(const asr::SceneCamera3D& camera, const asr::CameraTurn& turn)
{
#if ASR_VIEWER_SUPPORTED
    sCamera = camera;
    sCameraTurn = turn;
    sHasCamera = true;
#endif
}

F64 ASRViewer::takeShownFrames()
{
#if ASR_VIEWER_SUPPORTED
    if (ASR::active() && sRenderThread)
    {
        // The viewer shows the median of its loops' rates. With the render thread slower
        // than the main loop, as under a frame limit, most loops would see no new frame and
        // show 0. So each loop gets the render rate of the last quarter second, for its own
        // length, and the sum still comes to the frames shown.
        constexpr auto RATE_PERIOD = std::chrono::milliseconds(250);
        const Clock::time_point now = Clock::now();
        const uint64_t shown = sRenderThread->presentedFrames();
        if (sShownAt == Clock::time_point{})
        {
            sShownAt = now;
            sRateStart = now;
            sRateFrames = shown;
        }
        if (now - sRateStart >= RATE_PERIOD)
        {
            sShownRate = F64(shown - sRateFrames) / std::chrono::duration<F64>(now - sRateStart).count();
            sRateStart = now;
            sRateFrames = shown;
        }
        const F64 frames = sShownRate * std::chrono::duration<F64>(now - sShownAt).count();
        sShownAt = now;
        return frames;
    }
#endif
    return 1;
}
