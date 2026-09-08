# VK_EXT_present_timing 扩展研究记录

> **状态**：调研（已归档 2026-09-08；决策"暂不引入"，待真实帧 pacing 需求出现时按第六节步骤执行）

**日期**: 2026-08-22
**环境**: Gentoo, Vulkan 头文件 1.4.341 (系统), loader 1.4.341, RADV ICD 1.4.354 / NVIDIA 610.57.04
**结论**: 技术上可引入（API 已在本地头文件/loader 就绪，`vulkan.hpp` wrapper 齐备），但**当前收益以测量价值为主**。驱动运行时支持未验证、项目硬性要求 eImmediate（该扩展的调度功能仅 FIFO 有效）。**决策：暂不引入**，待驱动支持确认或出现真实帧 pacing 需求时，按"仅测量"方式引入（见第六节步骤）。

---

## 一、扩展概况

| 项目 | 值 |
|---|---|
| 名称 | `VK_EXT_present_timing`（扩展号 209，device 扩展） |
| 规范版本 | 3（Last Modified 2024-10-09，已 ratified） |
| 主导 | NVIDIA（Lionel Duc）；参与者含 AMD、Intel、Google、Collabora、Red Hat |
| 定位 | `VK_GOOGLE_display_timing` 的"宽厂商"继任者：更细粒度（present stages）、更严格的 spec 语言，目标是全厂商可实现 |
| 依赖扩展 | `VK_KHR_swapchain` + `VK_KHR_present_id2` + `VK_KHR_get_surface_capabilities2` + `VK_KHR_calibrated_timestamps` |

解决的核心问题：渲染与呈送解耦后，应用无法对齐"几何摆位时刻"与"像素可见时刻"，产生卡顿（stutter）和输入延迟。扩展提供三件事：

1. **显示刷新属性** — 刷新周期时长、FRR vs VRR 判定、动态变化检测；
2. **每次 present 的分阶段时间戳** — 4 个 present stage，异步反馈（数帧延迟）；
3. **按目标时间调度 present** — 绝对/相对时间指定"最早可见时刻"，仅 FIFO 系列 present mode 有效。

### 设备 feature（`VkPhysicalDevicePresentTimingFeaturesEXT`）

| feature | 含义 |
|---|---|
| `presentTiming` | 扩展被暴露时**必须**支持；使能 `vkGetPastPresentationTimingEXT` 等查询 |
| `presentAtAbsoluteTime` | 允许用绝对时间（指定时间域内）调度 present |
| `presentAtRelativeTime` | 允许用相对时间调度（前一张图至少可见多久） |

### Surface 能力（`VkPresentTimingSurfaceCapabilitiesEXT`，链入 `VkSurfaceCapabilities2KHR` pNext）

`presentTimingSupported` / `presentAtAbsoluteTimeSupported` / `presentAtRelativeTimeSupported` / `presentStageQueries`（该 surface 可查询的 stage 位掩码）。

### Present stages（`VkPresentStageFlagBitsEXT`）

| stage | 含义 |
|---|---|
| `VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT` | `vkQueuePresentKHR` 提交的队列操作结束（如 blit 到系统内部 surface）；`presentTimingSupported` 时**必须**支持 |
| `VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT` | 请求按当前 present mode 出队 |
| `VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT` | 首个像素的数据离开呈现引擎、进入显示硬件 |
| `VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT` | 显示硬件使首个像素可见 |

### 时间域

两个新的 swapchain 局部时间域（单位 ns，仅同一 swapchain 内可比）：

- `VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT`（必须支持）— 特定 swapchain + 特定 stage 独有；
- `VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT` — 该 swapchain 所有 stage 共享。

每个时间域有实现分配的 opaque id（`vkGetSwapchainTimeDomainPropertiesEXT` 返回）。要与其他时间域对齐（如 device 时间域 / `CLOCK_MONOTONIC`），把 `VkSwapchainCalibratedTimestampInfoEXT{swapchain, presentStage, timeDomainId}` 链入 `VkCalibratedTimestampInfoKHR::pNext` 走 `vkGetCalibratedTimestampsKHR`（VUID-12227：stage/swapchain 局部域必须带该结构；VUID-12228：stage 局部域 presentStage 必须恰好一个 bit）。

---

## 二、API 表面（本地头文件 vulkan_core.h:17283-17410）

### Swapchain 使能

