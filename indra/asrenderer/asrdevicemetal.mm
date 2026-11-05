/**
 * asrdevicemetal.mm
 * Native Metal backend (Apple GPU family 7 and later, M1 onward), with no
 * translation layer. Draws the UI through one bindless pipeline per blend, and
 * the test triangle. Needs macOS 15 for residency sets, which make a bindless
 * texture cost nothing per frame. See asrdevice.h.
 */
#include "asrdevice.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstring>
#include <unordered_map>
#include <vector>

namespace asr
{

namespace
{

constexpr uint32_t FRAMES_IN_FLIGHT = 3;

// Where the vertex buffer goes. The fragment stage's buffer 0 is the texture array.
constexpr NSUInteger UI_VERTEX_BUFFER = 0;

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

class DeviceMetal final : public Device
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
        refreshMemoryBudget();

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
        NSError* failure = nil;
        mPipeline = [mDevice newRenderPipelineStateWithDescriptor:descriptor error:&failure];
        if (!mPipeline)
        {
            error = std::string("pipeline: ") + failure.localizedDescription.UTF8String;
            return false;
        }
        return createUI(options.shaderDir, error);
    }

    const DeviceInfo& info() const override { return mInfo; }
    const std::string& lastError() const override { return mError; }
    uint32_t validationErrors() const override { return 0; }

