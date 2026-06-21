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
          depthImageView(vkDevice.getDevice(), buildDepthView(*depthImage))
    {
        createBuffers();
    }

    const RGL::VmaImage& getDepthImage() const { return depthImage; }
    const vk::raii::ImageView& getDepthImageView() const { return depthImageView; }
    const RGL::VmaBuffer& getVertexBuffer() const { return vertexBuffer; }
    const RGL::VmaBuffer& getIndexBuffer() const { return indexBuffer; }
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

    void createBuffers() {
        vk::BufferCreateInfo bufferCreateInfo{};
        bufferCreateInfo.size = RGL::cubeVertices.size() * sizeof(RGL::Vertex);
        bufferCreateInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer |
                                 vk::BufferUsageFlagBits::eShaderDeviceAddress;

        vk::BufferCreateInfo indexBufInfo{};
        indexBufInfo.size = RGL::cubeIndices.size() * sizeof(uint32_t);
        indexBufInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer |
                             vk::BufferUsageFlagBits::eShaderDeviceAddress;

        VmaAllocationCreateInfo allocCreateInfo{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO
        };

        VmaAllocationInfo allocInfo;
        vertexBuffer = allocator.createBuffer(bufferCreateInfo, allocCreateInfo, &allocInfo);
        std::memcpy(allocInfo.pMappedData, RGL::cubeVertices.data(), bufferCreateInfo.size);

        VmaAllocationInfo indexAllocInfo;
        indexBuffer = allocator.createBuffer(indexBufInfo, allocCreateInfo, &indexAllocInfo);
        std::memcpy(indexAllocInfo.pMappedData, RGL::cubeIndices.data(), indexBufInfo.size);
    }

    RGL::VkInstance& vkInstance_;
    RGL::VkDevice& vkDevice_;
    RGL::VkSwapchain& vkSwapchain_;
    RGL::VmaAllocatorRAII allocator;
    RGL::VmaImage depthImage;
    vk::raii::ImageView depthImageView;
    RGL::VmaBuffer vertexBuffer;
    RGL::VmaBuffer indexBuffer;
};

}
