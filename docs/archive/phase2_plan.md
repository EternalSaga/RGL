# Phase 2 实施计划：glTF 加载

> **状态**：历史（Phase 2 已于 2026-08-23 完成，执行情况与计划外发现见 [next_roadmap.md](../next_roadmap.md) Phase 2 完成情况。本文仅作实施过程留档，行号引用已漂移。）

> 配套 [next_roadmap.md](../next_roadmap.md) 的 Phase 2 小节。本文是**可执行粒度**的实施计划，
> 含 2026-08-23 规划评审确认的决策与事实核查结论。

---

## 0. 目标

可以加载外部 3D 模型（glTF/glTF-Binary），替代**硬编码 cube 作为默认渲染内容**
（cube 与模型**并存**渲染）。落地 roadmap 的 Phase 2，并完成其前置的
**Shader/Renderer 解耦**（为 Phase 4 ShaderRegistry、Phase 7 G-buffer 多 pass 铺路）。

---

## 1. 已确认决策

| 项 | 决定 | 备注 |
|---|---|---|
| 网格绘制 | **无 task，按三角形分组** | 导入时去索引展开；CPU 用 `drawMeshTasksEXT(ceil(tri/TRI_PER_GROUP),1,1)` 派生 N 个 mesh group；保留 `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT`，规避 `git:min_mesh_crash_已删除.md`（原 `min_mesh_crash/` 子项目）记录的 NVIDIA task 坑 |
| 顶点属性 | **position + normal + uv**，**走反射契约** | 本阶段**增强反射工具**使其支持顶点布局（现状只反射 push constant + slot，顶点结构体手写在 `cube.cppm`） |
| 着色 | **flat unlit baseColor** | 每 primitive 输出其材质 baseColor；alpha 通道忽略（无混合/透明排序，那是 Phase 5） |
| cube | **并存**（cube + 模型同帧渲染） | 需要**多契约**反射（cube 契约 + model 契约各一套 `.slang`/schema/`.cppm`） |
| 测试模型 | **`assets/transparent.glb`** | 自建模：4 个方块（2 透明 2 不透明）= 多 node/多 primitive/多材质，几何简单好核对 |
| 驱动验证 | **以 AMD/RADV 为准** | 本机仅 Strix Halo 的 Radeon 8060S iGPU；验证走 `just run-igpu`（`RADV_EXPERIMENTAL=heap`） |

### transparent.glb 的 Phase 2 预期
4 个方块全部以 flat unlit baseColor 渲染；**2 个"透明"方块显示为实色方块**
（Phase 2 无 alpha 混合），属预期，不影响对几何/层级/多 primitive/材质引用的验证。

---

## 2. 事实核查结论（2026-08-23）

1. **测试模型是 git-lfs 指针，本地无真实数据**：`assets/*.glb` 均为 130 字节 lfs 指针。
   `git-lfs 3.7.1` 已安装 → 开工第一步 `git lfs pull --include=assets/transparent.glb`。
2. **parent-child 是 legacy 自写组件**（`legacy/render_core/gl/EnTTRelationship.hpp:41`
   的 `Relationship{entity parent; vector<entity> children;}`），**非 entt 内置**
   （本机 entt 3.16 无 `relationships` 头）。新工程照此定义。
3. **动态状态 / push data 在 draw 时采样**（Vulkan spec `chapters/shaders.adoc`
   "Setting State"：相关 set 命令只需在 draw 之前调用即可）。现状 bind→setState→draw
   已验证 → **解耦时状态设置与 bindShaders 的先后无约束**，只要都在 `drawMeshTasksEXT` 前。
4. **tinygltf 可用**：vcpkg port 存在（header-only，依赖 nlohmann_json 已在依赖中），
   加进 `vcpkg.json` 即可，CMake 链接 `tinygltf::tinygltf`。
