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

	// ⚠ 消息当**参数**、不当格式串：VVL 的消息体自带花括号（syncval 实测含
	//   `pImageMemoryBarriers[6]: {`），而 `RLLogger::log` 里的 `fmt::runtime` 只绕开编译期
	//   检查、运行时照旧把它当格式串解析 ⟹ 抛 fmt::format_error，而异常逃出 Vulkan C 回调
	//   = std::terminate（实测：一开 syncval 引擎必崩在第一条消息，`just smoke` 只报
	//   「引擎非零退出」，真正的 hazard 永远看不到）。诊断设施不得反杀主程序。
	format_with_location fmtObj{"{}", std::source_location::current()};

	fmtObj.loc.filename = "Vulkan-Validation";
	fmtObj.loc.funcname =
		pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "UNKNOWN";
	fmtObj.loc.line = 0;

	logger->log(lvl, fmtObj, pCallbackData->pMessage);

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
 * 设备 mesh/task 能力快照 —— "这台设备到底给多少"的**唯一现读口**（★M-3T）。
 *
 * ## 为什么长在这里、为什么必须无条件打印
 * 计划文档的四机矩阵此前手抄 mesh/task 上限与 granularity，而这类数字**换机器就漂**（三机
 * 轮换 + `run-igpu/dgpu/intel` 强制换设备）。引擎侧原有的查询面也不够当现读口：
 * `maxMeshOutputVertices` 等只在 `checkMeshletDeviceFit` **不够用时**才 warn，且调用点排在
 * `wireMeshletSwitch` 的门关早退**之后** ⟹ 门关的机器根本查不到；而 task 侧上限与两个
 * granularity 全仓从未查询——偏偏它们正是"mesh 线程数能不能降到 128 / payload 有多大预算"
 * 那一类问题的判据。本行**与任何配置门无关**。
 *
 * 🔴 **口径 = `pickPhysicalDevice` 实际选中的那台设备**，不是"枚举到的某台"。独立 vkinfo 式
 * 工具在双显卡机器上会给出与引擎选路不一致的答案，所以现读口必须长在选设备这一步之后。
 *
 * 🔴 只读属性、不碰 device 创建。mesh shader 扩展缺席时各字段留 **0**（含义是"没有这个能力"，
 * 不是"上限为 0"）；读侧按 0 判不支持。
 *
 * @note 与 `checkMeshletDeviceFit` 的关系：那边是**闸门**（拿规格常量判够不够、不够就停用
 *       簇路径），这边是**读数**（不带阈值）。两者各自查一次属性，没有第二份阈值。
 */
export struct MeshTaskDeviceCaps {
	uint32_t maxMeshWorkGroupInvocations = 0;  // mesh 入口 numthreads 的硬上限
	uint32_t maxTaskWorkGroupInvocations = 0;  // task 入口 numthreads 上限（与 mesh 上限正交）
	uint32_t maxPreferredMeshWorkGroupInvocations = 0;  // 性能取向（硬上限之下的"推荐值"）
	uint32_t maxPreferredTaskWorkGroupInvocations = 0;
	uint32_t maxMeshOutputVertices = 0;  // 去索引发射的判据是 **3×** 三角形数（见 checkMeshletDeviceFit）
	uint32_t maxMeshOutputPrimitives = 0;
	uint32_t meshOutputPerVertexGranularity = 0;  // 输出声明要按它向上取整
	uint32_t meshOutputPerPrimitiveGranularity = 0;
	uint32_t maxMeshWorkGroupCountX = 0;  // 单 draw 的 groupCount 上界（MDI 行表 groupCount 判据）
	uint32_t maxTaskPayloadSize = 0;  // task→mesh payload 的字节预算（spec 名就叫这个，不是 …MemoryEXT）
};

/**
 * 查询 mesh/task 能力（纯属性读取，不碰 device 创建）。
 * @param device 待查询的物理设备（只读）
 * @return 能力快照（口径与 0 值语义见 MeshTaskDeviceCaps）
 */
