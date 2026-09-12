module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <fmt/format.h>
module texture_upload:impl;

import texture_upload;  // clang 不给 partition impl unit 隐式 import 主接口
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import VmaRaii;
import vulkan_utils;
import vk_device;
import vk_resources;
import mip_gen;        // Phase 8 ③b：compute mip 链（录进本纹理上传 CB）
import asset_table;
import diagnostics;  // texture_force_unorm 诊断单例（configs/diagnostics.lua）

namespace RGL {

// uploadTexture/uploadCubeMap 定义（2026-09-02 编译提速 A 方案）：~530 行函数体移出
// 接口 TU——BMI 只含声明；本单元是叶子边、编译不上关键路径。语义注释留接口声明处。

namespace {

/**
 * 填充 linear staging 图（逐行；rowPitch 由驱动给出，linear tiling 行距可能 > w*4）。
 * @param dev 设备句柄（vkGetImageSubresourceLayout）
 * @param staging linear staging 图（已映射，CPU_TO_GPU）
 * @param allocData 分配信息（pMappedData 行基址）
 * @param rgba CPU 侧 RGBA8 像素（w×h×4B，top-down）
 * @param w 宽
 * @param h 高
 */
void fillLinearStaging(vk::raii::Device& dev, RGL::VmaImage& staging,
	const VmaAllocationInfo& allocData, const uint8_t* rgba, uint32_t w, uint32_t h) {
	VkImageSubresource sub{};
	sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	VkSubresourceLayout subLayout;
	vkGetImageSubresourceLayout(*dev, staging.image, &sub, &subLayout);
	uint8_t* rowBase = static_cast<uint8_t*>(allocData.pMappedData) + subLayout.offset;
	const size_t rowBytes = static_cast<size_t>(w) * 4;
	for (uint32_t y = 0; y < h; ++y)
		std::memcpy(rowBase + y * subLayout.rowPitch, rgba + y * rowBytes, rowBytes);
}

/**
 * 录 mip0 上传 + compute mip 链进 cmd（Phase 8 ③b）：staging UNDEFINED→SRC + 目标图
 * mip0 UNDEFINED→TRANSFER_DST → 拷贝 mip0 → mip0 →SHADER_READ_ONLY → mipLevels>1 时
 * MipGen::recordChain 续录 level 1..L−1 链（per-level barrier + dispatch，见 mip_gen）。
 * 裸 C barrier（v341 的 vk::DependencyInfo wrapper 缺 stage/access 字段，勿用 hpp
 * 包装类；现行 spec v1.4.360：DependencyInfo 无 stage/access 字段，全部由 barrier
 * 自身携带）。mip0 的 SRO 过渡 dstStage = COMPUTE——同 CB 内首个消费者 = 下级 mip
 * 的 compute 采样（渲染端 fragment 消费在后续提交，fence 同步）。
 * @param cmd 目标命令缓冲（录制中）
 * @param staging linear staging 图
 * @param image 目标图（mip0 = 拷贝目标；level ≥1 = mip 链目标）
 * @param w 宽
 * @param h 高
 * @param mipLevels 总 mip 级数（1 = 仅拷贝，不录链）
 * @param format 图格式
 * @param isSrgb sRGB 纹理（mip 链走 recordMipBlitChain；false = UNORM compute 链）
 * @param mipGen mip 链生成器（per-level 视图/描述符集存其成员容器）
 * @param name 日志名
 * @return Error = mip 链录制失败（描述符集分配失败）
 */
std::expected<void, Error> recordMipZeroAndChain(vk::raii::CommandBuffer& cmd,
	RGL::VmaImage& staging, RGL::VmaImage& image, uint32_t w, uint32_t h, uint32_t mipLevels,
	vk::Format format, bool isSrgb, RGL::MipGen& mipGen, const char* name) {
	// staging UNDEFINED→SRC_OPTIMAL（首用需显式过渡，含 host write 同步）+ mip0 UNDEFINED→TRANSFER_DST
	auto toSrcBarrier = RGL::createImageBarrier(
		staging.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferSrcOptimal,
		vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eBottomOfPipe, vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eTransferRead);
	auto toDstBarrier = RGL::createImageBarrier(
		image.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
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
	::vkCmdCopyImage(*cmd, staging.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
	auto toReadBarrier = RGL::createImageBarrier(
		image.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderSampledRead);
	VkDependencyInfo toReadDep{};
	toReadDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	toReadDep.imageMemoryBarrierCount = 1;
	toReadDep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&toReadBarrier);
	::vkCmdPipelineBarrier2(*cmd, &toReadDep);
	// Phase 8 ③b：mip 链（level ≥1；1×1 / 单级图跳过）。sRGB = LINEAR blit 链（RADV
	// SRGB 无 STORAGE_IMAGE 能力，见 uploadTexture usage 注释）；UNORM = compute 链
	if (mipLevels > 1) {
		if (isSrgb) {
			recordMipBlitChain(cmd, image.image, w, h, mipLevels);
			return {};
		}
		return mipGen.recordChain(cmd, image.image, format, w, h, mipLevels, name);
	}
	return {};
}

}  // namespace

/** uploadTexture 定义（契约与行为注释见 texture_upload.cppm 接口声明）。 */
export [[nodiscard]] std::expected<uint32_t, Error> uploadTexture(RGL::Resources& resources,
	RGL::DeviceContext& device, const uint8_t* rgba, uint32_t w, uint32_t h, bool linear,
	const char* name) {
	auto* logger = RLLogger::getInstance();
	auto& dev = device.getDevice();
	auto& allocator = resources.getAllocator();

	// sRGB（glTF baseColor 纹理约定，硬件完成 sRGB→线性）；不支持采样回落 UNORM（R4）
	// linear = 数据纹理（normal/ORM）强制 UNORM——sRGB 解码污染线性数值（批次 ③a 实证：
	// G-buffer roughness 恒 0.502 = 0.737 的 sRGB→线性值）
	// DIAG（configs/diagnostics.lua texture_force_unorm，原 env 转配置）：强制 UNORM（sRGB 嫌疑隔离）
	vk::Format format = (linear || diagnostics().textureForceUnorm)
		? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
	auto formatProps = device.getPhysicalDevice().getFormatProperties(format);
	if ((formatProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
		== vk::FormatFeatureFlags{}) {
		format = vk::Format::eR8G8B8A8Unorm;
		if (logger) logger->warn("texture: R8G8B8A8_SRGB 不支持采样，回落 UNORM ({})", name);
	}
	// Phase 8 ③b：全 mip 链 1+floor(log2(max(w,h)))（1×1 = 1 级，跳过 mip 生成）——
	// 消除缩小采样无 mip 的走样 speckle（平面墙/广场），见 mip_gen。sRGB 图（baseColor）
	// 不能带 eStorage（RADV SRGB 无 STORAGE_IMAGE 能力 → 图创建 FORMAT_NOT_SUPPORTED，
	// 且本 spec 视图格式须支持图的全部 usage）→ 走 LINEAR blit 链；UNORM 走 compute 链
	const bool isSrgb = format == vk::Format::eR8G8B8A8Srgb;
	uint32_t mipLevels = 1;
	for (uint32_t s = std::max(w, h); s > 1; s >>= 1) ++mipLevels;

	vk::ImageCreateInfo imageInfo{};
	imageInfo.imageType = vk::ImageType::e2D;
	imageInfo.format = format;
	imageInfo.extent = vk::Extent3D(w, h, 1);
	imageInfo.mipLevels = mipLevels;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = vk::SampleCountFlagBits::e1;
	imageInfo.tiling = vk::ImageTiling::eOptimal;
	imageInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
		vk::ImageUsageFlagBits::eTransferSrc |  // TransferSrc：texel 回读诊断/后续校验用
		(!isSrgb ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlags{});  // compute 链（仅 UNORM）
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

	// 全 mip 链 view（自注册段上移——注册段描述符堆 SAMPLED_IMAGE 描述符须覆盖全链；
	// create info 值保留 = v341 pView 契约。vk::raii::* 默认构造已删除：先建局部 view，
	// 提交 + fence 后再整体 move 进资产记录（项目 RAII 规则））
	vk::ImageViewCreateInfo viewCreate{};
	viewCreate.image = *image;
	viewCreate.viewType = vk::ImageViewType::e2D;
	viewCreate.format = format;
	viewCreate.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1};
	auto view = vk::raii::ImageView(dev, viewCreate);

	// linear staging（host-visible + 常驻映射）：CPU 填充后 GPU 拷贝
	// mipLevels 显式 1——linear 硬限 VUID-VkImageCreateInfo-tiling-02260（目标图全链
	// 拷贝到 imageInfo 会带上 mipLevels>1）
	vk::ImageCreateInfo stagingInfo = imageInfo;
	stagingInfo.tiling = vk::ImageTiling::eLinear;
	stagingInfo.mipLevels = 1;
	stagingInfo.usage = vk::ImageUsageFlagBits::eTransferSrc;
	VmaAllocationCreateInfo stagingAlloc{};
	stagingAlloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	stagingAlloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo stagingAllocData{};
	auto stagingResult = allocator.createImage(stagingInfo, stagingAlloc, &stagingAllocData);
	if (!stagingResult) return std::unexpected(std::move(stagingResult).error());
	auto staging = std::move(*stagingResult);
	fillLinearStaging(dev, staging, stagingAllocData, rgba, w, h);

	// 一次性命令缓冲（graphics 队列，启动期无并发）：mip0 拷贝 +（Phase 8 ③b）compute
	// mip 链——同一 fence 等全部完成（mip 链录进本 CB，无额外 submit 往返）
	auto cmdAlloc = dev.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
		*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary, 1});
	auto cmd = std::move(cmdAlloc[0]);
	cmd.begin(vk::CommandBufferBeginInfo{});
	// UNORM compute 链走 descriptor heap + shader object：录制前备齐本纹理 mip 级数对应的
	// level shader/槽（幂等增量；仅 UNORM 多 mip 触发，sRGB blit 链无描述符）。时序天然
	// 晚于几何段与白纹理首槽（A2 阶段，见 ensureMipGenReady 接口"时序契约"）。
	if (!isSrgb && mipLevels > 1) {
		if (auto ready = resources.ensureMipGenReady(mipLevels); !ready)
			return std::unexpected(std::move(ready).error());
	}
	auto recordResult = recordMipZeroAndChain(cmd, staging, image, w, h, mipLevels,
		format, isSrgb, resources.getMipGen(), name);
	if (!recordResult) return std::unexpected(std::move(recordResult).error());
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

	// slot 分配 + 描述符写入（SAMPLED_IMAGE，v341 4 参形态，spike 已验证）
	auto slotResult = resources.allocSlot(name);
	if (!slotResult) return std::unexpected(std::move(slotResult).error());
	const uint32_t slot = *slotResult;
	const auto& heap = resources.getDescriptorHeap();

	// 资产记录（GPU 数据 + 描述符指针链）入资产表：记录在 deque（元素地址稳定），
	// 自/兄弟引用在入表后设置有效。跨驱动防御：本机 RADV 写时即解析
	//（docs/archive/heap_texture_research.md §12.2，A/B 实验证伪"采样期解引用"旧归因），
	// 但 spec 无调用后指针生命周期条款，指针稳定性 = 对其他驱动的正确性前提
	TextureAsset asset{std::move(image), viewCreate, std::move(view), slot, w, h, mipLevels};
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

	return static_cast<uint32_t>(resources.getAssetTable().textureSize() - 1);
}


}  // namespace RGL
