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
import types_generated;
import model_types_generated;
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
// 相机原始输入状态（CameraInput：本 system 写入，cameraSystem 每帧消费并重置累积器）
void eventSystem(entt::registry& rg) {
	auto& extent = rg.ctx().get<WindowExtent>();
	auto& exit = rg.ctx().get<AppExit>();
	auto& input = rg.ctx().get<CameraInput>();

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_QUIT ||
			event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
			exit.requested = true;
		} else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
			extent.width = static_cast<uint32_t>(event.window.data1);
			extent.height = static_cast<uint32_t>(event.window.data2);
		} else if (event.type == SDL_EVENT_KEY_DOWN) {
			switch (event.key.key) {
				case SDLK_ESCAPE:
					exit.requested = true;
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

// 窗口尺寸变化（WindowExtent 与 swapchain 实际尺寸不一致）时重建 swapchain + 深度图。
// 深度图必须与 swapchain extent 同步重建，否则 renderArea 会超出图像范围。
std::expected<void, Error> resizeSystem(entt::registry& rg, RGL::Swapchain& swapchain,
	RGL::Resources& resources) {
	auto& extent = rg.ctx().get<WindowExtent>();
	auto current = swapchain.getExtent();
	if (extent.width == current.width && extent.height == current.height) {
		return {};
	}

	swapchain.recreate();
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

// 每帧 push constant 存储（帧数据：活过 renderSystem 循环，DrawCall 的 span 指向其中；
// vkCmdPushDataEXT 录制时读取，只要求内存活到录制结束）
struct RenderListState {
	RGL::DrawArgs cubeArgs{};  // cube（64B，types_generated static_assert 与 shader 反射一致）
	std::vector<RGL::ModelDrawArgs> modelArgs;  // 模型每 primitive 一份（96B，model_types_generated）
};

// ViewProjection + 场景实体 + 场景旋转 → FrameDrawList context（单一 draw 路径）。
//
// cube（过渡态，尚未 entity 化）：管线条目存在性 = 画不画（showCube 在构造期 gating
// "cube" 条目），MVP = proj·view·spin（无 Transform）；Phase 4 入 ECS 后走与 model
// 同一 view 路径（ecs_compliance_review E1.4/H1）。
//
// model：每 primitive 实体（Transform/MeshRef/MaterialRef）推一份 ModelDrawArgs
//（mvp = proj·view·spin·world，按 primitiveIndex 对齐管线表条目 / heap slot）；
// group 数 = ceil(triangles / kTrianglesPerGroup)（契约常量由 model_schema.lua 生成，
// 见 model_types_generated；mesh shader 无 task stage，group 序号经 SV_GroupID.x 派生）
void renderListSystem(entt::registry& rg, RGL::Renderer& renderer, RenderListState& state) {
	auto& list = rg.ctx().get<FrameDrawList>();
	const auto& viewProj = rg.ctx().get<ViewProjection>();
	const auto& spin = rg.ctx().get<SceneSpin>();

	FrameDrawList fresh;

	if (auto cubeIdx = renderer.pipelineIndex("cube")) {
		state.cubeArgs.mvp = viewProj.proj * viewProj.view * spin.matrix;
		fresh.calls.push_back(DrawCall{
			*cubeIdx, std::as_bytes(std::span<const RGL::DrawArgs>{&state.cubeArgs, 1}), 1});
	}

	rg.view<RGL::Transform, RGL::MeshRef, RGL::MaterialRef>().each(
		[&](RGL::Transform& tf, RGL::MeshRef& meshRef, RGL::MaterialRef& matRef) {
			if (meshRef.primitiveIndex >= state.modelArgs.size())
				state.modelArgs.resize(meshRef.primitiveIndex + 1);
			auto& args = state.modelArgs[meshRef.primitiveIndex];
			args.mvp = viewProj.proj * viewProj.view * spin.matrix * tf.world;
			args.totalTriangles = meshRef.triangleCount;
			args.baseColor = matRef.baseColor;

			auto idx = renderer.pipelineIndex("model_" + std::to_string(meshRef.primitiveIndex));
			if (!idx) return;
			const uint32_t groupCount =
				(args.totalTriangles + kTrianglesPerGroup - 1) / kTrianglesPerGroup;
			if (groupCount == 0) return;
			fresh.calls.push_back(DrawCall{
				*idx, std::as_bytes(std::span<const RGL::ModelDrawArgs>{&args, 1}), groupCount});
		});

	list.calls = std::move(fresh.calls);
}

// 每帧 system（执行顺序 = 注册顺序，即数据流契约；新增 system = 此处 push_back）
struct FrameSystem {
	const char* name;
	std::function<void(entt::registry&, float dt)> fn;
};

export std::expected<void, Error> renderSystem(entt::registry& rg, RGL::Renderer& renderer) {
	auto& swapchain = renderer.getSwapchainRef();
	auto& resources = renderer.getResourcesRef();
	auto* logger = RLLogger::getInstance();
	std::optional<Error> lastError;

	RenderListState renderListState;

	// system 顺序（数据流契约）：
	//   camera（CameraInput→Camera）→ animation（SceneSpin）→ bounds（Bounds→SceneBounds）
	//   → projection（Camera+SceneBounds+WindowExtent→ViewProjection）→ renderlist（→FrameDrawList）
	std::vector<FrameSystem> systems{
		{"camera",     [](entt::registry& rg, float dt) { cameraSystem(rg, dt); }},
		{"animation",  [](entt::registry& rg, float dt) { animationSystem(rg, dt); }},
		{"bounds",     [](entt::registry& rg, float) { boundsSystem(rg); }},
		{"projection", [](entt::registry& rg, float) { projectionSystem(rg); }},
		{"renderlist", [&](entt::registry& rg, float) { renderListSystem(rg, renderer, renderListState); }},
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

		auto result = renderer.drawFrame(rg.ctx().get<FrameDrawList>());
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
