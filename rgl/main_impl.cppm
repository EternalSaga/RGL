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
import shader;  // 批 9.5：shaderObjectStats()（条目/句柄/创建耗时启动基线）
import vk_renderer;
import frame_draw_list;
import render_graph;
import render_graph_config;
import gate_lint;  // 门组合自检（启动期一次，docs/gate_lint_plan.md §3-C）
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
import shadow_config;    // 批 N6a：configs/shadow.lua（阴影路线枚举）——本批**只回显不回灌**
import profiler;         // Phase 9：帧循环 zone 收口 dump（docs/phase9_plan.md 批 9.1）
import gpu_profiler;     // Phase 9.2：GPU 时间戳会话（main 栈持有 = device 生命周期内）
import llm_debug;
import imgui_layer;  // Phase 10 批 10.1：IMGUI 调试面板层（main 装配 = 唯一认识 imgui 的地方之一）
import picking;  // PickingService（Phase 10 批 10.5 装配）
import debug_rpc;       // DebugRpcServer（Phase 10b 批 11b.2 传输层：帧循环后显式 stop）
import debug_rpc_exec;  // wireDebugRpc（11b.2 执行层 + 装配，门 = diagnostics.rpc 默认关）
import param_registry;  // DebugPanelState ctx（面板跨帧 UI 态，10.2c）
import param_pages;  // buildDebugParamPages（参数页构建器，批 10.6 自 ecs 拆出）
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

/** 配置目录（RGL_CONFIG_DIR env 覆盖；契约见 main.cppm configDir 注释）。 */
std::string configDir() {
	const char* dir = std::getenv("RGL_CONFIG_DIR");  // std::getenv 经 import rlstd（rlstd.cppm 已导出）
	return (dir && *dir) ? std::string(dir) : std::string("configs");
}

/** 配置文件路径拼接（configDir + 尾分隔符按需 + 文件名）。 */
std::string configPath(const char* fileName) {
	std::string dir = configDir();
	if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
		dir.push_back('/');
	dir += fileName;
	return dir;
}

/** 启动配置加载（错误日志在内）+ setDiagnostics（先于一切 GPU 对象构造）。 */
std::optional<StartupConfigs> loadAllConfigs() {
	auto cameraResult = loadCameraConfig(configPath("camera.lua").c_str());
	if (!cameraResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", cameraResult.error().describe());
		return std::nullopt;
	}
	auto sceneResult = loadSceneConfig(configPath("scene.lua").c_str());
	if (!sceneResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", sceneResult.error().describe());
		return std::nullopt;
	}
	// 2026-09-08 env→配置：runtime.lua（运行/窗口模式）+ diagnostics.lua（诊断开关单例）
	auto runtimeResult = loadRuntimeConfig(configPath("runtime.lua").c_str());
	if (!runtimeResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", runtimeResult.error().describe());
		return std::nullopt;
	}
	auto diagResult = loadDiagnosticsConfig(configPath("diagnostics.lua").c_str());
	if (!diagResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", diagResult.error().describe());
		return std::nullopt;
	}
	setDiagnostics(*diagResult);
	// 批 N6a（docs/shadow_routes_plan.md §3）建文件、**N6b 回灌**：configs/shadow.lua 的
	// 阴影路线枚举（枚举 mode + 正交 izb）。setShadowConfig 之后 6 处消费点经
	// shadowConfig() / shadowFormPlan() 读它；diagnostics 侧九个旧字段已删。
	// 失败口径与其余配置一致 = 启动失败。
	auto shadowResult = loadShadowConfig(configPath("shadow.lua").c_str());
	if (!shadowResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", shadowResult.error().describe());
		return std::nullopt;
	}
	setShadowConfig(*shadowResult);
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
	// WindowExtent 初值口径 = 窗口**物理像素**（与 swapchain extent 同口径——不同则首帧
	// resizeSystem 误判不匹配、recreate 又得同值返回 false = 假 "clamped" 告警）。
	// 像素查询失败（0x0）回落创建尺寸；headless 无窗口用配置尺寸
	if (pre.headless) {
		pre.extentW = pre.headlessW;
		pre.extentH = pre.headlessH;
	} else {
		auto px = pre.window->pixelSize();
		if (px[0] <= 0 || px[1] <= 0)
			px = {pre.window->getWidth(), pre.window->getHeight()};
		pre.extentW = static_cast<uint32_t>(px[0]);
		pre.extentH = static_cast<uint32_t>(px[1]);
	}
	pre.llm = pre.llmEnabled ? &runtime.llm : nullptr;
	return pre;
}

