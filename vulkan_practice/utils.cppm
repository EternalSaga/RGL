module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
export module vulkan_utils;

import rlstd;
import rllogger;
import vkerror;

namespace RGL {

export std::vector<const char*> findMissingValidationLayers(
	const std::vector<vk::LayerProperties>& availableLayers,
	const std::vector<const char*>& requiredLayers) {
	std::vector<const char*> missing;

	for (const char* req : requiredLayers) {
		std::string_view reqView(req);

		bool found =
			std::ranges::any_of(availableLayers, [reqView](const auto& prop) {
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
	const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData) {
	auto* logger = RLLogger::getInstance();
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

	return VK_FALSE;
}

export [[nodiscard]] std::expected<vk::raii::DebugUtilsMessengerEXT, Error>
setupDebugMessenger(vk::raii::Instance& instance) {
	auto* logger = RLLogger::getInstance();

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

	if (logger)
		logger->info("Debug messenger created");

	return vk::raii::DebugUtilsMessengerEXT(instance, createInfo);
}

export [[nodiscard]] std::expected<const vk::raii::PhysicalDevice*, Error>
pickPhysicalDevice(const vk::raii::PhysicalDevices& physicalDevices) {
	auto logger = RLLogger::getInstance();
	if (physicalDevices.empty()) {
		if (logger)
			logger->error("No physical devices found");
		return std::unexpected(Error{ErrorCode::NoPhysicalDevices, "No physical devices found"});
	}
	std::map<uint64_t, const vk::raii::PhysicalDevice*> deviceCandidates;
	std::map<const vk::raii::PhysicalDevice*, std::string> deviceNames;

	for (const auto& device : physicalDevices) {
		uint64_t score = 0;

		vk::PhysicalDeviceProperties properties = device.getProperties();
		std::string deviceName(properties.deviceName.data());
		std::string deviceNameLower = deviceName;
		std::ranges::transform(deviceNameLower, deviceNameLower.begin(),
			[](unsigned char c) { return std::tolower(c); });

		uint32_t coreCount = 0;

		if (deviceNameLower.find("amd") != std::string::npos) {
			auto props2 = device.getProperties2<
				vk::PhysicalDeviceProperties2,
				vk::PhysicalDeviceSubgroupSizeControlProperties,
				vk::PhysicalDeviceShaderCorePropertiesAMD>();
			auto& amdShaderProps = props2.get<vk::PhysicalDeviceShaderCorePropertiesAMD>();
			if (amdShaderProps.shaderEngineCount > 0) {
				coreCount = amdShaderProps.shaderEngineCount *
							amdShaderProps.shaderArraysPerEngineCount *
							amdShaderProps.computeUnitsPerShaderArray;
			}
		}

		if (coreCount == 0) {
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
			deviceNames.insert({&device, deviceName});
			if (logger)
				logger->debug("GPU candidate: {} (type={}, cores={}, vram={}GiB, score={})",
					deviceName, vk::to_string(properties.deviceType),
					coreCount, vramBytes / (1024 * 1024 * 1024), score);
		}
	}

	if (deviceCandidates.empty() || deviceCandidates.rbegin()->first == 0) {
		if (logger)
			logger->error("failed to find a suitable GPU!");
		return std::unexpected(Error{ErrorCode::NoSuitableGPU, "failed to find a suitable GPU!"});
	}

	auto [bestScore, selectedDevice] = *deviceCandidates.rbegin();
	if (logger)
		logger->info("Selected rendering GPU: {} (score={})",
			deviceNames.at(selectedDevice), bestScore);
	return selectedDevice;
}

export [[nodiscard]] std::expected<vk::PresentModeKHR, Error>
selectPresentMode(const std::vector<vk::PresentModeKHR>& presentModes) {
	auto it = std::ranges::find(presentModes, vk::PresentModeKHR::eImmediate);
	if (it != presentModes.end()) {
		return *it;
	}
	return std::unexpected(Error{ErrorCode::NoPresentMode,
		"eImmediate not supported -- compositor or platform incompatible"});
}

export constexpr vk::ImageMemoryBarrier2
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