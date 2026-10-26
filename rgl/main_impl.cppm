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
import device_fault;  // GPU fault 诊断通路（device lost 出口抓一次；纯旁路）
import meshlet_shadow_args;  // heap 槽审计执行账（登记进诊断账注册表）
import shutdown_ledger;       // 诊断账注册表（登记 + 两条路结算）
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
import gate_lint;  // 门组合自检（启动期一次，docs/archive/gate_lint_plan.md §3-C）
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
import profiler;         // Phase 9：帧循环 zone 收口 dump（docs/archive/phase9_plan.md 批 9.1）
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
 * 本行给人读；生效核对（`tools/smoke.py --render-scale`）读的是 `/state` 的 render 段
 * （能力观测批 2），改这行文案不会弄坏任何判据——但也别删，三机排障都靠它。
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

/**
 * 退出前的诊断收口（main 保持在 120 行红线内，本函数把三件"帧循环结束后才做"的事收一处）。
 *
 * 顺序有理由：① profiler dump 与 ② gpu profiler 解绑必须在 `gpuProfiler` 这个 main 栈对象
 * 析构之前（Phase 9 / 9.2 既有契约）；③ 轮询账**成功退出也要打**——否则"跑完 600 帧一条
 * masked 都没抓到"（结论）与"根本没开门"（配置）在日志里长得一样。
 *
 * 🔴 **device fault 的抓取不在这里**（曾在这里，2026-10-20 实测挪走）：设备 lost 之后收尾的
 * `waitIdle` 会抛 vk::SystemError，异常展开时 `vkDevice` 已析构，本函数根本执行不到——
 * hang 那次的日志停在「错误: vk::raii::Device::waitIdle: ErrorDeviceLost」而 device_fault
 * 一行未出，就是证据。抓取点 = `ecs_impl` 的 `drawFrameChecked`（帧循环所有 device-lost
 * 来源汇流的最早一点，且此刻 device 还活着）。
 */

void exitDiagnostics() {
	RGL::profilerDump();       // Phase 9：trace/stats/摘要收口（profiler=0 = 静默 no-op）
	bindGpuProfiler(nullptr);  // Phase 9.2：gpuProfiler 析构前解绑（帧循环已结束）
	// 诊断账结算（注册表 = 唯一清单，登记点见 registerDiagnosticReporters）。
	// 🔴 device-lost 之路上也调同一条：见 `ShutdownLedger` 的类注释（为什么必须两处都调）。
	RGL::shutdownLedger().run();
}

/** Phase 5.5 IBL 风格契约检查（validateEnvironmentContract）已随刀②.2 删除，理由见
 *  main.cppm 的 tombstone 注释。 */

/**
 * SceneEnv + TaaState context 装配（main 启动流调用；三处配置读取收一处，main 保持
 * 在 120 行红线内）。
 *
 * - **SceneEnv**（Phase 5.5）：曝光（Phase 8 收尾）与 SSAO 强度（批 11.4）都与 env
 *   开关无关（任何变体的图都有 tonemap / ao pass），故在此直填；env cubemap 自身的
 *   intensity/mipRange 由 env 上传成功后回填（见 main_scene 的装配函数；skybox 字段
 *   已随 forward 绘制机制删除，刀②.2）。
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
 * 每帧调参 context（Phase 15 批 15.1 SSR 系数 + 批 15.2 子步 A 湿面参数）+ 渲染原点门
 * （相对坐标批 3）——configs/runtime.lua 的 `ssr`/`wet` 块与 `coordinate.rebase` 键 →
 * registry context 的映射。
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
	// 渲染原点（相对坐标批 3 装配置门；批 5 第 1 刀起 position 由 projectionSystem 每帧写）：
	// 这里**只搬配置门**，一个数都不算。门关 = position 恒 (0,0,0) ⟹ toRelative 减法恒等
	// ⟹ 逐位不变；门开 = projectionSystem 把相机位置 snap 到级联纹素粒度（粒度 NaN 才抛错拒跑）。
	// 它是 **渲染这一个消费者的参数**，不是引擎级全局原点（→ scene_context 的 RenderOrigin 注释）。
	rg.ctx().emplace<RenderOrigin>(RenderOrigin{.enabled = runtime.coordinateRebase});
	// 门开态的**回显行**（门关一行都不打 = 出厂日志逐字不变）。为什么必须有它：配置覆写器的写后
	// 读回只证明「文件里是 true」，证明不了「引擎按这个值跑」；而「键没被读到」与「原点恒零」两种
	// 失效在图像上都长得像绿（同 `shadow: mode=` / `csm.force_cascade=` 两条回显的既有纪律）。
	// 消费方 = `imgqa shadow-edge-rebase` 的 G3 第 4 查。
	if (auto* l = runtime.coordinateRebase ? RLLogger::getInstance() : nullptr)
		l->info("coordinate.rebase = true（原点每帧 snap 到级联纹素粒度；算不出即拒启动）");
}

/**
 * 门组合自检（gate_lint 批 §3-C，docs/archive/gate_lint_plan.md）：启动期跑一次全量表。
 * 门在进程内不可变（setDiagnostics 全仓仅此一处运行期调用）→ 结论对全程权威。
 * 时序契约 = compileGraphOrThrow 之后（要图事实）、GPU 大对象装配之前（findings
 * 早于一切渲染日志，先于问题现场）。lint **不阻止启动**（诊断门不该让程序起不来）。
 * @param runtime runtime.lua 产物（headless / llm_debug 两个消费前提 + meshlet 块生效值，债-106）
 * @param device 设备上下文（staticGraphicsIndirect 能力事实）
 * @param graphResult 编译图产物（passes = 图事实；2026-10-18 刀② 起 lint 只吃图事实——
 *   原 variant 参数 = 配置意图 + 两者交叉校验，随 `RgGraphVariant` 枚举一起删除）
 */