`VkSwapchainCreateInfoKHR::flags` 增加 `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT`（bit 9）。

**关键探测路径**：该 flag 是否可用与 (surface, present mode) 组合相关。`VK_EXT_swapchain_maintenance1`（本项目已启用）的 `VkSwapchainFlagsSurfaceCapabilitiesEXT::swapchainSupportedFlags` 可回答"该 surface 在指定 present mode 下支持哪些 swapchain create flag"——查询时把 `VkSurfacePresentModeKHR{presentMode}` 链入 `VkPhysicalDeviceSurfaceInfo2KHR::pNext` 再调 `vkGetSurfaceCapabilities2KHR` 即可。

### 四个新入口点（`vulkan.hpp` Device 方法均已生成）

| 入口点 | 作用 | 返回 |
|---|---|---|
| `vkSetSwapchainPresentTimingQueueSizeEXT(device, swapchain, size)` | 设定内部结果队列容量（实现需据此分配资源）；可重复调用（重分配，放不下未完成结果时返回 `VK_NOT_READY`） | `VK_SUCCESS`/`VK_NOT_READY` |
| `vkGetSwapchainTimingPropertiesEXT(device, swapchain, &props, &counter)` | `refreshDuration`（ns）+ `refreshInterval`（ns）+ 变化计数器 | `VK_SUCCESS`/`VK_NOT_READY`（部分平台首次 present 后才可查） |
| `vkGetSwapchainTimeDomainPropertiesEXT(device, swapchain, &domains, &counter)` | 时间域列表 + opaque id 数组（先 count 后填充两次调用）+ 变化计数器 | `VK_SUCCESS`/`VK_INCOMPLETE` |
| `vkGetPastPresentationTimingEXT(device, &info, &props)` | 异步收集历史 present 的分阶段时间戳；释放已完成结果的队列 slot | `VK_SUCCESS`/`VK_INCOMPLETE` |

### 刷新属性语义（`VkSwapchainTimingPropertiesEXT`）

- `refreshDuration == 0`：平台暂时无法提供（常见于首次 present 前）；
- `refreshInterval == UINT64_MAX`：**VRR** 模式，`refreshDuration` 为最小刷新周期；
- `refreshInterval == refreshDuration`：**FRR** 模式；
- 属性可在运行中动态变化（省电降频、窗口化→全屏、24Hz 视频覆盖等），`counter` 单调递增用于检测，`vkGetPastPresentationTimingEXT` 的返回值也带回两个 counter。

### 每次 present：`VkPresentInfoKHR` pNext 链

```
VkPresentTimingsInfoEXT { swapchainCount, pTimingInfos }   // 与 PresentInfo.swapchainCount 一一对应
  └─ VkPresentTimingInfoEXT {
       flags,                  // PRESENT_AT_RELATIVE_TIME / PRESENT_AT_NEAREST_REFRESH_CYCLE
       targetTime,             // 绝对时刻或相对时长（ns，按 flags 解释）；0 = 尽快
       timeDomainId,           // 解释 targetTime 并收集结果的 opaque 时间域 id
       presentStageQueries,    // 要收集哪些 stage
       targetTimeDomainPresentStage  // stage 局部时间域对应哪个 stage
     }
```

- `PRESENT_AT_RELATIVE_TIME`：`targetTime` 解释为"前一张图至少可见 ns"；
- `PRESENT_AT_NEAREST_REFRESH_CYCLE`：允许对齐到**早于** targetTime 开始的最近一个刷新周期（实现目标是把 `IMAGE_FIRST_PIXEL_VISIBLE` 对齐到 targetTime）；
- 帧关联：`VkPresentId2KHR::presentId` 加入同一 pNext 链，结果里原样带回。

### 结果结构（`vkGetPastPresentationTimingEXT`）

```
VkPastPresentationTimingPropertiesEXT {
  timingPropertiesCounter,   // 刷新属性 counter
  timeDomainsCounter,        // 时间域列表 counter
  presentationTimingCount,   // 本次返回条数（pPresentationTimings=NULL 时查询待处理数量）
  pPresentationTimings[]     // 每条一个 VkPastPresentationTimingEXT
}
VkPastPresentationTimingEXT {
  presentId,                 // 关联到 VkPresentId2KHR
  targetTime,
  presentStageCount,         // 有确定结果的 stage 数
  pPresentStages[] { stage, time },   // 时间 0 = 该 stage 未到达/不可用（如请求被丢弃）
  timeDomain, timeDomainId,  // 可能与请求时不同（原时间域失效时的 fallback）
  reportComplete             // 全部 stage 报齐；此后不再重复返回，队列 slot 释放
}
```

