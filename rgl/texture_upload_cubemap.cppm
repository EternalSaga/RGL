module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <fmt/format.h>
module texture_upload:cubemap;

import texture_upload;  // clang 不给 partition impl unit 隐式 import 主接口
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import VmaRaii;
import vulkan_utils;
import vk_device;
import vk_resources;
import env_prefilter;  // Phase 8 ④b：GGX 预滤波 env cubemap（env.getPrefilter().recordPrefilter，16F 分支）
import environment;  // 环境服务（env 单例入库/sky sampler/预滤波供给，架构债务续账步骤 3）
import asset_table;
import diagnostics;  // texture_force_unorm 诊断单例（configs/diagnostics.lua）

namespace RGL {

// cubemap 上传单元（2026-09-11 Phase 8 批次 ②b 自 texture_upload_impl 拆出——文件超
// 500 代码行阈值）：uploadCubeMap / uploadCubeMap16F 共享核心（建图/上传/金丝雀/入库）。
// 语义注释留接口声明处；本单元是叶子边、编译不上关键路径。

namespace {

/** cubemap 上传核心参数：格式 + 每 texel 字节数 + mip 链 blit 滤镜 + 日志标签 + 是否走
 * GGX 预滤波（16F HDR env = true：加 eStorage usage + mip 链走 heap compute 预滤波；8-bit
 * PNG env = false：usage 不含 eStorage，mip 链走 LINEAR/NEAREST blit，完全不变）。 */
struct CubeFormat {
	vk::Format format;
	uint32_t bytesPerTexel;
	VkFilter mipFilter;
	const char* label;
	bool ggxPrefilter = false;
};

/** 上传期资源束：cube 目标图（optimal，全 mip）+ 6 张 per-face linear staging。 */
struct CubeImages {
	RGL::VmaImage image;
	std::vector<RGL::VmaImage> stagings;
	std::array<VmaAllocationInfo, 6> stagingData{};
	uint32_t mipLevels = 1;
};

/**
 * 建 cube 目标图（e2D+arrayLayers=6+eCubeCompatible，全 mip，GPU_ONLY 独占分配）+
 * 6 张 linear staging（RADV linear 三硬限：mipLevels=1 / 无 cube flag / maxArrayLayers=1，
 * 见原注释 2026-09-01 坑）。
 * @param resources 资源层（VMA 分配器）
 * @param size 面边长（2 的幂，调用方校验）
 * @param fmt 上传格式参数
 * @return 资源束；VMA 失败 = Error
 */
std::expected<CubeImages, Error> createCubeImages(RGL::Resources& resources, uint32_t size,
	const CubeFormat& fmt) {
	auto& allocator = resources.getAllocator();
	uint32_t mipLevels = 1;
	for (uint32_t s = size; s > 1; s >>= 1) ++mipLevels;

	// Vulkan cubemap 无独立 imageType：e2D + arrayLayers=6 + eCubeCompatible flag
	//（view 才用 eCube；GL/D3D 的 CUBE 类型概念在 Vulkan 不存在）
	vk::ImageCreateInfo imageInfo{};
	imageInfo.flags = vk::ImageCreateFlagBits::eCubeCompatible;
	imageInfo.imageType = vk::ImageType::e2D;
	imageInfo.format = fmt.format;
	imageInfo.extent = vk::Extent3D(size, size, 1);
	imageInfo.mipLevels = mipLevels;
	imageInfo.arrayLayers = 6;
	imageInfo.samples = vk::SampleCountFlagBits::e1;
	imageInfo.tiling = vk::ImageTiling::eOptimal;
	imageInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
		vk::ImageUsageFlagBits::eTransferSrc;
	// GGX 预滤波（16F HDR env）额外加 eStorage：dst 第 level 级 storage 2DArray view 写前提。
	// 8-bit sRGB cube 不加（RADV SRGB 无 STORAGE_IMAGE 能力；且视图格式须支持图全部 usage）。
	if (fmt.ggxPrefilter)
		imageInfo.usage |= vk::ImageUsageFlagBits::eStorage;
	imageInfo.initialLayout = vk::ImageLayout::eUndefined;
	VmaAllocationCreateInfo imageAlloc{};
	imageAlloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	imageAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;  // 独占分配（2D 路径现状一致）
	auto imageResult = allocator.createImage(imageInfo, imageAlloc);
	if (!imageResult)
		return std::unexpected(std::move(imageResult).error());

	CubeImages out{std::move(*imageResult), {}, {}, mipLevels};
	// linear staging：**6 张独立 2D 图**（每面一张）。不继承 imageInfo——linear 有三条硬限
	//（RADV 实测连踩两坑 2026-09-01）：VUID-VkImageCreateInfo-tiling-02260 linear 必须
	// mipLevels=1；CUBE_COMPATIBLE flag linear 不支持（cube-ness 只需目标图有）；
	// RADV linear 图 maxArrayLayers=1（arrayed linear FORMAT_NOT_SUPPORTED）→ per-face 单图
	vk::ImageCreateInfo stagingInfo{};
	stagingInfo.imageType = vk::ImageType::e2D;
	stagingInfo.format = fmt.format;
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
	for (uint32_t face = 0; face < 6; ++face) {
		auto stagingResult = allocator.createImage(stagingInfo, stagingAlloc,
			&out.stagingData[face]);
		if (!stagingResult)
			return std::unexpected(std::move(stagingResult).error());
		out.stagings.push_back(std::move(*stagingResult));
	}
	return out;
}

/** 单条 ImageMemoryBarrier2 包成 VkDependencyInfo 提交（recordCubeUpload 的 emitBarrier 与
 * cubeCanary 的 rbEmit 两处逻辑逐字相同、仅命令缓冲不同 → 提升为文件内共用 helper）。
 * @param cmd 目标命令缓冲（原 lambda 捕获的 *cmd / *rbCmd）
 * @param barrier 单条 image barrier（内部 reinterpret 为 C 结构喂 vkCmdPipelineBarrier2） */
void emitSingleBarrier(VkCommandBuffer cmd, const vk::ImageMemoryBarrier2& barrier) {
	VkDependencyInfo dep{};
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&barrier);
	::vkCmdPipelineBarrier2(cmd, &dep);
}

/** CPU 侧按行距把 6 面源像素逐行 memcpy 进各自 linear staging（staging 行距经
 * vkGetImageSubresourceLayout 取 rowPitch/offset；源面行距 = size×bytesPerTexel）。
 * @param device 设备（取 vk::raii::Device 查 subresource layout）
 * @param images 资源束（staging 图 + mapped 内存）
 * @param faces 6 面源像素（top-down）
 * @param size 面边长
 * @param fmt 格式（bytesPerTexel 定源行距） */
void fillCubeStagings(RGL::DeviceContext& device, CubeImages& images,
	std::span<const uint8_t* const, 6> faces, uint32_t size, const CubeFormat& fmt) {
	auto& dev = device.getDevice();
	const size_t faceRowBytes = static_cast<size_t>(size) * fmt.bytesPerTexel;
	for (uint32_t face = 0; face < 6; ++face) {
		VkImageSubresource sub{};
		sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		VkSubresourceLayout layerLayout;
		vkGetImageSubresourceLayout(*dev, *images.stagings[face], &sub, &layerLayout);
		uint8_t* base = static_cast<uint8_t*>(images.stagingData[face].pMappedData)
			+ layerLayout.offset;
		for (uint32_t y = 0; y < size; ++y)
			std::memcpy(base + static_cast<size_t>(y) * layerLayout.rowPitch,
				faces[face] + static_cast<size_t>(y) * faceRowBytes, faceRowBytes);
	}
}

/** 录制上传段：6 张 staging→SRC + cube→DST 批量 barrier（GGX 只覆盖 mip0，level ≥1 留
 * UNDEFINED 待预滤波）→ per-face copy 进 cube mip0。
 * @param cmd 录制中的命令缓冲
 * @param images 资源束
 * @param size 面边长
 * @param fmt 格式（ggxPrefilter 决定 DST barrier level 覆盖）
 * @param mipLevels 全级数（非 GGX 时 DST barrier 覆盖全部 mip） */
void recordCubeBarriersAndCopies(vk::raii::CommandBuffer& cmd, CubeImages& images,
	uint32_t size, const CubeFormat& fmt, uint32_t mipLevels) {
	std::vector<vk::ImageMemoryBarrier2> uploadBarriers;
	uploadBarriers.reserve(7);
	for (uint32_t face = 0; face < 6; ++face)
		uploadBarriers.push_back(RGL::createImageBarrier(
			*images.stagings[face], vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferSrcOptimal,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eBottomOfPipe, vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eTransferRead));
	uploadBarriers.push_back(RGL::createImageBarrier(
		*images.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
		vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eBottomOfPipe, vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eTransferWrite,
		0, fmt.ggxPrefilter ? 1u : mipLevels, 0, 6));  // GGX：只覆盖 mip0（level ≥1 留 UNDEFINED 待预滤波）
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
		::vkCmdCopyImage(*cmd, *images.stagings[face], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			*images.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
	}
}

/** GGX 预滤波 mip 链（16F HDR env，Phase 8 ④b）：mip0 TRANSFER_DST→SHADER_READ_ONLY barrier
 * → ensureEnvPrefilterReady → recordPrefilter（逐级 UNDEFINED→GENERAL→dispatch→SRO，返回后全级
 * 已 SRO、无需 blit 收尾 barrier）；任一失败 = Error（调用方据此不 submit）。
 * @param resources 资源层（env-prefilter heap 基础设施）
 * @param cmd 录制中的命令缓冲
 * @param images cube 目标图
 * @param fmt 格式（GGX 用 format 传 recordPrefilter）
 * @param size 面边长
 * @param mipLevels 全级数
 * @param name 日志名
 * @return env-prefilter 准备 / recordPrefilter 失败 = Error */
std::expected<void, Error> recordGgxMipChain(RGL::Resources& resources, RGL::Environment& env,
	vk::raii::CommandBuffer& cmd, CubeImages& images, const CubeFormat& fmt, uint32_t size,
	uint32_t mipLevels, const char* name) {
	// 16F HDR env（Phase 8 ④b）：mip 链走 heap compute GGX importance-sample 预滤波（替代
	// LINEAR blit box 平均）。先 mip0：TRANSFER_DST → SHADER_READ_ONLY（compute 采样源
	// SampleLevel(_,L,0)，SampleLevel lod=0 恒取 mip0）；level ≥1 留 UNDEFINED（初始上传
	// barrier 只覆盖 mip0），recordPrefilter 逐级 UNDEFINED→GENERAL→dispatch(fs/8,fs/8,6)
	// →SHADER_READ_ONLY，返回后全级已 SRO（渲染端最终布局），无需下方 blit 收尾 barrier。
	emitSingleBarrier(*cmd, RGL::createImageBarrier(*images.image, vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderSampledRead,
		0, 1, 0, 6));
	// 备齐 env-prefilter heap 基础设施（幂等增量；prefilter 辅助槽走运行时段）
	if (auto ready = env.ensurePrefilterReady(mipLevels); !ready)
		return std::unexpected(std::move(ready).error());
	if (auto pf = env.getPrefilter().recordPrefilter(cmd, *images.image, fmt.format,
			size, mipLevels, name); !pf)
		return std::unexpected(std::move(pf).error());
	return {};
}

/** 非预滤波 mip 链（8-bit）：逐 level 先 DST→SRC 再 6 面 blit 到 level i（同图不同 subresource
 * 不重叠，spec 允许；滤镜 = fmt.mipFilter）→ 全级 →SHADER_READ_ONLY（level 0..L−2 停 SRC、末级停 DST）。
 * @param cmd 录制中的命令缓冲
 * @param images cube 目标图
 * @param size 面边长
 * @param mipLevels 全级数
 * @param mipFilter blit 滤镜（sRGB 8-bit 只能 NEAREST，VUID-vkCmdBlitImage-filter-00239） */
void recordBlitMipChain(vk::raii::CommandBuffer& cmd, CubeImages& images, uint32_t size,
	uint32_t mipLevels, VkFilter mipFilter) {
	// mip 链（8-bit / 非预滤波）：level i−1 先 DST→SRC，再 6 面 blit 到 level i（同图不同
	// subresource 不重叠，spec 允许；滤镜见 CubeFormat::mipFilter 注）
	for (uint32_t level = 1; level < mipLevels; ++level) {
		emitSingleBarrier(*cmd, RGL::createImageBarrier(
			*images.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
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
		::vkCmdBlitImage(*cmd, *images.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			*images.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, blits.data(), mipFilter);
	}
	// → SHADER_READ_ONLY：level 0..L−2 停在 TRANSFER_SRC（blit 源），末级停 TRANSFER_DST
	if (mipLevels > 1)
		emitSingleBarrier(*cmd, RGL::createImageBarrier(*images.image, vk::ImageLayout::eTransferSrcOptimal,
			vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
			vk::AccessFlagBits2::eTransferRead, vk::AccessFlagBits2::eShaderSampledRead,
			0, mipLevels - 1, 0, 6));
	emitSingleBarrier(*cmd, RGL::createImageBarrier(*images.image, vk::ImageLayout::eTransferDstOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderSampledRead,
		mipLevels - 1, 1, 0, 6));
}

/** submit 上传 CB + fence 等待；任一失败 = Error（错误码与消息逐字保留）。
 * @param device 设备（graphics 队列提交 + 等待）
 * @param cmd 已 end() 的上传命令缓冲
 * @param name 日志名（错误消息）
 * @return submit 失败 / fence 超时 = Error */
std::expected<void, Error> submitCubeUpload(RGL::DeviceContext& device,
	vk::raii::CommandBuffer& cmd, const char* name) {
	auto& dev = device.getDevice();
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
	return {};
}

/**
 * 录制并执行上传：staging 行距填充 → 一次性 CB（staging→SRC / cube→DST 批量 barrier +
 * per-face copy + 逐 level 6 面 blit mip 链 + 全级 →SHADER_READ_ONLY）→ submit + fence。
 * mip 链滤镜 = fmt.mipFilter（sRGB 8-bit 只能 NEAREST，VUID-vkCmdBlitImage-filter-00239；
 * 16F float 可 LINEAR）。
 * @param images 资源束（createCubeImages 产物）
 * @param faces 6 面像素（各 size×size×bytesPerTexel B，top-down，Vulkan 层序 0-5）
 * @param size 面边长
 * @param fmt 上传格式参数
 * @param name 日志名
 * @return submit/fence 失败 = Error
 */
std::expected<void, Error> recordCubeUpload(RGL::Resources& resources, RGL::Environment& env, RGL::DeviceContext& device,
	CubeImages& images, std::span<const uint8_t* const, 6> faces, uint32_t size,
	const CubeFormat& fmt, const char* name) {
	auto& dev = device.getDevice();
	const uint32_t mipLevels = images.mipLevels;

	fillCubeStagings(device, images, faces, size, fmt);

	// 一次性命令缓冲（graphics 队列，启动期无并发）：per-face copy + 逐 level blit 链
	auto cmdAlloc = dev.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
		*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary, 1});
	auto cmd = std::move(cmdAlloc[0]);
	cmd.begin(vk::CommandBufferBeginInfo{});

	recordCubeBarriersAndCopies(cmd, images, size, fmt, mipLevels);

	if (fmt.ggxPrefilter) {
		// GGX 预滤波 mip 链：失败则不 submit（保持原 early-return 语义）
		if (auto r = recordGgxMipChain(resources, env, cmd, images, fmt, size, mipLevels, name); !r)
			return std::unexpected(std::move(r).error());
	} else {
		recordBlitMipChain(cmd, images, size, mipLevels, fmt.mipFilter);
	}
	cmd.end();

	return submitCubeUpload(device, cmd, name);
}

/** 构造 12 个回读 copy region：6 面 × {texel(0,0), 中心(mid,mid)}，bufferOffset 逐点累加 bytes。
 * @param size 面边长
 * @param bytes 每 texel 字节数
 * @return 12 项 VkBufferImageCopy（顺序 = face*2+t，与 compareCubeReadback 展平序一致） */
std::array<VkBufferImageCopy, 12> buildCubeReadbackCopies(uint32_t size, uint32_t bytes) {
	const uint32_t mid = size / 2;
	std::array<VkBufferImageCopy, 12> copies{};
	for (uint32_t face = 0; face < 6; ++face)
		for (uint32_t t = 0; t < 2; ++t) {
			copies[face * 2 + t].bufferOffset = (face * 2 + t) * bytes;
			copies[face * 2 + t].bufferImageHeight = 1;
			copies[face * 2 + t].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
			copies[face * 2 + t].imageOffset = t == 0
				? VkOffset3D{0, 0, 0} : VkOffset3D{static_cast<int32_t>(mid), static_cast<int32_t>(mid), 0};
			copies[face * 2 + t].imageExtent = {1, 1, 1};
		}
	return copies;
}

/** 逐面回读比对：12 采样点 memcmp 源像素，find_if 找首个不匹配即 error 日志并停（语义 = 原
 * 双循环 face<6 && pass + 内层 break 的“首个不匹配即停”）；全过则 PASS。只日志、不改返回值。
 * @param faces CPU 源像素（6 面）
 * @param px 回读 buffer mapped 指针（12×bytes，顺序 = buildCubeReadbackCopies）
 * @param size 面边长（算中心 texel）
 * @param bytes 每 texel 字节数
 * @param name 日志名 */
void compareCubeReadback(std::span<const uint8_t* const, 6> faces, const uint8_t* px,
	uint32_t size, uint32_t bytes, const char* name) {
	auto* logger = RLLogger::getInstance();
	const uint32_t mid = size / 2;
	// 展平 6 面 × {角, 中心} = 12 采样点（i = face*2 + t），逐点 memcmp 源 vs 回读
	auto expectPixel = [&](uint32_t i) -> const uint8_t* {
		const uint32_t face = i / 2;
		return i % 2 == 0 ? faces[face]
			: faces[face] + (static_cast<size_t>(mid) * size + mid) * bytes;
	};
	const std::array<uint32_t, 12> idx{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
	auto bad = std::ranges::find_if(idx,
		[&](uint32_t i) { return std::memcmp(expectPixel(i), px + i * bytes, bytes) != 0; });
	if (bad != idx.end()) {
		const uint32_t i = *bad;
		const uint8_t* expect = expectPixel(i);
		const uint8_t* got = px + i * bytes;
		if (logger)
			logger->error("cubemap canary {}: face {} texel {} = {}/{}/{}/{} != 源 {}/{}/{}/{}",
				name, i / 2, i % 2, got[0], got[1], got[2], got[3],
				expect[0], expect[1], expect[2], expect[3]);
	} else if (logger) {
		logger->info("cubemap canary {}: 6 面 × 2 texel 回读比对 PASS", name);
	}
}

/**
 * 拷贝金丝雀：逐面回读 mip0 texel (0,0) + 中心，比对 CPU 源（6×2×bytesPerTexel B）。
 * 违例只 error 日志、不改返回值（UAF 教训——把"时好时坏"变"启动必炸"的观测面）。
 * @param images 资源束（上传完成，SRO 布局）
 * @param faces CPU 源像素
 * @param size 面边长
 * @param fmt 上传格式参数（bytesPerTexel）
 * @param name 日志名
 */
void cubeCanary(RGL::Resources& resources, RGL::DeviceContext& device, CubeImages& images,
	std::span<const uint8_t* const, 6> faces, uint32_t size, const CubeFormat& fmt,
	const char* name) {
	auto& dev = device.getDevice();
	auto& allocator = resources.getAllocator();
	const uint32_t bytes = fmt.bytesPerTexel;

	// 回读 buffer：6 面 × 2 texel（角 + 中心）× bytesPerTexel
	VkBufferCreateInfo rbInfo{};
	rbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	rbInfo.size = 6 * 2 * bytes;
	rbInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo rbAlloc{};
	rbAlloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	rbAlloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo rbData{};
	auto rbResult = allocator.createBuffer(rbInfo, rbAlloc, &rbData);
	if (!rbResult)
		return;
	auto& rb = *rbResult;

	const std::array<VkBufferImageCopy, 12> copies = buildCubeReadbackCopies(size, bytes);

	// 回读 CB：SRO→SRC barrier → copy 12 点 → SRC→SRO barrier
	auto rbCmdAlloc = dev.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
		*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary, 1});
	auto rbCmd = std::move(rbCmdAlloc[0]);
	rbCmd.begin(vk::CommandBufferBeginInfo{});
	emitSingleBarrier(*rbCmd, RGL::createImageBarrier(*images.image, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::ImageLayout::eTransferSrcOptimal, vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eFragmentShader, vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eShaderSampledRead, vk::AccessFlagBits2::eTransferRead,
		0, 1, 0, 6));
	::vkCmdCopyImageToBuffer(*rbCmd, *images.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		*rb, static_cast<uint32_t>(copies.size()), copies.data());
	emitSingleBarrier(*rbCmd, RGL::createImageBarrier(*images.image, vk::ImageLayout::eTransferSrcOptimal,
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

	// 逐面回读比对（首个不匹配即 error 日志；全过 PASS）
	compareCubeReadback(faces, static_cast<const uint8_t*>(rbData.pMappedData), size, bytes, name);
}

/**
 * 入库：eCube view（全 mip + 6 层）+ 统一 slot + SAMPLED_IMAGE 描述符（指针链模式同 2D）
 * → Resources env 单例（**非** AssetTable textures——env 绑场景不绑材质）+ sky sampler
 *（maxLod = mipLevels−1）+ 日志。
 * @param images 资源束（image 移交给 env 单例）
 * @param size 面边长
 * @param fmt 上传格式参数
 * @param name 日志名
 * @return slot 分配 / 槽地址查询失败 = Error
 */
std::expected<void, Error> registerCubeEnv(RGL::Resources& resources, RGL::Environment& env, RGL::DeviceContext& device,
	CubeImages& images, uint32_t size, const CubeFormat& fmt, const char* name) {
	auto* logger = RLLogger::getInstance();
	auto& dev = device.getDevice();
	vk::ImageViewCreateInfo viewCreate{};
	viewCreate.image = *images.image;
	viewCreate.viewType = vk::ImageViewType::eCube;
	viewCreate.format = fmt.format;
	viewCreate.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, images.mipLevels, 0, 6};
	auto view = vk::raii::ImageView(dev, viewCreate);

	auto slotResult = resources.allocSlot(RGL::SlotSegment::Texture, name);
	if (!slotResult)
		return std::unexpected(std::move(slotResult).error());
	const uint32_t slot = *slotResult;
	const auto& heap = resources.getDescriptorHeap();

	TextureAsset asset{std::move(images.image), viewCreate, std::move(view), slot, size, size,
		images.mipLevels};
	auto& stored = env.emplace(std::move(asset));
	stored.imageDesc.pView = &stored.viewCreate;
	stored.imageDesc.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
	stored.resourceDesc.type = vk::DescriptorType::eSampledImage;
	stored.resourceDesc.data.pImage = &stored.imageDesc;
	auto slotAddr = heap.getSlotCpuAddress(slot);
	if (!slotAddr)
		return std::unexpected(std::move(slotAddr).error());
	stored.dst = vk::HostAddressRangeEXT{*slotAddr, heap.getDescriptorSize()};
	dev.writeResourceDescriptorsEXT(stored.resourceDesc, stored.dst);

	const uint32_t skySlot = env.ensureSkySampler(images.mipLevels);
	if (logger)
		logger->info("cubemap: {} {}²×6 {} mips={} → res slot {} + sky sampler slot {}", name,
			size, fmt.label, images.mipLevels, slot, skySlot);
	return {};
}

/**
 * 上传核心（uploadCubeMap / uploadCubeMap16F 共享）：建图 → 上传 → 金丝雀 → 入库。
 * @param faces 6 面像素（各 size×size×fmt.bytesPerTexel B，top-down，Vulkan 层序 0-5）
 * @param size 面边长（非 2 的幂 = TextureDecodeFailed）
 * @param fmt 格式参数（调用方解析，含 blit 滤镜与日志标签）
 * @param name 日志名
 * @return 任一环节失败 = Error（金丝雀除外，只日志）
 */
std::expected<void, Error> uploadCubeMapCore(RGL::Resources& resources, RGL::Environment& env,
	RGL::DeviceContext& device, std::span<const uint8_t* const, 6> faces, uint32_t size,
	const CubeFormat& fmt, const char* name) {
	if (size == 0 || (size & (size - 1)) != 0)
		return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
			fmt::format("cubemap {}: size {} 必须为 2 的幂", name, size)});
	auto images = createCubeImages(resources, size, fmt);
	if (!images)
		return std::unexpected(std::move(images).error());
	if (auto r = recordCubeUpload(resources, env, device, *images, faces, size, fmt, name); !r)
		return std::unexpected(std::move(r).error());
	cubeCanary(resources, device, *images, faces, size, fmt, name);
	return registerCubeEnv(resources, env, device, *images, size, fmt, name);
}

}  // namespace