void runGateLint(const RuntimeConfig& runtime, const DeviceContext& device,
	const CompiledGraphResult& graphResult) {
	GateEnv gateEnv;
	gateEnv.headless = runtime.headless;
	gateEnv.llmCapture = runtime.llm.enabled;
	gateEnv.staticGraphicsIndirect = device.getDgcSupport().staticGraphicsIndirect;
	// device fault 的 masked 上报是否**真的启用了**（探测事实，fault_poll 的生效前置）
	gateEnv.deviceFaultMasked = device.getDeviceFaultSupport().reportMasked;
	gateEnv.printfBufferSet = printfBufferEnabled();  // 债-140 A2 自证的生效前置（口径在 diagnostics）
	// 三份配置各带一份出厂默认（第二份 = 「只报偏离出厂值的门」的比对基准）；
	// shadow 侧与 diagnostics 侧同为「启动期单例」口径（loadAllConfigs 已回灌），
	// runtime 侧的基准 = `RuntimeConfig{}`（meshlet 块出厂态 assets + shadow_lod = 5，债-106）。
	const auto findings = lintDiagnosticsGates(diagnostics(), DiagnosticsConfig{},
		shadowConfig(), ShadowConfig{}, runtime, RuntimeConfig{},
		graphResult.graph.passes, gateEnv);
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


/**
 * 启动期 registry context 装配 = `main()` 的**第一个阶段**（AGENTS 构造顺序骨架里
 * "entt::registry（全部每帧 context）"那一格）。
 *
 * 【为什么能整体搬出 main()，而别的阶段不能】本函数只做"往已经存在的 registry 里填东西"：
 * 15 条 emplace / wire*Context 调用，**不构造任何需要活在 main 作用域的对象**。
 * 🔴 反面上限：`main()` 里剩下的每一段几乎都是在声明 RAII 对象（AppContext / DeviceContext /
 * Swapchain / Resources / SceneDataService / SamplerLibrary / Environment / RenderTargetCache /
 * AssetTable / 两个簇容器 / ShaderRegistry / CompiledGraphResult / UploadedAssets /
 * MeshletAssembly / RenderBackEnd / GpuProfiler / ImGuiLayer / PickingService / DebugRpcServer），
 * 它们的**声明序就是析构序就是生命周期契约**（AGENTS 生命周期红线：挪进函数 = 函数返回即析构
 * = 帧循环读到已释放的 VA；RenderBackEnd 内部序见其注释）。所以本刀只搬这三段纯装配，
 * 骨架里的 RAII 声明一行都不动 —— 这就是"搬哪个阶段、留哪个在 main()"的依据。
 *
 * @param registry 刚构造好的场景 registry（批量 emplace 每帧 ctx 的唯一落点）
 * @param configs  启动三配置（读 camera / scene / runtime）
 * @param pre      建窗派生量（WindowExtent 初值 + 窗口指针，headless 窗口为空）
 */
void wireStartupContexts(entt::registry& registry, StartupConfigs& configs,
	const PreWindow& pre) {
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
}

/**
 * IMGUI 调试面板装配 = `main()` 的**调试面阶段之一**（与 wireHotReload / wirePicking 同族）。
 * 三段挂载（consume 挂点 + 帧末钩子 + 参数页构建器）自 main() 逐字搬来，短路顺序与门判据
 * 零变化（门关 / 无窗 / init 失败三条路都不进 if 体 = 与拆前一致降级不崩）。
 * 🔴 `imguiLayer` 本体**留在 main() 声明**（声明序契约 → 调用点注释，此处不复述），
 * 本函数只接它的**引用**做 init + 挂两个钩子 + 装参数页构建器。
 * @param imguiLayer main() 栈上的面板层，本函数就地 init
 * @param registry   场景 registry（写 InputFilter.consume 挂点）
 * @param backEnd    已装配的后端（写 Renderer 帧末钩子）
 * @param pre        建窗派生量（headless 门 + 窗口句柄）
 * @param configs    启动配置（读 runtime 的 uiFont / uiFontSize）
 * @param assetTable 资产表（参数页构建器的第二入参——imgui 层不认识 ECS 与资产表）
 */
void wireImGuiLayer(RGL::ImGuiLayer& imguiLayer, entt::registry& registry,
	RenderBackEnd& backEnd, AppContext& vkInstance, DeviceContext& vkDevice,
	Swapchain& vkSwapchain, const PreWindow& pre, const StartupConfigs& configs,
	AssetTable& assetTable) {
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
}

/**
 * DIAG 可见窗 fault 排查的每帧回调工厂 = `main()` 的**调试面阶段之二**。
 * 整段（含 `onFrame` 的声明形态）自 main() 逐字搬来，零行为变化；判据与解读口径
 * → 调用点注释，此处不复述。
 * @param pre 建窗派生量（diagShow 门 + showAfter 帧号 + 窗口句柄）
 * @return 每帧回调；门关 = 空 std::function（= 正常路径零变化）
 */
std::function<void(uint32_t)> makeDiagShowWindowCallback(const PreWindow& pre) {
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
	return onFrame;
}

}  // namespace RGL

