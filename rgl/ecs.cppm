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
 * **口径 = 窗口物理像素**（与 swapchain extent 同口径——resizeSystem 拿两者比较，口径不同即误判）；
 * Wayland 分数缩放下与 SDL「逻辑点」尺寸不同，见 ecs_impl eventSystem。
 * 无 needsResize 标志：resizeSystem 比较本值与 swapchain 实际尺寸（派生状态不存标志位）。
 */
export struct WindowExtent {
	uint32_t width = 800;
	uint32_t height = 600;
};

/**
 * 渲染尺寸（registry context，Phase 14 批 14.3）：内部渲染网格的像素尺寸。
 *
 * **口径 = 渲染像素**（= swapchain extent × `runtime.lua render_scale`）。它与
 * `WindowExtent`（窗口物理像素）是两个坐标空间，此前本项目从未区分——renderScale 恒 1
 * 时两者逐像素相等，所以混用没暴露过问题。
 *
 * 谁读：
 * - `projectionSystem` —— 宽高比必须与**渲染网格**同源。按窗口比例算投影、却按渲染网格
 *   铺像素 = 各向异性拉伸（renderScale≠1 时整幅画面被横向/纵向压扁）。
 * - `taaSystem` —— 抖动幅度按像素计、历史可用性判定的"尺寸变了没"，两者都必须落在渲染网格上。
 *   按窗口像素算 jitter 再落到渲染网格 = 抖动幅度按比例错（0.75 scale 下错 33%）。
 *
 * 谁**不**读：点选 / 鼠标 / imgui —— 那些是窗口像素（见 `WindowExtent` / `WindowScale`）。
 *
 * 谁写：main 在 Swapchain 构造后写初值；`resizeSystem` 每帧帧首从 swapchain 同步一次
 * （含 OUT_OF_DATE 路径重建的情况——那条不改 WindowExtent，但会改渲染 extent）。
 */
export struct RenderExtent {
	uint32_t width = 800;
	uint32_t height = 600;
};

/**
 * 窗口像素尺寸查询钩子（registry context，main 建窗时装配；headless 未装配 = 窗口不存在，
 * 也恒无 SDL 窗口事件）：eventSystem 收到 WINDOW_RESIZED / WINDOW_PIXEL_SIZE_CHANGED 后经
 * 本钩子**重新查询**像素尺寸写 WindowExtent——不信任事件携带的 data1/2（RESIZED 带逻辑点、
 * PIXEL_SIZE_CHANGED 带像素，分数缩放下两者不同，而 swapchain/projection 口径是像素）。
 * std::function 注入 = ecs 不依赖窗口模块（与 PickingHook/InputFilter 同款类型擦除）。
 */
export struct WindowPixelQuery {
	std::function<std::optional<std::array<int, 2>>()> pixels = nullptr;
};

/**
 * 窗口内容缩放倍数（registry context，main 建窗时装配 = SDLWindow::displayScale）。
 * 为什么需要它：SDL 鼠标事件坐标恒为**逻辑点**（实测：同一物理指针位置下，开/关
 * SDL_WINDOW_HIGH_PIXEL_DENSITY 回读值逐位相同，不随 pixels 变），而点选回读的是
 * **帧缓冲像素**上的 objectID 附件 ⟹ 两个坐标空间必须换算，否则分数缩放桌面
 *（本机 KDE Wayland 1.75）下点选整体偏 scale 倍——点窗口右下角会采样到窗外。
 * imgui 侧的同类换算在 ImGuiLayer::handleEvent（那边 UI 坐标系被钉成像素）。
 */
