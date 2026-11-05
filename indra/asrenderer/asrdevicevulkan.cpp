/**
 * asrdevicevulkan.cpp
 * Vulkan 1.3 backend: dynamic rendering, synchronization2 and no OpenGL. Vulkan
 * itself is loaded at runtime through volk, so a machine without it reaches
 * create() and gets an error, not a failure to start. Draws the scene through
 * compute culling and a visibility buffer, the UI through one bindless pipeline per
 * blend, and the test triangle. See asrdevice.h.
 */
#include "asrdevice.h"
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

constexpr uint32_t CULL_GROUP_SIZE = 64;
constexpr uint32_t RESOLVE_GROUP_SIZE = 8;

// Scene timing points in one frame: start, culled, drawn, resolved.
constexpr uint32_t SCENE_TIMESTAMPS = 4;

constexpr VkShaderStageFlags SCENE_PUSH_STAGES = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

// Mirrors the push constants of asrSceneCommon.glsl.
struct ScenePush
{
    uint64_t view;
    uint64_t instances;
    uint64_t geometries;
    uint64_t clusters;
    uint64_t materials;
    uint64_t visible;
    uint64_t counts;
    uint32_t bucket;
};

// One bucket's draw: a VkDrawIndirectCommand that culling counts into.
constexpr uint64_t SCENE_COUNTS_BYTES = sizeof(VkDrawIndirectCommand) * CLUSTER_BUCKETS;

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

    return v13.dynamicRendering && v13.synchronization2 && v12.timelineSemaphore &&
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

        const uint32_t slot = mFrame % FRAMES_IN_FLIGHT;
        if (!check(vkWaitForFences(mDevice, 1, &mFences[slot], VK_TRUE, UINT64_MAX), "wait for frame"))
        {
            return false;
        }

        uint32_t image = 0;
        const VkResult acquired = vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX, mImageReady[slot], VK_NULL_HANDLE, &image);
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
        if (!check(vkResetFences(mDevice, 1, &mFences[slot]), "reset fence") ||
            !prepareUI(slot, input.ui) || !recordCommands(slot, image, input))
        {
            return false;
        }

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

        ++mFrame;
        return true;
    }

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

    bool createBufferObject(GpuBuffer& buffer, uint64_t bytes, VkBufferUsageFlags usage, const char* what)
    {
        if (!mMemoryBudget.reserve(MemoryCategory::Geometry, bytes))
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
            mMemoryBudget.release(MemoryCategory::Geometry, bytes);
            return false;
        }
        VkBufferDeviceAddressInfo address{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        address.buffer = buffer.buffer;
        buffer.address = vkGetBufferDeviceAddress(mDevice, &address);
        buffer.size = bytes;
        return true;
    }

    void destroyBufferObject(GpuBuffer& buffer)
    {
        if (buffer.buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mAllocator, buffer.buffer, buffer.allocation);
            mMemoryBudget.release(MemoryCategory::Geometry, buffer.size);
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
            const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamic.dynamicStateCount = 2;
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
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout.bindingCount = 3;
        layout.pBindings = bindings;

        const VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;

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
        mResolvePipeline = mCullPipeline ? createComputePipeline("asrSceneResolveC", error) : VK_NULL_HANDLE;
        mVisibilityPipeline = mResolvePipeline ? createSceneRasterPipeline("asrSceneVisibilityV", "asrSceneVisibilityF", VISIBILITY_FORMAT, true, error) : VK_NULL_HANDLE;
        mCompositePipeline = mVisibilityPipeline ? createSceneRasterPipeline("asrSceneCompositeV", "asrSceneCompositeF", mSurfaceFormat.format, false, error) : VK_NULL_HANDLE;
        if (!mCompositePipeline)
        {
            return false;
        }

        VkQueryPoolCreateInfo queries{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queries.queryCount = SCENE_TIMESTAMPS * FRAMES_IN_FLIGHT;
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
                !growBuffer(mSceneReadback[i], SCENE_COUNTS_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryCategory::Staging, "scene readback"))
            {
                error = mError;
                return false;
            }
        }
        if (!growBuffer(mSceneCountsTemplate, SCENE_COUNTS_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryCategory::Staging, "scene draw template") ||
            !createBufferObject(mSceneCounts, SCENE_COUNTS_BYTES,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                "scene draw counts"))
        {
            error = mError;
            return false;
        }
        VkDrawIndirectCommand draws[CLUSTER_BUCKETS]{};
        for (uint32_t bucket = 0; bucket < CLUSTER_BUCKETS; ++bucket)
        {
            draws[bucket].vertexCount = CLUSTER_BUCKET_TRIANGLES[bucket] * 3;
        }
        memcpy(mSceneCountsTemplate.mapped, draws, sizeof(draws));
        vmaFlushAllocation(mAllocator, mSceneCountsTemplate.allocation, 0, sizeof(draws));
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
        mSceneExtent = mExtent;

        if (!createTarget(mVisibilityImage, VISIBILITY_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 4, "visibility buffer") ||
            !createTarget(mDepthImage, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 4, "depth buffer") ||
            !createTarget(mAlbedoImage, ALBEDO_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 4, "albedo target") ||
            !createTarget(mNormalImage, NORMAL_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 8, "normal target"))
        {
            return false;
        }

        const VkImageView views[3] = { mVisibilityImage.view, mAlbedoImage.view, mNormalImage.view };
        VkDescriptorImageInfo images[3]{};
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            images[i].imageView = views[i];
            images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = mSceneSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(mDevice, 3, writes, 0, nullptr);
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
        destroyBufferObject(mSceneCounts);
        destroyBuffer(mSceneCountsTemplate, MemoryCategory::Staging);
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
        {
            destroyBuffer(mSceneViews[i], MemoryCategory::Staging);
            destroyBuffer(mSceneStaging[i], MemoryCategory::Staging);
            destroyBuffer(mSceneReadback[i], MemoryCategory::Staging);
        }
        destroyTarget(mVisibilityImage);
        destroyTarget(mDepthImage);
        destroyTarget(mAlbedoImage);
        destroyTarget(mNormalImage);
        for (VkPipeline pipeline : { mCullPipeline, mVisibilityPipeline, mResolvePipeline, mCompositePipeline })
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
    // the old buffer can go at once.
    bool growBuffer(MappedBuffer& buffer, uint64_t bytes, VkBufferUsageFlags usage, MemoryCategory category, const char* what)
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
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
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

    // A texture that does not fit in the budget shows as white and is not an error:
    // the budget's pressure levels are what keep this from happening.
    bool createTexture(VkCommandBuffer commands, const TextureCommand& command)
    {
        if (mTextureObjects.size() <= command.slot)
        {
            mTextureObjects.resize(command.slot + 1);
        }
        Texture& texture = mTextureObjects[command.slot];
        const uint64_t bytes = textureBytes(command.desc);
        if (!mMemoryBudget.reserve(MemoryCategory::Textures, bytes))
        {
            writeTextureDescriptor(command.slot, mTextureObjects[0].view);
            return true;
        }

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
        if (!check(vmaCreateImage(mAllocator, &image, &allocation, &texture.image, &texture.allocation, nullptr), "create texture"))
        {
            mMemoryBudget.release(MemoryCategory::Textures, bytes);
            return false;
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

    // Sends the given rows of an upload, which may be a part of it.
    void uploadTexture(VkCommandBuffer commands, const TextureCommand& command, uint32_t rows, uint32_t frameSlot, uint64_t& stagingOffset)
    {
        const Texture& texture = mTextureObjects[command.slot];
        if (texture.image == VK_NULL_HANDLE)
        {
            return; // did not fit in the budget
        }
        const uint64_t rowBytes = command.pixels.size() / command.height;
        const uint64_t bytes = rowBytes * rows;
        memcpy(static_cast<char*>(mStaging[frameSlot].mapped) + stagingOffset, command.pixels.data() + rowBytes * command.rowsDone, bytes);

        textureBarrier(commands, texture.image, texture.mipLevels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       TEXTURE_READ_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = stagingOffset;
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, command.level, 0, 1 };
        copy.imageOffset = { int32_t(command.x), int32_t(command.y + command.rowsDone), 0 };
        copy.imageExtent = { command.width, rows, 1 };
        vkCmdCopyBufferToImage(commands, mStaging[frameSlot].buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        textureBarrier(commands, texture.image, texture.mipLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       TEXTURE_READ_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        stagingOffset += (bytes + UPLOAD_ALIGNMENT - 1) / UPLOAD_ALIGNMENT * UPLOAD_ALIGNMENT;
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
    }

    // Runs what the main thread queued, in order, up to this frame's upload limit.
    bool runTextureCommands(VkCommandBuffer commands, uint32_t frameSlot)
    {
        mTextures.take(mTextureCommands);

        // Find how far this frame goes first, because the staging buffer cannot grow
        // once something in it has been recorded.
        uint64_t stagingBytes = 0;
        size_t end = 0;
        uint32_t lastRows = 0; // rows of the command at end, when only part of it fits
        for (; end < mTextureCommands.size(); ++end)
        {
            const TextureCommand& command = mTextureCommands[end];
            if (command.type == TextureCommand::Type::Upload)
            {
                // Whole rows that fit, leaving room for rounding up to the alignment.
                const uint64_t rowBytes = command.pixels.size() / command.height;
                const uint64_t room = mUploadBytesPerFrame - std::min(mUploadBytesPerFrame, stagingBytes + UPLOAD_ALIGNMENT);
                const uint32_t remaining = command.height - command.rowsDone;
                const uint32_t rows = uint32_t(std::min<uint64_t>(remaining, room / rowBytes));
                if (rows < remaining)
                {
                    lastRows = rows;
                    stagingBytes += (rows * rowBytes + UPLOAD_ALIGNMENT - 1) / UPLOAD_ALIGNMENT * UPLOAD_ALIGNMENT;
                    break;
                }
                stagingBytes += (rows * rowBytes + UPLOAD_ALIGNMENT - 1) / UPLOAD_ALIGNMENT * UPLOAD_ALIGNMENT;
            }
        }
        if (stagingBytes != 0 &&
            !growBuffer(mStaging[frameSlot], mUploadBytesPerFrame, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryCategory::Staging, "texture staging buffer"))
        {
            return false;
        }

        uint64_t stagingOffset = 0;
        for (size_t i = 0; i < end; ++i)
        {
            TextureCommand& command = mTextureCommands[i];
            switch (command.type)
            {
                case TextureCommand::Type::Create:
                    if (!createTexture(commands, command))
                    {
                        return false;
                    }
                    break;
                case TextureCommand::Type::Upload:
                    uploadTexture(commands, command, command.height - command.rowsDone, frameSlot, stagingOffset);
                    break;
                case TextureCommand::Type::GenerateMips:
                    generateMips(commands, command.slot);
                    break;
                case TextureCommand::Type::Destroy:
                    mRetiredTextures.add(command.slot, command.lastList);
                    break;
            }
        }
        if (lastRows != 0)
        {
            uploadTexture(commands, mTextureCommands[end], lastRows, frameSlot, stagingOffset);
            mTextureCommands[end].rowsDone += lastRows;
        }
        if (stagingOffset != 0)
        {
            vmaFlushAllocation(mAllocator, mStaging[frameSlot].allocation, 0, stagingOffset);
        }
        mTextureCommands.erase(mTextureCommands.begin(), mTextureCommands.begin() + end);
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

    static void targetBarrier(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                              VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
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
        barrier.subresourceRange = { aspect, 0, 1, 0, 1 };
        VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);
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

    // Applies this frame's scene changes on the GPU.
    bool updateScene(VkCommandBuffer commands, uint32_t slot)
    {
        while (!mRetiredBuffers.empty() && mRetiredBuffers.front().frame <= mFrame)
        {
            destroyBufferObject(mRetiredBuffers.front().buffer);
            mRetiredBuffers.pop_front();
        }

        mSceneStore.update(mScene, *this, mFrame, mUploadBytesPerFrame);

        // Earlier frames' reads, and their counts, finish before anything here writes.
        memoryBarrier(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);

        for (uint32_t i = 0; i < SCENE_TABLES; ++i)
        {
            const uint64_t bytes = mSceneStore.tableCapacity(SceneTable(i));
            if (bytes > mSceneTables[i].size && !growTable(commands, mSceneTables[i], bytes))
            {
                return false;
            }
        }

        uint64_t visibleEntries = 0;
        for (uint32_t bucket = 0; bucket < CLUSTER_BUCKETS; ++bucket)
        {
            visibleEntries += mSceneStore.bucketCapacity(bucket);
        }
        const uint64_t visibleBytes = visibleEntries * sizeof(uint32_t) * 2;
        if (visibleBytes > mSceneVisible.size)
        {
            if (mSceneVisible.buffer != VK_NULL_HANDLE)
            {
                mRetiredBuffers.push_back({ mSceneVisible, mFrame + FRAMES_IN_FLIGHT + 1 });
                mSceneVisible = GpuBuffer();
            }
            if (!createBufferObject(mSceneVisible, visibleBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "visible cluster list"))
            {
                return false;
            }
        }

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

        const VkBufferCopy reset{ 0, 0, SCENE_COUNTS_BYTES };
        vkCmdCopyBuffer(commands, mSceneCountsTemplate.buffer, mSceneCounts.buffer, 1, &reset);
        memoryBarrier(commands, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        return true;
    }

    void readSceneResults(uint32_t slot)
    {
        if (!mSceneQueriesWritten[slot])
        {
            return;
        }
        uint64_t times[SCENE_TIMESTAMPS];
        if (vkGetQueryPoolResults(mDevice, mSceneQueries, slot * SCENE_TIMESTAMPS, SCENE_TIMESTAMPS, sizeof(times), times,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            mSceneFrameStats.cullMilliseconds = double(times[1] - times[0]) * mTimestampMilliseconds;
            mSceneFrameStats.visibilityMilliseconds = double(times[2] - times[1]) * mTimestampMilliseconds;
            mSceneFrameStats.resolveMilliseconds = double(times[3] - times[2]) * mTimestampMilliseconds;
        }
        vmaInvalidateAllocation(mAllocator, mSceneReadback[slot].allocation, 0, SCENE_COUNTS_BYTES);
        VkDrawIndirectCommand draws[CLUSTER_BUCKETS];
        memcpy(draws, mSceneReadback[slot].mapped, sizeof(draws));
        mSceneFrameStats.visibleClusters = 0;
        for (const VkDrawIndirectCommand& draw : draws)
        {
            mSceneFrameStats.visibleClusters += draw.instanceCount;
        }
    }

    // Culls, draws the visibility buffer, and resolves it to the G-buffer.
    bool recordScene(VkCommandBuffer commands, uint32_t slot, const SceneCamera3D& camera)
    {
        readSceneResults(slot);
        const uint32_t firstQuery = slot * SCENE_TIMESTAMPS;
        vkCmdResetQueryPool(commands, mSceneQueries, firstQuery, SCENE_TIMESTAMPS);
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, mSceneQueries, firstQuery);

        if (!updateScene(commands, slot))
        {
            return false;
        }

        GpuSceneView view{};
        fillSceneView(view, camera, mSceneExtent.width, mSceneExtent.height);
        uint32_t base = 0;
        for (uint32_t bucket = 0; bucket < CLUSTER_BUCKETS; ++bucket)
        {
            view.bucketBase[bucket] = base;
            base += mSceneStore.bucketCapacity(bucket);
        }
        view.instanceEnd = mSceneStore.instanceEnd();
        memcpy(mSceneViews[slot].mapped, &view, sizeof(view));
        vmaFlushAllocation(mAllocator, mSceneViews[slot].allocation, 0, sizeof(view));

        VkBufferDeviceAddressInfo address{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        address.buffer = mSceneViews[slot].buffer;

        ScenePush push{};
        push.view = vkGetBufferDeviceAddress(mDevice, &address);
        push.instances = mSceneTables[uint32_t(SceneTable::Instances)].address;
        push.geometries = mSceneTables[uint32_t(SceneTable::Geometries)].address;
        push.clusters = mSceneTables[uint32_t(SceneTable::Clusters)].address;
        push.materials = mSceneTables[uint32_t(SceneTable::Materials)].address;
        push.visible = mSceneVisible.address;
        push.counts = mSceneCounts.address;

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mCullPipeline);
        vkCmdPushConstants(commands, mScenePipelineLayout, SCENE_PUSH_STAGES, 0, sizeof(push), &push);
        vkCmdDispatch(commands, (view.instanceEnd + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE, 1, 1);
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mSceneQueries, firstQuery + 1);

        memoryBarrier(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy readback{ 0, 0, SCENE_COUNTS_BYTES };
        vkCmdCopyBuffer(commands, mSceneCounts.buffer, mSceneReadback[slot].buffer, 1, &readback);

        targetBarrier(commands, mVisibilityImage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        targetBarrier(commands, mDepthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        color.imageView = mVisibilityImage.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color.uint32[0] = VISIBILITY_EMPTY;
        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView = mDepthImage.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
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
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, mVisibilityPipeline);
        for (uint32_t bucket = 0; bucket < CLUSTER_BUCKETS; ++bucket)
        {
            push.bucket = bucket;
            vkCmdPushConstants(commands, mScenePipelineLayout, SCENE_PUSH_STAGES, 0, sizeof(push), &push);
            vkCmdDrawIndirect(commands, mSceneCounts.buffer, bucket * sizeof(VkDrawIndirectCommand), 1, sizeof(VkDrawIndirectCommand));
        }
        vkCmdEndRendering(commands);

        targetBarrier(commands, mVisibilityImage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        for (VkImage image : { mAlbedoImage.image, mNormalImage.image })
        {
            targetBarrier(commands, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mSceneQueries, firstQuery + 2);

        const VkDescriptorSet sets[2] = { mUISet, mSceneSet };
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mResolvePipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mScenePipelineLayout, 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(commands, mScenePipelineLayout, SCENE_PUSH_STAGES, 0, sizeof(push), &push);
        vkCmdDispatch(commands, (mSceneExtent.width + RESOLVE_GROUP_SIZE - 1) / RESOLVE_GROUP_SIZE,
                      (mSceneExtent.height + RESOLVE_GROUP_SIZE - 1) / RESOLVE_GROUP_SIZE, 1);

        // After the barrier: a timestamp alone does not make the GPU finish a dispatch.
        memoryBarrier(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mSceneQueries, firstQuery + 3);
        mSceneQueriesWritten[slot] = true;

        mSceneFrameStats.instances = view.instanceEnd;
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

        mRetiredTextures.run(input.ui ? input.ui->generation : 0, mFrame, FRAMES_IN_FLIGHT, [this](uint32_t retired)
        {
            destroyTexture(retired);
            mTextures.releaseSlot(retired);
        });
        if (!runTextureCommands(commands, slot))
        {
            return false;
        }
        if (input.camera && !recordScene(commands, slot, *input.camera))
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
            destroySwapchainViews();
            destroyScene();
            for (uint32_t slot = 0; slot < mTextureObjects.size(); ++slot)
            {
                destroyTexture(slot);
            }
            for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
            {
                destroyBuffer(mUIBuffers[i], MemoryCategory::UI);
                destroyBuffer(mStaging[i], MemoryCategory::Staging);
            }
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
    MappedBuffer mStaging[FRAMES_IN_FLIGHT];
    uint64_t mUploadBytesPerFrame = 0;
    std::vector<Texture> mTextureObjects;                   // by slot
    std::vector<TextureCommand> mTextureCommands;           // taken, not yet run
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
    GpuBuffer mSceneCounts;
    MappedBuffer mSceneCountsTemplate;
    MappedBuffer mSceneViews[FRAMES_IN_FLIGHT];
    MappedBuffer mSceneStaging[FRAMES_IN_FLIGHT];
    MappedBuffer mSceneReadback[FRAMES_IN_FLIGHT];
    std::deque<RetiredBuffer> mRetiredBuffers;
    std::vector<VkBufferCopy> mSceneRegions;
    VkDescriptorSetLayout mSceneSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool mScenePool = VK_NULL_HANDLE;
    VkDescriptorSet mSceneSet = VK_NULL_HANDLE;
    VkPipelineLayout mScenePipelineLayout = VK_NULL_HANDLE;
    VkPipeline mCullPipeline = VK_NULL_HANDLE;
    VkPipeline mVisibilityPipeline = VK_NULL_HANDLE;
    VkPipeline mResolvePipeline = VK_NULL_HANDLE;
    VkPipeline mCompositePipeline = VK_NULL_HANDLE;
    TargetImage mVisibilityImage;
    TargetImage mDepthImage;
    TargetImage mAlbedoImage;
    TargetImage mNormalImage;
    VkExtent2D mSceneExtent{};
    VkQueryPool mSceneQueries = VK_NULL_HANDLE;
    bool mSceneQueriesWritten[FRAMES_IN_FLIGHT]{};
    float mTimestampMilliseconds = 0.f;
    SceneStats mSceneFrameStats;
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
