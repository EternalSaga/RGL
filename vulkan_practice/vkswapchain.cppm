module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vector>
#include <SDL3/SDL_video.h>
export module vk_swapchain;

import vulkan_utils;
import vulkan_window;
import vk_instance;
import vk_device;

namespace RGL {

export class Swapchain {
public:
    Swapchain(RGL::DeviceContext& vkDevice, RGL::SDLWindow& window)
        : vkDevice(vkDevice),
          window(window),
          surface(window.createSurface(vkDevice.getVkInstance().getInstance())),
          format(window.selectFormat(
              vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface))),
          presentMode(RGL::selectPresentMode(
              vkDevice.getPhysicalDevice().getSurfacePresentModesKHR(*surface))),
          surfaceCapabilities(vkDevice.getPhysicalDevice().getSurfaceCapabilitiesKHR(*surface)),
          extent(window.getSwapchainExtend(surfaceCapabilities)),
          imageCount(window.getImageCount(surfaceCapabilities.minImageCount, surfaceCapabilities.maxImageCount)),
          swapchain(vkDevice.getDevice(), buildSwapchainCreateInfo()),
          swapchainImages(swapchain.getImages()),
          swapchainImageViews(createImageViews(vkDevice.getDevice(), swapchainImages, format.format))
    {
        vkDevice.getQueueManager().checkPresentAbility(surface);
    }

    const vk::raii::SurfaceKHR& getSurface() const { return surface; }
    const vk::raii::SwapchainKHR& getSwapchain() const { return swapchain; }
    const std::vector<vk::Image>& getImages() const { return swapchainImages; }
    const std::vector<vk::raii::ImageView>& getImageViews() const { return swapchainImageViews; }
    vk::Extent2D getExtent() const { return extent; }
    vk::SurfaceFormatKHR getFormat() const { return format; }
    vk::PresentModeKHR getPresentMode() const { return presentMode; }

    void recreate() {
        int width, height;
        SDL_GetWindowSize(window, &width, &height);
        vk::Extent2D newExtent = window.getSwapchainExtend(surfaceCapabilities);

        if (newExtent.width == extent.width && newExtent.height == extent.height) {
            return;
        }

        // 等待 GPU 空闲
        vkDevice.getDevice().waitIdle();

        // 销毁旧 swapchain
        swapchain = nullptr;

        // 重建
        extent = newExtent;
        imageCount = window.getImageCount(surfaceCapabilities.minImageCount,
                                            surfaceCapabilities.maxImageCount);
        swapchain = vk::raii::SwapchainKHR(vkDevice.getDevice(), buildSwapchainCreateInfo());
        swapchainImages = swapchain.getImages();
        swapchainImageViews = createImageViews(vkDevice.getDevice(), swapchainImages, format.format);
    }

private:
    vk::SwapchainCreateInfoKHR buildSwapchainCreateInfo() const {
        vk::SwapchainCreateInfoKHR swapchainCreateInfo{};
        swapchainCreateInfo.surface = *surface;
        swapchainCreateInfo.minImageCount = imageCount;
        swapchainCreateInfo.imageFormat = format.format;
        swapchainCreateInfo.imageColorSpace = format.colorSpace;
        swapchainCreateInfo.imageExtent = extent;
        swapchainCreateInfo.imageArrayLayers = 1;
        swapchainCreateInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
        swapchainCreateInfo.imageSharingMode = vk::SharingMode::eExclusive;
        swapchainCreateInfo.preTransform = surfaceCapabilities.currentTransform;
        swapchainCreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        swapchainCreateInfo.presentMode = presentMode;
        swapchainCreateInfo.clipped = VK_TRUE;
        swapchainCreateInfo.oldSwapchain = nullptr;
        return swapchainCreateInfo;
    }

    static std::vector<vk::raii::ImageView> createImageViews(vk::raii::Device& device,
                                                               const std::vector<vk::Image>& images,
                                                               vk::Format format) {
        std::vector<vk::raii::ImageView> views;
        views.reserve(images.size());
        for (const auto& image : images) {
            vk::ImageViewCreateInfo viewCreateInfo{};
            viewCreateInfo.image = image;
            viewCreateInfo.viewType = vk::ImageViewType::e2D;
            viewCreateInfo.format = format;
            viewCreateInfo.components.r = vk::ComponentSwizzle::eIdentity;
            viewCreateInfo.components.g = vk::ComponentSwizzle::eIdentity;
            viewCreateInfo.components.b = vk::ComponentSwizzle::eIdentity;
            viewCreateInfo.components.a = vk::ComponentSwizzle::eIdentity;
            viewCreateInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            viewCreateInfo.subresourceRange.levelCount = 1;
            viewCreateInfo.subresourceRange.baseArrayLayer = 0;
            viewCreateInfo.subresourceRange.layerCount = 1;
            views.emplace_back(device, viewCreateInfo);
        }
        return views;
    }

    RGL::DeviceContext& vkDevice;
    RGL::SDLWindow& window;
    vk::raii::SurfaceKHR surface;
    vk::SurfaceFormatKHR format;
    vk::PresentModeKHR presentMode;
    vk::SurfaceCapabilitiesKHR surfaceCapabilities;
    vk::Extent2D extent;
    uint32_t imageCount;
    vk::raii::SwapchainKHR swapchain;
    std::vector<vk::Image> swapchainImages;
    std::vector<vk::raii::ImageView> swapchainImageViews;
};

}
