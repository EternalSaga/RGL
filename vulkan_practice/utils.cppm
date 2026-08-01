module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
export module vulkan_utils;

import rlstd;
import rllogger;

namespace RGL {

export std::vector<const char *> findMissingValidationLayers(
    const std::vector<vk::LayerProperties> &availableLayers,
    const std::vector<const char *> &requiredLayers) {
    std::vector<const char *> missing;

    for (const char *req : requiredLayers) {
        std::string_view reqView(req);

        bool found =
            std::ranges::any_of(availableLayers, [reqView](const auto &prop) {
                return std::string_view(prop.layerName.data()) == reqView;
            });

        if (!found) {
            missing.push_back(req);
        }
    }

    return missing;
}

static VKAPI_ATTR vk::Bool32 VKAPI_CALL
vulkanDebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                    vk::DebugUtilsMessageTypeFlagsEXT messageType,
                    const vk::DebugUtilsMessengerCallbackDataEXT *pCallbackData,
                    void *pUserData) {
    auto *logger = RLLogger::getInstance();
    if (!logger)
        return VK_FALSE;

    RGL::LogLevel lvl = RGL::LogLevelInfo;
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose) {
        lvl = RGL::LogLevelTrace;
    }
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo) {
        lvl = RGL::LogLevelInfo;
    }
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
        lvl = RGL::LogLevelWarn;
    }
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
        lvl = RGL::LogLevelError;
    }

    format_with_location fmtObj{pCallbackData->pMessage,
                                std::source_location::current()};

    fmtObj.loc.filename = "Vulkan-Validation";
    fmtObj.loc.funcname =
        pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "UNKNOWN";
    fmtObj.loc.line = 0;

    logger->log(lvl, fmtObj);

    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
        throw std::runtime_error(std::string("Vulkan validation error: ") + pCallbackData->pMessage);
    }

    return VK_FALSE;
}

export [[nodiscard]] vk::raii::DebugUtilsMessengerEXT
setupDebugMessenger(vk::raii::Instance &instance) {
    auto *logger = RLLogger::getInstance();

    vk::DebugUtilsMessengerCreateInfoEXT createInfo;

    createInfo.setMessageSeverity(
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);

    createInfo.setMessageType(
        vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
        vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance);

    createInfo.setPfnUserCallback(vulkanDebugCallback);

    createInfo.setPUserData(logger);

    return vk::raii::DebugUtilsMessengerEXT(instance, createInfo);
}

export const vk::raii::PhysicalDevice &
pickPhysicalDevice(const vk::raii::PhysicalDevices &physicalDevices) {
    auto logger = RLLogger::getInstance();
    if (physicalDevices.empty()) {
        logger->error("No physical devices found");
        throw std::runtime_error("No physical devices found");
    }
    std::map<uint64_t, const vk::raii::PhysicalDevice *> deviceCandidates;

    for (const auto &device : physicalDevices) {
        uint64_t score = 0;

        vk::PhysicalDeviceProperties properties = device.getProperties();
        std::string deviceName(properties.deviceName.data());
        std::ranges::transform(deviceName, deviceName.begin(),
                               [](unsigned char c) { return std::tolower(c); });

        uint32_t coreCount = 0;
        vk::PhysicalDeviceProperties2 props2 = device.getProperties2();
        vk::PhysicalDeviceShaderCorePropertiesAMD amdShaderProps;
        vk::PhysicalDeviceSubgroupSizeControlProperties subgroupProps;

        props2.pNext = &subgroupProps;
        subgroupProps.pNext = &amdShaderProps;

        if (deviceName.find("amd") != std::string::npos &&
            amdShaderProps.shaderEngineCount > 0) {
            coreCount = amdShaderProps.shaderEngineCount *
                        amdShaderProps.shaderArraysPerEngineCount *
                        amdShaderProps.computeUnitsPerShaderArray;
        } else {
            coreCount = properties.limits.maxComputeWorkGroupInvocations;
        }

        score += coreCount * 1000;

        auto memProps = device.getMemoryProperties();
        uint64_t vramBytes = 0;
        for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
            if (memProps.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
                vramBytes += memProps.memoryHeaps[i].size;
            }
        }

        score += (vramBytes / (1024 * 1024 * 1024)) * 10;

        if (properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
            score += 500;
        } else if (properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu) {
            score += 200;
        }

        if (score > 0) {
            deviceCandidates.insert({score, &device});
        }
    }

    if (deviceCandidates.empty() || deviceCandidates.rbegin()->first == 0) {
        logger->error("failed to find a suitable GPU!");
        throw std::runtime_error("failed to find a suitable GPU!");
    }

    return *(deviceCandidates.rbegin()->second);
}

export vk::PresentModeKHR
selectPresentMode(const std::vector<vk::PresentModeKHR> &presentModes) {
    auto it = std::ranges::find(presentModes, vk::PresentModeKHR::eImmediate);
    if (it != presentModes.end()) {
        return *it;
    }
    throw std::runtime_error(
        "eImmediate not supported -- compositor or platform incompatible");
}

export vk::ImageMemoryBarrier2
createImageBarrier(VkImage image, vk::ImageLayout oldLayout,
                   vk::ImageLayout newLayout, vk::ImageAspectFlags aspectMask,
                   vk::PipelineStageFlags2 srcStage,
                   vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 srcAccess,
                   vk::AccessFlags2 dstAccess, uint32_t baseMipLevel = 0,
                   uint32_t levelCount = 1, uint32_t baseArrayLayer = 0,
                   uint32_t layerCount = 1) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.image = image;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = baseMipLevel;
    barrier.subresourceRange.levelCount = levelCount;
    barrier.subresourceRange.baseArrayLayer = baseArrayLayer;
    barrier.subresourceRange.layerCount = layerCount;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcStageMask = srcStage;
    barrier.dstStageMask = dstStage;
    return barrier;
}

}  // namespace RGL
