# RGL 路线图

> **状态**：现行（开发路线图；当前节点 = Phase 7 进行中：D1 延迟单灯 `70ce9d1` + D3 多灯/反射 +
> 批次 4 CPU culling + **批次 5=7b GPU culling/DGC 完成（2026-09-09，forward+deferred BIT-IDENTICAL；
> DGC 路径待 NVIDIA 机实测）**，批次 6 文档销账待续；并行工作流 = **Phase 10 批次 0 Renderer 拆分
> 完成（2026-09-10，drawFrame 577→291，5 模块摘出，BIT-IDENTICAL）** + 架构债务 1+2 修复
> （scene→scene_context / cube 出内核，见 [架构债务-依赖方向审计.md](架构债务-依赖方向审计.md)，`b1788e0`））

Phase 0–6 已全部完成（各阶段结论一段话归档于下，实施细节与计划-实现偏离的权威记录在
`docs/archive/phaseX_plan.md`）。（legacy 旧 OpenGL 项目已于 2026-09-08 删除，见 git history。）

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
| **1** G-buffer MRT + offscreen 显式格式 | 格式词表、4-MRT `deferred_geometry`、`dump_gbuf` 直读验 albedo | **✅ 核心完成** | normal/metalRough **纹理**采样（→ Phase 8） |
| **2** Light pass + sampleRead 接线 | SAMPLED_IMAGE 描述符入 heap、全屏 light pass GGX、forward/deferred 切换（选路已转 render_graph.lua variant，2026-09-08） | **✅ 核心完成** | ①light pass 简化 IBL ③skybox 背景合成 |
| **3** 多灯 + 灯数据并入反射（D3） | 第 4 条反射管线 `GpuLight`、scene data 尾区灯数组 @262272（头不动保 forward diff0）、`lights` 配置、逐灯循环 + 点光衰减 | **✅ 核心完成** | 灯 entity 化（ECS 规范 3）；forward `LightDataGpu@0` 头仍手工双源；shadow map |
| **4** CPU frustum culling | 逐图元 node-local `CullSphere`、Gribb-Harman 6 面（z∈[0,1]）、可见统计 | **✅ 核心完成** | 真 AABB（现球近似，够 frustum） |
| **5** GPU culling / DGC | `vk_indirect_commands`、compute cull、IES/静态/读回重放三路、CPU 剔除降兜底+对账 | **✅ 核心完成**（2026-09-09；2026-09-10 三驱动 + 静态 spike 全运行时验证） | 静态 DGC 默认关（满槽实现慢 1.85×，翻盘 = 紧凑分桶 + 去同步等待，`docs/radv_static_dgc_spike.md`） |
| **6** 文档销账 | — | **🔶 部分** | 完整销账待批次 5 收尾（devlog/research/roadmap/AGENTS 已随 5d 更新） |

**净结论**：延迟渲染单灯核心 + 多灯 + **GPU culling（7b）** 跑通——deferred 逐灯循环（定向+点光、
per-light 颜色、距离衰减）headless 零 VVL error；forward 只读 `lights[0]` diff0 不变；GPU culling
= 默认路径（culling compute + DGC execute / 读回重放双路，能力门控），CPU 剔除降为兜底+每帧对账
基准（默认视角 GPU=CPU=1571、背向 0/0 match）。验证方法学注：**BIT-IDENTICAL 比对须同会话双构建
（stash 基线 vs 改动），跨日全量重建有 ±1 LSB 噪声**（调试历史 2026-09-08）。DGC 路径本机
（RADV graphics 侧不可用）未运行时实测，代码经 VVL 审查+编译绿。
**2026-09-10 更新**：三驱动运行时全验证（NV DGC execute 石锤 + 两 AMD/Intel 画像补测），
静态 shader DGC spike 功能通（本机 Strix Halo，VVL 零 error + 1px tie 级回归）但满槽实现
慢重放 1.85× → 默认关（`dgc_static` 门控）——详见 调试历史 2026-09-10 条目群 +
`docs/radv_static_dgc_spike.md`。

### 剩余计划