/**
 * 窗口相关 registry context 装配（main 启动流，替代原 emplace 行以控 main 行数）：
 * ① WindowExtent 初值 = prepareWindow 实测的窗口**物理像素**尺寸（headless = 配置尺寸）；
 * ② WindowPixelQuery 查询钩子（eventSystem resize 重查 extent 用；headless 窗口为空 =
 *   不装配 = 兜底走事件值，且 headless 恒无 SDL 窗口事件。像素口径理由 → ecs.cppm）。
 * @param rg 场景 registry（写入 context）
 * @param pre 建窗派生量（extent 初值 + 窗口指针，headless 窗口为空）
 */
void wireWindowContexts(entt::registry& rg, const PreWindow& pre) {
	rg.ctx().emplace<WindowExtent>(pre.extentW, pre.extentH);
	if (!pre.window)
		return;
	SDLWindow* window = pre.window.get();
	rg.ctx().emplace<WindowPixelQuery>(WindowPixelQuery{
		[window] { return std::optional{window->pixelSize()}; }});
	// 鼠标逻辑点 → 帧缓冲像素的换算倍数（点选口径；为什么必须换算 → ecs.cppm WindowScale）
	rg.ctx().emplace<WindowScale>(WindowScale{window->displayScale()});
}

/**
 * 渲染尺寸 context 装配 + 口径日志（Phase 14 批 14.3）。
 *
 * 为什么走 context 而不是把 Swapchain 传进 system：`ecs` 不 import `vk_swapchain`
 * （分层红线，假边会让慢模块进本不该等它的编译前置），而 projectionSystem / taaSystem
 * 的像素口径必须是渲染网格 ⟹ 以 context 组件为界。
 *
 * 只装**初值**：之后每帧由 `resizeSystem` 帧首从 swapchain 同步（那条覆盖 OUT_OF_DATE
 * 重建路径——它改渲染 extent 却不改 WindowExtent）。
 *
 * 日志是判据的一部分：`tools/smoke.py --render-scale` 靠解析本行核对"请求的倍率真的
 * 生效了"，改了文案那条判据会当场报"口径日志没了"。
 * @param rg registry（写 RenderExtent）
 * @param swapchain 已构造的交换链（渲染 extent 的唯一来源）
 */
