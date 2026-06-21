module;
#include <vk_mem_alloc.h>
#include <stdexcept>
#include <cstdint>
#include <vulkan/vulkan_raii.hpp>
export module VmaRaii;

namespace RGL {

export struct VmaBuffer {
    VmaAllocator allocator = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    VmaBuffer() = default;
    VmaBuffer(VmaAllocator alloc, VkBuffer buf, VmaAllocation alloctn)
        : allocator(alloc), buffer(buf), allocation(alloctn) {}

    ~VmaBuffer() { if (allocator && buffer && allocation) vmaDestroyBuffer(allocator, buffer, allocation); }
    
    VmaBuffer(const VmaBuffer&) = delete;
    VmaBuffer& operator=(const VmaBuffer&) = delete;
    
    VmaBuffer(VmaBuffer&& o) noexcept : allocator(o.allocator), buffer(o.buffer), allocation(o.allocation) { 
        o.allocator = nullptr; o.buffer = VK_NULL_HANDLE; o.allocation = nullptr; 
    }
    
    VmaBuffer& operator=(VmaBuffer&& o) noexcept {
        if (this != &o) {
            if (allocator && buffer && allocation) vmaDestroyBuffer(allocator, buffer, allocation);
            allocator = o.allocator; buffer = o.buffer; allocation = o.allocation;
            o.allocator = nullptr; o.buffer = VK_NULL_HANDLE; o.allocation = nullptr;
        }
        return *this;
    }
    
    VkBuffer operator*() const { return buffer; }
    
    vk::DeviceAddress getAddress(const vk::raii::Device& device) const {
        vk::BufferDeviceAddressInfo addressInfo{};
        addressInfo.buffer = buffer;
        return device.getBufferAddress(addressInfo);
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
    VmaBuffer bufferStorage;       // 组合底层的 VmaBuffer 负责生命周期
    void* pMappedData = nullptr;   // CPU 端直接写入的映射首地址
    uint64_t gpuAddress = 0;       // 该大堆在 GPU 端的绝对物理地址 (BDA)
    uint32_t descriptorSize = 0;   // 当前显卡硬件上，单个描述符占用的字节数 (32B / 64B 等)
    uint32_t capacity = 0;         // 大堆的总格子容量

    VmaDescriptorHeap() = default;
    VmaDescriptorHeap(VmaBuffer&& buf, void* mapped, uint64_t gpuAddr, uint32_t descSize, uint32_t cap)
        : bufferStorage(std::move(buf)), pMappedData(mapped), gpuAddress(gpuAddr), descriptorSize(descSize), capacity(cap) {}

    ~VmaDescriptorHeap() = default; // 移动走后留下的空壳 bufferStorage 会安全析构，不释放任何东西

    VmaDescriptorHeap(const VmaDescriptorHeap&) = delete;
    VmaDescriptorHeap& operator=(const VmaDescriptorHeap&) = delete;

    VmaDescriptorHeap(VmaDescriptorHeap&& o) noexcept 
        : bufferStorage(std::move(o.bufferStorage))
        , pMappedData(o.pMappedData)
        , gpuAddress(o.gpuAddress)
        , descriptorSize(o.descriptorSize)
        , capacity(o.capacity) 
    {
        o.pMappedData = nullptr;
        o.gpuAddress = 0;
        o.descriptorSize = 0;
        o.capacity = 0;
    }

    VmaDescriptorHeap& operator=(VmaDescriptorHeap&& o) noexcept {
        if (this != &o) {
            bufferStorage = std::move(o.bufferStorage);
            pMappedData = o.pMappedData;
            gpuAddress = o.gpuAddress;
            descriptorSize = o.descriptorSize;
            capacity = o.capacity;

            o.pMappedData = nullptr;
            o.gpuAddress = 0;
            o.descriptorSize = 0;
            o.capacity = 0;
        }
        return *this;
    }

    VkBuffer operator*() const { return *bufferStorage; }
    uint64_t getGpuAddress() const { return gpuAddress; }
    uint32_t getDescriptorSize() const { return descriptorSize; }
    uint32_t getCapacity() const { return capacity; }

    // 💡 极其方便的辅助函数：直接获取第 N 个格子的 CPU 物理写入地址
    template<typename T = void>
    T* getSlotCpuAddress(uint32_t slot) const {
        if (slot >= capacity) throw std::out_of_range("Descriptor heap slot out of bounds!");
        return reinterpret_cast<T*>(static_cast<char*>(pMappedData) + (static_cast<size_t>(slot) * descriptorSize));
    }
};

// ==========================================
// 4. 完美的 VMA Allocator RAII 管家
// ==========================================
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

    VmaBuffer createBuffer(const vk::BufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo = nullptr) const {
        VkBuffer buffer;
        VmaAllocation allocation;
        if (vmaCreateBuffer(allocator, reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo), &allocInfo, &buffer, &allocation, pAllocInfo) != VK_SUCCESS) {
            throw std::runtime_error("VMA failed to create buffer");
        }
        return VmaBuffer(allocator, buffer, allocation);
    }

    VmaImage createImage(const vk::ImageCreateInfo& imageInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo = nullptr) const {
        VkImage image;
        VmaAllocation allocation;
        if (vmaCreateImage(allocator, reinterpret_cast<const VkImageCreateInfo*>(&imageInfo), &allocInfo, &image, &allocation, pAllocInfo) != VK_SUCCESS) {
            throw std::runtime_error("VMA failed to create image");
        }
        return VmaImage(allocator, image, allocation);
    }

    // 💡 ✨【新增】工厂方法 C：一键轰出一个现代硬件描述符大堆
    VmaDescriptorHeap createDescriptorHeap(
        const vk::raii::Device& device,
        uint32_t capacity,
        uint32_t descriptorSize,
        vk::BufferUsageFlags extraUsageFlags // 传入类似 eResourceDescriptorBufferEXT 或 eSamplerDescriptorBufferEXT 的标志
    ) const {
        vk::BufferCreateInfo bufferInfo{};
        bufferInfo.size = static_cast<vk::DeviceSize>(descriptorSize) * capacity;
        // 大堆必须有显存绝对寻址能力 (BDA)
        bufferInfo.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress | extraUsageFlags;

        // 大堆必须在 CPU 端可见，以便我们用写内存的方式烧录描述符
        VmaAllocationCreateInfo allocCreateInfo{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO
        };

        VmaAllocationInfo allocInfo;
        VmaBuffer storage = createBuffer(bufferInfo, allocCreateInfo, &allocInfo);
        
        uint64_t gpuAddress = storage.getAddress(device);

        return VmaDescriptorHeap(
            std::move(storage),
            allocInfo.pMappedData,
            gpuAddress,
            descriptorSize,
            capacity
        );
    }

    VmaAllocator get() const { return allocator; }
};

}  // namespace RGL