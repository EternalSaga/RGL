module;
#include "rllogger.hpp"
#include <stdexcept>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <cstdint>
#include <map>
#include <vector>
export module queue_family_manager;
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
    QueueManager(const vk::raii::PhysicalDevice& physicalDevice) {
        auto queueFamilies = physicalDevice.getQueueFamilyProperties();
        std::map<QueueFamilyRole, uint32_t> roleToFamilies;

        // 1. 初步筛选：寻找最符合条件的 Queue Family
        for (uint32_t i = 0; i < queueFamilies.size(); ++i) {
            const auto& props = queueFamilies[i];
            
            if (!roleToFamilies.count(QueueFamilyRole::Graphics) && (props.queueFlags & vk::QueueFlagBits::eGraphics)) {
                roleToFamilies[QueueFamilyRole::Graphics] = i;
            }
            if (!roleToFamilies.count(QueueFamilyRole::Compute) && (props.queueFlags & vk::QueueFlagBits::eCompute)) {
                roleToFamilies[QueueFamilyRole::Compute] = i;
            }
            // 尽量找纯粹的 Transfer 队列
            if (!roleToFamilies.count(QueueFamilyRole::Transfer) && (props.queueFlags & vk::QueueFlagBits::eTransfer) &&
                !(props.queueFlags & vk::QueueFlagBits::eGraphics) && !(props.queueFlags & vk::QueueFlagBits::eCompute)) {
                roleToFamilies[QueueFamilyRole::Transfer] = i;
            }
        }

        // 2. 回退机制：如果没有独立的，就复用
        if (!roleToFamilies.count(QueueFamilyRole::Transfer)) {
            // 如果连 Transfer 标志都没找到，找一个支持 Transfer 的（通常 Graphics 默认带 Transfer 能力）
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

        // 3. 核心修正：安全地分配物理队列索引（严格遵守硬件 queueCount 上限）
        std::map<uint32_t, uint32_t> familyAllocatedCount; // 记录该家族实际申请了多少个物理队列
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

        // 4. 正确构建 vk::DeviceQueueCreateInfo，动态生成优先级数组
        // 因为 priorities_ 必须在 vkCreateDevice 调用期间保持存活，所以作为类成员变量存储
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

private:
    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos_;
    std::vector<float> priorities_; 
    std::map<QueueFamilyRole, QueueAllocation> allocations_;
};