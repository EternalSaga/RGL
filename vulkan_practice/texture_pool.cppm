module;
#include <stb_image.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
export module texture_pool;

import rlstd;
import rllogger;
import vkerror;
import VmaRaii;
import vulkan_utils;
import vk_device;
import vk_resources;

namespace RGL {

// 纹理资产 id 的"未设置"值（同 kInvalidPipelineIndex 模式）
export inline constexpr uint32_t kInvalidTextureId = std::numeric_limits<uint32_t>::max();

// 纹理资产（Phase 3）：GPU image + view + heap 描述符槽位。
// viewCreate 必须保留：v341 的 VkImageDescriptorInfoEXT::pView 要 image view 的
// create info（值拷贝），不是句柄（docs/heap_texture_research.md §二 条款表）。
// 生命周期：TexturePool 由 main 启动期持有（本地对象）——纹理 image 经 SharedAllocator
// 控制块共享 VMA 所有权（随 Resources 的 allocator 存活），描述符 + slot 常驻 heap
//（Resources 持有），管线 mapping 烘焙 slot 后不再依赖池本体。
export struct TextureAsset {
	VmaImage image;
	vk::ImageViewCreateInfo viewCreate{};  // hpp create info（值保留）：v341 VkImageDescriptorInfoEXT::pView 要它
	vk::raii::ImageView view;
	uint32_t heapSlot = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};

// 纹理池（Phase 3）：纹理解码（stb_image）+ GPU 上传（启动期一次性）+ 描述符写入
//（SAMPLED_IMAGE，单 heap 混存 buffer+image，spike 已验证）+ slot 分配（Resources::allocSlot）。
// 上传路径：linear staging（host-visible 填充）→ optimal 拷贝 + UNDEFINED→SHADER_READ_ONLY，
// graphics 队列一次性命令缓冲 + fence waitIdle（D7：启动期路径，不启用闲置 transfer queue）。
// 内嵌 sampler 共享一份（pEmbeddedSampler，v341 位于 sourceData.constantOffset）；
// 无 mipmap（minFilter=linear、mipmapMode=NEAREST、maxLod=0），mipmap 后续加。
export class TexturePool {
   public:
	TexturePool(RGL::VmaAllocatorRAII& allocator, RGL::DeviceContext& device, RGL::Resources& resources)
		: allocator(allocator), device(device), resources(resources) {
		// 共享内嵌 sampler：linear + CLAMP_TO_EDGE（无 mipmap，maxLod 0）
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.anisotropyEnable = VK_FALSE;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 0.0f;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
		// 共享 sampler 描述符写入 sampler heap（启动期一次性）：
		// pEmbeddedSampler 对 Slang OpTypeImage+OpTypeSampler 分离对未获 RADV 采纳（采样恒 0），
		// 走独立 sampler heap（vkCmdBindSamplerHeapEXT 每帧绑定，见 vkresources.cppm createSamplerHeap）
		sharedSamplerSlot = resources.writeSamplerDescriptor(samplerInfo);
	}