5. **slangc 实际用的是 vcpkg 版**（重要纠正）：
   - `vulkan_practice/CMakeLists.txt:1-6` 硬编码 `SLANG_INSTALL_DIR=$HOME/program/slang/build/Release`
     是**死代码**——vcpkg toolchain 开 `VCPKG_SETUP_CMAKE_PROGRAM_PATH=ON`，
     `find_program` 搜 `CMAKE_PROGRAM_PATH` **优先于显式 `PATHS`**，
     故 CMakeCache 解析到 `build/vcpkg_installed/x64-linux-native/tools/shader-slang/slangc`。
   - 本地 `$HOME/program/slang/build/Release/bin/slangc`（6-18 旧 build）**未被使用**。
   - vcpkg 版 slangc（port 2026.7.1）已确认支持构建用到的全部 flag：
     `-reflection-json` / `-no-codegen` / `-fvk-use-scalar-layout` / `-fvk-use-c-layout` /
     `-capability` / `-profile` / `-I`。
   - **修正**：删掉误导性硬编码，改用干净的 `find_program(slangc)`（经 `CMAKE_PROGRAM_PATH`
     自然解析到 vcpkg）；FATAL_ERROR 提示保留（其"检查 shader-slang"恰好属实）。
6. **现有反射 JSON 只含入口参数**：`build/.../generated_shaders/types_reflection.json`
   顶层仅 `parameters`（push constant + descriptor slot）+ `bindlessSpaceIndex`。
   顶点结构体 `Vertex`/`VertexOutput` **不在其中**（非入口参数）。
   且项目禁用 `StructuredBuffer`（见 [技术概念.md](技术概念.md)），未用 push constant 有验证层风险
   → 顶点布局契约机制需在 Step 0 spike 中定夺（见 §3 Step 0）。
7. **transparent.glb 实际结构**（`git lfs pull` 后解析，2026-08-23）：
   - 4 node **无层级**（全根节点），仅 translation（无 rotation/scale）→ world = local。
   - 4 mesh × 1 primitive，每 primitive **12 三角形 / 24 顶点 / 带索引**（mode=TRIANGLES）。
   - 顶点属性 **POSITION(VEC3) + NORMAL(VEC3) + TEXCOORD_0(VEC2)**，与 position+normal+uv 布局**完全一致**。
   - 4 材质：2 OPAQUE（蓝 `[0.0585,0,0.8]`/红 `[0.8,0.007,0.0386]`）+ 2 BLEND（黄/青，alpha 0.25），
     **均无纹理**，只有 baseColorFactor → flat unlit baseColor 完美适用。
   - 去索引后每 primitive 36 顶点、共 144 顶点（极小）；`TRI_PER_GROUP=10` → 每 primitive 2 个 mesh group。
   - 无 sparse accessor、无纹理、无动画 → loader 只需处理 translation + 索引展开 + baseColor。

---

## 3. 分步实施

### Step 0 — Spike + 清理（先摸清，再动手）

**0a. 修正 CMake 硬编码 slang 路径**
- `vulkan_practice/CMakeLists.txt`：删除 `SLANG_INSTALL_DIR` + 其 `PATHS/HINTS`，
  改为 `find_program(SLANGC_EXECUTABLE slangc REQUIRED)`（vcpkg 经 `CMAKE_PROGRAM_PATH` 提供）。
- 改后 `just config` 重配，确认 CMakeCache 仍解析到 vcpkg slangc。

**0b. 拉取并解析测试模型**
- `git lfs pull --include=assets/transparent.glb`。
- 解析其结构并记录：node 层级、mesh/primitive 数、顶点属性（position/normal/uv 是否齐全）、
  是否带索引、material（baseColorFactor、alphaMode 是否 BLEND）、每个 primitive 的顶点/三角形数。
- 若某属性缺失（如无 uv/normal），loader 按 schema 回退（见 §3 Step 3 的 loader 约定）。

**0c. 顶点布局反射机制 spike（定契约机制）— 已完成，结论：schema 声明（V1）**
- 用 **vcpkg slangc**（`build/vcpkg_installed/.../tools/shader-slang/slangc`）
  编译含顶点结构体的测试 shader（`-no-codegen -reflection-json`）实测：
  - **顶点 buffer 结构体 `ModelVertex` 不被反射**（它只用于 ByteAddressBuffer 手动读，非入口参数）。
  - push constant（`ModelDrawArgs`）被反射：`elementVarLayout.binding.size=96` + 字段**类型**，
    但 `fields[].offset=None`（**无逐字段字节偏移**）。
  - varying 结构体（`ModelVertexOutput`）被反射：带语义（SV_POSITION/NORMAL/TEXCOORD/COLOR）但**无字节偏移**。
  - 且项目禁用 `StructuredBuffer`（否则可反射元素布局）、未用 push constant 有验证层风险。
