module;
#include <fmt/format.h>
#include "rllogger.hpp"
#include <stdexcept>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <cstdint>
#include <map>
#include <vector>
export module queue_family_manager;

namespace RGL {

export enum class QueueFamilyRole {
    Graphics,
    Compute,
    Transfer
};

struct QueueAllocation {
    uint32_t familyIndex;
    uint32_t baseQueueIndex; // 对应物理队列的索引
};

export class QueueManager {
public:
    QueueManager(const vk::raii::PhysicalDevice& physicalDevice):physicalDevice_(physicalDevice) {
        auto queueFamilies = physicalDevice.getQueueFamilyProperties();
        std::map<QueueFamilyRole, uint32_t> roleToFamilies;
        logger = RLLogger::getInstance();
        // Find best queue family for each role
        for (uint32_t i = 0; i < queueFamilies.size(); ++i) {
            const auto& props = queueFamilies[i];
            
            if (!roleToFamilies.count(QueueFamilyRole::Graphics) && (props.queueFlags & vk::QueueFlagBits::eGraphics)) {
                roleToFamilies[QueueFamilyRole::Graphics] = i;
            }
            if (!roleToFamilies.count(QueueFamilyRole::Compute) && (props.queueFlags & vk::QueueFlagBits::eCompute)) {
                roleToFamilies[QueueFamilyRole::Compute] = i;
            }
            // Prefer dedicated transfer queue
            if (!roleToFamilies.count(QueueFamilyRole::Transfer) && (props.queueFlags & vk::QueueFlagBits::eTransfer) &&
                !(props.queueFlags & vk::QueueFlagBits::eGraphics) && !(props.queueFlags & vk::QueueFlagBits::eCompute)) {
                roleToFamilies[QueueFamilyRole::Transfer] = i;
            }
        }

        // Fallback: reuse existing family if dedicated not available
        if (!roleToFamilies.count(QueueFamilyRole::Transfer)) {
            // Transfer-capable family defaults to Graphics
            for (uint32_t i = 0; i < queueFamilies.size(); ++i) {
                if (queueFamilies[i].queueFlags & vk::QueueFlagBits::eTransfer) {
                    roleToFamilies[QueueFamilyRole::Transfer] = i;
                    break;
                }
            }
            if (!roleToFamilies.count(QueueFamilyRole::Transfer)) {
                roleToFamilies[QueueFamilyRole::Transfer] = roleToFamilies[QueueFamilyRole::Graphics];
            }
        }
        if (!roleToFamilies.count(QueueFamilyRole::Compute)) {
            roleToFamilies[QueueFamilyRole::Compute] = roleToFamilies[QueueFamilyRole::Graphics];
        }

        // Allocate physical queue indices within hardware queueCount limits
        std::map<uint32_t, uint32_t> familyAllocatedCount;
        std::vector<QueueFamilyRole> roleOrder = {QueueFamilyRole::Graphics, QueueFamilyRole::Compute, QueueFamilyRole::Transfer};

        for (QueueFamilyRole role : roleOrder) {
            uint32_t familyIdx = roleToFamilies[role];
            uint32_t maxAvailable = queueFamilies[familyIdx].queueCount; // 硬件上限
            uint32_t currentAllocated = familyAllocatedCount[familyIdx];

            if (currentAllocated < maxAvailable) {
                // 硬件还有空闲的物理队列，独占一个
                allocations_[role] = {familyIdx, currentAllocated};
                familyAllocatedCount[familyIdx]++;
            } else {
                // 硬件队列不够用了！只能和前面的人共享最后一个可用的物理队列
                allocations_[role] = {familyIdx, maxAvailable - 1};
            }
        }

        // Build vk::DeviceQueueCreateInfo with dynamic priority arrays
        // priorities_ must outlive vkCreateDevice, so stored as class member
        priorities_.clear();
        queueCreateInfos_.clear();

        for (const auto& [familyIdx, count] : familyAllocatedCount) {
            // 为当前 Family 创建独立的一组优先级数组（长度等于 count）
            size_t startIdx = priorities_.size();
            for (uint32_t i = 0; i < count; ++i) {
                priorities_.push_back(1.0f); // 默认优先级
            }

            queueCreateInfos_.emplace_back(
                vk::DeviceQueueCreateFlags{}, 
                familyIdx, 
                count, 
                &priorities_[startIdx] // 传入对应的子数组指针
            );
        }
    }

    const std::vector<vk::DeviceQueueCreateInfo>& getQueueCreateInfos() const {
        return queueCreateInfos_;
    }

    vk::Queue getQueue(const vk::Device& device, QueueFamilyRole role) const {
        auto it = allocations_.find(role);
        if (it == allocations_.end()) {
            throw std::runtime_error("Requested queue role not allocated!");
        }
        return device.getQueue(it->second.familyIndex, it->second.baseQueueIndex);
    }

    uint32_t getFamilyIndex(QueueFamilyRole role) const {
        return allocations_.at(role).familyIndex;
    }

void checkPresentAbility(const vk::raii::SurfaceKHR& surface) const {
    // Verify graphics queue family supports present on this surface
         uint32_t graphicsFamilyIdx = getFamilyIndex(QueueFamilyRole::Graphics);

         if (!physicalDevice_.getSurfaceSupportKHR(graphicsFamilyIdx, *surface)) {
             const auto errMsg = fmt::format("Graphics queue family (Index: {}) cannot present to surface", graphicsFamilyIdx);
             logger->critical(errMsg);
            throw std::runtime_error(
               errMsg
            );
        }
    }

private:
    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos_;
    std::vector<float> priorities_; 
    std::map<QueueFamilyRole, QueueAllocation> allocations_;
    const vk::raii::PhysicalDevice& physicalDevice_;
    RGL::RLLogger* logger;
};

}  // namespace RGL