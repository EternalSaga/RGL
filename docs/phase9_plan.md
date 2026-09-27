# Phase 9 实施计划：性能观测（Profiler）

> **状态**：现行——**批 9.1/9.2/9.3a/9.3b/9.4 全部落地**（2026-09-27，判定见 §10，
> 本计划转 archive 的唯一前置 = 跨机复测未闭环，见 §11）。起草时三决策见 §9；
> 实施期偏差与勘误集中在 §3.4 / §4.4。
> 依据 = [next_roadmap.md](next_roadmap.md) Phase 9 ①–⑤（:184-194）+ 2026-09-27 方案讨论
> （CPU/GPU/present_timing 2.5 问 + ECS/AOP 取舍）。完成后本文件转 `archive/`，进度回写 next_roadmap。
> **阶段号消歧（2026-09-27）**：文中"Phase 10 阴影"现指 **Phase 11**——Phase 10 已被调试交互
> （[phase10_plan.md](archive/phase10_plan.md)）接管，正文历史指称不改。

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

1. query pool 按帧槽分段（`kFramesInFlight=2`，`frame_session` 双 fence 已保证语义），
   槽内**双段不相交**：主段 [0,48)（gpu.frame+pass）、cull 段 [48,64)（cull）。
   **复位走 CB 内 `vkCmdResetQueryPool`，否决 host 版**——host 版需 `hostQueryReset`
   特性（spec 可选，本机 RADV 未启用即 VVL 报错，实施期真跑踩过）：主段复位记主 CB 头、
   cull 段复位记 cull CB 头——cull CB 先于主 CB 执行（同帧 fence 等待），段不相交
   才互不抹除；各自复位先于自身时间戳记录，亦规避 VVL 跨 CB 记录序限制；
2. 录制期每 pass 首尾各一次 `vkCmdWriteTimestamp2`（统一 ALL_COMMANDS 收口）+
   帧首末（GPU 总跨度）+ cull 区域（cull CB 内配对）；
3. **槽复用时（fence 已信号 → 滞后 2 帧）两段分别 `vkGetQueryPoolResults`（64-bit，
   不带 WAIT，不加 stall）——只读各段实际写入部分**：复位后未写入的查询为"不可用"，
   扩读到池末端必然 `VK_NOT_READY`（实施期真跑踩过）；`timestampValidBits=64` 无需
   掩码；滞后 2 帧 → 诊断窗口需 `diagnostics.profiler ≥ 3`（否则记录计 orphan drops）；
4. 同步顺手 `vkGetCalibratedTimestampsEXT{DEVICE_EXT, CLOCK_MONOTONIC_EXT}` →
   仿射换算（`timestampPeriod=10.019ns/tick`），**maxDeviation 记进报告**。

**否决的扩展**：`VK_EXT_performance_query`（多 pass 采集 + `vkAcquireProfilingLockKHR`
+ devtools 特化，过重）、`VK_EXT_device_fault`（崩溃诊断非计时）。

**开销自检**：时间戳 ≈ 亚微秒/次，个别驱动有序列化效应——9.2 验收时开着/关着各跑
`frame_stats` 对比一遍，差值记入文档。
→ **实施实测**（headless llm_debug，8 帧小窗口、capture 帧 ~240ms 主导噪声）：OFF
（9.1 基线）avg=34.56ms / p95=247.90ms；ON（9.2 终版）avg=37.74ms / p95=251.63ms
——差值落在噪声与 capture 主导内，8 帧样本不足定量；GPU 侧区域自洽：gpu.frame
2.656ms ≈ opaque 2.574 + skybox 0.052 + transparent 0.001 + tonemap 0.019，
cull 14µs，校准 maxDeviation 2.4µs。定量开销留真实窗口批补测。

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

### 4.4 实施记录（2026-09-27，批 9.3a + 9.3b 落地，本机 RADV 实测）

**结构**：新 `present_timing.cppm`（接口 = PresentTiming 类 + PresentTimingStats）+
`present_timing_impl.cppm`（重函数体）。**FrameSession 成员直持**（不像 9.2 走 bind 全局——
唯一消费者就是帧会话 present()）。§191 改动面表的 `vkrenderer.cppm` present 行号已失效
（present 早迁至 `frame_session_impl.cppm::present()`），接线点即在那里。

