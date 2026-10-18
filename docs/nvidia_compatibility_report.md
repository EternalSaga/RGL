# NVIDIA 跨平台兼容性排查报告

> **状态**：现行（跨驱动兼容性排坑记录，含历史排查过程；最新跨平台全流程验证见第八节，
> 最新驱动支持状态（present_timing）见第九节）

**日期**: 2026-08-12
**环境**: Gentoo + RTX 5090 (GB202) / AMD Granite Ridge (Raphael_Mendocino)；2026-08-27 增补：Intel Arc A310（dGPU，anv）/ AMD Strix Halo (Radeon 8060S，当前开发机)
**驱动**: NVIDIA 595.84 → 610.57.04
**状态**: AMD ✅ | NVIDIA ✅ (vertex + mesh shader) — 2026-08-17 更新：原"NVIDIA 驱动 bug"结论已推翻，真实根因是应用侧缺 `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT` flag，见第五节；**2026-08-27 更新：该时点代码（commit `1f43d65`）经 NVIDIA / Intel / AMD Strix Halo 三平台全流程验证通过**，见第八节

---

## 一、硬件拓扑

```
01:00.0  NVIDIA RTX 5090 (GB202) — DiscreteGpu, 无 display output
7a:00.0  AMD Granite Ridge (Raphael_Mendocino) — IntegratedGpu, display output
```

双 GPU + prime offload 模式（NVIDIA 渲染，AMD 输出 display）。

---

## 二、已完成的修改

| 文件 | 修改 | 原因 |
|---|---|---|
| `utils.cppm:114-129` | AMD pNext 链修复 | 原代码无条件链入 `VK_AMD_shader_core_properties`，非 AMD 上为 UB。改用模板 `getProperties2<...>`，仅在 AMD GPU 上查询 |
| `Justfile` run 路径 | ~~条件化 `RADV_EXPERIMENTAL=heap`~~（Mesa ≥26.2.3 已移除，无需该 env） | 历史：`lspci` 检测 AMD GPU 时设置环境变量，NVIDIA 上跳过 |
| `features_config.json` | 移除 `descriptorHeapCaptureReplay` | NVIDIA 595.84 不支持此 feature（610 驱动支持） |
| `vkresources.cppm` | Slot 编号动态化 | NVIDIA reservedSlots=6048，原固定 slot 8/12 在 reserved range 内部 |
| `VmaBuffer.cppm` | GPU address 对齐 | 修正 `resourceHeapAlignment` 对齐（NVIDIA=32B）+ CPU mapped data 对齐 |
| `shader.cppm` | frag shader 沿用同一 mappingInfo | frag 未声明的 binding 映射空转无副作用（Phase 3 起 frag 声明 binding 1/2，实际消费该映射） |
| `shader.cppm:44` | `mappings[0].resourceMask` 改为 `ALL_EXT` | NVIDIA 驱动可能更严格校验 resourceMask 与 SPIR-V 资源类型的匹配 |
| （主工程未启用） | `VK_SHADER_CREATE_LINK_STAGE_BIT_EXT` 仅存在于 heap_texture_spike / min_mesh_crash 子工程 | 主工程 shader.cppm 从未添加此 flag |
| `CMakeLists.txt` | Slang profile 维持 `sm_6_6+spirv_1_6` | 最终未改为 `glsl_460+spirv_1_6`（早期记录有误；profile 不是 NVIDIA 兼容性的核心问题） |
| `vcpkg.json` + `CMakeLists.txt` | slang 依赖标准化 | vcpkg 管理 slang 编译器 |

---

## 三、NVIDIA descriptor heap 属性对比

| 属性 | NVIDIA RTX 5090 | AMD Granite Ridge (RADV) |
|---|---|---|
| `bufferDescriptorSize` | **16** | **16** |
| `minResourceHeapReservedRange` | **96768** (当前 32B 统一步长 = 3024 槽；16B 步长时代 6048 槽) | **0** |
| `resourceHeapAlignment` | **32** | **4** |

**关键差异**：
- 两者 descriptor size 相同（16 字节）
- NVIDIA reserved range 巨大（96768B = 3024 槽@32B 步长 vs 0）— 需要跳过 reserved range 才能使用
- 对齐要求不同：NVIDIA=32B，AMD=4B
- 当前 `VmaBuffer.cppm` 的对齐逻辑同时满足两者（32 是 4 的倍数）

