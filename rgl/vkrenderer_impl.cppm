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
import frame_draw_list;  // isKnownDrawSource / segmentSpan（段名 → draw 序列）
import model_types_generated;
import lights_types_generated;
import material_types_generated;  // DrawArgs（GPU culling 对账经 objectIndex 映射 CPU draw）
import features;
import render_graph;

namespace RGL {

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
	// 阴影段名 = "shadow" + 分块号（scene_context::shadowDrawSource）。不列入本词表：
	// 分块号可变（kActiveShadowCascades 1→4），写死字面量会在切级联时漏校验。
	// 改用前缀匹配（与 segmentSpan 同一条路径）——两处必须同步，否则拓扑允许未知
	// drawSource 静默落到 transparent 段上（=静默多画一遍透明）。
	const auto isKnownDrawSource = [](std::string_view s) {
		return s == "opaque" || s == "skybox" || s == "transparent" || s.starts_with("shadow");
	};
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
		// state 必在 kFullscreenPassStates 词表（"light"/"ao"/"tonemap"）
		if (pass.kind == RgPassKind::drawLight) {
			if (!std::ranges::any_of(kFullscreenPassStates,
					[&](std::string_view s) { return pass.state == s; }))
				fail(fmt::format(
					"render graph fullscreen pass '{}' has state '{}' (expected one of the "
					"kFullscreenPassStates vocabulary)",
					pass.name, pass.state));
			return;
		}
		// Phase 10 批 10.5/10.7：picking（id pass）/ highlight（高亮 pass）——两者都是
		// 克隆条目重放，StateKit 各有对应动态状态段
		// 状态名词表（与 StateKit 键集合同步）：三成员 + picking / highlight（批 10.5 /
		// 10.7，克隆条目重放）+ shadow（批 11.2 基建，depth-only pass）
		static constexpr std::array<const char*, 6> kKnownStates{"opaque", "skybox",
			"transparent", "picking", "highlight", "shadow"};
		if (!std::ranges::any_of(kKnownStates,
				[&](const char* s) { return pass.state == s; }))
			fail(fmt::format("render graph pass '{}' has unknown state '{}'", pass.name,
				pass.state));
		if (!isKnownDrawSource(pass.drawSource))
			fail(fmt::format("render graph pass '{}' has unknown drawSource '{}'", pass.name,
				pass.drawSource));
	};
	for (const auto& pass : graph.passes)
		validatePass(pass);
}


// Renderer 重成员函数定义（2026-09-02 编译提速 A 方案）：ctor/drawFrame/重建路径/
// 同步对象/staging 全部移入本 partition implementation unit——接口 TU
// （vkrenderer.cppm）只留类声明，200+ 成员函数体的 AST 不再序列化进 BMI；
// 本单元是叶子（无人 import），编译完全并行、不上关键路径。
// 各函数语义/步骤注释随体迁自 vkrenderer.cppm。

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
	  // 双条件门（12.3d 就范于 issue_history 声明）：eds3 && depth_clip_enable base，
	  // 缺 base 时动态调 vkCmdSetDepthClipEnableEXT 违 VUID-07451（见 features.cppm）
	  stateKit(depthClipDynamicSupported(vkDevice.getOptionalFeatures()),
		conservativeModeDynamicSupported(vkDevice.getOptionalFeatures())),
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
	// Phase 11 批 11.4：SSAO 强度写 @60（头部保留区最后一格；tonemap.frag Load(60) 消费）
	std::memcpy(stage + kSceneDataAoOffset, &list.sceneData.aoStrength, sizeof(float));
	// Phase 5.5 批次 4：skyboxInvViewProj 写 @64（sky.slang skyClipToWorld 消费）
	std::memcpy(stage + kSceneDataSkyOffset, &list.sceneData.skyInvViewProj, sizeof(glm::mat4));
	// Phase 11 批 11.2：阴影光空间 VP 区（尾区追加，4 槽）+ 采样参数子区（3 个 vec4）
	std::memcpy(stage + kSceneDataShadowMatrixOffset, list.sceneData.shadowViewProj.data(),
		kSceneDataShadowMatrixBytes);
	std::memcpy(stage + kSceneDataShadowSplitsOffset, &list.sceneData.shadowSplits,
		sizeof(glm::vec4));
	std::memcpy(stage + kSceneDataShadowTexelOffset, &list.sceneData.shadowTexelSizes,
		sizeof(glm::vec4));
	std::memcpy(stage + kSceneDataShadowParamsOffset, &list.sceneData.shadowParams,
		sizeof(glm::vec4));
	std::memcpy(stage + kSceneDataShadowAtlasOffset, &list.sceneData.shadowAtlas,
		sizeof(glm::vec4));
	// 批 11.5：PCSS 参数区（fillShadowData 填，-DPCSS_SOFT 变体消费；原版不引用）
	std::memcpy(stage + kSceneDataPcssOffset, &list.sceneData.shadowPcss, sizeof(glm::vec4));
	// 批 11.3-1e：IZB 消费区 (flag, W, H, 保留)——light.frag izbNearShadow 消费。
	// flag = izbShadow 已构造（门关恒空 = 全 0 → ×1.0 逐位旁路）；W/H = mask 位图行距
	{
		const auto ext = izbShadow ? izbShadow->getExtent() : vk::Extent2D{};
		const std::array<float, 4> izbRegion = izbShadow
			? std::array{1.0f, static_cast<float>(ext.width), static_cast<float>(ext.height), 0.0f}
			: std::array{0.0f, 0.0f, 0.0f, 0.0f};
		std::memcpy(stage + kSceneDataIzbOffset, izbRegion.data(), sizeof(izbRegion));
	}
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

}  // namespace RGL
