# Phase 4 实施计划：Shader 反射扩展 + 多 Shader 支持

> **状态**：执行中（2026-08-28 规划；批次 1 完成——统一 push constant 契约 + scalar 布局
> 定案 D0 + 管线表 argsSize/pushDataSet 显式化 + renderListSystem 归一，cottage/cube 双场景
> 截图对基线零差异）。配套 [next_roadmap.md](next_roadmap.md) 的 Phase 4 小节。
> 可执行粒度实施计划，含已确认决策、事实核查、分步执行、文件变更清单与风险。
>
> 前置债来源：Phase 3 完成后遗留两项（next_roadmap.md Phase 4 状态注记）——
> ① 管线表 per-primitive 条目（392 条）去重；② `renderListSystem` AssetKind 分支归一
> （push constant 布局 64B/96B 不统一，H1 注记 + L5 set-0 隐式约定，
> 权威记录见 [ecs_compliance_review.md](ecs_compliance_review.md)）。本计划是两者的统一落点。

---

## 0. 目标

支持多种 shader 风格（unlit、pbr），材质字段（baseColor/roughness/metallic）进统一 push constant
契约，渲染时按材质风格选择管线；同时清偿两项前置债（管线去重 + push constant 归一）。

**验收**：
- cottage（392 primitive / 11 张 4K 纹理）管线表条目 **392 → 12**（= 不同纹理 slot 数），
  heap 顶点 slot 392 → 1；
- `renderListSystem` 零 AssetKind 分支（单一统一 args 写入路径）；
- pbr 风格管线真实存在并被引用（占位 frag 输出与 unlit 一致，**视觉零变化**）；
- `just build` 全绿 + llm_debug 截图与改前一致 + 交互运行无 fault。

**不做（延后/明确排除）**：
- 光照 / DirectionalLight / 透明排序（Phase 5；pbr 本期只做占位 frag）
- imgui（roadmap 自注"Phase 4 之后考虑加入"）
- RenderGraph（Phase 6）
- 反射工具链缺口加固（bool/uint64 布局表、跨 shader 契约校验、生成失配改致命）——本期字段用不到
- cube `Vertex` 改反射契约（Phase 2 延后项，low priority）
- 多模型同时加载的回归测试（设计兼容，现 config 单模型）

## 1. 事实核查（2026-08-28，子 agent 调研 + 人工复核）

### 1.1 管线表 392 条的根因

- `PipelineEntry{name, RenderPipeline}`（vkrenderer.cppm:148-151）：只存名字 + 管线对象，
  **无 args 布局、无 stage 描述、无 binding 元数据、无 per-pipeline 动态状态**。
  注释原文："Phase 2 按条目索引；Phase 4 升级为 name→pipeline 的 ShaderRegistry"（:147）。
- binding mapping 在 `VkShaderEXT` **创建时**经 `pNext` 链烘焙（shader.cppm:114-188，
  `VkShaderDescriptorSetAndBindingMappingInfoEXT`），运行期不可换。
  `addModelPipeline`（vkrenderer.cppm:125-138）每 primitive 一次：
  顶点 slot（`reserved+2+i`）+ 纹理 slot + sampler 字节偏移 三个运行时参数各烘焙一份。
- **去重障碍（本计划核心设计问题）**：顶点 slot 每 primitive 不同（`reserved+2+i`，
  model_importer.cppm:62），所以"按 (顶点 slot, 纹理 slot) 组合归并"在**当前布局下去不动**——
  必须把顶点 buffer 引用移出 binding mapping（每模型一个拼接大 buffer + per-draw 字节偏移
  进 push constant），条目才可按 (模型顶点 slot, 纹理 slot) 组合归并。

### 1.2 AssetKind 分支与 set-0 约定

- 分支点：ecs.cppm:220 `if (asset.kind == AssetKind::Cube)`——按资产表 `AssetKind` 分流：
  - cube → `state.cubeArgs`（`DrawArgs` 64B，**单例共享**，ecs.cppm:191），groupCount 恒 1
  - model → `state.modelArgs[assetId]`（`ModelDrawArgs` 96B，**每资产一份**，ecs.cppm:192），
    groupCount = ceil(triangleCount / kTrianglesPerGroup)（ecs.cppm:233-234）
- `DrawCall.pushData` 是类型擦除 `std::span<const std::byte>`（vkrenderer.cppm:54），
  `drawFrame` 盲执行（:561-567），`pushDataEXT(0, ...)` 的 set 0 被 64B/96B 两种 struct
  隐式共享（L5，phase2_coupling_review.md:178）。
