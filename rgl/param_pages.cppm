module;
#include <entt/entt.hpp>  // buildDebugParamPages 签名含 entt::registry

/**
 * IMGUI 调试面板页构建（Phase 10 批 10.6 自 ecs 拆出——ecs_impl 破 500 代码行红线，
 * 面板页构建与帧循环 system 职责本不同源）。
 *
 * 分层与依赖方向：本模块**认识** ECS（读 registry ctx/组件搭控件），imgui_layer 仍
 * 零认识 ECS（经 builder 回调注入，10.1 契约不变）；ecs 不 import 本模块（无环）。
 * 控件语义（label/range/speed）= param_semantics_generated（数据源
 * rgl/param_semantics.json，D1 单一事实来源）；本模块只补反射永远给不了的写入通道
 * （get/set 闭包捕获 ctx/组件/资产指针）。
 */
export module param_pages;

import rlstd;
import param_registry;  // ParamPage 返回类型
import asset_table;    // AssetTable（材质写通道）

namespace RGL {

/**
 * 构建调试参数页（imgui_layer builder 回调消费；每帧重建，调试路径分配）。
 *
 * 页 = "渲染"（SceneEnv ctx：曝光/env 强度/mip）+ "灯光"（Light 组件按 index 升序，
 * index 0 主光附共享 ambient）+ "材质"（选择器 + 五参数，10.2c）+ "选中"（点选实体
 * 材质上下文，10.5）。回调捕获组件/资产指针当帧即用（面板不增删 entity，entt 组件
 * 地址稳定；材质表仅注册期增长）。写回 = 改组件/ctx/材质表本体，下一帧渲染生效。
 * @param rg 场景 registry（读 SceneEnv/Light/MaterialRef/SelectedObject/DebugPanelState ctx）
 * @param assets 资产表（材质可变写通道）
 * @return 页列表（值语义，交 imgui_layer 渲染）
 */
export std::vector<ParamPage> buildDebugParamPages(entt::registry& rg, RGL::AssetTable& assets);

}  // namespace RGL
