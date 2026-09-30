module;
#include <entt/entt.hpp>  // wireDebugRpc 签名含 entt::registry

export module debug_rpc_exec;

import rlstd;
import debug_rpc;    // DebugRpcServer（返回类型 + 停机入口）
import asset_table;  // AssetTable（材质写通道 / 参数页构建）
import vk_renderer;  // Renderer（/shot 帧捕获宿主）

namespace RGL {

/**
 * HTTP POST RPC 调试通道**执行层 + 装配**（Phase 10b 批 11b.2；
 * docs/phase10b_plan.md §4 批 11b.2）。
 *
 * 分层：`debug_rpc` = 传输层（HTTP 线程 / 队列 / 超时与停机结算，**零 ECS 零协议**）；
 * 本模块 = 协议解析 → registry/渲染器套用 → `ConfigReloadHook` 帧边界挂点。它是
 * 唯一同时认识 ECS / 渲染器 / 协议的 RPC 侧模块（main 是装配调用点）。
 *
 * 门 = `diagnostics.rpc`（默认 false = 返回 nullptr 零装配，**与 imgui 门独立**，
 * headless 可用）。装配 = 建服务（只绑 127.0.0.1，bind 失败 = 日志降级关不崩）+
 * applier 闭包（捕获共享装配态：registry/资产表/渲染器 + 在途 /shot）+
 * `ConfigReloadHook` 挂点（已挂 = 组合追加在配置/shader 热重载之后，未挂 = 独占 emplace）。
 *
 * 帧边界时序（`configReloadTick` 在 system 链之前）：`/camera`、`/param` 套用后
 * **本帧即生效**；`/shot` 帧首请求 → 本帧 drawFrame 回读 → **下帧**帧首结算写盘；
 * `/quit` 置 `AppExit` → 帧首判定 → 与用户关窗同一条干净退出路径（供自动化测试收尾）。
 *
 * @param registry 场景 registry（ConfigReloadHook ctx 挂点 + Camera/面板态）
 * @param assets 资产表（材质写通道 + 参数页构建）
 * @param renderer 渲染器（/shot 帧捕获）
 * @param llmActive llm 截图模式在跑（true = `/shot` 拒 409——防抢帧卡死 llm 退出）
 *
 * 端点：`GET /`（路由发现）`GET /state`（相机 + 参数页）`POST /camera` `POST /param`
 * `POST /shot`（帧捕获写盘）`POST /quit`（置 AppExit 干净退出，自动化测试收尾用）。
 * @return 服务实例（nullptr = 门关 / bind 失败）；所有权归调用方（main 栈，
 *   声明序须在 registry / assets / backEnd 之后 = 析构序其先，帧循环后可显式 stop）
 */
export std::unique_ptr<DebugRpcServer> wireDebugRpc(entt::registry& registry,
	AssetTable& assets, Renderer& renderer, bool llmActive);

}  // namespace RGL
