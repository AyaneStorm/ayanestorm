/**
 * asrdevicemetal.mm
 * Native Metal backend (Apple GPU family 7 and later, M1 onward), with no
 * translation layer. Draws the scene through compute culling and a visibility
 * buffer, the UI through one bindless pipeline per blend, and the test triangle.
 * Needs macOS 15 for residency sets, which make a bindless
 * texture cost nothing per frame. See asrdevice.h.
 */
#include "asrdevice.h"
#include "asrscenegraph.h"
#include "asrscenestore.h"
#include "asrtexturestreaming.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace asr
{

namespace
{

constexpr uint32_t FRAMES_IN_FLIGHT = 3;

// Where the vertex buffer goes. The fragment stage's buffer 0 is the texture array.
constexpr NSUInteger UI_VERTEX_BUFFER = 0;

// Where asr_spirv2msl puts the GLSL push constants, on every stage.
constexpr NSUInteger PUSH_CONSTANT_BUFFER = 8;

constexpr uint64_t UPLOAD_ALIGNMENT = 16;

// Zeros that new textures are cleared from, a run of rows at a time.
constexpr NSUInteger ZERO_BYTES = 1024 * 1024;

// One pass's timestamps: start, end, and for a draw the end of vertices and start of fragments.
constexpr uint32_t SAMPLES_PER_PASS = 4;

static_assert(sizeof(GpuDrawArguments) == sizeof(MTLDrawPrimitivesIndirectArguments), "culling counts into these");

const char* const NEEDS_MACOS_15 = "the renderer needs macOS 15 or later";

MTLBlendFactor toMetal(UIBlendFactor factor)
{
    switch (factor)
    {
        case UIBlendFactor::One:                 return MTLBlendFactorOne;
        case UIBlendFactor::Zero:                return MTLBlendFactorZero;
        case UIBlendFactor::DestColor:           return MTLBlendFactorDestinationColor;
        case UIBlendFactor::SourceColor:         return MTLBlendFactorSourceColor;
        case UIBlendFactor::OneMinusDestColor:   return MTLBlendFactorOneMinusDestinationColor;
        case UIBlendFactor::OneMinusSourceColor: return MTLBlendFactorOneMinusSourceColor;
        case UIBlendFactor::DestAlpha:           return MTLBlendFactorDestinationAlpha;
        case UIBlendFactor::SourceAlpha:         return MTLBlendFactorSourceAlpha;
        case UIBlendFactor::OneMinusDestAlpha:   return MTLBlendFactorOneMinusDestinationAlpha;
        case UIBlendFactor::OneMinusSourceAlpha: return MTLBlendFactorOneMinusSourceAlpha;
    }
    return MTLBlendFactorOne;
}

class DeviceMetal final : public Device, private SceneChunks
{
public:
    ~DeviceMetal() override
    {
        // Frames still running hold the semaphore, and signal it as they finish.
        for (uint32_t i = 0; mFrameSlots && i < FRAMES_IN_FLIGHT; ++i)
        {
            dispatch_semaphore_wait(mFrameSlots, DISPATCH_TIME_FOREVER);
        }
        for (uint32_t i = 0; mFrameSlots && i < FRAMES_IN_FLIGHT; ++i)
        {
            dispatch_semaphore_signal(mFrameSlots);
        }
        // Texture copies tell the queue when they finish, so none may still be running.
        if (mQueue)
        {
            id<MTLCommandBuffer> last = [mQueue commandBuffer];
            [last commit];
            [last waitUntilCompleted];
        }
        while (mRingHandlers.load(std::memory_order_acquire) != 0)
        {
            std::this_thread::yield();
        }
        mTextures.setUploadRing(nullptr, 0, 0);
    }

    bool init(const SurfaceTarget& target, const DeviceOptions& options, std::string& error)
    {
        mDevice = MTLCreateSystemDefaultDevice();
        if (!mDevice)
        {
            error = "no Metal device";
            return false;
        }
        if (![mDevice supportsFamily:MTLGPUFamilyApple7])
        {
            error = "this GPU is older than Apple family 7";
            return false;
        }
        if (!@available(macOS 15.0, *))
        {
            error = NEEDS_MACOS_15;
            return false;
        }

        mInfo.backend = "Metal";
        mInfo.vendor = "Apple";
        mInfo.name = mDevice.name.UTF8String;
        mInfo.memoryBudgetBytes = mDevice.recommendedMaxWorkingSetSize;
        mMemoryCapBytes = options.memoryCapBytes;
        mOcclusion = options.occlusion;
        refreshMemoryBudget();
        mUploadBytesPerFrame = mMemoryBudget.uploadBytesPerFrame(FRAMES_IN_FLIGHT);

        // The main thread writes texture pixels here, and a blit copies them into
        // textures only the GPU reaches, which it keeps compressed.
        const uint64_t ringBytes = mMemoryBudget.uploadRingBytes(FRAMES_IN_FLIGHT);
        if (!mMemoryBudget.reserve(MemoryCategory::Staging, ringBytes + ZERO_BYTES))
        {
            error = "the texture upload ring does not fit in its memory budget";
            return false;
        }
        mUploadRing = [mDevice newBufferWithLength:ringBytes options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        mZeros = [mDevice newBufferWithLength:ZERO_BYTES options:MTLResourceStorageModeShared];
        if (!mUploadRing || !mZeros)
        {
            error = "cannot create the texture upload ring";
            return false;
        }
        memset(mZeros.contents, 0, ZERO_BYTES);
        fprintf(stderr, "asrenderer: texture upload ring at 0x%llx to 0x%llx\n", (unsigned long long)addressOf(mUploadRing),
                (unsigned long long)(addressOf(mUploadRing) + ringBytes));
        mUploadPieceBytes = mMemoryBudget.uploadRingPieceBytes(FRAMES_IN_FLIGHT);
        mTextures.setUploadRing(static_cast<uint8_t*>(mUploadRing.contents), ringBytes, mUploadPieceBytes);
        // Fetched textures are kept in BC formats where the GPU samples them: Apple silicon does.
        mTextures.setCompressionSupported(mDevice.supportsBCTextureCompression);

        NSView* view = (__bridge NSView*)target.nativeWindow;
        mLayer = [CAMetalLayer layer];
        mLayer.device = mDevice;
        mLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        mLayer.framebufferOnly = YES;
        mLayer.maximumDrawableCount = 3;
        mLayer.displaySyncEnabled = options.vsync;
        mLayer.contentsScale = view.window ? view.window.backingScaleFactor : 1.0;
        view.wantsLayer = YES;
        view.layer = mLayer;

        mQueue = [mDevice newCommandQueue];

        id<MTLFunction> vertex = loadFunction(options.shaderDir, "asrTestTriangleV", error);
        id<MTLFunction> fragment = loadFunction(options.shaderDir, "asrTestTriangleF", error);
        if (!vertex || !fragment)
        {
            return false;
        }

        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = mLayer.pixelFormat;
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float; // the window pass has the UI depth
        NSError* failure = nil;
        mPipeline =[mDevice newRenderPipelineStateWithDescriptor:descriptor error:&failure];
        if (!mPipeline)
        {
            error = std::string("pipeline: ") + failure.localizedDescription.UTF8String;
            return false;
        }
        return createUI(options.shaderDir, error) && createScene(options.shaderDir, error);
    }

    const DeviceInfo& info() const override { return mInfo; }
    const std::string& lastError() const override { return mError; }
    uint32_t validationErrors() const override { return 0; }

    std::string takeGpuErrors() override
    {
        std::lock_guard<std::mutex> lock(mGpuErrors->mutex);
        if (mGpuErrors->count == 0)
        {
            return std::string();
        }
        std::string text = std::to_string(mGpuErrors->count) + " GPU command buffers failed";
        for (const std::string& error : mGpuErrors->first)
        {
            text += "\n  " + error;
        }
        mGpuErrors->count = 0;
        mGpuErrors->first.clear();
        return text;
    }

    // Apple's figure is fixed for the device, so unlike Vulkan it does not move
    // with what other applications use.
    void refreshMemoryBudget() override
    {
        driverBudgetBytes = mDevice.recommendedMaxWorkingSetSize;
        driverUsageBytes = mDevice.currentAllocatedSize;
        setDriverBudget(mDevice.recommendedMaxWorkingSetSize);
    }

    bool renderFrame(const FrameInput& input) override
    {
        @autoreleasepool
        {
            const uint32_t width = input.width;
            const uint32_t height = input.height;
            if (width == 0 || height == 0)
            {
                return true; // minimized
            }
            const CGSize size = CGSizeMake(width, height);
            if (!CGSizeEqualToSize(mLayer.drawableSize, size))
            {
                mLayer.drawableSize = size;

                // An estimate at 4 bytes per pixel, for each image the layer can hold.
                mMemoryBudget.release(MemoryCategory::RenderTargets, mDrawableBytes);
                mDrawableBytes = uint64_t(width) * height * 4 * mLayer.maximumDrawableCount;
                // The screen cannot wait for room: count it anyway, and textures shrink to give it back.
                mMemoryBudget.reserveAnyway(MemoryCategory::RenderTargets, mDrawableBytes);
            }

            id<CAMetalDrawable> drawable = [mLayer nextDrawable];
            if (!drawable)
            {
                return true; // none free right now: not an error
            }

            // The buffers of this slot are free once its earlier frame has finished.
            dispatch_semaphore_wait(mFrameSlots, DISPATCH_TIME_FOREVER);
            const uint32_t slot = uint32_t(mFrame % FRAMES_IN_FLIGHT);
            const std::chrono::steady_clock::time_point frameStart = std::chrono::steady_clock::now();
            mFrameSeconds = mFrame != 0 ? std::chrono::duration<double>(frameStart - mPreviousFrameStart).count() : 0.0;
            mPreviousFrameStart = frameStart;
            releaseFreedTextures();
            mRetiredTextures.run(input.ui ? input.ui->generation : 0, mFrame, FRAMES_IN_FLIGHT, [this](uint32_t retired)
            {
                destroyTexture(retired);
                mTextures.releaseSlot(retired);
            });
            if (!runTextureCommands() || !prepareUI(slot, input.ui))
            {
                dispatch_semaphore_signal(mFrameSlots);
                return false;
            }
            bringSlots(slot);

            id<MTLCommandBuffer> commands = newCommandBuffer();
            watchErrors(commands, "frame");
            if (input.camera && !recordScene(commands, slot, *input.camera, width, height, input.audit))
            {
                dispatch_semaphore_signal(mFrameSlots);
                return false;
            }

            if (!ensureUIDepth(uint32_t(drawable.texture.width), uint32_t(drawable.texture.height)))
            {
                dispatch_semaphore_signal(mFrameSlots);
                return false;
            }
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0); // what the viewer clears to, and transparent UI shows it
            pass.depthAttachment.texture = mUIDepth;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.storeAction = MTLStoreActionDontCare;
            pass.depthAttachment.clearDepth = 0.0; // reversed depth: 0 is far away

            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            if (input.camera)
            {
                [encoder waitForFence:mSceneFence beforeStages:MTLRenderStageFragment];
                [encoder setRenderPipelineState:mCompositePipeline];
                [encoder setFragmentTexture:mSceneColorTarget atIndex:9];
                // The composite reads the world view's offset from push.list: none here yet,
                // the scene fills the window (FrameInput::worldView is Vulkan's so far).
                const ScenePush noOffset{};
                [encoder setFragmentBytes:&noOffset length:sizeof(noOffset) atIndex:PUSH_CONSTANT_BUFFER];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            if (input.testTriangle)
            {
                [encoder setRenderPipelineState:mPipeline];
                [encoder setFragmentBytes:&input.seconds length:sizeof(input.seconds) atIndex:PUSH_CONSTANT_BUFFER];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            if (input.ui && !input.ui->vertices().empty())
            {
                // The world part against the scene depth, then the depth farthest again for
                // the HUD's part, as Vulkan's recordOverlays(). No HUD in Metal yet.
                const UIList& ui = *input.ui;
                const uint32_t draws = uint32_t(ui.draws().size());
                const uint32_t hud = input.camera ? std::min(ui.hudDraw(), draws) : 0;
                if (hud != 0)
                {
                    if (ui.worldDepthTested())
                    {
                        writeUIDepth(encoder, 0, width, height);
                    }
                    drawUI(encoder, slot, ui, width, height, 0, hud);
                    writeUIDepth(encoder, 1, width, height);
                }
                drawUI(encoder, slot, ui, width, height, hud, draws);
            }
            [encoder endEncoding];
            [commands presentDrawable:drawable];

            dispatch_semaphore_t frameSlots = mFrameSlots;
            [commands addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(frameSlots); }];
            [commands commit];
            ++mFrame;
        }
        return true;
    }

private:
    bool createUI(const std::string& shaderDir, std::string& error)
    {
        mFrameSlots = dispatch_semaphore_create(FRAMES_IN_FLIGHT);
        mUIVertexFunction = loadFunction(shaderDir, "asrUIV", error);
        mUIFragmentFunction = loadFunction(shaderDir, "asrUIF", error);
        if (!mUIVertexFunction || !mUIFragmentFunction)
        {
            return false;
        }

        // The texture array lives in a buffer: the shader reads a slot from it, so
        // drawing with another texture changes no state. Each frame in flight has its own,
        // see TextureSlotCopies.
        mArgumentEncoder = [mUIFragmentFunction newArgumentEncoderWithBufferIndex:0];
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            mArguments[i] = [mDevice newBufferWithLength:mArgumentEncoder.encodedLength options:MTLResourceStorageModeShared];
            if (!mArguments[i])
            {
                error = "cannot create the texture arrays";
                return false;
            }
        }

        for (uint32_t i = 0; i < uint32_t(UISampler::Count); ++i)
        {
            const UISampler kind = UISampler(i);
            const bool nearest = kind == UISampler::NearestClamp || kind == UISampler::NearestWrap;
            const bool wrap = kind == UISampler::LinearWrap || kind == UISampler::NearestWrap;
            MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
            sampler.magFilter = nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
            sampler.minFilter = sampler.magFilter;
            sampler.mipFilter = nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
            sampler.sAddressMode = wrap ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
            sampler.tAddressMode = sampler.sAddressMode;
            sampler.supportArgumentBuffers = YES;
            mSamplers[i] = [mDevice newSamplerStateWithDescriptor:sampler];
            for (uint32_t copy = 0; copy < FRAMES_IN_FLIGHT; ++copy)
            {
                [mArgumentEncoder setArgumentBuffer:mArguments[copy] offset:0];
                [mArgumentEncoder setSamplerState:mSamplers[i] atIndex:MAX_TEXTURES + i];
            }
        }

        if (@available(macOS 15.0, *))
        {
            MTLResidencySetDescriptor* descriptor = [MTLResidencySetDescriptor new];
            descriptor.label = @"asrenderer textures";
            NSError* failure = nil;
            id<MTLResidencySet> residency = [mDevice newResidencySetWithDescriptor:descriptor error:&failure];
            if (!residency)
            {
                error = std::string("residency set: ") + failure.localizedDescription.UTF8String;
                return false;
            }
            [mQueue addResidencySet:residency];
            mResidency = residency;
        }
        return true;
    }

    // --- Scene ----------------------------------------------------------------

    // Shaders reach these by address, so they must be resident, and Metal cannot see
    // which ones a pass touches. That is why the passes are ordered by mSceneFence.
    // A required buffer is counted even past the budget: scene tables and lists must grow with
    // what is already in the scene, or the renderer stops. The geometry that fills them waits for
    // room instead (createSceneChunk()), and textureLimit() falls so textures give memory back.
    id<MTLBuffer> createSceneBuffer(uint64_t bytes, MemoryCategory category, const char* what, bool required = false)
    {
        if (required && !mMemoryBudget.reserve(category, bytes))
        {
            mMemoryBudget.reserveAnyway(category, bytes);
        }
        else if (!required && !mMemoryBudget.reserve(category, bytes))
        {
            mError = mMemoryBudget.refusal(what, category, bytes);
            return nil;
        }
        id<MTLBuffer> buffer = [mDevice newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (!buffer)
        {
            mMemoryBudget.release(category, bytes);
            mError = std::string("cannot create ") + what;
            return nil;
        }
        if (@available(macOS 15.0, *))
        {
            [(id<MTLResidencySet>)mResidency addAllocation:buffer];
            [(id<MTLResidencySet>)mResidency commit];
        }
        // Temporary: a GPU fault report gives the address read, and this says whose it was.
        fprintf(stderr, "asrenderer: frame %llu, %s at 0x%llx to 0x%llx\n", (unsigned long long)mFrame, what,
                (unsigned long long)addressOf(buffer), (unsigned long long)(addressOf(buffer) + bytes));
        return buffer;
    }

    void releaseSceneBuffer(id<MTLBuffer> buffer, MemoryCategory category)
    {
        fprintf(stderr, "asrenderer: frame %llu, released 0x%llx to 0x%llx\n", (unsigned long long)mFrame,
                (unsigned long long)addressOf(buffer), (unsigned long long)(addressOf(buffer) + buffer.length));
        if (@available(macOS 15.0, *))
        {
            [(id<MTLResidencySet>)mResidency removeAllocation:buffer];
            [(id<MTLResidencySet>)mResidency commit];
        }
        mMemoryBudget.release(category, buffer.length);
    }

    static uint64_t addressOf(id<MTLBuffer> buffer)
    {
        if (@available(macOS 15.0, *))
        {
            return buffer.gpuAddress;
        }
        return 0; // never reached: the probe refuses older systems
    }

    uint64_t createSceneChunk(uint32_t index) override
    {
        id<MTLBuffer> chunk = createSceneBuffer(SCENE_CHUNK_BYTES, MemoryCategory::Geometry, "scene geometry chunk");
        if (!chunk)
        {
            return 0;
        }
        if (mSceneChunks.size() <= index) mSceneChunks.resize(size_t(index) + 1);
        mSceneChunks[index] = chunk;
        return addressOf(chunk);
    }

    void releaseSceneChunk(uint32_t index) override
    {
        // SceneStore already waited for every in-flight reference to retire.
        releaseSceneBuffer(mSceneChunks[index], MemoryCategory::Geometry);
        mSceneChunks[index] = nil;
    }

    id<MTLComputePipelineState> createComputePipeline(const std::string& shaderDir, const char* name, std::string& error)
    {
        id<MTLFunction> function = loadFunction(shaderDir, name, error);
        if (!function)
        {
            return nil;
        }
        NSError* failure = nil;
        id<MTLComputePipelineState> pipeline = [mDevice newComputePipelineStateWithFunction:function error:&failure];
        if (!pipeline)
        {
            error = std::string(name) + ": " + failure.localizedDescription.UTF8String;
        }
        return pipeline;
    }

    id<MTLRenderPipelineState> createVisibilityPipeline(id<MTLFunction> vertex, id<MTLFunction> fragment, std::string& error)
    {
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError* failure = nil;
        id<MTLRenderPipelineState> pipeline = [mDevice newRenderPipelineStateWithDescriptor:descriptor error:&failure];
        if (!pipeline)
        {
            error = std::string("visibility pipeline: ") + failure.localizedDescription.UTF8String;
        }
        return pipeline;
    }

    bool createScene(const std::string& shaderDir, std::string& error)
    {
        mCullPipeline = createComputePipeline(shaderDir, "asrSceneCullC", error);
        mPyramidPipeline = createComputePipeline(shaderDir, "asrScenePyramidC", error);
        mResolvePipeline = createComputePipeline(shaderDir, "asrSceneResolveC", error);
        mSkinPipeline = createComputePipeline(shaderDir, "asrSceneSkinC", error);
        mTextureMipPipeline = createComputePipeline(shaderDir, "asrTextureMipC", error);
        mEncodeBC1Pipeline = createComputePipeline(shaderDir, "asrEncodeBC1C", error);
        mEncodeBC45Pipeline = createComputePipeline(shaderDir, "asrEncodeBC45C", error);
        mEncodeBC7Pipeline = createComputePipeline(shaderDir, "asrEncodeBC7C", error);
        mEncodeFence = [mDevice newFence];
        if (!mTextureMipPipeline || !mEncodeBC1Pipeline || !mEncodeBC45Pipeline || !mEncodeBC7Pipeline || !mEncodeFence)
        {
            return false;
        }
        id<MTLFunction> visibilityVertex = loadFunction(shaderDir, "asrSceneVisibilityV", error);
        id<MTLFunction> visibilityFragment = loadFunction(shaderDir, "asrSceneVisibilityF", error);
        id<MTLFunction> maskedVertex = loadFunction(shaderDir, "asrSceneMaskedV", error);
        id<MTLFunction> maskedFragment = loadFunction(shaderDir, "asrSceneMaskedF", error);
        id<MTLFunction> compositeVertex = loadFunction(shaderDir, "asrSceneCompositeV", error);
        id<MTLFunction> compositeFragment = loadFunction(shaderDir, "asrSceneCompositeF", error);
        if (!mCullPipeline || !mPyramidPipeline || !mResolvePipeline || !mSkinPipeline || !visibilityVertex || !visibilityFragment || !maskedVertex || !maskedFragment ||
            !compositeVertex || !compositeFragment)
        {
            return false;
        }

        mVisibilityPipeline = createVisibilityPipeline(visibilityVertex, visibilityFragment, error);
        mMaskedPipeline = createVisibilityPipeline(maskedVertex, maskedFragment, error);
        if (!mVisibilityPipeline || !mMaskedPipeline || !createMacOITPipelines(shaderDir, error))
        {
            return false;
        }

        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = compositeVertex;
        descriptor.fragmentFunction = compositeFragment;
        descriptor.colorAttachments[0].pixelFormat = mLayer.pixelFormat;
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float; // the window pass has the UI depth
        NSError* failure = nil;
        mCompositePipeline = [mDevice newRenderPipelineStateWithDescriptor:descriptor error:&failure];
        if (!mCompositePipeline)
        {
            error = std::string("composite pipeline: ") + failure.localizedDescription.UTF8String;
            return false;
        }
        // The scene depth into the UI depth for the UI's world part, or farthest after it.
        id<MTLFunction> depthCopy = loadFunction(shaderDir, "asrOverlayDepthF", error);
        if (!depthCopy)
        {
            return false;
        }
        descriptor.fragmentFunction = depthCopy;
        descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
        mDepthCopyPipeline = [mDevice newRenderPipelineStateWithDescriptor:descriptor error:&failure];
        if (!mDepthCopyPipeline)
        {
            error = std::string("depth copy pipeline: ") + failure.localizedDescription.UTF8String;
            return false;
        }

        MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
        depth.depthCompareFunction = MTLCompareFunctionGreater; // reversed depth: nearer is larger
        depth.depthWriteEnabled = YES;
        mDepthState = [mDevice newDepthStencilStateWithDescriptor:depth];
        mSceneFence = [mDevice newFence];

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            mSceneViews[i] = createSceneBuffer(sizeof(GpuSceneView), MemoryCategory::Staging, "scene view");
            mSceneReadback[i] = createSceneBuffer(sizeof(GpuSceneCounts), MemoryCategory::Staging, "scene readback");
            mFeedbackReadback[i] = createSceneBuffer(FEEDBACK_BYTES, MemoryCategory::Staging, "texture feedback readback");
            if (!mSceneViews[i] || !mSceneReadback[i] || !mFeedbackReadback[i])
            {
                error = mError;
                return false;
            }
        }
        mSceneCounts = createSceneBuffer(sizeof(GpuSceneCounts), MemoryCategory::Geometry, "scene draw counts");
        mSceneCountsTemplate = createSceneBuffer(sizeof(GpuSceneCounts), MemoryCategory::Staging, "scene draw template");
        mTextureFeedback = createSceneBuffer(FEEDBACK_BYTES, MemoryCategory::Geometry, "texture feedback");
        if (!mSceneCounts || !mSceneCountsTemplate || !mTextureFeedback)
        {
            error = mError;
            return false;
        }
        *static_cast<GpuSceneCounts*>(mSceneCountsTemplate.contents) = sceneCountsTemplate();

        declareSceneGraph(mGraph);
        setPassBodies();
        createPassTimers();
        return true;
    }

    // Timestamps at the edges of encoders, which is what Apple GPUs can sample. A pass's
    // vertex and fragment work run apart on them, so a draw gets four samples.
    void createPassTimers()
    {
        if (![mDevice supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
        {
            return;
        }
        for (id<MTLCounterSet> set in mDevice.counterSets)
        {
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
            {
                MTLCounterSampleBufferDescriptor* descriptor = [MTLCounterSampleBufferDescriptor new];
                descriptor.counterSet = set;
                descriptor.storageMode = MTLStorageModeShared;
                descriptor.sampleCount = SAMPLES_PER_PASS * MAX_TIMED_PASSES * FRAMES_IN_FLIGHT;
                mPassSamples = [mDevice newCounterSampleBufferWithDescriptor:descriptor error:nil];
            }
        }
        [mDevice sampleTimestamps:&mClockStartCpu gpuTimestamp:&mClockStartGpu];
    }

    NSUInteger passSample(uint32_t sample) const
    {
        return NSUInteger(mRecordSlot * MAX_TIMED_PASSES + mRecordPass) * SAMPLES_PER_PASS + sample;
    }

    id<MTLComputeCommandEncoder> beginCompute()
    {
        MTLComputePassDescriptor* pass = [MTLComputePassDescriptor computePassDescriptor];
        if (mPassSamples && mRecordPass < MAX_TIMED_PASSES)
        {
            pass.sampleBufferAttachments[0].sampleBuffer = mPassSamples;
            pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = passSample(0);
            pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = passSample(1);
        }
        id<MTLComputeCommandEncoder> encoder = [mRecordCommands computeCommandEncoderWithDescriptor:pass];
        encoder.label = passLabel();
        [encoder waitForFence:mSceneFence];
        return encoder;
    }

    id<MTLBlitCommandEncoder> beginBlit()
    {
        MTLBlitPassDescriptor* pass = [MTLBlitPassDescriptor blitPassDescriptor];
        if (mPassSamples && mRecordPass < MAX_TIMED_PASSES)
        {
            pass.sampleBufferAttachments[0].sampleBuffer = mPassSamples;
            pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = passSample(0);
            pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = passSample(1);
        }
        id<MTLBlitCommandEncoder> encoder = [mRecordCommands blitCommandEncoderWithDescriptor:pass];
        encoder.label = passLabel();
        [encoder waitForFence:mSceneFence];
        return encoder;
    }

    // Command buffers that say which encoder an error happened in.
    id<MTLCommandBuffer> newCommandBuffer()
    {
        MTLCommandBufferDescriptor* descriptor = [MTLCommandBufferDescriptor new];
        descriptor.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
        return [mQueue commandBufferWithDescriptor:descriptor];
    }

    // The pass being recorded, so a GPU error says where it happened.
    NSString* passLabel() const
    {
        return [NSString stringWithUTF8String:mGraph.passName(mRecordPass)];
    }

    // Reports a command buffer that ended in error, with the encoder it failed in and what
    // shader validation found, when it is on. Errors go to stderr at once, as the render
    // thread has no log, and the first few are kept for the viewer's log. Without this an
    // error went unseen: macOS stops running a process's GPU work after repeated faults,
    // and every frame then 'finishes' at once with nothing drawn.
    void watchErrors(id<MTLCommandBuffer> commands, const char* what)
    {
        std::shared_ptr<GpuErrors> errors = mGpuErrors; // a handler can run after the device is gone
        const unsigned long long frame = mFrame;
        [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
            if (done.status != MTLCommandBufferStatusError)
            {
                return;
            }
            NSMutableString* text = [NSMutableString stringWithFormat:@"%s command buffer of frame %llu failed: %@", what, frame,
                                                                      done.error.localizedDescription];
            NSArray* encoders = done.error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
            for (id<MTLCommandBufferEncoderInfo> info in encoders)
            {
                if (info.errorState == MTLCommandEncoderErrorStateFaulted || info.errorState == MTLCommandEncoderErrorStatePending)
                {
                    [text appendFormat:@"; encoder '%@' %s", info.label,
                                       info.errorState == MTLCommandEncoderErrorStateFaulted ? "faulted" : "did not finish"];
                }
            }
            for (id<MTLFunctionLog> log in done.logs)
            {
                [text appendFormat:@"; %@", log.description];
            }
            fprintf(stderr, "asrenderer: %s\n", text.UTF8String);
            errors->add(text.UTF8String);
        }];
    }

    id<MTLTexture> createTarget(MTLPixelFormat format, MTLTextureUsage usage, uint32_t width, uint32_t height, uint32_t pixelBytes, const char* what)
    {
        // The screen cannot wait for room: count it anyway, and textures shrink to give it back.
        const uint64_t bytes = uint64_t(width) * height * pixelBytes;
        mMemoryBudget.reserveAnyway(MemoryCategory::RenderTargets, bytes);
        mSceneTargetBytes += bytes;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = usage;
        id<MTLTexture> texture = [mDevice newTextureWithDescriptor:descriptor];
        if (!texture)
        {
            mError = std::string("cannot create ") + what;
        }
        return texture;
    }

    // Mac OIT, the native renderer's transparency, kept in one place: its targets, its
    // pipelines and its passes. Another transparency would replace this block and the passes
    // of declareMacOIT(), making the scene colour the same way.
    struct MacOITResources
    {
        id<MTLTexture> moments = nil;   // set 1 binding 4
        id<MTLTexture> tailDepth = nil; // 5
        id<MTLTexture> color = nil;     // 6
        id<MTLTexture> weights = nil;   // 7
        id<MTLBuffer> state = nil;      // see macoitBytes()
        id<MTLRenderPipelineState> keys = nil;
        id<MTLRenderPipelineState> tail = nil;
        id<MTLRenderPipelineState> colorPass = nil;
        id<MTLComputePipelineState> resolve = nil;
        id<MTLDepthStencilState> depthState = nil; // tested against the opaque depth, never written
    };

    id<MTLRenderPipelineState> createMacOITPipeline(id<MTLFunction> vertex, id<MTLFunction> fragment, std::initializer_list<MTLPixelFormat> colors,
                                                    std::string& error)
    {
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        NSUInteger index = 0;
        for (MTLPixelFormat format : colors)
        {
            // Every target adds, as the OpenGL version's accumulations do.
            MTLRenderPipelineColorAttachmentDescriptor* attachment = descriptor.colorAttachments[index++];
            attachment.pixelFormat = format;
            attachment.blendingEnabled = YES;
            attachment.rgbBlendOperation = MTLBlendOperationAdd;
            attachment.alphaBlendOperation = MTLBlendOperationAdd;
            attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
            attachment.destinationRGBBlendFactor = MTLBlendFactorOne;
            attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
            attachment.destinationAlphaBlendFactor = MTLBlendFactorOne;
        }
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError* failure = nil;
        id<MTLRenderPipelineState> pipeline = [mDevice newRenderPipelineStateWithDescriptor:descriptor error:&failure];
        if (!pipeline)
        {
            error = std::string("Mac OIT pipeline: ") + failure.localizedDescription.UTF8String;
        }
        return pipeline;
    }

    bool createMacOITPipelines(const std::string& shaderDir, std::string& error)
    {
        id<MTLFunction> vertex = loadFunction(shaderDir, "asrSceneMacOITV", error);
        id<MTLFunction> keys = loadFunction(shaderDir, "asrSceneMacOITKeysF", error);
        id<MTLFunction> tail = loadFunction(shaderDir, "asrSceneMacOITTailF", error);
        id<MTLFunction> color = loadFunction(shaderDir, "asrSceneMacOITColorF", error);
        mMacOIT.resolve = createComputePipeline(shaderDir, "asrSceneMacOITResolveC", error);
        if (!vertex || !keys || !tail || !color || !mMacOIT.resolve)
        {
            return false;
        }
        mMacOIT.keys = createMacOITPipeline(vertex, keys, {}, error);
        mMacOIT.tail = createMacOITPipeline(vertex, tail, { MTLPixelFormatRGBA32Float, MTLPixelFormatR32Float }, error);
        mMacOIT.colorPass = createMacOITPipeline(vertex, color, { MTLPixelFormatRGBA16Float, MTLPixelFormatRG32Float }, error);
        MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
        depth.depthCompareFunction = MTLCompareFunctionGreater; // reversed depth: nearer is larger
        depth.depthWriteEnabled = NO;
        mMacOIT.depthState = [mDevice newDepthStencilStateWithDescriptor:depth];
        return mMacOIT.keys && mMacOIT.tail && mMacOIT.colorPass && mMacOIT.depthState;
    }

    bool createMacOITTargets(uint32_t width, uint32_t height)
    {
        constexpr MTLTextureUsage usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        mMacOIT.moments = createTarget(MTLPixelFormatRGBA32Float, usage, width, height, 16, "Mac OIT moments");
        mMacOIT.tailDepth = createTarget(MTLPixelFormatR32Float, usage, width, height, 4, "Mac OIT tail depth");
        mMacOIT.color = createTarget(MTLPixelFormatRGBA16Float, usage, width, height, 8, "Mac OIT colour");
        mMacOIT.weights = createTarget(MTLPixelFormatRG32Float, usage, width, height, 8, "Mac OIT weights");
        if (mMacOIT.state)
        {
            mRetiredBuffers.push_back({ mMacOIT.state, mFrame + FRAMES_IN_FLIGHT + 1, MemoryCategory::OIT });
        }
        const uint64_t pixels = uint64_t(width) * height;
        mMacOIT.state = createSceneBuffer(macoitBytes(pixels), MemoryCategory::OIT, "Mac OIT state", true);
        if (!mMacOIT.moments || !mMacOIT.tailDepth || !mMacOIT.color || !mMacOIT.weights || !mMacOIT.state)
        {
            return false;
        }
        // Every key empty, every farthest depth and count 0, before any frame uses it: shared
        // memory, so written here. The resolve keeps it so.
        const uint64_t keyBytes = pixels * MACOIT_LAYERS * sizeof(uint32_t);
        memset(mMacOIT.state.contents, 0xFF, keyBytes);
        memset(static_cast<char*>(mMacOIT.state.contents) + keyBytes, 0, macoitBytes(pixels) - keyBytes);
        return true;
    }

    // One Mac OIT raster pass over the blended lists, early then late. targets are its
    // colour targets, cleared; with gated set, the draws come from GpuSceneCounts::macoitTail.
    bool passMacOIT(id<MTLRenderPipelineState> pipeline, std::initializer_list<id<MTLTexture>> targets, bool gated)
    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        NSUInteger index = 0;
        for (id<MTLTexture> target : targets)
        {
            pass.colorAttachments[index].texture = target;
            pass.colorAttachments[index].loadAction = MTLLoadActionClear; // sums start at 0
            pass.colorAttachments[index].storeAction = MTLStoreActionStore;
            pass.colorAttachments[index].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
            ++index;
        }
        pass.depthAttachment.texture = mDepthTarget;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        if (mPassSamples && mRecordPass < MAX_TIMED_PASSES)
        {
            pass.sampleBufferAttachments[0].sampleBuffer = mPassSamples;
            pass.sampleBufferAttachments[0].startOfVertexSampleIndex = passSample(0);
            pass.sampleBufferAttachments[0].endOfVertexSampleIndex = passSample(2);
            pass.sampleBufferAttachments[0].startOfFragmentSampleIndex = passSample(3);
            pass.sampleBufferAttachments[0].endOfFragmentSampleIndex = passSample(1);
        }

        id<MTLRenderCommandEncoder> draw = [mRecordCommands renderCommandEncoderWithDescriptor:pass];
        draw.label = passLabel();
        [draw waitForFence:mSceneFence beforeStages:MTLRenderStageVertex];
        [draw setRenderPipelineState:pipeline];
        [draw setDepthStencilState:mMacOIT.depthState];
        [draw setFrontFacingWinding:MTLWindingCounterClockwise];
        [draw setFragmentBuffer:mArguments[mRecordSlot] offset:0 atIndex:0];
        if (pipeline == mMacOIT.colorPass)
        {
            // Only the colour pass reads the moments; the tail pass writes them.
            [draw setFragmentTexture:mMacOIT.moments atIndex:4];
            [draw setFragmentTexture:mMacOIT.tailDepth atIndex:5];
        }
        for (uint32_t kind = SCENE_BLENDED_KIND; kind < SCENE_KINDS; ++kind)
        {
            [draw setCullMode:(kind & 1) ? MTLCullModeNone : MTLCullModeBack];
            for (uint32_t bucket = 0; bucket < CLUSTER_BUCKETS; ++bucket)
            {
                const uint32_t list = sceneList(kind, bucket);
                mScenePush.list = list;
                [draw setVertexBytes:&mScenePush length:sizeof(mScenePush) atIndex:PUSH_CONSTANT_BUFFER];
                [draw setFragmentBytes:&mScenePush length:sizeof(mScenePush) atIndex:PUSH_CONSTANT_BUFFER];
                for (uint32_t late = 0; late < 2; ++late)
                {
                    const uint64_t at = gated ? offsetof(GpuSceneCounts, macoitTail) +
                                                    (late * SCENE_BLENDED_LISTS + list - sceneList(SCENE_BLENDED_KIND, 0)) * sizeof(GpuDrawArguments)
                                              : (late ? offsetof(GpuSceneCounts, late) : offsetof(GpuSceneCounts, early)) + list * sizeof(GpuDrawArguments);
                    [draw drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:mSceneCounts indirectBufferOffset:at];
                }
            }
        }
        [draw updateFence:mSceneFence afterStages:MTLRenderStageFragment];
        [draw endEncoding];
        return true;
    }

    bool passMacOITGate()
    {
        id<MTLComputeCommandEncoder> gate = beginCompute();
        [gate setComputePipelineState:mCullPipeline];
        mScenePush.phase = MACOIT_GATE;
        [gate setBytes:&mScenePush length:sizeof(mScenePush) atIndex:PUSH_CONSTANT_BUFFER];
        [gate dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(CULL_GROUP_SIZE, 1, 1)];
        [gate updateFence:mSceneFence];
        [gate endEncoding];
        return true;
    }

    bool passMacOITResolve()
    {
        id<MTLComputeCommandEncoder> resolve = beginCompute();
        [resolve setComputePipelineState:mMacOIT.resolve];
        [resolve setBytes:&mScenePush length:sizeof(mScenePush) atIndex:PUSH_CONSTANT_BUFFER];
        [resolve setTexture:mMacOIT.color atIndex:6];
        [resolve setTexture:mMacOIT.weights atIndex:7];
        [resolve setTexture:mLitTarget atIndex:8];
        [resolve setTexture:mSceneColorTarget atIndex:9];
        [resolve dispatchThreadgroups:MTLSizeMake((mSceneWidth + RESOLVE_GROUP_SIZE - 1) / RESOLVE_GROUP_SIZE, (mSceneHeight + RESOLVE_GROUP_SIZE - 1) / RESOLVE_GROUP_SIZE, 1)
                threadsPerThreadgroup:MTLSizeMake(RESOLVE_GROUP_SIZE, RESOLVE_GROUP_SIZE, 1)];
        [resolve updateFence:mSceneFence];
        [resolve endEncoding];
        return true;
    }

    void setMacOITBodies()
    {
        mGraph.setBody(PassMacOITKeys, [this]() { return passMacOIT(mMacOIT.keys, {}, false); });
        mGraph.setBody(PassMacOITGate, [this]() { return passMacOITGate(); });
        mGraph.setBody(PassMacOITTail, [this]() { return passMacOIT(mMacOIT.tail, { mMacOIT.moments, mMacOIT.tailDepth }, true); });
        mGraph.setBody(PassMacOITColor, [this]() { return passMacOIT(mMacOIT.colorPass, { mMacOIT.color, mMacOIT.weights }, false); });
        mGraph.setBody(PassMacOITResolve, [this]() { return passMacOITResolve(); });
    }

    // Command buffers keep what they use alive, so the old targets can go at once.
    bool ensureSceneTargets(uint32_t width, uint32_t height)
    {
        if (mSceneWidth == width && mSceneHeight == height)
        {
            return true;
        }
        mMemoryBudget.release(MemoryCategory::RenderTargets, mSceneTargetBytes);
        mSceneTargetBytes = 0;
        mSceneWidth = width;
        mSceneHeight = height;
        mHasPyramid = false;
        mAuditTarget = nil;
        for (bool& written : mAuditWritten)
        {
            written = false;
        }
        mVisibilityTarget = createTarget(MTLPixelFormatR32Uint, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, width, height, 4, "visibility buffer");
        mDepthTarget = createTarget(MTLPixelFormatDepth32Float, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, width, height, 4, "depth buffer");
        mAlbedoTarget = createTarget(MTLPixelFormatRGBA8Unorm, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, width, height, 4, "albedo target");
        mNormalTarget = createTarget(MTLPixelFormatRGBA16Float, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, width, height, 8, "normal target");
        mLitTarget = createTarget(MTLPixelFormatRGBA16Float, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, width, height, 8, "lit target");
        mSceneColorTarget = createTarget(MTLPixelFormatRGBA16Float, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, width, height, 8, "scene colour target");
        if (!mLitTarget || !mSceneColorTarget || !createMacOITTargets(width, height))
        {
            return false;
        }

        if (mScenePyramid)
        {
            mRetiredBuffers.push_back({ mScenePyramid, mFrame + FRAMES_IN_FLIGHT + 1, MemoryCategory::RenderTargets });
        }
        uint32_t offsets[PYRAMID_MAX_LEVELS];
        uint32_t levels = 0;
        const uint64_t pyramidBytes = uint64_t(pyramidLayout(width, height, offsets, levels)) * sizeof(float);
        mScenePyramid = createSceneBuffer(pyramidBytes, MemoryCategory::RenderTargets, "depth pyramid", true);
        return mVisibilityTarget && mDepthTarget && mAlbedoTarget && mNormalTarget && mScenePyramid;
    }

    // Frames still running may read the old table, so it stays until they are done.
    bool growTable(id<MTLBlitCommandEncoder> blit, uint32_t table, uint64_t bytes, bool preserve = true)
    {
        id<MTLBuffer> grown = createSceneBuffer(bytes, MemoryCategory::Geometry, "scene table", true);
        if (!grown)
        {
            return false;
        }
        if (preserve) memset(grown.contents, 0, bytes);
        if (mSceneTables[table])
        {
            if (preserve)
                [blit copyFromBuffer:mSceneTables[table] sourceOffset:0 toBuffer:grown destinationOffset:0 size:std::min<uint64_t>(mSceneTables[table].length, bytes)];
            mRetiredBuffers.push_back({ mSceneTables[table], mFrame + FRAMES_IN_FLIGHT + 1, MemoryCategory::Geometry });
        }
        mSceneTables[table] = grown;
        return true;
    }

    bool growList(id<MTLBuffer> __strong& list, uint64_t bytes, const char* what)
    {
        // Lists are regenerated each frame; reclaim large unused tails after a crowd leaves.
        if (bytes <= list.length && bytes > list.length / 2)
        {
            return true;
        }
        if (list)
        {
            mRetiredBuffers.push_back({ list, mFrame + FRAMES_IN_FLIGHT + 1, MemoryCategory::Geometry });
        }
        list = createSceneBuffer(bytes, MemoryCategory::Geometry, what, true);
        return list != nil;
    }

    // Applies this frame's scene changes, and fills in the view.
    bool passUpload()
    {
        const uint32_t slot = mRecordSlot;
        while (!mRetiredBuffers.empty() && mRetiredBuffers.front().frame <= mFrame)
        {
            releaseSceneBuffer(mRetiredBuffers.front().buffer, mRetiredBuffers.front().category);
            mRetiredBuffers.pop_front();
        }

        mSceneStore.update(mScene, *this, mFrame, mUploadBytesPerFrame);

        id<MTLBlitCommandEncoder> blit = beginBlit();
        for (uint32_t i = 0; i < SCENE_TABLES; ++i)
        {
            const uint64_t bytes = mSceneStore.tableCapacity(SceneTable(i));
            const bool positions = SceneTable(i) == SceneTable::SkinnedPositions;
            const bool resize = bytes != mSceneTables[i].length || (positions && mSceneStore.positionsCompacted());
            if (resize && !growTable(blit, i, bytes, !positions))
            {
                [blit endEncoding];
                return false;
            }
        }

        const uint64_t visibleBytes = mSceneStore.visibleCapacity() * sizeof(GpuListEntry);
        const uint64_t groupBytes = mSceneStore.groupCapacity() * sizeof(GpuListEntry);
        const uint64_t retestBytes = (mSceneStore.visibleCapacity() + mSceneStore.groupCapacity() + mSceneStore.instanceEnd()) * sizeof(GpuListEntry);
        if (!growList(mSceneVisible, visibleBytes, "visible cluster list") || !growList(mSceneRetest, retestBytes, "retest list") ||
            !growList(mSceneGroups, groupBytes, "group list"))
        {
            [blit endEncoding];
            return false;
        }

        uint64_t stagingBytes = 0;
        for (const SceneWrite& write : mSceneStore.writes())
        {
            stagingBytes += (write.bytes + UPLOAD_ALIGNMENT - 1) / UPLOAD_ALIGNMENT * UPLOAD_ALIGNMENT;
        }
        if (stagingBytes > mSceneStaging[slot].length)
        {
            const uint64_t oldSize = mSceneStaging[slot].length;
            const uint64_t size = std::max(stagingBytes, mUploadBytesPerFrame);
            mMemoryBudget.release(MemoryCategory::Staging, oldSize);
            mSceneStaging[slot] = nil;
            if (!mMemoryBudget.reserve(MemoryCategory::Staging, size))
            {
                mError = "the scene staging buffer does not fit in its memory budget";
                [blit endEncoding];
                return false;
            }
            mSceneStaging[slot] = [mDevice newBufferWithLength:size options:MTLResourceStorageModeShared];
        }

        uint64_t offset = 0;
        uint8_t* staging = static_cast<uint8_t*>(mSceneStaging[slot].contents);
        const uint8_t* tableBytes = mSceneStore.tableBytes().data();
        for (const SceneWrite& write : mSceneStore.writes())
        {
            id<MTLBuffer> target = write.table != SceneTable::Count ? mSceneTables[uint32_t(write.table)] : mSceneChunks[write.chunk];
            memcpy(staging + offset, write.data ? write.data : tableBytes + write.source, write.bytes);
            [blit copyFromBuffer:mSceneStaging[slot] sourceOffset:offset toBuffer:target destinationOffset:write.offset size:write.bytes];
            offset += (write.bytes + UPLOAD_ALIGNMENT - 1) / UPLOAD_ALIGNMENT * UPLOAD_ALIGNMENT;
        }

        [blit copyFromBuffer:mSceneCountsTemplate sourceOffset:0 toBuffer:mSceneCounts destinationOffset:0 size:sizeof(GpuSceneCounts)];
        [blit updateFence:mSceneFence];
        [blit endEncoding];

        GpuSceneView view{};
        fillSceneView(view, *mRecordCamera, mHasPyramid ? &mPyramidCamera : nullptr, mSceneWidth, mSceneHeight);
        mSceneStore.fillView(view);
        view.skinAudit = mGraph.isRunning(PassAuditCull) ? 1 : 0;
        for (uint32_t table = SCENE_FIRST_SKIN_TABLE; table < SCENE_TABLES; ++table)
        {
            view.skinTables[table - SCENE_FIRST_SKIN_TABLE] = addressOf(mSceneTables[table]);
        }
        view.time = sceneTime(mRecordTime);
        view.feedback = mFeedbackValue;
        view.debugView = mRecordCamera->debugView;
        mSceneInstanceEnd = view.instanceEnd;
        mPyramidLevels = view.pyramidLevels;
        memcpy(mSceneViews[slot].contents, &view, sizeof(view));

        mScenePush = ScenePush{};
        mScenePush.view = addressOf(mSceneViews[slot]);
        mScenePush.instances = addressOf(mSceneTables[uint32_t(SceneTable::Instances)]);
        mScenePush.geometries = addressOf(mSceneTables[uint32_t(SceneTable::Geometries)]);
        mScenePush.clusters = addressOf(mSceneTables[uint32_t(SceneTable::Clusters)]);
        mScenePush.materials = addressOf(mSceneTables[uint32_t(SceneTable::Materials)]);
        mScenePush.visible = addressOf(mSceneVisible);
        mScenePush.counts = addressOf(mSceneCounts);
        mScenePush.retest = addressOf(mSceneRetest);
        mScenePush.pyramid = addressOf(mScenePyramid);
        mScenePush.groups = addressOf(mSceneGroups);
        mScenePush.feedback = addressOf(mTextureFeedback);
        mScenePush.terrains = addressOf(mSceneTables[uint32_t(SceneTable::Terrains)]);
        mScenePush.macoit = addressOf(mMacOIT.state);
        return true;
    }

    bool passCull(uint32_t phase)
    {
        id<MTLComputeCommandEncoder> cull = beginCompute();
        [cull setComputePipelineState:mCullPipeline];
        mScenePush.phase = phase;
        [cull setBytes:&mScenePush length:sizeof(mScenePush) atIndex:PUSH_CONSTANT_BUFFER];
        if (phase == CULL_LATE)
        {
            [cull dispatchThreadgroupsWithIndirectBuffer:mSceneCounts indirectBufferOffset:offsetof(GpuSceneCounts, retest)
                                   threadsPerThreadgroup:MTLSizeMake(CULL_GROUP_SIZE, 1, 1)];
        }
        else if (phase >= CULL_GROUPS_EARLY)
        {
            const uint32_t work = phase == CULL_GROUPS_LATE ? 1 : 0;
            [cull dispatchThreadgroupsWithIndirectBuffer:mSceneCounts
                                    indirectBufferOffset:offsetof(GpuSceneCounts, groups) + work * sizeof(GpuWorkCount)
                                   threadsPerThreadgroup:MTLSizeMake(CULL_GROUP_SIZE, 1, 1)];
        }
        else if (mSceneInstanceEnd != 0) // an empty dispatch is an error to Metal's validation
        {
            [cull dispatchThreadgroups:MTLSizeMake((mSceneInstanceEnd + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE, 1, 1)
                 threadsPerThreadgroup:MTLSizeMake(CULL_GROUP_SIZE, 1, 1)];
        }
        [cull updateFence:mSceneFence];
        [cull endEncoding];
        return true;
    }

    bool passSkin(uint32_t phase)
    {
        const uint32_t threads = mSceneStore.skinThreads(phase);
        if (threads == 0)
        {
            return true; // an empty dispatch is an error to Metal's validation
        }
        id<MTLComputeCommandEncoder> skin = beginCompute();
        [skin setComputePipelineState:mSkinPipeline];
        mScenePush.phase = phase;
        [skin setBytes:&mScenePush length:sizeof(mScenePush) atIndex:PUSH_CONSTANT_BUFFER];
        [skin dispatchThreadgroups:MTLSizeMake((threads + SKIN_BLOCK - 1) / SKIN_BLOCK, 1, 1) threadsPerThreadgroup:MTLSizeMake(SKIN_BLOCK, 1, 1)];
        [skin updateFence:mSceneFence];
        [skin endEncoding];
        return true;
    }

    bool passDraw(id<MTLTexture> color, uint64_t argumentsOffset, bool loadColor, bool loadDepth)
    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = color;
        pass.colorAttachments[0].loadAction = loadColor ? MTLLoadActionLoad : MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(double(VISIBILITY_EMPTY), 0.0, 0.0, 0.0);
        pass.depthAttachment.texture = mDepthTarget;
        pass.depthAttachment.loadAction = loadDepth ? MTLLoadActionLoad : MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 0.0; // reversed depth: 0 is far away
        if (mPassSamples && mRecordPass < MAX_TIMED_PASSES)
        {
            pass.sampleBufferAttachments[0].sampleBuffer = mPassSamples;
            pass.sampleBufferAttachments[0].startOfVertexSampleIndex = passSample(0);
            pass.sampleBufferAttachments[0].endOfVertexSampleIndex = passSample(2);
            pass.sampleBufferAttachments[0].startOfFragmentSampleIndex = passSample(3);
            pass.sampleBufferAttachments[0].endOfFragmentSampleIndex = passSample(1);
        }

        id<MTLRenderCommandEncoder> draw = [mRecordCommands renderCommandEncoderWithDescriptor:pass];
        draw.label = passLabel();
        [draw waitForFence:mSceneFence beforeStages:MTLRenderStageVertex];
        [draw setDepthStencilState:mDepthState];
        [draw setFrontFacingWinding:MTLWindingCounterClockwise];
        [draw setFragmentBuffer:mArguments[mRecordSlot] offset:0 atIndex:0];
        for (uint32_t kind = 0; kind < SCENE_OPAQUE_KINDS; ++kind) // blended kinds are Mac OIT's
        {
            [draw setRenderPipelineState:(kind & 2) ? mMaskedPipeline : mVisibilityPipeline];
            [draw setCullMode:(kind & 1) ? MTLCullModeNone : MTLCullModeBack];
            for (uint32_t bucket = 0; bucket < CLUSTER_BUCKETS; ++bucket)
            {
                mScenePush.list = sceneList(kind, bucket);
                [draw setVertexBytes:&mScenePush length:sizeof(mScenePush) atIndex:PUSH_CONSTANT_BUFFER];
                [draw drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:mSceneCounts
                  indirectBufferOffset:argumentsOffset + mScenePush.list * sizeof(GpuDrawArguments)];
            }
        }
        [draw updateFence:mSceneFence afterStages:MTLRenderStageFragment];
        [draw endEncoding];
        return true;
    }

    bool passPyramid()
    {
        id<MTLComputeCommandEncoder> encoder = beginCompute();
        [encoder setComputePipelineState:mPyramidPipeline];
        [encoder setTexture:mDepthTarget atIndex:3];
        for (uint32_t source = 0; source < mPyramidLevels; source += PYRAMID_LEVELS_PER_DISPATCH)
        {
            if (source != 0)
            {
                [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
            }
            mScenePush.phase = source;
            [encoder setBytes:&mScenePush length:sizeof(mScenePush) atIndex:PUSH_CONSTANT_BUFFER];
            const uint32_t coarsest = source + PYRAMID_LEVELS_PER_DISPATCH;
            [encoder dispatchThreadgroups:MTLSizeMake((pyramidLevelSize(mSceneWidth, coarsest) + PYRAMID_GROUP_SIZE - 1) / PYRAMID_GROUP_SIZE,
                                                      (pyramidLevelSize(mSceneHeight, coarsest) + PYRAMID_GROUP_SIZE - 1) / PYRAMID_GROUP_SIZE, 1)
                    threadsPerThreadgroup:MTLSizeMake(PYRAMID_GROUP_SIZE, PYRAMID_GROUP_SIZE, 1)];
        }
        [encoder updateFence:mSceneFence];
        [encoder endEncoding];
        return true;
    }

    bool passResolve()
    {
        if (mFeedbackCollector.takeClear())
        {
            // Not timed: the pass's timer samples belong to the resolve.
            id<MTLBlitCommandEncoder> clear = [mRecordCommands blitCommandEncoder];
            [clear waitForFence:mSceneFence];
            [clear fillBuffer:mTextureFeedback range:NSMakeRange(0, FEEDBACK_BYTES) value:0];
            [clear updateFence:mSceneFence];
            [clear endEncoding];
        }
        id<MTLComputeCommandEncoder> resolve = beginCompute();
        [resolve setComputePipelineState:mResolvePipeline];
        [resolve setBuffer:mArguments[mRecordSlot] offset:0 atIndex:0];
        [resolve setBytes:&mScenePush length:sizeof(mScenePush) atIndex:PUSH_CONSTANT_BUFFER];
        [resolve setTexture:mVisibilityTarget atIndex:0];
        [resolve setTexture:mAlbedoTarget atIndex:1];
        [resolve setTexture:mNormalTarget atIndex:2];
        [resolve setTexture:mLitTarget atIndex:8];
        [resolve dispatchThreadgroups:MTLSizeMake((mSceneWidth + RESOLVE_GROUP_SIZE - 1) / RESOLVE_GROUP_SIZE, (mSceneHeight + RESOLVE_GROUP_SIZE - 1) / RESOLVE_GROUP_SIZE, 1)
                threadsPerThreadgroup:MTLSizeMake(RESOLVE_GROUP_SIZE, RESOLVE_GROUP_SIZE, 1)];
        [resolve updateFence:mSceneFence];
        [resolve endEncoding];
        return true;
    }

    bool passReadFeedback()
    {
        id<MTLBlitCommandEncoder> readback = beginBlit();
        [readback copyFromBuffer:mTextureFeedback sourceOffset:0 toBuffer:mFeedbackReadback[mRecordSlot] destinationOffset:0 size:FEEDBACK_BYTES];
        [readback updateFence:mSceneFence];
        [readback endEncoding];
        mFeedbackRead[mRecordSlot] = mFeedbackEnding;
        return true;
    }

    bool passReadCounts()
    {
        id<MTLBlitCommandEncoder> readback = beginBlit();
        [readback copyFromBuffer:mSceneCounts sourceOffset:0 toBuffer:mSceneReadback[mRecordSlot] destinationOffset:0 size:sizeof(GpuSceneCounts)];
        [readback updateFence:mSceneFence];
        [readback endEncoding];
        return true;
    }

    bool passAuditReset()
    {
        id<MTLBlitCommandEncoder> blit = beginBlit();
        [blit copyFromBuffer:mSceneCountsTemplate sourceOffset:0 toBuffer:mSceneCounts destinationOffset:0 size:sizeof(GpuSceneCounts)];
        [blit updateFence:mSceneFence];
        [blit endEncoding];
        return true;
    }

    bool passAuditRead()
    {
        id<MTLBlitCommandEncoder> blit = beginBlit();
        [blit copyFromTexture:mAuditTarget sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(mSceneWidth, mSceneHeight, 1)
                     toBuffer:mAuditReadback[mRecordSlot] destinationOffset:0 destinationBytesPerRow:mSceneWidth * 4 destinationBytesPerImage:uint64_t(mSceneWidth) * mSceneHeight * 4];
        [blit updateFence:mSceneFence];
        [blit endEncoding];
        mAuditWritten[mRecordSlot] = true;
        return true;
    }

    void setPassBodies()
    {
        mGraph.setBody(PassUpload, [this]() { return passUpload(); });
        setMacOITBodies();
        const PassId skinPasses[SKIN_PHASES] = { PassSkinPose, PassSkinPalette, PassSkinBind, PassSkinMorph,
                                               PassSkinVertices, PassSkinClusters, PassSkinGroups, PassSkinVerticesLate, PassSkinAudit };
        for (uint32_t phase = 0; phase < SKIN_PHASES; ++phase)
        {
            mGraph.setBody(skinPasses[phase], [this, phase]()
            {
                return phase == SKIN_AUDIT && !mGraph.isRunning(PassAuditCull) ? true : passSkin(phase);
            });
        }
        mGraph.setBody(PassCullEarly, [this]() { return passCull(CULL_EARLY); });
        mGraph.setBody(PassGroupsEarly, [this]() { return passCull(CULL_GROUPS_EARLY); });
        mGraph.setBody(PassGroupsLate, [this]() { return passCull(CULL_GROUPS_LATE); });
        mGraph.setBody(PassAuditGroups, [this]() { return passCull(CULL_GROUPS_AUDIT); });
        mGraph.setBody(PassDrawEarly, [this]() { return passDraw(mVisibilityTarget, offsetof(GpuSceneCounts, early), false, false); });
        mGraph.setBody(PassPyramidEarly, [this]() { return passPyramid(); });
        mGraph.setBody(PassCullLate, [this]() { return passCull(CULL_LATE); });
        mGraph.setBody(PassDrawLate, [this]() { return passDraw(mVisibilityTarget, offsetof(GpuSceneCounts, late), true, true); });
        mGraph.setBody(PassPyramidFinal, [this]() { return passPyramid(); });
        mGraph.setBody(PassResolve, [this]() { return passResolve(); });
        mGraph.setBody(PassReadCounts, [this]() { return passReadCounts(); });
        mGraph.setBody(PassAuditReset, [this]() { return passAuditReset(); });
        mGraph.setBody(PassAuditCull, [this]() { return passCull(CULL_AUDIT); });
        mGraph.setBody(PassAuditDraw, [this]() { return passDraw(mAuditTarget, offsetof(GpuSceneCounts, early), false, true); });
        mGraph.setBody(PassAuditRead, [this]() { return passAuditRead(); });
        mGraph.setBody(PassReadFeedback, [this]() { return passReadFeedback(); });
    }

    // What this slot's earlier frame left behind. That frame has finished.
    void readSceneResults(uint32_t slot)
    {
        const uint32_t timed = mTimedPasses[slot];
        if (timed != 0 && mPassSamples)
        {
            // The two clocks are compared over as long a span as there is.
            MTLTimestamp cpu = 0;
            MTLTimestamp gpu = 0;
            [mDevice sampleTimestamps:&cpu gpuTimestamp:&gpu];
            const double millisecondsPerTick = gpu > mClockStartGpu ? double(cpu - mClockStartCpu) / double(gpu - mClockStartGpu) * 1e-6 : 0.0;

            NSData* data = [mPassSamples resolveCounterRange:NSMakeRange(slot * MAX_TIMED_PASSES * SAMPLES_PER_PASS, timed * SAMPLES_PER_PASS)];
            const MTLCounterResultTimestamp* samples = data ? static_cast<const MTLCounterResultTimestamp*>(data.bytes) : nullptr;
            if (samples && millisecondsPerTick > 0.0)
            {
                mSceneFrameStats.passCount = timed;
                for (uint32_t i = 0; i < timed; ++i)
                {
                    const MTLCounterResultTimestamp* pass = samples + i * SAMPLES_PER_PASS;
                    const bool isDraw = pass[2].timestamp != MTLCounterErrorValue && pass[2].timestamp != 0;
                    const double milliseconds = double(pass[1].timestamp - pass[0].timestamp) * millisecondsPerTick;
                    const double vertices = isDraw ? double(pass[2].timestamp - pass[0].timestamp) * millisecondsPerTick : 0.0;
                    recordPassTime(mSceneFrameStats.passes[i], mTimedNames[slot][i], milliseconds, vertices);
                }
            }
        }
        if (timed != 0)
        {
            const GpuSceneCounts* counts = static_cast<const GpuSceneCounts*>(mSceneReadback[slot].contents);
            mSceneFrameStats.earlyClusters = 0;
            mSceneFrameStats.lateClusters = 0;
            for (uint32_t list = 0; list < SCENE_LISTS; ++list)
            {
                mSceneFrameStats.earlyClusters += counts->early[list].instanceCount;
                mSceneFrameStats.lateClusters += counts->late[list].instanceCount;
            }
            mSceneFrameStats.retestEntries = counts->retest.entries;
            mSceneFrameStats.groupEntries = uint64_t(counts->groups[0].entries) + counts->groups[1].entries;
            mSceneFrameStats.guardBits |= counts->guardBits;
            mSceneFrameStats.guardPixels += counts->guardPixels;
            mSceneFrameStats.guardValue = std::max(mSceneFrameStats.guardValue, counts->guardValue);
            mSceneFrameStats.droppedEntries += counts->dropped;
            mSceneFrameStats.skinAuditSamples += counts->skinAuditSamples;
            mSceneFrameStats.skinAuditErrors += counts->skinAuditErrors;
            float skinError;
            memcpy(&skinError, &counts->skinAuditMaxError, sizeof(skinError));
            mSceneFrameStats.skinAuditMaxError = std::max(mSceneFrameStats.skinAuditMaxError, skinError);
        }
        if (mFeedbackRead[slot].number != 0)
        {
            publishTextureFeedback(static_cast<const uint32_t*>(mFeedbackReadback[slot].contents), mFeedbackRead[slot]);
            mFeedbackRead[slot] = {};
        }
        if (mAuditWritten[slot])
        {
            mAuditWritten[slot] = false;
            const uint64_t pixels = uint64_t(mSceneWidth) * mSceneHeight;
            const uint32_t* words = static_cast<const uint32_t*>(mAuditReadback[slot].contents);
            mSceneFrameStats.auditErrorPixels += uint64_t(pixels - std::count(words, words + pixels, VISIBILITY_EMPTY));
            ++mSceneFrameStats.auditedFrames;
        }
    }

    bool ensureAuditTargets(uint32_t slot)
    {
        if (!mAuditTarget)
        {
            mAuditTarget = createTarget(MTLPixelFormatR32Uint, MTLTextureUsageRenderTarget, mSceneWidth, mSceneHeight, 4, "audit target");
        }
        const uint64_t bytes = uint64_t(mSceneWidth) * mSceneHeight * sizeof(uint32_t);
        if (mAuditTarget && mAuditReadback[slot].length < bytes)
        {
            if (mAuditReadback[slot])
            {
                releaseSceneBuffer(mAuditReadback[slot], MemoryCategory::Staging);
            }
            mAuditReadback[slot] = createSceneBuffer(bytes, MemoryCategory::Staging, "audit readback");
        }
        return mAuditTarget && mAuditReadback[slot];
    }

    // Runs the frame graph: uploads, two phases of culling and drawing, and the resolve.
    // Metal orders the passes by mSceneFence, so the graph's transitions are not needed here.
    bool recordScene(id<MTLCommandBuffer> commands, uint32_t slot, const SceneCamera3D& camera, uint32_t width, uint32_t height, bool audit)
    {
        readSceneResults(slot);
        if (!ensureSceneTargets(width, height))
        {
            return false;
        }
        const bool readFeedback = mFeedbackCollector.frame(std::chrono::steady_clock::now(), mFeedbackValue, mFeedbackEnding);
        mGraph.setSink(ResultAuditPixels, GraphAccess::CopyRead, audit);
        mGraph.setSink(ResultFeedbackReadback, GraphAccess::CopyRead, readFeedback);
        if (!mGraph.compile(mError) || (audit && !ensureAuditTargets(slot)))
        {
            return false;
        }

        mRecordCommands = commands;
        mRecordSlot = slot;
        mRecordCamera = &camera;
        mRecordTime = std::chrono::steady_clock::now(); // what avatars' motion is carried on to
        for (uint32_t i = 0; i < mGraph.passCount(); ++i)
        {
            mRecordPass = i;
            if (!mGraph.run(i))
            {
                return false;
            }
            if (i < MAX_TIMED_PASSES)
            {
                mTimedNames[slot][i] = mGraph.passName(i);
            }
        }
        mTimedPasses[slot] = std::min(mGraph.passCount(), MAX_TIMED_PASSES);
        mRecordCommands = nil;

        mPyramidCamera = camera;
        mHasPyramid = mOcclusion;

        mSceneFrameStats.instances = mSceneInstanceEnd;
        mSceneFrameStats.triangles = mSceneStore.triangles();
        mSceneFrameStats.pendingGeometries = mSceneStore.pendingGeometries();
        mSceneFrameStats.chunks = mSceneStore.chunkCount();
        mSceneFrameStats.paletteEnd = mSceneStore.rangeEnd(SceneTable::Palette);
        mSceneFrameStats.paletteLive = mSceneStore.rangeLive(SceneTable::Palette);
        mSceneFrameStats.vertexBlockEnd = mSceneStore.rangeEnd(SceneTable::SkinBlocks);
        mSceneFrameStats.vertexBlockLive = mSceneStore.rangeLive(SceneTable::SkinBlocks);
        mSceneFrameStats.clusterBlockEnd = mSceneStore.rangeEnd(SceneTable::ClusterBlocks);
        mSceneFrameStats.clusterBlockLive = mSceneStore.rangeLive(SceneTable::ClusterBlocks);
        mSceneStore.memoryStats(mSceneFrameStats.chunkBytes, mSceneFrameStats.chunkFreeBytes, mSceneFrameStats.tableBytes, mSceneFrameStats.skinnedPositionBytes,
                                mSceneFrameStats.clusterTableBytes);
        mSceneFrameStats.textureCopiesRebased = mTextureCopiesRebased;
        mSceneFrameStats.textureCopiesRejected = mTextureCopiesRejected;
        publishSceneStats(mSceneFrameStats);
        return true;
    }

    // Creating a pipeline is slow, so each is made once, on first use, and UI code
    // only ever uses a handful.
    id<MTLRenderPipelineState> uiPipeline(const UIBlend& blend)
    {
        const uint32_t key = blend.key();
        const auto found = mUIPipelines.find(key);
        if (found != mUIPipelines.end())
        {
            return found->second;
        }

        MTLVertexDescriptor* vertex = [MTLVertexDescriptor vertexDescriptor];
        vertex.attributes[0].format = MTLVertexFormatFloat4;
        vertex.attributes[0].offset = offsetof(UIVertex, position);
        vertex.attributes[1].format = MTLVertexFormatFloat2;
        vertex.attributes[1].offset = offsetof(UIVertex, uv);
        vertex.attributes[2].format = MTLVertexFormatUChar4Normalized;
        vertex.attributes[2].offset = offsetof(UIVertex, color);
        vertex.attributes[3].format = MTLVertexFormatUInt;
        vertex.attributes[3].offset = offsetof(UIVertex, texture);
        for (NSUInteger i = 0; i < 4; ++i)
        {
            vertex.attributes[i].bufferIndex = UI_VERTEX_BUFFER;
        }
        vertex.layouts[UI_VERTEX_BUFFER].stride = sizeof(UIVertex);

        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = mUIVertexFunction;
        descriptor.fragmentFunction = mUIFragmentFunction;
        descriptor.vertexDescriptor = vertex;
        MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
        color.pixelFormat = mLayer.pixelFormat;
        color.blendingEnabled = YES;
        color.sourceRGBBlendFactor = toMetal(blend.colorSource);
        color.destinationRGBBlendFactor = toMetal(blend.colorDest);
        color.sourceAlphaBlendFactor = toMetal(blend.alphaSource);
        color.destinationAlphaBlendFactor = toMetal(blend.alphaDest);
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float; // the UI depth

        NSError* failure = nil;
        id<MTLRenderPipelineState> pipeline = [mDevice newRenderPipelineStateWithDescriptor:descriptor error:&failure];
        if (!pipeline)
        {
            mError = std::string("UI pipeline: ") + failure.localizedDescription.UTF8String;
            return nil;
        }
        mUIPipelines[key] = pipeline;
        return pipeline;
    }

    // Copies a new list to this frame's vertex buffer. A list that has not changed
    // costs nothing here, which is what lets the render thread replay it at world rate.
    bool prepareUI(uint32_t slot, const UIList* ui)
    {
        if (!ui || ui->vertices().empty())
        {
            return true;
        }
        if (mUIBufferGeneration[slot] != ui->generation)
        {
            const uint64_t bytes = ui->vertices().size() * sizeof(UIVertex);
            if (mUIBuffers[slot].length < bytes)
            {
                const uint64_t oldSize = mUIBuffers[slot].length;
                const uint64_t size = std::max<uint64_t>(std::max(bytes, oldSize * 2), 1024 * 1024);
                mMemoryBudget.release(MemoryCategory::UI, oldSize);
                mUIBuffers[slot] = nil;
                // Every frame needs it: counted anyway, and textures and avatars give way.
                mMemoryBudget.reserveAnyway(MemoryCategory::UI, size);
                mUIBuffers[slot] = [mDevice newBufferWithLength:size options:MTLResourceStorageModeShared];
                if (!mUIBuffers[slot])
                {
                    mMemoryBudget.release(MemoryCategory::UI, size);
                    mError = "cannot create the UI vertex buffer";
                    return false;
                }
            }
            memcpy(mUIBuffers[slot].contents, ui->vertices().data(), bytes);
            mUIBufferGeneration[slot] = ui->generation;
        }
        if (mUIPipelineGeneration != ui->generation)
        {
            mUIDrawPipelines.resize(ui->blends().size());
            for (size_t i = 0; i < ui->blends().size(); ++i)
            {
                mUIDrawPipelines[i] = uiPipeline(ui->blends()[i]);
                if (!mUIDrawPipelines[i])
                {
                    return false;
                }
            }
            mUIPipelineGeneration = ui->generation;
        }
        return true;
    }

    id<MTLDepthStencilState> uiDepthState(const UIDepth& depth)
    {
        const uint32_t key = (depth.test ? 16u : 0u) | (depth.write ? 8u : 0u) | (uint32_t(depth.compare) & 7);
        if (!mUIDepthStates[key])
        {
            static constexpr MTLCompareFunction COMPARES[8] = {
                MTLCompareFunctionNever,   MTLCompareFunctionLess,     MTLCompareFunctionEqual,        MTLCompareFunctionLessEqual,
                MTLCompareFunctionGreater, MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways };
            MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
            descriptor.depthCompareFunction = depth.test ? COMPARES[key & 7] : MTLCompareFunctionAlways;
            descriptor.depthWriteEnabled = depth.test && depth.write;
            mUIDepthStates[key] = [mDevice newDepthStencilStateWithDescriptor:descriptor];
        }
        return mUIDepthStates[key];
    }

    // The window pass's depth, as Vulkan's UI depth: see FrameInput and asruilist.h.
    bool ensureUIDepth(uint32_t width, uint32_t height)
    {
        if (mUIDepth && mUIDepth.width == width && mUIDepth.height == height)
        {
            return true;
        }
        mUIDepth = nil;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                              width:width height:height mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget;
        // Apple GPUs keep it on chip: it never reaches memory, so it costs none.
        descriptor.storageMode = [mDevice supportsFamily:MTLGPUFamilyApple1] ? MTLStorageModeMemoryless : MTLStorageModePrivate;
        mUIDepth = [mDevice newTextureWithDescriptor:descriptor];
        if (!mUIDepth)
        {
            mError = "cannot create the UI depth";
            return false;
        }
        return true;
    }

    // A fullscreen depth write over the window: the scene depth (phase 0) or farthest (1).
    void writeUIDepth(id<MTLRenderCommandEncoder> encoder, uint32_t phase, uint32_t width, uint32_t height)
    {
        [encoder setScissorRect:MTLScissorRect{ 0, 0, width, height }];
        UIDepth always;
        always.test = true;
        always.write = true;
        always.compare = UICompare::Always;
        [encoder setRenderPipelineState:mDepthCopyPipeline];
        [encoder setDepthStencilState:uiDepthState(always)];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setFragmentTexture:mDepthTarget atIndex:3];
        ScenePush push{};
        push.phase = phase;
        [encoder setFragmentBytes:&push length:sizeof(push) atIndex:PUSH_CONSTANT_BUFFER];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }

    // The draws from first to end.
    void drawUI(id<MTLRenderCommandEncoder> encoder, uint32_t slot, const UIList& ui, uint32_t width, uint32_t height,
                uint32_t first, uint32_t end)
    {
        [encoder setVertexBuffer:mUIBuffers[slot] offset:0 atIndex:UI_VERTEX_BUFFER];
        [encoder setFragmentBuffer:mArguments[slot] offset:0 atIndex:0];
        [encoder setViewport:MTLViewport{ 0.0, UI_EDGE_NUDGE, double(ui.width()), double(ui.height()), 0.0, 1.0 }];
        // GL's counter-clockwise front: the Metal shader build undoes asrUIV.glsl's Y flip.
        [encoder setFrontFacingWinding:MTLWindingCounterClockwise];

        static constexpr MTLCullMode CULLS[3] = { MTLCullModeNone, MTLCullModeFront, MTLCullModeBack };
        id<MTLRenderPipelineState> bound = nil;
        const std::vector<UIDraw>& draws = ui.draws();
        for (uint32_t i = first; i < end && i < draws.size(); ++i)
        {
            const UIDraw& draw = draws[i];
            if (draw.clearDepth)
            {
                writeUIDepth(encoder, 1, width, height);
                bound = nil;
                continue;
            }
            UIRect rect;
            if (!resolveScissor(draw.scissor, ui.width(), ui.height(), width, height, rect))
            {
                continue;
            }
            id<MTLRenderPipelineState> pipeline = mUIDrawPipelines[draw.blend];
            if (pipeline != bound)
            {
                [encoder setRenderPipelineState:pipeline];
                bound = pipeline;
            }
            // Cheap to set: the states are made once each, and Metal keeps what is current.
            [encoder setDepthStencilState:uiDepthState(draw.depth)];
            [encoder setCullMode:CULLS[uint32_t(draw.cull)]];
            [encoder setScissorRect:MTLScissorRect{ rect.x, rect.y, rect.width, rect.height }];
            const MTLPrimitiveType type = draw.primitive == UIPrimitive::Triangles ? MTLPrimitiveTypeTriangle
                                        : draw.primitive == UIPrimitive::Lines     ? MTLPrimitiveTypeLine
                                                                                   : MTLPrimitiveTypePoint;
            [encoder drawPrimitives:type vertexStart:draw.firstVertex vertexCount:draw.vertexCount];
        }
    }

    // The memory was counted when the main thread asked for this texture. Video memory
    // really running out shows as white and is not an error.
    void createTexture(const TextureCommand& command, id<MTLBlitCommandEncoder> blit)
    {
        mTextureReplacements.begin(command.slot);
        if (mTextureObjects.size() <= command.slot)
        {
            mTextureObjects.resize(command.slot + 1);
        }
        const uint64_t bytes = textureBytes(command.desc);
        const TextureDesc& desc = command.desc;
        if (command.replace)
        {
            const auto pending = mPendingTextures.find(command.slot);
            if (pending != mPendingTextures.end())
            {
                freeTexture(pending->second);
                mPendingTextures.erase(pending);
            }
        }
        uint32_t sourceMip = 0;
        id<MTLTexture> source = nil;
        if (command.sourceSlot != 0)
        {
            const Texture* resident = command.sourceSlot < mTextureObjects.size() ? &mTextureObjects[command.sourceSlot] : nullptr;
            if (!resident || !resident->texture || resident->desc.format != desc.format || resident->desc.swizzle != desc.swizzle ||
                !textureMipCopyLevel(uint32_t(resident->texture.width), uint32_t(resident->texture.height), uint32_t(resident->texture.mipmapLevelCount),
                                     desc.width, desc.height, desc.mipLevels, sourceMip))
            {
                ++mTextureCopiesRejected;
                if (command.replace) mTextureReplacements.fail(command.slot);
                mMemoryBudget.release(MemoryCategory::Textures, bytes);
                return; // keep the published image; never publish an unfilled shrink
            }
            mTextureCopiesRebased += sourceMip != command.sourceMip ? 1 : 0;
            if (command.replace && command.sourceSlot == command.slot && sourceMip == 0 && resident->desc.mipLevels == desc.mipLevels)
            {
                mMemoryBudget.release(MemoryCategory::Textures, bytes);
                return; // the published image already has exactly the requested size
            }
            source = resident->texture;
        }
        const MTLPixelFormat format = desc.format == TextureFormat::R8    ? MTLPixelFormatR8Unorm
                                    : desc.format == TextureFormat::RG8   ? MTLPixelFormatRG8Unorm
                                    : desc.format == TextureFormat::BGRA8 ? MTLPixelFormatBGRA8Unorm
                                    : desc.format == TextureFormat::BC1   ? MTLPixelFormatBC1_RGBA
                                    : desc.format == TextureFormat::BC4   ? MTLPixelFormatBC4_RUnorm
                                    : desc.format == TextureFormat::BC5   ? MTLPixelFormatBC5_RGUnorm
                                    : desc.format == TextureFormat::BC7   ? MTLPixelFormatBC7_RGBAUnorm
                                                                          : MTLPixelFormatRGBA8Unorm;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                              width:desc.width
                                                                                             height:desc.height
                                                                                          mipmapped:NO];
        descriptor.mipmapLevelCount = desc.mipLevels;
        descriptor.storageMode = MTLStorageModePrivate; // filled by blits, and kept compressed by the GPU
        descriptor.usage = MTLTextureUsageShaderRead;
        if (desc.swizzle == TextureSwizzle::AlphaOnly)
        {
            descriptor.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleZero, MTLTextureSwizzleZero,
                                                               MTLTextureSwizzleZero, MTLTextureSwizzleRed);
        }
        else if (desc.swizzle == TextureSwizzle::Luminance)
        {
            descriptor.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed,
                                                               MTLTextureSwizzleRed, MTLTextureSwizzleOne);
        }
        else if (desc.swizzle == TextureSwizzle::LuminanceAlpha)
        {
            descriptor.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed,
                                                               MTLTextureSwizzleRed, MTLTextureSwizzleGreen);
        }

        id<MTLTexture> texture = [mDevice newTextureWithDescriptor:descriptor];
        if (!texture)
        {
            if (command.replace) mTextureReplacements.fail(command.slot);
            mMemoryBudget.release(MemoryCategory::Textures, bytes);
            if (!command.replace)
            {
                mSlotCopies.changed(command.slot); // reads white; a replaced slot keeps its old image
            }
            return;
        }

        const uint32_t pixelBytes = textureBytesPerPixel(desc.format);
        Texture made{ texture, bytes, pixelBytes, desc };
        // For a replacement the source is the slot's own image, which is still showing.
        if (command.sourceSlot != 0)
        {
            // In command order, so after any mips made from the source just before.
            for (NSUInteger level = 0; level < desc.mipLevels; ++level)
            {
                const MTLSize size = MTLSizeMake(std::max<NSUInteger>(desc.width >> level, 1), std::max<NSUInteger>(desc.height >> level, 1), 1);
                [blit copyFromTexture:source
                          sourceSlice:0
                          sourceLevel:sourceMip + level
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:size
                            toTexture:texture
                     destinationSlice:0
                     destinationLevel:level
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
            }
        }
        else if (textureCompressed(desc.format))
        {
            // Cleared, because a list can draw this texture before its pixels arrive.
            if (!clearCompressedTexture(made, blit, command.encoded))
            {
                if (command.replace) mTextureReplacements.fail(command.slot);
                mMemoryBudget.release(MemoryCategory::Textures, bytes);
                if (!command.replace) mSlotCopies.changed(command.slot);
                return;
            }
        }
        else
        {
            // Cleared, because a list can draw this texture before its pixels arrive.
            uint32_t width = desc.width;
            uint32_t height = desc.height;
            for (uint32_t level = 0; level < desc.mipLevels; ++level)
            {
                const NSUInteger rowBytes = NSUInteger(width) * pixelBytes;
                const uint32_t rowsPerCopy = uint32_t(std::max<NSUInteger>(ZERO_BYTES / rowBytes, 1));
                for (uint32_t row = 0; row < height; row += rowsPerCopy)
                {
                    const uint32_t rows = std::min(rowsPerCopy, height - row);
                    [blit copyFromBuffer:mZeros
                               sourceOffset:0
                          sourceBytesPerRow:rowBytes
                        sourceBytesPerImage:rowBytes * rows
                                 sourceSize:MTLSizeMake(width, rows, 1)
                                  toTexture:texture
                           destinationSlice:0
                           destinationLevel:level
                          destinationOrigin:MTLOriginMake(0, row, 0)];
                }
                width = std::max(width / 2, 1u);
                height = std::max(height / 2, 1u);
            }
        }

        if (@available(macOS 15.0, *))
        {
            [(id<MTLResidencySet>)mResidency addAllocation:texture];
        }
        if (command.replace)
        {
            // Built out of sight: the slot keeps its image until publish.
            mPendingTextures[command.slot] = made;
            return;
        }
        mTextureObjects[command.slot] = made;
        // The main thread hands out a slot at once, so a material can name it before its
        // texture is created here, and a slot never written holds no texture: a GPU page
        // fault, 'null texture access'. White, the first texture made, fills every slot
        // that has none.
        if (command.slot == 0)
        {
            mSlotCopies.changedAll();
        }
        else
        {
            mSlotCopies.changed(command.slot);
        }
    }

    struct Texture; // below, with the other members

    // Where uploads and mips to a slot go: its replacement while one is being built.
    const Texture& textureFor(uint32_t slot) const
    {
        static const Texture none;
        if (mTextureReplacements.failed(slot)) return none;
        const auto pending = mPendingTextures.find(slot);
        return pending != mPendingTextures.end() ? pending->second : slot < mTextureObjects.size() ? mTextureObjects[slot] : none;
    }

    // The slot shows its replacement from this frame on, and the old image goes once
    // every frame's array has let go of it.
    void publishTexture(uint32_t slot)
    {
        if (mTextureReplacements.failed(slot)) return;
        const auto pending = mPendingTextures.find(slot);
        if (pending == mPendingTextures.end())
        {
            return;
        }
        Texture& current = mTextureObjects[slot];
        if (current.texture)
        {
            freeTexture(current);
        }
        current = pending->second;
        mPendingTextures.erase(pending);
        mSlotCopies.changed(slot);
    }

    // Brings this frame's texture array up to date, now that the frame that last used it
    // has finished, and after this frame's new textures were made resident.
    void bringSlots(uint32_t frameSlot)
    {
        id<MTLTexture> white = mTextureObjects.empty() ? nil : mTextureObjects[0].texture;
        if (!white)
        {
            return; // nothing is drawn with textures before white exists
        }
        [mArgumentEncoder setArgumentBuffer:mArguments[frameSlot] offset:0];
        const auto textureOf = [&](uint32_t slot) -> id<MTLTexture>
        {
            return slot < mTextureObjects.size() && mTextureObjects[slot].texture ? mTextureObjects[slot].texture : white;
        };
        mSlotCopies.bring(frameSlot,
            [&](uint32_t slot) { [mArgumentEncoder setTexture:textureOf(slot) atIndex:slot]; },
            [&]()
            {
                std::vector<id<MTLTexture>> textures(MAX_TEXTURES);
                for (uint32_t slot = 0; slot < MAX_TEXTURES; ++slot)
                {
                    textures[slot] = textureOf(slot);
                }
                [mArgumentEncoder setTextures:textures.data() withRange:NSMakeRange(0, MAX_TEXTURES)];
            });
    }

    // A material can still name this slot for a moment, so the slot reads white from now
    // on, in each frame's array as that frame starts. The texture itself goes once every
    // array has let go of it and the frames that used them have finished: the argument
    // buffer keeps nothing alive, and freeing it under them was a GPU page fault.
    void destroyTexture(uint32_t slot)
    {
        mTextureReplacements.begin(slot);
        mSlotCopies.changed(slot);
        const auto pending = mPendingTextures.find(slot);
        if (pending != mPendingTextures.end())
        {
            freeTexture(pending->second);
            mPendingTextures.erase(pending);
        }
        Texture& texture = mTextureObjects[slot];
        if (texture.texture)
        {
            freeTexture(texture);
        }
        texture = Texture();
    }

    // The texture goes once no recorded frame can sample it, with any buffers of an encode
    // that never ran.
    void freeTexture(Texture& texture)
    {
        mFreedTextures.push_back({ texture.texture, mFrame + FRAMES_IN_FLIGHT + 1, texture.bytes });
        retireEncodeBuffers(texture);
    }

    // A nonzero readBack hands the blocks to the texture queue once the frame is done.
    void retireEncodeBuffers(Texture& texture, uint64_t readBack = 0)
    {
        if (texture.encodeTexels || texture.encodeBlocks)
        {
            mFreedEncodes.push_back({ texture.encodeTexels, texture.encodeBlocks, mFrame + FRAMES_IN_FLIGHT + 1, readBack, texture.desc });
        }
        texture.encodeTexels = nil;
        texture.encodeBlocks = nil;
    }

    // Copies every mip's blocks from encodeBlocks into the texture.
    void copyBlocksToTexture(const Texture& texture, id<MTLBlitCommandEncoder> blit)
    {
        const TextureDesc& desc = texture.desc;
        for (uint32_t level = 0; level < desc.mipLevels; ++level)
        {
            const uint32_t width = std::max(desc.width >> level, 1u);
            const uint32_t height = std::max(desc.height >> level, 1u);
            const NSUInteger rowBytes = NSUInteger((width + 3) / 4) * textureBlockBytes(desc.format);
            [blit copyFromBuffer:texture.encodeBlocks
                       sourceOffset:textureMipOffset(desc, level, true)
                  sourceBytesPerRow:rowBytes
                sourceBytesPerImage:rowBytes * ((height + 3) / 4)
                         sourceSize:MTLSizeMake(width, height, 1)
                          toTexture:texture.texture
                   destinationSlice:0
                   destinationLevel:level
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
    }

    // A compressed texture is cleared with transparent blocks, from the block buffer its
    // encoding will use; uploads go to its RGBA8 texels. See the Vulkan backend.
    bool clearCompressedTexture(Texture& texture, id<MTLBlitCommandEncoder> blit, bool encoded)
    {
        TextureDesc texels = texture.desc;
        texels.format = TextureFormat::RGBA8;
        texture.encodeTexels = encoded ? nil : createSceneBuffer(textureBytes(texels), MemoryCategory::Staging, "texels to encode", true);
        texture.encodeBlocks = encoded || texture.encodeTexels ? createSceneBuffer(texture.bytes, MemoryCategory::Staging, "encoded blocks", true) : nil;
        if (!texture.encodeBlocks)
        {
            retireEncodeBuffers(texture);
            return false;
        }
        // BC1 with equal endpoints and every index 3 is transparent; zero is in the others.
        [blit fillBuffer:texture.encodeBlocks range:NSMakeRange(0, texture.encodeBlocks.length) value:texture.desc.format == TextureFormat::BC1 ? 0xFF : 0];
        copyBlocksToTexture(texture, blit);
        return true;
    }

    uint64_t encodeTexels(uint32_t slot) const
    {
        const Texture& texture = textureFor(slot);
        return texture.encodeTexels ? texture.encodeTexels.length / 4 : 0;
    }

    // In place of making mips: the RGBA8 mips, then every mip's blocks, then the copy into
    // the texture, between the blit encoders of the texture commands. Buffers reached by
    // address are not tracked, so a fence orders the encoders and barriers the dispatches.
    id<MTLBlitCommandEncoder> encodeTexture(uint32_t slot, id<MTLCommandBuffer> commands, id<MTLBlitCommandEncoder> blit, uint64_t readBack)
    {
        Texture& texture = mPendingTextures.count(slot) ? mPendingTextures[slot] : mTextureObjects[slot];
        if (mTextureReplacements.failed(slot) || !texture.texture || !texture.encodeBlocks)
        {
            return blit;
        }
        if (!texture.encodeTexels)
        {
            // Encoded before: the uploads filled the blocks, in this same blit encoder.
            copyBlocksToTexture(texture, blit);
            retireEncodeBuffers(texture);
            return blit;
        }
        [blit updateFence:mEncodeFence];
        [blit endEncoding];

        TextureDesc texels = texture.desc;
        texels.format = TextureFormat::RGBA8;
        const auto sizeOf = [&texture](uint32_t level) {
            return uint32_t(std::max(texture.desc.width >> level, 1u)) | (uint32_t(std::max(texture.desc.height >> level, 1u)) << 16);
        };
        const uint64_t texelsAt = addressOf(texture.encodeTexels);
        const uint64_t blocksAt = addressOf(texture.encodeBlocks);
        ScenePush push{};
        id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
        compute.label = @"texture encoding";
        [compute waitForFence:mEncodeFence];
        [compute setComputePipelineState:mTextureMipPipeline];
        for (uint32_t level = 1; level < texture.desc.mipLevels; ++level)
        {
            push.view = texelsAt + textureMipOffset(texels, level - 1, false);
            push.instances = texelsAt + textureMipOffset(texels, level, false);
            push.list = sizeOf(level - 1);
            [compute setBytes:&push length:sizeof(push) atIndex:PUSH_CONSTANT_BUFFER];
            const NSUInteger width = std::max(texture.desc.width >> level, 1u);
            const NSUInteger height = std::max(texture.desc.height >> level, 1u);
            [compute dispatchThreadgroups:MTLSizeMake((width + 7) / 8, (height + 7) / 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [compute memoryBarrierWithScope:MTLBarrierScopeBuffers];
        }
        const TextureFormat format = texture.desc.format;
        [compute setComputePipelineState:format == TextureFormat::BC1 ? mEncodeBC1Pipeline : format == TextureFormat::BC7 ? mEncodeBC7Pipeline : mEncodeBC45Pipeline];
        push.phase = format == TextureFormat::BC5 ? 2 : 1; // channels, for BC4 and BC5
        for (uint32_t level = 0; level < texture.desc.mipLevels; ++level)
        {
            push.view = texelsAt + textureMipOffset(texels, level, false);
            push.instances = blocksAt + textureMipOffset(texture.desc, level, true);
            push.list = sizeOf(level);
            [compute setBytes:&push length:sizeof(push) atIndex:PUSH_CONSTANT_BUFFER];
            const NSUInteger blocks = NSUInteger((std::max(texture.desc.width >> level, 1u) + 3) / 4) * ((std::max(texture.desc.height >> level, 1u) + 3) / 4);
            [compute dispatchThreadgroups:MTLSizeMake((blocks + ENCODE_GROUP_SIZE - 1) / ENCODE_GROUP_SIZE, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(ENCODE_GROUP_SIZE, 1, 1)];
        }
        [compute updateFence:mEncodeFence];
        [compute endEncoding];

        blit = [commands blitCommandEncoder];
        blit.label = @"texture commands";
        [blit waitForFence:mEncodeFence];
        copyBlocksToTexture(texture, blit);
        mEncodedTexels += encodeTexels(slot);
        ++mTexturesEncoded;
        retireEncodeBuffers(texture, readBack); // shared memory: read once the frame is done
        return blit;
    }

    // Only what is left of the texture queue, for now: the rest of the parts are Vulkan's.
    FrameParts frameParts() const override
    {
        FrameParts parts;
        parts.textureCommandsLeft = uint32_t(mTextureCommands.size() - mTextureCommandsDone);
        return parts;
    }

    // Tests: a mip's blocks (or texels) as stored, copied to shared memory. Waits for the GPU.
    bool readTextureData(uint32_t slot, uint32_t level, std::vector<uint8_t>& data) override
    {
        if (slot >= mTextureObjects.size() || !mTextureObjects[slot].texture || level >= mTextureObjects[slot].desc.mipLevels)
        {
            return false;
        }
        const Texture& texture = mTextureObjects[slot];
        const TextureDesc& desc = texture.desc;
        const uint32_t width = std::max(desc.width >> level, 1u);
        const uint32_t height = std::max(desc.height >> level, 1u);
        const bool blocks = textureCompressed(desc.format);
        const NSUInteger rowBytes = blocks ? NSUInteger((width + 3) / 4) * textureBlockBytes(desc.format) : NSUInteger(width) * texture.pixelBytes;
        const NSUInteger rows = blocks ? (height + 3) / 4 : height;
        id<MTLBuffer> buffer = [mDevice newBufferWithLength:rowBytes * rows options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> commands = [mQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromTexture:texture.texture
                         sourceSlice:0
                         sourceLevel:level
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(width, height, 1)
                            toBuffer:buffer
                   destinationOffset:0
              destinationBytesPerRow:rowBytes
            destinationBytesPerImage:rowBytes * rows];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted)
        {
            return false;
        }
        const uint8_t* bytes = static_cast<const uint8_t*>(buffer.contents);
        data.assign(bytes, bytes + rowBytes * rows);
        return true;
    }

    // Lets go of textures that no recorded frame can sample any more.
    void releaseFreedTextures()
    {
        for (size_t i = 0; i < mFreedEncodes.size();)
        {
            if (mFrame < mFreedEncodes[i].frame)
            {
                ++i;
                continue;
            }
            if (mFreedEncodes[i].readBack != 0 && mFreedEncodes[i].blocks)
            {
                TextureQueue::EncodedTexture encoded;
                encoded.token = mFreedEncodes[i].readBack;
                encoded.desc = mFreedEncodes[i].desc;
                const uint8_t* bytes = static_cast<const uint8_t*>(mFreedEncodes[i].blocks.contents);
                encoded.blocks.assign(bytes, bytes + textureBytes(encoded.desc));
                mTextures.addEncoded(std::move(encoded));
            }
            for (id<MTLBuffer> buffer : { mFreedEncodes[i].texels, mFreedEncodes[i].blocks })
            {
                if (buffer) releaseSceneBuffer(buffer, MemoryCategory::Staging);
            }
            mFreedEncodes[i] = mFreedEncodes.back();
            mFreedEncodes.pop_back();
        }
        bool removed = false;
        for (size_t i = 0; i < mFreedTextures.size();)
        {
            if (mFrame < mFreedTextures[i].frame)
            {
                ++i;
                continue;
            }
            if (@available(macOS 15.0, *))
            {
                [(id<MTLResidencySet>)mResidency removeAllocation:mFreedTextures[i].texture];
                removed = true;
            }
            mMemoryBudget.release(MemoryCategory::Textures, mFreedTextures[i].bytes);
            mFreedTextures[i] = mFreedTextures.back();
            mFreedTextures.pop_back();
        }
        if (removed)
        {
            if (@available(macOS 15.0, *))
            {
                [(id<MTLResidencySet>)mResidency commit];
            }
        }
    }

    // Runs what the main thread queued, in order, in one blit committed before the
    // frame's own commands, until this frame's time for it or its share of the ring
    // runs out. The rest waits for the next frame.
    bool runTextureCommands()
    {
        mTextures.take(mTextureCommands);
        if (mTextureCommandsDone == mTextureCommands.size())
        {
            return true;
        }

        using Clock = std::chrono::steady_clock;
        const Clock::time_point start = Clock::now();
        const auto limit = std::chrono::duration<double>(textureCommandSeconds(mFrameSeconds));
        id<MTLCommandBuffer> commands = newCommandBuffer();
        watchErrors(commands, "texture");
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        blit.label = @"texture commands";
        bool created = false;
        uint64_t uploaded = 0;
        mEncodedTexels = 0;
        uint64_t ringEnd = 0;
        size_t i = mTextureCommandsDone;
        for (; i < mTextureCommands.size(); ++i)
        {
            const TextureCommand& command = mTextureCommands[i];
            if (i != mTextureCommandsDone && Clock::now() - start > limit)
            {
                break;
            }
            if (command.type == TextureCommand::Type::Upload && uploaded != 0 && uploaded + command.ringBytes > mUploadPieceBytes)
            {
                break;
            }
            if (command.type == TextureCommand::Type::GenerateMips && mEncodedTexels != 0 &&
                mEncodedTexels + encodeTexels(command.slot) > ENCODE_TEXELS_PER_FRAME)
            {
                break; // the GPU's share of encoding for this frame is spent
            }
            const Texture& target = textureFor(command.slot); // a replacement being built takes the uploads
            id<MTLTexture> texture = command.type != TextureCommand::Type::Create ? target.texture : nil;
            switch (command.type)
            {
                case TextureCommand::Type::Create:
                    createTexture(command, blit);
                    created = true;
                    break;
                case TextureCommand::Type::Publish:
                    publishTexture(command.slot);
                    break;
                case TextureCommand::Type::Upload:
                    if (texture && textureCompressed(target.desc.format))
                    {
                        // Blocks encoded before go to the block buffer, a row being a row of blocks.
                        if (!target.encodeTexels && target.encodeBlocks && command.height != 0)
                        {
                            const uint64_t rowBytes = command.ringBytes / command.height;
                            const uint64_t at = textureMipOffset(target.desc, command.level, true) + uint64_t(command.y) * rowBytes;
                            if (at < target.encodeBlocks.length)
                            {
                                [blit copyFromBuffer:mUploadRing
                                        sourceOffset:command.ringOffset
                                            toBuffer:target.encodeBlocks
                                   destinationOffset:at
                                                size:std::min<uint64_t>(command.ringBytes, target.encodeBlocks.length - at)];
                            }
                        }
                        // Whole rows of level 0 into the texels being encoded.
                        else if (target.encodeTexels && command.level == 0 && command.x == 0 && command.width == target.desc.width)
                        {
                            const uint64_t at = uint64_t(command.y) * target.desc.width * 4;
                            [blit copyFromBuffer:mUploadRing
                                    sourceOffset:command.ringOffset
                                        toBuffer:target.encodeTexels
                               destinationOffset:at
                                            size:std::min<uint64_t>(command.ringBytes, target.encodeTexels.length - at)];
                        }
                        else
                        {
                            ++mCompressedUploadsDropped;
                        }
                    }
                    else if (texture)
                    {
                        const NSUInteger rowBytes = NSUInteger(command.width) * target.pixelBytes;
                        [blit copyFromBuffer:mUploadRing
                                   sourceOffset:command.ringOffset
                              sourceBytesPerRow:rowBytes
                            sourceBytesPerImage:rowBytes * command.height
                                     sourceSize:MTLSizeMake(command.width, command.height, 1)
                                      toTexture:texture
                               destinationSlice:0
                               destinationLevel:command.level
                              destinationOrigin:MTLOriginMake(command.x, command.y, 0)];
                    }
                    uploaded += command.ringBytes;
                    ringEnd = command.ringEnd;
                    break;
                case TextureCommand::Type::GenerateMips:
                    if (texture && textureCompressed(target.desc.format))
                    {
                        blit = encodeTexture(command.slot, commands, blit, command.readBack);
                    }
                    else if (texture && texture.mipmapLevelCount > 1)
                    {
                        [blit generateMipmapsForTexture:texture];
                    }
                    break;
                case TextureCommand::Type::Destroy:
                    mRetiredTextures.add(command.slot, command.lastList);
                    break;
            }
        }
        [blit endEncoding];

        if (created)
        {
            if (@available(macOS 15.0, *))
            {
                [(id<MTLResidencySet>)mResidency commit];
            }
        }
        if (ringEnd != 0)
        {
            // The ring is written again only once these copies have run.
            TextureQueue* textures = &mTextures;
            std::atomic<uint32_t>* handlers = &mRingHandlers;
            handlers->fetch_add(1, std::memory_order_relaxed);
            [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
                textures->releaseRing(ringEnd);
                handlers->fetch_sub(1, std::memory_order_release);
            }];
        }
        [commands commit];

        // Dropped from the front only now and then, as a teleport can leave thousands.
        mTextureCommandsDone = i;
        if (mTextureCommandsDone == mTextureCommands.size())
        {
            mTextureCommands.clear();
            mTextureCommandsDone = 0;
        }
        else if (mTextureCommandsDone >= 1024 && mTextureCommandsDone * 2 >= mTextureCommands.size())
        {
            mTextureCommands.erase(mTextureCommands.begin(), mTextureCommands.begin() + mTextureCommandsDone);
            mTextureCommandsDone = 0;
        }
        return true;
    }

    id<MTLFunction> loadFunction(const std::string& dir, const char* name, std::string& error)
    {
        NSString* path = [NSString stringWithFormat:@"%s/%s.metallib", dir.c_str(), name];
        NSError* failure = nil;
        id<MTLLibrary> library = [mDevice newLibraryWithURL:[NSURL fileURLWithPath:path] error:&failure];
        if (!library)
        {
            error = std::string("cannot load ") + path.UTF8String;
            return nil;
        }
        // SPIRV-Cross renames main, because main is reserved in Metal.
        id<MTLFunction> function = [library newFunctionWithName:@"main0"];
        if (!function)
        {
            error = std::string("no entry point in ") + path.UTF8String;
        }
        return function;
    }

    struct Texture
    {
        id<MTLTexture> texture = nil;
        uint64_t bytes = 0;
        uint32_t pixelBytes = 4;
        TextureDesc desc; // metadata of the successfully created image, not queued requests
        // A compressed texture not encoded yet: uploads go to encodeTexels (RGBA8, every
        // mip), and encodeTexture() writes encodeBlocks and copies them into the texture.
        id<MTLBuffer> encodeTexels = nil;
        id<MTLBuffer> encodeBlocks = nil;
    };

    id<MTLDevice> mDevice = nil;
    id<MTLCommandQueue> mQueue = nil;
    id<MTLRenderPipelineState> mPipeline = nil;
    CAMetalLayer* mLayer = nil;
    DeviceInfo mInfo;
    std::string mError;
    uint64_t mDrawableBytes = 0;
    uint64_t mUploadBytesPerFrame = 0;
    uint64_t mFrame = 0;

    dispatch_semaphore_t mFrameSlots = nil;
    id<MTLFunction> mUIVertexFunction = nil;
    id<MTLFunction> mUIFragmentFunction = nil;
    id<MTLArgumentEncoder> mArgumentEncoder = nil;
    id<MTLBuffer> mArguments[FRAMES_IN_FLIGHT];     // the texture array of each frame in flight
    TextureSlotCopies mSlotCopies{ FRAMES_IN_FLIGHT };
    id<MTLSamplerState> mSamplers[uint32_t(UISampler::Count)];
    id mResidency = nil; // id<MTLResidencySet>, which needs macOS 15 to name
    std::unordered_map<uint32_t, id<MTLRenderPipelineState>> mUIPipelines; // every one made so far
    std::vector<id<MTLRenderPipelineState>> mUIDrawPipelines;              // the current list's, by blend
    uint64_t mUIPipelineGeneration = 0;
    id<MTLBuffer> mUIBuffers[FRAMES_IN_FLIGHT];
    uint64_t mUIBufferGeneration[FRAMES_IN_FLIGHT]{};
    std::vector<Texture> mTextureObjects;         // by slot
    TextureReplacementState<MAX_TEXTURES> mTextureReplacements;
    uint64_t mTextureCopiesRebased = 0;
    uint64_t mTextureCopiesRejected = 0;
    std::unordered_map<uint32_t, Texture> mPendingTextures; // replacements being built, by slot
    std::vector<TextureCommand> mTextureCommands; // taken, not yet run
    size_t mTextureCommandsDone = 0;              // the front of mTextureCommands already run
    RetiredTextures mRetiredTextures;

    struct FreedTexture
    {
        id<MTLTexture> texture;
        uint64_t frame; // released from this frame on
        uint64_t bytes;
    };
    std::vector<FreedTexture> mFreedTextures;     // destroyed, waiting for the frames that may sample them
    struct FreedEncode
    {
        id<MTLBuffer> texels;
        id<MTLBuffer> blocks;
        uint64_t frame;
        uint64_t readBack; // nonzero: hand the blocks to the texture queue
        TextureDesc desc;
    };
    std::vector<FreedEncode> mFreedEncodes;       // a compressed texture's encoding buffers, until its frame is done
    id<MTLFence> mEncodeFence = nil;
    id<MTLComputePipelineState> mTextureMipPipeline = nil;
    id<MTLComputePipelineState> mEncodeBC1Pipeline = nil;
    id<MTLComputePipelineState> mEncodeBC45Pipeline = nil;
    id<MTLComputePipelineState> mEncodeBC7Pipeline = nil;
    uint64_t mEncodedTexels = 0; // this frame's, against ENCODE_TEXELS_PER_FRAME
    uint64_t mTexturesEncoded = 0;
    uint64_t mCompressedUploadsDropped = 0;
    id<MTLBuffer> mUploadRing = nil;              // texture pixels, written by the main thread
    uint64_t mUploadPieceBytes = 0;               // the most one frame copies from it
    id<MTLBuffer> mZeros = nil;
    std::atomic<uint32_t> mRingHandlers{ 0 };     // texture copies whose completion handler has not run yet

    // Command buffers that ended in error, from their completion handlers.
    struct GpuErrors
    {
        std::mutex mutex;
        uint64_t count = 0;
        std::vector<std::string> first; // a few, word for word, since the last take

        void add(const char* text)
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++count;
            if (first.size() < 3)
            {
                first.push_back(text);
            }
        }
    };
    std::shared_ptr<GpuErrors> mGpuErrors = std::make_shared<GpuErrors>();
    std::chrono::steady_clock::time_point mPreviousFrameStart{};
    double mFrameSeconds = 0.0;

    struct RetiredBuffer
    {
        id<MTLBuffer> buffer;
        uint64_t frame; // released from this frame on
        MemoryCategory category;
    };

    SceneStore mSceneStore{ FRAMES_IN_FLIGHT };
    std::vector<id<MTLBuffer>> mSceneChunks;
    id<MTLBuffer> mSceneTables[SCENE_TABLES];
    id<MTLBuffer> mSceneVisible = nil;
    id<MTLBuffer> mSceneRetest = nil;
    id<MTLBuffer> mSceneGroups = nil;
    id<MTLBuffer> mSceneCounts = nil;
    id<MTLBuffer> mScenePyramid = nil;
    id<MTLBuffer> mSceneCountsTemplate = nil;
    id<MTLBuffer> mSceneViews[FRAMES_IN_FLIGHT];
    id<MTLBuffer> mSceneReadback[FRAMES_IN_FLIGHT];
    id<MTLBuffer> mSceneStaging[FRAMES_IN_FLIGHT];
    id<MTLBuffer> mAuditReadback[FRAMES_IN_FLIGHT];
    bool mAuditWritten[FRAMES_IN_FLIGHT]{};
    id<MTLBuffer> mTextureFeedback = nil;
    id<MTLBuffer> mFeedbackReadback[FRAMES_IN_FLIGHT];
    TextureFeedback::Window mFeedbackRead[FRAMES_IN_FLIGHT]; // the window each slot's readback holds; 0 is none
    TextureFeedback::Window mFeedbackEnding;                 // the window this frame ends, when it ends one
    uint32_t mFeedbackValue = 0;                             // GpuSceneView::feedback for this frame
    std::deque<RetiredBuffer> mRetiredBuffers;
    id<MTLComputePipelineState> mCullPipeline = nil;
    id<MTLComputePipelineState> mPyramidPipeline = nil;
    id<MTLComputePipelineState> mResolvePipeline = nil;
    id<MTLComputePipelineState> mSkinPipeline = nil;
    id<MTLRenderPipelineState> mVisibilityPipeline = nil;
    id<MTLRenderPipelineState> mMaskedPipeline = nil;
    id<MTLRenderPipelineState> mCompositePipeline = nil;
    id<MTLRenderPipelineState> mDepthCopyPipeline = nil;   // asrOverlayDepthF: phase 0 copies, 1 clears
    id<MTLDepthStencilState> mDepthState = nil;
    id<MTLDepthStencilState> mUIDepthStates[32] = {};      // by UIDepth: test, write, compare
    id<MTLTexture> mUIDepth = nil;                          // window-sized, memoryless where it can be
    id<MTLFence> mSceneFence = nil;
    id<MTLTexture> mVisibilityTarget = nil;
    id<MTLTexture> mDepthTarget = nil;
    id<MTLTexture> mAlbedoTarget = nil;
    id<MTLTexture> mNormalTarget = nil;
    id<MTLTexture> mAuditTarget = nil;
    id<MTLTexture> mLitTarget = nil;        // the opaque image, set 1 binding 8
    id<MTLTexture> mSceneColorTarget = nil; // with transparency, set 1 binding 9
    MacOITResources mMacOIT;
    uint64_t mSceneTargetBytes = 0;
    uint32_t mSceneWidth = 0;
    uint32_t mSceneHeight = 0;
    SceneStats mSceneFrameStats;

    FrameGraph mGraph;
    id<MTLCounterSampleBuffer> mPassSamples = nil;
    MTLTimestamp mClockStartCpu = 0;
    MTLTimestamp mClockStartGpu = 0;
    uint32_t mTimedPasses[FRAMES_IN_FLIGHT]{};
    const char* mTimedNames[FRAMES_IN_FLIGHT][MAX_TIMED_PASSES]{};

    // What pass bodies work with, during one recordScene().
    id<MTLCommandBuffer> mRecordCommands = nil;
    uint32_t mRecordSlot = 0;
    uint32_t mRecordPass = 0;
    const SceneCamera3D* mRecordCamera = nullptr;
    std::chrono::steady_clock::time_point mRecordTime;
    ScenePush mScenePush{};
    uint32_t mSceneInstanceEnd = 0;
    uint32_t mPyramidLevels = 0;

    SceneCamera3D mPyramidCamera{}; // the view the pyramid holds
    bool mHasPyramid = false;
    bool mOcclusion = true;
};

} // namespace

ProbeResult Device::probe()
{
    ProbeResult result;
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
        {
            result.reason = "no Metal device";
        }
        else if (![device supportsFamily:MTLGPUFamilyApple7])
        {
            result.reason = "this GPU is older than Apple family 7";
        }
        else if (!@available(macOS 15.0, *))
        {
            result.reason = NEEDS_MACOS_15;
        }
        else
        {
            result.supported = true;
            result.info.backend = "Metal";
            result.info.vendor = "Apple";
            result.info.name = device.name.UTF8String;
            result.info.memoryBudgetBytes = device.recommendedMaxWorkingSetSize;
        }
    }
    return result;
}

std::unique_ptr<Device> Device::create(const SurfaceTarget& target, const DeviceOptions& options, std::string& error)
{
    auto device = std::make_unique<DeviceMetal>();
    if (!device->init(target, options, error))
    {
        return nullptr;
    }
    return device;
}

} // namespace asr