- **cube 不能套统一 group 公式**：cube mesh shader 固定输出 8 顶点/12 三角形
  （cube.mesh.slang:11-26，`if (ti < 12)` 分支不消费 totalTriangles/remaining），
  groupCount=ceil(12/10)=2 时第 2 个 group 会重复读 12 个三角形索引 → 越界/重复输出。
  ⇒ group 数必须 per-资产静态化（上传期按各 shader 契约算好），不能 per-frame 统一推导。

### 1.3 反射工具链能力（tools/ 在仓库根目录，CLI = `reflection_tool.cpp`）

- **多契约已是 CMake 层机制**：CLI 参数化（`--module-name`/`--template`），两条反射管线并存
  （vulkan_practice/CMakeLists.txt:33-104），:68 注释"多契约靠 CMake 增加调用"。
  新契约接入成本 = 一对 `add_custom_command` + `add_custom_target` + FILE_SET 加行 + dependencies。
- **`generateCppm` 是纯函数**（generator.cppm:92）：任何 `[[vk::push_constant]]` struct 自动获得
  C++ struct + GLSL 4.2.6 标准布局 padding + `static_assert(sizeof == 反射尺寸)`。
  struct 名取 `type.elementType.name`，总尺寸取 `elementVarLayout.binding.size`（:125-134）。
- **类型映射覆盖本期全部字段**：matrix→`glm::mat4`、uint→`uint32_t`、vec2→`glm::vec2`、
  vec4→`glm::vec4` 均在 `toCppType`（:60-90）+ `standardLayoutInfo`（:21-58）表内。
  **工具链代码零改动**。
- **schema 声明段模式**（vertexLayouts/constants，无反射端对应，schema 单一真相源）可复用。
- 反射 JSON 格式（build 目录 `*_reflection.json` 实测）：push constant 参数
  `binding.kind="pushConstantBuffer"`，字段偏移 `field.binding.offset`、尺寸/对齐
  `field.type.sizes[0]`；struct 总尺寸两处（`elementType.sizes[0].value` 与
  `elementVarLayout.binding.size`），generator 取后者。
- 已知缺口（本期不触碰）：bool 字段被静默跳过（布局表无 bool）、无跨 shader 契约校验、
  布局失配 `[FAIL]` 非致命（文件照写，靠 static_assert 兜底）、schema loader 字段拼错静默落默认值。

### 1.4 材质侧现状

- `MaterialAsset{baseColor, textureId, alphaModeBlend}` 资产表 + `MaterialRef{material}` 真引用
  （重构批次 2 已落地，比 roadmap"Phase 4 才做材质系统"早）——本期在其上扩展字段。
- glTF loader（gltf_loader.cppm）**未读** `pbrMetallicRoughness.metallicFactor/roughnessFactor`
  与 `KHR_materials_unlit` 扩展；per-primitive 平铺材质字段 + `registerScene` 去重注册
  （materialIndex → MaterialAsset）+ 同 materialIndex 字段一致性金丝雀（违例 = loader bug
  启动期报错，model_importer.cppm:137-146）——新字段沿用同一模式。
- 现有模型渲染 = flat unlit（model.frag.slang:10-11 `baseColor.rgb × texel.rgb`，alpha 忽略）。

### 1.5 slot 布局契约（本期改动波及面）

现状（vkresources.cppm:141-149 注释）：`[0,2) cube 顶点/索引` / `[2, 2+N) 模型顶点 per-primitive`
/ `[2+N, …) 纹理（首槽 = 白）` / 裕量 `kUserSlotMargin(1024)`。三处契约断言：
- Resources 构造期：cube 顶点/索引 slot == reserved+0/1（vkresources.cppm:176-183）
- model_importer：per-primitive slot == reserved + HeapSlotModelVertexBase(2) + i（model_importer.cppm:62-69）
- texture_pool 白纹理：slot == reserved + 2 + modelMeshCount（texture_pool.cppm:86-91）

## 2. 已定决策（2026-08-28 用户确认）

