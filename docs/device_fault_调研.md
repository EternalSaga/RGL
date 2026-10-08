# VK_EXT_device_fault / VK_KHR_device_fault 调研（device lost 诊断 + masked fault 盲区）

> **状态**：调研（**未实施**；集成路径与实验顺序见 §9）。本机能力实测 + spec 语义双源核实 + 归档三 device lost 案例推演；实施后本文转"现行"并补落地记录

**日期**：2026-09-28
**环境**：本机 AMD Radeon 8060S（RADV **STRIX_HALO**，RDNA 3.5），Mesa 26.2.3，device apiVersion 1.4.354；vulkaninfo/loader 1.4.357
**权威来源**：本地 spec clone `Vulkan-Docs` **v1.4.360**（`xml/vk.xml` + `chapters/features.adoc` + `chapters/debugging.adoc` + `proposals/VK_*_device_fault.adoc`）与系统头 `vulkan_core.h` **v357** 交叉核实；**结构体字段以系统头为准**（AGENTS.md 惯例，spec clone 仅语义依据）

⚠️ **换机欠账**：本机已换为 Strix Halo 8060S，`AGENTS.md` 与 `docs/nvidia_compatibility_report.md` 中"当前机 = 9950X Raphael RDNA2 核显"表述过期，待勘误。三驱动（RADV / NVIDIA / Intel）复测等换机（2026-08-27 老机器曾三卡：9950X iGPU + Arc A310 + RTX 5090，见 `docs/archive/heap_texture_research.md` 环境段）。

---

## 1. 背景与动机

RGL 现行策略 = **device lost 即死**：`rgl/frame_session_impl.cppm` 三处出口
（fence 等待超时 L90-105 / submit 返回 -4 L182-186 / present 返回 L273-276）
注释原文"DEVICE_LOST：GPU 挂起/复位，不可恢复——记录后退出帧循环"。该策略本身合理
（恢复代码 = 全代码库最不可测试的代码，见对话讨论），短板在**死得不明不白**：
fence 超时路径对"GPU 为什么停了"零信息。

归档（`docs/archive/`、`git:调试历史`）三个 device lost 案例：两个闭环但过程曲折
（依赖 root dmesg / 多日状态漂移分析 / 两轮误诊），一个**机理至今未闭环**
（NV DGC push 挂死，`git:调试历史` 明确留待复查）。本文回答：
**该扩展能不能、在多大程度上补上这个诊断盲区**。

`docs/vulkan_extensions_2025_调研.md` §L54/L126-127 已给出方向
（"补 GPUVM/Aftermath 诊断史"、"masked fault = 本项目 GPUVM 调试盲区"）；
本文把该结论落到**具体语义、本机实测能力、三案例推演、集成路径**四层。

## 2. 本机环境实测（2026-09-28，vulkaninfo）

| 项 | 实测值 | 备注 |
|---|---|---|
| GPU | AMD Radeon 8060S（RADV STRIX_HALO） | 单卡，非三驱动环境 |
| `VK_EXT_device_fault` | **advertised，rev 2** | EXT = 提升前别名 |
| `VK_KHR_device_fault` | **advertised，rev 1** | 1.4 提升后的规范名 |
| `deviceFault` | true | 基础故障报告 |
| `deviceFaultVendorBinary` | **false** | 无厂商二进制 crash dump |
| **`deviceFaultReportMasked`** | **true** | **关键能力：可报"驱动内部已恢复"的 masked fault** |
| `deviceFaultDeviceLostOnMasked` | false | 用途 = 把 masked 故障转成 device-lost（RGL 不需要此方向） |
| `maxDeviceFaultCount` | **1** | 故障队列深度 1（只留最近一条） |
| `VK_KHR_shader_abort` | advertised rev 1，`shaderAbort=true` | 见 §5.3 |
| `VK_KHR_shader_constant_data` | advertised rev 1 | shader_abort 的依赖扩展，齐备 |
| `maxShaderAbortMessageSize` | 65536（64KB） | 单条 abort 消息上限 |

