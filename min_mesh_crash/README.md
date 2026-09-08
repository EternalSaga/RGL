# min_mesh_crash — mesh shader + shader object 崩溃最小复现（根因：缺 NO_TASK_SHADER flag）

## 结论（2026-08-17 确认）

**不是 NVIDIA 驱动 bug，是应用侧 spec 合规问题。** 根因已确认并修复：

mesh shader 对象创建时缺少 `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT` flag，同时 TASK stage 绑定 `VK_NULL_HANDLE` 并发出 `vkCmdDrawMeshTasksEXT`。

规范对 flag 的语义定义（`VK_EXT_shader_object`, shaders.adoc）：

> `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT` specifies that a mesh shader **must only be used without a task shader. Otherwise, the mesh shader must only be used with a task shader.**

即：不带该 flag 的 mesh shader 对象，契约上**必须**配一个有效 task shader。NVIDIA 驱动 610.57.04 遵守这一契约，draw 时按 "task→mesh" 模式查找关联的 task shader，拿到空指针 → GPU page fault at VA=0x0 → engine reset → `VK_ERROR_DEVICE_LOST`。

## 为什么之前误判为驱动 bug

1. **Validation Layer 查不出来（spec 漏洞）**：本应报错的 VUID `vkCmdDrawMeshTasksEXT-None-08694`（"mesh shader 未带 NO_TASK_SHADER flag 时，TASK stage 必须绑定有效 task shader"）**仅在 `taskShader` 和 `meshShader` 两个 feature 都启用时才生效**。本程序只启用了 `meshShader`，所以 VVL 完全静默。这是已知的 spec/校验缺口（Vulkan-ValidationLayers issue #12886）。
2. **AMD (RADV) 不崩溃**：RADV 对此宽松——即使没有 flag，也按 mesh-only 模式直接发射 mesh shader。因此 rgl 在 AMD Strix Halo 上一直正常，掩盖了问题。
3. vertex+fragment shader object 路径不涉及 task/mesh 契约，正常工作，进一步加深了"mesh 组合被驱动特殊对待"的错觉。

## 修复

```cpp
// 修复前
createInfos[0].flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
// 修复后（main.cpp:252-253）
createInfos[0].flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
createInfos[0].flags |= VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
```

同 bug 也存在于 rgl，已同步修复：`rgl/shader.cppm:67`
（`meshCreateInfoRaw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT | VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;`）

## 验证结果（RTX 5090, 驱动 610.57.04）

| 配置 | 修复前 | 修复后 |
|---|---|---|
| Vertex shader + shader object | ✅ 正常 | ✅ 正常（且 VVL 零错误） |
| Mesh shader + shader object（无 NO_TASK_SHADER） | ❌ GPU page fault VA=0x0, frame 0 提交成功、frame 1 等待时 `VK_ERROR_DEVICE_LOST`，Aftermath dump 显示 `Engine reset occurred: true` | — |
| Mesh shader + shader object（**有 NO_TASK_SHADER**） | — | ✅ 10/10 帧全部 `fN ok`，VVL 零错误 |
| rgl（NVIDIA RTX 5090） | ❌ 同样崩溃 | ✅ 正常渲染 |
| rgl（AMD RADV，VK_DRIVER_FILES 强制 RADV） | ✅ 正常 | ✅ 正常（flag 跨厂商兼容） |

## 环境

| 项目 | 值 |
|---|---|
| GPU | NVIDIA GeForce RTX 5090 (GB202) / AMD Strix Halo (RADV) |
| 驱动 | 610.57.04 |
| Vulkan | 1.4.341 |
| OS | Gentoo Linux 6.18.43-lto |
| 模式 | prime offload (NVIDIA) / 原生 (AMD) |

## 构建

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH="/path/to/vcpkg/installed/x64-linux-native"
cmake --build build
```

依赖：Vulkan、SDL3、glslangValidator、Nsight Aftermath SDK（`$REPO/../Aftermath`）。

## 运行

NVIDIA（prime offload，带 validation layer）：

```bash
cd build
__NV_PRIME_RENDER_OFFLOAD=1 \
__NV_PRIME_RENDER_OFFLOAD_PROVIDER=3 \
__GLX_VENDOR_LIBRARY_NAME=zink \
__VK_LAYER_NV_optimus=NVIDIA_only \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
./min_mesh_crash
```

AMD（隐藏 NVIDIA ICD 强制 RADV；注意本复现程序无条件启用 NV 诊断扩展，RADV 会因 `VK_ERROR_EXTENSION_NOT_PRESENT` 拒绝创建 device——这是复现程序的局限，跨厂商验证请用 rgl）：

```bash
cd build
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json \
RADV_EXPERIMENTAL=heap ./min_mesh_crash   # 会失败于 createDevice（缺 NV 扩展）
```

## 代码修复记录

以下按时间顺序记录本程序从"无法运行"到"spec 全合规 + 双厂商通过"的所有变更。

### 0. 【真正的根因修复】mesh shader 补 `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT`

见上文"修复"节。这是唯一导致 NVIDIA 上 page fault 的变更，A/B 测试确认：去掉该 flag 立刻复现 engine reset，加上立刻 10/10 帧通过。

### 1. Feature chain 补全

```cpp
VkPhysicalDeviceVulkan13Features f13{...};
f13.dynamicRendering = VK_TRUE;    // vkCmdBeginRendering 需要
f13.synchronization2 = VK_TRUE;    // vkCmdPipelineBarrier2 需要
f13.maintenance4 = VK_TRUE;        // LocalSizeId 需要 (mesh shader SPIR-V 使用)

