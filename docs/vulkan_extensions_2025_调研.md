# 2025+ Vulkan 新扩展调研（结合 vulkan_practice 项目）

> **状态**：调研（2025-01→2026-08 新扩展全景 + 潜力候选评估级分析；补充 Vulkan 1.1–1.3 时代有价值扩展。不落地代码，仅作引入决策依据）

**日期**：2026-09-02
**环境**：本地 Vulkan-Docs `v1.4.360`（commit 2026-08-14，`/home/robin/program/Vulkan-Docs`）
**权威来源**：`xml/vk.xml`（registry，扩展状态 ratified/provisional）+ `ChangeLog.adoc`（1.4.305→1.4.360 逐版本 New/Extensions 段）+ `proposals/*.adoc`（语义）+ `appendices/*.adoc`（版本史）。注意 spec clone（v1.4.360）比系统头/loader（**v341**，主工程实际编译环境）**新**，写代码以系统头为准，本文版本/状态以本地 spec 为准。

---

## 一、背景与方法

### 1.1 范围

- **主体**：2025-01-17（Vulkan 1.4.305）→ 2026-08-14（1.4.360）间 registry 新增的 **87 个扩展**（含本项目核心 `VK_EXT_descriptor_heap`，见 §1.3/§2 注）。
- **补充（Part B）**：Vulkan 1.1–1.3 时代（2024 前）引入、**本项目尚未采用但对本项目轨迹有价值**的扩展——因本调研的资料库对 2024 前扩展更可靠，可考据后发挥，已与本地 registry 核对状态。

### 1.2 项目画像（评估基准，见 AGENTS.md）

- **架构**：mesh shader（`VK_EXT_mesh_shader`）+ `VK_EXT_shader_object` + **`VK_EXT_descriptor_heap`（核心）** + `VK_EXT_dynamic_state`（经 shader object）；scalar block layout；BDA；timeline semaphore；dynamic rendering + sync2。
- **渲染**：forward（opaque/skybox/transparent 三段）→ **Phase 6**（Lua 渲染意图 / RenderGraph 多 pass）→ **Phase 7**（延迟渲染 G-buffer + light pass + **GPU culling `VK_EXT_device_generated_commands`** + shadow）。
- **资产**：glTF（tinygltf）+ 内嵌纹理（4096²，cottage 257MB / 392 primitive / 11 张 4K）+ 环境 cubemap（IBL 简化版）+ 天空盒。
- **驱动**：AMD Strix Halo（RADV 核显，`RADV_EXPERIMENTAL=heap`）/ NVIDIA（prime offload）/ Intel Arc A310（anv）——**三驱动共存**，任何扩展必须"不支持时干净 no-op"。
- **历史痛点**：GPUVM TCP fault（UAF 根因，已解）、RADV 4K 纹理高 VA fault（UAF 复用，已解）、eImmediate 无 pacing 导致 GPU 满占用（present_timing_research.md §八）。

### 1.3 关键元发现

1. **`VK_EXT_descriptor_heap`（本项目核心依赖）已于 1.4.340（2026-01-23）ratified/正式化**——项目一直构建在 bleeding-edge 扩展上，现该扩展转正，生态稳定性提升。
2. 项目已用的 `VK_KHR_swapchain_maintenance1` 同期（1.4.321）新增姊妹扩展 `VK_KHR_surface_maintenance1`；`VK_EXT_descriptor_buffer`（已用）正被 `VK_EXT_descriptor_heap` 取代（changelog 多处标记 superseded）。
3. **maintenance9/10/11 含直接相关位**：`maintenance11` 的 `VK_PIPELINE_LAYOUT_CREATE_NO_TASK_SHADER_BIT_KHR`（mesh shader task 绑定，呼应 `min_mesh_crash` 的 `NO_TASK_SHADER` 坑）、`maintenance10` 的 resolve transfer-function 控制（sRGB MSAA resolve）、`maintenance9` 的 `VK_DEPENDENCY_ASYMMETRIC_EVENT_BIT_KHR`。

### 1.4 筛选判据

| 维度 | 判据 |
|---|---|
| 架构契合 | 与 mesh+shader-object+descriptor-heap+forward→deferred 路线有明确接缝 |
| 路线契合 | 服务于 Phase 6（多 pass/RenderGraph）或 Phase 7（compute/culling/MSAA/shadow）或既有痛点（纹理上传/延迟/内存诊断） |
| 驱动可得 | 至少一目标驱动（RADV/NVIDIA/anv）可支持；双/三 GPU 要求"不支持时干净 no-op" |
| 成本收益 | 评估级：关联点 + 集成点定位 + 工作量/风险 + 建议（不展开逐文件改动步骤） |

---

## 二、2025+ 新扩展全景表（87 个）

状态：`R`=ratified（已批准，稳定）/ `P`=provisional（草案）/ `X`=vendor experimental。"首现"= 该扩展首次进入 registry 的 spec 版本。

> **核心依赖注**：`VK_EXT_descriptor_heap`（本项目架构基石）亦属本批 87 个之一——**1.4.340（2026-01-23）ratified/正式化**（此前为 provisional）。因其为项目核心且已深度使用，单列于 §1.3/§8 元发现，不重复计入下列分类表。

### 2.1 T1 高潜力（本项目可近期评估）

| 扩展 | 首现 | 状态 | 一句话相关性 |
|---|---|---|---|
| `VK_EXT_zero_initialize_device_memory` | 1.4.315 (2025-05) | R | 分配期显式零初始化 + 新 `ZERO_INITIALIZED` image layout，直击本项目 UAF/未初始化内存痛点 |
| `VK_KHR_device_fault` | 1.4.347 (2026-03) | R | 统一设备故障上报（含未致 device-lost 的 masked fault）+ 阻塞查询 + GPU VA 解读，补 GPUVM/Aftermath 诊断史 |
| `VK_KHR_depth_clamp_zero_one` | 1.4.305 (2025-01) | R | 硬件深度钳制到 [0,1]，项目已用 glm 软件强制 zero-to-one，可改硬件路径；与天空盒 z=1 交互 |
| `VK_EXT_present_timing` | 1.4.335 (2025-11) | R | present 分阶段时间戳 + 调度；**已有** `present_timing_research.md`（决策"暂不引入"），2025+ ratified 可重评 |

### 2.2 T2 近期路线（Phase 6/7 相关）

| 扩展 | 首现 | 状态 | 一句话相关性 |
|---|---|---|---|
| `VK_KHR_unified_image_layouts` | 1.4.317 (2025-06) | R | 全用 `VK_IMAGE_LAYOUT_GENERAL` 等价最优，免 image layout 迁移——Phase 6/7 多 pass 同步简化 |
| `VK_EXT_custom_resolve` | 1.4.333 (2025-11) | R | shader 化 resolve（tile 高效）+ 动态渲染 + 异格式 resolve——Phase 7 G-buffer 若加 MSAA |
| `VK_KHR_copy_memory_indirect` | 1.4.328 (2025-09) | R | GPU 侧读参数间接拷贝 buffer/image——Phase 7 GPU-driven 场景数据 staging 下沉 |
| `VK_EXT_shader_subgroup_partitioned` | 1.4.340 (2026-01) | R | compute subgroup 分区——Phase 7 light pass / culling compute |
| `VK_EXT_memory_decompression` | 1.4.330 (2025-10) | R | GPU 解压 GDeflate 资产——大模型（257MB cottage）减少 CPU 解压瓶颈 |
| `VK_KHR_internally_synchronized_queues` | 1.4.340 (2026-01) | R | 队列内部同步，免应用侧互斥——Phase 6/7 多线程提交 |
| `VK_KHR_robustness2` | 1.4.314 (2025-05) | R | 鲁棒性增强（越界/未初始化访问）——呼应本项目越界 bug 史 |