**三层激活门**：设备组门 `features.cppm::presentTimingSupported`（present_timing +
present_id2 + KHR 校准三探测位全真，缺一整组不启用/不链接；feature 结构走 `optional_features`
条件链尾机制——**不可进固定链**：FALSE 结构挂未启用扩展 = UB，9.2 已踩，见
`features_generated.cppm` 头注释）∧ `diagnostics().profiler > 0`（默认关 = 交换链创建参数
逐位同旧版）∧ surface/模式能力探测（`vkGetPhysicalDeviceSurfaceCapabilities2KHR` 以
`VkSurfacePresentModeKHR` 过滤 + `VkPresentTimingSurfaceCapabilitiesEXT`）。
三行激活 log（probe 结果 / active+容量+掩码 / 基准 stage）= 跨驱动取证载体。
swapchain-local 域缺失时退 `present-stage-local`（Mesa 26.2 只 advertise 后者）。

**与 §4.2 计划清单的三处偏差（均由实测 VVL/驱动行为决定）**：

1. **flag 门槛不看 `VkSwapchainFlagsSurfaceCapabilitiesEXT.swapchainSupportedFlags`**：该结构的
   flags-subset VUID-12448 只在启用 `VK_EXT_multisampled_render_to_swapchain` 时生效（本项目
   恒不启用），而 RADV 对它返 0——按它筛会把明明支持的 present timing 误判为不支持。
   门槛改为 `presentTimingSupported` + stage 掩码含 `QUEUE_OPERATIONS_END`。
2. **挂 `VkPresentId2KHR` 要求 swapchain 创建带 `VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR`**
   （VVL 实测报错）→ 激活时与 present timing flag 同置；两 feature 已在组门内启用。
3. **报告侧不做 queue→pixel 跨度**：stage-local 域下本机实测 `QueueOperationsEnd` 与
   `ImageFirstPixelOut` 历元相差 ~10 s（首条结果 log 为证），跨 stage 相减无意义；改为基准
   stage **瞬时落位**（`present.stage`，tid=present，dur=0），与 CPU/GPU 轨同 steady 基
   （实测首帧 pixel-out 落在首个 present CPU zone 后 ~5.9 ms，一帧周期内 ✓）。校准的
   `VkSwapchainCalibratedTimestampInfoEXT.presentStage` 随域类型定（stage-local 须恰一位，
   VU-12228）。同 stage 相邻差（IPD）不受影响：min 恰 = refreshDuration（6.062 vs 6.061 ms）。

**queue-full 回退（必做项）实测**：`vk::Queue::presentKHR` 允许结果列表不含
`VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`，命中即抛 `PresentTimingQueueFullEXTError` →
帧会话 `presentKHRTolerantQueueFull` 折回 Result 再回退。回退 = 同一 image 重投，但
**pNext 清空（timing 链与 present fence 都摘）+ wait 信号量清零**：spec 述被拒 present 的
队列操作仍视为已入队（wait 照常执行、present fence 已 SIGNALED——VVL 两条报错分别印证
"再提交已 signaled fence"非法与同队列 wait-then-signal 死锁形态），故两者均不可重复提交；
重投排在已入队 wait 之后 ⇒ 图像必已渲染完成；VVL 对"重投同一 image"不报警。随后 8 帧冷却
不挂链。验证：临时把队列容量压到 1 强制触发（FIFO 2 次 / eImmediate 多次），恢复后零错误。

**collect 节奏**：每帧 present 前轮询（`flags = 0` = 全有或无 + 按 present 序），结果缓冲
32 项 × 4 stage；每轮重钉 `presentStageCount` 容量（VU-12231）；驱动侧
`timingPropertiesCounter` / `timeDomainsCounter` 变化 → 重解析刷新属性/时间域并重校准。