---

## 四、AMD heap 对齐问题（已修复）

**发现**：集成 diagnostic checkpoint 扩展时，发现 AMD 上原始代码也存在 `VK_ERROR_DEVICE_LOST`。

**根因**：validation layer 报错 `vkCmdBindResourceHeapEXT(): heapRange.address (0x6490150) must be aligned with resourceHeapAlignment (32)`。

**注意**：这个 validation error 实际来自 NVIDIA 驱动（AMD 的 `resourceHeapAlignment=4`）。当时排查环境是 NVIDIA GPU。

**修复**：`VmaBuffer.cppm:createDescriptorHeap()` 中计算 `gpuOffset` 修正偏移，确保 `rawAddr + offset` 满足 `resourceHeapAlignment` 对齐。

**验证**（NVIDIA 运行时输出）：
```
[Heap] rawGPUAddr=0x6490150 rawCPUAddr=0x7f...150 gpuOffset=16 cpuComp=0 totalOffset=16
[Heap] finalGPUAddr=0x6490160 finalCPUAddr=0x7f...160
[Heap] alignCheck: gpuMod32=0 cpuMod32=0  ← 对齐正确
```

**AMD 运行时验证**：
```
[Heap] descriptorSize=16 reservedSlots=0 heapAlignment=4 capacity=16
[Heap] rawGPUAddr=0xffff800000200000 gpuOffset=0 totalOffset=0
[Heap] alignCheck: gpuMod32=0 cpuMod32=0  ← 对齐正确
```
AMD 上 heap 对齐要求低（4B），VMA 分配的地址天然满足。程序正常运行，无 device lost。

---

## 五、NVIDIA GPU 异常（根因已定位）

### 5.1 错误信息

```
NVRM: Xid 13, Graphics Exception: Shader Program Header 18 Error
NVRM: Xid 13, Graphics Exception: ESR 0x405840=0x82040000
NVRM: Xid 13, Graphics Exception: ESR 0x405848=0x80000000
```

### 5.2 根因排查过程

| 测试 | 操作 | 结果 |
|---|---|---|
| 基础 | mesh shader + shader object + descriptor heap | ❌ Xid 13 |
| 简化 mesh shader | hardcoded triangle（无 buffer）+ shader object | ❌ Xid 13 |
| 移除 heap | mesh shader + shader object，无 heap | ❌ Xid 13 |
| 改用 vertex shader | vertex shader + shader object + descriptor heap | ✅ 正常 |

### 5.3 结论（2026-08-17 修订：**不是驱动 bug**）

**NVIDIA Xid 13 根因**：**应用侧 spec 合规问题** — mesh shader 对象创建时缺少 `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT` flag，同时 TASK stage 绑定 `VK_NULL_HANDLE`。规范对该 flag 的语义：不带此 flag 的 mesh shader **只能与 task shader 一起使用**。NVIDIA 驱动遵守该契约，dispatch 时查找关联的 task shader 得到空指针 → Xid 13 (Shader Program Header error) → engine reset。

**A/B 验证**（min_mesh_crash，RTX 5090, 610.57.04）：
- 去掉 `NO_TASK_SHADER` flag → 复现：frame 0 提交成功、frame 1 `VK_ERROR_DEVICE_LOST`，Aftermath `engine reset occurred: true`
- 加上 flag → 10/10 帧通过，VVL 零错误
- rgl 同步修复后 NVIDIA 与 AMD 均正常渲染

**为何之前误判为驱动 bug**（三层掩盖）：
1. **VVL 静默**：本应报错的 `VUID-vkCmdDrawMeshTasksEXT-None-08694`（无 flag 时 TASK stage 必须绑定有效 task shader）**仅在 `taskShader` + `meshShader` 双 feature 启用时才生效**；本项目只启用了 `meshShader` → spec 漏洞（Vulkan-ValidationLayers issue #12886）
2. **RADV 宽松**：AMD 上无 flag 也不崩（按 mesh-only 模式直接发射），主项目一直在 AMD 上正常，掩盖了问题
3. vertex+fragment 路径不涉及 task/mesh 契约，正常工作

**已推翻的旧依据**：
- ~~驱动 610.57.04 不支持 mesh shader + shader object 组合~~ — 实际支持，flag 设对即可
- ~~NVIDIA 官方 sample 没有该组合验证了这一点~~ — sample 缺失不是驱动不支持的证据

