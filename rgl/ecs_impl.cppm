module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>  // taaSystem 的 spin inverse（GMF include 不随 import 可见）
#include <ranges>
module ecs:impl;

import ecs;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import :frame_diag;  // 批 14.3：LlmCaptureFlow / FrameStatsTracker（帧循环诊断旁路，兄弟分区）
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
import param_registry;   // DebugPanelState（批 10.7 点选跳页请求位）
import scene_context;
import taa_jitter;  // 批 12.6 TAA：halton 抖动 / jitter→投影 / 跳切判据（纯函数层）
import camera;
import rllogger;

namespace RGL {

// 帧循环各 system + renderSystem 定义（2026-09-02 编译提速 A 方案）：system 体移出
// 接口 TU——BMI 只含 renderSystem 声明与 context struct；本单元是叶子边。

/**
 * Esc 取消点选（批 10.7；eventSystem 轮询循环内、**InputFilter 判定之前**调用）。
 *
 * 必须在 filter 之前判定：imgui 的 WantCaptureKeyboard 是**粘性**状态（点过面板后
 * 恒为真，见 git:调试历史 "面板吞键盘"事故），若让 Esc 先过 imgui 就会被吞掉、
 * 永远到不了这里。返回 true = 本事件是 Esc（调用方据此跳过 filter，键事件仍照常
 * 产 InputEvents——Esc 非相机绑定键，相机自然无响应）。
 */
bool handleEscapeDeselect(entt::registry& rg, const SDL_Event& event) {
	if (event.type != SDL_EVENT_KEY_DOWN || event.key.key != SDLK_ESCAPE)
		return false;
	// ctx 未 emplace = 无选中可清（picking 门关）
	if (auto it = rg.ctx().find<SelectedObject>(); it != nullptr) {
		it->entity = static_cast<entt::entity>(entt::null);
		if (auto* logger = RLLogger::getInstance())
			logger->info("picking: Esc 取消选中");
	}
	return true;
}

/**
 * 窗口 resize 事件的 extent 解析（eventSystem 的 RESIZED / PIXEL_SIZE_CHANGED 共用）：
 * 经 WindowPixelQuery 钩子**重新查询**像素尺寸写回——不信任事件携带的 data1/2
 *（RESIZED 带逻辑点、PIXEL_SIZE_CHANGED 带像素，Wayland 分数缩放下两者不同，而
 * swapchain/projection 口径是像素；同一次拖拽两条事件都会发，重查询幂等无害）。
 * 钩子未装配 / 查询结果非正 = 回落事件值（headless 兜底——headless 无 SDL 窗口恒不进本分支）。
 * @param rg registry（读 WindowPixelQuery，可未装配）
 * @param extent 写回目标（WindowExtent context）
 * @param event 触发本分支的 SDL 窗口事件（回落数据源）
 */
void applyWindowResizeExtent(entt::registry& rg, WindowExtent& extent, const SDL_Event& event) {
	auto* query = rg.ctx().find<WindowPixelQuery>();
	const std::optional<std::array<int, 2>> px =
		(query && query->pixels) ? query->pixels() : std::nullopt;
	if (px && (*px)[0] > 0 && (*px)[1] > 0) {
		extent.width = static_cast<uint32_t>((*px)[0]);
		extent.height = static_cast<uint32_t>((*px)[1]);
	} else {
		extent.width = static_cast<uint32_t>(event.window.data1);
		extent.height = static_cast<uint32_t>(event.window.data2);
	}
}

/**
 * 左键点选请求（eventSystem 内联逻辑提出，控 eventSystem 认知复杂度）。
 * **坐标换算在此**：SDL 鼠标事件是逻辑点，而点选回读的 objectID 附件按**渲染像素**
 * 索引 ⟹ 两级换算：
 *   ① 逻辑点 → 窗口像素：乘 `WindowScale`（未装配 = 1.0，等价于无缩放/无窗口路径）；
 *   ② 窗口像素 → 渲染像素：乘 `RenderExtent / WindowExtent`（批 14.3，renderScale≠1
 *      时才 ≠1；漏掉它 = 点窗口右下角会采样到 RT 之外，被 clamp 到边缘 → 打偏）。
 * 脚本化探针 pickingProbeTick 直接以像素坐标调钩子，故不经此处（否则会被二次缩放）。
 * 钩子未挂 = 零调用（与原内联写法等价）。
 *
 * 第 ② 级用**两个 extent 的比值**而不是直接读 renderScale：比值取自实际分配出的网格，
 * 取整后（1001×0.75=751 而非 750.75）比例仍然精确，不会与 RT 尺寸差半个像素。
 * WindowExtent 为 0（理论上不该发生：建窗即有尺寸）时比值回落 1.0 = 退化成旧行为。
 */
void requestPickAtCursor(entt::registry& rg, float logicalX, float logicalY) {
	auto* pk = rg.ctx().find<PickingHook>();
	if (!pk || !pk->requestPick)
		return;
	const auto* scale = rg.ctx().find<WindowScale>();
	const float s = scale ? scale->value : 1.0f;
	const auto& we = rg.ctx().get<WindowExtent>();
	const auto& re = rg.ctx().get<RenderExtent>();
	const float rx = we.width > 0u ? static_cast<float>(re.width) / static_cast<float>(we.width) : 1.0f;
	const float ry = we.height > 0u ? static_cast<float>(re.height) / static_cast<float>(we.height) : 1.0f;
	pk->requestPick(logicalX * s * rx, logicalY * s * ry);
}

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
		// 批 10.7：Esc 取消点选**必须排在 filter 之前**（imgui WantCaptureKeyboard 是
		// 粘性状态，放后面会被吞）——helper 只做「是不是 Esc / 清选中」，键事件仍
		// 照常产 InputEvents（Esc 非相机绑定键，相机自然无响应）。
		const bool isEscape = handleEscapeDeselect(rg, event);
		// 调试 UI 先手：imgui 消费的事件不产 InputEvents（相机不响应面板拖拽）。
		// void* = InputFilter 类型擦除约定（见 ecs.cppm InputFilter 注释）。
		if (!isEscape && filter && filter->consume && filter->consume(&event)) {
			++swallowed;
			continue;
		}
		switch (event.type) {
			case SDL_EVENT_QUIT:
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				exit.requested = true;
				break;
			case SDL_EVENT_WINDOW_RESIZED:
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				// extent 口径 = 窗口**物理像素**（与 swapchain extent 同口径）；重查不取事件值，
				// 两事件都接的理由与兜底 → applyWindowResizeExtent
				applyWindowResizeExtent(rg, extent, event);
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
				e.pos = {event.button.x, event.button.y};  // 窗口内绝对坐标 = **逻辑点**（Phase 10 点选）
				// picking 请求（Phase 10 批 10.5）：走到这里 = 已过 InputFilter（imgui
				// 控件区域内的点击被吞），左键 = 点选；逻辑点→帧缓冲像素的换算在 helper 内
				//（e.pos 保持逻辑点口径，与 SDL 事件一致；像素口径只给点选用）
				if (event.button.button == SDL_BUTTON_LEFT)
					requestPickAtCursor(rg, event.button.x, event.button.y);
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

	// 渲染尺寸每帧同步（批 14.3）：放在"尺寸没变就早退"**之前**——acquire/present 的
	// OUT_OF_DATE 路径会重建 swapchain 并改渲染 extent，却不改 WindowExtent，若挂在早退
	// 之后，那条路径下 RenderExtent 会永久停在旧值（投影比例与 RT 网格跨帧不一致）。
	// 两次 uint32 比较，正常帧成本可忽略。
	auto& renderExtent = rg.ctx().get<RenderExtent>();
	const auto newRender = swapchain.getRenderExtent();
	renderExtent.width = newRender.width;
	renderExtent.height = newRender.height;

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
	// 深度图按**渲染** extent 重建（批 14.3）。上面的比较仍用交换链 extent——WindowExtent 的
	// 口径定义就是窗口像素，换成渲染 extent 会让"窗口没变但 scale 变了"误判成需要重建。
	auto result = resources.recreateDepthImage(swapchain.getRenderExtent());
	if (!result) {
		return std::unexpected(std::move(result).error());
	}
	// 重建成功可观测（2026-10-03）：swapchain extent 是 clamp 后值，与窗口像素并打——
	// 两者不等 = surface 可达范围钳制生效，拖窗口时一眼看到重建与最终尺寸。
	// 批 14.3 起再并打渲染 extent：三者不等 = renderScale 生效，resize 后网格一眼可核。
	if (auto* logger = RLLogger::getInstance())
		logger->info("resizeSystem: swapchain rebuilt to {}x{}, render {}x{} (window pixel extent {}x{})",
			swapchain.getExtent().width, swapchain.getExtent().height,
			swapchain.getRenderExtent().width, swapchain.getRenderExtent().height,
			extent.width, extent.height);
	return {};
}

/**
 * 场景整体时间旋转（SceneSpin）：angle += dt * speed；matrix = rotate(axis, angle)。
 * speed = 0 → 直接返回，matrix 不更新（configs/scene.lua 的 spin 开关，main
 * 启动期转成 speed）。现状承载"全场景同一旋转"（renderListSystem 乘进各
 * MVP）；per-entity 旋转需求出现时改写 Transform 组件，结构同构，无需二次重构
 *（ecs_compliance_review E4.1）。
 *
 * 帧驱动门（`spin_frame_angle` ≠ 0，默认关）：门开走 angle = 帧号纯函数
 * （frameDrivenSpinAngle，单测钉），dt 累积停用——时域判据要跨进程渲染**同一帧序列**，
 * dt 累积随帧时长漂、测不出可复现的数（tools/imgqa/temporal.cpp 头注释）。
 * 门红线：frameAngle = 0（默认）时本分支不进，门关路径与今天逐位相同。
 * @param rg registry（读写 SceneSpin）
 * @param dt 帧间隔（秒，调用方已限幅到 0.1s；门开时不消费）
 */
void animationSystem(entt::registry& rg, float dt) {
	auto& spin = rg.ctx().get<SceneSpin>();
	if (spin.frameAngle != 0.0f) {  // 帧驱动门（默认 0 = 关）
		spin.frameIndex += 1u;
		spin.angle = frameDrivenSpinAngle(spin.frameAngle, spin.frameIndex);
		spin.matrix = glm::rotate(glm::mat4(1.0f), spin.angle, spin.axis);
		return;
	}
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
	// 批 14.3：宽高比取**渲染**网格。按窗口比例算投影、按渲染网格铺像素 = 各向异性拉伸。
	auto& extent = rg.ctx().get<RenderExtent>();
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
	// Phase 11 批 11.2：写回实际生效的 near/far（级联切分用；自适应值不等于
	// camera.nearPlane/farPlane，外部若直接拿 camera 的会切错分界）
	viewProj.nearPlane = nearPlane;
	viewProj.farPlane = farPlane;
}

/**
 * TAA 帧间 system（批 12.6；顺序 = projection 之后、renderlist 之前）。
 *
 * 干三件事：① 把亚像素抖动注进 `ViewProjection::proj`（未抖那份存 projUnjittered）；
 * ② 判本帧历史可用性（首帧 / resize / 相机跳切 ⇒ false ⇒ shader 逐位旁路）；
 * ③ 算本帧 scene data 载荷（上一帧**抖动后** viewProj + 全场景刚体运动补偿矩阵）并推进簿记。
 *
 * **门红线**：`env.taaEnabled == false` 时完全不碰 `proj`（不是「加 0」而是不调用）
 * ⇒ 下游 objectMvps / @64 / 剔除 逐位不变。
 *
 * 抖动为什么加在 proj 而不是各消费方：见 ViewProjection::jitterPx 注释（一处抖，
 * 深度缓冲与所有重建该深度的消费者自动同口径）。
 * @param rg registry（读 SceneEnv/SceneSpin/Camera/WindowExtent/SceneBounds/ViewProjection，
 *        读写 TaaState）
 */
void taaSystem(entt::registry& rg) {
	auto& viewProj = rg.ctx().get<ViewProjection>();
	auto& taa = rg.ctx().get<TaaState>();
	const auto& env = rg.ctx().get<SceneEnv>();
	const auto& spin = rg.ctx().get<SceneSpin>();
	const auto& camera = rg.ctx().get<Camera>();
	// 批 14.3：jitter 幅度按像素计、历史可用性的"尺寸变了没"，两者都必须落在**渲染**网格上。
	const auto& extent = rg.ctx().get<RenderExtent>();
	const auto& bounds = rg.ctx().get<SceneBounds>();

	viewProj.projUnjittered = viewProj.proj;  // projectionSystem 刚写的就是未抖那份
	const glm::ivec2 now{static_cast<int>(extent.width), static_cast<int>(extent.height)};
	const auto camPos = camera.position;
	const auto forward = cameraForward(camera);

	// ① 抖动注入（关 = 不调用 applyProjectionJitter ⇒ proj 逐位不变）
	//
	// 批 14.5 第四批：抖动与「谁做时域滤波」**解耦**——此前它和 AO 施加点一样被硬编码成
	// TAA 专属（`env.taaEnabled` 一关投影就不抖），而 FSR3-upscaler 是**抖动相机**升频器，
	// 无抖动的输入不在它的设计点上。
	//
	// **TAA 在 = 一切走原路**（周期 kTaaJitterPeriod=16、幅度 env.taaJitterScale）
	// ⟹ TAA 档逐位不变（本文件头那条门红线的加强版：不只是关的时候不变，开着也不能被我改坏）。
	// 只有「TAA 不在而 FSR 要抖动」才用 vendor 的相位数与**满幅**（±0.5px = 设计点，
	// 不是 taa_jitter_scale——那个是 TAA 的调参旋钮，拿它当 FSR 的实验变量等于两个旋钮互相决定）。
	const bool fsrJitterOnly = !env.taaEnabled && env.fsrJitter;
	viewProj.jitterPx = env.taaEnabled
		? taaJitterPixels(taa.frameIndex, env.taaJitterScale)
		: (fsrJitterOnly
			? taaJitterPixels(taa.frameIndex, 1.0f,
				env.fsrJitterPeriod ? env.fsrJitterPeriod : kTaaJitterPeriod)
			: glm::vec2{0.0f});
	if (env.taaEnabled || fsrJitterOnly)
		viewProj.proj = applyProjectionJitter(viewProj.proj, viewProj.jitterPx,
			{static_cast<float>(extent.width), static_cast<float>(extent.height)});

	// ② 历史可用性（判据本体 = taaHistoryUsable 纯函数，单测覆盖）。
	// **帧序契约**：本段必须在推进簿记之前算（用的还是上一帧的值）。
	taa.historyValid = taaHistoryUsable(env.taaEnabled, taa.frameIndex, taa.prevExtent, now,
		taa.prevCamPos, camPos, taa.prevForward, forward, bounds.radius);

	// ②b 本帧权重（scene data @723584.x = taaParams.x）**不经 TaaState**：批 13.14b 删掉
	// 静态机位权重提升（static boost）后 taaSystem 不再产出 feedback，载荷直接取
	// env.taaFeedback（写载荷处 = `render_list_impl.cppm::fillTaaData`，它拿得到 env）。
	// ⚠ 方向红线：它是**本帧权重**（taa.frag: lerp(clipped, cur, x)）⟹ **调小 = 更信历史**。

	// ③ 本帧载荷。prevViewProj = 上一帧**抖动后** proj·view —— 口径必须跟历史图的
	// **内容**一致：taa_hist 由 taa_out 逐像素 1:1 拷来，而 taa_out 的颜色输入是抖动投影
	// 渲的 ⇒ 历史图按「第 t 帧抖动后」的屏幕系索引。存未抖那份会让采样点系统性偏掉
	// -jitter_t（±0.5px 且每帧随机变），静止机位下也照样搅动历史。批 12.6 T4 实测抓到：
	// S1/S2 = 0.48×（开滤波逐帧差反而**翻倍**，闭式该是 3.95×）。
	// ⚠ 旧注释写的「两侧口径统一 ⇒ 历史查找位不漂 jitter」是把「历史图是像素中心对齐的」
	// 误读成「历史图在未抖网格里」——像素中心对齐 ≠ 未抖网格对齐。
	// 🔵 **批 13.5 补充（读改动前必读）**：本裁决**仍然成立、未被反转**——本字段至今产出
	//   **抖动网格**上的坐标。变的是**下游**：历史图自 13.5 起住在**基空间**（输入侧已
	//   un-jitter），故 shader 拿到抖动网格坐标后要**减一帧内容位移 c_{t−1}** 才落到基网格
	//   （`taa.frag.slang` 的 prevUv 修正）。⟹ **两处必须成对**：把本字段改成未抖 = 采样点
	//   少减一次 c_{t−1} = 系统性偏 c_{t−1}（静止机位 0.667px 游动原样回来）。
	//   若将来改存未抖那份，**必须同时删掉 shader 侧的减法**，二选一，不可都做也不可都不做。
	// motion = prevSpin·inverse(curSpin)：把「本帧世界坐标」刚体搬回「上一帧世界坐标」
	// ——本项目全场景共用一个旋转矩阵，所以一条 mat4 就是**精确**补偿（不是近似）。
	taa.payloadPrevViewProj = taa.prevViewProj;
	taa.payloadPrevContentShift = taa.prevContentShift;
	taa.payloadMotion =
		(env.taaMotionCompensation && taa.historyValid)
			? taa.prevSpin * glm::inverse(spin.matrix)
			: glm::mat4{1.0f};
	// 重投影消融门（`taa.reprojection`，默认 true = 本分支不进 = 逐位今天）。
	// 关 = 三份载荷联合改成本帧恒等形态：prevViewProj := **本帧抖动后** viewProj（此刻
	// viewProj.proj 已含本帧 jitter，与 @64 的 invViewProj 配套）+ 位移清零 + motion 单位阵
	// ⟹ shader 端 prevUvBase ≡ baseUv（world 由 (baseUv, depth) 经 invViewProj 重建、再被
	// 同一矩阵投影回去，浮点上也是同一对矩阵，采样点落回**同基位置**）。
	// 相机运动激励下历史错位 = 每帧像素位移 ⟹ 读数必须显著变差（temporal-motion 的 M3
	// 阳性锚点）；静止机位下 prevViewProj ≡ 本帧矩阵 ⟹ 关与不关逐位相同。
	// ⚠ 勿与 `motion_compensation` 混淆：那条只关 prevSpin 场景旋转补偿，相机动、spin 关
	//   时它是恒等阵 = 消融无效（这正是 temporal-motion 从 spin 换相机激励后必须新增本门
	//   的原因，见 tools/imgqa/temporal_motion.cpp 头注释）。
	if (!env.taaReprojection) {
		taa.payloadPrevViewProj = viewProj.proj * viewProj.view;
		taa.payloadPrevContentShift = glm::vec2{0.0f};
		taa.payloadMotion = glm::mat4{1.0f};
	}

	// ④ 簿记推进（必须在 ②③ 之后；下游 renderListSystem 读的是 payload*）
	// 存**抖动后**那份（此刻 viewProj.proj 已含本帧 jitter）⇒ 下一帧拿到的正是
	// 「渲染出这张历史图的那个矩阵」。门关时 proj 未被碰过 ⇒ 与未抖那份逐位相同 ⇒
	// 门红线（关 = 基线逐位不变）自动成立，jitter_scale=0 时同理。
	taa.prevViewProj = viewProj.proj * viewProj.view;
	// 内容位移与 prevViewProj **同生同灭**：它俩是同一件事（「渲染出上一帧那张图的变换」）
	// 的矩阵表达与平移表达。TAA 关 / jitter_scale=0 ⇒ jitterPx = 0 ⇒ c = 0 ⇒ 减法逐位恒等。
	taa.prevContentShift = contentShiftPx(viewProj.jitterPx);
	taa.prevSpin = spin.matrix;
	taa.prevCamPos = camPos;
	taa.prevForward = forward;
	taa.prevExtent = now;
	++taa.frameIndex;
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

// LlmCaptureFlow / FrameStatsTracker（帧循环诊断旁路）已拆至 ecs_frame_diag.cppm（批 14.3 文件长度红线；职责边界 = system 语义 vs 观测旁路）

}  // namespace

/** applyPickedSlot 前置声明（定义在 pickingTick 之后——pickingTick 只做「探针 → 取回读 →
 * 落 ctx」三步编排，落 ctx 的实体反查 + 跳页发信拆出以压低 pickingTick 认知复杂度）。 */
void applyPickedSlot(entt::registry& rg, uint32_t slot);

/** 配置热重载帧边界（Phase 10 批 10.3；renderSystem 帧首调用）：eventSystem 之后、
 * 本帧任何 system/draw 之前 = 上一帧已 submit、本帧未录制的换入点。钩子未挂
 * （config_reload 门默认关，main 不 emplace）= 空指针早退零开销。 */
void configReloadTick(entt::registry& rg) {
	auto* hook = rg.ctx().find<ConfigReloadHook>();
	if (!hook || !hook->poll)
		return;
	hook->poll();
}

/** 脚本化点选探针（diagnostics.picking_probe = {frame,x,y}）：第 frame 帧发一次请求
 *  ——headless 无鼠标验证全链（本帧 request → 帧末 record → 下帧 applyPickedSlot 处置集）。
 * 门关（frame=0）= 零行为。 */
void pickingProbeTick(PickingHook& hook) {
	static uint32_t frameCount = 0;
	const auto& probe = diagnostics().pickingProbeFrame;
	if (probe == 0 || !hook.requestPick || frameCount++ != probe)
		return;
	if (auto* logger = RLLogger::getInstance())
		logger->info("picking: probe 第 {} 帧请求 ({}, {})", probe, diagnostics().pickingProbeX,
			diagnostics().pickingProbeY);
	hook.requestPick(static_cast<float>(diagnostics().pickingProbeX),
		static_cast<float>(diagnostics().pickingProbeY));
}

/** picking 结果 tick（Phase 10 批 10.5；renderSystem 帧首调用，同 configReloadTick
 *  帧边界）：消费 PickingService 回读 → objectSlot 反查 MeshRef entity（点击触发，
 *  O(n) 扫一次可接受，不平行维护 slot→entity 映射表）→ 写 SelectedObject ctx。
 *  钩子未挂 = 空指针早退零开销；无新结果/空像素 = 保持上次选中（v1 不做清空）。 */
void pickingTick(entt::registry& rg) {
	auto* hook = rg.ctx().find<PickingHook>();
	if (!hook || !hook->takeResult)
		return;
	pickingProbeTick(*hook);
	const auto slot = hook->takeResult();
	if (!slot)
		return;
	applyPickedSlot(rg, *slot);
}

/** 回读槽位落 SelectedObject ctx：反查 MeshRef.entity（同 objectSlot 唯一），顺带取
 * MaterialRef 供高亮按材质过滤（批 10.7）。命中实体才发面板跳页请求（一次性）。 */
void applyPickedSlot(entt::registry& rg, uint32_t slot) {
	if (!rg.ctx().contains<SelectedObject>())
		rg.ctx().insert_or_assign<SelectedObject>(SelectedObject{});
	auto& sel = rg.ctx().get<SelectedObject>();
	sel.objectSlot = slot;
	sel.entity = static_cast<entt::entity>(entt::null);
	sel.materialId = 0;
	for (const auto entity : rg.view<MeshRef>())
		if (rg.get<MeshRef>(entity).objectSlot == slot) {
			sel.entity = entity;
			// 高亮按材质过滤（选中材质全部实体一起亮）——同循环取 MaterialRef，不二次遍历
			if (rg.all_of<MaterialRef>(entity))
				sel.materialId = rg.get<MaterialRef>(entity).material;
			break;
		}
	// 批 10.7：跳页请求（构建器消费即清）。**只在真的命中实体时**置位——miss（点空处）
	// 不发跳转，否则会因「点空」把用户从灯光页弹到空的"选中"页。
	const bool hit = sel.entity != static_cast<entt::entity>(entt::null);
	if (hit)
		if (auto* panel = rg.ctx().find<DebugPanelState>())
			panel->pendingSelectJump = true;
	if (auto* logger = RLLogger::getInstance())
		logger->info("picking: objectSlot {} → entity {}{}", slot,
			static_cast<std::uint32_t>(sel.entity),
			hit ? "" : "（无匹配 MeshRef，选中保持空）");
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
	//   → projection（Camera+SceneBounds+WindowExtent→ViewProjection）→ taa（抖动注入 +
	//   帧间簿记，批 12.6）→ renderlist（→FrameDrawList）
	std::vector<FrameSystem> systems{
		{"camera",     [](entt::registry& rg, float dt) { cameraSystem(rg, dt); }},
		{"animation",  [](entt::registry& rg, float dt) { animationSystem(rg, dt); }},
		{"bounds",     [](entt::registry& rg, float) { boundsSystem(rg); }},
		{"projection", [](entt::registry& rg, float) { projectionSystem(rg); }},
		{"taa",        [](entt::registry& rg, float) { taaSystem(rg); }},
		{"renderlist", [&](entt::registry& rg, float) { renderListSystem(rg, resources, renderer.getEnvironmentRef(), assets, renderListState,
			// ★M-1b：本帧录制槽（drawFrame **之前**取；slot 只在 drawFrame 末尾 advance ⟹
			// 采集期与录制期同值，执行器 prepareMdiShadowRows 有机器核对，失配 = 抛）
			renderer.currentFrameSlot()); }},
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
		// 债-27：input_probe 住 rlbase 不认识 SDL，合成事件在此适配成 SDL_Event 入队
		// （SDL3 include 面归属 = 拥有该 TU 的库层，见 rgl/CMakeLists.txt 头注释）
		auto pushProbeEvent = [](const ProbeEvent& e) {
			SDL_Event ev{};
			switch (e.kind) {
				case ProbeEventKind::KeyDown:
					ev.type = SDL_EVENT_KEY_DOWN;
					ev.key.key = e.key;
					break;
				case ProbeEventKind::KeyUp:
					ev.type = SDL_EVENT_KEY_UP;
					ev.key.key = e.key;
					break;
				case ProbeEventKind::MouseLeftDown:
				case ProbeEventKind::MouseLeftUp:
					ev.type = e.kind == ProbeEventKind::MouseLeftDown
						? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
					ev.button.button = SDL_BUTTON_LEFT;
					ev.button.x = static_cast<Sint32>(e.x);
					ev.button.y = static_cast<Sint32>(e.y);
					break;
			}
			SDL_PushEvent(&ev);
		};
		uiClickProbeTick(pushProbeEvent);
		cameraProbeTick(rg, pushProbeEvent);

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
