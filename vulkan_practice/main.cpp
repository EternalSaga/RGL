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

		// CPU 解析 + GPU 上传 + 纹理 + ECS 注册：资产数据（buffer/slot/三角形数/管线条目引用）
		// 在 Resources 资产表，生命周期不再依赖 main 作用域（E1.2/L6）
		// 纹理池**必须**提至模型块外——生命周期 = Phase 3 全黑/4K 黑屏/状态漂移的真根因修复
		//（2026-08-28 m3-scene 现场实验单变量实锤，docs/heap_texture_research.md §12.6）：
		// 原位置在 if 块内 → 块尾析构释放纹理 BO + 描述符存储，第 0 帧起 GPU 采样已释放 VA
		// = UAF（VA 未复用读旧页 = "好状态"渲染正常，被复用 = GPUVM TCP fault/全黑 = "坏状态"）。
		// 附带动机：RGL_CUBE_TEX 的 cube 纹理实验（无模型）也要用池本体。
		// 白纹理**不**在此上传（getWhite 保持块内）：布局约定"纹理首槽 = 白"依赖模型顶点槽
		//（base 2+i，HeapSlotModelVertexBase）先分配完——提前上传 = slot 契约漂移
		//（2026-08-28 排查踩坑：白纹理抢 slot 2 → model.vertex[0] drift）
		TexturePool texturePool(vkResources.getAllocator(), vkDevice, vkResources);
		uint32_t whiteTextureId = kInvalidTextureId;
		std::map<int, uint32_t> imageTextureCache;  // image 下标 → 纹理资产 id（RGL_CUBE_TEX 用首图）
		if (!sceneConfig.modelPath.empty()) {
			auto modelResult = loadGltfModel(sceneConfig.modelPath);
			if (!modelResult) {
				auto* logger = RLLogger::getInstance();
				logger->error("错误: {}", modelResult.error().describe());
				return -1;
			}
			auto assetIdsResult = uploadModelMeshes(vkDevice, vkResources, *modelResult);
			if (!assetIdsResult) {
				auto* logger = RLLogger::getInstance();
				logger->error("错误: {}", assetIdsResult.error().describe());
				return -1;
			}
			// 纹理（Phase 3）：模型顶点槽分配完后再取白纹理（布局约定"纹理首槽 = 白"），
			// 再按 primitive 的 baseColorTexture 加载（image 下标去重）；无纹理/无 uv → 白纹理
			auto& model = *modelResult;
			whiteTextureId = texturePool.getWhite();
			std::vector<uint32_t> primitiveTextureIds(model.primitives.size(), whiteTextureId);
			for (size_t i = 0; i < model.primitives.size(); ++i) {
				const int imageIndex = model.primitives[i].baseColorTextureIndex;
				if (imageIndex < 0 || !model.primitives[i].hasUv)
					continue;
				auto cached = imageTextureCache.find(imageIndex);
				if (cached != imageTextureCache.end()) {
					primitiveTextureIds[i] = cached->second;
					continue;
				}
				const auto& image = model.images[imageIndex];
				if (image.data.empty()) {
					auto* logger = RLLogger::getInstance();
					if (logger)
						logger->warn("纹理: image {} 无内嵌数据（多 buffer .gltf 不支持），primitive {} 用白纹理",
							imageIndex, i);
					continue;
				}
				auto textureResult = texturePool.loadEmbedded(
					image.data.data(), image.data.size(),
					("texture.image." + std::to_string(imageIndex)).c_str());
				if (!textureResult) {
					auto* logger = RLLogger::getInstance();
					if (logger) logger->error("错误: {}", textureResult.error().describe());
					return -1;
				}
				primitiveTextureIds[i] = *textureResult;
				imageTextureCache[imageIndex] = *textureResult;
			}
			{
				auto* logger = RLLogger::getInstance();
				uint32_t texturedPrimitives = 0;
				for (uint32_t id : primitiveTextureIds)
					if (id != whiteTextureId) texturedPrimitives++;
				if (logger)
					logger->info("textures: {} 资产（白 + {} 图片），{} / {} primitive 带纹理",
						texturePool.size(), texturePool.size() - 1, texturedPrimitives,
						model.primitives.size());
			}
			auto& assets = vkResources.getAssetTable();
			const uint32_t samplerSlot = texturePool.samplerSlot();
			for (size_t i = 0; i < assetIdsResult->size(); ++i) {
				const uint32_t id = (*assetIdsResult)[i];
				// 纹理 slot + sampler slot 烘焙进 per-primitive 管线 mapping（binding 1/2），每帧零成本
				vkRenderer.addModelPipeline(assets.get(id).heapSlot,
					texturePool.get(primitiveTextureIds[i]).heapSlot, samplerSlot, id);
			}
			registerScene(registry, model, std::span<const uint32_t>(*assetIdsResult));
		}

		// DIAG（RGL_CUBE_TEX）：cube + 纹理采样——隔离"dmabuf BO 存在 + 纹理采样"组合触发
		//（GPUVM TCP 权限 fault，2026-08-28 排查）：无模型 BDA / 无模型管线，仅 cube +
		// 纹理采样；纹理 = 模型首张内嵌图（如有，glb 已含真实 PNG 免 LFS），否则白 1×1；
		// cube 顶点/索引固定 slot 0/4（见 addCubeTexPipeline）
		if (std::getenv("RGL_CUBE_TEX")) {
			// 无模型时白纹理尚未上传（契约：模型顶点槽之后才取）——此处补取
			if (whiteTextureId == kInvalidTextureId)
				whiteTextureId = texturePool.getWhite();
			const uint32_t texId = imageTextureCache.empty()
				? whiteTextureId : imageTextureCache.begin()->second;
			const uint32_t texSlot = texturePool.get(texId).heapSlot;
			vkRenderer.addCubeTexPipeline(texSlot, texturePool.samplerSlot());
			if (auto logger = RLLogger::getInstance())
				logger->info("DIAG RGL_CUBE_TEX: cube_tex 条目（纹理资产 {} slot {}，sampler slot {}）",
					texId, texSlot, texturePool.samplerSlot());
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
