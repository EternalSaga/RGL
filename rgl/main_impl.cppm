module;
#include <entt/entt.hpp>
#include <SDL3/SDL_video.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // entity rootTransform（translate/rotate/scale 组装）
#include <fmt/format.h>
#include <filesystem>  // environment.hdr 本地素材存在性探测（GMF 例外清单成员）
#ifdef RGL_AFTERMATH  // 编译期开关（rgl/CMakeLists.txt option）：NVIDIA GPU crash dump 诊断
#include "GFSDK_Aftermath_GpuCrashDump.h"
#include "GFSDK_Aftermath_GpuCrashDumpDecoding.h"
#endif

// 入口模块 main 的分区实现单元（module main:impl;）——2026-09-14 由 main.cpp 转来，
// 纳入 stylecheck/doccheck 全规则管理。main() 合法于分区实现单元（标准仅禁模块接口
// 单元定义 main）；CMake FILE_SET 不收纯实现单元，主接口 = main.cppm 的声明单元。
// GMF 只放第三方头（entt/SDL/glm/fmt/filesystem + Aftermath SDK）；STL 走 import rlstd。
module main:impl;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;  // vk::raii::CommandBuffer 等（imgui hook lambda 形参类型）
import vulkan_window;
import vk_instance;
import vk_device;
import vk_swapchain;
import vk_resources;
import scene_data;  // SceneDataService（架构债务续账步骤 1）
import sampler_library;  // SamplerLibrary（架构债务续账步骤 4a/4b）
import environment;  // Environment（架构债务续账步骤 3）
import render_target_cache;  // RenderTargetCache（架构债务续账步骤 2）
import asset_table;
import shader_registry;
import vk_renderer;
import frame_draw_list;
import render_graph;
import render_graph_config;
import scene_config;
import gltf_loader;
import model_importer;
import texture_pool;
import scene_context;
import builtin_geometry;
import gpu_cull;  // Phase 7b：GPU culling 子系统（C3 候选表构建 + 构造，先于 Renderer）
import vk_indirect_commands;  // Phase 7b：DGC 命令生成（NVIDIA 路，管线表定后条件构造）
import material_types_generated;  // DrawArgs（push constant 尺寸契约检查）
import camera;
import camera_config;
import runtime_config;  // 2026-09-08 env→配置：runtime.lua（headless/窗口/llm 截图）
import diagnostics;      // diagnostics.lua 诊断单例（须先于一切 GPU 对象构造 setDiagnostics）
import profiler;         // Phase 9：帧循环 zone 收口 dump（docs/phase9_plan.md 批 9.1）
import gpu_profiler;     // Phase 9.2：GPU 时间戳会话（main 栈持有 = device 生命周期内）
import llm_debug;
import imgui_layer;  // Phase 10 批 10.1：IMGUI 调试面板层（main 装配 = 唯一认识 imgui 的地方之一）
import param_registry;  // DebugPanelState ctx（面板跨帧 UI 态，10.2c）
import ecs;
import rllogger;
import vkerror;
using namespace RGL;

#ifdef RGL_AFTERMATH  // 编译期开关：回调体在模块 purview（std::FILE/vector 经 import rlstd）
namespace {
// Aftermath GPU crash dump 回调：崩溃时驱动回调本函数，落盘 .bin + 解码 .json（供离线分析）。
// 须早于任何 device 创建启用（见 main 顶部 EnableGpuCrashDumps）。
void rglAftermathDumpCb(const void* data, uint32_t size, void*) {
	std::FILE* f = std::fopen("aftermath_crash.bin", "wb");
	if (f) { std::fwrite(data, 1, size, f); std::fclose(f); }
	std::cerr << "[aftermath] crash dump " << size << " bytes\n";
	GFSDK_AFTERMATH_GpuCrashDump_Decoder dec = nullptr;
	if (GFSDK_AFTERMATH_SUCCEED(GFSDK_AFTERMATH_GpuCrashDump_CreateDecoder(
		    GFSDK_AFTERMATH_Version_API, data, size, &dec))) {
		uint32_t jsonSize = 0;
		GFSDK_AFTERMATH_GpuCrashDump_GenerateJSON(dec, 0x3fff, 2,
			nullptr, nullptr, nullptr, nullptr, &jsonSize);
		if (jsonSize > 0) {
			std::vector<char> buf(jsonSize);
			GFSDK_AFTERMATH_GpuCrashDump_GetJSON(dec, jsonSize, buf.data());
			std::FILE* jf = std::fopen("aftermath_crash.json", "wb");
			if (jf) { std::fwrite(buf.data(), 1, jsonSize, jf); std::fclose(jf); }
		}
		GFSDK_AFTERMATH_GpuCrashDump_DestroyDecoder(dec);
	}
}
}  // namespace
#endif

