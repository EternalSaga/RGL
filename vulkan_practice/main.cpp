#include <entt/entt.hpp>
#include <SDL3/SDL_video.h>
#include <glm/glm.hpp>
#include <fmt/format.h>
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
import scene_config;
import gltf_loader;
import model_importer;
import texture_pool;
import scene;
import material_types_generated;  // DrawArgs（push constant 尺寸契约检查）
import camera;
import camera_config;
import llm_debug;
import ecs;
import rllogger;
import vkerror;
using namespace RGL;
int main() {
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
	auto sceneConfig = std::move(*sceneResult);
	auto cameraConfig = std::move(*cameraResult);
	const bool llmEnabled = cameraConfig.llm.enabled;

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

	// headless（VK_EXT_headless_surface）：脱离 SDL/compositor——RGL_HEADLESS=1 时不建
	// SDL 窗口，surface 经 vkCreateHeadlessSurfaceEXT 创建（离屏渲染/CI 截图，无需显示）。
	// 尺寸 RGL_HEADLESS_SIZE="WxH"（默认 1280x720）。headless 下 llm_debug 截图直接读
	// swapchain image 回读（compositor 无关）
	const bool headless = std::getenv("RGL_HEADLESS") != nullptr;
	int headlessW = 1280, headlessH = 720;
	if (const char* hs = std::getenv("RGL_HEADLESS_SIZE")) {
		if (std::sscanf(hs, "%dx%d", &headlessW, &headlessH) != 2) {
			std::fprintf(stderr, "RGL_HEADLESS_SIZE 解析失败: %s\n", hs);
			return -1;
		}
	}

	// llm_debug 模式：隐藏窗（永不显示，compositor 须存活）+ 配置尺寸；否则交互窗 800x600
	// DIAG（可见窗 GPUVM fault 排查 2026-08-28）：RGL_WIN_SIZE="WxH" 覆盖交互窗尺寸
	//（免重编译二分可见窗的尺寸阈值，同 RGL_NO_HDR / TEXTURE_FORCE_* 门模式）
	int winW = 800, winH = 600;
	int showAfter = -1;  // DIAG：隐藏窗启动、第 N 帧后显示（可见窗 fault 排查；-1 = 立即显示）
	if (const char* winSize = std::getenv("RGL_WIN_SIZE")) {
		if (std::sscanf(winSize, "%dx%d", &winW, &winH) != 2) {
			std::fprintf(stderr, "RGL_WIN_SIZE 解析失败: %s\n", winSize);
			return -1;
		}
	}
	if (const char* sa = std::getenv("RGL_SHOW_AFTER"))
		showAfter = std::atoi(sa);

	// headless 时不建 SDL 窗口（window=nullptr）；否则照旧
	std::unique_ptr<SDLWindow> window;
	if (!headless) {
		auto windowResult = SDLWindow::create(
			llmEnabled ? static_cast<int>(cameraConfig.llm.width) : winW,
			llmEnabled ? static_cast<int>(cameraConfig.llm.height) : winH,
			"Vulkan Practice", llmEnabled || showAfter >= 0);
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
				cameraConfig.llm.delayFrames);
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
		registry.ctx().emplace<Light>(sceneConfig.light);  // Phase 5 批次 2：定向光（scene.lua light 段）
		registry.ctx().emplace<SceneEnv>();  // Phase 5.5：环境参数（env 上传成功后回填，见下）

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
		// "cube" 条目恒建（批次 3：画不画 = cube entity 有无，数据驱动，下方按 showCube
		// 决定是否注册）。先于 Renderer 构造（Renderer 持引用）
		ShaderRegistry shaderRegistry(vkDevice, vkResources, sceneConfig.pipelines);
		// Phase 6 批次 2：编译图（批次 3 前 = 默认三段图，与旧 drawFrame 硬编码语义等价；
		// 届时换成 configs/render_graph.lua 加载 + compileRenderGraph）
		Renderer vkRenderer(vkDevice, vkSwapchain, vkResources, shaderRegistry,
			defaultRenderGraph());

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
				if (specName == "model_pbr_ibl")
					iblStyles += (iblStyles.empty() ? "" : ", ") + style;
			if (!iblStyles.empty())
				throw std::runtime_error(fmt::format(
					"styles [{}] reference model_pbr_ibl but scene.lua 'environment' is disabled "
					"(env descriptor unmapped = undefined sampling)", iblStyles));
		}
		// Phase 5.5 环境 cubemap（scene.lua environment 段，faces 按 Vulkan 层序 0-5，
		// 定案表 docs/phase5_5_plan.md）。**调用时序契约**：必须在模型顶点槽 + 纹理槽
		// 全部按布局分配完之后（allocSlot 单调游标——插在之前会挤占白纹理契约槽
		// "reserved + 2 + 模型顶点槽数"，断言漂移启动报错，2026-09-01 实测踩坑），
		// 且先于 addModelEntry（其查 getEnvTexture 烘焙 binding 4/5）。
		auto uploadEnvironment = [&] {
			std::array<std::string, 6> paths;
			for (uint32_t f = 0; f < 6; ++f)
				paths[f] = fmt::format("{}/{}", sceneConfig.environment.dir,
					sceneConfig.environment.faces[f]);
			auto envResult = loadEnvCubeMapFiles(vkResources, vkDevice, paths, "scene.environment");
			if (!envResult)
				throw std::runtime_error(fmt::format("environment: {}", envResult.error().describe()));
			const auto& envAsset = *vkResources.getEnvTexture();
			auto& envCtx = registry.ctx().get<SceneEnv>();
			envCtx.intensity = sceneConfig.environment.intensity;
			envCtx.mipRange = static_cast<float>(envAsset.mipLevels - 1);
		};

		// CPU 解析 + GPU 上传 + 纹理 + ECS 注册：资产数据（buffer/slot/三角形数/管线条目引用、
		// 纹理资产、材质资产）全部在 Resources 资产表（初始化层 owner，生命周期不依赖
		// main 作用域——TexturePool 局部对象 UAF 教训，docs/heap_texture_research.md §12.6/§12.7）
		std::vector<uint32_t> primitiveTextureIds;  // per-primitive 纹理资产 id（管线烘焙 + 批次 2 材质注册消费）
		if (!sceneConfig.modelPath.empty()) {
			auto modelResult = loadGltfModel(sceneConfig.modelPath);
			if (!modelResult) {
				auto* logger = RLLogger::getInstance();
				logger->error("错误: {}", modelResult.error().describe());
				return -1;
			}
			auto& model = *modelResult;
			auto assetIdsResult = uploadModelMeshes(vkDevice, vkResources, model);
			if (!assetIdsResult) {
				auto* logger = RLLogger::getInstance();
				logger->error("错误: {}", assetIdsResult.error().describe());
				return -1;
			}
			// 纹理（Phase 3）：模型顶点槽分配完后再上传（先取白纹理——布局契约"纹理首槽 = 白"，
			// ensureWhiteTexture 带契约断言）；产物在资产表
			auto textureIdsResult = uploadModelTextures(vkResources, vkDevice, model);
			if (!textureIdsResult) {
				auto* logger = RLLogger::getInstance();
				logger->error("错误: {}", textureIdsResult.error().describe());
				return -1;
			}
			primitiveTextureIds = std::move(*textureIdsResult);
			if (sceneConfig.environment.enabled)
				uploadEnvironment();  // 时序契约见定义处注释（纹理槽后、管线烘焙前）
			auto& assets = vkResources.getAssetTable();
			const uint32_t samplerSlot = vkResources.ensureSharedSampler();
			for (size_t i = 0; i < assetIdsResult->size(); ++i) {
				const uint32_t id = (*assetIdsResult)[i];
				// 管线条目（Phase 4 批次 2：按 (规格, 顶点 slot, 纹理 slot) 组合去重——同模型
				// 全 primitive 共享一个拼接顶点 slot，仅纹理 slot 差异决定条目拆分；纹理/sampler
				// slot 烘焙进 binding mapping，每帧零成本）。批次 3：风格派生（loader unlit
				// 字段即数据源，与 MaterialAsset 一致性由 registerScene 金丝雀保证）→ scene.lua
				// styles 表映射到管线规格名（缺失 = 启动报错，同现有 config 错误路径）。
				const std::string style = model.primitives[i].unlit ? "unlit" : "pbr";
				const auto styleIt = sceneConfig.styleMap.find(style);
				if (styleIt == sceneConfig.styleMap.end())
					throw std::runtime_error(fmt::format("style not in scene.lua styles: {}", style));
				const uint32_t entryIdx = shaderRegistry.addModelEntry(
					styleIt->second,
					assets.get(id).heapSlot,  // = 模型拼接顶点 slot（同模型全 primitive 相同）
					assets.getTexture(primitiveTextureIds[i]).heapSlot,
					samplerSlot);
				shaderRegistry.bindToAsset(id, entryIdx);
			}
			// DIAG（Phase 4 批次 3 风格分布核查）：per-primitive 风格（unlit/pbr）+ 规格名分布
			if (std::getenv("RGL_STYLE_DIST") != nullptr) {
				std::map<std::string, uint32_t> styleCount;
				for (size_t i = 0; i < assetIdsResult->size(); ++i)
					++styleCount[model.primitives[i].unlit ? "unlit" : "pbr"];
				if (auto logger = RLLogger::getInstance()) {
					std::string s;
					for (const auto& [style, cnt] : styleCount)
						s += fmt::format("{}={} ", style, cnt);
					logger->info("DIAG style dist: {} primitives → {} styles: {}",
						assetIdsResult->size(), styleCount.size(), s);
				}
			}
			// DIAG（Phase 4 批次 2 纹理分布核查）：per-primitive 纹理 slot 分布（验证新旧管线用同一批纹理）
			if (std::getenv("RGL_TEX_DIST") != nullptr) {
				std::map<uint32_t, uint32_t> texSlotCount;
				for (size_t i = 0; i < assetIdsResult->size(); ++i)
					++texSlotCount[assets.getTexture(primitiveTextureIds[i]).heapSlot];
				if (auto logger = RLLogger::getInstance()) {
					std::string s;
					for (const auto& [slot, cnt] : texSlotCount)
						s += fmt::format("slot{}={} ", slot, cnt);
					logger->info("DIAG tex dist: {} primitives → {} distinct tex slots: {}",
						assetIdsResult->size(), texSlotCount.size(), s);
				}
			}
			registerScene(registry, model, std::span<const uint32_t>(*assetIdsResult),
				std::span<const uint32_t>(primitiveTextureIds), vkResources);
			// scene data 容量契约（Phase 5 批次 2）：对象矩阵表按资产 id 索引，
			// 超容量 = 矩阵表越界写（staging 上传处有防御报错，此处启动期先拦截）
			if (const uint32_t meshCount = vkResources.getAssetTable().meshSize();
				meshCount > kSceneMatrixCapacity)
				throw std::runtime_error(fmt::format(
					"assets ({}) exceed scene object matrix capacity ({})",
					meshCount, kSceneMatrixCapacity));
		} else if (sceneConfig.environment.enabled) {
			uploadEnvironment();  // 无模型 + env（纯天空盒场景，Phase 5.5 批次 4）
		}

		// Phase 5.5 批次 4：天空盒管线条目（env 已上传才建——addSkyEntry 契约）+
		// SceneEnv 回填（renderListSystem 经 ctx 生成 skybox draw，画不画 = 配置有无）
		if (vkResources.getEnvTexture()) {
			auto& envCtx = registry.ctx().get<SceneEnv>();
			envCtx.skyboxPipeline = shaderRegistry.addSkyEntry();
			envCtx.skybox = sceneConfig.environment.skybox;
		}

		// cube 一等资产（批次 3）：showCube 配置 → 注册 entity（画不画 = 实体有无）+
		// 管线表下标写回（启动期一次）。cube 着色 = 每顶点色，MaterialRef 指向默认
		// 材质（baseColor 不被 cube 管线消费，见 registerCubeEntity 注释）
		if (sceneConfig.showCube) {
			auto& assets = vkResources.getAssetTable();
			const uint32_t cubeMat = assets.addMaterial(
				MaterialAsset{glm::vec4{1.0f}, kInvalidTextureId, false});
			registerCubeEntity(registry, vkResources.cubeAssetId(), cubeMat);
			shaderRegistry.bindPipelineToAsset("cube", vkResources.cubeAssetId());
			if (auto logger = RLLogger::getInstance())
				logger->info("cube entity: 资产 {} / 材质 {} / 管线下标 {}",
					vkResources.cubeAssetId(), cubeMat,
					assets.get(vkResources.cubeAssetId()).pipelineIndex);
		}

		// DIAG（可见窗 fault 排查）：隐藏窗启动、第 showAfter 帧后 SDL_ShowWindow——
		// 若显示瞬间开始 fault = present/合成期状态触发；若全程不 fault = 可见性非根因
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
		// llm 截图执行回调（ecs 不 import llm_debug——依赖图叶子解耦，2026-09-02）：
		// main 是唯一知道 captureFrame 的地方，config 已按值绑进 lambda
		auto renderResult = RGL::renderSystem(registry, vkRenderer,
			llmEnabled ? &cameraConfig.llm : nullptr,
			[&](RGL::Renderer& r, const RGL::FrameDrawList& l) {
				return RGL::captureFrame(r, l, cameraConfig.llm);
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
