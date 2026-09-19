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

/** texel 值比对与日志：白 = 255/255/255/255 已知值（sRGB 不影响全白）；FAIL = 纹理 BO UAF 签名。 */
void logTexelResult(const void* mapped, uint64_t frame) {
	const uint8_t* px = static_cast<const uint8_t*>(mapped);
	auto* logger = RLLogger::getInstance();
	if (px[0] == 255 && px[1] == 255 && px[2] == 255 && px[3] == 255) {
		if (logger)
			logger->info("texel canary (frame {}): 白纹理 texel (0,0) = 255/255/255/255 PASS", frame);
	} else {
		if (logger)
			logger->error("texel canary (frame {}): 白纹理 texel (0,0) = {}/{}/{}/{} —— 纹理 BO 已死"
				"（freed/VA 复用，docs/archive/heap_texture_research.md §12.6/§12.7 的 UAF 签名）",
				frame, px[0], px[1], px[2], px[3]);
	}
}

}  // namespace

// texelCanaryCheck 定义（2026-09-02 编译提速 A 方案）：诊断实现移出接口 TU；
// 本单元是叶子边、编译不上关键路径。

void texelCanaryCheck(RGL::Resources& resources, const RGL::AssetTable& assets,
	RGL::DeviceContext& device, const vk::raii::CommandPool& cmdPool, uint64_t frame) {
	auto* logger = RLLogger::getInstance();
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
	logTexelResult(rbData.pMappedData, frame);
}

}  // namespace RGL