（早前观察 Class 0000ce97 是 GB202 的 GPFAS 类、错误发生在 mesh shader 执行阶段，仍然成立。）

### 5.4 解决方案

~~临时方案（2026-08-12）：将 mesh shader 替换为 vertex shader~~ — **已不需要**。

**最终修复**（2026-08-17）：

```cpp
// rgl/shader.cppm:67（2026-08-17 时点；文件后更名重构，现为 rgl/shader.cppm:237）和 min_mesh_crash/main.cpp:252
meshCreateInfoRaw.flags |= VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
```

**最终验证**：
| 配置 | NVIDIA RTX 5090 | AMD Granite Ridge (RADV) |
|---|---|---|
| vertex shader + shader object + descriptor heap | ✅ | ✅ |
| mesh shader + shader object + descriptor heap（**带 NO_TASK_SHADER**） | ✅ | ✅ |
| mesh shader + shader object（不带 flag） | ❌ Xid 13 / engine reset | ✅（RADV 宽松） |

---

## 六、GPU 诊断工具集成

> **2026-08-17**：本节及第七节的诊断代码（NV 诊断扩展 + Aftermath 集成）已作为 NVIDIA 专属调试代码从 rgl 移除，仅 min_mesh_crash 保留 Aftermath 用于崩溃复现。以下为历史记录。

**已完成**：集成 `VK_NV_device_diagnostic_checkpoints` 和 `VK_EXT_device_fault` 扩展（仅限 NVIDIA GPU）。

### 6.1 Checkpoint 标记位置

```
[CP] HeapBarrier      → pipelineBarrier2(heapBarrier) 之前
[CP] BindHeap         → bindResourceHeapEXT 之前
[CP] PushData         → pushDataEXT 之前
[CP] BeginRendering   → beginRendering 之前
[CP] BindShaders      → recordRenderCommands 之前
[CP] DrawMeshTasks    → drawMeshTasksEXT 之前
[CP] AfterDraw        → drawMeshTasksEXT 之后
```

### 6.2 限制

- NVIDIA 610.57.04 驱动中这两个扩展的函数指针为空（`vkGetDeviceProcAddr` 返回 NULL）
- 可能是驱动不再公开这些扩展，或需要特殊配置
- 当前不可用；后续驱动更新后重新测试

---

## 七、Nsight Aftermath SDK 集成（2026-08-12）

**状态**：✅ Aftermath SDK 成功集成并捕获 GPU crash dump

### 7.1 集成方式

| 组件 | 文件 | 说明 |
|---|---|---|
| SDK 封装 | `aftermath.cppm` | `initAftermath()`, `shutdownAftermath()`, `waitForCrashDump()`, crash dump 解码 |
| 设备配置 | `vkdevice.cppm` | `VkDeviceDiagnosticsConfigCreateInfoNV` 注入 pNext 链 |
| 扩展列表 | `features.cppm` | 添加 `VK_NV_device_diagnostics_config` 扩展探测和启用 |
| 渲染器 | `vkrenderer.cppm` | Device lost 检测 + `waitForCrashDump()` 调用 |
| 入口 | `main.cpp` | 创建 Vulkan 设备前调用 `initAftermath()` |
| 构建 | `CMakeLists.txt` | 链接 `libGFSDK_Aftermath_Lib.x64.so` |

### 7.2 Crash Dump 结果（全诊断模式，NVIDIA RTX 5090, driver 610.57.04）

```
Aftermath: GPU crash dump enabled (SDK v2.27)
Device: enabling 12 extensions (含 3 个 Aftermath 扩展)
Aftermath diagnostics: checkpoints=true, fault=true, diagnosticsConfig=true, flags=15 (全开)
Aftermath: GPU crash dump received (60132 bytes)
Aftermath: crash dump processing finished
Aftermath: app=rgl, date=Wed Aug 12 23:06:39 2026, pid=427661, api=7 (Vulkan)
Aftermath: device status=Reset, adapterReset=false, engineReset=true
Aftermath: pageFault VA=0x0 type=0 access=0 engine=0 client=0
Aftermath: active shader count=0
Aftermath: event marker count=2 (1 NotStarted, 1 Executing)
Aftermath: JSON crash dump written to aftermath_crash_dump.json (5514 bytes)
```