VkPhysicalDeviceVulkan12Features f12{...};
f12.bufferDeviceAddress = VK_TRUE; // vkGetBufferDeviceAddress 需要

VkPhysicalDeviceShaderObjectFeaturesEXT sof{...};
sof.shaderObject = VK_TRUE;        // vkCreateShadersEXT 需要

VkPhysicalDeviceMeshShaderFeaturesEXT mf{...};
mf.meshShader = VK_TRUE;           // mesh shader 需要
// 注意：taskShader 保持 FALSE。
// 副作用：VUID 08694/08695（task shader 绑定规则）因此不生效 → VVL 静默（spec 漏洞）。
```

### 2. 设备扩展补全

- `VK_KHR_maintenance4` — maintenance4 feature 所需
- `VK_KHR_maintenance5` — `VK_EXT_descriptor_heap` 隐含依赖
- 移除 `VK_KHR_swapchain` — offscreen 渲染无需 swapchain

### 3. Shader 创建

- 使用 `VK_SHADER_CREATE_LINK_STAGE_BIT_EXT` flag + `nextStage` 指定 stage 链接
- 同一次 `vkCreateShadersEXT` 调用中的所有 graphics stage 元素都必须带 `LINK_STAGE`（VUID 08402/08403）——mesh+frag、vert+frag 两个 pair 的 frag 元素均已补上
- vertex 测试使用**独立创建**的 vert+frag pair，不复用与 mesh linked 的 frag（否则触发 VUID 08698：linked 的同批 shader 必须全部绑定）

### 4. Shader 绑定

绑定所有 7 个 stages，未使用的 stages 设为 `VK_NULL_HANDLE`：
```cpp
VkShaderStageFlagBits stages[] = {VERTEX, TESS_CTRL, TESS_EVAL, GEOMETRY, TASK, MESH, FRAGMENT};
VkShaderEXT bindShaders[] = {VK_NULL_HANDLE, ..., meshS, fragS};
vkCmdBindShadersEXT(cb, 7, stages, bindShaders);
```

### 5. 完整动态状态

参考 `nvvk::GraphicShaderObjectPipeline::cmdSetPipelineState`（`nvpro_core/nvvk/pipeline_vk.hpp:728`），设置所有 required dynamic state：
- Viewport, Scissor, LineWidth, CullMode, FrontFace
- DepthBiasEnable, DepthBias, DepthClampEnable, PolygonMode
- RasterizerDiscardEnable, BlendConstants
- DepthBounds, DepthBoundsTestEnable, DepthCompareOp
- DepthTestEnable, DepthWriteEnable, StencilTestEnable, StencilOp
- PrimitiveRestartEnable, PrimitiveTopology
- RasterizationSamples, SampleMask, AlphaToCoverageEnable, AlphaToOneEnable
- ColorBlendEnable, ColorBlendEquation, ColorWriteMask, LogicOpEnable
- VertexInputEXT（mesh-only draw 下该 state 不被消费，调用无害；vertex 路径下为必需）

### 6. Mesh shader 输出

```glsl
gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
SetMeshOutputsEXT(3, 1);  // vertexCount=3, primitiveCount=1
```

glslang 生成 `OpSetMeshOutputsEXT 3 1`，与 `layout(max_vertices=3, max_primitives=1)` 一致（已用 spirv-dis 验证）。

### 7. Image 内存绑定顺序

先 `vkBindImageMemory` 再 `vkCreateImageView`（validation 要求）。

### 8. 多帧同步

`vkWaitForFences` 参数顺序：`(device, fenceCount, pFences, waitAll, timeout)` — `waitAll` 在 `timeout` 之前。

### 9. VMA 移除

VMA vcpkg 构建版本过低（无 Vulkan 1.2+ 支持）。代码不使用 VMA 功能，直接使用 Vulkan 内存 API。

## 文件结构

| 文件 | 说明 |
|---|---|
| `main.cpp` | 单文件 Vulkan 程序，包含 vertex+fragment 测试和 mesh shader 测试 |
| `shaders/mesh.glsl` | Mesh shader（硬编码三角形，mesh-only，无 task） |
| `shaders/vert.glsl` | Vertex shader（用于诊断：证明 shader object 基础设施正常） |
| `shaders/frag.glsl` | Fragment shader（纯色） |
| `CMakeLists.txt` | 构建配置 |
| `vam_impl.cpp` | VMA 实现（`VMA_IMPLEMENTATION`） |

## 如何验证

1. **shader object 基础设施正常**：`VERT_TEST: SUCCESS` 输出
2. **根因复现**：注释掉 main.cpp:253 的 `NO_TASK_SHADER` flag → frame 0 提交成功、frame 1 `vkWaitForFences` 返回 -4 (`VK_ERROR_DEVICE_LOST`)，dmesg/Aftermath 显示 engine reset
3. **修复确认**：恢复 flag → 10 帧全部打印 `fN ok`，且 `VK_LAYER_KHRONOS_validation` 零错误
