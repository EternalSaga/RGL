# ECS 合规审查与修复方向（2026-08-24）

> **状态**：现行（批次 2/3 处置记录为权威；后续 Phase 的 ECS 架构决策以此为基线）

> **范围**：`vulkan_practice` 主项目（Phase 0–2 完成后的现状）。`min_mesh_crash` 为独立最小复现，不在范围。
> **性质**：问题清单 + 修复方向。2026-08-24 批次 2（draw call 抽象 + system 化）已落地，已决条目见 §处置记录。
> **与 [phase2_coupling_review.md](phase2_coupling_review.md) 的关系**：耦合审查从"耦合味道"视角诊断；
> 本文以 ECS 设计规范为标尺重新组织同一代码库，重点补上耦合审查未覆盖的结构性条目
> （E1 场景数据散落/构造参数泄漏、E3 system 化缺失、E5 组件设计）。
> 两份文档交叉引用：耦合审查已列条目（H1/H2/M1…）不重复正文，只在 §修复路线 映射到 ECS 修复路径。

---

## 元诊断

主导的 ECS 不合规**不是"没用 entt 写代码"**——view 驱动渲染、context 单例、中央事件轮询都已就位——而是：

> **registry 不拥有场景**。场景的三件事——"场景里有什么"（构成）、"资产在哪"（GPU 数据）、
> "每帧怎么画"（绘制决策）——分别落在 `main.cpp` 局部变量、`Resources`/`Renderer` 成员、
> `renderSystem` 函数体三处；registry 只是"元数据货架"。
> 每帧数据流仍是命令式调用链（renderSystem 手搓 MVP → Renderer setter → drawFrame 硬编码 draw 块），
> 不是"system 读组件 → 产出帧数据 → 执行器消费"的 ECS 数据流。

增量开发模式（每个 Phase 加一条平行路径而不回头归一）在 ECS 视角下的表现：
**场景构成路径（cube）和执行路径（draw）从未被拉进 ECS 框架**。
registry 只在 Phase 2 的 model 路径上生效了一半（entity 注册 + view 查询），同一帧里 cube 的绘制完全绕过它。

---

## ECS 设计规范（参照系）

| # | 规范 | 项目现状 |
|---|------|---------|
| 1 | registry 是场景唯一数据源，场景状态不在 registry 外平行维护 | ✅ 每帧/配置状态已全部进 registry（E3/E4 已修复：`SceneSpin`/`SceneBounds`/`ViewProjection`/`WindowExtent`/`AppExit`/`FrameDrawList`）；model/cube/纹理/材质资产数据全部收敛进 Resources 资产表（E1.1–1.4 已修复：批次 3 + 2026-08-28 重构批次 1-3） |
| 2 | entity 是 ID、component 是纯数据、system 是逻辑唯一载体；system 间经数据通信，顺序显式 | ✅ 已修复（批次 2）：5 个每帧 system（camera/animation/bounds/projection/renderlist）+ 显式 system 列表（未用 entt::scheduler——process 调度器执行顺序无保证，见 §处置记录）；E1/E5 仍 Open |
| 3 | 场景构成数据驱动："场景里有什么" = registry 里有什么 entity，而非全局变量 / config 字段 / 构造参数 | ✅ config 不再进每帧路径（E3 已修复）；model = 上传 → 资产表 → entity 数据驱动（E1.1–1.3 已修复，批次 3；构造参数去耦合）；cube 同理（E1.4/M1 已修复，2026-08-28 重构批次 3：`showCube` → 是否注册 cube entity，画不画 = 实体有无） |
| 4 | 分层：底层 Vulkan 类不依赖 ECS、不认识具体资产；渲染 system 产出抽象 draw list，执行器只执行 | ✅ 已修复（批次 2）：renderlist system 产出 `FrameDrawList`，`drawFrame` 盲执行单循环；Renderer 零 entt 依赖（L3 已修复） |
| 5 | 资产与场景实例分离，组件引用稳定资产 ID 而非侧数组下标 | ✅ 已修复（批次 3）：`MeshRef.asset` 资产 ID 引用 Resources 资产表；五方一致性收敛为资产表一处映射（E1.1/E5.1） |

> 正文"证据"行号基于批次 2 重构前代码（2026-08-24）；已修复条目的证据为当时快照，现状以 §处置记录 为准。

---

## 问题清单

