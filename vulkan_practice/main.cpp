#include <entt/entt.hpp>
import rlstd;
import vulkan_window;
import vk_instance;
import vk_device;
import vk_swapchain;
import vk_resources;
import vk_renderer;
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

	try {
		entt::registry registry;
		registry.ctx().emplace<Viewport>(800, 600, false);

		AppContext vkInstance(window);
		DeviceContext vkDevice(vkInstance);
		Swapchain vkSwapchain(vkDevice, window);
		Resources vkResources(vkInstance, vkDevice, vkSwapchain);
		Renderer vkRenderer(vkDevice, vkSwapchain, vkResources, registry);
		auto renderResult = RGL::renderSystem(registry, vkRenderer, window);
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