	// glb 内嵌图（buffer view byte span）→ 资产 id；解码强制 RGBA8 4 通道
	[[nodiscard]] std::expected<uint32_t, Error> loadEmbedded(const uint8_t* data, size_t size, const char* name) {
		int w = 0, h = 0, ch = 0;
		uint8_t* rgba = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &ch, 4);
		if (!rgba) {
			return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
				std::string("stb_image decode failed: ") + name});
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
					std::copy_n(s, 4, up.data() + (static_cast<size_t>(y) * target + x) * 4);
				}
			stbi_image_free(rgba);
			return upload(up.data(), target, target, name);
		}
		if (forceWhite) {
			std::fill(rgba, rgba + static_cast<size_t>(w) * h * 4, 255);
		}
		auto result = upload(rgba, static_cast<uint32_t>(w), static_cast<uint32_t>(h), name);
		stbi_image_free(rgba);
		return result;
	}

	// 外部图片文件（png/jpg）→ 资产 id（path 缓存，同图不重复上传）
	[[nodiscard]] std::expected<uint32_t, Error> loadFile(const std::string& path) {
		auto cached = fileCache.find(path);
		if (cached != fileCache.end()) return cached->second;
		int w = 0, h = 0, ch = 0;
		uint8_t* rgba = stbi_load(path.c_str(), &w, &h, &ch, 4);
		if (!rgba) {
			return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
				"stb_image decode failed: " + path});
		}
		auto result = upload(rgba, static_cast<uint32_t>(w), static_cast<uint32_t>(h), path.c_str());
		stbi_image_free(rgba);
		if (!result) return std::unexpected(std::move(result).error());
		fileCache[path] = *result;
		return *result;
	}

	// 1×1 白纹理（无纹理/无 uv primitive 的共用特例，D4）。
	// 布局约定"纹理首槽 = 白"依赖调用序：模型顶点槽分配完（importer）后再取白纹理，
	// 统一分配器单调分配自然落在 2+N（见 vkresources.cppm 布局注释）。
	uint32_t getWhite() {
		if (whiteId != kInvalidTextureId) return whiteId;
		const uint8_t rgba[4] = {255, 255, 255, 255};
		auto result = upload(rgba, 1, 1, "texture.white");
		if (!result)
			throw std::runtime_error("TexturePool: white texture: " + result.error().describe());
		whiteId = *result;
		return whiteId;
	}

	[[nodiscard]] const TextureAsset& get(uint32_t id) const { return assets[id]; }
	// 共享 sampler 在 sampler heap 的槽号（管线 OpTypeSampler 映射的 samplerHeapOffset 用；
	// 偏移 = samplerSlot * samplerHeap.getDescriptorSize()，见 vkrenderer addModelPipeline）
	[[nodiscard]] uint32_t samplerSlot() const { return sharedSamplerSlot; }
	[[nodiscard]] uint32_t size() const { return static_cast<uint32_t>(assets.size()); }

    private:
	// RGBA8 像素 → GPU 纹理 + 描述符 + slot（上传全流程，见类头注释）
	[[nodiscard]] std::expected<uint32_t, Error> upload(const uint8_t* rgba, uint32_t w, uint32_t h, const char* name) {
		auto* logger = RLLogger::getInstance();
		auto& dev = device.getDevice();

		// sRGB（glTF baseColor 纹理约定，硬件完成 sRGB→线性）；不支持回落 UNORM（R4）
		// DIAG（4K 黑屏排查）：TEXTURE_FORCE_UNORM=1 强制 UNORM（sRGB 嫌疑隔离）
		vk::Format format = std::getenv("TEXTURE_FORCE_UNORM")
			? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
		auto formatProps = device.getPhysicalDevice().getFormatProperties(format);
		if ((formatProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
			== vk::FormatFeatureFlags{}) {
			format = vk::Format::eR8G8B8A8Unorm;
			if (logger) logger->warn("TexturePool: R8G8B8A8_SRGB 不支持采样，回落 UNORM ({})", name);
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
		//（根因 = APU high VA 段，见 docs/heap_texture_research.md）
		imageAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		VmaAllocationInfo imageAllocData{};
		auto imageResult = allocator.createImage(imageInfo, imageAlloc, &imageAllocData);
		if (!imageResult) return std::unexpected(std::move(imageResult).error());
		auto image = std::move(*imageResult);
		// DIAG（4K 黑屏排查 2026-08-27）：VMA 选中的内存类型（实测 4K/1×1 均 type=0 pool=0
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

		// view（create info 值保留——v341 pView 契约；raii view 由 create info 构造，同 llm_debug 模式）。
		// pView 指向的 create info 必须**指针稳定**：RADV 在采样期（非写描述符期）解引用
		// pView 解析 image，upload() 返回后栈上局部 create info 悬空 → 采样命中运行时
		// 栈上复用的其他 view create info（实测 = 当前离屏渲染目标，采样恒 0.1 灰，
		// Phase 3 全黑排查 2026-08-27，见 docs/heap_texture_research.md）。
		// 存进 deque（push_back 不搬迁已有元素，地址永久有效）；spike 的 tvi 是
		// main() 内全局寿命所以从未触发。
		viewStores.push_back(vk::ImageViewCreateInfo{});
		auto& viewCreate = viewStores.back();
		viewCreate.image = *image;
		viewCreate.viewType = vk::ImageViewType::e2D;
		viewCreate.format = format;
		viewCreate.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		auto view = vk::raii::ImageView(dev, viewCreate);

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
				"TexturePool: upload submit failed: " + std::string(name)});
		if (dev.waitForFences(std::array<vk::Fence, 1>{*fence}, true, UINT64_MAX) != vk::Result::eSuccess)
			return std::unexpected(Error{ErrorCode::VulkanError, "TexturePool: upload fence timeout: " + std::string(name)});

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

		// 描述符结构体同样存稳定存储：RADV 对 descriptor heap 描述符的指针链
		// （pResources → data.pImage → pView）在**采样期**解引用（非写描述符期），
		// 栈上局部会在 upload() 返回后悬空——spike 的 infos/imgDi/tvi 全部 main()
		// 作用域、永活，所以从未触发（Phase 3 全黑排查 2026-08-27，
		// 见 docs/heap_texture_research.md）。deque 元素地址 push_back 后不失效。
		imageDescStores.push_back(vk::ImageDescriptorInfoEXT{});
		auto& imageDesc = imageDescStores.back();
		imageDesc.pView = &viewCreate;  // hpp create info 指针（v341 契约，spike 已验证）
		imageDesc.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
		resourceDescStores.push_back(vk::ResourceDescriptorInfoEXT{});
		auto& desc = resourceDescStores.back();
		desc.type = vk::DescriptorType::eSampledImage;
		desc.data.pImage = &imageDesc;
		auto slotAddr = heap.getSlotCpuAddress(slot);
		if (!slotAddr) return std::unexpected(std::move(slotAddr).error());
		// 写入目标范围（dst）同样必须指针稳定：4K 黑屏排查（2026-08-27）实测栈上局部
		// dst 在 upload() 返回后悬空——RADV 采样期解引用时读已释放栈槽（4K 路径栈槽
		// 被复用 → 坏地址 → GPUVM TCP 权限 fault / 整 context 丢失；256 路径栈槽恰好
		// 未复用 → 侥幸正常）。改存 deque + 裸 C 调用（绕开 hpp 重载/转换歧义）。
		dstStores.push_back(vk::HostAddressRangeEXT{*slotAddr, heap.getDescriptorSize()});
		auto& dst = dstStores.back();
		// 2 参 ArrayProxy 重载：proxy 引用 desc/dst（deque 元素，地址稳定），内部
		// reinterpret_cast 传地址给 driver——loader 不导出该设备级扩展符号，只能走
		// hpp dispatcher（vkGetDeviceProcAddr），同 spike 的运行时解析结论
		dev.writeResourceDescriptorsEXT(desc, dst);

		TextureAsset asset{std::move(image), viewCreate, std::move(view), slot, w, h};
		if (logger)
			logger->info("TexturePool: {} {}x{} {} → slot {} ({}MB)", name, w, h,
				format == vk::Format::eR8G8B8A8Srgb ? "sRGB" : "UNORM", slot,
				static_cast<double>(w) * h * 4.0 / (1024.0 * 1024.0));
		assets.push_back(std::move(asset));
		return static_cast<uint32_t>(assets.size() - 1);
	}

	RGL::VmaAllocatorRAII& allocator;
	RGL::DeviceContext& device;
	RGL::Resources& resources;
	std::vector<TextureAsset> assets;
	// 描述符指针链的稳定存储（deque：元素地址不随 push_back 失效）——RADV 在采样期
	// 解引用 pResources/data.pImage/pView（见 upload 内注释），生命周期须覆盖运行期
	std::deque<vk::ResourceDescriptorInfoEXT> resourceDescStores;
	std::deque<vk::ImageDescriptorInfoEXT> imageDescStores;
	std::deque<vk::HostAddressRangeEXT> dstStores;
	std::deque<vk::ImageViewCreateInfo> viewStores;
	uint32_t whiteId = kInvalidTextureId;
	std::map<std::string, uint32_t> fileCache;
	VkSamplerCreateInfo samplerInfo{};
	uint32_t sharedSamplerSlot = 0;  // 共享 sampler 在 sampler heap 的槽号（构造期写入）
};

}  // namespace RGL
