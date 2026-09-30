module;
#include <entt/entt.hpp>

export module ecs;

import rlstd;
import vkerror;
import vk_renderer;
import asset_table;  // AssetTable（renderSystem 参数；F 拆出）
import frame_draw_list;
import runtime_config;  // LlmDebugConfig（2026-09-08 自 camera_config 迁入 runtime_config）

namespace RGL {

/**
 * 窗口尺寸（registry context，eventSystem 在 resize 事件写入；projectionSystem/resizeSystem 读）。
 * 无 needsResize 标志：resizeSystem 比较本值与 swapchain 实际尺寸（派生状态不存标志位）。
 */
export struct WindowExtent {
	uint32_t width = 800;
	uint32_t height = 600;
};

/**
 * 退出请求（registry context，eventSystem 在 QUIT/窗口关闭请求（CLOSE_REQUESTED）事件写入；
 * 帧循环判定。ESC 不处理——退出只走窗口级事件）。
 *
 * **写者不止 eventSystem**：RPC `POST /quit` 也写本字段（自动化测试收尾——与用户
 * 关窗走同一条干净退出路径，能拿到帧循环之后的 profiler 收口输出；SIGTERM 杀进程
 * 打断 present/in-flight 且拿不到）。
 */
export struct AppExit {
	bool requested = false;
};

/**
 * 输入过滤器钩子（registry context，Phase 10 调试层可选挂接；main 装配，缺省空）：
 * eventSystem 在翻译每条 SDL 事件**之前**过一遍过滤器——返回 true = 事件被消费
 * （imgui 控件接管），不再产 InputEvents（相机等下游不响应）。空 = 零行为变化。
 * **参数为 `const SDL_Event*` 的 void\* 类型擦除**：ecs 接口刻意不 GMF include SDL
 * （GMF 收敛）且不在 purview 前置声明 SDL_Event（GMF 全局模块实体 vs purview 模块
 * 实体同名双声明地雷）；唯一两个调用侧（ecs_impl / imgui_layer）各自 GMF 有完整
 * SDL_Event 定义，static_cast 即还原类型安全。
 */
export struct InputFilter {
	std::function<bool(const void* sdlEvent)> consume = nullptr;
};

/**
 * 配置热重载轮询钩子（Phase 10 批 10.3；registry context，main 在
 * diagnostics.config_reload=true 时经 wireHotReload 装配，缺省未 emplace/空 = no-op）：
 * renderSystem 帧首调用（eventSystem 之后、本帧任何 system/draw 之前 = 帧边界——
 * 上一帧 submit 已完成、本帧录制未开始，套用配置值无撕裂）。钩子本体 =
 * HotReloader 轮询源 configs/ mtime + 变更回调套用（main.cppm wireHotReload）。
 */
export struct ConfigReloadHook {
	std::function<void()> poll;
};

/**
 * GPU id 点选双向桥（Phase 10 批 10.5；registry context，main 在
 * diagnostics.picking=true 时经 wirePicking 装配，缺省未 emplace = 全链 no-op）：
 * requestPick = eventSystem 鼠标左键按下（已过 InputFilter——imgui 控件区域内的
 * 点击不产请求）→ PickingService::request；takeResult = 帧边界 pickingTick 消费
 * 服务回读结果（objectSlot）。两向都是 std::function 注入——**ecs 不依赖 picking
 * 模块**（依赖方向：picking = 渲染下层，ecs 经函数桥零类型接触）。
 */
export struct PickingHook {
	std::function<void(float x, float y)> requestPick;
	std::function<std::optional<uint32_t>()> takeResult;
};

// 中央事件轮询（唯一 SDL_PollEvent 调用点）：窗口状态（WindowExtent/AppExit）+
// 输入事件流（InputEvents：本 system 只产事件，不做键位映射——键绑定在 Camera.keys，
// cameraSystem 消费事件时按绑定解释，E5.4；退出/窗口状态不走事件流）

// 窗口尺寸变化（WindowExtent 与 swapchain 实际尺寸不一致）时重建 swapchain + 深度图。
// 深度图必须与 swapchain extent 同步重建，否则 renderArea 会超出图像范围。

// 场景整体时间旋转：angle += dt * speed；matrix = rotate(axis, angle)。
// speed=0 = 静止（configs/scene.lua 的 spin 开关，main 启动期转成 speed）。
// 现状承载"全场景同一旋转"（renderListSystem 乘进各 MVP）；per-entity 旋转需求出现时
// 改写 Transform 组件，结构同构（ecs_compliance_review E4.1）。

// union 包围球（标准公式；radius=0 视为空球）：a 包含 b → a；b 包含 a → b；
// 否则 r = (ra+rb+d)/2，中心从 a 向 b 滑动 t = ra/(d+r)。

// 聚合全部 Bounds 组件 → SceneBounds context（空场景 = radius 0）。
// 现状场景静态（Bounds 仅导入期写入），每帧重聚合为未来动态场景变更留接缝。

// Camera + 场景包围球 + 窗口尺寸 → ViewProjection context。
// 自适应 near/far：SceneBounds 非空时按相机到球心实时距离 ± 球半径推导，保证任意相机
// 位置/场景尺度下整个场景在 frustum 内；无场景内容时回落 configs/camera.lua 固定值。


/**
 * 帧循环（main 启动流末站，运行到 AppExit 为止）：每帧 = eventSystem（事件/窗口状态）
 * → resizeSystem（尺寸变化重建）→ 显式 system 链（camera→animation→bounds→projection→
 * renderlist，顺序即数据流契约）→ drawFrame 盲执行 FrameDrawList。
 *
 * llm 批处理语义：第 llm->delayFrames 帧渲染完成后回读本帧真实输出 + 存 PNG，然后置
 * AppExit 退出——无常驻状态、无 mailbox（config 即控制通道：agent 改
 * configs/runtime.lua 后重跑）。
 * @param rg registry（读写全部每帧 context：WindowExtent/AppExit/InputEvents/Camera/
 *   SceneSpin/SceneBounds/ViewProjection/FrameDrawList）
 * @param renderer 执行器（resize/drawFrame/帧内捕获；swapchain/resources 引用经它取）
 * @param assets 资产表（renderListSystem/texelCanaryCheck 消费；F 拆出不再经内核取）
 * @param llm llm_debug 一次性截图配置（可空，非空 = 启用）
 * @param llmCapture 截图消费回调（可空，llm 非空时必传；main 传 llm_debug::writeCapturePng
 *   包装）——ecs 只数帧 + 触发 renderer.requestFrameCapture（帧内回读钩子，见
 *   vkrenderer.cppm），drawFrame 后调本回调取结果，不 import llm_debug（依赖图解耦：
 *   llm_debug 拖 vk_renderer 全链，本是叶子模块，不该在 ecs 的编译前置里；2026-09-02）
 * @param onFrame （DIAG，可空）每帧 drawFrame 完成后回调（帧号从 0 起）——可见窗
 *   GPUVM fault 排查用（runtime.lua show_after：隐藏窗启动、第 N 帧后显示，隔离"分配期
 *   状态"与"present 期状态"）；默认空 = 无回调，正常路径零变化
 * @return void；resize/drawFrame 失败或 llm 截图失败 → Error（正常 AppExit 退出 = 成功）
 */
export std::expected<void, Error> renderSystem(entt::registry& rg, RGL::Renderer& renderer,
	const RGL::AssetTable& assets,
	const RGL::LlmDebugConfig* llm = nullptr,
	std::function<std::expected<std::string, Error>(RGL::Renderer&)>
		llmCapture = {},
	std::function<void(uint32_t)> onFrame = {});

}  // namespace RGL
