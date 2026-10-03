#include "VulkanRenderer.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

static void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(what);
}

VulkanRenderer::~VulkanRenderer() { shutdown(); }

bool VulkanRenderer::gpuLuid(LUID& luid) const {
    if (!physical_) return false;
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &id;
    vkGetPhysicalDeviceProperties2(physical_, &props);
    if (!id.deviceLUIDValid) return false;
    std::memcpy(&luid, id.deviceLUID, sizeof(LUID));
    return true;
}

void VulkanRenderer::initialize(HWND hwnd) {
    hwnd_ = hwnd;
    createInstance();

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = hwnd_;
    vkCheck(vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_), "Could not create Vulkan Win32 surface.");

    pickPhysicalDevice();
    createDevice();
    createCommandResources();
    createSwapchain();
}

void VulkanRenderer::shutdown() {
    if (!instance_) return;
    if (device_) vkDeviceWaitIdle(device_);
    destroyTexture();
    destroySwapchain();
    if (device_) {
        if (imageAvailable_) vkDestroySemaphore(device_, imageAvailable_, nullptr);
        if (renderFinished_) vkDestroySemaphore(device_, renderFinished_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    vkDestroyInstance(instance_, nullptr);
    hwnd_ = nullptr;
    instance_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    queueFamily_ = 0;
    swapchain_ = VK_NULL_HANDLE;
    swapFormat_ = VK_FORMAT_UNDEFINED;
    extent_ = {};
    commandPool_ = VK_NULL_HANDLE;
    imageAvailable_ = VK_NULL_HANDLE;
    renderFinished_ = VK_NULL_HANDLE;
    texture_ = VK_NULL_HANDLE;
    textureMemory_ = VK_NULL_HANDLE;
    textureW_ = textureH_ = 0;
    resizePending_ = false;
    gpuName_[0] = '\0';
}

void VulkanRenderer::createInstance() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "Vulkan Image Mask Studio";
    ai.applicationVersion = VK_MAKE_VERSION(0,1,0);
    ai.pEngineName = "Vulkan direct blit viewer";
    ai.engineVersion = VK_MAKE_VERSION(0,1,0);
    ai.apiVersion = VK_API_VERSION_1_1;

    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    vkCheck(vkCreateInstance(&ci, nullptr, &instance_), "Could not create Vulkan instance. Is the Vulkan runtime installed?");
}

void VulkanRenderer::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (!count) throw std::runtime_error("No Vulkan-capable GPU was found.");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int bestScore = -1;
    for (auto d : devices) {
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qcount, qprops.data());
        for (uint32_t q = 0; q < qcount; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, q, surface_, &present);
            if ((qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                VkPhysicalDeviceProperties p{};
                vkGetPhysicalDeviceProperties(d, &p);
                int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 100 : 10;
                if (score > bestScore) {
                    bestScore = score;
                    physical_ = d;
                    queueFamily_ = q;
                    std::strncpy(gpuName_, p.deviceName, sizeof(gpuName_) - 1);
                }
            }
        }
    }
    if (!physical_) throw std::runtime_error("No Vulkan queue supports both graphics and Win32 presentation.");
}

void VulkanRenderer::createDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    vkCheck(vkCreateDevice(physical_, &dci, nullptr, &device_), "Could not create Vulkan device.");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
}

void VulkanRenderer::createCommandResources() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = queueFamily_;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(device_, &pci, nullptr, &commandPool_), "Could not create Vulkan command pool.");

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCheck(vkCreateSemaphore(device_, &sci, nullptr, &imageAvailable_), "Could not create Vulkan semaphore.");
    vkCheck(vkCreateSemaphore(device_, &sci, nullptr, &renderFinished_), "Could not create Vulkan semaphore.");
}

