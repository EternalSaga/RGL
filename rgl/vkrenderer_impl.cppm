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
import profiler;      // 批 9.5：writeSceneStaging 的 drawFrame 子段 zone
import diagnostics;
import vulkan_utils;
import VmaRaii;
import frame_draw_list;  // segmentSpan（段名 → draw 序列）
import pass_vocabulary;  // 词表单一真相源：kMeshPassStates / kFullscreenPassStates / kAllDrawSources
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
	// 词表 = 叶子模块 **pass_vocabulary**（单一真相源；本文件此前手抄过一份与
	// isKnownDrawSource 同义的本地 lambda，也自持一份 kKnownStates 字面量——两份真相
	// 并列，改一处漏一处）。state 名按 pass.kind **分档**判：drawMesh → meshPass 档、
	// drawLight → fullscreenPass 档。分档不可并成一张扁平表：并集会让 kind 与 state
	// 串味的 pass（如 drawLight 配 "opaque"）通过校验，而 drawLight 分支随后按 state 名
	// 去 registry 取预建全屏条目 → 放行 = 更深处崩溃或静默不画。
	// izb_mask 属 internal 档（图外硬编码录制路径直接 stateFor），两档都不认它。
	// 段名判 isKnownDrawSource；未知段名若不拦，segmentSpan 兜底分支返回 transparent 段
	// = 静默多画一遍透明体。
	auto listNames = [](const auto& names) {
		std::string out;
		for (const auto n : names) {
			if (!out.empty())
				out += " / ";
			out += n;
		}
		return out;
	};
	auto listSources = [] {
		std::string out;
		for (const auto& p : kAllDrawSources) {
			if (!out.empty())
				out += " / ";
			out += p.name;
			if (p.match == RgDrawSourceMatch::prefix)
				out += '*';  // 前缀档：标出 "shadow" 承 shadow0 / shadow1 / ...
		}
		return out;
	};
	// 单 pass 可解析性校验（三形态：blit 两端齐 / drawLight state 在词表 / draw state+
	// drawSource 名字表）。手术 6：提为 lambda 降宿主认知复杂度——fail 经捕获共用；
	// continue → return（lambda 内提前结束本 pass 校验语义等价）。
	auto validatePass = [&](const RgPassPlan& pass) {
		if (pass.kind == RgPassKind::blit) {
			if (!pass.blitSource || !pass.blitTarget)
				fail(fmt::format("blit pass '{}' missing source/target resolution", pass.name));
			return;
		}
		// Phase 7 D1 / Phase 8 ①泛化：drawLight = 固定全屏 draw（无 drawSource/段），
		// state 必在 fullscreenPass 档（同时是 registry 固定条目名）
		if (pass.kind == RgPassKind::drawLight) {
			if (!isFullscreenPassState(pass.state))
				fail(fmt::format(
					"render graph fullscreen pass '{}' has state '{}' (expected one of: {})",
					pass.name, pass.state, listNames(kFullscreenPassStates)));
			return;
		}
		// Phase 10 批 10.5/10.7：picking（id pass）/ highlight（高亮 pass）——两者都是
		// 克隆条目重放，StateKit 各有对应动态状态段；成员清单见 kMeshPassStates
		if (!isMeshPassState(pass.state))
			fail(fmt::format("render graph pass '{}' has unknown state '{}' (expected one of: {})",
				pass.name, pass.state, listNames(kMeshPassStates)));
		if (!isKnownDrawSource(pass.drawSource))
			fail(fmt::format(
				"render graph pass '{}' has unknown drawSource '{}' (expected one of: {})",
				pass.name, pass.drawSource, listSources()));
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
		RGL::GpuCull* vkGpuCull, RGL::DgcCommands* vkDgc, RGL::DgcStaticCommands* vkDgcStatic,
		bool fsrEnabled, bool fsrTemporal)
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
	  // RT generation 跨帧快照（批 12.6 T4）：初值 0 = 「还没见过任何 RT」，首帧 ensure 后
	  // 记下真实 generation，此后只在重建（+1）那一次才重置布局追踪
	  rtGenerations(this->renderGraph.attachments.size(), 0),
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
		// 批 14.5：FSR 门。在体内赋值而非初始化列表——成员声明序在 graphicsCommandPool
		// 之后，塞进列表要赌顺序（-Werror=reorder），体内赋值零风险且只此一次。
		this->fsrEnabled = fsrEnabled;
		this->fsrTemporal = fsrTemporal;
		// 批 14.5：FSR 在**构造期**建，不像 izb/evssm 那样首帧懒构造——tonemap 的色输入
		// 槽号是启动期烘进管线条目的，「槽已烘进条目却还没人写描述符」的窗口必须为零。
		if (fsrEnabled)
			initFsr();
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
					"pipeline entry (main must build it: addLightEntry / addAoEntry / "
					"addTonemapEntry, per state)",
					p.state));  // 原三元式只认 light/tonemap 两名，ao 缺条目会误报 Tonemap
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
				vkDevice.getDevice(), info, alloc, &allocData, "renderer/scene_staging");
			if (!result)
				throw std::runtime_error(fmt::format(
					"Renderer: scene staging alloc failed: {}", result.error().describe()));
			staging = std::move(*result);
			// 批 T0（§9.9 替代护栏）：表尾毒化哨兵元素**只在这里写一次**。之后 writeSceneStaging
			// 每帧只写各区有效段、永不碰哨兵字节，而每帧是 kSceneDataBytes 全量 vkCmdCopyBuffer
			// ⟹ 毒值随每次拷贝进 device buffer（GPU-AV 在本架构不可用，越界读只能靠这个变"必炸"）。
			writeSceneSentinels(
				std::span<std::byte> { static_cast<std::byte*>(staging.getMappedData()),
					kSceneDataBytes });
		}
	}