- **结论**：顶点布局契约以 **schema 声明为 source of truth**（`vertexLayout` 段显式列
  属性 + 偏移 + stride），工具据此生成 C++ 结构体 + `static_assert(sizeof==stride)`；
  shader 侧手动偏移与 schema 对齐（生成的 offset 常量作参照）。
- 反射工具增强以支持 `vertexLayout` 契约段并生成顶点结构体（见 Step 2）。

### Step 1 — Part A：Shader/Renderer 解耦（roadmap 前置，先做，风险低）

**A1 动态状态外移**（`shader.cppm`）
- 新增 `export struct RenderState`：polygonMode / cullMode / frontFace / rasterizerDiscard /
  depthTest / depthWrite / depthCompareOp / depthBoundsTest / stencilTest /
  rasterizationSamples / alphaToCoverage / alphaToOne / colorBlend / colorWriteMask / logicOp。
- `RenderPipeline::recordRenderCommands(cmd,w,h)` 拆为：
  - `bindShaders(cmd)` —— 只做 `bindShadersEXT`（纯 shader object 包装）；
  - 自由函数 `applyDynamicState(cmd,w,h,const RenderState&)` —— 设置全部动态状态。
- 顺序无约束（spec 确认 draw 时采样），保持现状 bind→state 亦可。

**A2 管线表**（`vkrenderer.cppm`）
- `cubePipeline` 单成员 → `std::vector<RenderPipeline> pipelines`
  （Phase 2 按条目索引；Phase 4 再升级为 name→pipeline 的 ShaderRegistry）。
- SPV 路径不再写死 `createRenderPipeline`，改由 `configs/scene.lua` 传入
  （sol2 沙箱加载，沿用 Phase 1 确立的 `configs/*.lua` 模式；字段可选回落默认值）。
- Renderer 持有默认 `opaqueState`（当前 cube 的 opaque pass 值），`beginRendering` 后调一次
  `applyDynamicState`，per-draw 仅重设 push data + bindShaders —— 为 Phase 7 多 pass 铺路。

### Step 2 — Part B0：增强反射工具（顶点布局契约 + 多契约）

现状：反射只生成 push constant（DrawArgs）+ slot 常量；顶点结构体手写在 `cube.cppm`。

- **schema 增加 `vertexLayout` 段**（`tools/schema_loader.cppm`）：
  `SchemaVertexAttr{name, type, offset}` + `SchemaVertexLayout{name, stride, attrs}`，`loadSchema` 解析。
- **validator 校验**（`tools/validator.cppm`）：attrs 偏移连续、`stride == 末字段 offset+size`、
  类型合法；（若 0c 确认可反射）交叉校验 schema 偏移 vs 反射偏移。
- **generator + 模板生成顶点结构体**（`tools/generator.cppm` + `tools/templates/`）：
  输出 `struct ModelVertex{ glm::vec3 position; glm::vec3 normal; glm::vec2 uv; }` +
  `static_assert(sizeof(ModelVertex) == stride)` + 每属性 offset `constexpr`。
- **多契约**：reflection_tool 已参数化（`--json/--schema/--output/--module-name/--template`），
  CMake 加第二条管线即可 → `generated/model_types_generated.cppm`（module `model_types_generated`）。
- **schema 落地**：
  - `shaders/model_schema.lua`：ModelVertex + ModelDrawArgs{mvp, totalTriangles, baseColor} + vertex slot。
  - **顺带**把 cube 的 `shaders/types_schema.lua` 也补上 vertexLayout（生成 cube 的 `Vertex` 结构体，
    替换 `cube.cppm` 手写版）——**放在模型跑通之后**做，保 known-good 基线。