### 2.3 maintenance 扩展（含相关位）

| 扩展 | 首现 | 状态 | 相关位 |
|---|---|---|---|
| `VK_KHR_maintenance8` | 1.4.305 (2025-01) | R | 一般清理（`vkCmdWaitEvents2` 等 VU 合并） |
| `VK_KHR_maintenance9` | 1.4.317 (2025-06) | R | `VK_DEPENDENCY_ASYMMETRIC_EVENT_BIT_KHR`（非对称事件依赖）、`VK_QUERY_POOL_CREATE_RESET_BIT_KHR` |
| `VK_KHR_maintenance10` | 1.4.330 (2025-10) | R | `VK_ATTACHMENT_DESCRIPTION_RESOLVE_SKIP/ENABLE_TRANSFER_FUNCTION_BIT_KHR`（resolve 是否套 sRGB EOTF） |
| `VK_KHR_maintenance11` | 1.4.350 (2026-05) | R | `VK_PIPELINE_LAYOUT_CREATE_NO_TASK_SHADER_BIT_KHR`（mesh task 绑定）、`VK_SHADER_CREATE_INDEPENDENT_SETS_BIT_KHR` |

### 2.4 T3 厂商特定（项目跑 AMD+NVIDIA+Intel）

| 扩展 | 厂商 | 首现 | 状态 | 相关性 |
|---|---|---|---|---|
| `VK_AMD_gpa_interface` | AMD | 1.4.351 (2026-05) | P | AMD GPA session（GPU 性能分析采样）；Strix Halo 可探测 |
| `VK_AMDX_dense_geometry_format` | AMD | 1.4.324 (2025-08) | X | AMD 稠密几何压缩格式；mesh shader 工作流适配待验证 |
| `VK_NV_present_metering` | NV | 1.4.308 (2025-02) | P | NV 帧 pacing/metering，`present_timing` 的 NV 侧互补（offload 路径） |
| `VK_NV_push_constant_bank_support` | NV | 1.4.340 (2026-01) | NV | NV push constant bank；本项目 push constant 走 `DrawArgs` 超集 |
| `VK_NV_compute_occupancy_priority` | NV | 1.4.336 (2025-12) | NV | compute 占用率优先级；Phase 7 compute |
| `VK_NV_cooperative_vector` | NV | 1.4.307 (2025-01) | NV | NV 协同向量（ML 向）；低相关 |
| `VK_NV_external_compute_queue_vendor` | NV | 1.4.312 (2025-04) | NV | NV 外部 compute 队列；低相关 |
| `VK_KHR_swapchain_maintenance1` | KHR | 1.4.321 (2025-07) | R | **项目已用**（features_config 含 `SwapchainMaintenance1KHR`）；同期新增 `surface_maintenance1` |

### 2.5 T4 不相关 / 低相关（简列，按类）

**ML / AI / 张量**：`VK_ARM_tensors`(1.4.317) `VK_ARM_data_graph`(1.4.319) `VK_ARM_data_graph_optical_flow`(1.4.349) `VK_ARM_data_graph_neural_accelerator_statistics`(1.4.350) `VK_ARM_graph_instruction_set_tosa`(1.4.348) `VK_ARM_format_pack`(1.4.316) `VK_ARM_tensor_controls`(1.4.355) `VK_ARM_shader_instrumentation`(1.4.345) `VK_ARM_scheduling_controls`(1.4.348, rev2) `VK_ARM_performance_counters_by_region`(1.4.331) `VK_EXT_shader_float8`(1.4.317) `VK_EXT_shader_ocp_microscaling_types`(1.4.356) `VK_EXT_shader_long_vector`(1.4.337) `VK_EXT_shader_64bit_indexing`(1.4.330) `VK_EXT_shader_uniform_buffer_unsized_array`(1.4.330) `VK_QCOM_cooperative_matrix_conversion`(1.4.342) `VK_NV_cooperative_matrix_decode_vector`(1.4.352) `VK_EXT_cooperative_matrix_maintenance1`(1.4.359)

**光追**（项目无 RT 路线）：`VK_KHR_opacity_micromap`(1.4.351) `VK_ARM_pipeline_opacity_micromap`(1.4.305) `VK_NV_cluster_acceleration_structure`(1.4.307) `VK_NV_partitioned_acceleration_structure`(1.4.307) `VK_NV_ray_tracing_linear_swept_spheres`(1.4.307) `VK_EXT_ray_tracing_invocation_reorder`(1.4.333)

**移动 / 其他厂商**：`VK_QCOM_tile_shading`(1.4.312) `VK_QCOM_tile_memory_heap`(1.4.313) `VK_QCOM_data_graph_model`(1.4.332) `VK_QCOM_queue_perf_hint`(1.4.348) `VK_QCOM_elapsed_timer_query`(1.4.351) `VK_QCOM_image_processing3`(1.4.351) `VK_QCOM_shader_multiple_wait_queues`(1.4.351) `VK_OHOS_surface`(1.4.318) `VK_OHOS_native_buffer`(1.4.330) `VK_OHOS_external_memory`(1.4.331) `VK_SEC_ubm_surface`(1.4.343) `VK_SEC_throttle_hint`(1.4.350) `VK_SEC_pipeline_cache_incremental_mode`(1.4.321) `VK_VALVE_fragment_density_map_layered`(1.4.318) `VK_VALVE_shader_mixed_float_dot_product`(1.4.344) `VK_IMG_filter_linear_2d`(1.4.354)

**视频编解码**：`VK_KHR_video_maintenance2`(1.4.306) `VK_KHR_video_decode_vp9`(1.4.317) `VK_KHR_video_encode_intra_refresh`(1.4.321) `VK_KHR_video_encode_feedback2`(1.4.353) `VK_VALVE_video_encode_rgb_conversion`(1.4.327)

**shader 特性（边际）**：`VK_KHR_shader_bfloat16`(1.4.311) `VK_KHR_shader_fma`(1.4.329) `VK_KHR_shader_untyped_pointers`(1.4.325) `VK_KHR_shader_abort`(1.4.347) `VK_KHR_shader_constant_data`(1.4.347) `VK_EXT_shader_split_barrier`(1.4.351)

**Present / WSI**：`VK_KHR_present_id2`(1.4.317) `VK_KHR_present_wait2`(1.4.317) `VK_KHR_present_mode_fifo_latest_ready`(1.4.321) `VK_KHR_surface_maintenance1`(1.4.321) `VK_EXT_multisampled_render_to_swapchain`(1.4.353) `VK_EXT_external_memory_metal`(1.4.306)

**杂项图形**：`VK_EXT_fragment_density_map_offset`(1.4.311) `VK_EXT_primitive_restart_index`(1.4.348) `VK_EXT_texture_compression_astc_3d`(1.4.337) `VK_EXT_image_tiling_control`(1.4.358) `VK_KHR_extended_flags`(1.4.353) `VK_KHR_device_address_commands`(1.4.346)