输入 flags：`VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT`（多 stage 允许部分结果先回）、`VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT`（允许乱序）。

**队列满行为**：带非零 `presentStageQueries` 的 present 占一个队列 slot；队列满时 `vkQueuePresentKHR` 返回新错误码 `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`（vulkan_core.h:180）。应对：轮询排空、扩大队列、或本次 present 不带 stage 查询（不带查询则永远不占 slot）。

**反馈延迟**：无 fence/wait API，只能轮询；典型延迟数帧（早期 Android 平台 ~5 个刷新周期）。队列容量建议取 swapchain image count 的数倍。

---

## 三、与当前项目的交互分析

### 3.1 eImmediate 冲突（核心问题）

`selectPresentMode()`（utils.cppm:163-171）**硬性要求** `VK_PRESENT_MODE_IMMEDIATE_KHR`，不可用即抛异常。而：

- **目标时间调度仅对 FIFO 系列有效**（`FIFO` / `FIFO_RELAXED` / `FIFO_LATEST_READY`）——eImmediate 下 `targetTime` 无意义；
- **stage 时间戳查询在任意 present mode 下均允许**，但 eImmediate 下部分 stage 可能永远不可达（`REQUEST_DEQUEUED` 无 FIFO 队列可言；窗口 + 合成器环境下 `IMAGE_FIRST_PIXEL_OUT/VISIBLE` 可能为 0）。唯一保证可查的是 `QUEUE_OPERATIONS_END`。

所以 eImmediate 下该扩展只剩"GPU 侧排队耗时 + 平台允许范围内的可见性时间"测量价值；要拿到完整的"渲染→可见"链路和调度能力，需要切 FIFO 模式（或让 present mode 可配置）。

### 3.2 运行时探测顺序（引入时的判断链）

1. 设备是否暴露 `VK_EXT_present_timing`（及其 3 个 KHR 依赖）→ 否：整体 no-op；
2. `vkGetPhysicalDeviceFeatures2` + `VkPhysicalDevicePresentTimingFeaturesEXT` → `presentTiming` 必须为真；
3. `vkGetSurfaceCapabilities2KHR`（pNext：`VkSurfacePresentModeKHR{当前 mode}` + `VkSwapchainFlagsSurfaceCapabilitiesEXT` + `VkPresentTimingSurfaceCapabilitiesEXT`）→ 当前 present mode 下 `swapchainSupportedFlags` 是否含 `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT`、`presentStageQueries` 有哪些 stage；
4. swapchain 创建后 `vkSetSwapchainPresentTimingQueueSizeEXT` + 查询时间域；刷新属性可能首次 present 前返回 `VK_NOT_READY`，需重试。

### 3.3 本地环境状态（2026-08-22 核查）

| 项 | 状态 |
|---|---|
| 系统头文件 `/usr/include/vulkan`（VK_HEADER_VERSION 341） | ✅ 扩展完整定义（vulkan_core.h:17283-17410；VkTimeDomainKHR 新值 @13463-13464） |
| C++ wrapper `vulkan.hpp` | ✅ 结构体 + Device 方法齐备（vulkan.hpp:5372-5390） |
| loader `libvulkan.so.1.4.341` | ✅ 版本 ≥ 扩展注册版本，entry point 可解析 |
| RADV（ICD manifest 1.4.354）运行时支持 | ❓ 未验证（本机无 vulkaninfo；计划模式未跑程序） |
| NVIDIA 610.57.04 运行时支持 | ❓ 未验证（同上） |
| VVL 校验覆盖 | ❓ Vulkan-ValidationLayers main 分支未见 PresentTiming 相关校验文件（2026-08 检索），新扩展校验覆盖大概率薄，引入后需留意 |

### 3.4 集成点清单（若引入，预计改动面）

