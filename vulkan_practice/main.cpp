#include <entt/entt.hpp>
#include <SDL3/SDL_video.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // entity rootTransform（translate/rotate/scale 组装）
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
import render_graph_config;
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
		registry.ctx().emplace<NextObjectSlot>();  // Phase 6 instancing：实例矩阵槽分配器
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
		// Phase 6 批次 3：渲染意图图 = configs/render_graph.lua（缺文件/缺段回落默认
		// 三段图）；compileRenderGraph 校验 + barrier 布局链推导，任何图错误启动期
		// 报错退出（Renderer 构造校验器兜底执行器可解析性）。
		// DIAG（RGL_OFFSCREEN，批次 4 转正）：env 置位 = 强制 offscreen 图变体
		//（三段渲 scene_rt + composite blit 上屏，旧手工通路等价；覆盖 Lua 配置——
		// 排障开关语义 = 与配置文件无关地切换渲染路线）
		auto graphDescResult = std::getenv("RGL_OFFSCREEN")
			? std::expected<RgGraphDesc, Error>{offscreenRenderGraphDesc()}
			: loadRenderGraphConfig("configs/render_graph.lua");
		if (!graphDescResult) {
			auto* logger = RLLogger::getInstance();
			if (logger) logger->error("错误: {}", graphDescResult.error().describe());
			return -1;
		}
		auto compiledGraph = compileRenderGraph(*graphDescResult);
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
		Renderer vkRenderer(vkDevice, vkSwapchain, vkResources, shaderRegistry,
			std::move(*compiledGraph));

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
		// Phase 6 批次 5：scene.lua entities 列表 = 场景构成数据源（画不画 = entity 有无，
		// ECS 规范 3；缺段兼容合成见 scene_config loader）。三段时序：A 解析 + GPU 上传
		// 全部模型（顶点/纹理槽 allocSlot 单调分配）→ B env 上传（**时序契约 = 全部纹理
		// 槽后、管线烘焙前**，挤占白纹理契约槽 = 启动断言，2026-09-01 踩坑）→ C 管线烘焙
		// + ECS 注册。CPU 解析产物（GltfModel）活到 C（unique_ptr 稳定地址）；资产数据
		// （buffer/slot/纹理/材质）全部在 Resources 资产表（初始化层 owner——TexturePool
		// 局部对象 UAF 教训，docs/heap_texture_research.md §12.6/§12.7）
		auto makeRootTransform = [](const SceneEntityDesc& ent) {
			glm::mat4 t = glm::translate(glm::mat4(1.0f), ent.position);
			t = glm::rotate(t, glm::radians(ent.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
			t = glm::rotate(t, glm::radians(ent.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
			t = glm::rotate(t, glm::radians(ent.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
			return glm::scale(t, glm::vec3(ent.scale));
		};
		// Phase 6 instancing：entities 按 **file 去重**——同文件多 entity = 一次解析
		// 一次上传（顶点/纹理/管线条目全部共享），每 entity 各注册一组 primitive
		// entity（独立 Transform + 独立实例矩阵槽 objectSlot）。资产与实例分离
		//（ECS 规范 5）：上传去重省显存，绘制仍是 per-entity per-primitive draw
		//（真 GPU instancing——drawMeshTasksEXT 实例维 + 矩阵表批量消费——留 Phase 7
		// 与 culling 一并议，本批先把共享资产的地基打对）。
		struct UploadedModel {
			std::string file;
			std::unique_ptr<GltfModel> model;
			std::vector<uint32_t> assetIds;    // per-primitive 网格资产 id
			std::vector<uint32_t> textureIds;  // per-primitive 纹理资产 id
			uint32_t instances = 0;            // entities 引用计数（日志）
		};
		std::vector<UploadedModel> uploaded;
		std::map<std::string, size_t> modelOf;  // file → uploaded 下标
		// A1：解析 + 顶点上传（**全部模型的顶点槽先于任何纹理槽分配**——布局契约
		// "[2, 2+N) 模型顶点槽、[2+N,…) 纹理槽"（vkresources.cppm），N = 全模型 chunk
		// 总数；交错分配 = 第二个模型首 chunk slot 期望漂移，批次 5 多模型实测踩坑）
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
			const auto& primitiveTextureIds = um.textureIds;
			auto& assets = vkResources.getAssetTable();
			const uint32_t samplerSlot = vkResources.ensureSharedSampler();
			for (size_t i = 0; i < um.assetIds.size(); ++i) {
				const uint32_t id = um.assetIds[i];
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
			// DIAG（Phase 4 批次 2 纹理分布核查）：per-primitive 纹理 slot 分布（验证新旧管线用同一批纹理）
			if (std::getenv("RGL_TEX_DIST") != nullptr) {
				std::map<uint32_t, uint32_t> texSlotCount;
				for (size_t i = 0; i < um.assetIds.size(); ++i)
					++texSlotCount[assets.getTexture(primitiveTextureIds[i]).heapSlot];
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
				std::span<const uint32_t>(um.textureIds), vkResources,
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

		// cube 一等资产（批次 3 + Phase 6 批次 5：entities 列表驱动，可多个）：
		// 注册 entity（画不画 = 实体有无）+ 管线表下标写回（启动期一次，幂等）。
		// cube 着色 = 每顶点色，MaterialRef 指向默认材质（baseColor 不被 cube 管线消费，
		// 见 registerCubeEntity 注释）
		for (const auto& ent : sceneConfig.entities) {
			if (ent.type != "cube")
				continue;
			auto& assets = vkResources.getAssetTable();
			const uint32_t cubeMat = assets.addMaterial(
				MaterialAsset{glm::vec4{1.0f}, kInvalidTextureId, false});
			registerCubeEntity(registry, vkResources.cubeAssetId(), cubeMat,
				makeRootTransform(ent));
			shaderRegistry.bindPipelineToAsset("cube", vkResources.cubeAssetId());
			if (auto logger = RLLogger::getInstance())
				logger->info("cube entity: 资产 {} / 材质 {} / 管线下标 {}",
					vkResources.cubeAssetId(), cubeMat,
					assets.get(vkResources.cubeAssetId()).pipelineIndex);
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