> 注：① `VK_KHR_extended_flags`（1.4.353）是扩展机制本身的升级（支持新 XML tag/feature 表达），非功能扩展，但对后续扩展生态有影响。② `VK_EXT_shader_untyped_pointers`（1.4.325，2025 新）与项目已用的**核心 1.4 `shaderUntypedPointers` feature**（features_config `ShaderUntypedPointersKHR`）是**不同东西**——前者是 2025 新增的后续扩展（非核心 feature），项目未用，归入边际 shader 特性。

---

## 三、T1 高潜力候选深入（评估级）

### 3.1 `VK_EXT_zero_initialize_device_memory`

- **是什么**：`vkAllocateMemory` 期显式控制零初始化。feature `zeroInitializeDeviceMemory` + 分配标志 `VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT`（经 `VkMemoryAllocateFlagsInfo`）+ 新 image layout `VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT`。
- **解决什么**：Vulkan 默认不保证 `vkAllocateMemory` 清零，应用要确定性零值需 `vkCmdFillBuffer`/`vkCmdClearColorImage`（浪费，尤其平台本就会清零或能后台高效清零时）。`ZERO_INITIALIZED` layout 让"从已知全零内存转出"有快速路径（免 metadata clear、工具可读性更好）。
- **与本项目关联**：本项目有 **UAF / 未初始化内存读** 的深刻历史（GPUVM TCP fault，archive/heap_texture_research.md §11/§12.6；cottage artifact 越界读，Phase 4）。零初始化是**防御性正确性 + 调试**工具：分配即零值可把"读未初始化 = 随机垃圾"变成"读零"，让 UAF/越界 bug 的表现更可预测、更易定位。
- **集成点定位**：`VmaBuffer.cppm`（VMA 分配路径，`vkAllocateMemory` 封装）+ `vkresources.cppm`（depth image / scene data buffer 创建）。VMA 需把 `VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT` 透传到 `VkMemoryAllocateFlagsInfo`（VMA 2.x 有对应 flag/`VMA_ALLOCATE_ZERO_INITIALIZE` 支持需核实）。image 侧 `ZERO_INITIALIZED` layout 用于 depth/临时 image 初始化。
- **工作量/风险**：低（feature 链尾追加 + VMA flag 透传 + 可选 layout 替换）。风险低——纯分配期语义，不改渲染路径；不支持驱动 = 分配不零值（行为同现状，干净降级）。
- **驱动支持**：ratified；RADV/anv/NVIDIA 支持情况需 `vulkaninfo` 实测（本调研未跑程序）。
- **建议**：**近期评估**。作为"内存正确性卫生"引入，与本项目 UAF 史高度契合；可先仅对 depth/临时 buffer 启用，观测调试收益。

### 3.2 `VK_KHR_device_fault`

- **是什么**：统一设备故障上报（取代 `VK_EXT_device_fault`）。`vkGetDeviceFaultReportsKHR`（**可任意时刻调用**、**支持 timeout 阻塞**、**可报未致 device-lost 的 masked fault**（`deviceFaultReportMasked` feature））。提供 fault 地址（GPU VA）解读 + vendor binary dump 分离。
- **解决什么**：现有 `VK_EXT_device_fault` 要求 device 已 lost 才能查；**可恢复（masked）故障**（驱动隐藏、未上报 device-lost）无法诊断——这正是本项目 GPUVM fault 调试的盲区（fault 未 sticky 时应用无感）。
- **与本项目关联**：本项目故障诊断史厚重（Aftermath 集成见 nvidia_compatibility_report.md；GPUVM TCP fault 排查见 archive/heap_texture_research.md §11）。本扩展把"masked fault"纳入可观测范围，对**三驱动（尤其 RADV 核显 + NVIDIA offload）的偶发故障**提供统一、可阻塞、带 GPU VA 的诊断通道，是 `llm_debug`/调试基建的自然延伸。
- **集成点定位**：新 `device_fault.cppm`（探测 + 轮询/阻塞 + ring 缓存 + rllogger 周期上报）；`vkdevice.cppm` feature 链尾追加（**类成员**，pNext 不悬空）；可选挂到 `llm_debug` 或独立诊断开关。
- **工作量/风险**：中（新模块 + 探测链 + 阻塞线程或轮询点）。风险低——纯诊断旁路，不进渲染关键路径；不支持 = no-op。
- **驱动支持**：ratified（2026-03 新，较年轻）；三家驱动实际 masked-fault 上报能力需实测，VVL 覆盖可能薄。
- **建议**：**近期评估**（诊断基建）。与 present_timing 同为"测量/诊断"类，可合并为一个"可观测性"批次推进。

### 3.3 `VK_KHR_depth_clamp_zero_one`

- **是什么**：device feature `depthClampZeroOne`（`VkPhysicalDeviceDepthClampZeroOneFeaturesKHR`），使能后硬件把片元深度**钳制到 [0,1]**。**仅 feature，无新命令/动态状态**（per-draw 的 `vkCmdSetDepthClampRangeEXT` + `VkPipelineViewportDepthClampControlCreateInfoEXT` 属另一个 EXT 扩展 `VK_EXT_depth_clamp_control`，非本 KHR 扩展）。
- **解决什么**：项目当前**用 glm 在 CPU 侧投影矩阵强制 depth zero-to-one**（AGENTS.md 强制约定）。NDC 深度越界（>1 或 <0）行为各异（通常 clip 或未定义）；硬件 clamp 是规范、零 shader 成本的兜底。
- **与本项目关联**：直接呼应"强制 depth zero-to-one"约定。天空盒用**全屏大三角 z=1=far**（sky.slang，依赖 depth clear=1.0 + LEQUAL）——需验证 clamp 与 z=1 写入的交互（clamp 不应破坏 z=1 恰好落在 far 的语义；确认 LEQUAL + clear 1.0 组合下 clamp 不引入近处误判）。
- **集成点定位**：`features_config.json` + `features.cppm`（feature 链尾追加 `VkPhysicalDeviceDepthClampZeroOneFeaturesKHR{depthClampZeroOne=TRUE}`，**类成员**）；与既有 viewport `depthClampEnable` 的交互需核对（二者语义叠加）。无渲染路径改动。
- **工作量/风险**：低（单 feature）。风险：与天空盒 z=1 / LEQUAL / clear 1.0 的交互需像素级验证；device-wide 使能（非 per-draw），影响所有渲染。
- **驱动支持**：ratified；需实测三家（尤其 RADV）。
- **建议**：**近期评估**（小、干净的 win）。与现有 zero-to-one 约定天然契合；验证天空盒交互后作为硬件兜底（软件 glm 映射保留，clamp 作保险）。

### 3.4 `VK_EXT_present_timing`（重评）