// （recreateSwapchainAndDepth 已随手术 4 迁 frame_session_impl.cppm）
 	/** drawFrame 定义（整体流程见接口声明 + 类头；逐步骤注记见体内 ①~⑮）：
	 * 双 fence wait/reset → acquire（100ms 超时跳帧）→ ③.5 scene staging 写
	 *（头 128B + 矩阵表 + 灯数组尾区）→ ⑥ heap barrier（3 dst flag）+ ⑥.5 scene 拷贝
	 *（kSceneDataBytes=263424B）→ ⑦ heap/sampler heap 绑定 → ⑤ 逐 pass（barrier 物化 /
	 * beginRendering / applyDynamicState / 段内逐 draw / blit / drawLight）→ ⑪.5 帧内
	 * 捕获回读 → ⑫ presentTransition → ⑬ submit（waitStage = 图派生）→ 捕获搬移 →
	 * ⑭ present（present fence）→ ⑮ 帧 slot 推进。 */
	std::expected<void, Error> Renderer::writeSceneStaging(const FrameDrawList& list,
		uint32_t frameSlot) {
	// ③.5 scene data 上传（Phase 5 批次 2；手术 5：块体提为私有方法，语义零改动）。
	auto z = zone("drawFrame.staging");  // 批 9.5 观测：scene data 逐字段 memcpy 进 mapped 块
	auto* stage = static_cast<std::byte*>(sceneStaging[frameSlot].getMappedData());
	// 头区（128B）：迁移批 S-5 起合并成契约 `SceneHeaderBlock`（binding 22 载体），**按生成
	// struct 整块写**——旧的五段 memcpy（light @0 / envParams @48 / exposure @56 /
	// aoStrength @60 / skyInvVP @64）删除，字段偏移只在反射生成物一处。
	// envParams 的 xy = env 两参数、z = tonemap 曝光、w = SSAO 强度（原本就是三个独立标量
	// 各自 memcpy 进同一段 16B 保留区，合并后字节形态逐位同：vec4 的 z/w 就是 @56/@60 两格）。
	// mat4 → ObjectMatrix 的**列直拷**与旧的「memcpy 64B」逐字节同（glm 是 col_major）。
	static_assert(sizeof(SceneHeaderBlock) == kSceneDataHeaderBytes
			&& sizeof(ObjectMatrix) == sizeof(glm::mat4),
		"SceneHeaderBlock 字节形态与头区容量不同源（列直拷前提失效）");
	const SceneHeaderBlock headerBlock {
		.light = list.sceneData.light,
		.envParams = glm::vec4 {list.sceneData.envParams.x, list.sceneData.envParams.y,
			list.sceneData.exposure, list.sceneData.aoStrength},
		.skyInvVP = ObjectMatrix {list.sceneData.skyInvViewProj[0],
		                          list.sceneData.skyInvViewProj[1],
		                          list.sceneData.skyInvViewProj[2],
		                          list.sceneData.skyInvViewProj[3]},
	};
	std::memcpy(stage + kSceneDataHeaderOffset, &headerBlock, sizeof(headerBlock));
	// Phase 11 批 11.2：阴影光空间 VP 区（尾区追加，4 槽）+ 采样参数子区（3 个 vec4）
	// 迁移批 S-1：读端已换 `ConstantBuffer<ShadowViewProjBlock>`（binding 28，元素 ObjectMatrix
	// 列主序），**写端逐字节不变**——源数组尺寸与区字节数由下面这条 assert 钉死
	//（区字节数又与反射生成物 sizeof(ShadowViewProjBlock) 同源，见 scene_data.cppm）。
	static_assert(sizeof(decltype(list.sceneData.shadowViewProj)) == kSceneDataShadowMatrixBytes,
		"SceneFrameData::shadowViewProj 字节数与阴影 VP 区容量失同步");
	std::memcpy(stage + kSceneDataShadowMatrixOffset, list.sceneData.shadowViewProj.data(),
		kSceneDataShadowMatrixBytes);
	// 迁移批 S-2：阴影采样参数（splits/texel/params/atlas/pcss/evssm/evssmConst）+ IZB 消费段
	// 合并成一个 `ShadowParamsBlock`（binding 29 载体），**按生成 struct 整块写**（同批 3 灯
	// 数组的做法）：字段偏移只存在于反射生成物一处，写端不再手抄 8 个基址常量（旧
	// kSceneDataShadow{Splits,Texel,Params,Atlas}Offset / Izb / Pcss / Evssm / EvssmConst 已删）。
	// IZB 段 (flag, W, H, 保留)：flag = izbShadow 已构造（门关恒空 = 全 0 → ×1.0 逐位旁路），
	// W/H = mask 位图行距——原本就是**无条件写**（写全 0 也写），合并后仍无条件，逐位同。
	const auto izbExtent = izbShadow ? izbShadow->getExtent() : vk::Extent2D {};
	const glm::vec4 izbParams = izbShadow
		? glm::vec4 {1.0f, static_cast<float>(izbExtent.width), static_cast<float>(izbExtent.height), 0.0f}
		: glm::vec4 {};
	const ShadowParamsBlock shadowBlock {
		.splits = list.sceneData.shadowSplits,
		.texelSizes = list.sceneData.shadowTexelSizes,
		.params = list.sceneData.shadowParams,
		.atlas = list.sceneData.shadowAtlas,
		.izb = izbParams,
		.pcss = list.sceneData.shadowPcss,
		.evssm = list.sceneData.shadowEvssm,
		.evssmConst = list.sceneData.shadowEvssmConst,  // x = c（kEvssmExponent 同源）
	};
	static_assert(sizeof(shadowBlock) == kSceneDataShadowParamsBytes,
		"ShadowParamsBlock 尺寸与区容量失同步（scene_data.cppm）");
	std::memcpy(stage + kSceneDataShadowParamsOffset, &shadowBlock, sizeof(shadowBlock));
	// 批 12.6 TAA 区：上一帧**抖动后** viewProj / 全场景刚体运动补偿 / 参数四元组。
	// 迁移批 S-4 起六段合并成契约 `TaaParamsBlock`（binding 30 载体），**按生成 struct 整块
	// 写**（同批 S-3 SSR 的做法）：旧的六个基址常量（kSceneDataTaa{,Motion,Params,
	// PrevContentShift,InputKernel,ClipControl}Offset）删除，字段偏移只在反射生成物一处。
	// 无条件写（192B）——TAA 关时前三段值本身无消费者（图里没有 taa pass），
	// 写它不改任何像素，且省下「按门分支写」的第二条路径（同 shadow 尾区纪律）。
	// 第四段（批 13.2 基空间输出偏移）**有**常驻消费者（tonemap 三条路线都在），但门关时
	// 值 = 全 0 ⟹ `uv + 0` 逐位 = `uv`，红线仍成立。
	// mat4 → ObjectMatrix 的**列直拷**与旧的「memcpy 64B」逐字节同（glm 是 col_major）。
	static_assert(sizeof(TaaParamsBlock) == sizeof(glm::mat4) * 2 + 4 * sizeof(glm::vec4),
		"TaaParamsBlock 字节形态与「2×mat4 + 4×vec4」不同源（契约 struct 出现 padding？）");
	const TaaParamsBlock taaBlock {
		.prevViewProj = ObjectMatrix {list.sceneData.taaPrevViewProj[0],
		                              list.sceneData.taaPrevViewProj[1],
		                              list.sceneData.taaPrevViewProj[2],
		                              list.sceneData.taaPrevViewProj[3]},
		.motion = ObjectMatrix {list.sceneData.taaMotion[0], list.sceneData.taaMotion[1],
		                        list.sceneData.taaMotion[2], list.sceneData.taaMotion[3]},
		.params = list.sceneData.taaParams,
		.prevContentShift = list.sceneData.taaPrevContentShift,
		.inputKernel = list.sceneData.taaInputKernel,
		.clipControl = list.sceneData.taaClipControl,
	};
	static_assert(sizeof(taaBlock) == kSceneDataTaaBytes,
		"TaaParamsBlock 尺寸与 TAA 区容量失同步（scene_data.cppm）");
	std::memcpy(stage + kSceneDataTaaOffset, &taaBlock, sizeof(taaBlock));
	// Phase 15 批 15.1 SSR 区：本帧**抖动后** proj·view（与 @64 skyInvViewProj 严格互逆）
	// + 参数 vec4 + 门控 vec4。迁移批 S-3 起三段合并成契约 `SsrParamsBlock`（binding 31
	// 载体），**按生成 struct 整块写**：旧的三个基址常量（kSceneDataSsr{,Params,Gate}Offset）
	// 删除，字段偏移只在反射生成物一处。无条件写（96B，交付时无消费者也不改像素，同 shadow/
	// TAA 尾区纪律：门关路径也保证值正确，省「按门分支写」的第二条路径）。
	// mat4 → ObjectMatrix 的**列直拷**与旧的「memcpy 64B」逐字节同（glm 是 col_major，
	// mat[i] = 第 i 列；下面第一条 assert 钉住两者同尺寸，viewProj 在偏移 0 由生成物 assert 钉）。
	static_assert(sizeof(ObjectMatrix) == sizeof(glm::mat4)
			&& sizeof(SsrParamsBlock) == sizeof(glm::mat4) + 2 * sizeof(glm::vec4),
		"ObjectMatrix 与 glm::mat4 字节形态不同源，或契约 struct 出现 padding（列直拷前提失效）");
	const SsrParamsBlock ssrBlock {
		.viewProj = ObjectMatrix {list.sceneData.ssrViewProj[0], list.sceneData.ssrViewProj[1],
		                          list.sceneData.ssrViewProj[2], list.sceneData.ssrViewProj[3]},
		.params = list.sceneData.ssrParams,
		.gate = list.sceneData.ssrGate,
	};
	static_assert(sizeof(ssrBlock) == kSceneDataSsrBytes,
		"SsrParamsBlock 尺寸与 SSR 区容量失同步（scene_data.cppm）");
	std::memcpy(stage + kSceneDataSsrOffset, &ssrBlock, sizeof(ssrBlock));
	// Phase 15 批 15.2 子步 A：湿面参数区（身份调制系数 + 空间变化形状参数，2×vec4）。
	// 无条件写（同 ssrParams/ssrGate 尾区纪律：门关路径也保证值正确，省第二条分支）。
	// 🔴 迁移批复补（scene data 收口后补做批 1 明确 defer 的**写侧半截**）：改成按契约
	// `WetParamsBlock` **整块写**，与其余 11 个区同形。旧写法是两次 memcpy，用
	// `kSceneDataWetSpatialOffset = 区首 + 16u` 这个**字面量 16** 表达 spatial 的位置，
	// 而同一事实的另一份在反射生成物 `offsetof(WetParamsBlock, spatial)` 里——两源之间
	// 当时**没有任何 assert 相连**（`scene_data.cppm` 旧注释声称的"双钉"第二钉实际不存在，
	// 那种"文档说有护栏"比"没护栏"更坏）。整块写之后字面量与子区常量一起消失，
	// 字段位置只剩契约一处；调换 shader 侧字段序 ⟹ 写端自动跟随，不再有读错成员的形态。
	static_assert(sizeof(WetParamsBlock) == kSceneDataWetBytes
		, "WetParamsBlock 字节形态与湿面区容量不同源（子范围描述符 range 会失同步）");
	const WetParamsBlock wetBlock {
		.params = list.sceneData.wetParams, .spatial = list.sceneData.wetSpatial };
	std::memcpy(stage + kSceneDataWetOffset, &wetBlock, sizeof(wetBlock));
	// 簇化轴 ★1-a1i：per-object 簇区间表（尾区 @724224，stride 16 × 4096）。表由装配层启动期
	// 扫产物 groups 得到、经 registry ctx 走到这里（生产者与索引口径 → SceneFrameData::clusterRanges）。
	// 写入本体在 scene_data::writeClusterRangeRegion（**无设备 ⟹ 可单测**：门关整区全 0 /
	// 有效段之外清零两条都在 tests/test_meshlet_ranges 里逐字节验）。
	// 🔴 本批零消费者（读取点归 ★1-a1 的 mesh shader），写它不改任何像素。
	if (!writeClusterRangeRegion(std::span<std::byte> {stage, kSceneDataBytes},
	                             list.sceneData.clusterRanges))
		return std::unexpected(Error {
			ErrorCode::Unknown,
			"scene clusterRanges exceeds capacity (same contract as objectDraws)" });
	// 批 11.3-1e 的 IZB 消费段（原独立 16B 区）已并入上面 ShadowParamsBlock（迁移批 S-2）。
	const auto matricesBytes = list.sceneData.objectMatrices.size() * sizeof(glm::mat4);
	if (matricesBytes > kSceneMatrixCapacity * kSceneMatrixStride)
		return std::unexpected(Error{ErrorCode::Unknown,
			"scene objectMatrices exceeds capacity (Phase 5 contract, main startup check bypassed)"});
	std::memcpy(stage + kSceneDataObjectMatrixOffset, list.sceneData.objectMatrices.data(),
		matricesBytes);
	// Phase 7 D3：多灯数组写尾区（迁移批 3：**按生成 struct 整块写** LightArrayBlock，
	// count@0 + pad + lights[8]@16 与旧 count@base + 数组@base+16 布局逐字节同；旧的两段
	// 字节 memcpy（含 +16 字面量）清零）。count 之外：lights 尾部 = lightArray 数组的零初始化
	// 条目、pad = 零（旧路径这些字节留 staging 残值——shader 只读 lights[i<count]，逐位不变，
	// 阳性对照与两档锚点实测）。
	if (list.sceneData.lightCount > kMaxLights)
		return std::unexpected(Error{ErrorCode::Unknown,
			"scene lightCount exceeds kMaxLights (lights_schema.lua contract)"});
	LightArrayBlock lightBlock{};
	lightBlock.count = list.sceneData.lightCount;
	lightBlock.lights = list.sceneData.lightArray;  // std::array<GpuLight, kMaxLights> 同型直拷
	std::memcpy(stage + kSceneDataLightArrayOffset, &lightBlock, sizeof(lightBlock));
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
	// 迁移批 2 起消费端 = scene.slang binding 27 的 IrradianceBlock；静态数据，随每帧 mirror
	// 拷入）。**按生成 struct 整块写**：尺寸取 sizeof(IrradianceBlock)（与区字节数的同步
	// 由 scene_data.cppm 的 static_assert 钉死），源 = SceneFrameData::irradianceSH
	// （std::array<glm::vec4, 9> 与生成字段同型同布局）。
	std::memcpy(stage + kSceneDataIrradianceOffset, list.sceneData.irradianceSH.data(),
		sizeof(IrradianceBlock));
	return {};
}

}  // namespace RGL