## 3. 扩展语义（spec v1.4.360 + 系统头 v357 双源）

### 3.1 纠正一个预期：是纯查询式，不是"富版本"

初判误以为存在 fault type 枚举 + 设备回调 + makeup value——**系统头与 vk.xml 双双
确认均不存在**（那套属 VulkanSC `VK_KHR_fault_handling` 或早期草案）。实际 API 表面：

- `vkGetDeviceFaultReportsKHR(device, timeout_ns, pFaultCounts, pFaultInfo)`
  —— **任意时刻可调**（不限 device lost 后）；`timeout` 单位纳秒，**0 = 非阻塞取当前**，
  `>0` = 阻塞等故障出现（`chapters/debugging.adoc` timeout 语义段）
- `vkGetDeviceFaultDebugInfoKHR(device, pDebugInfo)` —— 厂商二进制 crash dump，
  **仅 device lost 后可调**（RADV 不支持，见 §4）
- 无 callback、无 fault type 枚举、无 makeup value

### 3.2 报告内容（`VkDeviceFaultInfoKHR`，系统头 L14448-14457）

| 字段 | 含义 |
|---|---|
| `flags` | `DEVICE_LOST(1)` / **`MEMORY_ADDRESS(2)`** / **`INSTRUCTION_ADDRESS(4)`** / `VENDOR(8)` / **`WATCHDOG_TIMEOUT(10)`** / `OVERFLOW(20)`（hex，`VkDeviceFaultFlagBitsKHR` L14411-14418） |
| `groupId` | 相关故障的分组 id（并行执行同缺陷代码 → 大概率同组，见 EXT proposal issue 1） |
| `description` | 256B 人类可读描述 |
| `faultAddressInfo` | `{addressType, reportedAddress(GPU VA), addressPrecision}` —— type = READ/WRITE/EXECUTE_INVALID、INSTRUCTION_POINTER_*（L14387-14403） |
| `instructionAddressInfo` | 故障时执行的指令地址（同结构） |
| `vendorInfo` | `{description, vendorFaultCode, vendorFaultData}`（`VkDeviceFaultVendorInfoKHR`） |

`OVERFLOW` 标志 = 队列溢出（`maxDeviceFaultCount` 限制，更早的故障记录丢失）。
`addressPrecision` 允许按硬件粒度计算地址区间
（`lower = reported & ~(precision-1)`，见 EXT proposal §Interpreting GPU Virtual Addresses）。

### 3.3 masked fault 语义（本机 reportMasked=true 的那半个能力）

`chapters/features.adoc` L8265-8278 原文要点：

- **masked fault** = 实现内部恢复、**未**向应用返回 `VK_ERROR_DEVICE_LOST` 的故障；
  `deviceFaultReportMasked` 使其可被本扩展报出
- 关键警告：**masked fault 发生时，"所有正在被写入的资源内容 undefined"**
  —— 即驱动替你兜底了设备存活，但那帧的写目标已经是垃圾
- `deviceFaultDeviceLostOnMasked` = 另一半：把本应 masked 的故障转成 device-lost 返回
  （RADV 未提供；RGL 不需要）

### 3.4 异步故障语义（修正心智模型）

`proposals/VK_KHR_device_fault.adoc` 明确：故障可由**已返回的 submit 异步触发**
（"provoked asynchronously by a submitted command buffer, but were not triggered until
after the queue submit had returned"）。即 **submit 成功 ≠ 没事**；fence 永不信号
的超时点正是异步故障的"常规到达点"——RGL 现行 fence 超时出口恰好是查询故障的正确时机。

### 3.5 EXT vs KHR 与 apiVersion

- `vk.xml`：`VK_EXT_device_fault(#342) promotedto VK_KHR_device_fault(#574)`；
  两者 `depends = VK_KHR_get_physical_device_properties2 + VK_VERSION_1_1`（无 1.4 门槛）