**报告落位**：counter = `present.ipd` / `present.drop` / `present.queue_full` /
`present.refresh_ns`（profiler 新增 `profilerPushCounter` + `FrameSlot.counters` + trace
`ph:"C"` + stats `counters` 段 + 摘要行）；rllogger = 周期 2 s 一行 + 终值三行（含 IPD
p50/p95 与校准 maxDeviation）。§5 的 "stage 瞬时 `ph:"i"`" 落地为 dur=0 的 `ph:"X"`
（同轨同色、免再引第四种事件类型）。

**本机实测（Mesa 26.2.3 RADV STRIX_HALO / KWin Wayland；驱动报 refreshDuration=6.061 ms
≈ 165 Hz 面板；单实例、窗口 1280×720、forward 变体）**：

| 指标（各 25 s 单实例跑，eImmediate / FIFO 两行数字几乎重合） | eImmediate | FIFO |
|---|---|---|
| 帧墙钟 avg / p50 / p95 (ms) | 7.10 / 6.91 / 8.48 | 7.10 / 6.91 / 8.46 |
| CPU 录制主段 `drawFrame` mean / p95 (ms) | 7.03 / 8.40 | 7.03 / 8.38 |
| GPU 帧跨度 `gpu.frame` mean / p95 (ms) | 2.98 / 3.20 | 2.98 / 3.20 |
| CPU `acquire` mean (ms) | 0.05 | 0.04 |
| present 数 / IPD n / min / p50 / p95 (ms) | 1936 / 1934 / 6.063 / 6.96 / 8.36 | 2004 / 2002 / 6.059 / 6.97 / 8.28 |
| **丢帧（基准 stage 无时间戳）** | **0** | **0** |
| queueFull | 0 | 0 |

判定与结论见 §10（批 9.4）。**queue-full 回退**在正常容量（imageCount×2 = 6）下单实例不
自然触发（KWin 反馈仅滞后 1–2 帧），故临时把容量压到 1 强制触发多次 → 回退成功 + 8 帧冷却
后自动恢复，VVL 零报警（临时改动已撤销，路径本身成立）。**≥5 min 无 crash 红线**由那次
6.5 min 长跑满足（进程存活、零 [error]、零 VVL 报警）。

默认关（profiler=0）headless 与窗口 llm_debug 截图均 BIT-IDENTICAL（`10b00c78…`）；
headless + profiler>0 时探测 `presentTimingSupported=false` → 自动不激活、截图同基线。

> **⚠️ 数据勘误（同日自查，务必读）**：本节先前记录的两条长跑结论
> ① "eImmediate 下 85% 的 present 从未到达像素 stage（= ⑤ 帧率上限的直接证据）"、
> ② "FIFO 的 IPD p50 = 6.066 ms 恰 = 刷新周期"，**均作废**——那两次跑是我在 19:57:00 与
> 19:57:05 前后脚起了**两个实例**，两窗口互相遮挡、同时合成，测量环境被污染（副作用 =
> 呈现路径被拖成合成拷贝态、两进程帧率对半压）。单实例复跑 = 上表：drop 0、两模式分布
> 一致。教训（长跑必须单实例 + 关窗即作废当次数据）已入 `docs/调试历史.md`
> "并行双实例污染测量"条目。

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
| **9.2** GPU 时间戳 | query pool 分槽、pass 循环/帧首末/cull 插桩、calibrated 校准、同轴合并 | 新 `gpu_profiler.cppm(/_impl)`、`vkrenderer_impl.cppm` frameBegin/pass 钩子、`cull_driver_impl.cppm` cullCB 钩子、`main_impl.cppm` 构造/绑定、`features_config` `optional_extensions` 校准扩展探针 | **开跑也 BIT-IDENTICAL**；读回零 stall；开销自检（`frame_stats` 前后差记档） |
| **9.3** present_timing（独立批）**✅ 本机落地 2026-09-27** | §4.2 全清单 + 9.3a `present_mode` 双模式开关（D5）；偏差与实测见 §4.4 | 新 `present_timing.cppm(/_impl)`（FrameSession 成员）、`vkswapchain.cppm`（探测/flag/队列/recreate）、`frame_session_impl.cppm` present（collect/attach/queue-full 回退）、`vkdevice.cppm`（两 optional feature 结构）、`features.cppm` + `features_config.json`（组门三探针，`just reflect`）、`profiler.cppm(/_impl)`（counter 通道）、`rlvk.cppm`（导出面） | RADV ≥5min 无 crash、IPD ≈ 刷新周期、queue-full 回退实测触发 ✓；跨机 NVIDIA/Intel 补测录档（待有卡机器） |
| **9.4** 判定收口 **✅ 2026-09-27** | 三方对比成文（本文件 §10）+ profiler `logBoundVerdict` 判定行 + stats `present_ms` 段 → `next_roadmap.md` ⑤ 结论与阴影基线 | `profiler_impl.cppm`（bound 行 / counters 分位数）、`phase9_plan.md` §10、`next_roadmap.md` | CPU-bound 判定成文、④c 否决读回延后、⑤ 判定"当前不需要帧率上限"、GPU 余量 3.1 ms 交给 Phase 10 |

