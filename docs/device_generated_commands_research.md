# VK_EXT_device_generated_commands 扩展研究记录

> **状态**：调研 + 实施记录（Phase 7b 批次 5 已按本文落地，2026-09-09；三驱动（RADV/NVIDIA/anv）实测结论持续有效）

**日期**: 2026-08-22（实施更新 2026-09-09）
**环境**: Gentoo, Vulkan 头文件 1.4.341 (系统), loader 1.4.341, RADV ICD 1.4.354 / NVIDIA 610.57.04
**结论**: 双驱动均支持该扩展（rev 1，feature 全真）。但存在**关键驱动不对称**：RADV 仅支持 compute 侧 indirect execution set（graphics 侧 per-sequence shader 绑定 / mesh draw 不支持），NVIDIA 全支持（含 MESH+FRAGMENT shader object 绑定）。**决策：随 Phase 7（延迟渲染）作为 culling 前置子阶段引入**，graphics 侧路径必须带 CPU 回退（~~`vkCmdDrawMeshTasksIndirectCountEXT`~~ **勘误 2026-09-09：IndirectCount 无 per-draw push data 机制，回退改为读回重放，见 §4.3**）。

**实施状态（2026-09-09，Phase 7b 批次 5）**：已落地 `gpu_cull`（culling compute）+ `vk_indirect_commands`（DGC RAII）+ `vkrenderer` 双路（DGC execute / 读回重放），能力门控；forward+deferred BIT-IDENTICAL 验收通过（同会话双构建）。本机（9950X Raphael 核显 = RDNA2 小核，RADV 26.2.1）graphicsIndirect=false → 实际走读回重放；**注：非 Strix Halo，false 疑为驱动按硬件代际 gate，待真 Strix Halo 复测**。**2026-09-10 NVIDIA 机（RTX 5090/610.57.04）首次运行时实测**：初崩（DGC push token 挂死 GPU），经 DrawArgs bindless 化（push 降至 4B）后跑通——根因已石锤 = NV 驱动 push 挂死 bug（官方 sample 喂 100B push 复现同样 DEVICE_LOST，见 `docs/调试历史.md` 2026-09-10 条目）；**终验（同日续 2）：启动期选路 log 确认 NV 走 `DGC execute` 跑帧零 error/warning，期间一次 `vkCreateIndirectExecutionSetEXT: ErrorOutOfDeviceMemory` 为运行环境显存干扰假警报（干净环境重跑即通）**。实施偏离与 v357 API 差异见 §4.4。**2026-09-10 Intel Arc A310（DG2/anv 26.2.1）补测**：能力位 `graphicsIndirect=false computeIndirect=true`（`...ShaderBinding` 子集 = COMPUTE+RT，无 MESH/FRAGMENT，与 RADV 同画像；vulkaninfo 属性见 §3.2）→ 走读回重放，culling compute 照跑，reconcile 1571/1571 全匹配、零 error、bistro 画面正确。**至此三驱动（RADV/NVIDIA/anv）Phase 7b DGC/culling 路径全验证通过。**

---

## 一、扩展概况

| 项目 | 值 |
|---|---|
| 名称 | `VK_EXT_device_generated_commands`（device 扩展） |
| 规范版本 | 1（`VK_EXT_DEVICE_GENERATED_COMMANDS_SPEC_VERSION 1`，vulkan_core.h:23946） |
| 定位 | `VK_NV_device_generated_commands` 的 **EXT promoted 版，但 API 已重新设计，不是别名**——旧 NV API（`vkCmdBuildDeviceGeneratedCommandsNV`、shader group、stream）与 EXT API（indirect commands layout + execution set + preprocess/execute）是两套接口。本机 NVIDIA 610 同时 advertise 两者（NV rev 3 + EXT rev 1），引入时**只用 EXT API** |
| 核心思想 | GPU（compute/task）生成"命令序列数据"，设备端按 layout 描述把序列解释为真实的 draw/dispatch/push 命令，免去 CPU 逐 draw 提交 |

规范正文（`Vulkan-Docs` 仓库 `chapters/device_generated_commands/generatedcommands.adoc`）定义的主流程：

