module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <fmt/format.h>
module vk_resources:impl;

import vk_resources;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import VmaRaii;
import vk_instance;
import vk_device;
import vk_swapchain;
import cube;
import vkerror;
import asset_table;
import types_generated;
import model_types_generated;

namespace RGL {

// Resources 重方法定义（ctor/sampler/sceneData/heap 创建/描述符写入，2026-09-02
// 编译提速 A 方案）：函数体移出接口 TU，BMI 只留类声明；本单元是叶子边。
//（ensure*/create* 均为启动期一次性路径，无跨 TU 内联价值）

Resources::Resources(RGL::AppContext& vkInstance, RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain)
	: vkInstance(vkInstance),
		  vkDevice(vkDevice),
		  vkSwapchain(vkSwapchain),
 	  // flags 显式传 BDA（第 5 参）——VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT 不可省略，
 	  // 否则所有 eShaderDeviceAddress buffer 创建返回 VK_ERROR_INITIALIZATION_FAILED
 	  allocator(*vkInstance.getInstance(), *vkDevice.getPhysicalDevice(), *vkDevice.getDevice(),
 		  vkInstance.getApiVersion(),
 		  VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT),
		  depthImage(createDepthImage(allocator, vkSwapchain.getExtent()).value()),
		  depthImageView(vkDevice.getDevice(), buildDepthView(*depthImage)),
		  vertexBuffer(createGpuBuffer(
			  RGL::cubeVertices.size() * sizeof(RGL::Vertex),
			  vk::BufferUsageFlagBits::eStorageBuffer,
			  RGL::cubeVertices.data())
				  .value()),
		  indexBuffer(createGpuBuffer(
			  RGL::cubeIndices.size() * sizeof(uint32_t),
			  vk::BufferUsageFlagBits::eStorageBuffer,
			  RGL::cubeIndices.data())
				  .value()),
		  descriptorHeap(createDescriptorHeap().value()),
		  samplerHeap(createSamplerHeap().value()),
		  nextSlot(reservedSlots()) {
		// cube 槽位经统一分配器（Phase 3）：布局约定由反射生成的偏移常量钉死，违例 = 契约漂移
		auto vertexSlotResult = allocSlot("cube.vertex");
		if (!vertexSlotResult)
			throw std::runtime_error(vertexSlotResult.error().describe());
		heapSlotVertex = *vertexSlotResult;
		if (heapSlotVertex != reservedSlots() + HeapSlotVertexOffset)
			throw std::runtime_error("heap slot layout drift: cube.vertex");
		auto indexSlotResult = allocSlot("cube.index");
		if (!indexSlotResult)
			throw std::runtime_error(indexSlotResult.error().describe());
		heapSlotIndex = *indexSlotResult;
		if (heapSlotIndex != reservedSlots() + HeapSlotIndexOffset)
			throw std::runtime_error("heap slot layout drift: cube.index");
		writeDescriptors();
		// 批次 3：cube 一等资产——buffer 移入资产记录（kind=Cube，带索引），包装体随
		// Resources 存活（与模型 mesh 同层）；成员 vertexBuffer/indexBuffer move 后为空，
		// 访问一律经 cubeAssetId() → 资产表（删了直接访问器防 move 后误用）
		cubeAsset = assetTable.addIndexedMesh(std::move(vertexBuffer), heapSlotVertex,
			std::move(indexBuffer), heapSlotIndex,
			static_cast<uint32_t>(RGL::cubeIndices.size() / 3));
	}

uint32_t Resources::ensureSharedSampler() {
		if (sharedSamplerReady)
			return sharedSamplerSlot;
		VkSamplerCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = VK_FILTER_LINEAR;
		info.minFilter = VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.anisotropyEnable = VK_FALSE;
		info.compareEnable = VK_FALSE;
		info.minLod = 0.0f;
		info.maxLod = 0.0f;
		info.unnormalizedCoordinates = VK_FALSE;
		info.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
		sharedSamplerSlot = samplerHeap.getReservedSlots();
		auto slotAddr = samplerHeap.getSlotCpuAddress(sharedSamplerSlot);
		if (!slotAddr)
			throw std::runtime_error(slotAddr.error().describe());
		vk::HostAddressRangeEXT dst{*slotAddr, samplerHeap.getDescriptorSize()};
		samplerCreateInfo = vk::SamplerCreateInfo(info);
		// hpp raii 内部 resultCheck（失败抛 LogicError）
		vkDevice.getDevice().writeSamplerDescriptorsEXT(samplerCreateInfo, dst);
		sharedSamplerReady = true;
		return sharedSamplerSlot;
	}

uint32_t Resources::ensureSkySampler(uint32_t mipLevels) {
		if (skySamplerReady)
			return skySamplerSlot;
		VkSamplerCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = VK_FILTER_LINEAR;
		info.minFilter = VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.anisotropyEnable = VK_FALSE;
		info.compareEnable = VK_FALSE;
		info.minLod = 0.0f;
		info.maxLod = static_cast<float>(mipLevels - 1);
		info.unnormalizedCoordinates = VK_FALSE;
		info.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
		skySamplerSlot = samplerHeap.getReservedSlots() + 1;
		auto slotAddr = samplerHeap.getSlotCpuAddress(skySamplerSlot);
		if (!slotAddr)
			throw std::runtime_error(slotAddr.error().describe());
		vk::HostAddressRangeEXT dst{*slotAddr, samplerHeap.getDescriptorSize()};
		skySamplerCreateInfo = vk::SamplerCreateInfo(info);
		vkDevice.getDevice().writeSamplerDescriptorsEXT(skySamplerCreateInfo, dst);
		skySamplerReady = true;
		return skySamplerSlot;
	}

uint32_t Resources::ensureSceneData() {
		if (sceneDataReady)
			return sceneDataSlot;
		vk::BufferCreateInfo info{};
		info.size = kSceneDataBytes;
		info.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress |
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
		VmaAllocationCreateInfo alloc{};
		// device-local：CPU 不直接写（每帧唯一写入方 = staging copy），勿用 createGpuBuffer 的
		// CPU_TO_GPU+MAPPED 形态
		alloc.usage = VMA_MEMORY_USAGE_AUTO;
		auto bufResult = allocator.createBufferWithAddress(vkDevice.getDevice(), info, alloc);
		if (!bufResult)
			throw std::runtime_error(bufResult.error().describe());
		sceneDataBuffer = std::move(*bufResult);

		auto slotResult = allocSlot("scene.data");
		if (!slotResult)
			throw std::runtime_error(slotResult.error().describe());
		sceneDataSlot = *slotResult;

		vk::DeviceAddressRangeEXT range{sceneDataBuffer.getDeviceAddress(), kSceneDataBytes};
		vk::ResourceDescriptorInfoEXT desc{};
		desc.type = vk::DescriptorType::eStorageBuffer;
		desc.data.pAddressRange = &range;
		auto slotAddr = descriptorHeap.getSlotCpuAddress(sceneDataSlot);
		if (!slotAddr)
			throw std::runtime_error(slotAddr.error().describe());
		vk::HostAddressRangeEXT dst{*slotAddr, descriptorHeap.getDescriptorSize()};
		vkDevice.getDevice().writeResourceDescriptorsEXT(
			std::array<vk::ResourceDescriptorInfoEXT, 1>{desc},
			std::array<vk::HostAddressRangeEXT, 1>{dst});
		sceneDataReady = true;
		return sceneDataSlot;
	}

std::expected<RGL::VmaImage, Error> Resources::createDepthImage(RGL::VmaAllocatorRAII& allocator, vk::Extent2D extent) {
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

vk::ImageViewCreateInfo Resources::buildDepthView(VkImage image) {
		vk::ImageViewCreateInfo depthViewInfo{};
		depthViewInfo.image = image;
		depthViewInfo.viewType = vk::ImageViewType::e2D;
		depthViewInfo.format = vk::Format::eD32Sfloat;
		depthViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
		depthViewInfo.subresourceRange.levelCount = 1;
		depthViewInfo.subresourceRange.layerCount = 1;
		return depthViewInfo;
	}

std::expected<RGL::VmaBuffer, Error> Resources::createGpuBuffer(vk::DeviceSize size, vk::BufferUsageFlags extraUsage, const void* initData) {
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

std::expected<RGL::VmaDescriptorHeap, Error> Resources::createDescriptorHeap() {
		auto props = vkDevice.getPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();

		auto& heapProps = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		// Phase 3 统一步长：image 描述符（SAMPLED_IMAGE）比 buffer 描述符宽
		//（RADV 32B > 16B，docs/archive/heap_texture_research.md Q1），slot 步长取 max（运行时查询，勿硬编码）
		uint32_t bufferSize = static_cast<uint32_t>(heapProps.bufferDescriptorSize);
		uint32_t imageSize = static_cast<uint32_t>(heapProps.imageDescriptorSize);
		uint32_t descriptorSize = std::max(bufferSize, imageSize);
		uint32_t reservedSlots = static_cast<uint32_t>(heapProps.minResourceHeapReservedRange + descriptorSize - 1) / descriptorSize;
		vk::DeviceSize resourceHeapAlignment = heapProps.resourceHeapAlignment;
		uint32_t userSlots = HeapSlotModelVertexBase + kUserSlotMargin;
		uint32_t capacity = userSlots + reservedSlots;
		if (auto* logger = RLLogger::getInstance())
			logger->info("descriptor heap: bufferDesc={}B imageDesc={}B stride={}B reservedSlots={} alignment={}B capacity={}",
				bufferSize, imageSize, descriptorSize, reservedSlots, resourceHeapAlignment, capacity);

		return allocator.createDescriptorHeap(
			vkDevice.getDevice(),
			capacity, descriptorSize,
			vk::BufferUsageFlagBits::eDescriptorHeapEXT,
			reservedSlots,
			resourceHeapAlignment);
	}

std::expected<RGL::VmaDescriptorHeap, Error> Resources::createSamplerHeap() {
		auto props = vkDevice.getPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		auto& heapProps = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		uint32_t samplerSize = static_cast<uint32_t>(heapProps.samplerDescriptorSize);
		uint32_t reservedSlots = static_cast<uint32_t>(heapProps.minSamplerHeapReservedRange + samplerSize - 1) / samplerSize;
		uint32_t capacity = reservedSlots + 8;
		if (auto* logger = RLLogger::getInstance())
			logger->info("sampler heap: samplerDesc={}B reservedSlots={} alignment={}B capacity={}",
				samplerSize, reservedSlots, heapProps.samplerHeapAlignment, capacity);

		return allocator.createDescriptorHeap(
			vkDevice.getDevice(),
			capacity, samplerSize,
			vk::BufferUsageFlagBits::eDescriptorHeapEXT,
			reservedSlots,
			heapProps.samplerHeapAlignment);
	}

void Resources::writeDescriptors() {
		auto& device = vkDevice.getDevice();

		// 构造体内调用（cube buffer 移入资产表前）：直接用成员取地址
		vk::DeviceAddressRangeEXT vertexRange{
			vertexBuffer.getDeviceAddress(),
			RGL::cubeVertices.size() * sizeof(RGL::Vertex)};
		vk::DeviceAddressRangeEXT indexRange{
			indexBuffer.getDeviceAddress(),
			RGL::cubeIndices.size() * sizeof(uint32_t)};

		vk::ResourceDescriptorInfoEXT vertexDescInfo{};
		vertexDescInfo.type = vk::DescriptorType::eStorageBuffer;
		vertexDescInfo.data.pAddressRange = &vertexRange;

		vk::ResourceDescriptorInfoEXT indexDescInfo{};
		indexDescInfo.type = vk::DescriptorType::eStorageBuffer;
		indexDescInfo.data.pAddressRange = &indexRange;

		auto vertexSlotResult = descriptorHeap.getSlotCpuAddress(heapSlotVertex);
		if (!vertexSlotResult) {
			throw std::runtime_error(vertexSlotResult.error().describe());
		}

		auto indexSlotResult = descriptorHeap.getSlotCpuAddress(heapSlotIndex);
		if (!indexSlotResult) {
			throw std::runtime_error(indexSlotResult.error().describe());
		}

		vk::HostAddressRangeEXT vertexDst{
			*vertexSlotResult,
			descriptorHeap.getDescriptorSize()};
		vk::HostAddressRangeEXT indexDst{
			*indexSlotResult,
			descriptorHeap.getDescriptorSize()};

		device.writeResourceDescriptorsEXT(
			std::array<vk::ResourceDescriptorInfoEXT, 2>{vertexDescInfo, indexDescInfo},
			std::array<vk::HostAddressRangeEXT, 2>{vertexDst, indexDst});
}
}  // namespace RGL