- **是什么/解决什么**：详见 `present_timing_research.md`（已 ratified，spec v3；1.4.349 有一致性修复；1.4.355 澄清 `vkSetSwapchainPresentTimingQueueSizeEXT` 未调用时 present 也返回 `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`）。提供 present 分阶段时间戳 + 按目标时间调度（仅 FIFO 系）。
- **2025+ 变化**：扩展正式 ratified（生态稳定）；spec 文本一致性修复；**但 spec v 仍为 3（2024-10），无新 API**。
- **与本项目关联（重评）**：原决策"暂不引入"的两个理由——① eImmediate 冲突（调度仅 FIFO 有效）；② 收益以测量为主。**§八（2026-09-01）已确认 eImmediate 无 pacing 导致 Intel A310 GPU 100% 占用**，且文档自陈"若未来做动画质量/延迟优化，本节即触发条件"。Phase 6/7 增加 pass（延迟渲染、compute）后**延迟敏感性上升**，测量价值（端到端延迟、judder、offload 两路对比）更突出。
- **集成点定位**：完全沿用 `present_timing_research.md` §3.4 清单（新 `present_timing.cppm` + swapchain flag/队列容量 + drawFrame pNext 链 + 每帧 collect + rllogger 汇总；feature 链尾追加做类成员）。
- **工作量/风险**：中。风险同原文档（反馈延迟数帧 + 仅轮询 + queue-full 回退 + eImmediate 功能减半 + 窗口/合成器下部分 stage 拿不到）。
- **建议**：**近期评估（测量先行）**。按原文档"步骤 1 仅测量"落地（IPD 均值/p95、各 stage 延迟、丢帧计数、刷新属性变化），先量化 eImmediate 链路延迟 + offload 两路对比；帧 pacing（步骤 2）待 present mode 可配置化后做。

---

## 四、T2 近期路线候选深入（评估级，Phase 6/7）

### 4.1 `VK_KHR_unified_image_layouts`

- **是什么**：feature `unifiedImageLayouts` 保证全用 `VK_IMAGE_LAYOUT_GENERAL` 等价最优，**免 image layout 迁移**（仍可用 image/global memory barrier 做同步）。例外：`UNDEFINED`（初始化 metadata）、`PREINITIALIZED`、`PRESENT_SRC_KHR`/`SHARED_PRESENT_KHR`（与合成器交互）、feedback-loop（改 `VkAttachmentFeedbackLoopInfoEXT` 标志）。
- **与本项目关联**：Phase 6 RenderGraph + Phase 7 G-buffer 多 pass 会**频繁在 color/depth/临时 image 间切换 layout**。本扩展把 layout 状态机大幅简化（全 GENERAL），降低 RenderGraph 的 barrier 规划复杂度——与"渲染 system 产出抽象 draw list、执行器盲执行"的分层契合。
- **集成点定位**：Phase 6/7 RenderGraph 的 barrier 生成层 + `vkresources.cppm`（depth image）+ `vkrenderer.cppm`（动态渲染 attachment layout）。
- **工作量/风险**：中（需改 barrier 策略，但可渐进：先临时 image 用 GENERAL）。**关键风险 = 驱动支持**（proposal 明言"部分硬件架构需修改才能支持"），三家支持情况未知，可能仅部分驱动暴露。
- **建议**：**观察 / Phase 6 前探测**。先 `vulkaninfo` 确认三家 `unifiedImageLayouts` 是否暴露；若 RADV/anv 支持，Phase 6 RenderGraph 设计时直接采用（简化收益大）；否则维持现状 layout 迁移。

### 4.2 `VK_EXT_custom_resolve`

- **是什么**：在 render pass 实例内用 **shader 替代固定功能 resolve**（基于 `VK_QCOM_render_pass_shader_resolve` 提升 + **动态渲染支持** + **resolve 到不同格式**）。动态渲染下 `VK_RENDERING_CUSTOM_RESOLVE_BIT_EXT` + `VK_RESOLVE_MODE_CUSTOM_BIT_EXT` + `vkCmdBeginCustomResolveEXT`；单 quad 无 overdraw 读多采样、写单采样 resolve attachment。
- **与本项目关联**：当前无 MSAA。**Phase 7 G-buffer 若引入 MSAA**（抗锯齿是延迟渲染常见配套），本扩展提供 tile 高效的 shader resolve（比"写内存再 compute resolve"高效，比固定功能 resolve 灵活——可自定义滤波/异格式）。
- **集成点定位**：Phase 7 G-buffer pass 的 resolve 阶段 + 动态渲染 attachment（resolveMode）+ resolve frag shader object（链 `VkCustomResolveCreateInfoEXT`）。
- **工作量/风险**：中（依赖 MSAA 决策）。风险：单 quad 无 overdraw 约束 + 驱动对 dynamic-rendering custom resolve 的成熟度。
- **建议**：**Phase 7 G-buffer MSAA 的选项**。若 Phase 7 决定加 MSAA，优先评估本扩展（而非固定功能 resolve / compute resolve）；否则不引入。

### 4.3 `VK_KHR_copy_memory_indirect`

- **是什么**：`vkCmdCopyMemoryIndirectKHR` / `vkCmdCopyMemoryToImageIndirectKHR`——拷贝参数（src/dst 地址 + size，strided 数组）**从 device memory 读取**，命令录制期无需已知。feature `indirectMemoryCopy` / `indirectMemoryToImageCopy` + `supportedQueues` property。
- **与本项目关联**：项目已有**每帧 scene data staging ring**（Phase 5 批次 2：fence 后 CPU 写 + 录制期 `vkCmdCopyBuffer` 拷进 scene data device buffer + Transfer→Graphics barrier）。GPU-driven（Phase 7 culling 输出驱动拷贝）或"GPU 决定拷什么/拷多少"场景下，indirect copy 免 host↔device 往返取参数。
- **集成点定位**：Phase 7 GPU-driven 路径（culling 输出 → 间接拷贝可见 mesh 数据）；可与 `device_generated_commands` 组合。
- **工作量/风险**：中。风险：依赖 GPU-driven 决策；`supportedQueues` 需查询（可能仅 transfer 队列）。
- **建议**：**Phase 7 GPU-driven 的选项**。若 Phase 7 culling 走全 GPU-driven（NVIDIA device-generated / RADV IndirectCount），评估用本扩展替代 CPU 侧 staging copy；否则维持现状 ring。

### 4.4 `VK_EXT_shader_subgroup_partitioned`

- **是什么**：compute shader 内 **subgroup 分区**（把 subgroup 切成更小的执行单元，控制分区内同步/发散）。
- **与本项目关联**：Phase 7 light pass / GPU culling compute 可能受益于分区控制（如按光/按 tile 分区，减少 warp 发散）。
- **集成点定位**：Phase 7 compute shader（light pass / culling）。
- **工作量/风险**：低-中（shader 侧特性，需 Slang 支持 subgroup partitioned 操作）。风险：Slang→SPV 对该特性的支持 + 驱动成熟度。
- **建议**：**Phase 7 compute 优化选项**。非阻塞，culling/light pass 初版用普通 subgroup 即可，性能瓶颈出现再评估。

### 4.5 `VK_EXT_memory_decompression`

- **是什么**：GPU 解压 **GDeflate** 压缩资产：`vkCmdDecompressMemoryEXT`（+ indirect count 版）。`VkMemoryDecompressionMethodFlagsEXT`（当前仅 `GDEFLATE_1_0`）。
- **与本项目关联**：项目加载 **257MB cottage / 11 张 4K 纹理**，当前 CPU 解码（stb/tinygltf）+ 上传。GPU 解压可把 CPU 解压瓶颈移到 GPU（NVMe 流式 + GPU 并行），降低加载峰值内存与 CPU 占用。
- **集成点定位**：资产管线（需 **GDeflate 压缩的资产格式** + 解压命令插入上传路径）。
- **工作量/风险**：中-高（**资产管线依赖**：需工具链产出 GDeflate 资产，非现成 glTF 内嵌）。风险：驱动支持（较新）+ 资产格式改造成本。
- **建议**：**观察 / 可选**。仅当资产体积/加载时间成为瓶颈且愿意改造资产管线时引入；对当前 glTF 内嵌纹理流程改造成本高，优先级低。

