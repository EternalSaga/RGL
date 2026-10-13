# RGL 路线图

> **状态**：历史（Phase 0–7 开发路线图存档；**Phase 7 完成（2026-09-10）**：D1 延迟单灯 `70ce9d1` + D3 多灯/反射 +
> 批次 4 CPU culling + 批次 5=7b GPU culling/DGC + 批次 2 refine（IBL+skybox）+ 灯 entity 化 +
> **前向头 SceneLight 入反射（Slang C API 工具升级，双源消除）+ 批次 6 销账**；
> 并行工作流 = **批次 0 Renderer 拆分完成（2026-09-10，drawFrame 577→291，5 模块摘出，
> BIT-IDENTICAL；旧称 "Phase 10 批次 0"，属阴影 Phase 前置，现随阴影归 Phase 11）** + 架构债务 1+2 修复
> （scene→scene_context / cube 出内核，
> 见 [架构债务-依赖方向审计.md](架构债务-依赖方向审计.md)，`b1788e0`）；下一步路由 → 各 phase 计划文件，当前 `phase13_plan.md`）
>
> **阶段号重排（2026-09-27）**：**Phase 10 = 调试交互**（配置热重载 + IMGUI 参数面板 + shader 热重载
> + 点选取参，计划 = [phase10_plan.md](archive/phase10_plan.md)）插队，**阴影整体顺延为 Phase 11**。
> 历史代码注释里的旧口径 "Phase 10 批次 0"（= 2026-09-10 Renderer 拆分）**一律不改**；
> 本文件与 AGENTS/技术概念等活文档中的"阴影 Phase 10"引用已全部改为 Phase 11。

Phase 0–6 已全部完成（各阶段结论一段话归档于下，实施细节与计划-实现偏离的权威记录在
`docs/archive/phase*_plan.md`）。（legacy 旧 OpenGL 项目已于 2026-09-08 删除，见 git history。）

---

## 核心原则（当时定案；原则 5 后有更新，见条内注）

1. **反射是 build tool，不是 runtime component**。反射结果只输出为 `.cppm`，渲染代码经 `import`
   消费，永不依赖反射系统实现。
2. **编译期安全网**。生成 `.cppm` 含 `static_assert`，C++ struct 与 shader 布局不一致直接编译报错。
3. **glTF only**。放弃 Assimp，只用 tinygltf。
4. **逐步推进**。每阶段稳定后再进入下一阶段。
5. **bindless 原则**（Phase 5 定案，推翻"push constant 传递轻量"）：push constant 冻结在
   "mvp + 索引"级别，场景级/每帧变化数据一律进 descriptor heap（scene data）。
   **（Phase 7b+ 已更新**：DrawArgs 缩到纯索引 `objectIndex` 4B，mvp 迁入 scene data per-object 表；
   现行口径见 `docs/cpu_perf_plan.md` §4.1 与 skill `rgl-shader-contract`。）

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
| **2** Light pass + sampleRead 接线 | SAMPLED_IMAGE 描述符入 heap、全屏 light pass GGX、forward/deferred 切换（选路已转 render_graph.lua variant，2026-09-08）、**refine 残二完成**（2026-09-10：light pass 简化 IBL + skybox 背景合成，见下 refine 注） | **✅ 完成** | （无——refine 残二已销） |
| **3** 多灯 + 灯数据并入反射（D3） | 第 4 条反射管线 `GpuLight`、scene data 尾区灯数组 @262272（头不动保 forward diff0）、`lights` 配置、逐灯循环 + 点光衰减、**灯 entity 化**（2026-09-10，`Light` 组件 + `index` 正序契约，ECS 规范 3）、**前向头 `SceneLight` 入反射**（2026-09-10，第 6 条 `scene_forward` 管线，双源消除） | **✅ 完成** | shadow map（→ Phase 11） |
| **4** CPU frustum culling | 逐图元 node-local `CullSphere`、Gribb-Harman 6 面（z∈[0,1]）、可见统计 | **✅ 核心完成** | 真 AABB（现球近似，够 frustum） |
| **5** GPU culling / DGC | `vk_indirect_commands`、compute cull、IES/静态/读回重放三路、CPU 剔除降兜底+对账 | **✅ 核心完成**（2026-09-09；2026-09-10 三驱动 + 静态 spike 全运行时验证） | 静态 DGC 默认关（满槽实现慢 1.85×，翻盘 = 紧凑分桶 + 去同步等待，`docs/archive/radv_static_dgc_spike.md`） |
| **6** 文档销账 | 批次 2 refine + 灯 entity 化 + 批次 5 收尾 + 前向头反射 + C API 工具升级的文档同步（devlog/research/roadmap/AGENTS/调试历史） | **✅ 完成**（2026-09-10） | （无——Phase 7 全项销账） |

