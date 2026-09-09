module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <fmt/format.h>
module frame_capture:impl;

import frame_capture;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vk_resources;
import render_graph;
import VmaRaii;
import vulkan_utils;

namespace RGL {

// FrameCapturer 函数体（2026-09-10 Phase 10 批次 0 手术 2：随类自 vkrenderer 摘出，
// 逐步骤注记迁自 vkrenderer_impl ⑪.5 / 收口块）。

namespace {

/**
 * 捕获回读每像素字节数（仅可能出现的格式：swapchain 候选 4B/像素系 + A2B10G10R10=4B +
 * HDR FP16=8B + G-buffer 词表 rgba32_sfloat=16B，其余 8bit×4/rg16×2 = 4B）。
 * @param f 捕获格式（swapchain format 或 G-buffer RT format）
 * @return 每像素字节数（4 / 8 / 16）
 */
uint32_t captureBpp(vk::Format f) {
	switch (f) {
		case vk::Format::eR32G32B32A32Sfloat: return 16;
		case vk::Format::eR16G16B16A16Sfloat: return 8;
		default: return 4;
	}
}

}  // namespace

FrameCapturer::FrameCapturer(RGL::Resources& vkResources,
		const RGL::CompiledRenderGraph& renderGraph)
	: resources(vkResources), graph(renderGraph) {
	// 帧内捕获：定位 swapchain attachment slot（捕获时读写其布局追踪值）
	for (uint32_t s = 0; s < graph.attachments.size(); ++s)
		if (graph.attachments[s].kind == RgAttachmentKind::swapchain)
			swapchainSlot = s;
}

bool FrameCapturer::ensureCaptureStaging(VkDeviceSize bytes) {
	if (captureStagingSize >= bytes)
		return true;
	vk::BufferCreateInfo info{};
	info.size = bytes;
	info.usage = vk::BufferUsageFlagBits::eTransferDst;
	VmaAllocationCreateInfo alloc{};
	alloc.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
	alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
		VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
	VmaAllocationInfo allocData{};
	auto result = resources.getAllocator().createBuffer(info, alloc, &allocData);
	if (!result) {
		auto logger = RLLogger::getInstance();
		if (logger)
			logger->error("Renderer: capture staging alloc failed: {}", result.error().describe());
		return false;
	}
	captureStaging = std::move(*result);
	captureMapped = allocData.pMappedData;
	captureStagingSize = bytes;
	return true;
}

void FrameCapturer::record(vk::raii::CommandBuffer& cmd, VkImage swapchainImage,
		vk::Extent2D swapchainExtent, vk::Format swapchainFormat,
		std::vector<vk::ImageLayout>& layouts) {
	// ⑪.5 帧内捕获（llm_debug 回读，frame_capture 契约）：请求帧在 presentTransition
	// 前插回读拷贝（布局推进语义见接口 record 注释）。
	// acquire 超时跳帧的 return 在本段之前 = 请求保持 pending 至真正录制的帧。
	if (captureRequest == -2)
		return;
	auto slotOfName = [&](const std::string& n) -> RgAttachmentSlot {
		for (uint32_t s = 0; s < graph.attachments.size(); ++s)
			if (graph.attachments[s].name == n)
				return s;
		return kRgNoAttachment;
	};
	auto barrier = [&](VkImage image, vk::ImageLayout from, vk::ImageLayout to,
		vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
		vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
		vk::ImageMemoryBarrier2 b = RGL::createImageBarrier(image, from, to,
			vk::ImageAspectFlagBits::eColor, srcStage, dstStage, srcAccess, dstAccess);
		vk::DependencyInfo dep{};
		dep.setImageMemoryBarriers(b);
		cmd.pipelineBarrier2(dep);
	};
	auto copyOut = [&](VkImage image, uint32_t w, uint32_t h, vk::Format format, int channel) {
		const uint32_t bpp = captureBpp(format);
		const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * bpp;
		if (!ensureCaptureStaging(bytes))
			return false;
		VkBufferImageCopy region{};
		region.bufferRowLength = w;
		region.bufferImageHeight = h;
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {w, h, 1};
		::vkCmdCopyImageToBuffer(*cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			*captureStaging, 1, &region);
		captureInFlight = FrameCapture{w, h, format, channel, bytes, {}};
		captureArmed = true;
		return true;
	};
	if (captureRequest >= 0) {
		// GB 直读：deferred 图专属（前向图无 gbuffer_* 附件 / RT 未建 = 跳过并留观测）
		if (captureRequest < static_cast<int>(kGBufferAttachments.size())) {
			const std::string name{kGBufferAttachments[captureRequest].name};
			const auto slot = slotOfName(name);
			if (auto* rt = slot != kRgNoAttachment ? resources.findRtAttachment(name) : nullptr) {
				const auto from = layouts[slot];  // deferred 帧末 = SRO（light 采样后）
				barrier(rt->image.image, from, vk::ImageLayout::eTransferSrcOptimal,
					vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead,
					vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
				if (copyOut(rt->image.image, rt->extent.width, rt->extent.height,
				        rt->format, captureRequest))
					barrier(rt->image.image, vk::ImageLayout::eTransferSrcOptimal, from,
						vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
						vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
			} else if (auto logger = RLLogger::getInstance())
				logger->warn("frame capture: dump_gbuf={} 但 RT '{}' 不存在（非 deferred 图？）",
					captureRequest, name);
		}
	} else if (swapchainSlot != kRgNoAttachment) {
		const auto from = layouts[swapchainSlot];  // 帧末：直渲 = CAO、blit 变体 = TransferDst
		const bool blitWritten = from == vk::ImageLayout::eTransferDstOptimal;
		barrier(swapchainImage, from, vk::ImageLayout::eTransferSrcOptimal,
			blitWritten ? vk::PipelineStageFlagBits2::eTransfer
			            : vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			blitWritten ? vk::AccessFlagBits2::eTransferWrite
			            : vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
		if (copyOut(swapchainImage, swapchainExtent.width, swapchainExtent.height,
		        swapchainFormat, -1))
			layouts[swapchainSlot] = vk::ImageLayout::eTransferSrcOptimal;
	}
	captureRequest = -2;
}

void FrameCapturer::collect(vk::raii::Device& device, vk::Fence inFlightFence) {
	// 帧内捕获收口：同步等本帧命令（含回读拷贝）完成，把 mapped 字节搬进结果。
	// fence 不在此 reset（下帧 ① waitAll 对已信号 fence 立即通过 + 照常 reset，语义不变）；
	// 停顿只发生在捕获请求帧（llm 一次性截图），正常帧零影响。
	if (!captureArmed)
		return;
	(void)device.waitForFences(inFlightFence, VK_TRUE,
		std::numeric_limits<uint64_t>::max());
	if (captureMapped) {
		auto* src = static_cast<const uint8_t*>(captureMapped);
		captureInFlight.pixels.assign(src, src + captureInFlight.bytes);
		captureResult = std::move(captureInFlight);
	}
	captureArmed = false;
}

}  // namespace RGL
