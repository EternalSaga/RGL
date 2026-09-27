# Phase 9 实施计划：性能观测（Profiler）

> **状态**：计划（2026-09-27 起草；开工前三决策已确认，见 §9 决策记录）。
> 依据 = [next_roadmap.md](next_roadmap.md) Phase 9 ①–⑤（:184-194）+ 2026-09-27 方案讨论
> （CPU/GPU/present_timing 2.5 问 + ECS/AOP 取舍）。完成后本文件转 `archive/`，进度回写 next_roadmap。

## 0. 定位与目标

Phase 9 只回答问题，不做优化：**CPU-bound 还是 GPU-bound（还是 sync/present-bound）**，
为 Phase 10 阴影（next_roadmap :211 "X 由 Phase 9 profiler 定基线"）与 ⑤ 决策
（per-frame cull 读回是否延后、帧率上限 / present mode 可配置化必要性）提供输入。

三方判定（缺一不可，这就是 present_timing 进入 Phase 9 的理由）：

| 数据源 | 回答的问题 |
|---|---|
| CPU zones | 帧时间花在哪个 phase、**fence 等待（CPU 等 GPU）占多少** |
| GPU timestamps | GPU 实际干活多久（与 CPU 对比 → bound 判定） |
| present_timing | GPU 干完到**像素可见**还差多少（驱动排队 + 合成器）、实际呈现节奏 IPD、丢帧、刷新率/VRR |

硬约束：**默认关（`profiler = 0`）= 零行为变化**（BIT-IDENTICAL 截图回归过）；
开启时也不得改变渲染输出（时间戳不影响像素 → 开着也应 BIT-IDENTICAL，兼当验收）。

## 1. 现状盘点（调研事实，全部已核验 2026-09-27）

### 已有的地基

| 事实 | 定位 |
|---|---|
| 唯一现有帧计时 = `FrameStatsTracker`（整周期 avg/min/max，`diagnostics.frame_stats=N`） | `ecs_impl.cppm:335-370`，tick 调用 :458 |
| **system 显式列表已存在**（`FrameSystem{name, fn}`），执行循环一行可拦截 | 列表 `ecs_impl.cppm:411-417`，循环 **:438-440** |
| `FrameSystem.name` 注释写明"执行路径不读取"——profiler 将是它的**第一个消费者** | `ecs_impl.cppm:232` |
| **pass 循环已存在**，`RgPassPlan.name` 字段现成 | `vkrenderer_impl.cppm:503`，结构 `render_graph.cppm:233-243` |
| 帧相边界三入口（beginFrame/submit/present），双 fence 语义已文档化 | `frame_session.cppm:54/64/72`，双 fence 注释 :90-97 |
| cull 四阶段（dispatch→preprocess→**同帧等待**→readCull→reconcile）可量化 ④c 读回成本 | `cull_driver.cppm:58` |
| 诊断开关先例：`DiagnosticsConfig` + `configs/diagnostics.lua` + `loadDiagnosticsConfig` | `diagnostics.cppm:16-49` |
| 路线图 Phase 9 ①–⑤ 已规划（本计划即其展开） | `next_roadmap.md:184-194` |

### 缺的（= Phase 9 交付物）

- 无 per-phase CPU 计时、无 GPU 时间戳、无任何报告产物（grep `timestamp|profil|Timer` 全空，
  唯一 chrono 在 `texture_pool.cppm:458` env HDR 上传与 `llm_debug_impl.cppm:211` 帧名）。
- CPU/GPU/present 三方中**一方都没有**。

### 驱动/扩展实测（vulkaninfo，2026-09-27，本机单 GPU）

| 项 | 结果 |
|---|---|
| GPU0 = AMD Radeon 8060S（RADV **STRIX_HALO**），Mesa **26.2.3**，单 GPU（无 NVIDIA/Intel） | `vulkaninfo --summary` |
| `VK_EXT_present_timing` | ✅ **advertise，revision 3** |
| `VK_EXT_calibrated_timestamps`（+ `VK_KHR_calibrated_timestamps` 别名） | ✅ revision 2 / 1 |
| `timestampComputeAndGraphics=true`，`timestampPeriod=10.019`，图形队列 `timestampValidBits=64` | ✅ |
| 系统头 `VK_HEADER_VERSION 357`，`VK_EXT_present_timing` 完整定义（`vulkan_core.h:18454` 起，4 入口点 PFN :18571-18574） | ✅ |

