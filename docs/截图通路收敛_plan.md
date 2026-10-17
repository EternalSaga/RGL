# 截图通路收敛：删 `llm_debug` 配置路，统一到 RPC

> **状态：现行**（2026-10-17 立项）。裁决本体早在 `docs/phase14_plan.md` §六（"在册待办、未开工"），
> 本文是它的施工计划 + 本轮新实测，**不重开该不该删的讨论**。

## 0. 一句话

**两条采集路径 = 两个测量口径**（§六 原话，且已被实测坑过一次：同配置时域档 **3.47 vs 7.03**）。
删配置驱动的 `llm_debug` 路、截图统一走 RPC。
🔴 **但顺序上必须先做一件"删掉就永远做不了"的事：趁两条路并存，拿它们互相对拍。**

## 1. 为什么 RPC 是超集（实测，非引用注释）

| llm_debug 能做的 | RPC 侧 | 证据 |
|---|---|---|
| 定帧截图 | ✅ **更强**：`/shotburst {n:1, start_frame:N}` = 绝对帧锚 + 应答回显自证 + 锚已过响亮 409 | `debug_rpc_burst.cppm:112-120`、`debug_rpc_exec_impl.cppm:446-449` |
| 指定输出文件名 | ✅ `/shot {"file"}`、burst `dir`+`prefix` | `:308-310`、`:396-398` |
| G-buffer 通道回读 | ✅ `/shot {"gbuf":N}`，**且不写死出图名**（llm 档写死 `gbuffer_{N}.png` 并忽略 `llm_shot.file`） | `:311-313` vs `llm_debug_impl.cppm:256-257` |
| 具名附件回读（`object_id`/`taa_out`） | ✅ **RPC 独有**，llm 路根本没有 attachment 入口 | `:316-331`；`llm_debug_impl.cppm:260-263` 自陈 |
| headless 可用 | ✅ 且比文档更强：`wireDebugRpc` 只门 `diagnostics.rpc`，帧边界挂点在 hook 缺失时自己 emplace | `:690`、`:710-718` |
| 干净退出 | ✅ `/quit` 走与用户关窗**完全同一条**路径 | `:504-508` |
| **无客户端即可跑** | ❌ **补不了**，这是 RPC 的结构性属性 | 见 §4 |

⚠ `tools/imgqa/imgqa.h:176` 那句"单张精确钉帧仍只有它（llm）"**是过期陈述**，被债-89 的
`start_frame` 推翻（同文件 `:174-175` 已自认，`:176` 是残留）。**别拿它当"llm 不可替代"的证据。**

## 2. 🔴 第 0 刀（必须先做，且是窗口期唯一一次机会）

**趁双轨并存做交叉验证**：同一份配置，分别用 ① llm `delay_frames=N` 与 ② RPC
`/shotburst {n:1, start_frame:N}` 各截一次，逐位对拍。

为什么它非做不可：
- 它一次回答三个问题——**帧号口径错不错开**、**RPC 产物顶不顶得住逐位判据**、
  **债-88 的"低频漂移"里有多少其实是双轨口径**。
- 两条路**互斥**（`debug_rpc_exec_impl.cppm:300-303` 的 409），所以只能分两次运行对拍——
  而这正是能做的形态。
- 🔴 **删掉 llm_debug 之后这个实验永远做不了。** 双轨现在是一件量具，删了只剩一条腿，
  再想知道答案就只能靠回忆。

已知口径差异（本轮实测，是"为什么要对拍"的理由，不是"已经证出错开"）：
`llmFrame` 在 `afterDraw`（drawFrame **之后**）递增且"仅 llm 启用时"（`ecs_frame_diag.cppm:46,70`）；
`wiring->frame` 在 ConfigReloadHook 帧边界（**system 链之前**）无条件递增（`debug_rpc_exec_impl.cppm:703`）。
两者都从 0 起每帧 +1，**是否真的错开未证**——§六 那次 3.47 vs 7.03 是时域档读数差，
可能同源，也可能另有原因。**这条只能由第 0 刀定案。**

## 3. 删除面（本轮实测盘点，够施工用）

### 配置面：少 **5** 枚键（不是 4）
`llm_debug`、`llm_shot.file`、`llm_shot.size`、`llm_shot.delay_frames`
+ 🔴 **`dump_gbuf`**——它的**唯一读取点**在 `LlmCaptureFlow::requestBeforeDraw`
（`ecs_frame_diag.cppm:56`），删 llm 路即成无消费者之键；RPC 的 `/shot {"gbuf":N}` 覆盖同一能力且更好。
⟹ 债-129 的出厂空开关 **31 → 26**。

### 引擎侧：模块**不能整删**，只删配置语义
- 🔴 **必须保留**：`writeCapturePng` + `toRgba8` 全格式表（回读产物**唯一落盘出口**，
  `llm_debug_impl.cppm:171-172` 自陈）、`frame_capture` 全模块、`Renderer::requestFrameCapture` 族、
  `LlmDebugConfig` 类型（RPC 拿它当"只装 file 的参数袋"，`debug_rpc_exec_impl.cppm:436-438,:484-486`）。
  ⟹ 建议 `llm_debug.cppm` **改名**为 capture-sink 一类，别留"debug 模式"的语义。