**净结论**：延迟渲染单灯核心 + 多灯 + **GPU culling（7b）** 跑通——deferred 逐灯循环（定向+点光、
per-light 颜色、距离衰减）headless 零 VVL error；forward 只读 `lights[0]` diff0 不变；GPU culling
= 默认路径（culling compute + DGC execute / 读回重放双路，能力门控），CPU 剔除降为兜底+每帧对账
基准（默认视角 GPU=CPU=1571、背向 0/0 match）。验证方法学注：**BIT-IDENTICAL 比对须同会话双构建
（stash 基线 vs 改动），跨日全量重建有 ±1 LSB 噪声**（调试历史 2026-09-08）。DGC 路径本机
（RADV graphics 侧不可用）未运行时实测，代码经 VVL 审查+编译绿。
**2026-09-10 更新**：三驱动运行时全验证（NV DGC execute 石锤 + 两 AMD/Intel 画像补测），
静态 shader DGC spike 功能通（本机 Strix Halo，VVL 零 error + 1px tie 级回归）但满槽实现
慢重放 1.85× → 默认关（`dgc_static` 门控）——详见 调试历史 2026-09-10 条目群 +
 `docs/archive/radv_static_dgc_spike.md`。**批次 2 refine 完结**：deferred light pass 简化 IBL
 （env 采样替换常 ambient）+ skybox 背景合成（天空像素采 env）——deferred 与 forward IBL
 路线视觉对齐，forward BIT-IDENTICAL 不变（md5 一致），deferred 新基线稳定 + VVL 零 error。
 **前向头双源消除 + 反射工具升级（2026-09-10）**：`SceneLight` 经 Slang C API 实查布局入
 反射（第 6 条 `scene_forward` 管线），替换手写 `LightDataGpu`；连带 `reflection_tool` 加
 `slang` 子命令（链接 `libslang.so`，4 条 slang 管线去 JSON 中间层、升逐字段 offsetof 断言）——
 Phase 7 全项销账，forward BIT-IDENTICAL 不变。

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
- ~~**批次 2 refine 残二**~~ **已完成（2026-09-10）**：deferred light pass 简化 IBL（env
  cubemap 采样替换常 ambient，与 model_pbr_ibl.frag 逐式相同，mipRange=0 回落旧行为）+ skybox
  背景合成（天空像素 = G-buffer position 清 0，重建世界射线采 env，与 forward skybox.frag 同式）。
  env 走 light pass binding 9/10（4/5 被 G-buffer 占用）。验证：forward BIT-IDENTICAL（md5 不变）、
  deferred 新输出稳定（两次 md5 一致）+ VVL 零 error。
 - ~~**灯 entity 化**（ECS 规范 3）~~ **已完成（2026-09-10）**：`Light` 组件 + `index` 正序契约
   （画不画 = 实体有无，forward 读 index 0 = 主光，延迟按 index 落槽），BIT-IDENTICAL（forward+
   deferred md5 一致）+ VVL 零 error + 多灯边界验证。
 - ~~**前向头 `SceneLight`/`LightDataGpu` 双源**~~ **已完成（2026-09-10）**：新增第 6 条
   `scene_forward` 反射管线（`scene.slang` 的 `SceneLight` 经 **Slang C API** 实查布局 →
   `scene_forward_types_generated`，逐字段 offsetof 断言），替换 `vkresources.cppm` 手写
   `LightDataGpu`（4 处）。原默认成员初始化为死代码（renderListSystem 每帧显式聚合初始化），
   替换行为不变，forward BIT-IDENTICAL（md5 不变）。**工具侧连带升级**：`reflection_tool` 加
   `slang` 子命令（链接 `libslang.so` 直接查 C API，4 条 slang 管线去 JSON 中间层）——见
   [slang_api_reflection_research.md](slang_api_reflection_research.md)。
 - ~~**批次 6 文档销账**~~ **已完成（2026-09-10）**。

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
 Phase 7 完成（2026-09-10）：批次 2 refine（IBL+skybox）✓ / 灯 entity 化 ✓ / 批次 5=7b GPU culling ✓
   / 前向头 SceneLight 入反射（双源消除，C API 工具升级）✓ / 批次 6 销账 ✓。
   遗留（非阻塞）：静态 DGC 翻盘（紧凑分桶）、normal/metalRough 纹理采样（Phase 8）、
   shadow map（Phase 11）。
      ↓
 Phase 8 完成（2026-09-14）：① HDR tonemap ✓ / ② codec + HDR env（治死黑+天空盒主根因）✓ /
   ③ 纹理接缝 + forward 多灯 + emissive ✓ / ④ 完整版 IBL（SH9 + prefiltered）✓ /
   剩余批次 A 掠射角法线（A+C）✓ B MASK/双面/deferred emissive ✓ C 多灯 ✓ D 掠射角法线质量 ✓
   E 植被透射 SSS + 光源本色（forward）✓    F deferred 材质类别合并（per-pipeline class 常量 +
   lua 连连看路由，零 heap 契约）✓——实施细节与验证记录全在 phase8_plan.md §1–11
   结构债 8 条随批次拆完（"进场顺手拆"）；残留 style_check 1 条 = expandPrimitive 认知复杂度
   20>15（loader 批次字段累积，下次动加载路径时拆）
   **延后项（2026-09-14 用户拍板延后）——地面"水质感"**：现状地面"亮"但反射里只有环境（天空）、
   没有场景（建筑/树/灯）= 油亮 ≠ 湿。**Phase 15 接手（`docs/phase15_plan.md`）**，补齐清单：
   ① 反射源 = SSR（deferred G-buffer 已有 pos+normal；当年卡「deferred-only 破 D1 对等」，
   该约束 2026-10-10 已豁免，planar reflection 已否决）② 水洼状粗糙度贴图（洼 ≈0.05/湿石 0.3–0.5）
   ③ 湿区 albedo×0.5–0.7 压暗 ④ 水膜法线扰动（镜面扭曲 = "水"与"玻璃"的区分要素）
   ⑤ 太阳 glint（GGX 机制已在，被粗糙度分布解锁）⑥ AO/接触阴影（石缝积水暗部，批 11.4 已落地）；
   架构上长在 deferred light pass（逐像素 specular 现成）
     ↓
