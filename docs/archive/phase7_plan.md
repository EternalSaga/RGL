# Phase 7 实施计划：延迟渲染 + Culling（与 forward 并存）

> **状态**：计划（2026-09-07 起草；**批次 0–4 已落地** `70ce9d1`/`ed38446`/`4c30298`，进度权威 =
> [next_roadmap.md](../next_roadmap.md) Phase 7 表 + [phase7_devlog.md](../phase7_devlog.md)；
> 批次 5（=7b GPU culling）计划面仍有效）。本文件是开工前的分批拆解与验收设计。
> 完成后本文件转 `历史`，Phase 7 完成情况回写 next_roadmap。

## 0. 定位与总原则

延迟渲染作为**可选渲染路径**，与现有 forward 三段（opaque/skybox/transparent）**并存**，
经 `configs/render_graph.lua` 选路（缺省仍 forward，行为零漂移）。Phase 7 分两大块：

- **A. 延迟渲染管线**：G-buffer MRT pass → light pass → composite（复用 Phase 6 图 + blit）。
- **B. Culling**：CPU frustum/AABB 剔除 → GPU culling（`VK_EXT_device_generated_commands`）。

**已具备的地基**（Phase 6 兑现，Phase 7 直接复用）：
- 执行器 `drawFrame` 的 pass 循环 `pass.colors` **已是多色附件向量**（`vkrenderer_impl.cppm:455-495`）
  → MRT 的 `beginRendering` 天然可用，无需改渲染附件循环骨架。
- Phase 6 渲染图已有 `offscreen` attachment + `sampleRead` usage（编译面）+ `blit` pass
  → "渲进 offscreen RT → 采样 → 上屏" 的拓扑语言已就位。
- scene data buffer + staging ring（Phase 5）= 每帧动态 GPU 数据通路，多灯直接扩。

**要填的已知缺口**（AGENTS.md「已知问题」+ 代码核实，Phase 7 的硬前置）：
1. **offscreen 附件 format 恒 `inherit`**（`vkrenderer_impl.cppm:370-373` 拒绝非 inherit；
   `ensureRtAttachment` 收 format 但图侧不给具体值）→ G-buffer 要 R32G32B/RG16/RGBA8 等**显式格式**。
2. **sampleRead 描述符未接**（AGENTS Phase 6 已知 ④：编译面支持、执行器不入库）→ light pass
   采样 G-buffer RT 需要把 RT view 建成 SAMPLED_IMAGE 描述符入 heap。
3. **`LightDataGpu` 单灯 + 双源手工**（`vkresources.cppm:40-46`，48B 无反射）→ 多灯布局重设计 + 并反射管线。
4. **`gltf_loader` 只读单 translation**（`gltf_loader.cppm:147-161`，丢 rotation/scale/父链）
   → 多模型/Bistro 摆位散架，**批次 0 先补**（不靠烘焙，见 §批次 0）。
5. **MASK/doubleSided 未实现**（Phase 5 已知）→ Bistro 植被在延迟几何 pass 里是实心卡；
   留接缝（与 forward 同状态，不属 Phase 7 主体，Phase 8 处理）。

---

## 范围决策（开工前用户确认，未勾则按推荐默认）

- **D1 首个里程碑范围**：推荐 **A（G-buffer+light+composite，单灯）先跑通并目检**，
  再叠多灯（D3）与 culling（B）。理由：延迟管线正确性是大头且独立可验收；culling 需要
  一个正确的 G-buffer 场景做对照。GPU culling（DGC）单列，风险最高，可延到 Phase 7b。
- **D2 forward/deferred 并存形态**：推荐 **图级选路**（`render_graph.lua` 两套 desc：
  forward 三段 / deferred gbuffer+light+composite），FrameDrawList 增 deferred 段边界；
  forward 路径代码零改动（回归 = cottage/Bistro diff 0）。
- **D3 多灯**：推荐 **并入反射管线**（schema 声明 → 生成 `LightArray` 布局常量 + C++ struct），
  清偿 `LightDataGpu` 双源债；灯升格为 entity + `LightData` 组件（ECS 规范 3）。
- **D4 光源几何**：推荐 **全屏 light pass（mesh shader 大三角，仿 skybox）+ 逐灯循环**（不做
  clustered/Forward+- 的 tiled light list——那是 Phase 8+ 的事）。多灯 = light pass 内循环。
- **D5 GPU culling 是否入 Phase 7**：推荐 **拆出**：Phase 7 做到 CPU culling（正确性 + 统计），
  DGC 子系统（`vk_indirect_commands.cppm` + compute culling + NVIDIA/RADV 双路）单开
  **Phase 7b**，依 `device_generated_commands_research.md`。避免延迟渲染与 DGC 两个高风险同时上。