### 4.6 `VK_KHR_internally_synchronized_queues`

- **是什么**：`VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR`（`VkDeviceQueueCreateInfo::flags`）让队列**内部同步**，免应用侧互斥；feature `internallySynchronizedQueues`；per-queue opt-in。
- **与本项目关联**：当前单线程渲染循环（`ecs.cppm` `renderSystem`），无多模块并发提交需求。**Phase 6/7 若引入多线程录制/提交**（如 compute 与 graphics 并行录制），本扩展免去各模块对齐队列互斥的复杂度。
- **集成点定位**：`vkdevice.cppm`（队列创建 flag）；Phase 6/7 多线程提交架构。
- **工作量/风险**：低（单 flag）。风险：当前无需求；`vkQueuePresentKHR` 等内部同步可能降低并行度（proposal 已述）。
- **建议**：**观察**。待 Phase 6/7 多线程提交落地时再评估；当前单线程无收益。

### 4.7 `VK_KHR_robustness2`

- **是什么**：鲁棒性增强（越界/未初始化内存访问的规范化行为、更细的 robustness 控制），面向 Roadmap 2026。
- **与本项目关联**：本项目有**越界 bug 史**（gltf uv 步长、model.mesh triangles 索引，Phase 4 定案）+ UAF 史。robustness2 提供更规范的越界行为，配合 `zero_initialize_device_memory` 强化"读越界/未初始化"的可预测性。
- **集成点定位**：`features_config.json` + `features.cppm`（feature 链）。
- **工作量/风险**：低（feature 链）。风险：行为变化需回归（越界从"未定义"变"规范"，理论上更安全）。
- **建议**：**近期评估**（低成本卫生项），与 zero-init 同批。

---

## 五、maintenance 扩展相关位（Phase 6/7 顺手启用）

| 位 | 扩展 | 对本项目的价值 | 启用时机 |
|---|---|---|---|
| `VK_PIPELINE_LAYOUT_CREATE_NO_TASK_SHADER_BIT_KHR` | maintenance11 | mesh shader 管线 layout 显式声明"无 task stage"——呼应 `min_mesh_crash` 的 `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT` 坑（NVIDIA Xid 13），把 shader 级 bit 提升到 pipeline layout 级 | Phase 6 管线 layout 重构时 |
| `VK_ATTACHMENT_DESCRIPTION_RESOLVE_SKIP/ENABLE_TRANSFER_FUNCTION_BIT_KHR` | maintenance10 | resolve 时控制是否套 sRGB EOTF——Phase 7 G-buffer 若用 sRGB 附件 + MSAA resolve，避免双重/缺失 EOTF | Phase 7 G-buffer |
| `VK_DEPENDENCY_ASYMMETRIC_EVENT_BIT_KHR` | maintenance9 | 非对称事件依赖（subpass 间单向依赖），简化多 pass 同步 | Phase 6 RenderGraph |
| `VK_QUERY_POOL_CREATE_RESET_BIT_KHR` | maintenance9 | query pool 可 reset（免重建）——配合 present_timing/perf 查询复用 | 诊断批次 |

> maintenance8/9/10/11 均为 ratified 的一般清理 + 上述位，建议**整链启用**（链尾追加 feature 结构体，做类成员），具体位按 Phase 6/7 需要消费。

---

## 六、厂商特定扩展（AMD Strix Halo / NVIDIA offload 视角）

| 扩展 | 厂商 | 建议 |
|---|---|---|
| `VK_AMD_gpa_interface` (P, 1.4.351) | AMD | **可探测**。Strix Halo 上 GPA session 做 GPU 性能采样，对"Phase 5.5 场景 A310 100% 占用"类问题有诊断价值。provisional，需 RADV 暴露确认；低优先。 |
| `VK_AMDX_dense_geometry_format` (X, 1.4.324) | AMD | **观察**。稠密几何压缩格式，与 mesh shader 工作流适配未知；AMD 专属，跨驱动无意义，仅 AMD 路径可选。 |
| `VK_NV_present_metering` (P, 1.4.308) | NV | **与 present_timing 互补**。NVIDIA offload 路径的帧 pacing/metering；若做 present_timing 测量批次，NVIDIA 侧可一并探测本扩展。 |
| `VK_NV_push_constant_bank_support` (1.4.340) | NV | **观察**。push constant bank 机制，本项目 `DrawArgs` 超集走单 bank；NV 专属，无迫切需求。 |
| `VK_NV_compute_occupancy_priority` (1.4.336) | NV | **Phase 7 compute 选项**。NV compute 占用率优先级，light pass/culling 调优用；低优先。 |
| `VK_NV_cooperative_vector` / `VK_NV_external_compute_queue_vendor` | NV | **低相关**（ML 向 / 外部队列），不引入。 |

> 厂商扩展统一原则：**探测可用则启用、不可用则 no-op**，不破坏双/三 GPU 运行。AMD/NV 专属扩展不进 required 清单，走可选清单（同 present_timing 的可选化设计）。

---

## 七、补充：Vulkan 1.1–1.3 时代有价值扩展（Part B）

> 2024 前引入、**本项目尚未采用**、对当前轨迹有价值者。已与本地 registry 核对状态（多数 ratified / 已提升核心）。

### 7.1 `VK_EXT_host_image_copy`（R）— **高相关**

- **是什么**：CPU 内存 ↔ image **直接拷贝**（免 staging buffer）：`vkCopyMemoryToImageEXT` / `vkCopyImageToMemoryEXT` / `vkCopyImageToImageEXT` + host 侧 layout 迁移 `vkTransitionImageLayoutEXT`。image 需 `VK_IMAGE_USAGE_HOST_TRANSFER_BIT`。`VkHostImageCopyDevicePerformanceQueryEXT` 查 `optimalDeviceAccess`/`identicalMemoryLayout` 判断是否损失设备端性能。
- **与本项目关联**：项目纹理上传走 **staging buffer + `vkCmdCopyBuffer`/blit**（texture_pool.cppm：6 张独立 2D linear staging + 6 region copy + mipmap blit 链，env cubemap 同理）。本扩展可**省掉 staging buffer 的一次拷贝 + 峰值内存翻倍**（proposal 明言"同时初始化大量 image 时峰值内存翻倍 → 可能 OOM"——正是 cottage 11 张 4K 纹理的场景）。
- **集成点定位**：`texture_pool.cppm`（`uploadModelTextures`/`uploadCubeMap` 上传路径）+ `VmaImage` 创建加 `HOST_TRANSFER_BIT`。
- **工作量/风险**：中。**关键风险**：CPU 侧 swizzle 可能比 GPU 双拷贝慢（optimal tiling 非线性），proposal 明确"建议实测，非普遍推荐全走本扩展"。sRGB/线性格式 + 驱动支持需验证。
- **建议**：**近期评估（纹理上传优化）**。对 4K/大纹理上传路径 A/B 实测（host_image_copy vs staging+blit），`optimalDeviceAccess=TRUE` 的格式优先切换；保留 staging 路径作回退。