1. 创建 **indirect commands layout**（token 序列描述，action token 必须且只能有一个且必须最后）；
2. 可选创建 **indirect execution set**（可设备绑定的 pipeline 或 **shader object** 集合，per-sequence 选 slot）；
3. `vkGetGeneratedCommandsMemoryRequirementsEXT` 查 preprocess buffer 需求；
4. GPU/CPU 填充 indirect 数据 buffer（每个 sequence 一段，按 `indirectStride` 排布）；
5. 可选单独 `vkCmdPreprocessGeneratedCommandsEXT`（独立同步，stage = `COMMAND_PREPROCESS`）；
6. `vkCmdExecuteGeneratedCommandsEXT` 生成并执行命令（未单独 preprocess 时内部自动同步）。

与本项目架构的天然契合点：

- **execution set 支持 shader object 模式**（`VK_INDIRECT_EXECUTION_SET_INFO_TYPE_SHADER_OBJECTS_EXT` + `VkIndirectExecutionSetShaderInfoEXT` + `vkUpdateIndirectExecutionSetShaderEXT`）——本项目全程 shader object（`shader.cppm`），无需 pipeline；
- **`PUSH_DATA` token 为 descriptor heap 专属**（spec 中 `ifdef::VK_EXT_descriptor_heap` 守卫）——与本项目 `vkCmdPushDataEXT` + `DrawArgs` 设计同源，per-sequence 可推自己的 push data；
- 有 **`DRAW_MESH_TASKS(_COUNT)` token**（数据 `VkDrawMeshTasksIndirectCommandEXT`）——直接对接现有 mesh shader 管线；
- descriptor heap 已启用时 layout 的 `pipelineLayout` 可直接传 `VK_NULL_HANDLE`（spec 明文允许），**无需** `dynamicGeneratedPipelineLayout` feature。

---

## 二、API 表面（本地头文件）

### 对象

| 类型 | 说明 |
|---|---|
| `VkIndirectCommandsLayoutEXT` | token 序列布局（`VkIndirectCommandsLayoutCreateInfoEXT`：flags / shaderStages / indirectStride / pipelineLayout / pTokens） |
| `VkIndirectExecutionSetEXT` | pipeline 或 shader object 集合（union `VkIndirectExecutionSetInfoEXT`；shader 模式：`pInitialShaders` + `maxShaderCount` + `pPushConstantRanges` + setLayoutInfo） |

### 设备端入口（vulkan_core.h:24165-24235；vulkan.hpp:7416-7469、:5806、:28286 均有 wrapper，RAII 类 `vk::raii::IndirectCommandsLayoutEXT` @vulkan.hpp:11118）

| 入口点 | 作用 |
|---|---|
| `vkCreateIndirectCommandsLayoutEXT` / `vkDestroyIndirectCommandsLayoutEXT` | layout 生命周期 |
| `vkCreateIndirectExecutionSetEXT` / `vkDestroyIndirectExecutionSetEXT` | execution set 生命周期 |
| `vkUpdateIndirectExecutionSetShaderEXT` | 向 set 的 slot 写入 `VkShaderEXT`（启动时一次性填充各 material 的 shader；**改动后必须重查 memory requirements**） |
| `vkGetGeneratedCommandsMemoryRequirementsEXT` | `VkGeneratedCommandsMemoryRequirementsInfoEXT{set, layout, maxSequenceCount, maxDrawCount}` → `VkMemoryRequirements2`（preprocess buffer 大小） |

### 命令缓冲入口

| 入口点 | 作用 |
|---|---|
| `vkCmdPreprocessGeneratedCommandsEXT(cb, info, stateCommandBuffer)` | 预执行（可选单独步骤；`stateCommandBuffer` 用于 state token） |
| `vkCmdExecuteGeneratedCommandsEXT(cb, isPreprocessed, info)` | 生成并执行；`info`（`VkGeneratedCommandsInfoEXT`）含 indirect 数据地址、preprocess 地址、`sequenceCountAddress`（设备端读的 32 位 count）、maxSequenceCount/maxDrawCount |

### Token（`VkIndirectCommandsTokenTypeEXT`，vulkan_core.h:23952-23977）