- **D6 测试主场景**：**Bistro Exterior**（已转 `assets/bistro_exterior.glb`，2.8M 三角未减面、
  1024 贴图）作 G-buffer 视觉主体 + per-primitive culling 用例（1591 primitive、街道大跨度）；
  性能档用程序化散布实例（复用 scene.lua entities 散布机制）。

---

## 批次与验收

### 批次 0 — Bistro 进引擎 + loader 节点 TRS（forward 可跑，为后续铺路）

把转换好的 Bistro 在 **forward** 下正确显示，同时补 loader 层级账（本 Phase 一切测试的前提）。

- **loader 节点变换**（`gltf_loader.cppm`）：新增读 `node.rotation`（四元数）+ `node.scale` +
  父链合成，per-primitive 存一个 **local→world 矩阵**（替换现在只有 `translation` 的字段），
  或直接输出已含节点 TRS 的世界矩阵给上层。**不做顶点烘焙**（用户原则：先补原始能力）。
  - 影响面：`GltfPrimitive`（translation → transform 矩阵）、`model_importer`
    `registerScene`（rootTransform × 节点矩阵）、`render_list` 的 mvp 合成（`spin·world`）。
  - 兼容：现有单 translation 模型（kyoto/grass）行为不变（rotation=identity,scale=1）。
- **单位/居中**：Bistro 厘米 + 离原点远 → scene.lua entity `scale≈0.01` + `position` 摆正；
  不改模型数据（数据侧的事数据侧解，代码不硬编）。
- **CMake 接线**：`MODEL_GLB` 列表加 `bistro_exterior.glb`（`vulkan_practice/CMakeLists.txt:228`）；
  2K/424MB 模型进 `models/` 拷贝评估（体积，必要时出 1024 变体 `bistro_pack.py ... 1024`）。
- **heap slot 预算复核**：Bistro 1591 primitive 分块（16MB/块）+ 数百纹理 slot，对
  `kUserSlotMargin(1024)` 与 `kSceneMatrixCapacity(1024)` 做启动期契约核查（可能需上调 margin）。
- **转换脚本固化**：`tools/bistro_pack.py` 已就位（assimp .gltf → 规范 PBR .glb，dds→png ffmpeg，
  Specular→metallicRoughness+occlusion 通道映射）。**法线绿通道翻转 + BC5 Z 重建留此批次或批次 2**
  （引擎开始采样法线时一并做）。
- **验收**：forward 下 headless 渲染 Bistro，无 `[error]`、VVL 零 error；llm_debug 截图 = 贴图正确、
  部件位置正确（不散架）；kyoto/grass 回归 diff 0（loader 改动不破旧模型）。

### 批次 1 — G-buffer MRT pass（offscreen 显式格式）

延迟几何 pass：一次绘制写多 MRT。

- **offscreen 显式格式**（补缺口 1）：
  - `RgAttachmentDesc.format` 解析字符串 → `vk::Format`（新增小查表：`rgba8Unorm`/
    `r32g32b32Sfloat`/`rg16Snorm`/`rgba8Snorm` 等），`RgCompiledAttachment` 带 `vk::Format`。
  - `Resources::ensureRtAttachment(name, extent, vk::Format)` 支持任意 color 格式
    （usage 需 `COLOR_ATTACHMENT | SAMPLED_IMAGE`——light pass 要采样）。
  - `drawFrame:370-373` 去掉「只 inherit」拒绝。
- **G-buffer 布局（单一真相源，新建 `shaders/gbuffer.slang` + 契约）**：
  - `RT0 albedo` RGBA8_SRGB（+ alpha）
  - `RT1 normal` RG16_SNORM（octahedral 编码）或 RGBA16
  - `RT2 position` R32G32B32_SFLOAT（世界坐标，直接重建，免反投影）或存 view-depth
  - `RT3 metalRough` RG8（B/M + rough），深度 = 现有 depth attachment（写）
  - 与 C++ 侧格式常量用 `static_assert`/生成常量钉死（延续 schema 常量先例）。
- **几何 shader**：新 `deferred_geometry.mesh`（复用 `model.mesh` 顶点读路径）+
  `deferred_geometry.frag`（写 4 MRT：albedo 采 baseColor、normal 采/输出、position 插值世界、
  metalRough 现采样 metallicRoughness——**本批次开始接 normal/metalRough 纹理**，补缺口见 §转换）。
