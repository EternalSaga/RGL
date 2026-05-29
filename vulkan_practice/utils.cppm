module;
#include "vulkan/vulkan.hpp"
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include <string_view>
#include <algorithm>  // 为了 std::ranges::any_of
#include "rllogger.hpp"
#include <optional>
#include <cstdint>
export module vulkan_utils;

export std::vector<const char*> findMissingValidationLayser(
    const std::vector<vk::LayerProperties>& availableLayers,
    const std::vector<const char*>& requiredLayers)  // 采用引用传参，避免不必要的拷贝
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
    auto* logger = RGL::RLLogger::getInstance();
    if (!logger) return VK_FALSE;

    // 1. 映射 Vulkan 的严重程度到 spdlog 的级别

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

    // 2. 💡 避坑核心：因为 Vulkan 报错是运行时动态生成的，无法走 std::source_location 的编译期隐式构造
    // 我们必须手动构造你的 format_with_location 结构体
    RGL::format_with_location fmtObj{
	pCallbackData->pMessage,	 // 具体的报错内容
	std::source_location::current()	 // 这里的物理位置只能拿到当前回调函数的位置
    };

    // 3. 手动魔改 spdlog 的位置源（让 spdlog 打印出 "[Vulkan-Validation]" 而不是当前文件名）
    fmtObj.loc.filename = "Vulkan-Validation";
    fmtObj.loc.funcname = pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "UNKNOWN";
    fmtObj.loc.line = 0;

    // 4. 扔进你的日志系统
    logger->log(lvl, fmtObj);

    // 5. 如果是硬核调试，遇到了 ERROR 级别，直接死给你看（拉起 GDB）
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
// Linux/Gentoo 下最干净的断点触发，不污染调用栈
#if defined(__x86_64__) || defined(__i386__)
	__asm__ volatile("int $3");
#else
	__builtin_trap();
#endif
    }

    return VK_FALSE;
}

export void setupDebugMessenger(vk::raii::Instance& instance) {
    auto* logger = RGL::RLLogger::getInstance();
    vk::DebugUtilsMessengerCreateInfoEXT createInfo;
    createInfo.setMessageSeverity(vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
				  vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);
    createInfo.setMessageType(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
			      vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
			      vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance);
    createInfo.setPfnUserCallback(vulkanDebugCallback);	 // 👈 专门的 Setter
    createInfo.setPUserData(logger);
    vk::DebugUtilsMessageTypeFlagsEXT::BitsType a;
    // RAII 自动创建，销毁时自动调用销毁逻辑
    vk::raii::DebugUtilsMessengerEXT debugMessenger(instance, createInfo);
}


export const vk::raii::PhysicalDevice& pickPhysicalDevice(const vk::raii::PhysicalDevices& physicalDevices) {
    auto logger = RGL::RLLogger::getInstance();
    if (physicalDevices.empty()) {
        logger->error("No physical devices found");
        throw std::runtime_error("No physical devices found");
    }
    std::multimap<int, const vk::raii::PhysicalDevice*> deviceCandidates;

for (const auto& device : physicalDevices) {
        uint64_t score = 0;
        
        vk::PhysicalDeviceProperties properties = device.getProperties();
        std::string deviceName(properties.deviceName.data());
        // 转成小写方便匹配
        std::transform(deviceName.begin(), deviceName.end(), deviceName.begin(), ::tolower);

        // --- 1. 获取核心/执行单元数量 (真正的算力指标) ---
        uint32_t coreCount = 0;

        // 链式结构获取 Vulkan 1.1 / 1.2 的高级属性
        vk::PhysicalDeviceProperties2 props2;
        
        // 方案 A：针对 AMD，获取其 CU (Compute Units) 数量
        vk::PhysicalDeviceShaderCorePropertiesAMD amdShaderProps;
        
        // 方案 B：针对 Intel / NVIDIA，通过 Subgroup/EU 估算算力
        vk::PhysicalDeviceSubgroupSizeControlProperties subgroupProps;

        // 挂载结构体链
        props2.pNext = &subgroupProps;
        subgroupProps.pNext = &amdShaderProps;
        
        // 使用 RAII 风格获取扩展属性
        device.getProperties2(&props2);

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

export struct Graphics_Compute_Transfer_Indices{
    std::optional<uint32_t> graphicsFamilyIndex;
    std::optional<uint32_t> computeFamilyIndex;
    std::optional<uint32_t> transferFamilyIndex;
};

export Graphics_Compute_Transfer_Indices getGraphicsComputeTransferQueueFamilyIndices(const std::vector<vk::QueueFamilyProperties>& queueFamilyProps){
    Graphics_Compute_Transfer_Indices indices{};
    for (uint32_t i = 0; i < queueFamilyProps.size(); i++) {
        if (queueFamilyProps[i].queueFlags & vk::QueueFlags::BitsType::eGraphics){
            indices.graphicsFamilyIndex = i;
        }
        if (queueFamilyProps[i].queueFlags & vk::QueueFlags::BitsType::eCompute){
            indices.computeFamilyIndex = i;
        }
        if (queueFamilyProps[i].queueFlags & vk::QueueFlags::BitsType::eTransfer){
            indices.transferFamilyIndex = i;
        }
    }
    return indices;
}