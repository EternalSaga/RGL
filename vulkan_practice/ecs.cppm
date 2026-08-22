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
import camera;
import cube;
import rllogger;

namespace RGL {

export struct Viewport {
	uint32_t width = 800;
	uint32_t height = 600;
	bool needsResize = false;
	bool shouldClose = false;
};

// 中央事件轮询（唯一 SDL_PollEvent 调用点）：窗口生命周期 + 相机原始输入状态
void eventSystem(entt::registry& rg, RGL::SDLWindow& window) {
	auto& vp = rg.ctx().get<Viewport>();
	auto& input = rg.ctx().get<CameraInput>();

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_QUIT ||
			event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
			vp.shouldClose = true;
		} else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
			vp.width = static_cast<uint32_t>(event.window.data1);
			vp.height = static_cast<uint32_t>(event.window.data2);
			vp.needsResize = true;
		} else if (event.type == SDL_EVENT_KEY_DOWN) {
			switch (event.key.key) {
				case SDLK_ESCAPE:
					vp.shouldClose = true;
					break;
				case SDLK_W:
					input.forward = true;
					break;
				case SDLK_A:
					input.left = true;
					break;
				case SDLK_S:
					input.back = true;
					break;
				case SDLK_D:
					input.right = true;
					break;
				default:
					break;
			}
		} else if (event.type == SDL_EVENT_KEY_UP) {
			switch (event.key.key) {
				case SDLK_W:
					input.forward = false;
					break;
				case SDLK_A:
					input.left = false;
					break;
				case SDLK_S:
					input.back = false;
					break;
				case SDLK_D:
					input.right = false;
					break;
				default:
					break;
			}
		} else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
			if (event.button.button == SDL_BUTTON_LEFT) {
				input.mouseLeftDown = true;
			} else if (event.button.button == SDL_BUTTON_RIGHT) {
				input.mouseRightDown = true;
			}
		} else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
			if (event.button.button == SDL_BUTTON_LEFT) {
				input.mouseLeftDown = false;
			} else if (event.button.button == SDL_BUTTON_RIGHT) {
				input.mouseRightDown = false;
			}
		} else if (event.type == SDL_EVENT_MOUSE_MOTION) {
			// 注意：SDL3 的 motion.x/y 是窗口内绝对坐标，相对增量在 xrel/yrel
			if (input.mouseLeftDown || input.mouseRightDown) {
				input.mouseDelta.x += event.motion.xrel;
				input.mouseDelta.y += event.motion.yrel;
			}
		} else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
			input.wheelDelta += event.wheel.y;
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
	auto& camera = rg.ctx().get<Camera>();

	auto* logger = RLLogger::getInstance();
	std::optional<Error> lastError;

	float prevTicks = static_cast<float>(SDL_GetTicks());

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

		float ticks = static_cast<float>(SDL_GetTicks());
		float time = ticks / 1000.0f;
		// dt 限幅：窗口卡顿/切后台回来时防止相机瞬移
		float dt = std::clamp((ticks - prevTicks) / 1000.0f, 0.0f, 0.1f);
		prevTicks = ticks;

		cameraSystem(rg, dt);

		glm::mat4 model =
			glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.5f, 1.0f, 0.0f));
		glm::mat4 view = cameraView(camera);
		glm::mat4 proj =
			glm::perspective(camera.fov,
				static_cast<float>(swapchain.getExtent().width) /
					static_cast<float>(swapchain.getExtent().height),
				camera.nearPlane, camera.farPlane);

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
