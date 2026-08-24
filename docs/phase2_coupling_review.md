# Phase 2 耦合审查（2026-08-23）

> **范围**：`vulkan_practice` 主项目（Phase 0–2，含 cube/model 并存现状）。`min_mesh_crash` 为独立最小复现，不在范围。
> **性质**：问题清单 + 修复方向，**不含任何代码改动**。所有条目状态 = Open，供人工 review 后逐条处置。

---

## 元诊断

主导的耦合味道**不是"某处代码写得烂"**，而是：

> **增量开发时"加平行特例路径"而不是"统一到一个抽象"** —— 每个 Phase 都往旁边加一条平行路径，从不回头归一。

cube 只是这个 pattern 最显眼的实例；`DrawArgs`/`ModelDrawArgs`、slot `0/4` vs `reserved+8+i`、固定 frustum vs 自适应 frustum、全局 `spin` 变换，全是同一个 pattern 的实例。

因此修复**不是重写**，而是**一次归一化重构**（或统一推迟到 Phase 4 ShaderRegistry，见 `next_roadmap.md`）。

---

## 问题清单

| ID | 问题 | 严重度 |
|----|------|--------|
| [H1](#h1--cube-是绕开-ecs-的平行路径) | cube 是绕开 ECS 的平行路径 | High |
| [H2](#h2--没有共享-draw-call-抽象) | 无共享 draw-call 抽象：2 push constant struct + 2 slot 方案 + 2 draw 块 | High |
| [M1](#m1--showcube-开关是补丁且不一致) | `showCube` 开关是补丁：gate 渲染不 gate 资源，与 model 不对称 | Medium |
| [M2](#m2--ktrianglespergroup-契约常量-cppshader-各写一份) | `kTrianglesPerGroup=10` 契约常量 C++/shader 各写一份（护城河缺口） | Medium |
| [M3](#m3--两套-lua-机制易混) | 两套 Lua 机制（scene config vs schema）易混 | Medium |
| [L1](#l1--spin-是全局特例变换) | `spin` 是全局特例变换，非 per-entity | Low |
| [L2](#l2--自适应-frustum-不含-cube) | 自适应 frustum 只认 model，不含 cube | Low |
| [L3](#l3--renderer-持有未使用的-registry) | Renderer 持有未使用的 `entt::registry*`（死依赖） | Low |
| [L4](#l4--默认-spv-路径两处重复) | 默认 SPV 路径两处重复 | Low |
| [L5](#l5--pushdataext-set-0-约定隐式共享) | `pushDataEXT` "set 0" 约定被两种不同大小 struct 隐式共享 | Low |
| [L6](#l6--modelmeshassets-生命周期契约脆弱) | `ModelMeshAssets` 生命周期契约脆弱（隐式 RAII） | Low |

---

## High

### H1 — cube 是绕开 ECS 的平行路径

cube 与 model 是**两条平行渲染路径**；cube 完全绕开 ECS（无 entity、无 Transform/MeshRef/MaterialRef），自带独立的数据、buffer、slot、draw args、draw 块。

**证据：**

| 环节 | cube（特殊路径） | model（通用路径） |
|------|-----------------|------------------|
| 数据 | `cube.cppm:9-31` 硬编码 24B `Vertex`（pos+**color**） | `gltf_loader.cppm:106-231` 解析，32B packed（pos+normal+uv） |
| GPU 上传 | `vkresources.cppm:35-44` buffer 是 **Resources 类成员**，构造期建 | `model_importer.cppm:29-82` `uploadModelMeshes`，每 primitive 一个 buffer |
| 描述符 slot | `vkresources.cppm:46-48,150-188` slot **0/4**，Resources 构造期写 | `model_importer.cppm:64` slot `reserved+8+i`，upload 时写 |
| ECS | **无 entity**，不注册 | `model_importer.cppm:87-114` `registerScene`，每 primitive 一个 entity |
| draw args | `types_generated.cppm:10-13` `DrawArgs`(64B 仅 mvp) + `vkrenderer.cppm:76` `updateDrawArgs` + `ecs.cppm:190` **renderSystem 直调（绕过 ECS）** | `model_types_generated.cppm:10-16` `ModelDrawArgs`(96B) + `vkrenderer.cppm:78-82` `updateModelDrawArgs` + `ecs.cppm:194-202` `rg.view<..>().each()` **从 ECS 推** |
| 管线条目 | `vkrenderer.cppm:134-145` "cube" 条目（仅 `showCube` 时建） | `vkrenderer.cppm:155-161` 每 primitive 一条 "model_i" |
| 绘制 | `vkrenderer.cppm:406-411` `findPipelineEntry("cube")` 守卫 + `drawMeshTasksEXT(1,1,1)` | `vkrenderer.cppm:416-427` 循环 `model_i` + `drawMeshTasksEXT(groupCount,1,1)` |

**影响：** 两套渲染系统并存、共享同一个 mesh shader。cube 无法像 ECS entity 那样自然"移除"，只能靠 config bool 打补丁（见 M1）。将来加第三种资产要再开一条平行路径。

**关键 nuance（为什么不能几行合并）：** cube 与 model 不只是"同一个东西不同数据"，是**两种着色模型**——cube 走 per-vertex color（`cube.frag` 逐顶点读色），model 走 flat material baseColor（`model.frag` 用材质色）。顶点格式、frag shader、push constant 结构全不同。彻底归一需要 **Phase 4 ShaderRegistry**（`next_roadmap.md` "Phase 4：Shader 反射扩展 + 多 Shader 支持"）。

**修复方向：** cube 变一等资产、走与 model 同一路径（upload / registerScene / ModelDrawArgs / ECS view）。彻底解 = Phase 4 ShaderRegistry。

---

### H2 — 没有共享 "draw call" 抽象

无公共 `DrawCall`/`RenderItem` 概念；cube 与 model 各有一套 push constant struct、update 方法、slot 方案、draw 块，Renderer 必须同时"认识"两个特例。这是 H1 的根因。

**证据：**

- 两个 struct：`types_generated.cppm:10-13` `DrawArgs`(64B) vs `model_types_generated.cppm:10-16` `ModelDrawArgs`(96B)
- 两个 update 方法：`vkrenderer.cppm:76` `updateDrawArgs` vs `vkrenderer.cppm:78-82` `updateModelDrawArgs`
- 两套 slot 方案：`types_generated.cppm:17-19`（0/4）vs `model_types_generated.cppm:27`（`HeapSlotModelVertexBase=8`，`reserved+8+i`）
- 两个 draw 块：`vkrenderer.cppm:406-411`（cube 单 draw）vs `vkrenderer.cppm:416-427`（model 循环）

**影响：** 每加一种资产 = 加一组 struct + 方法 + slot 方案 + draw 块，Renderer 持续膨胀。

**修复方向：** 单一 per-primitive/per-entity draw-args + 单一 draw 循环，cube 只是其中一个 item。（与 H1 联动。）

---

## Medium

### M1 — `showCube` 开关是补丁，且不一致

`showCube` 只 gate **pipeline 创建**（`vkrenderer.cppm:134`）+ **draw 守卫**，但 cube 的 **buffer/descriptor 在 `Resources` 构造期无条件分配**（`vkresources.cppm:35-48`）——`cube=false` 不释放资源，只停绘制。

**证据：**

- `scene_config.cppm:25`（`showCube` 字段）+ `scene_config.cppm:84-86`（读 `cube` 字段）
- `vkrenderer.cppm:134`（pipeline 条目 gate）
- `vkresources.cppm:35-48`（buffer/descriptor 构造期无条件建，未 gate）
- `configs/scene.lua:21`（`cube = false`，注释"与模型互穿时建议关掉"）
- **不对称**：model 无 `showModel`，靠 `modelPath` 是否为空进出场景（`main.cpp:42-51`）；cube 靠专属 bool。

**影响：** 开关控制的是"渲不渲"不是"有没有"，资源照样占着。"对象如何进出场景"存在两套规则（cube: bool / model: path 有无），不统一。

**修复方向：** 场景成员数据驱动（哪些资产在场景里），删掉特例 bool。

---

### M2 — `kTrianglesPerGroup` 契约常量 C++/shader 各写一份

`kTrianglesPerGroup=10` 在 C++ 与 shader 各硬编码一份，无编译期/build 期校验。

**证据：**

- `vkrenderer.cppm:251`（`static constexpr uint32_t kTrianglesPerGroup = 10`，注释"必须与 model.mesh.slang 一致"）
- `model.mesh.slang:21`（`const uint kTrianglesPerGroup = 10`）
- `model.mesh.slang:8`（shader 自身注释"kTrianglesPerGroup ↔ C++ 侧"）

**影响：** 这正是反射护城河要防的"C++/shader 契约漂移"，但 group 数这个常量**未被反射/schema 管线覆盖**——护城河有个缺口。改一边忘改另一边，group 数错位，无任何告警。

**修复方向：** 纳入 schema 校验（schema 增加 group 数字段）；或至少 `static_assert`/运行时 check。**便宜，且直接强化护城河。**

---

### M3 — 两套 Lua 机制易混

运行时场景配置与 build 时 shader 契约**都是 Lua + sol2 沙箱**，形式相同、语义不同。

**证据：**

- `configs/scene.lua`（运行时意图：model/pipelines/spin/cube）
- `vulkan_practice/shaders/types_schema.lua` + `model_schema.lua`（build 时契约）
- 两者均经 sol2 沙箱加载（`scene_config.cppm:55-60` 与 `tools/` 侧）
- `next_roadmap.md` 注意事项 #2 专门写"Schema 和 Lua 配置是两套机制……不要混淆"

**影响：** 需要写警告本身就是个味道；后续贡献者/LLM 可能改错文件，或误以为 schema 控制运行时行为。

**修复方向：** 靠目录/命名把区别做显式（`configs/` = 运行时，`shaders/*_schema.lua` = build 时）；文档警告维持。

---

## Low

### L1 — `spin` 是全局特例变换

`spin` 是 renderSystem 层手搓的 time 旋转矩阵，乘进所有 MVP，不在 ECS Transform 里。

**证据：** `ecs.cppm:161-163`（spin 矩阵）+ `ecs.cppm:190`（乘进 cube MVP）+ `ecs.cppm:198`（乘进 model per-primitive MVP）

**影响：** 要 per-object 旋转就不 scale；且它是调试辅助（"便于肉眼判断渲染对错"）却被烘进渲染循环。

**修复方向：** 需要 per-object 旋转时纳入 ECS Transform；当前可接受，留注。

### L2 — 自适应 frustum 不含 cube

`ModelBounds` 仅 `registerScene` 写入，只反映 model；只有 cube 时回落固定 near/far。

**证据：** `model_importer.cppm:90`（`ModelBounds` 仅 model 写）+ `ecs.cppm:171`（`rg.ctx().find<ModelBounds>()`）+ `ecs.cppm:169-187`（无 model 回落固定值）

**影响：** 仅 cube 在场（无 model）时，cube（原点 ±0.5）可能被固定 `far=10` 切掉。frustum 逻辑只"知道"model。

**修复方向：** cube 保留为一等对象时，对全部场景内容（含 cube）算 bounds，并入 `ModelBounds`。

### L3 — Renderer 持有未使用的 registry

Renderer 构造存了 `entt::registry*` 但类内**零使用**——死依赖。

**证据：** `vkrenderer.cppm:56`（构造参数）+ `vkrenderer.cppm:60`（`registry(&registry)`）+ `vkrenderer.cppm:234`（成员）

**修复方向：** 确认未用后从 Renderer 构造器移除。

### L4 — 默认 SPV 路径两处重复

默认 pipeline 的 SPV 路径在 C++ 默认值与 Lua 各写一遍。

**证据：** `scene_config.cppm:31-34`（`defaultSceneConfig` 的 cube/model SPV 路径）+ `configs/scene.lua:7-17`（同一路径再写一遍）

**影响：** 改名 shader 要改两处。（scene.lua 存在时 C++ 默认被覆盖，Lua 是运行时真相，但默认仍是重复。）

**修复方向：** 默认 pipeline 路径单一真相源。

### L5 — `pushDataEXT` "set 0" 约定隐式共享

两种不同大小的 push constant struct 都隐式用 "set 0"。

**证据：** `vkrenderer.cppm:407`（cube `pushDataEXT(0, …, DrawArgs 64B)`）+ `vkrenderer.cppm:423`（model `pushDataEXT(0, …, ModelDrawArgs 96B)`）

**影响：** 因是独立 draw call + 独立 shader 绑定，当前无害；加第三种不同 push constant 布局的 shader 时，"set 0" 约定可能冲突。

**修复方向：** 纳入 ShaderRegistry 修复——push constant set 按 pipeline 显式化。

### L6 — `ModelMeshAssets` 生命周期契约脆弱

heap descriptor 经 device address 引用 per-primitive buffer；buffer 存活靠 `main.cpp` 局部变量"活到渲染结束"的隐式 RAII 保证。

**证据：** `model_importer.cppm:22-25`（`ModelMeshAssets` 持有 `vertexBuffers`）+ `main.cpp:67-77`（局部变量，注释"须活到渲染结束"）

**影响：** 契约是隐式 RAII（`SharedAllocator` 保证 `meshAssets` 存活即 buffer 存活），非显式所有权；顺序一变即悬空。

**修复方向：** 显式所有权（buffer 移入更长生命周期资源）或文档化。

---

## 好的部分（别动，别误伤）

- **VMA descriptor heap RAII + 双端对齐**（`VmaBuffer.cppm:105-314`）：GPU `resourceHeapAlignment` / CPU `descriptorSize` 对齐 + 厂商 reserved 槽位记账，理解深、封装干净。
- **反射护城河**（generated `.cppm` + `static_assert` + schema 校验）：契约单一真相源 + 编译期安全网。
- **双 fence 同步**（`vkrenderer.cppm:174-206,271-278,476-487`）：inFlight + present 双 fence，避免 VUID-00067 semaphore 重用违规。
- **中央事件轮询**（`ecs.cppm:33-106`）：单一 `SDL_PollEvent` 调用点，避免 legacy"多系统各自轮询"的坑。
- **错误处理切分**（init 抛异常 / 运行时 `std::expected` + `[[nodiscard]]`）。
- **shader object + 动态状态解耦**（`shader.cppm:36-97` `RenderState`/`applyDynamicState` 移出 `RenderPipeline`，为 Phase 7 多 pass 留接缝）。

---

## 建议

| 路线 | 做法 |
|------|------|
| **现在还债** | 先做 **H2 + M1**（统一 draw 路径 + 删开关）。注意会动 cube 着色模型（per-vertex color 需先决定归到哪类 material）。**L3/L4 可顺手修**（便宜）。 |
| **等 Phase 4** | 把 **H1/M1** 记成已知债（`AGENTS.md` 已知问题），只补 **M2**（契约缺口——便宜且直接强化护城河）。 |

> review 处置后，建议把已决条目回写本文件（标 `~~strikethrough~~` + 结论），并在 `AGENTS.md` 已知问题 / `next_roadmap.md` 对应 Phase 留接缝引用。