| # | 决策 | 说明 |
|---|---|---|
| D0 | **scalar block layout 项目策略**（2026-08-28 用户确认，替代原 112B 标准布局方案） | 所有 slangc 编译（3 反射管线 + `compile_slang` 实际 SPV）统一加 `-fvk-use-scalar-layout`：push constant 用 scalar（C 自然）布局，C++ struct（glm 自然对齐 4）与 shader 逐字段偏移天然一致、**零 padding、DrawArgs 96B**（标准布局下需 112B + 2 段 padding）。实测三条路：A 标准布局+padding 112B / B scalar 布局 96B（本决策）/ C 重排字段 96B（无依赖，备而不选）。**feature 侧零改动**：`scalarBlockLayout` 已在 `features_config.json`（`VkPhysicalDeviceVulkan12Features`，vulkan_core.h:6432）+ `features_generated.cppm` 请求 + `validateFeatures` 校验（此前空转，shader 未用 scalar 编译）。generator `standardLayoutInfo` 改 scalar 对齐（全 align 4）+ 修尾 padding 潜在 bug（单 `uint32_t` 只填 4B → `uint8_t[K]` 精确字节） |
| D1 | push constant 归一 = **统一超集契约** | 新 `shaders/material.slang` 声明唯一 `DrawArgs` 超集（**scalar 布局 96B**，见 D0），所有 shader 风格 `import material;` 同一布局；renderListSystem 零分支单一写入路径；一条 static_assert 契约。代价：加字段时所有风格契约同步改（集中在一个文件） |
| D2 | pbr 风格 = **占位 frag** | pbr 管线 = `model.mesh.spv` + 新 `model_pbr.frag.spv`，输出与 unlit **完全一致**（baseColor×albedo）；material 字段（roughness/metallic）已接线进 args 待 Phase 5 消费。零视觉变化，多风格机制全链路打通 |
| D3 | Lua 渲染意图 = **风格映射** | scene.lua 加 `styles` 表（风格名→管线规格名，缺失回落默认）；材质风格从 glTF 派生（KHR_materials_unlit→unlit，否则 pbr）；cube 恒 "cube" 风格。沿用 sol2 沙箱模式 |
| D4 | ShaderRegistry = **新独立模块** `shader_registry.cppm` | Vulkan 层（零 entt 依赖）：风格注册 + 管线条目（含 args size / push set 元数据）+ (顶点slot,纹理slot) 组合去重 + name→下标；main 构造后传引用给 Renderer |
| D5 | 管线去重 = **每模型一个拼接顶点 buffer** | per-primitive 392 个 BDA buffer → 每模型 1 个（chunk 偏移 = 累计 32B 倍数，天然 16B 对齐）；`MeshAsset.vertexOffset` 记 per-primitive 字节偏移，shader 读取加 `drawArgs.vertexOffset` 前缀。heap 顶点 slot 392 → 每模型 1 个 |
| D6 | group 数 **per-资产静态化** | `MeshAsset.groupCount` 上传期按各 shader 契约算好（cube=1、model=ceil(tri/10)）；per-frame 路径零推导零分支（§1.2 cube 不能套统一公式） |
| D7 | 批次节奏：每批独立可构建 + 截图验证（1/2/3 均要求**视觉零变化**），批次结束即更新 AGENTS.md 对应段落 | 避免文档债堆积到最后一批；截图基线 = 开工前 master 的 llm_debug PNG |

## 3. 关键设计

### 3.1 统一 push constant 契约（批次 1）

新 `shaders/material.slang`（第三个反射目标，与 types.slang / model.slang 平级）：

```slang
struct DrawArgs
{
    float4x4 mvp;
    uint totalTriangles;
    uint vertexOffset;      // 拼接顶点 buffer 内 per-primitive 字节偏移（批次 2 启用）
    float4 baseColor;
    float2 roughnessMetallic;  // x=roughness y=metallic（Phase 5 光照消费）
};

[[vk::push_constant]] DrawArgs drawArgs;
```

**scalar 布局（C 自然，D0）**：所有类型对齐 4，mvp@0(64) → totalTriangles@64(4) →
vertexOffset@68(4) → baseColor@72(16) → roughnessMetallic@88(8) → 总尺寸 **96B，零 padding**。
C++ glm 自然布局（vec4/mat4 对齐 4）逐字段吻合，static_assert(sizeof == 96)。
generator `standardLayoutInfo` 改 scalar 对齐（全 align 4），不插 `_padN` 字段。
（若改回标准布局：baseColor 需 align16 到 @80、roughnessMetallic align8 到 @96，总尺寸 112B + padding。）

契约归属重排（每契约文件拥有不相交段）：

