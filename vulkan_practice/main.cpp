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
import aftermath;
import vkerror;
using namespace RGL;
int main() {
    auto windowResult = SDLWindow::create(800, 600, "Vulkan Practice");
    if (!windowResult) {
        std::cerr << "错误: " << windowResult.error().describe() << std::endl;
        return -1;
    }
    auto window = std::move(*windowResult);

    RGL::initAftermath();

    try {
        entt::registry registry;
        registry.ctx().emplace<Viewport>(800, 600, false);

        AppContext vkInstance(window);
        DeviceContext vkDevice(vkInstance);
        Swapchain vkSwapchain(vkDevice, window);
        Resources vkResources(vkInstance, vkDevice, vkSwapchain);
        Renderer vkRenderer(vkDevice, vkSwapchain, vkResources, registry);
        RGL::renderSystem(registry, vkRenderer, window);
    } catch (const std::exception &e) {
        std::cerr << "错误: " << e.what() << std::endl;
        RGL::waitForCrashDump();
        RGL::shutdownAftermath();
        return -1;
    }
    RGL::shutdownAftermath();
    return 0;
}