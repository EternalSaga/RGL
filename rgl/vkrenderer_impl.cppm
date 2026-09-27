module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <ranges>
#include <fmt/format.h>
module vk_renderer:impl;

import vk_renderer;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_swapchain;
import vk_resources;
import scene_data;  // kSceneData* 布局常量 + SceneDataService（架构债务续账步骤 1）
import render_target_cache;  // RenderTargetCache/RtAttachment（架构债务续账步骤 2）
import scene_forward_types_generated;  // 前向头 SceneLight（C API 反射生成，替换原 LightDataGpu）
import shader_registry;
import shader;
import state_kit;
import gpu_cull;
import frame_capture;
import cull_driver;
import frame_session;
import gpu_profiler;  // Phase 9.2：GPU 时间戳钩子（读回归并进 9.1 同报告 gpu 轨）
import diagnostics;
import vulkan_utils;
import VmaRaii;
import frame_draw_list;
import model_types_generated;
import lights_types_generated;
import material_types_generated;  // DrawArgs（GPU culling 对账经 objectIndex@96 映射 CPU draw）
import features;
import render_graph;

namespace RGL {

// Renderer 重成员函数定义（2026-09-02 编译提速 A 方案）：ctor/drawFrame/重建路径/
// 同步对象/staging 全部移入本 partition implementation unit——接口 TU
// （vkrenderer.cppm）只留类声明，200+ 成员函数体的 AST 不再序列化进 BMI；
// 本单元是叶子（无人 import），编译完全并行、不上关键路径。
// 各函数语义/步骤注释随体迁自 vkrenderer.cppm。

namespace {

/**
 * 执行器可解析性校验（Phase 6，构造期 fail-fast——录制期零图错误）：
 * 图必须恰好引用 swapchain/depth 各一次；draw pass 的 state/drawSource 名字可解析；
 * blit pass 必须两端齐（compileRenderGraph 已保证拓扑，此处防绕过）；
 * drawLight pass 的 state 必为 "light"。
 * @param graph 编译渲染图（Renderer 构造入参）
 * @note 任一违例抛 runtime_error（"Renderer: …" 前缀，带 pass 名/实际值定位）
 */
void validateRenderGraphForExecutor(const RGL::CompiledRenderGraph& graph) {
	auto fail = [](std::string msg) { throw std::runtime_error("Renderer: " + msg); };
	if (graph.passes.empty())
		fail("render graph has no passes");
	uint32_t swapchainRefs = 0, depthRefs = 0;
	for (const auto& att : graph.attachments) {
		if (att.kind == RgAttachmentKind::swapchain) ++swapchainRefs;
		if (att.kind == RgAttachmentKind::depth) ++depthRefs;
	}
	if (swapchainRefs != 1)
		fail(fmt::format("render graph must reference exactly one swapchain attachment (got {})",
			swapchainRefs));
	if (depthRefs != 1)
		fail(fmt::format("render graph must reference exactly one depth attachment (got {})",
			depthRefs));
	static constexpr std::array<const char*, 3> kKnownDrawSources{"opaque", "skybox",
		"transparent"};
	// 单 pass 可解析性校验（三形态：blit 两端齐 / drawLight state 在词表 / draw state+
	// drawSource 名字表）。手术 6：提为 lambda 降宿主认知复杂度——fail 经捕获共用、
	// 错误消息逐字不变；continue → return（lambda 内提前结束本 pass 校验语义等价）。
	auto validatePass = [&](const RgPassPlan& pass) {
		if (pass.kind == RgPassKind::blit) {
			if (!pass.blitSource || !pass.blitTarget)
				fail(fmt::format("blit pass '{}' missing source/target resolution", pass.name));
			return;
		}
		// Phase 7 D1 / Phase 8 ①泛化：drawLight = 固定全屏 draw（无 drawSource/段），
		// state 必在 kFullscreenPassStates 词表（"light"/"tonemap"）
		if (pass.kind == RgPassKind::drawLight) {
			if (!std::ranges::any_of(kFullscreenPassStates,
					[&](std::string_view s) { return pass.state == s; }))
				fail(fmt::format(
					"render graph fullscreen pass '{}' has state '{}' (expected one of the "
					"kFullscreenPassStates vocabulary)",
					pass.name, pass.state));
			return;
		}
		if (pass.state != "opaque" && pass.state != "skybox" && pass.state != "transparent"
			&& pass.state != "picking")  // Phase 10 批 10.5：id pass（克隆条目重放，StateKit 有段）
			fail(fmt::format("render graph pass '{}' has unknown state '{}'", pass.name,
				pass.state));
		if (!std::ranges::any_of(kKnownDrawSources,
				[&](const char* s) { return pass.drawSource == s; }))
			fail(fmt::format("render graph pass '{}' has unknown drawSource '{}'", pass.name,
				pass.drawSource));
	};
	for (const auto& pass : graph.passes)
		validatePass(pass);
}

/**
 * FrameDrawList 段边界 → [begin, end)（drawSource 名 = 段名的机械映射，
 * 段序契约 = FrameDrawList 注释：opaque | skybox | transparent）。
 * @param drawSource 段名（"opaque" / "skybox" / 其余按 "transparent"）
 * @param list 每帧绘制列表（skyboxStart / transparentStart 边界 + calls）
 * @return calls 内的半开区间 [begin, end)
 */
std::pair<uint32_t, uint32_t> segmentRange(std::string_view drawSource,
	const FrameDrawList& list) {
	const uint32_t size = static_cast<uint32_t>(list.calls.size());
	if (drawSource == "opaque") return {0, list.skyboxStart};
	if (drawSource == "skybox") return {list.skyboxStart, list.transparentStart};
	return {list.transparentStart, size};
}

/** attachment slot → VkImage/View 解析 + barrier 物化发射（手术 6：drawFrame 内 imageForSlot/viewForSlot/emitBarriers 三个 lambda 改造为文件内 struct——搬移非重写、逐条命令等价；持 swapchain/depth 句柄 + offscreenRt + 追踪布局表引用 + 本帧 cmd，生命周期 = drawFrame 一次调用）。 */
struct SlotResolver {
	const std::vector<RgCompiledAttachment>& attachments;
	VkImage swapchainImage; VkImageView swapchainView;
	VkImage depthImage; VkImageView depthView;
	const std::vector<RtAttachment*>& offscreenRt;
	std::vector<vk::ImageLayout>& layouts;
	vk::raii::CommandBuffer& cmd;
	/** slot → VkImage（三 kind 穷尽）。 */
	VkImage imageForSlot(RgAttachmentSlot slot) const {
		switch (attachments[slot].kind) {
			case RgAttachmentKind::swapchain: return swapchainImage;
			case RgAttachmentKind::depth: return depthImage;
			case RgAttachmentKind::offscreen: return offscreenRt[slot]->image.image;
		}
		return VK_NULL_HANDLE;  // 不可达（穷尽），消 warning
	}
	/** slot → VkImageView（offscreen 走 optional + raii 双重解引用）。 */
	VkImageView viewForSlot(RgAttachmentSlot slot) const {
		switch (attachments[slot].kind) {
			case RgAttachmentKind::swapchain: return swapchainView;
			case RgAttachmentKind::depth: return depthView;
			case RgAttachmentKind::offscreen: return **offscreenRt[slot]->view;  /* optional 解引用 + raii handle */
		}
		return VK_NULL_HANDLE;
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
		for (const auto& plan : barriers)
			layouts[plan.attachment] = plan.newLayout;
	}
};

/** 构造并发起一个 pass 的 dynamic rendering（手术 6：自 drawFrame 摘出——color/depth RenderingAttachmentInfo 逐字段构造 + beginRendering，命令参数逐字不变；动态状态/draw 分发留 drawFrame 因需 stateKit/registry 成员）。 */
void beginPassRendering(vk::raii::CommandBuffer& cmd, const SlotResolver& slots,
	const RgPassPlan& pass, vk::Extent2D extent) {
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
	renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
	renderingInfo.layerCount = 1;
	renderingInfo.setColorAttachments(colorInfos);
	if (depthInfo)
		renderingInfo.setPDepthAttachment(&*depthInfo);
	cmd.beginRendering(renderingInfo);
}

}  // namespace

Renderer::Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
		RGL::SceneDataService& sceneData, RGL::Environment& env, RGL::RenderTargetCache& rtCache,
		RGL::ShaderRegistry& shaderRegistry, RGL::CompiledRenderGraph renderGraph,
		RGL::GpuCull* vkGpuCull, RGL::DgcCommands* vkDgc, RGL::DgcStaticCommands* vkDgcStatic)
	: vkDevice(vkDevice),
	  swapchain(&vkSwapchain),
	  resources(&vkResources),
	  sceneData(&sceneData),
	  env(&env),
	  rtCache(&rtCache),
	  registry(shaderRegistry),
	  renderGraph(std::move(renderGraph)),
	  // culling 帧编舞（手术 3）：④c 提交时序 + opaque 段三路（IES/静态 DGC/重放）+ 对账
	  // 全迁 CullDriver，构造期即分配 cull CB（inactive = 不分配，行为中性与摘出前一致）
	  cullDriver(vkDevice, vkGpuCull, vkDgc, vkDgcStatic, FrameSession::kFramesInFlight),
	  attachmentLayouts(this->renderGraph.attachments.size(), vk::ImageLayout::eUndefined),
	  // 帧内捕获器（手术 2）：持资源层 + 编译图引用，构造期自图定位 swapchain slot
	  capturer(vkResources, rtCache, this->renderGraph),
	  // edS3 能力门 + 四段状态值（Phase 10 批次 0 手术 1：状态表迁 StateKit，
	  // transparent/skybox/light 的差异值语义注释随迁 state_kit.cppm ctor）
	  stateKit(vkDevice.getOptionalFeatures().depthClipEnable),
	  // 帧生命周期会话（手术 4）：四套同步对象 + 每帧命令缓冲 + swapchain/depth 重建，
	  // 构造期即建（原 createSyncObjects/allocateCommandBuffers 移入其 ctor）
	  session(vkDevice, vkSwapchain, vkResources, diagnostics().noPresent),
	  graphicsCommandPool(vkDevice.getGraphicsCommandPool()) {
		validateRenderGraphForExecutor(this->renderGraph);
		for (const auto& warning : this->renderGraph.warnings) {
			auto logger = RLLogger::getInstance();
			if (logger) logger->warn("RenderGraph: {}", warning);
		}
		// descriptor heap 的 GPU 端地址，每帧传给 vkCmdBindResourceHeapEXT
		heapAddress = vkResources.getDescriptorHeap().getGpuAddress();
		// 四段状态值构造已迁 StateKit（Phase 10 批次 0 手术 1，语义注释随迁 state_kit.cppm）
		// Phase 8 ①泛化：逐词表名（"light"/"tonemap"）从 registry 取固定全屏管线下标；
		// 图含对应 state 的 drawLight pass 而条目缺失 = 配置错误（main 须先建条目）
		for (const auto name : kFullscreenPassStates)
			if (auto idx = shaderRegistry.pipelineIndex(name))
				fullscreenPipelineIdx.emplace(std::string{name}, *idx);
		for (const auto& p : this->renderGraph.passes)
			if (p.kind == RgPassKind::drawLight && !fullscreenPipelineIdx.contains(p.state))
				throw std::runtime_error(fmt::format(
					"Renderer: render graph has drawLight pass (state '{}') but no matching "
					"pipeline entry (main must add{}Entry before Renderer)",
					p.state, p.state == "light" ? "Light" : "Tonemap"));
		createSceneStaging(vkResources);  // 同步对象/命令缓冲已随手术 4 迁 FrameSession ctor
	}
