module;
#include <stb_image.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include <fmt/format.h>
export module texture_pool;

import rlstd;
import rllogger;
import vkerror;
import VmaRaii;
import vulkan_utils;
import vk_device;
import vk_resources;
import gltf_loader;
import model_types_generated;

namespace RGL {

// 纹理解码（stb_image）+ GPU 上传（启动期一次性）+ 描述符写入（SAMPLED_IMAGE，单 heap 混存
// buffer+image，spike 已验证）+ slot 分配（Resources::allocSlot）。
//
// 批次 1 重构（2026-08-28）：原 TexturePool 类持有资产 + 描述符存储（main 局部对象 = UAF
// 根因：块尾析构第 0 帧前释放纹理 BO，docs/heap_texture_research.md §12.6）。现为无状态
// 工具函数，注册产物（TextureAsset）落 Resources::AssetTable（初始化层 owner，所有权规则
// §12.7 ① 在此首次落地）；共享 sampler 同步移入 Resources::ensureSharedSampler。
// 上传路径：linear staging（host-visible 填充）→ optimal 拷贝 + UNDEFINED→SHADER_READ_ONLY，
// graphics 队列一次性命令缓冲 + fence waitIdle（D7：启动期路径，不启用闲置 transfer queue）。
// 无 mipmap（minFilter=linear、mipmapMode=NEAREST、maxLod=0），mipmap 后续加。

// RGBA8 像素 → GPU 纹理 + 资产记录（上传全流程，见模块头注释；module-private）
[[nodiscard]] std::expected<uint32_t, Error> uploadTexture(RGL::Resources& resources,
	RGL::DeviceContext& device, const uint8_t* rgba, uint32_t w, uint32_t h, const char* name);

// glb 内嵌图（buffer view byte span）→ 纹理资产 id；解码强制 RGBA8 4 通道
export [[nodiscard]] std::expected<uint32_t, Error> loadEmbedded(RGL::Resources& resources,
	RGL::DeviceContext& device, const uint8_t* data, size_t size, const char* name) {
	int w = 0, h = 0, ch = 0;
	uint8_t* rgba = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &ch, 4);
	if (!rgba) {
		return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
			fmt::format("stb_image decode failed: {}", name)});
	}
	// DIAG（Phase 3 4K 黑屏排查 2026-08-27）：环境变量驱动的实验开关（免重编译跑矩阵）：
	//   TEXTURE_FORCE_SIZE=N  近邻缩放强制 N×N（测尺寸阈值：16MB vs 64MB）
	//   TEXTURE_FORCE_WHITE=1 全白填充（测"均匀内容不 fault"假设）
	//   TEXTURE_FORCE_UNORM=1 upload 强制 UNORM（测 sRGB 嫌疑）
	const char* forceSize = std::getenv("TEXTURE_FORCE_SIZE");
	const char* forceWhite = std::getenv("TEXTURE_FORCE_WHITE");
	const uint32_t fw = forceWhite ? static_cast<uint32_t>(w) : 0;
	const uint32_t fh = forceWhite ? static_cast<uint32_t>(h) : 0;
	const uint32_t target = forceSize ? static_cast<uint32_t>(std::stoul(forceSize)) : 0;
	if (target > 0 && (static_cast<uint32_t>(w) < target || static_cast<uint32_t>(h) < target)) {
		std::vector<uint8_t> up(static_cast<size_t>(target) * target * 4,
			forceWhite ? 255 : 0);
		for (uint32_t y = 0; y < target; ++y)
			for (uint32_t x = 0; x < target; ++x) {
				if (forceWhite) continue;
				const uint8_t* s = rgba + (static_cast<size_t>(y * h / target) * w + x * w / target) * 4;
				std::copy_n(s, 4, up.data() + static_cast<size_t>(y) * target * 4 + x * 4);
			}
		stbi_image_free(rgba);
		return uploadTexture(resources, device, up.data(), target, target, name);
	}
	if (forceWhite) {
		std::fill(rgba, rgba + static_cast<size_t>(w) * h * 4, 255);
	}
	auto result = uploadTexture(resources, device, rgba, static_cast<uint32_t>(w), static_cast<uint32_t>(h), name);
	stbi_image_free(rgba);
	return result;
}