- **render_graph**：deferred desc = gbuffer pass（4× colorWrite + depthWrite，drawSource="opaque"）
  → light pass → composite blit；`compileRenderGraph` 校验多 colorWrite 合法（应已支持，加自测样本）。
- **验收**：gbuffer 各 RT 单独 debug 可视化（把 RT0..RT3 分别 blit 上屏）目检：albedo=场景色、
  normal=方向渐变色、position=合理世界坐标渐变、metalRough 合理；VVL 零 error。

### 批次 2 — Light pass + sampleRead 采样 RT 接线

把 G-buffer 点亮，走通"渲进 RT → 采样 RT → 输出"的完整依赖链（补缺口 2）。

- **sampleRead 执行器接线**（补缺口 2）：offscreen attachment 在**被采样 pass** 之前，
  其 view 建 SAMPLED_IMAGE 描述符入 heap（`Resources` 加 `ensureRtSamplerDescriptor(name)` 幂等，
  随 RT generation 失效重建）；shader object 经 binding 引 heap slot（同纹理机制）。
- **light pass = 全屏 mesh shader 大三角**（仿 `skybox.mesh` 机制，D4）：
  - `light_pass.mesh`（单 group 全屏大三角，无深度写）+ `light_pass.frag`：
    全屏遍历 → 采 G-buffer（position/normal/albedo/metalRough）→ 读 scene data 灯参数 →
    GGX + 简化 IBL（复用 `model_pbr_ibl` 的 BRDF/环境采样）→ 写 **lit HDR RT**。
  - FrameDrawList 增 `lightPassStart` 边界（deferred 变体：opaque-geometry→gbuffer 段 + 单 light draw）。
- **composite**：lit RT → swapchain，**直接复用现有 blit pass**（`offscreenRenderGraphDesc` 的 composite 通路）。
- **forward/deferred 选路**：`render_graph.lua` 两套 desc；`configs` 开关；forward 路径代码零改。
- **验收**：deferred 单灯目检 ≈ forward pbr（光照方向性一致，非必须逐位）；normal 翻转后
  凹凸正确（此批次一并补 normal 绿通道翻 + BC5 Z 重建，若批次 0 未做）；VVL 零 error；
  切回 forward diff 0。

### 批次 3 — 多灯 + 灯数据并入反射管线（清偿 Phase 5 债，D3）

- **多灯布局 + 反射**：新建 `shaders/lights.slang` + `lights_schema.lua` → 第四条反射管线生成
  `LightArray`（`{color,direction,pos,range,flags}` × `kMaxLights`）布局常量 + C++ struct
  （`static_assert` 钉死），替换 `LightDataGpu` 手工双源（AGENTS 已知问题：`LightDataGpu` 并反射）。
  scene data 头布局重排（light 48B 区 → 灯数组区，`kSceneDataHeaderBytes` 相应扩；env/sky 偏移同步）。
- **灯 entity 化**：`Light` 从 registry context 升格 **entity + `LightData` 组件**（ECS 规范 3：
  画不画/照不照 = 实体有无）；`renderListSystem` 每帧收集灯 entity → scene data。
- **light pass 逐灯循环**（D4）：`for i in lights { GGX accumulate }` + 距离衰减 + 上限 `kMaxLights`。
- **验收**：3 灯场景（含彩色点光）方向性/衰减正确；无灯 = 只剩 env/ambient；反射管线产物随改动提交、
  `just reflect` 校验一致；旧单灯 config 兼容（合成 1 灯）。

### 批次 4 — CPU Frustum + AABB Culling（culling 正确性，测试主力 = Bistro）

- **AABB 组件**：`Bounds` 现为**包围球**；新增 `AabbBounds{min,max}`（`scene.cppm`），
  `model_importer`/`boundsSystem` 从顶点算 AABB（含节点变换后世界 AABB）。
- **Frustum 提取**：`renderListSystem` 从 view·proj 提 6 面，per-entity **AABB×frustum**（球作快速预剔）。
- **剔除落点**：`renderListSystem` 生成 FrameDrawList 时跳过被剔 entity（不进 draw list）；
  透明排序前剔（省排序量）。
- **可见性统计（DIAG）**：每帧日志 `visible/total` + 各面剔除数——**这是 culling 正确性的可测信号**
  （相机移动时数字变化合理；相机对着 Bistro 时 visible 高、转开时低）。
- **验收**：相机平移/旋转，`visible/total` 随视野内容变化正确（对着墙/转开对比）；被剔对象确实不上屏
  （llm_debug 截图对比）；全可见场景与不剔 diff 0（剔错会 diff≠0，作反向哨兵）；Bistro 1591 primitive
  逐 prims 剔粒度可用。

