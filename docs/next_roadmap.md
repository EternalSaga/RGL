# RGL 路线图

> **状态**：现行（开发路线图；当前节点 = Phase 7 进行中：D1 延迟单灯 `70ce9d1` + D3 多灯/反射 +
> 批次 4 CPU culling 完成，批次 5=7b / 批次 6 文档销账待续）

Phase 0–6 已全部完成（各阶段结论一段话归档于下，实施细节与计划-实现偏离的权威记录在
`docs/archive/phaseX_plan.md`）。`legacy/`（旧 OpenGL 项目）仅思路参考、非工作重点（见 AGENTS.md）。

---

## 核心原则（现行）

1. **反射是 build tool，不是 runtime component**。反射结果只输出为 `.cppm`，渲染代码经 `import`
   消费，永不依赖反射系统实现。
2. **编译期安全网**。生成 `.cppm` 含 `static_assert`，C++ struct 与 shader 布局不一致直接编译报错。
3. **glTF only**。放弃 Assimp，只用 tinygltf。
4. **逐步推进**。每阶段稳定后再进入下一阶段。
5. **bindless 原则**（Phase 5 定案，推翻"push constant 传递轻量"）：push constant 冻结在
   "mvp + 索引"级别，场景级/每帧变化数据一律进 descriptor heap（scene data）。

## C++ 风格备忘（遗留方向）

`std::expected` 错误处理已全量落地（vkerror + 初始化 .value() 展开 + 运行时 [[nodiscard]]）。
未落地：`std::span` 全面替代指针+长度、`flat_map` 查找容器、`consteval` 编译期构造 Vulkan 结构体、
scope guard——低优先级，随改随做。

---

## 已完成阶段结论（Phase 0–6）

### Phase 0：Shader Reflection（已完成）
反射管线定型：`tools/`（schema_loader/validator/generator + reflection_tool CLI，inja 模板，
rllogger）+ `slangc -reflection-json` + `*_schema.lua` 契约 → 生成 `types_generated`。
shader/vkresources/vkrenderer import 生成模块，硬编码常量清零。细节 → [archive/shader_reflection_plan.md](archive/shader_reflection_plan.md)。

### Phase 1：Camera + Input（已完成 2026-08-22）
`camera.cppm`（trackball/FPS 双模式，统一 yaw/pitch 姿态）+ **`configs/*.lua` sol2 沙箱模式确立**
（缺文件回落默认 + 字段可选 + 禁 dofile——后续所有配置模块的模板）。eventSystem 中央轮询
（唯一 SDL_PollEvent）。GLM_FORCE_RADIANS 陷阱修复（fov 恒等函数坑）。

### Phase 2：glTF 加载（已完成 2026-08-23）
tinygltf + `gltf_loader`（去索引展开 + 包围球）+ `model_importer`（顶点上传 heap + registerScene
ECS 化）+ `scene_config`；第二条反射管线（`model_types_generated`）；前置解耦 = `RenderState`
动态状态外移 + Renderer 管线表（`PipelineEntry`）；自适应 near/far。**ECS 五规范自此为参照系**
（合规审查 → [archive/ecs_compliance_review.md](archive/ecs_compliance_review.md)）。
细节 → [archive/phase2_plan.md](archive/phase2_plan.md)（含 §7 计划外发现）。

### Phase 3：纹理管理（已完成 2026-08-28）
bindless 纹理：`AssetTable`（TextureAsset/MaterialAsset）+ 自由函数上传（`TexturePool` 类未落地——
main 局部对象 UAF 教训，整体降为自由函数 + `allocSlot` 统一分配）+ **独立 sampler heap**；
描述符统一步长 = `max(imageDescriptorSize, bufferDescriptorSize)`（三驱动实测：RADV/Intel/NVIDIA
全 PASS）。细节 → [archive/phase3_plan.md](archive/phase3_plan.md) + [archive/heap_texture_research.md](archive/heap_texture_research.md)。

### Phase 4：多 Shader + 统一契约（已完成 2026-08-28/30）
`material.slang` `DrawArgs` 超集（第三条反射管线，scalar 布局）+ `shader_registry` 管线去重
（392 primitive → 12 条目，按 (规格,顶点 slot,纹理 slot) 组合）+ 分块拼接顶点 buffer +
`styleMap` 风格映射。遗留 cottage artifact 根因 = 两个自 Phase 2 的越界 bug（uv 步长 /
triangles 组内索引，Windows/NVIDIA 移植暴露——"非确定性驱动竞态"系误诊，教训见 archive）。
细节 → [archive/phase4_plan.md](archive/phase4_plan.md)。

### Phase 5：透明排序 + 简单光照（已完成 2026-08-31）
双段 FrameDrawList + 距离² 稳定排序（像素级验收）+ **scene data bindless 基建**
（`LightDataGpu@0` + 对象矩阵表 + staging ring → `vkCmdCopyBuffer`——全项目首个每帧动态 GPU
数据通路，Phase 7 shadow/culling 复用）+ 双光照风格（`model_pbr` GGX / `model_phong` Blinn）。
细节 → [archive/phase5_plan.md](archive/phase5_plan.md)。

### Phase 5.5：IBL（简化版）+ 天空盒（已完成 2026-09-01）
`uploadCubeMap` 6 面直传（RADV linear staging 三坑 → 6 张独立 2D staging + mip blit NEAREST）+
简化 IBL（`env.slang` binding 4/5，无 prefiltered/BRDF LUT）+ 天空盒（全屏大三角 clip→world 射线）
+ 三段 `[opaque|skybox|transparent]` + `PipelineBindings` mapping 顺序填充修正。
细节 → [archive/phase5_5_plan.md](archive/phase5_5_plan.md)。

