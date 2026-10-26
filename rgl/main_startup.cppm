module;
#include <fmt/format.h>

// 入口模块 main 的分区实现单元（module main:startup;）——**启动前置**这一族：在 registry 与
// 任何 Vulkan 对象存在**之前**把进程需要的输入备好。产出两个接口层就已在 main.cppm 里命名的
// 阶段产物：`StartupConfigs`（四份 lua）与 `PreWindow`（自检 + 尺寸契约 + 窗口 + 派生量）。
//
// 【为什么从 main:impl 拆出来】main:impl 拆掉渲染图那一族（→ main:graph）之后仍在 408/500，
// 而 `main()` 单函数顶到 120 行上限（下一个启动步骤加不进去）。把 main() 按阶段抽 helper 时，
// helper 得有个家——留在 main:impl 就只能把文件推回 420 以上。真正的缝在这里，而且**接口层
// 早就承认它**：configDir / configPath / loadAllConfigs / createAppWindow / prepareWindow 这
// 五个函数在 main.cppm 里本来就成组 export，`PreWindow` 的接口注释写的就是"启动前置产物
// （loadAllConfigs 之后、registry 之前）"。本单元只是给这个**已命名的阶段**补一个实现文件，
// 拆完 main:impl 的接口面**零新增**。
//
// 时序契约（都在本族内部，改动前先读）：
//   · 配置先于窗口：llm_debug 模式影响建窗（离屏隐藏窗 + 配置尺寸），故 `loadAllConfigs`
//     必须早于 `prepareWindow`；
//   · `loadAllConfigs` 内的 `setDiagnostics` / `setShadowConfig` 是**先于一切 GPU 对象构造**
//     的唯一写入点（vkinstance 建 instance 时就要读 no_vvl），所以本族跑在 main() 的 try 块
//     之前，不在里面；
//   · `checkRenderGraphSelfCheck`（定义在 main:graph）早于建窗——纯 CPU 图算法回归守卫，
//     headless / llm 路径同样被它覆盖。
//
// 可见性同姊妹分区：声明在 main.cppm（export），定义在本单元（不带 export）。
module main:startup;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import vulkan_window;   // SDLWindow（建窗产物 + pixelSize/getWidth 口径）
import camera_config;   // loadCameraConfig
import scene_config;    // loadSceneConfig
import runtime_config;  // loadRuntimeConfig + RuntimeConfig（窗口/llm/headless 尺寸键）
import diagnostics;     // loadDiagnosticsConfig + setDiagnostics（启动期单例唯一写入点）
import shadow_config;   // loadShadowConfig + setShadowConfig（批 N6b 回灌）
import rllogger;

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
}  // namespace RGL