void VulkanRenderer::createSwapchain() {
    if (!device_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        throw std::runtime_error("This Vulkan surface does not support transfer-destination swapchain images.");

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, formats.data());
    VkSurfaceFormatKHR chosen = formats.front();
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f; break;
        }
    }
    swapFormat_ = chosen.format;

    if (caps.currentExtent.width != UINT32_MAX) extent_ = caps.currentExtent;
    else {
        extent_.width = std::clamp<uint32_t>(static_cast<uint32_t>(rc.right), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp<uint32_t>(static_cast<uint32_t>(rc.bottom), caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = std::max(2u, caps.minImageCount + 1);
    if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    else
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    vkCheck(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "Could not create Vulkan swapchain.");

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swapImages_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapImages_.data());
    resizePending_ = false;
}

void VulkanRenderer::destroySwapchain() {
    swapImages_.clear();
    if (device_ && swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanRenderer::resized() { resizePending_ = true; }

uint32_t VulkanRenderer::findMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("No suitable Vulkan memory type.");
}

VkCommandBuffer VulkanRenderer::beginOneTime() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = commandPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    vkCheck(vkAllocateCommandBuffers(device_, &ai, &cmd), "Could not allocate Vulkan command buffer.");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void VulkanRenderer::endOneTime(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkCheck(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE), "Vulkan queue submit failed.");
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
}

void VulkanRenderer::transition(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanRenderer::destroyTexture() {
    if (!device_) return;
    if (texture_) vkDestroyImage(device_, texture_, nullptr);
    if (textureMemory_) vkFreeMemory(device_, textureMemory_, nullptr);
    texture_ = VK_NULL_HANDLE;
    textureMemory_ = VK_NULL_HANDLE;
    textureW_ = textureH_ = 0;
}

void VulkanRenderer::setImage(const ImageRGBA& rgba) {
    if (!device_ || rgba.empty()) return;
    vkDeviceWaitIdle(device_);
    destroyTexture();

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgba.pixels.size());

    VkBuffer staging{};
    VkDeviceMemory stagingMem{};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device_, &bci, nullptr, &staging), "Could not create Vulkan staging buffer.");
    VkMemoryRequirements br{};
    vkGetBufferMemoryRequirements(device_, staging, &br);
    VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bai.allocationSize = br.size;
    bai.memoryTypeIndex = findMemoryType(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkCheck(vkAllocateMemory(device_, &bai, nullptr, &stagingMem), "Could not allocate Vulkan staging memory.");
    vkBindBufferMemory(device_, staging, stagingMem, 0);
    void* mapped = nullptr;
    vkMapMemory(device_, stagingMem, 0, bytes, 0, &mapped);
    std::memcpy(mapped, rgba.pixels.data(), static_cast<size_t>(bytes));
    vkUnmapMemory(device_, stagingMem);

    textureW_ = static_cast<uint32_t>(rgba.width);
    textureH_ = static_cast<uint32_t>(rgba.height);
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {textureW_, textureH_, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(device_, &ici, nullptr, &texture_), "Could not create Vulkan image.");
    VkMemoryRequirements ir{};
    vkGetImageMemoryRequirements(device_, texture_, &ir);
    VkMemoryAllocateInfo iai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    iai.allocationSize = ir.size;
    iai.memoryTypeIndex = findMemoryType(ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(device_, &iai, nullptr, &textureMemory_), "Could not allocate Vulkan image memory.");
    vkBindImageMemory(device_, texture_, textureMemory_, 0);

    VkCommandBuffer cmd = beginOneTime();
    transition(cmd, texture_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {textureW_, textureH_, 1};
    vkCmdCopyBufferToImage(cmd, staging, texture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(cmd, texture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    endOneTime(cmd);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
}

void VulkanRenderer::draw() {
    if (!device_ || !texture_) return;
    if (resizePending_) {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        createSwapchain();
    }
    if (!swapchain_ || extent_.width == 0 || extent_.height == 0) return;

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { resized(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) return;

    VkCommandBuffer cmd = beginOneTime();
    transition(cmd, swapImages_[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkClearColorValue black{{0.035f,0.035f,0.045f,1.0f}};
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(cmd, swapImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

    const double srcAspect = static_cast<double>(textureW_) / textureH_;
    const double dstAspect = static_cast<double>(extent_.width) / extent_.height;
    int x0 = 0, y0 = 0, x1 = static_cast<int>(extent_.width), y1 = static_cast<int>(extent_.height);
    if (dstAspect > srcAspect) {
        const int w = static_cast<int>(extent_.height * srcAspect);
        x0 = (static_cast<int>(extent_.width) - w) / 2; x1 = x0 + w;
    } else {
        const int h = static_cast<int>(extent_.width / srcAspect);
        y0 = (static_cast<int>(extent_.height) - h) / 2; y1 = y0 + h;
    }

    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1] = {static_cast<int32_t>(textureW_), static_cast<int32_t>(textureH_), 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = {x0,y0,0};
    blit.dstOffsets[1] = {x1,y1,1};
    vkCmdBlitImage(cmd, texture_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    transition(cmd, swapImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished_;
    if (vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, commandPool_, 1, &cmd); return;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) resized();
}