### 7.2 `VK_EXT_headless_surface`（R）— **中相关（llm_debug）**

- **是什么**：无窗口的 headless device/surface（`vkCreateHeadlessSurfaceEXT` 等），纯离屏渲染不需真实 surface。
- **与本项目关联**：`llm_debug` 当前用 **`SDL_WINDOW_HIDDEN` 隐藏窗**做离屏重渲。headless surface 是更干净的离屏路径（不依赖 SDL 窗口/compositor 存活），对 **CI / agent 自主工作 / 无头环境**更稳。
- **集成点定位**：`llm_debug.cppm`（离屏 surface 创建分支）+ `sdl3window.cppm`（headless 路径）。
- **工作量/风险**：低-中。风险：headless 格式/能力可能受限（经标准 `vkGetSurfaceFormatsKHR` 查询 headless surface 的可用格式，可能少于真实 surface）；驱动支持（RADV/anv/NVIDIA）。
- **建议**：**观察 / llm_debug 增强**。当前隐藏窗可用，headless 作为"无头环境更稳"的可选路径，优先级中低。

### 7.3 `VK_KHR_shader_clock`（R）— **中相关（延迟测量）**

- **是什么**：shader 内读**高精度 GPU 时钟**（`readClockKHR`），配合 `vkCmdWriteTimestamp` 做细粒度 GPU 侧计时。
- **与本项目关联**：与 present_timing 的**端到端延迟测量**互补——shader clock 量"渲染某阶段在 GPU 上的真实耗时"，present_timing 量"present→可见"，合起来是全链路延迟分解。对 Phase 6/7 多 pass 性能定位有价值。
- **集成点定位**：诊断/测量批次（与 present_timing/device_fault 同批）+ 测量用 shader。
- **工作量/风险**：低。风险：时钟分辨率/单调性依驱动；仅测量用途，不进渲染路径。
- **建议**：**近期评估（测量批次）**。与 present_timing 合并为"可观测性"批次，shader clock 量 GPU 侧分段耗时。

### 7.4 `VK_EXT_full_screen_exclusive`（R）— **中相关（延迟）**

- **是什么**：独占全屏模式（`VK_FULL_SCREEN_EXCLUSIVE_*` present mode / surface 能力），降低显示延迟（绕过合成器）。
- **与本项目关联**：项目 present 延迟敏感（eImmediate + 无 pacing）。独占全屏可降延迟，但**与窗口化 + 合成器（Wayland/KWin）工作流冲突**——本项目主要在窗口/Wayland 下开发，独占全屏适用场景窄。
- **集成点定位**：`vkswapchain.cppm` / `utils.cppm` `selectPresentMode`（present mode 可配置化后）。
- **建议**：**观察**。待 present mode 可配置化（present_timing_research.md 步骤 2）后，作为低延迟选项之一；当前窗口化工作流下优先级低。

### 7.5 `VK_EXT_extended_dynamic_state3`（R）— **低-中相关**

- **是什么**：更多动态状态（vertex input binding、viewport/w scissors 细化、conservative raster 等），减少管线变体。
- **与本项目关联**：项目已重度用 dynamic state（shader object + `applyDynamicState`）。edS3 可进一步减少管线变体（Phase 6/7 多 pass 管线爆炸时有益）。
- **集成点定位**：`vkrenderer.cppm` `applyDynamicState` + 管线创建。
- **工作量/风险**：低。风险：边际收益，当前管线数（12 条去重）不大。
- **建议**：**观察**。Phase 6/7 管线变体增多时再评估。

### 7.6 `VK_EXT_color_write_enable`（R/SC）+ `VK_KHR_load_store_op_none`（R）— **低-中相关（Phase 7 多 pass）**

- **是什么**：`color_write_enable`：per-attachment 动态颜色写开关（免改 loadOp/storeOp）；`load_store_op_none`：`VK_LOAD_OP_NONE`/`VK_STORE_OP_NONE`（附件内容未定义时可跳过 load/store，省带宽）。
- **与本项目关联**：Phase 7 G-buffer 多 pass——中间 pass 的附件常"不需 load 旧值 / 不需 store 回"，`NONE` op 省带宽；`color_write_enable` 做 per-attachment 写控制（如 G-buffer 某 pass 只写 normal 不写 albedo）。
- **集成点定位**：Phase 7 G-buffer pass 的渲染附件配置 + 动态状态。
- **工作量/风险**：低。风险：`NONE` op 要求附件内容确未定义（用错 = 读到垃圾），需 careful。
- **建议**：**Phase 7 G-buffer 顺手启用**（低成本带宽优化）。

### 7.7 compute 配套（Phase 7）

| 扩展 | 状态 | 相关性 | 建议 |
|---|---|---|---|
| `VK_EXT_subgroup_size_control` | R/SC | Phase 7 compute（light/culling）控制 subgroup size | 观察，culling 调优时 |
| `VK_KHR_workgroup_memory_explicit_layout` | R | Phase 7 compute 显式 workgroup memory 布局 | 观察，compute 复杂化时 |

### 7.8 交叉引用（已调研，非新）

- **`VK_EXT_device_generated_commands`**（R）：Phase 7 GPU culling 核心，已有 `device_generated_commands_research.md`（双驱动实测）。本扩展非 2025+ 新，但与 T2 的 `copy_memory_indirect`/`subgroup_partitioned` 同属 Phase 7 GPU-driven 栈，**三者一并评估**。
- **`VK_EXT_tooling_info`**（已提升核心 1.3）：设备信息（供 profiling 工具），项目已隐式可用（Vulkan 1.3），无需额外引入，诊断批次可查询。
- **`VK_EXT_calibrated_timestamps`**（→`VK_KHR_calibrated_timestamps`）：时间域校准（device 域 ↔ 其它域），是 `present_timing` 的依赖（swapchain 局部时间域对齐 device/CLOCK_MONOTONIC）。诊断批次随 present_timing 一并引入，无需单独评估。

---

## 八、与本项目关键契合点（总结）

1. **核心依赖转正**：`VK_EXT_descriptor_heap` 2026-01 ratified——项目架构基石从 bleeding-edge 转为稳定，后续可放心深化 bindless 用法（descriptor heap 的 VU 在 1.4.352-1.4.356 持续细化，见 §二 changelog）。
2. **可观测性三件套**（建议合并为一个"测量/诊断"批次）：`present_timing`（重评，测量先行）+ `device_fault`（masked fault 诊断）+ `shader_clock`（GPU 侧分段计时）——直击本项目 GPUVM fault 史 + eImmediate 延迟盲区。
3. **内存正确性卫生**（建议一个批次）：`zero_initialize_device_memory` + `robustness2`——针对 UAF/越界 bug 史的防御性正确性。
4. **depth 约定硬件化**：`depth_clamp_zero_one` 替代软件 zero-to-one，与天空盒 z=1 交互需像素级验证。
5. **Phase 6/7 地基**：`unified_image_layouts`（若驱动支持）简化多 pass 同步；`custom_resolve`（MSAA）+ `color_write_enable`/`load_store_op_none`（带宽）+ maintenance 相关位（NO_TASK_SHADER/resolve transfer fn/asymmetric event）；GPU-driven 栈 `copy_memory_indirect` + `subgroup_partitioned` + 既有 `device_generated_commands`。
6. **纹理上传优化**：`host_image_copy` 对 4K/大纹理 A/B 实测，省 staging 峰值内存。

