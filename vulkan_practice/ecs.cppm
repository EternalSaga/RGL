module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdint>

export module ecs;

import rlstd;
import vkerror;

import vk_swapchain;
import vk_resources;
import vk_renderer;
import frame_draw_list;
import render_list;
import camera_config;
import gpu_canary;
import scene;
import camera;
import rllogger;

namespace RGL {

// 窗口尺寸（registry context，eventSystem 在 resize 事件写入；projectionSystem/resizeSystem 读）。
// 无 needsResize 标志：resizeSystem 比较本值与 swapchain 实际尺寸（派生状态不存标志位）。
export struct WindowExtent {
	uint32_t width = 800;
	uint32_t height = 600;
};

// 退出请求（registry context，eventSystem 在 QUIT/ESC 写入；帧循环判定）
export struct AppExit {
	bool requested = false;
};

// 中央事件轮询（唯一 SDL_PollEvent 调用点）：窗口状态（WindowExtent/AppExit）+
// 输入事件流（InputEvents：本 system 只产事件，不做键位映射——键绑定在 Camera.keys，
// cameraSystem 消费事件时按绑定解释，E5.4；退出/窗口状态不走事件流）
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

// 窗口尺寸变化（WindowExtent 与 swapchain 实际尺寸不一致）时重建 swapchain + 深度图。
// 深度图必须与 swapchain extent 同步重建，否则 renderArea 会超出图像范围。
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

// 场景整体时间旋转：angle += dt * speed；matrix = rotate(axis, angle)。
// speed=0 = 静止（configs/scene.lua 的 spin 开关，main 启动期转成 speed）。
// 现状承载"全场景同一旋转"（renderListSystem 乘进各 MVP）；per-entity 旋转需求出现时
// 改写 Transform 组件，结构同构（ecs_compliance_review E4.1）。
void animationSystem(entt::registry& rg, float dt) {
	auto& spin = rg.ctx().get<SceneSpin>();
	if (spin.speed == 0.0f) {
		return;
	}
	spin.angle += spin.speed * dt;
	spin.matrix = glm::rotate(glm::mat4(1.0f), spin.angle, spin.axis);
}

// union 包围球（标准公式；radius=0 视为空球）：a 包含 b → a；b 包含 a → b；
// 否则 r = (ra+rb+d)/2，中心从 a 向 b 滑动 t = ra/(d+r)。
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

// 聚合全部 Bounds 组件 → SceneBounds context（空场景 = radius 0）。
// 现状场景静态（Bounds 仅导入期写入），每帧重聚合为未来动态场景变更留接缝。
void boundsSystem(entt::registry& rg) {
	auto& sceneBounds = rg.ctx().get<SceneBounds>();
	SceneBounds acc{};
	rg.view<Bounds>().each([&](const Bounds& b) {
		acc = unionSphere(acc, SceneBounds{b.center, b.radius});
	});
	sceneBounds = acc;
}

// Camera + 场景包围球 + 窗口尺寸 → ViewProjection context。
// 自适应 near/far：SceneBounds 非空时按相机到球心实时距离 ± 球半径推导，保证任意相机
// 位置/场景尺度下整个场景在 frustum 内；无场景内容时回落 configs/camera.lua 固定值。
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

// llm（可空）：llm_debug 一次性截图配置（非空 = 启用）。批处理语义：
// 第 llm->delayFrames 帧渲染完成后离屏重渲 + 存 PNG，然后置 AppExit 退出——
// 无常驻状态、无 mailbox（config 即控制通道：agent 改 configs/camera.lua 后重跑）。
// llmCapture（可空，llm 非空时必传）：截图执行回调（main 传 llm_debug::captureFrame 包装）——
// ecs 只数帧 + 触发 + 读结果，不 import llm_debug（依赖图解耦：llm_debug 拖 vk_renderer
// 全链，本是叶子模块，不该在 ecs 的编译前置里；2026-09-02）。
// onFrame（DIAG，可空）：每帧 drawFrame 完成后回调（帧号从 0 起）——
// 可见窗 GPUVM fault 排查用（RGL_SHOW_AFTER：隐藏窗启动、第 N 帧后显示，
// 隔离"分配期状态"与"present 期状态"）；默认空 = 无回调，正常路径零变化
export std::expected<void, Error> renderSystem(entt::registry& rg, RGL::Renderer& renderer,
	const RGL::LlmDebugConfig* llm = nullptr,
	std::function<std::expected<std::string, Error>(RGL::Renderer&, const RGL::FrameDrawList&)>
		llmCapture = {},
	std::function<void(uint32_t)> onFrame = {}) {
	auto& swapchain = renderer.getSwapchainRef();
	auto& resources = renderer.getResourcesRef();
	auto* logger = RLLogger::getInstance();
	std::optional<Error> lastError;

	RenderListState renderListState;
	uint32_t llmFrame = 0;
	uint32_t frame = 0;  // 每帧无条件计数（onFrame DIAG 回调用；llmFrame 仅在 llm 启用时递增）
	// texel 回读金丝雀（docs/archive/heap_texture_research.md §12.7 ②）：RGL_TEXEL_CANARY=N（帧号，
	// 默认关）——帧 N 的 drawFrame 前重读白纹理已知 texel 比对，纹理 BO 死 = 立即报错
	//（GPU 侧 UAF 签名，ASan 全盲；诊断路径单次 GPU 停顿，正常路径零影响）
	const uint64_t canaryFrame = [] {
		if (const char* env = std::getenv("RGL_TEXEL_CANARY"))
			return static_cast<uint64_t>(std::strtoull(env, nullptr, 10));
		return UINT64_MAX;  // 未设 = 不触发
	}();
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
