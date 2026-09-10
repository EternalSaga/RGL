module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
export module vk_device;

import rlstd;
import rlvk;
import rllogger;
import vulkan_utils;
import queue_family_manager;
import features;
import vk_instance;
import vkerror;

namespace RGL {

/**
 * DGC（VK_EXT_device_generated_commands）能力位（DeviceContext 构造期自
 * VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT.supportedIndirectCommandsShaderStagesShaderBinding
 * 派生；culling 子系统据此选路——Phase 7b，见 docs/device_generated_commands_research.md）。
 * @note 本机 RADV（Strix Halo 26.2.1）实测 binding 子集仅 COMPUTE——graphics 侧 execution set
 * /mesh draw 序列不可用，走读回重放回退（phase7_devlog 批次 5）；NVIDIA 全支持（含 MESH/FRAGMENT）。
 */
export struct DgcSupport {
	bool graphicsIndirect = false;  ///< binding 子集含 MESH|FRAGMENT：execution set 逐序列换 shader + DRAW_MESH_TASKS 序列（DGC 全路径）
	bool computeIndirect = false;   ///< binding 子集含 COMPUTE：DISPATCH 序列
	uint32_t maxIndirectShaderObjectCount = 0;  ///< execution set 容量上限（DgcCommands 构造期校验）
};

/**
 * Vulkan 设备层（main.cpp 初始化层构造顺序第 2 站：AppContext→DeviceContext→
 * Swapchain→Resources→ShaderRegistry→Renderer，见 main.cpp）：pickPhysicalDevice
 * 打分选出 PhysicalDevice（见 vulkan_utils）→ QueueFamilyManager 按角色
 * （Graphics/Compute/Transfer）各选一个 family + 一个物理队列下标 → 探测
 * optional feature 支持位 → 组装 required feature 链并按支持位条件链接
 * optional 结构进链尾 → 前置校验扩展/feature → 创建 Device + 三队列 +
 * 三命令池。
 * **成员生命周期规则**（AGENTS.md"重要成员生命周期规则"）：
 * ① featuresChain 必须为类成员——createDevice 的 DeviceCreateInfo.setPNext
 * 存链头指针（非拷贝），pNext 不能悬空；
 * ② eds3Feature / depthClipBaseFeature / robustness2Feature 必须为类成员——
 * createDevice 条件链接它们的 pNext 进 required 链尾，成员保活（生命周期与
 * DeviceContext 一致）避免悬垂；
 * ③ raii 对象（physicalDevices / device / 三个 CommandPool）必须经成员
 * 初始化列表初始化，绝不在函数体内初始化；
 * ④ 成员声明顺序 = 初始化依赖顺序（vkInstance→physicalDevices→physicalDevice→
 * dgcSupport→queueManager→optionalFeatures→featuresChain→三 optional 结构→checked→
 * device→队列→命令池；checked 是纯顺序锚定成员，见其行内注释）。
 */
export class DeviceContext {
   public:
	/**
	 * 构建设备层（全部逻辑在成员初始化列表，按成员声明序）：
	 * vkInstance → physicalDevices → physicalDevice（pickPhysicalDevice 打分选择）→
	 * dgcSupport（DGC 能力位派生）→ queueManager → optionalFeatures（探测）→ featuresChain（required 链）→
	 * eds3Feature → depthClipBaseFeature → robustness2Feature（三个 optional 结构，
	 * 按支持位初始化）→ checked（校验前移）→ device → 三队列
	 * （graphics/compute/transfer）→ 三命令池（graphics/compute =
	 * eResetCommandBuffer，transfer = eTransient | eResetCommandBuffer）。
	 * @param vkInstance 实例层（读 raii Instance 枚举物理设备）
	 * @note 失败路径：pickPhysicalDevice 失败 / precheckDevice 扩展或 feature 缺失抛
	 *   std::runtime_error（describe() 带 [Code]）；raii Device / CommandPool 构造
	 *   失败抛 vk::SystemError（createCommandPool 的 expected Error 分支是预留路径，
	 *   不可达）；queueManager 与三个 getQueue 的 .value() 安全（create 当前恒成功；
	 *   三角色恒被 QueueFamilyManager 回落分配，QueueNotAllocated 不可达）
	 */
	DeviceContext(RGL::AppContext& vkInstance)
		: vkInstance(vkInstance),
		  physicalDevices(vkInstance.getInstance()),
	  physicalDevice([result = RGL::pickPhysicalDevice(physicalDevices)]() {
		  if (!result) throw std::runtime_error(result.error().describe());
		  return *result;
	  }()),
	  /** DGC 能力位（physical device properties 查询，先于 device 创建；culling 选路消费）。 */
	  dgcSupport(queryDgcSupport(*physicalDevice)),
	  queueManager(RGL::QueueFamilyManager::create(*physicalDevice).value()),
	  /** 可选 feature 探测（先于 featuresChain/eds3Feature——后者按支持位初始化）。 */
	  optionalFeatures(RGL::queryOptionalFeatures(*physicalDevice)),
	  /** required feature 链（恒 TRUE）。optional 结构**不进本链**（spec：vkCreateDevice pNext
	   * 链只含"已启用扩展"的 feature 结构；FALSE 结构挂未启用扩展 = UB，RADV 实测启动后
	   * 首次分配即崩）。 */
	  featuresChain(RGL::buildRequestedFeatures()),
	  /** optional feature 结构（成员 = 生命周期与 DeviceContext 一致；createDevice 按支持位
	   * 条件链接到 required 链尾，未启用则不链接——与 getDeviceExtensions 的扩展启用同源
	   * 一致）。 */
	  eds3Feature(initOptionalEds3Feature(optionalFeatures)),
	  /** VUID-vkCmdSetDepthClipEnableEXT-depthClipEnable-07451：edS3 只是把旧扩展
	   * VK_EXT_depth_clip_enable 的 depth clip 状态动态化，调用该命令仍要求旧扩展的
	   * base feature 同时启用（2026-09-02 深挖推翻"VVL 误报"旧结论——见 调试历史）。 */
	  depthClipBaseFeature(initOptionalDepthClipBaseFeature(optionalFeatures)),
	  /** robustness2（越界访问规范化 + null 描述符）：两支持位共用一个 feature 结构，
	   * 任一位可用即结构进链（字段按各自支持位置位）；不可用 = 不链接，no-op 降级。 */
	  robustness2Feature(initOptionalRobustness2Feature(optionalFeatures)),
	  /** 校验前移：必须在 raii::Device 构造（device 成员）之前——feature/extension
	   * 不支持时 raii 构造器先抛 vk::SystemError（不透明），自定义 Error 路径不可达
	   *（旧实现在构造体校验 = 失败路径死代码）。本成员只为钉死初始化顺序。 */
	  checked(precheckDevice(*physicalDevice, featuresChain)),
	  device(createDevice(*physicalDevice, queueManager, featuresChain, optionalFeatures, eds3Feature, depthClipBaseFeature, robustness2Feature)),
		  graphicsQueue(queueManager.getQueue(device, RGL::QueueFamilyRole::Graphics).value()),
		  computeQueue(queueManager.getQueue(device, RGL::QueueFamilyRole::Compute).value()),
		  transferQueue(queueManager.getQueue(device, RGL::QueueFamilyRole::Transfer).value()),
		  graphicsCommandPool(createCommandPool(device, queueManager.getFamilyIndex(RGL::QueueFamilyRole::Graphics),
			  vk::CommandPoolCreateFlagBits::eResetCommandBuffer)
				  .value()),
		  computeCommandPool(createCommandPool(device, queueManager.getFamilyIndex(RGL::QueueFamilyRole::Compute),
			  vk::CommandPoolCreateFlagBits::eResetCommandBuffer)
				  .value()),
		  transferCommandPool(createCommandPool(device, queueManager.getFamilyIndex(RGL::QueueFamilyRole::Transfer),
			  vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer)
				  .value()) {
		if (auto logger = RLLogger::getInstance()) {
			logger->info("Device DGC support: graphicsIndirect={} computeIndirect={}",
				dgcSupport.graphicsIndirect, dgcSupport.computeIndirect);
		}
	}

