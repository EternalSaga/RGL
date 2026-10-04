module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏）
#include <vk_mem_alloc.h>
module picking:impl;

import picking;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vk_resources;
import render_target_cache;
import render_graph;
import VmaRaii;
import vulkan_utils;

namespace RGL {

PickingService::PickingService(RGL::Resources& vkResources, RGL::RenderTargetCache& vkRts,
	const RGL::CompiledRenderGraph& compiledGraph)
	: resources(vkResources), rts(vkRts), graph(compiledGraph) {
	for (uint32_t s = 0; s < graph.attachments.size(); ++s)
		if (graph.attachments[s].name == kObjectIdAttachmentName)
			objectIdSlot = s;
}

bool PickingService::ensureStaging() {
	if (stagingMapped)
		return true;
	vk::BufferCreateInfo info{};
	info.size = 4;  // 1 texel × r32_uint
	info.usage = vk::BufferUsageFlagBits::eTransferDst;
	VmaAllocationCreateInfo alloc{};
	alloc.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
	alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
		VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
	VmaAllocationInfo allocData{};
	auto result = resources.getAllocator().createBuffer(info, alloc, &allocData);
	if (!result) {
		if (auto* logger = RLLogger::getInstance())
			logger->error("picking: staging alloc failed: {}", result.error().describe());
		return false;
	}
	staging = std::move(*result);
	stagingMapped = allocData.pMappedData;
	return true;
}

void PickingService::record(vk::raii::CommandBuffer& cmd,
	std::vector<vk::ImageLayout>& layouts) {
	if (!hasRequest)
		return;
	hasRequest = false;
	auto* rt = rts.find(std::string{kObjectIdAttachmentName});
	if (!rt || objectIdSlot == kRgNoAttachment) {
		if (auto* logger = RLLogger::getInstance(); logger && !warnedMissing) {
			logger->warn("picking: 请求点选但 object_id RT 不存在（图未装饰 picking pass？）");
			warnedMissing = true;
		}
		return;
	}
	if (!ensureStaging())
		return;
	// 越界 clamp（窗口坐标 vs RT extent；resize 竞争帧取边缘，不判 miss）
	// ⚠ 口径风险（2026-10-03 复核登记，未修）：pendingX/Y 源自 SDL 鼠标事件坐标 = SDL **逻辑点**
	// 空间，这里却被直接 clamp 到 object_id RT 的**像素** extent 采样。本机 KDE Wayland 默认配置
	// pixels==logical（SDL 对分数缩放失明）→ 无漂移；一旦桌面启用分数缩放（客户端拿到缩放后
	// 缓冲，pixels = logical × scale），此处会偏 scale 倍打偏像素 → 须与鼠标坐标的逻辑→像素
	// 变换一起修（改点：request 入口或本处换算，连同相机/输入侧同源坐标一起对账）。
	const uint32_t x = std::min(pendingX, rt->extent.width - 1);
	const uint32_t y = std::min(pendingY, rt->extent.height - 1);
	// 布局：picking pass 末 = CAO → TS（copy 源）；**不恢复**（帧内无后续消费者，
	// 下帧 pass barrier 从追踪表派生 oldLayout 自动衔接，同 frame_capture GB 分支的
	// 追踪语义但免"帧内还要采样"的恢复负担）
	const auto from = layouts[objectIdSlot];
	vk::ImageMemoryBarrier2 toSrc = RGL::createImageBarrier(rt->image.image, from,
		vk::ImageLayout::eTransferSrcOptimal, vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::AccessFlagBits2::eTransferRead);
	vk::DependencyInfo dep{};
	dep.setImageMemoryBarriers(toSrc);
	cmd.pipelineBarrier2(dep);
	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
	region.imageExtent = {1, 1, 1};
	::vkCmdCopyImageToBuffer(*cmd, rt->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		*staging, 1, &region);
	layouts[objectIdSlot] = vk::ImageLayout::eTransferSrcOptimal;
	armed = true;
}

void PickingService::collect(vk::raii::Device& device, vk::Fence inFlightFence) {
	if (!armed)
		return;
	(void)device.waitForFences(inFlightFence, VK_TRUE,
		std::numeric_limits<uint64_t>::max());
	if (stagingMapped) {
		const uint32_t rawId = *static_cast<const uint32_t*>(stagingMapped);
		if (auto* logger = RLLogger::getInstance())
			logger->info("picking: 回读 raw id = {}（0 = 未命中）", rawId);
		// raw = objectSlot+1（picking.frag 写 +1，0 = 未命中 → nullopt）
		cachedResult = rawId > 0 ? std::optional<uint32_t>(rawId - 1) : std::nullopt;
	}
	armed = false;
}

}  // namespace RGL
