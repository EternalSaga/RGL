module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include <fmt/format.h>
export module texture_upload;

import rlstd;
import rllogger;
import vkerror;
import VmaRaii;
import vulkan_utils;
import vk_device;
import vk_resources;
import asset_table;

namespace RGL {

// 纹理 GPU 上传核心（启动期一次性路径）：linear staging（host-visible 填充）→ optimal
// 拷贝 + UNDEFINED→SHADER_READ_ONLY，graphics 队列一次性命令缓冲 + fence waitIdle
//（D7：启动期路径，不启用闲置 transfer queue）。2D 路径无 mipmap；cubemap 路径带
// mipmap 链（blit 降采样 NEAREST——sRGB 格式 blit LINEAR 违 VUID-vkCmdBlitImage-
// filter-00239）。2026-09-02 拆自 texture_pool：上传实现（~530 行）与公共 API
//（解码/白纹理/模型纹理/env 文件加载）解耦——texture_pool 是薄壳，本模块是 GPU 侧。

// RGBA8 像素 → GPU 纹理 + 资产记录（上传全流程；module-private 于 texture_pool 时代
// 的实现，拆出后 export 供 texture_pool 薄壳调用）
export [[nodiscard]] std::expected<uint32_t, Error> uploadTexture(RGL::Resources& resources,
	RGL::DeviceContext& device, const uint8_t* rgba, uint32_t w, uint32_t h, const char* name);

// Phase 5.5：6 份 RGBA8 面像素（已按 Vulkan 层序 0-5 = +X,−X,+Y,−Y,+Z,−Z）→ eCube image +
// mipmap 链 + eCube view + SAMPLED_IMAGE 描述符 → Resources env 单例 + sky sampler。
// size 必须 2 的幂（mip 链 = 1+log2(size) 级）。启动期路径，上传模式同 uploadTexture。
// 内建拷贝金丝雀：逐面回读 mip0 两 texel（角/中心）比对 CPU 源（帧 N 回读版 = gpu_canary
// 模块 texelCanaryCheck；UAF 教训——把"时好时坏"变"启动必炸"）；blit 降采样不做 CPU 比对。
export [[nodiscard]] std::expected<void, Error> uploadCubeMap(RGL::Resources& resources,
	RGL::DeviceContext& device, std::span<const uint8_t* const, 6> faces, uint32_t size,
	const char* name);

export [[nodiscard]] std::expected<uint32_t, Error> uploadTexture(RGL::Resources& resources,
	RGL::DeviceContext& device, const uint8_t* rgba, uint32_t w, uint32_t h, const char* name) {
	auto* logger = RLLogger::getInstance();
	auto& dev = device.getDevice();
	auto& allocator = resources.getAllocator();

	// sRGB（glTF baseColor 纹理约定，硬件完成 sRGB→线性）；不支持回落 UNORM（R4）
	// DIAG（4K 黑屏排查）：TEXTURE_FORCE_UNORM=1 强制 UNORM（sRGB 嫌疑隔离）
	vk::Format format = std::getenv("TEXTURE_FORCE_UNORM")
		? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
	auto formatProps = device.getPhysicalDevice().getFormatProperties(format);
	if ((formatProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
		== vk::FormatFeatureFlags{}) {
		format = vk::Format::eR8G8B8A8Unorm;
		if (logger) logger->warn("texture: R8G8B8A8_SRGB 不支持采样，回落 UNORM ({})", name);
	}

	vk::ImageCreateInfo imageInfo{};
	imageInfo.imageType = vk::ImageType::e2D;
	imageInfo.format = format;
	imageInfo.extent = vk::Extent3D(w, h, 1);
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = vk::SampleCountFlagBits::e1;
	imageInfo.tiling = vk::ImageTiling::eOptimal;
	imageInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
		vk::ImageUsageFlagBits::eTransferSrc;  // TransferSrc：texel 回读诊断/后续校验用
	imageInfo.initialLayout = vk::ImageLayout::eUndefined;
	VmaAllocationCreateInfo imageAlloc{};
	imageAlloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	// DIAG（Phase 3 4K 黑屏排查 2026-08-27）：强制独占 VkDeviceMemory（不走 VMA 池子
	// 子分配），对齐 spike 的独占分配；4K 黑屏仍复现 → VMA 子分配非根因
	//（最终定性 = TexturePool 生命周期 UAF，见 docs/archive/heap_texture_research.md §12.6）
	imageAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	VmaAllocationInfo imageAllocData{};
	auto imageResult = allocator.createImage(imageInfo, imageAlloc, &imageAllocData);
	if (!imageResult) return std::unexpected(std::move(imageResult).error());
	auto image = std::move(*imageResult);
	// DIAG（4K 黑屏排查）：VMA 选中的内存类型（实测 4K/1×1 均 type=0 pool=0
	// DEVICE_LOCAL，与 spike 同池 → 池选择非根因）
	if (logger) {
		const auto& mt = device.getPhysicalDevice().getMemoryProperties().memoryTypes[imageAllocData.memoryType];
		logger->info("DIAG {} mem: type={} pool={} props=0x{:x}", name,
			imageAllocData.memoryType, imageAllocData.memoryType >> 4,
			static_cast<uint32_t>(mt.propertyFlags));
	}

	// linear staging（host-visible + 常驻映射）：CPU 填充后 GPU 拷贝
	vk::ImageCreateInfo stagingInfo = imageInfo;
	stagingInfo.tiling = vk::ImageTiling::eLinear;
	stagingInfo.usage = vk::ImageUsageFlagBits::eTransferSrc;
	VmaAllocationCreateInfo stagingAlloc{};
	stagingAlloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	stagingAlloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo stagingAllocData{};
	auto stagingResult = allocator.createImage(stagingInfo, stagingAlloc, &stagingAllocData);
	if (!stagingResult) return std::unexpected(std::move(stagingResult).error());
	auto staging = std::move(*stagingResult);

	// 填充 linear 图（rowPitch 由驱动给出，linear tiling 行距可能 > w*4）
	VkImageSubresource sub{};
	sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	VkSubresourceLayout subLayout;
	vkGetImageSubresourceLayout(*dev, *staging, &sub, &subLayout);
	if (logger)
		logger->info("DIAG {} linear layout: offset={} rowPitch={} size={} mapped={}",
			name, subLayout.offset, subLayout.rowPitch, subLayout.size,
			static_cast<const void*>(stagingAllocData.pMappedData));
	uint8_t* rowBase = static_cast<uint8_t*>(stagingAllocData.pMappedData) + subLayout.offset;
	const size_t rowBytes = static_cast<size_t>(w) * 4;
	for (uint32_t y = 0; y < h; ++y)
		std::memcpy(rowBase + y * subLayout.rowPitch, rgba + y * rowBytes, rowBytes);

	// 一次性命令缓冲（graphics 队列，启动期无并发）：
	// staging UNDEFINED→SRC_OPTIMAL + dst UNDEFINED→TRANSFER_DST → 拷贝 → dst →SHADER_READ_ONLY
	auto cmdAlloc = dev.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
		*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary, 1});
	auto cmd = std::move(cmdAlloc[0]);
	cmd.begin(vk::CommandBufferBeginInfo{});