// （captureBpp / Renderer::ensureCaptureStaging 已随手术 2 迁 frame_capture_impl.cppm）

// （createSyncObjects / allocateCommandBuffers 已随手术 4 迁 frame_session_impl.cppm）
void Renderer::createSceneStaging(RGL::Resources& vkResources) {
		for (auto& staging : sceneStaging) {
			vk::BufferCreateInfo info{};
			info.size = kSceneDataBytes;
			// eShaderDeviceAddress：createBufferWithAddress 无条件查询地址（staging 实际
			// 不用，usage 缺位会被 VVL 拦下 vkGetBufferDeviceAddress 报错）
			info.usage = vk::BufferUsageFlagBits::eTransferSrc |
				vk::BufferUsageFlagBits::eShaderDeviceAddress;
			VmaAllocationCreateInfo alloc{};
			alloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
			alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
				VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
			VmaAllocationInfo allocData{};
			auto result = vkResources.getAllocator().createBufferWithAddress(
				vkDevice.getDevice(), info, alloc, &allocData);
			if (!result)
				throw std::runtime_error(fmt::format(
					"Renderer: scene staging alloc failed: {}", result.error().describe()));
			staging = std::move(*result);
		}
	}
// （recreateSwapchainAndDepth 已随手术 4 迁 frame_session_impl.cppm）
 	/** drawFrame 定义（整体流程见接口声明 + 类头；逐步骤注记见体内 ①~⑮）：
	 * 双 fence wait/reset → acquire（100ms 超时跳帧）→ ③.5 scene staging 写
	 *（头 128B + 矩阵表 + 灯数组尾区）→ ⑥ heap barrier（3 dst flag）+ ⑥.5 scene 拷贝
	 *（kSceneDataBytes=262800B）→ ⑦ heap/sampler heap 绑定 → ⑤ 逐 pass（barrier 物化 /
	 * beginRendering / applyDynamicState / 段内逐 draw / blit / drawLight）→ ⑪.5 帧内
	 * 捕获回读 → ⑫ presentTransition → ⑬ submit（waitStage = 图派生）→ 捕获搬移 →
	 * ⑭ present（present fence）→ ⑮ 帧 slot 推进。 */
	std::expected<void, Error> Renderer::writeSceneStaging(const FrameDrawList& list,
		uint32_t frameSlot) {
	// ③.5 scene data 上传（Phase 5 批次 2；手术 5：块体提为私有方法，语义零改动）。
	auto* stage = static_cast<std::byte*>(sceneStaging[frameSlot].getMappedData());
	std::memcpy(stage, &list.sceneData.light, sizeof(SceneLight));
	// Phase 5.5：env 参数写 @48（light 48B 之后、64B 头内——与 objectMatrices 区无重叠）
	std::memcpy(stage + kSceneDataEnvOffset, &list.sceneData.envParams, sizeof(glm::vec2));
	// Phase 8 收尾：tonemap 曝光写 @56（env 16B 保留区内；tonemap.frag Load(56) 消费）
	std::memcpy(stage + kSceneDataExposureOffset, &list.sceneData.exposure, sizeof(float));
	// Phase 5.5 批次 4：skyboxInvViewProj 写 @64（sky.slang skyClipToWorld 消费）
	std::memcpy(stage + kSceneDataSkyOffset, &list.sceneData.skyInvViewProj, sizeof(glm::mat4));
	const auto matricesBytes = list.sceneData.objectMatrices.size() * sizeof(glm::mat4);
	if (matricesBytes > kSceneMatrixCapacity * kSceneMatrixStride)
		return std::unexpected(Error{ErrorCode::Unknown,
			"scene objectMatrices exceeds capacity (Phase 5 contract, main startup check bypassed)"});
	std::memcpy(stage + kSceneDataHeaderBytes, list.sceneData.objectMatrices.data(),
		matricesBytes);
	// Phase 7 D3：多灯数组写尾区（count@base + GpuLight[count]@base+16；lightCount=0 只写 count）
	if (list.sceneData.lightCount > kMaxLights)
		return std::unexpected(Error{ErrorCode::Unknown,
			"scene lightCount exceeds kMaxLights (lights_schema.lua contract)"});
	std::memcpy(stage + kSceneDataLightArrayOffset, &list.sceneData.lightCount, sizeof(uint32_t));
	std::memcpy(stage + kSceneDataLightArrayOffset + 16, list.sceneData.lightArray.data(),
		list.sceneData.lightCount * kGpuLightStride);
	// Phase 7b+（NV DGC 规避）：per-object MVP 表上传（尾区 @kSceneDataMvpTableOffset，
	// scene.slang readObjectMvp 消费；值 = 原 DrawArgs.mvp，位级一致）
	const auto mvpsBytes = list.sceneData.objectMvps.size() * sizeof(glm::mat4);
	if (mvpsBytes > kSceneMatrixCapacity * kSceneMatrixStride)
		return std::unexpected(Error{ErrorCode::Unknown,
			"scene objectMvps exceeds capacity (same contract as objectMatrices)"});
	std::memcpy(stage + kSceneDataMvpTableOffset, list.sceneData.objectMvps.data(), mvpsBytes);
	// Phase 7b+（续）：per-object 材质/几何表上传（@kSceneDataDrawTableOffset，
	// scene.slang readObjectDraw 消费；DrawArgs 仅剩 objectIndex）
	const auto drawsBytes = list.sceneData.objectDraws.size() * sizeof(ObjectDraw);
	if (drawsBytes > kSceneMatrixCapacity * kSceneDrawStride)
		return std::unexpected(Error{ErrorCode::Unknown,
			"scene objectDraws exceeds capacity (same contract as objectMatrices)"});
	std::memcpy(stage + kSceneDataDrawTableOffset, list.sceneData.objectDraws.data(), drawsBytes);
	// Phase 8 批次 ④a：漫反射辐照度 SH9 上传（scene data 尾区 kSceneDataIrradianceOffset，
	// scene.slang irradiance(N) 消费；静态数据，随每帧 mirror 拷入）
	std::memcpy(stage + kSceneDataIrradianceOffset, list.sceneData.irradianceSH.data(),
		kSceneDataIrradianceBytes);
	return {};
}
// ===== 手术 6（2026-09-12）：drawFrame 语义完整块摘为下列 Renderer 私有方法（声明见 vkrenderer.cppm 私有段）+ 文件内 SlotResolver/beginPassRendering——命令录制顺序、每条 vk 调用参数、错误路径逐字不变（逐块搬移，非重写）=====