- **删**：`LlmCaptureFlow`（`ecs_frame_diag.cppm:37-99`，`FrameStatsTracker` 留）、
  `llmActive` 全链 **9 处 + 2 个 409 分支**、`GateEnv::llmCapture` 字段 + `dump_gbuf` 那条
  **error 级 lint 规则**（`gate_lint_impl.cppm:176-178`）、`main_impl.cppm` 的 llm 尺寸/隐藏窗分支
  （⚠ `show_after` 仍要 hidden，别一起删）。

### 互斥约束消失：**~23 处**
引擎 reject 2 + plumbing 7 + imgqa 互斥管理 5 + regress 3 + qa 冻结配置注释 6。

### 工具侧：**这是成本大头**
- `tools/smoke.py` **42 处**：进程模型从 `subprocess.run(等自退)` 换成
  `Popen → 等端口 → 轮 /state.frame → POST /shot → POST /quit → wait`。
  🔴 **模板仓库里有两份现成的**：`regress.py:963-1011`（`_free_port`/`_wait_rpc`/`_wait_frame`/`_rpc`）
  + `:845-867`（完整生命周期含 `finally` 里 `/quit`）；C++ 侧 `imgqa/engine.cpp:591-608`。
  顺带**整块删掉** dump_gbuf 的补偿逻辑（`:375-396`、`:661-671`、`:680-710`）——那是纯工具税。
- `tools/regress.py`：`capture()` 换 RPC burst-with-anchor（4 处引用 + 重写）。
- `tools/imgqa/` **46 处**：删 `runEngineOnce`、`applyLlmShotGate`、`overrideLlmShot` 三字段；
  `shadow-softness` / `shadow-kernel-scale` / `probe` 三台仪器改 RPC 形态。
  ⚠ `just analytic`（闭式解探针）走 `runEngineOnce` = llm 形态，**在删除射程内**——
  与 `docs/图变体收敛_plan.md` §6 那条"删 forward 前要先重立探针"**撞同一处代码**，两件事要合排。
- `qa/configs/{analytic,ssr_rig}/runtime.lua` 改 `llm_debug=false` + `rpc=true`。

## 4. 🔴 已知代价（明确接受，不许假装能补齐）

1. **"无客户端即可跑"永久失去。** `just smoke` 从"零依赖一把梭"变成多一个端口、
   多一类"RPC 没起来"失败模式、多一条端口探测红线（`debug_rpc_impl.cppm:117-137`：
   `SO_REUSEPORT` 会让多实例**静默共存**，所以 bind 前必须探测）。
2. **窗口模式下"隐藏窗 + 任意尺寸"无 RPC 等价端点**（`/resize` 明确 headless-only）。
   可用已有键复现（`win_size` + `show_after ≥ 0`，二者与 llm 共享同一 `hidden` 开关），
   但 `smoke.py:434` 恒设 `headless=true` ⟹ 这条路径今天无人走，属**未验证等价**，删除时记一句边界。
3. `/shot` 的 10s 应答超时在慢设备/大尺寸 capture 帧下可能 504（llm 路无 HTTP 超时）。属调参。

## 5. 引擎侧唯一要先补的小口

🔴 `/shotburst` 的 `attachment` **白名单只收 `swapchain|taa_out`**（`debug_rpc_burst.cppm:49-50`），
而 `/shot` 的 attachment 无白名单 ⟹ **"钉帧 + `object_id"` 这个组合今天表达不出来**。
今天无人要（object_id 全走 `/shot`），但 imgqa 全量 RPC 化并保留钉帧采集时它是硬缺口。
放开白名单（或改成"非空即收"）是本次唯一需要的引擎侧功能改动。

## 6. 排序

| 刀 | 内容 | 依赖 |
|---|---|---|
| **0** | 放开 `/shotburst` attachment 白名单 + **双轨交叉对拍**（§2） | 无。**窗口期，越早越好** |
| 1 | `smoke.py` 加 RPC 一次性运行器（**与 llm 并存**），`regress floor/ab` 切过去 | 刀 0 的对拍结论 |
| 2 | imgqa 三台 llm 形态仪器 + `just analytic` 改 RPC | 刀 1 |
| 3 | 删配置面 5 枚键 + `LlmCaptureFlow` + `llmActive` 全链 + lint 那条 error + 互斥管理 | 刀 1、2 全绿 |
| 4 | `llm_debug.cppm` 改名、文档/skill `rgl-verify` 口径同步、`phase14_plan.md` §六 收账 | 刀 3 |

与债-129 的关系：**不冲突且互相成全**——本删除让配置面少 5 枚键、少一条 lint error、
出厂空开关 31→26，全是债-129 要的。但刀 2 与"闭式解探针重立"撞同一处代码（`runEngineOnce`），
**两件事必须合排**，别各改一遍。

## 7. 顺带查到的过期指针（建议一并修）

- `qa/configs/{nm_rig,env_band}/runtime.lua:5` 指 `/shotburst` 的 409 在 `:365`，实际在 `:376-378`
  （`/shot` 的在 `:300-303`）。
- `docs/技术概念.md:377-385` 端点表缺 `/resize`、`/pass`；`/shotburst` 参数缺 `attachment`、`start_frame`。
- `docs/gate_lint_plan.md:89-96` 的 `GateEnv` 列 6 字段，代码 `gate_lint.cppm:37-41` 只有 3 个。
- `tools/imgqa/imgqa.h:176`「单张精确钉帧仍只有它」（§1 已述）。