Phase 9：性能观测（profiler）——先回答"CPU-bound 还是 GPU-bound"，再谈优化
  **进度（2026-09-27）**：批 9.1（① CPU zone profiler，零宏 `RGL::zone()`）/ 9.2（③ GPU
  时间戳 + 校准同轴）/ 9.3（④ `VK_EXT_present_timing` 测量通路 + 9.3a `present_mode`
  双模式开关）/ 9.4（⑤ 判定收口）全部落地，默认关全部 BIT-IDENTICAL；②（gprof/perf）
  未做（判定不依赖）。实施与实测记录 = `docs/phase9_plan.md` §3.4/§4.4/§10；
  跨驱动状态 = `docs/nvidia_compatibility_report.md` §九。
  **⑤ 判定结论（本机 forward 1280×720，RADV Strix Halo + KWin，165 Hz）**：
  ① **CPU-bound** —— profiler 判定行 `frame=7.10ms cpu.record=7.03ms gpu.frame=2.98ms
  -> CPU-bound`，GPU 只占 42% 帧预算；② ④c **否决**"cull 读回延后一帧"（GPU cull 14µs、
  fenceWait 5µs，无 stall；且延后不减 CPU 总量）；③ ⑤ **当前不需要帧率上限** —— CPU 把
  帧率压到 ≈141 fps 低于 165 Hz，eImmediate 与 FIFO 分布重合且 `drop` 双零，
  不存在"满速渲被丢弃"（archive §八 的 A310 现象不可外推到本机）；`present_mode` 开关保留
  作复测入口；④ **头寸在未归因的 CPU 录制段**（`drawFrame` 7.03ms 中 ~93% 无子 zone）
  → 列为可选批 9.5（纯插桩）；⑤ **给阴影（现 Phase 11）的基线：GPU 侧余量 ≈ 3.1 ms/帧**
  （2.98ms 已用 / 6.061ms 预算；CPU 侧无余量），阴影 GPU 成本 ≲3ms 则整机帧时不变。
  ① CPU 帧相 profiler（内建）：每帧逐相 chrono（ECS/render_list/命令录制/④c cull
     waitForFences/present acquire）+ rllogger 直方图——关键 = 分清 CPU 干活 vs 等 GPU stall
  ② 函数级深挖：gprof（clang `-pg` 兼容，同一套 mcount 插桩；勿开 LTO）或 perf + flamegraph
  ③ GPU per-pass 时间戳：`vkCmdWriteTimestamp`（sync2 stage）插渲染图 pass 边界 →
     per-pass GPU ms（GPU 写入，与 CPU 快慢无关；图已有 pass 循环，零新架构）
  ④ VK_EXT_present_timing 测量模式（archive/present_timing_research.md §6 步骤 1）：IPD/
     丢帧/刷新属性/eImmediate 延迟链 —— **✅ 批 9.3 落地，RADV 运行时支持实测确认**
     （激活 / queue-full 回退 / 校准全跑通、VVL 零报警）；NVIDIA / Intel anv 待跨机复测，
     判读口径 = `docs/nvidia_compatibility_report.md` §九
  ⑤ 判定 + 决策输入：CPU 帧时 vs GPU 帧跨度 vs IPD 三方对比 —— **✅ 批 9.4 成文
     （`docs/phase9_plan.md` §10）**：本机 CPU-bound；④c"读回延后一帧"**否决**；
     帧率上限**当前不需要**；下一步头寸 = `drawFrame` 内部 CPU 细分（可选批 9.5）
    ↓
 Phase 10：调试交互（配置热重载 + IMGUI 参数面板 + shader 热重载 L1 + GPU id 点选取参）
   **计划已立（2026-09-27，[phase10_plan.md](archive/phase10_plan.md)），插队阴影（原 Phase 10 → 11）**。
   总纲 = **单一事实来源**（控件字段结构引用反射生成物，注册表不镜像布局；ObjectDraw 材质表
   单源化为前置批）。批次：10.1 imgui 骨架 ✅ / 10.2a ObjectDraw 单源化 ✅ /
   10.2b 灯曝光控件 ✅ / 10.2c 材质控件 ✅ / 10.3 配置热重载 ✅（均 2026-09-28）/
   10.4 shader 热重载 L1 ✅（2026-09-28，SPV 二进制签名守卫 + 两阶段同槽重建；顺带暴露并
   修复 rgl_shader DEP 表 11 处 import 漏登）/ 10.5 点选 ✅（2026-09-28，id pass 克隆
   管线重放 + r32ui 回读 + SelectedObject ctx + 面板"选中"页；顺带 picking_probe 配置
   探针 + D12 RPC 延后决策）/ 10.6 反射语义扩产 ✅（2026-09-28，param_semantics.json
    → 第 7 条反射管线生成语义表 + 面板构建拆出 param_pages 模块）/ **10.7 点选高亮 +
    选中跳页 ✅（2026-09-28 补做）**——起因是用户报"点选没生效"，排查结论 = 链路一直
    通（headless 探针 + 窗口合成左键两轮实测命中），真因是 `picking` 默认关 + 选中后
    **零视觉反馈**（唯一反馈是面板"选中"页一行字，而 Tab 默认停在"渲染"页）。落
    `highlight.frag` 克隆管线 + 高亮 pass（写既有色附件、插 tonemap **前**——插图末
    会落在色附件被采样之后 = 画了白画）+ **按材质** CPU 过滤 draw（同材质全部实体
    一起发光，刻意不进 scene data 契约以免触发 reflect 重生成）+ 面板自动切"选中"页
    （`ImGuiTabItemFlags_SetSelected`，允许打断）+ Esc 取消。`picking=false`
    BIT-IDENTICAL。**Phase 10 全批收口**，剩 UI 视觉验收（用户）。
    ↓
  **2026-09-28 增补批（Phase 10b，[phase10b_plan.md](phase10b_plan.md)，D12 翻案）**：
    11b.1 imgui 中文字体 ✅（问号根因三层：从不加载字体 + stb 不吃 CFF + 1.92 按需烘焙
    → `imgui[freetype]` 常开 + 系统字体探测链 MiSans Light 首选，**不进仓库**；
    验收走窗口 + 外部截图 —— llm 截图在 `frameEndHook` 之前 → 天生不含 UI）
    / 11b.2 loopback HTTP POST RPC ✅（`diagnostics.rpc` 默认关，`GET /` `/state` +
    `POST /camera` `/param` `/shot`，127.0.0.1:8788，帧边界命令队列；传输 `debug_rpc`
    + 协议执行 `debug_rpc_exec` 两模块，main 只一行装配；headless 可用 = agent 远程
    调参/截图姿势）。验收期抓到并修掉两个真 bug（error handler 覆盖自产 4xx body /
    responder move 后 catch 空句柄 → 恒 504，见 调试历史 2026-09-28）。
    **验收全绿**：BIT-IDENTICAL 回归 + `checks --strict` 0 警告 + RPC 全端点与负例 +
    中文字形目检；Windows 构建留到有环境时（非门）。
     ↓
