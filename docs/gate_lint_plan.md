> **状态**：已落地（2026-10-02 立项并当日收口，commit `55e4b67`→`ebf5042`→`bee4f7a`→`3d80fa1`；
> 与计划的两处偏差记录在文末「执行偏差」）
> 一句话：把「门组合无效」与「装配顺序契约」里**能被静态判定的那部分**变成启动期可执行检查，
> 并把门回显从手抄子集改成派生 + 覆盖率护栏。不做 main 装配序列的影子哨兵（§2 非目标）。

# 门配置自检与全量回显（gate_lint）

## 1. 现状账（每条都复核过）

| 缺口 | 证据 | 后果 |
|---|---|---|
| 门回显是**手写子集**，已漂移 | `DiagnosticsConfig` 有 **38 个字段**（`rgl/diagnostics.cppm:17-54`），回显行只列约 11 个（`rgl/diagnostics_impl.cppm:207-213`）；`shadow_pass`（**默认就开**）、`picking`、`shadow_evssm`、`izb_*`、`rpc`、`imgui`、`llm` 全不出现 | 启动日志看起来"门都关着"，实际 `configs/diagnostics.lua:34` 里 `izb_shadow = true` |
| 「配了但无效」判在**每帧录制路径**里 | `rgl/vkrenderer_passrec.cppm:286`（EVSSM 非 deferred，warn-once）、`:297`（slot 缺失）、`:321`（IZB shadow0 空段） | 不走到那一帧就永远不喊；且判据散在执行器热路径 |
| IZB 与 EVSSM **不对称** | `izbPreFrame`（`vkrenderer_passrec.cppm:233-241`）非 deferred 时**静默早退**，EVSSM 同条件 warn | 同一个坑，一个喊一个不喊 |
| 「是不是 deferred」**4 处 3 语义** | 配置层枚举（`render_graph_config.cppm:39` + `main_impl.cppm:251`）／扫 passes（`vkrenderer_passrec.cppm:235-236` 与 `:283-284`，两处逐字相同的 `any_of`）／按 RT 名查（`frame_capture_impl.cppm:116-134`） | 自定义 `variant="forward"` + 手写 `light` pass 时两侧分歧（今天被 `vkrenderer_impl.cppm:174-179` 的条目缺失 throw 兜住，但确实是两个真相源） |
| 装饰失败只有 picking 一处喊 | `main_impl.cppm:252-260` 两个 warn；`decorateShadowPass`（`:266`）返回值**直接丢弃** | 高亮/阴影插在哪只有读代码才知道 |
| 已有 lint 的**误杀史** | `main.cppm:125-137`：`validateEnvironmentContract` 曾用 `contains` 子串匹配，误伤 `model_pbr_ibl_v2` 类未来命名 + 把"表里存在但未引用"（设计上合法的惰性）当错误 | 本计划 §5 三条防腐条款全部由此而来 |
| 门在进程内**不可变** | `setDiagnostics` 全仓只有 `main_impl.cppm:134` 一处调用（另一处是测试） | 启动期 lint 对整个进程生命周期权威；`vkrenderer_passrec.cppm:302`「热开门（RPC 拨开关）」的注释**已过时**，随本批改正 |

现存「配了但无效 → 日志」共 18 处（12 warn + 2 解析期 warn + 3 静默早退 + 1 error 降级），
形态、级别、时机各不相同；另有约 10 条路径**完全静默**（IZB×forward、EVSSM×`shadow_pass=false`、
PCSS 让位 EVSSM、`picking_probe`×`picking=false`、输入探针×headless、`rpc_port` 越界回落、
`dgc_static` 能力不符、`pcss_*` 死值、`texel_canary` 帧号不可达）。

> 2026-10-09 批 N6b：阴影门迁 configs/shadow.lua 枚举，本节旧键名是当时现场；现行规则表口径见下文级别判据表。

## 2. 目标 / 非目标

**目标**：四件事，按收益/成本排序。
1. **门回显派生 + 覆盖率护栏**（§3-B）——补一条已经在撒谎的日志行。
2. **`gate_lint` 纯函数**（§3-C）——申报式规则表，启动期跑全表，与回显并排打印。
3. **deferred 语义收口 + 交叉校验**（§3-A）——两个真相源不强行合一，各命名 + 互相校验。
4. **案例迁移**（§3-D）——把 passrec 的 3 处 warn-once 与 picking 的 2 处 warn 收进同一张表。

