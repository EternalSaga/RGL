export module llm_debug;

import rlstd;
import vkerror;
import vk_renderer;
import frame_draw_list;
import runtime_config;  // LlmDebugConfig（2026-09-08 自 camera_config 迁入——截图属运行模式）

namespace RGL {

// 离屏一次性渲染 + 截图（llm_debug 模式，每次运行至多调用一次）：
// 把给定帧的 draw list 重渲到临时离屏目标（8-bit UNORM 颜色 + D32Sfloat 深度，
// 与上屏 pass 同状态同管线表），回读 CPU 写 PNG。
//
// 为什么离屏重渲而不读回 swapchain 后缓冲：后缓冲的 present 生命周期由
// compositor 掌控（Wayland 息屏后 present 可能永久停滞，后缓冲状态不可靠），
// 而离屏目标是自持资源 + 独立提交/独立 fence——对 present 循环状态免疫。
// 8-bit UNORM 直接映射 PNG，免 HDR 格式转换。
//
// 返回 PNG 绝对路径（agent 据此读取截图）。
//
// 声明在接口、定义在实现单元 llm_debug_impl.cppm（2026-09-02 编译提速 A 方案）：
// 250 行函数体 + 其 vulkan_raii/vma/stb/fmt GMF 不进接口 TU——WriteAST（BMI
// 序列化）大幅缩水，importer（main）load 的是瘦身 BMI；实现单元是叶子边
// （无人 import），与下游完全并行、不上关键路径。
export [[nodiscard]] std::expected<std::string, Error>
captureFrame(RGL::Renderer& renderer, const RGL::FrameDrawList& list, const LlmDebugConfig& cfg);

}  // namespace RGL
