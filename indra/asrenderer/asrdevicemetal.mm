/**
 * asrdevicemetal.mm
 * Native Metal backend (Apple GPU family 7 and later, M1 onward), with no
 * translation layer. Draws the test triangle. See asrdevice.h.
 */
#include "asrdevice.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace asr
{

namespace
{

class DeviceMetal final : public Device
{
public:
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

        mInfo.backend = "Metal";
        mInfo.name = mDevice.name.UTF8String;
        mInfo.memoryBudgetBytes = mDevice.recommendedMaxWorkingSetSize;
        mMemoryCapBytes = options.memoryCapBytes;
        refreshMemoryBudget();

        NSView* view = (__bridge NSView*)target.nativeWindow;
        mLayer = [CAMetalLayer layer];
        mLayer.device = mDevice;
        mLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        mLayer.framebufferOnly = YES;
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
        return true;
    }

    const DeviceInfo& info() const override { return mInfo; }
    const std::string& lastError() const override { return mError; }
    uint32_t validationErrors() const override { return 0; }

    // Apple's figure is fixed for the device, so unlike Vulkan it does not move
    // with what other applications use.
    void refreshMemoryBudget() override { setDriverBudget(mDevice.recommendedMaxWorkingSetSize); }

    bool renderTestFrame(uint32_t width, uint32_t height, float seconds) override
    {
        @autoreleasepool
        {
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

            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0.02, 0.02, 0.05, 1.0);

            id<MTLCommandBuffer> commands = [mQueue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:mPipeline];
            // Buffer 0 is where SPIRV-Cross puts the GLSL push constants.
            [encoder setFragmentBytes:&seconds length:sizeof(seconds) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            [commands presentDrawable:drawable];
            [commands commit];
        }
        return true;
    }

private:
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

    id<MTLDevice> mDevice = nil;
    id<MTLCommandQueue> mQueue = nil;
    id<MTLRenderPipelineState> mPipeline = nil;
    CAMetalLayer* mLayer = nil;
    DeviceInfo mInfo;
    std::string mError;
    uint64_t mDrawableBytes = 0;
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
        else
        {
            result.supported = true;
            result.info.backend = "Metal";
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
