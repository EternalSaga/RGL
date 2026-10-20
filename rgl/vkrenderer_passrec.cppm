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
import meshlet_shadow_args;  // ★M-1b：MeshletShadowArgsService::upload + 间接 buffer 句柄
import mdi_rows;             // ★M-1b：kMdiShadowRowStride（MDI 步长唯一真相源，不写字面 24）
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
	/** slot → VkImage（五 kind 穷尽）。buffer 档没有 image 可给：它的资源归模块持有，
	 * 图侧只是名字申报（plan-only），且 buffer 附件的 usage 必然带 `layout == nullopt`
	 * ⟹ 根本进不了物化路径（materializeBarrier 只吃 image barrier 计划）。给 NULL 而不是
	 * 编一个句柄：真走到这里说明上面那条不变式被破，NULL 会让 VVL 立刻点名。 */
	VkImage imageForSlot(RgAttachmentSlot slot) const {
		switch (attachments[slot].kind) {
			case RgAttachmentKind::swapchain: return swapchainImage;
			case RgAttachmentKind::depth: return depthImage;
			case RgAttachmentKind::depthOffscreen:  // 阴影图类：同走 RT 缓存
			case RgAttachmentKind::offscreen: return offscreenRt[slot]->image.image;
			case RgAttachmentKind::buffer: return VK_NULL_HANDLE;  // 见函数注释：不可达
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
			case RgAttachmentKind::buffer: return VK_NULL_HANDLE;  // buffer 档无 view，见 imageForSlot 注释
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
			case RgAttachmentKind::buffer: return {};  // buffer 档无 extent（blit 两端只允许 image kind）
		}
		return {};  // 不可达（穷尽）
	}
	/**
	 * 本 pass 的渲染区域矩形（viewport/scissor + beginRendering.renderArea 共用）。
	 * 解析顺序：① pass 声明的子矩形（图集分块；同时限定 clear 与渲染区）
	 *   ② 否则 depthOffscreen（阴影图）**或写 extentScale > 0 的缩放 RT（批 15.3B
	 *     ssr_trace 半分辨率）**→ 用该 RT 自己的 extent（分辨率与窗口解耦/被缩放；若用
	 *     窗口尺寸会出现 viewport 与 renderArea 不一致 = 贴图比例错 + 边缘漏画）
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
		// 批 15.3B：写**跟随缩放** offscreen RT（extentScale > 0，= ssr_trace 半分辨率）的
		// pass → 渲染区域随该 RT 自己的 extent（viewport/renderArea 同源，depthOffscreen
		// 同档纪律）。既有 offscreen 颜色 RT 全恒 extentScale = 0 ⟹ 本分支不走 = 逐位不变。
		for (const auto& c : pass.colors)
			if (attachments[c.slot].extentScale > 0.0f)
				return {{0, 0}, offscreenRt[c.slot]->extent};
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

/**
 * blit pass 的整幅缩放拷贝（批次 4 语义；批次 B1 随 drawFrame 越 100 行红线提为文件内函数，
 * 命令流与参数逐条不变，口径同本文件 `beginPassRendering` 的「手术 6」摘出）。
 *
 * 两端布局转换**不在这里**——已由 `pass.barriers` 完成（scene_rt CAO→TransferSrc、
 * swapchain 帧首 Undefined→TransferDst）。
 *
 * @param cmd       录制中的命令缓冲
 * @param srcImage  源（transferRead 侧）
 * @param dstImage  目标（transferWrite 侧）
 * @param srcExtent 源 slot 自己的 extent（批 14.3：renderScale≠1 时两端不再等尺寸）
 * @param dstExtent 目标 slot 自己的 extent
 */
void recordBlitPass(vk::raii::CommandBuffer& cmd, VkImage srcImage, VkImage dstImage,
	vk::Extent2D srcExtent, vk::Extent2D dstExtent) {
	vk::ImageBlit blitRegion{};
	blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	// 两端尺寸**各取自自己那个 slot**（批 14.3）：renderScale≠1 时源（scene_rt，渲染分辨率）
	// 与目标（后缓冲，窗口分辨率）不再等尺寸，旧的"1:1 同尺寸"假设会把放大静默截成左上角
	// 拷贝。LINEAR + 满幅两端 = 双线性缩放。
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
}

}  // namespace

// ===== Phase 11 批 11.2（2026-09-29）：将 drawFrame 录制主流程与其原语从 vkrenderer_impl.cppm 整块搬出到本
// partition（主因：那个文件碰到 500 代码行红线，而本块内容内联、与其余部分无关）。
// 命令录制顺序、每条 vk 调用参数、错误路径逐字不变（逐块搬移，非重写）。

