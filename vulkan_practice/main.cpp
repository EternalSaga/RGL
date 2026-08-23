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
		registry.ctx().emplace<Viewport>(800, 600, false);
		registry.ctx().emplace<Camera>(std::move(*cameraResult));
		registry.ctx().emplace<CameraInput>();

		AppContext vkInstance(window);
		DeviceContext vkDevice(vkInstance);
		Swapchain vkSwapchain(vkDevice, window);
		Resources vkResources(vkInstance, vkDevice, vkSwapchain, primitiveCount);
		Renderer vkRenderer(vkDevice, vkSwapchain, vkResources, sceneConfig, primitiveCount, registry);

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

		auto renderResult = RGL::renderSystem(registry, vkRenderer, window, sceneConfig);
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
