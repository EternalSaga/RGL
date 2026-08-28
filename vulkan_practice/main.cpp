#include <entt/entt.hpp>
#include <SDL3/SDL_video.h>
import rlstd;
import vulkan_window;
import vk_instance;
import vk_device;
import vk_swapchain;
import vk_resources;
import vk_renderer;
import scene_config;
import gltf_loader;
import model_importer;
import texture_pool;
import scene;
import camera;
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
	auto windowResult = SDLWindow::create(
		llmEnabled ? static_cast<int>(cameraConfig.llm.width) : winW,
		llmEnabled ? static_cast<int>(cameraConfig.llm.height) : winH,
		"Vulkan Practice", llmEnabled || showAfter >= 0);
	if (!windowResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", windowResult.error().describe());
		return -1;
	}
	auto window = std::move(*windowResult);
	if (llmEnabled) {
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
		registry.ctx().emplace<WindowExtent>(static_cast<uint32_t>(window.getWidth()),
			static_cast<uint32_t>(window.getHeight()));
		registry.ctx().emplace<AppExit>();
		registry.ctx().emplace<Camera>(std::move(cameraConfig.camera));
		registry.ctx().emplace<InputEvents>();
		registry.ctx().emplace<CameraState>();
		auto& spin = registry.ctx().emplace<SceneSpin>();
		spin.speed = sceneConfig.spin ? 1.0f : 0.0f;  // 0 = 静止（configs/scene.lua 的 spin 开关）
		registry.ctx().emplace<SceneBounds>();
		registry.ctx().emplace<ViewProjection>();
		registry.ctx().emplace<FrameDrawList>();

		AppContext vkInstance(window);
		DeviceContext vkDevice(vkInstance);
		Swapchain vkSwapchain(vkDevice, window);
		// heap 容量为固定裕量（与场景内容解耦，批次 3 E1.3）：CPU 解析模型不再需要
		// 先于 Vulkan 初始化
		Resources vkResources(vkInstance, vkDevice, vkSwapchain);
		// 管线规格来自 configs/scene.lua；cubeEntry = showCube（cube 渲染的 gating，
		// 决定 "cube" 管线条目是否创建 → 渲染 system 侧的 cube DrawCall 是否产生）
		Renderer vkRenderer(vkDevice, vkSwapchain, vkResources, sceneConfig.pipelines,
			sceneConfig.showCube);

		// CPU 解析 + GPU 上传 + 纹理 + ECS 注册：资产数据（buffer/slot/三角形数/管线条目引用、
		// 纹理资产、材质资产）全部在 Resources 资产表（初始化层 owner，生命周期不依赖
		// main 作用域——TexturePool 局部对象 UAF 教训，docs/heap_texture_research.md §12.6/§12.7）
		std::vector<uint32_t> primitiveTextureIds;  // per-primitive 纹理资产 id（批次 2 材质注册 + RGL_CUBE_TEX 门消费）
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
			auto& assets = vkResources.getAssetTable();
			const uint32_t samplerSlot = vkResources.ensureSharedSampler();
			for (size_t i = 0; i < assetIdsResult->size(); ++i) {
				const uint32_t id = (*assetIdsResult)[i];
				// 纹理 slot + sampler slot 烘焙进 per-primitive 管线 mapping（binding 1/2），每帧零成本
				vkRenderer.addModelPipeline(assets.get(id).heapSlot,
					assets.getTexture(primitiveTextureIds[i]).heapSlot, samplerSlot, id);
			}
			registerScene(registry, model, std::span<const uint32_t>(*assetIdsResult),
				std::span<const uint32_t>(primitiveTextureIds), vkResources);
		}

		// DIAG（RGL_CUBE_TEX）：cube + 纹理采样——隔离"dmabuf BO 存在 + 纹理采样"组合触发
		//（GPUVM TCP 权限 fault，2026-08-28 排查）：无模型 BDA / 无模型管线，仅 cube +
		// 纹理采样；纹理 = 模型首个真实带纹理 primitive（如有），否则白 1×1；
		// cube 顶点/索引固定 slot 0/1（见 addCubeTexPipeline）
		if (std::getenv("RGL_CUBE_TEX")) {
			// 无模型时白纹理尚未上传（契约：模型顶点槽之后才取）——此处补取
			const uint32_t whiteId = vkResources.getAssetTable().textureSize() > 0
				? 0
				: ensureWhiteTexture(vkResources, vkDevice);
			uint32_t texId = whiteId;
			for (uint32_t id : primitiveTextureIds)
				if (id != whiteId) {
					texId = id;
					break;
				}
			const uint32_t texSlot = vkResources.getAssetTable().getTexture(texId).heapSlot;
			const uint32_t samplerSlot = vkResources.ensureSharedSampler();
			vkRenderer.addCubeTexPipeline(texSlot, samplerSlot);
			if (auto logger = RLLogger::getInstance())
				logger->info("DIAG RGL_CUBE_TEX: cube_tex 条目（纹理资产 {} slot {}，sampler slot {}）",
					texId, texSlot, samplerSlot);
		}

		// DIAG（可见窗 fault 排查）：隐藏窗启动、第 showAfter 帧后 SDL_ShowWindow——
		// 若显示瞬间开始 fault = present/合成期状态触发；若全程不 fault = 可见性非根因
		std::function<void(uint32_t)> onFrame;
		if (showAfter >= 0) {
			SDL_Window* sdlWindow = window;
			onFrame = [sdlWindow, showAfter](uint32_t frame) {
				if (static_cast<int>(frame) == showAfter) {
					auto* logger = RLLogger::getInstance();
					if (logger) logger->info("DIAG: frame {} → SDL_ShowWindow", frame);
					SDL_ShowWindow(sdlWindow);
				}
			};
		}
		auto renderResult = RGL::renderSystem(registry, vkRenderer,
			llmEnabled ? &cameraConfig.llm : nullptr, onFrame);
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
