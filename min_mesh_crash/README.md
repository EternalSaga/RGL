# min_mesh_crash — NVIDIA mesh shader + shader object 驱动 bug 最小复现

## 目的

最小复现 `docs/nvidia_compatibility_report.md` 中确认的 NVIDIA 驱动 bug：

> **mesh shader + shader object → Xid 13 (GPU engine reset)**

确认与 Slang 编译器、descriptor heap、swapchain 均无关。

## 环境

| 项目 | 值 |
|---|---|
| GPU | NVIDIA RTX 5090 (GB202) / AMD Granite Ridge (RAPHAEL_MENDOCINO) |
| 驱动 | NVIDIA 610.57.04 |
| OS | Gentoo Linux 6.18.43-lto |
| 模式 | prime offload（NVIDIA 渲染，AMD 输出） |

## 构建

```bash
cmake -B build_min -S min_mesh_crash
cmake --build build_min
```

依赖：Vulkan、SDL3、VMA、glslangValidator、Nsight Aftermath SDK（`$REPO/../Aftermath`）。
编译标志 `-fpermissive` 绕过 `VkShaderEXT` 不完整类型的 `void**` 转换检查。

## 运行

```bash
cd build_min
__NV_PRIME_RENDER_OFFLOAD=1 \
__NV_PRIME_RENDER_OFFLOAD_PROVIDER=NVIDIA-G0 \
__GLX_VENDOR_LIBRARY_NAME=nvidia \
./min_mesh_crash
```

程序自动选择 NVIDIA GPU（vendorID=0x10de）。

## 预期输出（NVIDIA 610.57.04）

```
frame 0 start
f0 ok
frame 1 start
f1 ok
...
frame 9 start
f9 ok

[Aftermath] dump 58668 bytes
[Aftermath] status=5
FAIL vkDeviceWaitIdle(dev)=-4
```

10 帧全部提交成功，`vkDeviceWaitIdle` 返回 `VK_ERROR_DEVICE_LOST`，GPU 异步 engine reset。

## 预期输出（AMD RADV）

10 帧正常完成，`vkDeviceWaitIdle` 返回 `VK_SUCCESS`，无 crash。

## 设计决策

| 项目 | 选择 | 原因 |
|---|---|---|
| 着色器 | 纯 GLSL（glslangValidator 编译） | 排除 Slang 编译器影响 |
| 渲染目标 | 离屏 image（无 swapchain） | NVIDIA 无 display output，避免 present crash 干扰 |
| 诊断 | Nsight Aftermath SDK | 捕获 GPU crash dump，输出 `min_dump.bin` + `min_dump.json` |
| 扩展 | mesh_shader + shader_object + descriptor_heap | heap 存在但不被着色器引用，用于对照测试 |

## 排查结论

| 配置 | 结果 |
|---|---|
| mesh shader + shader object (GLSL) | ❌ Xid 13 |
| mesh shader + shader object (Slang) | ❌ Xid 13 |
| mesh shader + shader object (无 heap) | ❌ Xid 13 |
| vertex shader + shader object | ✅ 正常 |

**根因**：NVIDIA RTX 5090 驱动 610.57.04 不支持 `VK_EXT_mesh_shader` + `VK_EXT_shader_object` 组合。
Aftermath dump 确认 GPU engine reset（status=5/DeviceReset），非 page fault，非内存访问错误。

## 文件结构

| 文件 | 说明 |
|---|---|
| `main.cpp` | 单文件最小 Vulkan 程序：实例→设备→着色器→10帧渲染→crash |
| `CMakeLists.txt` | 构建配置，glslangValidator 编译 GLSL |
| `shaders/mesh.glsl` | 硬编码三角形 mesh shader（3 顶点，1 三角形） |
| `shaders/frag.glsl` | 纯色 fragment shader（红色） |
| `vam_impl.cpp` | VMA 实现（`#define VMA_IMPLEMENTATION`） |

## 参考

- `docs/nvidia_compatibility_report.md` — 完整排查报告
- `docs/DESCRIPTOR_HEAP_SPEC.md` — descriptor heap 排坑记录
