# RADV 静态 shader DGC spike（计划）

> **状态**：历史（spike 完结 2026-09-10：**功能验证通过、满槽实现性能净亏 → 定案默认不开**，
> 门控 `dgc_static` 保留可手动启用；定案见 §8.4，排坑记录已入 `git:调试历史`。
> 性能翻盘前提 = 紧凑分桶 + 去同步等待，届时以本文档为设计底稿重启）。

## 1. 背景

Phase 7b 起 DGC 双路：NVIDIA（`graphicsIndirect=true`）走 `vkCmdExecuteGeneratedCommandsEXT`
IES 路（EXECUTION_SET token 逐序列换 shader object）；AMD/Intel 因
`supportedIndirectCommandsShaderStages{Pipeline,Shader}Binding` 仅含 COMPUTE 而走**读回重放**
（cull 提交 → 同帧同步等待 → CPU 逐 candidate 重放 draw）。

2026-09-10 调研（本机 Strix Halo 8060S，RADV 26.2.1 + Mesa main 源码 + Vulkan-Docs v1.4.360）
推翻了"AMD 不可用"的笼统结论：**RADV 的 graphics DGC 执行路径是实现了的**，缺的只有
per-sequence shader 绑定；spec 允许"每次 Execute 静态 shader"模式绕开该缺口。

## 2. 证据链

### 2.1 驱动侧（Mesa main，本机 26.2.1 同源）