namespace RGL {

/** 四配置加载（错误日志在内）+ setDiagnostics（先于一切 GPU 对象构造）。 */
std::optional<StartupConfigs> loadAllConfigs() {
	auto cameraResult = loadCameraConfig("configs/camera.lua");
	if (!cameraResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", cameraResult.error().describe());
		return std::nullopt;
	}
	auto sceneResult = loadSceneConfig("configs/scene.lua");
	if (!sceneResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", sceneResult.error().describe());
		return std::nullopt;
	}
	// 2026-09-08 env→配置：runtime.lua（运行/窗口模式）+ diagnostics.lua（诊断开关单例）
	auto runtimeResult = loadRuntimeConfig("configs/runtime.lua");
	if (!runtimeResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", runtimeResult.error().describe());
		return std::nullopt;
	}
	auto diagResult = loadDiagnosticsConfig("configs/diagnostics.lua");
	if (!diagResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", diagResult.error().describe());
		return std::nullopt;
	}
	setDiagnostics(*diagResult);
	return StartupConfigs{std::move(*cameraResult), std::move(*sceneResult),
		std::move(*runtimeResult)};
}

/** 渲染图编译器自检（失败项逐条日志；false = 启动期报错退出）。 */
bool checkRenderGraphSelfCheck() {
	auto failures = renderGraphSelfCheck();
	if (failures.empty())
		return true;
	auto* logger = RLLogger::getInstance();
	for (const auto& f : failures)
		if (logger) logger->error("RenderGraph self-check: {}", f);
	return false;
}

/** 建交互窗（headless = nullptr + 日志；llm 按 llm_shot.size 隐藏窗；建窗失败抛错）。 */
std::unique_ptr<SDLWindow> createAppWindow(const RuntimeConfig& runtime, bool llmEnabled,
										   bool headless, uint32_t headlessW, uint32_t headlessH) {
	if (headless) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->info("headless 模式：脱离 SDL/compositor（{}x{}）", headlessW, headlessH);
		return nullptr;
	}
	// 交互窗尺寸 win_size={W,H}（默认 800x600；llm 模式用 llm_shot.size，headless 用
	// headless_size）；show_after = DIAG（原 RGL_SHOW_AFTER env）：隐藏窗启动、第 N 帧后
	// 显示（可见窗 fault 排查；-1 = 立即显示）
	const int winW = static_cast<int>(runtime.winWidth);
	const int winH = static_cast<int>(runtime.winHeight);
	const int showAfter = runtime.showAfter;
	auto windowResult = SDLWindow::create(
		llmEnabled ? static_cast<int>(runtime.llm.width) : winW,
		llmEnabled ? static_cast<int>(runtime.llm.height) : winH,
		"RGL", llmEnabled || showAfter >= 0);
	if (!windowResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", windowResult.error().describe());
		throw std::runtime_error(windowResult.error().describe());
	}
	if (llmEnabled) {
		auto* logger = RLLogger::getInstance();
		if (logger)
			logger->info("llm_debug: 一次性截图模式（延迟 {} 帧后截图并退出）",
				runtime.llm.delayFrames);
	}
	return std::make_unique<SDLWindow>(std::move(*windowResult));
}

