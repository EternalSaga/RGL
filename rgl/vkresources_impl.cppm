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
import asset_table;
import render_graph;  // Phase 7：G-buffer 契约表 kGBufferAttachments + resolveRgFormat（单源）

namespace RGL {

// Resources 重方法定义（ctor/sampler/sceneData/heap 创建，2026-09-02
// 编译提速 A 方案）：函数体移出接口 TU，BMI 只留类声明；本单元是叶子边。
//（ensure*/create* 均为启动期一次性路径，无跨 TU 内联价值）

/**
 * 初始化列表顺序与 heap 槽位布局契约见接口注释。实现补充：allocator 的 flags 显式传
 * BDA（第 5 参）——VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT 不可省略，否则所有
 * eShaderDeviceAddress buffer 创建返回 VK_ERROR_INITIALIZATION_FAILED（见体内注释）；
 * 内核不创建任何内容资产（架构债务 1 方案 B——几何/纹理/scene data/RT 采样槽全部由
 * 内容层按上传时序经 allocSlot 领取，见体内注释）。
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
		  samplerHeap(createSamplerHeap().value()),
   		  // Phase 8 ③b：compute mip 链生成器（传统管线全栈，加载 SPV 建管线；启动期一次性）
   		  mipGen(vkDevice),
   		  // Phase 8 ④b：GGX 预滤波 env cubemap 生成器（heap + shader object，加载 SPV；启动期一次性）
   		  envPrefilter(vkDevice),
   		  nextSlot(reservedSlots()) {
		// 内核不创建任何内容资产（架构债务 1 方案 B）：几何（内建 cube / gltf 模型）、
		// 纹理、scene data、RT 采样槽全部由内容层按上传时序经 allocSlot 领取
	}

/**
 * 幂等契约与 create info 稳定存储规则见接口注释。实现：slot =
 * samplerHeap.getReservedSlots()（首个可用槽）；sampler 参数 linear + CLAMP_TO_EDGE、
 * Phase 8 ③b 起 mipmapMode=LINEAR + maxLod=20（2D 纹理带 compute mip 链——采样自动
 * 用 mip，maxLod 覆盖 ≤2^20 纹理链长，Vulkan 按各图实际 mip 范围钳制）；经
 * vkWriteSamplerDescriptorsEXT 写入（hpp raii 内部 resultCheck，失败抛 LogicError），
 * dst 大小 = 一个槽步长。
 */
uint32_t Resources::ensureSharedSampler() {
		if (sharedSamplerReady)
			return sharedSamplerSlot;
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
		info.maxLod = 20.0f;
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

/**
 * 幂等契约见接口注释。实现：slot = samplerHeap.getReservedSlots() + 1（共享 sampler
 * 之后第二槽）；sampler 参数 linear mipmap（mipmapMode=LINEAR）+ maxLod = mipLevels−1
 *（烘焙进描述符，调用点须已知环境贴图 mip 级数）；经 vkWriteSamplerDescriptorsEXT
 * 写入。不动共享 sampler——模型纹理路径零影响。
 */
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

/**
 * 幂等契约与时序契约见接口注释。实现：首次注入 heap 引用 + 共享 sampler 槽（复用
 * ensureSharedSampler 的 LINEAR sampler——全 level 共用一份，SampleLevel 显式 lod）；
 * 之后把 level shader/槽 补齐到 levels（每 level allocSlot 两槽 + MipGen::addLevel 建
 * 一个烘焙到该槽的 compute shader object）。领槽推进 allocSlot 游标——调用点保证晚于
 * 几何段与白纹理首槽（见接口"时序契约"）。
 */
std::expected<void, Error> Resources::ensureMipGenReady(uint32_t levels) {
		if (levels <= 1)
			return {};  // 单级图无需 mip 链
		if (!mipGenBound) {
			const uint32_t samplerSlot = ensureSharedSampler();  // 幂等：LINEAR/CLAMP，mip-gen 共用
			mipGen.init(descriptorHeap, samplerHeap, samplerSlot);
			mipGenBound = true;
		}
		while (mipGen.levelCount() < levels) {
			auto srcSlot = allocSlot("mipgen.src");
			if (!srcSlot)
				return std::unexpected(std::move(srcSlot).error());
			auto dstSlot = allocSlot("mipgen.dst");
			if (!dstSlot)
				return std::unexpected(std::move(dstSlot).error());
			if (auto added = mipGen.addLevel(*srcSlot, *dstSlot); !added)
				return std::unexpected(std::move(added).error());
		}
		return {};
	}

/**
 * 幂等契约与时序契约见接口注释。实现：首次注入 heap 引用 + 共享 sampler 槽（复用
 * ensureSharedSampler 的 LINEAR sampler——全 level 共用一份，SampleLevel 显式 lod=0 读 mip0）；
 * 之后把 level shader/槽 补齐到 levels（每 level allocSlot 两槽 + EnvPrefilter::addLevel 建
 * 一个烘焙到该槽的 compute shader object）。env 单例启动期只上传一次，实际只补一轮。领槽
 * 推进 allocSlot 游标——时序契约同 ensureMipGenReady（env 上传晚于几何段/白纹理首槽）。
 */
std::expected<void, Error> Resources::ensureEnvPrefilterReady(uint32_t levels) {
		if (levels <= 1)
			return {};  // 单级图无需预滤波
		if (!envPrefilterBound) {
			const uint32_t samplerSlot = ensureSharedSampler();  // 幂等：LINEAR/CLAMP，共用一份
			envPrefilter.init(descriptorHeap, samplerHeap, samplerSlot);
			envPrefilterBound = true;
		}
		while (envPrefilter.levelCount() < levels) {
			auto srcSlot = allocSlot("envpf.src");
			if (!srcSlot)
				return std::unexpected(std::move(srcSlot).error());
			auto dstSlot = allocSlot("envpf.dst");
			if (!dstSlot)
				return std::unexpected(std::move(dstSlot).error());
			if (auto added = envPrefilter.addLevel(*srcSlot, *dstSlot); !added)
				return std::unexpected(std::move(added).error());
		}
		return {};
	}

/**
 * 幂等契约与调用时序见接口注释。实现：buffer usage =
 * eShaderDeviceAddress | eStorageBuffer | eTransferDst，VMA_MEMORY_USAGE_AUTO
 * device-local（CPU 不直接写——每帧唯一写入方 = staging copy，勿用 createGpuBuffer 的
 * CPU_TO_GPU+MAPPED 形态，见体内注释）；slot = allocSlot("scene.data")；描述符 =
 * eStorageBuffer，data = buffer BDA 地址范围（全 kSceneDataBytes）。
 */
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

/**
 * 创建/重建行为契约见接口注释（RtAttachment 注释）。实现：命中 = 已存在且 extent/format
 * 均匹配（不重建）；重建先 waitIdle（双帧在飞安全）再换 image/view、generation+1；
 * SAMPLED_IMAGE 槽首建 allocSlot、跨 resize 复用（槽号永久有效，light 管线 baked 不失效）；
 * 描述符指针链存 map 节点（v341 契约：imageDesc.pView 指 create info 而非 VkImageView，
 * 见体内注释）；forward/offscreen 图不 bind 此槽 = 无副作用（行为中性）。
 */
std::expected<Resources::RtAttachment*, Error> Resources::ensureRtAttachment(
		const std::string& name, vk::Extent2D extent, vk::Format format) {
		auto it = rtAttachments.find(name);
		if (it != rtAttachments.end() && it->second.extent == extent &&
			it->second.format == format)
			return &it->second;

		// 重建（extent 随 swapchain resize / 格式漂移）：双帧在飞下旧 RT 可能仍被
		// 另一帧命令引用 → waitIdle 后替换（同 recreateDepthImage 规则）
		vkDevice.getDevice().waitIdle();

		vk::ImageCreateInfo info{};
		info.imageType = vk::ImageType::e2D;
		info.format = format;
		info.extent = vk::Extent3D(extent.width, extent.height, 1);
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = vk::SampleCountFlagBits::e1;
		info.tiling = vk::ImageTiling::eOptimal;
		info.initialLayout = vk::ImageLayout::eUndefined;
		// 消费面：colorWrite（渲入）+ transferRead/Write（blit 两端）+ sampleRead
		//（shader 采样，后处理类图预留）
		info.usage = vk::ImageUsageFlagBits::eColorAttachment |
			vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
			vk::ImageUsageFlagBits::eSampled;
		VmaAllocationCreateInfo alloc{};
		alloc.usage = VMA_MEMORY_USAGE_AUTO;
		auto imageResult = allocator.createImage(info, alloc);
		if (!imageResult)
			return std::unexpected(std::move(imageResult).error());

		RtAttachment& slot = rtAttachments[name];  // 覆盖旧值（map 节点地址稳定）
		const uint64_t prevGeneration = slot.generation;
		slot.image = std::move(*imageResult);
		slot.extent = extent;
		slot.format = format;
		slot.generation = prevGeneration + 1;
		vk::ImageViewCreateInfo viewInfo{};
		viewInfo.image = slot.image.image;
		viewInfo.viewType = vk::ImageViewType::e2D;
		viewInfo.format = format;
		viewInfo.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		slot.view.emplace(vkDevice.getDevice(), viewInfo);

		// Phase 7 gap ④：写 SAMPLED_IMAGE 描述符供 deferred light pass 采样。首建 allocSlot，
		// 跨 resize 复用同槽（重建只换 image→更新 sampleViewCreate，指针链稳定）；light 管线
		// baked 的槽号因此永久有效。forward/offscreen 图不 bind 此槽 = 无副作用（行为中性）。
		if (!slot.hasSampleDescriptor) {
			auto sampleSlot = allocSlot(name.c_str());
			if (!sampleSlot)
				return std::unexpected(std::move(sampleSlot).error());
			slot.heapSlot = *sampleSlot;
			slot.hasSampleDescriptor = true;
		}
		slot.sampleViewCreate = vk::ImageViewCreateInfo{};
		slot.sampleViewCreate.image = slot.image.image;
		slot.sampleViewCreate.viewType = vk::ImageViewType::e2D;
		slot.sampleViewCreate.format = format;
		slot.sampleViewCreate.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		slot.imageDesc = vk::ImageDescriptorInfoEXT{};
		slot.imageDesc.pView = &slot.sampleViewCreate;  // v341 契约：pView 指 create info（非 VkImageView）
		slot.imageDesc.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
		slot.resourceDesc = vk::ResourceDescriptorInfoEXT{};
		slot.resourceDesc.type = vk::DescriptorType::eSampledImage;
		slot.resourceDesc.data.pImage = &slot.imageDesc;
		auto slotAddr = descriptorHeap.getSlotCpuAddress(slot.heapSlot);
		if (!slotAddr)
			return std::unexpected(std::move(slotAddr).error());
		slot.dst = vk::HostAddressRangeEXT{*slotAddr, descriptorHeap.getDescriptorSize()};
		vkDevice.getDevice().writeResourceDescriptorsEXT(slot.resourceDesc, slot.dst);

		return &slot;
	}

/**
 * 调用时序契约（须在几何/纹理/env 槽之后）与槽语义见接口注释。实现：名字/格式取自
 * render_graph kGBufferAttachments 契约表（单一真相源，与 deferredRenderGraphDesc
 * 共享，无漂移可能，见体内注释）；extent = 当前 swapchain；逐附件 ensureRtAttachment +
 * rtHeapSlot（任一失败立即返回）；成功打日志 4 槽号（albedo/normal/position/metalrough）。
 */
std::expected<std::array<uint32_t, 4>, Error> Resources::ensureDeferredGBufferSlots() {
	// 名字/格式 = render_graph kGBufferAttachments 契约表（单一真相源，与
	// deferredRenderGraphDesc 共享（llm_debug 重渲器已退役），无漂移可能）
	const vk::Extent2D extent = vkSwapchain.getExtent();
	std::array<uint32_t, 4> slots{};
	for (uint32_t i = 0; i < kGBufferAttachments.size(); ++i) {
		const std::string name{kGBufferAttachments[i].name};
		const vk::Format format = resolveRgFormat(
			std::string{kGBufferAttachments[i].format}, vk::Format::eUndefined);
		auto rt = ensureRtAttachment(name, extent, format);
		if (!rt)
			return std::unexpected(std::move(rt).error());
		auto slot = rtHeapSlot(name);
		if (!slot)
			return std::unexpected(Error{ErrorCode::Unknown,
				fmt::format("gbuffer RT '{}' has no sample descriptor slot", name)});
		slots[i] = *slot;
	}
	if (auto* logger = RLLogger::getInstance())
		logger->info("deferred G-buffer: {}x{} slots {}/{}/{}/{} (albedo/normal/position/metalrough)",
			extent.width, extent.height, slots[0], slots[1], slots[2], slots[3]);
	return slots;
}

/**
 * Phase 8 批次 ①（接口注释见 vkresources.cppm）：名字/格式 = render_graph
 * kSceneHdrName/kSceneHdrFormat 契约常量（与 default/deferred 图描述共享，无漂移）；
 * extent = 当前 swapchain；ensureRtAttachment（rgba16_sfloat 自带
 * COLOR_ATTACHMENT|SAMPLED usage）+ rtHeapSlot。启动期一次（main 在 deferred 装配后、
 * addTonemapEntry 前）；drawFrame 图循环每帧对同名命中复用。
 */
std::expected<uint32_t, Error> Resources::ensureSceneHdrSlot() {
	const std::string name{std::string{kSceneHdrName}.c_str()};
	const vk::Extent2D extent = vkSwapchain.getExtent();
	const vk::Format format = vk::Format::eR16G16B16A16Sfloat;  // = kSceneHdrFormat 物化
	auto rt = ensureRtAttachment(name, extent, format);
	if (!rt)
		return std::unexpected(std::move(rt).error());
	auto slot = rtHeapSlot(name);
	if (!slot)
		return std::unexpected(Error{ErrorCode::Unknown,
			fmt::format("scene_hdr RT has no sample descriptor slot", name)});
	if (auto* logger = RLLogger::getInstance())
		logger->info("scene_hdr (RGBA16F): {}x{} slot {}", extent.width, extent.height, *slot);
	return *slot;
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
		depthImageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;

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
 * @32B 统一步长，16B 步长时代旧值 6048 槽；RADV/Intel = 0）；userSlots = kUserSlotMargin；
 * capacity = reserved + userSlots；实测值打日志。
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
		// 用户槽位全为运行时分配（几何/纹理/scene data/RT 按上传时序续接，无内容专属预留）
		uint32_t userSlots = kUserSlotMargin;
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