	// 裸 C barrier（v341 的 vk::DependencyInfo wrapper 缺 stage/access 字段，勿用 hpp 包装类；
	// 现行 spec v1.4.360：DependencyInfo 无 stage/access 字段，全部由 barrier 自身携带）
	// staging UNDEFINED→SRC_OPTIMAL（首用需显式过渡，含 host write 同步）+ 目标图 UNDEFINED→TRANSFER_DST
	auto toSrcBarrier = RGL::createImageBarrier(
		*staging, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferSrcOptimal,
		vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eBottomOfPipe, vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eTransferRead);
	auto toDstBarrier = RGL::createImageBarrier(
		*image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
		vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eBottomOfPipe, vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eTransferWrite);
	std::array<vk::ImageMemoryBarrier2, 2> uploadBarriers{toSrcBarrier, toDstBarrier};
	VkDependencyInfo toDstDep{};
	toDstDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	toDstDep.imageMemoryBarrierCount = 2;
	toDstDep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(uploadBarriers.data());  // hpp wrapper 与裸 struct 布局兼容（hpp 自身转换同法）
	::vkCmdPipelineBarrier2(*cmd, &toDstDep);
	// 裸 C 拷贝（同 llm_debug 回读拷贝风格）
	VkImageCopy copyRegion{};
	copyRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};  // aspect, mip, baseArrayLayer, layerCount
	copyRegion.srcOffset = {0, 0, 0};
	copyRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copyRegion.dstOffset = {0, 0, 0};
	copyRegion.extent = {w, h, 1};
	::vkCmdCopyImage(*cmd, *staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		*image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
	auto toReadBarrier = RGL::createImageBarrier(
		*image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderSampledRead);
	VkDependencyInfo toReadDep{};
	toReadDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	toReadDep.imageMemoryBarrierCount = 1;
	toReadDep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&toReadBarrier);
	::vkCmdPipelineBarrier2(*cmd, &toReadDep);
	cmd.end();

	// 提交 + 等待（独立 fence；裸 C 提交同 llm_debug ⑫ 风格）
	auto fence = vk::raii::Fence(dev, vk::FenceCreateInfo{});
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	VkCommandBuffer cmdBuffer = *cmd;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmdBuffer;
	if (auto result = vk::Result(::vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, *fence));
		result != vk::Result::eSuccess)
		return std::unexpected(Error{ErrorCode::VulkanError,
			fmt::format("texture: upload submit failed: {}", name)});
	if (dev.waitForFences(std::array<vk::Fence, 1>{*fence}, true, UINT64_MAX) != vk::Result::eSuccess)
		return std::unexpected(Error{ErrorCode::VulkanError, fmt::format("texture: upload fence timeout: {}", name)});

	// 临时诊断（Phase 3 全黑排查）：多位置 texel 回读（空间模式定位上传/采样问题）
	{
		VkBufferCreateInfo rbInfo{};
		rbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		rbInfo.size = 64;
		rbInfo.usage = static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		VmaAllocationCreateInfo rbAlloc{};
		rbAlloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		rbAlloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo rbData{};
		auto rbResult = allocator.createBuffer(rbInfo, rbAlloc, &rbData);
		if (rbResult) {
			auto& rb = *rbResult;
			VkBufferImageCopy copies[4] = {};
			const int32_t pts[4][2] = {{0, 0}, {static_cast<int32_t>(w / 2), static_cast<int32_t>(h / 2)},
				{0, static_cast<int32_t>(h - 1)}, {static_cast<int32_t>(w - 1), 0}};
			for (int i = 0; i < 4; ++i) {
				copies[i].bufferOffset = static_cast<VkDeviceSize>(i) * 4;
				copies[i].bufferImageHeight = 1;
				copies[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				copies[i].imageOffset = {pts[i][0], pts[i][1], 0};
				copies[i].imageExtent = {1, 1, 1};
			}
			// 图当前在 SHADER_READ_ONLY_OPTIMAL：回读前迁到 TRANSFER_SRC
		auto toSrc = RGL::createImageBarrier(
			*image, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
				vk::ImageAspectFlagBits::eColor,
				vk::PipelineStageFlagBits2::eFragmentShader, vk::PipelineStageFlagBits2::eTransfer,
				vk::AccessFlagBits2::eShaderSampledRead, vk::AccessFlagBits2::eTransferRead);
			VkDependencyInfo toSrcDep{};
			toSrcDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			toSrcDep.imageMemoryBarrierCount = 1;
			toSrcDep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&toSrc);
			auto cmdAlloc = dev.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
				*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary, 1});
			auto cmd2 = std::move(cmdAlloc[0]);
			cmd2.begin(vk::CommandBufferBeginInfo{});
			::vkCmdPipelineBarrier2(*cmd2, &toSrcDep);
			::vkCmdCopyImageToBuffer(*cmd2, *image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *rb, 4, copies);
			// 迁回 SHADER_READ_ONLY（描述符声明的布局；采样前必须）
			auto toRead2 = RGL::createImageBarrier(
				*image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
				vk::ImageAspectFlagBits::eColor,
				vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
				vk::AccessFlagBits2::eTransferRead, vk::AccessFlagBits2::eShaderSampledRead);
			VkDependencyInfo toRead2Dep{};
			toRead2Dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			toRead2Dep.imageMemoryBarrierCount = 1;
			toRead2Dep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&toRead2);
			::vkCmdPipelineBarrier2(*cmd2, &toRead2Dep);
			cmd2.end();
			auto fence2 = vk::raii::Fence(dev, vk::FenceCreateInfo{});
			VkSubmitInfo si{};
			si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			VkCommandBuffer cb2 = *cmd2;
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cb2;
			::vkQueueSubmit(device.getGraphicsQueue(), 1, &si, *fence2);
			(void)dev.waitForFences(std::array<vk::Fence, 1>{*fence2}, true, UINT64_MAX);
			const uint8_t* px = static_cast<const uint8_t*>(rbData.pMappedData);
			if (logger)
				logger->info("DIAG {} texels (0,0)={}/{}/{}/{} ({},{})={}/{}/{}/{} (0,{})={}/{}/{}/{} ({},0)={}/{}/{}/{}",
					name,
					px[0], px[1], px[2], px[3],
					w / 2, h / 2, px[4], px[5], px[6], px[7],
					h - 1, px[8], px[9], px[10], px[11],
					w - 1, px[12], px[13], px[14], px[15]);
		}
	}

	// slot 分配 + 描述符写入（SAMPLED_IMAGE，v341 4 参形态，spike 已验证）
	auto slotResult = resources.allocSlot(name);
	if (!slotResult) return std::unexpected(std::move(slotResult).error());
	const uint32_t slot = *slotResult;
	const auto& heap = resources.getDescriptorHeap();

	// view（create info 值保留——v341 pView 契约；raii view 由 create info 构造，同 llm_debug 模式）。
	// vk::raii::* 默认构造已删除：先建局部 view，再整体 move 进资产记录（项目 RAII 规则）
	vk::ImageViewCreateInfo viewCreate{};
	viewCreate.image = *image;
	viewCreate.viewType = vk::ImageViewType::e2D;
	viewCreate.format = format;
	viewCreate.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	auto view = vk::raii::ImageView(dev, viewCreate);

	// 资产记录（GPU 数据 + 描述符指针链）入资产表：记录在 deque（元素地址稳定），
	// 自/兄弟引用在入表后设置有效。跨驱动防御：本机 RADV 写时即解析
	//（docs/archive/heap_texture_research.md §12.2，A/B 实验证伪"采样期解引用"旧归因），
	// 但 spec 无调用后指针生命周期条款，指针稳定性 = 对其他驱动的正确性前提
	TextureAsset asset{std::move(image), viewCreate, std::move(view), slot, w, h};
	auto& stored = resources.getAssetTable().emplaceTexture(std::move(asset));
	stored.imageDesc.pView = &stored.viewCreate;  // hpp create info 指针（v341 契约，spike 已验证）
	stored.imageDesc.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
	stored.resourceDesc.type = vk::DescriptorType::eSampledImage;
	stored.resourceDesc.data.pImage = &stored.imageDesc;
	auto slotAddr = heap.getSlotCpuAddress(slot);
	if (!slotAddr) return std::unexpected(std::move(slotAddr).error());
	stored.dst = vk::HostAddressRangeEXT{*slotAddr, heap.getDescriptorSize()};
	// 2 参 ArrayProxy 重载：proxy 引用 stored.resourceDesc/dst（deque 元素，地址稳定），内部
	// reinterpret_cast 传地址给 driver——loader 不导出该设备级扩展符号，只能走
	// hpp dispatcher（vkGetDeviceProcAddr），同 spike 的运行时解析结论
	dev.writeResourceDescriptorsEXT(stored.resourceDesc, stored.dst);

	if (logger)
		logger->info("texture: {} {}x{} {} → slot {} ({}MB)", name, w, h,
			format == vk::Format::eR8G8B8A8Srgb ? "sRGB" : "UNORM", slot,
			static_cast<double>(w) * h * 4.0 / (1024.0 * 1024.0));
	return static_cast<uint32_t>(resources.getAssetTable().textureSize() - 1);
}