/** uploadCubeMap 定义（契约与行为注释见 texture_upload.cppm 接口声明；格式策略 sRGB 优先回落 UNORM）。 */
[[nodiscard]] std::expected<void, Error> uploadCubeMap(RGL::Resources& resources, RGL::Environment& env,
	RGL::DeviceContext& device, std::span<const uint8_t* const, 6> faces, uint32_t size,
	const char* name) {
	auto* logger = RLLogger::getInstance();

	// 格式策略同 2D 路径：sRGB 优先 + 不支持采样回落 UNORM（R4）+ DIAG 强制 UNORM 门
	vk::Format format = diagnostics().textureForceUnorm
		? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
	auto formatProps = device.getPhysicalDevice().getFormatProperties(format);
	if ((formatProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
		== vk::FormatFeatureFlags{}) {
		format = vk::Format::eR8G8B8A8Unorm;
		if (logger) logger->warn("cubemap: R8G8B8A8_SRGB 不支持采样，回落 UNORM ({})", name);
	}
	const CubeFormat fmt{format, 4, VK_FILTER_NEAREST,
		format == vk::Format::eR8G8B8A8Srgb ? "sRGB" : "UNORM"};
	return uploadCubeMapCore(resources, env, device, faces, size, fmt, name);
}

/**
 * uploadCubeMap16F 定义（契约见 texture_upload.cppm 接口声明）：RGBA16_SFLOAT cube——
 * float 格式不受 sRGB blit VUID-00239 限制，mip 链 LINEAR 降采样（设备缺 FILTER_LINEAR
 * 能力回落 NEAREST + 警告）；面像素每 texel 4 half（R,G,B,A），top-down，Vulkan 层序 0-5。
 */
[[nodiscard]] std::expected<void, Error> uploadCubeMap16F(RGL::Resources& resources, RGL::Environment& env,
	RGL::DeviceContext& device, std::span<const uint8_t* const, 6> faces, uint32_t size,
	const char* name) {
	auto* logger = RLLogger::getInstance();
	const vk::Format format = vk::Format::eR16G16B16A16Sfloat;
	const auto props = device.getPhysicalDevice().getFormatProperties(format);
	if ((props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
		== vk::FormatFeatureFlags{})
		return std::unexpected(Error{ErrorCode::VulkanError,
			fmt::format("cubemap {}: RGBA16_SFLOAT 不支持采样", name)});
	const bool linearBlit = (props.optimalTilingFeatures
		& vk::FormatFeatureFlagBits::eSampledImageFilterLinear)
		!= vk::FormatFeatureFlags{};
 	if (!linearBlit && logger)
 		logger->warn("cubemap: RGBA16_SFLOAT 无 LINEAR blit 能力（GGX 预滤波不依赖 blit，仅旧回退路径相关）({})", name);
 	// GGX 预滤波开启：mip 链走 heap compute（mipFilter 对 16F 路径失效——不再 blit）。
 	// EnvPrefilter 构造期已校验 RGBA16F STORAGE_IMAGE 能力（缺则启动即抛），故此处恒开安全。
 	const CubeFormat fmt{format, 8, linearBlit ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, "HDR16F", true};
	return uploadCubeMapCore(resources, env, device, faces, size, fmt, name);
}


}  // namespace RGL
