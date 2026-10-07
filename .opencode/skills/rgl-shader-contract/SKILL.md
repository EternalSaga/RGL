---
name: rgl-shader-contract
description: 修改 Slang 着色器契约与反射管线——just reflect、scalar block layout、bindless 原则、关键契约文件位置表。触发词：改 shader、slang、schema、反射、reflect、DrawArgs、scene data、generated 产物、字节偏移。
---

# 着色器契约与反射管线

## 编译与反射流程

- `rgl/shaders/` 的 Slang → SPV 输出到 `build/rgl/generated_shaders/`（CMake `compile_slang()`
  构建时编译；运行时按相对路径读，故 CWD 必须是 `build/rgl/`）
- **反射管线显式触发**：`generated/*.cppm` + `reflection_manifest.txt` 是**已提交产物**，只在改
  契约后 `just reflect` 重生成并随改动提交；构建开头 `reflection_check` 比对输入 SHA1，
  忘跑 = FATAL 指路。共 **7 条**（types/model/material/lights/scene_forward 反射生成 +
  features/param_semantics 模板生成），Profile `sm_6_6+spirv_1_6` + `spvDescriptorHeapEXT`
- 5 条 slang 管线走 `reflection_tool slang` 子命令（直接查 slang C API，**不产 JSON 中间层**）：
  struct 布局 = 编译器实查真值（逐字段 `offsetof` static_assert），schema 降为提取清单 + 交叉校验
- **Scalar Block Layout（定案）**：所有 slangc 编译（反射 + SPV 两处 flag **必须一致**）统一
  `-fvk-use-scalar-layout`——C++（glm 自然对齐）与 shader 逐字段偏移天然一致零 padding。
  前提 `scalarBlockLayout` feature（features_config.json + validateFeatures）
- **bindless 原则**：push constant 冻结在"纯索引"级别（`DrawArgs` 仅 `objectIndex` 4B，不再扩）
  ——场景级/每帧变化的数据（光照、对象矩阵、MVP、材质、env 参数、灯数组）一律进 descriptor heap
  （scene data）。**端侧拆分**：shader 端保留 `[[vk::push_constant]]` 属性，CPU 端经
  `vkCmdPushDataEXT` 供给（`vkCmdPushConstants` 与 descriptor heap 互斥，已被 source_checks 禁用）
- **字节布局不抄文档**：C++↔shader 的每个结构都有 `static_assert(sizeof(...) == N, "...drifted...")`
  钉住（先例 `gpu_cull.cppm`），shader 侧偏移由反射实查。要布局数字**去读 static_assert 和
  `generated/*_generated.cppm`**，别信任何散文里的数字（债-19 就抓到过注释把 722240B 写成 262800B）。
  数值契约回归由 `slang_load_bitcast_checks`（source_checks）+ `tests/test_izb_engine_contract.cppm`
  + imgqa `izb`/`swim` 判据拦截。

- **glm 两个 define 改的是"静默错"（写错不报错，只在新机位上崩）**：`GLM_FORCE_RADIANS` 使
  `glm::radians()` 退化成**恒等函数**（配置字段叫 `fovDegrees`，角度→弧度须手动换算）、
  `GLM_FORCE_DEPTH_ZERO_TO_ONE` 使 clip z∈`[0,1]`（线性深度重建 / shadow map 比较口径）。
  细则以 `rgl/camera.cppm` 与 `rgl/scene_context.cppm` 的头注释为准，相机数学回归 = `tests/test_camera.cppm`。

## 关键文件位置（契约与接缝）

| 位置 | 说明 |
|---|---|
| `shaders/material.slang` | **统一 push constant 契约**：`DrawArgs` 单一真相源 → schema 校验 → `material_types_generated` |
| `shaders/scene.slang` | **场景级 bindless 契约**：`sceneData`（binding 3）+ `SceneLight`（前向头，**C API 反射生成**，双源已消）+ `readSceneLight`/`readObjectMatrix`/`readObjectMvp`/`readObjectDraw`/`worldPos`/`worldNormal`/`clipPos` |
| `shaders/env.slang` / `sky.slang` | env 采样契约（binding 4/5）/ 天空盒契约（`invViewProj` 重建射线） |
| `shaders/lights.slang` | **多灯契约**：`GpuLight` + `readLightCount`/`readGpuLight`；`light.frag` import，forward 不 import |
| `shaders/model_pbr/model_phong/model_pbr_ibl.frag` | 三条 frag 路线（GGX / Blinn / GGX+简化 IBL），`scene.lua styles` 一行切换 |
| `shaders/deferred_geometry.frag` + `light.mesh/frag` | deferred 路线：多 MRT G-buffer 写入（oct 法线）/ 全屏大三角逐灯 GGX + 简化 IBL + skybox 背景合成（天空像素 = position 清 0）；格式由 `kGBufferAttachments` 表定 |
| `shaders/model_schema.lua` `constants` | 契约常量单一真相源（→ 生成 constexpr）；`HeapSlotGeometryBase` = 用户槽区起点 = 几何段基址 |
| `rgl/builtin_geometry.cppm` | **内建几何契约**：cube 等程序化几何的上传/注册，与 gltf 同路；`vk_resources` 不认识任何内容 |
| `shaders/cull.comp.slang` + `rgl/gpu_cull.cppm` | **GPU culling 契约**：布局走 static_assert（见上）；CPU 算 mvp 进 scene data per-object 表、GPU 只写可见性（保 forward diff0）；DGC 序列契约见 `vk_indirect_commands.cppm` + `docs/device_generated_commands_research.md` |
| `configs/*.lua` | 运行时配置（sol2 沙箱，字段可选回落默认）；**与 `shaders/*_schema.lua`（build 时契约）是两套机制，勿混** |
