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
// Lua 形如（全字段可选；缺文件 = 回落 defaultRenderGraphDesc 三段图；文件在但
// 缺段 = 空表透传，结构错误由 compileRenderGraph 报，不静默回落）：
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

/**
 * 渲染路线变体（render_graph.lua 顶层 variant 字段；2026-09-08 替代 RGL_DEFERRED/
 * RGL_OFFSCREEN 环境变量）。deferred/offscreen = 内建图（exclusive，忽略文件 passes）；
 * forward（缺省）= 文件 passes/attachments（文件在但缺段 = 空表透传，由
 * compileRenderGraph 报结构错误，不静默回落）。
 */
export enum class RgGraphVariant : uint8_t { forward, deferred, offscreen };

/**
 * loadRenderGraphConfig 的输出：variant（路线选择，main 按此决定装配路径）+
 * desc（渲染意图声明；deferred/offscreen = 内建图，forward = 文件解析结果）。
 */
export struct RenderGraphConfig {
	RgGraphVariant variant = RgGraphVariant::forward;
	RgGraphDesc desc;
};

/**
 * 从 Lua 配置初始化渲染图声明（configs/render_graph.lua → RenderGraphConfig）。
 * 文件缺失 → warn + 默认三段图 forward；文件在但缺段 → 空表透传（结构错误留给
 * compileRenderGraph 报，不重复校验）；脚本/类型/枚举字符串错误 →
 * std::unexpected(ConfigLoadFailed)。
 * @param path 配置文件路径（相对 CWD，须为 build/rgl）
 * @return variant + RgGraphDesc；解析失败返回 Error
 */
export [[nodiscard]] std::expected<RenderGraphConfig, Error> loadRenderGraphConfig(std::string_view path);

}  // namespace RGL
