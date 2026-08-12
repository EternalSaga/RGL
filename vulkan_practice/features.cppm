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

export inline bool isNvidiaGPU(const vk::raii::PhysicalDevice& device) {
    return device.getProperties().vendorID == 0x10DE;
}

export inline bool hasDiagnosticCheckpoints(const vk::raii::PhysicalDevice& device) {
    if (!isNvidiaGPU(device)) return false;
    auto available = device.enumerateDeviceExtensionProperties();
    return std::ranges::any_of(available, [](const auto& prop) {
        return std::string_view(prop.extensionName.data()) == VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    });
}

export inline bool hasDeviceFault(const vk::raii::PhysicalDevice& device) {
    if (!isNvidiaGPU(device)) return false;
    auto available = device.enumerateDeviceExtensionProperties();
    return std::ranges::any_of(available, [](const auto& prop) {
        return std::string_view(prop.extensionName.data()) == VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    });
}

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

inline bool supportsCheckpoints_ = false;
inline bool supportsFault_ = false;

export inline bool supportsDiagnosticCheckpoints() { return supportsCheckpoints_; }
export inline bool supportsDeviceFault() { return supportsFault_; }

export void updateDebugExtensionSupport(const vk::raii::PhysicalDevice& device) {
    supportsCheckpoints_ = hasDiagnosticCheckpoints(device);
    supportsFault_ = hasDeviceFault(device);
}

export std::vector<const char*> getDeviceExtensions() {
    std::vector<const char*> extensions = {
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
    if (supportsCheckpoints_) {
        extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    }
    if (supportsFault_) {
        extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    }
    return extensions;
}

}  // namespace RGL