**对比旧版（仅 ShaderErrorReporting + ResourceTracking）：**
| 项目 | 旧版 | 全诊断模式 |
|---|---|---|
| Crash dump 大小 | 58064 bytes | 60132 bytes |
| JSON 大小 | 1089 bytes | 5514 bytes |
| Event markers | 无 | 2 个（含 CPU call stack） |
| 扩展数量 | 9 | 12 |

### 7.3 自动 Checkpoint 调用栈分析

Aftermath 的自动 checkpoint 在每个 Vulkan 命令后插入 CPU 调用栈。崩溃时捕获了 2 个事件：

| 事件 | 状态 | 说明 |
|---|---|---|
| #1 | NotStarted | 该命令 GPU 尚未开始执行 |
| **#2** | **Executing** | **GPU 正在执行此命令时崩溃** |

**事件 #2 的 CPU 调用栈反解：**

| 运行时地址 | 二进制偏移 | 符号 |
|---|---|---|
| 0x55837cd8cf0c | 0xa4007 | `RGL::renderSystem` (+1607) |
| 0x55837cd86a1b | 0x9db16 | `RGL::Renderer::drawFrame` 附近 |
| 0x55837cd219c3 | 0x38abe | `main` 内部 |
| 0x55837cd216c5 | 0x387c0 | `main` |

**分析**：GPU 在执行 `drawMeshTasksEXT` 发出的 mesh shader 时发生 engine reset。崩溃发生在 `renderSystem` 的渲染循环中，命令已通过 validation layer 和 NVIDIA 驱动提交给 GPU。

### 7.6 关键发现

1. **Aftermath SDK 路径有效**：即使 `vkGetDeviceProcAddr` 返回 NULL，`GFSDK_Aftermath_EnableGpuCrashDumps` 通过独立 NvAPI 通道正常工作
2. **设备状态**：Reset (engine reset, 非 adapter reset) — GPU 引擎因 mesh shader 执行重置
3. **VA=0**：pageFault VA=0x0 — 当时解读为"不是内存访问错误"；结合 2026-08-17 确认的根因（驱动按契约取不存在的 task shader → 空指针），VA=0x0 与之吻合
4. **无 Active Shader**：崩溃太快，驱动来不及在 shader 执行中期捕获活跃 shader
5. **自动 Checkpoint 定位**：CPU call stack 显示崩溃发生在 `renderSystem` 的渲染循环中，GPU 正在执行通过 `drawMeshTasksEXT` 提交的 mesh shader 命令
6. **Fingerprint 一致性**：hash=18443650224304399981，每次运行相同 — 确认是确定性行为（当时记录为"驱动 bug"；2026-08-17 确认根因为应用侧缺 NO_TASK_SHADER flag，输入状态相同故指纹恒定）
7. **Heap 问题已修复**：`heapRange.size` 从 `descriptorSize * capacity` 改为 `getHeapSize()`，扣除对齐偏移。验证层不再报错，GPU 能走到 mesh shader 执行阶段

### 7.7 诊断配置

设备级 Aftermath 功能通过 `VK_NV_device_diagnostics_config` 配置（flags=15, 全部启用）：
- `VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV` — 零开销 shader 错误报告
- `VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV` — 资源跟踪
- `VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV` — 自动 CPU call stack 标记
- `VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV` — shader 调试信息

---

## 八、2026-08-27 跨平台验证更新

该时点代码（commit `1f43d65`，Phase 2 完成态：glTF 加载 + flat unlit + 资产表 + llm_debug 一次性截图）在以下平台全流程验证通过（mesh shader + descriptor heap + dynamic state + 模型渲染，`just run` 无 warn/error）：

| 平台 | 配置 | 结果 |
|---|---|---|
| NVIDIA RTX 5090 | prime offload（driver 610.57.04） | ✅ |
| Intel Arc A310 (dGPU, anv) | 见 commit `1f43d65` run-intel 注释 | ✅ |
| AMD Strix Halo (Radeon 8060S) | RADV 单卡核显机（Mesa ≥26.2.3，无需 `RADV_EXPERIMENTAL`） | ✅ |

**当前开发机（2026-08-27 起）**：Strix Halo 纯核显单卡机器（无独显），RADV + Mesa ≥26.2.3——`VK_EXT_descriptor_heap` 已无条件 advertise，不再依赖 experimental 开关。

