module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdint>

export module ecs;

import rlstd;
import vkerror;

import vulkan_window;
import vk_swapchain;
import vk_resources;
import vk_renderer;
import cube;
import rllogger;

namespace RGL {

export struct Viewport {
	uint32_t width = 800;
	uint32_t height = 600;
	bool needsResize = false;
	bool shouldClose = false;
};

void eventSystem(entt::registry& rg, RGL::SDLWindow& window) {
	auto& vp = rg.ctx().get<Viewport>();

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

std::expected<void, Error> resizeSystem(entt::registry& rg, RGL::Swapchain& swapchain,
	RGL::Resources& resources, RGL::Renderer& renderer) {
	auto& vp = rg.ctx().get<Viewport>();
	if (!vp.needsResize) {
		return {};
	}

	swapchain.recreate();
	auto result = resources.recreateDepthImage(swapchain.getExtent());
	if (!result) {
		return std::unexpected(std::move(result).error());
	}
	vp.needsResize = false;
	return {};
}

export std::expected<void, Error> renderSystem(entt::registry& rg, RGL::Renderer& renderer,
	RGL::SDLWindow& window) {
	auto& swapchain = renderer.getSwapchainRef();
	auto& resources = renderer.getResourcesRef();
	auto& vp = rg.ctx().get<Viewport>();

	auto* logger = RLLogger::getInstance();
	std::optional<Error> lastError;

	while (true) {
		eventSystem(rg, window);

		auto resizeResult = resizeSystem(rg, swapchain, resources, renderer);
		if (!resizeResult) {
			if (logger) logger->error("Resize 错误: {}", resizeResult.error().describe());
			renderer.waitForIdle();
			return std::unexpected(std::move(resizeResult).error());
		}

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

		auto result = renderer.drawFrame();
		if (!result) {
			if (logger) logger->error("渲染错误: {}", result.error().describe());
			lastError = result.error();
			break;
		}
	}

	renderer.waitForIdle();
	if (lastError) {
		return std::unexpected(std::move(*lastError));
	}
	return {};
}

}  // namespace RGL