- **机制取舍**：以 **schema 声明为 source of truth**（稳健、可控、无未用 push constant 风险）；
  若 0c 证明 slangc 能干净反射结构体，则加"反射交叉校验"作第二道安全网。

### Step 3 — Part B：glTF 加载 + 渲染

**依赖**：`vcpkg.json` 加 `tinygltf`；CMake 链接 `tinygltf::tinygltf`。

**`gltf_loader.cppm`（纯 CPU，无 Vulkan/entt）**
- `loadGltf(path) → expected<GltfModel, Error>`。
- `GltfModel` =
  - nodes（local 矩阵 + **导入时算好的 world 矩阵** + children 下标）；
  - primitives（**去索引展开**后的顶点数据 position+normal+uv + 三角形数 + materialIndex）；
  - materials（baseColorFactor 等，**仅存引用**，完整材质系统 Phase 4）。
- 去索引展开：每个三角形复制其 3 个顶点（position+normal+uv），保证每 mesh group 自包含
  （规避跨 group 索引引用问题）；顶点重复为已知代价（大模型 2–3x 显存），记为后续优化。
- 属性缺失回退：无 normal → 平面法线；无 uv → (0,0)。

**`model_importer.cppm`（GPU + ECS）**
- `class MeshAssets`（RAII，持 `VmaBuffer`）：每 primitive 建 **1 个顶点 VmaBuffer**、
  分配 **1 个 heap slot**、批量 `vkWriteResourceDescriptorsEXT`。
- `registerScene(registry, const GltfModel&, const MeshAssets&)`：建 entity（见下）。

**ECS 组件**
- `Transform{ glm::mat4 local; glm::mat4 world; }`（world 导入时按层级烘焙）。
- `Relationship{ entt::entity parent; std::vector<entt::entity> children; }`（照 legacy）。
- `MeshRef{ uint32_t primitiveIndex; }`、`MaterialRef{ uint32_t materialIndex; }`（暂只存引用）。
- 每 node 1 entity（Transform + Relationship）；每 primitive 1 entity
  （Transform(world 烘焙) + MeshRef + MaterialRef + Relationship→所属 node）。
- 可渲染集 = 带 `MeshRef` 的 entity。

**shader（`shaders/`）**
- `model.slang`：`ModelVertex` + `ModelDrawArgs{mat4 mvp; uint totalTriangles; float4 baseColor;}` +
  `[[vk::binding(0,0)]] ByteAddressBuffer vertexBuffer`（**只用 ByteAddressBuffer**，手动字节偏移）。
- `model.mesh.slang`：`numthreads(TRI_PER_GROUP,1,1)`，线程 = 1 三角形；
  读 3 个展开顶点、`SV_GroupID*TRI_PER_GROUP + SV_GroupIndex` 定位全局三角形、
  `if(triIndex < drawArgs.totalTriangles)` 收尾（末组不足时丢弃）；
  `TRI_PER_GROUP` 取保守值 ≤ `maxMeshOutputVertices/3`（spec 最小 32 → 取 10，即 30 顶点/组）。
- `model.frag.slang`：flat 输出 `drawArgs.baseColor`（unlit）。
- **保留** `NO_TASK_SHADER_BIT_EXT`（无 task stage）。

**Resources 扩容**（`vkresources.cppm`）
- `capacity = max(16, N + margin) + reservedSlots` 按 primitive 数 N 动态（N 在模型 CPU 解析后已知，
  解析先于 Vulkan 初始化，故 Resources 构造时可得）。
- 加运行时 slot 分配器（`nextSlot` 从 `reservedSlots + baseOffset` 递增，越界检查）。
- 构造签名加 mesh slot 数参数。

**renderSystem 改造**（`ecs.cppm`）
- 从"单一 cube + 时间旋转"改为：查 registry 中带 `MeshRef` 的 entity →
  逐个 `mvp = proj * view * sceneRot * world` → 循环内
  `pushDataEXT(ModelDrawArgs)` + `pipelines[i].bindShaders` + `drawMeshTasksEXT(ceil(tri/TRI_PER_GROUP),1,1)`。