**闭环归档遗留**：[archive/present_timing_research.md](archive/present_timing_research.md) §3.3
的两个 ❓ 中 **RADV 侧今日确认支持**（当年决策"暂不引入"的前提 = 驱动支持未验证，已解除）；
NVIDIA/Intel 侧仍 ❓（本机无卡，跨机补测，见 §4.3）。

## 2. CPU profiler 设计（批 9.1）

### 2.1 形态：零宏，函数调用 + `source_location` 默认实参（2026-09-27 D4 翻案）

```cpp
// rgl/profiler.cppm（接口）——纯函数 + RAII 类，零宏
export class Zone {                 // RAII：ctor 取时、dtor 写帧槽环形缓冲
  public:
    explicit Zone(std::string_view label = "",
                  std::source_location loc = std::source_location::current());
    ~Zone();
};
// 调用形态：auto z = RGL::zone("staging");   或   auto z = RGL::zone();
//   省略标签 → 函数名取 loc.function_name()，报告里带 file:line
```

**D4 原"最小宏"方案被两项实测推翻（2026-09-27，用户确认翻案）**：

1. **本项目 `__FILE__`/`source_location` 本来就是绝对路径**——CMake/Ninja 编译规则直接传
   绝对源路径（`build/build.ninja:793`：`CXX_COMPILER__rgl_scanned_Release
   /home/robin/program/RGL/rgl/ecs_impl.cppm`），无需 `RGL_SOURCE_ROOT` 拼接；
2. **named module 不导出宏**——clang 22 实测（`#define` 在 `import` 后 `use of
   undeclared identifier`），项目先例注释同结论（`rlvk.cppm:12`"#define 不随模块导出"）。
   `RGL_ZONE` 只能经 CMake 函数宏硬塞（`__COUNTER__` JOIN 拼接藏 CMake 字符串，
   Doxygen/风格检查盲区）或逐文件复制 define（DRY 违背）——两条路都比零宏差。

分工（宏总数 = 0）：

| 层 | 职责 |
|---|---|
| **函数 `RGL::zone(label?)`** | 默认实参 `std::source_location::current()` 捕获**调用点** file:line/function（绝对路径由工具链保证）；可选标签（服务 `FrameSystem.name` / `RgPassPlan.name` 运行时字符串） |
| **RAII 类 `Zone`** | 计时核心、`ZoneKind`（cpu/gpu/present）、开关早退、depth 配对 |

### 2.2 存储与开销

- **thread_local 定长环形数组**（如 4096 zone/帧）：零堆分配、零锁；本项目录制单线程，
  `tid` 固定。帧边界清零（挂在 frame_session beginFrame）。
- 时钟 `std::chrono::steady_clock`（glibc = `CLOCK_MONOTONIC`，TSC 实现 ≈ 20ns/次，
  每 zone 2 次）——选它的**关键原因**是与 `VK_EXT_calibrated_timestamps` 校准目标域
  `CLOCK_MONOTONIC` 同源 → CPU/GPU 同轴（§3）。
- 关断（`profiler=0`）= 构造函数首行早退 = 每个插桩点 2 个可预测分支，近零开销。
  **不做** `#ifdef` 编译期剔除（双层开关复杂度不值，见 §9 决策）。

### 2.3 插桩策略：分发点拦截，不做 per-entity component（AOP 讨论结论）

**否决"profiler component"**：zone 是**执行点**数据不是实体数据（system 是单例，
画不画与实体无关），component 无处安放，且违反 ECS 规范 1（每帧状态进 context）与
规范 4（分层：profiler 不该长在 registry 里）。

**C++ 无 AOP 织入**，可达的三档 = 分发点拦截（首选）/ 装饰器包 `FrameSystem.fn` /
RAII 调用点插桩；编译器级 `-finstrument-functions` 全函数插桩否决（开销大 + 仍需外部工具）。
本代码库**送了两个天然分发点**，一处插入、全量覆盖：

