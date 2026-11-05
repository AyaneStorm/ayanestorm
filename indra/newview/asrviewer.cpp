/**
 * @file asrviewer.cpp
 * @brief See asrviewer.h.
 */
#include "llviewerprecompiledheaders.h"

#include "asrviewer.h"

#include "asrbridge.h"
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

std::unique_ptr<asr::Device> sDevice;
std::unique_ptr<asr::RenderThread> sRenderThread;
void* sPlatformWindow = nullptr;

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

        std::vector<uint8_t> copy(targetRow * height);
        const U8* source = static_cast<const U8*>(pixels);
        U8* target = copy.data();
        for (U32 row = 0; row < height; ++row)
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
        sDevice->textures().upload(slot, 0, x, y, width, height, std::move(copy));
    }

    void generateMips(U32 slot) override
    {
        sDevice->textures().generateMips(slot);
    }

    void destroyTexture(U32 slot) override
    {
        sDevice->textures().destroy(slot, sRenderThread->currentUIGeneration());
    }

    void beginUI() override
    {
        mList = &sRenderThread->ui();
        mList->clear();
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
    ASR::setRendererLink(&sViewerLink);
    LL_INFOS("ASRenderer") << "running on " << sDevice->info().backend << ", " << sDevice->info().name << ", budget "
                           << sDevice->memoryBudget().budget() / (1024 * 1024) << " MB" << LL_ENDL;
    return std::string();
}

void stopRenderer()
{
    ASR::setRendererLink(nullptr);
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

void ASRViewer::beginUI()
{
    ASR::beginUI();
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