Phase 11：阴影（大特性，一口气；风险分层 = CSM 承诺交付 + HFTS 带退出条件的 spike；
   原编号 Phase 10，2026-09-27 因调试交互插队顺延）
   **批次 0 已完成（2026-09-10，Renderer 拆分，BIT-IDENTICAL；旧口径 "Phase 10 批次 0"）**：drawFrame 577→291 行，
    摘出 5 模块（state_kit / frame_capture / cull_driver / frame_session / writeSceneStaging
    私有方法，详见 `git:调试历史` 2026-09-10）。阴影特性归属预置：新 pass 种类 →
    render_graph + ⑤ pass 循环；per-cascade 光锥剔除 → CullDriver（复用 ④c 编舞）；
    depth-only pass 状态 → StateKit 新增条目；不再往 drawFrame 塞步骤。
  **前置批 11.0 + 11.4 已完成（2026-09-29）**：绿光源（`pack.py` emissive remap 漏补）、
    湿面（`_Wet` 材质 roughnessFactor → effective 0.12）、"背光面过暗"（env 0.2→1.0 /
    sun 15→12 / exp 1.2→1.0，同材质阴影/受光线性比 0.0022 → 0.037）、SSAO（depth-only，
    默认 0.5，强度 0 逐位旁路）。计划与实际偏差（判据改了、11.0c 未做）见
    [phase11_plan.md](phase11_plan.md) §8；排障细节见 调试历史 2026-09-29。
  ① 保守光栅化基建（VK_EXT_conservative_rasterization，RADV 26.2.1 已实测可用 rev 1）：
     深度写入保守化（重叠三角形不留缝）——CSM/HFTS 共同硬前提。
     **能力边界已实测（2026-09-29，phase11_plan.md §9）**：OVERESTIMATE 基线语义可用，
     但 `maxExtraPrimitiveOverestimationSize = 0` = **没有外扩（fattening）余量**，
     漏光只能靠 normal offset + slope-scaled depth bias；好在
     `extendedDynamicState3ConservativeRasterizationMode = true` → 走 DynamicState，
     不为它加管线条目
  ② CSM（Cascaded Shadow Maps）先行【承诺交付】：depth-only 阴影深度 pass（mesh 管线
     新条目类型，无材质分叉）+ 4 级 cascade + PCSS 采样；**光锥剔除复用 ④c GPU culling
     模式**（现 GPU cull 只剔相机视锥，阴影 pass 需按光锥重跑一遍 culling dispatch）；
     PCSS penumbra 在 Phase 8 HDR float 累加里算
  ③ HFTS/IzB（Hybrid Frustum-Traced Shadows）【spike，带退出条件，非承诺项】：
     Irregular Z-Buffering（全视锥 Z-buffer + 光线步进，无 texel 走样）远场/场景遮挡 +
     近场 CSM/PCSS 混合。**验收**：Bistro 场景光线步进软阴影视觉正确 + 成本 ≤X ms
     （**X = 3 ms**，Phase 9 批 9.4 定的 GPU 侧余量基线：本机 forward 1280×720 的
     `gpu.frame` 2.98 ms / 165 Hz 预算 6.06 ms，超出即掉帧——注意本机现状是 **CPU-bound**，
     CPU 录制侧无余量，详见 `docs/phase9_plan.md` §10）；
     **达不到即收手，CSM+PCSS 为最终交付**（The
     Division 自身近场也是 shadow map，HFTS 只负责远场）。优先走 FTIZB 路线
     （光栅管线 + 硬件保守光栅 + early-z，非重 compute）
     参考链（公开，勿逆向游戏——系统逻辑在 CPU 侧，shader 里没有）：
     - FTIZB "Frustum-Traced Raster Shadows: Revisiting Irregular Z-Buffers"（NVIDIA 2015，
       research.nvidia.com 有 camera-ready PDF）——最贴近本实现的版本
     - "Conservative Z-Prepass for FTIZB"（Tokuyoshi 2018）——性能加速
     - 《Irregular Z-buffer Shadows》assassin-plus.github.io（2025-03）——最新教程式 walkthrough
     - HFTS 原文（Wyman SIGGRAPH 2016，cwyman.org/abstracts/sig2016_hfts.pdf）+ Story 2015 原文
     - github.com/strexicious/izb-on（实验参考，非生产级；2026-10-01 核实：13 commits，作者已弃坑）
     - **执行计划（2026-10-01 调研定稿）→ `docs/hfts_izb_plan.md`**：论文全文结论 /
       项目 gap / 架构方案 / 分阶段 1a–1e / 收手判据；验收分批 = 阶段 1 只验正确性
       （无 ms 门），预算口径阶段 2 实测后定（见其 §4.4，替代上文 X=3 ms 口径）
    ↓
    ↓