| 契约文件 | 拥有段 | 生成模块 |
|---|---|---|
| `material.slang`（新） | push constant `DrawArgs` 超集 | `material_types_generated`（新） |
| `types.slang`（cube） | Vertex/VertexOutput + binding 0,1（vertex/index buffer slot） | `types_generated`（删 struct 段，留 slot 常量） |
| `model.slang` | ModelVertex/ModelVertexOutput + binding 0,1,2 + kTrianglesPerGroup 常量 | `model_types_generated`（删 struct 段，留 ModelVertex/slot/常量） |

`types.slang` / `model.slang` **不** import material（契约文件独立反射，避免 push constant 参数
重复进两份反射 JSON → 两个模块重复导出同名 struct）。mesh shader 同时 import 两个契约：
`cube.mesh.slang` = `import types; import material;`；`model.mesh.slang`/`model.frag.slang`
= `import model; import material;`（变量 `modelDrawArgs` 改名 `drawArgs`）。

### 3.2 ShaderRegistry（批次 2，新模块 `shader_registry.cppm`）

```cpp
export struct ShaderStyle {
    std::string name;          // 风格名（scene.lua pipelines[].name，"cube"/"model"/"model_pbr"）
    RGL::PipelineSpec spec;    // SPV 路径（沿用 scene_config 的 PipelineSpec）
};

export struct PipelineEntry {
    std::string name;
    RGL::RenderPipeline pipeline;
    uint32_t argsSize;         // 显式 args 布局（= sizeof(DrawArgs) 112B，L5 显式化）
    uint32_t pushDataSet;      // 显式 push constant set（= 0，L5 显式化）
};

export class ShaderRegistry {
    // 构造：(DeviceContext&, Resources&, span<const PipelineSpec>)
    //   建 styles + "cube" 条目（读 cube 资产 heapSlot/indexHeapSlot，现 makePipelineTable 逻辑迁入）
    [[nodiscard]] std::optional<uint32_t> pipelineIndex(std::string_view name) const;
    [[nodiscard]] const PipelineEntry& entry(uint32_t index) const;
    // 模型条目（批次 2 去重）：按 (styleName, vertexSlot, textureSlot) 组合查找，缺失才建
    // VkShaderEXT 对（binding mapping 烘焙顶点 slot/纹理 slot/sampler 偏移）；返回条目下标
    [[nodiscard]] uint32_t addModelEntry(const std::string& styleName, uint32_t vertexSlot,
        uint32_t textureSlot, uint32_t samplerSlot);
    void bindToAsset(RGL::Resources& resources, uint32_t assetId, uint32_t entryIndex);
};
```

- 组合去重键 = `(顶点slot, 纹理slot)`（sampler 共享一份，不进键）；条目名
  `"model_<vertexSlot>.<textureSlot>"`（name→下标 map 保留，供 cube 按名绑定 + 日志排查）。
- `PipelineEntry`/管线表成员从 `vkrenderer.cppm` 移出；Renderer 持 `ShaderRegistry&`
  （构造参数替换现 `span<const PipelineSpec>`），draw 循环经 `registry.entry(call.pipeline)`
  取管线 + `pushDataEXT(entry.pushDataSet, ...)`。
- `Renderer::pipelineAt()` 保留（委托 registry，llm_debug.cppm 零改动）。
- `addModelPipeline`/`bindPipelineToAsset` 从 Renderer 删除（功能并入 registry 两方法）。

### 3.3 顶点拼接 + slot 契约更新（批次 2，D5）

- `uploadModelMeshes`（model_importer.cppm）：
  - 总量 = Σ prim.vertices.size()×32B；每 primitive chunk 偏移 = 累计和（32B 倍数 →
    16B 对齐，VMA 分配基址 256B 对齐，BDA Load3/Load2 的 4B 对齐安全）。
  - 一个 VmaBuffer（整模型）+ 一次 per-model heap slot + 一个 storage buffer 描述符
    （pAddressRange 全长）。
  - 契约断言：**首个模型** slot == reserved + HeapSlotModelVertexBase(2)（语义从"首 primitive"
    改"首模型"，违例仍报错）；后续模型运行时 allocSlot（无固定断言）。
  - 每 primitive `MeshAsset`：`vertexOffset` = chunk 偏移、`heapSlot` = 模型 slot、
    `groupCount` = ceil(triangleCount / kTrianglesPerGroup)（D6）。
- `MeshAsset`（vkresources.cppm）+ 字段：`vertexOffset`、`groupCount`；
  资产表 + `modelSlotCount`（每上传一个模型 +1，供白纹理契约）。