**注**：其后 Phase 3–7 演进（最新 `b1788e0` 架构债务 1+2）仅在本机 Strix Halo 验证，NVIDIA/Intel 全流程未复验。

**边界说明**：主工程当时全流程通过的是 buffer 描述符路径（vertex/index BDA）；descriptor heap 的 **image 描述符 + 内嵌/分离 sampler** 路径已在 spike 级完成三驱动（RADV/Intel/NVIDIA）× 5 变体像素级交叉测试（2026-08-27，`heap_texture_spike/`，见 `docs/archive/heap_texture_research.md` §九）；主工程含纹理的全流程另在 RADV 与 Windows/NVIDIA（Phase 4 时代，`602e372`）实测过，Linux NVIDIA prime offload 全流程未复测。

---

## 九、Phase 9.3 `VK_EXT_present_timing` 驱动支持状态（2026-09-27）

批 9.3 把 present_timing 作为**可选组门**引入（`present_timing + present_id2 +
VK_KHR_calibrated_timestamps` 三探测全真才启用，任一缺 = 整组 no-op，不破坏启动），
因此跨驱动差异只影响诊断能力、不影响渲染正确性。当前状态：

| 平台 | 扩展 advertise | 运行时实测 | 结论 |
|---|---|---|---|
| **AMD Strix Halo（Radeon 8060S，RADV，Mesa 26.2.3）= 当前开发机** | ✅ 五项齐（present_timing / present_id2 / get_surface_capabilities2 / KHR+EXT calibrated_timestamps，`vulkaninfo` 复核，且 `VK_EXT_descriptor_heap` 同机 advertise） | ✅ **激活并跑通**（单实例，各 25 s）：present 结果持续回收、eImmediate 与 FIFO 分布一致（帧墙钟 avg 7.10 ms、IPD p50 6.96/6.97 ms、**drop=0**、queueFull=0）、queue-full 回退以临时压容量强制触发并自动恢复、VVL 零报警；另有一次 6.5 min 长跑无 crash（该跑的 IPD/drop 数值因双实例污染作废，见 `git:调试历史` 勘误条目） | 支持完整（**只 advertise `VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT`**，无 swapchain-local；且不同 stage 时间戳历元相差 ~10 s → 跨 stage 跨度不可用，详见 `git:调试历史` 2026-09-27 条目） |
| **NVIDIA（RTX 5090，prime offload / Windows）** | ❓ 未查（本机无 NVIDIA 可见；`vulkaninfo` 枚举不到该设备） | ⏳ 待有卡机器：`just run-igpu` 之外需 `just run`（NVIDIA 路）跑一次窗口 FIFO + eImmediate，看激活三行 log（`Present timing probe` / `active` / `first result`）与 drop/IPD 分布 | 未验证 |
| **Intel Arc A310（anv）** | ❓ 未查 | ⏳ 同上（`just run-intel`） | 未验证 |

**验证操作口径**（跨机接手时照此）：`configs/diagnostics.lua` 置 `profiler = 30`、
`configs/runtime.lua` 保持窗口模式，分别以 `present_mode = "immediate"` / `"fifo"` 各跑
≥60 s；判读 = ① 是否出现 `Present timing active`（未出现则看 `Present timing probe`
行的 `presentTimingSupported` / `stageQueries`，即该驱动/合成器的支持面）；②
`present_timing: first result` 行里哪些 stage 给了非 0 时间；③ 终值
`present_timing summary` / `IPD` 两行的 mean/p50/p95 与 `drop`、`queueFull` 计数。
未激活或结果全 0 均**不影响程序正确性**（测量-only，默认关时交换链创建参数逐位同旧版，
headless 与窗口 llm_debug 截图已验证 BIT-IDENTICAL）。

---

## 十、参考

- `min_mesh_crash/` 子项目（`516cf5a` 删除，墓碑已删 → `git:min_mesh_crash_已删除.md`）— 最小复现 + 根因证据链（本调查的权威记录，代码见 git history）
- NVIDIA 官方 sample：`/home/robin/mounts/gm7000/program/vk_mini_samples/`
- `docs/DESCRIPTOR_HEAP_SPEC.md` — descriptor heap 排坑记录
- Vulkan spec Debugging chapter: https://docs.vulkan.org/spec/latest/chapters/debugging.html