| 文件 | 改动 | 备注 |
|---|---|---|
| 新 `present_timing.cppm` | `PresentTiming` 模块：支持探测、时间域/刷新属性缓存、结果 ring buffer、`collect()`、周期统计 | 模块优先规范；`import rlstd/rllogger` |
| `vkswapchain.cppm` | caps2 探测（3.2 步骤 3）；支持时 `swapchainCreateInfo.flags |= ...`（当前 flags 未设置）；创建后设队列容量；`recreate()`（:46-68）换新 swapchain 后需重设队列容量 + 重查时间域 | 成员生命周期规则：探测结果结构体做类成员 |
| `vkrenderer.cppm:261-283` | pNext 链改造：`setPNext` 只接受单指针，需手工串 `VkPresentTimingsInfoEXT → (VkPresentId2KHR) → SwapchainPresentFenceInfoEXT`；处理 `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`（本次不带 stage 查询重试 present + 短暂退避）；每帧 `collect()`；周期日志 | 局部变量即可（仅调用期间存活） |
| `vkdevice.cppm` | `DeviceCreateInfo::pNext` 当前指向 `featuresChain`（`PhysicalDeviceFeatures2` 头部）；扩展可用时在链尾（`DescriptorBufferFeaturesEXT`，config 列表最后一项）追加 `VkPhysicalDevicePresentTimingFeaturesEXT{presentTiming=TRUE}`，**必须做类成员**（pNext 不能悬空，同 featuresChain_ 规则） | 不动 codegen：生成的 `features_generated.cppm` 是无条件 TRUE 的固定链，直接加会破坏不支持该扩展的驱动 |
| `features.cppm:16-59` | 两个扩展列表：`VK_EXT_present_timing` + `VK_KHR_present_id2` + `VK_KHR_calibrated_timestamps` + `VK_KHR_get_surface_capabilities2` 走**可选**清单（全部可用才启用，缺一即整体跳过 + 日志提示），不进现有 required 清单 | 保双 GPU 运行不破坏 |

`MAX_FRAMES_IN_FLIGHT = 2`、unlimited FPS 循环（ecs.cppm:133 `while(true)`，无 pacing）意味着结果队列容量取小值（如 16）+ queue-full 回退路径是必须的。

---

## 四、潜在益处

1. **eImmediate 链路延迟实测**：`QUEUE_OPERATIONS_END`（GPU/驱动侧排队结束）→ 可见 stage 的差值，量化"present 调用 → 像素可见"的整段延迟；RADV 核显原生 vs NVIDIA prime offload 两条路径直接对比，是 `docs/nvidia_compatibility_report.md` 缺的时序维度。
2. **卡顿/丢帧诊断**：unlimited FPS + eImmediate 下实际可见间隔 = 刷新周期，帧的"实际 IPD"分布（first-visible 时间戳差分）可直接暴露 judder；dropped 请求（stage time 全 0）计数暴露丢帧。
3. **刷新率感知**：FRR/VRR 判定 + 刷新周期动态变化检测（monitor Hz 切换、24Hz 视频覆盖、省电降频），对 offload 场景排障有价值。
4. **未来帧 pacing 基础**：present mode 可配置后，FIFO + `targetTime`（nearest refresh cycle）可做平滑动画 + 受控延迟——路线图 Phase 2+ 模型增多后动画质量需求会上升。
5. **端到端延迟测量**：时间域校准（device 域 ↔ swapchain 局部域）后，`vkCmdWriteTimestamp`（渲染完成）→ first-visible 即全链路延迟，对后续多 pass/compute 工作有意义。
6. **学习价值**：新的 WSI/present 交互模式（异步反馈、opaque 时间域、per-mode 能力探测），与项目学习 modern Vulkan 的定位一致。

## 五、风险与成本

- **扩展年轻**：spec v3（2024-10 ratified），本地头文件 341 才带全；两家驱动的实际行为需实测，VVL 校验覆盖薄；
- **反馈延迟数帧 + 仅轮询**：无同步等待 API，设计必须容忍延迟与乱序/部分结果；
- **queue-full 错误路径**：unlimited FPS 下高频 present 容易撞满小队列，必须有回退（不带查询重试/退避），否则 present 失败会破坏渲染循环；
- **eImmediate 下功能减半**：调度完全不可用；stage 查询也只有部分 stage 可靠——投入产出比取决于是否顺带做 present mode 可配置；
- **窗口 + 合成器环境**：`IMAGE_FIRST_PIXEL_OUT/VISIBLE` 可能拿不到（time=0），测量粒度受限；
- **生命周期负担**：`recreate()` 后重设队列容量/重查时间域；特性结构体必须做成员；
- **可选化设计**：双 GPU 支持（run-igpu/run-dgpu）要求驱动不支持时干净 no-op，探测链（3.2）不可省略。