// ④⑥⑥.5⑦ 数据通路固定段（heap barrier + scene staging 拷贝/屏障 + heap/sampler heap
// 绑定，Phase 6 起不入图——非 attachment 状态同步；手术 6：块体提为 recordFixedDataPath
// 私有方法，命令流逐条不变）。旧 color/depth attachment barrier 已由图 passes[0].barriers
// 推导发射、与此处 heap barrier 相互独立、命令流先后对换无语义影响。
// ★M-1b：本函数尾部追加 `prepareMdiShadowRows`（**host 直写**行表进 ring 本帧槽，
// 一条 vkCmd 都不发、fixedCmds 计数不动；位置仍在 pass 循环前 = 图首阴影段的行表先于
// 任何 draw 落位）。门关/回落 = mdiSegments 空 = 首行早退（逐位不变的构造性保证）。
void Renderer::recordFixedDataPath(vk::raii::CommandBuffer& cmd, const FrameDrawList& list,
	uint32_t frameSlot) {
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
	// ★M-1b 真 MDI 行表落 GPU（host memcpy+flush 进 ring 本帧槽，**不进命令流**，上面计数
	// 不含它；此时 beginFrame 的 fence wait 已完成 = 写本槽不踩在途读）。fail-fast 四道与
	// 理由 → prepareMdiShadowRows 定义；采集期预检（同源对账/容量/轮转）在 render_list:mdi。
	prepareMdiShadowRows(list, frameSlot);
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

namespace {

/**
 * ★M-1b（§19.2 D3 参数序红线）：`vkCmdDrawMeshTasksIndirectEXT` 的**唯一具名入口**。
 *
 * 真实签名 = `(commandBuffer, buffer, offset, drawCount, stride)`（vulkan_core.h:27021）。
 * spike §6.3 的 M4 教训：`offset == drawCount == 2` 这类"两个数相等 ⟹ 互换测不出东西"
 * 曾让参数序变异静默存活 ⟹ 本入口的形参名逐一对应 vkCmd 语义，且**全项目只此一处**调用
 * （行步长取 kMdiShadowRowStride = 24，而实测行数 1530/偏移 KB 级/12 的命令尾——四个数
 * 两两不等，互换必现形；`just test-all` 的自证回路 + A4 阳性对照是它的牙）。
 * @param cmd          本帧命令缓冲（录制期）
 * @param buffer       行表 VkBuffer（MeshletShadowArgsService 的 ring 整张）
 * @param offsetBytes  行表字节偏移（= DrawCall::argsOffset，同源 pair 的一半）
 * @param drawCount    行数（> 1，调用方守；VUID-07090 的边界核对在 upload 采集两侧都拦）
 * @param strideBytes  行步长（= kMdiShadowRowStride，调用点不许写数值）
 */
void recordMeshTasksIndirectEXT(vk::raii::CommandBuffer& cmd, vk::Buffer buffer,
	uint64_t offsetBytes, uint32_t drawCount, uint64_t strideBytes) {
	cmd.drawMeshTasksIndirectEXT(buffer, offsetBytes, drawCount, strideBytes);
}

}  // namespace

// 单个 mesh draw：push DrawArgs（pushDataEXT）→ bindShaders → drawMeshTasksEXT
//（★M-1b：drawCount > 1 = 唯一判据，第三条换成一次 MDI 展开，见接口注释）。
void Renderer::drawOne(vk::raii::CommandBuffer& cmd, const DrawCall& call) {
	const auto& entry = registry.entry(call.pipeline);
	cmd.pushDataEXT(vk::PushDataInfoEXT(
		entry.pushDataSet, vk::HostAddressRangeConstEXT(call.pushData.data(), call.pushData.size())));
	entry.pipeline.bindShaders(cmd);
	if (call.drawCount > 1u) {
		// 行表载体必然在位：prepareMdiShadowRows 已在 pass 循环前把段写好并核对过服务指针
		// （段非空而指针空 = 那里已 throw，走到这里 mdiShadowArgs 非空是构造性事实）。
		recordMeshTasksIndirectEXT(cmd, vk::Buffer{mdiShadowArgs->buffer()}, call.argsOffset,
			call.drawCount, kMdiShadowRowStride);
		profilerAddMdi(call.drawCount);  // mdi.count/mdi.rows + draw.count 的逻辑口径修正
		return;
	}
	cmd.drawMeshTasksEXT(call.groupCount, 1, 1);
}

// ★M-1b：录制期行表落 GPU（契约与四道 fail-fast 见 vkrenderer.cppm::prepareMdiShadowRows 声明）。
void Renderer::prepareMdiShadowRows(const FrameDrawList& list, uint32_t frameSlot) {
	if (list.mdiSegments.empty())
		return;  // 门关 / 回落 = 零 upload 零新命令（逐位不变的构造性保证；首行判空即全退）
	// 🔴 雷 3 定案：以下每一条按构造都不可达（采集期预检全数拦下 + 装配层同点接线），
	// 到达 = 接线 bug，一律 throw —— upload 被拒后照常提交 = GPU 拿陈旧行表画影子，
	// 「画面错但没有任何信号」正是 §19.8 第 3 条与债-105 反复堵的形态。
	if (mdiShadowArgs == nullptr)
		throw std::runtime_error(fmt::format(
			"mdi shadow: draw list 带 {} 段但行表载体未挂（装配层 setMeshletShadowArgsService "
			"漏接或 MeshletAssembly 生命周期错位）", list.mdiSegments.size()));
	// MDI DrawCall 条数 = 段数（每段恰一条 draw；分家 = A5 读数骗人 + 某级联读陈旧行）。
	uint32_t mdiCalls = 0;
	for (const auto& call : list.shadowCalls)
		if (call.drawCount > 1u)
			++mdiCalls;
	if (mdiCalls != static_cast<uint32_t>(list.mdiSegments.size()))
		throw std::runtime_error(fmt::format(
			"mdi shadow: shadowCalls 内 drawCount>1 条目 {} ≠ mdiSegments {}（行表与 draw 分家）",
			mdiCalls, list.mdiSegments.size()));
	// 🔴 债-140 第三击 ①：描述符堆**槽内容**对账（门 = diagnostics.heap_slot_audit，默认关）。
	// 为什么在 upload 之前：要判的是"本帧录制时这个槽里还是不是构造期写进去的那份字节"。放在
	// upload 之后读，会把"槽被改写"与"行内容写错"两类现场混在一起（虽然它们是不同的内存）。
	// zone = 开销量具（沿用上批教训：帧墙钟的 run-to-run 带比被测量大几个数量级，量不动它）。
	if (diagnostics().heapSlotAudit) {
		auto z = zone("heapSlotAudit");
		mdiShadowArgs->auditAndLog(frameSlot, list.mdiSegments.size());
		mdiShadowArgs->auditEntrySlotsOnce();  // 全部 8 枚槽，不只 34
	}
	uint32_t tasks = 0;  // 债-113：本帧 MDI task 数（= Σ 段锚）。上界 = 总行数（不变量 4 逐行守
	                     // gcX ≤ 1 ⟹ tasks ≤ rows ≤ ring 容量），uint32_t 内不会回卷
	for (const auto& seg : list.mdiSegments) {
		if (seg.frameSlot != frameSlot)
			throw std::runtime_error(fmt::format(
				"mdi shadow: 段 c{} 自带 frameSlot {} ≠ 本帧录制槽 {}（写错槽 = 踩在途帧读，"
				"ring×2 前提破）", seg.cascadeIndex, seg.frameSlot, frameSlot));
		if (auto err = mdiShadowArgs->upload(seg.frameSlot, seg.firstRow, seg.rows); err)
			throw std::runtime_error(fmt::format("mdi shadow: c{} upload 被拒 —— {}",
				seg.cascadeIndex, *err));
		tasks += seg.taskCount;
	}
	// 债-113：`mdi.tasks` 入账 = 全部段真的落进 ring 之后（与 mdi.count/mdi.rows 同为录制期口径；
	// 为什么不在 drawOne、也不在采集侧 → profiler.cppm::profilerAddMdiTasks 声明注释）。
	profilerAddMdiTasks(tasks);
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
		// 簇化轴 ★1-a1ii-α：影子叠加段 = opaque 段之后盲执行（同 pass 同动态状态；正常档
		// 与底共面 + eLess ⟹ 零像素写入，A1/A2 逐位不变的构造性保证）。独立向量不进
		// 候选序对账（FrameDrawList::meshletCalls 注释）；`cull_replay=gpu_sync` 走上面
		// cullDriver 分支 = 本分支不执行 = 叠加静默跳过（本批不接 DGC，★1-b 再议）。
		// 门 meshlet.assets 关 = 空向量 = 零 draw 零开销。
		// ★0.5-b2：叠加 draw 自带深度比较（coplanar = equal/ne 时 loop 前 set loop 后复原，
		// 复原目标 = 本 pass 段首 applyDynamicState 用的值）；off/门关 = 零 set，本 pass 的
		// 命令流与旧逐 bit 同（→ executeMeshletOverlay 注释）。
		if (pass.drawSource == "opaque")
			executeMeshletOverlay(cmd, list,
				stateKit.stateFor(pass.state).depthCompareOp);
	}
}

