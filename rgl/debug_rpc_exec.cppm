module;
#include <entt/entt.hpp>  // wireDebugRpc 签名含 entt::registry

export module debug_rpc_exec;

import rlstd;
import debug_rpc;    // DebugRpcServer（返回类型 + 停机入口）
import asset_table;  // AssetTable（材质写通道 / 参数页构建）
import vk_renderer;  // Renderer（/shot 帧捕获宿主）
import runtime_config;  // RuntimeConfig（/decorate 探针读配置门：aa / velocityField / ssr.enabled）

namespace RGL {

/**
 * HTTP POST RPC 调试通道**执行层 + 装配**（Phase 10b 批 11b.2；
 * docs/archive/phase10b_plan.md §4 批 11b.2）。
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
 * @param runtime runtime.lua 生效值（`/decorate` 探针的配置门读数；**只读**，门是创建期量，
 *   热重载改了它而图没变 = 探针读到的是配置现值，这一分歧不在本批判据范围，见 exec 侧注释）
 * @param llmActive llm 截图模式在跑（true = `/shot` 拒 409——防抢帧卡死 llm 退出）
 *
 * 端点与各自的超时**不在本文件抄**（抄一次漂一次）——名单 = `rgl/rpc_schema.lua`，
 * 运行期发现面 = 根路由的回包。本模块的分发按生成表 `rpcRouteByPath` 查表后走穷尽 switch，
 * 新增端点忘写处理分支 = 编译不过。请求字段同理：读字段名吃 `rpc_field::<端点>::<字段>`
 * 常量，形状（含未声明的键 = 400）由 `rpcClaimRoute` 里的 `rpcValidateRequest` 按生成表判，
 * 本模块不写任何一条字段名或形状文案。
 * @return 服务实例（nullptr = 门关 / bind 失败）；所有权归调用方（main 栈，
 *   声明序须在 registry / assets / backEnd 之后 = 析构序其先，帧循环后可显式 stop）
 */
export std::unique_ptr<DebugRpcServer> wireDebugRpc(entt::registry& registry,
	AssetTable& assets, Renderer& renderer, const RuntimeConfig& runtime, bool llmActive);

}  // namespace RGL
