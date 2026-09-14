#include <entt/entt.hpp>
#include <SDL3/SDL_video.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // entity rootTransform（translate/rotate/scale 组装）
#include <fmt/format.h>
#include <filesystem>  // environment.hdr 本地素材存在性探测（GMF 例外清单成员）
#ifdef RGL_AFTERMATH  // 编译期开关（rgl/CMakeLists.txt option）：NVIDIA GPU crash dump 诊断
#include "GFSDK_Aftermath_GpuCrashDump.h"
#include "GFSDK_Aftermath_GpuCrashDumpDecoding.h"
#include <cstdio>
namespace {
// Aftermath GPU crash dump 回调：崩溃时驱动回调本函数，落盘 .bin + 解码 .json（供离线分析）。
// 须早于任何 device 创建启用（见 main 顶部 EnableGpuCrashDumps）。
void GFSDK_AFTERMATH_CALL rglAftermathDumpCb(const void* data, uint32_t size, void*) {
	std::FILE* f = std::fopen("aftermath_crash.bin", "wb");
	if (f) { std::fwrite(data, 1, size, f); std::fclose(f); }
	std::fprintf(stderr, "[aftermath] crash dump %u bytes\n", size);
	GFSDK_Aftermath_GpuCrashDump_Decoder dec = nullptr;
	if (GFSDK_Aftermath_SUCCEED(GFSDK_Aftermath_GpuCrashDump_CreateDecoder(
		    GFSDK_Aftermath_Version_API, data, size, &dec))) {
		uint32_t jsonSize = 0;
		GFSDK_Aftermath_GpuCrashDump_GenerateJSON(dec, 0x3fff, 2,
			nullptr, nullptr, nullptr, nullptr, &jsonSize);
		if (jsonSize > 0) {
			std::vector<char> buf(jsonSize);
			GFSDK_Aftermath_GpuCrashDump_GetJSON(dec, jsonSize, buf.data());
			std::FILE* jf = std::fopen("aftermath_crash.json", "wb");
			if (jf) { std::fwrite(buf.data(), 1, jsonSize, jf); std::fclose(jf); }
		}
		GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(dec);
	}
}
}  // namespace
#endif
import rlstd;
import vulkan_window;
import vk_instance;
import vk_device;
import vk_swapchain;
import vk_resources;
import asset_table;
import shader_registry;
import vk_renderer;
import frame_draw_list;
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
import llm_debug;
import ecs;
import rllogger;
import vkerror;
using namespace RGL;
/**
 * 启动流主函数（逐段契约见体内块注释；全景对照 AGENTS.md "main.cpp 启动流" 节）：
 * 加载四配置（camera/scene/runtime/diagnostics；setDiagnostics 必须先于一切 GPU
 * 对象构造）→ renderGraphSelfCheck（纯 CPU 图算法回归守卫）→ 解析 headless/llm
 * 尺寸（headless+llm 时 headless_size 对齐 llm_shot.size）→ 建窗（headless 不建
 * SDL 窗，surface 走 vkCreateHeadlessSurfaceEXT）→ try 内：entt::registry（全部
 * 每帧 context）→ 初始化层 AppContext→DeviceContext（push data 容量契约检查）→
 * Swapchain→Resources→ShaderRegistry → 渲染图配置加载 + compileRenderGraph →
 * IBL 风格契约检查 + uploadEnvironment 闭包定义 → entities 四段循环（A0 内建 cube
 * → A1 解析+顶点上传 → A2 纹理 → env 上传（全部纹理槽后、addModelEntry 前）→
 * C1 管线条目烘焙 → C2 实体注册 + 矩阵容量契约 → cube entity → skybox 条目）→
 * deferred 装配（G-buffer 堆槽 + light 条目，须先于 Renderer 构造）→ Renderer →
 * renderSystem（llm 截图回调 + onFrame DIAG 回调）。
 *
 * 长生命周期对象全部 RAII（按声明逆序析构）；任何启动期错误 = 日志 + return -1。
 * @return 0 = 正常结束（含 llm 截图完成后退出）；-1 = 配置/自检/建窗/图编译/
 *   资产上传/渲染错误
 */
