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
// **选路语义（2026-10-18 刀②，`docs/archive/图变体收敛_plan.md` §6.1-2）**：
// `variant` 键与 `RgGraphVariant` 枚举**已删除**——它是"没有消费者的真相源"
// （消费方要的是图事实 `graphHasDeferredLight`，不是配置里写了什么；两个真相源
// 的对账 lint 只是维护税）。现在只有一条规则：
//   文件里有 `attachments` / `passes` 任一键 → 解析为**自定义图**（"图是数据"路）；
//   否则（缺文件 / 空表 / 只有注释）→ **内建 deferred 图**。
// 缺文件不再静默换路线（旧形态：缺文件 = forward 而出厂默认 = deferred，路径写错
// 就换一个渲染器——本仓实测活雷，随本刀拆除）。
//
// Lua 形如（两键都缺 = 内建 deferred；全字段速查见 configs/render_graph.lua 头注）：
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
 * 从 Lua 配置解析渲染图声明（configs/render_graph.lua → RgGraphDesc）。
 * 选路 = 上方模块头的唯一规则（有 attachments/passes → 自定义图；否则内建 deferred）。
 * 文件缺失 → warn + 内建 deferred；自定义图文件在但缺段 → 空表透传（结构错误留给
 * compileRenderGraph 报，不重复校验）；`variant` 键出现 = ConfigLoadFailed（键已删除，
 * 静默忽略会让用户以为切了路线）；脚本/类型/枚举字符串错误 → std::unexpected。
 * @param path 配置文件路径（相对 CWD，须为 build/rgl）
 * @return RgGraphDesc；解析失败返回 Error
 */
export [[nodiscard]] std::expected<RgGraphDesc, Error> loadRenderGraphConfig(std::string_view path);

}  // namespace RGL
