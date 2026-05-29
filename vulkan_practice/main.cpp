#include <vulkan/vulkan_core.h>
#include <sol/call.hpp>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include "vulkan/vulkan.hpp"
#include <iostream>
#include <array>
import vulkan_utils;
int main() {
    try {
	vk::raii::Context context;

	auto apiVersion = context.enumerateInstanceVersion();

	vk::ApplicationInfo engineInfo;
	engineInfo.applicationVersion = vk::makeVersion(0, 1, 0);
	engineInfo.apiVersion = apiVersion;
	engineInfo.engineVersion = vk::makeVersion(0, 0, 1);
	engineInfo.pEngineName = "practice";

	std::vector<const char*> layers = {"VK_LAYER_KHRONOS_validation"};
	std::vector<const char*> extensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

	const auto availableLayers = context.enumerateInstanceLayerProperties();

	if (!findMissingValidationLayser(availableLayers, layers).empty()) {
	    throw std::runtime_error("Missing validation layers!");
	}

	vk::InstanceCreateInfo instanceCreateInfo;
	instanceCreateInfo.pApplicationInfo = &engineInfo;

	instanceCreateInfo.setPEnabledLayerNames(layers);
	instanceCreateInfo.setPEnabledExtensionNames(extensions);

	vk::raii::Instance instance(context, instanceCreateInfo);

	setupDebugMessenger(instance);

	vk::raii::PhysicalDevices physicalDevices(instance);

	const vk::raii::PhysicalDevice& bestPhysicalDevice = pickPhysicalDevice(physicalDevices);

	std::vector<vk::QueueFamilyProperties> queueFamilyProps = bestPhysicalDevice.getQueueFamilyProperties();

	auto allIndices = getGraphicsComputeTransferQueueFamilyIndices(queueFamilyProps);

	const float graphicsPriority = 1.0f;
	const std::array<float,2> computePriorities = {1.0f, 0.5f};
	const float transferPriority = 0.5f;

	vk::DeviceQueueCreateInfo graphicsQueueInfo{
	    {},
	    allIndices.graphicsFamilyIndex.value(),
	    1,
	    &graphicsPriority};
	vk::DeviceQueueCreateInfo computeQueueInfo{{}, allIndices.computeFamilyIndex.value(), 2, computePriorities.data()};
	vk::DeviceQueueCreateInfo transferQueueInfo{{}, allIndices.transferFamilyIndex.value(), 1, &transferPriority};
	std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos = {

	    graphicsQueueInfo,

	    computeQueueInfo,

	    transferQueueInfo

	};
	vk::DeviceCreateInfo deviceCreateInfo({}, queueCreateInfos);
	vk::raii::Device device{bestPhysicalDevice, deviceCreateInfo};


	vk::Queue graphicsQueue = device.getQueue(allIndices.graphicsFamilyIndex.value(),0);
	std::array<vk::Queue,computePriorities.size()> computeQueues;
	for (size_t i = 0; i < computePriorities.size();++i) {
		computeQueues[i] = device.getQueue(allIndices.computeFamilyIndex.value(),i);
	}
	vk::Queue transferQueue = device.getQueue(allIndices.transferFamilyIndex.value(),0);

	


    } catch (const std::exception& e) {
	std::cerr << "错误: " << e.what() << std::endl;
	return -1;
    }

    return 0;
}