void Renderer::executeMeshletOverlay(vk::raii::CommandBuffer& cmd, const FrameDrawList& list,
	vk::CompareOp passDepthCompareOp) {
	if (list.meshletCalls.empty())
		return;  // 门关 / 无叠加条目 = 零 draw 零命令（连深度 set 都不发）
	// ★0.5-b2 共面双臂：叠加 draw 携带自己的深度比较（`meshlet.coplanar` = equal/ne，
	// 装配 = wireMeshletOverlayDepthCompare → setter）。
	// 🔴 nullopt（off / 门关）⟹ 本函数**一条 setDepthCompareOp 都不发** ⟹ 门关与 off 档
	// 的动态状态序列逐 bit 不变 = A1 判据的构造性保证（改这条 = 撞 A1 红线）。
	// equal/ne = loop 前 set、loop 后复原 pass 原值：状态窗口恰好盖住叠加那几条 draw，
	// pass 与旧路径的状态序列不动（旧路径 draw 已在本 loop 之前提交；后续 pass 各有段首
	// applyDynamicState，复原本身也不被它们观察到——复原只是把「叠加自己改的」还原，
	// 让本函数对 pass 外零影响，RADV「显式 set ≠ 从不 set」教训的防御性写法）。
	const bool overrideCompare =
		meshletOverlayDepthCompareOp && *meshletOverlayDepthCompareOp != passDepthCompareOp;
	if (overrideCompare) {
		cmd.setDepthCompareOp(*meshletOverlayDepthCompareOp);
		profilerAddCommands(1);  // 批 9.5 vkcmd.count 口径：与 applyDynamicState 同法入账
	}
	profilerAddDraws(static_cast<uint32_t>(list.meshletCalls.size()));
	for (const auto& call : list.meshletCalls)
		drawOne(cmd, call);
	if (overrideCompare) {
		cmd.setDepthCompareOp(passDepthCompareOp);
		profilerAddCommands(1);
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
		// B2-4③′（D5 第一刀）**安全点**：围栏后（beginFrame 的 fence wait 已完成）、任何
		// 录制前（gpuProfilerFrameBegin 会录命令）——脏则重编译，相位表随新图重建后再继续
		// （offGraph.beginFrame 因此跑在新表上）。失败只出声不中断：本帧继续用旧图，
		// 脏位保留下帧重试（重编译失败 = 编程错，日志点名后由测试/冒烟收口）。
		// B2-5：pass 级开关的复合事务落在**同一个安全点**、重编译之前——图在这里变，
		// 紧随其后的重编译才吃得到新 desc（≤1 帧延迟）。两者合起来才是"一个安全点"：
		// 分开调就变成两个事务，中间那一帧会拿着「新申报 + 旧图」建相位表。
		applyPassToggles();  // 无在途意图 = 一次 optional 判空（出厂档零成本）
		recompileGraphIfNeeded();  // 失败内部出声（void 返回 = drawFrame 零分支，认知复杂度不转嫁）
		// 批次 B1：图外工作申报面本帧记账清零（相位归 0；位置/覆盖两条判据的现场都从这里起）
		offGraph.beginFrame();
		// 批次 B1b：图外同步发射账清零（应发次数是启动期事实，跨帧保留）
		offGraphSync.beginFrame();

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
	recordFixedDataPath(cmd, list, frameSlot);
	// 批次 B1：进入「帧首段」相位——meshlet_cull 的申报挂点就在这里（它必须在 heap 已绑定、
	// scene data 已拷完，且任何 render pass 之外）。
	offGraph.advanceTo(offGraph.prologuePhase());

	// 簇化轴 ★1-a2-i 逐簇视锥剔除（图外 compute，定义在 :offgraph）。位置三条硬约束：
	// ① 必须在 recordFixedDataPath **之后**——heap 已绑定、scene data 已拷完（剔除要读对象矩阵
	//    表 @128）；② 必须在 pass 循环**之前**——还在 gpu.frame 内、任何 render pass 外（barrier
	//    的 eHost stage 不能在 pass 内），且 geometry pass 的 mesh shader 是本 dispatch 的消费者；
	// ③ 门关 = meshletCull 恒 nullptr ⟹ 一次指针判空、**零新命令**（逐位不变的构造性保证）。
	recordMeshletCullDispatch(cmd, list);

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
	if (auto r = ensureOffscreenAttachments(renderExtent, frame, offscreenRt); !r)
		return std::unexpected(std::move(r).error());

	// slot → VkImage/View 解析 + barrier 物化（手术 6：三个 lambda 改造为文件内 struct）
	SlotResolver slots{renderGraph.attachments, swapchainImage, swapchainView, depthImage,
		depthView, swapchainExtent, renderExtent, offscreenRt, attachmentLayouts, cmd};

	// 批 9.5：CPU 侧逐 pass zone（GPU 侧 9.2 早有同名时间戳，CPU 侧此前整块无计时）。
	// label 直接复用 pass.name（RgPassPlan.name 是 std::string，活在编译图里 ≥ dump，满足
	// ZoneRecord.name 生命周期要求）→ 热循环零分配零字符串构造，profiler 关时只多一次分支。
	// stats JSON 里按 kind=cpu / kind=gpu 区分同名两侧（Perfetto 也天然分轨）。
	for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(renderGraph.passes.size());
			++passIndex) {
		const auto& pass = renderGraph.passes[passIndex];
		auto z = zone(pass.name);
		// 批次 B1：推进到「第 passIndex 个 pass 之前」相位（图外工作按相位对账，
		// 单调递增 ⟹ 挂点被挪到循环内/循环后都会与申报相位对不上）。
		offGraph.advanceTo(offGraph.beforePassPhase(passIndex));
		gpuProfilerBegin(cmd, pass.name);  // Phase 9.2：pass 时间戳（barrier 段计入）
		slots.emitBarriers(pass.barriers);
		// 11.3-1d IZB 创建 sandwich（图外 compute，录在 deferred light pass 头部；
		// 门关 = izbShadow 恒空，本调用零命令，判据全在方法体内）
		izbLightPassPre(cmd, pass, list);
		evssmLightPassPre(cmd, pass);
		fsrTonemapPassPre(cmd, pass, list);  // 批 14.5 FSR（图外 compute，录在 tonemap
		                                     // 之前；门关 = fsr 恒空，本调用零命令）
		// blit pass（批次 4，offscreen 图变体的 composite）：transferRead 源 → transferWrite
		// 目标整幅拷贝；两端布局转换已由 pass.barriers 完成，无绘制、无动态状态。
		if (pass.kind == RgPassKind::blit) {
			recordBlitPass(cmd, slots.imageForSlot(*pass.blitSource),
				slots.imageForSlot(*pass.blitTarget), slots.extentForSlot(*pass.blitSource),
				slots.extentForSlot(*pass.blitTarget));
			gpuProfilerEnd(cmd);  // blit 无渲染段，pass 时间戳就地收口
			continue;
		}
		// 本 pass 的渲染区域：普通 pass = 窗口尺寸（逐位不变），写 depthOffscreen（阴影图）
		// 的 pass = 该 RT 自己的 extent，并受 pass.viewport 子矩形约束（图集分块；见 SlotResolver::renderRectFor）
		const auto renderRect = slots.renderRectFor(pass);
		beginPassRendering(cmd, slots, pass, renderRect);
		// 批次 B1：进入「本 pass 体内」相位。分这一档是为了让「sandwich 挂点从 pass 外
		// 漂进 pass 内」可归因——IZB 自己 beginMaskPass、FSR 发 barrier + compute，这些命令
		// 都不能出现在 dynamic rendering 实例里，而只按 pass 分档的话那种漂移完全 invisible。
		offGraph.advanceTo(offGraph.inPassPhase(passIndex));
		// 动态状态在 draw 时采样（spec chapters/shaders.adoc "Setting State"），与
		// bindShadersEXT 先后无约束；段首各 applyDynamicState 一次（Phase 5 语义，
		// 状态对象经 pass.state 名解析（成员表 = StateKit kStateSlots，名字 = pass_vocabulary）；
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
	// 批次 B1：帧末覆盖对账（申报了却没录 = 静默旁路；门关了却录了 = 破逐位不变红线）。
	// 放在这里而不是 CB end 之后：四个图外工作的挂点全在 pass 循环之内/之前，此刻门位已定。
	offGraphEndFrame(list);

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
