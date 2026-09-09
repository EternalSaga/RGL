export module llm_debug;

import rlstd;
import vkerror;
import vk_renderer;
import runtime_config;  // LlmDebugConfig（2026-09-08 自 camera_config 迁入——截图属运行模式）

namespace RGL {

/**
 * llm_debug 截图消费端（2026-09-08：离屏重渲器退役——截图 = drawFrame 本帧真实输出的
 * 帧内回读，见 vkrenderer.cppm FrameCapture）。渲染侧已在 drawFrame 内完成（present 前
 * 录回读拷贝 + 同步等本帧 fence），本函数 = takeFrameCapture → 按格式转 RGBA8 →
 * stb PNG。
 *
 * 旧 rationale（"后缓冲 present 生命周期由 compositor 掌控、Wayland 息屏 present
 * 停滞、隐藏窗 extent 协商抖动，故离屏重渲"）被新设计消解：拷贝录在 present 之前
 *（buffer 尚归我们）、headless 主路径无 compositor；截图尺寸 = swapchain extent
 *（headless 下 main 契约对齐 llm_shot.size）。收益：截图与真实上屏内容零分歧、
 * capture 路径不再平行维护 pass 逻辑（图演进零跟随成本）。
 *
 * 文件名：dump_gbuf 直读 = screenshots/gbuffer_N.png；否则 cfg.file（空 = 时间戳名）。
 * 返回 PNG 绝对路径（agent 据此读取截图）。
 *
 * 声明在接口、定义在实现单元 llm_debug_impl.cppm（接口=声明+文档，实现=叶子单元）。
 * @param renderer 执行器（takeFrameCapture 取本帧捕获产物）
 * @param cfg 截图配置（file = PNG 相对路径，空 = 时间戳名）
 * @return PNG 绝对路径；无捕获产物 / 回读格式无转换规则 / 写盘失败 → Error
 */
export [[nodiscard]] std::expected<std::string, Error>
writeCapturePng(RGL::Renderer& renderer, const LlmDebugConfig& cfg);

}  // namespace RGL