export struct WindowScale {
	float value = 1.0f;
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
 * 投影 system（Camera + SceneBounds + RenderExtent → ViewProjection context）：
 * 自适应 near/far + 投影矩阵 + 实际生效深度界写回 + **本帧渲染原点**（相对坐标批 5 第 1 刀起，
 * 原 `originSystem` 搬进本 system）+ view 矩阵（吃刚写好的原点）。
 *
 * ## 🔴 为什么原点的生产者是本 system，而不是一个排在最前面的 originSystem（本批判决）
 * 吸附粒度 = `rebaseSnapGranularity(texels)`，而 texels ← 级联球半径 ← **near/far**；near/far
 * 只在本 system 里按 SceneBounds 现算，同时 view 又要吃原点。留在独立的 originSystem 里只有
 * 两条路：复制一份 near/far 算式（造出第二份会漂的来源），或吃上一帧的值（悄悄错一帧）。
 * 搬进来之后「先有粒度、再有原点、再有 view」变成**词法顺序**，写不错。
 * 顺序（承重，别调换）：near/far → proj → 级联球半径 → texels → 粒度 → 原点 → view。
 *
 * ## 门关（`RenderOrigin::enabled == false`；**出厂默认已是门开**，门关要显式
 * `--set coordinate.rebase=false` 才到达）
 * position 钉回 dvec3(0)（幂等：main emplace 的初值也是它）后**不进粒度那条路**，
 * 与接原点之前逐位相同（toRelative 在零原点是恒等）。⚠ 这条早退分支**不再是出厂路径**
 * （2026-10-26 翻默认），它是阴影/时域归因的对照态，别当死码删。
 * ## 门开：粒度 NaN 必须响亮 throw
 * `rebaseSnapGranularity` 对坏输入返回 NaN，而 **NaN 原点会让相机凭空消失**（view 的平移列
 * 全 NaN ⟹ 整个场景被裁掉，且不报任何错）——那是本项目最难查的形态。取代批 3 留的
 * 「门开未实现」throw（那条的判据现在由「粒度 NaN 才 throw」+「门开原点闭式」两条接住）。
 * 🔴 **不许**把 NaN 静默按门关跑（配置开了却没生效 = 本仓反复登记的假绿形态）。
 *
 * 🔴 为什么导出（与 taaSystem 同一条理由）：**接线**问题纯函数测不到——
 * `cameraView(camera, origin)` 只保证「给了原点就减对」，不保证调用点真把刚算出的那份传进去
 * （调用点改回传 `dvec3{0}` 编译照过、门关照绿、门开静默错位）。栈上 registry 驱动一次就验得到
 * （tests/test_view_light_rebase.cppm / tests/test_render_origin.cppm）。
 * @param rg registry（读 Camera/SceneBounds/RenderExtent，**写 RenderOrigin**，写 ViewProjection；
 *   缺任一 context 时 entt 直接抛）
 */
export void projectionSystem(entt::registry& rg);

/**
 * TAA 帧间 system（批 12.6；顺序 = projection 之后、renderlist 之前）：抖动注入 +
 * 历史可用性判定 + 本帧 scene data 载荷（prevViewProj / motion / 内容位移）+ 跨帧簿记。
 *
 * 相对坐标批 4 第一刀起，载荷 `motion` 还负责**把两帧渲染原点之差复合进去**
 * （`RenderOrigin::position` 与它自己记的 `TaaState::prevOrigin`）。
 *
 * 🔴 为什么导出（与 taaSystem 同一条理由）：帧序契约（`projection` 必须排在 `taa` 之前）
 * 与「motion 吃的到底是哪两个原点、推进的是哪一个」这类**接线**问题，纯函数
 * `motionWithOriginShift` 自己测不到——它只保证「给定两个原点算得对」，不保证调用点把
 * 参数传对、不保证 `prevOrigin` 真的被推进。驱动两帧才验得到
 * （tests/test_taa_cross_frame.cppm；不这么做就会重演批 3 那条「生产点变异单测不红」）。
 * @param rg registry（读 SceneEnv/SceneSpin/Camera/RenderExtent/SceneBounds/ViewProjection/
 *   RenderOrigin，读写 TaaState；缺任一 context 时 entt 直接抛）
 */
export void taaSystem(entt::registry& rg);

/**
 * 帧循环（main 启动流末站，运行到 AppExit 为止）：每帧 = eventSystem（事件/窗口状态）
 * → resizeSystem（尺寸变化重建）→ 显式 system 链（camera→origin→animation→bounds→
 * projection→taa→renderlist，顺序即数据流契约）→ drawFrame 盲执行 FrameDrawList。
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
