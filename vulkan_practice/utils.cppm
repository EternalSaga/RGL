module;
#include "vulkan/vulkan.hpp"
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include <string_view>
#include <algorithm>
#include "rllogger.hpp"
#include <optional>
#include <cstdint>
export module vulkan_utils;

namespace RGL {

export std::vector<const char*> findMissingValidationLayser(
    const std::vector<vk::LayerProperties>& availableLayers,
    const std::vector<const char*>& requiredLayers)
{
    std::vector<const char*> missing;

    for (const char* req : requiredLayers) {
	std::string_view reqView(req);

	bool found = std::ranges::any_of(availableLayers, [reqView](const auto& prop) {
	    return std::string_view(prop.layerName.data()) == reqView;
	});

	if (!found) {
	    missing.push_back(req);
	}
    }

    return missing;
}

static VKAPI_ATTR vk::Bool32 VKAPI_CALL vulkanDebugCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    vk::DebugUtilsMessageTypeFlagsEXT messageType,
    const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {
    auto* logger = RLLogger::getInstance();
    if (!logger) return VK_FALSE;

    spdlog::level::level_enum lvl = spdlog::level::info;
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose) {
	lvl = spdlog::level::trace;
    }
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo) {
	lvl = spdlog::level::info;
    }
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
	lvl = spdlog::level::warn;
    }
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
	lvl = spdlog::level::err;
    }

   // Vulkan errors are runtime-generated; source_location can only point to this callback
     format_with_location fmtObj{
 	pCallbackData->pMessage,
 	std::source_location::current()
     };

     fmtObj.loc.filename = "Vulkan-Validation";
     fmtObj.loc.funcname = pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "UNKNOWN";
     fmtObj.loc.line = 0;

     logger->log(lvl, fmtObj);

     if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
     // Trigger SIGTRAP for debugger attachment on x86
#if defined(__x86_64__) || defined(__i386__)
	__asm__ volatile("int $3");
#else
	__builtin_trap();
#endif
    }

    return VK_FALSE;
}

export [[nodiscard]] vk::raii::DebugUtilsMessengerEXT 

setupDebugMessenger(vk::raii::Instance& instance) {

    auto* logger = RLLogger::getInstance();

    

    vk::DebugUtilsMessengerCreateInfoEXT createInfo;

    createInfo.setMessageSeverity(vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |

                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);

    createInfo.setMessageType(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |

                              vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |

                              vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance);

    createInfo.setPfnUserCallback(vulkanDebugCallback);

    createInfo.setPUserData(logger);


    return vk::raii::DebugUtilsMessengerEXT(instance, createInfo);

}


export const vk::raii::PhysicalDevice& pickPhysicalDevice(const vk::raii::PhysicalDevices& physicalDevices) {
    auto logger = RLLogger::getInstance();
    if (physicalDevices.empty()) {
        logger->error("No physical devices found");
        throw std::runtime_error("No physical devices found");
    }
    std::multimap<int, const vk::raii::PhysicalDevice*> deviceCandidates;

for (const auto& device : physicalDevices) {
        uint64_t score = 0;
        
        vk::PhysicalDeviceProperties properties = device.getProperties();
        std::string deviceName(properties.deviceName.data());
        // Lowercase for vendor matching
        std::transform(deviceName.begin(), deviceName.end(), deviceName.begin(), ::tolower);

        // Estimate compute unit count
        uint32_t coreCount = 0;

        vk::PhysicalDeviceProperties2 props2 = device.getProperties2();

        vk::PhysicalDeviceShaderCorePropertiesAMD amdShaderProps;
        vk::PhysicalDeviceSubgroupSizeControlProperties subgroupProps;

        // Chain properties for AMD and generic GPU detection
        props2.pNext = &subgroupProps;
        subgroupProps.pNext = &amdShaderProps;


        
        if (deviceName.find("amd") != std::string::npos && amdShaderProps.shaderEngineCount > 0) {

            coreCount = amdShaderProps.shaderEngineCount 
              * amdShaderProps.shaderArraysPerEngineCount 
              * amdShaderProps.computeUnitsPerShaderArray;
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

export vk::PresentModeKHR selectPresentMode(const std::vector<vk::PresentModeKHR>& presentModes){
    for(const auto& mode : presentModes){
        if (mode == vk::PresentModeKHR::eImmediate) {
            return mode;
        }
    }
    throw std::runtime_error("eImmediate not supported — compositor or platform incompatible");
}

export vk::ImageMemoryBarrier2 createImageBarrier(
    VkImage image,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    vk::ImageAspectFlags aspectMask,
    vk::PipelineStageFlags2 srcStage,
    vk::PipelineStageFlags2 dstStage,
    vk::AccessFlags2 srcAccess,
    vk::AccessFlags2 dstAccess,
    uint32_t baseMipLevel = 0,
    uint32_t levelCount = 1,
    uint32_t baseArrayLayer = 0,
    uint32_t layerCount = 1
) {
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
