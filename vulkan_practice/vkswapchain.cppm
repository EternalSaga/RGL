module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <SDL3/SDL_video.h>
export module vk_swapchain;

import rlstd;
import rllogger;
import vulkan_utils;
import vulkan_window;
import vk_instance;
import vk_device;
import vkerror;

namespace RGL {

export class Swapchain {
   public:
	// headless：SDL 窗口可为空，surface 经 vkCreateHeadlessSurfaceEXT 创建（脱离
	// compositor）；extent = 指定 width/height，format 固定 B8G8R8A8Srgb，imageCount =
	// minImageCount（无窗口属性可参考）。非 headless 时行为不变（window 必非空）
	Swapchain(RGL::DeviceContext& vkDevice, RGL::SDLWindow* window, bool headless,
		uint32_t headlessWidth = 0, uint32_t headlessHeight = 0)
		: vkDevice(vkDevice),
		  window(window),
		  surface(headless ? createHeadlessSurface(vkDevice.getVkInstance().getInstance())
		                   : createSurface(vkDevice.getVkInstance().getInstance(), *window)),
		  format(headless ? selectHeadlessFormat(vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface))
		                  : window->selectFormat(vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface))),
		  presentMode(headless ? selectHeadlessPresentMode(vkDevice.getPhysicalDevice().getSurfacePresentModesKHR(*surface))
		                      : createPresentMode(vkDevice, surface)),
		  surfaceCapabilities(vkDevice.getPhysicalDevice().getSurfaceCapabilitiesKHR(*surface)),
		  extent(headless ? vk::Extent2D{headlessWidth, headlessHeight}
		                 : window->getSwapchainExtend(surfaceCapabilities)),
		  imageCount(headless ? surfaceCapabilities.minImageCount
		                     : window->getImageCount(surfaceCapabilities.minImageCount, surfaceCapabilities.maxImageCount)),
		  swapchain(vkDevice.getDevice(), buildSwapchainCreateInfo()),
		  swapchainImages(swapchain.getImages()),
		  swapchainImageViews(createImageViews(vkDevice.getDevice(), swapchainImages, format.format)) {
		auto result = vkDevice.getQueueManager().checkPresentAbility(surface);
		if (!result) {
			throw std::runtime_error(result.error().describe());
		}
	}

	const vk::raii::SurfaceKHR& getSurface() const { return surface; }
	const vk::raii::SwapchainKHR& getSwapchain() const { return swapchain; }
	const std::vector<vk::Image>& getImages() const { return swapchainImages; }
	const std::vector<vk::raii::ImageView>& getImageViews() const { return swapchainImageViews; }
	vk::Extent2D getExtent() const { return extent; }
	vk::SurfaceFormatKHR getFormat() const { return format; }
	vk::PresentModeKHR getPresentMode() const { return presentMode; }

	// 重查 caps/format/presentMode 后按需重建。仅按 extent 判断（旧实现）会漏掉
	// format/颜色空间/transform 漂移（如 HDR 状态切换）：acquire OUT_OF_DATE 后 extent 未变
	// 时静默 no-op → 每帧再失败的死循环。返回 false = 全部未变（重建无益），调用方
	// 负责观测（日志）且不得重建深度图等附属资源。
	[[nodiscard]] bool recreate() {
		auto newCaps = vkDevice.getPhysicalDevice().getSurfaceCapabilitiesKHR(*surface);
		auto newFormat = window->selectFormat(
			vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface));
		auto newPresentMode = createPresentMode(vkDevice, surface);
		const vk::Extent2D newExtent = window->getSwapchainExtend(newCaps);

		const bool changed =
			newExtent.width != extent.width || newExtent.height != extent.height ||
			newFormat.format != format.format || newFormat.colorSpace != format.colorSpace ||
			newPresentMode != presentMode || newCaps.currentTransform != surfaceCapabilities.currentTransform;
		if (!changed) {
			return false;
		}

		// 等待 GPU 空闲
		vkDevice.getDevice().waitIdle();

		// 销毁旧 swapchain
		swapchain = nullptr;

		// 重建（按重查值，非构造期缓存）
		surfaceCapabilities = newCaps;
		format = newFormat;
		presentMode = newPresentMode;
		extent = newExtent;
		imageCount = window->getImageCount(surfaceCapabilities.minImageCount,
			surfaceCapabilities.maxImageCount);
		swapchain = vk::raii::SwapchainKHR(vkDevice.getDevice(), buildSwapchainCreateInfo());
		swapchainImages = swapchain.getImages();
		swapchainImageViews = createImageViews(vkDevice.getDevice(), swapchainImages, format.format);
		return true;
	}

   private:
	static vk::raii::SurfaceKHR createSurface(vk::raii::Instance& instance, RGL::SDLWindow& window) {
		auto result = window.createSurface(instance);
		if (!result) throw std::runtime_error(result.error().describe());
		return std::move(*result);
	}

	// headless surface（VK_EXT_headless_surface）：脱离 compositor 创建虚拟 surface。
	// v357 的 VkHeadlessSurfaceCreateInfoEXT 仅 sType/pNext/flags（无尺寸/格式——尺寸由
	// swapchain 的 imageExtent 决定，见 buildSwapchainCreateInfo 的 extent）。present 为
	// no-op（无真实显示），swapchain image 可回读（离屏渲染/CI 截图）
	static vk::raii::SurfaceKHR createHeadlessSurface(vk::raii::Instance& instance) {
		VkHeadlessSurfaceCreateInfoEXT createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		if (vkCreateHeadlessSurfaceEXT(*instance, &createInfo, nullptr, &surface) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create headless surface (vkCreateHeadlessSurfaceEXT)");
		}
		if (auto* logger = RLLogger::getInstance())
			logger->info("Headless surface created (VK_EXT_headless_surface)");
		return vk::raii::SurfaceKHR(instance, surface);
	}

	// headless 格式选择：优先 B8G8R8A8Srgb（SDR 8-bit，离屏渲染/截图足够），回落首个可用
	static vk::SurfaceFormatKHR selectHeadlessFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats) {
		for (const auto& available : availableFormats) {
			if (available.format == vk::Format::eB8G8R8A8Srgb && available.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
				return available;
			}
		}
		return availableFormats[0];
	}

	// headless present mode：present 为 no-op（无真实显示），任何模式都行。优先 eFIFO
	//（最通用），回落首个可用——不走 selectPresentMode 的 eImmediate 硬要求（headless 无此模式）
	static vk::PresentModeKHR selectHeadlessPresentMode(const std::vector<vk::PresentModeKHR>& presentModes) {
		auto it = std::ranges::find(presentModes, vk::PresentModeKHR::eFifo);
		if (it != presentModes.end()) {
			return *it;
		}
		return presentModes[0];
	}

	static vk::PresentModeKHR createPresentMode(RGL::DeviceContext& vkDevice, const vk::raii::SurfaceKHR& surface) {
		auto modes = vkDevice.getPhysicalDevice().getSurfacePresentModesKHR(*surface);
		auto modeResult = RGL::selectPresentMode(modes);
		if (!modeResult) throw std::runtime_error(modeResult.error().describe());
		return *modeResult;
	}

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
	RGL::SDLWindow* window;  // headless 时为空（surface 经 headless 扩展创建）
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

}  // namespace RGL
