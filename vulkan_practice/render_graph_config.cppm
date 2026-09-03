export module render_graph_config;

import rlstd;
import vkerror;
import render_graph;

namespace RGL {

// Phase 6 批次 3：渲染意图 Lua 配置（configs/render_graph.lua → RgGraphDesc）。
// 沿用 Phase 1 确立的 configs/*.lua 模式（sol2 沙箱 + 字段可选回落默认值）——
// 与 camera_config/scene_config 同构：本模块只做"文本 → 声明数据"的解析，
// 校验与编译归 render_graph::compileRenderGraph（main 启动期串接）。
//
// Lua 形如（全字段可选，缺文件/缺段 = 回落 defaultRenderGraphDesc 三段图）：
//   return {
//     attachments = { { name="swapchain", kind="swapchain" },
//                     { name="depth", kind="depth" },
//                     { name="scene_rt", kind="offscreen" } },
//     passes = {
//       { name="scene", kind="draw", state="opaque", draws="opaque",
//         outputs = { { attachment="scene_rt", usage="colorWrite",
//                       load="clear", store="store", clear={0.1,0.1,0.1,1.0} },
//                     { attachment="depth", usage="depthWrite", clearDepth=1.0 } } },
//       { name="composite", kind="blit",
//         outputs = { { attachment="swapchain", usage="transferWrite" } },
//         inputs  = { { attachment="scene_rt", usage="transferRead" } } },
//     },
//   }
// 名字字符串引用（attachment 名 / state 名 / draws 段名），零 handle——执行侧
// （Renderer 构造校验 + compileRenderGraph）负责全部结构检查。

// 从 Lua 配置初始化渲染图声明。文件缺失 → warn + 默认三段图；脚本/类型/枚举
// 字符串错误 → std::unexpected（结构错误留给 compileRenderGraph 报，不重复校验）。
export [[nodiscard]] std::expected<RgGraphDesc, Error> loadRenderGraphConfig(std::string_view path);

}  // namespace RGL
