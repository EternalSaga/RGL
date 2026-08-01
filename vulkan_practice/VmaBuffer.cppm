module;
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
export module VmaRaii;

import rlstd;
import vkerror;

namespace RGL {

export struct VmaBuffer {
    VmaAllocator allocator = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    vk::DeviceAddress deviceAddress = 0;
    void* pMappedData = nullptr;

    VmaBuffer() = default;
    VmaBuffer(VmaAllocator alloc, VkBuffer buf, VmaAllocation alloctn, vk::DeviceAddress addr = 0, void* mapped = nullptr)
        : allocator(alloc), buffer(buf), allocation(alloctn), deviceAddress(addr), pMappedData(mapped) {}

    ~VmaBuffer() { if (allocator && buffer && allocation) vmaDestroyBuffer(allocator, buffer, allocation); }

    VmaBuffer(const VmaBuffer&) = delete;
    VmaBuffer& operator=(const VmaBuffer&) = delete;

    VmaBuffer(VmaBuffer&& o) noexcept : allocator(o.allocator), buffer(o.buffer), allocation(o.allocation), deviceAddress(o.deviceAddress), pMappedData(o.pMappedData) {
        o.allocator = nullptr; o.buffer = VK_NULL_HANDLE; o.allocation = nullptr; o.deviceAddress = 0; o.pMappedData = nullptr;
    }

    VmaBuffer& operator=(VmaBuffer&& o) noexcept {
        if (this != &o) {
            if (allocator && buffer && allocation) vmaDestroyBuffer(allocator, buffer, allocation);
            allocator = o.allocator; buffer = o.buffer; allocation = o.allocation; deviceAddress = o.deviceAddress; pMappedData = o.pMappedData;
            o.allocator = nullptr; o.buffer = VK_NULL_HANDLE; o.allocation = nullptr; o.deviceAddress = 0; o.pMappedData = nullptr;
        }
        return *this;
    }

    VkBuffer operator*() const { return buffer; }
    vk::DeviceAddress getDeviceAddress() const { return deviceAddress; }
    void* getMappedData() const { return pMappedData; }
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

    VmaImage(VmaImage&& o) noexcept : allocator(o.allocator), image(o.image), allocation(o.allocation) {
        o.allocator = nullptr; o.image = VK_NULL_HANDLE; o.allocation = nullptr;
    }

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

export struct VmaDescriptorHeap {
    VmaBuffer bufferStorage;
    void* pMappedData = nullptr;
    uint64_t gpuAddress = 0;
    uint32_t descriptorSize = 0;
    uint32_t capacity = 0;
    uint32_t reservedSlots = 0;

    VmaDescriptorHeap() = default;
    VmaDescriptorHeap(VmaBuffer&& buf, void* mapped, uint64_t gpuAddr, uint32_t descSize, uint32_t cap, uint32_t reserved = 0)
        : bufferStorage(std::move(buf)), pMappedData(mapped), gpuAddress(gpuAddr), descriptorSize(descSize), capacity(cap), reservedSlots(reserved) {}

    ~VmaDescriptorHeap() = default;

    VmaDescriptorHeap(const VmaDescriptorHeap&) = delete;
    VmaDescriptorHeap& operator=(const VmaDescriptorHeap&) = delete;

    VmaDescriptorHeap(VmaDescriptorHeap&& o) noexcept
        : bufferStorage(std::move(o.bufferStorage))
        , pMappedData(o.pMappedData)
        , gpuAddress(o.gpuAddress)
        , descriptorSize(o.descriptorSize)
        , capacity(o.capacity)
        , reservedSlots(o.reservedSlots)
    {
        o.pMappedData = nullptr;
        o.gpuAddress = 0;
        o.descriptorSize = 0;
        o.capacity = 0;
        o.reservedSlots = 0;
    }

    VmaDescriptorHeap& operator=(VmaDescriptorHeap&& o) noexcept {
        if (this != &o) {
            bufferStorage = std::move(o.bufferStorage);
            pMappedData = o.pMappedData;
            gpuAddress = o.gpuAddress;
            descriptorSize = o.descriptorSize;
            capacity = o.capacity;
            reservedSlots = o.reservedSlots;

            o.pMappedData = nullptr;
            o.gpuAddress = 0;
            o.descriptorSize = 0;
            o.capacity = 0;
            o.reservedSlots = 0;
        }
        return *this;
    }