void wireRenderExtentContext(entt::registry& rg, const Swapchain& swapchain) {
	const auto r = swapchain.getRenderExtent();
	rg.ctx().emplace<RenderExtent>(r.width, r.height);
	if (auto* logger = RLLogger::getInstance()) {
		const auto s = swapchain.getExtent();
		logger->info("render scale = {} → render {}x{} (swapchain {}x{})",
			swapchain.getRenderScale(), r.width, r.height, s.width, s.height);
	}
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

/**
 * SceneEnv + TaaState context 装配（main 启动流调用；三处配置读取收一处，main 保持
 * 在 120 行红线内）。
 *
 * - **SceneEnv**（Phase 5.5）：曝光（Phase 8 收尾）与 SSAO 强度（批 11.4）都与 env
 *   开关无关（任何变体的图都有 tonemap / ao pass），故在此直填；env cubemap 自身的
 *   intensity/mipRange/skybox 由 env 上传成功后回填（见 main_scene 的装配函数）。
 * - **TaaState**（批 12.6）= 本项目第一个**跨帧 CPU context**（与 GPU 侧 `taa_hist`
 *   附件同生死）；默认值语义 = 「首帧无历史」（frameIndex 0 ⇒ historyValid false ⇒
 *   shader 逐位旁路）。运行参数（feedback / jitterScale / motionCompensation）搭
 *   SceneEnv 同路，imgui 参数页可调（同 aoStrength 先例）。
 *   `taaEnabled` 在这里只是**每帧抖动**开关，**图形态**由 compileGraphOrThrow 的
 *   decorateTaaPass 决定——两处读同一配置源，缺一就会出现「抖了但没滤波」或
 *   「有 pass 但没抖动」的半开状态。批 14.6 起两处读的都是 `RuntimeConfig::aaWantsTaa()`
 *   （原 scene.lua `taa.enabled` 键已删），本形参只是把它穿下来。
 * @param rg 场景 registry（ViewProjection 等基础 ctx 已 emplace）
 * @param scene configs/scene.lua 产物
 * @param taaEnabled 调用点传 `configs.runtime.aaWantsTaa()`（批 14.6 三态推导）
 */
void wireSceneEnvContexts(entt::registry& rg, const SceneConfig& scene, bool taaEnabled) {
	auto& env = rg.ctx().emplace<SceneEnv>();
	env.exposure = scene.exposure;
	env.aoStrength = scene.aoStrength;
	env.taaEnabled = taaEnabled;
	env.taaFeedback = scene.taaFeedback;
	env.taaJitterScale = scene.taaJitterScale;
	env.taaMotionCompensation = scene.taaMotionCompensation;
	env.taaBaseSpace = scene.taaBaseSpace;  // 批 13.5 基空间累积（唯一消费者 = taa.frag）
	// 重投影消融 + 历史拒收诊断（均默认 = 逐位今天；语义 → SceneEnv 字段注释）
	env.taaReprojection = scene.taaReprojection;
	env.taaDiagReject = scene.taaDiagReject;
	// 输入侧重建核（批 13.8；默认 0 = 双线性 = 逐位今天；判据消费方 = imgqa --kernel）
	env.taaInputKernel = scene.taaInputKernel;
	// 历史裁剪消融门（批 13.11；默认 0 = 现行 = 逐位今天；判据消费方 = imgqa --clip）
	env.taaClipMode = scene.taaClipMode;
	env.taaClipSoft = scene.taaClipSoft;
	rg.ctx().emplace<TaaState>();
}

/**
 * SceneSpin（全场景同一旋转）——scene.lua 三键 → registry context 的映射。
 *
 * 抽 helper 的直接原因 = `main()` 的 120 行红线，顺手把「配置 → 旋转态」收成一处。
 * 三键里 `spin_frame_angle` / `spin_axis` 是**诊断激励门**（默认 0 = 关 = 逐位今天；
 * 判据本体 = `frameDrivenSpinAngle`），消费方 = `imgqa temporal-motion`。热重载侧的
 * 同名映射见 main_scene.cppm::wireHotReload（与 `spin` 键的既有做法一致）。
 * @param rg 场景 registry（尚未 emplace SceneSpin）
 * @param scene configs/scene.lua 产物
 */
void wireSceneSpinContext(entt::registry& rg, const SceneConfig& scene) {
	auto& spin = rg.ctx().emplace<SceneSpin>();
	spin.speed = scene.spin ? 1.0f : 0.0f;  // 0 = 静止（configs/scene.lua 的 spin 开关）
	spin.frameAngle = scene.spinFrameAngle;  // rad/帧；0 = 帧驱动门关（dt 累积路径照旧）
	spin.axis = scene.spinAxis;              // 默认 (0.5,1,0) = SceneSpin 现值
}

/**
 * 每帧调参 context（Phase 15 批 15.1 SSR 系数 + 批 15.2 子步 A 湿面参数）——
 * configs/runtime.lua 的 `ssr`/`wet` 块 → registry context 的映射。
 *
 * 抽 helper 的直接原因 = `main()` 的 120 行红线（wireSceneSpinContext 同款），顺手把
 * 「配置 → 每帧调参」收成一处。注意分工：这里搬的是**系数**（每帧量：引擎每帧现取现写
 * scene data）；`ssr.enabled` 是**创建期量**，走 configs.runtime.ssrEnabled 决定
 * decorateSsrPass 建不建 pass，不进本 ctx、也不进 scene data（湿面**没有**创建期开关：
 * 调制住在 deferred_geometry（G-buffer 调制），不改图形态，`strength = 0` 就是门关档）。缺块时 ctx 里
 * 就是结构体默认值 = 出厂系数（默认值唯一出处在 C++）。runtime.lua 无热重载
 * （RuntimeConfig 只在启动期 load() 一次）⟹ 两类改动都要重启进程才读到，区别只在
 * "生效"要不要重建 RT / 重建管线。
 * @param rg      场景 registry（尚未 emplace SsrTuning/WetTuning）
 * @param runtime configs/runtime.lua 产物
 */
void wireTuningContexts(entt::registry& rg, const RuntimeConfig& runtime) {
	rg.ctx().emplace<SsrTuning>(runtime.ssr);
	rg.ctx().emplace<WetTuning>(runtime.wet);
}

/** 渲染图配置加载 + compileRenderGraph（校验 + barrier 布局链推导，错误抛 std::runtime_error）+ deferredMode 求值。
 *  Phase 10 批 10.5：diagnostics.picking（默认关）= 编译前 decoratePickingPass 装饰
 *  object_id pass（D10 编译图期门控——默认关不调用 = 图逐位同 diff0）。 */
/**
 * 批 14.4 速度场图装饰（与 decorateTaaOrWarn 同构；抽函数控复杂度）。
 * 关（默认）= 不调 decorateVelocityPass = 图里根本没有这个 pass（**结构性**逐位不变，
 * 不是"有但旁路"）。开但图形态不支持 = warn + false，装配层据 false 整套不建。
 * @return 是否真正装饰落图
 */
bool decorateVelocityOrWarn(RgGraphDesc& desc, bool velocityEnabled) {
	if (!velocityEnabled)
		return false;
	if (decorateVelocityPass(desc))
		return true;
	if (auto* logger = RLLogger::getInstance())
		logger->warn("velocity: 图形态不支持速度场装饰（锚点 = 写 swapchain 且采样场景色的 "
			"pass，且其前须有可 store 的 depth 写者），本次运行无逐像素速度场");
	return false;
}

/**
 * 批 14.5 第三批：AO 施加专用 pass 的图装饰（抽函数控复杂度）。
 *
 * 门 = **FSR 门开**（`runtime.fsr_upscaler`）。为什么不是「FSR 真的接起来了」：FSR 的可用性
 * （shader16 / 三个输入附件）要到装配层才裁决完，而本装饰在图编译期。这个先后顺序**不影响
 * 正确性**——施加点标志取的是**实际图形态**（`aoApplyDecorated`），所以「本 pass 在图里但
 * FSR 最终没接」= AO 由本 pass 施加、tonemap 跳过，画面只是比 tonemap 路径多一次 RGBA16F
 * 量化，不会双施也不会零施。
 *
 * 装饰器自己还会查「taa 不在图里」与「有 ao 附件」，任一不满足返回 false = 图逐位不变。
 */
bool decorateAoApplyOrWarn(RgGraphDesc& desc, bool fsrEnabled) {
	if (!fsrEnabled)
		return false;
	if (decorateAoApplyPass(desc))
		return true;
	if (auto* logger = RLLogger::getInstance())
		logger->debug("ao_apply: 未插 AO 施加 pass（taa 已在图里 = 施加点在它里面，"
			"或本图无 ao 附件），AO 施加点保持现状");
	return false;
}

/**
 * 批 12.6 TAA 图装饰（compileGraphOrThrow 尾段调用；抽函数控复杂度）。
 * 关（默认）= 不调 decorateTaaPass = 图逐位不变；开但图形态不支持（offscreen DIAG
 * 变体用 blit 上屏，无「写 swapchain 且采色」的 pass）= warn + 返回 false，
 * 装配层据 false 整套不建 TAA RT/条目（条目建了也无人执行）。
 * @return 是否真正装饰落图
 */
bool decorateTaaOrWarn(RgGraphDesc& desc, bool taaEnabled) {
	if (!taaEnabled)
		return false;
	if (decorateTaaPass(desc))
		return true;
	if (auto* logger = RLLogger::getInstance())
		logger->warn("taa: 图形态不支持 TAA 装饰（锚点 = 写 swapchain 且采样场景色的 pass，"
			"offscreen 变体用 blit 上屏故不支持），本次运行无时域抗锯齿");
	return false;
}

/**
 * 批 15.1 前半：SSR 图装饰（与 decorateVelocityOrWarn 同构；抽函数控复杂度）。
 * 关（默认）= 不调 decorateSsrPass = 图里根本没有这个 pass（**结构性**逐位不变，
 * 不是"有但旁路"）。开但图形态不支持（offscreen 变体无采色上屏消费者 / 图缺 G-buffer
 * 契约表通道（forward 图 = D1 豁免）/ 借不到可 store 的 depth）= warn + false，
 * 装配层据 false 整套不建。
 * @return 是否真正装饰落图
 */
bool decorateSsrOrWarn(RgGraphDesc& desc, bool ssrEnabled) {
	if (!ssrEnabled)
		return false;
	if (decorateSsrPass(desc))
		return true;
	if (auto* logger = RLLogger::getInstance())
		logger->warn("ssr: 图形态不支持 SSR 装饰（锚点 = 写 swapchain 且采样场景色的 pass，"
			"且图里须有 G-buffer 契约表全部通道 = deferred 图，且锚点前须有可 store 的 depth "
			"写者；forward 不跟 = D1 对等豁免，docs/phase15_plan.md §0.1；或 taa/ao_apply "
			"已在图里 = 装饰太晚，见 decorateSsrPass 跳过条件 1），本次运行无屏幕空间反射");
	return false;
}

CompiledGraphResult compileGraphOrThrow(const char* path, const RuntimeConfig& runtime) {
	// 批 15.1 前半：四个图形态开关改由 RuntimeConfig 派生（= 原四形参的逐位同值来源，
	// 单一真相源在 runtime_config；main() 调用点不再逐项推导，省行也防漏传）。
	const bool taaEnabled = runtime.aaWantsTaa();
	const bool velocityEnabled = runtime.velocityField;
	const bool fsrEnabled = runtime.aaWantsFsr();
	const bool ssrEnabled = runtime.ssrEnabled;
	auto graphConfig = loadRenderGraphConfig(path);
	if (!graphConfig)
		throw std::runtime_error(graphConfig.error().describe());
	bool deferredMode = graphConfig->variant == RgGraphVariant::deferred;
	// 装饰失败 warn 留在装饰期（失败与否 = 装饰函数对图形态的判定结果，lint 复刻判据
	// = 影子模型，防腐条款 1）；消息带锚点定义，让「为什么没插进去」不用读代码。
	if (diagnostics().picking && !decoratePickingPass(graphConfig->desc)) {
		if (auto* logger = RLLogger::getInstance())
			logger->warn("picking: 图形态不支持 object_id pass 装饰（锚点 = 首个 drawSource=opaque "
				"且输出 depth 的 drawMesh pass，未找到），点选不可用");
	}
	// 批 10.7：高亮 pass 装饰（写既有 scene_hdr + depth，插图末；同门 = 默认关不调用）
	if (diagnostics().picking && !decorateHighlightPass(graphConfig->desc)) {
		if (auto* logger = RLLogger::getInstance())
			logger->warn("highlight: 图形态不支持高亮 pass 装饰（锚点 = 首个采样场景色附件的 pass "
				"之前，且其前须有 depth 写者可借），选中无视觉高亮");
	}
	// Phase 11 批 11.2：阴影 pass 装饰**无条件**（不再由 diagnostics 门控）。原因（实测驱动）：
	// 光照管线条目**无条件把阴影图的描述槽烘焙进去**，所以图里必须无条件地有这张图
	// 与其布局链（否则开关时描述绑定的图从未被任何 pass 转换布局 → 采样它就报 VVL）。
	// 门控改到渲染侧：关时阴影段**零 draw**（只清 4 MB 深度，近乎免费），图内全是
	// 1.0 = 无遮挡 → 采样结果恒为全亮 → 成片逐位不变。
	decorateShadowPass(graphConfig->desc, kShadowMapSize);
	if (auto* logger = RLLogger::getInstance())
		logger->info("shadow: 已装饰 depth-only 阴影 pass（{}² {}，{} 级；投影体清单由 "
			"shadow.lua 的 mode 门控（形态表 drawAtlas），mode = \"none\" 时段内零 draw = 画面逐位不变）",
			kShadowMapSize, kShadowMapFormat, kActiveShadowCascades);
	// 批 15.1 前半：SSR 装饰插在 shadow **之后**、velocity / taa / ao_apply **之前**。理由：
	//  ① SSR 读 scene_hdr（light pass 的产物），必须在其后——light 在图本体里，任何装饰都
	//     在它之后跑，shadow 之后自然满足；
	//  ② velocity / taa / ao_apply 三个装饰器都**动态**读「消费者当前的色附件」
	//     （sampledColorName）⟹ SSR 先插，它们自动读到 ssr_applied，链自己接成
	//     scene_hdr → ssr_applied → [taa_out | ao_applied] → tonemap。这是这些装饰器可
	//     复合的既有性质，不要破坏它。TAA 读到含 SSR 的成片 = 正确（时域滤波该滤最终画面）。
	// 批 15.1 后半：这条顺序不变量不再只由本注释承载——decorateSsrPass 跳过条件 1 在
	//   taa / ao_apply 已在图里时返回 false（= SSR 静默消失，smoke --ssr 双向核对可见），
	//   把「挪错顺序」从不可观测（SSR 静默建在时域滤波之后 = 图/布局/像素三层全无异常）
	//   变成可观测失败。挪错顺序的端到端兜底（aa=taa 档 SSR 开/关差分必须不同）→
	//   phase15_plan §3.2 缺口 1/2。
	// 默认关 = 不调用 = 图逐位不变（D10 纪律；门 = runtime.lua ssr.enabled，与 aa 正交）。
	const bool ssrDecorated = decorateSsrOrWarn(graphConfig->desc, ssrEnabled);
	// 批 12.6 TAA 装饰**必须最后**（在 picking / highlight / shadow 之后）：它把
	// 「写 swapchain 且采色」的 pass（tonemap）的输入换成时域结果，若 highlight 后插
	// 就会落在 taa 与 tonemap 之间而吃不到滤波。默认关 = 不装饰 = 图逐位不变（D10 纪律）。
	// 批 14.4 速度场装饰**先于** TAA（TAA 保持「必须最后」的既有纪律）。落点顺序
	// = [velocity, taa, tonemap]：两者都只 sampleRead depth，彼此无依赖，而 TAA 的锚点
	// 是**现搜**的（不缓存下标）⟹ velocity 先插不会让 TAA 认错消费者。
	const bool velocityDecorated = decorateVelocityOrWarn(graphConfig->desc, velocityEnabled);
	const bool taaDecorated = decorateTaaOrWarn(graphConfig->desc, taaEnabled);
	// 批 14.5 第三批：AO 施加 pass **必须在 taa 之后**装饰——它自己要先看「taa 在不在图里」
	// 决定跳过，而它本身是真正的成片消费者，先插会让前面的装饰认错锚点。
	const bool aoApplyDecorated = decorateAoApplyOrWarn(graphConfig->desc, fsrEnabled);
	auto compiledGraph = compileRenderGraph(graphConfig->desc);
	if (!compiledGraph)
		throw std::runtime_error(fmt::format("render graph compile failed: {}",
			compiledGraph.error().describe()));
	return CompiledGraphResult{std::move(*compiledGraph), deferredMode, graphConfig->variant,
		taaDecorated, velocityDecorated, aoApplyDecorated, ssrDecorated};
}

/**
 * 门组合自检（gate_lint 批 §3-C，docs/gate_lint_plan.md）：启动期跑一次全量表。
 * 门在进程内不可变（setDiagnostics 全仓仅此一处运行期调用）→ 结论对全程权威。
 * 时序契约 = compileGraphOrThrow 之后（要图事实）、GPU 大对象装配之前（findings
 * 早于一切渲染日志，先于问题现场）。lint **不阻止启动**（诊断门不该让程序起不来）。
 * @param runtime runtime.lua 产物（headless / llm_debug 两个消费前提）
 * @param device 设备上下文（staticGraphicsIndirect 能力事实）
 * @param graphResult 编译图产物（passes = 图事实；variant = 配置意图，交叉校验）
 */
void runGateLint(const RuntimeConfig& runtime, const DeviceContext& device,
	const CompiledGraphResult& graphResult) {
	GateEnv gateEnv;
	gateEnv.headless = runtime.headless;
	gateEnv.llmCapture = runtime.llm.enabled;
	gateEnv.staticGraphicsIndirect = device.getDgcSupport().staticGraphicsIndirect;
	// 两份配置各带一份出厂默认（第二份 = 「只报偏离出厂值的门」的比对基准）；
	// shadow 侧与 diagnostics 侧同为「启动期单例」口径（loadAllConfigs 已回灌）。
	const auto findings = lintDiagnosticsGates(diagnostics(), DiagnosticsConfig{},
		shadowConfig(), ShadowConfig{},
		graphResult.graph.passes, graphResult.variant, gateEnv);
	const uint32_t errors = logGateFindings(findings);
	if (auto* logger = RLLogger::getInstance())
		logger->info("gate_lint: {} findings ({} error)", findings.size(), errors);
}

/**
 * 启动基线两行（批 9.5 观测面；调用点 = 全部 GPU 资源装配完、进帧循环前）。
 *
 * 立这两行的动机：① 全仓此前**没有任何显存总量读数**（vmaBuildStatsString /
 * GetHeapStatistics / CalcAllocationStats 零命中，VMA 泄漏检测未开），各 RT 预算只
 * 散在四份文档注释里（EVSSM 44.7MB / 阴影图集 16MB / …）→ 先做成可回归的启动日志；
 * ② shader object 条目基线（312 条目 / 1252 句柄 / ≈2.3s）是 2026-09-28 那次
 * forward + picking 临时开运行的数，此后加了 TAA/EVSSM binding 且默认路改 deferred，
 * 条目数与 mapping 条数都可能漂移——CONSTANT_OFFSET 那条债的收益（顶点块调大 →
 * 条目 312→≈132）没有现行基线就无法评估。
 *
 * 纯读统计，不改任何状态；开销 = VMA 全块遍历一次 + 计数器读取，启动期一次可忽略。
 * 顶点块数另有现成日志行（model_importer_impl.cppm "model: N primitives → M 块"），
 * 本行的 geometrySlots = 几何段总槽数（模型块 + 内建几何顶点/索引槽）。
 * @param res 资源层（VMA 分配器宿主）
 * @param registry 管线表（条目数读数）
 * @param assets 资产表（几何槽计数读数）
 */
void logStartupBaseline(const Resources& res, const ShaderRegistry& registry,
	const AssetTable& assets) {
	res.logMemoryStats("startup");
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	const auto so = shaderObjectStats();
	logger->info(
		"shader_object baseline: registryEntries={} pipelinesCreated={} handles={} mappings={} "
		"createTotal={:.2f}s avgPerEntry={:.2f}ms geometrySlots={}",
		registry.entriesView().size(), so.pipelines, so.handles, so.mappings,
		static_cast<double>(so.createNs) / 1e9,
		so.pipelines ? static_cast<double>(so.createNs) / 1e6 / static_cast<double>(so.pipelines)
			: 0.0,
		assets.getGeometrySlotCount());
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
		wireWindowContexts(registry, pre);
		registry.ctx().emplace<AppExit>();
		registry.ctx().emplace<Camera>(std::move(configs.camera.camera));
		registry.ctx().emplace<InputEvents>();
		registry.ctx().emplace<InputFilter>();  // Phase 10 调试层输入先手过滤器（imgui 装配；缺省空 = 不过滤）
		registry.ctx().emplace<DebugPanelState>();  // 面板跨帧 UI 态（材质选择器等，10.2c）
		registry.ctx().emplace<CameraState>();
		wireSceneSpinContext(registry, configs.scene);  // SceneSpin（dt 累积 + 帧驱动门）
		registry.ctx().emplace<SceneBounds>();
		registry.ctx().emplace<ViewProjection>();
		registry.ctx().emplace<FrameDrawList>();
		registry.ctx().emplace<NextObjectSlot>();  // Phase 6 instancing：实例矩阵槽分配器
		// SceneEnv（曝光/AO/TAA）+ TaaState；TAA 开关 = aa 三态推导（批 14.6）
		wireSceneEnvContexts(registry, configs.scene, configs.runtime.aaWantsTaa());
		// SSR 系数 + 湿面参数（批 15.1 参数面 + 批 15.2 子步 A）：configs/runtime.lua 的 ssr/wet 块 → ctx →
		// 每帧 fill*Params 写 scene data 尾区（湿面本步**零消费者**，写它不改像素，红线 A = regress ab）。
		// 创建期/每帧量的分工与"缺块 = 结构体默认值" → wireTuningContexts 注释。
		wireTuningContexts(registry, configs.runtime);

		AppContext vkInstance(pre.window.get(), pre.headless);
		DeviceContext vkDevice(vkInstance);
		checkPushDataLimits(vkDevice);
		// 末位 = 内部渲染分辨率倍率（批 14.3，默认 1.0 = 交付路径逐位不变）
		Swapchain vkSwapchain(vkDevice, pre.window.get(), pre.headless, pre.headlessW,
			pre.headlessH, configs.runtime.presentMode, configs.runtime.renderScale);
		wireRenderExtentContext(registry, vkSwapchain);  // 渲染尺寸进 registry + 口径日志
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
		// 簇产物容器（★0-b2）：与 assetTable 同层由装配层持有——**不进 UploadedAssets**
		//（那是"已上传 GPU 的东西"，本批什么都没传；理由全文 → MeshletAssetStore 注释）。
		// 声明序紧随 assetTable：纯 CPU 数据无 GPU 生命周期契约，★1-a 直接消费本容器。
		MeshletAssetStore meshletAssets;
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
		// 批 14.6：TAA 图形态与 FSR 开关都是 aa 三态推导（原 scene.taaEnabled / fsr_upscaler
		// 键已删）；velocityField 在 aa=fsr 时已被解析期隐含打开（单一来源在解析处）。
		// 批 15.1 前半：四个图形态开关（taa/velocity/fsr/ssr）改由 configs.runtime 一次传入
		// （派生点搬进 compileGraphOrThrow，见其函数头注释）。
		auto graphResult = compileGraphOrThrow(configPath("render_graph.lua").c_str(), configs.runtime);
		const bool deferredMode = graphResult.deferredMode;
		// 图来源/规模观测（排"配置没生效"类问题：pass 数与 configs/render_graph.lua 对账）
		if (auto* logger = RLLogger::getInstance())
			logger->info("RenderGraph: {} attachments / {} passes compiled",
				graphResult.graph.attachments.size(), graphResult.graph.passes.size());
		// 门组合自检（gate_lint 批，docs/gate_lint_plan.md §3-C）
		runGateLint(configs.runtime, vkDevice, graphResult);
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
		auto assets = uploadModelAssets(vkDevice, vkResources, samplers, assetTable, configs.scene,
			configs.runtime.meshletEnable, meshletAssets);
		if (configs.scene.environment.enabled)
			uploadEnvironment(vkResources, env, vkDevice, configs.scene, registry);  // 时序契约见定义处注释

		// C1：管线条目烘焙（per **unique 模型**——instancing 共享条目；addModelEntry
		// 按 (规格,顶点 slot,纹理 slot) 组合去重兜底）
		bakePipelineEntries(assets, configs.scene, deferredMode, shaderRegistry, vkResources, samplers,
			assetTable, rtCache);
		// 克隆管线条目（Phase 10 批 10.5 picking / 10.7 highlight）：契约与门控见 helper
		// 注释；自 main() 抽出（★0-b2 装配层行数预算，纯代码搬移零行为变化）。
		addDiagnosticCloneEntries(shaderRegistry);
		// C2/C2b：场景实体注册（模型/cube/灯）+ 实例矩阵槽容量契约
		registerSceneEntities(registry, assets, configs.scene, vkResources, assetTable, shaderRegistry);

		// C3 GPU culling 候选表 + GpuCull + skybox + deferred G-buffer/HDR + DGC + Renderer
		//（装配时序契约见 assembleBackEnd 接口注释；graph 在此 move 进 Renderer）
		auto backEnd = assembleBackEnd(vkDevice, vkSwapchain, vkResources, sceneData, samplers, env, rtCache, assetTable, shaderRegistry,
			std::move(graphResult), registry, configs.scene,
			// 批 14.6：FSR 开关 = aa 三态推导（原 fsr_upscaler 键已删）；两枚调参留在 fsr 块
			configs.runtime.aaWantsFsr(), configs.runtime.fsrTemporal, configs.runtime.fsrJitter);

		// Phase 9.2：GPU 时间戳会话（声明序在 backEnd/vkDevice 后 = 析构序其先，
		// query pool 不越过 device 生命周期——GPU 资源所有权规则；profiler=0 /
		// 校准扩展缺位 = 内部不建 pool，钩子全早退）
		GpuProfiler gpuProfiler(vkDevice);
		bindGpuProfiler(&gpuProfiler);

		// Phase 10 批 10.1：IMGUI 调试面板（docs/archive/phase10_plan.md §4）。装配门 = 有窗 &&
		// diagnostics.imgui（headless 天然关）；声明序在 backEnd 之后 = 析构序其先
		//（后端 shutdown 需 device/swapchain 存活）。init 失败 = 日志 + 无 UI 继续（降级不崩）。
		// 挂载点两处：InputFilter.consume（事件先手，WantCapture 门）+ Renderer 帧末扩展段
		//（UI 渲染段，布局推进由执行器负责，契约见 setFrameEndHook 注释）。
		RGL::ImGuiLayer imguiLayer;
		if (!pre.headless && RGL::diagnostics().imgui && pre.window
			&& imguiLayer.init(vkInstance, vkDevice, vkSwapchain, *pre.window,
				configs.runtime.uiFont, configs.runtime.uiFontSize)) {
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

		// 配置热重载（Phase 10 批 10.3）：门 = diagnostics.config_reload（默认关）；
		// 挂 ConfigReloadHook 由 renderSystem 帧边界轮询源 configs/。独立于 imgui 门。
		wireHotReload(registry);
		// shader 热重载（Phase 10 批 10.4）：门 = diagnostics.shader_reload（默认关）；
		// 复用同一 ConfigReloadHook 帧边界挂点（组合追加）。需 backEnd 已装配（waitForIdle/DGC）。
		wireShaderHotReload(registry, shaderRegistry, backEnd);

		// GPU id 点选（Phase 10 批 10.5）：门 = diagnostics.picking（wirePicking 内部
		// 判，关 = nullptr 零装配）。声明序在 backEnd 后 = Renderer 持裸指针存活契约；
		// 声明序在 registry 块内其后 = 逆序析构先释放服务再毁 registry ctx 闭包。
		auto pickingService = wirePicking(registry, vkResources, rtCache, backEnd.renderer);

		// HTTP POST RPC 调试通道（Phase 10b 批 11b.2，docs/phase10b_plan.md）：门 =
		// diagnostics.rpc（默认关；与 imgui 门独立，headless 也可用）。门关 / bind 失败
		// 都降级 nullptr。声明序在 registry/assets/backEnd 之后 = 析构序其先 →
		// try 块退出（帧循环已结束，configReloadTick 不会再跑）时 ~DebugRpcServer =
		// stop() 收口：关监听线程、在途请求全 503，闭包里的裸服务指针绝不越权。
		auto debugRpc = wireDebugRpc(registry, assetTable, backEnd.renderer, pre.llmEnabled);

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
		// ===== 批 9.5 观测：启动基线两行（显存账 + shader object 条目账）=====
		// 动机与口径见 logStartupBaseline 注释（纯读统计，不改任何状态）。
		logStartupBaseline(vkResources, shaderRegistry, assetTable);

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
