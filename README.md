# RGL

现代 Vulkan 渲染与游戏引擎探索项目。目标是**最小游戏引擎内核 + 插拔模块**（渲染内核、物理插拔、脚本插拔、RPC 与资产管线对接编辑器，目标编辑器 Blender）；当前在渲染内核阶段。

探索项目：API 会变，不承诺稳定。`docs/` 下的设计与取证文档是项目的一部分。

## 技术要点

- **Mesh shader 管线**（task/mesh），全程 **ShaderObject + DescriptorHeap + DynamicState**
- MDI（multi-draw indirect）、DGC（device generated commands）、GPU 剔除、级联阴影；vendor FSR 3.1.4
- **C++20 modules** 全代码库，四层库（rlbase / rlgfx / rlscene / rlframe，设备依赖分层可机器验证）
- **Slang** 着色器语言 + 构建期反射管线：C++ ↔ shader 布局事实由生成物单一来源承载
- 测试与工具链：doctest 单测（无 GPU 无窗口毫秒级）、headless 冒烟/逐位截图回归、imgqa 区域归因判据、闭式解像素探针、样式/Doxygen/分层机器检查
- Linux / Windows 双平台，`just` 驱动

## 快速开始

```bash
just config      # CMake 配置（默认 clang + Release）
just build       # 增量编译
just run         # 运行
just test        # 单测（不需要 GPU/窗口）
just gates       # 一条命令跑完编译 + 测试 + 严格检查
```

前置：支持 modules 的 clang、CMake + Ninja、Vulkan loader、vcpkg（依赖）、Slang 工具链（细节见 `Justfile` 与 `docs/技术概念.md`）。

## 已知平台问题 / Known Platform Issues

- **gfx1151（Radeon 8060S / Strix Halo）— mesh shader MDI GPU hang, standalone reproducer available.**
  On RADV / Mesa 26.2.x, launching mesh-task dispatches via **multi-draw-indirect**
  (`vkCmdDrawMeshTasksIndirectMultiEXT`, ~1.5k rows × 3 segments) whose mesh shaders
  **actually dereference buffer data** triggers probabilistic GPU hangs
  (12/14 rounds × 400 frames): `amdgpu [gfxhub] page fault ... at 0x0`,
  `GCVM_L2_PROTECTION_FAULT_STATUS 0x701431` (client `SQC (data)`),
  `ring gfx timeout` → `MODE2 reset`; occasionally a secondary userspace crash,
  `libvulkan_radeon.so` calling through a NULL function pointer.
  Clean on NVIDIA / Intel / RDNA2 iGPU with the identical binary; direct dispatch
  (`vkCmdDrawMeshTasksEXT`) or trivial mesh shaders are clean — dose threshold
  ≈ 500–1500 mesh groups per dispatch, which is why CTS never caught it.
  Repro & full forensics: `tools/debt140_repro/` (see its `README` / `REPRODUCE.txt`),
  `docs/债140_跨驱动与上游取证.md`; upstream Mesa issue pending.
  Engine-side auto workaround for this PCI id: `meshlet.mdi_shadow_direct = "auto"`
  (direct-dispatch emulation, bit-identical output).

- **gfx1151（Radeon 8060S / Strix Halo）**：MDI × mesh 着色器真取数链会概率性 GPU 挂死
  （fault@0 + CP wedge；跨厂商对照 RDNA2 核显 / NVIDIA / Intel 全净）。
  完整取证与**独立最小复现器**：`tools/debt140_repro/`、`docs/债140_跨驱动与上游取证.md`；
  上游 issue 已投：https://gitlab.freedesktop.org/mesa/mesa/-/issues/16670。引擎内置该平台自动绕过：`meshlet.mdi_shadow_direct = "auto"`
  （直派模拟，输出逐位一致）。

## 仓库结构

| 目录 | 内容 |
|---|---|
| `rgl/` | 引擎核心（C++20 modules）与 Slang 着色器 |
| `tools/` | 检查、QA、复现工具链 |
| `tests/` | doctest 单测与设备原语 |
| `qa/` | 进程级集成（冻结配置 + 追加式台账） |
| `docs/` | 设计、技术概念、还债台账与调试取证 |
| `third_party/` | vendor 组件（各自原许可证） |

## 许可证

GPL-3.0（见 `LICENSE`）；`third_party/` 下组件随各自原许可证。
