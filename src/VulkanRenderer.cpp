#include "VulkanRenderer.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <array>
#include <cmath>
#include <fstream>
#include <cctype>

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
    createGearResources();
}

void VulkanRenderer::shutdown() {
    if (!instance_) return;
    if (device_) vkDeviceWaitIdle(device_);
    destroyTexture();
    destroyGearResources();
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
    const VkImageUsageFlags requiredUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((caps.supportedUsageFlags & requiredUsage) != requiredUsage)
        throw std::runtime_error("This Vulkan surface does not support transfer-destination + color-attachment swapchain images.");

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
    ci.imageUsage = requiredUsage;
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
    if(gearRenderPass_) createGearFramebuffers();
}

void VulkanRenderer::destroySwapchain() {
    destroyGearFramebuffers();
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


namespace {
struct GearVertex { float p[3]; float n[3]; float uv[2]; };
struct Mat4 { float m[16]{}; };
Mat4 ident(){Mat4 r{};r.m[0]=r.m[5]=r.m[10]=r.m[15]=1;return r;}
Mat4 mul(const Mat4&a,const Mat4&b){Mat4 r{};for(int col=0;col<4;++col)for(int row=0;row<4;++row)for(int k=0;k<4;++k)r.m[col*4+row]+=a.m[k*4+row]*b.m[col*4+k];return r;}
Mat4 translate(float x,float y,float z){auto r=ident();r.m[12]=x;r.m[13]=y;r.m[14]=z;return r;}
Mat4 rotZ(float a){auto r=ident();float c=std::cos(a),s=std::sin(a);r.m[0]=c;r.m[4]=-s;r.m[1]=s;r.m[5]=c;return r;}
Mat4 perspective(float f,float aspect,float zn,float zf){Mat4 r{};float q=1/std::tan(f*.5f);r.m[0]=q/aspect;r.m[5]=-q;r.m[10]=zf/(zn-zf);r.m[11]=-1;r.m[14]=(zn*zf)/(zn-zf);return r;}
struct V3{float x,y,z;}; V3 sub(V3 a,V3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};} float dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;} V3 cross(V3 a,V3 b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};} V3 norm(V3 a){float l=std::sqrt(dot(a,a));return{a.x/l,a.y/l,a.z/l};}
Mat4 lookAt(V3 e,V3 at,V3 up){V3 f=norm(sub(at,e)),s=norm(cross(f,up)),u=cross(s,f);Mat4 r=ident();r.m[0]=s.x;r.m[1]=s.y;r.m[2]=s.z;r.m[4]=u.x;r.m[5]=u.y;r.m[6]=u.z;r.m[8]=-f.x;r.m[9]=-f.y;r.m[10]=-f.z;r.m[12]=-dot(s,e);r.m[13]=-dot(u,e);r.m[14]=dot(f,e);return r;}
void appendGear(std::vector<GearVertex>&v,std::vector<uint32_t>&ix,int teeth,float root,float outer,float hole,float halfZ){
    const int seg=teeth*4;std::vector<float> rad(seg);for(int i=0;i<seg;++i)rad[i]=(i%4==1||i%4==2)?outer:root;
    auto push=[&](float x,float y,float z,float nx,float ny,float nz){
        float u=.5f+x/(outer*2.4f), vv=.5f+y/(outer*2.4f);
        if(std::abs(nz)<.5f){u=std::atan2(y,x)/6.28318530718f+.5f;vv=(z+halfZ)/(2*halfZ);}
        v.push_back({{x,y,z},{nx,ny,nz},{u,vv}});return uint32_t(v.size()-1);
    };
    for(int i=0;i<seg;++i){int j=(i+1)%seg;float a=6.28318530718f*i/seg,b=6.28318530718f*j/seg;float r0=rad[i],r1=rad[j];
        // Front/back annular quads.
        uint32_t f0=push(hole*std::cos(a),hole*std::sin(a),halfZ,0,0,1),f1=push(r0*std::cos(a),r0*std::sin(a),halfZ,0,0,1),f2=push(r1*std::cos(b),r1*std::sin(b),halfZ,0,0,1),f3=push(hole*std::cos(b),hole*std::sin(b),halfZ,0,0,1);
        ix.insert(ix.end(),{f0,f1,f2,f0,f2,f3});
        uint32_t q0=push(hole*std::cos(a),hole*std::sin(a),-halfZ,0,0,-1),q1=push(hole*std::cos(b),hole*std::sin(b),-halfZ,0,0,-1),q2=push(r1*std::cos(b),r1*std::sin(b),-halfZ,0,0,-1),q3=push(r0*std::cos(a),r0*std::sin(a),-halfZ,0,0,-1);
        ix.insert(ix.end(),{q0,q1,q2,q0,q2,q3});
        // Outer tooth wall.
        float x0=r0*std::cos(a),y0=r0*std::sin(a),x1=r1*std::cos(b),y1=r1*std::sin(b);V3 n=norm(V3{y1-y0,-(x1-x0),0});
        uint32_t o0=push(x0,y0,-halfZ,n.x,n.y,0),o1=push(x1,y1,-halfZ,n.x,n.y,0),o2=push(x1,y1,halfZ,n.x,n.y,0),o3=push(x0,y0,halfZ,n.x,n.y,0);ix.insert(ix.end(),{o0,o1,o2,o0,o2,o3});
        // Inner bore wall.
        V3 ni{-std::cos((a+b)*.5f),-std::sin((a+b)*.5f),0};uint32_t h0=push(hole*std::cos(a),hole*std::sin(a),-halfZ,ni.x,ni.y,0),h1=push(hole*std::cos(a),hole*std::sin(a),halfZ,ni.x,ni.y,0),h2=push(hole*std::cos(b),hole*std::sin(b),halfZ,ni.x,ni.y,0),h3=push(hole*std::cos(b),hole*std::sin(b),-halfZ,ni.x,ni.y,0);ix.insert(ix.end(),{h0,h1,h2,h0,h2,h3});
    }
}
void appendSpindle(std::vector<GearVertex>&v,std::vector<uint32_t>&ix,float radius,float halfZ,int seg=32){
    auto push=[&](float x,float y,float z,float nx,float ny,float nz){
        float u=.5f+x/(radius*2.4f), vv=.5f+y/(radius*2.4f);
        if(std::abs(nz)<.5f){u=std::atan2(y,x)/6.28318530718f+.5f;vv=(z+halfZ)/(2*halfZ);}
        v.push_back({{x,y,z},{nx,ny,nz},{u,vv}});return uint32_t(v.size()-1);
    };
    for(int i=0;i<seg;++i){int j=(i+1)%seg;float a=6.28318530718f*i/seg,b=6.28318530718f*j/seg;
        float ca=std::cos(a),sa=std::sin(a),cb=std::cos(b),sb=std::sin(b);
        uint32_t t0=push(0,0,halfZ,0,0,1),t1=push(radius*ca,radius*sa,halfZ,0,0,1),t2=push(radius*cb,radius*sb,halfZ,0,0,1);ix.insert(ix.end(),{t0,t1,t2});
        uint32_t b0=push(0,0,-halfZ,0,0,-1),b1=push(radius*cb,radius*sb,-halfZ,0,0,-1),b2=push(radius*ca,radius*sa,-halfZ,0,0,-1);ix.insert(ix.end(),{b0,b1,b2});
        V3 n0{ca,sa,0},n1{cb,sb,0};uint32_t s0=push(radius*ca,radius*sa,-halfZ,n0.x,n0.y,0),s1=push(radius*cb,radius*sb,-halfZ,n1.x,n1.y,0),s2=push(radius*cb,radius*sb,halfZ,n1.x,n1.y,0),s3=push(radius*ca,radius*sa,halfZ,n0.x,n0.y,0);ix.insert(ix.end(),{s0,s1,s2,s0,s2,s3});
    }
}
std::vector<uint32_t> readSpv(const wchar_t* name){wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);std::wstring path(exe);auto slash=path.find_last_of(L"\\/");path=(slash==std::wstring::npos?L"":path.substr(0,slash+1))+name;std::ifstream f(path.c_str(),std::ios::binary|std::ios::ate);if(!f)return{};auto n=f.tellg();std::vector<uint32_t>d((size_t(n)+3)/4);f.seekg(0);f.read((char*)d.data(),n);return d;}
}
void VulkanRenderer::destroyGearFramebuffers(){
    if(!device_)return;
    for(auto f:gearFramebuffers_)vkDestroyFramebuffer(device_,f,nullptr);gearFramebuffers_.clear();
    for(auto v:gearColorViews_)vkDestroyImageView(device_,v,nullptr);gearColorViews_.clear();
    if(gearOffscreenFramebuffer_)vkDestroyFramebuffer(device_,gearOffscreenFramebuffer_,nullptr);
    if(gearOffscreenColorView_)vkDestroyImageView(device_,gearOffscreenColorView_,nullptr);
    if(gearOffscreenColor_)vkDestroyImage(device_,gearOffscreenColor_,nullptr);
    if(gearOffscreenColorMemory_)vkFreeMemory(device_,gearOffscreenColorMemory_,nullptr);
    if(gearDepthView_)vkDestroyImageView(device_,gearDepthView_,nullptr);if(gearDepth_)vkDestroyImage(device_,gearDepth_,nullptr);if(gearDepthMemory_)vkFreeMemory(device_,gearDepthMemory_,nullptr);
    gearOffscreenFramebuffer_=VK_NULL_HANDLE;gearOffscreenColorView_=VK_NULL_HANDLE;gearOffscreenColor_=VK_NULL_HANDLE;gearOffscreenColorMemory_=VK_NULL_HANDLE;
    gearDepthView_=VK_NULL_HANDLE;gearDepth_=VK_NULL_HANDLE;gearDepthMemory_=VK_NULL_HANDLE;
}
void VulkanRenderer::createGearFramebuffers(){
    if(!device_||!gearRenderPass_||!gearTargetExtent_.width||!gearTargetExtent_.height)return;destroyGearFramebuffers();
    const uint32_t w=gearTargetExtent_.width,h=gearTargetExtent_.height;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=swapFormat_;ci.extent={w,h,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(device_,&ci,nullptr,&gearOffscreenColor_),"Could not create gear offscreen color image.");VkMemoryRequirements mr{};vkGetImageMemoryRequirements(device_,gearOffscreenColor_,&mr);VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=findMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);vkCheck(vkAllocateMemory(device_,&ma,nullptr,&gearOffscreenColorMemory_),"Could not allocate gear offscreen color memory.");vkBindImageMemory(device_,gearOffscreenColor_,gearOffscreenColorMemory_,0);
    VkImageViewCreateInfo cv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};cv.image=gearOffscreenColor_;cv.viewType=VK_IMAGE_VIEW_TYPE_2D;cv.format=swapFormat_;cv.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;cv.subresourceRange.levelCount=1;cv.subresourceRange.layerCount=1;vkCheck(vkCreateImageView(device_,&cv,nullptr,&gearOffscreenColorView_),"Could not create gear offscreen color view.");
    gearDepthFormat_=VK_FORMAT_D32_SFLOAT;VkImageCreateInfo di{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};di.imageType=VK_IMAGE_TYPE_2D;di.format=gearDepthFormat_;di.extent={w,h,1};di.mipLevels=1;di.arrayLayers=1;di.samples=VK_SAMPLE_COUNT_1_BIT;di.tiling=VK_IMAGE_TILING_OPTIMAL;di.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;di.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(device_,&di,nullptr,&gearDepth_),"Could not create gear depth image.");vkGetImageMemoryRequirements(device_,gearDepth_,&mr);ma.allocationSize=mr.size;ma.memoryTypeIndex=findMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);vkCheck(vkAllocateMemory(device_,&ma,nullptr,&gearDepthMemory_),"Could not allocate gear depth memory.");vkBindImageMemory(device_,gearDepth_,gearDepthMemory_,0);
    VkImageViewCreateInfo dv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};dv.image=gearDepth_;dv.viewType=VK_IMAGE_VIEW_TYPE_2D;dv.format=gearDepthFormat_;dv.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;dv.subresourceRange.levelCount=1;dv.subresourceRange.layerCount=1;vkCheck(vkCreateImageView(device_,&dv,nullptr,&gearDepthView_),"Could not create gear depth view.");
    VkImageView at[]={gearOffscreenColorView_,gearDepthView_};VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=gearRenderPass_;fi.attachmentCount=2;fi.pAttachments=at;fi.width=w;fi.height=h;fi.layers=1;vkCheck(vkCreateFramebuffer(device_,&fi,nullptr,&gearOffscreenFramebuffer_),"Could not create gear offscreen framebuffer.");
}
void VulkanRenderer::destroyGearResources(){
    if(!device_)return;destroyGearFramebuffers();if(gearPipeline_)vkDestroyPipeline(device_,gearPipeline_,nullptr);if(hudPipeline_)vkDestroyPipeline(device_,hudPipeline_,nullptr);if(hudPipelineLayout_)vkDestroyPipelineLayout(device_,hudPipelineLayout_,nullptr);
    if(gearTextureSampler_)vkDestroySampler(device_,gearTextureSampler_,nullptr);
    for(int mi=0;mi<3;++mi){if(gearTextureViews_[mi])vkDestroyImageView(device_,gearTextureViews_[mi],nullptr);if(gearTextures_[mi])vkDestroyImage(device_,gearTextures_[mi],nullptr);if(gearTextureMemories_[mi])vkFreeMemory(device_,gearTextureMemories_[mi],nullptr);}
    if(gearDescriptorPool_)vkDestroyDescriptorPool(device_,gearDescriptorPool_,nullptr);
    if(gearPipelineLayout_)vkDestroyPipelineLayout(device_,gearPipelineLayout_,nullptr);
    if(gearDescriptorSetLayout_)vkDestroyDescriptorSetLayout(device_,gearDescriptorSetLayout_,nullptr);
    if(gearRenderPass_)vkDestroyRenderPass(device_,gearRenderPass_,nullptr);if(gearVertexBuffer_)vkDestroyBuffer(device_,gearVertexBuffer_,nullptr);if(gearVertexMemory_)vkFreeMemory(device_,gearVertexMemory_,nullptr);if(gearIndexBuffer_)vkDestroyBuffer(device_,gearIndexBuffer_,nullptr);if(gearIndexMemory_)vkFreeMemory(device_,gearIndexMemory_,nullptr);
    gearPipeline_=VK_NULL_HANDLE;gearPipelineLayout_=VK_NULL_HANDLE;hudPipeline_=VK_NULL_HANDLE;hudPipelineLayout_=VK_NULL_HANDLE;gearRenderPass_=VK_NULL_HANDLE;
    gearDescriptorSetLayout_=VK_NULL_HANDLE;gearDescriptorPool_=VK_NULL_HANDLE;
    for(int mi=0;mi<3;++mi){gearDescriptorSets_[mi]=VK_NULL_HANDLE;gearTextures_[mi]=VK_NULL_HANDLE;gearTextureMemories_[mi]=VK_NULL_HANDLE;gearTextureViews_[mi]=VK_NULL_HANDLE;}gearTextureSampler_=VK_NULL_HANDLE;gearVertexBuffer_=gearIndexBuffer_=VK_NULL_HANDLE;gearVertexMemory_=gearIndexMemory_=VK_NULL_HANDLE;gearIndexCount_=0;
}
void VulkanRenderer::createGearResources(){
    auto vs=readSpv(L"cog.vert.spv"),fs=readSpv(L"cog.frag.spv");if(vs.empty()||fs.empty())return;
    std::vector<GearVertex> verts;std::vector<uint32_t> inds;
    // Matched-module 16T/8T pair. Radius is proportional to tooth count so both
    // gears retain the same circumferential tooth pitch.
    appendGear(verts,inds,16,1.00f,1.16f,.32f,.22f);
    gearLargeIndexCount_=(uint32_t)inds.size();gearLargeSpindleFirstIndex_=gearLargeIndexCount_;
    appendSpindle(verts,inds,.25f,.40f);gearLargeSpindleIndexCount_=(uint32_t)inds.size()-gearLargeSpindleFirstIndex_;
    const uint32_t smallVertexBase=(uint32_t)verts.size();gearSmallFirstIndex_=(uint32_t)inds.size();
    std::vector<GearVertex> sv;std::vector<uint32_t> si;appendGear(sv,si,8,.50f,.58f,.20f,.22f);
    gearSmallIndexCount_=(uint32_t)si.size();gearSmallSpindleFirstIndex_=gearSmallFirstIndex_+gearSmallIndexCount_;
    appendSpindle(sv,si,.15f,.40f);gearSmallSpindleIndexCount_=(uint32_t)si.size()-gearSmallIndexCount_;
    verts.insert(verts.end(),sv.begin(),sv.end());for(uint32_t i:si)inds.push_back(i+smallVertexBase);
    gearIndexCount_=(uint32_t)inds.size();
    auto buffer=[&](VkDeviceSize size,VkBufferUsageFlags use,VkBuffer&b,VkDeviceMemory&m,const void*src){VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=use;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;vkCheck(vkCreateBuffer(device_,&bi,nullptr,&b),"Could not create gear buffer.");VkMemoryRequirements mr{};vkGetBufferMemoryRequirements(device_,b,&mr);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=findMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);vkCheck(vkAllocateMemory(device_,&ai,nullptr,&m),"Could not allocate gear buffer.");vkBindBufferMemory(device_,b,m,0);void*p{};vkMapMemory(device_,m,0,size,0,&p);std::memcpy(p,src,(size_t)size);vkUnmapMemory(device_,m);};
    buffer(verts.size()*sizeof(GearVertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,gearVertexBuffer_,gearVertexMemory_,verts.data());buffer(inds.size()*4,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,gearIndexBuffer_,gearIndexMemory_,inds.data());
    VkAttachmentDescription at[2]{};at[0].format=swapFormat_;at[0].samples=VK_SAMPLE_COUNT_1_BIT;at[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;at[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;at[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;at[0].finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;at[1].format=VK_FORMAT_D32_SFLOAT;at[1].samples=VK_SAMPLE_COUNT_1_BIT;at[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;at[1].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;at[1].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;at[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cr{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},dr{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};VkSubpassDescription sp{};sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sp.colorAttachmentCount=1;sp.pColorAttachments=&cr;sp.pDepthStencilAttachment=&dr;VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};ri.attachmentCount=2;ri.pAttachments=at;ri.subpassCount=1;ri.pSubpasses=&sp;vkCheck(vkCreateRenderPass(device_,&ri,nullptr,&gearRenderPass_),"Could not create gear render pass.");
    // Each calibration material owns a completely independent Vulkan image,
    // view and descriptor set.  No atlas is used, so filtering cannot bleed
    // wood/resin/metal texels into a neighbouring material.
    wchar_t texExe[MAX_PATH]{};GetModuleFileNameW(nullptr,texExe,MAX_PATH);
    std::wstring textureDir(texExe);auto texSlash=textureDir.find_last_of(L"\\/");
    textureDir=(texSlash==std::wstring::npos?L"":textureDir.substr(0,texSlash+1));
    auto loadPpm=[&](const wchar_t* name,uint32_t& w,uint32_t& h){
        std::wstring path=textureDir+name;std::ifstream tf(path.c_str());
        if(!tf) throw std::runtime_error("Missing calibration PPM texture");
        auto token=[&]()->std::string{std::string q;while(tf>>q){if(!q.empty()&&q[0]=='#'){std::string rest;std::getline(tf,rest);continue;}return q;}return {};};
        if(token()!="P3") throw std::runtime_error("Invalid calibration texture: expected P3 PPM");
        w=(uint32_t)std::stoul(token());h=(uint32_t)std::stoul(token());uint32_t maxv=(uint32_t)std::stoul(token());
        if(!w||!h||w>4096||h>4096||!maxv||maxv>65535) throw std::runtime_error("Invalid calibration texture dimensions/range");
        std::vector<uint8_t> px(size_t(w)*h*4);
        for(size_t i=0;i<size_t(w)*h;++i){for(int ch=0;ch<3;++ch){auto q=token();if(q.empty())throw std::runtime_error("Truncated calibration texture");unsigned v=(unsigned)std::stoul(q);px[i*4+ch]=(uint8_t)std::min(255u,(v*255u)/maxv);}px[i*4+3]=255;}
        return px;
    };
    const wchar_t* materialFiles[3]={L"cog_wood.ppm",L"cog_resin.ppm",L"spindle_metal.ppm"};
    for(int mi=0;mi<3;++mi){
        uint32_t tw=0,th=0;auto tex=loadPpm(materialFiles[mi],tw,th);VkDeviceSize tbytes=tex.size();
        VkBuffer ts{};VkDeviceMemory tsm{};VkBufferCreateInfo tbi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};tbi.size=tbytes;tbi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;tbi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;vkCheck(vkCreateBuffer(device_,&tbi,nullptr,&ts),"Could not create material texture staging buffer.");
        VkMemoryRequirements tmr{};vkGetBufferMemoryRequirements(device_,ts,&tmr);VkMemoryAllocateInfo tma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};tma.allocationSize=tmr.size;tma.memoryTypeIndex=findMemoryType(tmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);vkCheck(vkAllocateMemory(device_,&tma,nullptr,&tsm),"Could not allocate material texture staging memory.");vkBindBufferMemory(device_,ts,tsm,0);void*tmapped{};vkMapMemory(device_,tsm,0,tbytes,0,&tmapped);std::memcpy(tmapped,tex.data(),tex.size());vkUnmapMemory(device_,tsm);
        VkImageCreateInfo ti{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ti.imageType=VK_IMAGE_TYPE_2D;ti.format=VK_FORMAT_R8G8B8A8_UNORM;ti.extent={tw,th,1};ti.mipLevels=1;ti.arrayLayers=1;ti.samples=VK_SAMPLE_COUNT_1_BIT;ti.tiling=VK_IMAGE_TILING_OPTIMAL;ti.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ti.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ti.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;vkCheck(vkCreateImage(device_,&ti,nullptr,&gearTextures_[mi]),"Could not create material texture image.");
        vkGetImageMemoryRequirements(device_,gearTextures_[mi],&tmr);tma.allocationSize=tmr.size;tma.memoryTypeIndex=findMemoryType(tmr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);vkCheck(vkAllocateMemory(device_,&tma,nullptr,&gearTextureMemories_[mi]),"Could not allocate material texture memory.");vkBindImageMemory(device_,gearTextures_[mi],gearTextureMemories_[mi],0);
        VkCommandBuffer tc=beginOneTime();transition(tc,gearTextures_[mi],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);VkBufferImageCopy bic{};bic.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;bic.imageSubresource.layerCount=1;bic.imageExtent={tw,th,1};vkCmdCopyBufferToImage(tc,ts,gearTextures_[mi],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bic);transition(tc,gearTextures_[mi],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);endOneTime(tc);vkDestroyBuffer(device_,ts,nullptr);vkFreeMemory(device_,tsm,nullptr);
        VkImageViewCreateInfo tiv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};tiv.image=gearTextures_[mi];tiv.viewType=VK_IMAGE_VIEW_TYPE_2D;tiv.format=VK_FORMAT_R8G8B8A8_UNORM;tiv.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;tiv.subresourceRange.levelCount=1;tiv.subresourceRange.layerCount=1;vkCheck(vkCreateImageView(device_,&tiv,nullptr,&gearTextureViews_[mi]),"Could not create material texture view.");
    }
    VkSamplerCreateInfo tsi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};tsi.magFilter=VK_FILTER_LINEAR;tsi.minFilter=VK_FILTER_LINEAR;tsi.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;tsi.addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT;tsi.addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT;tsi.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;tsi.maxLod=0;vkCheck(vkCreateSampler(device_,&tsi,nullptr,&gearTextureSampler_),"Could not create material texture sampler.");
    VkDescriptorSetLayoutBinding dlb{};dlb.binding=0;dlb.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;dlb.descriptorCount=1;dlb.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dlci.bindingCount=1;dlci.pBindings=&dlb;vkCheck(vkCreateDescriptorSetLayout(device_,&dlci,nullptr,&gearDescriptorSetLayout_),"Could not create gear descriptor layout.");
    VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3};VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=3;dpci.poolSizeCount=1;dpci.pPoolSizes=&dps;vkCheck(vkCreateDescriptorPool(device_,&dpci,nullptr,&gearDescriptorPool_),"Could not create gear descriptor pool.");
    VkDescriptorSetLayout layouts[3]={gearDescriptorSetLayout_,gearDescriptorSetLayout_,gearDescriptorSetLayout_};VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dsai.descriptorPool=gearDescriptorPool_;dsai.descriptorSetCount=3;dsai.pSetLayouts=layouts;vkCheck(vkAllocateDescriptorSets(device_,&dsai,gearDescriptorSets_),"Could not allocate material descriptor sets.");
    for(int mi=0;mi<3;++mi){VkDescriptorImageInfo dii{gearTextureSampler_,gearTextureViews_[mi],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet dw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};dw.dstSet=gearDescriptorSets_[mi];dw.dstBinding=0;dw.descriptorCount=1;dw.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;dw.pImageInfo=&dii;vkUpdateDescriptorSets(device_,1,&dw,0,nullptr);}

    struct PC{Mat4 mvp,model;float color[4];};VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(PC)};VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&gearDescriptorSetLayout_;li.pushConstantRangeCount=1;li.pPushConstantRanges=&pr;vkCheck(vkCreatePipelineLayout(device_,&li,nullptr,&gearPipelineLayout_),"Could not create gear pipeline layout.");
    auto sm=[&](const std::vector<uint32_t>&d){VkShaderModule s{};VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};si.codeSize=d.size()*4;si.pCode=d.data();vkCheck(vkCreateShaderModule(device_,&si,nullptr,&s),"Could not create gear shader.");return s;};VkShaderModule vsm=sm(vs),fsm=sm(fs);VkPipelineShaderStageCreateInfo ss[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vsm,"main",nullptr},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fsm,"main",nullptr}};
    VkVertexInputBindingDescription bd{0,sizeof(GearVertex),VK_VERTEX_INPUT_RATE_VERTEX};VkVertexInputAttributeDescription ad[3]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(GearVertex,p)},{1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(GearVertex,n)},{2,0,VK_FORMAT_R32G32_SFLOAT,offsetof(GearVertex,uv)}};VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&bd;vi.vertexAttributeDescriptionCount=3;vi.pVertexAttributeDescriptions=ad;VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.scissorCount=1;VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_LESS;VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};bs.attachmentCount=1;bs.pAttachments=&ba;VkDynamicState dyns[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dyns;VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gi.stageCount=2;gi.pStages=ss;gi.pVertexInputState=&vi;gi.pInputAssemblyState=&ia;gi.pViewportState=&vp;gi.pRasterizationState=&rs;gi.pMultisampleState=&ms;gi.pDepthStencilState=&ds;gi.pColorBlendState=&bs;gi.pDynamicState=&dy;gi.layout=gearPipelineLayout_;gi.renderPass=gearRenderPass_;vkCheck(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&gi,nullptr,&gearPipeline_),"Could not create 3D gear pipeline.");vkDestroyShaderModule(device_,vsm,nullptr);vkDestroyShaderModule(device_,fsm,nullptr);
    // Dedicated depth-free framebuffer HUD, sharing the gear render pass.
    auto hv=readSpv(L"hud.vert.spv"),hf=readSpv(L"hud.frag.spv");
    if(!hv.empty()&&!hf.empty()){
        VkPipelineLayoutCreateInfo hli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        vkCheck(vkCreatePipelineLayout(device_,&hli,nullptr,&hudPipelineLayout_),"HUD pipeline layout failed.");
        VkShaderModule hvs=sm(hv),hfs=sm(hf);
        VkPipelineShaderStageCreateInfo hs[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,hvs,"main",nullptr},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,hfs,"main",nullptr}};
        VkVertexInputBindingDescription hbd{0,sizeof(float)*2,VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription had{0,0,VK_FORMAT_R32G32_SFLOAT,0};
        VkPipelineVertexInputStateCreateInfo hvi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        hvi.vertexBindingDescriptionCount=1;hvi.pVertexBindingDescriptions=&hbd;hvi.vertexAttributeDescriptionCount=1;hvi.pVertexAttributeDescriptions=&had;
        VkPipelineDepthStencilStateCreateInfo hds=ds;hds.depthTestEnable=VK_FALSE;hds.depthWriteEnable=VK_FALSE;
        VkGraphicsPipelineCreateInfo hgi=gi;hgi.pStages=hs;hgi.pVertexInputState=&hvi;hgi.pDepthStencilState=&hds;hgi.layout=hudPipelineLayout_;
        vkCheck(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&hgi,nullptr,&hudPipeline_),"HUD pipeline creation failed.");
        vkDestroyShaderModule(device_,hvs,nullptr);vkDestroyShaderModule(device_,hfs,nullptr);
    }
}
bool VulkanRenderer::drawGearCalibration(float seconds,uint32_t targetWidth,uint32_t targetHeight){
    if(!device_||!gearPipeline_||!swapchain_)return false;
    if(resizePending_){vkDeviceWaitIdle(device_);destroySwapchain();createSwapchain();if(!swapchain_)return false;}
    VkExtent2D wanted{targetWidth?targetWidth:extent_.width,targetHeight?targetHeight:extent_.height};
    if(!wanted.width||!wanted.height)return false;
    if(wanted.width!=gearTargetExtent_.width||wanted.height!=gearTargetExtent_.height||!gearOffscreenFramebuffer_){vkDeviceWaitIdle(device_);gearTargetExtent_=wanted;createGearFramebuffers();}
    uint32_t imageIndex=0;VkResult acq=vkAcquireNextImageKHR(device_,swapchain_,UINT64_MAX,imageAvailable_,VK_NULL_HANDLE,&imageIndex);if(acq==VK_ERROR_OUT_OF_DATE_KHR){resized();return false;}if(acq!=VK_SUCCESS&&acq!=VK_SUBOPTIMAL_KHR)return false;
    VkCommandBuffer cmd=beginOneTime();VkClearValue clears[2]{};clears[0].color={{.025f,.035f,.055f,1}};clears[1].depthStencil={1,0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=gearRenderPass_;rb.framebuffer=gearOffscreenFramebuffer_;rb.renderArea.extent=gearTargetExtent_;rb.clearValueCount=2;rb.pClearValues=clears;vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gearPipeline_);VkDeviceSize off=0;vkCmdBindVertexBuffers(cmd,0,1,&gearVertexBuffer_,&off);vkCmdBindIndexBuffer(cmd,gearIndexBuffer_,0,VK_INDEX_TYPE_UINT32);
    VkViewport viewport{0,0,(float)gearTargetExtent_.width,(float)gearTargetExtent_.height,0,1};VkRect2D sc{{0,0},gearTargetExtent_};vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&sc);
    struct PC{Mat4 mvp,model;float color[4];};float orbit=seconds*.45f;V3 eye{5.8f*std::cos(orbit),5.8f*std::sin(orbit),4.1f};float aspect=float(gearTargetExtent_.width)/float(gearTargetExtent_.height);float fov=(aspect<1.0f)?1.02f:.90f;Mat4 vp=mul(perspective(fov,aspect,.035f,40.0f),lookAt(eye,{0.22f,0,0},{0,0,1}));
    auto part=[&](float x,float spin,float material,uint32_t count,uint32_t first){int mi=std::clamp((int)(material+0.5f),0,2);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gearPipelineLayout_,0,1,&gearDescriptorSets_[mi],0,nullptr);Mat4 model=mul(translate(x,0,0),rotZ(spin));PC pc{mul(vp,model),model,{0,0,0,material}};vkCmdPushConstants(cmd,gearPipelineLayout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(pc),&pc);vkCmdDrawIndexed(cmd,count,1,first,0,0);};
    constexpr float leftX=-.54f,rightX=1.08f,smallHalfTooth=3.14159265359f/8.0f;const float spin=seconds*1.35f,smallSpin=-2.0f*spin+smallHalfTooth;
    part(leftX,spin,0.0f,gearLargeIndexCount_,0);part(leftX,spin,2.0f,gearLargeSpindleIndexCount_,gearLargeSpindleFirstIndex_);part(rightX,smallSpin,1.0f,gearSmallIndexCount_,gearSmallFirstIndex_);part(rightX,smallSpin,2.0f,gearSmallSpindleIndexCount_,gearSmallSpindleFirstIndex_);
    // Generate a compact 3x5 raster font as native Vulkan triangles.
    if(hudPipeline_&&!calibrationBackend_.empty()){
        static const char* glyph[]={"111101101101111","010110010010111","111001111100111","111001111001111","101101111001001","111100111001111","111100111101111","111001001001001","111101111101111","111101111001111","010101111101101","110101110101110","111100100100111","110101101101110","111100110100111","111100110100100","111100101101111","101101111101101","111010010010111","001001001101111","101101110101101","100100100100111","101111111101101","101111111111101","111101101101111","111101111100100","111101101111001","111101111110101","111100111001111","111010010010010","101101101101111","101101101101010","101101111111101","101101010101101","101101010010010","111001010100111"};
        std::string label=calibrationBackend_+"  "+calibrationGpu_;
        std::transform(label.begin(),label.end(),label.begin(),[](unsigned char ch){return (char)std::toupper(ch);});
        std::vector<float> triangles;float unit=std::min(.010f,1.82f/std::max<size_t>(1,label.size()*4));float x=-.94f,y=.91f;
        auto square=[&](float xx,float yy){float w=unit*.85f,h=unit*.85f;float q[]={xx,yy,xx+w,yy,xx+w,yy-h,xx,yy,xx+w,yy-h,xx,yy-h};triangles.insert(triangles.end(),q,q+12);};
        for(char ch:label){if(ch==' '){x+=unit*2.5f;continue;}int idx=-1;if(ch>='0'&&ch<='9')idx=ch-'0';else if(ch>='A'&&ch<='Z')idx=ch-'A'+10;
            if(idx>=0)for(int r=0;r<5;++r)for(int col=0;col<3;++col)if(glyph[idx][r*3+col]=='1')square(x+col*unit,y-r*unit);x+=unit*3.8f;}
        if(!triangles.empty()){
            VkBuffer vb{};VkDeviceMemory vm{};VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=triangles.size()*sizeof(float);bi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            vkCheck(vkCreateBuffer(device_,&bi,nullptr,&vb),"HUD buffer creation failed.");VkMemoryRequirements mr{};vkGetBufferMemoryRequirements(device_,vb,&mr);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=findMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkCheck(vkAllocateMemory(device_,&ai,nullptr,&vm),"HUD memory allocation failed.");vkBindBufferMemory(device_,vb,vm,0);void* ptr=nullptr;vkMapMemory(device_,vm,0,bi.size,0,&ptr);std::memcpy(ptr,triangles.data(),bi.size);vkUnmapMemory(device_,vm);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,hudPipeline_);VkDeviceSize offset=0;vkCmdBindVertexBuffers(cmd,0,1,&vb,&offset);vkCmdDraw(cmd,(uint32_t)(triangles.size()/2),1,0,0);
            // The recorded draw must complete before releasing its transient vertex buffer.
            vkCmdEndRenderPass(cmd);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
            vkCheck(vkQueueSubmit(queue_,1,&submit,VK_NULL_HANDLE),"HUD queue submit failed.");vkQueueWaitIdle(queue_);
            vkDestroyBuffer(device_,vb,nullptr);vkFreeMemory(device_,vm,nullptr);
            vkFreeCommandBuffers(device_,commandPool_,1,&cmd);cmd=beginOneTime();
        }else vkCmdEndRenderPass(cmd);
    }else vkCmdEndRenderPass(cmd);
    transition(cmd,swapImages_[imageIndex],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black{{.025f,.035f,.055f,1}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,swapImages_[imageIndex],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
    double sa=double(gearTargetExtent_.width)/gearTargetExtent_.height,da=double(extent_.width)/extent_.height;int x0=0,y0=0,x1=(int)extent_.width,y1=(int)extent_.height;if(da>sa){int w=(int)(extent_.height*sa);x0=((int)extent_.width-w)/2;x1=x0+w;}else{int h=(int)(extent_.width/sa);y0=((int)extent_.height-h)/2;y1=y0+h;}
    VkImageBlit bl{};bl.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;bl.srcSubresource.layerCount=1;bl.srcOffsets[1]={(int32_t)gearTargetExtent_.width,(int32_t)gearTargetExtent_.height,1};bl.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;bl.dstSubresource.layerCount=1;bl.dstOffsets[0]={x0,y0,0};bl.dstOffsets[1]={x1,y1,1};vkCmdBlitImage(cmd,gearOffscreenColor_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,swapImages_[imageIndex],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bl,VK_FILTER_LINEAR);
    transition(cmd,swapImages_[imageIndex],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cmd);VkPipelineStageFlags wait=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.waitSemaphoreCount=1;si.pWaitSemaphores=&imageAvailable_;si.pWaitDstStageMask=&wait;si.commandBufferCount=1;si.pCommandBuffers=&cmd;si.signalSemaphoreCount=1;si.pSignalSemaphores=&renderFinished_;if(vkQueueSubmit(queue_,1,&si,VK_NULL_HANDLE)!=VK_SUCCESS){vkFreeCommandBuffers(device_,commandPool_,1,&cmd);return false;}VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&renderFinished_;pi.swapchainCount=1;pi.pSwapchains=&swapchain_;pi.pImageIndices=&imageIndex;VkResult pr=vkQueuePresentKHR(queue_,&pi);vkQueueWaitIdle(queue_);vkFreeCommandBuffers(device_,commandPool_,1,&cmd);if(pr==VK_ERROR_OUT_OF_DATE_KHR||pr==VK_SUBOPTIMAL_KHR)resized();return pr==VK_SUCCESS||pr==VK_SUBOPTIMAL_KHR;
}

double VulkanRenderer::benchmarkGearCalibration(float seconds,uint32_t targetWidth,uint32_t targetHeight,unsigned milliseconds){
    if(!device_||!gearPipeline_||!targetWidth||!targetHeight)return 0.0;
    VkExtent2D wanted{targetWidth,targetHeight};
    if(wanted.width!=gearTargetExtent_.width||wanted.height!=gearTargetExtent_.height||!gearOffscreenFramebuffer_){vkDeviceWaitIdle(device_);gearTargetExtent_=wanted;createGearFramebuffers();}
    auto record=[&](float t){
        VkCommandBuffer cmd=beginOneTime();VkClearValue clears[2]{};clears[0].color={{.025f,.035f,.055f,1}};clears[1].depthStencil={1,0};
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=gearRenderPass_;rb.framebuffer=gearOffscreenFramebuffer_;rb.renderArea.extent=gearTargetExtent_;rb.clearValueCount=2;rb.pClearValues=clears;vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gearPipeline_);VkDeviceSize off=0;vkCmdBindVertexBuffers(cmd,0,1,&gearVertexBuffer_,&off);vkCmdBindIndexBuffer(cmd,gearIndexBuffer_,0,VK_INDEX_TYPE_UINT32);
        VkViewport viewport{0,0,(float)gearTargetExtent_.width,(float)gearTargetExtent_.height,0,1};VkRect2D sc{{0,0},gearTargetExtent_};vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&sc);
        struct PC{Mat4 mvp,model;float color[4];};float orbit=t*.45f;V3 eye{5.8f*std::cos(orbit),5.8f*std::sin(orbit),4.1f};float aspect=float(gearTargetExtent_.width)/float(gearTargetExtent_.height);float fov=(aspect<1.0f)?1.02f:.90f;Mat4 vp=mul(perspective(fov,aspect,.035f,40.0f),lookAt(eye,{0.22f,0,0},{0,0,1}));
        auto part=[&](float x,float spin,float material,uint32_t count,uint32_t first){int mi=std::clamp((int)(material+0.5f),0,2);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gearPipelineLayout_,0,1,&gearDescriptorSets_[mi],0,nullptr);Mat4 model=mul(translate(x,0,0),rotZ(spin));PC pc{mul(vp,model),model,{0,0,0,material}};vkCmdPushConstants(cmd,gearPipelineLayout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(pc),&pc);vkCmdDrawIndexed(cmd,count,1,first,0,0);};
        constexpr float leftX=-.54f,rightX=1.08f,smallHalfTooth=3.14159265359f/8.0f;const float spin=t*1.35f,smallSpin=-2.0f*spin+smallHalfTooth;
        part(leftX,spin,0.0f,gearLargeIndexCount_,0);part(leftX,spin,2.0f,gearLargeSpindleIndexCount_,gearLargeSpindleFirstIndex_);part(rightX,smallSpin,1.0f,gearSmallIndexCount_,gearSmallFirstIndex_);part(rightX,smallSpin,2.0f,gearSmallSpindleIndexCount_,gearSmallSpindleFirstIndex_);
        vkCmdEndRenderPass(cmd);vkEndCommandBuffer(cmd);return cmd;
    };
    const ULONGLONG start=GetTickCount64();unsigned frames=0;
    while(GetTickCount64()-start<milliseconds){
        VkCommandBuffer cmd=record(seconds+float(frames)*0.001f);VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;
        if(vkQueueSubmit(queue_,1,&si,VK_NULL_HANDLE)!=VK_SUCCESS){vkFreeCommandBuffers(device_,commandPool_,1,&cmd);break;}
        vkQueueWaitIdle(queue_);vkFreeCommandBuffers(device_,commandPool_,1,&cmd);++frames;
    }
    const double elapsed=std::max(0.001,double(GetTickCount64()-start)/1000.0);return frames/elapsed;
}
