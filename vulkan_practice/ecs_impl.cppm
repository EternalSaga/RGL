module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <ranges>
module ecs:impl;

import ecs;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import vkerror;
import vk_swapchain;
import vk_resources;
import vk_renderer;
import frame_draw_list;
import render_list;
import runtime_config;  // LlmDebugConfig（renderSystem 参数类型）
import diagnostics;     // texel_canary 诊断单例（configs/diagnostics.lua）
import gpu_canary;
import scene;
import camera;
import rllogger;

namespace RGL {

// 帧循环各 system + renderSystem 定义（2026-09-02 编译提速 A 方案）：system 体移出
// 接口 TU——BMI 只含 renderSystem 声明与 context struct；本单元是叶子边。

void eventSystem(entt::registry& rg) {
	auto& extent = rg.ctx().get<WindowExtent>();
	auto& exit = rg.ctx().get<AppExit>();
	auto& events = rg.ctx().get<InputEvents>();

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
			case SDL_EVENT_QUIT:
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				exit.requested = true;
				break;
			case SDL_EVENT_WINDOW_RESIZED:
				extent.width = static_cast<uint32_t>(event.window.data1);
				extent.height = static_cast<uint32_t>(event.window.data2);
				break;
			case SDL_EVENT_KEY_DOWN: {
				InputEvent e;
				e.type = InputEvent::Type::KeyDown;
				e.key = event.key.key;
				events.events.push_back(e);
				break;
			}
			case SDL_EVENT_KEY_UP: {
				InputEvent e;
				e.type = InputEvent::Type::KeyUp;
				e.key = event.key.key;
				events.events.push_back(e);
				break;
			}
			case SDL_EVENT_MOUSE_BUTTON_DOWN: {
				InputEvent e;
				e.type = InputEvent::Type::MouseButton;
				e.button = static_cast<uint8_t>(event.button.button);
				e.pressed = true;
				events.events.push_back(e);
				break;
			}
			case SDL_EVENT_MOUSE_BUTTON_UP: {
				InputEvent e;
				e.type = InputEvent::Type::MouseButton;
				e.button = static_cast<uint8_t>(event.button.button);
				e.pressed = false;
				events.events.push_back(e);
				break;
			}
			case SDL_EVENT_MOUSE_MOTION: {
				// 注意：SDL3 的 motion.x/y 是窗口内绝对坐标，相对增量在 xrel/yrel
				InputEvent e;
				e.type = InputEvent::Type::MouseDelta;
				e.delta = {static_cast<float>(event.motion.xrel),
					static_cast<float>(event.motion.yrel)};
				events.events.push_back(e);
				break;
			}
			case SDL_EVENT_MOUSE_WHEEL: {
				InputEvent e;
				e.type = InputEvent::Type::Wheel;
				e.amount = static_cast<float>(event.wheel.y);
				events.events.push_back(e);
				break;
			}
			default:
				break;
		}
	}
}

std::expected<void, Error> resizeSystem(entt::registry& rg, RGL::Swapchain& swapchain,
	RGL::Resources& resources) {
	auto& extent = rg.ctx().get<WindowExtent>();
	auto current = swapchain.getExtent();
	if (extent.width == current.width && extent.height == current.height) {
		return {};
	}

	// recreate 内部重查 caps/format/presentMode；窗口尺寸超出 swapchain 可达范围
	//（maxImageExtent/compositor 钳制）时返回 false = 重建无益，不得每帧重建深度图
	//（每帧 waitIdle + 销毁重建 = 持续 GPU 停顿）
	if (!swapchain.recreate()) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			if (auto logger = RLLogger::getInstance())
				logger->warn("resizeSystem: window {}x{} but surface unchanged ({}x{}) — "
					"swapchain clamped, skipping depth rebuild",
					extent.width, extent.height, current.width, current.height);
		}
		return {};
	}
	auto result = resources.recreateDepthImage(swapchain.getExtent());
	if (!result) {
		return std::unexpected(std::move(result).error());
	}
	return {};
}

void animationSystem(entt::registry& rg, float dt) {
	auto& spin = rg.ctx().get<SceneSpin>();
	if (spin.speed == 0.0f) {
		return;
	}
	spin.angle += spin.speed * dt;
	spin.matrix = glm::rotate(glm::mat4(1.0f), spin.angle, spin.axis);
}

SceneBounds unionSphere(const SceneBounds& a, const SceneBounds& b) {
	if (a.radius == 0.0f) return b;
	if (b.radius == 0.0f) return a;
	const float d = glm::length(b.center - a.center);
	if (a.radius >= d + b.radius) return a;
	if (b.radius >= d + a.radius) return b;
	const float r = 0.5f * (a.radius + b.radius + d);
	const float t = (d + r) > 0.0f ? a.radius / (d + r) : 0.0f;
	return SceneBounds{a.center + (b.center - a.center) * t, r};
}

void boundsSystem(entt::registry& rg) {
	auto& sceneBounds = rg.ctx().get<SceneBounds>();
	SceneBounds acc{};
	rg.view<Bounds>().each([&](const Bounds& b) {
		acc = unionSphere(acc, SceneBounds{b.center, b.radius});
	});
	sceneBounds = acc;
}