**非目标（明确不做）**：
- **不做 main 装配序列的影子哨兵**。顺序契约（`main_scene.cppm:74` 几何槽先于纹理槽、
  `:164` 阴影图槽先于模型条目烘焙、`:355` RT/深度槽先于 `add*Entry`）要检查就得让"装配状态"
  成为可检查对象，而在 main 外面重建一份装配序列 = 影子模型，改架构必滞后，**滞后的检查
  就是谎言行**（比没检查更坏）。正确落点是让被检查对象自带断言——先例已有：
  `AssetTable::expectedGeometrySlot`（堆槽三段分配契约自己校验）、`checkPushDataLimits`。
- 不让 lint **阻止进程启动**（诊断门不该让程序起不来；级别用 ERROR 日志，返回值不改）。
- 不改 `configs/*.lua` 的任何现值（`izb_shadow = true` 是用户现值，lint 只喊不改）。
- 不引入自定义宏（rgl/ 目前除第三方开关外**零自定义 `#define`**，不为覆盖率护栏破这个惯例；
  退路方案见 §7）。

## 3. 设计

### A. deferred 语义收口（先做，lint 依赖它）

两个语义各自命名，**不二合一**：

```cpp
// render_graph.cppm（图事实层）
/** 图里是否有 deferred 光照 pass（drawLight + state=="light"）——
 *  IZB / EVSSM / dump_gbuf 的消费方判据（它们要的是"有没有人消费阴影/有没有 G-buffer"，
 *  不是"配置写了什么"）。收口 vkrenderer_passrec 两处逐字相同的 any_of。 */
export bool graphHasDeferredLight(std::span<const RgPassPlan> passes);
```

配置意图层保留 `RgGraphVariant variant`（`main_impl.cppm:251` 的 `deferredMode` 不动）。
两者的**分歧交给 lint 交叉校验**（§3-C 规则 R-01），而不是让某一方消失。

### B. 门回显派生 + 覆盖率护栏

1. **解析器记录读过的键**：`readBool` / `readF64` / `readI64` / `readRpcPort` /
   `readProbeNum` / `readCameraProbeKey` / 三个 probe 表读点（`diagnostics_impl.cppm:38,49,61,83,89,129,152,162,164,177`
   ——全仓对 sol 表的直接下标就这 10 行，无旁路）统一把键名记进一个 `KeyTrace{ read, tables }`。
2. **回显表**（`diagnostics.cppm` 新增，单一真相）：
   `std::array<std::pair<std::string_view, const DiagnosticsConfig::* T>, N>`
   ——成员指针 + lua 键名，通用格式化打印。新增门时**只在这一处加一条**，回显自动跟上。
3. **两个护栏（`just test` 里跑，毫秒级）**：
   - 覆盖率：`解析器读到的键集合 == 回显表键集合`（漏一个 → 红；回显有而解析器不读的
     陈旧键也 → 红）。这是"不出声失效"的正面解法。
   - **未知键**：遍历 Lua 顶层键，凡解析器没读过、又不是已知子表名 → `warn`
     （`diagnostics.lua` 里**拼错的键今天完全静默**，这是普查的附带收获）。
4. **打印形态**（避免几十键一行的噪音）：
   - `info`：只列**偏离默认值**的门（= "你实际配了什么"），一行；
   - `debug`：全量键表。
   "偏离默认"的比较基准 = `DiagnosticsConfig{}`（零新机制）。**批 N6b 起两份配置各一份回显表**
   = diagnostics 30 键（`kGateEcho`）+ shadow 11 键（`kShadowEcho`），键名各以回显表为单一
   真相源；shadow 侧另有一行恒打的路线行（`shadow: mode=… izb=…`，见
   `docs/shadow_routes_plan.md` §3.1 第 6 条）。

### C. `gate_lint` 模块（新，rlscene）

```cpp
// rgl/gate_lint.cppm —— 纯函数，不碰设备、不碰单例（一切事实作入参 → just test 可测）
export enum class GateSeverity : uint8_t { inertNote, warn, error };
export struct GateEnv {
    bool headless = false;          // runtime.lua
    bool llmCapture = false;        // runtime.lua llm_debug（dump_gbuf 的消费方）
    bool staticGraphicsIndirect = false;  // 设备能力（调用方填；测试自由构造）
    bool graphicsIndirect = false;
    bool sourceConfigsAvailable = true;   // 源 configs/ 可得（热重载前提）
    bool slangcAvailable = true;          // shader 热重载三件套
};
export struct GateFinding {
    std::string_view gate; GateSeverity severity; std::string message;
};
export std::vector<GateFinding> lintDiagnosticsGates(const DiagnosticsConfig& cfg,
    const DiagnosticsConfig& defaults, std::span<const RgPassPlan> passes,
    RgGraphVariant variant, const GateEnv& env);
```

- **申报式规则表**（一条规则 = 门名 + 谓词 + 原因），全表集中一文件、顺序固定。现行表 =
  **43 行 / 41 门**（diagnostics 侧 30 行 30 门 + shadow 侧 13 行 11 门；行数 ≠ 门数，
  一门可多条规则 → §8.6）。
