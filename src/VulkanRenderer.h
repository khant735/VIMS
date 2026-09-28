#pragma once
#include <windows.h>
#include <vulkan/vulkan.h>
#include "Image.h"
#include <vector>

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    void initialize(HWND hwnd);
    void shutdown();
    void setImage(const ImageRGBA& rgba);
    void draw();
    void resized();
    bool ready() const { return device_ != VK_NULL_HANDLE; }
    const char* gpuName() const { return gpuName_; }

private:
    HWND hwnd_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> swapImages_;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;

    VkImage texture_ = VK_NULL_HANDLE;
    VkDeviceMemory textureMemory_ = VK_NULL_HANDLE;
    uint32_t textureW_ = 0, textureH_ = 0;
    bool resizePending_ = false;
    char gpuName_[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]{};

    void createInstance();
    void pickPhysicalDevice();
    void createDevice();
    void createSwapchain();
    void destroySwapchain();
    void createCommandResources();
    void destroyTexture();
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;
    VkCommandBuffer beginOneTime();
    void endOneTime(VkCommandBuffer cmd);
    static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                           VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                           VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
};