/** 尺寸契约求值 + 建窗 + 派生量（自检失败 → 空 optional，错误已日志）。 */
std::optional<PreWindow> prepareWindow(const StartupConfigs& configs) {
	const auto& runtime = configs.runtime;
	// Phase 6 批次 2：渲染图编译器自检（无 GPU，纯 CPU 图算法回归守卫）——失败 =
	// 编译器/默认图被改坏，启动期报错退出（早于建窗，headless/llm 路径同样被覆盖）
	if (!checkRenderGraphSelfCheck())
		return std::nullopt;
	PreWindow pre;
	pre.llmEnabled = runtime.llm.enabled;
	// headless（VK_EXT_headless_surface）：脱离 SDL/compositor——runtime.lua
	// headless=true 时不建 SDL 窗口（离屏渲染/CI 截图，无需显示）。尺寸
	// headless_size={W,H}（默认 1280x720）
	pre.headless = runtime.headless;
	pre.headlessW = runtime.headlessWidth;
	pre.headlessH = runtime.headlessHeight;
	// 截图尺寸契约（2026-09-08 帧内回读：截图 = swapchain extent）：headless+llm 时
	// headless_size 对齐 llm_shot.size——与非 headless llm 按 llm 尺寸建窗同语义
	if (pre.headless && pre.llmEnabled) {
		pre.headlessW = runtime.llm.width;
		pre.headlessH = runtime.llm.height;
	}
	pre.showAfter = runtime.showAfter;
	pre.diagShow = pre.showAfter >= 0 && !pre.headless;
	pre.window = createAppWindow(runtime, pre.llmEnabled, pre.headless,
		pre.headlessW, pre.headlessH);
	pre.extentW = pre.headless ? pre.headlessW : static_cast<uint32_t>(pre.window->getWidth());
	pre.extentH = pre.headless ? pre.headlessH : static_cast<uint32_t>(pre.window->getHeight());
	pre.llm = pre.llmEnabled ? &runtime.llm : nullptr;
	return pre;
}

/** push data 容量契约检查（超限抛 std::runtime_error）。 */
void checkPushDataLimits(const DeviceContext& device) {
	// push data 契约（Phase 5）：shader object + descriptor heap 模式无 pipeline layout →
	// 无 VkPushConstantRange，per-draw push data（DrawArgs 4B——Phase 7b+ 仅 objectIndex，
	// vkCmdPushDataEXT，vkrenderer.cppm drawFrame）受设备硬限约束（双驱动实测 256B）：
	// ① maxPushDataSize（descriptor heap 模式专属 limit，VUID-VkPushDataInfoEXT-offset-11243：
	//    offset+size ≤ 本值）② maxPushConstantsSize（经典 VkPhysicalDeviceLimits，
	//    descriptor set 模式上限）。超限 = 设备不支持，启动期报错优于运行时布局错配
	//（shader 从 push constant 读垃圾非校验错误，是静默画面错）
	const auto [maxPushConstants, maxPushData] = device.getPushDataLimits();
	if (maxPushConstants < sizeof(DrawArgs) || maxPushData < sizeof(DrawArgs))
		throw std::runtime_error(fmt::format(
			"push data limit (maxPushConstantsSize={}, maxPushDataSize={}) < sizeof(DrawArgs) ({})",
			maxPushConstants, maxPushData, sizeof(DrawArgs)));
}

/** Phase 5.5 IBL 风格契约检查（违约抛 std::runtime_error）。 */
void validateEnvironmentContract(const SceneConfig& scene) {
	if (!scene.environment.enabled) {
		std::string iblStyles;
		for (const auto& [style, specName] : scene.styleMap)
			if (specName == "model_pbr_ibl" || specName == "model_vegetation"
				|| specName == "model_light")
				iblStyles += (iblStyles.empty() ? "" : ", ") + style;
		if (!iblStyles.empty())
			throw std::runtime_error(fmt::format(
				"styles [{}] reference model_pbr_ibl but scene.lua 'environment' is disabled "
				"(env descriptor unmapped = undefined sampling)", iblStyles));
	}
}

/** 渲染图配置加载 + 编译（错误抛 std::runtime_error）+ deferredMode 求值。 */
CompiledGraphResult compileGraphOrThrow(const char* path) {
	auto graphConfig = loadRenderGraphConfig(path);
	if (!graphConfig)
		throw std::runtime_error(graphConfig.error().describe());
	bool deferredMode = graphConfig->variant == RgGraphVariant::deferred;
	auto compiledGraph = compileRenderGraph(graphConfig->desc);
	if (!compiledGraph)
		throw std::runtime_error(fmt::format("render graph compile failed: {}",
			compiledGraph.error().describe()));
	return CompiledGraphResult{std::move(*compiledGraph), deferredMode};
}

}  // namespace RGL