| token | 数据 | 备注 |
|---|---|---|
| `EXECUTION_SET_EXT` | u32（shader object 模式下为 `u32[]` slot 索引数组） | 若有必须第一个；至多 1 个 |
| `PUSH_CONSTANT_EXT` | u32[] 原始数据 | 需 pipelineLayout（本项目不用） |
| `PUSH_DATA_EXT` | u8[] 原始数据 | **descriptor heap 专属**，per-sequence push data |
| `PUSH_DATA_SEQUENCE_INDEX_EXT` | u32 占位 | 同上守卫 |
| `SEQUENCE_INDEX_EXT` | u32 占位 | 至多 1 个 |
| `INDEX_BUFFER_EXT` / `VERTEX_BUFFER_EXT` | `VkBindIndex/VertexBufferIndirectCommandEXT` | 设备端绑 buffer |
| `DRAW_EXT` / `DRAW_INDEXED_EXT` | 标准 indirect draw 结构 | — |
| `DRAW_COUNT_EXT` / `DRAW_INDEXED_COUNT_EXT` / `DRAW_MESH_TASKS_COUNT_EXT` | `VkDrawIndirectCountIndirectCommandEXT`（bufferAddress + stride + commandCount） | 需 `deviceGeneratedCommandsMultiDrawIndirectCount`（双驱动均 true） |
| `DRAW_MESH_TASKS_EXT` | `VkDrawMeshTasksIndirectCommandEXT` | 本项目目标 token |
| `DISPATCH_EXT` | `VkDispatchIndirectCommand` | compute 侧（RADV 唯一可用路径） |

layout 规则：offset 单调不减、4 字节对齐、action token 必须最后；token 数 ≤ `maxIndirectCommandsTokenCount`、stride ≤ `maxIndirectCommandsIndirectStride`。usage flags：`EXPLICIT_PREPROCESS_BIT_EXT`（总是单独 preprocess）、`UNORDERED_SEQUENCES_BIT_EXT`（序列处理顺序实现定义）。

### feature / property

`VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT`（vulkan_core.h:23989）：

| feature | 说明 |
|---|---|
| `deviceGeneratedCommands` | 扩展主体，必须使能 |
| `dynamicGeneratedPipelineLayout` | 允许 layout 不带 pipelineLayout 而经 pNext 链 `VkPipelineLayoutCreateInfo`；本项目**不需要**（descriptor heap 下 pipelineLayout 可为 NULL） |

`VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT`（vulkan_core.h:23996）：`maxIndirectPipelineCount`、`maxIndirectShaderObjectCount`、`maxIndirectSequenceCount`、`maxIndirectCommandsTokenCount`、`maxIndirectCommandsTokenOffset`、`maxIndirectCommandsIndirectStride`、`supportedIndirectCommandsInputModes`、`supportedIndirectCommandsShaderStages`（及 `...PipelineBinding` / `...ShaderBinding` 两个子集）、`deviceGeneratedCommandsTransformFeedback`、`deviceGeneratedCommandsMultiDrawIndirectCount`。

### 同步（sync2 风格，与项目 `pipelineBarrier2` 惯例兼容）

- stage：`VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_EXT`（0x00020000，vulkan_core.h:7091）
- access：`VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_EXT`（0x00020000）/ `VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_EXT`（0x00040000，vulkan_core.h:2641-2642、7176-7177）
- 数据流：indirect 数据 buffer（写方 = culling dispatch 或 host）→ preprocess（读 indirect / 写 preprocess）→ execute（读 preprocess）；单独 preprocess 时必须对 execute 显式同步，内联时驱动自动处理。

---

## 三、本地驱动实测（vulkaninfo，2026-08-22；Intel A310 列 2026-09-10 补测）

### 3.1 扩展与 feature

| 项 | GPU0: RADV (Raphael_Mendocino, ICD 1.4.354) | GPU1: NVIDIA RTX 5090 (610.57.04) | GPU2: Intel Arc A310 (DG2, anv 26.2.1) |
|---|---|---|---|
| `VK_EXT_device_generated_commands` | ✅ rev 1 | ✅ rev 1（另 advertise legacy `VK_NV_device_generated_commands` rev 3、`..._compute` rev 2） | ✅ rev 1 |
| `deviceGeneratedCommands` | true | true | true |
| `dynamicGeneratedPipelineLayout` | true | true | true |

### 3.2 属性对比（`VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT`）

