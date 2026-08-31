# Phase 5 实施计划与记录：透明排序 + 简单光照

> **状态**：现行（2026-08-31 完成；实施记录与决策权威，行号以当时代码为准）

## 范围决策（开工前用户确认）

1. **bindless 原则**（用户拍板）："坚持 bindless，放不下的都进 descriptor heap，push constant
   尽量少用，mvp 了不起"——光照数据不走原路线图的 push constant 方案，DrawArgs 仅
   +`objectIndex`(4B)；光参数 + 对象矩阵表进 scene data buffer（binding 3）。
2. **光照深度**：简化 PBR + 基础 Phong **两条路线并存**（scene.lua `styles.pbr` 一行切换）。
3. **法线空间**：世界空间光照（原"CPU 转 view space"因 shader 只有融合 mvp、frag 无法
   重建 view 空间位置而改道；对象矩阵表 bindless 索引，Phase 7 shadow/culling 复用）。
4. **既有 DrawArgs 字段迁移**：只加不动（baseColor/roughnessMetallic/vertexOffset 搬 heap
   留后续单独批次，降低 cottage 392-primitive 回归风险）。
5. **状态通道**：双段（opaque/transparent）+ 段首两次 applyDynamicState，不做 per-draw 状态。
6. **执行模式**：简单工作子 agent、主 agent 验收（实际全程主 agent 直接实施 + 像素级验收）。

## 批次与验收

### 批次 0 — Phase 4 文档销账
- AGENTS.md「ECS 已知债」头行划线（5 子项全已解）；`ecs_compliance_review.md`
  "仍 Open（归 Phase 4）" 陈旧节加覆盖划线注记。
- 顺手修 `Justfile` `run` recipe：Python 3.12+ 起 subprocess 无路径分隔符的相对名走 PATH
  搜索（裸 `vulkan_practice` → FileNotFoundError），改 `os.path.join('.', exe)`。

### 批次 1 — 透明排序
- `model.frag.slang`/`model_pbr.frag.slang`：alpha = `baseColor.a × texel.a`（白纹理 a=1 无副作用）。
- `shader.cppm` `applyDynamicState`：`colorBlend=true` 时 `setColorBlendEquationEXT`
  （SRC_ALPHA/ONE_MINUS_SRC_ALPHA/ADD；alpha 通道 ONE/ZERO）。**主工程此前零 blend equation
  调用——enable 不开 factors 不混合**（v341 头 `VkColorBlendEquationEXT` 单 colorBlendOp
  字段，与新版头 src/dst 双字段不同）。
- `vkrenderer.cppm`：`FrameDrawList.transparentStart` 分段；`transparentState`
  （depthWrite=false + colorBlend=true + **cullMode=eBack**）；drawFrame 两段各
  applyDynamicState 一次；llm_debug 离屏同步。
- `ecs.cppm` `renderListSystem`：`MaterialAsset.alphaModeBlend` 分桶（此前零消费），
  透明段距离² 降序 `stable_sort`。
- **验收（像素级）**：重叠区实测 (65,98,53) = B2F 正确序（错序预测 (53,89,60)）；
  绿/黄单层 (19,64,70)/(70,69,19) 精确命中；透明叠不透明 (204,52,8) 命中。
- **过程中发现**：初测有效 alpha = 0.4375 = 1-(1-0.25)²——draw list DIAG 证明非双画，
  真因 = 封闭立方体正反面在关深度写后各混合一次；透明段开背面剔除修复。
  （opaque 段保持 eNone——硬编码 cube 绕序未验证。）

### 批次 2 — 动态堆数据基建 + 光照契约
- `vkresources.cppm`：`LightDataGpu`(48B) + `kSceneDataHeaderBytes(64)`/`kSceneMatrixStride(64)`/
  `kSceneMatrixCapacity(1024)` 布局契约 + `ensureSceneData()`（device-local BDA buffer +
  allocSlot 运行时槽 + 描述符一次写入；**slot 不做 schema 常量**——纹理槽数随场景动态，
  light 槽无编译期偏移，同 slotTexture 模式）。
- `vkrenderer.cppm`：`SceneFrameData{light, objectMatrices span}` 进 FrameDrawList；
  staging ring ×2（persistent mapped，drawFrame ① fence 后写本 slot——全项目首个每帧动态
  GPU 数据）；录制期 `vkCmdCopyBuffer` + Transfer→AllGraphics shader-read barrier（同 cmd
  的 storage buffer 写读也须显式依赖，VVL BUFFER_SYNC）。
