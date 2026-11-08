/**
 * asrdevicevulkan.cpp
 * Vulkan 1.3 backend: dynamic rendering, synchronization2 and no OpenGL. Vulkan
 * itself is loaded at runtime through volk, so a machine without it reaches
 * create() and gets an error, not a failure to start. Draws the scene through
 * compute culling and a visibility buffer, the UI through one bindless pipeline per
 * blend, and the test triangle. See asrdevice.h.
 */
#include "asrdevice.h"
#include "asrscenegraph.h"
#include "asrscenestore.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "volk.h"
#include "vk_mem_alloc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace asr
{

namespace
{

constexpr uint32_t FRAMES_IN_FLIGHT = 2;

constexpr uint64_t UPLOAD_ALIGNMENT = 16;

// The UI samples textures in fragment shaders, and the scene resolve in compute.
constexpr VkPipelineStageFlags2 TEXTURE_READ_STAGES = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

constexpr VkFormat VISIBILITY_FORMAT = VK_FORMAT_R32_UINT;
constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat ALBEDO_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat NORMAL_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

constexpr VkShaderStageFlags SCENE_PUSH_STAGES = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

static_assert(sizeof(GpuDrawArguments) == sizeof(VkDrawIndirectCommand), "culling counts into these");

// The features section 2 of the plan requires.
bool hasRequiredFeatures(VkPhysicalDevice gpu)
{
    VkPhysicalDeviceVulkan13Features v13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13 };
    VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
    vkGetPhysicalDeviceFeatures2(gpu, &features);

    if (!features.features.shaderInt64)
    {
        return false;
    }

    VkPhysicalDeviceVulkan12Properties limits{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &limits };
    vkGetPhysicalDeviceProperties2(gpu, &properties);

    return v13.dynamicRendering && v13.synchronization2 && v13.shaderDemoteToHelperInvocation && v12.timelineSemaphore &&
           v12.descriptorIndexing && v12.bufferDeviceAddress && v12.descriptorBindingPartiallyBound &&
           v12.descriptorBindingSampledImageUpdateAfterBind && v12.shaderSampledImageArrayNonUniformIndexing &&
           limits.maxPerStageDescriptorUpdateAfterBindSampledImages >= MAX_TEXTURES &&
           limits.maxDescriptorSetUpdateAfterBindSampledImages >= MAX_TEXTURES;
}

VkBlendFactor toVulkan(UIBlendFactor factor)
{
    switch (factor)
    {
        case UIBlendFactor::One:                 return VK_BLEND_FACTOR_ONE;
        case UIBlendFactor::Zero:                return VK_BLEND_FACTOR_ZERO;
        case UIBlendFactor::DestColor:           return VK_BLEND_FACTOR_DST_COLOR;
        case UIBlendFactor::SourceColor:         return VK_BLEND_FACTOR_SRC_COLOR;
        case UIBlendFactor::OneMinusDestColor:   return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case UIBlendFactor::OneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case UIBlendFactor::DestAlpha:           return VK_BLEND_FACTOR_DST_ALPHA;
        case UIBlendFactor::SourceAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
        case UIBlendFactor::OneMinusDestAlpha:   return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case UIBlendFactor::OneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

const char* vendorName(uint32_t pciVendorId)
{
    switch (pciVendorId)
    {
        case 0x10DE: return "NVIDIA Corporation";
        case 0x1002: return "ATI Technologies Inc.";
        case 0x8086: return "Intel";
        default:     return "Unknown";
    }
}

struct GpuChoice
{
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    uint32_t memoryHeap = 0; // the largest device-local heap
    DeviceInfo info;
};

// Takes the best GPU that has Vulkan 1.3, the required features, and a queue that
// can both draw and present. Discrete GPUs win. canPresent(gpu, family) decides
// the last part, so probing and device creation apply one rule.
template <class CanPresent>
GpuChoice chooseGpu(VkInstance instance, CanPresent canPresent)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());

    GpuChoice choice;
    int bestScore = 0;
    for (VkPhysicalDevice gpu : gpus)
    {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(gpu, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3 || !hasRequiredFeatures(gpu))
        {
            continue;
        }

        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueCount, queues.data());
        for (uint32_t i = 0; i < queueCount; ++i)
        {
            if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && canPresent(gpu, i))
            {
                const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > bestScore)
                {
                    bestScore = score;
                    choice.gpu = gpu;
                    choice.queueFamily = i;
                    choice.info.name = properties.deviceName;
                    choice.info.vendor = vendorName(properties.vendorID);
                }
                break;
            }
        }
    }

    if (choice.gpu != VK_NULL_HANDLE)
    {
        choice.info.backend = "Vulkan";
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(choice.gpu, &memory);
        for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
        {
            if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                memory.memoryHeaps[i].size > choice.info.memoryBudgetBytes)
            {
                choice.info.memoryBudgetBytes = memory.memoryHeaps[i].size;
                choice.memoryHeap = i;
            }
        }
    }
    return choice;
}

// Stays mapped for its whole life.
struct MappedBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint64_t size = 0;
};

// Device-local, reached by address from shaders.
struct GpuBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    uint64_t address = 0;
    uint64_t size = 0;
};

struct TargetImage
{
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t bytes = 0;
};

struct Texture
{
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t bytes = 0;
    uint32_t mipLevels = 1;
    uint32_t width = 1;
    uint32_t height = 1;
};

class DeviceVulkan final : public Device, private SceneChunks
{
public:
    ~DeviceVulkan() override { shutdown(); }

    bool init(const SurfaceTarget& target, const DeviceOptions& options, std::string& error)
    {
        mOptions = options;
        mMemoryCapBytes = options.memoryCapBytes;
        mOcclusion = options.occlusion;
        mWindow = static_cast<HWND>(target.nativeWindow);

        if (volkInitialize() != VK_SUCCESS)
        {
            error = "Vulkan is not installed";
            return false;
        }
        return createInstance(error) && createSurface(target, error) && pickGpu(error) &&
               createLogicalDevice(error) && createFrames(error) && createPipeline(error) && createUI(error) && createScene(error);
    }

    const DeviceInfo& info() const override { return mInfo; }
    const std::string& lastError() const override { return mError; }
    uint32_t validationErrors() const override { return mValidationErrors.load(std::memory_order_relaxed); }

    void refreshMemoryBudget() override
    {
        uint64_t bytes = mInfo.memoryBudgetBytes;
        if (mHasMemoryBudgetExtension)
        {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
            VkPhysicalDeviceMemoryProperties2 memory{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget };
            vkGetPhysicalDeviceMemoryProperties2(mGpu, &memory);
            if (budget.heapBudget[mMemoryHeap] != 0)
            {
                bytes = budget.heapBudget[mMemoryHeap];
            }
        }
        setDriverBudget(bytes);
    }

    bool renderFrame(const FrameInput& input) override
    {
        const uint32_t width = input.width;
        const uint32_t height = input.height;
        if (width == 0 || height == 0)
        {
            return true; // minimized
        }
        // The size can lag behind the real window, because it comes from the main
        // thread, and the surface then decides the size. So compare with what was
        // last asked for: comparing with mExtent would rebuild every frame.
        if (mSwapchain == VK_NULL_HANDLE || mNeedSwapchain || width != mRequestedWidth || height != mRequestedHeight)
        {
            mRequestedWidth = width;
            mRequestedHeight = height;
            if (!createSwapchain(width, height))
            {
                return false;
            }
            if (mSwapchain == VK_NULL_HANDLE)
            {
                return true; // the surface has no size yet
            }
        }

        if (input.camera && !ensureSceneTargets())
        {
            return false;
        }

        using Clock = std::chrono::steady_clock;
        const auto seconds = [](Clock::time_point from, Clock::time_point to) { return std::chrono::duration<double>(to - from).count(); };
        mFrameParts = {};
        const Clock::time_point frameStart = Clock::now();
        mFrameParts = FrameParts{}; // the parts inside record fill in as it runs

        const uint32_t slot = mFrame % FRAMES_IN_FLIGHT;
        if (!check(vkWaitForFences(mDevice, 1, &mFences[slot], VK_TRUE, UINT64_MAX), "wait for frame"))
        {
            return false;
        }
        const Clock::time_point fenceDone = Clock::now();
        if (mRingEnd[slot] != 0)
        {
            mTextures.releaseRing(mRingEnd[slot]); // that frame's copies have run
            mRingEnd[slot] = 0;
        }
        mFrameSeconds = mFrame != 0 ? seconds(mPreviousFrameStart, frameStart) : 0.0;
        mPreviousFrameStart = frameStart;

        uint32_t image = 0;
        const VkResult acquired = vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX, mImageReady[slot], VK_NULL_HANDLE, &image);
        const Clock::time_point imageDone = Clock::now();
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            mNeedSwapchain = true;
            return true;
        }
        if (acquired != VK_SUBOPTIMAL_KHR && !check(acquired, "acquire image"))
        {
            return false;
        }

        // Reset only now: an early return above must leave the fence signalled.
        if (!check(vkResetFences(mDevice, 1, &mFences[slot]), "reset fence") || !prepareUI(slot, input.ui))
        {
            return false;
        }
        const Clock::time_point uiDone = Clock::now();
        if (!recordCommands(slot, image, input))
        {
            return false;
        }
        const Clock::time_point recordDone = Clock::now();

        VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        wait.semaphore = mImageReady[slot];
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        signal.semaphore = mRenderDone[image];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo commands{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        commands.commandBuffer = mCommands[slot];
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commands;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        if (!check(vkQueueSubmit2(mQueue, 1, &submit, mFences[slot]), "submit"))
        {
            return false;
        }
        const Clock::time_point submitDone = Clock::now();

        VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &mRenderDone[image];
        present.swapchainCount = 1;
        present.pSwapchains = &mSwapchain;
        present.pImageIndices = &image;
        const VkResult shown = vkQueuePresentKHR(mQueue, &present);
        if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR)
        {
            mNeedSwapchain = true;
        }
        else if (!check(shown, "present"))
        {
            return false;
        }

        mFrameParts.waitForGpu = seconds(frameStart, fenceDone);
        mFrameParts.acquire = seconds(fenceDone, imageDone);
        mFrameParts.prepareUI = seconds(imageDone, uiDone);
        mFrameParts.record = seconds(uiDone, recordDone);
        mFrameParts.submit = seconds(recordDone, submitDone);
        mFrameParts.present = seconds(submitDone, Clock::now());
        ++mFrame;
        return true;
    }

    FrameParts frameParts() const override { return mFrameParts; }

