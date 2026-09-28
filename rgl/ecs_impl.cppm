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
import asset_table;  // AssetTable（renderSystem 参数透传；F 拆出）
import vk_renderer;
import frame_draw_list;
import render_list;
import runtime_config;  // LlmDebugConfig（renderSystem 参数类型）
import diagnostics;     // texel_canary 诊断单例（configs/diagnostics.lua）
import input_probe;     // 脚本化输入探针（2026-09-28 输入链回归护栏，帧循环 tick）
import profiler;        // Phase 9 帧相 zone（frameBegin + 帧循环拦截点，docs/phase9_plan.md）
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
 * 滚轮 = wheel.y。鼠标事件另带窗口内绝对坐标 pos（Phase 10 点选）。
 * 事件流先手 = InputFilter 钩子（registry context 可选，imgui 调试层消费的事件
 * 不产 InputEvents；缺省空 = 逐位不变）。
 * @param rg registry（写 WindowExtent/AppExit/InputEvents；读 InputFilter）
 */
void eventSystem(entt::registry& rg) {
	auto& extent = rg.ctx().get<WindowExtent>();
	auto& exit = rg.ctx().get<AppExit>();
	auto& events = rg.ctx().get<InputEvents>();
	auto* filter = rg.ctx().find<InputFilter>();  // Phase 10 调试层可选挂接（main 装配；缺省 = 不过滤）
	auto* logger = RLLogger::getInstance();
	int swallowed = 0;  // 本帧被调试层吞没的输入事件数（debug 级可观测，2026-09-28 配套）

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		// 调试 UI 先手：imgui 消费的事件不产 InputEvents（相机不响应面板拖拽）。
		// void* = InputFilter 类型擦除约定（见 ecs.cppm InputFilter 注释）。
		if (filter && filter->consume && filter->consume(&event)) {
			++swallowed;
			continue;
		}
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
				e.pos = {event.button.x, event.button.y};  // 窗口内绝对坐标（Phase 10 点选）
				// picking 请求（Phase 10 批 10.5）：走到这里 = 已过 InputFilter（imgui
				// 控件区域内的点击被吞），左键 = 点选；钩子未挂 = 空函数零调用
				if (event.button.button == SDL_BUTTON_LEFT) {
					if (auto* pk = rg.ctx().find<PickingHook>(); pk && pk->requestPick)
						pk->requestPick(event.button.x, event.button.y);
				}
				events.events.push_back(e);
				break;
			}
			case SDL_EVENT_MOUSE_BUTTON_UP: {
				InputEvent e;
				e.type = InputEvent::Type::MouseButton;
				e.button = static_cast<uint8_t>(event.button.button);
				e.pressed = false;
				e.pos = {event.button.x, event.button.y};
				events.events.push_back(e);
				break;
			}
			case SDL_EVENT_MOUSE_MOTION: {
				// 注意：SDL3 的 motion.x/y 是窗口内绝对坐标，相对增量在 xrel/yrel
				InputEvent e;
				e.type = InputEvent::Type::MouseDelta;
				e.delta = {static_cast<float>(event.motion.xrel),
					static_cast<float>(event.motion.yrel)};
				e.pos = {event.motion.x, event.motion.y};  // 绝对坐标同带（Phase 10）
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
	if (swallowed > 0 && logger)
		logger->debug("eventSystem: 本帧 {} 个输入事件被 InputFilter（调试层）吞没", swallowed);
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

namespace {

/** texel 回读金丝雀单次触发（docs/archive/heap_texture_research.md §12.7 ②）：
 * texel_canary=N（帧号，默认关）——帧 N 的 drawFrame 前重读白纹理已知 texel 比对，
 * 纹理 BO 死 = 立即报错（GPU 侧 UAF 签名，ASan 全盲；诊断路径单次 GPU 停顿，
 * 正常路径零影响）。texel_canary = configs/diagnostics.lua（原 RGL_TEXEL_CANARY
 * env 2026-09-08 转配置）。触发时机 = 本帧 drawFrame 前（waitForIdle 已在函数内
 * 排空 in-flight）；fired 由调用方持有，保证至多触发一次。 */
void texelCanaryOnce(bool& fired, uint64_t canaryFrame, uint32_t frame,
	RGL::Renderer& renderer, const RGL::AssetTable& assets) {
	if (fired || frame < canaryFrame) {
		return;
	}
	fired = true;
	texelCanaryCheck(renderer.getResourcesRef(), assets, renderer.getDeviceContextRef(),
		renderer.getGraphicsCommandPoolRef(), frame);
}

/** resize 处理 + 失败日志（文案原样）：转调 resizeSystem；失败打 "Resize 错误: {}"
 * 并把错误原样上抛（调用方 waitForIdle 后退出循环返回）。 */
std::expected<void, Error> resizeChecked(entt::registry& rg, RGL::Swapchain& swapchain,
	RGL::Resources& resources, RLLogger* logger) {
	auto result = resizeSystem(rg, swapchain, resources);
	if (!result && logger) {
		logger->error("Resize 错误: {}", result.error().describe());
	}
	return result;
}

/** drawFrame + 失败日志（文案原样）：成功返回空 optional；失败打 "渲染错误: {}"
 * 并把错误上抛（调用方记录 lastError 后 break）。 */
std::optional<Error> drawFrameChecked(RGL::Renderer& renderer, entt::registry& rg,
	RLLogger* logger) {
	auto result = renderer.drawFrame(rg.ctx().get<FrameDrawList>());
	if (result) {
		return {};
	}
	if (logger) {
		logger->error("渲染错误: {}", result.error().describe());
	}
	return result.error();
}

/** llm 帧捕获节奏（2026-09-08：截图 = 帧内回读本帧真实输出，离屏重渲退役）：
 * drawFrame 前请求（vkrenderer 帧内钩子，dump_gbuf 通道一并传入）；drawFrame 后
 * 仅当真有捕获产物时消费写 PNG 并请求退出（acquire 超时跳帧 = 请求保持 pending 重试）。 */
struct LlmCaptureFlow {
	/** 截图配置（nullptr = llm_debug 未启用）。 */
	const RGL::LlmDebugConfig* llm;
	/** 捕获写盘回调（空 = 未启用；引用 renderSystem 形参，同生命周期）。 */
	std::function<std::expected<std::string, Error>(RGL::Renderer&)>& capture;
	/** 每帧计数（含跳帧），wantCapture 判定的基准（仅 llm 启用时递增）。 */
	uint32_t llmFrame = 0;

	/** 是否到捕获时机：llm 启用且已过 delayFrames 帧。 */
	bool wanted() const {
		return llm && capture && llmFrame >= llm->delayFrames;
	}

	/** drawFrame 前挂本帧回读请求（dump_gbuf 通道一并传入）；未到时机不动作。 */
	void requestBeforeDraw(RGL::Renderer& renderer) const {
		if (wanted()) {
			renderer.requestFrameCapture(diagnostics().dumpGbuf);
		}
	}

	/** drawFrame 后消费：真产物时调 consumeShot 写 PNG + 置 AppExit；llm 启用时
	 * 无论是否命中每帧递增 llmFrame（在消费之后，与逐帧时序一致）。
	 * @return 写盘失败错误（调用方记录 lastError；成功或无产物 = 空） */
	std::optional<Error> afterDraw(entt::registry& rg, RGL::Renderer& renderer,
		RLLogger* logger) {
		if (llm && capture) {
			std::optional<Error> error;
			if (wanted() && renderer.hasFrameCapture()) {
				error = consumeShot(rg, renderer, logger);
			}
			++llmFrame;  // 每帧计数（含跳帧），wantCapture 判定的基准
			return error;
		}
		return {};
	}

	/** 真有产物时调捕获回调写 PNG + 成功/失败日志（文案原样）+ 置 AppExit。
	 * @return 写盘失败错误（成功 = 空） */
	std::optional<Error> consumeShot(entt::registry& rg, RGL::Renderer& renderer,
		RLLogger* logger) {
		auto shot = capture(renderer);
		std::optional<Error> error;
		if (shot) {
			if (logger) logger->info("llm_debug: 截图已保存 ({})", *shot);
		} else {
			if (logger) logger->error("llm_debug: 截图失败 ({})", shot.error().describe());
			error = shot.error();
		}
		rg.ctx().get<AppExit>().requested = true;
		return error;
	}
};

/** 帧时间统计（diagnostics.frame_stats = N：每 N 帧一行 avg/min/max，0 = 关）：
 * 帧循环整周期（input+systems+drawFrame），headless 无 present 限帧时 ≈ GPU/提交吞吐。 */
struct FrameStatsTracker {
	/** 打印周期 N（0 = 关闭）。 */
	uint32_t n;
	/** 上一帧帧尾时刻（ns；0 = 首帧只记起点、不累积）。 */
	uint64_t prevNs = 0;
	/** 本周期帧时长累加（ms）。 */
	double sumMs = 0.0;
	/** 本周期最小帧时长（ms）。 */
	double minMs = 1e30;
	/** 本周期最大帧时长（ms）。 */
	double maxMs = 0.0;
	/** 本周期已累积帧数（达 N 打印并清零）。 */
	uint32_t count = 0;

	/** 帧尾调用：累积本帧整周期；周期满 N 帧打一行 avg/min/max 并复位。 */
	void tick() {
		if (n == 0) {
			return;
		}
		const uint64_t nowNs = SDL_GetTicksNS();  // 帧尾 = 本帧整周期终点
		if (prevNs != 0) {
			const double ms = static_cast<double>(nowNs - prevNs) / 1'000'000.0;
			sumMs += ms;
			++count;
			minMs = std::min(minMs, ms);
			maxMs = std::max(maxMs, ms);
		}
		prevNs = nowNs;
		if (count >= n) {
			if (auto logger = RLLogger::getInstance())
				logger->info("frame stats: avg={:.3f}ms min={:.3f} max={:.3f} ({} 帧)",
					sumMs / count, minMs, maxMs, count);
			sumMs = 0.0;
			count = 0;
			minMs = 1e30;
			maxMs = 0.0;
		}
	}
};

}  // namespace

/** 配置热重载帧边界（Phase 10 批 10.3；renderSystem 帧首调用）：eventSystem 之后、
 * 本帧任何 system/draw 之前 = 上一帧已 submit、本帧未录制的换入点。钩子未挂
 * （config_reload 门默认关，main 不 emplace）= 空指针早退零开销。 */
void configReloadTick(entt::registry& rg) {
	auto* hook = rg.ctx().find<ConfigReloadHook>();
	if (!hook || !hook->poll)
		return;
	hook->poll();
}

/** picking 结果 tick（Phase 10 批 10.5；renderSystem 帧首调用，同 configReloadTick
 *  帧边界）：消费 PickingService 回读 → objectSlot 反查 MeshRef entity（点击触发，
 *  O(n) 扫一次可接受，不平行维护 slot→entity 映射表）→ 写 SelectedObject ctx。
 *  钩子未挂 = 空指针早退零开销；无新结果/空像素 = 保持上次选中（v1 不做清空）。 */
void pickingTick(entt::registry& rg) {
	auto* hook = rg.ctx().find<PickingHook>();
	if (!hook || !hook->takeResult)
		return;
	// 脚本化探针（diagnostics.picking_probe = {frame,x,y}）：第 frame 帧发一次请求
	// ——headless 无鼠标验证全链（本帧 request → 帧末 record → 下帧此处置集）
	static uint32_t frameCount = 0;
	const auto& probe = diagnostics().pickingProbeFrame;
	if (probe > 0 && hook->requestPick && frameCount++ == probe) {
		if (auto* logger = RLLogger::getInstance())
			logger->info("picking: probe 第 {} 帧请求 ({}, {})", probe,
				diagnostics().pickingProbeX, diagnostics().pickingProbeY);
		hook->requestPick(static_cast<float>(diagnostics().pickingProbeX),
			static_cast<float>(diagnostics().pickingProbeY));
	}
	const auto slot = hook->takeResult();
	if (!slot)
		return;
	if (!rg.ctx().contains<SelectedObject>())
		rg.ctx().insert_or_assign<SelectedObject>(SelectedObject{});
	auto& sel = rg.ctx().get<SelectedObject>();
	sel.objectSlot = *slot;
	sel.entity = static_cast<entt::entity>(entt::null);
	for (const auto entity : rg.view<MeshRef>())
		if (rg.get<MeshRef>(entity).objectSlot == *slot) {
			sel.entity = entity;
			break;
		}
	if (auto* logger = RLLogger::getInstance())
		logger->info("picking: objectSlot {} → entity {}{}", *slot,
			static_cast<std::uint32_t>(sel.entity),
			sel.entity == static_cast<entt::entity>(entt::null)
				? "（无匹配 MeshRef，选中保持空）" : "");
}

/** 实现流水（语义/契约见接口声明）：构建显式 system 列表（camera→animation→bounds→
 * projection→renderlist）→ 主循环：eventSystem → AppExit 判定 → dt clamp [0, 0.1s]
 * → resizeSystem（失败 = waitForIdle 后退出循环返回 Error）→ system 链 →
 * texel 金丝雀（帧 N 一次性，drawFrame 前）→ llm 捕获请求（drawFrame 前，
 * dump_gbuf 通道一并传入）→ drawFrame（失败 = 记录 lastError 退出循环）→
 * llm 捕获消费（真有产物时调 llmCapture 写 PNG + 置 AppExit，跳帧时请求保持
 * pending 重试）/ onFrame 回调 → 循环外 waitForIdle 后返回 lastError（若有）。 */
std::expected<void, Error> renderSystem(entt::registry& rg, RGL::Renderer& renderer,
	const RGL::AssetTable& assets,
	const RGL::LlmDebugConfig* llm,
	std::function<std::expected<std::string, Error>(RGL::Renderer&)>
		llmCapture,
	std::function<void(uint32_t)> onFrame) {
	auto& swapchain = renderer.getSwapchainRef();
	auto& resources = renderer.getResourcesRef();
	auto* logger = RLLogger::getInstance();
	std::optional<Error> lastError;

	RenderListState renderListState;
	uint32_t frame = 0;  // 每帧无条件计数（onFrame DIAG 回调用；llmFrame 在捕获流内、仅 llm 启用时递增）
	// texel 回读金丝雀：帧号门控 + 单次触发标记（触发逻辑见 texelCanaryOnce）
	const uint64_t canaryFrame = diagnostics().texelCanary;
	bool canaryFired = false;

	// 帧时间统计（状态与打印见 FrameStatsTracker；tick 挂在帧尾）
	FrameStatsTracker stats{diagnostics().frameStats};
	// llm 帧捕获节奏（请求/消费时序与状态见 LlmCaptureFlow）
	LlmCaptureFlow capture{llm, llmCapture};

	// system 顺序（数据流契约）：
	//   camera（InputEvents→Camera，消费即清空）→ animation（SceneSpin）→ bounds（Bounds→SceneBounds）
	//   → projection（Camera+SceneBounds+WindowExtent→ViewProjection）→ renderlist（→FrameDrawList）
	std::vector<FrameSystem> systems{
		{"camera",     [](entt::registry& rg, float dt) { cameraSystem(rg, dt); }},
		{"animation",  [](entt::registry& rg, float dt) { animationSystem(rg, dt); }},
		{"bounds",     [](entt::registry& rg, float) { boundsSystem(rg); }},
		{"projection", [](entt::registry& rg, float) { projectionSystem(rg); }},
		{"renderlist", [&](entt::registry& rg, float) { renderListSystem(rg, resources, renderer.getEnvironmentRef(), assets, renderListState); }},
	};

	float prevTicks = static_cast<float>(SDL_GetTicks());

	while (true) {
		profilerFrameBegin();  // Phase 9：帧槽推进 + 帧墙钟基准（off = 早退）
		{
			auto z = zone("events");  // 拦截点 1（phase9_plan §2.3）：SDL 事件轮询
			eventSystem(rg);
		}
		if (rg.ctx().get<AppExit>().requested) {
			break;
		}

		// 配置热重载帧边界（Phase 10 批 10.3）：本帧任何 system/draw 之前套用变更
		configReloadTick(rg);

		// picking 结果帧边界（Phase 10 批 10.5）：消费上帧回读 → 反查 entity → ctx
		pickingTick(rg);

		// 脚本化输入探针（2026-09-28 输入链回归护栏；配置门控，默认关 = 早退零开销）
		uiClickProbeTick();
		cameraProbeTick(rg);

		float ticks = static_cast<float>(SDL_GetTicks());
		// dt 限幅：窗口卡顿/切后台回来时防止相机瞬移
		float dt = std::clamp((ticks - prevTicks) / 1000.0f, 0.0f, 0.1f);
		prevTicks = ticks;

		{
			auto z = zone("resize");  // swapchain/深度重建（罕见路径，触发时单列）
			auto resizeResult = resizeChecked(rg, swapchain, resources, logger);
			if (!resizeResult) {
				renderer.waitForIdle();
				return std::unexpected(std::move(resizeResult).error());
			}
		}

		for (auto& system : systems) {
			auto z = zone(system.name);  // 拦截点 1 核心：显式 system 列表全量计时
			system.fn(rg, dt);
		}

		// texel 金丝雀（本帧 drawFrame 前，waitForIdle 已在函数内排空 in-flight）
		texelCanaryOnce(canaryFired, canaryFrame, frame, renderer, assets);

		// llm 帧捕获：drawFrame 前请求 → drawFrame → 真产物才消费（时序契约见 LlmCaptureFlow）
		capture.requestBeforeDraw(renderer);
		{
			auto z = zone("drawFrame");  // CPU 命令录制主段（vs 9.2 GPU 跨度对比）
			if (auto drawError = drawFrameChecked(renderer, rg, logger)) {
				lastError = std::move(drawError);
				break;
			}
		}
		{
			auto z = zone("capture");  // llm 帧回读（真截图帧才有 GPU 同步等待）
			if (auto captureError = capture.afterDraw(rg, renderer, logger)) {
				lastError = std::move(captureError);
			}
		}
		if (onFrame)
			onFrame(frame);
		++frame;

		stats.tick();
	}

	renderer.waitForIdle();
	if (lastError) {
		return std::unexpected(std::move(*lastError));
	}
	return {};
}

}  // namespace RGL