- 白纹理契约（texture_pool.cppm:86-91）：expected = reserved + 2 + **modelSlotCount**
  （替换 modelMeshCount）。
- `model.mesh.slang`：三处 `Load3/Load2` 偏移加 `drawArgs.vertexOffset +` 前缀。
- 语义等价性：per-primitive 相对偏移不变（旧 = 独立 buffer 内 i×32；新 = 全局
  chunkOffset + i×32）→ 渲染输出逐像素一致（验收基线）。

### 3.4 材质字段 + pbr 占位风格（批次 3，D2/D3）

- gltf_loader：per-primitive 材质字段 + `metallic{1.0f}`/`roughness{1.0f}`
  （`pbrMetallicRoughness.metallicFactor/roughnessFactor`，缺省 1.0）/
  `unlit{false}`（`material.extensions["KHR_materials_unlit"]` 存在性）；
  `registerScene` 金丝雀扩到新字段，写入 `MaterialAsset`（+同名字段）。
- `shaders/model_pbr.frag.slang`（新）：输出与 model.frag 完全一致
  （`drawArgs.baseColor.rgb × texel.rgb`），头注释"Phase 4 占位：material 字段已接线，
  光照 Phase 5"。CMake 加一行 compile_slang（mesh 复用 model.mesh.spv）。
- scene_config：`SceneConfig` + `std::map<std::string,std::string> styleMap`
  （风格名→管线规格名，缺失回落默认 `{unlit="model", pbr="model_pbr"}`，
  未知风格 → 管线规格名查不到 = 启动报错，同现有 config 错误路径）；
  scene.lua + `styles` 表 + pipelines 数组 `model_pbr` 条目。
- 风格选择（main.cpp 上传期一次）：primitive 风格 = `prim.unlit ? "unlit" : "pbr"`
  （loader 输出即数据源，与 MaterialAsset 一致性由金丝雀保证）→ styleMap → 管线规格名 →
  `registry.addModelEntry(styleName, ...)` 取对应 spec 建条目。
  cube 资产恒 "cube" 风格（per-vertex color 独立着色模型，H1 nuance，kind 派生合法）。

### 3.5 renderListSystem 归一（批次 1，D1/D6）

- `RenderListState`：删 `cubeArgs` 单例 + `modelArgs` 向量 → 单一
  `std::vector<RGL::DrawArgs> args`（资产 id 索引，resize 到 assetId+1）。
- `renderListSystem` 单路径（无 AssetKind 分支）：

```cpp
auto& args = state.args[meshRef.asset];
args.mvp = viewProj.proj * viewProj.view * spin.matrix * tf.world;
args.totalTriangles = asset.triangleCount;
args.vertexOffset = asset.vertexOffset;
const auto& mat = assets.getMaterial(matRef.material);
args.baseColor = mat.baseColor;
args.roughnessMetallic = {mat.roughness, mat.metallic};
if (asset.groupCount == 0) return;
fresh.calls.push_back(DrawCall{asset.pipelineIndex,
    std::as_bytes(std::span<const RGL::DrawArgs>{&args, 1}), asset.groupCount});
```

  cube：shader 只读 mvp（totalTriangles/vertexOffset 不消费，写资产数据无害）；
  per-asset 存储语义与现状一致（多 entity 共享资产 = 后者覆盖，现项目资产 1:1 entity）。

## 4. 执行步骤

> 每批次结束：`just build` + llm_debug 截图比对基线 + AGENTS.md 对应段落更新。

### 批次 1：统一 push constant 契约

1. **spike**（首步，5 分钟）：material.slang 单独 `slangc -no-codegen -reflection-json` 通过；
   types.slang / model.slang 删 push constant 段后反射 JSON 无 `pushConstantBuffer` 参数
   （generator 对二者只出 slot 段）。
2. 写 `shaders/material.slang` + `shaders/material_schema.lua`（pushConstants 5 字段）。
3. 改 `shaders/types.slang`（删 DrawArgs + 声明）、`shaders/model.slang`（删 ModelDrawArgs + 声明）、
   `shaders/cube.mesh.slang`（+import material）、`shaders/model.mesh.slang`
   （+import material；modelDrawArgs→drawArgs；**本批不加 vertexOffset 前缀**，批次 2 启用）、
   `shaders/model.frag.slang`（modelDrawArgs→drawArgs + import material 视现有 import 行）。