// 1×1 白纹理（无纹理/无 uv primitive 的共用特例，D4）。
// 幂等（纹理首槽 = 白，调用序契约：先于任何图片纹理取）。
// 布局契约断言（批次 1 新增，Phase 4 批次 2 改按模型数）：slot = reserved +
// HeapSlotModelVertexBase + 模型顶点槽数（拼接 buffer = 每模型一个 slot，getModelSlotCount）
//（违例 = 契约漂移报错——原靠"别提前取白"纪律，2026-08-28 排查踩坑：白纹理抢 slot 2
// → model.vertex[0] drift 抛错）
export uint32_t ensureWhiteTexture(RGL::Resources& resources, RGL::DeviceContext& device) {
	auto& assets = resources.getAssetTable();
	if (assets.textureSize() > 0)
		return 0;  // 首个纹理资产 = 白（调用序契约）
	const uint8_t rgba[4] = {255, 255, 255, 255};
	auto result = uploadTexture(resources, device, rgba, 1, 1, "texture.white");
	if (!result)
		throw std::runtime_error(fmt::format("white texture: {}", result.error().describe()));
	const uint32_t expected = resources.getDescriptorHeap().getReservedSlots() +
		HeapSlotModelVertexBase + assets.getModelSlotCount();
	const uint32_t slot = assets.getTexture(*result).heapSlot;
	if (slot != expected)
		throw std::runtime_error(fmt::format(
			"heap slot layout drift: texture.white (slot {} != expected {})", slot, expected));
	return *result;
}

// 模型纹理上传（批次 1：main.cpp 循环迁入）：模型顶点槽分配完（importer）后调用——
// 先取白纹理，再按 primitive 的 baseColorTexture 加载（image 下标去重，同图不重复上传）；
// 无纹理/无 uv → 白纹理。返回纹理资产 id 列表（与 model.primitives 下标对齐，
// 供管线烘焙 / registerScene 材质注册消费）。
export [[nodiscard]] std::expected<std::vector<uint32_t>, Error> uploadModelTextures(
	RGL::Resources& resources, RGL::DeviceContext& device, const RGL::GltfModel& model) {
	const uint32_t whiteId = ensureWhiteTexture(resources, device);
	std::vector<uint32_t> primitiveTextureIds(model.primitives.size(), whiteId);
	std::map<int, uint32_t> imageTextureCache;  // image 下标 → 纹理资产 id（去重）
	for (size_t i = 0; i < model.primitives.size(); ++i) {
		const int imageIndex = model.primitives[i].baseColorTextureIndex;
		if (imageIndex < 0 || !model.primitives[i].hasUv)
			continue;
		auto cached = imageTextureCache.find(imageIndex);
		if (cached != imageTextureCache.end()) {
			primitiveTextureIds[i] = cached->second;
			continue;
		}
		const auto& image = model.images[imageIndex];
		if (image.data.empty()) {
			auto* logger = RLLogger::getInstance();
			if (logger)
				logger->warn("纹理: image {} 无内嵌数据（多 buffer .gltf 不支持），primitive {} 用白纹理",
					imageIndex, i);
			continue;
		}
		auto textureResult = loadEmbedded(resources, device,
			image.data.data(), image.data.size(),
			fmt::format("texture.image.{}", imageIndex).c_str());
		if (!textureResult)
			return std::unexpected(std::move(textureResult).error());
		primitiveTextureIds[i] = *textureResult;
		imageTextureCache[imageIndex] = *textureResult;
	}
	auto* logger = RLLogger::getInstance();
	if (logger) {
		const uint32_t textureCount = resources.getAssetTable().textureSize();
		uint32_t texturedPrimitives = 0;
		for (uint32_t id : primitiveTextureIds)
			if (id != whiteId) texturedPrimitives++;
		logger->info("textures: {} 资产（白 + {} 图片），{} / {} primitive 带纹理",
			textureCount, textureCount - 1, texturedPrimitives, model.primitives.size());
	}
	return primitiveTextureIds;
}

