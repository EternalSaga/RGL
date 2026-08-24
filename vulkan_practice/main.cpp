#include <entt/entt.hpp>
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
import scene;
import camera;
import ecs;
import rllogger;
import vkerror;
using namespace RGL;
int main() {
	auto windowResult = SDLWindow::create(800, 600, "Vulkan Practice");
	if (!windowResult) {
		auto* logger = RLLogger::getInstance();
		if (logger) logger->error("错误: {}", windowResult.error().describe());
		return -1;
	}
	auto window = std::move(*windowResult);

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

	// CPU 侧解析模型（先于 Vulkan 初始化：primitive 数 N 决定 descriptor heap 容量）
	std::optional<GltfModel> model;
	if (!sceneConfig.modelPath.empty()) {
		auto modelResult = loadGltfModel(sceneConfig.modelPath);
		if (!modelResult) {
			auto* logger = RLLogger::getInstance();
			if (logger) logger->error("错误: {}", modelResult.error().describe());
			return -1;
		}
		model = std::move(*modelResult);
	}
	const uint32_t primitiveCount = model ? static_cast<uint32_t>(model->primitives.size()) : 0;

	try {
		entt::registry registry;
		// 窗口/应用状态（eventSystem 写）+ 每帧 system 的单例数据（context）：
		// WindowExtent/AppExit（ecs.cppm）、SceneSpin/SceneBounds/ViewProjection（scene.cppm）、
		// FrameDrawList（vkrenderer.cppm，渲染 system 产出、drawFrame 消费）
		registry.ctx().emplace<WindowExtent>(static_cast<uint32_t>(window.getWidth()),
			static_cast<uint32_t>(window.getHeight()));
		registry.ctx().emplace<AppExit>();
		registry.ctx().emplace<Camera>(std::move(*cameraResult));
		registry.ctx().emplace<CameraInput>();
		auto& spin = registry.ctx().emplace<SceneSpin>();
		spin.speed = sceneConfig.spin ? 1.0f : 0.0f;  // 0 = 静止（configs/scene.lua 的 spin 开关）
		registry.ctx().emplace<SceneBounds>();
		registry.ctx().emplace<ViewProjection>();
		registry.ctx().emplace<FrameDrawList>();

		AppContext vkInstance(window);
		DeviceContext vkDevice(vkInstance);
		Swapchain vkSwapchain(vkDevice, window);
		Resources vkResources(vkInstance, vkDevice, vkSwapchain, primitiveCount);
		// 管线规格来自 configs/scene.lua；cubeEntry = showCube（cube 渲染的 gating，
		// 决定 "cube" 管线条目是否创建 → 渲染 system 侧的 cube DrawCall 是否产生）
		Renderer vkRenderer(vkDevice, vkSwapchain, vkResources, sceneConfig.pipelines,
			sceneConfig.showCube, primitiveCount);

		// GPU 上传 + ECS 注册（meshAssets 持有每 primitive 的顶点 buffer，须活到渲染结束）
		ModelMeshAssets meshAssets;
		if (model) {
			auto uploadResult = uploadModelMeshes(vkDevice, vkResources, *model);
			if (!uploadResult) {
				auto* logger = RLLogger::getInstance();
				if (logger) logger->error("错误: {}", uploadResult.error().describe());
				return -1;
			}
			meshAssets = std::move(*uploadResult);
			registerScene(registry, *model);
		}

		auto renderResult = RGL::renderSystem(registry, vkRenderer);
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
