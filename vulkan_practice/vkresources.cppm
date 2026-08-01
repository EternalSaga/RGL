module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
export module vk_resources;

import rlstd;
import VmaRaii;
import vk_instance;
import vk_device;
import vk_swapchain;
import types_generated;
import cube;
import vkerror;

namespace RGL {

export class Resources {
public:
    Resources(RGL::AppContext& vkInstance, RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain)
        : vkInstance(vkInstance),
          vkDevice(vkDevice),
          vkSwapchain(vkSwapchain),
          allocator(*vkInstance.getInstance(), *vkDevice.getPhysicalDevice(), *vkDevice.getDevice(),
                    vkInstance.getApiVersion()),
          depthImage(createDepthImage(allocator, vkSwapchain.getExtent()).value()),
          depthImageView(vkDevice.getDevice(), buildDepthView(*depthImage)),
          vertexBuffer(createGpuBuffer(
              RGL::cubeVertices.size() * sizeof(RGL::Vertex),
              vk::BufferUsageFlagBits::eStorageBuffer,
              RGL::cubeVertices.data()).value()),
          indexBuffer(createGpuBuffer(
              RGL::cubeIndices.size() * sizeof(uint32_t),
              vk::BufferUsageFlagBits::eStorageBuffer,
              RGL::cubeIndices.data()).value()),
          descriptorHeap(createDescriptorHeap().value())
    {
        writeDescriptors();
    }

    const RGL::VmaImage& getDepthImage() const { return depthImage; }
    const vk::raii::ImageView& getDepthImageView() const { return depthImageView; }

    void recreateDepthImage(vk::Extent2D extent) {
        vkDevice.getDevice().waitIdle();

        auto result = createDepthImage(allocator, extent);
        if (!result) throw std::runtime_error(result.error().describe());
        depthImage = std::move(*result);
        depthImageView = vk::raii::ImageView(vkDevice.getDevice(), buildDepthView(*depthImage));
    }

    const RGL::VmaDescriptorHeap& getDescriptorHeap() const { return descriptorHeap; }
    vk::DeviceSize getReservedRangeOffset() const { return 0; }
    vk::DeviceSize getReservedRangeSize() const { return descriptorHeap.getReservedSlots() * descriptorHeap.getDescriptorSize(); }
    vk::DeviceAddress getVertexBufferAddress() const { return vertexBuffer.getDeviceAddress(); }
    vk::DeviceAddress getIndexBufferAddress() const { return indexBuffer.getDeviceAddress(); }
    RGL::VmaAllocatorRAII& getAllocator() { return allocator; }

private:
    static std::expected<RGL::VmaImage, Error> createDepthImage(RGL::VmaAllocatorRAII& allocator, vk::Extent2D extent) {
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

    std::expected<RGL::VmaBuffer, Error> createGpuBuffer(vk::DeviceSize size, vk::BufferUsageFlags extraUsage, const void* initData = nullptr) {
        vk::BufferCreateInfo bufferInfo{};
        bufferInfo.size = size;
        bufferInfo.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress | extraUsage;
        bufferInfo.sharingMode = vk::SharingMode::eExclusive;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        VmaAllocationInfo allocData;
        auto result = allocator.createBufferWithAddress(vkDevice.getDevice(), bufferInfo, allocInfo, &allocData);
        if (!result) return std::unexpected(result.error());

        auto buf = std::move(*result);

        if (initData && size > 0) {
            std::memcpy(allocData.pMappedData, initData, size);
        }

        return buf;
    }

    std::expected<RGL::VmaDescriptorHeap, Error> createDescriptorHeap() {
        auto props = vkDevice.getPhysicalDevice().getProperties2<
            vk::PhysicalDeviceProperties2,
            vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();

        uint32_t descriptorSize = static_cast<uint32_t>(
            props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>().bufferDescriptorSize);
        auto& heapProps = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
        uint32_t reservedSlots = static_cast<uint32_t>(heapProps.minResourceHeapReservedRange + descriptorSize - 1) / descriptorSize;
        uint32_t capacity = 16 + reservedSlots;

        return allocator.createDescriptorHeap(
            vkDevice.getDevice(),
            capacity, descriptorSize,
            vk::BufferUsageFlagBits::eDescriptorHeapEXT,
            reservedSlots
        );
    }

    void writeDescriptors() {
        auto& device = vkDevice.getDevice();

        vk::DeviceAddressRangeEXT vertexRange{
            getVertexBufferAddress(),
            RGL::cubeVertices.size() * sizeof(RGL::Vertex)
        };
        vk::DeviceAddressRangeEXT indexRange{
            getIndexBufferAddress(),
            RGL::cubeIndices.size() * sizeof(uint32_t)
        };

        vk::ResourceDescriptorInfoEXT vertexDescInfo{};
        vertexDescInfo.type = vk::DescriptorType::eStorageBuffer;
        vertexDescInfo.data.pAddressRange = &vertexRange;

        vk::ResourceDescriptorInfoEXT indexDescInfo{};
        indexDescInfo.type = vk::DescriptorType::eStorageBuffer;
        indexDescInfo.data.pAddressRange = &indexRange;

        auto vertexSlotResult = descriptorHeap.getSlotCpuAddress(RGL::HeapSlotVertex);
        if (!vertexSlotResult) return;

        auto indexSlotResult = descriptorHeap.getSlotCpuAddress(RGL::HeapSlotIndex);
        if (!indexSlotResult) return;

        vk::HostAddressRangeEXT vertexDst{
            *vertexSlotResult,
            descriptorHeap.getDescriptorSize()
        };
        vk::HostAddressRangeEXT indexDst{
            *indexSlotResult,
            descriptorHeap.getDescriptorSize()
        };

        device.writeResourceDescriptorsEXT(
            std::array<vk::ResourceDescriptorInfoEXT, 2>{vertexDescInfo, indexDescInfo},
            std::array<vk::HostAddressRangeEXT, 2>{vertexDst, indexDst}
        );
    }

    RGL::AppContext& vkInstance;
    RGL::DeviceContext& vkDevice;
    RGL::Swapchain& vkSwapchain;
    RGL::VmaAllocatorRAII allocator;
    RGL::VmaImage depthImage;
    vk::raii::ImageView depthImageView;
    RGL::VmaBuffer vertexBuffer;
    RGL::VmaBuffer indexBuffer;
    RGL::VmaDescriptorHeap descriptorHeap;
};

}