4. 改 `shaders/types_schema.lua`（删 pushConstants 段）、`shaders/model_schema.lua`（删 pushConstants 段）。
5. CMake：第三条反射管线（material.slang → material_reflection.json →
   generated/material_types_generated.cppm，`--module-name material_types_generated`，模板复用）
   + FILE_SET 加行 + add_dependencies。
6. `vkresources.cppm`：`MeshAsset` + `groupCount{1}`；cube 资产创建处写 1（Resources 构造期）；
   model_importer 写 ceil(tri/kTrianglesPerGroup)。
7. `ecs.cppm`：`RenderListState` 单容器 + `renderListSystem` 单路径（§3.5）。
8. `vkrenderer.cppm`：draw 循环 `pushDataEXT(entry.pushDataSet, ...)`——本批先给
   `PipelineEntry` 加 `argsSize/pushDataSet` 两字段（registry 模块批次 2 再搬家）。
9. 验证（§7 行 1）。

### 批次 2：管线去重 + ShaderRegistry

1. 新模块 `vulkan_practice/shader_registry.cppm`（§3.2；"cube" 条目构建逻辑从
   Renderer::makePipelineTable 迁入 registry 构造）。
2. CMake FILE_SET 加 `shader_registry.cppm`。
3. `vkrenderer.cppm`：删 `PipelineEntry`/`pipelines`/`pipelineIndexMap`/`addModelPipeline`/
   `bindPipelineToAsset`/makePipelineTable，持 `ShaderRegistry&`（构造参数替换 specs span）；
   `pipelineAt()` 委托 registry。
4. `model_importer.cppm`：拼接 buffer + per-model slot + `vertexOffset`（§3.3）。
5. `vkresources.cppm`：`MeshAsset` + `vertexOffset`；资产表 + `modelSlotCount` 计数。
6. `texture_pool.cppm`：白纹理契约断言改 modelSlotCount。
7. `shaders/model.mesh.slang`：三处 Load 偏移加 `drawArgs.vertexOffset +` 前缀。
8. `main.cpp`：构造 `ShaderRegistry`（Resources 后、Renderer 前）传 Renderer；
   per-primitive 循环 → `addModelEntry("model", 模型slot, 纹理slot, samplerSlot)` +
   `bindToAsset`（批次 3 改带 styleName）；cube → `bindToAsset(registry.pipelineIndex("cube"))`。
9. 验证（§7 行 2）。

### 批次 3：材质字段 + pbr 占位 + Lua 风格映射

1. `gltf_loader.cppm`：metallic/roughness/unlit 解析（§3.4）。
2. `vkresources.cppm` `MaterialAsset` + 三字段；`model_importer.cppm` registerScene
   金丝雀扩展 + 写入。
3. 新 `shaders/model_pbr.frag.slang` + CMake compile_slang 加行。
4. `scene_config.cppm`：`styleMap` 字段 + sol2 读取（styles 表可选回落默认）+
   `defaultSceneConfig` 默认 styles；`configs/scene.lua` + `styles` 表 + `model_pbr` 管线条目。
5. `shader_registry.cppm`：`addModelEntry` 首参 styleName（按风格取 spec）。
6. `main.cpp`：primitive 风格派生（prim.unlit）→ styleMap → addModelEntry(styleName, ...)；
   风格→管线名解析失败 = 启动报错。
7. 验证（§7 行 3-4）。

### 批次 4：文档销账 + 总验证

1. `AGENTS.md`：模块表 +`shader_registry`/`material_types_generated` 行；更新 `vk_renderer`
   （drawFrame 经 registry 盲执行、push set 显式）/`vk_resources`（MeshAsset.vertexOffset/
   groupCount、modelSlotCount）/`ecs`（renderListSystem 归一）/`model_importer`
   （拼接 buffer + per-primitive 偏移）/`gltf_loader`（metallic/roughness/unlit）行；
   已知问题销两条债（push constant 归一、管线去重，改"已解 2026-08-28 重构批次 4"注记格式）；
   关键文件位置表 +material.slang/model_pbr.frag.slang/scene.lua styles。
2. `next_roadmap.md`：Phase 4 状态 → 已完成 + 完成情况清单 + 偏离注记
   （pbr 占位 frag、Lua=风格映射、Material 走 push constant 超集而非独立 binding、
   顶点拼接 D5 超计划新增）。