- **覆盖率护栏**：测试断言 41 个门**每个都有一条规则或显式 `noPrecondition` 标注**——
  加门时必须回答"这门的生效前置是什么"，答不出就得显式写"无前置"。批 N6b 起两份配置各一份
  回显表，判据 = **规则表门名集合 == 两份回显表键集合之并**（`gateLintRuleGates()` 对
  `diagnosticsEchoKeys() ∪ shadowConfigEchoKeys()`）。
  （与 `pass_vocabulary` 同构：把"记得同步"变成"同步不了"。）
- **判据基准**：只对「bool 门 == true」或「数值 ≠ 默认」的配置喊——`configs/diagnostics.lua`
  显式写了 30 个键且多数等于默认值，否则启动日志会因为文件里写满默认值而狂喊。
- **接线**：`main_impl.cppm` 在 `compileGraphOrThrow` 之后（拿到图事实）、构造 GPU 对象之前跑，
  输出 `gate_lint: N findings`（error 计数进启动摘要）；与 §3-B 的门回显**相邻打印**。

### D. 案例迁移（4 条）

| # | 现状 | 迁后 |
|---|---|---|
| 1 | `izbPreFrame` 非 deferred **静默**早退（`vkrenderer_passrec.cppm:239-241`） | 启动期 `error`（R-02）；运行期早退保留、无需 warn |
| 2 | `evssmPreFrame` 非 deferred warn-once（`:283-289`） | 启动期 `error`（R-03）；删 `warnedNonDeferred` |
| 3 | `warnedNoSlots`（`:297`，slot 缺失） | **保留运行期**——依赖设备资源，不是配置组合（§4 裁决标准） |
| 4 | picking / highlight 装饰失败 warn（`main_impl.cppm:252-260`） | 进 lint 表（R-04/R-05），消息补上"锚点是什么、为什么没插进去" |

附带：改正 `vkrenderer_passrec.cppm:302`「热开门（RPC 拨开关）」过时注释；
`rpc_port` 越界静默回落（`diagnostics_impl.cppm:37-45`）补解析期 warn。

## 4. 分级裁决标准（不是"哪个门"而是"哪一类"）

| 级别 | 判据 | 例 |
|---|---|---|
| `error` | 用户明确开了 X，X 在当前配置下**必然完全无效**，且**没有其他任何信号** | `izb`×非 deferred、`izb`×`mode="none"`、`mode="evssm"`×非 deferred、`picking_probe`×`picking=false`、输入探针×headless、`dump_gbuf`×llm 关、variant 意图与图事实矛盾（R-01） |
| `warn` | 部分生效 / 被更高优先级让位 / 设备能力不符 | `izb.dump_frame`×`izb` 关、`dgc_static`×非 RADV、`cull_replay`×`dgc_static` 开、`rpc_port`×`rpc=false` |
| `inertNote`（debug 级） | **设计上合法的惰性**——喊它就是制造噪音、教人不信任日志 | 阴影装饰无条件（`main_impl.cppm:261-266`：条目无条件烘槽，故图必须有无条件阴影 pass）、未引用的 pipeline 条目、`pcss.*` / `evssm.*` 调参块在 `mode` 不是本档时没人读（按档分立）、`izb.grid`×`izb` 关 |

> 「依赖设备资源是否缺失」**不是**配置 lint 的判据（`warnedNoSlots` 留运行期）——
> 否则 lint 就不能在 `just test` 里跑，且会把跨机差异带进启动判定（违反"工作机器不著书"）。

## 5. 摩擦审计与三条防腐条款

TAA 视角（立项时按它演算过）：TAA 必然插一个**采样场景色附件**的 pass，而
`decorateHighlightPass` 的锚点正是"首个采样场景色附件的 pass 之前"
（`render_graph_decorate.cppm:36-48`）——**今天它抢锚点不会有任何检查变红**。
所以"检查"对 TAA 是净收益，前提是检查的是契约而不是实现细节。三条防腐：

1. **判据来自申报，不来自枚举组合，也不来自字符串猜测**。
   `validateEnvironmentContract` 的 `contains` 误杀史（`main.cppm:125-137`）是本项目一手教训。
   新子系统只需在自己那一行写清前置谓词，不必去别人的表里补规则。
2. **每条规则必配"合法组合不得喊"的反向用例**（`rgl-test-discipline` 的反向用例纪律在此的
   具体化）。只测"该喊的喊了"= 一个常错的 lint，而常错的 lint 等于被静音的 lint。
3. **禁止把实现细节写成断言**（如"tonemap 是 passes 末位"、"ao 必须紧邻 tonemap 之前"、
   pass 下标/名字表）。这类断言在架构演进时的必然结局是被人删掉或加例外——护栏比没有更坏，
   因为它训练人不信任护栏。