	/** @return 实例层引用（AppContext，非 const）；下游（Swapchain 等）经它取 raii Instance。 */
	RGL::AppContext& getVkInstance() { return vkInstance; }
	/** @return 选中的物理设备 const 引用（physicalDevice 指针指向 physicalDevices 列表内部，同生命周期）。 */
	const vk::raii::PhysicalDevice& getPhysicalDevice() const { return *physicalDevice; }
	/**
	 * push data 硬上限（启动期契约检查，main.cpp）：descriptor heap 模式下 per-draw push data
	 * （DrawArgs，vkCmdPushDataEXT）受设备硬限约束，两个 limit 双驱动实测均 256B：
	 * ① maxPushDataSize（VkPhysicalDeviceDescriptorHeapPropertiesEXT，heap 模式专属；
	 *    VUID-VkPushDataInfoEXT-offset-11243：vkCmdPushDataEXT 的 offset+size ≤ 本值）
	 * ② maxPushConstantsSize（VkPhysicalDeviceLimits，descriptor set 模式上限；
	 *    heap 模式下 shader push constant 块成员须落在 [0, maxPushDataSize] 内）
	 * 返回纯值：vk:: 命名空间对 main.cpp 不可见（各模块均无 export import vulkan 头，
	 * main 只用方法链）——vk 类型不泄漏到应用入口。
	 * @return {maxPushConstantsSize, maxPushDataSize}（按此序）
	 */
	[[nodiscard]] std::pair<uint32_t, uint32_t> getPushDataLimits() const {
		const auto props = getPhysicalDevice()
			.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		return {static_cast<uint32_t>(props.get<vk::PhysicalDeviceProperties2>().properties.limits.maxPushConstantsSize),
			static_cast<uint32_t>(props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>().maxPushDataSize)};
	}
	/** @return raii Device 引用（非 const / const 两重载，同一对象）；下游（Swapchain /
	 * Resources / Renderer / 模型 / 纹理 / 管线表等）创建 GPU 资源的设备入口。 */
	vk::raii::Device& getDevice() { return device; }
	const vk::raii::Device& getDevice() const { return device; }
	/** @return 图形队列（vk::Queue 为 C 句柄，按值返回）；主渲染队列。 */
	vk::Queue getGraphicsQueue() const { return graphicsQueue; }
	/** @return 计算队列（C 句柄，按值）；保留给未来使用，当前未用（见 AGENTS.md 已知问题）。 */
	vk::Queue getComputeQueue() const { return computeQueue; }
	/** @return 传输队列（C 句柄，按值）；保留给未来使用，当前未用（见 AGENTS.md 已知问题）。 */
	vk::Queue getTransferQueue() const { return transferQueue; }
	/** @return 图形命令池（const 引用；eResetCommandBuffer）；主渲染（Renderer）/ 资产
	 * 上传（模型 / 纹理）的命令缓冲分配与录制。 */
	const vk::raii::CommandPool& getGraphicsCommandPool() const { return graphicsCommandPool; }
	/** @return 计算命令池（const 引用；eResetCommandBuffer）；保留给未来使用，当前未用。 */
	const vk::raii::CommandPool& getComputeCommandPool() const { return computeCommandPool; }
	/** @return 传输命令池（const 引用；eTransient | eResetCommandBuffer）；保留给未来
	 * 使用，当前未用。 */
	const vk::raii::CommandPool& getTransferCommandPool() const { return transferCommandPool; }
	/** @return 队列 family 管理器引用（非 const / const 两重载，同一对象）；Swapchain
	 * 用它做 checkPresentAbility 等。 */
	RGL::QueueFamilyManager& getQueueManager() { return queueManager; }
	const RGL::QueueFamilyManager& getQueueManager() const { return queueManager; }
	/** @return 可选 feature 支持位（queryOptionalFeatures 探测结果；Renderer 消费——
	 * edS3 depthClip 动态状态）。 */
	const RGL::OptionalFeatureSupport& getOptionalFeatures() const { return optionalFeatures; }
	/** @return DGC 能力位（queryDgcSupport 派生；culling 子系统据此选 DGC 全路径 /
	 * 读回重放回退，见 vk_indirect_commands 模块与 phase7_devlog 批次 5）。 */
	const RGL::DgcSupport& getDgcSupport() const { return dgcSupport; }