    // Apple's figure is fixed for the device, so unlike Vulkan it does not move
    // with what other applications use.
    void refreshMemoryBudget() override { setDriverBudget(mDevice.recommendedMaxWorkingSetSize); }

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
                if (!mMemoryBudget.reserve(MemoryCategory::RenderTargets, mDrawableBytes))
                {
                    mDrawableBytes = 0;
                    mError = "the drawables do not fit in the render target budget";
                    return false;
                }
            }

            id<CAMetalDrawable> drawable = [mLayer nextDrawable];
            if (!drawable)
            {
                return true; // none free right now: not an error
            }

            // The buffers of this slot are free once its earlier frame has finished.
            dispatch_semaphore_wait(mFrameSlots, DISPATCH_TIME_FOREVER);
            const uint32_t slot = uint32_t(mFrame % FRAMES_IN_FLIGHT);
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

            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0); // what the viewer clears to, and transparent UI shows it

            id<MTLCommandBuffer> commands = [mQueue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            if (input.testTriangle)
            {
                [encoder setRenderPipelineState:mPipeline];
                // Buffer 0 is where SPIRV-Cross puts the GLSL push constants.
                [encoder setFragmentBytes:&input.seconds length:sizeof(input.seconds) atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            if (input.ui && !input.ui->vertices().empty())
            {
                drawUI(encoder, slot, *input.ui, width, height);
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
        // drawing with another texture changes no state.
        mArgumentEncoder = [mUIFragmentFunction newArgumentEncoderWithBufferIndex:0];
        mArguments = [mDevice newBufferWithLength:mArgumentEncoder.encodedLength options:MTLResourceStorageModeShared];
        if (!mArguments)
        {
            error = "cannot create the UI texture array";
            return false;
        }
        [mArgumentEncoder setArgumentBuffer:mArguments offset:0];

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
            [mArgumentEncoder setSamplerState:mSamplers[i] atIndex:MAX_TEXTURES + i];
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
                if (!mMemoryBudget.reserve(MemoryCategory::UI, size))
                {
                    mError = "the UI vertex buffer does not fit in its memory budget";
                    return false;
                }
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

    void drawUI(id<MTLRenderCommandEncoder> encoder, uint32_t slot, const UIList& ui, uint32_t width, uint32_t height)
    {
        [encoder setVertexBuffer:mUIBuffers[slot] offset:0 atIndex:UI_VERTEX_BUFFER];
        [encoder setFragmentBuffer:mArguments offset:0 atIndex:0];
        [encoder setViewport:MTLViewport{ 0.0, UI_EDGE_NUDGE, double(ui.width()), double(ui.height()), 0.0, 1.0 }];

        id<MTLRenderPipelineState> bound = nil;
        for (const UIDraw& draw : ui.draws())
        {
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
            [encoder setScissorRect:MTLScissorRect{ rect.x, rect.y, rect.width, rect.height }];
            const MTLPrimitiveType type = draw.primitive == UIPrimitive::Triangles ? MTLPrimitiveTypeTriangle
                                        : draw.primitive == UIPrimitive::Lines     ? MTLPrimitiveTypeLine
                                                                                   : MTLPrimitiveTypePoint;
            [encoder drawPrimitives:type vertexStart:draw.firstVertex vertexCount:draw.vertexCount];
        }
    }

    // A texture that does not fit in the budget shows as white and is not an error:
    // the budget's pressure levels are what keep this from happening.
    void createTexture(const TextureCommand& command)
    {
        if (mTextureObjects.size() <= command.slot)
        {
            mTextureObjects.resize(command.slot + 1);
        }
        const uint64_t bytes = textureBytes(command.desc);
        if (!mMemoryBudget.reserve(MemoryCategory::Textures, bytes))
        {
            [mArgumentEncoder setTexture:mTextureObjects[0].texture atIndex:command.slot];
            return;
        }

        const TextureDesc& desc = command.desc;
        const MTLPixelFormat format = desc.format == TextureFormat::R8    ? MTLPixelFormatR8Unorm
                                    : desc.format == TextureFormat::RG8   ? MTLPixelFormatRG8Unorm
                                    : desc.format == TextureFormat::BGRA8 ? MTLPixelFormatBGRA8Unorm
                                                                          : MTLPixelFormatRGBA8Unorm;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                              width:desc.width
                                                                                             height:desc.height
                                                                                          mipmapped:NO];
        descriptor.mipmapLevelCount = desc.mipLevels;
        descriptor.storageMode = MTLStorageModeShared; // on Apple GPUs the CPU writes it directly
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
            mMemoryBudget.release(MemoryCategory::Textures, bytes);
            [mArgumentEncoder setTexture:mTextureObjects[0].texture atIndex:command.slot];
            return;
        }

        // Cleared, because a list can draw this texture before its pixels arrive.
        const uint32_t pixelBytes = textureBytesPerPixel(desc.format);
        std::vector<uint8_t> zeros;
        uint32_t width = desc.width;
        uint32_t height = desc.height;
        for (uint32_t level = 0; level < desc.mipLevels; ++level)
        {
            zeros.assign(size_t(width) * height * pixelBytes, 0);
            [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:level withBytes:zeros.data() bytesPerRow:width * pixelBytes];
            width = std::max(width / 2, 1u);
            height = std::max(height / 2, 1u);
        }

        mTextureObjects[command.slot].texture = texture;
        mTextureObjects[command.slot].bytes = bytes;
        mTextureObjects[command.slot].pixelBytes = pixelBytes;
        [mArgumentEncoder setTexture:texture atIndex:command.slot];
        if (@available(macOS 15.0, *))
        {
            [(id<MTLResidencySet>)mResidency addAllocation:texture];
        }
    }

    void destroyTexture(uint32_t slot)
    {
        Texture& texture = mTextureObjects[slot];
        if (texture.texture)
        {
            if (@available(macOS 15.0, *))
            {
                [(id<MTLResidencySet>)mResidency removeAllocation:texture.texture];
                [(id<MTLResidencySet>)mResidency commit];
            }
            mMemoryBudget.release(MemoryCategory::Textures, texture.bytes);
        }
        texture = Texture();
    }

    bool runTextureCommands()
    {
        mTextures.take(mTextureCommands);
        if (mTextureCommands.empty())
        {
            return true;
        }

        bool created = false;
        std::vector<id<MTLTexture>> needMips;
        for (TextureCommand& command : mTextureCommands)
        {
            switch (command.type)
            {
                case TextureCommand::Type::Create:
                    createTexture(command);
                    created = true;
                    break;
                case TextureCommand::Type::Upload:
                {
                    const Texture& texture = mTextureObjects[command.slot];
                    if (texture.texture)
                    {
                        [texture.texture replaceRegion:MTLRegionMake2D(command.x, command.y, command.width, command.height)
                                           mipmapLevel:command.level
                                             withBytes:command.pixels.data()
                                           bytesPerRow:command.width * texture.pixelBytes];
                    }
                    break;
                }
                case TextureCommand::Type::GenerateMips:
                {
                    const Texture& texture = mTextureObjects[command.slot];
                    if (texture.texture && texture.texture.mipmapLevelCount > 1)
                    {
                        needMips.push_back(texture.texture);
                    }
                    break;
                }
                case TextureCommand::Type::Destroy:
                    mRetiredTextures.add(command.slot, command.lastList);
                    break;
            }
        }
        mTextureCommands.clear();

        if (created)
        {
            if (@available(macOS 15.0, *))
            {
                [(id<MTLResidencySet>)mResidency commit];
            }
        }

        // Committed before the frame's own commands, so the frame sees the mips.
        if (!needMips.empty())
        {
            id<MTLCommandBuffer> commands = [mQueue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            for (id<MTLTexture> texture : needMips)
            {
                [blit generateMipmapsForTexture:texture];
            }
            [blit endEncoding];
            [commands commit];
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
    };

    id<MTLDevice> mDevice = nil;
    id<MTLCommandQueue> mQueue = nil;
    id<MTLRenderPipelineState> mPipeline = nil;
    CAMetalLayer* mLayer = nil;
    DeviceInfo mInfo;
    std::string mError;
    uint64_t mDrawableBytes = 0;
    uint64_t mFrame = 0;

    dispatch_semaphore_t mFrameSlots = nil;
    id<MTLFunction> mUIVertexFunction = nil;
    id<MTLFunction> mUIFragmentFunction = nil;
    id<MTLArgumentEncoder> mArgumentEncoder = nil;
    id<MTLBuffer> mArguments = nil;
    id<MTLSamplerState> mSamplers[uint32_t(UISampler::Count)];
    id mResidency = nil; // id<MTLResidencySet>, which needs macOS 15 to name
    std::unordered_map<uint32_t, id<MTLRenderPipelineState>> mUIPipelines; // every one made so far
    std::vector<id<MTLRenderPipelineState>> mUIDrawPipelines;              // the current list's, by blend
    uint64_t mUIPipelineGeneration = 0;
    id<MTLBuffer> mUIBuffers[FRAMES_IN_FLIGHT];
    uint64_t mUIBufferGeneration[FRAMES_IN_FLIGHT]{};
    std::vector<Texture> mTextureObjects;         // by slot
    std::vector<TextureCommand> mTextureCommands; // only held while they run
    RetiredTextures mRetiredTextures;
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
