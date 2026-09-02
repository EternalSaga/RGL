module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include <fmt/format.h>
export module gpu_canary;

import rlstd;
import rllogger;
import vkerror;
import VmaRaii;
import vulkan_utils;
import vk_device;
import vk_resources;

namespace RGL {

// GPU 侧回读金丝雀：把"时好时坏"的 GPU 侧 UAF（ASan 结构性失明，heap_texture_research.md
// §12.7）变成"帧 N 必炸"。texelCanaryCheck = 白纹理已知值回读（重构批次 4 首落；
// 2026-09-02 拆自 texture_pool——诊断逻辑与上传逻辑解耦，ecs 帧循环不再依赖整个
// 纹理上传栈）。
//
// texel 回读金丝雀：第 N 帧重读白纹理 texel (0,0) 比对已知值 255——纹理 BO 死了
// （freed/VA 复用）回读立即偏/fault。白纹理已知值 = 免启动基线回读；白纹理与图片
// 纹理同层同寿命，它死 = 全死。
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

}  // namespace RGL