// texel 回读金丝雀（heap_texture_research.md §12.7 手段②，重构批次 4）：
// 第 N 帧重读白纹理 texel (0,0) 比对已知值 255——纹理 BO 死了（freed/VA 复用）回读立即
// 偏/fault，把"时好时坏"变成"帧 N 必炸"（UAF 签名，ASan 对 GPU 侧 UAF 结构性失明 §12.7）。
// 白纹理已知值 = 免启动基线回读；白纹理与图片纹理同层同寿命，它死 = 全死。
// 门控：RGL_TEXEL_CANARY=N（帧号，默认关）；调用点在 renderSystem 帧 N 的 drawFrame 前。
export void texelCanaryCheck(RGL::Resources& resources, RGL::DeviceContext& device,
	const vk::raii::CommandPool& cmdPool, uint64_t frame) {
	auto* logger = RLLogger::getInstance();
	const auto& assets = resources.getAssetTable();
	if (assets.textureSize() == 0) {
		if (logger)
			logger->warn("texel canary (frame {}): 无纹理资产（未加载模型），跳过", frame);
		return;
	}
	const auto& white = assets.getTexture(0);  // 首纹理槽 = 白（调用序契约）
	// ① waitForIdle：排空 in-flight 采样，消回读/采样竞态（诊断路径，帧 N 单次 GPU 停顿
	// 可接受；env 门控默认关，正常路径零影响）
	device.getDevice().waitIdle();
	// ② 16B 回读缓冲（texel 4B + 余量），常驻映射（同 upload 期 DIAG 回读模式）
	vk::BufferCreateInfo rbInfo{};
	rbInfo.size = 16;
	rbInfo.usage = vk::BufferUsageFlagBits::eTransferDst;
	VmaAllocationCreateInfo rbAlloc{};
	rbAlloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	rbAlloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo rbData{};
	auto rbResult = resources.getAllocator().createBuffer(rbInfo, rbAlloc, &rbData);
	if (!rbResult) {
		if (logger)
			logger->error("texel canary (frame {}): 回读缓冲创建失败 ({})", frame,
				std::move(rbResult).error().describe());
		return;
	}
	auto& rb = *rbResult;  // VmaBuffer 随函数作用域存活（RAII）
	// ③ SHADER_READ_ONLY → TRANSFER_SRC（回读前）→ 拷贝 → 迁回（描述符声明布局，采样前必须）
	// ——barrier 链同 upload 期 DIAG 回读（texture_pool.cppm 内）
	const auto toSrc = RGL::createImageBarrier(
		*white.image, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
		vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eFragmentShader, vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eShaderSampledRead, vk::AccessFlagBits2::eTransferRead);
	VkDependencyInfo toSrcDep{};
	toSrcDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	toSrcDep.imageMemoryBarrierCount = 1;
	toSrcDep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&toSrc);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {1, 1, 1};
	const auto toRead = RGL::createImageBarrier(
		*white.image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
		vk::AccessFlagBits2::eTransferRead, vk::AccessFlagBits2::eShaderSampledRead);
	VkDependencyInfo toReadDep{};
	toReadDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	toReadDep.imageMemoryBarrierCount = 1;
	toReadDep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&toRead);
	auto cmdAlloc = device.getDevice().allocateCommandBuffers(vk::CommandBufferAllocateInfo{
		cmdPool, vk::CommandBufferLevel::ePrimary, 1});
	auto cmd = std::move(cmdAlloc[0]);
	cmd.begin(vk::CommandBufferBeginInfo{});
	::vkCmdPipelineBarrier2(*cmd, &toSrcDep);
	::vkCmdCopyImageToBuffer(*cmd, *white.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		*rb, 1, &copy);
	::vkCmdPipelineBarrier2(*cmd, &toReadDep);
	cmd.end();
	// ④ 提交 + 等待 + 比对（白 = 255/255/255/255 已知值；sRGB 不影响全白）
	auto fence = vk::raii::Fence(device.getDevice(), vk::FenceCreateInfo{});
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	VkCommandBuffer cb = *cmd;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cb;
	if (auto result = vk::Result(::vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, *fence));
		result != vk::Result::eSuccess) {
		if (logger)
			logger->error("texel canary (frame {}): submit 失败 ({})", frame, vk::to_string(result));
		return;
	}
	(void)device.getDevice().waitForFences(*fence, true, UINT64_MAX);
	const uint8_t* px = static_cast<const uint8_t*>(rbData.pMappedData);
	if (px[0] == 255 && px[1] == 255 && px[2] == 255 && px[3] == 255) {
		if (logger)
			logger->info("texel canary (frame {}): 白纹理 texel (0,0) = 255/255/255/255 PASS", frame);
	} else {
		if (logger)
			logger->error("texel canary (frame {}): 白纹理 texel (0,0) = {}/{}/{}/{} —— 纹理 BO 已死"
				"（freed/VA 复用，heap_texture_research.md §12.6/§12.7 的 UAF 签名）",
				frame, px[0], px[1], px[2], px[3]);
	}
}

[[nodiscard]] std::expected<uint32_t, Error> uploadTexture(RGL::Resources& resources,
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
	//（最终定性 = TexturePool 生命周期 UAF，见 docs/heap_texture_research.md §12.6）
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
	//（docs/heap_texture_research.md §12.2，A/B 实验证伪"采样期解引用"旧归因），
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

}  // namespace RGL
