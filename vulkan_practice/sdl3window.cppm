module;
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_version.h>
#include <SDL3/SDL_vulkan.h>
#include <fmt/format.h>
#include <vulkan/vulkan_raii.hpp>
export module vulkan_window;

import rlstd;
import rllogger;
import vkerror;

namespace RGL {

export class SDLWindow {
public:
    SDLWindow(SDL_Window* window, int width, int height)
        : window(window), width(width), height(height), logger(RLLogger::getInstance()) {}

    ~SDLWindow() {
        if (window) {
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        }
    }

    SDLWindow(const SDLWindow&) = delete;
    SDLWindow& operator=(const SDLWindow&) = delete;

    SDLWindow(SDLWindow&& o) noexcept
        : window(o.window), width(o.width), height(o.height), logger(o.logger) {
        o.window = nullptr;
    }

    SDLWindow& operator=(SDLWindow&& o) noexcept {
        if (this != &o) {
            if (window) {
                SDL_DestroyWindow(window);
                SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
            }
            window = o.window;
            width = o.width;
            height = o.height;
            logger = o.logger;
            o.window = nullptr;
        }
        return *this;
    }

    [[nodiscard]] static std::expected<SDLWindow, Error>
    create(int width, int height, std::string title) {
        auto logger = RLLogger::getInstance();

        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            std::string errMsg = std::string("SDL init failed: ") + SDL_GetError();
            if (logger)
                logger->critical(errMsg);
            return std::unexpected(Error{ErrorCode::SDLError, errMsg});
        }

        SDL_Window* wnd = SDL_CreateWindow(title.c_str(), width, height,
                                           SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

        if (!wnd) {
            std::string errMsg = std::string("Failed to create SDL Window: ") + SDL_GetError();
            if (logger)
                logger->critical(errMsg);
            SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
            return std::unexpected(Error{ErrorCode::SDLError, errMsg});
        }

        if (logger)
            logger->info("SDL window created ({}x{})", width, height);

        return SDLWindow{ wnd, width, height };
    }

    [[nodiscard]] std::expected<vk::raii::SurfaceKHR, Error> createSurface(vk::raii::Instance& instance) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;

        if (!SDL_Vulkan_CreateSurface(window, *instance, nullptr, &surface)) {
            const std::string errMsg = fmt::format("Failed to create Vulkan Surface: {}", SDL_GetError());
            if (logger)
                logger->critical(errMsg);
            return std::unexpected(Error{ErrorCode::VulkanError, errMsg});
        }

        if (logger)
            logger->info("Vulkan surface created");

        return vk::raii::SurfaceKHR(instance, surface);
    }

    [[nodiscard]] std::expected<std::pair<const char* const*, uint32_t>, Error> getSurfaceExtensions() {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) {
            if (logger)
                logger->critical("SDL Video subsystem not initialized!");
            return std::unexpected(Error{ErrorCode::SDLError, "SDL Video subsystem not initialized!"});
        }
        uint32_t extensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        if (!sdlExtensions) {
            if (logger)
                logger->critical("Failed to get SDL surface extensions");
            return std::unexpected(Error{ErrorCode::SDLError, "Failed to get SDL surface extensions"});
        }
        return std::make_pair(sdlExtensions, extensionCount);
    }

    inline operator SDL_Window*() { return window; }
    inline int getWidth() const { return width; }
    inline int getHeight() const { return height; }

    vk::SurfaceFormatKHR selectFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats) {
        SDL_PropertiesID windowProps = SDL_GetWindowProperties(window);
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

        for (const auto& preferred : preferredSdrFormats) {
            for (const auto& available : availableFormats) {
                if (available.format == preferred.format && available.colorSpace == preferred.colorSpace) {
                    return available;
                }
            }
        }

        logger->warn("[Vulkan] Warning: no ideal sRGB format found, using default {}",
                     vk::to_string(availableFormats[0].format));
        return availableFormats[0];
    }

    vk::Extent2D getSwapchainExtend(const vk::SurfaceCapabilitiesKHR& surfaceCapabilities) const {
        vk::Extent2D swapchainExtent = surfaceCapabilities.currentExtent;

        if (surfaceCapabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            swapchainExtent = surfaceCapabilities.currentExtent;
        } else {
            int w, h;
            SDL_GetWindowSize(window, &w, &h);
            swapchainExtent.width = std::clamp(static_cast<uint32_t>(w),
                surfaceCapabilities.minImageExtent.width,
                surfaceCapabilities.maxImageExtent.width);
            swapchainExtent.height = std::clamp(static_cast<uint32_t>(h),
                surfaceCapabilities.minImageExtent.height,
                surfaceCapabilities.maxImageExtent.height);
        }
        return swapchainExtent;
    }

    uint32_t getImageCount(uint32_t minImageCount, uint32_t maxImageCount) const {
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        uint32_t imageCount = minImageCount;

        if (SDL_HasProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER)) {
            return imageCount;
        }

        imageCount = imageCount + 1;

        if (maxImageCount > 0 && imageCount > maxImageCount) {
            imageCount = maxImageCount;
        }

        return imageCount;
    }

private:
    SDL_Window* window = nullptr;
    int width;
    int height;
    RLLogger* logger;
};

}  // namespace RGL