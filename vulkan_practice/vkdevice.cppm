module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vector>
#include <map>
export module vk_device;

import vulkan_utils;
import queue_family_manager;
import features;
import vk_instance;

namespace RGL {

export class VkDevice {
public:
    VkDevice(RGL::VkInstance& vkInstance)
        : vkInstance_(vkInstance),
          physicalDevices_(vkInstance_.getInstance()),
         physicalDevice_(&RGL::pickPhysicalDevice(physicalDevices_)),
          queueManager_(*physicalDevice_),
          featuresChain_(RGL::buildRequestedFeatures()),
          device_(createDevice(*physicalDevice_, queueManager_, featuresChain_)),
          graphicsQueue_(queueManager_.getQueue(*device_, RGL::QueueFamilyRole::Graphics)),
          computeQueue_(queueManager_.getQueue(*device_, RGL::QueueFamilyRole::Compute)),
          transferQueue_(queueManager_.getQueue(*device_, RGL::QueueFamilyRole::Transfer)),
          graphicsCommandPool_(createCommandPool(device_, queueManager_.getFamilyIndex(RGL::QueueFamilyRole::Graphics),
                                                    vk::CommandPoolCreateFlagBits::eResetCommandBuffer)),
          computeCommandPool_(createCommandPool(device_, queueManager_.getFamilyIndex(RGL::QueueFamilyRole::Compute),
                                                   vk::CommandPoolCreateFlagBits::eResetCommandBuffer)),
          transferCommandPool_(createCommandPool(device_, queueManager_.getFamilyIndex(RGL::QueueFamilyRole::Transfer),
                                                    vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer))
    {
        RGL::checkDeviceExtensions(*physicalDevice_);
        RGL::validateFeatures(*physicalDevice_, featuresChain_);
    }

    RGL::VkInstance& getVkInstance() { return vkInstance_; }
    const vk::raii::PhysicalDevice& getPhysicalDevice() const { return *physicalDevice_; }
    vk::raii::Device& getDevice() { return device_; }
    const vk::raii::Device& getDevice() const { return device_; }
    vk::Queue getGraphicsQueue() const { return graphicsQueue_; }
    vk::Queue getComputeQueue() const { return computeQueue_; }
    vk::Queue getTransferQueue() const { return transferQueue_; }
    const vk::raii::CommandPool& getGraphicsCommandPool() const { return graphicsCommandPool_; }
    const vk::raii::CommandPool& getComputeCommandPool() const { return computeCommandPool_; }
    const vk::raii::CommandPool& getTransferCommandPool() const { return transferCommandPool_; }
    RGL::QueueManager& getQueueManager() { return queueManager_; }
    const RGL::QueueManager& getQueueManager() const { return queueManager_; }

private:
   static vk::raii::Device createDevice(const vk::raii::PhysicalDevice& physicalDevice,
                                           RGL::QueueManager& queueManager,
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

    RGL::VkInstance& vkInstance_;
    vk::raii::PhysicalDevices physicalDevices_;
    const vk::raii::PhysicalDevice* physicalDevice_;
    RGL::QueueManager queueManager_;
    RGL::FeatureChain featuresChain_;
    vk::raii::Device device_;
    vk::Queue graphicsQueue_;
    vk::Queue computeQueue_;
    vk::Queue transferQueue_;
    vk::raii::CommandPool graphicsCommandPool_;
    vk::raii::CommandPool computeCommandPool_;
    vk::raii::CommandPool transferCommandPool_;
};

}
