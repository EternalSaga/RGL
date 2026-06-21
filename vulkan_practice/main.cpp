#include <iostream>
#include <stdexcept>
import vulkan_window;
import vk_instance;
import vk_device;
import vk_swapchain;
import vk_resources;
import vk_renderer;
using namespace RGL;
int main() {
  try {
    SDLWindow window(800, 600, "Vulkan Practice");
    VkInstance vkInstance(window);
    VkDevice vkDevice(vkInstance);
    VkSwapchain vkSwapchain(vkDevice, window);
    VkResources vkResources(vkInstance, vkDevice, vkSwapchain);
    VkRenderer vkRenderer(vkDevice, vkSwapchain, vkResources);

    vkRenderer.render(window);
  } catch (const std::exception& e) {
    std::cerr << "错误: " << e.what() << std::endl;
    return -1;
  }
  return 0;
}
