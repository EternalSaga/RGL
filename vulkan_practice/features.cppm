module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
export module features;

import rlstd;
import rllogger;
import vkerror;
import features_generated;

export import features_generated;

namespace RGL {

export [[nodiscard]] std::expected<void, Error> checkDeviceExtensions(const vk::raii::PhysicalDevice& device) {
    auto available = device.enumerateDeviceExtensionProperties();
    std::vector<const char*> required = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME,
        VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME
    };

    std::vector<const char*> missing;
    for (const char* ext : required) {
        bool found = std::ranges::any_of(available, [ext](const auto& prop) {
            return std::string_view(prop.extensionName.data()) == ext;
        });
        if (!found) {
            missing.push_back(ext);
        }
    }

    if (!missing.empty()) {
        std::string msg = fmt::format("Required extension not supported by device: {}",
                                      fmt::join(missing, ", "));
        return std::unexpected(Error{ErrorCode::ExtensionUnavailable, msg});
    }
    return {};
}

export const std::vector<const char*>& getDeviceExtensions() {
    static const std::vector<const char*> extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME,
        VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME
    };
    return extensions;
}

}  // namespace RGL
