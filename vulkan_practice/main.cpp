#include <cstdint>
#include <sol/call.hpp>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include "vulkan/vulkan.hpp"
#include <iostream>

import vulkan_utils;
import vulkan_window;
import queue_family_manager;
using namespace RGL;
int main() {
    try {
	vk::raii::Context context;

	SDLWindow window(800, 600, "Vulkan Practice");

	auto [surfaceExtensions, surfaceCount] = window.getSurfaceExtensions();

	auto apiVersion = context.enumerateInstanceVersion();

	vk::ApplicationInfo engineInfo;
	engineInfo.applicationVersion = vk::makeVersion(0, 1, 0);
	engineInfo.apiVersion = apiVersion;
	engineInfo.engineVersion = vk::makeVersion(0, 0, 1);
	engineInfo.pEngineName = "practice";

	std::vector<const char*> layers = {"VK_LAYER_KHRONOS_validation"};
	std::vector<const char*> instanceExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

	for (uint32_t i = 0; i < surfaceCount; i++) {
	    instanceExtensions.push_back(surfaceExtensions[i]);	 // add surface
	}

	const auto availableLayers = context.enumerateInstanceLayerProperties();

	if (!findMissingValidationLayser(availableLayers, layers).empty()) {
	    throw std::runtime_error("Missing validation layers!");
	}

	vk::InstanceCreateInfo instanceCreateInfo;
	instanceCreateInfo.pApplicationInfo = &engineInfo;

	instanceCreateInfo.setPEnabledLayerNames(layers);
	instanceCreateInfo.setPEnabledExtensionNames(instanceExtensions);

	vk::raii::Instance instance(context, instanceCreateInfo);

	auto debugMessenger = setupDebugMessenger(instance);

	vk::raii::PhysicalDevices physicalDevices(instance);

	const vk::raii::PhysicalDevice& bestPhysicalDevice = pickPhysicalDevice(physicalDevices);

	QueueManager queueManager(bestPhysicalDevice);

	auto featuresChain = vk::StructureChain<
	    vk::PhysicalDeviceFeatures2,	 // 1.0 的扩展基类（链表头）
	    vk::PhysicalDeviceVulkan11Features,	 // 1.1
	    vk::PhysicalDeviceVulkan12Features,	 // 1.2
	    vk::PhysicalDeviceVulkan13Features,	 // 1.3
	    vk::PhysicalDeviceVulkan14Features	 // 1.4
	    >();

	featuresChain.get<vk::PhysicalDeviceFeatures2>().features.robustBufferAccess = VK_TRUE;

	auto& f11 = featuresChain.get<vk::PhysicalDeviceVulkan11Features>();
	f11.shaderDrawParameters = VK_TRUE;
	f11.multiview = VK_TRUE;
	f11.storageBuffer16BitAccess = VK_TRUE;

	auto& f12 = featuresChain.get<vk::PhysicalDeviceVulkan12Features>();
	f12.descriptorIndexing = VK_TRUE;
	f12.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
	f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	f12.descriptorBindingPartiallyBound = VK_TRUE;
	f12.scalarBlockLayout = VK_TRUE;
	f12.timelineSemaphore = VK_TRUE;

	auto& f13 = featuresChain.get<vk::PhysicalDeviceVulkan13Features>();
	f13.dynamicRendering = VK_TRUE;
	f13.synchronization2 = VK_TRUE;
	f13.shaderZeroInitializeWorkgroupMemory = VK_TRUE;

	auto& f14 = featuresChain.get<vk::PhysicalDeviceVulkan14Features>();
	f14.pushDescriptor = VK_TRUE;
	f14.dynamicRenderingLocalRead = VK_TRUE;
	f14.maintenance5 = VK_TRUE;
	f14.maintenance6 = VK_TRUE;

	vk::DeviceCreateInfo deviceCreateInfo{};
	deviceCreateInfo.setPNext(&featuresChain.get<vk::PhysicalDeviceFeatures2>());


	deviceCreateInfo.setQueueCreateInfos(queueManager.getQueueCreateInfos());
	std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

	deviceCreateInfo.setPEnabledExtensionNames(deviceExtensions);

	vk::raii::Device device(bestPhysicalDevice, deviceCreateInfo);
	vk::Queue graphicsQueue = queueManager.getQueue(*device, QueueFamilyRole::Graphics);
	vk::Queue computeQueue = queueManager.getQueue(*device, QueueFamilyRole::Compute);
	vk::Queue transferQueue = queueManager.getQueue(*device, QueueFamilyRole::Transfer);

	vk::CommandPoolCreateInfo graphicsPoolInfo{};
	graphicsPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;  // 允许单独重置 CommandBuffer
	graphicsPoolInfo.queueFamilyIndex = queueManager.getFamilyIndex(QueueFamilyRole::Graphics);

	vk::raii::CommandPool graphicsCommandPool(device, graphicsPoolInfo);

	vk::CommandPoolCreateInfo computePoolInfo{};
	computePoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	computePoolInfo.queueFamilyIndex = queueManager.getFamilyIndex(QueueFamilyRole::Compute);

	vk::raii::CommandPool computeCommandPool(device, computePoolInfo);

	vk::CommandPoolCreateInfo transferPoolInfo{};

	transferPoolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	transferPoolInfo.queueFamilyIndex = queueManager.getFamilyIndex(QueueFamilyRole::Transfer);

	vk::raii::CommandPool transferCommandPool(device, transferPoolInfo);

	auto surface = window.createSurface(instance);

	auto surfaceFormats = bestPhysicalDevice.getSurfaceFormatsKHR(*surface);
	auto properFormat = window.createSurface(instance);

	const auto presentModes = bestPhysicalDevice.getSurfacePresentModesKHR(*surface);
	vk::PresentModeKHR selectedPresentMode = selectPresentMode(presentModes);
	

    } catch (const std::exception& e) {
	std::cerr << "错误: " << e.what() << std::endl;
	return -1;
    }

    return 0;
}