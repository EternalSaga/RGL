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
import llm_debug;
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
	std::vector<RGL::ModelDrawArgs> modelArgs;  // 模型每资产一份（96B，model_types_generated；按资产 id 索引）
};

// ViewProjection + 场景实体 + 场景旋转 → FrameDrawList context（单一 draw 路径）。
//
// cube（过渡态，尚未 entity 化）：管线条目存在性 = 画不画（showCube 在构造期 gating
// "cube" 条目），MVP = proj·view·spin（无 Transform）；Phase 4 入 ECS 后走与 model
// 同一 view 路径（ecs_compliance_review E1.4/H1）。
//
// model：每 primitive 实体（Transform/MeshRef{asset}/MaterialRef）推一份 ModelDrawArgs
//（mvp = proj·view·spin·world；资产 id → Resources 资产表记录：三角形数 / 管线条目
// 下标，五方约定收敛为一处映射，E1.1/E5.1）；
// group 数 = ceil(triangles / kTrianglesPerGroup)（契约常量由 model_schema.lua 生成，
// 见 model_types_generated；mesh shader 无 task stage，group 序号经 SV_GroupID.x 派生）
void renderListSystem(entt::registry& rg, RGL::Renderer& renderer, RGL::Resources& resources,
	RenderListState& state) {
	auto& list = rg.ctx().get<FrameDrawList>();
	const auto& viewProj = rg.ctx().get<ViewProjection>();
	const auto& spin = rg.ctx().get<SceneSpin>();
	const auto& assets = resources.getAssetTable();

	FrameDrawList fresh;

	// DIAG（RGL_CUBE_TEX）："cube_tex" 条目优先（cube + 纹理采样实验管线，见 main.cpp 门）
	auto cubeIdx = renderer.pipelineIndex("cube_tex");
	if (!cubeIdx)
		cubeIdx = renderer.pipelineIndex("cube");
	if (cubeIdx) {
		state.cubeArgs.mvp = viewProj.proj * viewProj.view * spin.matrix;
		fresh.calls.push_back(DrawCall{
			*cubeIdx, std::as_bytes(std::span<const RGL::DrawArgs>{&state.cubeArgs, 1}), 1});
	}

	rg.view<RGL::Transform, RGL::MeshRef, RGL::MaterialRef>().each(
		[&](RGL::Transform& tf, RGL::MeshRef& meshRef, RGL::MaterialRef& matRef) {
			const auto& asset = assets.get(meshRef.asset);
			if (asset.pipelineIndex == kInvalidPipelineIndex)
				return;  // 资产无管线条目（防御：注册顺序异常，见 registerScene/addModelPipeline）
			if (meshRef.asset >= state.modelArgs.size())
				state.modelArgs.resize(meshRef.asset + 1);
			auto& args = state.modelArgs[meshRef.asset];
			args.mvp = viewProj.proj * viewProj.view * spin.matrix * tf.world;
			args.totalTriangles = asset.triangleCount;
			args.baseColor = matRef.baseColor;

			const uint32_t groupCount =
				(asset.triangleCount + kTrianglesPerGroup - 1) / kTrianglesPerGroup;
			if (groupCount == 0) return;
			fresh.calls.push_back(DrawCall{
				asset.pipelineIndex,
				std::as_bytes(std::span<const RGL::ModelDrawArgs>{&args, 1}), groupCount});
		});

	list.calls = std::move(fresh.calls);
}

// 每帧 system（执行顺序 = 注册顺序，即数据流契约；新增 system = 此处 push_back）
struct FrameSystem {
	const char* name;
	std::function<void(entt::registry&, float dt)> fn;
};

// llm（可空）：llm_debug 一次性截图配置（非空 = 启用）。批处理语义：
// 第 llm->delayFrames 帧渲染完成后离屏重渲 + 存 PNG，然后置 AppExit 退出——
// 无常驻状态、无 mailbox（config 即控制通道：agent 改 configs/camera.lua 后重跑）。
// onFrame（DIAG，可空）：每帧 drawFrame 完成后回调（帧号从 0 起）——
// 可见窗 GPUVM fault 排查用（RGL_SHOW_AFTER：隐藏窗启动、第 N 帧后显示，
// 隔离"分配期状态"与"present 期状态"）；默认空 = 无回调，正常路径零变化
export std::expected<void, Error> renderSystem(entt::registry& rg, RGL::Renderer& renderer,
	const RGL::LlmDebugConfig* llm = nullptr, std::function<void(uint32_t)> onFrame = {}) {
	auto& swapchain = renderer.getSwapchainRef();
	auto& resources = renderer.getResourcesRef();
	auto* logger = RLLogger::getInstance();
	std::optional<Error> lastError;

	RenderListState renderListState;
	uint32_t llmFrame = 0;
	uint32_t frame = 0;  // 每帧无条件计数（onFrame DIAG 回调用；llmFrame 仅在 llm 启用时递增）

	// system 顺序（数据流契约）：
	//   camera（InputEvents→Camera，消费即清空）→ animation（SceneSpin）→ bounds（Bounds→SceneBounds）
	//   → projection（Camera+SceneBounds+WindowExtent→ViewProjection）→ renderlist（→FrameDrawList）
	std::vector<FrameSystem> systems{
		{"camera",     [](entt::registry& rg, float dt) { cameraSystem(rg, dt); }},
		{"animation",  [](entt::registry& rg, float dt) { animationSystem(rg, dt); }},
		{"bounds",     [](entt::registry& rg, float) { boundsSystem(rg); }},
		{"projection", [](entt::registry& rg, float) { projectionSystem(rg); }},
		{"renderlist", [&](entt::registry& rg, float) { renderListSystem(rg, renderer, resources, renderListState); }},
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
		// llm_debug：本帧渲染完成后计数；超过延迟帧数 → 用本帧 draw list 离屏
		// 重渲截图（场景已推进 N 步），然后请求退出（下轮循环头 break）
		if (llm) {
			if (++llmFrame > llm->delayFrames) {
				auto shot = captureFrame(renderer, rg.ctx().get<FrameDrawList>(), *llm);
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