| 属性 | RADV | NVIDIA | Intel A310 (anv 26.2.1) |
|---|---|---|---|
| `maxIndirectShaderObjectCount` | 4096 | 16384 | 4096 |
| `maxIndirectSequenceCount` | 1048576 | 4194303 | 1048576 |
| `maxIndirectCommandsTokenCount` | 128 | 32 | 32 |
| `maxIndirectCommandsTokenOffset` / `...IndirectStride` | 2047 / **2048B** | 4G-1 / 4G-1 | 65536 / 4G-1 |
| `supportedIndirectCommandsInputModes` | VULKAN_INDEX_BUFFER + DXGI_INDEX_BUFFER | 同左 | 同左 |
| `supportedIndirectCommandsShaderStages` | 14 个 stage（全 graphics + compute + RT + task/mesh） | 8 个 stage（graphics + compute + task/mesh，无 RT） | 12 个 stage（graphics + compute + RT，**无 MESH/TASK**） |
| **`...ShaderStagesPipelineBinding`** | **仅 COMPUTE** | 全部 8 个（含 MESH/FRAGMENT） | 7 个（COMPUTE + 全 RT，**无 MESH/FRAGMENT/VERTEX**） |
| **`...ShaderStagesShaderBinding`** | **仅 COMPUTE** | 全部 8 个（含 MESH/FRAGMENT） | 7 个（COMPUTE + 全 RT，**无 MESH/FRAGMENT/VERTEX**） |
| `...TransformFeedback` / `...MultiDrawIndirectCount` | true / true | true / true | true / **false** |

### 3.3 关键结论

1. **RADV 目前只实现了 compute 侧**：`supportedIndirectCommandsShaderStages{Pipeline,Shader}Binding` 均只含 `COMPUTE` → graphics 侧的 per-sequence shader 绑定与 mesh/vertex draw 序列**不可用**；`DISPATCH` 序列可用（如 GPU culling dispatch 本身）。
2. **NVIDIA 全支持**，含本项目需要的 MESH + FRAGMENT shader object 绑定、push data、`DRAW_MESH_TASKS(_COUNT)`。
3. **能力探测必须运行时做**：`supportsGraphicsIndirect = (shaderStagesShaderBinding & (MESH|FRAGMENT)) != 0`，据此选 GPU-driven 路径（NVIDIA）或 CPU 回退（RADV）。
4. RADV 侧限制（stride 2048B、sequence 1M）对 culling 用途无碍；NVIDIA token 数上限 32 也远大于本项目需要（3-4 个 token）。
5. **驱动探测陷阱**：NVIDIA ICD 是 thin loader，`libGLX_nvidia.so.0` 内 grep 不到任何扩展名串（连 `descriptor_heap` 都没有），**不能用 .so 字符串判断 NVIDIA 能力**；RADV 则相反（扩展名串在 `.so` 内）。一律以 `vulkaninfo` / 运行时 enumerate 为准。
6. **Intel A310 (anv) 与 RADV 同画像**（2026-09-10 补测）：`...ShaderStagesShaderBinding` 子集仅 COMPUTE + 全 RT（**无 MESH/FRAGMENT/VERTEX**）→ `graphicsIndirect=false`，走读回重放；culling compute（COMPUTE 在 binding 内）照跑。其全 stage 集（12）**连 MESH/TASK 都不含**（RADV 14 / NVIDIA 8 均含 task/mesh），且 `MultiDrawIndirectCount=false`（另两家 true）——anv 的 DGC graphics 侧目前比 RADV 还保守，日后跟进后能力位自动生效、无需改代码。

---

## 四、与当前项目的交互分析

### 4.1 现状：已移除（2026-08-22；此前为"已启用、零使用"）