- **feature 结构两代不同**：`VkPhysicalDeviceFaultFeaturesKHR` **4 字段**（多
  `deviceFaultReportMasked` / `deviceFaultDeviceLostOnMasked`）；EXT 版仅 2 字段
  （L20982-20989）。**要 masked 能力必须链 KHR 结构**
- **apiVersion 已从 `VkDeviceCreateInfo` 删除**（系统头 L3725-3738 与 `vk.xml` L1295-1306
  双验，layer 字段标 `deprecated="unused"`）——"1.4 核心 vs EXT 路径"的老问题在本时间线
  spec 下不存在；扩展名按项目惯例仍可用 EXT，feature 结构用 KHR 4 字段版
- NVDIA 是本扩展作者（vk.xml `contact = Ralph Potter`，NVIDIA）；NVIDIA 官网 Vulkan driver
  support 页列出 `VK_EXT_device_fault [Linux]` → 换机后 5090 侧大概率三驱动最全
  （`deviceFaultVendorBinary` 大概率 true）

## 4. 对 RGL 的价值（补 die 策略，不改 die 策略）

### 5.1 死后验尸（主用途）

三处 DeviceLost 出口在退出前排空故障队列（`timeout=0` 非阻塞），结构化落盘：

1. **`WATCHDOG_TIMEOUT` vs `MEMORY_ADDRESS` 二分**：GPU 挂死（驱动/硬件/真 hang）
   vs 非法访存（**几乎必是应用 bug**：坏 BDA / 越界写 / UAF）。现 fence 超时路径零信息，
   这一对 flags 直接把"为什么死"砍成二选一
2. **GPU VA → RGL 对象**：fault VA 对 VMA 分配地址表 → 哪个 buffer/纹理/heap 槽 →
   哪个资产。UAF 场景（VA 已释放）对到的是**现持有者**，VA 未复用即可定罪；
   要铁证需 recently-freed ring log（可选二期）
3. 结合 `groupId`/`description`/`instructionAddressInfo`，验尸报告从"一行日志"
   升级为可直接写进驱动 bug report 的签名

### 5.2 masked fault 金丝雀（超出 device lost 的半个能力）

diagnostics 门控下每 N 帧 `timeout=0` 轮询一次（happy path 零成本）：
报出 masked fault = **该帧写目标已被污染但设备活着**——UAF 类 bug 的潜伏期探测器
（先 masked、后 device-lost 是常见恶化路径）。与 `gpu_canary` 互补：
canary 答"GPU 还活着吗"，masked fault 答"GPU 干了坏事但驱动帮你瞒着"。
`docs/vulkan_extensions_2025_调研.md:127` 所称"GPUVM 调试盲区"即此
（fault 不 sticky 时应用无感——老案例"状态漂移"现象的来源）。

### 5.3 shader abort = 带消息的 GPU 侧 assert（bonus）

`VK_KHR_shader_abort`：着色器 `OpAbortKHR` 主动中止并携带消息，
消息经 `VkDeviceFaultShaderAbortMessageInfoKHR`（`{messageDataSize, pMessageData}`）
从故障通道上报（`chapters/debugging.adoc:2669`）。CPU 侧有 `renderGraphSelfCheck`，
这是它的 GPU 侧对偶：顶点索引越界 / draw-args 槽位非法等**只有 GPU 上可见的不变量**，
从"静默损坏 → device lost"升级为指名道姓的消息。本机 64KB 消息、依赖扩展齐备。

### 5.4 与 Aftermath 的关系

`RGL_AFTERMATH` 编译开关保留（NV 侧 GPU dump / engine reset 追踪）。
本扩展是**标准化跨驱动**的对应物（AMD/Intel/NV 各自实现同一接口），
两者叠加 = 结构化 fault 报告 + 厂商深 dump，互不替代。

## 6. 归档三案例推演（能不能查出"为什么"）

**方法论前提**：故障队列随 device 消亡——**往事不能事后验尸，一律"复现 + 验尸"**。
三案例恰都有复现路径，价值各异：

