module;
#include <vk_mem_alloc.h>
#include <stdexcept>
#include <cstdint>
#include <vulkan/vulkan_raii.hpp>
export module VmaRaii;


export struct VmaBuffer {
    VmaAllocator allocator = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    VmaBuffer() = default;
    VmaBuffer(VmaAllocator alloc, VkBuffer buf, VmaAllocation alloctn)
        : allocator(alloc), buffer(buf), allocation(alloctn) {}

    ~VmaBuffer() { if (allocator && buffer && allocation) vmaDestroyBuffer(allocator, buffer, allocation); }
    
    // 移动语义（禁止拷贝）
    VmaBuffer(const VmaBuffer&) = delete;
    VmaBuffer& operator=(const VmaBuffer&) = delete;
    VmaBuffer(VmaBuffer&& o) noexcept : allocator(o.allocator), buffer(o.buffer), allocation(o.allocation) { o.allocator = nullptr; o.buffer = VK_NULL_HANDLE; o.allocation = nullptr; }
    VmaBuffer& operator=(VmaBuffer&& o) noexcept {
        if (this != &o) {
            if (allocator && buffer && allocation) vmaDestroyBuffer(allocator, buffer, allocation);
            allocator = o.allocator; buffer = o.buffer; allocation = o.allocation;
            o.allocator = nullptr; o.buffer = VK_NULL_HANDLE; o.allocation = nullptr;
        }
        return *this;
    }
    VkBuffer operator*() const { return buffer; }
    uint64_t getAddress(const vk::raii::Device& device) const {
        vk::BufferDeviceAddressInfo addressInfo{};
        addressInfo.buffer = buffer;
        uint64_t address = device.getBufferAddress(addressInfo);
        return address;
    }

};

    

export struct VmaImage {
    VmaAllocator allocator = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    VmaImage() = default;
    VmaImage(VmaAllocator alloc, VkImage img, VmaAllocation alloctn)
        : allocator(alloc), image(img), allocation(alloctn) {}

    ~VmaImage() { if (allocator && image && allocation) vmaDestroyImage(allocator, image, allocation); }

    VmaImage(const VmaImage&) = delete;
    VmaImage& operator=(const VmaImage&) = delete;
    VmaImage(VmaImage&& o) noexcept : allocator(o.allocator), image(o.image), allocation(o.allocation) { o.allocator = nullptr; o.image = VK_NULL_HANDLE; o.allocation = nullptr; }
    VmaImage& operator=(VmaImage&& o) noexcept {
        if (this != &o) {
            if (allocator && image && allocation) vmaDestroyImage(allocator, image, allocation);
            allocator = o.allocator; image = o.image; allocation = o.allocation;
            o.allocator = nullptr; o.image = VK_NULL_HANDLE; o.allocation = nullptr;
        }
        return *this;
    }
    VkImage operator*() const { return image; }
};

// 2. 完美的 VMA Allocator RAII 管家
export class VmaAllocatorRAII {
private:
    VmaAllocator allocator = nullptr;
public:
    VmaAllocatorRAII(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion,VmaAllocatorCreateFlagBits flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT) {
        VmaAllocatorCreateInfo allocatorInfo{
            .flags = flags,
            .physicalDevice = physicalDevice,
            .device = device,
            .instance = instance,
            .vulkanApiVersion = apiVersion
        };
        if (vmaCreateAllocator(&allocatorInfo, &allocator) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create VMA Allocator");
        }
    }



    ~VmaAllocatorRAII() { if (allocator) vmaDestroyAllocator(allocator); }

    VmaAllocatorRAII(const VmaAllocatorRAII&) = delete;
    VmaAllocatorRAII& operator=(const VmaAllocatorRAII&) = delete;
    VmaAllocatorRAII(VmaAllocatorRAII&& o) noexcept : allocator(o.allocator) { o.allocator = nullptr; }

    // 工厂方法 A：创建 Buffer 并直接返回 RAII 包装
    VmaBuffer createBuffer(const vk::BufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo = nullptr) const {
        VkBuffer buffer;
        VmaAllocation allocation;
        // 利用 vulkan.hpp 结构体与原生 C 结构体二进制兼容的特性，干净利落地强转
        if (vmaCreateBuffer(allocator, reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo), &allocInfo, &buffer, &allocation, pAllocInfo) != VK_SUCCESS) {
            throw std::runtime_error("VMA failed to create buffer");
        }
        return VmaBuffer(allocator, buffer, allocation);
    }

    // 工厂方法 B：创建 Image 并直接返回 RAII 包装
    VmaImage createImage(const vk::ImageCreateInfo& imageInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo = nullptr) const {
        VkImage image;
        VmaAllocation allocation;
        if (vmaCreateImage(allocator, reinterpret_cast<const VkImageCreateInfo*>(&imageInfo), &allocInfo, &image, &allocation, pAllocInfo) != VK_SUCCESS) {
            throw std::runtime_error("VMA failed to create image");
        }
        return VmaImage(allocator, image, allocation);
    }

    VmaAllocator get() const { return allocator; }
};