- （移除前状态）`features.cppm:25,55`：`VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME` 在 required 检查列表和启用列表中（2026-08-22 移除）；`min_mesh_crash/main.cpp:95` 仍启用。
- （移除前状态）全项目 grep（`IndirectExecutionSet|IndirectCommandsLayout|PreprocessGeneratedCommands|ExecuteGeneratedCommands`）**零 API 调用**——纯"启用未使用"。
- **AGENTS.md"隐式扩展依赖"条目核查**（"被其他已启用的扩展隐式依赖，不可随意删除"）：**registry 级已定论，该说法不成立**——
  - 权威 registry（`Vulkan-Docs/xml/vk.xml:30852`）中该扩展块：`depends="((VK_KHR_buffer_device_address,VK_VERSION_1_2)+(VK_KHR_extended_flags,VK_KHR_maintenance5)),VK_VERSION_1_3"` → 仅依赖 **Vulkan 1.3 + device address 能力族**（本项目均已满足），**不依赖** descriptor_heap / shader_object / mesh_shader（正文中对它们的引用只是 ifdef 守卫个别 token，如 PUSH_DATA）；
  - **反向查询**：全部 `<extension>` 标签中无任何其他扩展 `depends` 声明引用本扩展（vk.xml 中 5 处 `<require depends="VK_EXT_device_generated_commands">` 位于**其他扩展块内**——descriptor_heap / NV_mesh_shader / sync2 / EXT_mesh_shader / ray_tracing_maintenance1——为条件性 token 追加，仅当 DGC 启用时生效，方向为 token→DGC，非其他扩展对 DGC 的硬依赖）；
  - 实测佐证（Mesa 26.2.1 时代，`descriptor_heap` 尚需 `RADV_EXPERIMENTAL` 开关时）：RADV 可不 advertise `descriptor_heap` 却仍 advertise `device_generated_commands`（spec 规定被支持的扩展其依赖必须同时被支持，否则 RADV 不应 advertise 它）。Mesa ≥26.2.3 起 `descriptor_heap` 已无条件 advertise，二者独立性结论不变；
  - 该条目是 NVIDIA Xid 排查期（`min_mesh_crash` 参考 NVIDIA sample 扩展列表）的保守遗留。原拟保留现状（双驱动均支持），A/B 实验（见下）确认移除零回归后正式移除，Phase 7 引入时从"隐式依赖"转为**显式使用**（feature chain + culling 子系统），AGENTS.md 表述已同步修正。
  - **A/B 编译运行实验（2026-08-22 实测）**：从 `features.cppm` 两处列表移除该扩展 → 增量编译 → 双 GPU 各跑 25s（`just run-igpu` / `run-dgpu`）→ 恢复代码对照复跑：

    | 运行 | GPU | 扩展 | 结果 |
    |---|---|---|---|
    | 基线 | iGPU (RADV) | ✅ 9 ext | exit 124（持续渲染，无错误） |
    | 基线 | dGPU (NVIDIA) | ✅ 9 ext | exit 124（持续渲染，无错误） |
    | A：移除 | iGPU (RADV) | ❌ 8 ext | exit 124，无错误 |
    | A：移除 | dGPU (NVIDIA) | ❌ 8 ext | exit 124，无错误，dmesg 无 Xid |
    | 对照：恢复 | iGPU / dGPU | ✅ 9 ext | 均 exit 124 |

    **结论：移除该扩展双驱动零回归，"隐式依赖"定论（registry 级）获运行时实测确认。该扩展现已从 `features.cppm` 两处列表正式移除（2026-08-22，移除后双 GPU 复跑验证通过）。** 注意：`features_config.json` 是 feature chain 配置，本就没有该扩展的 feature（`deviceGeneratedCommands` 从未启用过），无需变更；本实验只覆盖当前用法（纯 shader object + heap + push data 渲染），未覆盖任何 device-generated-commands API 调用（项目本就没有）。

### 4.2 与 roadmap 各阶段的映射

| 阶段 | 是否需要 | 分析 |
|---|---|---|
| P2 (glTF) | 非必需，可选 spike | 多 mesh 渲染出现后，GPU culling 可替代 CPU 逐 mesh `drawMeshTasksEXT`；但 P2 明确"不做 instancing、逐步推进"，混入会扩大阶段面 |
| P3 (纹理) | 否 | 纹理/descriptor 管理与本扩展无关 |
| P4 (多 shader) | 部分相关 | `ShaderRegistry` 是 CPU 选 shader；execution-set token（per-sequence 换 shader）是后续优化，且 RADV graphics 侧不支持，P4 阶段做不出跨驱动价值 |
| P5 (透明排序 + 光照) | 否 | 透明排序按设计在 CPU（"排序结果暂存，renderSystem 按排序顺序渲染"） |
| **P7 (延迟渲染)** | **是，主战场** | roadmap 明确 "AABB / Frustum Culling：延迟渲染的前提优化" 与 "Light culling (frustum + AABB)"——GPU-driven culling → indirect draw 是本扩展的教科书场景；且届时 P2 多 mesh + P4 多 shader 基建就位，per-sequence shader 选择可用 |

### 4.3 集成设计要点（P7 落地时）

**Layout（token 序列，graphics 侧 mesh 路径；实施值 2026-09-09）**：