### 案例 A：NO_TASK_SHADER 缺 flag（NVIDIA 610.57.04，**已闭环**）

- 形态（原 `min_mesh_crash/` README，`516cf5a` 删除 → `docs/archive/min_mesh_crash_已删除.md`）：不带 `NO_TASK_SHADER_BIT_EXT` 的 mesh shader
  对象契约上须配有效 task shader → NV 驱动 draw 时按 "task→mesh" 查到**空指针**
  → **GPU page fault at VA=0x0** → engine reset → `VK_ERROR_DEVICE_LOST`
- 当时证据：dmesg/Aftermath（`engine reset occurred: true`）+ 最小复现 + 契约推理
- 故障报告会给：`MEMORY_ADDRESS` + **fault VA=0x0** + READ/EXECUTE_INVALID + NV description
  —— **VA=0x0 = 空指针解引用**，进程内一步到位（省 dmesg/等待 reset 确认）
- 判定：已闭环，重跑价值 = 留一份**"空指针类驱动 fault"报告模板**，以后 NV 再出
  device lost 可直接对签名

### 案例 B：NV DGC push 挂死（5090/610.57.04，**机理未闭环——唯一还能真拿答案的**）

- 形态（`git:调试历史`）：官方 sample `PUSH_CONSTANT` × 44412 序列
  → `VK_ERROR_DEVICE_LOST` + 黑屏，栈卡 fence 等待；RGL 侧已 bindless 化规避（push 降 4B）
- 未闭环（`git:调试历史`）：**"push 字节数 × 序列数阈值规律太邪门"**，
  真正的 NV DGC 挂死机理留待复查（候选实验全部未做）
- 故障报告会强制二分：
  1. **底下是驱动内部 staging OOB 读** → `MEMORY_ADDRESS` + fault VA → 对 push token
     布局 → "邪门阈值"变具体越界算术（如"每序列尾部越 8B、N 序列跨页"）
  2. **纯挂死无 fault**（队列空或仅 `WATCHDOG_TIMEOUT`）→ **排除 OOB 假设**，
     机理收窄到驱动状态机/死锁，bug report 从"不明不白"变可证伪刻画
- 两条出路都通向"可报告的驱动 bug"。复现 = 官方 sample；机器 = 换机后 5090
  （与当年同卡同驱动 610.57.04 系）

### 案例 C：TexturePool UAF（RADV，**已闭环；本机可重跑 = 流水线验证实验**）

- 形态（`docs/archive/heap_texture_research.md` §3 状态段/§11/§12.6）：
  TexturePool 声明在 main() if 块内，块尾析构释放纹理 BO → 第 0 帧采样已释放 VA；
  **VA 未复用 = 读旧页"正常"，被复用 = TCP fault/全黑**；复现 `0x8001000fa000` /
  复跑相邻页 `0x8001000fb000`，首个 submit 即 device lost，2/2 确定性
- 当时证据链：**root dmesg**（amdgpu VM fault 行）+ lldb + "状态漂移"多日分析
  + **两轮误诊**（先定性 48 位描述符编码内核 bug，后 §12.6 整体撤回改写为 UAF）
- 故障报告会给：**同一个 fault VA，进程内、带类型、带精度**——崩的那一刻对
  VMA 分配表 = "该 VA 是 TexturePool N 号分配，if 块尾释放"，根因直指，
  免 root、免 dmesg、免漂移分析
- 判定：根因已闭环（修复 2de6731），重跑价值 = **换机前唯一能做的端到端验证**：
  复现路径现成（§12.6 记录"m3-scene 分支现场复现 + 最小修复单变量实锤"，
  checkout 修复前构建即得）；硬件换代不影响 UAF 逻辑与 GPUVM 机制同族
- 预期收益还包括：实测 RADV 报告质量（description 质量 / 地址精度）够不够做
  VA→对象对号——验证通过，案例 B 的实验方法即现成

## 7. 集成路径（计划级，能力门控 = DGC 同款模式）

