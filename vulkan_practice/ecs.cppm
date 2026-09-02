module;
#include <entt/entt.hpp>

export module ecs;

import rlstd;
import vkerror;
import vk_renderer;
import frame_draw_list;
import camera_config;

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
	std::function<void(uint32_t)> onFrame = {});

}  // namespace RGL