```
[0]  EXECUTION_SET   (shader object 模式, stages = MESH|FRAGMENT)  → 数据: u32[] 每 stage-bit 一个索引（LSB→MSB：@0=frag slot, @4=mesh slot，共 8B）
[8]  PUSH_DATA       (offset 对齐 4)                              → 数据: RGL::DrawArgs（**现 4B = 仅 objectIndex**；2026-09-10 bindless 化前是 100B 超集、逐序列带 MVP/材质参数——非 64B，超集含 baseColor/rm/objectIndex。@8..108 槽区 100B 宽度不变，余 96B 空置）
[108] DRAW_MESH_TASKS (action token, 必须最后)                    → 数据: VkDrawMeshTasksIndirectCommandEXT
indirectStride = 120B（≥ 各 token 数据总长；NVIDIA 无上限；RADV 上限 2048B）
```

**Execution set（shader object 模式）**：`vkCreateIndirectExecutionSetEXT{type=SHADER_OBJECTS, info.pShaderInfo{pInitialShaders, maxShaderCount=min(条目数×2, maxIndirectShaderObjectCount)}}`；**每个管线条目占 2 个 slot（2j=frag, 2j+1=mesh）**——`VkWriteIndirectExecutionSetShaderEXT` 只有 `{index, shader}` 无 stage 字段，唯一 index（VU）约束下按"每 stage-bit 一位"排布；`pipelineLayout = VK_NULL_HANDLE`（descriptor heap 已启用）。注意：shader object 的 binding mapping（`VkDescriptorSetAndBindingMappingEXT`）仍按 `shader.cppm` 现有方式在各 shader 创建时设置，execution set 不改变这一点。

**帧流程**（culling → 执行，实施版）：

```
① CPU 预写：seqStaging（MAPPED ring×2）逐 candidate 写 execSlot(2j/2j+1) + DrawArgs(4B objectIndex；mvp 等改走 scene data per-object 表) + meshCmd(x=groupCount)
   → staging→sequence 拷贝（cull pass 内）
② culling compute dispatch（cull.comp.slang，push 100B = planes[6]+sequenceCount）
   输入: CullEntry 表 + 对象矩阵表（全量刷新，见 devlog 矩阵表陈值 bug）
   输出: 序列 meshCmd.x（可见=groupCount / 不可见=0）+ count buffer（total@4；@0 保留未用）
③ barrier: COMPUTE_SHADER → COMMAND_PREPROCESS（eCommandPreprocessEXT）
④ vkCmdPreprocessGeneratedCommandsEXT（sequenceCountAddress=0 → 用 maxSequenceCount）
⑤ render pass 内 vkCmdExecuteGeneratedCommandsEXT（VK_TRUE, sequenceCountAddress=count BDA+4）→ 设备端发出 N 个 mesh draw
```

**CPU 回退路径（RADV / 未来其他驱动）——读回重放（勘误 2026-09-09，取代原 IndirectCount 方案）**：
`vkCmdDrawMeshTasksIndirectCountEXT` **不可行**：该命令无 per-draw push data 机制——所有 indirect
draw 共享当前 push region，mesh shader 拿不到 per-draw DrawArgs（mvp/objectIndex 等），而项目
push constant 契约要求每 draw 独立 DrawArgs。改法 = **读回重放**：culling compute 后 CPU 读回
count + 序列可见性（meshCmd.x），逐可见 candidate 重放 `vkCmdPushDataEXT` + `vkCmdDrawMeshTasksEXT`
（与 forward 现有提交路径逐位一致 → BIT-IDENTICAL 天然成立）。两路径共享同一 culling compute
与同一序列布局（CPU args + GPU 可见性），只换提交方式。

**能力门控**：`DeviceContext::queryDgcSupport()` 派生 `DgcSupport{graphicsIndirect,
computeIndirect, maxIndirectShaderObjectCount}`（graphicsIndirect = `shaderStagesShaderBinding`
与 MESH|FRAGMENT 交集非空），`vkrenderer` 据此选 DGC execute / 读回重放；culling compute
本身三驱动（RADV/NVIDIA/anv）都跑（默认路径，GPU culling 无配置开关）。

### 4.4 实施记录（2026-09-09，v357 系统头 + 本项目落地）

**v357 API 面比本文原记录（1.4.341）窄**（字段真相源 = 系统头 `/usr/include/vulkan/vulkan_core.h`）：

