#include <entt/entt.hpp>
import rlstd;
import vulkan_window;
import vk_instance;
import vk_device;
import vk_swapchain;
import vk_resources;
import vk_renderer;
import ecs;
using namespace RGL;
int main() {
  try {
    entt::registry registry;
    registry.ctx().emplace<Viewport>(800, 600, false);

    SDLWindow window(800, 600, "Vulkan Practice");
    AppContext vkInstance(window);
    DeviceContext vkDevice(vkInstance);
    Swapchain vkSwapchain(vkDevice, window);
    Resources vkResources(vkInstance, vkDevice, vkSwapchain);
    Renderer vkRenderer(vkDevice, vkSwapchain, vkResources, registry);
    RGL::renderSystem(registry, vkRenderer, window);
  } catch (const std::exception &e) {
    std::cerr << "错误: " << e.what() << std::endl;
    return -1;
  }
  return 0;
}