// ④⑥⑥.5⑦ 数据通路固定段（heap barrier → scene staging 拷贝/屏障 → heap/sampler heap 绑定，Phase 6 起不入图——非 attachment 状态同步；旧 color/depth attachment barrier 已由图 passes[0].barriers 推导发射、与此处 heap barrier 相互独立、命令流先后对换无语义影响）
void Renderer::recordFixedDataPath(vk::raii::CommandBuffer& cmd, uint32_t frameSlot) {
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

	// ⑥.5 scene data staging → device buffer 拷贝（Phase 5 批次 2）+ Transfer 写 →
	// 图形阶段 shader 读 barrier（storage buffer 的写/读即使同 cmd 也须显式依赖，
	// VVL BUFFER_SYNC 检查项）。目标未创建（无模型场景 = SceneDataService::ensure
	// 未被调用）则跳过——cube 管线不绑 scene data。
	{
		const auto& sceneBuffer = sceneData->getBuffer();
		if (sceneBuffer.buffer != VK_NULL_HANDLE) {
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
		}
	}

	// ⑦ 把 descriptor heap 绑定到命令缓冲（必须在 beginRendering 之前）：
	// 此后所有描述符引用走 heap，与传统 descriptor set 状态互斥（坑 9）。
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

// offscreen attachment RT 幂等 ensure（批次 4；extent 随 resize 重建含 waitIdle，录制期调用安全；format 经词表解析，"inherit"=swapchain 格式、具名=显式 RT）。出参 offscreenRt 按 slot 填（非 offscreen 槽留 null）。
std::expected<void, Error> Renderer::ensureOffscreenAttachments(vk::Extent2D extent,
	std::vector<RtAttachment*>& offscreenRt) {
	offscreenRt.assign(renderGraph.attachments.size(), nullptr);
	// RT 重建检测：generation 变化（新图布局 Undefined）→ 同步重置 attachmentLayouts，否则下帧首 barrier oldLayout 失配（resize 必现）
	std::vector<uint64_t> rtGenerations(renderGraph.attachments.size(), 0);
	for (uint32_t s = 0; s < renderGraph.attachments.size(); ++s) {
		const auto& att = renderGraph.attachments[s];
		if (att.kind != RgAttachmentKind::offscreen)
			continue;
		const auto fmt = RGL::resolveRgFormat(att.format, swapchain->getFormat().format);
		auto rtResult = rtCache->ensure(att.name, extent, fmt);
		if (!rtResult)
			return std::unexpected(std::move(rtResult).error());
		offscreenRt[s] = *rtResult;
		if ((*rtResult)->generation != rtGenerations[s]) {
			rtGenerations[s] = (*rtResult)->generation;
			attachmentLayouts[s] = vk::ImageLayout::eUndefined;
		}
	}
	return {};
}

// 单个 mesh draw：push DrawArgs（pushDataEXT）+ bindShaders + drawMeshTasksEXT。
void Renderer::drawOne(vk::raii::CommandBuffer& cmd, const DrawCall& call) {
	const auto& entry = registry.entry(call.pipeline);
	cmd.pushDataEXT(vk::PushDataInfoEXT(
		entry.pushDataSet, vk::HostAddressRangeConstEXT(call.pushData.data(), call.pushData.size())));
	entry.pipeline.bindShaders(cmd);
	cmd.drawMeshTasksEXT(call.groupCount, 1, 1);
}

// ⑪.6 帧末扩展段（Phase 10 批 10.1，接口注释见 vkrenderer.cppm emitFrameEndOverlay）：
// 布局推进（含追踪表更新）→ hook。src 侧按当前布局精确给（blit 写 = Transfer/TransferWrite、
// 捕获读 = Transfer/TransferRead）；其余布局兜底全命令同步（仅调试路径付代价）。
void Renderer::emitFrameEndOverlay(vk::raii::CommandBuffer& cmd, VkImage swapchainImage,
	vk::ImageView swapchainView, vk::Extent2D extent) {
	if (extent.width == 0 || extent.height == 0)
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
	frameEndHook(cmd, extent, swapchainView);
}

// picking 回读录制（Phase 10 批 10.5）：服务未挂 = 零调用（drawFrame 复杂度预算内
// 收拢判空）。布局推进/未装饰图 warn 语义见 PickingService::record。
void Renderer::pickingRecord(vk::raii::CommandBuffer& cmd) {
	if (pickingService)
		pickingService->record(cmd, attachmentLayouts);
}

// pass draw 三路分发：drawLight 固定全屏单 group（无 push constant）/ opaque 段 GPU culling（DGC execute 或读回重放，能力位自动切换）/ 普通段逐 draw。
// Phase 10 批 10.5：picking pass（state="picking"）走第〇分支 = 克隆管线重放（见函数体注）。
void Renderer::executePassDraws(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass,
	const FrameDrawList& list, uint32_t frameSlot) {
	if (pass.state == "picking") {
		// GPU id 点选（批 10.5，D3）：逐 opaque 段 draw 用**克隆条目**重放——heap slot
		// 绑定/scene data 全 bindless 已就位（recordFixedDataPath 在 pass 循环前），
		// per-draw 只差 push 的 DrawArgs(objectIndex)。不走 cull/DGC（遮挡由 depth
		// 测试保证，点选正确性与剔除无关）；非 model 族 draw（cube 等）无克隆 = 跳过。
		const auto [segBegin, segEnd] = segmentRange("opaque", list);
		for (uint32_t i = segBegin; i < segEnd; ++i) {
			const auto& call = list.calls[i];
			auto clone = registry.pickingEntryFor(call.pipeline);
			if (!clone)
				continue;
			const auto& entry = registry.entry(*clone);
			cmd.pushDataEXT(vk::PushDataInfoEXT(entry.pushDataSet,
				vk::HostAddressRangeConstEXT(call.pushData.data(), call.pushData.size())));
			entry.pipeline.bindShaders(cmd);
			cmd.drawMeshTasksEXT(call.groupCount, 1, 1);
		}
	} else if (pass.kind == RgPassKind::drawLight) {
		// Phase 7 D1 / Phase 8 ①：固定全屏单 group draw（无 push constant；采样描述符
		// baked 在 shader object 的 extra mapping，输入 RT 的 CAO→SRO 布局转换已由
		// pass.barriers 完成）；条目按 pass.state 名解析（"light"/"tonemap"，
		// 构造期校验过必命中）
		registry.entry(fullscreenPipelineIdx.at(pass.state)).pipeline.bindShaders(cmd);
		cmd.drawMeshTasksEXT(1, 1, 1);
	} else if (pass.drawSource == "opaque" && cullDriver.active()) {
		// GPU culling 路（手术 3：双路提交迁 CullDriver::executeOpaqueSegment——
		// DGC execute（NVIDIA）/ 读回重放（RADV，args = CPU 位级真相源 → BIT-IDENTICAL）
		// 能力位自动切换，重放顺序契约见其类注释）
		cullDriver.executeOpaqueSegment(cmd, list, registry, frameSlot);
	} else {
		const auto [segBegin, segEnd] = segmentRange(pass.drawSource, list);
		for (uint32_t i = segBegin; i < segEnd; ++i)
			drawOne(cmd, list.calls[i]);
	}
}

std::expected<void, Error> Renderer::drawFrame(const FrameDrawList& list) {
		// ①②③ 帧会话开场（Phase 10 批次 0 手术 4：双 fence wait/reset + acquire 分类 +
		// 命令缓冲 begin 全迁 FrameSession::beginFrame；skipped/recreated = 本帧不渲染）
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
	gpuProfilerFrameBegin(cmd, frameSlot);
	gpuProfilerBegin(cmd, "gpu.frame");

	// ③.5 scene data 上传（Phase 5 批次 2；手术 5：块体提为 writeSceneStaging 私有方法）
	if (auto r = writeSceneStaging(list, frameSlot); !r)
		return std::unexpected(std::move(r).error());

	// ④c GPU culling 数据通路（Phase 7b 批次 5；手术 3：块体迁 CullDriver::runFrame——
	// cull CB 录制/提交/同帧同步等待/读回/对账 + fence 与主 CB 共用的时序契约见其类注释）
	if (cullDriver.active()) {
		if (auto r = cullDriver.runFrame(frameSlot, frame, list,
				sceneStaging[frameSlot], session.currentInFlightFence()); !r)
			return std::unexpected(std::move(r).error());
	}

	// ④⑥⑥.5⑦ 数据通路固定段（heap barrier + scene staging 拷贝/屏障 + heap/sampler heap
	// 绑定，Phase 6 起不入图——非 attachment 状态同步；手术 6：块体提为 recordFixedDataPath
	// 私有方法，命令流逐条不变）
	recordFixedDataPath(cmd, frameSlot);

	// ⑤ pass 循环（执行序 = 图线性序，Phase 6）：attachment barrier（布局链推导物化）
	// → beginRendering（逐 pass 附件）→ applyDynamicState(pass.state) → 段内逐 draw
	// （pass.drawSource）→ endRendering。执行器盲消费——barrier/load/store/布局全部来自
	// 编译图，手写布局转换清零。
	auto& swapchainImages = swapchain->getImages();
	auto& swapchainImageViews = swapchain->getImageViews();
	const auto extent = swapchain->getExtent();
	VkImage swapchainImage = swapchainImages[imageIndex];
	VkImageView swapchainView = *swapchainImageViews[imageIndex];
	VkImage depthImage = *resources->getDepthImage();
	VkImageView depthView = *resources->getDepthImageView();

	// offscreen attachment RT（批次 4）：幂等 ensure + generation 漂移重置布局（手术 6：
	// 块体提为 ensureOffscreenAttachments 私有方法）
	std::vector<RtAttachment*> offscreenRt;
	if (auto r = ensureOffscreenAttachments(extent, offscreenRt); !r)
		return std::unexpected(std::move(r).error());

	// slot → VkImage/View 解析 + barrier 物化（手术 6：三个 lambda 改造为文件内 struct）
	SlotResolver slots{renderGraph.attachments, swapchainImage, swapchainView, depthImage,
		depthView, offscreenRt, attachmentLayouts, cmd};

	for (const auto& pass : renderGraph.passes) {
		gpuProfilerBegin(cmd, pass.name);  // Phase 9.2：pass 时间戳（barrier 段计入）
		slots.emitBarriers(pass.barriers);
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
			const int32_t w = static_cast<int32_t>(extent.width);
			const int32_t h = static_cast<int32_t>(extent.height);
			// hpp ArrayWrapper1D 不接受 brace 整体赋值，逐元素填（同旧 DIAG 通路写法）
			blitRegion.srcOffsets[0] = vk::Offset3D{0, 0, 0};
			blitRegion.srcOffsets[1] = vk::Offset3D{w, h, 1};
			blitRegion.dstOffsets[0] = vk::Offset3D{0, 0, 0};
			blitRegion.dstOffsets[1] = vk::Offset3D{w, h, 1};
			::vkCmdBlitImage(*cmd, srcImage,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
				reinterpret_cast<const VkImageBlit*>(&blitRegion), VK_FILTER_LINEAR);
			gpuProfilerEnd(cmd);  // blit 无渲染段，pass 时间戳就地收口
			continue;
		}
		beginPassRendering(cmd, slots, pass, extent);
		// 动态状态在 draw 时采样（spec chapters/shaders.adoc "Setting State"），与
		// bindShadersEXT 先后无约束；段首各 applyDynamicState 一次（Phase 5 语义，
		// 状态对象经 pass.state 名解析——opaque/skybox/transparent 三成员不变）
		RGL::applyDynamicState(cmd, extent.width, extent.height,
			stateKit.stateFor(pass.state), stateKit.depthClipDynamicEnabled(),
			static_cast<uint32_t>(pass.colors.size()));
		// draw 三路分发（手术 6：提为 executePassDraws 私有方法——drawLight 全屏 /
		// opaque GPU-culling / 普通段逐 draw）
		executePassDraws(cmd, pass, list, frameSlot);
		cmd.endRendering();
		gpuProfilerEnd(cmd);  // Phase 9.2：pass 时间戳收口（render 段完整窗口）
	}

	// ⑪.5 帧内捕获（Phase 10 批次 0 手术 2：块体迁 FrameCapturer::record——后缓冲/
	// GB 通道两分支的布局追踪推进语义见 frame_capture.cppm 声明注释）。
	// acquire 超时跳帧的 return 在本段之前 = 请求保持 pending 至真正录制的帧。
	capturer.record(cmd, swapchainImage, extent,
		swapchain->getFormat().format, attachmentLayouts);

	// picking 回读（Phase 10 批 10.5）：请求帧录 object_id RT 1-texel → staging
	//（布局推进/未装饰图 warn/nullptr 早退语义见方法体与 picking.cppm）
	pickingRecord(cmd);

	// ⑪.6 帧末扩展段（Phase 10 批 10.1：imgui 调试面板；**空 hook = 零调用零 barrier，
	// 正常路径逐位不变**）。布局推进与 hook 调用体见 emitFrameEndOverlay。
	if (frameEndHook)
		emitFrameEndOverlay(cmd, swapchainImage, swapchainView, extent);

	// ⑫ swapchain → PresentSrcKHR（图 presentTransition——src 阶段/访问由最后
	// usage 推导：直渲 = CAO 写、blit 上屏 = Transfer 写，等价旧手工分支；
	// 无 swapchain 写入的离屏图 = nullopt 跳过）
	if (renderGraph.presentTransition)
		slots.emitBarriers(std::span<const RgBarrierPlan>{&*renderGraph.presentTransition, 1});

	gpuProfilerEnd(cmd);  // Phase 9.2：gpu.frame 收口（终点 eAllCommands = 全命令完成）

	// 录制完毕
	cmd.end();

	// ⑬ 提交（手术 4：块体迁 FrameSession::submit——waitStage = 图派生 swapchain
	// 首用阶段；DEVICE_LOST → Error 退出帧循环，其余失败留观测）
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
	if (auto r = session.present(); !r)
		return std::unexpected(std::move(r).error());
	session.advance();

	return {};
}

}  // namespace RGL