/**
 * 启动流主函数（逐段契约见体内块注释；全景对照 AGENTS.md "main 启动流" 节 /
 * docs/技术概念.md §main 启动流）：
 * 加载四配置（camera/scene/runtime/diagnostics；setDiagnostics 必须先于一切 GPU
 * 对象构造）→ renderGraphSelfCheck（纯 CPU 图算法回归守卫）→ 解析 headless/llm
 * 尺寸（headless+llm 时 headless_size 对齐 llm_shot.size）→ 建窗（headless 不建
 * SDL 窗，surface 走 vkCreateHeadlessSurfaceEXT）→ try 内：entt::registry（全部
 * 每帧 context）→ 初始化层 AppContext→DeviceContext（push data 容量契约检查）→
 * Swapchain→Resources→ShaderRegistry → 渲染图配置加载 + compileRenderGraph →
 * IBL 风格契约检查 → 资产四段（A0 内建 cube → A1 解析+顶点 → A2 纹理 → env
 * 上传（全部纹理槽后、管线烘焙前）→ C1 管线条目烘焙 → C2 实体注册 + 矩阵容量
 * 契约 → C3+deferred+DGC+Renderer 装配）→ renderSystem（llm 截图回调 + onFrame
 * DIAG 回调）。
 *
 * 长生命周期对象全部 RAII（按声明逆序析构；RenderBackEnd 内部序见其注释）；
 * 任何启动期错误 = 日志 + return -1。
 * @return 0 = 正常结束（含 llm 截图完成后退出）；-1 = 配置/自检/建窗/图编译/
 *   资产上传/渲染错误
 */