export MeshTaskDeviceCaps queryMeshTaskDeviceCaps(const vk::raii::PhysicalDevice& device) {
	const auto props = device.getProperties2<vk::PhysicalDeviceProperties2,
		vk::PhysicalDeviceMeshShaderPropertiesEXT>();
	const auto& m = props.get<vk::PhysicalDeviceMeshShaderPropertiesEXT>();
	return MeshTaskDeviceCaps{
		.maxMeshWorkGroupInvocations = m.maxMeshWorkGroupInvocations,
		.maxTaskWorkGroupInvocations = m.maxTaskWorkGroupInvocations,
		.maxPreferredMeshWorkGroupInvocations = m.maxPreferredMeshWorkGroupInvocations,
		.maxPreferredTaskWorkGroupInvocations = m.maxPreferredTaskWorkGroupInvocations,
		.maxMeshOutputVertices = m.maxMeshOutputVertices,
		.maxMeshOutputPrimitives = m.maxMeshOutputPrimitives,
		.meshOutputPerVertexGranularity = m.meshOutputPerVertexGranularity,
		.meshOutputPerPrimitiveGranularity = m.meshOutputPerPrimitiveGranularity,
		.maxMeshWorkGroupCountX = m.maxMeshWorkGroupCount[0],
		.maxTaskPayloadSize = m.maxTaskPayloadSize,
	};
}

/**
 * 无条件打印一行能力快照（现读口本体）。
 *
 * 键名与 Vulkan 属性**逐字同名** ⟹ 文档只准写"现读口 = 启动日志 `Mesh/task device caps` 行"，
 * 不准再抄一遍数值。
 * @param device 引擎实际选中的物理设备
 */
export void logMeshTaskDeviceCaps(const vk::raii::PhysicalDevice& device) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	const auto c = queryMeshTaskDeviceCaps(device);
	logger->info("Mesh/task device caps: maxMeshWorkGroupInvocations={} maxTaskWorkGroupInvocations={} "
				 "maxPreferredMeshWorkGroupInvocations={} maxPreferredTaskWorkGroupInvocations={} "
				 "maxMeshOutputVertices={} maxMeshOutputPrimitives={} "
				 "meshOutputPerVertexGranularity={} meshOutputPerPrimitiveGranularity={} "
				 "maxMeshWorkGroupCountX={} maxTaskPayloadSize={}",
		c.maxMeshWorkGroupInvocations, c.maxTaskWorkGroupInvocations,
		c.maxPreferredMeshWorkGroupInvocations, c.maxPreferredTaskWorkGroupInvocations,
		c.maxMeshOutputVertices, c.maxMeshOutputPrimitives,
		c.meshOutputPerVertexGranularity, c.meshOutputPerPrimitiveGranularity,
		c.maxMeshWorkGroupCountX, c.maxTaskPayloadSize);
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
	// 设备能力现读口（口径 → MeshTaskDeviceCaps）：紧跟选设备打，**与任何配置门无关**——
	// 门关 / 簇路径停用 / 换 igpu 都得能查到，否则文档里的能力列又只能手抄。
	logMeshTaskDeviceCaps(*selectedDevice);
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

/**
 * 图外布局对账的**判定 + 值可读化**（纯函数：不发日志、不碰命令缓冲）。
 *
 * 为什么抽出来：判定与「怎么把 vk::ImageLayout 打成人才看的数」在两处护栏各写一份，
 * 而它是唯一有对错的部分——日志发射与后果说明留在调用方（两处诊断语义不同，不该并）。
 * 可测性只是副产品：判定进了纯函数才有毫秒级单测（见 `tests/test_layout_drift.cppm`），
 * 而"护栏到底会不会响"这件事在真机上只能靠故意改坏 barrier 才能验。
 *
 * @param what     漂移对象名（进消息，用于点名是哪张图/哪个输入）
 * @param expected 期望布局（渲染图追踪表里的值 / 向 vendor 申报的布局）
 * @param actual   实际布局（图外模块发出的 barrier 真实写入的值）
 * @return 一致 = `std::nullopt`；不一致 = 完整可读消息（含对象名与两个布局的数值）
 */
export std::optional<std::string> layoutDrift(std::string_view what,
	vk::ImageLayout expected, vk::ImageLayout actual) {
	if (expected == actual)
		return std::nullopt;
	// 数值而非 vk::to_string：与两处护栏改动前的口径一致（原本就打 uint32），且排查时
	// 拿这个数直接对 vulkan.h 的枚举表；单测也据此在消息里找数字而不是找整串文本。
	return fmt::format("{}布局漂移 —— 期望 = {}，实际 = {}", what,
		static_cast<uint32_t>(expected), static_cast<uint32_t>(actual));
}

}  // namespace RGL