    VkBuffer operator*() const { return *bufferStorage; }
    uint64_t getGpuAddress() const { return gpuAddress; }
    uint32_t getDescriptorSize() const { return descriptorSize; }
    uint32_t getCapacity() const { return capacity; }
    uint32_t getReservedSlots() const { return reservedSlots; }

    [[nodiscard]] std::expected<void*, Error> getSlotCpuAddress(uint32_t slot) const {
        if (slot >= capacity) {
            return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
                "Descriptor heap slot out of bounds!"});
        }
        return static_cast<char*>(pMappedData) + (static_cast<size_t>(slot) * descriptorSize);
    }
};

export class VmaAllocatorRAII {
private:
    VmaAllocator allocator = nullptr;
public:
    VmaAllocatorRAII(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion,
                     VmaAllocatorCreateFlags flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT) {
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

    [[nodiscard]] std::expected<VmaBuffer, Error> createBuffer(const vk::BufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo = nullptr) const {
        VkBuffer buffer;
        VmaAllocation allocation;
        VkResult result = vmaCreateBuffer(allocator, reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo), &allocInfo, &buffer, &allocation, pAllocInfo);
        if (result != VK_SUCCESS) {
            return std::unexpected(Error{ErrorCode::VMASuccessFailed,
                "VMA failed to create buffer"});
        }
        return VmaBuffer(allocator, buffer, allocation);
    }

    [[nodiscard]] std::expected<VmaBuffer, Error> createBufferWithAddress(const vk::raii::Device& device, const vk::BufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo = nullptr) const {
        VkBuffer buffer;
        VmaAllocation allocation;
        VkResult result = vmaCreateBuffer(allocator, reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo), &allocInfo, &buffer, &allocation, pAllocInfo);
        if (result != VK_SUCCESS) {
            return std::unexpected(Error{ErrorCode::VMASuccessFailed, "VMA failed to create buffer"});
        }
        vk::BufferDeviceAddressInfo addrInfo{};
        addrInfo.buffer = buffer;
        vk::DeviceAddress addr = device.getBufferAddress(addrInfo);
        return VmaBuffer(allocator, buffer, allocation, addr, pAllocInfo ? pAllocInfo->pMappedData : nullptr);
    }

    [[nodiscard]] std::expected<VmaImage, Error> createImage(const vk::ImageCreateInfo& imageInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo = nullptr) const {
        VkImage image;
        VmaAllocation allocation;
        VkResult result = vmaCreateImage(allocator, reinterpret_cast<const VkImageCreateInfo*>(&imageInfo), &allocInfo, &image, &allocation, pAllocInfo);
        if (result != VK_SUCCESS) {
            return std::unexpected(Error{ErrorCode::VMASuccessFailed, "VMA failed to create image"});
        }
        return VmaImage(allocator, image, allocation);
    }

    [[nodiscard]] std::expected<VmaDescriptorHeap, Error> createDescriptorHeap(
        const vk::raii::Device& device,
        uint32_t capacity,
        uint32_t descriptorSize,
        vk::BufferUsageFlags extraUsageFlags,
        uint32_t reservedSlots = 0
    ) const {
        vk::BufferCreateInfo bufferInfo{};
        bufferInfo.size = static_cast<vk::DeviceSize>(descriptorSize) * capacity;
        bufferInfo.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress | extraUsageFlags;

        VmaAllocationCreateInfo allocCreateInfo{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO
        };

        VmaAllocationInfo allocInfo;
        auto storageResult = createBufferWithAddress(device, bufferInfo, allocCreateInfo, &allocInfo);
        if (!storageResult) return std::unexpected(storageResult.error());

        return VmaDescriptorHeap(
            std::move(*storageResult),
            allocInfo.pMappedData,
            storageResult->getDeviceAddress(),
            descriptorSize,
            capacity,
            reservedSlots
        );
    }

    VmaAllocator get() const { return allocator; }
};

}  // namespace RGL