int main() {
#ifdef RGL_AFTERMATH  // 编译期开关：须早于任何 device 创建（Aftermath 文档要求）
	GFSDK_AFTERMATH_EnableGpuCrashDumps(GFSDK_AFTERMATH_Version_API,
		GFSDK_AFTERMATH_GpuCrashDumpWatchedApiFlags_Vulkan,
		GFSDK_AFTERMATH_GpuCrashDumpFeatureFlags_Default,
		rglAftermathDumpCb, nullptr, nullptr, nullptr, nullptr);
#endif
	// 配置先于窗口：llm_debug 模式影响建窗（离屏隐藏窗 + 配置尺寸）
	auto configsOpt = loadAllConfigs();
	if (!configsOpt)
		return -1;
	auto& configs = *configsOpt;
	// 自检 + 尺寸契约 + 建窗 + 派生量（契约细节见 prepareWindow 注释）
	auto preOpt = prepareWindow(configs);
	if (!preOpt)
		return -1;
	auto& pre = *preOpt;

	try {
		entt::registry registry;
		// 窗口/应用状态（eventSystem 写）+ 每帧 system 的单例数据（context）：
		// WindowExtent/AppExit（ecs.cppm）、SceneSpin/SceneBounds/ViewProjection（scene.cppm）、
		// FrameDrawList（vkrenderer.cppm，渲染 system 产出、drawFrame 消费）、
		// InputEvents/CameraState（camera.cppm，输入事件流 + 相机自管 level 状态）
		registry.ctx().emplace<WindowExtent>(pre.extentW, pre.extentH);
		registry.ctx().emplace<AppExit>();
		registry.ctx().emplace<Camera>(std::move(configs.camera.camera));
		registry.ctx().emplace<InputEvents>();
		registry.ctx().emplace<InputFilter>();  // Phase 10 调试层输入先手过滤器（imgui 装配；缺省空 = 不过滤）
		registry.ctx().emplace<DebugPanelState>();  // 面板跨帧 UI 态（材质选择器等，10.2c）
		registry.ctx().emplace<CameraState>();
		auto& spin = registry.ctx().emplace<SceneSpin>();
		spin.speed = configs.scene.spin ? 1.0f : 0.0f;  // 0 = 静止（configs/scene.lua 的 spin 开关）
		registry.ctx().emplace<SceneBounds>();
		registry.ctx().emplace<ViewProjection>();
		registry.ctx().emplace<FrameDrawList>();
		registry.ctx().emplace<NextObjectSlot>();  // Phase 6 instancing：实例矩阵槽分配器
		registry.ctx().emplace<SceneEnv>();  // Phase 5.5：环境参数（env 上传成功后回填，见下）
		registry.ctx().get<SceneEnv>().exposure = configs.scene.exposure;  // Phase 8 收尾：tonemap 曝光（与 env 开关无关）

		AppContext vkInstance(pre.window.get(), pre.headless);
		DeviceContext vkDevice(vkInstance);
		checkPushDataLimits(vkDevice);
		Swapchain vkSwapchain(vkDevice, pre.window.get(), pre.headless,
			pre.headlessW, pre.headlessH, configs.runtime.presentMode);
		// heap 容量为固定裕量（与场景内容解耦，批次 3 E1.3）：CPU 解析模型不再需要
		// 先于 Vulkan 初始化
		Resources vkResources(vkInstance, vkDevice, vkSwapchain);
		// SceneData 服务（架构债务续账步骤 1：buffer/槽自 Resources 拆出，构造紧随内核）
		SceneDataService sceneData(vkResources, vkDevice);
		// 内容服务层（架构债务续账步骤 3/4a/4b：共享 sampler+mip-gen 供给 / env-IBL 拆自内核）
		SamplerLibrary samplers(vkResources, vkDevice);
		Environment env(vkResources, vkDevice, samplers);
		RenderTargetCache rtCache(vkResources, vkDevice);
		// 资产表（ECS 规范 5 单一数据源；架构债务续账 F：所有权自 Resources 移入装配层，
		// 声明序在 Renderer（backEnd）之前 = 析构序其后，表内 GPU 资源不在渲染器存活期释放）
		AssetTable assetTable;
		// 管线表（Phase 4 批次 2：ShaderRegistry，零 entt）：规格来自 configs/scene.lua；
		// 条目全按需建（画不画 = entity 有无，数据驱动）——模型条目 C1 期 addModelEntry、
		// 内建 cube 条目 cube 资产上传后 addCubeEntry、天空盒/光源条目各自前提满足时建。
		// 先于 Renderer 构造（Renderer 持引用）
		ShaderRegistry shaderRegistry(vkDevice, vkResources, sceneData, env, assetTable, configs.scene.pipelines);
		// Phase 6 批次 3：渲染意图图 = configs/render_graph.lua（缺文件/缺段回落默认
		// 三段图）；compileRenderGraph 校验 + barrier 布局链推导，任何图错误启动期
		// 报错退出（Renderer 构造校验器兜底执行器可解析性）。
		// Phase 7 路线选路（2026-09-08 env→配置）：顶层 variant = "forward"(默认)/
		// "deferred"/"offscreen"——后两者为内建 exclusive 图变体（覆盖文件 passes）
		auto graphResult = compileGraphOrThrow("configs/render_graph.lua");
		const bool deferredMode = graphResult.deferredMode;
		// 图来源/规模观测（排"配置没生效"类问题：pass 数与 configs/render_graph.lua 对账）
		if (auto* logger = RLLogger::getInstance())
			logger->info("RenderGraph: {} attachments / {} passes compiled",
				graphResult.graph.attachments.size(), graphResult.graph.passes.size());
		// Renderer 构造下移至 deferred G-buffer RT / light 管线就绪之后（deferred 模式需在构造前
		// addLightEntry 供 Renderer 取 light 管线下标）；compiledGraph 此时保持存活（下方 move 进 Renderer）

		// Phase 5.5 契约：引用 ibl/植被/光源风格必须配 environment（检查语义见
		// validateEnvironmentContract 接口注释）
		validateEnvironmentContract(configs.scene);

		// CPU 解析 + GPU 上传 + 纹理 + ECS 注册：资产数据（buffer/slot/三角形数/管线条目引用、
		// 纹理资产、材质资产）全部在 Resources 资产表（初始化层 owner，生命周期不依赖
		// main 作用域——TexturePool 局部对象 UAF 教训，docs/archive/heap_texture_research.md §12.6/§12.7）
		// Phase 6 批次 5：scene.lua entities 列表 = 场景构成数据源（画不画 = entity 有无，
		// ECS 规范 3；缺段兼容合成见 scene_config loader）。三段时序：A 解析 + GPU 上传
		// 全部模型（顶点/纹理槽按段分配）→ B env 上传（**调用序契约 = 管线烘焙前**，
		// env 纹理仍走纹理段，白纹理首槽由段首固定校验）→ C 管线烘焙
		// + ECS 注册。CPU 解析产物（GltfModel）活到 C（unique_ptr 稳定地址）；资产数据
		// （buffer/slot/纹理/材质）全部在 Resources 资产表（初始化层 owner——TexturePool
		// 局部对象 UAF 教训，docs/archive/heap_texture_research.md §12.6/§12.7）
		auto assets = uploadModelAssets(vkDevice, vkResources, samplers, assetTable, configs.scene);
		if (configs.scene.environment.enabled)
			uploadEnvironment(vkResources, env, vkDevice, configs.scene, registry);  // 时序契约见定义处注释

		// C1：管线条目烘焙（per **unique 模型**——instancing 共享条目；addModelEntry
		// 按 (规格,顶点 slot,纹理 slot) 组合去重兜底）
		bakePipelineEntries(assets, configs.scene, deferredMode, shaderRegistry, vkResources, samplers, assetTable);
		// C2/C2b：场景实体注册（模型/cube/灯）+ 实例矩阵槽容量契约
		registerSceneEntities(registry, assets, configs.scene, vkResources, assetTable, shaderRegistry);

		// C3 GPU culling 候选表 + GpuCull + skybox + deferred G-buffer/HDR + DGC + Renderer
		//（装配时序契约见 assembleBackEnd 接口注释；graph 在此 move 进 Renderer）
		auto backEnd = assembleBackEnd(vkDevice, vkSwapchain, vkResources, sceneData, samplers, env, rtCache, assetTable, shaderRegistry,
			std::move(graphResult.graph), registry, configs.scene, deferredMode);

		// Phase 9.2：GPU 时间戳会话（声明序在 backEnd/vkDevice 后 = 析构序其先，
		// query pool 不越过 device 生命周期——GPU 资源所有权规则；profiler=0 /
		// 校准扩展缺位 = 内部不建 pool，钩子全早退）
		GpuProfiler gpuProfiler(vkDevice);
		bindGpuProfiler(&gpuProfiler);

		// Phase 10 批 10.1：IMGUI 调试面板（docs/phase10_plan.md §4）。装配门 = 有窗 &&
		// diagnostics.imgui（headless 天然关）；声明序在 backEnd 之后 = 析构序其先
		//（后端 shutdown 需 device/swapchain 存活）。init 失败 = 日志 + 无 UI 继续（降级不崩）。
		// 挂载点两处：InputFilter.consume（事件先手，WantCapture 门）+ Renderer 帧末扩展段
		//（UI 渲染段，布局推进由执行器负责，契约见 setFrameEndHook 注释）。
		RGL::ImGuiLayer imguiLayer;
		if (!pre.headless && RGL::diagnostics().imgui && pre.window
			&& imguiLayer.init(vkInstance, vkDevice, vkSwapchain, *pre.window)) {
			registry.ctx().get<InputFilter>().consume = [layer = &imguiLayer](const void* e) {
				return layer->handleEvent(e);
			};
			backEnd.renderer.setFrameEndHook([layer = &imguiLayer](
				vk::raii::CommandBuffer& cmd, vk::Extent2D extent, vk::ImageView view) {
				return layer->renderFrame(cmd, extent, view);
			});
			// 参数页构建器（10.2b/c）：imgui 层不认识 ECS，页 = buildDebugParamPages(registry, 资产表)
			imguiLayer.setPageBuilder([&registry, &assetTable] {
				return buildDebugParamPages(registry, assetTable);
			});
		}

		/**
		 * DIAG（可见窗 fault 排查）每帧回调：隐藏窗启动、frame == showAfter 时
		 * SDL_ShowWindow（恰好一次）——若显示瞬间开始 fault = present/合成期状态
		 * 触发；若全程不 fault = 可见性非根因。headless 或 showAfter < 0 时为空
		 * 回调（正常路径零变化）。
		 */
		std::function<void(uint32_t)> onFrame;
		if (pre.diagShow) {
			SDL_Window* sdlWindow = *pre.window;
			onFrame = [sdlWindow, showAfter = pre.showAfter](uint32_t frame) {
				if (static_cast<int>(frame) == showAfter) {
					auto* logger = RLLogger::getInstance();
					if (logger) logger->info("DIAG: frame {} → SDL_ShowWindow", frame);
					SDL_ShowWindow(sdlWindow);
				}
			};
		}
		// llm 截图消费回调（ecs 不 import llm_debug——依赖图叶子解耦，2026-09-02）：
		// main 是唯一知道 writeCapturePng 的地方，config 指针绑进 lambda。截图内容 =
		// drawFrame 帧内回读的本帧真实输出（vkrenderer FrameCapture，2026-09-08 离屏重渲退役）
		auto renderResult = RGL::renderSystem(registry, backEnd.renderer, assetTable,
			pre.llm,
			[&](RGL::Renderer& r) {
				return RGL::writeCapturePng(r, *pre.llm);
			},
			onFrame);
		RGL::profilerDump();  // Phase 9：trace/stats/摘要收口（profiler=0 = 静默 no-op）
		bindGpuProfiler(nullptr);  // Phase 9.2：gpuProfiler 析构前解绑（帧循环已结束）
		if (!renderResult) {
			auto* logger = RLLogger::getInstance();
			if (logger) logger->error("错误: {}", renderResult.error().describe());
			return -1;
		}
	} catch (const std::exception& e) {
		RGL::profilerDump();  // 异常路径也收口（RAII 已展开，帧内记录完整）
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", e.what());
		return -1;
	}
	return 0;
}