| ID | 问题 | 严重度 | 状态 |
|----|------|--------|------|
| [E1](#e1--registry-不拥有场景数据三处散落) | registry 不拥有场景：数据三处散落 + 场景形状泄漏进构造器 | High | E1.1–1.3 **已修复**（2026-08-25 批次 3：资产表 + 生命周期收敛 + 构造参数去耦合，含 L6）；E1.4（cube 一等资产）**已修复**（2026-08-28 重构批次 3） |
| [~~E2~~](#e2--renderer-认识具体资产分层倒置无-draw-list) | Renderer 认识具体场景资产，分层倒置，无 draw list | High | **已修复**（2026-08-24 批次 2） |
| [~~E3~~](#e3--rendersystem-god-function无调度器config-泄漏) | renderSystem god function：帧循环 + 未 system 化逻辑 + 无调度器 + config 泄漏 | High | **已修复**（2026-08-24 批次 2） |
| [~~E4~~](#e4--全局状态不进-ecs) | 全局状态不进 ECS：spin / ModelBounds / Viewport | Medium | **已修复**（2026-08-24 批次 2） |
| [E5](#e5--组件设计偏离纯数据引用约定) | 组件设计偏离"纯数据引用"约定：MeshRef 下标 / MaterialRef 值拷贝 / Relationship 双向 / CameraInput 原始状态 + 键位硬编码 | Medium | E5.1/E5.2/E5.4 **已修复**（2026-08-25 批次 3：MeshRef 资产 ID 化 + Relationship 单向化 + 输入事件化）；E5.3（MaterialRef → 材质资产表）**已修复**（2026-08-28 重构批次 2：`MaterialRef{material}` 真引用） |
| [~~L3~~](#l3--renderer-持有未使用的-registry) | Renderer 持有未使用的 `entt::registry*`（死依赖） | Low | **已修复**（2026-08-24 批次 2 顺手） |

> L4（默认 SPV 路径重复）/ L5（set-0 约定）/ L6（ModelMeshAssets 生命周期）为耦合审查条目，
> 与 ECS 修复路线的关系见 §修复路线 映射表；正文见 [phase2_coupling_review.md](phase2_coupling_review.md)。

---

## High

### E1 — registry 不拥有场景，数据三处散落

**现状（证据）：**

- 同一 primitive 的信息存三份，互相靠下标约定对齐：
  - CPU 数据：`main.cpp:42-52` `std::optional<GltfModel> model`（main 局部变量）
  - GPU 资源：`main.cpp:67-77` `ModelMeshAssets meshAssets`（main 局部变量，注释"须活到渲染结束"，即 L6）
  - 元数据：registry entity（`model_importer.cppm:95-112`）
- **五方一致性靠约定**：entity i ↔ `GltfModel::primitives[i]` ↔ heap slot `reserved+8+i`（`model_importer.cppm:64`）↔ 管线条目 `"model_i"`（`vkrenderer.cppm:159`）↔ `modelDrawArgs[i]`（`vkrenderer.cppm:67`、`:416-427`）↔ renderSystem view 里的 `primitiveIndex`（`ecs.cppm:194-202`）。
- **场景形状泄漏进基础设施构造器**：`primitiveCount` 是 `Resources` 构造参数（`vkresources.cppm:26-27`，决定 heap 容量 `:139-140`）和 `Renderer` 构造参数（`vkrenderer.cppm:55-56`，决定管线表条目数 + `modelDrawArgs` 大小 `:67`）。
  由此产生 `main.cpp:41` 的硬顺序约束（"CPU 侧解析模型先于 Vulkan 初始化：primitive 数 N 决定 descriptor heap 容量"）——
  场景内容决定了基础设施的构造，因果方向反了。

**ECS 违反：** 规范 1 + 3 + 5。registry 不是场景唯一真相源；"场景里有什么"由 config 字段与构造参数共同决定；
组件（MeshRef）引用侧数组下标而非资产身份。

**影响：** 每加一种资产 / 一个场景来源，三份数据 + 五方约定 + 构造参数链都要同步扩展；
运行时增删场景内容没有入口；`main.cpp` 构造顺序脆弱（顺序一变即悬空，L6 的根）。

**修复方向：**

1. **资产表（AssetTable）**：新增资产层——每个已上传 primitive 一条资产记录
   （GPU buffer + heap slot + 三角形数 + 管线条目引用），以稳定 id 索引
   （registry 内的资产 entity，或 unique id + 资产 vector 表）。
   `MeshRef.primitiveIndex` → `MeshRef.asset`（资产 id），五方约定收敛为一处映射（资产表）。
2. **生命周期收敛**（顺手修 L6）：`ModelMeshAssets` 从 `main.cpp` 局部变量移入资产表——
   资产表成为 registry context（或 `Resources` 成员，与 cube buffer 并列），
   buffer 存活不再靠 main 作用域隐式保证。
3. **构造参数去耦合**：`Resources`/`Renderer` 构造签名删除 `primitiveCount`。
   heap 容量改为启动期固定裕量（如 `reserved + 8 + 256`）——
   `VmaDescriptorHeap` 当前不支持运行时扩容（容量 = buffer 分配大小，`VmaBuffer.cppm:271`；
   运行时扩容 = 重建 heap + 重写全部描述符，归 Phase 3 TexturePool 一并设计）。
   管线表条目数 = 资产表大小，注册资产时动态 `emplace_back`，不再按构造参数预 resize。
4. **（依赖 E2/H1）** cube 变一等资产：buffer 从 `Resources` 成员（`vkresources.cppm:35-44`）移入资产表，
   slot 0/4 走同一分配器；cube 的"有无" = 资产/entity 在场景里的有无（顺手删 M1 的 `showCube` bool）。

**依赖：** 1–3 独立可做（model 路径内部重构，行为不变）；4 依赖 H1/Phase 4
（cube 的 per-vertex color 归哪类 material 需先定）。

---

### E2 — Renderer 认识具体资产，分层倒置，无 draw list

**现状（证据）：**

- 底层 Vulkan 类 `Renderer` 硬编码两类资产的全部知识：
  - 两份每帧数据成员：`cubeDrawArgs`（`vkrenderer.cppm:246`）、`modelDrawArgs`（`:248`）
  - 两个 setter：`updateDrawArgs`（`:76`）、`updateModelDrawArgs`（`:78-82`）
  - 两个 draw 块：cube（`:406-411`）+ model 循环（`:416-427`）
  - 每帧字符串查管线：`"model_" + to_string(i)` → `findPipelineEntry`（`:417-418`）
- **draw 顺序隐式写在 drawFrame**：先 cube 后 model（下标序），渲染 system 无法控制绘制顺序、
  无法批量、无法插入排序——Phase 5 透明排序、Phase 7 culling 输出都没有落点。

**ECS 违反：** 规范 4。执行器（Renderer）应只消费抽象帧数据（draw list）；
"画什么、画几次、什么顺序"是渲染 system 的职责，不是 GPU 驱动类的知识。
每加一种资产 = Renderer 成员 + setter + draw 块三处同步膨胀（H2 的根因）。

**影响：** 同 H2；另使 Phase 5/7 的排序与 culling 集成被迫"改 Renderer 加块"，
与 roadmap Phase 7 "能力门控不改提交逻辑"的目标相悖。

**修复方向：**

1. **DrawCall 抽象**：`struct DrawCall { 管线条目引用; push data 内容; uint32_t groupCount; }`。
2. **FrameDrawList（registry context）**：`renderListSystem` 每帧产出——
   model 部分由 `view<Transform,MeshRef,MaterialRef>` 推（现 `ecs.cppm:194-202` 的逻辑迁入），
   cube 部分在 cube 入 ECS 前由 system 特例生成（见 E1.4）。
   **每帧数据属于帧，不属于 Renderer**：删除 `cubeDrawArgs`/`modelDrawArgs` 成员与两个 setter。
3. **盲执行**：`Renderer::drawFrame(const FrameDrawList&)` 单循环：
   `pushDataEXT`（push constant 布局随管线显式化，见 L5）→ `bindShaders` → `drawMeshTasksEXT(groupCount,1,1)`。
   draw 顺序 = draw list 顺序，显式可控。
4. **管线引用去字符串化**：DrawCall 存管线条目下标（资产表 → 条目下标），删除 `"model_i"` 字符串约定；
   name→pipeline 的 ShaderRegistry 归 Phase 4。

**依赖：** 1–3 与 E3 是同一批（renderListSystem 是两者的桥）；cube 并入 draw list 的完整形态
依赖 E1.4/Phase 4——过渡期 cube 的 DrawCall 由 renderListSystem 特例生成
（特例从 drawFrame 上移到 system 层，位置正确）。

---

### E3 — renderSystem god function，无调度器，config 泄漏

**现状（证据）：**

- `renderSystem`（`ecs.cppm:124-217`）是自由函数且内含整个帧循环（`while(true)` `:137`），
  函数体内联了四类未 system 化的每帧逻辑：
  - 场景 spin 矩阵（`:161-163`，读 `SceneConfig.spin`）
  - 自适应 near/far（`:169-181`，读 `ModelBounds` context）
  - 投影矩阵（`:183-187`，读 Camera + swapchain extent）
  - per-entity MVP 组装（`:194-202`，读 Transform/MeshRef/MaterialRef）
- **无注册/调度**：system 顺序 = 函数体调用顺序
  （eventSystem `:138` → resizeSystem `:140` → cameraSystem `:157` → 内联逻辑 → drawFrame `:204`）。
  加一个 system = 改 renderSystem 函数体；system 间顺序契约（如 eventSystem 必须先于 cameraSystem）无声明处。
- **config 泄漏进每帧**：`renderSystem(…, const SceneConfig& sceneConfig)`（`:125`）——
  `spin`/`showCube` 字段直接决定每帧行为；config（启动期数据）与每帧逻辑（运行期）混在一处。

**ECS 违反：** 规范 2 + 3。逻辑应住在 system 里、经数据通信；帧循环应是调度器而非 god function。

**影响：** 任何新增 system（Phase 5 排序、Phase 7 culling、动画）都要编辑 renderSystem；
system 间契约靠阅读函数体理解；config 改动的影响面无法从 registry 看出。

**修复方向：**

1. **逻辑拆 system**：
   - `animationSystem`：spin 从 config bool + 手搓矩阵变为场景根 entity 的 `Animation` 组件
     （axis/speed，可选）→ 每帧更新场景旋转（过渡期写 `SceneRotation` context，MVP 组装消费；
     per-entity 需求出现时改写 Transform，见 E4.1）
   - `projectionSystem`：Camera + SceneBounds + swapchain extent → `ViewProjection{view, proj, near, far}`
     context（现 `:164-187` 全部迁入，含自适应 near/far）
   - `renderListSystem`：ViewProjection + 场景实体 → FrameDrawList（E2.2）
2. **引入调度器**：本地 entt 3.16 已含 `entt::scheduler`（`process/scheduler.hpp`，按帧 tick 多个 system），
   或轻量替代（`std::vector<frame system>` 注册表 + 显式顺序）。帧循环收敛为：
   `scheduler.tick(rg, dt)` → `renderer.drawFrame(frameDrawList)` → 退出判定。
   eventSystem/resizeSystem 一并注册，顺序显式声明（event 先、resize 后、相机/动画/投影/绘制依次）。
3. **config → registry（启动期一次性）**：`SceneConfig` 在 main 里转成 registry 数据
   （场景根 entity + `Animation` / cube 开关组件），renderSystem 签名去掉 `SceneConfig` 参数。
   每帧 system 只读 registry；config 文件 = 场景初始数据，不再是运行期依赖。

**依赖：** 独立可做，且是 E2 的前置（renderListSystem 同时是 E2 的载体）。

---

## Medium

### E4 — 全局状态不进 ECS

**E4.1 spin（= 耦合审查 L1）：**

- 证据：`ecs.cppm:161-163`（config bool + 手搓矩阵）+ `:190`（乘 cube MVP）+ `:198`（乘 model MVP）。
- 修复：见 E3.1（`Animation` 组件 + `animationSystem`）。当前"全场景同一旋转"用 `SceneRotation` context 承载即可，
  数据结构（组件 + system）与 per-entity 需求出现后同构，无需二次重构。

**E4.2 ModelBounds 只认 model（= 耦合审查 L2）：**

- 证据：`model_importer.cppm:90`（仅 model 写入）+ `ecs.cppm:171`（renderSystem 硬编码 `rg.ctx().find<ModelBounds>()`）。
- 违反：context 的 producer（importer）与 consumer（renderSystem）耦合隐式，无 system 契约声明；
  聚合范围（"全部场景内容"）由单一 importer 的局部知识决定。
- 修复：`Bounds{center, radius}` 变**组件**（挂在有包围球的 entity 上，model primitive entity 与未来的 cube 都挂），
  `boundsSystem` 聚合全部 Bounds → `SceneBounds` context（union 包围球）。
  `projectionSystem` 只读 `SceneBounds`。cube 入 ECS（E1.4）后自动纳入聚合，L2 同时消解。

**E4.3 Viewport 混合数据与控制流：**

- 证据：`ecs.cppm:26-31`（width/height + needsResize + shouldClose 同 context）；
  eventSystem 写（`:42`、`:44-46`）、resizeSystem 读（`:110-121`）、renderSystem 读（`:128`、`:147`）。
- 违反：一个 context 服务三种职责（几何数据 / resize 请求 / 退出请求），三个 system 共享无封装。
- 修复：拆 `WindowExtent{width, height}`（数据，eventSystem 写、projectionSystem/resizeSystem 读）
  + 退出请求（bool 或 event，帧循环判定）。`needsResize` 降为派生状态：
  消费者比较 extent 与 swapchain 实际尺寸即可，不需要显式标志位。

---

### E5 — 组件设计偏离"纯数据引用"约定

**E5.1 MeshRef 是侧数组下标（与 E1 联动）：**

- 证据：`scene.cppm:28-31`（`primitiveIndex` + `triangleCount`）。
- 修复：`primitiveIndex` → 资产 id（E1.1）；`triangleCount` 移入资产表
  （它是资产属性，不是场景实例属性）。

**E5.2 Relationship 双向冗余：**

- 证据：`scene.cppm:21-24`（`parent` + `children` 双字段），`model_importer.cppm:110-111`（注册时双向写）。
- 违反：同一关系存两份，一致性靠写入方自觉；children vector 在注册后无增删入口。
- 修复：只保留 child 的 `parent`（单向）；children 查询由 `rg.view<Relationship>` 过滤 parent 派生
  （或 system 缓存）。legacy 的双向结构不继承
  （phase2_plan §2.2 已核查 entt 3.16 无内置 relationships 头，手写组件形状可自行定义）。

**E5.3 MaterialRef 名为 Ref 实为值拷贝：**

- 证据：`scene.cppm:34-36`（内嵌 `glm::vec4 baseColor`）；
  roadmap Phase 2 原文"材质信息解析为 `MaterialRef`（暂时只存引用）"，实现存的是值。
- 修复：Phase 2 过渡——改名 `Material{baseColor}`（名副其实）；
  Phase 4——`MaterialRef{materialId}` + 材质资产表（PBR `Material` struct 走反射生成，
  roadmap Phase 4 已规划）。

**E5.4 CameraInput 原始状态 + 键位硬编码：**

- 证据：`camera.cppm:40-49`（level-state bool 袋）；`ecs.cppm:47-104`（WASD/鼠标键位映射硬编码在 eventSystem）；
  `camera.cppm:142-143`（消费后只重置 delta 累积器，level bool 不重置）。
- 违反：输入不是事件流；键位绑定不是数据（不可配置）；
  eventSystem 与 cameraSystem 的"先写后消费 + 部分重置"契约隐式（靠 E3 的调用顺序保证）。
- 修复：输入事件化——`InputEvent` 流（context 内 deque：key press/release、mouse delta、wheel），
  eventSystem 只产生事件，cameraSystem 消费即清空；键位映射（WASD）移入 `configs/camera.lua`
  （沿用 sol2 沙箱 + 可选字段回落默认值模式）。mouseLeftDown 这类 level 状态保留为状态字段，
  但由消费方显式管理（cameraSystem 读写自己的拖动状态，而非共享 bool）。

---

## Low

### L3 — Renderer 持有未使用的 registry

- 证据：`vkrenderer.cppm:56`（构造参数）+ `:60`（`registry(&registry)`）+ `:234`（成员，类内零使用）；
  GMF 的 entt include（`:2`）。
- ECS 违反：规范 4（底层类对 ECS 模块的依赖无实际用途）。
- 修复：删构造参数 + 成员 + GMF entt include（`main.cpp:64` 调用点同步）。

> L4 / L5 / L6 见 [phase2_coupling_review.md](phase2_coupling_review.md)；与 ECS 修复路线的关系见下表。

---

## 好的部分（别动，别误伤）

- **view 驱动的 model 渲染**（`ecs.cppm:195` `rg.view<Transform,MeshRef,MaterialRef>().each()`）——
  ECS 数据流在 model 路径上已跑通；修复是扩展与归一，不是推倒。
- **中央事件轮询**（`ecs.cppm:34-106`，唯一 `SDL_PollEvent` 调用点）。
- **context 单例用法**（Camera/CameraInput/Viewport/ModelBounds 经 `rg.ctx()`）——
  合法 ECS 模式；问题在职责划分（E4/E5）不在机制。
- **RAII + `std::expected` 错误处理切分**（init 抛异常 / 运行时 expected + `[[nodiscard]]`）。
- **反射护城河**（generated `.cppm` + static_assert + schema 校验）——
  E1/E2 的修复全部在其契约框架内进行，不破坏单一真相源。

---

## 修复路线

### 与耦合审查条目的映射

| 耦合审查条目 | ECS 修复路径 |
|---|---|
| H1（cube 平行路径） | E1.4 + E2：cube 一等资产 + 统一 draw list；per-vertex color 的 material 归类决定在 Phase 4 |
| H2（无 draw-call 抽象） | E2（DrawCall + FrameDrawList + 盲执行） |
| M1（showCube 补丁） | E3.3 + E1.4：config → registry 数据驱动，场景成员 = entity 有无，删 bool |
| M2（kTrianglesPerGroup 契约） | 与 ECS 无关（反射护城河缺口）：schema 加 group 数字段或 static_assert，按耦合审查单独处置 |
| M3（两套 Lua 机制） | 与 ECS 无关：目录/命名显式化，按耦合审查单独处置 |
| L1（spin） | E4.1（Animation 组件 + animationSystem） |
| L2（frustum 不含 cube） | E4.2（Bounds 组件 + boundsSystem 聚合）+ E1.4 |
| L3（死 registry 依赖） | L3 |
| L4（SPV 路径重复） | 与 ECS 无关：默认 pipeline 路径单一真相源，按耦合审查单独处置 |
| L5（set-0 约定） | E2.3 的 push data 显式化，随 Phase 4 ShaderRegistry 落地 |
| L6（ModelMeshAssets 生命周期） | E1.2（资产表收敛所有权） |

### 分批执行

| 批次 | 内容 | 依赖 | 风险 |
|---|---|---|---|
| **批次 1**（现在，便宜独立） | L3（死依赖）；E5.2（Relationship 单向化）；E5.3 过渡改名（MaterialRef→Material） | 无 | 低，行为不变 |
| **批次 2**（现在，核心重构，一个单元） | E3（system 化 + 调度器 + config→registry）+ E2（DrawCall/FrameDrawList/盲执行）+ E4.1/E4.2（Animation/Bounds 进 ECS）+ E4.3（Viewport 拆分） | 批次 1 | 中：动帧循环骨架 + Renderer 接口；cube 的 DrawCall 过渡期由 renderListSystem 特例生成，行为不变 |
| **批次 3**（现在可做，或推迟到 Phase 3 开工前） | E1.1–1.3（资产表 + 生命周期收敛 + 构造参数去耦合）+ E5.1（MeshRef 资产化）+ E5.4（输入事件化 + 键位配置化） | 批次 2 | 中：heap 容量策略变化（固定裕量）；建议与 Phase 3 TexturePool 的 slot 分配设计并案——资产表是 TexturePool 的天然宿主 |
| **批次 4**（Phase 4） | H1/E1.4（cube 一等资产 + 删 showCube）+ E2 完整形态（cube 入 draw list）+ L5（push constant 按管线显式化）+ E5.3 完整形态（材质资产表） | Phase 4 ShaderRegistry + cube material 归类决定 | — |

> **批次 2/3 的决策点**：若倾向"逐步推进、每阶段稳定后再进下一阶段"（roadmap 核心原则 #4），
> 批次 3 可整体推迟到 Phase 3 开工前与 TexturePool 并案；
> **批次 2 建议现在做**——它是 Phase 5（透明排序）与 Phase 7（culling 输出）的前置落点，越晚做集成面越大。

**执行状态（2026-08-24）**：批次 2 已落地（E3+E2+E4 + M2/L3 顺手；见 §处置记录）。
批次 1 部分落地：L3 已修；E5.2（Relationship 单向化）未做（便宜，可独立随时做）；
E5.3 过渡（MaterialRef 值拷贝问题）仅在 `scene.cppm` 留注释，改名未做。

**执行状态（2026-08-25）**：批次 3 已落地（E1.1–1.3 + E5.1 + E5.2 + E5.4 + L6；
见 §处置记录 2026-08-25 批次 3 节）。批次 1 剩余（E5.2）已随批次 3 顺手完成；
E5.3 过渡（MaterialRef 改名）仍未做（完整形态归批次 4）。

---

## 处置记录

### 2026-08-24 批次 2 落地（draw call 抽象 + system 化）

- ~~E2~~ **已修复**：`DrawCall`/`FrameDrawList`（`vkrenderer.cppm`，消费者定义输入契约）；
  `Renderer::drawFrame(const FrameDrawList&)` 单循环盲执行（pushDataEXT → bindShaders → drawMeshTasksEXT）；
  删除 `updateDrawArgs`/`updateModelDrawArgs`/`cubeDrawArgs`/`modelDrawArgs`/`findPipelineEntry`；
  新增 `pipelineIndex(name) → std::optional<uint32_t>`（name→index map 构造时建一次，录制路径零字符串查找）。
  cube 的 DrawCall 过渡期由 renderListSystem 特例生成（gating = "cube" 管线条目存在性，showCube 语义不变）。
- ~~E3~~ **已修复**：renderSystem 拆出 5 个每帧 system（camera/animation/bounds/projection/renderlist），
  显式 system 列表（注册顺序 = 数据流契约，新增 system = push_back）。
  **选型记录：未用 entt::scheduler**——本地 entt 3.16 的 scheduler 是 process 调度器
  （进程终止即移除、执行 "no specific order"，`process/scheduler.hpp:17-20,175`），每帧 system 的顺序契约无法保证。
  config→registry：main 启动期 emplace WindowExtent/AppExit/SceneSpin/SceneBounds/ViewProjection/FrameDrawList，
  renderSystem 签名去掉 SceneConfig 与 window。eventSystem（删未用的 window 参数）/resizeSystem（删未用的
  renderer 参数）/drawFrame 保留在帧循环体内直调——它们持有控制流（退出/resize 错误/绘制错误）。
- ~~E4.1~~ **已修复**：spin → `SceneSpin` context（axis/speed/angle/matrix，speed=0 静止）+ `animationSystem`
  （`angle += dt*speed`）——绝对时间 → dt 累积：切后台回来旋转不跳变（符合 dt clamp 原则，行为差异可接受）。
- ~~E4.2~~ **已修复**：`ModelBounds` context 删除；`Bounds` 组件（场景根挂模型整体球）+ `SceneBounds` context
  + `boundsSystem`（union 包围球聚合，标准公式）；`projectionSystem` 只读 SceneBounds。
  cube 入 ECS（E1.4）后挂自己的 Bounds 即自动纳入聚合，L2 消解。
- ~~E4.3~~ **已修复**：`Viewport` 拆分 → `WindowExtent`（数据）+ `AppExit`（退出请求）；
  删 needsResize 标志 → resizeSystem 比较 WindowExtent 与 swapchain 实际尺寸（派生状态不存标志位）。
- ~~M2~~ **已修复**（随批次 2 一并）：`model_schema.lua` 新增 `constants` 段（`kTrianglesPerGroup = 10`）→
  schema_loader/validator/generator/模板四件套（无反射端对应，同 vertexLayouts 模式）→
  `model_types_generated` 生成 `inline constexpr kTrianglesPerGroup`；`vkrenderer.cppm` 硬编码常量删除，
  `renderListSystem` 消费生成常量；`model.mesh.slang` 头注释契约指向改为 schema（source of truth）。
- ~~L3~~ **已修复**（随批次 2 顺手）：Renderer 删除 `registry` 构造参数/成员 + GMF entt include；Renderer 零 entt 依赖。

**验证**：`cmake --build`（工具链变更自动重建 + 重新生成，generated diff 只多常量）；
`just run-igpu` 双配置目视——`cube=false`：仅 4 个 glTF 盒（cube DrawCall 不产生，gating 生效）；
`cube=true`：彩色 cube + 4 盒同帧渲染。日志核对：slot 8-11、
bounds center=(0.1004, 0, -0.4999) radius=6.25 与 Phase 2 记录（phase2_plan §7.3）一致，无 warn/error。

**仍 Open**：
- E1（批次 3，建议与 Phase 3 TexturePool 并案）：资产表 + 生命周期收敛（含 L6）+ 构造参数去耦合（`primitiveCount`）
- E5.1（批次 3，与 E1 联动）：MeshRef 资产 ID 化；E5.4（批次 3）：输入事件化 + 键位配置化
- E5.2：Relationship 单向化（便宜，未随批次 2 做，可独立随时做）
- E1.4/H1/M1/L5 + E2 完整形态（cube 入 draw list 的完整路径）+ E5.3 完整形态（材质资产表）：批次 4 = Phase 4
- 耦合审查的 M3（两套 Lua 机制）/ L4（SPV 路径重复）：与 ECS 无关，未动

> 接缝已回写：`AGENTS.md` 模块结构（scene/scene_config/gltf_loader/model_importer/model_types_generated 行 +
> vk_renderer/ecs 行更新）、关键文件位置（drawFrame/DrawCall/renderListSystem/schema constants）、已知问题（ECS 已知债）、
> 其他文档（本文 + phase2_coupling_review + device_generated_commands_research + present_timing_research）。

### 2026-08-25 批次 3 落地（资产表 + 输入事件化）

- ~~E1.1~~ **已修复**：`AssetTable`（`vkresources.cppm`，**Resources 成员**——资产表持 GPU 资源，
  ECS 层不引入 Vulkan 依赖；registry 侧 `MeshRef{asset}` 只存资产 id，system 经
  `Resources&` 参数解析，同 renderListSystem 现有 renderer 参数模式）。
  `MeshAsset{VmaBuffer, heapSlot, triangleCount, pipelineIndex}` + 稳定 id（vector 下标，上传顺序分配）。
- ~~E1.2/L6~~ **已修复**：`ModelMeshAssets` 结构删除，buffer 所有权收敛进资产表
  （随 `SharedAllocator` 控制块 + Resources 存活）；`main.cpp` 局部变量与
  "须活到渲染结束"注释一并消失。
- ~~E1.3~~ **已修复**：`Resources`/`Renderer` 构造签名删除 `modelPrimitiveCount`；
  heap 容量 = `reserved + HeapSlotModelVertexBase + kUserSlotMargin(1024)` 固定裕量
  （`VmaDescriptorHeap` 不支持运行时扩容，统一 slot 分配器归 Phase 3 TexturePool）。
  `main.cpp` "先 CPU 解析模型再 Vulkan 初始化" 硬顺序约束删除（解析移到初始化之后）。
  模型管线条目从构造期移到资产上传期：`Renderer::addModelPipeline(vertexSlot, assetId)`
  （binding mapping 显式烘焙顶点 slot，条目下标写回资产记录——`renderListSystem`
  不再经 `"model_i"` 字符串查管线，E2.4 残余消解）。
- ~~E5.1~~ **已修复**：`MeshRef{primitiveIndex, triangleCount}` → `MeshRef{asset}`；
  `triangleCount` 移入资产表（资产属性，不是场景实例属性）。
- ~~E5.2~~ **已修复**：`Relationship` 删 `children`（单向 parent）；children 查询由
  `rg.view<Relationship>` 按 parent 过滤派生（现状无消费点，接缝留注释）。
- ~~E5.4~~ **已修复**：`CameraInput`（level-state 袋 + 累积器）删除 →
  `InputEvents`（context 内 `std::deque<InputEvent>`：KeyDown/KeyUp/MouseButton/MouseDelta/Wheel）
  + `CameraState`（cameraSystem 独占的 level 状态）。`eventSystem` 只产事件、不做键位映射；
  `cameraSystem` 消费即清空。键位 → `Camera.keys`（`KeyBindings`，`configs/camera.lua`
  的 `keys` 字段，sol2 沙箱 + 可选回落默认 WASD；未知键名报错退出）。
  行为差异：mouse delta 不再按"按下时"过滤产生（eventSystem 全产，cameraSystem 按
  自管 dragging 状态过滤应用）——帧内 click+motion 边缘情形语义等价。
- **slot 分配器（最小版，用户决策）**：slot 值仍 `reserved + 8 + i`（反射契约不变），
  但显式记录进资产表 + 管线 mapping 显式传 slot（隐式下标耦合消解）；
  统一 `allocSlot` 与 Phase 3 TexturePool 并案设计（资产表为宿主）。

**验证**：`just build` 通过；`just run-igpu` 双配置各 12s 无 error/warn
（`cube=false`：仅 4 盒，cube DrawCall 不产生；`cube=true`：彩色 cube + 4 盒）；
日志核对 slot 8-11 / asset 0-3、bounds center=(0.1004, 0, -0.4999) radius=6.25
与 Phase 2 记录一致。相机输入（WASD/拖拽/滚轮）交互回归待人工目视确认。

**接缝回写**：`AGENTS.md`（模块结构 vk_resources/vk_renderer/scene/camera/ecs/model_importer 行
+ main.cpp 描述 + 关键文件位置 addModelPipeline/renderListSystem/eventSystem + 已知问题）、
`phase2_coupling_review.md`（L6 标已修复 + 状态更新节）。

### 2026-08-28 重构批次 1-3 落地（纹理/材质/cube 资产化，Phase 3 收尾）

> 背景：Phase 3 纹理管理实现后（cottage 4K 模型渲染 PASS），发现纹理侧旁路了资产表
>（`TexturePool` main 局部对象——GPUVM fault 的 UAF 根因，heap_texture_research.md §12.6/§12.7）、
> 材质身份导入期丢弃（MaterialRef 值拷贝）、cube 非 entity。三批次纯结构重构，行为零变化。

- ~~纹理旁路~~ **已修复（重构批次 1）**：`TexturePool` 类删除 → 自由函数
  （`loadEmbedded`/`ensureWhiteTexture`/`uploadModelTextures`，`texture_pool.cppm`）；
  `TextureAsset{VmaImage, view, heapSlot, 描述符指针链}` 进 `AssetTable`（**deque 存储**——
  元素地址稳定 = 描述符指针链稳定存储单一介质，替代原 4 个平行 deque）；
  共享 sampler 归 `Resources::ensureSharedSampler()`（幂等，slot 归 Resources 成员）；
  白纹理 slot 契约断言（`reserved + HeapSlotModelVertexBase + 模型顶点槽数`，按 kind 计数）。
  **UAF 结构性消除**：纹理 BO/描述符存储随 Resources 存活，不再依赖 main 局部对象生命周期。
- ~~E5.3~~ **已修复（重构批次 2）**：`MaterialAsset{baseColor, textureId, alphaModeBlend}` 进
  资产表（第三容器 materials）+ `MaterialRef{material}` **真引用**（E5.3 解决）；
  `GltfPrimitive.materialIndex`（glTF materials 下标，-1 = 无材质 → 默认材质）——
  材质身份不再导入期丢弃；`registerScene` 按 materialIndex 去重注册 + **同 materialIndex
  字段一致性金丝雀**（baseColor/alphaMode 漂移 = loader bug，启动期报错不静默带病渲染；
  textureId 不校验——含 per-primitive hasUv 投影非材质级格式保证）。
  语义区分（MaterialAsset 注释钉死）：`textureId` = 材质**引用**的纹理；
  primitive **有效**纹理 = 有 uv 且图有数据 ? material.textureId : 白（选择留在上传返回值
  `primitiveTextureIds`，管线烘焙消费）。Phase 5 透明排序 / Phase 7 culling 的材质分类基础。
- ~~E1.4/H1/M1~~ **已修复（重构批次 3）**：cube 一等资产——`MeshAsset` 增可选
  `indexBuffer`/`indexHeapSlot` + `AssetTable::addIndexedMesh(kind=Cube)`；cube buffer
  构造体 move 进资产记录（资产 0，三角形数 = 12），四个直接访问器删除
  （访问经 `cubeAssetId()` → 资产表）；`scene.cppm` 新增 `registerCubeEntity`
  （Transform(identity) + Bounds(顶点半径派生) + MeshRef + MaterialRef + Relationship(根)）；
  main 按 `showCube` 决定是否注册——**画不画 = 实体有无（数据驱动），无构造期 gating**；
  `"cube"` 管线条目恒建（多建 2 个 shader object 成本可忽略）+ `bindPipelineToAsset`
  （启动期一次写回）；`renderListSystem` 删 cube 过渡态特例 + 每帧
  `pipelineIndex("cube"|"cube_tex")` 字符串查找（E2.2 面），统一 view 按资产 kind 分支；
  RGL_CUBE_TEX 诊断链整体删除（shader/CMake/ecs/main 四处 + cube_debug.frag.slang）。
- ~~L2~~ **消解（随 E1.4）**：cube 挂自己的 Bounds 组件，经 boundsSystem 自动纳入
  SceneBounds 聚合（frustum/near-far 自适应含 cube）。
- **仍 Open（归 Phase 4 ShaderRegistry）**：
  - push constant 布局未归一：`renderListSystem` 留 `AssetKind` 分支
    （Cube→DrawArgs 64B / Model→ModelDrawArgs 96B；H1 注记——每管线自己的 args 布局）
  - 管线表 per-primitive 条目（392 条）未去重：binding mapping 是 shader object 级状态，
    去重需按 (顶点 slot, 纹理 slot) 组合归并 + 资产表存条目引用（批次 3 刻意未做，行为零变化优先）

**验证（行为零变化，llm 1280×720 逐像素）**：三批次各自对基线 diff=0（cottage 4K 纹理 /
grass / transparent+cube / 无模型+cube 四路），启动零 warn/error；附加实锤——批次 3 前后
（master 态 vs cb3231e 同码重拍）截图 **diff=0 位级一致**，全部三批次渲染位级等价。
注：batch 0 保存基线有 1px 捕获瞬态离群（(683,279) 纹理缝边 0→38，cb3231e 同码重拍复现），
三基线已全部重拍替换（`build/vulkan_practice/screenshots/refactor_baseline_*.png`）。

**接缝回写**：`AGENTS.md`（模块结构 vk_resources/vk_renderer/scene/gltf_loader/model_importer
+ texture_pool 行 + main.cpp 描述 + 已知问题 ECS 债节）、`next_roadmap.md`（Phase 3 已完成
+ 计划-实现偏离注记 + Phase 4 前置债）、`phase3_plan.md`（状态行）、
`heap_texture_research.md`（§12.7 手段① 标已落地）。

---

## 下一步（2026-08-24 会话结论）

> **状态（2026-08-28）**：Phase 3 已落地 + 重构批次 1-3 收尾（§处置记录 2026-08-28 节）：
> 纹理/材质/cube 全部资产化（E1.4/E5.3/L2/M1 解决），剩余债（push constant 归一 + 管线去重）
> 归 **Phase 4（ShaderRegistry）**，预研结论见下（历史）。

**原计划（Phase 3 + 批次 3 并案）顺序**：

1. ~~**先批次 3**~~（**已完成 2026-08-25**）：资产表 + MeshRef 资产 ID 化 + 构造参数去耦合 + 输入事件化。
   **理由**：Phase 2 计划里的"运行时 slot 分配器"实际未落地（现状：cube 固定 slot 0/4 走 `types_generated`，
   model 固定 base offset 8 + i 走 `model_types_generated`，无统一分配器）；Phase 3 TexturePool 的 slot 分配
   需要统一分配器，资产表是它的天然宿主。
2. **再 Phase 3**：`TexturePool` 模块（`allocSlot(imageData)` + path→slot 缓存）+ 纹理契约
   （`types.slang` `TextureSampler` 绑定 + schema 扩展 + 生成端校验）+ model 纹理采样（`model.frag` 采 baseColorTexture）。
3. ~~E5.2（Relationship 单向化）小，可随批次 3 顺手。~~（**已完成 2026-08-25**，随批次 3）

**Phase 3 预研（2026-08-25 已做 RADV 侧，权威记录 `docs/heap_texture_research.md`）**：
- spec 条款已核实（v1.4.360 `chapters/descriptorheaps.adoc`）：heap 采样纹理**只能**
  `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`（VUID 11210 不含 combined image sampler），sampler 走
  `pEmbeddedSampler`（内嵌，替代 sampler heap）；单 resource heap 可混存 buffer+image 描述符。
- **spike 实测（`heap_texture_spike/`，RADV，离屏像素级验证）**：Q3/Q4 **PASS**（单 heap 混存 +
  内嵌 sampler 采样，像素精确匹配）；**Q1 关键约束**：`imageDescriptorSize=32B > bufferDescriptorSize=16B`，
  现有 slot 寻址（`slot * bufferDescriptorSize`）不能容纳 image 描述符 → **Phase 3 slot 步长改
  `max(bufferDescriptorSize, imageDescriptorSize)`（运行时查询，勿硬编码）**。NVIDIA 侧待 dGPU 机器补测。
- **v341 头（本机/主工程）与 v1.4.360 spec 的 API 差异**已记录（调研文档 §二）：buffer 描述符走
  `pAddressRange` 裸地址、写描述符 4 参、`pEmbeddedSampler` 在 `constantOffset` 内——写代码以系统头为准。
- tinygltf 内嵌纹理解码**当前是关的**：`TINYGLTF_NO_STB_IMAGE` 在 `gltf_loader.cppm:3` 和
  `tinygltf_impl.cpp:5` 各定义一处（glb 内嵌纹理需两处都删）。
- 测试模型 `transparent.glb` 无纹理——Phase 3 需换/补带 baseColorTexture 的测试模型。
- 设计决定点（已定，见调研文档 §六）：纹理与 buffer 描述符**同 heap**（Q3 PASS）；slot 统一步长 `max(buffer,image)`；
  一律 `pEmbeddedSampler`（不建 sampler heap）；mipmaps 后补；schema 扩展纹理绑定校验。
  Slang 变体（`Texture2D<float>`+`samplerState` = OpTypeImage+OpTypeSampler 对）spike 未覆盖（glslang 构造器限制），主工程接入时直接验证。

**未排期的便宜项**：M3（两套 Lua 机制命名区隔）、L4（默认 SPV 路径，现三处：`defaultSceneConfig` +
`scene.lua` + `makePipelineTable` 默认回落 static）、E5.2（若未随批次 3 做）。