| # | 拦截点 | 自动覆盖 |
|---|---|---|
| 1 | `ecs_impl.cppm:438` system 循环 | 全部 5 个 system（camera/animation/bounds/projection/renderlist），名字用现成 `FrameSystem.name` |
| 2 | `vkrenderer_impl.cppm:503` pass 循环 | 全部 render graph pass（CPU 录制侧；GPU 侧见 §3） |
| 3 | `frame_session.cppm:54/64/72` 帧相 | fence 等待 / acquire / submit / present —— **"CPU 干活 vs 等 GPU"的分离靠这里** |

深挖点按需手动 `RGL_ZONE`：`cull_driver` runFrame 四阶段（④c 读回成本）、
`executePassDraws` 三路分发、`writeSceneStaging`、事件/resize/capture 段——
**先有全局图再决定深挖，不预铺**。

## 3. GPU profiler 设计（批 9.2）

**核心事实**：per-pass GPU 计时**不需要扩展**——`vkCmdWriteTimestamp2`（Vulkan 1.3 core，
本项目 1.4）+ timestamp query pool；真正需要的扩展是 **`VK_EXT_calibrated_timestamps`**
（本机已 advertise），唯一用途 = 把 device 时钟域校准到 `CLOCK_MONOTONIC` →
**CPU zone 与 GPU zone 落进同一时间轴**（报告可分析性的关键）。

通路（侵入面 = 2 行/pass，读回零 stall）：

1. query pool 按帧槽分段（`kFramesInFlight=2`，`frame_session` 双 fence 已保证语义）：
   **beginFrame 等完 fence 后 reset 本槽**（上一槽此刻必已完成，host reset 安全）；
2. 录制期每 pass 首尾各一次 `vkCmdWriteTimestamp2`（begin 取 ALL_COMMANDS 收口、
   end 取 NONE/BOTTOM 语义，sync2 stage 掩码细节实施时定）+ 帧首末（GPU 总跨度）
   + cull compute dispatch；
3. **下一帧 beginFrame（fence 已信号）`vkGetQueryPoolResults`（64-bit，不带 WAIT，
   不加 stall）**——读回延迟恒一帧；`timestampValidBits=64` 无需掩码；
4. 同步顺手 `vkGetCalibratedTimestampsEXT{DEVICE_EXT, CLOCK_MONOTONIC_EXT}` →
   仿射换算（`timestampPeriod=10.019ns/tick`），**maxDeviation 记进报告**。

**否决的扩展**：`VK_EXT_performance_query`（多 pass 采集 + `vkAcquireProfilingLockKHR`
+ devtools 特化，过重）、`VK_EXT_device_fault`（崩溃诊断非计时）。

**开销自检**：时间戳 ≈ 亚微秒/次，个别驱动有序列化效应——9.2 验收时开着/关着各跑
`frame_stats` 对比一遍，差值记入文档。

## 4. VK_EXT_present_timing（批 9.3，独立批）

### 4.1 对 profiler 的具体价值

1. **`QUEUE_OPERATIONS_END` stage**：present 提交后驱动侧排队结束——**任意 present mode
   保证可查**，当前 eImmediate 下唯一可靠 stage；
2. **`IMAGE_FIRST_PIXEL_VISIBLE`（窗口合成器可能给）**：GPU 末时间戳 → 像素可见 =
   端到端延迟；
3. **IPD 分布 + 丢帧计数**（stage time 全 0 = 请求被丢弃）：当前 `while(true)` 无限帧 +
   eImmediate = 满速渲染大量帧被 KWin 丢弃（archive §8 实测），IPD 抖动与丢帧率直接量化
   浪费——即 ⑤"帧率上限必要性"的证据；
4. **`refreshDuration`/FRR/VRR 判定**：归一化 IPD、识别刷新率切换。

eImmediate 限制（archive §3.1）：`targetTime` 调度仅 FIFO 系有效 → **我们只要测量、
不要调度，不受影响**；stage 查询仅部分可靠 → 先 probe 打日志再定深浅（§4.3）。

### 4.2 用法 = archive §6 步骤 1 原样（本计划不重复展开，只记要点）

- **可选扩展清单 all-or-nothing**：`VK_EXT_present_timing` + `VK_KHR_present_id2` +
  `VK_KHR_get_surface_capabilities2` + `VK_KHR_calibrated_timestamps` 全可用才启用，
  缺一整体 no-op + 日志（不进 `features.cppm` required 清单，双 GPU 不破坏；
  注意**本机两个 KHR 别名/依赖均需在实现时逐项核对 advertise**）；
