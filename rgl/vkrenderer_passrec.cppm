module;
#include <vulkan/vulkan.h>  // VK_* 句柄类型；rlvk 不含
#include <vk_mem_alloc.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <ranges>
#include <fmt/format.h>
module vk_renderer:passrec;

import vk_renderer;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_swapchain;
import vk_resources;
import scene_data;  // kSceneData* 布局常量 + SceneDataService
import render_target_cache;  // RenderTargetCache/RtAttachment
import scene_forward_types_generated;  // 前向头 SceneLight
import shader_registry;
import shader;
import state_kit;
import gpu_cull;
import frame_capture;
import cull_driver;
import frame_session;
import gpu_profiler;
import profiler;  // 批 9.5：drawFrame 内 CPU 子 zone + 帧命令计数（phase9_plan 点名头寸）
import diagnostics;
import vulkan_utils;  // createImageBarrier（物化用）
import VmaRaii;
import frame_draw_list;  // segmentSpan（段名 → draw 序列）
import scene_context;  // shadowDrawSource（IZB mask 重放段名单一来源，批 11.3-1d）
import material_types_generated;  // DrawArgs
import lights_types_generated;
import features;
import render_graph;
import evssm_shadow;  // 批 11.6d-2：EvssmShadow（门控构造，make_unique 需完整类型）