| 差异 | 原记录（1.4.341） | v357 实际 |
|---|---|---|
| execution set 绑定 | `vkCmdBindIndirectExecutionSetEXT` | **不存在**——经 `VkGeneratedCommandsInfoEXT.indirectExecutionSet` 传 |
| sequence 地址 | `vkCmdSetGeneratedCommandsSequenceAddressEXT` | **不存在**——`info.indirectAddress/indirectAddressSize` |
| count buffer | `vkCmdSetGeneratedCommandsSequenceCountBufferEXT` | **不存在**——`info.sequenceCountAddress`（BDA；0 → 用 maxSequenceCount） |
| `VkWriteIndirectExecutionSetShaderEXT` | — | `{sType, pNext, index, shader}` **无 stage 字段** → 扁平 2j/2j+1 排布 |
| EXECUTION_SET token 数据 | "u32 slot 索引" | **u32[] 每 stage-bit 一位，LSB→MSB**（MESH|FRAGMENT → @0=frag, @4=mesh） |

**loader 不导出 DGC 符号**：系统 `libvulkan.so`（1.4.357）无 `vkCreateIndirectCommandsLayoutEXT`
等任何 DGC 全局符号 → **8 个入口点全走 `vkGetDeviceProcAddr`**（PFN 模式同
`texture_upload` 的 `writeResourceDescriptorsEXT` 先例）；BDA 用 core `vkGetBufferDeviceAddress`。

**BIT-IDENTICAL 设计约束**（forward diff0 硬约束推导）：mvp 必须 **CPU 算进序列**（GPU 的
float4x4 乘法 FMA 收缩序与 glm 无位级契约）——GPU 只覆盖可见性（meshCmd.x）+ count；
DGC 路与重放路消费同一序列。详见 `phase7_devlog.md` 2026-09-09 条目"设计修正"。

---

## 五、风险与成本

- **驱动不对称是最大风险**：graphics 路径 NVIDIA-only；RADV 跟进（其 `supportedIndirectCommandsShaderStages` 全集已含 mesh/fragment，只是 binding 子集未开）后能力位自动生效，无需改代码；
- **2026-09-10 补（静态模式）**："RADV graphics 不可用"对**静态 shader 模式**不成立——layout 去
  EXECUTION_SET token + pNext `VkGeneratedCommandsShaderInfoEXT` 只受全集约束（VU-10241/11080），
  RADV 执行路径已实现；本机 spike 功能石锤通（VVL 零 error + 1px tie 级回归，见
  `docs/archive/radv_static_dgc_spike.md`），但满槽孔洞实现 preprocess ×N 导致 **慢重放路 1.85×** →
  默认不开（`diagnostics.dgc_static` 门控保留）；翻盘前提 = 紧凑分桶 + 去同步等待。
- **扩展年轻**：EXT rev 1，VVL 校验覆盖未验证（引入后需留意静默通过，参照 `docs/nvidia_compatibility_report.md` 第五节 VVL 静默教训）；
- **preprocess buffer**：新增一块 device-local buffer（大小由 `vkGetGeneratedCommandsMemoryRequirementsEXT` 决定，与 maxSequenceCount 成正比），VMA 分配即可；
- **execution set 改动**：材质增删后必须重查 memory requirements 并重建 preprocess buffer（低频，场景加载时一次性）；
- **RADV 小限制**：stride 2048B 上限要求 token 布局紧凑（本项目 3 token + 64B push data 无压力）；
- **学习曲线**：preprocess/execute 两阶段 + 新 stage/access，是本扩展相对 indirect draw 的核心新东西，与项目"现代特性学习"定位一致。

---

## 六、引入步骤（计划随 Phase 7，已写入 `docs/next_roadmap.md`）

**全部完成（2026-09-09，Phase 7b 批次 5）**：1-6 均已落地，实际实现与 §4.4 勘误一致
（PFN 解析、读回重放取代 IndirectCount、2j/2j+1 execution set 排布）。

