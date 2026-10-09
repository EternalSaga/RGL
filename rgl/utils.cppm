module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <fmt/format.h>     // submitCmdBufferAndWait 的错误消息拼装
export module vulkan_utils;

import rlstd;
import rlvk;
import rllogger;
import vkerror;

namespace RGL {

/**
 * 校验层可用性检查：requiredLayers 中不在 availableLayers 里的层名。
 * @param availableLayers 驱动/系统可用层属性列表
 * @param requiredLayers 请求的层名
 * @return 缺失层名列表（空 = 全部可用）
 */
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

/**
 * VVL 调试回调（C 函数指针，经 setupDebugMessenger 注册）：severity → RLLogger
 * 级别映射（Verbose→Trace / Info→Info / Warning→Warn / Error→Error，后判覆盖先判），
 * 消息经 format_with_location 注入（filename 固定 "Vulkan-Validation"，funcname =
 * messageIdName）后落日志。恒返回 VK_FALSE（不中止应用）。
 * @param messageSeverity 消息严重级别
 * @param messageType 消息类型（未用）
 * @param pCallbackData 回调数据（取 pMessage/pMessageIdName）
 * @param pUserData 用户数据（注册时传的 RLLogger*，回调内未用——直接取单例）
 * @return VK_FALSE
 */
static VKAPI_ATTR vk::Bool32 VKAPI_CALL
vulkanDebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	[[maybe_unused]] vk::DebugUtilsMessageTypeFlagsEXT messageType,
	const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
	[[maybe_unused]] void* pUserData) {
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

/**
 * 创建调试消息器（Warning+Error × General/Validation/Performance），回调接 RLLogger。
 * @param instance Vulkan 实例
 * @return RAII DebugUtilsMessengerEXT（预期恒成功；raii 构造失败路径预留 Error）
 */
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

/**
 * 提交单条命令缓冲并等待其 fence 完成——一次性上传 / 传输的通用原语。
 *
 * 错误消息模板固定为 `"{sub}: upload submit failed: {name}"` 与
 * `"{sub}: upload fence timeout: {name}"`——`sub` 就是各调用点自己的排查前缀
 * （`"cubemap"` / `"texture"`），两个既有调用点的文案因此**逐字节不变**。
 *
 * ⚠ 只共享机制，**不统一错误策略**：本函数返回 expected；仓库里另有 throw 型
 *   （fsr_upscaler / evssm_shadow，启动期 fail-fast 契约）与忽略结果型（gpu_canary
 *   / 回读路径）的同形代码，那些**不在本次范围**，各自边界自行决定怎么处置返回值。
 *
 * @param dev   逻辑设备（建 fence + 等 fence）
 * @param cmd   已 end() 的命令缓冲
 * @param queue 提交目标队列
 * @param sub   错误消息前缀（排查抓手，如 "cubemap"）
 * @param name  资源名（拼进消息）
 */
export [[nodiscard]] std::expected<void, Error>
submitCmdBufferAndWait(vk::raii::Device& dev, vk::raii::CommandBuffer& cmd, VkQueue queue,
	std::string_view sub, const char* name) {
	auto fence = vk::raii::Fence(dev, vk::FenceCreateInfo{});
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	VkCommandBuffer cmdBuffer = *cmd;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmdBuffer;
	if (auto result = vk::Result(::vkQueueSubmit(queue, 1, &submitInfo, *fence));
		result != vk::Result::eSuccess)
		return std::unexpected(Error{ErrorCode::VulkanError,
			fmt::format("{}: upload submit failed: {}", sub, name)});
	if (dev.waitForFences(std::array<vk::Fence, 1>{*fence}, true, UINT64_MAX) != vk::Result::eSuccess)
		return std::unexpected(Error{ErrorCode::VulkanError,
			fmt::format("{}: upload fence timeout: {}", sub, name)});
	return {};
}

/** 单设备打分（core 数 ×1000 + 设备本地 GiB ×10 + 类型分）；score = 0 = 不参与。 */
uint64_t scorePhysicalDevice(const vk::raii::PhysicalDevice& device, uint32_t& coreCount,
	std::string& deviceName, uint64_t& vramBytes) {
	vk::PhysicalDeviceProperties properties = device.getProperties();
	deviceName = properties.deviceName.data();
	std::string deviceNameLower = deviceName;
	std::ranges::transform(deviceNameLower, deviceNameLower.begin(),
		[](unsigned char c) { return std::tolower(c); });

	coreCount = 0;
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
	if (coreCount == 0)
		coreCount = properties.limits.maxComputeWorkGroupInvocations;

	uint64_t score = static_cast<uint64_t>(coreCount) * 1000;

	auto memProps = device.getMemoryProperties();
	vramBytes = 0;
	for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
		if (memProps.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal)
			vramBytes += memProps.memoryHeaps[i].size;
	}
	score += (vramBytes / (1024 * 1024 * 1024)) * 10;

	if (properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
		score += 500;
	else if (properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu)
		score += 200;
	return score;
}

/** 单设备候选入表（score > 0）+ debug 日志。 */
void addDeviceCandidate(const vk::raii::PhysicalDevice& device, uint64_t score,
	uint32_t coreCount, uint64_t vramBytes, std::string deviceName,
	std::map<uint64_t, const vk::raii::PhysicalDevice*>& deviceCandidates,
	std::map<const vk::raii::PhysicalDevice*, std::string>& deviceNames, RLLogger* logger) {
	if (score == 0)
		return;
	deviceCandidates.insert({score, &device});
	deviceNames.insert({&device, deviceName});
	if (logger) {
		const auto props = device.getProperties();
		logger->debug("GPU candidate: {} (type={}, cores={}, vram={}GiB, score={})",
			deviceName, vk::to_string(props.deviceType),
			coreCount, vramBytes / (1024 * 1024 * 1024), score);
	}
}

/**
 * GPU 选择（打分制，取最高分）：score = 核心数 × 1000（AMD 经
 * ShaderCorePropertiesAMD 精确计算，其余设备回落 maxComputeWorkGroupInvocations）
 * + 设备本地显存 GiB × 10 + 设备类型（DiscreteGpu +500 / IntegratedGpu +200）。
 * score = 0 的设备不参与；全为 0 = 无合适 GPU。
 * @param physicalDevices 实例的物理设备列表
 * @return 最高分设备指针（生命周期 = 列表）；无候选返回 NoPhysicalDevices /
 *         全 0 分返回 NoSuitableGPU
 */
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
		uint32_t coreCount = 0;
		std::string deviceName;
		uint64_t vramBytes = 0;
		const uint64_t score = scorePhysicalDevice(device, coreCount, deviceName, vramBytes);
		addDeviceCandidate(device, score, coreCount, vramBytes, std::move(deviceName),
			deviceCandidates, deviceNames, logger);
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

/**
 * present 模式选择（交互窗路径）：**硬要求 eImmediate**，缺失即失败
 *（compositor/平台不兼容）。headless 表面无 eImmediate，走 vkswapchain 内
 * 的 eFifo 回落路径，不调本函数；runtime.lua present_mode = "fifo" 的窗口
 * 路径也直接在 vkswapchain::createPresentMode 选 eFifo，不调本函数
 *（批 9.3 双模式开关——本函数语义保持"eImmediate 硬要求"不变）。
 * @param presentModes surface 支持的 present 模式列表
 * @return eImmediate；列表中无 eImmediate 返回 NoPresentMode
 */
export [[nodiscard]] std::expected<vk::PresentModeKHR, Error>
selectPresentMode(const std::vector<vk::PresentModeKHR>& presentModes) {
	auto it = std::ranges::find(presentModes, vk::PresentModeKHR::eImmediate);
	if (it != presentModes.end()) {
		return *it;
	}
	return std::unexpected(Error{ErrorCode::NoPresentMode,
		"eImmediate not supported -- compositor or platform incompatible"});
}

/**
 * 构造 vk::ImageMemoryBarrier2（同队列内 layout/access 转换；queue family
 * 均 IGNORED）。render_graph::materializeBarrier 的底层物化器。
 * @param image 目标 image 句柄
 * @param oldLayout 旧布局（eUndefined = 内容丢弃）
 * @param newLayout 新布局
 * @param aspectMask 子资源 aspect（eColor / eDepth）
 * @param srcStage 源管线 stage mask
 * @param dstStage 目标管线 stage mask
 * @param srcAccess 源 access mask
 * @param dstAccess 目标 access mask
 * @param baseMipLevel 起始 mip（默认 0）
 * @param levelCount mip 数（默认 1）
 * @param baseArrayLayer 起始 array layer（默认 0）
 * @param layerCount layer 数（默认 1）
 * @return 填好的 barrier（constexpr，可编译期使用）
 */
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