- **cube 并存**：cube 走原 pipeline 同帧渲染（其 `DrawArgs{mvp}` 单独 push）。
- `sceneRot` 保留现状的时间旋转（作用于模型整体，便于肉眼判断渲染对错）。

**resizeSystem**
- 顶点 buffer 与窗口尺寸无关，**无需重传**（仅深度图重建，现状已满足）→ 该项为 no-op，确认即可。

### Step 4 — Part C：构建 / 配置

- `vcpkg.json`：加 `tinygltf`。
- CMake（`vulkan_practice/CMakeLists.txt`）：
  - `compile_slang` 编译 `model.mesh.slang` / `model.frag.slang`；
  - 第二条反射管线（`model.slang` + `model_schema.lua` → `model_types_generated.cppm`）；
  - 新模块（`gltf_loader.cppm` / `model_importer.cppm`）进 `FILE_SET CXX_MODULES`；
  - 链接 `tinygltf::tinygltf`。
- 新增 `models/`（放 transparent.glb，git-lfs）+ CMake 拷贝到 build（同 `configs/` 的
  `copy_if_different` 增量拷贝）。
- 新增 `configs/scene.lua`：`model` 路径 + `pipelines`（SPV 列表）+ 可选 `spin`，
  sol2 沙箱加载（同 Phase 1），字段可选回落默认值。

### Step 5 — 验证（以 AMD/RADV 为准）

- `just build-release` 编译通过（`static_assert` 生效，C++ 结构体与 shader 布局一致）。
- `just run-igpu`（`RADV_EXPERIMENTAL=heap`）运行，肉眼核对 **4 个方块**
  （2 实色 + 2 实色，无透明；几何/层级/多 primitive 正确）。
- 可选：Vulkan 验证层过一遍（heap/slot 对齐、push data 范围、mesh 输出计数）。
- NVIDIA 路径本机无硬件，留待有 dGPU 环境时补验（`just run-dgpu`）。

---

## 4. 文件变更清单

**新增**
- `vulkan_practice/gltf_loader.cppm`
- `vulkan_practice/model_importer.cppm`
- `vulkan_practice/shaders/model.slang`
- `vulkan_practice/shaders/model.mesh.slang`
- `vulkan_practice/shaders/model.frag.slang`
- `vulkan_practice/shaders/model_schema.lua`
- `vulkan_practice/generated/model_types_generated.cppm`（auto-generated）
- `vulkan_practice/configs/scene.lua`
- `vulkan_practice/models/transparent.glb`（git-lfs，或沿用 `assets/`）

**修改**
- `tools/schema_loader.cppm` / `tools/validator.cppm` / `tools/generator.cppm` /
  `tools/templates/*`（vertexLayout 契约 + 顶点结构体生成 + 多契约）
- `vulkan_practice/shader.cppm`（RenderState + bindShaders/applyDynamicState 拆分）
- `vulkan_practice/vkrenderer.cppm`（管线表 + 多 entity 渲染 + SPV 来自 config + opaqueState）
- `vulkan_practice/vkresources.cppm`（heap 扩容 + 运行时 slot 分配器 + 构造签名）
- `vulkan_practice/ecs.cppm`（renderSystem 多 entity 循环）
- `vulkan_practice/cube.cppm`（`Vertex` 改为反射生成，**可选**，模型跑通后做）
- `vulkan_practice/shaders/types_schema.lua`（补 vertexLayout，**可选**，同上）
- `vulkan_practice/CMakeLists.txt`（slang 路径修正 + 新 shader/反射管线/模块/tinygltf/models 拷贝）
- `vcpkg.json`（加 `tinygltf`）

---

## 5. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 顶点布局反射机制未定 | Step 0c spike 先行定夺；默认以 schema 声明为准（稳健），反射交叉校验为加分项 |
| 去索引展开显存膨胀 | Phase 2 可接受；记为后续优化（后续可改 indexed + 空间分块） |
| transparent.glb 实际结构未知（lfs 指针） | Step 0b 拉取后按真实属性/层级适配 loader；属性缺失按 schema 回退 |
| cube 并存使多契约反射范围变大 | cube 的 vertexLayout retrofit 放在模型跑通之后，保 known-good 基线 |
| 仅 AMD iGPU 可验证 | 以 RADV 为准（`run-igpu`）；NVIDIA 路径留待有 dGPU 环境补验 |
| 未用 push constant 触发验证层 | 顶点布局不走未用 push constant（见 §2.6 / Step 0c 决策树） |

