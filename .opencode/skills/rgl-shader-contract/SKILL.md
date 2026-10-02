---
name: rgl-shader-contract
description: 修改 Slang 着色器契约与反射管线——just reflect、scalar block layout、bindless 原则、DrawArgs/scene.slang 字节布局与关键契约文件位置表。触发词：改 shader、slang、schema、反射、reflect、DrawArgs、scene data、generated 产物、字节偏移。
---

# 着色器契约与反射管线

## 编译与反射流程

- `rgl/shaders/` 下的 Slang → SPV 输出到 `build/rgl/generated_shaders/`（CMake `compile_slang()` 构建时编译；运行时相对路径读取）
- **反射管线显式触发（C API 路径，2026-09-10 起）**：`generated/*.cppm` + `reflection_manifest.txt` 为**已提交产物**，只在改契约后 `just reflect` 重生成并随改动提交；构建开头 `reflection_check` 比对输入 SHA1，忘跑 = FATAL 指路。共 **7 条管线**（types/model/material/lights/scene_forward 反射生成 + features/param_semantics 模板生成；旧文档"5 条"系 scene_forward 2026-09-10 加入后未同步，"6 条"系批 10.6 param_semantics 加入后未同步），Profile `sm_6_6+spirv_1_6` + capability `spvDescriptorHeapEXT`
- **5 条 slang 管线走 `reflection_tool slang` 子命令**（链接 libslang.so 直接查 C API，**不产 JSON 中间层**）：struct 布局 = 编译器实查真值（逐字段 offsetof static_assert），schema 降为提取清单 + 交叉校验期望值。详见 `docs/archive/slang_api_reflection_research.md`
- **Scalar Block Layout（定案）**：所有 slangc 编译（反射 + SPV 两处 flag **必须一致**）统一 `-fvk-use-scalar-layout`——C++（glm 自然对齐）与 shader 逐字段偏移天然一致零 padding（`DrawArgs` 4B）。前提 `scalarBlockLayout` feature（features_config.json + validateFeatures）
- **bindless 原则**：push constant 冻结在"纯索引"级别（Phase 7b+ 起 `DrawArgs` 仅 `objectIndex` 4B，不再扩）——场景级/每帧变化的数据（光照、对象矩阵、MVP、材质、env 参数、灯数组）一律进 descriptor heap（scene data）。**端侧拆分**：shader 端保留 `[[vk::push_constant]]` 属性（`material.slang`），CPU 端经 `vkCmdPushDataEXT` 供给——`vkCmdPushConstants` 与 descriptor heap 互斥，该 CPU API 被 source_checks 护栏禁用（见 skill `rgl-new-module` 红线）
- **scene data / ByteAddressBuffer 数值契约（守卫已就位，无需背细则）**：浮点读数 `asfloat()` 位转、Load/Store 偏移=字节、屏幕→世界 ndc 走引擎口径（fb 行 0 ↔ +1）——回归由 `slang_load_bitcast_checks`（source_checks）+ `tests/test_izb_engine_contract.cppm`（引擎真 SPV 闭式）+ imgqa `izb`/`swim` 判据拦截；根因史 → 调试历史 2026-10-02 补账条目

## 关键文件位置（契约与接缝）

| 位置 | 说明 |
|---|---|
| `shaders/material.slang` | **统一 push constant 契约**：`DrawArgs`（Phase 7b+ 仅 `objectIndex` 4B）单一真相源 → schema 校验 → `material_types_generated` |
| `shaders/scene.slang` | **场景级 bindless 契约**：`sceneData`（binding 3）+ `SceneLight`（@0 前向头，**C API 反射生成** `scene_forward_types_generated`，双源已消）+ `readSceneLight`/`readObjectMatrix`/`readObjectMvp`/`readObjectDraw`/`worldPos`/`worldNormal`/`clipPos`；@0-128 头 + 各 per-object 表字节布局 = shader 字面量（per-object 表仍手工一致，见已知问题） |
| `shaders/env.slang` / `sky.slang` | env 采样契约（binding 4/5 + @48）/ 天空盒契约（@64 invViewProj 重建射线） |
| `shaders/lights.slang` | **多灯契约**（反射管线 4）：`GpuLight` + `readLightCount/readGpuLight` 读尾区 @262272；`light.frag` import，forward 不 import |
| `shaders/model_pbr/model_phong/model_pbr_ibl.frag` | 三条 frag 路线（GGX / Blinn / GGX+简化 IBL），`scene.lua styles` 一行切换 |
| `shaders/deferred_geometry.frag` + `light.mesh/frag` | deferred 路线：4-MRT G-buffer 写入（oct 法线）/ 全屏大三角逐灯 GGX + **简化 IBL**（env 采样替换常 ambient，与 model_pbr_ibl 逐式相同）+ **skybox 背景合成**（天空像素 = position 清 0，重建射线采 env）；env 走 light pass binding 9/10（4/5 被 G-buffer 占）；`kGBufferAttachments` 表定格式 |
| `shaders/model_schema.lua` `constants` | 契约常量单一真相源（`kTrianglesPerGroup` 等 → 生成 constexpr）；`HeapSlotGeometryBase` = 用户槽区起点 = 几何段基址 |
| `rgl/builtin_geometry.cppm` | **内建几何契约**（架构债务 1 方案 B）：cube 等程序化几何的上传/注册，与 gltf 同路；`vk_resources` 不认识任何内容 |
| `shaders/cull.comp.slang` + `rgl/gpu_cull.cppm` | **GPU culling 契约**（Phase 7b）：CullEntry 60B / 序列 120B（execSlot 2j/2j+1 + DrawArgs@8(4B) + meshCmd@108）/ CullPush 100B（planes[6]+sequenceCount）/ count 64B（total@4，@0 保留未用）；CPU 算 mvp 进 scene data per-object 表、GPU 只写可见性（保 forward diff0）；DGC 序列契约见 `vk_indirect_commands.cppm` + `docs/device_generated_commands_research.md` §4.4。静态 DGC 路追加 `shaders/stream_fanout.comp.slang`（per-entry 满槽孔洞流扇出，`dgc_static` 门控，见 `docs/archive/radv_static_dgc_spike.md`） |
| `configs/*.lua` | 运行时配置（sol2 沙箱，字段可选回落默认）；**与 `shaders/*_schema.lua`（build 时契约）是两套机制，勿混** |