### 批次 5 — GPU Culling / device_generated_commands（依 D5：可推 Phase 7b）

> 若 D5 定为拆出，本批次整体顺延为 **Phase 7b**，Phase 7 到批次 4 即收口。

- **feature 回归**：`features_config.json` 加回 `deviceGeneratedCommands` +
  `vkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT` 查询 → `DeviceContext` 派生
  `supportsGraphicsIndirectCommands`（运行时选路）。
- **新模块 `vk_indirect_commands.cppm`**：generated-commands layout + execution set + preprocess
  buffer RAII + sync2 barrier。
- **culling compute**：frustum+AABB 输出 per-visible sequence（shader slot + `DrawArgs` + mesh task count）
  + count。
- **双路提交**（同一份 culling 输出）：NVIDIA = `vkCmdPreprocessGeneratedCommandsEXT`→
  `vkCmdExecuteGeneratedCommandsEXT`（execution-set + push data + DRAW_MESH_TASKS token）；
  RADV 回退 = `vkCmdDrawMeshTasksIndirectCountEXT`（`VK_EXT_mesh_shader` 双驱动可用）。
- **验收**：NVIDIA 路径 sequence 数 == CPU 期望 visible 数；RADV 路径 indirect count 与 CPU culling 一致；
  两路画面与批次 4 CPU 全画 diff≈0（能力门控不改结果只改提交）。

### 批次 6 — 文档销账

AGENTS.md（模块表 +`vk_indirect_commands`/`lights_schema`、已知问题更新缺口 1-4 销账、Phase 7 近似条目）、
next_roadmap Phase 7 完成情况 + 偏离注记、本文件转历史。

---

## 验证矩阵（设计，实施时填结果）

| 项 | 判据 | 手段 |
|---|---|---|
| Bistro forward 显示 | 贴图/位置正确、散架=0 | llm_debug 截图（批次 0） |
| loader 回归 | kyoto/grass 改动前后 | headless diff=0 |
| G-buffer 各 RT | 4 通道可视化目检 | 逐 RT blit 上屏（批次 1） |
| light pass 点亮 | 方向性与 forward pbr 一致 | 截图对比（批次 2） |
| 采样 RT 链 | 无未初始化采样、VVL 零 error | VVL + 截图（批次 2） |
| forward/deferred 切换 | forward 路径 diff=0 | headless diff（批次 2） |
| 多灯 | 3 灯方向/衰减 | 截图（批次 3） |
| 反射管线一致 | `just reflect` 产物无 diff | CI/构建校验（批次 3） |
| frustum 剔除 | visible/total 随视野变化合理 | 逐帧日志（批次 4） |
| 剔除正确性 | 全可见场景 diff=0（反向哨兵） | headless diff（批次 4） |
| DGC 双路 | sequence/count 与 CPU 一致 | 日志 + 截图（批次 5/7b） |
| VVL | 全批次零 validation error | headless 冒烟 |

---

## 风险与依赖

- **R1 MRT 上限 / 格式支持**：RADV/NVIDIA `maxColorAttachments` 与 G-buffer 格式（R32G32B32_SFloat
  blendable? 不需 blend）——批次 1 启动期查 `maxColorAttachmentBytes` + 格式支持，必要时减通道/换格式。
- **R2 采样 RT 与 descriptor heap 时序**：G-buffer 写 → light pass 采样是同帧跨 pass 依赖，
  布局转换由图推导；sampleRead 描述符入库的时机（RT 存在但内容帧序）要小心（照 Phase 3 纹理描述符机制）。
- **R3 Bistro 体积**：2.8M 三角 + 1591 primitive + 数百 slot，启动/上传/内存偏重；先 1024 贴图变体跑通，
  性能不足再减面（几何减面属性能优化，用户原则允许此时做）。
- **R4 DGC 驱动不对称**：RADV graphics 侧 indirect execution set 未开 → 双路是硬要求，
  不是可选项；RADV 上以 IndirectCount 路验收。
- **R5 灯数据布局变更波及 forward**：批次 3 重排 scene data 头，forward 的 pbr/ibl/skybox 采样偏移同步改，
  回归面大 → 用 diff=0 哨兵 + 反射 static_assert 兜。

## 与其他文档

- 依据：`next_roadmap.md` Phase 7、`AGENTS.md` 已知问题 1-5、`device_generated_commands_research.md`。
- 前置产物：`assets/bistro_exterior.glb`（本 Phase 转换）、`tools/bistro_pack.py`（转换管线）。
- 转换管线待办：normal 绿通道翻 + BC5 Z 重建（批次 0 或 2）；Interior/Wine 同法可转（按需）。