int main() {
#ifdef RGL_AFTERMATH  // 编译期开关：须早于任何 device 创建（Aftermath 文档要求）
	GFSDK_Aftermath_EnableGpuCrashDumps(GFSDK_Aftermath_Version_API,
		GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
		GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default,
		rglAftermathDumpCb, nullptr, nullptr, nullptr, nullptr);
#endif
	// 配置先于窗口：llm_debug 模式影响建窗（离屏隐藏窗 + 配置尺寸）
	auto cameraResult = loadCameraConfig("configs/camera.lua");
	if (!cameraResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", cameraResult.error().describe());
		return -1;
	}

	auto sceneResult = loadSceneConfig("configs/scene.lua");
	if (!sceneResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", sceneResult.error().describe());
		return -1;
	}
	// 2026-09-08 env→配置：runtime.lua（运行/窗口模式）+ diagnostics.lua（诊断开关单例）。
	// setDiagnostics 必须先于一切 GPU 对象构造（vkinstance 建 instance 时读 no_vvl）
	auto runtimeResult = loadRuntimeConfig("configs/runtime.lua");
	if (!runtimeResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", runtimeResult.error().describe());
		return -1;
	}
	auto diagResult = loadDiagnosticsConfig("configs/diagnostics.lua");
	if (!diagResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", diagResult.error().describe());
		return -1;
	}
	setDiagnostics(*diagResult);
	auto sceneConfig = std::move(*sceneResult);
	auto cameraConfig = std::move(*cameraResult);
	auto runtimeConfig = std::move(*runtimeResult);
	const bool llmEnabled = runtimeConfig.llm.enabled;

	// Phase 6 批次 2：渲染图编译器自检（无 GPU，纯 CPU 图算法回归守卫）——失败 =
	// 编译器/默认图被改坏，启动期报错退出（早于建窗，headless/llm 路径同样被覆盖）
	{
		auto failures = renderGraphSelfCheck();
		if (!failures.empty()) {
			auto* logger = RLLogger::getInstance();
			for (const auto& f : failures)
				if (logger) logger->error("RenderGraph self-check: {}", f);
			return -1;
		}
	}

	// headless（VK_EXT_headless_surface）：脱离 SDL/compositor——runtime.lua headless=true
	// 时不建 SDL 窗口，surface 经 vkCreateHeadlessSurfaceEXT 创建（离屏渲染/CI 截图，无需显示）。
	// 尺寸 headless_size={W,H}（默认 1280x720）。headless 下 llm_debug 截图直接读
	// swapchain image 回读（compositor 无关）
	const bool headless = runtimeConfig.headless;
	uint32_t headlessW = runtimeConfig.headlessWidth;
	uint32_t headlessH = runtimeConfig.headlessHeight;
	// 截图尺寸契约（2026-09-08 帧内回读：截图 = swapchain extent）：headless+llm 时
	// headless_size 对齐 llm_shot.size——与非 headless llm 按 llm 尺寸建窗（下方建窗段）同语义
	if (headless && llmEnabled) {
		headlessW = runtimeConfig.llm.width;
		headlessH = runtimeConfig.llm.height;
	}
	// Phase 7：deferred 路线 = render_graph.lua variant="deferred"（exclusive，与 forward
	// 二选一）——deferredMode 在下方图配置加载后按 variant 求值（opaque 段用
	// deferred_geometry 管线 + light pass 采样 G-buffer，须在 env/models 槽后建 G-buffer RT）

	// 交互窗尺寸 win_size={W,H}（默认 800x600；llm 模式用 llm_shot.size，headless 用
	// headless_size）；show_after = DIAG（原 RGL_SHOW_AFTER env）：隐藏窗启动、第 N 帧后
	// 显示（可见窗 fault 排查；-1 = 立即显示）
	const int winW = static_cast<int>(runtimeConfig.winWidth);
	const int winH = static_cast<int>(runtimeConfig.winHeight);
	const int showAfter = runtimeConfig.showAfter;

	// headless 时不建 SDL 窗口（window=nullptr）；否则照旧
	std::unique_ptr<SDLWindow> window;
	if (!headless) {
		auto windowResult = SDLWindow::create(
			llmEnabled ? static_cast<int>(runtimeConfig.llm.width) : winW,
			llmEnabled ? static_cast<int>(runtimeConfig.llm.height) : winH,
			"RGL", llmEnabled || showAfter >= 0);
		if (!windowResult) {
			auto* logger = RLLogger::getInstance();
			if (logger) logger->error("错误: {}", windowResult.error().describe());
			return -1;
		}
		window = std::make_unique<SDLWindow>(std::move(*windowResult));
	} else {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->info("headless 模式：脱离 SDL/compositor（{}x{}）", headlessW, headlessH);
	}
	if (llmEnabled && !headless) {
		auto* logger = RLLogger::getInstance();
		if (logger)
			logger->info("llm_debug: 一次性截图模式（延迟 {} 帧后截图并退出）",
				runtimeConfig.llm.delayFrames);
	}

	try {
		entt::registry registry;
		// 窗口/应用状态（eventSystem 写）+ 每帧 system 的单例数据（context）：
		// WindowExtent/AppExit（ecs.cppm）、SceneSpin/SceneBounds/ViewProjection（scene.cppm）、
		// FrameDrawList（vkrenderer.cppm，渲染 system 产出、drawFrame 消费）、
		// InputEvents/CameraState（camera.cppm，输入事件流 + 相机自管 level 状态）
		registry.ctx().emplace<WindowExtent>(
			headless ? static_cast<uint32_t>(headlessW) : static_cast<uint32_t>(window->getWidth()),
			headless ? static_cast<uint32_t>(headlessH) : static_cast<uint32_t>(window->getHeight()));
		registry.ctx().emplace<AppExit>();
		registry.ctx().emplace<Camera>(std::move(cameraConfig.camera));
		registry.ctx().emplace<InputEvents>();
		registry.ctx().emplace<CameraState>();
		auto& spin = registry.ctx().emplace<SceneSpin>();
		spin.speed = sceneConfig.spin ? 1.0f : 0.0f;  // 0 = 静止（configs/scene.lua 的 spin 开关）
		registry.ctx().emplace<SceneBounds>();
		registry.ctx().emplace<ViewProjection>();
		registry.ctx().emplace<FrameDrawList>();
		registry.ctx().emplace<NextObjectSlot>();  // Phase 6 instancing：实例矩阵槽分配器
		registry.ctx().emplace<SceneEnv>();  // Phase 5.5：环境参数（env 上传成功后回填，见下）
		registry.ctx().get<SceneEnv>().exposure = sceneConfig.exposure;  // Phase 8 收尾：tonemap 曝光（与 env 开关无关）

		AppContext vkInstance(window.get(), headless);
		DeviceContext vkDevice(vkInstance);
		// push data 契约（Phase 5）：shader object + descriptor heap 模式无 pipeline layout →
		// 无 VkPushConstantRange，per-draw push data（DrawArgs 100B，vkCmdPushDataEXT，
		// vkrenderer.cppm drawFrame）受设备硬限约束（双驱动实测 256B）：
		// ① maxPushDataSize（descriptor heap 模式专属 limit，VUID-VkPushDataInfoEXT-offset-11243：
		//    offset+size ≤ 本值）② maxPushConstantsSize（经典 VkPhysicalDeviceLimits，
		//    descriptor set 模式上限）。超限 = 设备不支持，启动期报错优于运行时布局错配
		//（shader 从 push constant 读垃圾非校验错误，是静默画面错）
		const auto [maxPushConstants, maxPushData] = vkDevice.getPushDataLimits();
		if (maxPushConstants < sizeof(DrawArgs) || maxPushData < sizeof(DrawArgs))
			throw std::runtime_error(fmt::format(
				"push data limit (maxPushConstantsSize={}, maxPushDataSize={}) < sizeof(DrawArgs) ({})",
				maxPushConstants, maxPushData, sizeof(DrawArgs)));
		Swapchain vkSwapchain(vkDevice, window.get(), headless,
			static_cast<uint32_t>(headlessW), static_cast<uint32_t>(headlessH));
		// heap 容量为固定裕量（与场景内容解耦，批次 3 E1.3）：CPU 解析模型不再需要
		// 先于 Vulkan 初始化
		Resources vkResources(vkInstance, vkDevice, vkSwapchain);
		// 管线表（Phase 4 批次 2：ShaderRegistry，零 entt）：规格来自 configs/scene.lua；
		// 条目全按需建（画不画 = entity 有无，数据驱动）——模型条目 C1 期 addModelEntry、
		// 内建 cube 条目 cube 资产上传后 addCubeEntry、天空盒/光源条目各自前提满足时建。
		// 先于 Renderer 构造（Renderer 持引用）
		ShaderRegistry shaderRegistry(vkDevice, vkResources, sceneConfig.pipelines);
		// Phase 6 批次 3：渲染意图图 = configs/render_graph.lua（缺文件/缺段回落默认
		// 三段图）；compileRenderGraph 校验 + barrier 布局链推导，任何图错误启动期
		// 报错退出（Renderer 构造校验器兜底执行器可解析性）。
		// Phase 7 路线选路（2026-09-08 env→配置）：顶层 variant = "forward"(默认)/
		// "deferred"/"offscreen"——后两者为内建 exclusive 图变体（覆盖文件 passes）
		auto graphConfig = loadRenderGraphConfig("configs/render_graph.lua");
		if (!graphConfig) {
			auto* logger = RLLogger::getInstance();
			if (logger) logger->error("错误: {}", graphConfig.error().describe());
			return -1;
		}
		const bool deferredMode = graphConfig->variant == RgGraphVariant::deferred;
		auto compiledGraph = compileRenderGraph(graphConfig->desc);
		if (!compiledGraph) {
			auto* logger = RLLogger::getInstance();
			if (logger) logger->error("错误: render graph compile failed: {}",
				compiledGraph.error().describe());
			return -1;
		}
		// 图来源/规模观测（排"配置没生效"类问题：pass 数与 configs/render_graph.lua 对账）
		if (auto* logger = RLLogger::getInstance())
			logger->info("RenderGraph: {} attachments / {} passes compiled",
				compiledGraph->attachments.size(), compiledGraph->passes.size());
		// Renderer 构造下移至 deferred G-buffer RT / light 管线就绪之后（deferred 模式需在构造前
		// addLightEntry 供 Renderer 取 light 管线下标）；compiledGraph 此时保持存活（下方 move 进 Renderer）

		// Phase 5.5 契约：引用 model_pbr_ibl 风格必须配 environment——frag 声明的
		// binding 4/5 无 mapping 烘焙 = 采样未写描述符（内容未定义；VVL 会抓
		// "no mapping for envSampler"，但启动期明确报错优于运行时未定义）。
		// 检查对象只取 styleMap 的值（风格真正引用的规格）：pipelines 表里存在未被
		// 引用的 ibl 条目是合法配置（addModelEntry 按引用建条目，无引用 = 不建，
		// 条目永不生效）；子串 contains 匹配还会误伤 model_pbr_ibl_v2 类未来命名
		//（2026-09-03 教程拆解 ③：教程截图配置 styles.pbr="model_pbr" 但 pipelines
		// 保留 ibl 条目被旧检查误杀）
		if (!sceneConfig.environment.enabled) {
			std::string iblStyles;
			for (const auto& [style, specName] : sceneConfig.styleMap)
				if (specName == "model_pbr_ibl" || specName == "model_vegetation"
					|| specName == "model_light")
					iblStyles += (iblStyles.empty() ? "" : ", ") + style;
			if (!iblStyles.empty())
				throw std::runtime_error(fmt::format(
					"styles [{}] reference model_pbr_ibl but scene.lua 'environment' is disabled "
					"(env descriptor unmapped = undefined sampling)", iblStyles));
		}
		/**
		 * Phase 5.5 环境 cubemap 上传（scene.lua environment 段，faces 按 Vulkan 层序
		 * 0-5，定案表 docs/archive/phase5_5_plan.md）+ SceneEnv 回填（intensity/mipRange）。
		 * **调用时序契约**：必须在模型顶点槽 + 纹理槽全部按布局分配完之后
		 *（allocSlot 单调游标——插在之前会挤占白纹理契约槽
		 * "reserved + 2 + 模型顶点槽数"，断言漂移启动报错，2026-09-01 实测踩坑），
		 * 且先于 addModelEntry（其查 getEnvTexture 烘焙 binding 4/5）。
		 *
		 * Phase 8 批次 ②b：`environment.hdr` 非空 = 优先走 Radiance .hdr 路径（CPU
		 * 等距柱状→RGBA16F cubemap 重投影）；.hdr 属 gitignore 本地素材，文件不存在时
		 * 告警并回落 faces 六面 PNG 路径（干净 checkout 仍可运行）。
		 * @note 文件读取 / cubemap 创建失败 → 抛 std::runtime_error
		 */
		auto uploadEnvironment = [&] {
			auto& envCfg = sceneConfig.environment;
			if (!envCfg.hdr.empty()) {
				std::error_code ec;
				if (std::filesystem::exists(envCfg.hdr, ec) && !ec) {
					auto hdrResult = loadEnvCubeMapHdr(vkResources, vkDevice, envCfg.hdr,
						envCfg.hdrSize, "scene.environment");
					if (!hdrResult)
						throw std::runtime_error(
							fmt::format("environment: {}", hdrResult.error().describe()));
					const auto& envAsset = *vkResources.getEnvTexture();
					auto& envCtx = registry.ctx().get<SceneEnv>();
					envCtx.intensity = envCfg.intensity;
					envCtx.mipRange = static_cast<float>(envAsset.mipLevels - 1);
					return;
				}
				if (auto* logger = RLLogger::getInstance())
					logger->warn("environment.hdr '{}' 不存在（本地素材），回落 faces PNG 路径",
						envCfg.hdr);
			}
			std::array<std::string, 6> paths;
			for (uint32_t f = 0; f < 6; ++f)
				paths[f] = fmt::format("{}/{}", envCfg.dir, envCfg.faces[f]);
			auto envResult = loadEnvCubeMapFiles(vkResources, vkDevice, paths, "scene.environment");
			if (!envResult)
				throw std::runtime_error(fmt::format("environment: {}", envResult.error().describe()));
			const auto& envAsset = *vkResources.getEnvTexture();
			auto& envCtx = registry.ctx().get<SceneEnv>();
			envCtx.intensity = envCfg.intensity;
			envCtx.mipRange = static_cast<float>(envAsset.mipLevels - 1);
		};

		// CPU 解析 + GPU 上传 + 纹理 + ECS 注册：资产数据（buffer/slot/三角形数/管线条目引用、
		// 纹理资产、材质资产）全部在 Resources 资产表（初始化层 owner，生命周期不依赖
		// main 作用域——TexturePool 局部对象 UAF 教训，docs/archive/heap_texture_research.md §12.6/§12.7）
		// Phase 6 批次 5：scene.lua entities 列表 = 场景构成数据源（画不画 = entity 有无，
		// ECS 规范 3；缺段兼容合成见 scene_config loader）。三段时序：A 解析 + GPU 上传
		// 全部模型（顶点/纹理槽 allocSlot 单调分配）→ B env 上传（**时序契约 = 全部纹理
		// 槽后、管线烘焙前**，挤占白纹理契约槽 = 启动断言，2026-09-01 踩坑）→ C 管线烘焙
		// + ECS 注册。CPU 解析产物（GltfModel）活到 C（unique_ptr 稳定地址）；资产数据
		// （buffer/slot/纹理/材质）全部在 Resources 资产表（初始化层 owner——TexturePool
		// 局部对象 UAF 教训，docs/archive/heap_texture_research.md §12.6/§12.7）
		/**
		 * 实体 rootTransform 组装（scene.lua entities 的 transform 字段 → 4x4 世界矩阵）：
		 * 先 translate(position)，再按 X→Y→Z 顺序依次 glm::rotate 后乘（rotation 为度，
		 * 手动转弧度——GLM_FORCE_RADIANS 下 radians() 恒等），最后 scale 后乘——
		 * 结果 = T·Rx·Ry·Rz·S（旋转应用序 X→Y→Z，同 SceneEntityDesc 契约）。
		 * @param ent entity 声明（position/rotation/scale）
		 * @return 实体世界变换（registerScene/registerBuiltinCube 写入 Transform 组件）
		 */
		auto makeRootTransform = [](const SceneEntityDesc& ent) {
			glm::mat4 t = glm::translate(glm::mat4(1.0f), ent.position);
			t = glm::rotate(t, glm::radians(ent.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
			t = glm::rotate(t, glm::radians(ent.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
			t = glm::rotate(t, glm::radians(ent.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
			return glm::scale(t, glm::vec3(ent.scale));
		};
		/**
		 * Phase 6 instancing：entities 按 **file 去重**——同文件多 entity = 一次解析
		 * 一次上传（顶点/纹理/管线条目全部共享），每 entity 各注册一组 primitive
		 * entity（独立 Transform + 独立实例矩阵槽 objectSlot）。资产与实例分离
		 *（ECS 规范 5）：上传去重省显存，绘制仍是 per-entity per-primitive draw
		 *（真 GPU instancing——drawMeshTasksEXT 实例维 + 矩阵表批量消费——留 Phase 7
		 * 与 culling 一并议，本批先把共享资产的地基打对）。
		 */
		struct UploadedModel {
			std::string file;
			std::unique_ptr<GltfModel> model;
			std::vector<uint32_t> assetIds;    // per-primitive 网格资产 id
			ModelTextureIds textureIds;        // per-primitive 纹理资产 id 四元组（base/normal/orm/emissive，Phase 8 ③）
			uint32_t instances = 0;            // entities 引用计数（日志）
		};
		std::vector<UploadedModel> uploaded;
		std::map<std::string, size_t> modelOf;  // file → uploaded 下标
		// A0：内建几何（架构债务 1 方案 B）——entities 里有 cube 就上传内建 cube 资产，
		// 与普通模型的顶点槽同属"几何段"（先于任何纹理槽）；先于模型循环上传 = 与旧
		// "Resources 构造期占 slot 0/1"一致的槽序（截图回归基线可比）
		std::optional<uint32_t> cubeAsset;
		for (const auto& ent : sceneConfig.entities) {
			if (ent.type != "cube" || cubeAsset)
				continue;
			auto cubeResult = uploadBuiltinCube(vkDevice, vkResources);
			if (!cubeResult) {
				auto* logger = RLLogger::getInstance();
				logger->error("错误: {}", cubeResult.error().describe());
				return -1;
			}
			cubeAsset = *cubeResult;
		}
		// A1：解析 + 顶点上传（**全部模型的顶点槽先于任何纹理槽分配**——布局契约
		// "几何槽先于纹理槽"（vkresources.cppm），N = 全模型 chunk + 内建几何槽；
		// 交错分配 = 第二个模型首 chunk slot 期望漂移，批次 5 多模型实测踩坑）
		for (const auto& ent : sceneConfig.entities) {
			if (ent.type != "model")
				continue;
			if (auto [it, inserted] = modelOf.emplace(ent.file, uploaded.size()); !inserted) {
				uploaded[it->second].instances++;
				continue;  // 同文件已上传（instancing 共享资产）
			}
			auto modelResult = loadGltfModel(ent.file);
			if (!modelResult) {
				auto* logger = RLLogger::getInstance();
				logger->error("错误: {}", modelResult.error().describe());
				return -1;
			}
			auto mesh = std::make_unique<GltfModel>(std::move(*modelResult));
			auto assetIdsResult = uploadModelMeshes(vkDevice, vkResources, *mesh);
			if (!assetIdsResult) {
				auto* logger = RLLogger::getInstance();
				logger->error("错误: {}", assetIdsResult.error().describe());
				return -1;
			}
			uploaded.push_back(UploadedModel{.file = ent.file, .model = std::move(mesh),
				.assetIds = std::move(*assetIdsResult), .instances = 1});
		}
		// A2：纹理上传（Phase 3）：全部模型顶点槽分配完后再上传（先取白纹理——布局契约
		// "纹理首槽 = 白"，ensureWhiteTexture 带契约断言）；产物在资产表
		for (auto& um : uploaded) {
			auto textureIdsResult = uploadModelTextures(vkResources, vkDevice, *um.model);
			if (!textureIdsResult) {
				auto* logger = RLLogger::getInstance();
				logger->error("错误: {}", textureIdsResult.error().describe());
				return -1;
			}
			um.textureIds = std::move(*textureIdsResult);
		}
		if (sceneConfig.environment.enabled)
			uploadEnvironment();  // 时序契约见定义处注释（全部模型纹理槽后、管线烘焙前）

		// C1：管线条目烘焙（per **unique 模型**——instancing 共享条目；addModelEntry
		// 按 (规格,顶点 slot,纹理 slot) 组合去重兜底）
		for (auto& um : uploaded) {
			auto& model = *um.model;
			const auto& texIds = um.textureIds;
			auto& assets = vkResources.getAssetTable();
			const uint32_t samplerSlot = vkResources.ensureSharedSampler();
			for (size_t i = 0; i < um.assetIds.size(); ++i) {
				const uint32_t id = um.assetIds[i];
				// 管线条目（Phase 4 批次 2：按 (规格, 顶点 slot, base 纹理 slot, normal slot,
				// orm slot, emissive slot) 六元组组合去重——同模型全 primitive 共享一个拼接顶点
				// slot，仅纹理槽差异决定条目拆分；纹理/sampler slot 烘焙进 binding mapping，每帧
				// 零成本；Phase 8 ③a 扩 normal/orm 槽、③ 扩 emissive 槽）。批次 3：风格派生（loader unlit
				// 字段即数据源，与 MaterialAsset 一致性由 registerScene 金丝雀保证）→ scene.lua
				// styles 表映射到管线规格名（缺失 = 启动报错，同现有 config 错误路径）。
				// Phase 8：forward 路风格派生——植被（MASK+doubleSided 叶片卡）/ 光源（emissive
				// 非零）走各自专属 frag。判据纯数据驱动（loader 输出字段，零名字匹配）。deferred
				// 无视 styleMap（下方恒 deferred_geometry），style 仅求其在 styleMap 中存在，
				// 植被/光源在 deferred 下不派生新风格 → 逐位 D1 不变。
				const auto& prim = model.primitives[i];
				std::string style = prim.unlit ? "unlit" : "pbr";
				if (!deferredMode && !prim.unlit) {
					const bool isLight = prim.emissive.r > 0.0f || prim.emissive.g > 0.0f
						|| prim.emissive.b > 0.0f;
					if (isLight)
						style = "light";
					else if (prim.alphaModeMask && prim.doubleSided)
						style = "vegetation";
				}
				const auto styleIt = sceneConfig.styleMap.find(style);
				if (styleIt == sceneConfig.styleMap.end())
					throw std::runtime_error(fmt::format("style not in scene.lua styles: {}", style));
				const uint32_t entryIdx = shaderRegistry.addModelEntry(
					deferredMode ? std::string("deferred_geometry") : styleIt->second,
					assets.get(id).heapSlot,  // = 模型拼接顶点 slot（同模型全 primitive 相同）
					assets.getTexture(texIds.base[i]).heapSlot,
					assets.getTexture(texIds.normal[i]).heapSlot,
					assets.getTexture(texIds.orm[i]).heapSlot,
					assets.getTexture(texIds.emissive[i]).heapSlot,
					samplerSlot);
				shaderRegistry.bindToAsset(id, entryIdx);
			}
			// DIAG（configs/diagnostics.lua style_dist；Phase 4 批次 3 风格分布核查）：
			// per-primitive 风格（unlit/pbr）+ 规格名分布
			if (diagnostics().styleDist) {
				std::map<std::string, uint32_t> styleCount;
				for (size_t i = 0; i < um.assetIds.size(); ++i)
					++styleCount[model.primitives[i].unlit ? "unlit" : "pbr"];
				if (auto logger = RLLogger::getInstance()) {
					std::string t;
					for (const auto& [style, cnt] : styleCount)
						t += fmt::format("{}={} ", style, cnt);
					logger->info("DIAG style dist ({} x{}): {} primitives → {} styles: {}",
						um.file, um.instances, um.assetIds.size(), styleCount.size(), t);
				}
			}
			// DIAG（configs/diagnostics.lua tex_dist；Phase 4 批次 2 纹理分布核查）：
			// per-primitive 纹理 slot 分布（验证新旧管线用同一批纹理）
			if (diagnostics().texDist) {
				std::map<uint32_t, uint32_t> texSlotCount;
				for (size_t i = 0; i < um.assetIds.size(); ++i)
					++texSlotCount[assets.getTexture(texIds.base[i]).heapSlot];
				if (auto logger = RLLogger::getInstance()) {
					std::string t;
					for (const auto& [slot, cnt] : texSlotCount)
						t += fmt::format("slot{}={} ", slot, cnt);
					logger->info("DIAG tex dist ({} x{}): {} primitives → {} distinct tex slots: {}",
						um.file, um.instances, um.assetIds.size(), texSlotCount.size(), t);
				}
			}
		}
		// C2：场景实体注册（per **entity**——同文件各实例独立 rootTransform/矩阵槽；
		// 材质经 addMaterial find-or-add 去重，重复注册返回既有 id）
		for (const auto& ent : sceneConfig.entities) {
			if (ent.type != "model")
				continue;
			auto& um = uploaded[modelOf[ent.file]];
			registerScene(registry, *um.model, std::span<const uint32_t>(um.assetIds),
				std::span<const uint32_t>(um.textureIds.base), vkResources,
				makeRootTransform(ent));
		}
		// scene data 容量契约（Phase 5 批次 2，Phase 6 instancing 改**实例矩阵槽**计数
		//——资产共享后 meshSize < 槽数才是常态）：对象矩阵表按实例槽索引，超容量 =
		// 矩阵表越界写（staging 上传处有防御报错，此处启动期先拦截）
		if (const uint32_t slotCount = registry.ctx().get<NextObjectSlot>().next;
			slotCount > kSceneMatrixCapacity)
			throw std::runtime_error(fmt::format(
				"scene instances ({}) exceed object matrix capacity ({})",
				slotCount, kSceneMatrixCapacity));
		if (auto logger = RLLogger::getInstance())
			logger->info("instancing: {} unique models / {} primitive instances (matrix slots)",
				uploaded.size(), registry.ctx().get<NextObjectSlot>().next);

		// 内建 cube entity 注册（架构债务 1 方案 B：cube = 内建几何资产，与模型同路）：
		// 注册 entity（画不画 = 实体有无）+ 管线条目（按资产 slot 烘焙，cube 无纹理/无 scene
		// data）+ 下标写回（启动期一次，幂等）。cube 着色 = 每顶点色，MaterialRef 指向默认
		// 材质（baseColor 不被 cube 管线消费，见 registerBuiltinCube 注释）
		for (const auto& ent : sceneConfig.entities) {
			if (ent.type != "cube")
				continue;
			auto& assets = vkResources.getAssetTable();
			const uint32_t cubeMat = assets.addMaterial(
				MaterialAsset{glm::vec4{1.0f}, kInvalidTextureId, false});
			registerBuiltinCube(registry, *cubeAsset, cubeMat, makeRootTransform(ent));
			const uint32_t cubeEntry = shaderRegistry.addCubeEntry(*cubeAsset);
			shaderRegistry.bindPipelineToAsset("cube", *cubeAsset);
			if (auto logger = RLLogger::getInstance())
				logger->info("cube entity: 资产 {} / 材质 {} / 管线下标 {}",
					*cubeAsset, cubeMat, assets.get(*cubeAsset).pipelineIndex);
			(void)cubeEntry;
		}

		// C2b：灯 entity 注册（Phase 7 灯 entity 化，ECS 规范 3：画不画 = 实体有无）。
		// 数据源 = sceneConfig.lights（scene_config 保证非空：lights 段缺 → 回落 [light]）。
		// 每灯一个 entity + Light 组件；index = lights 列表位（正序契约：index 0 = 主光
		// forward @0 头，全数组按 index 落槽 deferred）。灯无资产/上传/矩阵槽，逻辑内联最简。
		for (uint32_t i = 0; i < sceneConfig.lights.size(); ++i) {
			auto e = registry.create();
			auto& L = registry.emplace<Light>(e);
			L = sceneConfig.lights[i];
			L.index = i;
		}

		// C3：GPU culling 候选表（Phase 7b 批次 5）：candidate = 不透明 primitive。
		// 守卫与迭代序和 render_list CPU 路径**完全一致**（同 view、同守卫）——GPU 输出
		// 固定槽非紧凑，重放序与 CPU opaque 段序逐位一致（截图 BIT-IDENTICAL 前提）。
		// 透明段不入候选（CPU 排序路径不受 GPU culling 影响）。
		std::vector<CullEntry> cullEntries;
		registry.view<MeshRef, MaterialRef, CullSphere>().each(
			[&](const MeshRef& meshRef, const MaterialRef& matRef, const CullSphere& cull) {
				auto& assets = vkResources.getAssetTable();
				const auto& asset = assets.get(meshRef.asset);
				if (asset.pipelineIndex == kInvalidPipelineIndex)
					return;
				if (asset.groupCount == 0)
					return;
				const auto& mat = assets.getMaterial(matRef.material);
				if (mat.alphaModeBlend)
					return;
				cullEntries.push_back(CullEntry{
					cull.center, cull.radius, meshRef.objectSlot, asset.pipelineIndex,
					asset.groupCount, asset.triangleCount, asset.vertexOffset,
					mat.baseColor, glm::vec2(mat.roughness, mat.metallic)});
			});
		// 静态 shader DGC 路选路（spike，docs/radv_static_dgc_spike.md）：IES 路缺席
		// （非 graphicsIndirect）但全集含 MESH 且 diagnostics.dgcStatic 开 → per-entry 流
		// 随 GpuCull 分配（fanout kernel 输出）。N = 此刻管线表条目数——sky/light 条目在
		// 其后加入、非 candidate，DgcStaticCommands 必须用同一数保证流布局一致。
		const auto& dgcSup = vkDevice.getDgcSupport();
		const uint32_t dgcStaticEntryCount = static_cast<uint32_t>(shaderRegistry.entriesView().size());
		const bool dgcStaticEnabled = !dgcSup.graphicsIndirect && dgcSup.staticGraphicsIndirect
			&& diagnostics().dgcStatic && !cullEntries.empty();
		GpuCull gpuCull(vkDevice, vkResources, cullEntries,
			dgcStaticEnabled ? dgcStaticEntryCount : 0u);

		// Phase 5.5 批次 4：天空盒管线条目（env 已上传才建——addSkyEntry 契约）+
		// SceneEnv 回填（renderListSystem 经 ctx 生成 skybox draw，画不画 = 配置有无）
		if (vkResources.getEnvTexture()) {
			auto& envCtx = registry.ctx().get<SceneEnv>();
			envCtx.skyboxPipeline = shaderRegistry.addSkyEntry();
			envCtx.skybox = sceneConfig.environment.skybox;
		}

		// （旧 showCube 直注册块已删：entities 缺段时 loader 从 cube 字段合成 entity
		//（scene_config_impl 缺段兼容），构成判定唯一经 entities 循环——ECS 规范 3）

		// Phase 7 D1：deferred 资源 + Renderer 构造。时序关键——env/models 全部堆槽已分配，此处才
		// 分配 G-buffer RT 堆槽（allocSlot 单调，早分配破坏布局契约）+ 建 light 管线，再构造
		// Renderer（其构造期校验 drawLight pass 有 light 条目）。forward 路径 deferredMode=false 全跳过。
		if (deferredMode) {
			auto gbSlots = vkResources.ensureDeferredGBufferSlots();
			if (!gbSlots) {
				auto* logger = RLLogger::getInstance();
				if (logger) logger->error("deferred: G-buffer RT 创建失败: {}", gbSlots.error().describe());
				return -1;
			}
			// light 条目下标由 Renderer 构造期按名 pipelineIndex("light") 取，此处仅需触发建条目
			[[maybe_unused]] auto lightIdx = shaderRegistry.addLightEntry(
				(*gbSlots)[0], (*gbSlots)[1], (*gbSlots)[2], (*gbSlots)[3], (*gbSlots)[4],
				vkResources.ensureSharedSampler());
		}
		// Phase 8 批次 ①：HDR 累加链——scene_hdr RT + tonemap 条目（forward/deferred 图都含
		// tonemap pass，无条件建；offscreen DIAG 变体图不含 = 条目闲置无害）。时序同
		// G-buffer：几何/纹理/env（deferred 含 G-buffer）槽之后、DGC/Renderer 之前
		//（allocSlot 单调游标契约）。
		{
			auto hdrSlot = vkResources.ensureSceneHdrSlot();
			if (!hdrSlot) {
				auto* logger = RLLogger::getInstance();
				if (logger) logger->error("scene_hdr RT 创建失败: {}", hdrSlot.error().describe());
				return -1;
			}
			// tonemap 条目下标由 Renderer 构造期按名 pipelineIndex("tonemap") 取
			[[maybe_unused]] auto tonemapIdx = shaderRegistry.addTonemapEntry(
				*hdrSlot, vkResources.ensureSharedSampler());
		}
		// Phase 7b：DGC 命令生成（NVIDIA 路）：仅 graphicsIndirect 能力时构造（RADV =
		// compute only → null → Renderer 读回重放路）。管线表定后建（execution set
		// 容量 = 2×entries）；构造异常（容量超设备上限等）回落重放路
		std::unique_ptr<DgcCommands> dgc;
		if (vkDevice.getDgcSupport().graphicsIndirect && gpuCull.active()) {
			try {
				dgc = std::make_unique<DgcCommands>(vkDevice, shaderRegistry,
					gpuCull.candidateCount(), 2);  // framesInFlight=2（preprocess 槽 ×2，11142）
			} catch (const std::exception& e) {
				auto* logger = RLLogger::getInstance();
				if (logger)
					logger->warn("dgc: 构造失败回落读回重放: {}", e.what());
			}
		}
		// 静态 shader DGC 路（RADV spike）：IES 未建且门控开时构造；异常回落读回重放
		// （流 buffer 已随 GpuCull 分配但无人消费——仅多占显存，像素路径无影响）
		std::unique_ptr<DgcStaticCommands> dgcStatic;
		if (dgcStaticEnabled && gpuCull.active() && !dgc) {
			try {
				dgcStatic = std::make_unique<DgcStaticCommands>(vkDevice, shaderRegistry,
					gpuCull.candidateCount(), dgcStaticEntryCount, 2);
			} catch (const std::exception& e) {
				auto* logger = RLLogger::getInstance();
				if (logger)
					logger->warn("dgc static: 构造失败回落读回重放: {}", e.what());
			}
		}
		// 清晰选路日志（启动期一次）：哪个设备走 DGC execute / 静态 execute / 读回重放
		if (auto* logger = RLLogger::getInstance()) {
			std::string devName(vkDevice.getPhysicalDevice().getProperties().deviceName.data());
			if (dgc && dgc->active())
				logger->info("DGC path: device='{}' graphicsIndirect={} computeIndirect={} static={} -> DGC execute (vkCmdExecuteGeneratedCommandsEXT)",
					devName, dgcSup.graphicsIndirect, dgcSup.computeIndirect, dgcSup.staticGraphicsIndirect);
			else if (dgcStatic && dgcStatic->active())
				logger->info("DGC path: device='{}' graphicsIndirect={} computeIndirect={} static={} -> DGC static execute ({} entries × vkCmdExecuteGeneratedCommandsEXT)",
					devName, dgcSup.graphicsIndirect, dgcSup.computeIndirect, dgcSup.staticGraphicsIndirect,
					dgcStatic->entryCount());
			else
				logger->info("DGC path: device='{}' graphicsIndirect={} computeIndirect={} static={} -> 读回重放 (CPU 回退)",
					devName, dgcSup.graphicsIndirect, dgcSup.computeIndirect, dgcSup.staticGraphicsIndirect);
		}
		Renderer vkRenderer(vkDevice, vkSwapchain, vkResources, shaderRegistry,
			std::move(*compiledGraph), &gpuCull, dgc.get(), dgcStatic.get());

		/**
		 * DIAG（可见窗 fault 排查）每帧回调：隐藏窗启动、frame == showAfter 时
		 * SDL_ShowWindow（恰好一次）——若显示瞬间开始 fault = present/合成期状态
		 * 触发；若全程不 fault = 可见性非根因。headless 或 showAfter < 0 时为空
		 * 回调（正常路径零变化）。
		 */
		std::function<void(uint32_t)> onFrame;
		if (showAfter >= 0 && !headless) {
			SDL_Window* sdlWindow = *window;
			onFrame = [sdlWindow, showAfter](uint32_t frame) {
				if (static_cast<int>(frame) == showAfter) {
					auto* logger = RLLogger::getInstance();
					if (logger) logger->info("DIAG: frame {} → SDL_ShowWindow", frame);
					SDL_ShowWindow(sdlWindow);
				}
			};
		}
		// llm 截图消费回调（ecs 不 import llm_debug——依赖图叶子解耦，2026-09-02）：
		// main 是唯一知道 writeCapturePng 的地方，config 已按值绑进 lambda。截图内容 =
		// drawFrame 帧内回读的本帧真实输出（vkrenderer FrameCapture，2026-09-08 离屏重渲退役）
		auto renderResult = RGL::renderSystem(registry, vkRenderer,
			llmEnabled ? &runtimeConfig.llm : nullptr,
			[&](RGL::Renderer& r) {
				return RGL::writeCapturePng(r, runtimeConfig.llm);
			},
			onFrame);
		if (!renderResult) {
			auto* logger = RLLogger::getInstance();
			if (logger) logger->error("错误: {}", renderResult.error().describe());
			return -1;
		}
	} catch (const std::exception& e) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", e.what());
		return -1;
	}
	return 0;
}