   private:
	/**
	 * device 创建前置校验（初始化列表经 checked 成员调用）：extension 可用性
	 * （checkDeviceExtensions）+ feature 链支持度（validateFeatures）。失败抛带
	 * [Code] 的 runtime_error（describe()），而非 raii Device 构造器的不透明
	 * vk::SystemError。
	 * @param physicalDevice 待校验的物理设备
	 * @param featuresChain required feature 链（逐项校验其支持度）
	 * @return true（成功时；值只赋给 checked 成员，用于钉死初始化顺序）
	 */
	static bool precheckDevice(const vk::raii::PhysicalDevice& physicalDevice,
		RGL::FeatureChain& featuresChain) {
		if (auto result = RGL::checkDeviceExtensions(physicalDevice); !result) {
			throw std::runtime_error(result.error().describe());
		}
		if (auto result = RGL::validateFeatures(physicalDevice, featuresChain); !result) {
			throw std::runtime_error(result.error().describe());
		}
		return true;
	}

	/**
	 * DGC 能力位派生（physical device properties 查询，与 device 创建无关；结果存
	 * dgcSupport 成员）。判据 = 研究文档 §3.3：graphics 侧可用 = shader binding 子集含
	 * MESH|FRAGMENT（execution set 逐序列换 shader + mesh draw 序列）；compute 侧 = 含
	 * COMPUTE（DISPATCH 序列）。
	 * @param physicalDevice 待查询的物理设备
	 * @return 派生能力位（本机 RADV 预期 {false, true}，NVIDIA 预期 {true, true}）
	 */
	static DgcSupport queryDgcSupport(const vk::raii::PhysicalDevice& physicalDevice) {
		const auto props = physicalDevice
			.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT>();
		const auto stages = props.get<vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT>()
			.supportedIndirectCommandsShaderStagesShaderBinding;
		// BMI 不随类序列化自由运算符（rlstd 同教训）：!= 0 的 operator== 不可见，
		// 经 explicit operator bool 显式转换判定
		return DgcSupport{static_cast<bool>(stages & (vk::ShaderStageFlagBits::eMeshEXT | vk::ShaderStageFlagBits::eFragment)),
			static_cast<bool>(stages & vk::ShaderStageFlagBits::eCompute),
			props.get<vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT>().maxIndirectShaderObjectCount};
	}

