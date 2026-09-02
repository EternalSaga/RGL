module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
export module vk_device;

import rlstd;
import rllogger;
import vulkan_utils;
import queue_family_manager;
import features;
import vk_instance;
import vkerror;

namespace RGL {

export class DeviceContext {
   public:
	DeviceContext(RGL::AppContext& vkInstance)
		: vkInstance(vkInstance),
		  physicalDevices(vkInstance.getInstance()),
		  physicalDevice([result = RGL::pickPhysicalDevice(physicalDevices)]() {
			  if (!result) throw std::runtime_error(result.error().describe());
			  return *result;
		  }()),
	  queueManager(RGL::QueueFamilyManager::create(*physicalDevice).value()),
	  // 可选 feature 探测（先于 featuresChain/eds3Feature——后者按支持位初始化）
	  optionalFeatures(RGL::queryOptionalFeatures(*physicalDevice)),
	  // required feature 链（恒 TRUE）。optional 结构**不进本链**（spec：vkCreateDevice pNext 链
	  // 只含"已启用扩展"的 feature 结构；FALSE 结构挂未启用扩展 = UB，RADV 实测启动后首次分配即崩）
	  featuresChain(RGL::buildRequestedFeatures()),
	  // optional feature 结构（成员 = 生命周期与 DeviceContext 一致；createDevice 按支持位条件
	  // 链接到 required 链尾，未启用则不链接——与 getDeviceExtensions 的扩展启用同源一致）
	  eds3Feature(initOptionalEds3Feature(optionalFeatures)),
	  // 校验前移：必须在 raii::Device 构造（device 成员）之前——feature/extension
	  // 不支持时 raii 构造器先抛 vk::SystemError（不透明），自定义 Error 路径不可达
	  //（旧实现在构造体校验 = 失败路径死代码）。本成员只为钉死初始化顺序
	  checked(precheckDevice(*physicalDevice, featuresChain)),
	  device(createDevice(*physicalDevice, queueManager, featuresChain, optionalFeatures, eds3Feature)),
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
	}

	RGL::AppContext& getVkInstance() { return vkInstance; }
	const vk::raii::PhysicalDevice& getPhysicalDevice() const { return *physicalDevice; }
	// push data 硬上限（启动期契约检查，main.cpp）：descriptor heap 模式下 per-draw push data
	// （DrawArgs，vkCmdPushDataEXT）受设备硬限约束，两个 limit 双驱动实测均 256B：
	// ① maxPushDataSize（VkPhysicalDeviceDescriptorHeapPropertiesEXT，heap 模式专属；
	//    VUID-VkPushDataInfoEXT-offset-11243：vkCmdPushDataEXT 的 offset+size ≤ 本值）
	// ② maxPushConstantsSize（VkPhysicalDeviceLimits，descriptor set 模式上限；
	//    heap 模式下 shader push constant 块成员须落在 [0, maxPushDataSize] 内）
	// 返回纯值：vk:: 命名空间对 main.cpp 不可见（各模块均无 export import vulkan 头，
	// main 只用方法链）——vk 类型不泄漏到应用入口
	[[nodiscard]] std::pair<uint32_t, uint32_t> getPushDataLimits() const {
		const auto props = getPhysicalDevice()
			.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		return {static_cast<uint32_t>(props.get<vk::PhysicalDeviceProperties2>().properties.limits.maxPushConstantsSize),
			static_cast<uint32_t>(props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>().maxPushDataSize)};
	}
	vk::raii::Device& getDevice() { return device; }
	const vk::raii::Device& getDevice() const { return device; }
	vk::Queue getGraphicsQueue() const { return graphicsQueue; }
	vk::Queue getComputeQueue() const { return computeQueue; }
	vk::Queue getTransferQueue() const { return transferQueue; }
	const vk::raii::CommandPool& getGraphicsCommandPool() const { return graphicsCommandPool; }
	const vk::raii::CommandPool& getComputeCommandPool() const { return computeCommandPool; }
	const vk::raii::CommandPool& getTransferCommandPool() const { return transferCommandPool; }
	RGL::QueueFamilyManager& getQueueManager() { return queueManager; }
	const RGL::QueueFamilyManager& getQueueManager() const { return queueManager; }
	// 可选 feature 支持位（Renderer 消费——edS3 depthClip 动态状态）
	const RGL::OptionalFeatureSupport& getOptionalFeatures() const { return optionalFeatures; }