3. `ecs_compliance_review.md`：H1 注记 + L5 → 已解（批次 4 = Phase 4）；E5.3 完整形态达成注记。
4. `phase2_coupling_review.md`：L5 行加"已解"注记（历史文档，权威记录归 ecs_compliance_review）。
5. 总验证（§7 行 5）。

## 5. 文件变更清单

| 文件 | 批次 | 变更 |
|---|---|---|
| `vulkan_practice/shaders/material.slang` | 1 | 新增：DrawArgs 超集 + push constant 声明 |
| `vulkan_practice/shaders/material_schema.lua` | 1 | 新增：pushConstants 5 字段 |
| `vulkan_practice/shaders/types.slang` | 1 | 删 DrawArgs struct + push constant 声明 |
| `vulkan_practice/shaders/model.slang` | 1 | 删 ModelDrawArgs struct + push constant 声明 |
| `vulkan_practice/shaders/cube.mesh.slang` | 1 | +`import material;` |
| `vulkan_practice/shaders/model.mesh.slang` | 1,2 | +`import material;`、modelDrawArgs→drawArgs（批 1）；Load 偏移 +vertexOffset 前缀（批 2） |
| `vulkan_practice/shaders/model.frag.slang` | 1 | modelDrawArgs→drawArgs（+import material 视现有 import 行） |
| `vulkan_practice/shaders/types_schema.lua` | 1 | 删 pushConstants 段 |
| `vulkan_practice/shaders/model_schema.lua` | 1 | 删 pushConstants 段（descriptorSlots/vertexLayouts/constants 保留，heapSlotOffset=2 注释语义改"首模型"） |
| `vulkan_practice/shaders/model_pbr.frag.slang` | 3 | 新增：占位 frag（输出同 model.frag） |
| `vulkan_practice/CMakeLists.txt` | 1,2,3 | 第三条反射管线 + **所有 slangc 加 `-fvk-use-scalar-layout`**（3 反射 + compile_slang）+ EXTRA_DEPS 同步 import 图 + scalar 策略注释（批 1）；FILE_SET +shader_registry.cppm（批 2）；compile_slang +model_pbr.frag（批 3） |
| `tools/generator.cppm` | 1 | `standardLayoutInfo` 改 scalar 对齐（全 align 4，C 自然布局）+ 头注释更新 + 修尾 padding 潜在 bug（单 `uint32_t` 只填 4B → `uint8_t[K]` 精确字节差，与行内 pad 同款） |
| `vulkan_practice/generated/material_types_generated.cppm` | 1 | 自动生成（git-ignored）：DrawArgs 112B + static_assert |
| `vulkan_practice/vkresources.cppm` | 1,2,3 | MeshAsset +groupCount（批 1）/+vertexOffset + modelSlotCount 计数（批 2）；MaterialAsset +metallic/roughness/unlit（批 3） |
| `vulkan_practice/ecs.cppm` | 1 | RenderListState 单容器 + renderListSystem 单路径（删 AssetKind 分支） |
| `vulkan_practice/vkrenderer.cppm` | 1,2 | PipelineEntry +argsSize/pushDataSet（批 1）；管线表成员/方法移出、持 ShaderRegistry&（批 2） |
| `vulkan_practice/shader_registry.cppm` | 2,3 | 新增：ShaderStyle/PipelineEntry/ShaderRegistry（批 3 addModelEntry 加 styleName） |
| `vulkan_practice/model_importer.cppm` | 2,3 | 拼接 buffer + per-model slot + vertexOffset（批 2）；金丝雀扩展 + 材质新字段写入（批 3） |
| `vulkan_practice/texture_pool.cppm` | 2 | 白纹理契约断言 modelMeshCount → modelSlotCount |
| `vulkan_practice/gltf_loader.cppm` | 3 | metallic/roughness/unlit 解析 |
| `vulkan_practice/scene_config.cppm` | 3 | SceneConfig +styleMap + sol2 读取（可选回落默认） |
| `configs/scene.lua` | 3 | +styles 表 +model_pbr 管线条目 |
| `vulkan_practice/main.cpp` | 2,3 | ShaderRegistry 构造传参（批 2）；风格派生 → styleMap → addModelEntry（批 3） |
| `AGENTS.md` | 1,2,3,4 | 每批结束更新对应段落（模块表/已知问题/关键文件） |
| `docs/next_roadmap.md` / `docs/ecs_compliance_review.md` / `docs/phase2_coupling_review.md` | 4 | 状态/债销账 |

## 6. 风险与对策