	/**
	 * optional feature 结构初始化（sType + 按支持位置位）——结果存入成员，生命周期
	 * 与 DeviceContext 一致。
	 * @param opt 可选 feature 支持位（depthClipBase 位决定 depthClipEnable 字段）
	 * @return feature 结构（存入 depthClipBaseFeature 成员，createDevice 条件链接进链尾）
	 */
	static vk::PhysicalDeviceDepthClipEnableFeaturesEXT initOptionalDepthClipBaseFeature(const RGL::OptionalFeatureSupport& opt) {
		vk::PhysicalDeviceDepthClipEnableFeaturesEXT f{};
		f.sType = vk::StructureType::ePhysicalDeviceDepthClipEnableFeaturesEXT;
		f.depthClipEnable = opt.depthClipBase ? VK_TRUE : VK_FALSE;
		return f;
	}

	/**
	 * edS3（ExtendedDynamicState3）feature 结构初始化（sType + 按支持位置位）。
	 * @param opt 可选 feature 支持位（depthClipEnable 位决定 extendedDynamicState3DepthClipEnable 字段）
	 * @return feature 结构（存入 eds3Feature 成员，createDevice 条件链接进链尾）
	 */
	static vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT initOptionalEds3Feature(const RGL::OptionalFeatureSupport& opt) {
		vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT f{};
		// 独立结构体（非 StructureChain 管理）：显式钉死 sType，不依赖 hpp 默认构造器
		f.sType = vk::StructureType::ePhysicalDeviceExtendedDynamicState3FeaturesEXT;
		f.extendedDynamicState3DepthClipEnable = opt.depthClipEnable ? VK_TRUE : VK_FALSE;
		return f;
	}

	/**
	 * robustness2 feature 结构初始化（sType + 按各自支持位置位
	 * robustBufferAccess2 / nullDescriptor 两位）。
	 * @param opt 可选 feature 支持位
	 * @return feature 结构（存入 robustness2Feature 成员，createDevice 条件链接进链尾）
	 */
	static vk::PhysicalDeviceRobustness2FeaturesKHR initOptionalRobustness2Feature(const RGL::OptionalFeatureSupport& opt) {
		vk::PhysicalDeviceRobustness2FeaturesKHR f{};
		f.sType = vk::StructureType::ePhysicalDeviceRobustness2FeaturesKHR;
		f.robustBufferAccess2 = opt.robustBufferAccess2 ? VK_TRUE : VK_FALSE;
		f.nullDescriptor = opt.nullDescriptor ? VK_TRUE : VK_FALSE;
		return f;
	}

