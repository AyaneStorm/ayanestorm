/**
 * asrdevicevulkan.cpp
 * Vulkan 1.3 backend: dynamic rendering, synchronization2 and no OpenGL. Vulkan
 * itself is loaded at runtime through volk, so a machine without it reaches
 * create() and gets an error, not a failure to start. Draws the test triangle.
 * See asrdevice.h.
 */
#include "asrdevice.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "volk.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace asr
{

namespace
{

constexpr uint32_t FRAMES_IN_FLIGHT = 2;

// The features section 2 of the plan requires.
bool hasRequiredFeatures(VkPhysicalDevice gpu)
{
    VkPhysicalDeviceVulkan13Features v13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13 };
    VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
    vkGetPhysicalDeviceFeatures2(gpu, &features);
    return v13.dynamicRendering && v13.synchronization2 && v12.timelineSemaphore &&
           v12.descriptorIndexing && v12.bufferDeviceAddress;
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

class DeviceVulkan final : public Device
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
               createLogicalDevice(error) && createFrames(error) && createPipeline(error);
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

    bool renderTestFrame(uint32_t width, uint32_t height, float seconds) override
    {
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
            !recordCommands(mCommands[slot], image, seconds))
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

        VkDeviceCreateInfo create{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v12 };
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

    bool recordCommands(VkCommandBuffer commands, uint32_t image, float seconds)
    {
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

        imageBarrier(commands, mImages[image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        color.imageView = mViews[image];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = { { 0.02f, 0.02f, 0.05f, 1.f } };
        VkRenderingInfo rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        rendering.renderArea = { { 0, 0 }, mExtent };
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(commands, &rendering);

        const VkViewport viewport{ 0.f, 0.f, float(mExtent.width), float(mExtent.height), 0.f, 1.f };
        const VkRect2D scissor{ { 0, 0 }, mExtent };
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline);
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        vkCmdPushConstants(commands, mPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(seconds), &seconds);
        vkCmdDraw(commands, 3, 1, 0, 0);
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