1. **`rgl/vkdevice.cppm`**：precheck 加扩展探测；**链 4 字段
   `VkPhysicalDeviceFaultFeaturesKHR`**（masked bit 只在 KHR 版）进 featuresChain，
   走既有 optional feature 模式（同 eds3Feature/presentTimingFeature：类成员 + 条件链尾）；
   `getDeviceExtensions` optional 段加扩展名（按项目惯例 EXT 名）；
   `VkPhysicalDeviceFaultPropertiesKHR` 取 `maxDeviceFaultCount`
2. **`rgl/frame_session.cppm`**：三处 DeviceLost 出口 drain + 结构化日志落盘
   （flags/groupId/description/fault VA/精度/instruction VA/vendor code；
   pNext 挂 `VkDeviceFaultShaderAbortMessageInfoKHR` 若 shader_abort 已启）
3. **`rlvk.cppm`**：导出名单补 `vk::DeviceFaultInfoKHR` 等 `using` 行（维护规则见该文件头）
4. **可选批**：masked 轮询（diagnostics.lua 门控，每 N 帧 timeout=0）→ shader abort
   （**先验 Slang 对 `OpAbortKHR` 带消息变体的 spelling 支持**，未验）
5. **能力门控**：全路径按 feature 位分支（RADV 如本表 / NV 待测 / Intel 待测），
   扩展缺席 = 静默降级回现行裸日志，零开销

## 8. 限制与未验证项（诚实清单）

- `maxDeviceFaultCount = 1`：只留最近一条；masked 与 device-lost 连发可能只见最后一条
  （`OVERFLOW` 标志可察觉丢记录）
- RADV `vendorBinary = false`：无厂商 crash dump 可喂外部工具（NV 侧待换机验证）
- `deviceFaultDeviceLostOnMasked = false`：不能把 masked 转 device-lost（本机；非需求）
- 报告内容 = driver best-effort：地址精度硬件粒度、description 质量看驱动
- **无报告也有信息量**：若死于 CPU 侧驱动 assert（无 GPU fault 发生），队列空本身
  即"排除 GPU 越界假设"的证据（案例 B 出路 2）
- 本机仅 RADV 单卡：NVIDIA / Intel 能力位与报告质量待换机
- Slang `OpAbortKHR` 拼写支持未验
- 该扩展**不改变恢复语义**：device lost 仍 sticky，die 策略不变（spec 亦无恢复路径）

## 9. 执行顺序

1. **本机先做案例 C 重跑**（RADV，checkout 修复前构建）：验证整条
   "fault 报告 → VA → VMA 对号 → 资产名"流水线 + RADV 报告质量——
   集成代码量约 = 一个 optional feature 结构 + 三处出口各十几行
2. **换机后做案例 B**（官方 sample 加本扩展重跑）：拿机理（§6 二分）
3. **顺手做案例 A**（5090 重跑 min_mesh_crash）：留 NV 空指针类报告模板
4. masked 金丝雀 / shader abort 为独立可选批，随 diagnostics 门按需上

三个实验共用同一套集成代码，步骤 1 做完，2/3 只差环境变量与机器。

## 10. 关联文档

- `docs/vulkan_extensions_2025_调研.md` §L54/L126-127 —— 本文的方向前身
- `docs/archive/heap_texture_research.md` §11/§12.6 —— 案例 C 全过程与勘误
- `docs/archive/min_mesh_crash_已删除.md` —— 案例 A 最小复现墓碑（原 `min_mesh_crash/` 子项目 `516cf5a` 删除，代码见 git history）
- `git:调试历史` 2026-09-10 两条 —— 案例 B（含未闭环存疑段）
- `docs/nvidia_compatibility_report.md` —— 案例 A 跨驱动记录（其中机器表述待勘误）
- `docs/技术概念.md §构建检查工具机制` / `docs/风格检查规则.md` —— 无关，但本文状态头
  遵循 docs 状态行检查（`tools/checks/rules_text.py docs_status_checks`）