## 6. 提交切分与验收

| commit | 内容 | 验收 |
|---|---|---|
| 1 | §3-A `graphHasDeferredLight` 收口（passrec 两处 lambda → 1 个导出函数）+ 测试 | `just build` 0 警告；`just test` 绿；变异：删 `light` pass → 判据红 |
| 2 | §3-B `KeyTrace` + 回显表 + 两护栏 + 非默认门打印 | 覆盖率用例红/绿可验；变异：回显表删一键 → 覆盖率红；lua 塞未知键 → warn |
| 3 | §3-C `gate_lint` 模块 + 规则表 + §3-D 迁移 + main 接线 + 门分类测试 | 每条规则正例 + 反向用例；覆盖率 38 门；实机启动日志能看到 findings |

**判据纪律**（`rgl-test-discipline`）：每条新用例带变异红线声明；`gate_lint` 是纯函数 →
无设备毫秒级，进 `just test`；`tests/CMakeLists.txt` 需登记 `test_gate_lint.cppm`。

## 7. 风险与退路

| 风险 | 缓解 / 退路 |
|---|---|
| 误杀未来命名或合法组合（有历史） | §5 条款 1+2；error 判据只允许"必然无效"，其余一律 warn |
| 启动日志变长 | `info` 只列偏离默认值的门；全量走 `debug` |
| 回显表成员指针形态与 Doxygen/style 检查打架 | 先小步试编译；退路 = 回显表用 `{键名, std::function<std::string(const Cfg&)>}`（多一次分配，但只在启动期） |
| `KeyTrace` 记录影响解析路径 | 记录发生在启动期一次、38 个键量级；解析器返回值不变（`KeyTrace` 出参/侧信道，不进 `DiagnosticsConfig`，不污染数据体） |
| lint 表覆盖率护栏做不到"编译期" | 退到测试期断言（41 门逐个查），已足够：加门时 `just test` 红 = 同一个提醒效果，只是晚到测试阶段 |

## 8. 执行偏差（落地当日记，与上文设计不一致处以此为准）

1. **回显表形态**（§3-B.2）：未用「成员指针 + 通用格式化」，实际 =
   `{键名, 非捕获 lambda → 函数指针}`；「偏离默认」按**格式化后字符串**比较差集
   （门全是标量，零类型机制；成员指针形态反而要为 bool/整/浮点各写分发）。
2. **GateEnv 字段砍半**（§3-C）：`sourceConfigsAvailable` / `slangcAvailable` /
   `graphicsIndirect` 删除——源 configs 目录 / slangc 缺失时
   `wireHotReload` / `ShaderHotReloader::init` **已有各自降级 warn**，lint 重复喊
   = 影子真相源（违反自家防腐条款 1 的精神）。`dgc_static` 判据只需
   `staticGraphicsIndirect` 一个能力位。
3. **§3-D #4 未整条迁移**：picking/highlight 装饰失败 warn **留在装饰期**
   （失败与否 = 装饰函数对图形态的判定结果；lint 复刻锚点判据 = 影子模型）。
   实际处置 = 消息改为自带锚点定义（"为什么没插进去"不再需要读代码）。
4. **回显打印位置在解析器内**（替代此前手写 11 键 info 行的原位），不在 main
   ——与 lint findings 打印（`runGateLint`，图编译后）相隔一条 RenderGraph 行，
   「相邻打印」按此满足。
5. 附带收获：`loadDiagnosticsConfig` 拆分 `applyScalarGates` +
   `applyProbeAndNumericGates`（阈值倒逼，纯搬运）；`kDeferredLightState` 具名常量 +
   `static_assert` 与词表绑定；`vkrenderer_impl` 的 drawLight 缺条目消息修掉
   ao 误报 Tonemap 的三元式。
6. **一门可以有多条规则（2026-10-09，债-32 收口时新增）**：`izb` 现有两行
   （非 deferred 图 / `mode = "none"`）——同一扇门的两条**不同根因**各配一条消息，
   合并成一条谓词 = 丢归因（本模块的存在理由正是"报在肇事配置上并说清为什么"）。
   ⟹ §3-C 的覆盖率护栏钉的是**去重后的门数** == 两份回显表键数之和，规则表行数另作"表没被整段
   删空"的下界哨兵（两者都在 `tests/test_gate_lint.cppm`）。同批 N6b 把阴影侧整段换成
   **枚举口径**后，「PCSS 让位 EVSSM」那条 warn **整条删除**（枚举下两门同开表达不出来，
   让位链不是被修好，是被消掉）；现行阴影侧 13 行 / 11 门的清单 →
   `docs/shadow_routes_plan.md` §3.2 第 4 条。