| 事实 | 位置 |
|---|---|
| graphics DGC 全套预处理：DRAW/DRAW_INDEXED/**DRAW_MESH_TASKS(_COUNT)**、GFX IB2 链、task shader ACE 双队列、GFX11+ mesh direct dispatch | `src/amd/vulkan/radv_dgc.c`（3656 行，2024 起长期维护） |
| execute 非 compute 分支 = 真 graphics draw / taskmesh draw，shader 从 pNext `VkGeneratedCommandsPipelineInfoEXT`/`VkGeneratedCommandsShaderInfoEXT` 读（`radv_dgc_get_shader(pipeline_info, eso_info, …)`） | `radv_cmd_buffer.c:14876+` |
| mesh x=0 显式跳过（no-op draw 原生支持） | `radv_dgc.c: dgc_emit_draw_mesh_tasks_gfx`（`nir_push_if(x≠0 ∧ y≠0 ∧ z≠0)`） |
| binding 子集硬编码 `COMPUTE`、`graphicsPipelineBinding` 未置位（无 drirc 例外）= "未放行"而非"未实现" | `radv_physical_device.c:2358-2359` |

### 2.2 spec（v1.4.360 `chapters/device_generated_commands/generatedcommands.adoc`）

- binding 子集**只约束 layout 里的 per-sequence PIPELINE/SHADER/EXECUTION_SET token**；
- layout 不含 EXECUTION_SET token 时，`indirectExecutionSet` 必须为 NULL（VU-11011），
  且 pNext **必须**携带静态 `VkGeneratedCommandsPipelineInfoEXT` 或
  `VkGeneratedCommandsShaderInfoEXT`（VU-11012）——**无子集要求**；
- layout 的 `shaderStages` 只要求 ⊆ `supportedIndirectCommandsShaderStages` **全集**
  （RADV = 14 stage 含 MESH|FRAGMENT ✅），与 binding 子集无关。

### 2.3 本机实测

```
Device DGC support: graphicsIndirect=false computeIndirect=true
supportedIndirectCommandsShaderStages: count = 14（含 TASK|MESH）
…PipelineBinding / …ShaderBinding: count = 1（仅 COMPUTE）
```

## 3. 方案：per-entry 满槽孔洞流 + 静态 shader pNext + N× Execute

DGC 的唯一不可替代点 = "一次 Execute 内逐序列换 shader"。绕开它 = **每个管线条目一次
Execute**（shader 静态），序列内只携带索引类数据。

### 3.1 layout（共享，2 token；对比 NV 3 token 去掉 EXECUTION_SET）

```
indirectStride = kCullSequenceStride = 120（gpu_cull 单一真相源，不动）
@8   PUSH_DATA（DrawArgs 4B = objectIndex，updateRange.shaderStages = ALL——NV 同款）
@108 DRAW_MESH_TASKS（x=0 no-op）
shaderStages = MESH|FRAGMENT；pipelineLayout = NULL（descriptor heap）
```

### 3.2 序列流：**满槽孔洞（holey）**而非紧凑分桶

每条目一条与基础序列等长的 120B 步长流（N × candidateCount × 120B）：

- 流 j 的槽 i：candidate i 可见且 `pipelineIndex==j` → 完整 120B 行（DrawArgs+meshCmd）；
  否则 meshCmd.x=0 孔洞。
- **条目内顺序 = candidate 序 = CPU entity 迭代序（确定性）**——原子分桶会破坏该契约
  （NV IES 路正是靠固定槽保序）；紧凑分桶（两级稳定 scan）留作后续优化，spike 不做。
- sequenceCountAddress 复用 count buffer @4（= candidate 数，全条目同一个数）——
  **cull 主 kernel 一字节不改**。
- 代价：preprocess 工作量 ×N、序列显存 ×N（bistro N≈数十、1591 候选 → ~MB 级，可接受）。

### 3.3 fanout kernel（新增 `shaders/stream_fanout.comp.slang`）

cull dispatch 之后追加一个 compute dispatch（仅静态模式；普通模式连 shader 都不建）：

```
grid = (ceil(cand/64), N, 1)；thread (i,j)：
  table@20 pipelineIndex ≠ j 或 sequences[i].meshCmd.x == 0 → 流[j][i] 写 12B 零（x=0）
  否则 → 流[j][i] ← sequences[i] 拷 @8..120（DrawArgs 槽区 100B + meshCmd 12B；DrawArgs 本体 4B，@12..107 空置）
binding：0 = cullTable(ro) / 2 = sequences(ro) / 新增 4 = streams(rw)（allocSlot + heap 映射）
```

独立 kernel（不改 cull kernel）= NV/重放路**零回归风险**（diff0 维持）。

### 3.4 新类 `DgcStaticCommands`（`vk_indirect_commands` 同模块 + impl partition）

- 构造：layout（①1 个共享）→ memReq（**IES=NULL + pNext 挂条目 0 的 ShaderInfo**，
  VU-11012 对查询同样生效；各条目同尺寸按 max 取）→ preprocess buffer
  **N×framesInFlight 槽**（VU-11142：每次 execute 独占区，槽号 = `entry×FIS + frameSlot`）
  → 缓存逐条目 {frag,mesh} `VkShaderEXT`（`entries[j].pipeline.{frag,mesh}ShaderHandle()`）。
- `recordExecute(cmd, entryIndex, streamAddress, countAddress, frameSlot)`：
  `VkGeneratedCommandsInfoEXT` 栈上 pNext 挂该条目
  `VkGeneratedCommandsShaderInfoEXT{[frag,mesh]}`；VU-11053（execution set 初始态）不适用，
  **但 execute 前仍须绑 shader 态**——VVL 实测（§8）VU-11154/11155 要求 CB 上
  graphics shader 已绑且 mesh 管线的 vertex/task 显式 `VK_NULL_HANDLE` 绑过，
  实现 = 每次 execute 前 `bindShadersEXT{vertex:null, task:null, frag, mesh}` 四阶段全绑
  （NV 驱动不校验该条，故 IES 路仅按 11053 绑初始态）。
- 构造失败抛异常 → main catch 回落重放（NV 路同款契约）。

### 3.5 选路（能力位三级）

```
DgcSupport += staticGraphicsIndirect = supportedIndirectCommandsShaderStages(全集) 含 MESH
graphicsIndirect           → IES 路（NV，不动）
staticGraphicsIndirect 否则 → 静态路（本 spike；S1/S2 期由 diagnostics.dgcStatic 门控，
                              S3 验证过后默认开、diagnostics 提供关闭开关）
两者皆无                    → 读回重放（现状兜底，不动）
```

帧内其余时序（cull CB 提交→同帧等待→读回→对账）**全部保留**——去同步等待是独立优化
（NV 路同样没去），明确不入本 spike 范围。

## 4. 文件级改动清单

| 文件 | 改动 | 阶段 |
|---|---|---|
| `rgl/vkdevice.cppm` | `DgcSupport` + `staticGraphicsIndirect`（`queryDgcSupport` 判全集含 MESH；NV 也 true 但优先级低于 IES 路） | S0 |
| `rgl/diagnostics.cppm/_impl.cppm` | `+dgcStatic`（默认 false）+ Lua 字段 `dgc_static` | S0 |
| `rgl/vk_indirect_commands.cppm/_impl.cppm` | `+DgcStaticCommands`（§3.4） | S1 |
| `rgl/shaders/stream_fanout.comp.slang` | 新增（§3.3，手写偏移契约，无反射管线，`just build` 即编） | S1 |
| `rgl/gpu_cull.cppm/_impl.cppm` | ctor +`entryStreamCount`（0=关）：streams buffer 分配 + heap 槽 + fanout shader object + `recordCullPass` 尾段追加 dispatch + `streamsAddress()` 访问器 | S1 |
| `rgl/cull_driver.cppm/_impl.cppm` | ctor +`DgcStaticCommands*`；`executeOpaqueSegment` 三路分发 | S1 |
| `rgl/vk_renderer.cppm/_impl.cppm` | ctor 透传 `DgcStaticCommands*` | S1 |
| `rgl/main.cpp` | 条件构造 `DgcStaticCommands` + `staticGraphicsIndirect` 选路日志 | S1 |
| `configs/diagnostics.lua` | `dgc_static = true`（仅 build 副本，S2 验证期） | S1 |
| `rgl/CMakeLists.txt` | fanout shader 登记（如 compile_slang 需显式列表） | S1 |
| S2/S3 | 无新代码；回归 + 默认开 + 文档 |  |

## 5. 验收标准

- **S0**：`just build` 通过；启动日志打印 `staticGraphicsIndirect=true`；行为零变化。
- **S1**：build 副本 `dgc_static=true` → headless 冒烟（`timeout 8`）
  **VVL 开启、零 `[error]`/`Validation Error`**；llm_debug 截图目测正确（bistro 场景完整、
  无缺块/黑块）；日志 `DGC path: … -> DGC static execute (×N entries)`。
- **S2**：同会话双构建截图回归（重放路 vs 静态路，方法学见 AGENTS"截图回归方法学"）——
  预期**近似一致**：跨条目 tie 深度处可能 ±像素（静态路 draw 序 = 条目优先，重放路 =
  全局 candidate 序），报告差异像素占比与分布；相机移出场景（count=0）无故障、多帧
  稳定；reconcile 对账持续通过。
- **S3**：默认开（AMD）；NV 代码路径零改动（本机无 NV，靠路径隔离 + 代码评审保证）；
  文档更新（AGENTS 已知问题 DGC 条目改写、research 文档 §3.3 补"静态模式可用"、
  `git:调试历史` 新增条目、`next_roadmap.md` Phase 7b 行）。

## 6. 风险与回退

| # | 风险 | 应对 |
|---|---|---|
| 1 | RADV 的 **ShaderInfo（shader object）静态 pNext 路**无原生 Vulkan 先例（vkd3d 走的多是 pipeline 路），可能踩驱动洞 | spike 的意义即暴露之；洞大则记录复现回落重放路（三路结构保证回退零成本）。Plan B `VkGeneratedCommandsPipelineInfoEXT` 需建 `VkPipeline`（大手术），不做 |
| 2 | 隐式 preprocess（`isPreprocessed=FALSE`）在 RADV 的行为差异（`radv_prepare_dgc` 内部 suspend/flush） | VVL + 冒烟；必要时试显式 preprocess 两段路 |
| 3 | gfx1151（Strix Halo）DGC 覆盖度（RADV CI 以 gfx10/11 为主） | 断言/挂死则记录 `RADV_DEBUG` 输出、回落；`timeout` 包裹冒烟 |
| 4 | sequenceCount=0 边界（`radv_use_dgc_predication`） | S2 专测相机移出场外 |
| 5 | preprocess 内存 memoryTypeBits 严格匹配（NV 路已有先例代码可抄） | 复用 DgcCommands 分配段模式 |
| 6 | 性能回退（N× preprocess、×N 序列写） | spike 不追求性能；记录帧时间对比数据供后续决策 |

## 7. 明确不做（本 spike 边界）

去 cull 同步等待（NV/AMD 通用优化，独立立项）；紧凑分桶（破坏序确定性，需配套回归）；
anv/Intel 适配；deferred/transparent 段；`graphicsPipelineBinding` 期待（等 RADV 放行后
IES 路自动激活，能力位已数据驱动）。

## 8. 验证结果（2026-09-10，本机 Strix Halo 8060S / RADV 26.2.1）

**结论：方案成立，S1/S2 全过，VVL 零 error，渲染与读回重放路近似 BIT-IDENTICAL。**

### 8.1 运行数据（bistro 主场景）

| 项 | 实测值 |
|---|---|
| 管线条目数 N | **262**（1591 primitive → (风格,顶点 slot,纹理 slot) 组合去重后） |
| preprocess buffer | 121600B × 524 槽（262 条目 × 2 帧）≈ **64MB** |
| per-entry 流 buffer | 262 × 190920B ≈ **50MB** |
| 帧内 Execute 次数 | 262（每次序列 = 1591 × 120B 满槽孔洞流） |

### 8.2 验收对照

- **VVL 零 error**：开 VVL headless 多帧运行，`vkCmdExecuteGeneratedCommandsEXT` 无告警
  （踩坑：shader 态绑定要求见 §3.4 修正——vertex/task 需显式 null 绑，NV 驱动不校验此条）。
- **截图回归（同二进制 toggle `dgc_static`，比双构建更严格）**：1280×720 全图 diff =
  **恰好 1 像素、±1 LSB**（(698,415)，R 通道 103 vs 102）——即 §5 预期的跨条目
  tie 深度序差（静态路 draw 序 = 条目优先，重放路 = 全局 candidate 序）。
- **全剔边界**：相机移出场景反方向（1591 候选全不可见、序列全孔洞）零 error，
  天空盒正确保留（不入候选）。
- **多帧稳定**：8s headless 循环零 error；baseline（dgc_static=false 重放路）零回归。

### 8.3 性能 A/B（diagnostics.frame_stats=30，headless 1280×720，同机同二进制 toggle）

| 路 | avg 帧时间 | 备注 |
|---|---|---|
| 读回重放（基线） | **~5.9–6.2ms** | CPU 逐 visible drawMeshTasksEXT 重放 |
| 静态 DGC（满槽孔洞） | **~10.0–11.4ms** | **慢 ~1.85×** |

根因 = 满槽设计把 preprocess 工作量放大到 N×candidateCount（262 × 1591 ≈ 417k 序列/帧）
+ 262 次 preprocess dispatch + IB2 链 + fanout 417k 行写；而重放路的 CPU 端在 RADV 上
本来不贵（帧内同步等待两路同担、未成差异项）。**两路均未做去同步等待**，对比公平。

### 8.4 定案

- **功能与正确性：成立**（VVL 零 error、近似 BIT-IDENTICAL、全剔/多帧稳定）。
- **性能：满槽实现净亏 → 默认不开**——`dgc_static` 保持 diagnostics 门控（默认 false），
  默认路仍 = 读回重放。
- 性能翻盘的前提 = **紧凑分桶**（preprocess 量 ∝ visible 而非 N×candidateCount；
  需两级稳定 scan 保条目内 candidate 序）+ **去 cull 同步等待**（独立立项），届时再评默认开。
- 本 spike 资产保留：`DgcCommands`/`DgcStaticCommands`/`stream_fanout`/三路分发/
  `staticGraphicsIndirect` 能力位 + `dgc_static` 门 + `frame_stats` 测量门。
