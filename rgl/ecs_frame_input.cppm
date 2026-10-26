module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <entt/entt.hpp>
export module ecs:frame_input;

/**
 * ecs:frame_input —— 帧首的「外部世界 → registry」边界翻译（本刀自 :impl 拆出）
 *
 * ## 为什么这是一条职责边界（三条判据同时成立，不是"哪个文件有空位"）
 *
 * 本分区的九个函数共享同一件事：**每帧开头、system 链之前，把一个住在 registry 之外的
 * 源搬进 registry context**。三条独立判据都落在同一条缝上：
 *
 * 1. **生命周期阶段**：全部是"帧首"这一拍（renderSystem 里的调用序 = eventSystem →
 *    configReloadTick → pickingTick → resizeChecked），没有一个是 system 链成员。
 * 2. **数据流方向 + 输入输出类型**：输入是**外部源**（SDL 事件队列 / swapchain 实际
 *    尺寸 / 配置文件监视钩子 / GPU 异步回读槽），输出是 registry context；而留在
 *    `:impl` 的 animation/bounds/projection/taa 是 **registry → registry** 的纯几何变换
 *    （输入只有 registry 状态与 dt，没有任何外部源）。
 * 3. **可选性同构**：四个源全部"未装配/门未开 = 零调用早退"（headless 无窗口事件、
 *    InputFilter/ConfigReloadHook/PickingHook 钩子为空、探针帧号 0）。
 *
 * 可机械复核的推论（两条都验过）：本分区**不出现** ViewProjection / RenderOrigin /
 * FrameDrawList / SceneBounds（几何侧的输入输出全在 `:impl`）；SDL 事件队列的**读取点**
 * `SDL_PollEvent` 只在本分区——`:impl` 里只剩 `SDL_PushEvent`（脚本化探针往队列里
 * **注入**事件，是同一根管子的另一头，跟着探针 tick 留在调用点）。
 *
 * ## 为什么不再往下切一刀
 *
 * 事件翻译 + resize 重建（147 代码行）与帧首可选钩子（51 代码行）各自都太小，且共享第 2
 * 条判据（外部源 → context）与同一个调用点；切开只会得到两个"半个边界"。
 *
 * ## 可见性
 *
 * 本分区是 `ecs` 的**接口分区**，四个函数用 `export` 只为让 `:impl`（renderSystem 与
 * resizeChecked）看见；主接口 `ecs` 不 `export import` 本分区 ⟹ 模块外不可见（等价于
 * 拆行之前的文件内可见性）。其余五个只被本分区内部调用，保持不导出。
 * 先例 = `ecs_frame_diag.cppm`（批 14.3 自 :impl 拆出，同款接口分区）。
 *
 * ## 搬移性质
 *
 * **纯搬移，非重写**：函数体逐行原样（脚本按行区间切片，非重打），只有四处函数签名
 * 加了 `export` 关键字、以及本分区的 import 面。
 */

import ecs;   // clang 不给 partition 隐式 import 主接口——必须显式
import rlstd;
import vkerror;
import vk_swapchain;
import vk_resources;
import diagnostics;     // pickingProbeTick 的脚本化点选探针门（configs/diagnostics.lua）
import param_registry;   // DebugPanelState（批 10.7 点选跳页请求位）
import scene_context;   // SelectedObject / MeshRef / MaterialRef（applyPickedSlot）
import camera;          // InputEvent / InputEvents（eventSystem 产出的事件流类型）
import rllogger;

namespace RGL {

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
export void eventSystem(entt::registry& rg) {
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
export std::expected<void, Error> resizeSystem(entt::registry& rg, RGL::Swapchain& swapchain,
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

/** applyPickedSlot 前置声明（定义在 pickingTick 之后——pickingTick 只做「探针 → 取回读 →
 * 落 ctx」三步编排，落 ctx 的实体反查 + 跳页发信拆出以压低 pickingTick 认知复杂度）。 */
void applyPickedSlot(entt::registry& rg, uint32_t slot);

/** 配置热重载帧边界（Phase 10 批 10.3；renderSystem 帧首调用）：eventSystem 之后、
 * 本帧任何 system/draw 之前 = 上一帧已 submit、本帧未录制的换入点。钩子未挂
 * （config_reload 门默认关，main 不 emplace）= 空指针早退零开销。 */
export void configReloadTick(entt::registry& rg) {
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
export void pickingTick(entt::registry& rg) {
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

}  // namespace RGL