export [[nodiscard]] std::expected<void, Error> uploadCubeMap(RGL::Resources& resources,
	RGL::DeviceContext& device, std::span<const uint8_t* const, 6> faces, uint32_t size,
	const char* name) {
	auto* logger = RLLogger::getInstance();
	auto& dev = device.getDevice();
	auto& allocator = resources.getAllocator();

	if (size == 0 || (size & (size - 1)) != 0)
		return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
			fmt::format("cubemap {}: size {} 必须为 2 的幂", name, size)});
	uint32_t mipLevels = 1;
	for (uint32_t s = size; s > 1; s >>= 1) ++mipLevels;

	// 格式策略同 2D 路径：sRGB 优先 + 不支持采样回落 UNORM（R4）+ DIAG 强制 UNORM 门
	vk::Format format = std::getenv("TEXTURE_FORCE_UNORM")
		? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
	auto formatProps = device.getPhysicalDevice().getFormatProperties(format);
	if ((formatProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
		== vk::FormatFeatureFlags{}) {
		format = vk::Format::eR8G8B8A8Unorm;
		if (logger) logger->warn("cubemap: R8G8B8A8_SRGB 不支持采样，回落 UNORM ({})", name);
	}

	// Vulkan cubemap 无独立 imageType：e2D + arrayLayers=6 + eCubeCompatible flag
	//（view 才用 eCube；GL/D3D 的 CUBE 类型概念在 Vulkan 不存在）
	vk::ImageCreateInfo imageInfo{};
	imageInfo.flags = vk::ImageCreateFlagBits::eCubeCompatible;
	imageInfo.imageType = vk::ImageType::e2D;
	imageInfo.format = format;
	imageInfo.extent = vk::Extent3D(size, size, 1);
	imageInfo.mipLevels = mipLevels;
	imageInfo.arrayLayers = 6;
	imageInfo.samples = vk::SampleCountFlagBits::e1;
	imageInfo.tiling = vk::ImageTiling::eOptimal;
	imageInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
		vk::ImageUsageFlagBits::eTransferSrc;
	imageInfo.initialLayout = vk::ImageLayout::eUndefined;
	VmaAllocationCreateInfo imageAlloc{};
	imageAlloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	imageAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;  // 独占分配（2D 路径现状一致）
	auto imageResult = allocator.createImage(imageInfo, imageAlloc);
	if (!imageResult) return std::unexpected(std::move(imageResult).error());
	auto image = std::move(*imageResult);

	// linear staging：**6 张独立 2D 图**（每面一张）。不继承 imageInfo——linear 有三条硬限
	//（RADV 实测连踩两坑 2026-09-01）：VUID-VkImageCreateInfo-tiling-02260 linear 必须
	// mipLevels=1；CUBE_COMPATIBLE flag linear 不支持（cube-ness 只需目标图有）；
	// RADV linear 图 maxArrayLayers=1（arrayed linear FORMAT_NOT_SUPPORTED）→ per-face 单图
	vk::ImageCreateInfo stagingInfo{};
	stagingInfo.imageType = vk::ImageType::e2D;
	stagingInfo.format = format;
	stagingInfo.extent = vk::Extent3D(size, size, 1);
	stagingInfo.mipLevels = 1;
	stagingInfo.arrayLayers = 1;
	stagingInfo.samples = vk::SampleCountFlagBits::e1;
	stagingInfo.tiling = vk::ImageTiling::eLinear;
	stagingInfo.usage = vk::ImageUsageFlagBits::eTransferSrc;
	stagingInfo.initialLayout = vk::ImageLayout::eUndefined;
	VmaAllocationCreateInfo stagingAlloc{};
	stagingAlloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	stagingAlloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	std::vector<RGL::VmaImage> stagings;
	std::array<VmaAllocationInfo, 6> stagingData{};
	for (uint32_t face = 0; face < 6; ++face) {
		auto stagingResult = allocator.createImage(stagingInfo, stagingAlloc, &stagingData[face]);
		if (!stagingResult) return std::unexpected(std::move(stagingResult).error());
		stagings.push_back(std::move(*stagingResult));
	}

	const size_t faceRowBytes = static_cast<size_t>(size) * 4;
	for (uint32_t face = 0; face < 6; ++face) {
		VkImageSubresource sub{};
		sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		VkSubresourceLayout layerLayout;
		vkGetImageSubresourceLayout(*dev, *stagings[face], &sub, &layerLayout);
		uint8_t* base = static_cast<uint8_t*>(stagingData[face].pMappedData) + layerLayout.offset;
		for (uint32_t y = 0; y < size; ++y)
			std::memcpy(base + static_cast<size_t>(y) * layerLayout.rowPitch,
				faces[face] + static_cast<size_t>(y) * faceRowBytes, faceRowBytes);
	}

	// 一次性命令缓冲（graphics 队列，启动期无并发）：per-face copy + 逐 level blit 链
	auto cmdAlloc = dev.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
		*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary, 1});
	auto cmd = std::move(cmdAlloc[0]);
	cmd.begin(vk::CommandBufferBeginInfo{});
	const auto emitBarrier = [&](const vk::ImageMemoryBarrier2& b) {
		VkDependencyInfo dep{};
		dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&b);
		::vkCmdPipelineBarrier2(*cmd, &dep);
	};

	std::vector<vk::ImageMemoryBarrier2> uploadBarriers;
	uploadBarriers.reserve(7);
	for (uint32_t face = 0; face < 6; ++face)
		uploadBarriers.push_back(RGL::createImageBarrier(
			*stagings[face], vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferSrcOptimal,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eBottomOfPipe, vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eTransferRead));
	uploadBarriers.push_back(RGL::createImageBarrier(
		*image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
		vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eBottomOfPipe, vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eTransferWrite,
		0, mipLevels, 0, 6));
	VkDependencyInfo toDstDep{};
	toDstDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	toDstDep.imageMemoryBarrierCount = static_cast<uint32_t>(uploadBarriers.size());
	toDstDep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(uploadBarriers.data());
	::vkCmdPipelineBarrier2(*cmd, &toDstDep);

	VkImageCopy copyRegion{};
	copyRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copyRegion.extent = {size, size, 1};
	for (uint32_t face = 0; face < 6; ++face) {
		copyRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
		::vkCmdCopyImage(*cmd, *stagings[face], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			*image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
	}

	// mip 链：level i−1 先 DST→SRC，再 6 面 blit 到 level i（同图不同 subresource 不重叠，
	// spec 允许；sRGB 格式 blit 只能 NEAREST，见函数头注释）
	for (uint32_t level = 1; level < mipLevels; ++level) {
		emitBarrier(RGL::createImageBarrier(
			*image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eTransferRead,
			level - 1, 1, 0, 6));
		const uint32_t srcSize = size >> (level - 1);
		const uint32_t dstSize = std::max(1u, size >> level);
		std::array<VkImageBlit, 6> blits{};
		for (uint32_t face = 0; face < 6; ++face) {
			blits[face].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, face, 1};
			blits[face].srcOffsets[0] = {0, 0, 0};
			blits[face].srcOffsets[1] = {static_cast<int32_t>(srcSize), static_cast<int32_t>(srcSize), 1};
			blits[face].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
			blits[face].dstOffsets[0] = {0, 0, 0};
			blits[face].dstOffsets[1] = {static_cast<int32_t>(dstSize), static_cast<int32_t>(dstSize), 1};
		}
		::vkCmdBlitImage(*cmd, *image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			*image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, blits.data(), VK_FILTER_NEAREST);
	}
	// → SHADER_READ_ONLY：level 0..L−2 停在 TRANSFER_SRC（blit 源），末级停 TRANSFER_DST
	if (mipLevels > 1)
		emitBarrier(RGL::createImageBarrier(*image, vk::ImageLayout::eTransferSrcOptimal,
			vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
			vk::AccessFlagBits2::eTransferRead, vk::AccessFlagBits2::eShaderSampledRead,
			0, mipLevels - 1, 0, 6));
	emitBarrier(RGL::createImageBarrier(*image, vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderSampledRead,
		mipLevels - 1, 1, 0, 6));
	cmd.end();

	auto fence = vk::raii::Fence(dev, vk::FenceCreateInfo{});
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	VkCommandBuffer cmdBuffer = *cmd;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmdBuffer;
	if (auto result = vk::Result(::vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, *fence));
		result != vk::Result::eSuccess)
		return std::unexpected(Error{ErrorCode::VulkanError,
			fmt::format("cubemap: upload submit failed: {}", name)});
	if (dev.waitForFences(std::array<vk::Fence, 1>{*fence}, true, UINT64_MAX) != vk::Result::eSuccess)
		return std::unexpected(Error{ErrorCode::VulkanError,
			fmt::format("cubemap: upload fence timeout: {}", name)});

	// 拷贝金丝雀：逐面回读 mip0 texel (0,0) + 中心，比对 CPU 源（6×2×4B = 48B）
	{
		VkBufferCreateInfo rbInfo{};
		rbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		rbInfo.size = 6 * 2 * 4;
		rbInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VmaAllocationCreateInfo rbAlloc{};
		rbAlloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		rbAlloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo rbData{};
		auto rbResult = allocator.createBuffer(rbInfo, rbAlloc, &rbData);
		if (!rbResult) return std::unexpected(std::move(rbResult).error());
		auto& rb = *rbResult;
		const uint32_t mid = size / 2;
		std::array<VkBufferImageCopy, 12> copies{};
		for (uint32_t face = 0; face < 6; ++face)
			for (uint32_t t = 0; t < 2; ++t) {
				copies[face * 2 + t].bufferOffset = (face * 2 + t) * 4;
				copies[face * 2 + t].bufferImageHeight = 1;
				copies[face * 2 + t].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
				copies[face * 2 + t].imageOffset = t == 0
					? VkOffset3D{0, 0, 0} : VkOffset3D{static_cast<int32_t>(mid), static_cast<int32_t>(mid), 0};
				copies[face * 2 + t].imageExtent = {1, 1, 1};
			}
		auto rbCmdAlloc = dev.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
			*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary, 1});
		auto rbCmd = std::move(rbCmdAlloc[0]);
		rbCmd.begin(vk::CommandBufferBeginInfo{});
		const auto rbEmit = [&](const vk::ImageMemoryBarrier2& b) {
			VkDependencyInfo dep{};
			dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dep.imageMemoryBarrierCount = 1;
			dep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&b);
			::vkCmdPipelineBarrier2(*rbCmd, &dep);
		};
		rbEmit(RGL::createImageBarrier(*image, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::ImageLayout::eTransferSrcOptimal, vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eFragmentShader, vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eShaderSampledRead, vk::AccessFlagBits2::eTransferRead,
			0, 1, 0, 6));
		::vkCmdCopyImageToBuffer(*rbCmd, *image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			*rb, static_cast<uint32_t>(copies.size()), copies.data());
		rbEmit(RGL::createImageBarrier(*image, vk::ImageLayout::eTransferSrcOptimal,
			vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
			vk::AccessFlagBits2::eTransferRead, vk::AccessFlagBits2::eShaderSampledRead,
			0, 1, 0, 6));
		rbCmd.end();
		auto rbFence = vk::raii::Fence(dev, vk::FenceCreateInfo{});
		VkSubmitInfo rbSi{};
		rbSi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		VkCommandBuffer rbCb = *rbCmd;
		rbSi.commandBufferCount = 1;
		rbSi.pCommandBuffers = &rbCb;
		::vkQueueSubmit(device.getGraphicsQueue(), 1, &rbSi, *rbFence);
		(void)dev.waitForFences(std::array<vk::Fence, 1>{*rbFence}, true, UINT64_MAX);
		const uint8_t* px = static_cast<const uint8_t*>(rbData.pMappedData);
		bool pass = true;
		for (uint32_t face = 0; face < 6 && pass; ++face) {
			const uint8_t* src0 = faces[face];
			const uint8_t* srcM = faces[face] +
				(static_cast<size_t>(mid) * size + mid) * 4;
			for (uint32_t t = 0; t < 2; ++t) {
				const uint8_t* expect = t == 0 ? src0 : srcM;
				const uint8_t* got = px + (face * 2 + t) * 4;
				if (std::memcmp(expect, got, 4) != 0) {
					pass = false;
					if (logger)
						logger->error("cubemap canary {}: face {} texel {} = {}/{}/{}/{} != 源 {}/{}/{}/{}",
							name, face, t, got[0], got[1], got[2], got[3],
							expect[0], expect[1], expect[2], expect[3]);
					break;
				}
			}
		}
		if (pass && logger)
			logger->info("cubemap canary {}: 6 面 × 2 texel 回读比对 PASS", name);
	}

	// view（eCube 全 mip 链 + 6 层）+ slot 分配 + SAMPLED_IMAGE 描述符（指针链模式同 2D；
	// 存储 = Resources env 单例 optional，非 AssetTable textures——env 绑场景不绑材质）
	vk::ImageViewCreateInfo viewCreate{};
	viewCreate.image = *image;
	viewCreate.viewType = vk::ImageViewType::eCube;
	viewCreate.format = format;
	viewCreate.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 6};
	auto view = vk::raii::ImageView(dev, viewCreate);

	auto slotResult = resources.allocSlot(name);
	if (!slotResult) return std::unexpected(std::move(slotResult).error());
	const uint32_t slot = *slotResult;
	const auto& heap = resources.getDescriptorHeap();

	TextureAsset asset{std::move(image), viewCreate, std::move(view), slot, size, size, mipLevels};
	auto& stored = resources.emplaceEnvTexture(std::move(asset));
	stored.imageDesc.pView = &stored.viewCreate;
	stored.imageDesc.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
	stored.resourceDesc.type = vk::DescriptorType::eSampledImage;
	stored.resourceDesc.data.pImage = &stored.imageDesc;
	auto slotAddr = heap.getSlotCpuAddress(slot);
	if (!slotAddr) return std::unexpected(std::move(slotAddr).error());
	stored.dst = vk::HostAddressRangeEXT{*slotAddr, heap.getDescriptorSize()};
	dev.writeResourceDescriptorsEXT(stored.resourceDesc, stored.dst);

	const uint32_t skySlot = resources.ensureSkySampler(mipLevels);
	if (logger)
		logger->info("cubemap: {} {}²×6 {} mips={} → res slot {} + sky sampler slot {}", name, size,
			format == vk::Format::eR8G8B8A8Srgb ? "sRGB" : "UNORM", mipLevels, slot, skySlot);
	return {};
}

}  // namespace RGL