## 六、未来引入步骤（如决定做）

| 步骤 | 内容 | 验收 |
|---|---|---|
| 0. 驱动探测 | 装 `vulkan-tools`（vulkaninfo）或写 20 行 probe 程序：列设备扩展 + 3.2 步骤 2/3 查询；**两张 GPU 都要跑**（`just run-igpu` / `run-dgpu` 同款环境变量） | 确认 RADV/NVIDIA 各自支持情况、eImmediate 下可用的 stage 集合 |
| 1. 仅测量（推荐范围） | 按 3.4 清单落地：新模块 + swapchain flag/队列容量 + drawFrame pNext 链 + 每帧 collect + rllogger 周期汇总（IPD 均值/p95、各 stage 延迟、丢帧计数、刷新属性变化）；可选写 `logs/present_timing.json` 供 python 绘图 | 两张 GPU 各跑 ≥5 分钟无 crash，日志数据合理（IPD ≈ 刷新周期） |
| 2. 帧 pacing（可选，后续） | config 增加 presentMode 选项（immediate/fifo）；FIFO 下启用 `targetTime` 调度（基于历史 first-visible 推算下一帧目标时间） | 对比 immediate vs fifo+调度 的卡顿指标 |

## 七、参考资料

- 注册表（权威 API 定义）：`KhronosGroup/Vulkan-Docs` 仓库 `xml/vk.xml`，扩展块 @ ~25170 行
- 设计文档（语义细节、示例、已决问题，目前最完整来源）：同仓库 `proposals/VK_EXT_present_timing.adoc`
- 扩展附录（元数据/贡献者/版本历史）：同仓库 `appendices/VK_EXT_present_timing.adoc`
- 注意：截至 2026-08，该扩展的 per-command 规范正文（VU 列表）尚未合入 Vulkan-Docs 仓库章节文件（仅在 `chapters/synchronization.adoc` 有 calibrated-timestamps 相关 ifdef 片段、`chapters/fundamentals.adoc` 有错误码描述），引用规范条文时以 registry + proposal 为准
- 本地头文件：`/usr/include/vulkan/vulkan_core.h:17283-17410`（扩展块）、`:13458-13468`（`VkTimeDomainKHR`）、`:180`（`VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`）、`:8955`（swapchain flag）

## 八、Wayland/KWin 下 eImmediate 与 GPU 占用实测（2026-09-01，Phase 5.5）

**现象**：Phase 5.5 默认场景（cottage 392 primitive + IBL + skybox）在 **Intel Arc A310**
（anv，Linux/Wayland/KWin）上运行时 GPU 占用 **100%**。

**结论：正常，与"KWin 强制垂直同步"不矛盾**——KWin 的 vsync 只卡**显示侧**，不卡**渲染侧**：

- Wayland 下 app 从不直接翻屏，只 commit buffer 给 compositor；KWin 每个 vblank 取最新
  buffer 合成（显示帧率 = 刷新率，vsync 生效），旧 buffer 被替换后**立即 release**；
- `selectPresentMode`（utils.cppm）优先选 `eImmediate`（mailbox）——anv 该模式下
  **acquire 不等 frame callback**，只要 image 被 release 就能继续全速提交；
- 于是渲染帧率无上限、KWin 丢弃多余帧：100% 占用 = GPU 在满速渲染将被丢弃的帧。
  这正是第三节所述 "unlimited FPS 循环（ecs.cppm `while(true)` 无 pacing）" 的直接后果。

**对照实验**（临时改 `selectPresentMode` 优先 `eFifo`，未提交）：占用 **100% → 72%**——
eFifo 让驱动在 acquire 处阻塞等 vblank，渲染被压回刷新率；72% 而非更低 = 该场景对 A310
（8 Xe core）本身不轻（392 mesh-shader draw + 11 张 4K 纹理 + IBL/skybox mipmap 采样 +
每帧 scene data 拷贝）。

**决策**：保持 `eImmediate` 现状（用户确认不加环境变量开关）。优化空间留档：
① 帧率上限（CPU pacing）；② present mode 可配置化（第六节步骤 2 的前半）；
③ `VK_EXT_present_timing` 的 `targetTime` 调度（需 FIFO 系，见 3.1）——与本文档
"待真实帧 pacing 需求出现时引入"的决策衔接：**若未来做动画质量/延迟优化，本节即触发条件**。