Phase 12：测试体系（**补立计划** 2026-09-30，[phase12_plan.md](phase12_plan.md)）——
   12.1–12.3 滚动落地：tests/ 单测层（113 用例无 GPU 毫秒级）+ 设备原语层 +
   闭式解探针 + **imgqa**（C++，区域归因判据：objectID 反解可见 piece / 闭式解逐面
   预测 / 开关影响域域外恰为 0；反模式登记 ssim 等；排查渲染问题默认起手）。
   剩余按缺口清单排：12.3b 闭式解收单源+switch 首战 / 12.3c 变异测试晋升 /
   12.4 三机跨机（字节锚点每机登记制）/ 12.5 qa_scenes C++ 重写 / 12.6 收尾。
    ↓
Phase 13：TAA 收口（**2026-10-03 立项**，[phase13_plan.md](phase13_plan.md)；设计与踩坑账的权威
   载体 = [taa_plan.md](taa_plan.md)，批 12.6 T0–T4 判据收口，优化项未动）。
   **立项理由不是"TAA 没做完"，而是三件更糟的事**：① T3 的三条 + T2 的一条验收被"跨帧历史每帧被
   `VK_IMAGE_LAYOUT_UNDEFINED` 销毁"这个 bug **空洞满足 / 作废**（历史是死的 ⟹ 按构造不可能有拖影；
   "无拖影/无残影"这类**负向观测**同时被"实现正确"与"实现根本没跑"满足），修好后一条都没重跑；
   ② 全部判据跑在合成靶 + env/skybox 关闭的配置上，真实 bistro 一次都没看过；③ 默认关且从未做过
   "要不要开"的决策（无性能账）。
  **已收口（2026-10-03 / 10-04）**：
  ① **真实场景首测 = 不合格**（用户肉眼"地震" + 梯度能量 −49.2%，而用 `feedback=1.0` 连拍恰好一个
     抖动周期取平均构造出的**理想 16 相位平均**独立参考只该糊 −24.2% ⟹ 比自身应收敛到的结果还糊
     1.5 倍）→ 当天拍板的"默认开"当天**撤回**。性能不是障碍（frame p50 +0.335ms = +1.3%、
     `gpu.frame` 仅 +0.064ms，代价全在 CPU record；数字绑机器 = 2026-10-03 的 AMD 8060S /
     RADV STRIX_HALO，跨机先复测再比）。
  ② **三个真 bug 修掉**：**历史重采样复利糊化**（历史就是累积图，每帧一次双线性重采样 = 老贡献被
     反复低通 → 精确 4×4 Catmull-Rom；判据 = 糊度对历史年龄的依赖归零）· **CR 核写成奇函数**
     （三次项 `a³` 应为 `|a|³` ⟹ t=0 权重和 6 ⟹ 2D 直流增益 36× ⟹ 98.3% 像素每帧拒收历史
     = 输出退化单帧，即用户看到的"边缘疯狂闪烁"）· **`saturate` 顶平线性 HDR 亮部**（时域不动点
     变成 `X=(1−α)·1+α·cur` ⟹ 白屋顶 8.83% 像素压暗 −18.5 LSB；修 = `clampHdr` 只夹下界）。
  ③ **"整幅游动"定案为机制不是 bug**：输出长在抖动空间 `out_t(uv)=F(uv−j_t)`，重投影只把历史对齐
     到基空间、不改 `j_t` ⟹ **时域累积压不掉它**（`feedback=1` 完全不累积时仍 0.674px）。
     拍板消掉 = **基空间输出**（门 `taa.base_space` **默认开**：把本帧内容位移折进 tonemap 的采样
     坐标，零额外 pass，核用双线性——CR 在 `aa-coverage` 上退化 + 负瓣振铃，实测否决），
     游动 0.643 → **0.060px**。
  ④ **判据新增**（`imgqa taa-swim` / `aa-coverage`）：`temporal-identity`（零抖动 + 静止 ⟹ TAA 开
     必须等价于关；可复用判断 = **做重采样算子，第一个判据是"采样点对齐纹素中心时必须等于直接
     取值"**，而不是任何端到端指标）· `temporal-jitter-transfer`（抖动传递对闭式，误差 0.4%，
     含坐标口径守卫）· `temporal-swim-pixels`（游动以像素计，钉**机制**不钉好坏）·
     `temporal-base-space`（门开主判据，阳性锚点 = 同次运行的门关态）。
  ⑤ **量具教训**（本项目已三次栽在"参考的观测坐标系与被测项不一致"）：软件 EMA 在**未抖基空间**
     累积、引擎在**抖动空间**，两者不是同一个量（据此得出的"实机/软件 EMA = 8.66×"已撤回）；
     phaseCorrelate 的三点抛物线亚像素在位移恰为半整数处系统性偏移 −0.320px（旧锚点正踩奇点
     ⟹ 换 (3.25,−2.125)，阈值一字未动）。
   **剩余（下一步从 phase13_plan §2 第一条未收口项读）**：13.0 / 13.0b 重验被空洞满足的验收 +
   把"预热无关性"探针（`warm=2` vs `warm=128` 截图逐位相同）固化成判据 / 13.1 真实场景画质判据
   固化 / 13.3 细杆欠累积缓解（**先修实验的区分力再动手**，box9 消融已翻车）/ 13.4 双图交替省
   `taa_commit` copy（测不出收益就砍）/ 13.2 收尾 = 重跑性能账后由用户拍"默认开"——**仓库源
   `configs/scene.lua` 的 `taa.enabled` 现仍为 false**（红线：TAA 关 = 画面逐位不变，且必须用
   基线配置验）。记债继承（AO 施加在 TAA 之后 / 无 velocity buffer ⟹ per-entity 动画会拖影 /
   YCoCg 扩盒 / 跨帧资源布局追踪被销毁 = 现有判据体系看不见）→ phase13_plan §3。
    ↓
远期候选：完整 IBL（prefiltered + BRDF LUT）、MASK alphaTest、OIT（imgui 已升格为 Phase 10，2026-09-27）
```

## 注意事项（现行）

1. **Reflection 输出是 `.cppm` 而非 `.hpp`**（全项目 C++20 modules）。
2. **Schema 与 Lua 配置是两套机制**：schema 校验 shader 契约（build time），Lua 描述场景意图
   （runtime），不要混淆。