---

## 6. 与后续 Phase 的接缝

- **Phase 4（多 Shader）**：管线表（Step 1 A2）是 ShaderRegistry 的接缝；多契约反射（Step 2）
  是"每种 shader 风格独立 schema + 生成模块"的机制基础。
- **Phase 5（透明 + 光照）**：`MaterialRef` 已存引用；normal 已传入（Step 3 顶点布局含 normal），
  加 Lambert 光照与透明排序时无需重做导入/契约。
- **Phase 7（延迟渲染 + GPU culling）**：`applyDynamicState` 外移（Step 1 A1）支持同一 mesh shader
  不同状态的多 pass；per-primitive 的 `DrawArgs` + heap slot 是 culling compute 输出的基础。

---

## 7. 执行记录（2026-08-23 完成）

### 7.1 落地内容
- **Step 0**：lfs pull `assets/transparent.glb`（7016B，4 node 无层级仅 translation，
  4 mesh × 1 primitive，每 primitive 12 三角形/24 顶点/带索引，POSITION+NORMAL+TEXCOORD_0，
  4 材质 2 OPAQUE + 2 BLEND 无纹理仅 baseColorFactor）。slangc spike 结论：
  **顶点 buffer 结构体不反射** → 顶点契约以 **schema 声明为 source of truth**。
- **Step 1 解耦**：`shader.cppm` 拆出 `RenderState` + 自由函数 `applyDynamicState(cmd,w,h,state)`
  （viewport/scissor 走宽高参数）+ `RenderPipeline::bindShaders(cmd)`；
  `vkrenderer.cppm` 改为**管线表**（`PipelineEntry{name, RenderPipeline}`，初始化列表经
  `makePipelineTable` 构建），SPV 路径来自 `configs/scene.lua`（新 `scene_config.cppm`，
  sol2 沙箱 + 缺失回落默认，同 camera 模式）；`opaqueState` 成员（Phase 7 多 pass 接缝）。
- **Step 2 反射增强**：`tools/schema_loader.cppm` 加 `vertexLayouts`（name/stride/attributes
  {name,type,offset}）+ `vertexAttrSize/vertexAttrCppType`（packed C++/glm 约定）；
  validator 校验内部一致性（类型已知 + 偏移 packed 连续 + stride = 尺寸和）；
  generator 直接由 schema 生成顶点结构体（packed，模板 static_assert sizeof==stride 兜底）。
  新增第二条反射管线（CMake，CLI 已参数化）：`model.slang` + `model_schema.lua`
  → `generated/model_types_generated.cppm`（`ModelDrawArgs` 96B / `ModelVertex` 32B /
  `HeapSlotModelVertexBase=8`）。
- **Step 3 glTF**：`gltf_loader.cppm`（tinygltf v3 C++ API，CPU 解析 + 去索引展开 +
  node translation / baseColorFactor / alphaMode 提取，逐 accessor 严格校验）；
  `model_importer.cppm`（每 primitive 一个 BDA storage buffer + 描述符写 heap slot
  `reserved+8+i`；`registerScene` 建场景根 + per-primitive entity）；
  `scene.cppm`（`Transform`/`Relationship`（legacy 同款）/`MeshRef{primitiveIndex,triangleCount}`/
  `MaterialRef{baseColor}`，独立模块避免 ecs↔importer 循环 import）；
  `model.mesh.slang`（无 task，`SV_GroupID.x` 派生 group，每 group 10 三角形/30 顶点，
  显式字节偏移读 packed 顶点流）+ `model.frag.slang`（flat unlit baseColor，alpha=1）。