- feature 链 `VkPhysicalDevicePresentTimingFeaturesEXT` 追加在 `featuresChain` 尾部、
  **必须做类成员**（vkdevice pNext 不悬空规则）；
- swapchain flag 探测走已启用的 `VK_EXT_swapchain_maintenance1`
  （`swapchainSupportedFlags` + `VkPresentTimingSurfaceCapabilitiesEXT`，archive :65）；
- 建后 `vkSetSwapchainPresentTimingQueueSizeEXT`（容量 ≈ image count × 倍数，如 16）+
  `vkGetSwapchainTimingPropertiesEXT`（首 present 前可能 `VK_NOT_READY`，重试）；
- present pNext 手工串联 `VkPresentTimingsInfoEXT → VkPresentId2KHR → SwapchainPresentFenceInfoEXT`；
- 每帧 `vkGetPastPresentationTimingEXT` 轮询（异步、数帧延迟、容忍部分/乱序结果）；
- **`VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT` 回退**（无限帧率下必撞）：本次不带 stage
  查询重试 present + 退避——**present 失败不得破坏渲染循环**；
- `recreate()` 后重设队列容量 + 重查时间域。

改动面（archive §3.4 原表）：新 `present_timing.cppm`、`vkswapchain.cppm`（flag/探测/recreate）、
`vkrenderer.cppm` present pNext + collect、`vkdevice.cppm`（featuresChain 尾）、
`features.cppm`（可选清单）。

### 4.3 风险与验证

| 风险 | 处置 |
|---|---|
| VVL 校验覆盖薄（archive :154） | 引入后留意校验告警，别静默吞 |
| eImmediate 下 stage 可能全 0 | 运行时 probe 打日志（哪些 stage 有值）→ 决定 9.3 深浅；只有 QUEUE_OPERATIONS_END 也可交付（IPD/丢帧仍有效） |
| 反馈仅轮询、queue-full | §4.2 回退路径为必须项，非可选 |
| **跨平台未验**：本机单 GPU RADV 已确认；NVIDIA/Intel ❓ | 9.3 验收在有卡机器补测，结果补录 `nvidia_compatibility_report.md`（archive §3.3 两个 ❓ 全部闭环） |

## 5. 报告设计（主格式已确认 = Perfetto）

**主产物：Chrome Trace Event JSON → Perfetto UI**（`ui.perfetto.dev` 浏览器拖入，
零安装无 sudo；Tracy/Chrome/Unreal 同款格式）：

- `ph:"X"` Complete 事件天然嵌套成火焰视图；`tid` 分轨：`main`（CPU zone 层级）/
  `gpu.graphics`（pass 时间戳，校准后**与 CPU 同时钟**）/ `present`（stage 瞬时 `ph:"i"`）；
- IPD、刷新周期用 `ph:"C"` counter track（随帧曲线，抖动一眼可见）；
- 产物：`build/rgl/profiles/trace_<时间戳>.json`（CWD = `build/rgl` 约定）；
- **防爆**：无限帧率下 trace 体积失控 → 帧环形缓冲（N 帧，见 §6）+ 退出时 dump +
  事件数硬上限。

**辅产物两条**（不开 UI 也能看）：

1. **rllogger 周期摘要 + 自动判定行**（扩展现有 `FrameStatsTracker` 语义）：
   `frame cpu=12.4ms gpu=8.2ms → GPU-bound` + 嵌套 breakdown（systems / fenceWait /
   acquire / 各 pass 占比）——**⑤ 的判定直接写进日志**；
2. **`profiler_stats.json`**：每 zone count/mean/p50/p95/max，机器可读，供 python 绘图
   或 llm 分析（archive §6 也计划了 `logs/present_timing.json`，并此处理）。

## 6. 配置开关（已确认形态）

`configs/diagnostics.lua` + `DiagnosticsConfig` 加键：

```lua
profiler = 0,   -- 0 = 关（默认）；N = 保留最近 N 帧，退出时 dump trace + 摘要
```

- 照 `frameStats` 先例（`diagnostics.cppm:29`）：`uint32_t profiler = 0;`
- 运行时开关（非 `#ifdef`）：关 = 早退分支，零产物；开 = 环形窗口 N 帧 + 退出 dump；
- **不**做周期 dump / 采样模式（YAGNI，需求出现再加）。

