module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include <cstring>
export module vk_resources;

import VmaRaii;
import vk_instance;
import vk_device;
import vk_swapchain;
import cube;

namespace RGL {

export class VkResources {
public:
    VkResources(RGL::VkInstance& vkInstance, RGL::VkDevice& vkDevice, RGL::VkSwapchain& vkSwapchain)
        : vkInstance_(vkInstance),
          vkDevice_(vkDevice),
          vkSwapchain_(vkSwapchain),
          allocator(*vkInstance.getInstance(), *vkDevice.getPhysicalDevice(), *vkDevice.getDevice(),
                    vkInstance.getApiVersion()),
          depthImage(createDepthImage(allocator, vkSwapchain.getExtent())),
          depthImageView(vkDevice.getDevice(), buildDepthView(*depthImage)),
        vertexBuffer(createGpuBuffer("vertex",
              RGL::cubeVertices.size() * sizeof(RGL::Vertex),
              vk::BufferUsageFlagBits::eStorageBuffer,
              RGL::cubeVertices.data())),
          indexBuffer(createGpuBuffer("index",
              RGL::cubeIndices.size() * sizeof(uint32_t),
              vk::BufferUsageFlagBits::eStorageBuffer,
              RGL::cubeIndices.data())),
          descriptorHeap(createDescriptorHeap())
    {
        writeDescriptors();
    }

    const RGL::VmaImage& getDepthImage() const { return depthImage; }
    const vk::raii::ImageView& getDepthImageView() const { return depthImageView; }

    void recreateDepthImage(vk::Extent2D extent) {
        vkDevice_.getDevice().waitIdle();

        depthImage = createDepthImage(allocator, extent);
        depthImageView = vk::raii::ImageView(vkDevice_.getDevice(), buildDepthView(*depthImage));
    }
    const RGL::VmaDescriptorHeap& getDescriptorHeap() const { return descriptorHeap; }
    vk::DeviceSize getReservedRangeOffset() const { return 0; }
    vk::DeviceSize getReservedRangeSize() const { return descriptorHeap.getReservedSlots() * descriptorHeap.getDescriptorSize(); }
    vk::DeviceAddress getVertexBufferAddress() const { return vertexBuffer.getDeviceAddress(); }
    vk::DeviceAddress getIndexBufferAddress() const { return indexBuffer.getDeviceAddress(); }
    RGL::VmaAllocatorRAII& getAllocator() { return allocator; }

private:
    static RGL::VmaImage createDepthImage(RGL::VmaAllocatorRAII& allocator, vk::Extent2D extent) {
        vk::ImageCreateInfo depthImageInfo{};
        depthImageInfo.imageType = vk::ImageType::e2D;
        depthImageInfo.format = vk::Format::eD32Sfloat;
        depthImageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
        depthImageInfo.mipLevels = 1;
        depthImageInfo.arrayLayers = 1;
        depthImageInfo.samples = vk::SampleCountFlagBits::e1;
        depthImageInfo.tiling = vk::ImageTiling::eOptimal;
        depthImageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;

        VmaAllocationCreateInfo depthAllocInfo{};
        depthAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        depthAllocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        return allocator.createImage(depthImageInfo, depthAllocInfo);
    }

    static vk::ImageViewCreateInfo buildDepthView(VkImage image) {
        vk::ImageViewCreateInfo depthViewInfo{};
        depthViewInfo.image = image;
        depthViewInfo.viewType = vk::ImageViewType::e2D;
        depthViewInfo.format = vk::Format::eD32Sfloat;
        depthViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
        depthViewInfo.subresourceRange.levelCount = 1;
        depthViewInfo.subresourceRange.layerCount = 1;
        return depthViewInfo;
    }

    RGL::VmaBuffer createGpuBuffer(const char* label, vk::DeviceSize size, vk::BufferUsageFlags extraUsage, const void* initData = nullptr) {
        vk::BufferCreateInfo bufferInfo{};
        bufferInfo.size = size;
        bufferInfo.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress | extraUsage;
        bufferInfo.sharingMode = vk::SharingMode::eExclusive;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        VmaAllocationInfo allocData;
        auto buf = allocator.createBufferWithAddress(vkDevice_.getDevice(), bufferInfo, allocInfo, &allocData);

        if (initData && size > 0) {
            std::memcpy(allocData.pMappedData, initData, size);
        }

        return buf;
    }

    RGL::VmaDescriptorHeap createDescriptorHeap() {
        VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProps{};
        heapProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;

        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &heapProps;

        vkGetPhysicalDeviceProperties2(*vkDevice_.getPhysicalDevice(), &props2);

        uint32_t descriptorSize = static_cast<uint32_t>(heapProps.bufferDescriptorSize);
        uint32_t reservedSlots = static_cast<uint32_t>(heapProps.minResourceHeapReservedRange + descriptorSize - 1) / descriptorSize;
        uint32_t capacity = 16 + reservedSlots;

        auto buf = allocator.createDescriptorHeap(
            vkDevice_.getDevice(),
            capacity, descriptorSize,
            vk::BufferUsageFlagBits::eDescriptorHeapEXT,
            reservedSlots
        );
        return buf;
    }

    void writeDescriptors() {
        auto& device = vkDevice_.getDevice();

        vk::DeviceAddressRangeEXT vertexRange{
            reinterpret_cast<VkDeviceAddress>(getVertexBufferAddress()),
            RGL::cubeVertices.size() * sizeof(RGL::Vertex)
        };
        vk::DeviceAddressRangeEXT indexRange{
            reinterpret_cast<VkDeviceAddress>(getIndexBufferAddress()),
            RGL::cubeIndices.size() * sizeof(uint32_t)
        };

        vk::ResourceDescriptorInfoEXT vertexDescInfo{};
        vertexDescInfo.type = vk::DescriptorType::eStorageBuffer;
        vertexDescInfo.data.pAddressRange = &vertexRange;

        vk::ResourceDescriptorInfoEXT indexDescInfo{};
        indexDescInfo.type = vk::DescriptorType::eStorageBuffer;
        indexDescInfo.data.pAddressRange = &indexRange;

        vk::HostAddressRangeEXT vertexDst{
            descriptorHeap.getSlotCpuAddress<void>(RGL::HeapSlotVertex),
            descriptorHeap.getDescriptorSize()
        };
        vk::HostAddressRangeEXT indexDst{
            descriptorHeap.getSlotCpuAddress<void>(RGL::HeapSlotIndex),
            descriptorHeap.getDescriptorSize()
        };

        device.writeResourceDescriptorsEXT(
            std::vector<vk::ResourceDescriptorInfoEXT>{vertexDescInfo, indexDescInfo},
            std::vector<vk::HostAddressRangeEXT>{vertexDst, indexDst}
        );
    }

    RGL::VkInstance& vkInstance_;
    RGL::VkDevice& vkDevice_;
    RGL::VkSwapchain& vkSwapchain_;
    RGL::VmaAllocatorRAII allocator;
    RGL::VmaImage depthImage;
    vk::raii::ImageView depthImageView;
    RGL::VmaBuffer vertexBuffer;
    RGL::VmaBuffer indexBuffer;
    RGL::VmaDescriptorHeap descriptorHeap;
};

}
