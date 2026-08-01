module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
export module features;

import rlstd;
import vkerror;

namespace RGL {

export using FeatureChain = vk::StructureChain<
    vk::PhysicalDeviceFeatures2,
    vk::PhysicalDeviceVulkan11Features,
    vk::PhysicalDeviceVulkan12Features,
    vk::PhysicalDeviceVulkan13Features,
    vk::PhysicalDeviceVulkan14Features,
    vk::PhysicalDeviceMeshShaderFeaturesEXT,
    vk::PhysicalDeviceDescriptorHeapFeaturesEXT,
    vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR,
    vk::PhysicalDeviceShaderObjectFeaturesEXT,
    vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR,
    vk::PhysicalDeviceDescriptorBufferFeaturesEXT
>;

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

export FeatureChain buildRequestedFeatures() {
    FeatureChain chain{};

    chain.get<vk::PhysicalDeviceFeatures2>().features.robustBufferAccess = VK_TRUE;
    chain.get<vk::PhysicalDeviceFeatures2>().features.shaderInt64 = VK_TRUE;

    auto& f11 = chain.get<vk::PhysicalDeviceVulkan11Features>();
    f11.shaderDrawParameters = VK_TRUE;
    f11.multiview = VK_TRUE;
    f11.storageBuffer16BitAccess = VK_TRUE;

    auto& f12 = chain.get<vk::PhysicalDeviceVulkan12Features>();
    f12.descriptorIndexing = VK_TRUE;
    f12.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.timelineSemaphore = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;

    auto& f13 = chain.get<vk::PhysicalDeviceVulkan13Features>();
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderZeroInitializeWorkgroupMemory = VK_TRUE;

    auto& f14 = chain.get<vk::PhysicalDeviceVulkan14Features>();
    f14.pushDescriptor = VK_TRUE;
    f14.dynamicRenderingLocalRead = VK_TRUE;
    f14.maintenance5 = VK_TRUE;
    f14.maintenance6 = VK_TRUE;

    auto& fMesh = chain.get<vk::PhysicalDeviceMeshShaderFeaturesEXT>();
    fMesh.meshShader = VK_TRUE;
    fMesh.taskShader = VK_FALSE;

    auto& fShaderObject = chain.get<vk::PhysicalDeviceShaderObjectFeaturesEXT>();
    fShaderObject.shaderObject = VK_TRUE;

    auto& fDescriptorBuffer = chain.get<vk::PhysicalDeviceDescriptorBufferFeaturesEXT>();
    fDescriptorBuffer.descriptorBuffer = VK_TRUE;

    auto& fDescriptorHeap = chain.get<vk::PhysicalDeviceDescriptorHeapFeaturesEXT>();
    fDescriptorHeap.descriptorHeap = VK_TRUE;
    fDescriptorHeap.descriptorHeapCaptureReplay = VK_TRUE;

    auto& fSwapchainMaintenance = chain.get<vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR>();
    fSwapchainMaintenance.swapchainMaintenance1 = VK_TRUE;

    auto& fUntypedPointers = chain.get<vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR>();
    fUntypedPointers.shaderUntypedPointers = VK_TRUE;

    return chain;
}

export [[nodiscard]] std::expected<void, Error> validateFeatures(const vk::raii::PhysicalDevice& device, FeatureChain& requested) {
    auto supported = device.getFeatures2<
        vk::PhysicalDeviceFeatures2,
        vk::PhysicalDeviceVulkan11Features,
        vk::PhysicalDeviceVulkan12Features,
        vk::PhysicalDeviceVulkan13Features,
        vk::PhysicalDeviceVulkan14Features,
        vk::PhysicalDeviceMeshShaderFeaturesEXT,
        vk::PhysicalDeviceDescriptorHeapFeaturesEXT,
        vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR,
        vk::PhysicalDeviceShaderObjectFeaturesEXT,
        vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR,
        vk::PhysicalDeviceDescriptorBufferFeaturesEXT
    >();

    auto checkFeature = [](vk::Bool32 actual, const char* name) -> std::expected<void, Error> {
        if (!actual) {
            return std::unexpected(Error{ErrorCode::FeatureUnavailable,
                fmt::format("Required feature not supported by device: {}", name)});
        }
        return {};
    };

    if (auto result = checkFeature(supported.get<vk::PhysicalDeviceFeatures2>().features.robustBufferAccess, "robustBufferAccess");
        !result) return result;

    if (auto result = checkFeature(supported.get<vk::PhysicalDeviceFeatures2>().features.shaderInt64, "shaderInt64");
        !result) return result;

    auto& s11 = supported.get<vk::PhysicalDeviceVulkan11Features>();
    if (auto result = checkFeature(s11.shaderDrawParameters, "Vulkan11::shaderDrawParameters");
        !result) return result;
    if (auto result = checkFeature(s11.multiview, "Vulkan11::multiview");
        !result) return result;
    if (auto result = checkFeature(s11.storageBuffer16BitAccess, "Vulkan11::storageBuffer16BitAccess");
        !result) return result;

    auto& s12 = supported.get<vk::PhysicalDeviceVulkan12Features>();
    if (auto result = checkFeature(s12.descriptorIndexing, "Vulkan12::descriptorIndexing");
        !result) return result;
    if (auto result = checkFeature(s12.shaderUniformBufferArrayNonUniformIndexing, "Vulkan12::shaderUniformBufferArrayNonUniformIndexing");
        !result) return result;
    if (auto result = checkFeature(s12.descriptorBindingSampledImageUpdateAfterBind, "Vulkan12::descriptorBindingSampledImageUpdateAfterBind");
        !result) return result;
    if (auto result = checkFeature(s12.descriptorBindingPartiallyBound, "Vulkan12::descriptorBindingPartiallyBound");
        !result) return result;
    if (auto result = checkFeature(s12.scalarBlockLayout, "Vulkan12::scalarBlockLayout");
        !result) return result;
    if (auto result = checkFeature(s12.timelineSemaphore, "Vulkan12::timelineSemaphore");
        !result) return result;
    if (auto result = checkFeature(s12.bufferDeviceAddress, "Vulkan12::bufferDeviceAddress");
        !result) return result;

    auto& s13 = supported.get<vk::PhysicalDeviceVulkan13Features>();
    if (auto result = checkFeature(s13.dynamicRendering, "Vulkan13::dynamicRendering");
        !result) return result;
    if (auto result = checkFeature(s13.synchronization2, "Vulkan13::synchronization2");
        !result) return result;
    if (auto result = checkFeature(s13.shaderZeroInitializeWorkgroupMemory, "Vulkan13::shaderZeroInitializeWorkgroupMemory");
        !result) return result;

    auto& s14 = supported.get<vk::PhysicalDeviceVulkan14Features>();
    if (auto result = checkFeature(s14.pushDescriptor, "Vulkan14::pushDescriptor");
        !result) return result;
    if (auto result = checkFeature(s14.dynamicRenderingLocalRead, "Vulkan14::dynamicRenderingLocalRead");
        !result) return result;
    if (auto result = checkFeature(s14.maintenance5, "Vulkan14::maintenance5");
        !result) return result;
    if (auto result = checkFeature(s14.maintenance6, "Vulkan14::maintenance6");
        !result) return result;

    if (auto result = checkFeature(supported.get<vk::PhysicalDeviceMeshShaderFeaturesEXT>().meshShader, "MeshShaderEXT::meshShader");
        !result) return result;
    if (auto result = checkFeature(supported.get<vk::PhysicalDeviceShaderObjectFeaturesEXT>().shaderObject, "ShaderObjectEXT::shaderObject");
        !result) return result;
    if (auto result = checkFeature(supported.get<vk::PhysicalDeviceDescriptorBufferFeaturesEXT>().descriptorBuffer, "DescriptorBufferEXT::descriptorBuffer");
        !result) return result;
    if (auto result = checkFeature(supported.get<vk::PhysicalDeviceDescriptorHeapFeaturesEXT>().descriptorHeap, "DescriptorHeapEXT::descriptorHeap");
        !result) return result;
    if (auto result = checkFeature(supported.get<vk::PhysicalDeviceDescriptorHeapFeaturesEXT>().descriptorHeapCaptureReplay, "DescriptorHeapEXT::descriptorHeapCaptureReplay");
        !result) return result;
    if (auto result = checkFeature(supported.get<vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR>().swapchainMaintenance1, "SwapchainMaintenance1KHR::swapchainMaintenance1");
        !result) return result;
    if (auto result = checkFeature(supported.get<vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR>().shaderUntypedPointers, "ShaderUntypedPointersKHR::shaderUntypedPointers");
        !result) return result;

    return {};
}

}  // namespace RGL