- `shader.cppm`：RenderPipeline +`slotSceneData/hasSceneData`（mappings[4]，binding 3；
  mesh/unlit 阶段不声明 = 映射空转，同 mesh 对 binding 1/2 既证行为）。
- 契约：material.slang/schema +objectIndex → 反射自动重生成 100B；`roughnessMetallic`
  接线 `{mat.roughness, mat.metallic}`（占位 0 退役）；`Light` context（scene.cppm）+
  scene.lua light 段（scene_config readVec3）；main 启动期 `maxPushConstantsSize` +
  矩阵容量契约检查。
- **验收**：VVL 抓出 staging 缺 `eShaderDeviceAddress` usage（createBufferWithAddress 无条件
  查地址）→ 修复后零 error；cottage 回归 **diff=0 位级一致**。

### 批次 3 — 双光照风格
- `shaders/scene.slang`（新）：`ByteAddressBuffer sceneData` binding 3 + `readSceneLight`/
  `readObjectMatrix`（列主序 4 列显式读，列组合变换——无行/列约定歧义）/`worldPos`/
  `worldNormal`（mat3 直乘，均匀缩放假定）；布局字面量与 vkresources 契约一致
  （kTrianglesPerGroup 先例，无反射端）。
- `model.slang`/`model.mesh.slang`：+`positionOS : TEXCOORD1` 插值器（object-space 位置
  透传，frag 做 object→world；unlit 不消费零影响）。
- `model_pbr.frag.slang` 转正：简化 Cook-Torrance（GGX D + Smith-G + Schlick-F，
  F0=lerp(0.04,albedo,metallic)，diffuse=albedo(1-m)/π）。
- `model_phong.frag.slang`（新）：ambient + Lambert + Blinn 高光，`shininess=lerp(128,4,roughness)`，
  metallic 染高光色。
- 接线：CMake compile_slang 行（**坑：SPV 还必须进 `add_executable` 源列表才会被构建**，
  首次漏加 = 改 CMakeLists 后 ninja "no work to do" 但 SPV 不存在）+ scene_config 默认
  pipelines + shader_registry findSpec 回落 + scene.lua pipelines 条目。
- **验收**：cottage（392/392 pbr 风格——此前占位 frag 掩盖）光照几何正确（受光面/背光面/
  阴影侧）；pbr↔phong 切换 diff=40813px 实锤两条路线独立生效；transparent.glb BLEND×PBR
  组合（每面方向性明暗 + 单层透明）。
- 已知：Slang 无 GLSL `mix`（用 `lerp`）；改 CMakeLists 后 `just build` 不一定触发
  reconfigure（ninja 只跟踪它认识的依赖），新增编译目标需手动 `cmake -B build -S .`。

### 批次 4 — 文档销账
- AGENTS.md：模块表（vk_renderer/shader_registry/vk_resources/ecs/scene/scene_config/
  material_types_generated/shader 行）+ 着色器编译节 bindless 原则 + 关键文件位置
  （scene.slang/model_phong 新行、model_pbr 转正）+ main.cpp 段 + 已知问题（Phase 5
  四条近似：MASK 无 alphaTest / 均匀缩放假定 / 中心距离排序 / 透明段背面剔除）。
- next_roadmap.md：Phase 5 完成情况 + 偏离注记；当前节点 → Phase 6。

## 验证矩阵（全 PASS）

| 项 | 判据 | 结果 |
|---|---|---|
| 透明混合 | 单层 α=0.25 像素预测 | (19,64,70)/(70,69,19) 命中 |
| B2F 排序 | 重叠区正确序 vs 错序预测色 | (65,98,53) 命中正确序 |
| opaque→transparent 序 | 透明叠不透明预测色 | (204,52,8) 命中 |
| 背面剔除 | 有效 alpha 0.25（非 0.4375） | 修复后命中 |
| 批次 2 回归 | cottage diff=0 | 0/921600 |
| 光照几何 | 受光/背光面方向性 | 目检 PASS |
| 双风格切换 | pbr↔phong diff>0 | 40813px |
| BLEND×PBR | 透明盒带方向性明暗 | 目检 PASS |
| VVL | 零 validation error | PASS |
