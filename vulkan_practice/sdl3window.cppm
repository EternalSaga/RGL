module;
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_version.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdint>
#include <fmt/format.h>
#include <stdexcept>
#include <utility>
#include <vulkan/vulkan_raii.hpp>
export module vulkan_window;

import rllogger;
namespace RGL {
export class SDLWindow {
   private:
    SDL_Window* window_;
    int width;
    int height;
	RLLogger* logger;
   public:
    SDLWindow(int width, int height, std::string title) {
	this->width = width;
	this->height = height;
	logger = RLLogger::getInstance(); 
    
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
	    logger->critical("SDL init failed");
	    throw std::runtime_error("SDL init failed");
	}

	window_ = SDL_CreateWindow(title.c_str(), width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

	if (!window_) {
	    std::string errMsg = std::string("Failed to create SDL Window: ") + SDL_GetError();
	    logger->critical(errMsg);
	    throw std::runtime_error(errMsg);
	}
    }

    std::pair<const char* const*, uint32_t> getSurfaceExtensions() {
	if (!SDL_WasInit(SDL_INIT_VIDEO)) {
	    logger->critical("SDL Video subsystem not initialized!");
	    throw std::runtime_error("SDL Video subsystem not initialized!");
	}
	uint32_t extensionCount = 0;
	const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
	if (!sdlExtensions) {
	    logger->critical("Failed to get SDL surface extensions");
	    throw std::runtime_error("Failed to get SDL surface extensions");
	}
	return std::make_pair(sdlExtensions, extensionCount);
    }

    ~SDLWindow() {
	SDL_DestroyWindow(window_);
	SDL_Quit();
    }
    vk::raii::SurfaceKHR createSurface(vk::raii::Instance& instance) {
	VkSurfaceKHR surface = VK_NULL_HANDLE;

	if (!SDL_Vulkan_CreateSurface(window_, *instance, nullptr, &surface)) {
	    const std::string errMsg = fmt::format("Failed to create Vulkan Surface: {}", SDL_GetError());
	    logger->critical(errMsg);
	    throw std::runtime_error(errMsg);
	}

	return vk::raii::SurfaceKHR(instance, surface);
    }
    inline operator SDL_Window*() { return window_; }
    inline int getWidth() const { return this->width; }
    inline int getHeight() const { return this->height; }

    vk::SurfaceFormatKHR selectFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats) {
	SDL_PropertiesID windowProps = SDL_GetWindowProperties(this->window_);
	bool isHdrEnabled = SDL_GetBooleanProperty(windowProps, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
	std::vector<vk::SurfaceFormatKHR> preferredHdrFormats = {
	    {vk::Format::eR16G16B16A16Sfloat, vk::ColorSpaceKHR::eExtendedSrgbLinearEXT},
	    {vk::Format::eA2B10G10R10UnormPack32, vk::ColorSpaceKHR::eHdr10St2084EXT}};

	std::vector<vk::SurfaceFormatKHR> preferredSdrFormats = {
	    {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
	    {vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear}};

	if (isHdrEnabled) {
    logger->info("[SDL3] HDR enabled, matching HDR surface format...");
	    for (const auto& preferred : preferredHdrFormats) {
	    for (const auto& available : availableFormats) {
		    if (available.format == preferred.format && available.colorSpace == preferred.colorSpace) {
			logger->info("[Vulkan] HDR format matched: {}", vk::to_string(available.format));
			return available;
		    }
		}
	    }
	    logger->info("[Vulkan] No HDR format found, falling back to SDR.");
	}

	// SDR fallback
	for (const auto& preferred : preferredSdrFormats) {
	    for (const auto& available : availableFormats) {
		if (available.format == preferred.format && available.colorSpace == preferred.colorSpace) {
		    return available;
		}
	    }
	}

	logger->warn("[Vulkan] 警告：未找到理想的 sRGB 格式，使用默认首选项。{}", vk::to_string(availableFormats[0].format));
	return availableFormats[0];
    }

	vk::Extent2D getSwapchainExtend(const vk::SurfaceCapabilitiesKHR& surfaceCapabilities){
		vk::Extent2D swapchainExtent = surfaceCapabilities.currentExtent;


	// Use window size when currentExtent is unspecified
	if (surfaceCapabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
		swapchainExtent = surfaceCapabilities.currentExtent;
	}else{
		int width,height;
		SDL_GetWindowSize(window_,&width,&height);
swapchainExtent.width = std::clamp(static_cast<uint32_t>(width), 
        surfaceCapabilities.minImageExtent.width, surfaceCapabilities.maxImageExtent.width);
    swapchainExtent.height = std::clamp(static_cast<uint32_t>(height), 
        surfaceCapabilities.minImageExtent.height, surfaceCapabilities.maxImageExtent.height);
	}
	return swapchainExtent;
	}

uint32_t getImageCount(const uint32_t minImageCount, const uint32_t maxImageCount) {
    SDL_PropertiesID props = SDL_GetWindowProperties(window_);
    uint32_t imageCount = minImageCount;

    // Wayland: use compositor-managed buffer count, no +1
    if (SDL_HasProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER)) {
        return imageCount; 
    }

    imageCount = imageCount + 1;

    if (maxImageCount > 0 && imageCount > maxImageCount) {
        imageCount = maxImageCount;
    }

    return imageCount;
}
};

}  // namespace RGL