- ~~**批次 5 = Phase 7b GPU culling**~~ **已完成（2026-09-09）**：culling compute（`gpu_cull` +
  `cull.comp.slang`）+ `vk_indirect_commands`（DGC RAII，8 入口点 PFN via `vkGetDeviceProcAddr`）+
  `vkrenderer` 双路（DGC execute / **读回重放**——IndirectCount 无 per-draw push data 故弃用，
  见 research §4.4 勘误）；能力门控（`DgcSupport.graphicsIndirect`）。DGC 路径待 NVIDIA 机实测。
  设计细节与双驱动实测 → [device_generated_commands_research.md](device_generated_commands_research.md)。
- **静态 shader DGC（RADV 路，2026-09-10 spike 完结）**：功能验证通过（VVL 零 error +
  1px tie 级回归），但满槽孔洞实现 preprocess ×N 慢重放 1.85× → **默认不开**
  （`diagnostics.dgc_static` 门控保留）。翻盘前提 = 紧凑分桶（∝visible）+ 去 cull 同步等待。
  设计底稿 → [radv_static_dgc_spike.md](radv_static_dgc_spike.md)。
- **批次 2 refine 残二**：deferred light pass 简化 IBL、skybox 背景合成进 deferred 图
  （选路已转正：render_graph.lua `variant`，2026-09-08 重构批次 3）。
- **灯 entity 化**（ECS 规范 3）+ forward `LightDataGpu@0` 头并入反射（Phase 7 多灯已把灯数组
  入反射管线 4，前向头为 diff0 暂留手工双源，见 AGENTS 已知问题）。
- **批次 6 文档销账**（待批次 5）。

### 设计决策（现行）

- 延迟渲染为可选路径，不破坏 forward 管线（render_graph.lua `variant` 选路，forward diff0 是硬约束）。
- GPU culling = **默认路径**（扩展 required，无开关），同一 culling 输出（CPU 算 mvp 进序列 +
  GPU 只写可见性）驱动两条提交路径：DGC execute（NVIDIA）/ 读回重放（RADV），能力位自动切换，
  不改 culling 逻辑；CPU 剔除降为兜底+每帧对账基准（不一致告警不中断）。mvp 必须 CPU 算
  （GPU float4x4 乘法 FMA 收缩序与 glm 无位级契约 → 保 forward diff0 硬约束）。
- CullSphere（node-local 剔除球）已随批次 4 落地——原"AABB 组件 Phase 2 开始积累"以球近似兑现。

---

## 后续走向

