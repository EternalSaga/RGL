module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_vulkan.h>
#include "rllogger.hpp"
export module vk_instance;

import vulkan_utils;
import vulkan_window;

namespace RGL {

export class VkInstance {
public:
    VkInstance(RGL::SDLWindow& window)
        : context_(),
          apiVersion_(context_.enumerateInstanceVersion()),
          instance_(context_, buildInstanceCreateInfo(window)),
          debugMessenger_(RGL::setupDebugMessenger(instance_))
    {
    }

    vk::raii::Instance& getInstance() { return instance_; }
    const vk::raii::Instance& getInstance() const { return instance_; }
    uint32_t getApiVersion() const { return apiVersion_; }

private:
    vk::InstanceCreateInfo buildInstanceCreateInfo(RGL::SDLWindow& window) {
        auto [surfaceExtensions, surfaceCount] = window.getSurfaceExtensions();

        engineInfo_.applicationVersion = vk::makeVersion(0, 1, 0);
        engineInfo_.apiVersion = apiVersion_;
        engineInfo_.engineVersion = vk::makeVersion(0, 0, 1);
        engineInfo_.pEngineName = "practice";

        instanceExtensions_ = {
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
            VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
            VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME
        };
        for (uint32_t i = 0; i < surfaceCount; i++) {
            instanceExtensions_.push_back(surfaceExtensions[i]);
        }

        const auto availableLayers = context_.enumerateInstanceLayerProperties();
        if (!RGL::findMissingValidationLayser(availableLayers, layers_).empty()) {
            throw std::runtime_error("Missing validation layers!");
        }

        instanceCreateInfo_.setPApplicationInfo(&engineInfo_);
        instanceCreateInfo_.setPEnabledLayerNames(layers_);
        instanceCreateInfo_.setPEnabledExtensionNames(instanceExtensions_);
        return instanceCreateInfo_;
    }

    vk::raii::Context context_;
    uint32_t apiVersion_;
    vk::ApplicationInfo engineInfo_;
    vk::InstanceCreateInfo instanceCreateInfo_;
    std::vector<const char*> layers_ = {"VK_LAYER_KHRONOS_validation"};
    std::vector<const char*> instanceExtensions_;
    vk::raii::Instance instance_;
    vk::raii::DebugUtilsMessengerEXT debugMessenger_;
};

}