### Phase 6：Lua 渲染意图（架构升级，已完成 2026-09-03）
`render_graph` 模块（`RgGraphDesc` → `compileRenderGraph` 校验 + **barrier 自动推导** +
present/waitStage 派生）；drawFrame 手写布局转换清零、pass 循环盲执行（验收 = 截图位级一致）；
`configs/render_graph.lua`（sol2 沙箱）；offscreen DIAG 转正为图变体（blit pass +
`ensureRtAttachment`）；scene.lua entities 列表化（A1/A2 两段 heap 布局时序契约）。
业界定位 = Frostbite 第一代全自动图保留"自动 barrier"核心一条；aliasing/fusion/多线程录制
明确不做。**实施记录与六条偏离注记 → [archive/phase6_record.md](archive/phase6_record.md)**。

---

## Phase 7：延迟渲染（进行中）

> **实施计划**：[archive/phase7_plan.md](archive/phase7_plan.md)（批次拆解、范围决策 D1–D6、验证矩阵）。
> **开发日志**（过程/踩坑滚动记录）：[phase7_devlog.md](phase7_devlog.md)。
> 测试主场景 = `assets/bistro_exterior.glb`（转换管线 `tools/bistro_pack.py`）。

### 实施进度（2026-09-08）

| 批次 | 内容 | 状态 | 剩余 |
|---|---|---|---|
| **0** Bistro + loader 节点 TRS（forward） | nodeWorld mat4 父链、Bistro forward、packer、`kSceneMatrixCapacity`↑4096 | **✅ 完成** | normal 绿通道翻 + BC5 Z 重建（→ Phase 8） |
| **1** G-buffer MRT + offscreen 显式格式 | 格式词表、4-MRT `deferred_geometry`、`RGL_DUMP_GBUF` 直读验 albedo | **✅ 核心完成** | normal/metalRough **纹理**采样（→ Phase 8） |
| **2** Light pass + sampleRead 接线 | SAMPLED_IMAGE 描述符入 heap、全屏 light pass GGX、forward/deferred 切换 | **✅ 核心完成** | ①light pass 简化 IBL ②选路走 render_graph.lua（现 env）③skybox 背景合成 |
| **3** 多灯 + 灯数据并入反射（D3） | 第 4 条反射管线 `GpuLight`、scene data 尾区灯数组 @262272（头不动保 forward diff0）、`lights` 配置、逐灯循环 + 点光衰减 | **✅ 核心完成** | 灯 entity 化（ECS 规范 3）；forward `LightDataGpu@0` 头仍手工双源；shadow map |
| **4** CPU frustum culling | 逐图元 node-local `CullSphere`、Gribb-Harman 6 面（z∈[0,1]）、可见统计 | **✅ 核心完成** | 真 AABB（现球近似，够 frustum） |
| **5** GPU culling / DGC | `vk_indirect_commands`、compute cull、NVIDIA/RADV 双路 | **⬜ 未开工** | 按 D5 归 **Phase 7b** |
| **6** 文档销账 | — | **🔶 部分** | 完整销账待批次 5 |

**净结论**：延迟渲染单灯核心 + 多灯 + CPU culling 跑通——deferred 逐灯循环（定向+点光、per-light
颜色、距离衰减）headless 零 VVL error；forward 只读 `lights[0]` diff0 不变；CPU 剔除默认视角
1571/20、背向 0/1591、开剔除图像与不剔一致（保守球不误剔）。验证方法学注：**BIT-IDENTICAL
比对须同会话双构建（stash 基线 vs 改动），跨日全量重建有 ±1 LSB 噪声**（调试历史 2026-09-08）。

### 剩余计划

- **批次 5 = Phase 7b GPU culling**（`VK_EXT_device_generated_commands`）：culling compute 输出
  per-visible sequence → NVIDIA execution-set 路径 / RADV `vkCmdDrawMeshTasksIndirectCountEXT`
  回退；能力门控双路选路。设计细节与双驱动实测 → [device_generated_commands_research.md](device_generated_commands_research.md) + archive/phase7_plan.md。
- **批次 2 三个 refine**：deferred light pass 简化 IBL、选路从 env 转正 render_graph.lua、
  skybox 背景合成进 deferred 图。
- **灯 entity 化**（ECS 规范 3）+ forward `LightDataGpu@0` 头并入反射（Phase 7 多灯已把灯数组
  入反射管线 4，前向头为 diff0 暂留手工双源，见 AGENTS 已知问题）。
- **批次 6 文档销账**（待批次 5）。

### 设计决策（现行）

- 延迟渲染为可选路径，不破坏 forward 管线（`RGL_DEFERRED` 选路，forward diff0 是硬约束）。
- GPU culling = 同一 culling 输出驱动两条提交路径，驱动能力升级后自动切换，不改 culling 逻辑。
- CullSphere（node-local 剔除球）已随批次 4 落地——原"AABB 组件 Phase 2 开始积累"以球近似兑现。

---

## 后续走向

```
Phase 7 剩余（批次 5 = 7b GPU culling / 批次 2 refine / 灯 entity 化 / 批次 6 销账）
    ↓
Phase 8：纹理接缝（normal/ORM 贴图采样、BC5 Z 重建、法线绿通道、线性 mip 质量）
    ↓
远期候选：完整 IBL（prefiltered + BRDF LUT）、shadow、MASK alphaTest、OIT、imgui（低优先）
```

## 注意事项（现行）

1. **Reflection 输出是 `.cppm` 而非 `.hpp`**（全项目 C++20 modules）。
2. **Schema 与 Lua 配置是两套机制**：schema 校验 shader 契约（build time），Lua 描述场景意图
   （runtime），不要混淆。
