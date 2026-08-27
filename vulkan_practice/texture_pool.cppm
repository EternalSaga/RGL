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
		vk::Format format = vk::Format::eR8G8B8A8Srgb;
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
		imageInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
		imageInfo.initialLayout = vk::ImageLayout::eUndefined;
		VmaAllocationCreateInfo imageAlloc{};
		imageAlloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		auto imageResult = allocator.createImage(imageInfo, imageAlloc);
		if (!imageResult) return std::unexpected(std::move(imageResult).error());
		auto image = std::move(*imageResult);

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
		uint8_t* rowBase = static_cast<uint8_t*>(stagingAllocData.pMappedData) + subLayout.offset;
		const size_t rowBytes = static_cast<size_t>(w) * 4;
		for (uint32_t y = 0; y < h; ++y)
			std::memcpy(rowBase + y * subLayout.rowPitch, rgba + y * rowBytes, rowBytes);

		// view（create info 值保留——v341 pView 契约；raii view 由 create info 构造，同 llm_debug 模式）
		vk::ImageViewCreateInfo viewCreate{};
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

		// slot 分配 + 描述符写入（SAMPLED_IMAGE，v341 4 参形态，spike 已验证）
		auto slotResult = resources.allocSlot(name);
		if (!slotResult) return std::unexpected(std::move(slotResult).error());
		const uint32_t slot = *slotResult;
		const auto& heap = resources.getDescriptorHeap();

		vk::ImageDescriptorInfoEXT imageDesc{};
		imageDesc.pView = &viewCreate;  // hpp create info 指针（v341 契约，spike 已验证）
		imageDesc.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
		vk::ResourceDescriptorInfoEXT desc{};
		desc.type = vk::DescriptorType::eSampledImage;
		desc.data.pImage = &imageDesc;
		auto slotAddr = heap.getSlotCpuAddress(slot);
		if (!slotAddr) return std::unexpected(std::move(slotAddr).error());
		vk::HostAddressRangeEXT dst{*slotAddr, heap.getDescriptorSize()};
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
	uint32_t whiteId = kInvalidTextureId;
	std::map<std::string, uint32_t> fileCache;
	VkSamplerCreateInfo samplerInfo{};
	uint32_t sharedSamplerSlot = 0;  // 共享 sampler 在 sampler heap 的槽号（构造期写入）
};

}  // namespace RGL
