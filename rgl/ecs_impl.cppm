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
import scene_context;
import camera;
import rllogger;

namespace RGL {

// 帧循环各 system + renderSystem 定义（2026-09-02 编译提速 A 方案）：system 体移出
// 接口 TU——BMI 只含 renderSystem 声明与 context struct；本单元是叶子边。

/**
 * 中央事件轮询（唯一 SDL_PollEvent 调用点）：把 SDL 事件流翻译进 registry context——
 * 窗口状态（WindowExtent/AppExit 直接写入，不走事件流）+ 输入事件流（InputEvents：
 * 本 system 只产事件，不做键位映射——键绑定在 Camera.keys，cameraSystem 消费事件时
 * 按绑定解释，E5.4；退出/窗口状态不走事件流）。
 * 事件携带内容：键事件 = SDL 键码；鼠标按键 = button + pressed；鼠标增量取
 * motion.xrel/yrel（SDL3 的 motion.x/y 是窗口内绝对坐标，相对增量在 xrel/yrel）；
 * 滚轮 = wheel.y。
 * @param rg registry（写 WindowExtent/AppExit/InputEvents）
 */
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

/**
 * 窗口尺寸变化（WindowExtent 与 swapchain 实际尺寸不一致）时重建 swapchain + 深度图。
 * 深度图必须与 swapchain extent 同步重建，否则 renderArea 会超出图像范围。
 * recreate 返回 false（窗口尺寸超出 swapchain 可达范围：maxImageExtent/compositor
 * 钳制，重建无益）时跳过深度重建——每帧 waitIdle + 销毁重建 = 持续 GPU 停顿，
 * 只 warn 一次。
 * @param rg registry（读 WindowExtent）
 * @param swapchain 交换链层（extent 比较 / recreate 重查 caps/format/presentMode）
 * @param resources 资源层（recreateDepthImage）
 * @return void；深度图重建失败 → Error
 */
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

/**
 * 场景整体时间旋转（SceneSpin）：angle += dt * speed；matrix = rotate(axis, angle)。
 * speed = 0 → 直接返回，matrix 不更新（configs/scene.lua 的 spin 开关，main
 * 启动期转成 speed）。现状承载"全场景同一旋转"（renderListSystem 乘进各
 * MVP）；per-entity 旋转需求出现时改写 Transform 组件，结构同构，无需二次重构
 *（ecs_compliance_review E4.1）。
 * @param rg registry（读写 SceneSpin）
 * @param dt 帧间隔（秒，调用方已限幅到 0.1s）
 */
void animationSystem(entt::registry& rg, float dt) {
	auto& spin = rg.ctx().get<SceneSpin>();
	if (spin.speed == 0.0f) {
		return;
	}
	spin.angle += spin.speed * dt;
	spin.matrix = glm::rotate(glm::mat4(1.0f), spin.angle, spin.axis);
}

/**
 * union 包围球（标准公式；radius = 0 视为空球）：a 包含 b → a；b 包含 a → b；
 * 否则 r = (ra+rb+d)/2，中心从 a 向 b 滑动 t = ra/(d+r)。
 * @param a 包围球 A（空球 = radius 0，直接返回 b）
 * @param b 包围球 B（空球 = radius 0，直接返回 a）
 * @return 同时包含两者的包围球
 */
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

/**
 * 聚合全部 Bounds 组件 → SceneBounds context（unionSphere 逐个合并，空场景 =
 * radius 0）。现状场景静态（Bounds 仅导入期写入），每帧重聚合为未来动态场景
 * 变更留接缝。
 * @param rg registry（遍历 Bounds view，写 SceneBounds）
 */
void boundsSystem(entt::registry& rg) {
	auto& sceneBounds = rg.ctx().get<SceneBounds>();
	SceneBounds acc{};
	rg.view<Bounds>().each([&](const Bounds& b) {
		acc = unionSphere(acc, SceneBounds{b.center, b.radius});
	});
	sceneBounds = acc;
}

/**
 * Camera + 场景包围球 + 窗口尺寸 → ViewProjection context。
 * 自适应 near/far：SceneBounds 非空时按相机到球心实时距离 ± 球半径推导
 *（near 下限 0.05 防相机进球内），保证任意相机位置/场景尺度下整个场景在
 * frustum 内；无场景内容时回落 configs/camera.lua 的固定 near/far。
 * @param rg registry（读 Camera/SceneBounds/WindowExtent，写 ViewProjection）
 */
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


/**
 * 每帧 system（执行顺序 = 注册顺序，即数据流契约；新增 system = 此处 push_back）。
 */
struct FrameSystem {
	const char* name;  // system 名（文档/日志标识，执行路径不读取）
	std::function<void(entt::registry&, float dt)> fn;
};

/** 实现流水（语义/契约见接口声明）：构建显式 system 列表（camera→animation→bounds→
 * projection→renderlist）→ 主循环：eventSystem → AppExit 判定 → dt clamp [0, 0.1s]
 * → resizeSystem（失败 = waitForIdle 后退出循环返回 Error）→ system 链 →
 * texel 金丝雀（帧 N 一次性，drawFrame 前）→ llm 捕获请求（drawFrame 前，
 * dump_gbuf 通道一并传入）→ drawFrame（失败 = 记录 lastError 退出循环）→
 * llm 捕获消费（真有产物时调 llmCapture 写 PNG + 置 AppExit，跳帧时请求保持
 * pending 重试）/ onFrame 回调 → 循环外 waitForIdle 后返回 lastError（若有）。 */
std::expected<void, Error> renderSystem(entt::registry& rg, RGL::Renderer& renderer,
	const RGL::LlmDebugConfig* llm,
	std::function<std::expected<std::string, Error>(RGL::Renderer&)>
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

	// 帧时间统计（diagnostics.frame_stats = N：每 N 帧一行 avg/min/max，0 = 关）：
	// 帧循环整周期（input+systems+drawFrame），headless 无 present 限帧时 ≈ GPU/提交吞吐
	const uint32_t statsN = diagnostics().frameStats;
	uint64_t statsPrevNs = 0;
	double statsSum = 0.0, statsMin = 1e30, statsMax = 0.0;
	uint32_t statsCount = 0;

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

		// llm 帧捕获（2026-09-08：截图 = 帧内回读本帧真实输出，离屏重渲退役）：
		// drawFrame 前请求（vkrenderer 帧内钩子，dump_gbuf 通道一并传入）；drawFrame 后
		// 仅当真有捕获产物时消费写 PNG 并请求退出（acquire 超时跳帧 = 请求保持 pending 重试）
		const bool wantCapture = llm && llmCapture && llmFrame >= llm->delayFrames;
		if (wantCapture)
			renderer.requestFrameCapture(diagnostics().dumpGbuf);

		auto result = renderer.drawFrame(rg.ctx().get<FrameDrawList>());
		if (!result) {
			if (logger) logger->error("渲染错误: {}", result.error().describe());
			lastError = result.error();
			break;
		}
		if (wantCapture) {
			if (renderer.hasFrameCapture()) {
				auto shot = llmCapture(renderer);
				if (shot) {
					if (logger) logger->info("llm_debug: 截图已保存 ({})", *shot);
				} else {
					if (logger) logger->error("llm_debug: 截图失败 ({})", shot.error().describe());
					lastError = shot.error();
				}
				rg.ctx().get<AppExit>().requested = true;
			}
		}
		if (llm && llmCapture)
			++llmFrame;  // 每帧计数（含跳帧），wantCapture 判定的基准
		if (onFrame)
			onFrame(frame);
		++frame;

		if (statsN > 0) {
			const uint64_t nowNs = SDL_GetTicksNS();  // 帧尾 = 本帧整周期终点
			if (statsPrevNs != 0) {
				const double ms = static_cast<double>(nowNs - statsPrevNs) / 1'000'000.0;
				statsSum += ms;
				++statsCount;
				statsMin = std::min(statsMin, ms);
				statsMax = std::max(statsMax, ms);
			}
			statsPrevNs = nowNs;
			if (statsCount >= statsN) {
				if (logger)
					logger->info("frame stats: avg={:.3f}ms min={:.3f} max={:.3f} ({} 帧)",
						statsSum / statsCount, statsMin, statsMax, statsCount);
				statsSum = 0.0;
				statsCount = 0;
				statsMin = 1e30;
				statsMax = 0.0;
			}
		}
	}

	renderer.waitForIdle();
	if (lastError) {
		return std::unexpected(std::move(*lastError));
	}
	return {};
}

}  // namespace RGL