namespace RGL {

namespace {

/** uint32 → int32 显式转（vk::Rect2D::offset 是 int32，声明里的 viewport 是 uint32；
 *  聚合初始化列表里不能隐式缩宽，且 rlvk 的 Offset2D 类型在 GMF 头里不可见）。 */
int32_t asI32(uint32_t v) { return static_cast<int32_t>(v); }

/**
 * attachment slot → VkImage/View 解析 + barrier 物化发射（手术 6：drawFrame 内 imageForSlot/viewForSlot/emitBarriers
 * 三个 lambda 改造为文件内 struct——搬移非重写、逐条命令等价；持 swapchain/depth 句柄 + offscreenRt
 * + 追踪布局表引用 + 本帧 cmd，生命周期 = drawFrame 一次调用）。
 */
struct SlotResolver {
	const std::vector<RgCompiledAttachment>& attachments;
	VkImage swapchainImage; VkImageView swapchainView;
	VkImage depthImage; VkImageView depthView;
	/** 后缓冲尺寸：写 swapchain 附件的 pass（tonemap / composite）按它铺满渲染区。 */
	vk::Extent2D swapchainExtent;
	/** 渲染尺寸：其余全部 pass 按它铺满。**也是深度图的尺寸**——深度图在
	 * `Resources` 构造期就按 `getRenderExtent()` 分配（批 14.3），故 extentForSlot
	 * 的 depth 分支直接返回本值，不另设成员（多一个字段 = 多一种失步可能）。 */
	vk::Extent2D renderExtent;
	const std::vector<RtAttachment*>& offscreenRt;
	std::vector<vk::ImageLayout>& layouts;
	vk::raii::CommandBuffer& cmd;
	/** slot → VkImage（四 kind 穷尽）。 */
	VkImage imageForSlot(RgAttachmentSlot slot) const {
		switch (attachments[slot].kind) {
			case RgAttachmentKind::swapchain: return swapchainImage;
			case RgAttachmentKind::depth: return depthImage;
			case RgAttachmentKind::depthOffscreen:  // 阴影图类：同走 RT 缓存
			case RgAttachmentKind::offscreen: return offscreenRt[slot]->image.image;
		}
		return VK_NULL_HANDLE;  // 不可达（穷尽），消 warning
	}
	/** slot → VkImageView（offscreen 走 optional + raii 双重解引用）。 */
	VkImageView viewForSlot(RgAttachmentSlot slot) const {
		switch (attachments[slot].kind) {
			case RgAttachmentKind::swapchain: return swapchainView;
			case RgAttachmentKind::depth: return depthView;
			case RgAttachmentKind::depthOffscreen:  // 阴影图类：同走 RT 缓存
			case RgAttachmentKind::offscreen: return **offscreenRt[slot]->view;  /* optional 解引用 + raii handle */
		}
		return VK_NULL_HANDLE;
	}
	/** slot → 该附件自己的 extent（批 14.3：blit 两端尺寸可不同，不能再假设 1:1 同尺寸。
	 * swapchain → 后缓冲尺寸；depth → 深度图（= 渲染 extent）；offscreen/depthOffscreen →
	 * RT 缓存里那份的实际尺寸）。 */
	vk::Extent2D extentForSlot(RgAttachmentSlot slot) const {
		switch (attachments[slot].kind) {
			case RgAttachmentKind::swapchain: return swapchainExtent;
			case RgAttachmentKind::depth: return renderExtent;  // 深度图按渲染 extent 分配（见成员注释）
			case RgAttachmentKind::depthOffscreen:  // 阴影图类：同走 RT 缓存
			case RgAttachmentKind::offscreen: return offscreenRt[slot]->extent;
		}
		return {};  // 不可达（穷尽）
	}
	/**
	 * 本 pass 的渲染区域矩形（viewport/scissor + beginRendering.renderArea 共用）。
	 * 解析顺序：① pass 声明的子矩形（图集分块；同时限定 clear 与渲染区）
	 *   ② 否则 depthOffscreen（阴影图）→ 用该 RT 自己的 extent（分辨率与窗口解耦；若用窗口尺寸
	 *     会出现 viewport 与 renderArea 不一致 = 贴图比例错 + 边缘漏画）
	 *   ③ 否则**写 swapchain 附件的 pass** → 后缓冲尺寸（批 14.3：tonemap 是全屏大三角，
	 *     它必须铺满后缓冲；scene_hdr 若是渲染分辨率，归一化 uv 采样天然完成放大）
	 *   ④ 否则 → 渲染 extent（几何 / G-buffer / ao / taa 全在这一档）。
	 * renderScale = 1.0 时 ③④ 同值 ⟹ 既有全部 pass 逐位不变。
	 */
	vk::Rect2D renderRectFor(const RgPassPlan& pass) const {
		// Rect2D 的 offset 是 int32，而声明里是 uint32；显式转后归一构造
		// （rlvk 的 Offset2D 类型在 GMF 头里不可见，不能单独引用）。
		if (pass.viewport[2] != 0u)
			return vk::Rect2D{{asI32(pass.viewport[0]), asI32(pass.viewport[1])},
				{vk::Extent2D{pass.viewport[2], pass.viewport[3]}}};
		if (pass.depth && attachments[pass.depth->slot].kind == RgAttachmentKind::depthOffscreen)
			return {{0, 0}, offscreenRt[pass.depth->slot]->extent};
		const bool writesSwapchain = std::ranges::any_of(pass.colors,
			[this](const auto& c) { return attachments[c.slot].kind == RgAttachmentKind::swapchain; });
		return {{0, 0}, writesSwapchain ? swapchainExtent : renderExtent};
	}
	/** 物化 + 发射一组 attachment barrier（单 DependencyInfo 批量）；oldLayout = 帧首丢弃（eUndefined）或跨帧/帧内追踪值，发射后推进 layouts 追踪。 */
	void emitBarriers(std::span<const RgBarrierPlan> barriers) {
		if (barriers.empty()) return;
		std::vector<vk::ImageMemoryBarrier2> materialized;
		materialized.reserve(barriers.size());
		for (const auto& plan : barriers) {
			const auto oldLayout = plan.oldLayoutDiscard ? vk::ImageLayout::eUndefined
			                                              : layouts[plan.attachment];
			materialized.push_back(RGL::materializeBarrier(plan, imageForSlot(plan.attachment),
				oldLayout));
		}
		vk::DependencyInfo depInfo;
		depInfo.setImageMemoryBarriers(materialized);
		cmd.pipelineBarrier2(depInfo);
		profilerAddCommands(1);  // 批 9.5：本函数每次调用恰发 1 条 vkCmdPipelineBarrier2
		for (const auto& plan : barriers)
			layouts[plan.attachment] = plan.newLayout;
	}
	/**
	 * ⑫ 帧末 present 转换（图派生 plan + **按追踪布局补上真实末次访问**）。
	 *
	 * 🔴 为什么要补：帧内捕获（⑪.5）会把后缓冲推进到 TransferSrcOptimal 并留下一次
	 * **Transfer 读**（回读拷贝），而图派生的 src 只覆盖 pass 里的最后一次写（CAO 写）
	 * ⟹ present 的布局转换与那次回读读之间没有依赖。syncval 实测（2026-10-09）：
	 * `WRITE_AFTER_READ hazard ... layout transition on the VkImage <后缓冲>, which was
	 * previously read by vkCmdCopyImageToBuffer`，Hint 直接点名 srcStageMask 须含 COPY。
	 * 处理方式与 ⑪.6 帧末叠加层同一条惯例（emitFrameEndOverlay 的"src 侧按当前布局精确
	 * 给"）：**并进** src 而不是替换——CAO 写仍是真值，丢了它就换成另一个 hazard。
	 * @param plan 图编译产物 presentTransition（原样不改，本地副本上补）
	 */
	void emitPresentBarrier(const RgBarrierPlan& plan) {
		auto widened = plan;
		if (layouts[widened.attachment] == vk::ImageLayout::eTransferSrcOptimal) {
			widened.srcStage |= vk::PipelineStageFlagBits2::eTransfer;
			widened.srcAccess |= vk::AccessFlagBits2::eTransferRead;
		}
		emitBarriers(std::span<const RgBarrierPlan>{&widened, 1});
	}
};

/** 构造并发起一个 pass 的 dynamic rendering（手术 6：自 drawFrame 摘出——color/depth RenderingAttachmentInfo 逐字段构造 + beginRendering，命令参数逐字不变；动态状态/draw 分发留 drawFrame 因需 stateKit/registry 成员）。 */
void beginPassRendering(vk::raii::CommandBuffer& cmd, const SlotResolver& slots,
	const RgPassPlan& pass, vk::Rect2D renderRect) {
	std::vector<vk::RenderingAttachmentInfo> colorInfos;
	colorInfos.reserve(pass.colors.size());
	for (const auto& cp : pass.colors) {
		vk::RenderingAttachmentInfo info{};
		info.imageView = slots.viewForSlot(cp.slot);
		info.imageLayout = cp.layout;
		info.loadOp = cp.loadOp;
		info.storeOp = cp.storeOp;
		info.clearValue = cp.clearValue;
		colorInfos.push_back(info);
	}
	// 背景清深灰色 (0.1)/深度清 1.0 等清除值来自图计划（默认图 = 原 ⑤ 逐位一致，末段 DontCare 省带宽）
	std::optional<vk::RenderingAttachmentInfo> depthInfo;
	if (pass.depth) {
		vk::RenderingAttachmentInfo info{};
		info.imageView = slots.viewForSlot(pass.depth->slot);
		info.imageLayout = pass.depth->layout;
		info.loadOp = pass.depth->loadOp;
		info.storeOp = pass.depth->storeOp;
		info.clearValue = pass.depth->clearValue;
		depthInfo = info;
	}
	vk::RenderingInfo renderingInfo{};
	renderingInfo.renderArea = renderRect;
	renderingInfo.layerCount = 1;
	renderingInfo.setColorAttachments(colorInfos);
	if (depthInfo)
		renderingInfo.setPDepthAttachment(&*depthInfo);
	cmd.beginRendering(renderingInfo);
	profilerAddCommands(1);  // 批 9.5：每 pass 恰 1 条 vkCmdBeginRendering
}

}  // namespace

// ===== Phase 11 批 11.2（2026-09-29）：将 drawFrame 录制主流程与其原语从 vkrenderer_impl.cppm 整块搬出到本
// partition（主因：那个文件碰到 500 代码行红线，而本块内容内联、与其余部分无关）。
// 命令录制顺序、每条 vk 调用参数、错误路径逐字不变（逐块搬移，非重写）。

// ④⑥⑥.5⑦ 数据通路固定段（heap barrier → scene staging 拷贝/屏障 → heap/sampler heap 绑定，Phase 6 起不入图——非 attachment 状态同步；旧 color/depth attachment barrier 已由图 passes[0].barriers 推导发射、与此处 heap barrier 相互独立、命令流先后对换无语义影响）
void Renderer::recordFixedDataPath(vk::raii::CommandBuffer& cmd, uint32_t frameSlot) {
	auto z = zone("drawFrame.dataPath");  // 批 9.5 观测：heap barrier + staging 拷贝 + heap 绑定
	// ⑥ Descriptor heap 同步：描述符此前由 vkWriteResourceDescriptorsEXT 写进
	// CPU mapped 区域（vkresources.cppm），GPU 读取前需要 host write →
	// resource heap read 的内存 barrier。eHost stage 不能在 render pass 内部使用，
	// 故该 barrier 必须放在 beginRendering 之前（见 docs/DESCRIPTOR_HEAP_SPEC.md 坑 8）。
	vk::MemoryBarrier2 heapBarrier{};
	heapBarrier.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
	// 两个 heap 的读访问 flag 相互独立（spec：resource heap 与 sampler heap 各有专属
	// access bit）——只设 eResourceHeapReadEXT 时 sampler 描述符的 host write 未同步到
	// GPU，sampler 描述符读到未初始化内存 → 采样恒 garbage（7e1abce 内修复，当时注释
	// "Phase 3 黑屏根因"；2026-08-28 G 实验：当前启动期 fence/waitIdle 已掩盖该缺位
	// 的影响，spec 层面仍必须保留，见 docs/archive/heap_texture_research.md §12）
	// eTransferRead：⑥.5 的 vkCmdCopyBuffer 在 eTransfer 阶段读 host-mapped staging
	// buffer（scene data 源），本 eHost→eAllGraphics barrier 是该读之前唯一的
	// host write → device read 排序点，dstAccess 须覆盖具体读（VVL sync validation）
	heapBarrier.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT |
		vk::AccessFlagBits2::eSamplerHeapReadEXT | vk::AccessFlagBits2::eTransferRead;
	heapBarrier.srcStageMask = vk::PipelineStageFlagBits2::eHost;
	heapBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics;
	vk::DependencyInfo heapDepInfo{};
	heapDepInfo.setMemoryBarriers(heapBarrier);
	cmd.pipelineBarrier2(heapDepInfo);
	// 批 9.5 vkcmd.count 口径：本函数固定段 = heap barrier 1 + [scene 写前依赖 barrier +
	// staging 拷贝 + barrier] 3 + heap/sampler heap 绑定 2（改本函数命令流须同步改这里的计数）。
	uint32_t fixedCmds = 1;

	// ⑥.5 scene data staging → device buffer 拷贝（Phase 5 批次 2）+ Transfer 写 →
	// 图形阶段 shader 读 barrier（storage buffer 的写/读即使同 cmd 也须显式依赖，
	// VVL BUFFER_SYNC 检查项）。目标未创建（无模型场景 = SceneDataService::ensure
	// 未被调用）则跳过——cube 管线不绑 scene data。
	//
	// 🔴 拷贝**前**先发一条"写前依赖"barrier（syncval 实测 2026-10-09：缺它每帧 2 条
	// WRITE_AFTER_WRITE hazard）。理由：scene data 是**单份跨帧共享**的 device buffer，
	// 而每帧有两个写者（本函数 = 主 CB、GpuCull::recordCullPass = cull CB，同队列），
	// kFramesInFlight=2 下"上一帧的 shader/compute 读"与"上一帧或另一 CB 的 copy 写"
	// 同本帧的 copy 写之间**没有任何依赖**——post-copy 那条 barrier 的 dst 只覆盖 shader
	// 读，覆盖不到下一次 copy 的 TransferWrite（VVL 原话：to prevent this hazard, it must
	// allow TRANSFER_WRITE_BIT accesses at COPY_BIT）。pipeline barrier 的 first scope 含
	// "同队列更早提交"的命令（跨 submission，见 planBarrier 里同一条 spec 论证），故本条
	// 一次补齐 RAW（上一帧读→本帧写）与 WAW（上一份写→本帧写）。
	// src 侧必须含 **Compute**：cull compute 读对象矩阵表 @128（只给 AllGraphics 漏掉它）。
	{
		const auto& sceneBuffer = sceneData->getBuffer();
		if (sceneBuffer.buffer != VK_NULL_HANDLE) {
			vk::BufferMemoryBarrier2 preBarrier{};
			preBarrier.srcStageMask = vk::PipelineStageFlagBits2::eAllGraphics
				| vk::PipelineStageFlagBits2::eComputeShader
				| vk::PipelineStageFlagBits2::eTransfer;
			preBarrier.srcAccessMask = vk::AccessFlagBits2::eShaderRead
				| vk::AccessFlagBits2::eTransferWrite;
			preBarrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
			preBarrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
			preBarrier.buffer = sceneBuffer.buffer;
			preBarrier.size = kSceneDataBytes;
			vk::DependencyInfo preDep{};
			preDep.setBufferMemoryBarriers(preBarrier);
			cmd.pipelineBarrier2(preDep);
			++fixedCmds;  // 写前依赖 barrier（与下面的 copy + post barrier 各计 1）
			VkBufferCopy region{0, 0, static_cast<VkDeviceSize>(kSceneDataBytes)};
			::vkCmdCopyBuffer(*cmd, *sceneStaging[frameSlot],
				sceneBuffer.buffer, 1, &region);
			vk::BufferMemoryBarrier2 sceneBarrier{};
			sceneBarrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
			sceneBarrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
			sceneBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics;
			sceneBarrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
			sceneBarrier.buffer = sceneBuffer.buffer;
			sceneBarrier.size = kSceneDataBytes;
			vk::DependencyInfo sceneDepInfo;
			sceneDepInfo.setBufferMemoryBarriers(sceneBarrier);
			cmd.pipelineBarrier2(sceneDepInfo);
			fixedCmds += 2;  // 批 9.5：vkCmdCopyBuffer + vkCmdPipelineBarrier2
		}
	}

	// ⑦ 把两个 descriptor heap 绑定到命令缓冲（必须在 beginRendering 之前）
	bindDescriptorHeaps(cmd);
	profilerAddCommands(fixedCmds + 2);  // 批 9.5：+2 = bindResourceHeapEXT + bindSamplerHeapEXT
}

// 坑 9（docs/DESCRIPTOR_HEAP_SPEC.md §坑 9）的正主。抽成函数是因为它现在有**第二个**
// 调用者：FSR 的 VK 后端走 `vkCmdBindDescriptorSets`，会把 CB 切进 classic 模式并**作废**
// 这里的绑定（VVL 原话："The command buffer is currently in 'Classic Descriptor' mode
// from previous call to vkCmdBindDescriptorSets which invalidated the previous
// 'Descriptor Heap' mode"）⟹ 每帧 FSR dispatch 之后必须重绑，否则同帧其后的
// vkCmdDrawMeshTasksEXT 全部报「heap was not bound」（实测 10 条）。
void Renderer::bindDescriptorHeaps(vk::raii::CommandBuffer& cmd) const {
	// heapRange.size 用 getHeapSize()（已扣除内部对齐偏移），而非
	// descriptorSize * capacity——原因见 VmaBuffer.cppm。
	// reservedRange = [0, reservedSlots * descriptorSize)：实现保留区域
	//（如 NVIDIA 保留 6048 槽），在此声明为不可用。
	vk::DeviceAddressRangeEXT heapRange{};
	heapRange.address = heapAddress;
	heapRange.size = resources->getDescriptorHeap().getHeapSize();
	cmd.bindResourceHeapEXT(vk::BindHeapInfoEXT(
		heapRange,
		resources->getReservedRangeOffset(),
		resources->getReservedRangeSize()));
	// sampler heap 同法绑定（Phase 3：OpTypeSampler 的 sampler 描述符；每帧无条件，
	// 与 resource heap 一致——未引用时绑定无副作用）
	vk::DeviceAddressRangeEXT samplerHeapRange{};
	samplerHeapRange.address = resources->getSamplerHeap().getGpuAddress();
	samplerHeapRange.size = resources->getSamplerHeap().getHeapSize();
	cmd.bindSamplerHeapEXT(vk::BindHeapInfoEXT(
		samplerHeapRange,
		0,
		resources->getSamplerReservedRangeSize()));
}

// 图外 compute 编舞（IZB / EVSSM 帧首与光照 pass 前段）已拆至 vkrenderer_offgraph.cppm（批 14.3 文件长度红线；职责边界 = 图内 pass 录制 vs 图外编舞）

// 单个 mesh draw：push DrawArgs（pushDataEXT）+ bindShaders + drawMeshTasksEXT。
void Renderer::drawOne(vk::raii::CommandBuffer& cmd, const DrawCall& call) {
	const auto& entry = registry.entry(call.pipeline);
	cmd.pushDataEXT(vk::PushDataInfoEXT(
		entry.pushDataSet, vk::HostAddressRangeConstEXT(call.pushData.data(), call.pushData.size())));
	entry.pipeline.bindShaders(cmd);
	cmd.drawMeshTasksEXT(call.groupCount, 1, 1);
}

/**
 * 克隆管线重放（Phase 10 批 10.5 picking / 10.7 highlight 共用）：逐 opaque 段 draw，
 * 把原 draw 的管线换成指定用途的克隆条目后重录一遍。
 *
 * 不走 cull/DGC：可见性由 depth 测试保证（借 scene depth），点选/高亮正确性与剔除无关。
 * 非 model 族 draw（cube/天空盒/光）无克隆条目 = 跳过。
 *
 * @param materialFilter kNoMaterialFilter = 不过滤（picking：整段重放）；否则只重放
 *   materialBySlot[objectIndex] == 该材质 id 的 draw（highlight：选中材质全部实体）
 */
void Renderer::replayClonedPass(vk::raii::CommandBuffer& cmd, const FrameDrawList& list,
	ClonePurpose purpose, uint32_t materialFilter) {
	const auto seg = segmentSpan("opaque", list);
	uint32_t clonedDraws = 0;  // 批 9.5：实际发出的 draw 数（跳过项不计）
	for (const auto& call : seg) {
		if (materialFilter != kNoMaterialFilter) {
			// objectIndex 在 pushData 里（DrawArgs 4B，同 cull_driver 的 reinterpret 手法）；
			// 越界 = 表与 draw list 不同步（契约破裂）→ 跳该 draw 而非读越界内存
			const auto* args = reinterpret_cast<const RGL::DrawArgs*>(call.pushData.data());
			if (args->objectIndex >= list.materialBySlot.size())
				continue;
			if (list.materialBySlot[args->objectIndex] != materialFilter)
				continue;
		}
		auto clone = registry.cloneEntryFor(purpose, call.pipeline);
		if (!clone)
			continue;
		const auto& entry = registry.entry(*clone);
		cmd.pushDataEXT(vk::PushDataInfoEXT(entry.pushDataSet,
			vk::HostAddressRangeConstEXT(call.pushData.data(), call.pushData.size())));
		entry.pipeline.bindShaders(cmd);
		cmd.drawMeshTasksEXT(call.groupCount, 1, 1);
		++clonedDraws;
	}
	profilerAddDraws(clonedDraws);  // 批 9.5：picking/highlight 克隆重放段（门默认关 = 0）
}

// ⑪.6 帧末扩展段（Phase 10 批 10.1，接口注释见 vkrenderer.cppm emitFrameEndOverlay）：
// 布局推进（含追踪表更新）→ hook。src 侧按当前布局精确给（blit 写 = Transfer/TransferWrite、
// 捕获读 = Transfer/TransferRead）；其余布局兜底全命令同步（仅调试路径付代价）。
void Renderer::emitFrameEndOverlay(vk::raii::CommandBuffer& cmd, VkImage swapchainImage,
	vk::ImageView swapchainView, vk::Extent2D swapchainExtent) {
	auto z = zone("drawFrame.overlay");  // 批 9.5 观测：imgui 帧末段（headless 无 hook ≈ 0）
	if (swapchainExtent.width == 0 || swapchainExtent.height == 0)
		return;
	RgAttachmentSlot uiSwapchainSlot = kRgNoAttachment;
	for (uint32_t s = 0; s < renderGraph.attachments.size(); ++s) {
		if (renderGraph.attachments[s].kind == RgAttachmentKind::swapchain) {
			uiSwapchainSlot = static_cast<RgAttachmentSlot>(s);
			break;
		}
	}
	if (uiSwapchainSlot == kRgNoAttachment)
		return;  // 纯离屏图无 swapchain 附件（headless 图外路径）
	auto& uiLayout = attachmentLayouts[uiSwapchainSlot];
	if (uiLayout != vk::ImageLayout::eColorAttachmentOptimal) {
		vk::PipelineStageFlags2 uiSrcStage = vk::PipelineStageFlagBits2::eAllCommands;
		vk::AccessFlags2 uiSrcAccess = vk::AccessFlagBits2::eMemoryRead
			| vk::AccessFlagBits2::eMemoryWrite;
		if (uiLayout == vk::ImageLayout::eTransferDstOptimal) {
			uiSrcStage = vk::PipelineStageFlagBits2::eTransfer;
			uiSrcAccess = vk::AccessFlagBits2::eTransferWrite;
		} else if (uiLayout == vk::ImageLayout::eTransferSrcOptimal) {
			uiSrcStage = vk::PipelineStageFlagBits2::eTransfer;
			uiSrcAccess = vk::AccessFlagBits2::eTransferRead;
		}
		vk::ImageMemoryBarrier2 toCao = RGL::createImageBarrier(swapchainImage, uiLayout,
			vk::ImageLayout::eColorAttachmentOptimal, vk::ImageAspectFlagBits::eColor,
			uiSrcStage, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			uiSrcAccess, vk::AccessFlagBits2::eColorAttachmentWrite);
		vk::DependencyInfo uiDep{};
		uiDep.setImageMemoryBarriers(toCao);
		cmd.pipelineBarrier2(uiDep);
		uiLayout = vk::ImageLayout::eColorAttachmentOptimal;
	}
	frameEndHook(cmd, swapchainExtent, swapchainView);
}

// picking 回读录制（Phase 10 批 10.5）：服务未挂 = 零调用（drawFrame 复杂度预算内
// 收拢判空）。布局推进/未装饰图 warn 语义见 PickingService::record。
void Renderer::pickingRecord(vk::raii::CommandBuffer& cmd) {
	auto z = zone("drawFrame.picking");  // 批 9.5 观测：门 = diagnostics.picking（默认关 ≈ 0）
	if (pickingService)
		pickingService->record(cmd, attachmentLayouts);
}

// pass draw 三路分发：drawLight 固定全屏单 group（无 push constant）/ opaque 段 GPU culling（DGC execute 或读回重放，能力位自动切换）/ 普通段逐 draw。
// Phase 10 批 10.5/10.7：picking 与 highlight pass 走第〇分支 = 克隆管线重放（见函数体注）。
void Renderer::executePassDraws(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass,
	const FrameDrawList& list, uint32_t frameSlot) {
	if (pass.state == "picking") {
		// GPU id 点选（批 10.5，D3）：逐 opaque 段 draw 用**克隆条目**重放——heap slot
		// 绑定/scene data 全 bindless 已就位（recordFixedDataPath 在 pass 循环前），
		// per-draw 只差 push 的 DrawArgs(objectIndex)。不走 cull/DGC（遮挡由 depth
		// 测试保证，点选正确性与剔除无关）；非 model 族 draw（cube 等）无克隆 = 跳过。
		replayClonedPass(cmd, list, ClonePurpose::picking, kNoMaterialFilter);
	} else if (pass.state == "highlight") {
		// 点选高亮（批 10.7）：与 picking 同机制，但**按材质过滤**——只重放选中材质的
		// draw，故同材质全部实体一起发光（材质是共享资产，见 scene_context SelectedObject）。
		// 过滤键 = materialBySlot[objectSlot]（renderListSystem 每帧填，纯 CPU 侧不进
		// scene data）。无选中 = 整段跳过（本 pass 零 draw，Esc 取消即走这条）。
		if (list.hasSelection)
			replayClonedPass(cmd, list, ClonePurpose::highlight, list.selectedMaterial);
	} else if (pass.kind == RgPassKind::drawLight) {
		// Phase 7 D1 / Phase 8 ①：固定全屏单 group draw（无 push constant；采样描述符
		// baked 在 shader object 的 extra mapping，输入 RT 的 CAO→SRO 布局转换已由
		// pass.barriers 完成）；条目按 pass.state 名解析（"light"/"tonemap"，
		// 构造期校验过必命中）
		registry.entry(fullscreenPipelineIdx.at(pass.state)).pipeline.bindShaders(cmd);
		cmd.drawMeshTasksEXT(1, 1, 1);
		profilerAddDraws(1);  // 批 9.5：drawLight 档（light/tonemap/ao/taa）恒 1 draw
	} else if (pass.drawSource == "opaque" && cullDriver.drivesDraws()) {
		// GPU culling 路（手术 3：双路提交迁 CullDriver::executeOpaqueSegment——
		// DGC execute（NVIDIA）/ 读回重放（RADV，args = CPU 位级真相源 → BIT-IDENTICAL）
		// 能力位自动切换，重放顺序契约见其类注释）。
		// 债-30 方案 C：判据是 drivesDraws() 而非 active()——`cull_replay` 为 cpu/gpu_lagged
		// 时 draw 由 CPU 剔除结果驱动，落下面那条通用 segmentSpan + drawOne 路（两者参数同源
		// CPU args ⟹ 逐位同，实测截图 SHA256 相同，docs/cpu_perf_plan.md §4）。
		cullDriver.executeOpaqueSegment(cmd, list, registry, frameSlot);
	} else {
		const auto seg = segmentSpan(pass.drawSource, list);
		// 批 9.5：整段一次性入账（每 pass 一次调用，不是每 draw 一次——观察者效应约束）
		profilerAddDraws(static_cast<uint32_t>(seg.size()));
		for (const auto& call : seg)
			drawOne(cmd, call);
	}
}

std::expected<void, Error> Renderer::drawFrame(const FrameDrawList& list) {
		// ①②③ 帧会话开场（Phase 10 批次 0 手术 4：双 fence wait/reset + acquire 分类 +
		// 命令缓冲 begin 全迁 FrameSession::beginFrame；skipped/recreated = 本帧不渲染）
		// 批 9.5 观测（phase9_plan 点名"drawFrame 内 ~95% CPU 时间无子 zone"）：黑洞各段一律
		// zone 化，命名统一 "drawFrame." 前缀 → **未归因 = drawFrame − Σ 前缀段 − 四个原有
		// 子段（fenceWait/acquire/submit/present）**，可直接从 stats JSON 算出。zone 埋点尽量
		// 落在**被调方法首行**（本函数有 100 代码行红线，逐段加花括号必超）。
		auto start = session.beginFrame();
		if (!start)
			return std::unexpected(start.error());
		if (*start != FrameSession::FrameStart::began)
			return {};
		const uint64_t frame = session.frameNumber();  // DIAG：device lost 帧号定位
		auto& device = vkDevice.getDevice();
		auto& cmd = session.commandBuffer();           // 本帧命令缓冲（录制期）
		const uint32_t frameSlot = session.currentFrameSlot();
		const uint32_t imageIndex = session.imageIndex();

	// Phase 9.2：GPU 时间戳——读回本槽上一轮两段（beginFrame 已等完 fence → 必就绪）+
	// 主段复位记入主 CB 头（cull 段复位在 cull CB，两段不相交）+ gpu.frame 跨度起点
	//（覆盖 staging→capture 全录制段；error return 天残的未闭合区域由该槽下一次
	// frameBegin 复位，不进报告）
	// 批 9.5：本行的 query 读回成本由 zone "drawFrame.gpuStamp" 覆盖（在 gpuProfilerFrameBegin 内）
	gpuProfilerFrameBegin(cmd, frameSlot);
	gpuProfilerBegin(cmd, "gpu.frame");

	// ③.5 scene data 上传（Phase 5 批次 2；手术 5：块体提为 writeSceneStaging 私有方法）
	// 批 9.5：zone "drawFrame.staging" 在 writeSceneStaging 首行
	if (auto r = writeSceneStaging(list, frameSlot); !r)
		return std::unexpected(std::move(r).error());

	// ④c GPU culling 数据通路（Phase 7b 批次 5；手术 3：块体迁 CullDriver::runFrame——
	// cull CB 录制/提交/同步等待/读回/对账 + 三档驱动方式与 fence 时序契约见其类注释）
	// 批 9.5：本段内含一次**无限超时** waitForFences（Phase 9 当年落在未归因段里从未被量过），
	// 其单独 zone = cull.fenceWait，见 cull_driver_impl.cppm::harvest。
	// 债-30 方案 C：判据是 gpuPassRuns() 而非 active()——`cull_replay=cpu`（默认）时本段整体
	// 不执行（连 zone 都不出现，别在 trace 里留一条 0 时间的假段）。
	if (cullDriver.gpuPassRuns()) {
		auto z = zone("drawFrame.cull");
		if (auto r = cullDriver.runFrame(frameSlot, frame, list,
				sceneStaging[frameSlot]); !r)
			return std::unexpected(std::move(r).error());
	}

	// ④⑥⑥.5⑦ 数据通路固定段（heap barrier + scene staging 拷贝/屏障 + heap/sampler heap
	// 绑定，Phase 6 起不入图——非 attachment 状态同步；手术 6：块体提为 recordFixedDataPath
	// 私有方法，命令流逐条不变）
	// 批 9.5：zone "drawFrame.dataPath" 在 recordFixedDataPath 首行
	recordFixedDataPath(cmd, frameSlot);

	// 11.3-1d IZB 帧首段（dump 收割 + 门控构造/重建；门关 = 一次布尔判断）
	// 批 9.5：两个图外帧首段各自成 zone（drawFrame.izbPre / drawFrame.evssmPre）
	izbPreFrame();
	evssmPreFrame();
	fsrPreFrame();  // 批 14.5 FSR（同在此之后、统计之前：懒构造的要进 first_frame 账）
	logFirstFrameMemory();

	// ⑤ pass 循环（执行序 = 图线性序，Phase 6）：attachment barrier（布局链推导物化）
	// → beginRendering（逐 pass 附件）→ applyDynamicState(pass.state) → 段内逐 draw
	// （pass.drawSource）→ endRendering。执行器盲消费——barrier/load/store/布局全部来自
	// 编译图，手写布局转换清零。
	auto& swapchainImages = swapchain->getImages();
	auto& swapchainImageViews = swapchain->getImageViews();
	// 批 14.3：两个 extent 分叉。renderExtent = 内部渲染网格（几何 / G-buffer / ao / taa /
	// 深度图 / IzbShadow）；swapchainExtent = 后缓冲（写 swapchain 的 pass、imgui 叠加层、
	// 捕获到后缓冲的口径）。renderScale = 1.0 时两者逐像素相等 ⟹ 交付路径逐位不变。
	const auto renderExtent = swapchain->getRenderExtent();
	const auto swapchainExtent = swapchain->getExtent();
	VkImage swapchainImage = swapchainImages[imageIndex];
	VkImageView swapchainView = *swapchainImageViews[imageIndex];
	VkImage depthImage = *resources->getDepthImage();
	VkImageView depthView = *resources->getDepthImageView();

	// offscreen attachment RT（批次 4）：幂等 ensure + generation 漂移重置布局（手术 6：
	// 块体提为 ensureOffscreenAttachments 私有方法）
	// 批 9.5：zone "drawFrame.offscreen" 在 ensureOffscreenAttachments 首行
	std::vector<RtAttachment*> offscreenRt;
	if (auto r = ensureOffscreenAttachments(renderExtent, offscreenRt); !r)
		return std::unexpected(std::move(r).error());

	// slot → VkImage/View 解析 + barrier 物化（手术 6：三个 lambda 改造为文件内 struct）
	SlotResolver slots{renderGraph.attachments, swapchainImage, swapchainView, depthImage,
		depthView, swapchainExtent, renderExtent, offscreenRt, attachmentLayouts, cmd};

	// 批 9.5：CPU 侧逐 pass zone（GPU 侧 9.2 早有同名时间戳，CPU 侧此前整块无计时）。
	// label 直接复用 pass.name（RgPassPlan.name 是 std::string，活在编译图里 ≥ dump，满足
	// ZoneRecord.name 生命周期要求）→ 热循环零分配零字符串构造，profiler 关时只多一次分支。
	// stats JSON 里按 kind=cpu / kind=gpu 区分同名两侧（Perfetto 也天然分轨）。
	for (const auto& pass : renderGraph.passes) {
		auto z = zone(pass.name);
		gpuProfilerBegin(cmd, pass.name);  // Phase 9.2：pass 时间戳（barrier 段计入）
		slots.emitBarriers(pass.barriers);
		// 11.3-1d IZB 创建 sandwich（图外 compute，录在 deferred light pass 头部；
		// 门关 = izbShadow 恒空，本调用零命令，判据全在方法体内）
		izbLightPassPre(cmd, pass, list);
		evssmLightPassPre(cmd, pass);
		fsrTonemapPassPre(cmd, pass, list);  // 批 14.5 FSR（图外 compute，录在 tonemap
		                                     // 之前；门关 = fsr 恒空，本调用零命令）
		// blit pass（批次 4，offscreen 图变体的 composite）：transferRead 源 → transferWrite
		// 目标整幅拷贝（1:1 同尺寸同格式，LINEAR = 精确拷贝）；两端布局转换已由 pass.barriers
		// 完成（scene_rt CAO→TransferSrc、swapchain 帧首 Undefined→TransferDst），无绘制、
		// 无动态状态
		if (pass.kind == RgPassKind::blit) {
			VkImage srcImage = slots.imageForSlot(*pass.blitSource);
			VkImage dstImage = slots.imageForSlot(*pass.blitTarget);
			vk::ImageBlit blitRegion{};
			blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			// 两端尺寸**各取自自己那个 slot**（批 14.3）：renderScale≠1 时源（scene_rt，
			// 渲染分辨率）与目标（后缓冲，窗口分辨率）不再等尺寸，旧的"1:1 同尺寸"假设
			// 会把放大静默截成左上角拷贝。LINEAR + 满幅两端 = 双线性缩放。
			const auto srcExtent = slots.extentForSlot(*pass.blitSource);
			const auto dstExtent = slots.extentForSlot(*pass.blitTarget);
			// hpp ArrayWrapper1D 不接受 brace 整体赋值，逐元素填（同旧 DIAG 通路写法）
			blitRegion.srcOffsets[0] = vk::Offset3D{0, 0, 0};
			blitRegion.srcOffsets[1] = vk::Offset3D{asI32(srcExtent.width), asI32(srcExtent.height), 1};
			blitRegion.dstOffsets[0] = vk::Offset3D{0, 0, 0};
			blitRegion.dstOffsets[1] = vk::Offset3D{asI32(dstExtent.width), asI32(dstExtent.height), 1};
			::vkCmdBlitImage(*cmd, srcImage,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
				reinterpret_cast<const VkImageBlit*>(&blitRegion), VK_FILTER_LINEAR);
			profilerAddCommands(1);  // 批 9.5：blit pass 恰 1 条 vkCmdBlitImage
			gpuProfilerEnd(cmd);  // blit 无渲染段，pass 时间戳就地收口
			continue;
		}
		// 本 pass 的渲染区域：普通 pass = 窗口尺寸（逐位不变），写 depthOffscreen（阴影图）
		// 的 pass = 该 RT 自己的 extent，并受 pass.viewport 子矩形约束（图集分块；见 SlotResolver::renderRectFor）
		const auto renderRect = slots.renderRectFor(pass);
		beginPassRendering(cmd, slots, pass, renderRect);
		// 动态状态在 draw 时采样（spec chapters/shaders.adoc "Setting State"），与
		// bindShadersEXT 先后无约束；段首各 applyDynamicState 一次（Phase 5 语义，
		// 状态对象经 pass.state 名解析——opaque/skybox/transparent 三成员不变；
		// viewport/scissor 尺寸随 passExtent，与上面 beginRendering.renderArea 同源）
		RGL::applyDynamicState(cmd, renderRect,
			stateKit.stateFor(pass.state), stateKit.depthClipDynamicEnabled(),
			static_cast<uint32_t>(pass.colors.size()));
		// draw 三路分发（手术 6：提为 executePassDraws 私有方法——drawLight 全屏 /
		// opaque GPU-culling / 普通段逐 draw）
		executePassDraws(cmd, pass, list, frameSlot);
		cmd.endRendering();
		profilerAddCommands(1);  // 批 9.5：每 pass 恰 1 条 vkCmdEndRendering
		gpuProfilerEnd(cmd);  // Phase 9.2：pass 时间戳收口（render 段完整窗口）
	}

	// ⑪.5 帧内捕获（Phase 10 批次 0 手术 2：块体迁 FrameCapturer::record——后缓冲/
	// GB 通道两分支的布局追踪推进语义见 frame_capture.cppm 声明注释）。
	// acquire 超时跳帧的 return 在本段之前 = 请求保持 pending 至真正录制的帧。
	// 批 9.5：zone "drawFrame.capture" / "drawFrame.collect" 分别在 FrameCapturer::record /
	// ::collect 首行（collect 内含本帧 fence 等待 = 真截图帧才有的大段）。
	capturer.record(cmd, swapchainImage, swapchainExtent,
		swapchain->getFormat().format, attachmentLayouts);

	// picking 回读（Phase 10 批 10.5）：请求帧录 object_id RT 1-texel → staging
	//（布局推进/未装饰图 warn/nullptr 早退语义见方法体与 picking.cppm）
	pickingRecord(cmd);  // 批 9.5：zone "drawFrame.picking" 在 pickingRecord 首行

	// ⑪.6 帧末扩展段（Phase 10 批 10.1：imgui 调试面板；**空 hook = 零调用零 barrier，
	// 正常路径逐位不变**）。布局推进与 hook 调用体见 emitFrameEndOverlay。
	if (frameEndHook)
		emitFrameEndOverlay(cmd, swapchainImage, swapchainView, swapchainExtent);  // zone 在其首行

	// ⑫ swapchain → PresentSrcKHR（图 presentTransition——src 阶段/访问由最后
	// usage 推导：直渲 = CAO 写、blit 上屏 = Transfer 写，等价旧手工分支；
	// 无 swapchain 写入的离屏图 = nullopt 跳过）。帧内捕获读过后缓冲时由
	// SlotResolver::emitPresentBarrier 补上那次 Transfer 读（理由写在那儿）。
	{
		auto z = zone("drawFrame.cmdEnd");  // 批 9.5：present barrier + gpu.frame 收口 + CB end
		if (renderGraph.presentTransition)
			slots.emitPresentBarrier(*renderGraph.presentTransition);

		gpuProfilerEnd(cmd);  // Phase 9.2：gpu.frame 收口（终点 eAllCommands = 全命令完成）

		// 录制完毕
		cmd.end();
	}

	// ⑬ 提交（手术 4：块体迁 FrameSession::submit——waitStage = 图派生 swapchain
	// 首用阶段；DEVICE_LOST → Error 退出帧循环，其余失败留观测）
	// 批 9.5：CPU 成本由原有 zone "submit" 覆盖（在 FrameSession::submit 首行）
	if (auto r = session.submit(renderGraph.swapchainWaitStage); !r)
		return std::unexpected(std::move(r).error());

	// 帧内捕获收口（手术 2：块体迁 FrameCapturer::collect——等本帧 fence、搬 mapped
	// 字节进结果；fence 不 reset（下帧 ① 照常），停顿只在请求帧，正常帧零影响）
	capturer.collect(device, session.currentInFlightFence());

	// picking 收口（批 10.5）：等本帧 fence 读回 raw id（+1 还原在 PickingService 内）
	if (pickingService)
		pickingService->collect(device, session.currentInFlightFence());

	// ⑭⑮ 上屏 + 帧 slot 推进（手术 4：块体迁 FrameSession::present/advance——
	// present fence 追踪、out-of-date 重建、noPresent 跳 present 语义全保留）
	// 批 9.5：CPU 成本由原有 zone "present" 覆盖（在 FrameSession::present 首行）
	if (auto r = session.present(); !r)
		return std::unexpected(std::move(r).error());
	session.advance();

	profilerFlushFrameCounters();  // 批 9.5：draw.count / vkcmd.count 落账（关 = 静默丢）

	return {};
}

}  // namespace RGL