---

## 九、优先级建议矩阵

| 优先级 | 扩展 | 批次 | 理由 |
|---|---|---|---|
| **近期评估（测量/诊断批次）** | `present_timing`(重评) · `device_fault` · `shader_clock` · `query pool reset`(maint9) | 可观测性 | 直击 GPUVM 史 + eImmediate 延迟盲区；测量先行，低风险旁路 |
| **近期评估（内存卫生批次）** | `zero_initialize_device_memory` · `robustness2` | 正确性 | UAF/越界史防御；低成本 feature 链 |
| **近期评估（小 win）** | `depth_clamp_zero_one` | depth 约定 | 替代软件 zero-to-one；验证天空盒交互 |
| **近期评估（上传优化）** | `host_image_copy` | 纹理上传 | 4K 大纹理省 staging 峰值内存；A/B 实测 |
| **Phase 6 前探测** | `unified_image_layouts` | RenderGraph | 驱动支持是关键；支持则简化多 pass 同步 |
| **Phase 7 G-buffer** | `custom_resolve` · `color_write_enable` · `load_store_op_none` · maint10 resolve transfer fn | 多 pass | MSAA/带宽优化；依赖 MSAA 决策 |
| **Phase 7 GPU-driven** | `copy_memory_indirect` · `subgroup_partitioned` · `device_generated_commands`(既有) · `subgroup_size_control` | compute/culling | GPU culling 栈；NVIDIA/RADV 双路 |
| **观察** | `internally_synchronized_queues` · `extended_dynamic_state3` · `memory_decompression` · `headless_surface` · `full_screen_exclusive` · `workgroup_memory_explicit_layout` | 按需 | 需求未现或资产管线依赖 |
| **厂商可选** | `AMD_gpa_interface` · `NV_present_metering` · `NV_compute_occupancy_priority` · `AMDX_dense_geometry_format` | 厂商 | 探测可用则启用，no-op 降级 |
| **不引入** | T4 全部（ML/ARM/QCOM/OHOS/SEC/VALVE/光追/视频/边际 shader 特性） | — | 与项目路线无接缝 |

> 落地总原则：**可选化设计**——所有新扩展走"探测可用则启用、不可用则 no-op"（同 present_timing_research.md §3.4 的 feature 链尾追加做类成员 + 可选清单模式），保双/三 GPU 运行不破坏。

---

## 十、不相关扩展简列（T4，仅备查）

见 §2.5（ML/张量、光追、移动/其他厂商、视频、边际 shader 特性、Present/WSI、杂项图形）。这些与本项目 mesh+shader-object+descriptor-heap + forward→deferred 路线无接缝，或为其他厂商/平台专属，**不评估引入**。

---

## 十一、本地参考资料

| 资源 | 位置 |
|---|---|
| registry（扩展状态权威） | `/home/robin/program/Vulkan-Docs/xml/vk.xml`（`<extension name=... ratified=... provisional=...>`） |
| ChangeLog（逐版本新扩展） | `/home/robin/program/Vulkan-Docs/ChangeLog.adoc`（"New Extensions"/"Extensions" 段，1.4.305→1.4.360） |
| 设计文档（语义） | `/home/robin/program/Vulkan-Docs/proposals/*.adoc`（`VK_EXT_present_timing` / `VK_KHR_device_fault` / `VK_EXT_zero_initialize_device_memory` / `VK_EXT_custom_resolve` / `VK_KHR_unified_image_layouts` / `VK_KHR_copy_memory_indirect` / `VK_EXT_memory_decompression` / `VK_KHR_internally_synchronized_queues` / `VK_EXT_host_image_copy` / `VK_EXT_calibrated_timestamps` / `VK_EXT_device_generated_commands` / `VK_EXT_extended_dynamic_state3` / `VK_KHR_load_store_op_none`） |
| 附录（版本史/元数据） | `/home/robin/program/Vulkan-Docs/appendices/*.adoc` |
| 章节（部分扩展正文） | `/home/robin/program/Vulkan-Docs/chapters/`（`descriptorheaps.adoc` / `descriptorbuffers.adoc` / `synchronization.adoc` 等） |
| 项目既有调研 | `docs/present_timing_research.md` · `docs/device_generated_commands_research.md` · `docs/archive/heap_texture_research.md` · `docs/nvidia_compatibility_report.md` |

> **注意**：spec clone（v1.4.360）比系统头/loader（v341）新，部分新扩展（尤其 2026 年的）系统头可能尚未包含——引入前以系统头 `/usr/include/vulkan` + `vulkaninfo` 实测为准。provisional 扩展（`AMD_gpa_interface`/`NV_present_metering` 等）API 可能变动，引入需锁版本。

---

## 十二、实施记录（2026-09-02）：edS3 + headless（zero_init 已回退）

> 状态：`现行`。§3.1（zero_init）/§7.2（headless）/§7.5（edS3）评估级候选的落地记录，含主崩溃误诊复盘 + 遗留项处置 + headless 关键坑。开发机 = Strix Halo 纯核显（RADV Mesa 26.2.1，`RADV_EXPERIMENTAL=heap`），VVL 1.4.357，系统头 v357。**最终落地 = edS3 + headless**；zero_init 经深挖（VMA 3.4 必撞 + 本 app 无收益 + VMA 无上游 zero-init 计划）后**回退**（见 §12.1 Phase C / §12.3）。

### 12.1 落地范围

| Phase | 内容 | 落点 |
|---|---|---|
| A | 可选 feature codegen：`features_config.json` `optional_features` 段 → `features_cppm.inja` 生成 `OptionalFeatureSupport`（`queryOptionalFeatures` 探测：扩展可用 **且** feature 字段置位）+ required-only `FeatureChain` | `features_config.json` / `tools/templates/features_cppm.inja` / `generated/features_generated.cppm` |
| B | device 接线：optional feature 结构**不进** required `StructureChain`（FALSE 结构挂未启用扩展 = UB），改由 `createDevice` 按支持位**条件链接**到 `FeatureChainTail`（required 链尾） | `vkdevice.cppm` |
| C | ~~zero_init~~（**已回退 2026-09-02**）：原实现 = VMA `vmaCreateDedicatedBuffer` 的 `pMemoryAllocateNext` 链 `VkMemoryAllocateFlagsInfo{ZERO_INITIALIZE_BIT}`，仅对 descriptor heap 启用。深挖发现 VMA 3.4 对 BDA 必撞（内部已加 `DEVICE_ADDRESS` flagsInfo）→ spec 违例 UB + VVL 噪声，且本 app 无收益 → **回退**（见 §12.3） | ~~`VmaBuffer.cppm` / `vkresources.cppm`~~ |
| D | edS3：`extendedDynamicState3DepthClipEnable` 粒度 flag → `RenderState.depthClipEnable` + `applyDynamicState` 内 `setDepthClipEnableEXT`（能力门 `depthClipDynamic`） | `shader.cppm` / `vkrenderer.cppm` / `llm_debug.cppm` |
| E | headless：`RGL_HEADLESS=1` 触发，`RGL_HEADLESS_SIZE="WxH"`（默认 1280×720）；instance 扩展用 `VK_KHR_surface`+`VK_EXT_headless_surface` 替代 SDL surface 扩展；`vkCreateHeadlessSurfaceEXT` 建虚拟 surface；脱离 SDL/compositor | `vkinstance.cppm` / `vkswapchain.cppm` / `main.cpp` |