模块规范走 skill `rgl-new-module`（impl partition / rlstd / FILE_SET / 检查红线），
验证走 skill `rgl-verify`（headless 冒烟 + BIT-IDENTICAL 回归）。
本方案**不碰 shader 契约**（无 DrawArgs/scene.slang 改动，`just reflect` 不涉）。

## 8. 验收红线（汇总）

1. 默认关 = BIT-IDENTICAL 截图回归 + `just checks` 过（9.1 起每批都要过）；
2. 9.2 开着跑回归**也 BIT-IDENTICAL**（时间戳不改像素）；
3. 9.3：RADV ≥5min 无 crash、IPD 合理（≈ 刷新周期）、queue-full 回退路径实测触发过
   （无限帧率下天然触发）——**✅ 2026-09-27 达成**（单实例无 crash、零 [error]、零 VVL 报警，
含一次 6.5 min 长跑；queue-full 本机不天然触发，改用临时压队列容量到 1 强制触发并验证
自动恢复；数据表与**一条作废结论的勘误**见 §4.4）；
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
| D5 | present_mode 双模式（**用户 2026-09-27 追加，折入 9.3**） | `configs/runtime.lua` 加 `present_mode = "immediate"|"fifo"`（默认 immediate = 现状 → BIT-IDENTICAL），窗口路径按偏好选模式（`vkswapchain::createPresentMode` 分支），headless 恒 eFifo 忽略此键。**修订** archive `present_timing_research.md` 2026-09-01 决策"不加开关"与本 plan 原"可配置化必要性留给 9.4 判断"——9.4 结论需要 FIFO vs eImmediate 双模式对比证据，开关即为其前置（改动面小：配置键 + 选择分支） |
| — | 否决项 | per-entity profiler component（ECS 规范 1/4）；编译器级 `-finstrument-functions`；引入 Tracy 等第三方库 |

## 10. 批 9.4 判定收口（2026-09-27 成文；数据 = §4.4 单实例复跑表）

三方对齐 = **帧墙钟 vs CPU 录制主段（9.1）vs GPU 帧跨度（9.2）**，present 侧（9.3）用显示
节奏与丢帧计数做交叉验证。判定行已落进 profiler 摘要（`logBoundVerdict`，补上 §7 批次表
里"bound 判定行随 9.2 GPU 数据落地"这笔欠账）：

```
bound: frame=7.10ms cpu.record=7.03ms gpu.frame=2.98ms -> CPU-bound (余量 4.05ms)
```

**结论 1｜当前工作负载 = CPU-bound**（forward 变体、1280×720 窗口、本机 Strix Halo）。
CPU 录制主段 7.03 ms ≈ 帧墙钟 7.10 ms（99%），GPU 帧跨度 2.98 ms 只占 42% 帧预算。
Phase 9 的原始问题"CPU-bound 还是 GPU-bound"就此收口：**CPU 侧**。