```
Phase 7 剩余（批次 2 refine / 灯 entity 化 / 批次 6 销账；批次 5=7b GPU culling 已完成 2026-09-09）
    ↓
Phase 8：Bistro 场景打磨
  ① HDR 累加链（先行——色彩判断基准）：light/forward 输出 → RGBA16F RT + ACES tone-map pass
     → SRGB8 swapchain（当前 intensity 4.0 已在 SRGB8 写入时裁切高光，无 tone map）；
     破现有 BIT-IDENTICAL 基线，重立新基线
  ② HDR env 路径：Radiance .hdr（stb 内置 loader，16F）→ RGBA16F 上传；
     Bistro `san_giuseppe_bridge_4k.hdr` 转正（现 skybox/rainbow 8-bit PNG 为占位）
  ③ 纹理接缝：normal/ORM 贴图采样、BC5 Z 重建、法线绿通道、线性 mip 质量
  **明确不升级**：G-buffer 四通道（albedo rgba8_srgb / normal rg16_snorm / worldPos rgba32f /
  metalRough rgba8）+ albedo/normal/metalRough 贴图维持 8-bit——物理量 ≤1.0，业界标准，
  升 16F 纯浪费带宽
    ↓
Phase 9：性能观测（profiler）——先回答"CPU-bound 还是 GPU-bound"，再谈优化
  ① CPU 帧相 profiler（内建）：每帧逐相 chrono（ECS/render_list/命令录制/④c cull
     waitForFences/present acquire）+ rllogger 直方图——关键 = 分清 CPU 干活 vs 等 GPU stall
  ② 函数级深挖：gprof（clang `-pg` 兼容，同一套 mcount 插桩；勿开 LTO）或 perf + flamegraph
  ③ GPU per-pass 时间戳：`vkCmdWriteTimestamp`（sync2 stage）插渲染图 pass 边界 →
     per-pass GPU ms（GPU 写入，与 CPU 快慢无关；图已有 pass 循环，零新架构）
  ④ VK_EXT_present_timing 测量模式（archive/present_timing_research.md §6 步骤 1）：IPD/
     丢帧/刷新属性/eImmediate 延迟链；RADV 运行时支持待实测（Arc A310 100% 占用问题
     在 Strix Halo 大概率同样存在，无限帧 + eImmediate = 满速渲被丢弃的帧）
  ⑤ 判定 + 决策输入：CPU 帧时 vs GPU 首末时间戳 vs IPD 三方对比；④c per-frame cull
     读回成本（若显著 → 读回延后一帧）；帧率上限/present mode 可配置化的必要性
    ↓
Phase 10：阴影（大特性，一口气；风险分层 = CSM 承诺交付 + HFTS 带退出条件的 spike）
  **批次 0 已完成（2026-09-10，Renderer 拆分，BIT-IDENTICAL）**：drawFrame 577→291 行，
    摘出 5 模块（state_kit / frame_capture / cull_driver / frame_session / writeSceneStaging
    私有方法，详见 docs/调试历史.md 2026-09-10）。阴影特性归属预置：新 pass 种类 →
    render_graph + ⑤ pass 循环；per-cascade 光锥剔除 → CullDriver（复用 ④c 编舞）；
    depth-only pass 状态 → StateKit 新增条目；不再往 drawFrame 塞步骤。
  ① 保守光栅化基建（VK_EXT_conservative_rasterization，RADV 26.2.1 已实测可用 rev 1）：
     深度写入保守化（重叠三角形不留缝）——CSM/HFTS 共同硬前提
  ② CSM（Cascaded Shadow Maps）先行【承诺交付】：depth-only 阴影深度 pass（mesh 管线
     新条目类型，无材质分叉）+ 4 级 cascade + PCSS 采样；**光锥剔除复用 ④c GPU culling
     模式**（现 GPU cull 只剔相机视锥，阴影 pass 需按光锥重跑一遍 culling dispatch）；
     PCSS penumbra 在 Phase 8 HDR float 累加里算
  ③ HFTS/IzB（Hybrid Frustum-Traced Shadows）【spike，带退出条件，非承诺项】：
     Irregular Z-Buffering（全视锥 Z-buffer + 光线步进，无 texel 走样）远场/场景遮挡 +
     近场 CSM/PCSS 混合。**验收**：Bistro 场景光线步进软阴影视觉正确 + 成本 ≤X ms
     （X 由 Phase 9 profiler 定基线）；**达不到即收手，CSM+PCSS 为最终交付**（The
     Division 自身近场也是 shadow map，HFTS 只负责远场）。优先走 FTIZB 路线
     （光栅管线 + 硬件保守光栅 + early-z，非重 compute）
     参考链（公开，勿逆向游戏——系统逻辑在 CPU 侧，shader 里没有）：
     - FTIZB "Frustum-Traced Raster Shadows: Revisiting Irregular Z-Buffers"（NVIDIA 2015，
       research.nvidia.com 有 camera-ready PDF）——最贴近本实现的版本
     - "Conservative Z-Prepass for FTIZB"（Tokuyoshi 2018）——性能加速
     - 《Irregular Z-buffer Shadows》assassin-plus.github.io（2025-03）——最新教程式 walkthrough
     - HFTS 原文（Wyman SIGGRAPH 2016，cwyman.org/abstracts/sig2016_hfts.pdf）+ Story 2015 原文
     - github.com/strexicious/izb-on（实验参考，非生产级）
    ↓
远期候选：完整 IBL（prefiltered + BRDF LUT）、MASK alphaTest、OIT、imgui（低优先）
```

## 注意事项（现行）

1. **Reflection 输出是 `.cppm` 而非 `.hpp`**（全项目 C++20 modules）。
2. **Schema 与 Lua 配置是两套机制**：schema 校验 shader 契约（build time），Lua 描述场景意图
   （runtime），不要混淆。