private:
    bool check(VkResult result, const char* what)
    {
        if (result == VK_SUCCESS)
        {
            return true;
        }
        mError = std::string(what) + " failed: VkResult " + std::to_string(int(result));
        return false;
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL onValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                       VkDebugUtilsMessageTypeFlagsEXT,
                                                       const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
    {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            static_cast<DeviceVulkan*>(user)->mValidationErrors.fetch_add(1, std::memory_order_relaxed);
            fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
        }
        return VK_FALSE;
    }

    bool createInstance(std::string& error)
    {
        if (volkGetInstanceVersion() < VK_API_VERSION_1_3)
        {
            error = "Vulkan 1.3 is not available";
            return false;
        }

        bool validation = false;
        if (mOptions.validation)
        {
            uint32_t count = 0;
            vkEnumerateInstanceLayerProperties(&count, nullptr);
            std::vector<VkLayerProperties> layers(count);
            vkEnumerateInstanceLayerProperties(&count, layers.data());
            for (const VkLayerProperties& layer : layers)
            {
                validation = validation || strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
            }
            if (!validation)
            {
                fprintf(stderr, "validation requested, but the layer is not installed\n");
            }
        }

        std::vector<const char*> extensions = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
        const char* layerName = "VK_LAYER_KHRONOS_validation";
        if (validation)
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "AyaneStorm";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo create{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        create.pApplicationInfo = &app;
        create.enabledExtensionCount = uint32_t(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        create.enabledLayerCount = validation ? 1 : 0;
        create.ppEnabledLayerNames = &layerName;
        const VkResult result = vkCreateInstance(&create, nullptr, &mInstance);
        if (result != VK_SUCCESS)
        {
            error = "vkCreateInstance failed: VkResult " + std::to_string(int(result));
            return false;
        }
        volkLoadInstance(mInstance);

        if (validation)
        {
            VkDebugUtilsMessengerCreateInfoEXT messenger{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            messenger.pfnUserCallback = onValidation;
            messenger.pUserData = this;
            vkCreateDebugUtilsMessengerEXT(mInstance, &messenger, nullptr, &mMessenger);
        }
        return true;
    }

    bool createSurface(const SurfaceTarget& target, std::string& error)
    {
        VkWin32SurfaceCreateInfoKHR create{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        create.hinstance = static_cast<HINSTANCE>(target.nativeInstance);
        create.hwnd = mWindow;
        if (vkCreateWin32SurfaceKHR(mInstance, &create, nullptr, &mSurface) != VK_SUCCESS)
        {
            error = "cannot create a Vulkan surface for the window";
            return false;
        }
        return true;
    }

    bool pickGpu(std::string& error)
    {
        const GpuChoice choice = chooseGpu(mInstance, [this](VkPhysicalDevice gpu, uint32_t family)
        {
            VkBool32 canPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, family, mSurface, &canPresent);
            return canPresent == VK_TRUE;
        });
        if (choice.gpu == VK_NULL_HANDLE)
        {
            error = "no Vulkan 1.3 GPU with the required features can present to this window";
            return false;
        }
        mGpu = choice.gpu;
        mQueueFamily = choice.queueFamily;
        mMemoryHeap = choice.memoryHeap;
        mInfo = choice.info;
        return true;
    }

    bool hasDeviceExtension(const char* name) const
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(mGpu, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateDeviceExtensionProperties(mGpu, nullptr, &count, extensions.data());
        for (const VkExtensionProperties& extension : extensions)
        {
            if (strcmp(extension.extensionName, name) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool createLogicalDevice(std::string& error)
    {
        VkPhysicalDeviceVulkan13Features v13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        v13.dynamicRendering = VK_TRUE;
        v13.synchronization2 = VK_TRUE;
        v13.shaderDemoteToHelperInvocation = VK_TRUE;
        VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13 };
        v12.timelineSemaphore = VK_TRUE;
        v12.descriptorIndexing = VK_TRUE;
        v12.bufferDeviceAddress = VK_TRUE;
        v12.descriptorBindingPartiallyBound = VK_TRUE;
        v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
        features.features.shaderInt64 = VK_TRUE;

        const float priority = 1.f;
        VkDeviceQueueCreateInfo queue{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queue.queueFamilyIndex = mQueueFamily;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;

        std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        mHasMemoryBudgetExtension = hasDeviceExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        if (mHasMemoryBudgetExtension)
        {
            extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }

        VkDeviceCreateInfo create{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features };
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue;
        create.enabledExtensionCount = uint32_t(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        const VkResult result = vkCreateDevice(mGpu, &create, nullptr, &mDevice);
        if (result != VK_SUCCESS)
        {
            error = "vkCreateDevice failed: VkResult " + std::to_string(int(result));
            return false;
        }
        volkLoadDevice(mDevice);
        vkGetDeviceQueue(mDevice, mQueueFamily, 0, &mQueue);
        refreshMemoryBudget();

        // Picked once and kept, so the pipeline never depends on the swapchain's life.
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(mGpu, mSurface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(mGpu, mSurface, &count, formats.data());
        if (formats.empty())
        {
            error = "the window offers no surface format";
            return false;
        }
        mSurfaceFormat = formats[0];
        for (const VkSurfaceFormatKHR& format : formats)
        {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM)
            {
                mSurfaceFormat = format;
            }
        }
        return true;
    }

    bool createFrames(std::string& error)
    {
        // Fixed here, so the budget moving later cannot strand a staging buffer.
        mUploadBytesPerFrame = mMemoryBudget.uploadBytesPerFrame(FRAMES_IN_FLIGHT);

        VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = mQueueFamily;
        VkCommandBufferAllocateInfo buffers{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        buffers.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        buffers.commandBufferCount = FRAMES_IN_FLIGHT;

        bool ok = vkCreateCommandPool(mDevice, &pool, nullptr, &mCommandPool) == VK_SUCCESS;
        buffers.commandPool = mCommandPool;
        ok = ok && vkAllocateCommandBuffers(mDevice, &buffers, mCommands) == VK_SUCCESS;
        for (uint32_t i = 0; ok && i < FRAMES_IN_FLIGHT; ++i)
        {
            VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo semaphore{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            ok = vkCreateFence(mDevice, &fence, nullptr, &mFences[i]) == VK_SUCCESS &&
                 vkCreateSemaphore(mDevice, &semaphore, nullptr, &mImageReady[i]) == VK_SUCCESS;
        }
        if (!ok)
        {
            error = "cannot create per-frame objects";
        }
        return ok;
    }

    VkShaderModule loadShader(const char* name, std::string& error)
    {
        const std::string path = mOptions.shaderDir + "/" + name + ".spv";
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const std::streamsize size = file ? std::streamsize(file.tellg()) : 0;
        if (size <= 0 || size % sizeof(uint32_t) != 0)
        {
            error = "cannot load " + path;
            return VK_NULL_HANDLE;
        }
        std::vector<uint32_t> code(size_t(size) / sizeof(uint32_t));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(code.data()), size);

        VkShaderModuleCreateInfo create{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        create.codeSize = size_t(size);
        create.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(mDevice, &create, nullptr, &module) != VK_SUCCESS)
        {
            error = "cannot create a shader from " + path;
        }
        return module;
    }

    bool createPipeline(std::string& error)
    {
        VkPushConstantRange push{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) };
        VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(mDevice, &layout, nullptr, &mPipelineLayout) != VK_SUCCESS)
        {
            error = "cannot create the pipeline layout";
            return false;
        }

        VkShaderModule vertex = loadShader("asrTestTriangleV", error);
        VkShaderModule fragment = vertex ? loadShader("asrTestTriangleF", error) : VK_NULL_HANDLE;
        if (!vertex || !fragment)
        {
            if (vertex)
            {
                vkDestroyShaderModule(mDevice, vertex, nullptr);
            }
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamics;
        VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &mSurfaceFormat.format;

        VkGraphicsPipelineCreateInfo create{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering };
        create.stageCount = 2;
        create.pStages = stages;
        create.pVertexInputState = &vertexInput;
        create.pInputAssemblyState = &assembly;
        create.pViewportState = &viewport;
        create.pRasterizationState = &raster;
        create.pMultisampleState = &multisample;
        create.pColorBlendState = &blend;
        create.pDynamicState = &dynamic;
        create.layout = mPipelineLayout;
        const VkResult result = vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &create, nullptr, &mPipeline);

        vkDestroyShaderModule(mDevice, vertex, nullptr);
        vkDestroyShaderModule(mDevice, fragment, nullptr);
        if (result != VK_SUCCESS)
        {
            error = "vkCreateGraphicsPipelines failed: VkResult " + std::to_string(int(result));
            return false;
        }
        return true;
    }

    // Host memory that the main thread writes texture pixels into and the GPU copies
    // from. Not video memory, so the window of it the CPU can reach is kept for what
    // needs it, and the GPU reads the pixels once, as it copies them.
    bool createUploadRing(std::string& error)
    {
        const uint64_t bytes = mMemoryBudget.uploadRingBytes(FRAMES_IN_FLIGHT);
        if (!mMemoryBudget.reserve(MemoryCategory::Staging, bytes))
        {
            error = "the texture upload ring does not fit in its memory budget";
            return false;
        }
        VkBufferCreateInfo create{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        create.size = bytes;
        create.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(mAllocator, &create, &allocation, &mUploadRing.buffer, &mUploadRing.allocation, &info) != VK_SUCCESS)
        {
            mMemoryBudget.release(MemoryCategory::Staging, bytes);
            error = "cannot create the texture upload ring";
            return false;
        }
        mUploadRing.mapped = info.pMappedData;
        mUploadRing.size = bytes;
        mUploadPieceBytes = mMemoryBudget.uploadRingPieceBytes(FRAMES_IN_FLIGHT);
        mTextures.setUploadRing(static_cast<uint8_t*>(info.pMappedData), bytes, mUploadPieceBytes);
        return true;
    }

    bool createUI(std::string& error)
    {
        VmaVulkanFunctions functions{};
        VmaAllocatorCreateInfo allocator{};
        allocator.physicalDevice = mGpu;
        allocator.device = mDevice;
        allocator.instance = mInstance;
        allocator.vulkanApiVersion = VK_API_VERSION_1_3;
        allocator.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        allocator.pVulkanFunctions = &functions;
        if (vmaImportVulkanFunctionsFromVolk(&allocator, &functions) != VK_SUCCESS ||
            vmaCreateAllocator(&allocator, &mAllocator) != VK_SUCCESS)
        {
            error = "cannot create the memory allocator";
            return false;
        }
        if (!createUploadRing(error))
        {
            return false;
        }

        for (uint32_t i = 0; i < uint32_t(UISampler::Count); ++i)
        {
            const UISampler kind = UISampler(i);
            const bool nearest = kind == UISampler::NearestClamp || kind == UISampler::NearestWrap;
            const bool wrap = kind == UISampler::LinearWrap || kind == UISampler::NearestWrap;
            VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sampler.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            sampler.minFilter = sampler.magFilter;
            sampler.mipmapMode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sampler.addressModeU = wrap ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler.addressModeV = sampler.addressModeU;
            sampler.addressModeW = sampler.addressModeU;
            sampler.maxLod = VK_LOD_CLAMP_NONE;
            if (vkCreateSampler(mDevice, &sampler, nullptr, &mSamplers[i]) != VK_SUCCESS)
            {
                error = "cannot create the UI samplers";
                return false;
            }
        }

        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[0].descriptorCount = MAX_TEXTURES;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[1].descriptorCount = uint32_t(UISampler::Count);
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].pImmutableSamplers = mSamplers;

        // Partly bound: most slots hold nothing. Update after bind: a new texture
        // can be added while earlier frames are still using the array.
        const VkDescriptorBindingFlags bindingFlags[2] = {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0 };
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        flags.bindingCount = 2;
        flags.pBindingFlags = bindingFlags;
        VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags };
        layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout.bindingCount = 2;
        layout.pBindings = bindings;

        const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_TEXTURES },
                                                { VK_DESCRIPTOR_TYPE_SAMPLER, uint32_t(UISampler::Count) } };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool.maxSets = 1;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;

        VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayout.setLayoutCount = 1;
        pipelineLayout.pSetLayouts = &mUISetLayout;

        if (vkCreateDescriptorSetLayout(mDevice, &layout, nullptr, &mUISetLayout) != VK_SUCCESS ||
            vkCreateDescriptorPool(mDevice, &pool, nullptr, &mUIPool) != VK_SUCCESS ||
            vkCreatePipelineLayout(mDevice, &pipelineLayout, nullptr, &mUIPipelineLayout) != VK_SUCCESS)
        {
            error = "cannot create the UI texture array";
            return false;
        }

        VkDescriptorSetAllocateInfo set{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        set.descriptorPool = mUIPool;
        set.descriptorSetCount = 1;
        set.pSetLayouts = &mUISetLayout;
        if (vkAllocateDescriptorSets(mDevice, &set, &mUISet) != VK_SUCCESS)
        {
            error = "cannot allocate the UI texture array";
            return false;
        }

        mUIVertexShader = loadShader("asrUIV", error);
        mUIFragmentShader = mUIVertexShader ? loadShader("asrUIF", error) : VK_NULL_HANDLE;
        return mUIVertexShader != VK_NULL_HANDLE && mUIFragmentShader != VK_NULL_HANDLE;
    }

    // --- Scene setup ----------------------------------------------------------

    bool createBufferObject(GpuBuffer& buffer, uint64_t bytes, VkBufferUsageFlags usage, const char* what, MemoryCategory category = MemoryCategory::Geometry)
    {
        if (!mMemoryBudget.reserve(category, bytes))
        {
            mError = std::string(what) + " does not fit in its memory budget";
            return false;
        }
        VkBufferCreateInfo create{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        create.size = bytes;
        create.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (!check(vmaCreateBuffer(mAllocator, &create, &allocation, &buffer.buffer, &buffer.allocation, nullptr), what))
        {
            mMemoryBudget.release(category, bytes);
            return false;
        }
        VkBufferDeviceAddressInfo address{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        address.buffer = buffer.buffer;
        buffer.address = vkGetBufferDeviceAddress(mDevice, &address);
        buffer.size = bytes;
        return true;
    }

    void destroyBufferObject(GpuBuffer& buffer, MemoryCategory category = MemoryCategory::Geometry)
    {
        if (buffer.buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mAllocator, buffer.buffer, buffer.allocation);
            mMemoryBudget.release(category, buffer.size);
        }
        buffer = GpuBuffer();
    }

    uint64_t createSceneChunk() override
    {
        GpuBuffer chunk;
        if (!createBufferObject(chunk, SCENE_CHUNK_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "scene geometry chunk"))
        {
            return 0;
        }
        mSceneChunks.push_back(chunk);
        return chunk.address;
    }

    VkPipeline createComputePipeline(const char* name, std::string& error)
    {
        VkShaderModule module = loadShader(name, error);
        if (module == VK_NULL_HANDLE)
        {
            return VK_NULL_HANDLE;
        }
        VkComputePipelineCreateInfo create{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        create.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        create.stage.module = module;
        create.stage.pName = "main";
        create.layout = mScenePipelineLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(mDevice, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline) != VK_SUCCESS)
        {
            error = std::string("cannot create the pipeline of ") + name;
        }
        vkDestroyShaderModule(mDevice, module, nullptr);
        return pipeline;
    }

    // Both scene raster pipelines: no vertex input, one color target, and depth on request.
    VkPipeline createSceneRasterPipeline(const char* vertexName, const char* fragmentName, VkFormat colorFormat, bool depth, std::string& error)
    {
        VkShaderModule vertex = loadShader(vertexName, error);
        VkShaderModule fragment = vertex ? loadShader(fragmentName, error) : VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (fragment)
        {
            VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                          { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertex;
            stages[0].pName = "main";
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragment;
            stages[1].pName = "main";

            VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = depth ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1.f;
            VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            depthStencil.depthTestEnable = depth;
            depthStencil.depthWriteEnable = depth;
            depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER; // reversed depth: nearer is larger
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            blend.attachmentCount = 1;
            blend.pAttachments = &attachment;
            // Double-sided lists turn culling off, with no pipelines just for that.
            const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE };
            VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamic.dynamicStateCount = depth ? 3 : 2;
            dynamic.pDynamicStates = dynamics;
            VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &colorFormat;
            rendering.depthAttachmentFormat = depth ? DEPTH_FORMAT : VK_FORMAT_UNDEFINED;

            VkGraphicsPipelineCreateInfo create{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering };
            create.stageCount = 2;
            create.pStages = stages;
            create.pVertexInputState = &vertexInput;
            create.pInputAssemblyState = &assembly;
            create.pViewportState = &viewport;
            create.pRasterizationState = &raster;
            create.pMultisampleState = &multisample;
            create.pDepthStencilState = &depthStencil;
            create.pColorBlendState = &blend;
            create.pDynamicState = &dynamic;
            create.layout = mScenePipelineLayout;
            if (vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline) != VK_SUCCESS)
            {
                error = std::string("cannot create the pipeline of ") + vertexName;
            }
        }
        if (vertex)
        {
            vkDestroyShaderModule(mDevice, vertex, nullptr);
        }
        if (fragment)
        {
            vkDestroyShaderModule(mDevice, fragment, nullptr);
        }
        return pipeline;
    }

    bool createScene(std::string& error)
    {
        // Visibility, albedo and normals, then depth, which the pyramid reads.
        VkDescriptorSetLayoutBinding bindings[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout.bindingCount = 4;
        layout.pBindings = bindings;

        const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 } };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets = 1;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;

        if (vkCreateDescriptorSetLayout(mDevice, &layout, nullptr, &mSceneSetLayout) != VK_SUCCESS ||
            vkCreateDescriptorPool(mDevice, &pool, nullptr, &mScenePool) != VK_SUCCESS)
        {
            error = "cannot create the scene descriptors";
            return false;
        }

        VkDescriptorSetAllocateInfo set{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        set.descriptorPool = mScenePool;
        set.descriptorSetCount = 1;
        set.pSetLayouts = &mSceneSetLayout;
        const VkDescriptorSetLayout sets[2] = { mUISetLayout, mSceneSetLayout };
        const VkPushConstantRange push{ SCENE_PUSH_STAGES, 0, sizeof(ScenePush) };
        VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayout.setLayoutCount = 2;
        pipelineLayout.pSetLayouts = sets;
        pipelineLayout.pushConstantRangeCount = 1;
        pipelineLayout.pPushConstantRanges = &push;
        if (vkAllocateDescriptorSets(mDevice, &set, &mSceneSet) != VK_SUCCESS ||
            vkCreatePipelineLayout(mDevice, &pipelineLayout, nullptr, &mScenePipelineLayout) != VK_SUCCESS)
        {
            error = "cannot create the scene pipeline layout";
            return false;
        }

        mCullPipeline = createComputePipeline("asrSceneCullC", error);
        mPyramidPipeline = mCullPipeline ? createComputePipeline("asrScenePyramidC", error) : VK_NULL_HANDLE;
        mResolvePipeline = mPyramidPipeline ? createComputePipeline("asrSceneResolveC", error) : VK_NULL_HANDLE;
        mVisibilityPipeline = mResolvePipeline ? createSceneRasterPipeline("asrSceneVisibilityV", "asrSceneVisibilityF", VISIBILITY_FORMAT, true, error) : VK_NULL_HANDLE;
        mMaskedPipeline = mVisibilityPipeline ? createSceneRasterPipeline("asrSceneMaskedV", "asrSceneMaskedF", VISIBILITY_FORMAT, true, error) : VK_NULL_HANDLE;
        mCompositePipeline = mMaskedPipeline ? createSceneRasterPipeline("asrSceneCompositeV", "asrSceneCompositeF", mSurfaceFormat.format, false, error) : VK_NULL_HANDLE;
        if (!mCompositePipeline)
        {
            return false;
        }

        declareSceneGraph(mGraph);
        setPassBodies();
        mResultImages[ResultVisibility] = &mVisibilityImage;
        mResultImages[ResultDepth] = &mDepthImage;
        mResultImages[ResultAlbedo] = &mAlbedoImage;
        mResultImages[ResultNormals] = &mNormalImage;

        VkQueryPoolCreateInfo queries{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queries.queryCount = (MAX_TIMED_PASSES + 1) * FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(mDevice, &queries, nullptr, &mSceneQueries) != VK_SUCCESS)
        {
            error = "cannot create the scene timers";
            return false;
        }
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(mGpu, &properties);
        mTimestampMilliseconds = properties.limits.timestampPeriod * 1e-6f;

        const VkBufferUsageFlags viewUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            if (!growBuffer(mSceneViews[i], sizeof(GpuSceneView), viewUsage, MemoryCategory::Staging, "scene view") ||
                !growBuffer(mSceneReadback[i], sizeof(GpuSceneCounts), VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryCategory::Staging, "scene readback", true) ||
                !growBuffer(mFeedbackReadback[i], FEEDBACK_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryCategory::Staging, "texture feedback readback", true))
            {
                error = mError;
                return false;
            }
        }
        if (!growBuffer(mSceneCountsTemplate, sizeof(GpuSceneCounts), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryCategory::Staging, "scene draw template") ||
            !createBufferObject(mSceneCounts, sizeof(GpuSceneCounts),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                "scene draw counts") ||
            !createBufferObject(mTextureFeedback, FEEDBACK_BYTES,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "texture feedback"))
        {
            error = mError;
            return false;
        }
        const GpuSceneCounts counts = sceneCountsTemplate();
        memcpy(mSceneCountsTemplate.mapped, &counts, sizeof(counts));
        vmaFlushAllocation(mAllocator, mSceneCountsTemplate.allocation, 0, sizeof(counts));
        return true;
    }

    bool createTarget(TargetImage& target, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t pixelBytes, const char* what)
    {
        const uint64_t bytes = uint64_t(mSceneExtent.width) * mSceneExtent.height * pixelBytes;
        if (!mMemoryBudget.reserve(MemoryCategory::RenderTargets, bytes))
        {
            mError = std::string(what) + " does not fit in the render target budget";
            return false;
        }
        VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format;
        image.extent = { mSceneExtent.width, mSceneExtent.height, 1 };
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = usage;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocation.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (!check(vmaCreateImage(mAllocator, &image, &allocation, &target.image, &target.allocation, nullptr), what))
        {
            mMemoryBudget.release(MemoryCategory::RenderTargets, bytes);
            return false;
        }
        target.bytes = bytes;
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = target.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = { aspect, 0, 1, 0, 1 };
        return check(vkCreateImageView(mDevice, &view, nullptr, &target.view), what);
    }

    void destroyTarget(TargetImage& target)
    {
        if (target.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(mDevice, target.view, nullptr);
        }
        if (target.image != VK_NULL_HANDLE)
        {
            vmaDestroyImage(mAllocator, target.image, target.allocation);
            mMemoryBudget.release(MemoryCategory::RenderTargets, target.bytes);
        }
        target = TargetImage();
    }

    // The set always names images of the current size, and frames using the old ones
    // have finished by the time it changes.
    bool ensureSceneTargets()
    {
        if (mSceneExtent.width == mExtent.width && mSceneExtent.height == mExtent.height)
        {
            return true;
        }
        if (!check(vkDeviceWaitIdle(mDevice), "wait for device"))
        {
            return false;
        }
        destroyTarget(mVisibilityImage);
        destroyTarget(mDepthImage);
        destroyTarget(mAlbedoImage);
        destroyTarget(mNormalImage);
        destroyTarget(mAuditImage);
        destroyBufferObject(mScenePyramid, MemoryCategory::RenderTargets);
        for (bool& written : mAuditWritten)
        {
            written = false;
        }
        mHasPyramid = false;
        mSceneExtent = mExtent;

        if (!createTarget(mVisibilityImage, VISIBILITY_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 4, "visibility buffer") ||
            !createTarget(mDepthImage, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 4, "depth buffer") ||
            !createTarget(mAlbedoImage, ALBEDO_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 4, "albedo target") ||
            !createTarget(mNormalImage, NORMAL_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 8, "normal target"))
        {
            return false;
        }

        uint32_t offsets[PYRAMID_MAX_LEVELS];
        uint32_t levels = 0;
        const uint64_t pyramidBytes = uint64_t(pyramidLayout(mSceneExtent.width, mSceneExtent.height, offsets, levels)) * sizeof(float);
        if (!createBufferObject(mScenePyramid, pyramidBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "depth pyramid", MemoryCategory::RenderTargets))
        {
            return false;
        }

        const VkImageView views[4] = { mVisibilityImage.view, mAlbedoImage.view, mNormalImage.view, mDepthImage.view };
        VkDescriptorImageInfo images[4]{};
        VkWriteDescriptorSet writes[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            images[i].imageView = views[i];
            images[i].imageLayout = i == 3 ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = mSceneSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(mDevice, 4, writes, 0, nullptr);
        return true;
    }

    void destroyScene()
    {
        for (GpuBuffer& chunk : mSceneChunks)
        {
            destroyBufferObject(chunk);
        }
        mSceneChunks.clear();
        for (GpuBuffer& table : mSceneTables)
        {
            destroyBufferObject(table);
        }
        for (RetiredBuffer& retired : mRetiredBuffers)
        {
            destroyBufferObject(retired.buffer);
        }
        mRetiredBuffers.clear();
        destroyBufferObject(mSceneVisible);
        destroyBufferObject(mSceneRetest);
        destroyBufferObject(mSceneGroups);
        destroyBufferObject(mSceneCounts);
        destroyBufferObject(mTextureFeedback);
        destroyBufferObject(mScenePyramid, MemoryCategory::RenderTargets);
        destroyBuffer(mSceneCountsTemplate, MemoryCategory::Staging);
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            destroyBuffer(mSceneViews[i], MemoryCategory::Staging);
            destroyBuffer(mSceneStaging[i], MemoryCategory::Staging);
            destroyBuffer(mSceneReadback[i], MemoryCategory::Staging);
            destroyBuffer(mFeedbackReadback[i], MemoryCategory::Staging);
            destroyBuffer(mAuditReadback[i], MemoryCategory::Staging);
        }
        destroyTarget(mVisibilityImage);
        destroyTarget(mDepthImage);
        destroyTarget(mAlbedoImage);
        destroyTarget(mNormalImage);
        destroyTarget(mAuditImage);
        for (VkPipeline pipeline : { mCullPipeline, mPyramidPipeline, mVisibilityPipeline, mMaskedPipeline, mResolvePipeline, mCompositePipeline })
        {
            if (pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(mDevice, pipeline, nullptr);
            }
        }
        if (mSceneQueries != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(mDevice, mSceneQueries, nullptr);
        }
        if (mScenePipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice, mScenePipelineLayout, nullptr);
        }
        if (mScenePool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice, mScenePool, nullptr);
        }
        if (mSceneSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice, mSceneSetLayout, nullptr);
        }
    }

    // Creating a pipeline is slow, so each is made once, on first use, and UI code
    // only ever uses a handful.
    VkPipeline uiPipeline(const UIBlend& blendState, UIPrimitive primitive)
    {
        const uint32_t key = blendState.key() | uint32_t(primitive) << 16;
        const auto found = mUIPipelines.find(key);
        if (found != mUIPipelines.end())
        {
            return found->second;
        }

        VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = mUIVertexShader;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = mUIFragmentShader;
        stages[1].pName = "main";

        const VkVertexInputBindingDescription binding{ 0, sizeof(UIVertex), VK_VERTEX_INPUT_RATE_VERTEX };
        const VkVertexInputAttributeDescription attributes[4] = {
            { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UIVertex, position) },
            { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVertex, uv) },
            { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(UIVertex, color) },
            { 3, 0, VK_FORMAT_R32_UINT, offsetof(UIVertex, texture) } };
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 4;
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = primitive == UIPrimitive::Triangles ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                          : primitive == UIPrimitive::Lines     ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = toVulkan(blendState.colorSource);
        attachment.dstColorBlendFactor = toVulkan(blendState.colorDest);
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = toVulkan(blendState.alphaSource);
        attachment.dstAlphaBlendFactor = toVulkan(blendState.alphaDest);
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;

        const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamics;
        VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &mSurfaceFormat.format;

        VkGraphicsPipelineCreateInfo create{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering };
        create.stageCount = 2;
        create.pStages = stages;
        create.pVertexInputState = &vertexInput;
        create.pInputAssemblyState = &assembly;
        create.pViewportState = &viewport;
        create.pRasterizationState = &raster;
        create.pMultisampleState = &multisample;
        create.pColorBlendState = &blend;
        create.pDynamicState = &dynamic;
        create.layout = mUIPipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (!check(vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline), "create UI pipeline"))
        {
            return VK_NULL_HANDLE;
        }
        mUIPipelines[key] = pipeline;
        return pipeline;
    }

    // Makes the buffer hold at least bytes. The frame using it has finished, so
    // the old buffer can go at once. cpuReads asks for memory the CPU reads quickly:
    // the default is video memory it writes directly, where a read can cost hundreds of
    // milliseconds for one full-screen image.
    bool growBuffer(MappedBuffer& buffer, uint64_t bytes, VkBufferUsageFlags usage, MemoryCategory category, const char* what, bool cpuReads = false)
    {
        if (buffer.buffer != VK_NULL_HANDLE && buffer.size >= bytes)
        {
            return true;
        }
        const uint64_t size = std::max<uint64_t>(std::max(bytes, buffer.size * 2), 1024 * 1024);
        destroyBuffer(buffer, category);
        if (!mMemoryBudget.reserve(category, size))
        {
            mError = std::string(what) + " does not fit in its memory budget";
            return false;
        }

        VkBufferCreateInfo create{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        create.size = size;
        create.usage = usage;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = cpuReads ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                         | (cpuReads ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        VmaAllocationInfo info{};
        if (!check(vmaCreateBuffer(mAllocator, &create, &allocation, &buffer.buffer, &buffer.allocation, &info), what))
        {
            mMemoryBudget.release(category, size);
            return false;
        }
        buffer.mapped = info.pMappedData;
        buffer.size = size;
        return true;
    }

    void destroyBuffer(MappedBuffer& buffer, MemoryCategory category)
    {
        if (buffer.buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mAllocator, buffer.buffer, buffer.allocation);
            mMemoryBudget.release(category, buffer.size);
        }
        buffer = MappedBuffer();
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
            if (!growBuffer(mUIBuffers[slot], bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, MemoryCategory::UI, "UI vertex buffer"))
            {
                return false;
            }
            memcpy(mUIBuffers[slot].mapped, ui->vertices().data(), bytes);
            vmaFlushAllocation(mAllocator, mUIBuffers[slot].allocation, 0, bytes);
            mUIBufferGeneration[slot] = ui->generation;
        }
        if (mUIPipelineGeneration != ui->generation)
        {
            // Only what the list uses gets made.
            mUIDrawPipelines.assign(ui->blends().size() * size_t(UIPrimitive::Count), VK_NULL_HANDLE);
            for (const UIDraw& draw : ui->draws())
            {
                VkPipeline& pipeline = mUIDrawPipelines[draw.blend * size_t(UIPrimitive::Count) + uint32_t(draw.primitive)];
                if (pipeline == VK_NULL_HANDLE)
                {
                    pipeline = uiPipeline(ui->blends()[draw.blend], draw.primitive);
                    if (pipeline == VK_NULL_HANDLE)
                    {
                        return false;
                    }
                }
            }
            mUIPipelineGeneration = ui->generation;
        }
        return true;
    }

    static void textureBarrier(VkCommandBuffer commands, VkImage image, uint32_t mipLevels, VkImageLayout from, VkImageLayout to,
                               VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                               VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, uint32_t firstLevel = 0)
    {
        VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, firstLevel, mipLevels, 0, 1 };
        VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    void writeTextureDescriptor(uint32_t slot, VkImageView view)
    {
        VkDescriptorImageInfo image{ VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = mUISet;
        write.dstBinding = 0;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
    }

    // Fills a new texture with the mips a larger one already has, with no pixels sent.
    void copySmallerMips(VkCommandBuffer commands, const Texture& texture, const Texture& source, uint32_t sourceMip)
    {
        constexpr VkPipelineStageFlags2 COPY = VK_PIPELINE_STAGE_2_COPY_BIT;
        textureBarrier(commands, texture.image, texture.mipLevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, COPY, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        textureBarrier(commands, source.image, source.mipLevels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       TEXTURE_READ_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, COPY, VK_ACCESS_2_TRANSFER_READ_BIT);

        std::vector<VkImageCopy> copies(texture.mipLevels);
        for (uint32_t level = 0; level < texture.mipLevels; ++level)
        {
            VkImageCopy& copy = copies[level];
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, sourceMip + level, 0, 1 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
            copy.extent = { std::max(texture.width >> level, 1u), std::max(texture.height >> level, 1u), 1 };
        }
        vkCmdCopyImage(commands, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       uint32_t(copies.size()), copies.data());

        textureBarrier(commands, source.image, source.mipLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       COPY, VK_ACCESS_2_TRANSFER_READ_BIT, TEXTURE_READ_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        textureBarrier(commands, texture.image, texture.mipLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       COPY, VK_ACCESS_2_TRANSFER_WRITE_BIT, TEXTURE_READ_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // The memory was counted when the main thread asked for this texture. Video memory
    // really running out shows as white and is not an error.
    bool createTexture(VkCommandBuffer commands, const TextureCommand& command)
    {
        if (mTextureObjects.size() <= command.slot)
        {
            mTextureObjects.resize(command.slot + 1);
        }
        Texture& texture = mTextureObjects[command.slot];
        const uint64_t bytes = textureBytes(command.desc);

        VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = command.desc.format == TextureFormat::R8    ? VK_FORMAT_R8_UNORM
                     : command.desc.format == TextureFormat::RG8   ? VK_FORMAT_R8G8_UNORM
                     : command.desc.format == TextureFormat::BGRA8 ? VK_FORMAT_B8G8R8A8_UNORM
                                                                   : VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = { command.desc.width, command.desc.height, 1 };
        image.mipLevels = command.desc.mipLevels;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (command.desc.mipLevels > 1)
        {
            image.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // each mip is made from the one before it
        }
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(mAllocator, &image, &allocation, &texture.image, &texture.allocation, nullptr) != VK_SUCCESS)
        {
            texture.image = VK_NULL_HANDLE;
            mMemoryBudget.release(MemoryCategory::Textures, bytes);
            writeTextureDescriptor(command.slot, mTextureObjects[0].view);
            return true;
        }
        texture.bytes = bytes;
        texture.mipLevels = command.desc.mipLevels;
        texture.width = command.desc.width;
        texture.height = command.desc.height;

        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = texture.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, command.desc.mipLevels, 0, 1 };
        if (command.desc.swizzle == TextureSwizzle::AlphaOnly)
        {
            view.components = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
        }
        else if (command.desc.swizzle == TextureSwizzle::Luminance)
        {
            view.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
        }
        else if (command.desc.swizzle == TextureSwizzle::LuminanceAlpha)
        {
            view.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G };
        }
        if (!check(vkCreateImageView(mDevice, &view, nullptr, &texture.view), "create texture view"))
        {
            return false;
        }

        const Texture& source = mTextureObjects[command.sourceSlot];
        if (command.sourceSlot != 0 && source.image != VK_NULL_HANDLE && source.mipLevels >= command.sourceMip + texture.mipLevels)
        {
            copySmallerMips(commands, texture, source, command.sourceMip);
            writeTextureDescriptor(command.slot, texture.view);
            return true;
        }

        // Cleared, because a list can draw this texture before its pixels arrive.
        textureBarrier(commands, texture.image, texture.mipLevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue transparent{};
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.mipLevels, 0, 1 };
        vkCmdClearColorImage(commands, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent, 1, &range);
        textureBarrier(commands, texture.image, texture.mipLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       TEXTURE_READ_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        writeTextureDescriptor(command.slot, texture.view);
        return true;
    }

    // Copies one piece of an upload from the ring, where the main thread wrote it.
    void uploadTexture(VkCommandBuffer commands, const TextureCommand& command)
    {
        const Texture& texture = mTextureObjects[command.slot];
        if (texture.image == VK_NULL_HANDLE)
        {
            return; // did not fit in the budget
        }

        textureBarrier(commands, texture.image, texture.mipLevels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       TEXTURE_READ_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = command.ringOffset;
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, command.level, 0, 1 };
        copy.imageOffset = { int32_t(command.x), int32_t(command.y), 0 };
        copy.imageExtent = { command.width, command.height, 1 };
        vkCmdCopyBufferToImage(commands, mUploadRing.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        textureBarrier(commands, texture.image, texture.mipLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       TEXTURE_READ_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    void generateMips(VkCommandBuffer commands, uint32_t slot)
    {
        const Texture& texture = mTextureObjects[slot];
        if (texture.image == VK_NULL_HANDLE || texture.mipLevels < 2)
        {
            return;
        }

        constexpr VkPipelineStageFlags2 SHADER = TEXTURE_READ_STAGES;
        constexpr VkPipelineStageFlags2 BLIT = VK_PIPELINE_STAGE_2_BLIT_BIT;
        textureBarrier(commands, texture.image, texture.mipLevels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       SHADER, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, BLIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        int32_t width = int32_t(texture.width);
        int32_t height = int32_t(texture.height);
        for (uint32_t level = 1; level < texture.mipLevels; ++level)
        {
            textureBarrier(commands, texture.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           BLIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, BLIT, VK_ACCESS_2_TRANSFER_READ_BIT, level - 1);

            const int32_t nextWidth = std::max(width / 2, 1);
            const int32_t nextHeight = std::max(height / 2, 1);
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
            blit.srcOffsets[1] = { width, height, 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
            blit.dstOffsets[1] = { nextWidth, nextHeight, 1 };
            vkCmdBlitImage(commands, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);
            width = nextWidth;
            height = nextHeight;
        }

        textureBarrier(commands, texture.image, texture.mipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       BLIT, VK_ACCESS_2_TRANSFER_READ_BIT, SHADER, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        textureBarrier(commands, texture.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       BLIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, SHADER, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, texture.mipLevels - 1);
    }

    void destroyTexture(uint32_t slot)
    {
        Texture& texture = mTextureObjects[slot];
        if (texture.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(mDevice, texture.view, nullptr);
        }
        if (texture.image != VK_NULL_HANDLE)
        {
            vmaDestroyImage(mAllocator, texture.image, texture.allocation);
            mMemoryBudget.release(MemoryCategory::Textures, texture.bytes);
        }
        texture = Texture();

        // A material can still name this slot for a moment, and white is safe to read.
        if (slot != 0 && !mShuttingDown && mTextureObjects[0].view != VK_NULL_HANDLE)
        {
            writeTextureDescriptor(slot, mTextureObjects[0].view);
        }
    }

    // Runs what the main thread queued, in order, until this frame's time for it or
    // its share of the ring runs out. The rest waits for the next frame.
    bool runTextureCommands(VkCommandBuffer commands, uint32_t frameSlot)
    {
        mTextures.take(mTextureCommands);

        using Clock = std::chrono::steady_clock;
        const Clock::time_point start = Clock::now();
        const auto limit = std::chrono::duration<double>(textureCommandSeconds(mFrameSeconds));
        uint64_t uploaded = 0;
        size_t i = mTextureCommandsDone;
        for (; i < mTextureCommands.size(); ++i)
        {
            TextureCommand& command = mTextureCommands[i];
            if (i != mTextureCommandsDone && Clock::now() - start > limit)
            {
                break;
            }
            if (command.type == TextureCommand::Type::Upload && uploaded != 0 && uploaded + command.ringBytes > mUploadPieceBytes)
            {
                break;
            }
            switch (command.type)
            {
                case TextureCommand::Type::Create:
                    if (!createTexture(commands, command))
                    {
                        return false;
                    }
                    ++mFrameParts.texturesCreated;
                    break;
                case TextureCommand::Type::Upload:
                    // A no-op on the coherent memory desktop GPUs give, needed on the rest.
                    vmaFlushAllocation(mAllocator, mUploadRing.allocation, command.ringOffset, command.ringBytes);
                    uploadTexture(commands, command);
                    uploaded += command.ringBytes;
                    mRingEnd[frameSlot] = command.ringEnd;
                    break;
                case TextureCommand::Type::GenerateMips:
                    generateMips(commands, command.slot);
                    break;
                case TextureCommand::Type::Destroy:
                    mRetiredTextures.add(command.slot, command.lastList);
                    break;
            }
        }
        mFrameParts.textureUploadBytes = uploaded;
        mFrameParts.textureCommandsLeft = uint32_t(mTextureCommands.size() - i);

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

    void drawUI(VkCommandBuffer commands, uint32_t slot, const UIList& ui)
    {
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commands, 0, 1, &mUIBuffers[slot].buffer, &offset);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, mUIPipelineLayout, 0, 1, &mUISet, 0, nullptr);
        const VkViewport viewport{ 0.f, UI_EDGE_NUDGE, float(ui.width()), float(ui.height()), 0.f, 1.f };
        vkCmdSetViewport(commands, 0, 1, &viewport);

        VkPipeline bound = VK_NULL_HANDLE;
        for (const UIDraw& draw : ui.draws())
        {
            UIRect rect;
            if (!resolveScissor(draw.scissor, ui.width(), ui.height(), mExtent.width, mExtent.height, rect))
            {
                continue;
            }
            const VkPipeline pipeline = mUIDrawPipelines[draw.blend * size_t(UIPrimitive::Count) + uint32_t(draw.primitive)];
            if (pipeline != bound)
            {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                bound = pipeline;
            }
            const VkRect2D scissor{ { int32_t(rect.x), int32_t(rect.y) }, { rect.width, rect.height } };
            vkCmdSetScissor(commands, 0, 1, &scissor);
            vkCmdDraw(commands, draw.vertexCount, 1, draw.firstVertex, 0);
        }
    }

    // --- Scene frame ----------------------------------------------------------

    static void memoryBarrier(VkCommandBuffer commands, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                              VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
    {
        VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    static VkPipelineStageFlags2 stagesOf(uint32_t uses)
    {
        VkPipelineStageFlags2 stages = 0;
        if (uses & (GraphAccess::CopyRead | GraphAccess::CopyWrite))
        {
            stages |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        }
        if (uses & GraphAccess::Indirect)
        {
            stages |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        }
        if (uses & GraphAccess::VertexRead)
        {
            stages |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        }
        if (uses & GraphAccess::FragmentRead)
        {
            stages |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        }
        if (uses & (GraphAccess::ComputeRead | GraphAccess::ComputeWrite))
        {
            stages |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        }
        if (uses & GraphAccess::ColorTarget)
        {
            stages |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        }
        if (uses & GraphAccess::DepthTarget)
        {
            stages |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        }
        return stages;
    }

    static VkAccessFlags2 writeAccessOf(uint32_t uses)
    {
        VkAccessFlags2 access = 0;
        if (uses & GraphAccess::CopyWrite)
        {
            access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
        }
        if (uses & GraphAccess::ComputeWrite)
        {
            access |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        }
        if (uses & GraphAccess::ColorTarget)
        {
            access |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        if (uses & GraphAccess::DepthTarget)
        {
            access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }
        return access;
    }

    static VkAccessFlags2 accessOf(uint32_t uses)
    {
        VkAccessFlags2 access = writeAccessOf(uses);
        if (uses & GraphAccess::CopyRead)
        {
            access |= VK_ACCESS_2_TRANSFER_READ_BIT;
        }
        if (uses & GraphAccess::Indirect)
        {
            access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        }
        if (uses & (GraphAccess::VertexRead | GraphAccess::FragmentRead | GraphAccess::ComputeRead | GraphAccess::ComputeWrite))
        {
            access |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        }
        if (uses & GraphAccess::ColorTarget)
        {
            access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        }
        if (uses & GraphAccess::DepthTarget)
        {
            access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        }
        return access;
    }

    static VkImageLayout layoutOf(uint32_t uses, VkImageAspectFlags aspect)
    {
        if (uses & GraphAccess::ColorTarget)
        {
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        if (uses & GraphAccess::DepthTarget)
        {
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        }
        if (uses & GraphAccess::CopyRead)
        {
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        return aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    }

    // One barrier call per pass, whatever it needs: buffers share one memory barrier.
    void applyTransitions(VkCommandBuffer commands, const GraphTransition* transitions, uint32_t count)
    {
        VkMemoryBarrier2 memory{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        VkImageMemoryBarrier2 images[ResultCount];
        uint32_t imageCount = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const GraphTransition& transition = transitions[i];
            const TargetImage* target = mResultImages[transition.result];
            if (!target)
            {
                memory.srcStageMask |= stagesOf(transition.source);
                memory.srcAccessMask |= writeAccessOf(transition.source);
                memory.dstStageMask |= stagesOf(transition.destination);
                memory.dstAccessMask |= accessOf(transition.destination);
                continue;
            }
            const VkImageAspectFlags aspect = target == &mDepthImage ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageMemoryBarrier2& barrier = images[imageCount++];
            barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = stagesOf(transition.source);
            barrier.srcAccessMask = writeAccessOf(transition.source);
            barrier.dstStageMask = stagesOf(transition.destination);
            barrier.dstAccessMask = accessOf(transition.destination);
            barrier.oldLayout = transition.discard ? VK_IMAGE_LAYOUT_UNDEFINED : layoutOf(transition.previous, aspect);
            barrier.newLayout = layoutOf(transition.destination, aspect);
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = target->image;
            barrier.subresourceRange = { aspect, 0, 1, 0, 1 };
        }
        VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = memory.dstStageMask != 0 ? 1 : 0;
        dependency.pMemoryBarriers = &memory;
        dependency.imageMemoryBarrierCount = imageCount;
        dependency.pImageMemoryBarriers = images;
        if (dependency.memoryBarrierCount + imageCount != 0)
        {
            vkCmdPipelineBarrier2(commands, &dependency);
        }
    }

    // Frames still running may read the old table, so it stays until they are done.
    bool growTable(VkCommandBuffer commands, GpuBuffer& table, uint64_t bytes)
    {
        GpuBuffer grown;
        if (!createBufferObject(grown, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "scene table"))
        {
            return false;
        }
        vkCmdFillBuffer(commands, grown.buffer, 0, VK_WHOLE_SIZE, 0);
        memoryBarrier(commands, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (table.buffer != VK_NULL_HANDLE)
        {
            const VkBufferCopy copy{ 0, 0, table.size };
            vkCmdCopyBuffer(commands, table.buffer, grown.buffer, 1, &copy);
            memoryBarrier(commands, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            mRetiredBuffers.push_back({ table, mFrame + FRAMES_IN_FLIGHT + 1 });
        }
        table = grown;
        return true;
    }

    void flushSceneRegions(VkCommandBuffer commands, uint32_t slot, VkBuffer target)
    {
        if (!mSceneRegions.empty())
        {
            vkCmdCopyBuffer(commands, mSceneStaging[slot].buffer, target, uint32_t(mSceneRegions.size()), mSceneRegions.data());
            mSceneRegions.clear();
        }
    }

    bool growList(GpuBuffer& list, uint64_t bytes, const char* what)
    {
        if (bytes <= list.size)
        {
            return true;
        }
        if (list.buffer != VK_NULL_HANDLE)
        {
            mRetiredBuffers.push_back({ list, mFrame + FRAMES_IN_FLIGHT + 1 });
            list = GpuBuffer();
        }
        return createBufferObject(list, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, what);
    }

    // Applies this frame's scene changes on the GPU, and fills in the view.
    bool passUpload()
    {
        VkCommandBuffer commands = mRecordCommands;
        const uint32_t slot = mRecordSlot;
        while (!mRetiredBuffers.empty() && mRetiredBuffers.front().frame <= mFrame)
        {
            destroyBufferObject(mRetiredBuffers.front().buffer);
            mRetiredBuffers.pop_front();
        }

        const auto updateStart = std::chrono::steady_clock::now();
        mSceneStore.update(mScene, *this, mFrame, mUploadBytesPerFrame);
        const auto growthStart = std::chrono::steady_clock::now();
        mFrameParts.sceneUpdate = std::chrono::duration<double>(growthStart - updateStart).count();

        for (uint32_t i = 0; i < SCENE_TABLES; ++i)
        {
            const uint64_t bytes = mSceneStore.tableCapacity(SceneTable(i));
            if (bytes > mSceneTables[i].size && !growTable(commands, mSceneTables[i], bytes))
            {
                return false;
            }
        }

        const uint64_t visibleBytes = mSceneStore.visibleCapacity() * sizeof(GpuListEntry);
        const uint64_t groupBytes = mSceneStore.groupCapacity() * sizeof(GpuListEntry);
        const uint64_t retestBytes = (mSceneStore.visibleCapacity() + mSceneStore.groupCapacity() + mSceneStore.instanceEnd()) * sizeof(GpuListEntry);
        if (!growList(mSceneVisible, visibleBytes, "visible cluster list") || !growList(mSceneRetest, retestBytes, "retest list") ||
            !growList(mSceneGroups, groupBytes, "group list"))
        {
            return false;
        }
        const auto stagingStart = std::chrono::steady_clock::now();
        mFrameParts.sceneGrowth = std::chrono::duration<double>(stagingStart - growthStart).count();

        uint64_t stagingBytes = 0;
        for (const SceneWrite& write : mSceneStore.writes())
        {
            stagingBytes += (write.bytes + UPLOAD_ALIGNMENT - 1) / UPLOAD_ALIGNMENT * UPLOAD_ALIGNMENT;
        }
        if (stagingBytes != 0)
        {
            if (!growBuffer(mSceneStaging[slot], std::max(stagingBytes, mUploadBytesPerFrame), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryCategory::Staging, "scene staging buffer"))
            {
                return false;
            }
            uint64_t offset = 0;
            VkBuffer target = VK_NULL_HANDLE;
            const uint8_t* tableBytes = mSceneStore.tableBytes().data();
            for (const SceneWrite& write : mSceneStore.writes())
            {
                const VkBuffer buffer = write.table != SceneTable::Count ? mSceneTables[uint32_t(write.table)].buffer : mSceneChunks[write.chunk].buffer;
                if (buffer != target)
                {
                    flushSceneRegions(commands, slot, target);
                    target = buffer;
                }
                memcpy(static_cast<char*>(mSceneStaging[slot].mapped) + offset, write.data ? write.data : tableBytes + write.source, write.bytes);
                mSceneRegions.push_back({ offset, write.offset, write.bytes });
                offset += (write.bytes + UPLOAD_ALIGNMENT - 1) / UPLOAD_ALIGNMENT * UPLOAD_ALIGNMENT;
            }
            flushSceneRegions(commands, slot, target);
            vmaFlushAllocation(mAllocator, mSceneStaging[slot].allocation, 0, offset);
        }
        mFrameParts.sceneStaging = std::chrono::duration<double>(std::chrono::steady_clock::now() - stagingStart).count();
        mFrameParts.sceneStagingBytes = stagingBytes;

        const VkBufferCopy reset{ 0, 0, sizeof(GpuSceneCounts) };
        vkCmdCopyBuffer(commands, mSceneCountsTemplate.buffer, mSceneCounts.buffer, 1, &reset);

        GpuSceneView view{};
        fillSceneView(view, *mRecordCamera, mHasPyramid ? &mPyramidCamera : nullptr, mSceneExtent.width, mSceneExtent.height);
        uint32_t base = 0;
        for (uint32_t list = 0; list < SCENE_LISTS; ++list)
        {
            view.listBase[list] = base;
            base += mSceneStore.listCapacity(list);
        }
        view.instanceEnd = mSceneStore.instanceEnd();
        view.feedback = mFeedbackValue;
        mSceneInstanceEnd = view.instanceEnd;
        mPyramidLevels = view.pyramidLevels;
        memcpy(mSceneViews[slot].mapped, &view, sizeof(view));
        vmaFlushAllocation(mAllocator, mSceneViews[slot].allocation, 0, sizeof(view));

        VkBufferDeviceAddressInfo address{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        address.buffer = mSceneViews[slot].buffer;
        mScenePush = ScenePush{};
        mScenePush.view = vkGetBufferDeviceAddress(mDevice, &address);
        mScenePush.instances = mSceneTables[uint32_t(SceneTable::Instances)].address;
        mScenePush.geometries = mSceneTables[uint32_t(SceneTable::Geometries)].address;
        mScenePush.clusters = mSceneTables[uint32_t(SceneTable::Clusters)].address;
        mScenePush.materials = mSceneTables[uint32_t(SceneTable::Materials)].address;
        mScenePush.visible = mSceneVisible.address;
        mScenePush.counts = mSceneCounts.address;
        mScenePush.retest = mSceneRetest.address;
        mScenePush.pyramid = mScenePyramid.address;
        mScenePush.groups = mSceneGroups.address;
        mScenePush.feedback = mTextureFeedback.address;
        return true;
    }

    void pushScene(VkCommandBuffer commands)
    {
        vkCmdPushConstants(commands, mScenePipelineLayout, SCENE_PUSH_STAGES, 0, sizeof(mScenePush), &mScenePush);
    }

    bool passCull(uint32_t phase)
    {
        VkCommandBuffer commands = mRecordCommands;
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mCullPipeline);
        mScenePush.phase = phase;
        pushScene(commands);
        if (phase == CULL_LATE)
        {
            vkCmdDispatchIndirect(commands, mSceneCounts.buffer, offsetof(GpuSceneCounts, retest));
        }
        else if (phase >= CULL_GROUPS_EARLY)
        {
            const uint32_t work = phase == CULL_GROUPS_LATE ? 1 : 0;
            vkCmdDispatchIndirect(commands, mSceneCounts.buffer, offsetof(GpuSceneCounts, groups) + work * sizeof(GpuWorkCount));
        }
        else
        {
            vkCmdDispatch(commands, (mSceneInstanceEnd + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE, 1, 1);
        }
        return true;
    }

    bool passDraw(VkImageView colorView, uint64_t argumentsOffset, bool loadColor, bool loadDepth)
    {
        VkCommandBuffer commands = mRecordCommands;
        VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        color.imageView = colorView;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = loadColor ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color.uint32[0] = VISIBILITY_EMPTY;
        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView = mDepthImage.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = loadDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = { 0.f, 0 }; // reversed depth: 0 is far away
        VkRenderingInfo rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        rendering.renderArea = { { 0, 0 }, mSceneExtent };
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(commands, &rendering);

        const VkViewport viewport{ 0.f, 0.f, float(mSceneExtent.width), float(mSceneExtent.height), 0.f, 1.f };
        const VkRect2D scissor{ { 0, 0 }, mSceneExtent };
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, mScenePipelineLayout, 0, 1, &mUISet, 0, nullptr);
        pushScene(commands);
        for (uint32_t kind = 0; kind < SCENE_KINDS; ++kind)
        {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, (kind & 2) ? mMaskedPipeline : mVisibilityPipeline);
            vkCmdSetCullMode(commands, (kind & 1) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            for (uint32_t bucket = 0; bucket < CLUSTER_BUCKETS; ++bucket)
            {
                const uint32_t list = sceneList(kind, bucket);
                vkCmdPushConstants(commands, mScenePipelineLayout, SCENE_PUSH_STAGES, offsetof(ScenePush, list), sizeof(list), &list);
                vkCmdDrawIndirect(commands, mSceneCounts.buffer, argumentsOffset + list * sizeof(GpuDrawArguments), 1, sizeof(GpuDrawArguments));
            }
        }
        vkCmdEndRendering(commands);
        return true;
    }

    bool passPyramid()
    {
        VkCommandBuffer commands = mRecordCommands;
        const VkDescriptorSet sets[2] = { mUISet, mSceneSet };
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPyramidPipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mScenePipelineLayout, 0, 2, sets, 0, nullptr);
        for (uint32_t source = 0; source < mPyramidLevels; source += PYRAMID_LEVELS_PER_DISPATCH)
        {
            if (source != 0)
            {
                memoryBarrier(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            }
            mScenePush.phase = source;
            pushScene(commands);
            const uint32_t coarsest = source + PYRAMID_LEVELS_PER_DISPATCH;
            vkCmdDispatch(commands, (pyramidLevelSize(mSceneExtent.width, coarsest) + PYRAMID_GROUP_SIZE - 1) / PYRAMID_GROUP_SIZE,
                          (pyramidLevelSize(mSceneExtent.height, coarsest) + PYRAMID_GROUP_SIZE - 1) / PYRAMID_GROUP_SIZE, 1);
        }
        return true;
    }

    bool passResolve()
    {
        VkCommandBuffer commands = mRecordCommands;
        const VkDescriptorSet sets[2] = { mUISet, mSceneSet };
        if (mFeedbackCollector.takeClear())
        {
            // The graph's barrier before this pass is for the resolve, so the fill has its own.
            memoryBarrier(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdFillBuffer(commands, mTextureFeedback.buffer, 0, FEEDBACK_BYTES, 0);
            memoryBarrier(commands, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mResolvePipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mScenePipelineLayout, 0, 2, sets, 0, nullptr);
        pushScene(commands);
        vkCmdDispatch(commands, (mSceneExtent.width + RESOLVE_GROUP_SIZE - 1) / RESOLVE_GROUP_SIZE,
                      (mSceneExtent.height + RESOLVE_GROUP_SIZE - 1) / RESOLVE_GROUP_SIZE, 1);
        return true;
    }

    bool passReadFeedback()
    {
        const VkBufferCopy readback{ 0, 0, FEEDBACK_BYTES };
        vkCmdCopyBuffer(mRecordCommands, mTextureFeedback.buffer, mFeedbackReadback[mRecordSlot].buffer, 1, &readback);
        mFeedbackRead[mRecordSlot] = mFeedbackEnding;
        return true;
    }

    bool passReadCounts()
    {
        const VkBufferCopy readback{ 0, 0, sizeof(GpuSceneCounts) };
        vkCmdCopyBuffer(mRecordCommands, mSceneCounts.buffer, mSceneReadback[mRecordSlot].buffer, 1, &readback);
        return true;
    }

    bool passAuditReset()
    {
        const VkBufferCopy reset{ 0, 0, sizeof(GpuSceneCounts) };
        vkCmdCopyBuffer(mRecordCommands, mSceneCountsTemplate.buffer, mSceneCounts.buffer, 1, &reset);
        return true;
    }

    bool passAuditRead()
    {
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { mSceneExtent.width, mSceneExtent.height, 1 };
        vkCmdCopyImageToBuffer(mRecordCommands, mAuditImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mAuditReadback[mRecordSlot].buffer, 1, &region);
        mAuditWritten[mRecordSlot] = true;
        return true;
    }

    void setPassBodies()
    {
        mGraph.setBody(PassUpload, [this]() { return passUpload(); });
        mGraph.setBody(PassCullEarly, [this]() { return passCull(CULL_EARLY); });
        mGraph.setBody(PassGroupsEarly, [this]() { return passCull(CULL_GROUPS_EARLY); });
        mGraph.setBody(PassGroupsLate, [this]() { return passCull(CULL_GROUPS_LATE); });
        mGraph.setBody(PassAuditGroups, [this]() { return passCull(CULL_GROUPS_AUDIT); });
        mGraph.setBody(PassDrawEarly, [this]() { return passDraw(mVisibilityImage.view, offsetof(GpuSceneCounts, early), false, false); });
        mGraph.setBody(PassPyramidEarly, [this]() { return passPyramid(); });
        mGraph.setBody(PassCullLate, [this]() { return passCull(CULL_LATE); });
        mGraph.setBody(PassDrawLate, [this]() { return passDraw(mVisibilityImage.view, offsetof(GpuSceneCounts, late), true, true); });
        mGraph.setBody(PassPyramidFinal, [this]() { return passPyramid(); });
        mGraph.setBody(PassResolve, [this]() { return passResolve(); });
        mGraph.setBody(PassReadCounts, [this]() { return passReadCounts(); });
        mGraph.setBody(PassAuditReset, [this]() { return passAuditReset(); });
        mGraph.setBody(PassAuditCull, [this]() { return passCull(CULL_AUDIT); });
        mGraph.setBody(PassAuditDraw, [this]() { return passDraw(mAuditImage.view, offsetof(GpuSceneCounts, early), false, true); });
        mGraph.setBody(PassAuditRead, [this]() { return passAuditRead(); });
        mGraph.setBody(PassReadFeedback, [this]() { return passReadFeedback(); });
    }

    // What this slot's earlier frame left behind. It has finished: its fence was waited on.
    void readSceneResults(uint32_t slot)
    {
        const uint32_t timed = mTimedPasses[slot];
        if (timed != 0)
        {
            uint64_t times[MAX_TIMED_PASSES + 1];
            if (vkGetQueryPoolResults(mDevice, mSceneQueries, slot * (MAX_TIMED_PASSES + 1), timed + 1, sizeof(times), times,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                mSceneFrameStats.passCount = timed;
                for (uint32_t i = 0; i < timed; ++i)
                {
                    recordPassTime(mSceneFrameStats.passes[i], mTimedNames[slot][i], double(times[i + 1] - times[i]) * mTimestampMilliseconds, 0.0);
                }
            }
            vmaInvalidateAllocation(mAllocator, mSceneReadback[slot].allocation, 0, sizeof(GpuSceneCounts));
            const GpuSceneCounts* counts = static_cast<const GpuSceneCounts*>(mSceneReadback[slot].mapped);
            mSceneFrameStats.earlyClusters = 0;
            mSceneFrameStats.lateClusters = 0;
            for (uint32_t list = 0; list < SCENE_LISTS; ++list)
            {
                mSceneFrameStats.earlyClusters += counts->early[list].instanceCount;
                mSceneFrameStats.lateClusters += counts->late[list].instanceCount;
            }
            mSceneFrameStats.retestEntries = counts->retest.entries;
            mSceneFrameStats.groupEntries = uint64_t(counts->groups[0].entries) + counts->groups[1].entries;
        }
        if (mFeedbackRead[slot].number != 0)
        {
            vmaInvalidateAllocation(mAllocator, mFeedbackReadback[slot].allocation, 0, FEEDBACK_BYTES);
            publishTextureFeedback(static_cast<const uint32_t*>(mFeedbackReadback[slot].mapped), mFeedbackRead[slot]);
            mFeedbackRead[slot] = {};
        }
        if (mAuditWritten[slot])
        {
            mAuditWritten[slot] = false;
            const uint64_t pixels = uint64_t(mSceneExtent.width) * mSceneExtent.height;
            vmaInvalidateAllocation(mAllocator, mAuditReadback[slot].allocation, 0, pixels * sizeof(uint32_t));
            const uint32_t* words = static_cast<const uint32_t*>(mAuditReadback[slot].mapped);
            mSceneFrameStats.auditErrorPixels += uint64_t(pixels - std::count(words, words + pixels, VISIBILITY_EMPTY));
            ++mSceneFrameStats.auditedFrames;
        }
    }

    bool ensureAuditTargets(uint32_t slot)
    {
        if (mAuditImage.image == VK_NULL_HANDLE &&
            !createTarget(mAuditImage, VISIBILITY_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 4, "audit target"))
        {
            return false;
        }
        const uint64_t bytes = uint64_t(mSceneExtent.width) * mSceneExtent.height * sizeof(uint32_t);
        return growBuffer(mAuditReadback[slot], bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryCategory::Staging, "audit readback", true);
    }

    // Runs the frame graph: uploads, two phases of culling and drawing, and the resolve.
    bool recordScene(VkCommandBuffer commands, uint32_t slot, const SceneCamera3D& camera, bool audit)
    {
        const auto readStart = std::chrono::steady_clock::now();
        readSceneResults(slot);
        mFrameParts.readResults = std::chrono::duration<double>(std::chrono::steady_clock::now() - readStart).count();
        const bool readFeedback = mFeedbackCollector.frame(std::chrono::steady_clock::now(), mFeedbackValue, mFeedbackEnding);
        mGraph.setSink(ResultAuditPixels, GraphAccess::CopyRead, audit);
        mGraph.setSink(ResultFeedbackReadback, GraphAccess::CopyRead, readFeedback);
        if (!mGraph.compile(mError) || (audit && !ensureAuditTargets(slot)))
        {
            return false;
        }
        mResultImages[ResultAuditVisibility] = audit ? &mAuditImage : nullptr;

        mRecordCommands = commands;
        mRecordSlot = slot;
        mRecordCamera = &camera;

        const uint32_t firstQuery = slot * (MAX_TIMED_PASSES + 1);
        const uint32_t passes = std::min(mGraph.passCount(), MAX_TIMED_PASSES);
        vkCmdResetQueryPool(commands, mSceneQueries, firstQuery, MAX_TIMED_PASSES + 1);
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mSceneQueries, firstQuery);
        for (uint32_t i = 0; i < mGraph.passCount(); ++i)
        {
            uint32_t count = 0;
            const GraphTransition* transitions = mGraph.transitions(i, count);
            applyTransitions(commands, transitions, count);
            if (!mGraph.run(i))
            {
                return false;
            }
            if (i < passes)
            {
                vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mSceneQueries, firstQuery + i + 1);
                mTimedNames[slot][i] = mGraph.passName(i);
            }
        }
        mTimedPasses[slot] = passes;
        uint32_t finalCount = 0;
        const GraphTransition* finalTransitions = mGraph.finalTransitions(finalCount);
        applyTransitions(commands, finalTransitions, finalCount);

        mPyramidCamera = camera;
        mHasPyramid = mOcclusion;

        mSceneFrameStats.instances = mSceneInstanceEnd;
        mSceneFrameStats.triangles = mSceneStore.triangles();
        mSceneFrameStats.pendingGeometries = mSceneStore.pendingGeometries();
        mSceneFrameStats.chunks = mSceneStore.chunkCount();
        publishSceneStats(mSceneFrameStats);
        return true;
    }

    void destroySwapchainViews()
    {
        for (VkImageView view : mViews)
        {
            vkDestroyImageView(mDevice, view, nullptr);
        }
        for (VkSemaphore semaphore : mRenderDone)
        {
            vkDestroySemaphore(mDevice, semaphore, nullptr);
        }
        mViews.clear();
        mRenderDone.clear();
        mImages.clear();
    }

    // Leaves mSwapchain null, and returns true, when the window has no size yet.
    bool createSwapchain(uint32_t width, uint32_t height)
    {
        if (!check(vkDeviceWaitIdle(mDevice), "wait for device"))
        {
            return false;
        }

        VkSurfaceCapabilitiesKHR caps;
        if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mGpu, mSurface, &caps), "read surface"))
        {
            return false;
        }
        VkExtent2D extent{ width, height };
        if (caps.currentExtent.width != UINT32_MAX)
        {
            extent = caps.currentExtent;
        }
        if (extent.width == 0 || extent.height == 0)
        {
            return true;
        }

        uint32_t count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(mGpu, mSurface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(mGpu, mSurface, &count, modes.data());
        const auto offered = [&](VkPresentModeKHR mode) { return std::find(modes.begin(), modes.end(), mode) != modes.end(); };
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; // always available, and the vsync mode
        if (!mOptions.vsync)
        {
            mode = offered(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                 : offered(VK_PRESENT_MODE_MAILBOX_KHR)   ? VK_PRESENT_MODE_MAILBOX_KHR
                                                          : mode;
        }

        uint32_t imageCount = std::max(caps.minImageCount + 1, 3u);
        if (caps.maxImageCount != 0)
        {
            imageCount = std::min(imageCount, caps.maxImageCount);
        }

        VkSwapchainKHR previous = mSwapchain;
        VkSwapchainCreateInfoKHR create{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        create.surface = mSurface;
        create.minImageCount = imageCount;
        create.imageFormat = mSurfaceFormat.format;
        create.imageColorSpace = mSurfaceFormat.colorSpace;
        create.imageExtent = extent;
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = caps.currentTransform;
        create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create.presentMode = mode;
        create.clipped = VK_TRUE;
        create.oldSwapchain = previous;
        if (!check(vkCreateSwapchainKHR(mDevice, &create, nullptr, &mSwapchain), "create swapchain"))
        {
            return false;
        }
        if (previous != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(mDevice, previous, nullptr);
        }
        destroySwapchainViews();
        mExtent = extent;
        mNeedSwapchain = false;

        vkGetSwapchainImagesKHR(mDevice, mSwapchain, &count, nullptr);
        mImages.resize(count);
        vkGetSwapchainImagesKHR(mDevice, mSwapchain, &count, mImages.data());
        mViews.resize(count, VK_NULL_HANDLE);
        mRenderDone.resize(count, VK_NULL_HANDLE);

        // The driver allocates these, so this is an estimate at 4 bytes per pixel.
        mMemoryBudget.release(MemoryCategory::RenderTargets, mSwapchainBytes);
        mSwapchainBytes = uint64_t(extent.width) * extent.height * 4 * count;
        if (!mMemoryBudget.reserve(MemoryCategory::RenderTargets, mSwapchainBytes))
        {
            mSwapchainBytes = 0;
            mError = "the swapchain does not fit in the render target budget";
            return false;
        }

        for (uint32_t i = 0; i < count; ++i)
        {
            VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view.image = mImages[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = mSurfaceFormat.format;
            view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkSemaphoreCreateInfo semaphore{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (!check(vkCreateImageView(mDevice, &view, nullptr, &mViews[i]), "create image view") ||
                !check(vkCreateSemaphore(mDevice, &semaphore, nullptr, &mRenderDone[i]), "create semaphore"))
            {
                return false;
            }
        }
        return true;
    }

    static void imageBarrier(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to,
                             VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess, VkPipelineStageFlags2 dstStage)
    {
        VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    bool recordCommands(uint32_t slot, uint32_t image, const FrameInput& input)
    {
        VkCommandBuffer commands = mCommands[slot];
        if (!check(vkResetCommandBuffer(commands, 0), "reset commands"))
        {
            return false;
        }
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!check(vkBeginCommandBuffer(commands, &begin), "begin commands"))
        {
            return false;
        }

        const auto retireStart = std::chrono::steady_clock::now();
        mRetiredTextures.run(input.ui ? input.ui->generation : 0, mFrame, FRAMES_IN_FLIGHT, [this](uint32_t retired)
        {
            destroyTexture(retired);
            mTextures.releaseSlot(retired);
            ++mFrameParts.texturesRetired;
        });
        const auto commandsStart = std::chrono::steady_clock::now();
        mFrameParts.retireTextures = std::chrono::duration<double>(commandsStart - retireStart).count();
        if (!runTextureCommands(commands, slot))
        {
            return false;
        }
        mFrameParts.textureCommands = std::chrono::duration<double>(std::chrono::steady_clock::now() - commandsStart).count();
        if (input.camera && !recordScene(commands, slot, *input.camera, input.audit))
        {
            return false;
        }

        imageBarrier(commands, mImages[image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        color.imageView = mViews[image];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = { { 0.f, 0.f, 0.f, 1.f } }; // what the viewer clears to, and transparent UI shows it
        VkRenderingInfo rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        rendering.renderArea = { { 0, 0 }, mExtent };
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(commands, &rendering);

        const VkViewport viewport{ 0.f, 0.f, float(mExtent.width), float(mExtent.height), 0.f, 1.f };
        const VkRect2D scissor{ { 0, 0 }, mExtent };
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        if (input.camera)
        {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, mCompositePipeline);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, mScenePipelineLayout, 1, 1, &mSceneSet, 0, nullptr);
            vkCmdDraw(commands, 3, 1, 0, 0);
        }
        if (input.testTriangle)
        {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline);
            vkCmdPushConstants(commands, mPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(input.seconds), &input.seconds);
            vkCmdDraw(commands, 3, 1, 0, 0);
        }
        if (input.ui && !input.ui->vertices().empty())
        {
            drawUI(commands, slot, *input.ui);
        }
        vkCmdEndRendering(commands);

        imageBarrier(commands, mImages[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_PIPELINE_STAGE_2_NONE);
        return check(vkEndCommandBuffer(commands), "end commands");
    }

    void shutdown()
    {
        if (mDevice != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(mDevice);
            mShuttingDown = true;
            destroySwapchainViews();
            destroyScene();
            for (uint32_t slot = 0; slot < mTextureObjects.size(); ++slot)
            {
                destroyTexture(slot);
            }
            for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
            {
                destroyBuffer(mUIBuffers[i], MemoryCategory::UI);
            }
            mTextures.setUploadRing(nullptr, 0, 0); // what comes later waits, and is never written
            destroyBuffer(mUploadRing, MemoryCategory::Staging);
            for (const auto& pipeline : mUIPipelines)
            {
                vkDestroyPipeline(mDevice, pipeline.second, nullptr);
            }
            if (mUIVertexShader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(mDevice, mUIVertexShader, nullptr);
            }
            if (mUIFragmentShader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(mDevice, mUIFragmentShader, nullptr);
            }
            if (mUIPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice, mUIPipelineLayout, nullptr);
            }
            if (mUIPool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(mDevice, mUIPool, nullptr);
            }
            if (mUISetLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(mDevice, mUISetLayout, nullptr);
            }
            for (VkSampler sampler : mSamplers)
            {
                if (sampler != VK_NULL_HANDLE)
                {
                    vkDestroySampler(mDevice, sampler, nullptr);
                }
            }
            if (mAllocator != VK_NULL_HANDLE)
            {
                vmaDestroyAllocator(mAllocator);
            }
            if (mSwapchain != VK_NULL_HANDLE)
            {
                vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
            }
            if (mPipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(mDevice, mPipeline, nullptr);
            }
            if (mPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice, mPipelineLayout, nullptr);
            }
            for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
            {
                if (mFences[i] != VK_NULL_HANDLE)
                {
                    vkDestroyFence(mDevice, mFences[i], nullptr);
                }
                if (mImageReady[i] != VK_NULL_HANDLE)
                {
                    vkDestroySemaphore(mDevice, mImageReady[i], nullptr);
                }
            }
            if (mCommandPool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
            }
            vkDestroyDevice(mDevice, nullptr);
        }
        if (mInstance != VK_NULL_HANDLE)
        {
            if (mSurface != VK_NULL_HANDLE)
            {
                vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
            }
            if (mMessenger != VK_NULL_HANDLE)
            {
                vkDestroyDebugUtilsMessengerEXT(mInstance, mMessenger, nullptr);
            }
            vkDestroyInstance(mInstance, nullptr);
        }
    }

    DeviceOptions mOptions;
    DeviceInfo mInfo;
    std::string mError;
    std::atomic<uint32_t> mValidationErrors{ 0 };
    HWND mWindow = nullptr;

    VkInstance mInstance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    VkPhysicalDevice mGpu = VK_NULL_HANDLE;
    uint32_t mQueueFamily = 0;
    uint32_t mMemoryHeap = 0;
    bool mHasMemoryBudgetExtension = false;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mQueue = VK_NULL_HANDLE;

    VkSurfaceFormatKHR mSurfaceFormat{};
    VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
    VkExtent2D mExtent{};
    uint32_t mRequestedWidth = 0;
    uint32_t mRequestedHeight = 0;
    uint64_t mSwapchainBytes = 0;
    bool mNeedSwapchain = false;
    std::vector<VkImage> mImages;
    std::vector<VkImageView> mViews;
    std::vector<VkSemaphore> mRenderDone; // per image: present can still hold the previous one

    VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
    VkPipeline mPipeline = VK_NULL_HANDLE;
    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    VkCommandBuffer mCommands[FRAMES_IN_FLIGHT]{};
    VkFence mFences[FRAMES_IN_FLIGHT]{};
    VkSemaphore mImageReady[FRAMES_IN_FLIGHT]{};
    uint64_t mFrame = 0;

    VmaAllocator mAllocator = VK_NULL_HANDLE;
    VkSampler mSamplers[uint32_t(UISampler::Count)]{};
    VkDescriptorSetLayout mUISetLayout = VK_NULL_HANDLE;
    VkDescriptorPool mUIPool = VK_NULL_HANDLE;
    VkDescriptorSet mUISet = VK_NULL_HANDLE;
    VkPipelineLayout mUIPipelineLayout = VK_NULL_HANDLE;
    VkShaderModule mUIVertexShader = VK_NULL_HANDLE;
    VkShaderModule mUIFragmentShader = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkPipeline> mUIPipelines;  // every one made so far
    std::vector<VkPipeline> mUIDrawPipelines;               // the current list's, by blend and primitive
    uint64_t mUIPipelineGeneration = 0;
    MappedBuffer mUIBuffers[FRAMES_IN_FLIGHT];
    uint64_t mUIBufferGeneration[FRAMES_IN_FLIGHT]{};
    MappedBuffer mUploadRing;                               // texture pixels, written by the main thread
    uint64_t mUploadPieceBytes = 0;                         // the most one frame copies from it
    uint64_t mRingEnd[FRAMES_IN_FLIGHT]{};                  // how far each slot's frame read it, 0 for not at all
    uint64_t mUploadBytesPerFrame = 0;
    std::vector<Texture> mTextureObjects;                   // by slot
    std::vector<TextureCommand> mTextureCommands;           // taken, not yet run
    size_t mTextureCommandsDone = 0;                        // the front of mTextureCommands already run
    std::chrono::steady_clock::time_point mPreviousFrameStart{};
    double mFrameSeconds = 0.0;                             // from the previous frame's start to this one's
    RetiredTextures mRetiredTextures;

    struct RetiredBuffer
    {
        GpuBuffer buffer;
        uint64_t frame; // destroyed from this frame on
    };

    SceneStore mSceneStore{ FRAMES_IN_FLIGHT };
    std::vector<GpuBuffer> mSceneChunks;
    GpuBuffer mSceneTables[SCENE_TABLES];
    GpuBuffer mSceneVisible;
    GpuBuffer mSceneRetest;
    GpuBuffer mSceneGroups;
    GpuBuffer mSceneCounts;
    GpuBuffer mScenePyramid;
    MappedBuffer mSceneCountsTemplate;
    MappedBuffer mSceneViews[FRAMES_IN_FLIGHT];
    MappedBuffer mSceneStaging[FRAMES_IN_FLIGHT];
    MappedBuffer mSceneReadback[FRAMES_IN_FLIGHT];
    MappedBuffer mAuditReadback[FRAMES_IN_FLIGHT];
    bool mAuditWritten[FRAMES_IN_FLIGHT]{};
    GpuBuffer mTextureFeedback;
    MappedBuffer mFeedbackReadback[FRAMES_IN_FLIGHT];
    TextureFeedback::Window mFeedbackRead[FRAMES_IN_FLIGHT]; // the window each slot's readback holds; 0 is none
    TextureFeedback::Window mFeedbackEnding;                 // the window this frame ends, when it ends one
    uint32_t mFeedbackValue = 0;                             // GpuSceneView::feedback for this frame
    std::deque<RetiredBuffer> mRetiredBuffers;
    std::vector<VkBufferCopy> mSceneRegions;
    VkDescriptorSetLayout mSceneSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool mScenePool = VK_NULL_HANDLE;
    VkDescriptorSet mSceneSet = VK_NULL_HANDLE;
    VkPipelineLayout mScenePipelineLayout = VK_NULL_HANDLE;
    VkPipeline mCullPipeline = VK_NULL_HANDLE;
    VkPipeline mPyramidPipeline = VK_NULL_HANDLE;
    VkPipeline mVisibilityPipeline = VK_NULL_HANDLE;
    VkPipeline mMaskedPipeline = VK_NULL_HANDLE;
    VkPipeline mResolvePipeline = VK_NULL_HANDLE;
    VkPipeline mCompositePipeline = VK_NULL_HANDLE;
    TargetImage mVisibilityImage;
    TargetImage mDepthImage;
    TargetImage mAlbedoImage;
    TargetImage mNormalImage;
    TargetImage mAuditImage;
    VkExtent2D mSceneExtent{};
    VkQueryPool mSceneQueries = VK_NULL_HANDLE;
    float mTimestampMilliseconds = 0.f;
    SceneStats mSceneFrameStats;

    bool mShuttingDown = false;
    FrameGraph mGraph;
    const TargetImage* mResultImages[ResultCount]{}; // null for results that are buffers
    uint32_t mTimedPasses[FRAMES_IN_FLIGHT]{};
    const char* mTimedNames[FRAMES_IN_FLIGHT][MAX_TIMED_PASSES]{};

    FrameParts mFrameParts;

    // What pass bodies work with, during one recordScene().
    VkCommandBuffer mRecordCommands = VK_NULL_HANDLE;
    uint32_t mRecordSlot = 0;
    const SceneCamera3D* mRecordCamera = nullptr;
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
    if (volkInitialize() != VK_SUCCESS)
    {
        result.reason = "Vulkan is not installed";
        return result;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3)
    {
        result.reason = "Vulkan 1.3 is not available";
        return result;
    }

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "AyaneStorm";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* const extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo create{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    create.pApplicationInfo = &app;
    create.enabledExtensionCount = 2;
    create.ppEnabledExtensionNames = extensions;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = vkCreateInstance(&create, nullptr, &instance);
    if (created != VK_SUCCESS)
    {
        result.reason = "vkCreateInstance failed: VkResult " + std::to_string(int(created));
        return result;
    }
    volkLoadInstance(instance);

    // There is no window yet, so ask whether the queue can present to any window.
    const GpuChoice choice = chooseGpu(instance, [](VkPhysicalDevice gpu, uint32_t family)
    {
        return vkGetPhysicalDeviceWin32PresentationSupportKHR(gpu, family) == VK_TRUE;
    });
    vkDestroyInstance(instance, nullptr);

    if (choice.gpu == VK_NULL_HANDLE)
    {
        result.reason = "no Vulkan 1.3 GPU with the required features";
        return result;
    }
    result.supported = true;
    result.info = choice.info;
    return result;
}

std::unique_ptr<Device> Device::create(const SurfaceTarget& target, const DeviceOptions& options, std::string& error)
{
    auto device = std::make_unique<DeviceVulkan>();
    if (!device->init(target, options, error))
    {
        return nullptr;
    }
    return device;
}

} // namespace asr