| # | 风险 | 对策 |
|---|---|---|
| R1 | Slang `import material;` + 契约文件独立反射不成立（import 的 push constant 参数漏进/重进契约反射 JSON） | 批次 1 首步 spike 先验证；契约文件**不** import material（§3.1 契约归属重排），mesh shader 双 import——spike 不过则回退：material 段留在各自契约、生成器去重（emittedStructs 已有按名去重，两模块各导一份同名 struct 再手工合并） |
| R2 | 112B 超集布局与 Slang 标准布局偏差（roughnessMetallic 对齐假设错） | static_assert(sizeof == 反射尺寸) 编译期兜底；spike 阶段直接看反射 JSON 的 offset/size 与手算对表 |
| R3 | 拼接 buffer 动 heap slot 布局契约，三处断言不同步 = 启动期 "layout drift" 报错或静默错位 | 批次 2 内清单核对（model_importer/texture_pool/Resources 构造期三处）；断言全保留（首模型/白纹理/cube 各一条） |
| R4 | BDA Load 对齐越界（vertexOffset 非 4B 倍数） | chunk 偏移 = 32B 倍数（stride）、基址 = VMA 256B 对齐 → 天然满足；mesh shader 只用 Load3/Load2（4B 对齐要求） |
| R5 | pbr 占位 frag 与 unlit 输出"看起来一样"，管线切换未被真实验证（静默 fallback 到 unlit） | 日志显式输出每风格解析结果（materials: N 资产 / 风格分布）+ 管线表条目名含 styleName（`model_pbr_<v>.<t>`）+ 批次 3 开发期临时给占位 frag 加 tint（×1.1）截图证明确走 pbr 管线后还原 |
| R6 | 多模型场景未回归（设计兼容 per-model slot + 组合去重，现 config 单模型） | 文档记录；后续加第二模型时补回归（非本期验收项） |
| R7 | 批次 1 单独落地后中间态：args 112B 但 shader 只读 96B 前缀（vertexOffset 未启用） | 合法（push 数据大于 shader 消费范围无 VUID 冲突）；批次 1 截图仍须与基线一致 |

## 7. 验证矩阵

| # | 时点 | 检查项 | 通过判据 |
|---|---|---|---|
| 1 | 批次 1 末 | `just build` | 全绿；static_assert 112B 通过；反射 JSON offset 与手算一致（mvp@0/tri@64/voff@68/baseColor@80/rm@96/size 112） |
| 2 | 批次 1 末 | llm_debug 截图（cottage，同 camera.lua 基线） | 与 master 基线 PNG 目检一致（视觉零变化） |
| 3 | 批次 2 末 | 启动日志 | 管线表条目数 = 1(cube) + 12(model 纹理 slot 组合)；heap 顶点 slot = 1；无 layout drift |
| 4 | 批次 2 末 | llm_debug 截图 + 交互 run-igpu | 截图与基线一致；WASD/鼠标拖拽/滚轮正常，无 GPUVM fault |
| 5 | 批次 3 末 | 启动日志 | 材质风格解析（pbr/unlit 分布）+ model_pbr 条目被引用（条目名含 styleName） |
| 6 | 批次 3 末 | pbr 管线真实启用证明 | 临时 tint 截图 ≠ unlit 截图 → 还原后截图 = 基线 |
| 7 | 批次 3 末 | scene.lua styles 切换 | `styles = {pbr="model"}`（全 unlit）截图仍 = 基线；未知风格名 → 启动报错信息正确 |
| 8 | 批次 4 末 | cube + model 并存 | scene.lua `cube = true` 截图：cube 可见 + 模型正常 |
| 9 | 批次 4 末 | 全量 `just all`（clean-all + build-release） | 从零构建全绿（反射管线/FILE_SET 无遗漏） |

## 8. 预估

- 批次 1：shader 契约 3 文件 + schema 2 文件 + CMake 一段 + ecs/vkrenderer/vkresources 各一小改。中等（契约归属重排是主要心智成本）。
- 批次 2：新模块 + model_importer 重写上传段 + 三处断言 + main 流程。中等偏上（拼接 buffer 的偏移算术 + 断言同步）。
- 批次 3：glTF 解析 + 新 frag + config 字段 + 风格派生。小（模式全是现成的：金丝雀/sol2 沙箱/compile_slang）。
- 批次 4：纯文档。小。

每批次含验证（build + 截图），单批预计 1 个工作会话内可完成。