#ifdef RGL_AFTERMATH
/** 崩溃转储挂钩（自 main() 抽出 = 装配层行数预算，纯搬移零行为变化；编译期开关须早于
 *  任何 device 创建 = 调用点仍在 main() 首行）。 */
void enableAftermath() {
	GFSDK_AFTERMATH_EnableGpuCrashDumps(GFSDK_AFTERMATH_Version_API,
		GFSDK_AFTERMATH_GpuCrashDumpWatchedApiFlags_Vulkan,
		GFSDK_AFTERMATH_GpuCrashDumpFeatureFlags_Default,
		rglAftermathDumpCb, nullptr, nullptr, nullptr, nullptr);
}
#endif

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
	enableAftermath();
#endif
	// 诊断账登记（🔴 第一件事：任何一条退出/崩溃路都要已经能结算，包括配置加载失败）
	RGL::registerDiagnosticReporters(RGL::shutdownLedger());
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
		// 阶段：registry 每帧 context 装配（WindowExtent/AppExit/SceneSpin/SceneEnv/TaaState/
		// SsrTuning/WetTuning/RenderOrigin + 一批空 ctx）。为什么这段能搬、RAII 声明段不能搬
		// → wireStartupContexts 注释。
		wireStartupContexts(registry, configs, pre);

		AppContext vkInstance(pre.window.get(), pre.headless);
		// 第二参 = diagnostics.fault_poll（masked fault 轮询门）。🔴 传的是**门**不是"驱动支持"：
		// 启用 deviceFaultReportMasked 后驱动可以内部恢复故障而不报 device lost，默认不开。
		DeviceContext vkDevice(vkInstance, diagnostics().faultPoll);
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
		//（那是"顶点/纹理已上传 GPU 的东西"，本容器只有 CPU 只读视图；理由全文 →
		// MeshletAssetStore 注释）。声明序紧随 assetTable：纯 CPU 数据无 GPU 生命周期契约。
		MeshletAssetStore meshletAssets;
		// 簇产物 GPU 侧持有者（★1-a0）：键与 meshletAssets 同源（模型路径），门开才有条目。
		// 🔴 声明序必须与 meshletAssets 同作用域且在其后（AGENTS 生命周期红线：元素是 VmaBuffer，
		// 放进 main() 局部块 = 块尾析构 = GPU 侧引用已释放的 VA）。本批无人消费，★1-a1 起
		// 若被 Renderer 引用，须按 assetTable 的同一条规则挪到 backEnd 之前声明。
		MeshletGeometryStore meshletGeometry;
		// 管线表（Phase 4 批次 2：ShaderRegistry，零 entt）：规格来自 configs/scene.lua；
		// 条目全按需建（画不画 = entity 有无，数据驱动）——模型条目 C1 期 addModelEntry、
		// 内建 cube 条目 cube 资产上传后 addCubeEntry、天空盒/光源条目各自前提满足时建。
		// 先于 Renderer 构造（Renderer 持引用）
		ShaderRegistry shaderRegistry(vkDevice, vkResources, sceneData, env, assetTable, configs.scene.pipelines);
		// Phase 6 批次 3：渲染意图图 = configs/render_graph.lua（选路见下）；
		// compileRenderGraph 校验 + barrier 布局链推导，任何图错误启动期
		// 报错退出（Renderer 构造校验器兜底执行器可解析性）。
		// 选路（2026-10-18 图变体收敛刀②）：文件有 attachments/passes = 自定义图，
		// 否则 = 内建 deferred；variant 枚举与 forward 图本体已删（docs/archive/图变体收敛_plan.md）。
		// 批 14.6：TAA 图形态与 FSR 开关都是 aa 三态推导（原 scene.taaEnabled / fsr_upscaler
		// 键已删）；velocityField 在 aa=fsr 时已被解析期隐含打开（单一来源在解析处）。
		// 批 15.1 前半：四个图形态开关（taa/velocity/fsr/ssr）改由 configs.runtime 一次传入
		// （派生点搬进 compileGraphOrThrow，见其函数头注释）。
		auto graphResult = compileGraphOrThrow(configPath("render_graph.lua").c_str(), configs.runtime);
		// 图来源/规模观测（排"配置没生效"类问题：pass 数与 configs/render_graph.lua 对账）
		if (auto* logger = RLLogger::getInstance())
			logger->info("RenderGraph: {} attachments / {} passes compiled",
				graphResult.graph.attachments.size(), graphResult.graph.passes.size());
		// 门组合自检（gate_lint 批，docs/archive/gate_lint_plan.md §3-C）
		runGateLint(configs.runtime, vkDevice, graphResult);
		// Renderer 构造下移至 deferred G-buffer RT / light 管线就绪之后（deferred 模式需在构造前
		// addLightEntry 供 Renderer 取 light 管线下标）；compiledGraph 此时保持存活（下方 move 进 Renderer）

		// Phase 5.5 契约检查（validateEnvironmentContract）已随刀②.2 删除——它吃的
		// styles 表与三条 forward 规格同刀消失（tombstone → main.cppm）。

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
			configs.runtime.meshletAssets, meshletAssets, meshletGeometry);
		if (configs.scene.environment.enabled)
			uploadEnvironment(vkResources, env, vkDevice, configs.scene, registry);  // 时序契约见定义处注释

		// 透明混合管线刀2：BLEND 图元的装配期对账（两条不过即拒启动）。🔴 **必须在 C1 之前**：
		// env 缺失时的真实崩溃点就是 C1 的 vkCreateShadersEXT（实测放在 C3 的对账根本没跑到）。
		validateBlendSupport(assets, env, graphResult.graph);
		// C1：管线条目烘焙（per **unique 模型**——instancing 共享条目；addModelEntry
		// 按 (规格,顶点 slot,纹理 slot) 组合去重兜底）
		bakePipelineEntries(assets, configs.scene, shaderRegistry, vkResources, samplers,
			assetTable, rtCache);
		// 克隆管线条目（Phase 10 批 10.5 picking / 10.7 highlight）：契约与门控见 helper
		// 注释；自 main() 抽出（★0-b2 装配层行数预算，纯代码搬移零行为变化）。
		addDiagnosticCloneEntries(shaderRegistry);
		// 簇化轴 ★1-a1ii-α：影子叠加装配（描述符接线 + 条目烘焙，顺序契约内聚在 helper）。
		// 时序 = 克隆段之后（picking/highlight 重放天然跳过）且先于 assembleBackEnd（GpuCull
		// 条目数快照 = 容量语义，多几条无害）。门关 = 两步各自首行早退（零条目零日志）。
		// ★1-a2-i / ★M-1a：返回装配持有者（🔴 所有权在装配层，声明序 = 本行 = backEnd 之前
		// ⟹ 析构序在其后；两格各持 GPU buffer，Renderer 只持裸指针，块尾析构 = 帧循环读到已销毁的 VA）。
		auto meshletAssembly = wireMeshletOverlay(vkResources, vkDevice, meshletGeometry, assets,
			configs.scene, shaderRegistry, assetTable, samplers, rtCache, configs.runtime, sceneData);
		// C2/C2b：场景实体注册（模型/cube/灯）+ 实例矩阵槽容量契约
		registerSceneEntities(registry, assets, configs.scene, vkResources, assetTable, shaderRegistry);
		// 簇化轴 ★1-a1i + ★1-a1ii-β-1：簇区间表装配 + 原位取代开关落地（**必须在实体注册之后**——
		// 两步都读 MeshRef，且开关的逐资产对账要读区间表；顺序契约内聚在 helper）。门关 = 两步
		// 各自首行返回，ctx 不建条目 ⟹ scene data 尾区 @724224 全 0（逐位不变）。
		// ★1-a2-i：扫出来的剔除项表就地交给 cull 服务（cullBase 与 items 同生共死）；
		// ★M-1a：第四步就地建 MDI 行表载体（容量读的是本函数 emplace 的选级表 = 生效事实）。
		wireMeshletPostRegistration(registry, assets, meshletAssets, assetTable, shaderRegistry,
			vkResources, configs.runtime, vkDevice, meshletAssembly);

		// C3 GPU culling 候选表 + GpuCull + deferred G-buffer/HDR + DGC + Renderer
		//（装配时序契约见 assembleBackEnd 接口注释；graph 在此 move 进 Renderer）
		auto backEnd = assembleBackEnd(vkDevice, vkSwapchain, vkResources, sceneData, samplers, env, rtCache, assetTable, shaderRegistry,
			std::move(graphResult), registry,
			// 批 14.6：FSR 开关 = aa 三态推导（原 fsr_upscaler 键已删）；两枚调参留在 fsr 块
			configs.runtime.aaWantsFsr(), configs.runtime.fsrTemporal, configs.runtime.fsrJitter);
		// 簇化轴 ★0.5-b2 / ★1-a2-i / ★M-1b：「Renderer 装配后挂」三连（coplanar 双臂 INFO +
		// 逐簇剔除生效档 + MDI 行表载体指针；顺序契约与各自早退口径 → main:meshlets 定义处）。
		// 🔴 必须在 assembleBackEnd 之后（Renderer 已构造，与 wireMeshletOverlay 的「之前」
		// 契约互补）。门关 = 零调用零日志；门开 = INFO 打印生效档（四臂假绿的第二道机器护栏）。
		wireMeshletRendererServices(registry, backEnd.renderer, configs.runtime, meshletAssembly);

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
		// 本行只留声明（析构序契约），init + 两个钩子 + 参数页构建器在 wireImGuiLayer。
		RGL::ImGuiLayer imguiLayer;
		wireImGuiLayer(imguiLayer, registry, backEnd, vkInstance, vkDevice, vkSwapchain,
			pre, configs, assetTable);

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

		// HTTP POST RPC 调试通道（Phase 10b 批 11b.2，docs/archive/phase10b_plan.md）：门 =
		// diagnostics.rpc（默认关；与 imgui 门独立，headless 也可用）。门关 / bind 失败
		// 都降级 nullptr。声明序在 registry/assets/backEnd 之后 = 析构序其先 →
		// try 块退出（帧循环已结束，configReloadTick 不会再跑）时 ~DebugRpcServer =
		// stop() 收口：关监听线程、在途请求全 503，闭包里的裸服务指针绝不越权。
		auto debugRpc = wireDebugRpc(registry, assetTable, backEnd.renderer, configs.runtime,
			pre.llmEnabled);

		/**
		 * DIAG（可见窗 fault 排查）每帧回调：隐藏窗启动、frame == showAfter 时
		 * SDL_ShowWindow（恰好一次）——若显示瞬间开始 fault = present/合成期状态
		 * 触发；若全程不 fault = 可见性非根因。headless 或 showAfter < 0 时为空
		 * 回调（正常路径零变化）。
		 */
		auto onFrame = makeDiagShowWindowCallback(pre);
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
		exitDiagnostics();
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
