module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
export module vk_instance;

import rlstd;
import rllogger;
import vulkan_utils;
import vulkan_window;

namespace RGL {

export class AppContext {
public:
    AppContext(RGL::SDLWindow &window)
        : context(), apiVersion(context.enumerateInstanceVersion()),
          instance(context, buildInstanceCreateInfo(window)),
          debugMessenger(RGL::setupDebugMessenger(instance)) {}

    vk::raii::Instance &getInstance() { return instance; }
    const vk::raii::Instance &getInstance() const { return instance; }
    uint32_t getApiVersion() const { return apiVersion; }

private:
    vk::InstanceCreateInfo buildInstanceCreateInfo(RGL::SDLWindow &window) {
        auto [surfaceExtensions, surfaceCount] = window.getSurfaceExtensions();

        engineInfo.applicationVersion = vk::makeVersion(0, 1, 0);
        engineInfo.apiVersion = apiVersion;
        engineInfo.engineVersion = vk::makeVersion(0, 0, 1);
        engineInfo.pEngineName = "practice";

        instanceExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                              VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
                              VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                              VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};
        std::ranges::copy_n(surfaceExtensions, surfaceCount,
                            std::back_inserter(instanceExtensions));

        const auto availableLayers = context.enumerateInstanceLayerProperties();
        if (!RGL::findMissingValidationLayers(availableLayers, layers).empty()) {
            throw std::runtime_error("Missing validation layers!");
        }

        instanceCreateInfo.setPApplicationInfo(&engineInfo);
        instanceCreateInfo.setPEnabledLayerNames(layers);
        instanceCreateInfo.setPEnabledExtensionNames(instanceExtensions);
        return instanceCreateInfo;
    }

    vk::raii::Context context;
    uint32_t apiVersion;
    vk::ApplicationInfo engineInfo;
    vk::InstanceCreateInfo instanceCreateInfo;
    std::vector<const char *> layers = {"VK_LAYER_KHRONOS_validation"};
    std::vector<const char *> instanceExtensions;
    vk::raii::Instance instance;
    vk::raii::DebugUtilsMessengerEXT debugMessenger;
};

}  // namespace RGL