void projectionSystem(entt::registry& rg) {
	auto& extent = rg.ctx().get<WindowExtent>();
	const auto& camera = rg.ctx().get<Camera>();
	const auto& sceneBounds = rg.ctx().get<SceneBounds>();
	auto& viewProj = rg.ctx().get<ViewProjection>();

	viewProj.view = cameraView(camera);

	float nearPlane = camera.nearPlane;
	float farPlane = camera.farPlane;
	if (sceneBounds.radius > 0.0f) {
		const float dist = glm::length(camera.position - sceneBounds.center);
		nearPlane = std::max(0.05f, dist - sceneBounds.radius);
		farPlane = dist + sceneBounds.radius;
	}
	viewProj.proj =
		glm::perspective(camera.fov,
			static_cast<float>(extent.width) / static_cast<float>(extent.height),
			nearPlane, farPlane);
}


// 每帧 system（执行顺序 = 注册顺序，即数据流契约；新增 system = 此处 push_back）
struct FrameSystem {
	const char* name;
	std::function<void(entt::registry&, float dt)> fn;
};

std::expected<void, Error> renderSystem(entt::registry& rg, RGL::Renderer& renderer,
	const RGL::LlmDebugConfig* llm,
	std::function<std::expected<std::string, Error>(RGL::Renderer&, const RGL::FrameDrawList&)>
		llmCapture,
	std::function<void(uint32_t)> onFrame) {
	auto& swapchain = renderer.getSwapchainRef();
	auto& resources = renderer.getResourcesRef();
	auto* logger = RLLogger::getInstance();
	std::optional<Error> lastError;

	RenderListState renderListState;
	uint32_t llmFrame = 0;
	uint32_t frame = 0;  // 每帧无条件计数（onFrame DIAG 回调用；llmFrame 仅在 llm 启用时递增）
	// texel 回读金丝雀（docs/archive/heap_texture_research.md §12.7 ②）：texel_canary=N（帧号，
	// 默认关）——帧 N 的 drawFrame 前重读白纹理已知 texel 比对，纹理 BO 死 = 立即报错
	//（GPU 侧 UAF 签名，ASan 全盲；诊断路径单次 GPU 停顿，正常路径零影响）
	// texel_canary = configs/diagnostics.lua（原 RGL_TEXEL_CANARY env 2026-09-08 转配置）
	const uint64_t canaryFrame = diagnostics().texelCanary;
	bool canaryFired = false;

	// system 顺序（数据流契约）：
	//   camera（InputEvents→Camera，消费即清空）→ animation（SceneSpin）→ bounds（Bounds→SceneBounds）
	//   → projection（Camera+SceneBounds+WindowExtent→ViewProjection）→ renderlist（→FrameDrawList）
	std::vector<FrameSystem> systems{
		{"camera",     [](entt::registry& rg, float dt) { cameraSystem(rg, dt); }},
		{"animation",  [](entt::registry& rg, float dt) { animationSystem(rg, dt); }},
		{"bounds",     [](entt::registry& rg, float) { boundsSystem(rg); }},
		{"projection", [](entt::registry& rg, float) { projectionSystem(rg); }},
		{"renderlist", [&](entt::registry& rg, float) { renderListSystem(rg, resources, renderListState); }},
	};

	float prevTicks = static_cast<float>(SDL_GetTicks());

	while (true) {
		eventSystem(rg);
		if (rg.ctx().get<AppExit>().requested) {
			break;
		}

		float ticks = static_cast<float>(SDL_GetTicks());
		// dt 限幅：窗口卡顿/切后台回来时防止相机瞬移
		float dt = std::clamp((ticks - prevTicks) / 1000.0f, 0.0f, 0.1f);
		prevTicks = ticks;

		auto resizeResult = resizeSystem(rg, swapchain, resources);
		if (!resizeResult) {
			if (logger) logger->error("Resize 错误: {}", resizeResult.error().describe());
			renderer.waitForIdle();
			return std::unexpected(std::move(resizeResult).error());
		}

		for (auto& system : systems) {
			system.fn(rg, dt);
		}

		// texel 金丝雀（本帧 drawFrame 前，waitForIdle 已在函数内排空 in-flight）
		if (!canaryFired && frame >= canaryFrame) {
			canaryFired = true;
			texelCanaryCheck(resources, renderer.getDeviceContextRef(),
				renderer.getGraphicsCommandPoolRef(), frame);
		}

		auto result = renderer.drawFrame(rg.ctx().get<FrameDrawList>());
		if (!result) {
			if (logger) logger->error("渲染错误: {}", result.error().describe());
			lastError = result.error();
			break;
		}
		// llm_debug：本帧渲染完成后计数；超过延迟帧数 → 用本帧 draw list 离屏
		// 重渲截图（场景已推进 N 步），然后请求退出（下轮循环头 break）
		if (llm && llmCapture) {
			if (++llmFrame > llm->delayFrames) {
				auto shot = llmCapture(renderer, rg.ctx().get<FrameDrawList>());
				if (shot) {
					if (logger) logger->info("llm_debug: 截图已保存 ({})", *shot);
				} else {
					if (logger) logger->error("llm_debug: 截图失败 ({})", shot.error().describe());
					lastError = shot.error();
				}
				rg.ctx().get<AppExit>().requested = true;
			}
		}
		if (onFrame)
			onFrame(frame);
		++frame;
	}

	renderer.waitForIdle();
	if (lastError) {
		return std::unexpected(std::move(*lastError));
	}
	return {};
}

}  // namespace RGL
