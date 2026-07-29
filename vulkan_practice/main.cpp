#include <iostream>
#include <stdexcept>
#include <entt/entt.hpp>
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
    VkInstance vkInstance(window);
    VkDevice vkDevice(vkInstance);
    VkSwapchain vkSwapchain(vkDevice, window);
    VkResources vkResources(vkInstance, vkDevice, vkSwapchain);
    VkRenderer vkRenderer(vkDevice, vkSwapchain, vkResources, registry);
    vkRenderer.setWindow(&window);

    RGL::renderSystem(registry, vkRenderer);
  } catch (const std::exception& e) {
    std::cerr << "错误: " << e.what() << std::endl;
    return -1;
  }
  return 0;
}
