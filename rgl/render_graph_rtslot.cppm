module;
#include <fmt/format.h>
module render_graph:rtslot;

import render_graph;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_swapchain;
import render_target_cache;

namespace RGL {

// RT 装配分区（2026-09-19 架构债务续账步骤 2 自 vkresources_impl 搬入）：
// deferred G-buffer / scene_hdr 是 **deferred 图的装配函数**——kGBufferAttachments
// 契约表本就住本模块，搬入后"G-buffer 名字/格式"知识不出 render_graph；资源层
//（RenderTargetCache）只留通用"名字 → RT"缓存。

/**
 * 调用序契约（装配后期、Renderer 前）与槽语义见接口注释；RT 采样槽走运行时段。实现：名字/格式取自
 * kGBufferAttachments 契约表（单一真相源，与 deferredRenderGraphDesc 共享，无漂移
 * 可能）；extent = 当前 swapchain；逐附件 ensure + heapSlot（任一失败立即返回）；
 * 成功打日志 5 槽号（albedo/normal/position/metalrough/emissive）。
 */
std::expected<std::array<uint32_t, 5>, Error> ensureDeferredGBufferSlots(
		RenderTargetCache& rts, Swapchain& swapchain) {
	const vk::Extent2D extent = swapchain.getExtent();
	std::array<uint32_t, 5> slots{};
	for (uint32_t i = 0; i < kGBufferAttachments.size(); ++i) {
		const std::string name{kGBufferAttachments[i].name};
		const vk::Format format = resolveRgFormat(
			std::string{kGBufferAttachments[i].format}, vk::Format::eUndefined);
		auto rt = rts.ensure(name, extent, format);
		if (!rt)
			return std::unexpected(std::move(rt).error());
		auto slot = rts.heapSlot(name);
		if (!slot)
			return std::unexpected(Error{ErrorCode::Unknown,
				fmt::format("gbuffer RT '{}' has no sample descriptor slot", name)});
		slots[i] = *slot;
	}
	if (auto* logger = RLLogger::getInstance())
		logger->info("deferred G-buffer: {}x{} slots {}/{}/{}/{}/{} (albedo/normal/position/metalrough/emissive)",
			extent.width, extent.height, slots[0], slots[1], slots[2], slots[3], slots[4]);
	return slots;
}

/**
 * Phase 8 批次 ①（接口注释见 render_graph.cppm）：名字/格式 = kSceneHdrName/
 * kSceneHdrFormat 契约常量（与 default/deferred 图描述共享，无漂移）；extent = 当前
 * swapchain；ensure + heapSlot（rgba16_sfloat 自带 COLOR_ATTACHMENT|SAMPLED usage）。
 * 启动期一次（main 在 deferred 装配后、addTonemapEntry 前）；drawFrame 图循环每帧对
 * 同名命中复用。
 */
std::expected<uint32_t, Error> ensureSceneHdrSlot(
		RenderTargetCache& rts, Swapchain& swapchain) {
	const std::string name{std::string{kSceneHdrName}.c_str()};
	const vk::Extent2D extent = swapchain.getExtent();
	const vk::Format format = vk::Format::eR16G16B16A16Sfloat;  // = kSceneHdrFormat 物化
	auto rt = rts.ensure(name, extent, format);
	if (!rt)
		return std::unexpected(std::move(rt).error());
	auto slot = rts.heapSlot(name);
	if (!slot)
		return std::unexpected(Error{ErrorCode::Unknown,
			fmt::format("scene_hdr RT has no sample descriptor slot", name)});
	if (auto* logger = RLLogger::getInstance())
		logger->info("scene_hdr (RGBA16F): {}x{} slot {}", extent.width, extent.height, *slot);
	return *slot;
}

}  // namespace RGL
