module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vector>
export module vk_swapchain;

import vulkan_utils;
import vulkan_window;
import vk_instance;
import vk_device;

namespace RGL {

export class VkSwapchain {
public:
    VkSwapchain(RGL::VkDevice& vkDevice, RGL::SDLWindow& window)
        : vkDevice_(vkDevice),
          window_(window),
          surface_(window_.createSurface(vkDevice_.getVkInstance().getInstance())),
          format_(window_.selectFormat(
              vkDevice_.getPhysicalDevice().getSurfaceFormatsKHR(*surface_))),
          presentMode_(RGL::selectPresentMode(
              vkDevice_.getPhysicalDevice().getSurfacePresentModesKHR(*surface_))),
          surfaceCapabilities_(vkDevice_.getPhysicalDevice().getSurfaceCapabilitiesKHR(*surface_)),
          extent_(window_.getSwapchainExtend(surfaceCapabilities_)),
          imageCount_(window_.getImageCount(surfaceCapabilities_.minImageCount, surfaceCapabilities_.maxImageCount)),
          swapchain_(vkDevice_.getDevice(), buildSwapchainCreateInfo()),
          swapchainImages_(swapchain_.getImages()),
          swapchainImageViews_(createImageViews(vkDevice_.getDevice(), swapchainImages_, format_.format))
    {
        vkDevice_.getQueueManager().checkPresentAbility(surface_);
    }

    const vk::raii::SurfaceKHR& getSurface() const { return surface_; }
    const vk::raii::SwapchainKHR& getSwapchain() const { return swapchain_; }
    const std::vector<vk::Image>& getImages() const { return swapchainImages_; }
    const std::vector<vk::raii::ImageView>& getImageViews() const { return swapchainImageViews_; }
    vk::Extent2D getExtent() const { return extent_; }
    vk::SurfaceFormatKHR getFormat() const { return format_; }
    vk::PresentModeKHR getPresentMode() const { return presentMode_; }

private:
    vk::SwapchainCreateInfoKHR buildSwapchainCreateInfo() const {
        vk::SwapchainCreateInfoKHR swapchainCreateInfo{};
        swapchainCreateInfo.surface = *surface_;
        swapchainCreateInfo.minImageCount = imageCount_;
        swapchainCreateInfo.imageFormat = format_.format;
        swapchainCreateInfo.imageColorSpace = format_.colorSpace;
        swapchainCreateInfo.imageExtent = extent_;
        swapchainCreateInfo.imageArrayLayers = 1;
        swapchainCreateInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
        swapchainCreateInfo.imageSharingMode = vk::SharingMode::eExclusive;
        swapchainCreateInfo.preTransform = surfaceCapabilities_.currentTransform;
        swapchainCreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        swapchainCreateInfo.presentMode = presentMode_;
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
            viewCreateInfo.subresourceRange.baseMipLevel = 0;
            viewCreateInfo.subresourceRange.levelCount = 1;
            viewCreateInfo.subresourceRange.baseArrayLayer = 0;
            viewCreateInfo.subresourceRange.layerCount = 1;
            views.emplace_back(device, viewCreateInfo);
        }
        return views;
    }

    RGL::VkDevice& vkDevice_;
    RGL::SDLWindow& window_;
    vk::raii::SurfaceKHR surface_;
    vk::SurfaceFormatKHR format_;
    vk::PresentModeKHR presentMode_;
    vk::SurfaceCapabilitiesKHR surfaceCapabilities_;
    vk::Extent2D extent_;
    uint32_t imageCount_;
    vk::raii::SwapchainKHR swapchain_;
    std::vector<vk::Image> swapchainImages_;
    std::vector<vk::raii::ImageView> swapchainImageViews_;
};

}
