module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

export module ecs;

import vulkan_window;
import vk_swapchain;
import vk_resources;
import vk_renderer;
import cube;

namespace RGL {

export struct Viewport {
    uint32_t width = 800;
    uint32_t height = 600;
    bool needsResize = false;
    bool shouldClose = false;
};

void eventSystem(entt::registry &rg, RGL::SDLWindow &window) {
    auto &vp = rg.ctx().get<Viewport>();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT ||
            event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            vp.shouldClose = true;
        } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
            vp.width = static_cast<uint32_t>(event.window.data1);
            vp.height = static_cast<uint32_t>(event.window.data2);
            vp.needsResize = true;
        }
    }
}

void resizeSystem(entt::registry &rg, RGL::Swapchain &swapchain,
                  RGL::Resources &resources, RGL::Renderer &renderer) {
    auto &vp = rg.ctx().get<Viewport>();
    if (!vp.needsResize) {
        return;
    }

    swapchain.recreate();
    resources.recreateDepthImage(swapchain.getExtent());
    vp.needsResize = false;
}

export void renderSystem(entt::registry &rg, RGL::Renderer &renderer, RGL::SDLWindow &window) {
    auto &swapchain = renderer.getSwapchainRef();
    auto &resources = renderer.getResourcesRef();
    auto &vp = rg.ctx().get<Viewport>();

    bool shouldClose = false;

    while (!shouldClose) {
        eventSystem(rg, window);
        resizeSystem(rg, swapchain, resources, renderer);

        if (vp.shouldClose) {
            break;
        }

        float time = SDL_GetTicks() / 1000.0f;

        glm::mat4 model =
            glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.5f, 1.0f, 0.0f));
        glm::mat4 view =
            glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f),
                        glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 proj =
            glm::perspective(glm::radians(45.0f),
                             static_cast<float>(swapchain.getExtent().width) /
                                 static_cast<float>(swapchain.getExtent().height),
                             0.1f, 10.0f);

        renderer.updateDrawArgs(proj * view * model);
        renderer.drawFrame();
    }

    renderer.waitForIdle();
}

}  // namespace RGL