   private:
	// device 创建前置校验（初始化列表经 checked 成员调用）：extension 可用性 +
	// feature 链支持度。失败抛带 [Code] 的 runtime_error（describe()），而非 raii
	// Device 构造器的不透明 vk::SystemError
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

	// optional feature 结构初始化（sType + 按支持位置位）——成员，生命周期与 DeviceContext 一致
	static vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT initOptionalEds3Feature(const RGL::OptionalFeatureSupport& opt) {
		vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT f{};
		// 独立结构体（非 StructureChain 管理）：显式钉死 sType，不依赖 hpp 默认构造器
		f.sType = vk::StructureType::ePhysicalDeviceExtendedDynamicState3FeaturesEXT;
		f.extendedDynamicState3DepthClipEnable = opt.depthClipEnable ? VK_TRUE : VK_FALSE;
		return f;
	}

	static vk::raii::Device createDevice(const vk::raii::PhysicalDevice& physicalDevice,
		RGL::QueueFamilyManager& queueManager,
		RGL::FeatureChain& featuresChain,
		const RGL::OptionalFeatureSupport& optionalFeatures,
		vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT& eds3Feature) {
		// 条件链接 optional feature 结构进 required 链尾（spec：vkCreateDevice pNext 链只含
		// "已启用扩展"的 feature 结构——FALSE 结构挂未启用扩展 = UB，RADV 实测启动后首次分配
		// 即崩）。仅启用者链接；未启用 = 链尾保持 nullptr。扩展启用（getDeviceExtensions）
		// 与本链接同源一致（都按 optionalFeatures）。
		void* optHead = nullptr;
		if (optionalFeatures.depthClipEnable) { eds3Feature.pNext = optHead; optHead = &eds3Feature; }
		featuresChain.get<RGL::FeatureChainTail>().pNext = optHead;


		vk::DeviceCreateInfo deviceCreateInfo{};
		deviceCreateInfo.setPNext(&featuresChain.get<vk::PhysicalDeviceFeatures2>());
		deviceCreateInfo.setQueueCreateInfos(queueManager.getQueueCreateInfos());

		// required 恒启用 + optional 按探测支持位追加（与上方条件链接同源一致）
		std::vector<const char*> extensions = RGL::getDeviceExtensions(optionalFeatures);
		deviceCreateInfo.setPEnabledExtensionNames(extensions);

		auto logger = RLLogger::getInstance();
		if (logger) {
			logger->info("Device: enabling {} extensions", extensions.size());
			for (const char* ext : extensions) {
				logger->info("  - {}", ext);
			}
			logger->info("Device optional features: depthClipEnable={}",
				optionalFeatures.depthClipEnable);
		}

		return vk::raii::Device(physicalDevice, deviceCreateInfo);
	}

	static std::expected<vk::raii::CommandPool, Error> createCommandPool(vk::raii::Device& device, uint32_t queueFamilyIndex,
		vk::CommandPoolCreateFlags flags) {
		vk::CommandPoolCreateInfo poolInfo{};
		poolInfo.flags = flags;
		poolInfo.queueFamilyIndex = queueFamilyIndex;
		return vk::raii::CommandPool(device, poolInfo);
	}

	RGL::AppContext& vkInstance;
	vk::raii::PhysicalDevices physicalDevices;
	const vk::raii::PhysicalDevice* physicalDevice;
	RGL::QueueFamilyManager queueManager;
	RGL::OptionalFeatureSupport optionalFeatures;  // 可选 feature 支持位（queryOptionalFeatures 探测，先于 featuresChain）
	RGL::FeatureChain featuresChain;
	// optional feature 结构（createDevice 条件链接进 required 链尾；成员保活避免 pNext 悬垂）
	vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT eds3Feature;
	bool checked;  // 仅用于钉死初始化顺序：precheckDevice 先于 device 构造（见构造器）
	vk::raii::Device device;
	vk::Queue graphicsQueue;
	vk::Queue computeQueue;
	vk::Queue transferQueue;
	vk::raii::CommandPool graphicsCommandPool;
	vk::raii::CommandPool computeCommandPool;
	vk::raii::CommandPool transferCommandPool;
};

}  // namespace RGL