| 步骤 | 内容 | 验收 |
|---|---|---|
| 1. 重新启用扩展 + feature chain | `features.cppm` 两处列表加回 `VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME`（2026-08-22 已移除，见 4.1）；`features_config.json` 加 `vk::PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT` 到 `chain_types` + `deviceGeneratedCommands: true`（不启用 `dynamicGeneratedPipelineLayout`）→ `just build` 经 `reflection_tool render` 重新生成 `features_generated.cppm` | 双 GPU 启动通过，`validateFeatures` 不报错 |
| 2. 属性查询 + 能力位 | 按 `vkresources.cppm:123` 的 `getProperties2<..., vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT>()` 模式在 `DeviceContext` 查询并缓存；派生 `supportsGraphicsIndirectCommands` / `supportsComputeIndirectCommands` | 启动日志输出两 GPU 的能力位（RADV: compute only / NVIDIA: full） |
| 3. `vk_indirect_commands.cppm` 模块 | layout + shader object execution set + preprocess buffer（VMA）RAII 封装；录制 helper（preprocess/execute + sync2 barrier）；C++20 module，`import rlstd/rllogger/vkerror` | 单元测试性场景：固定 4 序列执行 4 个 draw，VVL 零错误 |
| 4. culling compute（Slang） | 视锥 + AABB culling，输出 per-sequence 数据 + count（ByteAddressBuffer 输入输出，遵守 `docs/技术概念.md` 缓冲访问规则） | 静态场景 culling 结果正确（CPU 对照） |
| 5. 接入渲染循环 + 回退 | 能力位选路：NVIDIA 走 preprocess/execute；RADV 走 `vkCmdDrawMeshTasksIndirectCountEXT`；同一 culling 输出 | 双 GPU 多 mesh 场景渲染正确，FPS 对比记录 |
| 6. 文档 | 更新 AGENTS.md（隐式依赖条目 + 新模块表）；culling 排坑记入 `docs/调试历史.md`（如有） | — |

---

## 七、离线 Vulkan 文档（2026-08-22 建立，优先于在线检索）

`docs.vulkan.org` / `registry.khronos.org` 有 Cloudflare 拦截，在线抓取不可靠。已在本地磁盘克隆官方 spec 源仓库：

**位置**：`/home/robin/program/Vulkan-Docs`（本地 NVMe，shallow clone，约 37MB；更新：`git pull` 或重新 `git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Docs.git`）

| 内容 | 路径 | 用途 |
|---|---|---|
| 规范正文（含全部扩展章节） | `chapters/`（如 `chapters/device_generated_commands/generatedcommands.adoc`） | 语义 + VU 条文，可 grep |
| 权威 registry | `xml/vk.xml` | 扩展依赖表（`<extension ... depends="...">` / `<require depends="...">`）、feature/limit/token 元数据——**核查"扩展 X 是否依赖 Y"以此为准** |
| 扩展附录（元数据/贡献者/版本史） | `appendices/*.adoc`（约 492 个，含 VK_EXT_ 153 / VK_KHR_ 150 / VK_NV_ 69） | 扩展概况 |
| 设计提案 | `proposals/` | 未合入/新扩展的设计讨论 |
| registry schema 说明 | `registry.adoc` | XML 字段语义（`depends` 表达式语法等） |

本文的 4.1"隐式依赖"定论即出自 `xml/vk.xml:30852`（扩展块 `depends` 属性）+ 全文反向查询。

---

## 八、参考资料

- 规范正文（权威语义 + VU）：本地 `/home/robin/program/Vulkan-Docs/chapters/device_generated_commands/generatedcommands.adoc`（EXT 与 NV 两节并存，含 token 处理伪代码）
- 扩展依赖权威来源：本地 `/home/robin/program/Vulkan-Docs/xml/vk.xml`（registry，`depends` 属性）
- 本地头文件：`/usr/include/vulkan/vulkan_core.h:23946-24011`（扩展块：enums/features/properties）、`:24020-24150`（structs）、`:24165-24235`（入口点）、`:7091/:2641-2642/:7176-7177`（COMMAND_PREPROCESS stage/access）
- C++ wrapper：`vulkan.hpp:7416-7469`（raw 命令）、`:5806/:28286`（`vk::Device` 方法）、`:11118`（RAII）
- 驱动实测：`vulkaninfo`（2026-08-22 本机双 GPU 基线 + 2026-09-24 Mesa 26.2.3 复核；现 RADV 无条件 advertise `descriptor_heap` 与 `device_generated_commands`，无需 `RADV_EXPERIMENTAL`）
- 相关项目文档：`docs/nvidia_compatibility_report.md`（双驱动差异排查史）、`docs/DESCRIPTOR_HEAP_SPEC.md`（heap 排坑）、`docs/技术概念.md`（sync2 / push data / 缓冲访问规则）、`docs/next_roadmap.md` Phase 7