跨驱动降级：`queryOptionalFeatures` 探测（扩展可用 **且** feature 置位，缺一即 false）→ 不支持时整体 no-op，启动不破。headless 是独立启用路径（`RGL_HEADLESS` env），非降级。

### 12.2 主崩溃：实参错位导致 BDA flag 丢失（误诊 RADV bug）

**现象**：启用 zero_init 或 edS3 后，BDA 顶点 buffer 的 `vmaCreateBuffer` 返回 `VK_ERROR_INITIALIZATION_FAILED`（-3）→ `bad access to std::expected`（VMA 错误被 `.value()` 吞）。基线（不启用）稳定。

**误诊过程（教训核心）**：把"启用扩展就崩、不启用不崩"归因为 **RADV 驱动 bug**，做了大量排除法——结构体进/不进 pNext 链、edS3 单独 / zero_init 单独、强制 baseline device（8 扩展）。**全部在改"设备配置"，没有回到"自己代码与基线的逐字节 diff"**。用户指出 RADV 官方（Mesa 25.2）支持 zero_init 后才重新审视代码 diff。

**真根因**：`VmaAllocatorRAII` 构造器签名 `(…, uint32_t apiVersion, VmaAllocatorCreateFlags flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT, bool zeroInit = false)`。追加 `zeroInit` 参数时，调用点把 `/*zeroInit=*/false` 塞进**第 5 位 = `flags`**，BDA flag 被清 0 → 所有 `eShaderDeviceAddress` buffer 创建失败。**与扩展完全无关**——每个"启用扩展"版本都同时带着这个错位调用，所以看起来像"扩展触发"。

**教训**：
1. 给函数加参数（尤其插在已有**默认参数**前）时，所有调用点实参位置必须重查——默认参数掩盖实参错位（编译通过、运行才崩）。
2. "A 变了就崩"不一定是 A 的错——可能是 A 与 B 同时变（本次 A=扩展、B=实参错位）。
3. 归因到外部（驱动/硬件）前，先把"自己代码与基线的 diff"逐行审干净。
4. 外部强反证（RADV 官方支持该扩展）是重要信号，应尽早质疑"驱动 bug"假设。

### 12.3 遗留非致命项（程序正常运行，VVL 报 error 但不阻塞渲染）

> **2026-09-02 深挖定案**：两项均经临时诊断实锤（TEMP DIAG 已清）。

- **zero_init：**~~VMA 内部 `VkMemoryAllocateFlagsInfo` 撞车（spec 违例，UB）~~ → **已回退（2026-09-02 定案）**。原问题：VMA 3.4 对 BDA buffer 自动 `VmaPnextChainPushFront` 一个 `VkMemoryAllocateFlagsInfo`（`DEVICE_ADDRESS_BIT`，子分配块 `:12150` / dedicated `:14054`），用户经 `pMemoryAllocateNext` 传的（`ZERO_INITIALIZE_BIT`）形成 `vkAllocateMemory.pNext` 链里**两个同类结构** → VVL 报 `duplicate structure types`；VMA 无内置零初始化 flag，BDA buffer 在 VMA 3.4 下**无法干净做 zero_init**。**深挖三结论**：① **本 app 无收益**——对照实验（强制 `zeroInit=false`）显示 RADV **新分配内存本身全零**（mapped[0..4096] 两路均 `nonzero=0/4096`），描述符堆只分配一次不重分配，"读未初始化"场景不出现；② duplicate = spec 违例（UB），RADV 容忍但**不保证**应用 `ZERO_INITIALIZE`；③ **"等 VMA 上游"不成立**——v3.4.0 是最新版（2025-06，距今 ~15 月无新版），无任何版本有 zero-init flag，v3.4.0 changelog 完全未提 zero-init。**处置 = 回退 feature**：移除 `features_config.json` zero_init 条目 + codegen 重生成 + `vkdevice` ziFeature/链接 + `VmaBuffer` zeroInit 路径 + `features.cppm` 扩展 + `vkresources` allocator 实参。回退后 9 扩展（原 10）、duplicate 报错消失、无回归。将来若出现真实 zero-init 需求（频繁分配/释放场景），走原生 `vkAllocateMemory`（单 `VkMemoryAllocateFlagsInfo{DEVICE_ADDRESS|ZERO_INITIALIZE}`）绕过 VMA。

- **edS3：VVL 误报 `depthClipEnable feature 未启用`（VVL 1.4.357 gap，app 侧无 bug）→ 保留现状（2026-09-02 定案）**。深挖定案：① feature **确实在设备级启用了**——临时诊断在 `vkCreateDevice` 时刻遍历 pNext 链（zero_init 回退前 13 元素，edS3 为末位 `sType=1000455000` `flag=1 (TRUE)`），回退后 12 元素、edS3 仍末位 flag=TRUE；② hpp `PhysicalDevice::createDevice`（`vulkan_funcs.hpp:478`）把 `&createInfo` **原样** `reinterpret_cast` 传给 `vkCreateDevice`，pNext 链一字不改——VVL 拦截到的链 = 诊断的链；③ VVL 二进制 `strings` 命中 `extendedDynamicState3DepthClipEnable` + 该 VUID（VVL **认识**此 feature），却仍报"未启用" → **VVL 设备级 feature 解析未把该结构体记进"已启用"集**（较新 ratified 扩展的 VVL 追踪 gap）。非致命：RADV 接受 `setDepthClipEnableEXT`，渲染正常。**处置 = 保留现状**（VVL 噪声留着，非致命；可选消噪 = DebugMessenger 过滤 `VUID-vkCmdSetDepthClipEnableEXT-depthClipEnable-07451` 误报，未做）。

### 12.4 headless 关键坑

- **v357 的 `VkHeadlessSurfaceCreateInfoEXT` 仅 `sType`/`pNext`/`flags`——无 width/height/format**（尺寸由 swapchain `imageExtent` 决定，非 surface）。初版误按"带尺寸"写 → 编译报 `no member named width`。
- **headless 时 instance 扩展须显式加 `VK_KHR_surface`**：其它 surface 族扩展（`maintenance1`/`get_surface_capabilities2`/`swapchain_colorspace`/`headless_surface`）都依赖它；SDL 路径由 `SDL_Vulkan_GetInstanceExtensions` 隐式带，headless 脱 SDL 后须显式，否则 `vkCreateInstance` 报 `Missing extension required ... VK_KHR_surface`。
- **headless surface 无 `eImmediate` present mode**（`selectPresentMode` 硬要求 `eImmediate`）→ 须用 `eFifo` 回落（`selectHeadlessPresentMode`）。headless 的 present 是 no-op，任何模式皆可。
- **`SDLWindow` 改 `unique_ptr` 可空**：`AppContext`/`Swapchain` 收 `SDLWindow*`，headless 传 nullptr 跳过建窗；`main.cpp` 的 `SDL_ShowWindow` 诊断路径按 `!headless` 门。