- **Step 4 渲染**：`vkresources.cppm` heap 容量 = `reserved + 8 + N`（构造加
  `modelPrimitiveCount`）；`vkrenderer.cppm` 加 `model_i` 管线条目（每 primitive 一个
  shader object，binding mapping 各自烘焙）+ `modelDrawArgs` 向量 +
  `drawMeshTasksEXT(ceil(tri/10),1,1)`；`ecs.cppm` renderSystem 遍历
  `view<Transform,MeshRef,MaterialRef>` 推 per-entity MVP（含场景 spin，cube 共用）。
  `main.cpp`：模型 CPU 解析**先于** Vulkan 初始化（N 决定 heap 容量）→ 上传 → 注册 → 渲染。
  CMake：新 3 模块 + 2 shader + models/ 拷贝（`assets/transparent.glb` → build `models/`）。

### 7.2 计划外发现与处理（重要，后续复用）
1. **vcpkg tinygltf port 哈希过期**：baseline 旧（`vcpkg-configuration.json` 钉在 b81cab76），
   portfile 的 SHA512 与 GitHub 当前 v3.0.0 tag tarball 不符（GitHub tag tarball 再生，
   网络无关——换节点重试无效）。修复：baseline 指到最小修复提交
   `2bc124da`（"[tinygltf] Fix out-dated hash"，2026-08-05）。
   **坑**：vcpkg 安装状态损坏后，光删 installed 文件/manifest-install.log 无效——
   真正状态在 `build/vcpkg_installed/vcpkg/status`（段落库，删段要整段删，删残会
   "missing required field 'Package'"）+ `manifest-info.json`。
2. **tinygltf v3 不是 header-only**：实现体移入 `TINYGLTF_IMPLEMENTATION` 宏守卫，
   port 只装 3 个头（`tiny_gltf.h`/`tiny_gltf_v3.h`/`tinygltf_json.h`），官方
   `tiny_gltf.cc` 仅 4 行（= 宏 + include 头）。项目按 `vam_impl.cpp` 同款模式加
   `vulkan_practice/tinygltf_impl.cpp`（`TINYGLTF_NO_STB_IMAGE` + `TINYGLTF_IMPLEMENTATION`）。
   v3 API 变更：类名 `Loader`→`TinyGLTF`；`Primitive::attributes/indices` 是
   accessor 索引（map<string,int> / int）；`BufferView` 无 data 成员
   （数据在 `Model::buffers[bv.buffer].data` + byteStride 语义）。
3. **push constant 标准布局 padding**：Slang 按 GLSL 42.6 标准布局（float4 对齐 16），
   glm 默认自然对齐（vec4/mat4 对齐 4）——`ModelDrawArgs` C++ 侧 84B vs 反射 96B。
   生成器现按标准布局计算偏移并插入 `uint8_t _padN[M]` 字段（对齐 1，任意字节数，
   不引入额外 padding），`standardLayoutInfo()` 表驱动（size/align 查询），
   并校验计算总尺寸 == 反射 totalSize。**所有 push constant 结构体都经此路径**，
   后续新契约自动受益。
4. **构建内存**：`tinygltf_impl.cpp` 单 TU 编译 8756 行实现，-O2 下 clang 慢且吃数 GB；
   24 并发（`just build` = `--parallel $(nproc)`）把 15GB 内存 + swap 打爆（实测 7.4GB swap，
   构建十几分钟）。该 TU 是启动期一次性路径（模型加载），CMake 对其单独 `-O0`。
   全量重建（clean-all）仍偏重，验证构建建议 `-j 4` 量级。

### 7.3 验证状态（AMD iGPU，`just run-igpu`）
- 日志全绿：`glTF loaded (models/transparent.glb): 4 primitives, 144 vertices (de-indexed)`；
  `model primitive 0..3: 36 vertices (1152B) → heap slot 8..11`（AMD reserved=0，
  基址 8 + i；cube 占 0/4 无冲突）；无 warn/error，渲染循环 15s 无 device lost。
- **待人工目视确认**：窗口内 4 个方块（2 实色蓝/红 + 2 实色黄/青——"透明"的
  2 个按 Phase 2 决策渲染为实色）+ cube 并存，整体绕 (0.5,1,0) 轴时间旋转。