	/**
	 * 创建 raii Device：先按支持位条件链接 optional feature 结构进 required 链尾（见
	 * 体内注释），再组装 DeviceCreateInfo（pNext = required 链头、queue create infos
	 * 来自 QueueFamilyManager、扩展 = required 恒启用 + optional 按探测追加）并打日志，
	 * 最后构造 raii Device。
	 * @param physicalDevice 待创建的设备
	 * @param queueManager 队列 family 管理器（读 queue create infos；priorities 是其
	 *   成员，须比 vkCreateDevice 长寿——作为 DeviceContext 成员满足）
	 * @param featuresChain required feature 链（本函数把链尾 pNext 写为 optional 链头）
	 * @param optionalFeatures 可选 feature 支持位（与扩展启用同源，见 getDeviceExtensions）
	 * @param eds3Feature edS3 feature 结构（depthClipEnable 置位时链接）
	 * @param depthClipBaseFeature 旧扩展 base feature 结构（depthClipBase 置位时链接）
	 * @param robustness2Feature robustness2 feature 结构（两位支持位任一置位时链接）
	 * @return 创建好的 raii Device
	 * @note 扩展/feature 支持须先经 precheckDevice（校验前移，见构造器）；raii 构造
	 *   失败抛 vk::SystemError
	 */
	static vk::raii::Device createDevice(const vk::raii::PhysicalDevice& physicalDevice,
		RGL::QueueFamilyManager& queueManager,
		RGL::FeatureChain& featuresChain,
		const RGL::OptionalFeatureSupport& optionalFeatures,
		vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT& eds3Feature,
		vk::PhysicalDeviceDepthClipEnableFeaturesEXT& depthClipBaseFeature,
		vk::PhysicalDeviceRobustness2FeaturesKHR& robustness2Feature) {
		// 条件链接 optional feature 结构进 required 链尾（spec：vkCreateDevice pNext 链只含
		// "已启用扩展"的 feature 结构——FALSE 结构挂未启用扩展 = UB，RADV 实测启动后首次分配
		// 即崩）。仅启用者链接；未启用 = 链尾保持 nullptr。扩展启用（getDeviceExtensions）
		// 与本链接同源一致（都按 optionalFeatures）。
		void* optHead = nullptr;
		if (optionalFeatures.depthClipBase) { depthClipBaseFeature.pNext = optHead; optHead = &depthClipBaseFeature; }
		if (optionalFeatures.depthClipEnable) { eds3Feature.pNext = optHead; optHead = &eds3Feature; }
		if (optionalFeatures.robustBufferAccess2 || optionalFeatures.nullDescriptor) {
			robustness2Feature.pNext = optHead; optHead = &robustness2Feature;
		}
		featuresChain.get<RGL::FeatureChainTail>().pNext = optHead;


		vk::DeviceCreateInfo deviceCreateInfo{};
		deviceCreateInfo.setPNext(&featuresChain.get<vk::PhysicalDeviceFeatures2>());
		deviceCreateInfo.setQueueCreateInfos(queueManager.getQueueCreateInfos());

		// required 恒启用 + optional 按探测支持位追加（与上方条件链接同源一致）
		std::vector<const char*> extensions = RGL::getDeviceExtensions(optionalFeatures);
#ifdef RGL_AFTERMATH  // 编译期开关（rgl/CMakeLists.txt option）：Aftermath 富 dump 前置
		// NVIDIA Aftermath 富 dump——探测 diagnostics_config 扩展（NV-only，RADV/Intel
		// 无 = 整体 no-op），命中则加两扩展 + 链 config(flags=15：错误报告/资源跟踪/
		// 自动 checkpoint=CPU 调用栈/shader 调试信息）。仅 RGL_AFTERMATH=ON 时编译
		bool nvDiag = false;
		for (const auto& p : physicalDevice.enumerateDeviceExtensionProperties())
			if (std::string_view(p.extensionName.data()) == VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME)
				nvDiag = true;
		vk::DeviceDiagnosticsConfigCreateInfoNV diagCfg{};
		if (nvDiag) {
			extensions.push_back(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
			extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
			diagCfg.flags = vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderErrorReporting |
				vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableResourceTracking |
				vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableAutomaticCheckpoints |
				vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderDebugInfo;
			diagCfg.pNext = deviceCreateInfo.pNext;
			deviceCreateInfo.pNext = &diagCfg;
		}
#endif
		deviceCreateInfo.setPEnabledExtensionNames(extensions);

		auto logger = RLLogger::getInstance();
		if (logger) {
			logger->info("Device: enabling {} extensions", extensions.size());
			for (const char* ext : extensions) {
				logger->info("  - {}", ext);
			}
			logger->info("Device optional features: depthClipEnable={}",
				optionalFeatures.depthClipEnable);
			logger->info("Device optional features: robustBufferAccess2={} nullDescriptor={}",
				optionalFeatures.robustBufferAccess2, optionalFeatures.nullDescriptor);
		}

		return vk::raii::Device(physicalDevice, deviceCreateInfo);
	}

	/**
	 * 为给定 queue family 创建命令池。
	 * @param device 创建来源设备
	 * @param queueFamilyIndex 队列 family 下标（来自 QueueFamilyManager::getFamilyIndex）
	 * @param flags 池标志（graphics/compute = eResetCommandBuffer；transfer =
	 *   eTransient | eResetCommandBuffer）
	 * @return 创建好的 raii CommandPool（expected 包装）
	 * @note raii 构造失败实际抛 vk::SystemError（Error 分支是预留路径，当前不可达）；
	 *   调用点经 .value() 取用
	 */
	static std::expected<vk::raii::CommandPool, Error> createCommandPool(vk::raii::Device& device, uint32_t queueFamilyIndex,
		vk::CommandPoolCreateFlags flags) {
		vk::CommandPoolCreateInfo poolInfo{};
		poolInfo.flags = flags;
		poolInfo.queueFamilyIndex = queueFamilyIndex;
		return vk::raii::CommandPool(device, poolInfo);
	}

	RGL::AppContext& vkInstance;  // 实例层引用（最先初始化）；其余成员初始化均依赖它
	vk::raii::PhysicalDevices physicalDevices;  // raii 物理设备列表（枚举自实例）；physicalDevice 指针指向
	                                           //   本列表内部，须比该指针长寿
	const vk::raii::PhysicalDevice* physicalDevice;  // 选中的物理设备指针（pickPhysicalDevice 打分；raii
	                                                //   PhysicalDevice 不可拷贝，存指针）
	DgcSupport dgcSupport;  // DGC 能力位（queryDgcSupport 派生；culling 选路消费，Phase 7b）
	RGL::QueueFamilyManager queueManager;  // 队列 family 管理器（持 physicalDevice 引用，须后于其初始化）；
	                                      //   queueCreateInfos / priorities 是其成员，须比 vkCreateDevice 长寿
	RGL::OptionalFeatureSupport optionalFeatures;  // 可选 feature 支持位（queryOptionalFeatures 探测，先于 featuresChain）
	RGL::FeatureChain featuresChain;  // required feature 链（恒 TRUE）——必须类成员：createDevice 的
	                                  //   DeviceCreateInfo.setPNext 存链头指针（非拷贝），pNext 不能悬空（AGENTS.md）；
	                                  //   optional 结构不进本链
	// optional feature 结构（createDevice 条件链接进 required 链尾；成员保活避免 pNext 悬垂）
	vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT eds3Feature;
	vk::PhysicalDeviceDepthClipEnableFeaturesEXT depthClipBaseFeature;
	vk::PhysicalDeviceRobustness2FeaturesKHR robustness2Feature;
	bool checked;  // 仅用于钉死初始化顺序：precheckDevice 先于 device 构造（见构造器）
	vk::raii::Device device;  // raii Device（初始化列表由 createDevice 构造；扩展/feature 校验已前移到 checked）
	vk::Queue graphicsQueue;  // 图形队列（vk::Queue C 句柄，按值）；主渲染队列
	vk::Queue computeQueue;  // 计算队列（保留给未来使用，当前未用）
	vk::Queue transferQueue;  // 传输队列（保留给未来使用，当前未用）
	vk::raii::CommandPool graphicsCommandPool;  // 图形命令池（eResetCommandBuffer）
	vk::raii::CommandPool computeCommandPool;  // 计算命令池（eResetCommandBuffer；保留给未来使用，当前未用）
	vk::raii::CommandPool transferCommandPool;  // 传输命令池（eTransient | eResetCommandBuffer；保留给未来使用，当前未用）
};

}  // namespace RGL
