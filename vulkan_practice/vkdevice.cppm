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
	  featuresChain(RGL::buildRequestedFeatures()),
	  // 校验前移：必须在 raii::Device 构造（device 成员）之前——feature/extension
	  // 不支持时 raii 构造器先抛 vk::SystemError（不透明），自定义 Error 路径不可达
	  //（旧实现在构造体校验 = 失败路径死代码）。本成员只为钉死初始化顺序
	  checked(precheckDevice(*physicalDevice, featuresChain)),
	  device(createDevice(*physicalDevice, queueManager, featuresChain)),
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

	static vk::raii::Device createDevice(const vk::raii::PhysicalDevice& physicalDevice,
		RGL::QueueFamilyManager& queueManager,
		RGL::FeatureChain& featuresChain) {
		vk::DeviceCreateInfo deviceCreateInfo{};
		deviceCreateInfo.setPNext(&featuresChain.get<vk::PhysicalDeviceFeatures2>());
		deviceCreateInfo.setQueueCreateInfos(queueManager.getQueueCreateInfos());

		std::vector<const char*> extensions = RGL::getDeviceExtensions();
		deviceCreateInfo.setPEnabledExtensionNames(extensions);

		auto logger = RLLogger::getInstance();
		if (logger) {
			logger->info("Device: enabling {} extensions", extensions.size());
			for (const char* ext : extensions) {
				logger->info("  - {}", ext);
			}
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
	RGL::FeatureChain featuresChain;
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
