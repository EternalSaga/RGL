# NVIDIA 跨平台兼容性排查报告

**日期**: 2026-08-12
**环境**: Gentoo + RTX 5090 (GB202) / AMD Granite Ridge (Raphael_Mendocino)
**驱动**: NVIDIA 595.84 → 610.57.04
**状态**: AMD ✅ | NVIDIA ✅ (vertex shader) | NVIDIA ❌ (mesh shader + shader object 驱动 bug)

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
| `Justfile:70` | 条件化 `RADV_EXPERIMENTAL=heap` | `lspci` 检测 AMD GPU 时保留环境变量，NVIDIA 上跳过 |
| `features_config.json` | 移除 `descriptorHeapCaptureReplay` | NVIDIA 595.84 不支持此 feature（610 驱动支持） |
| `vkresources.cppm` | Slot 编号动态化 | NVIDIA reservedSlots=6048，原固定 slot 8/12 在 reserved range 内部 |
| `VmaBuffer.cppm` | GPU address 对齐 | 修正 `resourceHeapAlignment` 对齐（NVIDIA=32B）+ CPU mapped data 对齐 |
| `shader.cppm` | frag shader mappingInfo 移除 | frag shader 无 binding，`pNext = nullptr` 即可 |
| `shader.cppm:44` | `mappings[0].resourceMask` 改为 `ALL_EXT` | NVIDIA 驱动可能更严格校验 resourceMask 与 SPIR-V 资源类型的匹配 |
| `shader.cppm:66,78` | 添加 `VK_SHADER_CREATE_LINK_STAGE_BIT_EXT` | NVIDIA 官方 sample 使用此 flag，影响 shader 二进制生成 |
| `CMakeLists.txt` | Slang profile 改为 `glsl_460+spirv_1_6` | 匹配 NVIDIA sample 的编译配置 |
| `vcpkg.json` + `CMakeLists.txt` | slang 依赖标准化 | vcpkg 管理 slang 编译器 |

---

## 三、NVIDIA descriptor heap 属性对比

| 属性 | NVIDIA RTX 5090 | AMD Granite Ridge (RADV) |
|---|---|---|
| `bufferDescriptorSize` | **16** | **16** |
| `minResourceHeapReservedRange` | **96768** (6048 slots) | **0** |
| `resourceHeapAlignment` | **32** | **4** |

**关键差异**：
- 两者 descriptor size 相同（16 字节）
- NVIDIA reserved range 巨大（6048 slots vs 0）— 需要跳过 reserved range 才能使用
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

### 5.3 结论

**NVIDIA Xid 13 根因**：RTX 5090 (GB202) 驱动 610.57.04 不支持 `VK_EXT_mesh_shader` + `VK_EXT_shader_object` 的组合。

- `mesh shader` + `shader object` → ❌ Xid 13（驱动 bug）
- `vertex shader` + `shader object` + `descriptor heap` → ✅ 正常工作
- Class 0000ce97 是 GB202 的 GPFAS 类，确认错误发生在 mesh shader 执行阶段
- NVIDIA 官方 sample 没有 mesh shader + shader object 的组合验证了这一点

### 5.4 临时解决方案

将 mesh shader 替换为 vertex shader，使用 `SV_VertexID` 从 ByteAddressBuffer 加载顶点数据。

**已验证工作**：
| 配置 | NVIDIA RTX 5090 | AMD Granite Ridge (RADV) |
|---|---|---|
| vertex shader + shader object + descriptor heap | ✅ | ✅ |
| mesh shader + shader object + descriptor heap | ❌ Xid 13 | ✅ |
| mesh shader + traditional pipeline | ⚠️ 未测试 | ✅ (NVIDIA sample) |

---

## 六、GPU 诊断工具集成

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

## 七、参考

- NVIDIA 官方 sample：`/home/robin/mounts/gm7000/program/vk_mini_samples/`
- `docs/DESCRIPTOR_HEAP_SPEC.md` — descriptor heap 排坑记录
- Vulkan spec Debugging chapter: https://docs.vulkan.org/spec/latest/chapters/debugging.html
