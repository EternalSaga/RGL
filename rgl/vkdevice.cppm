module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
export module vk_device;

import rlstd;
import rlvk;
import rllogger;
import vulkan_utils;
import queue_family_manager;
import features;
import device_fault;  // GPU fault 诊断通路的探测结论（feature 结构条件链 + getter 暴露）
import vk_instance;
import vkerror;

namespace RGL {

/**
 * 设备是否支持 shaderFloat16 **且** shaderInt16（批 14.5，FSR 3.1.4 的 shader 前置）。
 *
 * 两个位**分居两处**，所以必须分别读：`shaderInt16` 在 `VkPhysicalDeviceFeatures`（基结构），
 * `shaderFloat16` 只在 `VkPhysicalDeviceVulkan12Features`（`VkPhysicalDeviceFeatures` 里
 * 根本没有它）。
 *
 * 为什么不走 `features_config.json` 的 optional 机制：该机制「无扩展门」的分支只读
 * `VkPhysicalDeviceFeatures`（放不进 shaderFloat16），而给它挂
 * `VK_KHR_shader_float16_int8` 又**永远探不到**——该扩展 1.2 已进核心，被 instance 的 API
 * 版本从 `enumerateDeviceExtensionProperties` 的表里过滤掉了（同一机制另见
 * docs/phase14_plan.md §1.12 第 6 条：FSR 自己的排列探测正是因此恒选最低配）。
 *
 * 语义是 **optional**：缺位的机器只是跑不了 FSR，不该连应用都起不来（消费侧 =
 * `Renderer::fsrPreFrame` 探测后降级，同 samplerAnisotropy → 不开 AF 的模式）。
 */
bool probeShader16Supported(const vk::raii::PhysicalDevice& device) {
	const auto probed = device.getFeatures2<vk::PhysicalDeviceFeatures2,
		vk::PhysicalDeviceVulkan12Features>();
	return probed.get<vk::PhysicalDeviceVulkan12Features>().shaderFloat16 != 0
	    && probed.get<vk::PhysicalDeviceFeatures2>().features.shaderInt16 != 0;
}

/**
 * DGC（VK_EXT_device_generated_commands）能力位（DeviceContext 构造期自
 * VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT.supportedIndirectCommandsShaderStagesShaderBinding
 * 派生；culling 子系统据此选路——Phase 7b，见 docs/device_generated_commands_research.md）。
 * @note 本机 RADV（Strix Halo 26.2.1）实测 binding 子集仅 COMPUTE——IES 逐序列换 shader 路
 * 不可用，但 graphics 执行路径驱动已实现 → staticGraphicsIndirect 走静态 shader 路
 * （docs/archive/radv_static_dgc_spike.md）；两者皆无才读回重放（phase7_devlog 批次 5 语义）。
 * NVIDIA binding 子集全开（含 MESH/FRAGMENT）→ 恒走 IES 路。
 */
export struct DgcSupport {
	bool graphicsIndirect = false;  ///< binding 子集含 MESH|FRAGMENT：execution set 逐序列换 shader + DRAW_MESH_TASKS 序列（DGC 全路径）
	bool computeIndirect = false;   ///< binding 子集含 COMPUTE：DISPATCH 序列
	/**
	 * 全集（supportedIndirectCommandsShaderStages）含 MESH：静态 shader DGC 路可用
	 * （layout 去 EXECUTION_SET token + pNext 挂 VkGeneratedCommandsShaderInfoEXT，
	 * 每管线一次 Execute——RADV 实现了 graphics 执行、只缺逐序列 binding，spec 无子集
	 * 要求，见 docs/archive/radv_static_dgc_spike.md §2）。graphicsIndirect 优先级更高。
	 */
	bool staticGraphicsIndirect = false;
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
 * dgcSupport→queueManager→optionalFeatures→featuresChain→五 optional 结构→checked→
 * device→队列→命令池；checked 是纯顺序锚定成员，见其行内注释）。
 */
export class DeviceContext {
   public:
	/**
	 * 构建设备层（全部逻辑在成员初始化列表，按成员声明序）：
	 * vkInstance → physicalDevices → physicalDevice（pickPhysicalDevice 打分选择）→
	 * dgcSupport（DGC 能力位派生）→ queueManager → optionalFeatures（探测）→ featuresChain（required 链）→
	 * eds3Feature → depthClipBaseFeature → robustness2Feature → presentTimingFeature →
	 * presentId2Feature（五个 optional 结构，
	 * 按支持位初始化）→ checked（校验前移）→ device → 图形队列 + 图形命令池
	 *（eResetCommandBuffer）。**只有一条队列**：桌面 / APU 的 family 0 是全功能族，
	 * 旧版多出来的 compute/transfer 两个句柄恒被钳回同一条队列且零使用，已删
	 *（判据与"将来怎么拿第二条队列"的形态 → `QueueFamilyManager` 类注释）。
	 * @param vkInstance 实例层（读 raii Instance 枚举物理设备）
	 * @param faultPollWanted 调用方**是否要** masked fault 轮询（= `diagnostics.fault_poll`）。
	 *        🔴 传的是门，不是"驱动支持"：启用 `deviceFaultReportMasked` 后驱动可以内部恢复
	 *        故障而不再报 device lost，默认开 = 可能把要查的 hang 变成"什么都没发生"。门不开
	 *        = feature 位传 FALSE，通路仍在（device lost 后照样能查 fault）。
	 * @note 失败路径：pickPhysicalDevice 失败 / precheckDevice 扩展或 feature 缺失抛
	 *   std::runtime_error（describe() 带 [Code]）；raii Device / CommandPool 构造
	 *   失败抛 vk::SystemError（createCommandPool 的 expected Error 分支是预留路径，
	 *   不可达）；queueManager 与 getQueue 的 .value() 在**物理设备无 eGraphics family**
	 *   时抛 bad_expected_access（打分制不查图形能力，故此路径理论可达；旧版此处是
	 *   std::map::at 抛 out_of_range，语义相同、信息更少）
	 */
	DeviceContext(RGL::AppContext& vkInstance, bool faultPollWanted = false)
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
	  shader16Ok(probeShader16Supported(*physicalDevice)),
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
	   * base feature 同时启用（2026-09-02 深挖推翻"VVL 误报"旧结论——见 git:调试历史）。 */
	  depthClipBaseFeature(initOptionalDepthClipBaseFeature(optionalFeatures)),
	  /** robustness2（越界访问规范化 + null 描述符）：两支持位共用一个 feature 结构，
	   * 任一位可用即结构进链（字段按各自支持位置位）；不可用 = 不链接，no-op 降级。 */
	  robustness2Feature(initOptionalRobustness2Feature(optionalFeatures)),
	  /** Phase 9.3 present_timing 整组（presentTiming+presentId2+KHR 校准同门，见
	   * features.cppm presentTimingSupported——与扩展启用同源，任一缺 = 都不链接）。 */
	  presentTimingFeature(initOptionalPresentTimingFeature(optionalFeatures)),
	  presentId2Feature(initOptionalPresentId2Feature(optionalFeatures)),
	  /** device fault 诊断通路的探测结论（🔴 必须先于下面两个 feature 结构——它们读它置位，
	   *  门控后的 masked 决定只写在这一处，见 deriveDeviceFaultSupport）。 */
	  deviceFaultSupport(RGL::deriveDeviceFaultSupport(*physicalDevice, optionalFeatures.deviceFaultKhr,
	      optionalFeatures.deviceFaultExt,
	      faultPollWanted && optionalFeatures.deviceFaultReportMaskedKhr,
	      optionalFeatures.deviceFaultVendorBinaryKhr, optionalFeatures.deviceFaultVendorBinaryExt)),
	  deviceFaultKhrFeature(initDeviceFaultKhrFeature(deviceFaultSupport)),
	  deviceFaultExtFeature(initDeviceFaultExtFeature(deviceFaultSupport)),
	  /** 校验前移：必须在 raii::Device 构造（device 成员）之前——feature/extension
	   * 不支持时 raii 构造器先抛 vk::SystemError（不透明），自定义 Error 路径不可达
	   *（旧实现在构造体校验 = 失败路径死代码）。本成员只为钉死初始化顺序。 */
	  checked(precheckDevice(*physicalDevice, featuresChain)),
	  device(createDevice(*physicalDevice, queueManager, featuresChain, optionalFeatures, eds3Feature, depthClipBaseFeature, robustness2Feature, presentTimingFeature, presentId2Feature, deviceFaultSupport, deviceFaultKhrFeature, deviceFaultExtFeature)),
		  graphicsQueue(queueManager.getQueue(device).value()),
		  graphicsCommandPool(createCommandPool(device, queueManager.getFamilyIndex(),
			  vk::CommandPoolCreateFlagBits::eResetCommandBuffer)
				  .value()) {
		if (auto logger = RLLogger::getInstance()) {
			logger->info("Device DGC support: graphicsIndirect={} computeIndirect={} staticGraphicsIndirect={}",
				dgcSupport.graphicsIndirect, dgcSupport.computeIndirect, dgcSupport.staticGraphicsIndirect);
			// device fault 探测结果**恒打一行**（含 no-op 那两种）：诊断通路"没启用"这件事
			// 必须在日志里看得见，否则"日志里没有 fault 段"会被读成"启用了但没抓到"。
			logger->info("Device device_fault: backend={} reportMasked={} vendorBinary={} maxFaultCount={} ({})",
				deviceFaultSupport.backend == RGL::DeviceFaultBackend::none ? "none"
				           : deviceFaultSupport.backend == RGL::DeviceFaultBackend::khr ? "khr" : "ext",
				deviceFaultSupport.reportMasked, deviceFaultSupport.vendorBinary,
				deviceFaultSupport.maxFaultCount,
				RGL::describeDeviceFaultBackend(deviceFaultSupport.backend));
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
	/** @return 图形队列 family 下标（QueueFamilyManager 派生；Phase 10 imgui 后端 init 等按值场景）。 */
	uint32_t getGraphicsQueueFamily() const { return queueManager.getFamilyIndex(); }
	/** @return 图形命令池（const 引用；eResetCommandBuffer）；主渲染（Renderer）/ 资产
	 * 上传（模型 / 纹理）的命令缓冲分配与录制。 */
	const vk::raii::CommandPool& getGraphicsCommandPool() const { return graphicsCommandPool; }
	/** @return 队列 family 管理器引用（非 const / const 两重载，同一对象）；Swapchain
	 * 用它做 checkPresentAbility 等。 */
	RGL::QueueFamilyManager& getQueueManager() { return queueManager; }
	const RGL::QueueFamilyManager& getQueueManager() const { return queueManager; }
	/** @return 可选 feature 支持位（queryOptionalFeatures 探测结果；Renderer 消费——
	 * edS3 depthClip 动态状态）。 */
	const RGL::OptionalFeatureSupport& getOptionalFeatures() const { return optionalFeatures; }
	/** 批 14.5：设备支持 shaderFloat16 **且** shaderInt16（两者都已在本 device 启用）。
	 *  FSR 3.1.4 的 SPIR-V 声明这两个 capability，缺位则 FSR 不可用（探测式降级，
	 *  不进 required 清单——理由见 createDevice 内同批注释）。 */
	bool shader16Supported() const { return shader16Ok; }
	/** @return DGC 能力位（queryDgcSupport 派生；culling 子系统据此选 DGC 全路径 /
	 * 读回重放回退，见 vk_indirect_commands 模块与 phase7_devlog 批次 5）。 */
	const RGL::DgcSupport& getDgcSupport() const { return dgcSupport; }
	/**
	 * 债-140 §26/§27：**MDI 间接命令 × mesh 真取数链崩溃**的受影响平台判据（gfx1151 workaround
	 * 的设备识别脚，上游 issue 已投 **Mesa#16670**，2026-10-23）。
	 *
	 * 判据 = PCI (vendor 0x1002, device 0x1586)。**0x1586 = 本机实测**（Radeon 8060S /
	 * RADV STRIX_HALO，`/sys/bus/pci/devices/0000:65:00.0/{vendor,device}` 亲读）。
	 * ⚠ gfx1151（RDNA 3.5 Strix 系）家族另有近亲 SKU 的 device id（Strix Point 的 890M/880M
	 * 等）——**本机未实测，一律不列进判据**；要扩列的准入 = 该机器上先用 `ab/repro/ --chain`
	 * 复现器复红（§27 口径），再进来。宁缺勿滥的理由：漏列 = 那台机器维持现状（照崩但行为
	 * 不变），错列 = 在未验证设备上静默换派发形态。
	 *
	 * 消费者唯一 = `wireMeshletMdiShadowActivation`（喂 `resolveMdiShadowDirectEmu` 的
	 * `affectedDevice` 脚）；纯查询零副作用，启动期一次。
	 */
	[[nodiscard]] bool isMdiMeshChainAffected() const {
		const auto p = getPhysicalDevice().getProperties();
		return p.vendorID == 0x1002 && p.deviceID == 0x1586;
	}
	/** @return device fault 诊断通路的探测结论（后端 / masked 是否启用 / maxFaultCount）。
	 *  消费者：main 的 device lost 出口（`reportDeviceFaultOnLost`）与 ecs 帧循环的 masked
	 *  轮询（`pollDeviceFaults`）——两者都在 `backend == none` 时零调用返回。 */
	const RGL::DeviceFaultSupport& getDeviceFaultSupport() const { return deviceFaultSupport; }

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
	 * COMPUTE（DISPATCH 序列）；静态 shader 路 = **全集**含 MESH|FRAGMENT（RADV graphics
	 * 执行已实现、只缺逐序列 binding，docs/archive/radv_static_dgc_spike.md）。
	 * @param physicalDevice 待查询的物理设备
	 * @return 派生能力位（本机 RADV 预期 {false, true, true}，NVIDIA 预期 {true, true, true}）
	 */
	static DgcSupport queryDgcSupport(const vk::raii::PhysicalDevice& physicalDevice) {
		const auto props = physicalDevice
			.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT>();
		const auto& dgcProps = props.get<vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT>();
		const auto stages = dgcProps.supportedIndirectCommandsShaderStagesShaderBinding;
		// 静态 shader 路判据 = **全集**（非 binding 子集）含 MESH|FRAGMENT：layout token
		// 无 SHADER/EXECUTION_SET 时仅受全集约束（radv_static_dgc_spike §2.2）
		const auto allStages = dgcProps.supportedIndirectCommandsShaderStages;
		// MESH 与 FRAGMENT 必须**同时**在位（layout shaderStages = MESH|FRAGMENT 须为子集）；
		// 单 bit OR 会在 Intel（全集 12 stage 含 FRAGMENT、无 MESH/TASK）误报 true →
		// 建 layout 撞 VU-11091（VVL 违规）。BMI 不序列化 flag 枚举 == 运算符（同上）→
		// 拆两个 static_cast<bool> 相与
		const bool bindingMeshFrag = static_cast<bool>(stages & vk::ShaderStageFlagBits::eMeshEXT)
			&& static_cast<bool>(stages & vk::ShaderStageFlagBits::eFragment);
		const bool allMeshFrag = static_cast<bool>(allStages & vk::ShaderStageFlagBits::eMeshEXT)
			&& static_cast<bool>(allStages & vk::ShaderStageFlagBits::eFragment);
		return DgcSupport{bindingMeshFrag,
			static_cast<bool>(stages & vk::ShaderStageFlagBits::eCompute),
			allMeshFrag,
			dgcProps.maxIndirectShaderObjectCount};
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
		f.extendedDynamicState3ConservativeRasterizationMode =
			opt.conservativeRasterizationDynamic ? VK_TRUE : VK_FALSE;  // 批 11.3 1b：保守光栅模式动态化（IZB pass 用）
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
	 * Phase 9.3 present_timing feature 结构初始化（sType + presentTiming 组门置位；
	 * 调度两特征 presentAtAbsoluteTime/presentAtRelativeTime 恒 FALSE——9.3 测量-only，
	 * 不做 targetTime 调度）。
	 * @param opt 可选 feature 支持位（组门 = presentTimingSupported）
	 * @return feature 结构（存入 presentTimingFeature 成员，createDevice 组门链接进链尾）
	 */
	static vk::PhysicalDevicePresentTimingFeaturesEXT initOptionalPresentTimingFeature(const RGL::OptionalFeatureSupport& opt) {
		vk::PhysicalDevicePresentTimingFeaturesEXT f{};
		f.sType = vk::StructureType::ePhysicalDevicePresentTimingFeaturesEXT;
		f.presentTiming = RGL::presentTimingSupported(opt) ? VK_TRUE : VK_FALSE;
		f.presentAtAbsoluteTime = VK_FALSE;
		f.presentAtRelativeTime = VK_FALSE;
		return f;
	}

	/**
	 * Phase 9.3 VK_KHR_present_id2 feature 结构初始化（sType + 组门置位——与
	 * presentTiming 同门，保证两扩展恒同启用同链接）。
	 * @param opt 可选 feature 支持位（组门 = presentTimingSupported）
	 * @return feature 结构（存入 presentId2Feature 成员，createDevice 组门链接进链尾）
	 */
	static vk::PhysicalDevicePresentId2FeaturesKHR initOptionalPresentId2Feature(const RGL::OptionalFeatureSupport& opt) {
		vk::PhysicalDevicePresentId2FeaturesKHR f{};
		f.sType = vk::StructureType::ePhysicalDevicePresentId2FeaturesKHR;
		f.presentId2 = RGL::presentTimingSupported(opt) ? VK_TRUE : VK_FALSE;
		return f;
	}

	/**
	 * device fault 的 KHR feature 结构初始化（读**已派生好**的探测结论，不重算门控）。
	 *
	 * 两个不请求的位各有理由：`deviceFaultDeviceLostOnMasked` 会把 masked fault **升级**成
	 * device lost——那是"让故障更响"，而本批要的是"让故障可见"，升级会改变提交语义（红线）；
	 * `deviceFaultVendorBinary` 则是**按驱动支持请求**（support.vendorBinary 已从探测位算好，
	 * 驱动没报支持就是 false）——它只允许驱动多吐一份二进制 crash dump，不改任何提交语义，
	 * 所以不需要门控。不启用它，`vkGetDeviceFaultDebugInfoKHR` 按 spec 恒返 size 0。
	 * @param support deriveDeviceFaultSupport 的结果（backend / reportMasked / vendorBinary）
	 */
	static vk::PhysicalDeviceFaultFeaturesKHR initDeviceFaultKhrFeature(const RGL::DeviceFaultSupport& support) {
		vk::PhysicalDeviceFaultFeaturesKHR f{};
		f.sType = vk::StructureType::ePhysicalDeviceFaultFeaturesKHR;
		f.deviceFault = support.backend == RGL::DeviceFaultBackend::khr ? VK_TRUE : VK_FALSE;
		f.deviceFaultVendorBinary = support.vendorBinary ? VK_TRUE : VK_FALSE;
		f.deviceFaultReportMasked = support.reportMasked ? VK_TRUE : VK_FALSE;
		f.deviceFaultDeviceLostOnMasked = VK_FALSE;
		return f;
	}

	/** device fault 的 EXT 回落 feature 结构初始化（rev 2 只有两位，无 masked 上报）。 */
	static vk::PhysicalDeviceFaultFeaturesEXT initDeviceFaultExtFeature(const RGL::DeviceFaultSupport& support) {
		vk::PhysicalDeviceFaultFeaturesEXT f{};
		f.sType = vk::StructureType::ePhysicalDeviceFaultFeaturesEXT;
		f.deviceFault = support.backend == RGL::DeviceFaultBackend::ext ? VK_TRUE : VK_FALSE;
		f.deviceFaultVendorBinary = support.vendorBinary ? VK_TRUE : VK_FALSE;
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
	 * @param presentTimingFeature present_timing feature 结构（组门置位时链接）
	 * @param presentId2Feature present_id2 feature 结构（与 present_timing 同组门链接）
	 * @param deviceFaultSupport device fault 探测结论（决定链接哪一个后端结构，none = 都不链）
	 * @param deviceFaultKhrFeature device fault KHR feature 结构（backend==khr 时链接）
	 * @param deviceFaultExtFeature device fault EXT feature 结构（backend==ext 时链接）
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
		vk::PhysicalDeviceRobustness2FeaturesKHR& robustness2Feature,
		vk::PhysicalDevicePresentTimingFeaturesEXT& presentTimingFeature,
		vk::PhysicalDevicePresentId2FeaturesKHR& presentId2Feature,
		const RGL::DeviceFaultSupport& deviceFaultSupport,
		vk::PhysicalDeviceFaultFeaturesKHR& deviceFaultKhrFeature,
		vk::PhysicalDeviceFaultFeaturesEXT& deviceFaultExtFeature) {
		// 条件链接 optional feature 结构进 required 链尾（spec：vkCreateDevice pNext 链只含
		// "已启用扩展"的 feature 结构——FALSE 结构挂未启用扩展 = UB，RADV 实测启动后首次分配
		// 即崩）。仅启用者链接；未启用 = 链尾保持 nullptr。扩展启用（getDeviceExtensions）
		// 与本链接同源一致（都按 optionalFeatures）。
		void* optHead = nullptr;
		if (optionalFeatures.depthClipBase) { depthClipBaseFeature.pNext = optHead; optHead = &depthClipBaseFeature; }
		if (optionalFeatures.depthClipEnable || optionalFeatures.conservativeRasterizationDynamic) {
			eds3Feature.pNext = optHead; optHead = &eds3Feature;  // 与 getDeviceExtensions 的 EDS3 底座条件同源
		}
		if (optionalFeatures.robustBufferAccess2 || optionalFeatures.nullDescriptor) {
			robustness2Feature.pNext = optHead; optHead = &robustness2Feature;
		}
		if (RGL::presentTimingSupported(optionalFeatures)) {  // Phase 9.3 整组（与扩展启用同门）
			presentId2Feature.pNext = optHead; optHead = &presentId2Feature;
			presentTimingFeature.pNext = optHead; optHead = &presentTimingFeature;
		}
		// device fault：按探测结论链接**其中一条**（none = 都不链）。判据与 features.cppm
		// 的扩展启用同一个 backend，两处的"用哪条"不可能分叉。
		switch (deviceFaultSupport.backend) {
		case RGL::DeviceFaultBackend::khr:
			deviceFaultKhrFeature.pNext = optHead; optHead = &deviceFaultKhrFeature;
			break;
		case RGL::DeviceFaultBackend::ext:
			deviceFaultExtFeature.pNext = optHead; optHead = &deviceFaultExtFeature;
			break;
		case RGL::DeviceFaultBackend::none:
			break;  // 整模块 no-op：不链任何结构
		}
		featuresChain.get<RGL::FeatureChainTail>().pNext = optHead;

		// core 可选特性（与上面 optional 扩展结构的条件链接不同：VkPhysicalDeviceFeatures2
		// 本身就是 required 链头，无需挂链，按探测结果直接置位）：不支持 = 恒 FALSE，
		// 消费侧按 getOptionalFeatures() 降级（samplerAnisotropy → SamplerLibrary 不开 AF）。
		if (optionalFeatures.samplerAnisotropy)
			featuresChain.get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy = VK_TRUE;
		// 批 14.5：FSR 3.1.4 的 SPIR-V 声明了 Float16 / Int16 capability（**与它选哪个 fp16
		// 数学排列无关**——生成期 `..._REPROJECT_SAMPLERS_USE_DATA_HALF = 1` 让 shader 用
		// half **采样数据**，那条路径恒开），缺这两个位 = vkCreateShaderModule 直接报
		// VUID-VkShaderModuleCreateInfo-pCode-08740。置位口径见 probeShader16Supported。
		if (probeShader16Supported(physicalDevice)) {
			featuresChain.get<vk::PhysicalDeviceFeatures2>().features.shaderInt16 = VK_TRUE;
			featuresChain.get<vk::PhysicalDeviceVulkan12Features>().shaderFloat16 = VK_TRUE;
		}

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
			logger->info("Device optional features: samplerAnisotropy={} (core 位，命中则共享 sampler 开 AF)"
				" shader16={} (批 14.5：FSR shader 前置，缺位则 FSR 降级)",
				optionalFeatures.samplerAnisotropy, probeShader16Supported(physicalDevice));
			logger->info("Device optional features: robustBufferAccess2={} nullDescriptor={}",
				optionalFeatures.robustBufferAccess2, optionalFeatures.nullDescriptor);
			logger->info("Device optional ext: calibratedTimestamps={} (Phase 9.2 GPU 时间戳)",
				optionalFeatures.calibratedTimestamps);
			logger->info("Device optional: presentTiming gate presentTiming={} presentId2={} calibratedKhr={} -> group={}",
				optionalFeatures.presentTiming, optionalFeatures.presentId2,
				optionalFeatures.calibratedTimestampsKhr,
				RGL::presentTimingSupported(optionalFeatures));
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
	bool shader16Ok = false;  ///< 批 14.5：Float16+Int16 core 位（初始化列表探，与 createDevice 同源）
	RGL::FeatureChain featuresChain;  // required feature 链（恒 TRUE）——必须类成员：createDevice 的
	                                  //   DeviceCreateInfo.setPNext 存链头指针（非拷贝），pNext 不能悬空（AGENTS.md）；
	                                  //   optional 结构不进本链
	// optional feature 结构（createDevice 条件链接进 required 链尾；成员保活避免 pNext 悬垂）
	vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT eds3Feature;
	vk::PhysicalDeviceDepthClipEnableFeaturesEXT depthClipBaseFeature;
	vk::PhysicalDeviceRobustness2FeaturesKHR robustness2Feature;
	vk::PhysicalDevicePresentTimingFeaturesEXT presentTimingFeature;  // Phase 9.3（presentTiming 组门置位才链接）
	vk::PhysicalDevicePresentId2FeaturesKHR presentId2Feature;  // Phase 9.3（同组，与 presentTiming 整组同进退）
	// device fault 诊断通路（纯旁路）：support 是探测结论（先初始化，两个 feature 结构读它置位），
	// 两个结构按 backend **只链其一**（成员保活 = pNext 不悬空，AGENTS 生命周期红线）。
	RGL::DeviceFaultSupport deviceFaultSupport;
	vk::PhysicalDeviceFaultFeaturesKHR deviceFaultKhrFeature;
	vk::PhysicalDeviceFaultFeaturesEXT deviceFaultExtFeature;
	bool checked;  // 仅用于钉死初始化顺序：precheckDevice 先于 device 构造（见构造器）
	vk::raii::Device device;  // raii Device（初始化列表由 createDevice 构造；扩展/feature 校验已前移到 checked）
	vk::Queue graphicsQueue;  // 图形队列（vk::Queue C 句柄，按值）；主渲染队列，也是**唯一**队列
	                          //（全功能族 ⟹ 它同时跑 compute / transfer；第二条队列的形态见
	                          // `QueueFamilyManager` 类注释）
	vk::raii::CommandPool graphicsCommandPool;  // 图形命令池（eResetCommandBuffer）
};

}  // namespace RGL
