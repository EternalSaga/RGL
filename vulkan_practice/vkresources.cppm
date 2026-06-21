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
          descriptorHeap(createDescriptorHeap()),
          vertexOffset(allocateVertexData()),
          indexOffset(allocateIndexData())
    {
    }

    const RGL::VmaImage& getDepthImage() const { return depthImage; }
    const vk::raii::ImageView& getDepthImageView() const { return depthImageView; }
    const RGL::VmaDescriptorHeap& getDescriptorHeap() const { return descriptorHeap; }
    uint32_t getVertexOffset() const { return vertexOffset; }
    uint32_t getIndexOffset() const { return indexOffset; }
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

    RGL::VmaDescriptorHeap createDescriptorHeap() {
        constexpr uint32_t descriptorSize = 256;
        constexpr uint32_t capacity = 4;
        return allocator.createDescriptorHeap(
            vkDevice_.getDevice(),
            capacity, descriptorSize,
            vk::BufferUsageFlagBits::eStorageBuffer
        );
    }

    uint32_t allocateVertexData() {
        uint32_t slot = 0;
        void* cpuAddr = descriptorHeap.getSlotCpuAddress<char>(slot);
        std::memcpy(cpuAddr, RGL::cubeVertices.data(),
                     RGL::cubeVertices.size() * sizeof(RGL::Vertex));
        return slot * descriptorHeap.getDescriptorSize();
    }

    uint32_t allocateIndexData() {
        uint32_t slot = 1;
        void* cpuAddr = descriptorHeap.getSlotCpuAddress<char>(slot);
        std::memcpy(cpuAddr, RGL::cubeIndices.data(),
                     RGL::cubeIndices.size() * sizeof(uint32_t));
        return slot * descriptorHeap.getDescriptorSize();
    }

    RGL::VkInstance& vkInstance_;
    RGL::VkDevice& vkDevice_;
    RGL::VkSwapchain& vkSwapchain_;
    RGL::VmaAllocatorRAII allocator;
    RGL::VmaImage depthImage;
    vk::raii::ImageView depthImageView;
    RGL::VmaDescriptorHeap descriptorHeap;
    uint32_t vertexOffset;
    uint32_t indexOffset;
};

}
