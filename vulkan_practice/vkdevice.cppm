module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
export module vk_device;

import rlstd;
import vulkan_utils;
import queue_family_manager;
import features;
import vk_instance;

namespace RGL {

export class DeviceContext {
public:
    DeviceContext(RGL::AppContext& vkInstance)
        : vkInstance(vkInstance),
          physicalDevices(vkInstance.getInstance()),
          physicalDevice(&RGL::pickPhysicalDevice(physicalDevices)),
          queueManager(*physicalDevice),
          featuresChain(RGL::buildRequestedFeatures()),
          device(createDevice(*physicalDevice, queueManager, featuresChain)),
          graphicsQueue(queueManager.getQueue(device, RGL::QueueFamilyRole::Graphics)),
          computeQueue(queueManager.getQueue(device, RGL::QueueFamilyRole::Compute)),
          transferQueue(queueManager.getQueue(device, RGL::QueueFamilyRole::Transfer)),
          graphicsCommandPool(createCommandPool(device, queueManager.getFamilyIndex(RGL::QueueFamilyRole::Graphics),
                                                     vk::CommandPoolCreateFlagBits::eResetCommandBuffer)),
          computeCommandPool(createCommandPool(device, queueManager.getFamilyIndex(RGL::QueueFamilyRole::Compute),
                                                    vk::CommandPoolCreateFlagBits::eResetCommandBuffer)),
          transferCommandPool(createCommandPool(device, queueManager.getFamilyIndex(RGL::QueueFamilyRole::Transfer),
                                                     vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer))
    {
        RGL::checkDeviceExtensions(*physicalDevice);
        RGL::validateFeatures(*physicalDevice, featuresChain);
    }

    RGL::AppContext& getVkInstance() { return vkInstance; }
    const vk::raii::PhysicalDevice& getPhysicalDevice() const { return *physicalDevice; }
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
    static vk::raii::Device createDevice(const vk::raii::PhysicalDevice& physicalDevice,
                                            RGL::QueueFamilyManager& queueManager,
                                            RGL::FeatureChain& featuresChain) {
        vk::DeviceCreateInfo deviceCreateInfo{};
        deviceCreateInfo.setPNext(&featuresChain.get<vk::PhysicalDeviceFeatures2>());
        deviceCreateInfo.setQueueCreateInfos(queueManager.getQueueCreateInfos());
        deviceCreateInfo.setPEnabledExtensionNames(RGL::getDeviceExtensions());
        return vk::raii::Device(physicalDevice, deviceCreateInfo);
    }

    static vk::raii::CommandPool createCommandPool(vk::raii::Device& device, uint32_t queueFamilyIndex,
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
    vk::raii::Device device;
    vk::Queue graphicsQueue;
    vk::Queue computeQueue;
    vk::Queue transferQueue;
    vk::raii::CommandPool graphicsCommandPool;
    vk::raii::CommandPool computeCommandPool;
    vk::raii::CommandPool transferCommandPool;
};

}