## 7. 分期批次

| 批 | 内容 | 插桩点/改动 | 验收 |
|---|---|---|---|
| **9.1** CPU + 报告骨架 | 新 `profiler.cppm(/_impl)`、`RGL::zone()` 零宏 API、拦截点 1-3、Perfetto writer、摘要（bound 判定行随 9.2 GPU 数据落地）、`diagnostics.profiler=N` | `ecs_impl.cppm:438`、`frame_session.cppm:54/64/72`、`diagnostics.cppm` + `configs/diagnostics.lua`、CMake FILE_SET | 默认关 BIT-IDENTICAL + `just checks` 过；开 = trace 进 Perfetto、摘要自洽 |
| **9.2** GPU 时间戳 | query pool 分槽、pass 循环/帧首末/cull 插桩、calibrated 校准、同轴合并 | 新 `gpu_profiler.cppm(/_impl)`、`vkrenderer_impl.cppm:503`、`cull_driver_impl.cppm runFrame`、`frame_session` reset/readback 挂点 | **开跑也 BIT-IDENTICAL**；读回零 stall；开销自检（`frame_stats` 前后差记档） |
| **9.3** present_timing（独立批） | §4.2 全清单：探测→flag→队列→pNext→collect→IPD/丢帧进 trace | 新 `present_timing.cppm`、`vkswapchain.cppm`、`vkrenderer.cppm` present、`vkdevice.cppm`、`features.cppm` | RADV ≥5min 无 crash、IPD ≈ 刷新周期；跨机 NVIDIA/Intel 补测录档 |
| **9.4** 判定收口 | CPU vs GPU vs IPD 三方对比结论 → `next_roadmap.md` ⑤ 决策建议成文 | 文档 | cull 读回成本、帧率上限/present mode 结论成文 |

模块规范走 skill `rgl-new-module`（impl partition / rlstd / FILE_SET / 检查红线），
验证走 skill `rgl-verify`（headless 冒烟 + BIT-IDENTICAL 回归）。
本方案**不碰 shader 契约**（无 DrawArgs/scene.slang 改动，`just reflect` 不涉）。

## 8. 验收红线（汇总）

1. 默认关 = BIT-IDENTICAL 截图回归 + `just checks` 过（9.1 起每批都要过）；
2. 9.2 开着跑回归**也 BIT-IDENTICAL**（时间戳不改像素）；
3. 9.3：RADV ≥5min 无 crash、IPD 合理（≈ 刷新周期）、queue-full 回退路径实测触发过
   （无限帧率下天然触发）；
4. 报告三件套齐：trace 进 Perfetto 可读 / 摘要判定行 / `profiler_stats.json` 合法 JSON；
5. 文档回写：`next_roadmap.md` Phase 9 状态、`nvidia_compatibility_report.md` 跨机
   present_timing 结果、archive `present_timing_research.md` 状态头补"已引入（仅测量）"、
   本文件转 archive。

## 9. 决策记录（2026-09-27，用户确认）

| # | 决策 | 结论 |
|---|---|---|
| D1 | present_timing 归属 | **独立批 9.3**（核心 = 9.1/9.2，CPU vs GPU 判定不依赖它；改动面集中在 swapchain 生命周期，风险隔离；先 probe 再定深浅） |
| D2 | 报告主格式 | **Perfetto（Chrome Trace Event JSON）为主** + rllogger 摘要 + 机器可读 JSON |
| D3 | 开关形态 | **`diagnostics.profiler = N` 帧窗口**（0=关默认，退出 dump）；不做 `#ifdef` 双层 |
| D4 | `source_location` 路径问题 | 原结论"最小宏"→ **翻案为零宏（2026-09-27 当日）**：实测 ① 编译规则传绝对路径（`build.ninja:793`）→ 绝对路径本来就有；② named module 不导出宏（clang 22 实测 + `rlvk.cppm:12` 先例）→ 宏无处安放。定稿 = `RGL::zone(label?)` 函数 + `source_location` 默认实参，调用形态 `auto z = RGL::zone("staging");` |
| — | 否决项 | per-entity profiler component（ECS 规范 1/4）；编译器级 `-finstrument-functions`；引入 Tracy 等第三方库 |
