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
import vkerror;

namespace RGL {

// Resources 重方法定义（ctor/sampler/sceneData/heap 创建，2026-09-02
// 编译提速 A 方案）：函数体移出接口 TU，BMI 只留类声明；本单元是叶子边。
//（ensure*/create* 均为启动期一次性路径，无跨 TU 内联价值）

/**
 * 初始化列表顺序与 heap 槽位布局契约见接口注释。实现补充：allocator 的 flags 显式传
 * BDA（第 5 参）——VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT 不可省略，否则所有
 * eShaderDeviceAddress buffer 创建返回 VK_ERROR_INITIALIZATION_FAILED（见体内注释）；
 * 内核不创建任何内容资产（架构债务 1 方案 B——几何/纹理/scene data/RT 采样槽全部由
 * 内容层经 allocSlot 按用途段领取，见体内注释）。
 */
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
		  descriptorHeap(createDescriptorHeap().value()),
		  samplerHeap(createSamplerHeap().value()) {
		initializeSlotSegments(reservedSlots());
		// 内核不创建任何内容资产（架构债务 1 方案 B）：几何（内建 cube / gltf 模型）、
		// 纹理、scene data、RT 采样槽全部由内容层经 allocSlot 按用途段领取
	}













/** 参数/返回见接口注释。实现：eD32Sfloat、单采样、eDepthStencilAttachment、
 * VMA 专用内存（VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT）。 */
std::expected<RGL::VmaImage, Error> Resources::createDepthImage(RGL::VmaAllocatorRAII& allocator, vk::Extent2D extent) {
		vk::ImageCreateInfo depthImageInfo{};
		depthImageInfo.imageType = vk::ImageType::e2D;
		depthImageInfo.format = vk::Format::eD32Sfloat;
		depthImageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
		depthImageInfo.mipLevels = 1;
		depthImageInfo.arrayLayers = 1;
		depthImageInfo.samples = vk::SampleCountFlagBits::e1;
		depthImageInfo.tiling = vk::ImageTiling::eOptimal;
		depthImageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
			// Phase 11 批 11.4 SSAO：ao.frag 以 SAMPLED_IMAGE 读深度重建世界坐标
			//（见 Resources::ensureDepthSampleSlot）。此前无任何 shader 采深度，
			// 故本 flag 增量为纯行为中性（不改变既有渲染结果）。
			vk::ImageUsageFlagBits::eSampled;

		VmaAllocationCreateInfo depthAllocInfo{};
		depthAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		depthAllocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

		return allocator.createImage(depthImageInfo, depthAllocInfo);
	}

/** 契约见接口注释。实现：e2D / eD32Sfloat / eDepth aspect / levelCount = layerCount = 1
 *（局部临时，供构造器 / recreateDepthImage 的 raii ImageView 构造消费）。 */
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

/**
 * 容量构成与槽位分区契约见接口注释。实现：运行时查询
 * VkPhysicalDeviceDescriptorHeapPropertiesEXT——统一步长 = max(bufferDescriptorSize,
 * imageDescriptorSize)（image 描述符（SAMPLED_IMAGE）比 buffer 描述符宽：RADV 64B > 16B，
 * docs/archive/heap_texture_research.md Q1，32B 为旧 Mesa 首测值；运行时查询，勿硬编码）；
 * reservedSlots = ceil(minResourceHeapReservedRange / 步长)（NVIDIA = 96768B → 3024 槽
 * @32B 统一步长，16B 步长时代旧值 6048 槽；RADV/Intel = 0）；userSlots = geometry +
 * texture + runtime 三段固定容量；capacity = reserved + userSlots；实测值打日志。
 */
std::expected<RGL::VmaDescriptorHeap, Error> Resources::createDescriptorHeap() {
		auto props = vkDevice.getPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();

		auto& heapProps = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		// Phase 3 统一步长：image 描述符（SAMPLED_IMAGE）比 buffer 描述符宽
		//（RADV 64B > 16B，docs/archive/heap_texture_research.md Q1；32B 为旧 Mesa 首测值），slot 步长取 max（运行时查询，勿硬编码）
		uint32_t bufferSize = static_cast<uint32_t>(heapProps.bufferDescriptorSize);
		uint32_t imageSize = static_cast<uint32_t>(heapProps.imageDescriptorSize);
		uint32_t descriptorSize = std::max(bufferSize, imageSize);
		uint32_t reservedSlots = static_cast<uint32_t>(heapProps.minResourceHeapReservedRange + descriptorSize - 1) / descriptorSize;
		vk::DeviceSize resourceHeapAlignment = heapProps.resourceHeapAlignment;
		// 用户槽位全为运行时分配（几何/纹理/scene data/RT 按用途段领取，无具体资产专属预留）
		uint32_t userSlots = kGeometrySlotCapacity + kTextureSlotCapacity + kRuntimeSlotCapacity;
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

/**
 * 背景与容量构成见接口注释。实现：步长 = samplerDescriptorSize（运行时查询）；
 * reservedSlots = ceil(minSamplerHeapReservedRange / 步长)（NVIDIA = 512B → 16 槽 @32B，
 * RADV/Intel = 0）；capacity = reserved + 8（固定小裕量：共享 / sky sampler 各一槽，
 * 余量充足）；实测值打日志。
 */
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

}  // namespace RGL
