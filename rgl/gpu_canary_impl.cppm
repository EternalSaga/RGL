module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <fmt/format.h>
module gpu_canary:impl;

import gpu_canary;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import VmaRaii;
import vulkan_utils;
import vk_device;
import vk_resources;
import asset_table;  // AssetTable（F 拆出）

namespace RGL {

namespace {

/** 已知值纹理回读台账：纹理 id → 名字 + 上传期写入的已知 texel（比对 (0,0) 单点）。 */
struct KnownTexel {
	uint32_t id;
	std::string_view name;
	std::array<uint8_t, 4> expect;
};

// 白 = 全 255（sRGB/UNORM 双路同值）；中性法线 = (128,128,255,255) UNORM（decodeNormalDx
// 解码不动点）。两条都登记的理由（2026-09-30 黑面 bug 实证）：白纹理活 ≠ 全纹理活——
// 无贴图画全走的中性法线槽被覆写成 (64,64,122)-ish 时，白 canary 全绿而画面受光面纯黑。
inline constexpr KnownTexel kKnownTexels[] = {
	{0, "白纹理", {255, 255, 255, 255}},
	{1, "中性法线", {128, 128, 255, 255}},
};

/** texel 值比对与日志（回读缓冲按 kKnownTexels 序每 4B 一节）；FAIL 只打日志不 throw。 */
void logTexelResults(const void* mapped, uint64_t frame, size_t count) {
	const uint8_t* px = static_cast<const uint8_t*>(mapped);
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	for (size_t i = 0; i < count; ++i) {
		const auto& known = kKnownTexels[i];
		const uint8_t* t = px + i * 4;
		if (std::equal(known.expect.begin(), known.expect.end(), t)) {
			logger->info("texel canary (frame {}): {} texel (0,0) = {}/{}/{}/{} PASS", frame,
				known.name, t[0], t[1], t[2], t[3]);
		} else {
			logger->error("texel canary (frame {}): {} texel (0,0) = {}/{}/{}/{} ≠ 已知值 {}/{}/{}/{}"
				" —— 纹理被覆写或 BO 已死（freed/VA 复用，docs/archive/heap_texture_research.md"
				" §12.6/§12.7 的 UAF 签名）",
				frame, known.name, t[0], t[1], t[2], t[3],
				known.expect[0], known.expect[1], known.expect[2], known.expect[3]);
		}
	}
}

}  // namespace

// texelCanaryCheck 定义（2026-09-02 编译提速 A 方案）：诊断实现移出接口 TU；
// 本单元是叶子边、编译不上关键路径。

void texelCanaryCheck(RGL::Resources& resources, const RGL::AssetTable& assets,
	RGL::DeviceContext& device, const vk::raii::CommandPool& cmdPool, uint64_t frame) {
	auto* logger = RLLogger::getInstance();
	const size_t count = std::min<size_t>(assets.textureSize(), std::size(kKnownTexels));
	if (count == 0) {
		if (logger)
			logger->warn("texel canary (frame {}): 无纹理资产（未加载模型），跳过", frame);
		return;
	}
	// ① waitForIdle：排空 in-flight 采样，消回读/采样竞态（诊断路径，帧 N 单次 GPU 停顿
	// 可接受；env 门控默认关，正常路径零影响）
	device.getDevice().waitIdle();
	// ② 16B 回读缓冲（每纹理 texel 4B），常驻映射（同 upload 期 DIAG 回读模式）
	vk::BufferCreateInfo rbInfo{};
	rbInfo.size = 16;
	rbInfo.usage = vk::BufferUsageFlagBits::eTransferDst;
	VmaAllocationCreateInfo rbAlloc{};
	rbAlloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	rbAlloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo rbData{};
	auto rbResult = resources.getAllocator().createBuffer(rbInfo, rbAlloc, &rbData, "gpu_canary/readback");
	if (!rbResult) {
		if (logger)
			logger->error("texel canary (frame {}): 回读缓冲创建失败 ({})", frame,
				std::move(rbResult).error().describe());
		return;
	}
	auto& rb = *rbResult;  // VmaBuffer 随函数作用域存活（RAII）
	// ③ SHADER_READ_ONLY → TRANSFER_SRC → 拷贝 → 迁回（描述符声明布局，采样前必须）
	// ——barrier 链同 upload 期 DIAG 回读（texture_pool.cppm 内）；逐已知纹理各一条拷进
	// 回读缓冲自己的 4B 节（bufferOffset = i*4，比对按同一布局）。
	auto cmdAlloc = device.getDevice().allocateCommandBuffers(vk::CommandBufferAllocateInfo{
		cmdPool, vk::CommandBufferLevel::ePrimary, 1});
	auto cmd = std::move(cmdAlloc[0]);
	cmd.begin(vk::CommandBufferBeginInfo{});
	for (size_t i = 0; i < count; ++i) {
		const auto& tex = assets.getTexture(kKnownTexels[i].id);
		const auto toSrc = RGL::createImageBarrier(
			*tex.image, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eFragmentShader, vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eShaderSampledRead, vk::AccessFlagBits2::eTransferRead);
		VkDependencyInfo toSrcDep{};
		toSrcDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		toSrcDep.imageMemoryBarrierCount = 1;
		toSrcDep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&toSrc);
		::vkCmdPipelineBarrier2(*cmd, &toSrcDep);
		VkBufferImageCopy copy{};
		copy.bufferOffset = static_cast<VkDeviceSize>(i * 4);
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {1, 1, 1};
		::vkCmdCopyImageToBuffer(*cmd, *tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			*rb, 1, &copy);
		const auto toRead = RGL::createImageBarrier(
			*tex.image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
			vk::AccessFlagBits2::eTransferRead, vk::AccessFlagBits2::eShaderSampledRead);
		VkDependencyInfo toReadDep{};
		toReadDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		toReadDep.imageMemoryBarrierCount = 1;
		toReadDep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&toRead);
		::vkCmdPipelineBarrier2(*cmd, &toReadDep);
	}
	cmd.end();
	// ④ 提交 + 等待 + 比对（已知值不符 = 覆写/UAF 签名，见 kKnownTexels 注释）
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
	logTexelResults(rbData.pMappedData, frame, count);
}

}  // namespace RGL