**结论 2｜④c（per-frame cull 读回成本）不显著，否决"读回延后一帧"**。判据：GPU `cull`
区域 14 µs（帧预算 0.2%）；CPU `fenceWait` 5 µs、`submit` 37 µs，无 stall 迹象。
归因边界须写明：`drawFrame` 内部未插桩，cull 编舞的 CPU 成本落在结论 4 的未归因段里；
但"延后一帧读回"并不减少 CPU 总工作量，而 CPU 总量本身已是瓶颈 → 无收益，否决。

**结论 3｜⑤ 帧率上限 / present mode 可配置化：当前不需要**。两条独立证据：① 单实例下
eImmediate 与 FIFO 的帧墙钟/IPD 分布在噪声内重合（avg 7.10/7.10，p50 6.91/6.91）；
② 两模式 `drop` 计数均为 **0**，即没有 present 拿不到像素 stage 时间戳。根因 = CPU-bound
把帧率天然压到 ≈141 fps **低于** 165 Hz 刷新率，渲染的每一帧都被显示，不存在"满速渲被
合成器丢弃"的浪费（archive §八 在 Intel Arc A310 上看到的 100% 占用现象在本机不成立——
那台是 GPU-bound 且慢于刷新率的情形，两种形态不可混推）。
`present_mode` 开关（9.3a）**保留**，用途从"当前调优手段"转为"负载形态改变后的复测入口"：
Phase 10 阴影若把成本推向 GPU 侧、使 `gpu.frame` 反超大过 `cpu.record`，FIFO 的 acquire
背压才成为有意义的话题。

**结论 4｜真正的优化头寸在 CPU 录制段，而它当前不可归因**。已命名 CPU 子段合计仅 ≈0.4 ms
（present 0.21 + renderlist 0.07 + acquire 0.05 + submit 0.04 + 其余 µs 级），
**`drawFrame` 7.03 ms 的 ~93% 落在没有子 zone 的区段**（固定数据通路写入 / pass 录制 /
SlotResolver / executePassDraws / cull 编舞 / descriptor heap 写）。故 Phase 10 之前若要提
帧率，第一步是往 `drawFrame` 内部补子 zone（纯插桩、零渲染改动），而不是先动 GPU 侧或先动
cull。此项列为**批 9.5（可选，插桩-only）**。

**结论 5｜给 Phase 10 阴影的基线数值**（= `next_roadmap.md` 里"X 由 Phase 9 profiler 定基线"
的答案）：GPU 帧跨度 **2.98 ms**（其中 `opaque` 2.92 ms = 98%，其余 pass 合计 <0.1 ms），
对 165 Hz（6.061 ms 预算）的 **GPU 侧余量 ≈ 3.1 ms/帧**；CPU 侧无余量（占满 99%）。
即阴影的 GPU 成本若 ≲3 ms/帧，整机帧时不变（仍 CPU-bound）——这是 cascade 数/分辨率选型的
硬上界输入；而阴影带来的**额外 CPU 录制成本**（多 pass 多次录制）没有余量，需与结论 4 一起考虑。

**未收口项（不阻塞 Phase 10）**：① 跨机复测（NVIDIA / Intel anv，判读口径见
`nvidia_compatibility_report.md` §九）；② `drawFrame` 内部细分（结论 4 / 批 9.5）；
③ stage-local 域下跨 stage 历元不一致的根因（Mesa 侧；本项目已用"不做跨 stage 跨度"规避）；
④ 端到端延迟（输入 → 像素）仍需支持 swapchain-local 域或直显路径的机器才能测。

## 11. 本计划终态

- 批 9.1 ✅ `dc2ab50`｜9.2 ✅ `3b1f9c0`｜9.3a ✅ `bdaa097`｜9.3b ✅ `1d0fbbe`｜
  9.4 ✅ 本节（+ profiler `logBoundVerdict` 与 stats `present_ms` 段）。
- 按 §8 红线 5 本文可转 archive；**唯一未闭环 = 跨机复测**（§4.3 风险表末行），故暂留
  `docs/phase9_plan.md`，状态头标注"待跨机复测后转 archive"。
- ②（gprof / perf 函数级深挖）全程未做，判定不依赖它；将来要 CPU 细分时优先走结论 4 的
  补 zone 路线（零第三方依赖、与既有 trace 同轴），gprof/perf 作后备。
