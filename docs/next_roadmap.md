# vulkan_practice 路线图

> **状态**：现行（开发路线图；各 Phase 完成情况随提交更新，当前节点 = Phase 7 进行中：D1 延迟单灯 `70ce9d1` + D3 多灯/反射 + 批次 4 CPU culling 完成，批次 5=7b/批次 6 待续）

基于对 legacy 代码痛点的反思和新架构讨论，整理出分阶段开发计划。

> **注**：文中所有 `legacy/` 路径（旧 OpenGL 项目）仅作为思路参考，不是当前工作重点（见 AGENTS.md）。legacy 目录本身计划长期保留作参考，不随本路线图删除。

---

## 核心原则

1. **反射是 build tool，不是 runtime component**。反射结果只输出为 `.cppm`，渲染代码通过 `import` 消费，永不依赖反射系统的实现。
2. **编译期安全网**。生成的 `.cppm` 中包含 `static_assert`，C++ struct 与 shader 布局不一致时直接编译报错，杜绝运行时诡异行为。
3. **glTF only**。放弃 Assimp，只用 tinygltf。
4. **逐步推进**。每阶段稳定后再进入下一阶段。

---

## 现代 C++ 风格改造 (备忘)

基于 [cppreference][cppref] 和 [C++20/23 要解](/blog/cpp-modern) 讨论，记录后续重构方向。

### 错误处理：`std::expected`

> **已完成**

- `vkerror.cppm`：定义 `Error` 结构体（含 `ErrorCode` 枚举） + `Error::fromVkResult()` 静态工厂
- 初始化阶段 (constructors)：RAII 构造函数失败抛出 `std::runtime_error`，由 `main.cpp` 的 `try-catch` 统一处理
- 运行时路径：使用 `std::expected<T, Error>` 返回，`[[nodiscard]]` 禁止静默丢弃
- `VmaAllocatorRAII` 的创建方法（`createBufferWithAddress`、`createImage`、`createDescriptorHeap`）返回 `std::expected<T, Error>`
- `vk::raii::*` 创建函数（如 `loadShaderSpv`、`setupDebugMessenger`、`pickPhysicalDevice`、`selectPresentMode`）返回 `std::expected<T, Error>`
- `VmaDescriptorHeap::getSlotCpuAddress()` 返回 `std::expected<void*, Error>` 而非抛出
- `Renderer::drawFrame()` 返回 `std::expected<void, Error>`
- 初始化类构造函数在 init list 中用 `.value()` 展开 `expected`，失败则抛出
- **注意**：`std::expected` 不支持引用类型，返回引用的函数改为返回指针

### 接口：`std::span`
- 替代 `const void*` + `size` 参数对
- 顶点/索引数据上传、descriptor 写入、uniform 缓冲映射
- 配合 `std::as_bytes` / `std::as_writable_bytes` 做类型擦除

### 容器：`std::flat_map`
- descriptor set/binding 查找、material uniform 名称映射
- 缓存友好，比 `unordered_map` 对每帧查找更合适

### 性能：`consteval` / `assume_aligned`
- Vulkan 结构体用 `consteval` 编译期构造 + `static_assert` 布局校验
- `std::assume_aligned<N>` 优化 CPU→GPU 上传路径 SIMD

### 清理：scope guard
- RAII 之外的临时清理（`vkCmdBeginDebugUtilsLabelEXT` 配对 `End`）

---

## Phase 0：Shader Reflection

### 目标
建立编译期 shader 契约校验 + C++ 类型生成管线。

### 要做的事
- 创建 `tools/` 目录，构建两个独立 CLI 工具：
  - `schema_validator`：读取 `slangc -reflection-json` 的输出和 Lua schema，校验一致性
  - `generate_layout`：读取 reflection JSON，生成 `.cppm`（而非 `.hpp`，因为整个项目用 modules）
- `shaders/types_schema.lua`：定义 `types.slang` 的契约
- `shaders/types.slang` 是反射目标（包含 PushConstants、Vertex、binding 等通用定义）
- 修改 `CMakeLists.txt` 的 `compile_slang()`，增加 `-reflection-json` 和 `-fvk-use-scalar-layout`
- `shader.cppm` 改为 `import` 生成模块，用生成的 struct 和常量替换硬编码

### 生成的 `.cppm` 示例
```cpp
// build/generated_shaders/types.cppm
module;
export module types_generated;
import glm;

namespace RGL::Shaders::Types {
    export struct PushConstants {
        glm::mat4 mvp;
    };
    static_assert(sizeof(PushConstants) == 64, "PushConstants size mismatch with shader reflection");

    export inline constexpr uint32_t BindVertex = 8;
    export inline constexpr uint32_t BindIndex  = 12;
}
```

### 完成情况 (已完成)

- [x] `tools/` 目录：`reflection_tool` CLI 工具（validate + generate 子命令），使用 **C++20 modules** 拆分为 `schema_loader.cppm`、`validator.cppm`、`generator.cppm`
- [x] `shaders/types_schema.lua`：定义 `DrawArgs` push constant 和 `vertexBuffer`/`indexBuffer` descriptor slots（含 `heapSlotOffset`、`cppName`）
- [x] `shaders/types.slang`：反射目标
- [x] `CMakeLists.txt` `compile_slang()` 增加 `-reflection-json`
- [x] `reflection_tool generate` 使用 **inja 模板** (`tools/templates/types_cppm.inja`) 生成 `.cppm`
- [x] `reflection_tool` 接入项目统一日志库 `rllogger`，替代 `std::cout`/`std::cerr`
- [x] `shader.cppm`、`vkresources.cppm`、`vkrenderer.cppm` 已 `import types_generated`，硬编码常量从 `cube.cppm` 移除

> **完成的改动**：原 `reflection_tool.cpp` 467 行单文件被拆分为 4 个文件（3 个 `.cppm` + 1 `.cpp`），代码生成逻辑下沉到 inja 模板，日志统一走 `rllogger`。

### 边界
- `shader.cppm` 只 `import` 生成的模块，不 import 任何 reflection 相关代码
- `vkrenderer.cppm` 使用生成的 slot 常量和 struct 类型
- legacy 的 `ShaderReflection/` 目录在 Phase 3 后删除

---

## Phase 1：Camera + Input

### 目标
让场景可以交互，替换硬编码的相机和渲染循环。

### 要做的事
- **Camera component**：position、rotation、FOV、near/far，存入 `entt::registry` context
- **CameraSystem**：每帧根据 camera component 计算 view matrix
- **InputSystem**：复用 legacy `ControllerECS` 和 `CameraECS` 的逻辑
  - WASD + 鼠标 look（FPS 模式）
  - Trackball 控制（model viewer 模式）
  - 启动参数切换：`--fps` / `--trackball`
- **ecs.cppm 的 renderSystem**：用 camera 的 view matrix 替换硬编码的 `lookAt`
- 退出逻辑从 `Viewport.shouldClose` 扩展到支持 ESC 键

### legacy 复用
- `legacy/app/ControllerECS.hpp`：鼠标键盘输入逻辑
- `legacy/app/CameraECS.hpp`：透视相机参数
- 只参考思路，不抄代码，适配 C++20 modules + RAII 风格

### 完成情况 (已完成 2026-08-22)

- [x] 新增 `camera.cppm` 模块：`Camera`/`CameraInput` context（registry ctx）、`cameraView()`、`cameraSystem(rg, dt)`
- [x] 双模式统一 yaw/pitch 姿态表示：legacy trackball 的"矩阵左乘"（pitch 绕相机右轴 + yaw 绕世界 Y）在 `R = Ry(yaw)·Rx(pitch)` 下数学上等价于欧拉角增量，无需矩阵存储
- [x] **模式切换从启动参数改为 Lua 配置**：`configs/camera.lua`（`return` 顶层 table：mode/position/target/fovDegrees/near/far + `trackball`/`fps` 两个子 table 各带速度灵敏度，全部可选）由 `loadCameraConfig()` 经 sol2 沙箱加载（禁用 dofile/require 等，同 tools/schema_loader）；文件缺失 → 默认值 + warn，脚本/类型错误 → 报错退出
- [x] `ecs.cppm` `eventSystem` 保持中央轮询：WASD/鼠标按键/拖拽 delta/滚轮 → `CameraInput`，ESCAPE → 退出（legacy 多系统各自 `SDL_PollEvent` 的坑未引入）
- [x] renderSystem 补 dt（clamp ≤0.1s，防切后台瞬移）；view/proj 从 camera 取，model 时间旋转保留
- [x] 顺带修复 GLM_FORCE_RADIANS 陷阱：`glm::radians(45.0f)` 在该项目下是恒等函数，原 `perspective` 实际传入 45 弧度（等效 FOV ≈58.6°）；新代码手动角度→弧度换算
- [x] pitch clamp ±89° 防万向翻转
- [x] 运行基建：`configs/` 改为按文件 `copy_if_different` 增量拷贝（改 config 无需 relink）；`just run`/`run-igpu`/`run-dgpu` 统一 `check-run-ready` 资源检查；AGENTS.md 运行要求明确 CWD = `build/vulkan_practice/`

---

## Phase 2：glTF 加载

> **实施计划**：见 [phase2_plan.md](archive/phase2_plan.md)（2026-08-23 规划评审，含已确认决策、
> 事实核查、可执行分步、文件变更清单与风险）。
>
> **ECS 设计规范（参照系）**：本 Phase 起的场景代码必须符合五条 ECS 规范
> （registry 唯一数据源 / 纯数据组件 / 数据驱动场景构成 / 分层 / 资产-实例分离）——
> 摘要在 `AGENTS.md` 代码规范 §ECS 设计规范，合规状态与逐条处置记录权威记录在
> [ecs_compliance_review.md](archive/ecs_compliance_review.md) §ECS 设计规范。

### 目标
可以加载外部 3D 模型，替代硬编码的 cube（cube 与模型并存渲染）。

### 前置：Shader/Renderer 解耦（2026-08-22 架构评审记录）

当前架构下 shader 与 renderer **强耦合**（耦合点在 `vkrenderer.cppm` 的决策层，
`shader.cppm` 模块本身较干净）。多 mesh 渲染会直接撞墙：多 mesh → 多组 buffer/
descriptor slot → 每条管线一份 binding mapping，单一 `cubePipeline` 成员 + 硬编码
SPV 路径的模式撑不住。Phase 4 的 ShaderRegistry 是最终解法，但以下两步低成本
改动应在 Phase 2 开工前完成：

- [x] 动态状态从 `RenderPipeline` 移出：`RenderState` + `applyDynamicState(cmd, w, h, state)`
      由调用点（`Renderer::drawFrame`）设置，RenderPipeline 退化为纯 shader object
      包装（`bindShaders`）——为 Phase 7 G-buffer 多 pass（同一 mesh shader 不同
      状态）铺路
- [x] Renderer 持有管线表而非单成员：`std::vector<PipelineEntry>`（name →
      RenderPipeline），SPV 路径从 `configs/scene.lua` 传入（沿用 Phase 1 确立的
      `configs/*.lua` 模式，sol2 沙箱 + 缺失回落默认），为 Phase 4 的
      ShaderRegistry 留接缝

耦合点定位（评审时）：
- SPV 文件名硬编码：`vkrenderer.cppm` 的 `createRenderPipeline`（写死
  `cube.mesh.spv`/`cube.frag.spv`），成员名 `cubePipeline`
- `ShaderEffect` 固定 mesh+frag 二元组（shader.cppm），无 task stage /
  vertex+fragment 路径——结构体形状为当前 cube 定制
- 动态状态烘焙进 `recordRenderCommands`（depth write、blend off、1x sample
  等），实际是"cube 的 opaque pass 状态"而非 shader object 固有属性
- Renderer 单成员 `cubePipeline`，加 shader 需改构造器 + drawFrame，
  无注册/查找机制

已解耦的部分（保持现状）：
- `RenderPipeline` 构造器接收 `descriptorSize/slotVertex/slotIndex` 参数，
  slot 号由 `Resources` 运行时动态传入（非硬编码），binding mapping 干净
- SPV 数据与管线对象分离（`ShaderEffect` 传数据进构造器）；
  `loadShaderSpv` 为可复用自由函数

### 要做的事
- **tinygltf** 集成（替换 legacy 的 Assimp）
- **ModelImporter 模块**：
  - 解析 glTF 文件，flatten node 树为 ECS 层级
  - 每个 glTF node → entity，带 `Transform` component
  - mesh 数据（顶点/索引）上传为 `VmaBuffer`，共享存储
  - 材质信息解析为 `MaterialRef`（暂时只存引用，完整材质系统在 Phase 4）
- **ResizeSystem 扩展**：resize 时重新上传 model data（如果需要）
- **ecs.cppm 的 renderSystem**：支持从 registry 中查询 entity 列表进行渲染，而非单一的 cube

### 设计决策
- glTF 的 node 层级用 ECS 的 parent-child 关系表示（entt 支持）
- mesh data 按 primitive 分组上传，每个 mesh 一个 VmaBuffer
- 不做 instancing（后续优化时再加）

### 完成情况 (已完成 2026-08-23)

> 执行细节与计划外发现（vcpkg baseline/哈希坑、tinygltf v3 非 header-only、
> 标准布局 padding、构建内存）见 [phase2_plan.md §7](archive/phase2_plan.md)。

- [x] 前置两步完成（见上勾选项）：`shader.cppm` 的 `RenderState`/`applyDynamicState` 外移 + `vkrenderer.cppm` 管线表（`PipelineEntry`，per-primitive 条目 `model_i`，binding mapping 各自烘焙）
- [x] **tinygltf 集成**（vcpkg，替换 legacy Assimp 思路）：`gltf_loader.cppm` CPU 解析——POSITION/NORMAL/TEXCOORD_0 严格校验 + byteStride 处理 + 去索引展开 + node translation 扁平化 + baseColorFactor/alphaMode，输出 `GltfModel`（含**包围球**）；`tinygltf_impl.cpp` 自组 impl TU（vcpkg port 只装头、实现在 `TINYGLTF_IMPLEMENTATION` 宏后，-O0）
- [x] **ModelImporter**：`model_importer.cppm`——`uploadModelMeshes`（每 primitive 一个 BDA storage buffer + 描述符写 heap slot `reserved+8+i`，避开 cube 的 0/4）+ `registerScene`（场景根 Relationship + 每 primitive 一个 entity：`Transform`/`MeshRef`/`MaterialRef`，双向父子挂根）
- [x] **多契约反射**（schema 为顶点契约 source of truth，slangc 不反射顶点结构体）：`model.slang` + `model_schema.lua`（pushConstants/descriptorSlots `heapSlotOffset=8`/vertexLayouts packed 32B）→ 第二条 CMake 反射管线生成 `model_types_generated`；生成器按 GLSL 42.6 标准布局为 push constant 插 `uint8_t _padN[M]`（Slang float4 align 16 vs glm vec4 align 4，static_assert 对反射总尺寸）
- [x] **模型 shader**：`model.mesh.slang`（无 task stage，`SV_GroupID` 派生 group，每 group 10 三角形/30 顶点，显式偏移读 packed 顶点流）+ `model.frag.slang`（flat unlit baseColor，alpha 忽略）；CPU `drawMeshTasksEXT(ceil(tri/10),1,1)`
- [x] **场景配置**：`scene_config.cppm` + `configs/scene.lua`（model/pipelines/spin/cube，sol2 沙箱同 camera 模式）；cube 改 config 开关（`cube = false`，Phase 1 遗留 cube 在原点 ±0.5 与模型互穿）
- [x] **renderSystem 多 entity**：per-entity `ModelDrawArgs`（mvp = proj*view*spin*world，按 primitiveIndex 对齐管线表条目/heap slot）
- [x] **自适应空间范围**：模型包围球 → `ModelBounds` context → 每帧按相机到球心实时距离推导 near/far（`max(0.05, dist-r)` / `dist+r`），无模型回落 `configs/camera.lua` 固定值——修复固定 far=10 切掉深 z 模型的问题
- [x] CMake 全接入：第二条反射管线 + 2 个 `compile_slang` + `find_path(TINYGLTF_INCLUDE_DIRS)` + `models/` 拷贝（`assets/transparent.glb`）+ FILE_SET 新模块（`gltf_loader`/`model_importer`/`scene`/`scene_config`）
- [x] 运行验证（`just run-igpu`，AMD iGPU）：4 primitives × 36 顶点上传 heap slot 8-11、无 warn/error；`assets/transparent.glb`（4 盒，2 OPAQUE + 2 BLEND）渲染正确，BLEND 两盒按 Phase 2 决策渲染实色（透明排序归 Phase 5）

### 延后
- `cube.cppm` 的 `Vertex` 改走反射生成契约（low priority，cube 已 config 关断）
- 相机自动对准包围球初始化（蓝盒默认在相机背后，WASD 可移过去看）

---

## Phase 3：纹理管理

> **状态（2026-08-28）：已完成**（内嵌纹理采样 + 材质资产 + per-primitive 管线烘焙，
> 三驱动验证见 [heap_texture_research.md](archive/heap_texture_research.md)；真实 4K 纹理模型
> cottage 257MB/392 primitive/11 张 4096² 复验 PASS）。
> 计划-实现偏离注记（实现为准，AGENTS.md 模块表为现状权威记录）：
> - 纹理数据归属 `Resources::AssetTable`（`TextureAsset`/`MaterialAsset` 容器），
>   **TexturePool 类未落地**——2026-08-28 重构批次 1 把 main 局部 TexturePool 对象
>   （GPUVM fault UAF 根因，archive/heap_texture_research.md §12.6/§12.7）整体降为自由函数
>   （`loadEmbedded`/`ensureWhiteTexture`/`uploadModelTextures`），slot 分配统一走
>   `Resources::allocSlot`（"TexturePool 容量动态扩展"决策作废——固定裕量 1024 槽，与场景内容解耦）
> - 纹理采样走**独立 sampler heap**（非 pEmbeddedSampler）：spike 两路三驱动均 PASS，
>   选已验证可用的独立 sampler heap（phase3_plan R1 回退链既选路径，shader.cppm 注释）
> - 材质系统提前部分落地：`MaterialAsset{baseColor, textureId, alphaModeBlend}` 资产表 +
>   `MaterialRef{material}` 真引用（重构批次 2，E5.3）——比计划"Phase 4 才做材质系统"早，
>   Phase 4 的 ShaderRegistry 在其上扩展（per-pipeline args 布局归一）
> - **留待 Phase 4**：管线表 per-primitive 条目（392 条）去重（binding mapping 是 shader
>   object 级状态，按 (顶点 slot, 纹理 slot) 组合归并 + 资产表存条目引用）；
>   `renderListSystem` 的 AssetKind 分支（push constant 布局 64B/96B 不统一）
>
> **预研（历史）**：`heap_texture_spike/` 三驱动实测（2026-08-27：RADV/Intel/NVIDIA ×
> 5 变体全 PASS）；**关键约束**：`imageDescriptorSize`（RADV 64 / NVIDIA 32 / Intel 64）>
> `bufferDescriptorSize`（16/16/64），统一步长改 `max`（运行时查询，已落地）。
> 完整记录见 [heap_texture_research.md](archive/heap_texture_research.md)。

### 目标
支持纹理贴图，为 PBR 材质做准备。

### 要做的事
- **TexturePool 模块**：
  - 管理一组 `VmaBuffer`（每张纹理一个），以及对应的 descriptor heap slots
  - `allocSlot(imageData)` → 返回 slot index
  - 内部做 path → slot 缓存（避免重复加载）
- **TextureAsset**：封装纹理加载（stb_image 或 tinygltf 内嵌纹理）+ pool 分配
- **types.slang 扩展**：加入 `TextureSampler` 或 `SampledImage` binding
- **schema 扩展**：校验纹理 binding 的合法性
- **shader.cppm 扩展**：支持纹理 descriptor 的 binding mapping

### 设计决策
- 纹理 descriptor 和 buffer descriptor 放在同一个 heap 还是分开？初期放同一个 heap，后续按需拆分
- 不做 texture atlas（简单先做），mipmap 后续加
- `TexturePool` 容量可动态扩展（realloc heap）

---

## Phase 4：Shader 反射扩展 + 多 Shader 支持

> **状态（2026-08-28 完成；2026-08-30 收尾）**：**已完成**（4 批次，详见 `docs/archive/phase4_plan.md`）。前置债已清偿：
> ① 管线表 per-primitive 条目去重（392→按 (规格, 顶点 slot, 纹理 slot) 组合 = 12 条）；
> ② `renderListSystem` AssetKind 分支归一（统一 `DrawArgs` 超集，零 AssetKind 分支）。
> ShaderRegistry 是两者的统一落点。**2026-08-30**：遗留的 cottage 偶发 artifact 已解（根因 = 两个自 Phase 2 就存在的共享代码越界 bug——`gltf_loader` uv 步长 + `model.mesh.slang` triangles 索引，由 Windows/NVIDIA 移植暴露，见文末偏离注记）。

### 目标
支持多种 shader 风格（unlit、pbr），通过 Lua 描述渲染意图。

### 完成情况 (已完成 2026-08-28，4 批次)
- [x] **批次 1 统一 push constant 契约**：`material.slang` `DrawArgs` 超集（scalar 布局 96B
  零 padding）+ CMake 第三条反射管线（material.slang + material_schema.lua →
  `material_types_generated`）+ generator scalar 对齐 + `MeshAsset.groupCount`（上传期派生）
  + `renderListSystem` 归一（单一 DrawArgs 按资产 id 索引，零 AssetKind 分支）+
  `PipelineEntry.argsSize/pushDataSet` 显式化（L5）
- [x] **批次 2 管线去重 + ShaderRegistry**：`shader_registry.cppm` 新模块（零 entt，
  按 (规格, 顶点 slot, 纹理 slot) 组合去重）+ 分块拼接顶点 buffer（16MB×6 块）+
  per-draw `vertexOffset`（392 primitive → 12 条目 / 6 heap slot）
- [x] **批次 3 材质字段 + pbr 占位 + Lua 风格映射**：gltf_loader +metallic/roughness/unlit
  + `MaterialAsset` 三字段 + `model_pbr.frag.slang` 占位（输出同 model.frag，光照 Phase 5）
  + scene_config `styleMap`（风格名→管线规格名）+ scene.lua `styles` 表 + main 风格派生
  （prim.unlit → styleMap → 管线条目）
- [x] **批次 4 文档销账 + 总验证**：AGENTS.md/next_roadmap/ecs_compliance 销账

### 偏离注记
- **pbr 占位 frag**：`model_pbr.frag.slang` 输出与 model.frag 完全一致（`DrawArgs.roughnessMetallic`
  契约字段已留，但 `renderListSystem` 当前写 CPU 占位 0，mat.roughness/metallic 接线 + 光照均留
  Phase 5）——非真 PBR 光照
- **Lua = 风格映射**（非渲染意图）：scene.lua `styles` 表只做"风格名→管线规格名"映射，
  非 Phase 6 的渲染意图描述（entity 列表/pass 序列）
- **Material 走 push constant 超集**（非独立 binding）：材质字段（baseColor/roughness/
  metallic）进统一 `DrawArgs` 超集（96B scalar），非独立 descriptor binding
- **顶点拼接 D5 超计划新增**：分块拼接 buffer（16MB×6 块）+ per-draw vertexOffset 是
  计划外新增（为去重服务的 heap slot 收敛）
- **cottage 偶发 artifact（已解 2026-08-30，根因 = 两个共享代码越界 bug）**：先前"非确定性驱动
  竞态"为误诊。真根因两处自 Phase 2 就存在、由 Windows/NVIDIA 移植暴露：① `gltf_loader.cppm`
  uv 步长错（`uv[idx*3]` 应为 `uv[idx*2]`，vec2 误按 vec3 读）→ 越界读采样为黑（741px artifact
  根因）+ Windows AV 崩溃；② `model.mesh.slang` triangles 索引用全局顶点索引
  （`(firstTriangle+ti)*3` 应为组内 `ti*3`）→ NVIDIA 几何崩形（RADV mod 回绕恰好掩盖）。详见
  AGENTS.md 已知问题 + archive/phase4_plan.md 批次 2 定案注记（含教训）

### 设计决策
- 每个 shader 风格有独立的 `schema.lua` 和生成模块（实际：material.slang 统一契约 +
  各风格 frag 复用）
- `shader.cppm` 支持多 pipeline，通过 shader registry 切换
- **前置依赖**：先完成 Phase 2 的"前置：Shader/Renderer 解耦"两步
  （动态状态外移 + 管线表），ShaderRegistry 在其接缝上注册
- Lua 配置和 shader reflection 是两套独立机制，不要混淆

---

## Phase 5：透明排序 + 简单光照

> **状态（2026-08-31）：已完成**（4 批次，实施记录见 `docs/archive/phase5_plan.md`）。
> 计划-实现偏离注记（实现为准）：
> - **光照走 descriptor heap 而非 push constant**（原设计决策"push constant 传递轻量"被用户
>   bindless 原则推翻）：DrawArgs 仅 +`objectIndex`(4B) 进 100B，光参数 + 对象矩阵表全进
>   scene data buffer（binding 3，`Resources::ensureSceneData` + 每帧 staging ring →
>   `vkCmdCopyBuffer`——全项目首个每帧动态 GPU 数据基建，Phase 7 shadow/culling 直接复用）
> - **光照空间 = 世界空间**（原"CPU 转 view space"方案因 shader 只有融合 mvp、frag 无法
>   重建 view 空间位置而改道）：对象矩阵表 bindless 索引，光方向/eyePos 世界空间直传
> - **两条光照路线**（用户要求"简化 PBR + 基础 Phong 兼容"）：`model_pbr`（简化 GGX）+
>   `model_phong`（Blinn），同契约同 mesh SPV，scene.lua `styles.pbr` 一行切换
> - **透明段开背面剔除**（计划外必要修复）：关深度写后封闭网格正反面各混合一次 =
>   有效 alpha 1-(1-a)²（实测 0.25→0.4375 像素级定位），cull 后单层混合语义正确
> - 透明排序在 `renderListSystem` 内做（原设计决策兑现，不单独建 system）

### 完成情况 (已完成 2026-08-31，4 批次)
- [x] **批次 0 Phase 4 文档销账**：AGENTS.md「ECS 已知债」头行划线 + ecs_compliance_review
  陈旧 "仍 Open" 注记覆盖；顺手修 Justfile `run` 的 Python 3.12+ 兼容（裸相对名走 PATH 搜索）
- [x] **批次 1 透明排序**：`model.frag`/`model_pbr.frag` 输出 alpha（baseColor.a×texel.a）+
  `applyDynamicState` 接线 `setColorBlendEquationEXT`（主工程此前零调用，enable 不开 factors
  不混合）+ `FrameDrawList.transparentStart` 双段 + drawFrame 段首两次状态 + llm_debug 同步 +
  renderListSystem 分桶 + 距离² 降序稳定排序。**像素级验收**：重叠区实测 (65,98,53) =
  B2F 正确序（错序 (53,89,60)），透明叠不透明 (204,52,8) 精确命中
- [x] **批次 2 动态堆数据基建 + 光照契约**：`LightDataGpu`(48B) + 对象矩阵表（1024×64B）
  布局契约 + `ensureSceneData()`（allocSlot 运行时槽，幂等）+ staging ring ×2（fence 后写 +
  录制期 copy + Transfer→Graphics barrier）+ DrawArgs +`objectIndex`（96→100B，反射自动重生成）+
  `roughnessMetallic` 接线（占位 0 退役）+ `Light` context + scene.lua light 段 +
  `maxPushConstantsSize`/矩阵容量启动期契约检查。cottage 回归 diff=0 位级一致
- [x] **批次 3 双光照风格**：`scene.slang`（bindless 契约 + worldPos/worldNormal 助手）+
  `model.mesh`/`model.slang` 加 positionOS 插值器 + `model_pbr.frag` 真简化 PBR（GGX）+
  `model_phong.frag` 基础 Phong（新管线，scene.lua/CMake/回落表三处接线）。验证：cottage
  （392 全 pbr 风格）光照几何正确、两风格切换渲染差异实锤、transparent.glb BLEND×PBR 组合
- [x] **批次 4 文档销账**：AGENTS.md（模块表 8 处 + shader 编译 bindless 原则 + 已知问题
  Phase 5 近似四条）、next_roadmap 本节、archive/phase5_plan.md 留档

### 明确不做 / 留接缝
Shadow、多灯、OIT/per-pixel 透明排序、MASK alphaTest；scene data 基建与 staging+copy 模式
留 Phase 6（rendergraph 动态数据）/ Phase 7（shadow map、culling 矩阵表）复用。
常 ambient 项 → Phase 5.5 由 IBL 环境采样替换。

---

## Phase 5.5：IBL（简化版）+ 天空盒

> **状态（2026-09-01）：已完成**（4 批次，实施记录与素材面序定案见 `docs/archive/phase5_5_plan.md`）。
> 计划-实现偏离注记（实现为准）：
> - **素材是直接 6 面 cubemap**（非 equirect）→ 批次 1 砍掉 equirect→cube CPU 转换，改
>   `uploadCubeMap` 6 面直传（equirect 转换留作可选扩展）；linear staging 三坑（RADV 实测）
>   = linear 必须 mipLevels=1 + CUBE_COMPATIBLE 不支持 + linear maxArrayLayers=1 → 6 张
>   独立 2D staging
> - **mipmap blit 用 NEAREST**（sRGB 格式 blit LINEAR 违 VUID-vkCmdBlitImage-filter-00239）；
>   线性空间 box 降采样（CPU 生成 mip）留作质量接缝，本 phase 未做
> - **env 存储 = Resources 场景级单例**（`std::optional<TextureAsset>`，非 AssetTable
>   textures）——env 绑场景不绑材质，不占纹理 id 空间（白纹理 id 0 契约零影响）
> - **IBL 只接 pbr 线**：新风格 `model_pbr_ibl`（phong 线保留常 ambient）；参数走 scene data
>   头 @48（env）+ @64（skybox invViewProj，头 64→128）手工契约（LightDataGpu 双源模式）
> - **天空盒 = 全屏大三角 + clip→world 射线**（sky.slang，免 resolution）；三段
>   `[opaque|skybox|transparent]` 边界与 Phase 6 pass 序列同构
> - **`RenderPipeline` mapping 修正**：固定下标假设 textured=true，天空盒
>   textured=false+scene+env 组合暴露 → 改顺序填充（Phase 2 遗留隐患清偿）
> - **shadow 明确不做**（用户拍板留 Phase 7 后再议）

### 目标
环境贴图采样替换 `model_pbr` 的常 ambient 项（简化版 IBL），场景加天空盒渲染段。
主题归属 = Phase 5"简单光照"的补全（光照内容问题），不顶替 Phase 6 的 RenderGraph 架构主题。

### 范围决策（开工前用户确认）
- **简化版**：单张环境贴图 + roughness→LOD 采样；无 HDR/tone mapping、无
  pre-filtered/BRDF LUT（完整版留 Phase 7 延迟 light pass 时做）——不阻塞未来升级，
  基建（cubemap 上传 + mipmap 链 + 采样入口 + skybox 段 + 参数 slot）全复用
- **mipmap 链要补**：skybox 专用 sampler，不动 `ensureSharedSampler`
- **shadow 暂不做**：留待 Phase 7 后再议（forward shadow = 项目第一个每帧多 pass 带
  依赖，归 RenderGraph 用例更顺）
- **phong 线不动**：常 ambient 保留，IBL 只接 pbr 线

### 完成情况 (已完成 2026-09-01，4 批次)
- [x] **批次 1 cubemap + mipmap 纹理基建**：`uploadCubeMap`（6 面直传 e2D+eCubeCompatible
  目标图 + 6 张独立 2D linear staging + 6 单 region copy + 逐 level 6 面 blit NEAREST 链）
  + `loadEnvCubeMapFiles`（stb 解码 + 方形/2 幂/同尺寸校验）+ `TextureAsset.mipLevels` +
  `ensureSkySampler`（sampler heap 第二槽 mipmapMode=LINEAR+maxLod）+ env 单例存储 +
  拷贝金丝雀 + CMake `copy_skybox`
- [x] **批次 2/3 IBL 采样 + 参数 + 管线接线**：`env.slang`（binding 4/5 + readEnvParams）+
  `model_pbr_ibl.frag.slang`（IBL 替换 ambient）+ scene data 头 @48 env 参数 +
  `SceneEnv` context + `PipelineBindings` 聚合 + mapping 4→6 + `addModelEntry` 查 env 恒烘焙
  + scene.lua environment 段 + 两条 main 契约（时序 + 风格）
- [x] **批次 4 天空盒**：`sky.slang`（clip→world 射线）+ `skybox.mesh/frag.slang`（全屏大
  三角采 env cubemap）+ scene data 头 64→128（@64 invViewProj）+ `FrameDrawList` 三段 +
  `addSkyEntry` + skyboxState（LEQUAL 不写深度）+ drawFrame/llm_debug 三段 + **mapping
  顺序填充修正**
- [x] **批次 5 验收 + 文档销账**：验证矩阵 10 项全 PASS、AGENTS.md 模块表、next_roadmap 本节

### 设计决策
- 素材为直接 6 面 cubemap（equirect→cube CPU 转换砍掉，留作可选扩展）；面顺序以
  archive/phase5_5_plan.md 素材定案表为准（scene.lua 显式文件列表配置，方位歧义改配置零代码）
- IBL 参数走 scene data 手工契约（延续 LightDataGpu 双源模式，Phase 7 多灯重设计时并入反射）
- **无环境贴图**：模型管线不烘焙 binding 4/5（不建特例管线）；引用 model_pbr_ibl 风格但
  无 environment 段 = 启动契约报错（未映射描述符采样 = 未定义，明确拦截优于静默画面错）

---

## Phase 6：Lua 渲染意图（架构升级）

> **状态（2026-09-03）：已完成**（5 批次）。计划-实现偏离注记（实现为准）：
> - **barrier 自动推导纳入本期**（原计划未列，用户拍板"至少得有个雏形"）：layout/stage/access
>   从 pass 声明的 usage 推导（`render_graph.cppm` 头注释规则表 = 单一真相源），drawFrame
>   手写布局转换清零。业界定位 = Frostbite 第一代全自动图保留核心一条（自动 barrier），
>   aliasing/pass fusion/多线程录制明确不做（GDC 2022 "The Future of Render Layers" 的
>   API-tight 收敛方向，本就不需要全自动）
> - **Pass 拆分 render pass 实例**：原三段共享一个 beginRendering → 每 pass 独立实例，
>   跨实例 LOAD 靠编译图对同布局 attachment 也发 identity barrier（写→写转换）保证执行
>   依赖；中间 pass 的 color/depth storeOp 必须 store（编译器校验"后续使用的写不得
>   DontCare"）。像素结果与单实例版 BIT-IDENTICAL（headless swapchain 回读 sha 对账）
> - **RGL_OFFSCREEN DIAG 转正为图变体**（批次 4）：offscreen attachment（scene_rt，
>   format 暂恒 inherit=swapchain 格式）+ composite blit pass；旧手工四件套
>   （offscreenImage/offscreenLayout 追踪/④⑫分支/waitStage 特判）全部退役为图机制。
>   顺带修 VVL 实锤的潜伏违例：swapchain imageUsage 缺 TRANSFER_DST（blit dst 必需）
> - **Lua 面 = 纯声明数据**（sol2 沙箱，同 camera/scene_config 模式），零 handle 零
>   每帧回调零热重载——启动期 load→compile→fail-fast，录制期零图错误；pipeline/state/
>   attachment 全名字字符串引用
> - **entities 列表化**（批次 5）：type=model|cube + position/rotation(欧拉度)/scale
>   (均匀)；缺段由旧 model/cube 字段兼容合成（行为逐位不变）。多模型暴露 heap 布局
>   契约的时序强制：全部模型顶点槽先于任何纹理槽（A1/A2 两阶段循环，交错 = 契约
>   断言启动拦截）
> - **自检守卫先抓到真 bug**：`renderGraphSelfCheck()`（无 GPU，main 启动期跑）首跑
>   即查出"DontCare 后仍被**读**"漏校验——教训固化进编译器测试样本

### 目标
用 Lua 描述渲染意图，renderer system 将其转换为可执行序列。

### 完成情况（5 批次）
- [x] **批次 1 render_graph 核心模块**：`RgGraphDesc`（attachment/pass 声明）+
  `compileRenderGraph`（校验 + 布局链推导 + barrier 计划 + presentTransition +
  swapchainWaitStage 派生）+ `defaultRenderGraphDesc`（三段等价图）+ 自检；纯 C++
  零 Lua 零 entt，接口/impl 分离
- [x] **批次 2 Renderer 消费编译图**：drawFrame ④⑤⑧⑨ 手写段 → pass 循环盲执行
  （emitBarriers 物化 + attachmentLayouts 逐 slot 追踪表）；heap/staging barrier 留
  数据通路固定段不入图；**验收 = 旧二进制 vs 图化新二进制截图 BIT-IDENTICAL**
- [x] **批次 3 render_graph_config**：`configs/render_graph.lua`（sol2 沙箱 + 缺文件
  回落默认 + 枚举字符串启动报错）；main 接线 + "N attachments / M passes compiled"
  观测日志；Lua 图 vs 内建默认图渲染位级一致
- [x] **批次 4 offscreen 转正**：`Resources::ensureRtAttachment`（RT 缓存 + generation
  重建检测 + resize 布局追踪重置）+ blit pass 执行 + `offscreenRenderGraphDesc` +
  `RGL_OFFSCREEN` env 图变体；双路径 VVL 零 error + 三方截图位级一致
- [x] **批次 5 scene.lua entities**：SceneEntityDesc + registerScene/registerCubeEntity
  rootTransform + main 三段加载流（A1 顶点/A2 纹理/B env/C 烘焙注册）；兼容合成
  diff=0 + 双模型+cube 三 entity 场景实机验证
- [x] **批次 6 文档销账**：AGENTS.md（模块表 +2、启动流、已知问题）+ 本节 +
  docs/技术概念.md 渲染图小节

### 要做的事（原计划，全部兑现）
- **RenderGraph 模块**：
  - 定义 `Pass`（输入 attachments、输出 attachments、shader、draw calls）
  - 定义 `Attachment`（颜色、深度、临时缓冲区）
  - Lua 描述 pass 序列，C++ 编译为可执行序列
- **Lua 场景配置**：
  - 描述 entity 列表、shader 选择、渲染参数
  - 启动时读取，填充 registry
- **与 Phase 4 的关系**：Phase 4 的多 shader 支持是 Phase 6 的基础

### 设计决策
- RenderGraph 是独立模块，不耦合到 ecs.cppm
- Lua 配置和 schema 校验是两套独立机制：schema 校验 shader 契约，Lua 描述场景意图
- **机制模板已就位**：`configs/camera.lua`（Phase 1 落地）确立了 Lua 配置的标准做法——sol2 沙箱加载（禁用 dofile/require，同 tools/schema_loader）+ 字段可选回落默认值 + 构建期增量拷贝到 build 目录。Phase 6 的场景/渲染意图 Lua 沿用同一模式
- **执行器边界（批次 2 定案）**：图只管 attachment 状态同步；descriptor heap host-write
  barrier、scene staging copy、heap 绑定、acquire/present 信号量与双 fence 帧槽复用
  留 Renderer 固定段（非 attachment 语义，入图反而失真）
- **后续完善方向**（本期明确不做）：offscreen 显式格式（HDR/降采样 RT）、sampleRead
  描述符接线（后处理 shader 采样 RT——blit 路已通，采样路差描述符入库）、transient
  aliasing、pass fusion（同附件连续 pass 合并 render pass 实例）、多线程录制、
  每帧 Lua 回调/热重载

---

## Phase 7：延迟渲染（独立分支）

> **实施计划**：见 [phase7_plan.md](archive/phase7_plan.md)（2026-09-07 起草，含批次拆解、
> 范围决策 D1–D6 待确认、验证矩阵与风险）。**开发日志**（过程/踩坑/当前态滚动记录）：
> [phase7_devlog.md](phase7_devlog.md)。Bistro Exterior 已转 `assets/bistro_exterior.glb`
> 作测试主场景（转换管线 `tools/bistro_pack.py`）。

### 实施进度（2026-09-07，D1 里程碑已提交 `70ce9d1`）

| 批次 | 内容 | 状态 | 剩余 |
|---|---|---|---|
| **0** Bistro + loader 节点 TRS（forward） | nodeWorld mat4 父链、Bistro forward、packer、CMake、`kSceneMatrixCapacity`↑4096 | **✅ 完成** | normal 绿通道翻 + BC5 Z 重建（→ Phase 8 纹理接缝） |
| **1** G-buffer MRT + offscreen 显式格式 | `isKnownRgFormat`/`resolveRgFormat` 词表、4-MRT `deferred_geometry`、`RGL_DUMP_GBUF` 直读验 albedo | **✅ 核心完成** | normal/metalRough **纹理**采样（引擎材质仅 baseColor 贴图，→ Phase 8） |
| **2** Light pass + sampleRead 接线 | SAMPLED_IMAGE 描述符入 heap（K1）、全屏 light pass GGX（抄 model_pbr）、`RGL_DEFERRED` forward/deferred 切换 | **✅ 核心完成** | ①light pass **简化 IBL**（现仅平 ambient）②选路走 `render_graph.lua` 两套 desc（现 env）③**skybox 背景合成**（deferred 图缺 → 背景 clear 黑） |
| **3** 多灯 + 灯数据并入反射（D3） | `lights.slang`+`lights_schema.lua`+第 4 条反射管线生成 `GpuLight`、scene data 尾区灯数组 @262272（头 @0-128 不动保 forward diff0）、`SceneLights` context + `lights` 配置列表、`light.frag` 逐灯循环 + 点光线性衰减 | **✅ 核心完成** | 灯 entity 化（ECS 规范 3，现 `SceneLights` context）；forward `LightDataGpu@0` 头仍手工双源（为 diff0 未并反射）；shadow map |
| **4** CPU frustum + AABB culling | gltf 逐图元 node-local 包围球 → `CullSphere` 组件、Gribb-Harman 视锥 6 面（Vulkan z∈[0,1]）、render_list 逐 entity 变换测 6 面、可见统计 | **✅ 核心完成** | 真 AABB（现球近似，够 frustum）；GPU culling（批次 5=7b） |
| **5** GPU culling / DGC | `vk_indirect_commands`、compute cull、NVIDIA/RADV 双路 | **⬜ 未开工** | 按 D5 归 **Phase 7b** |
| **6** 文档销账 | — | **🔶 部分** | 完整销账待 4–5 落地 |

**净结论**：A 块（延迟渲染）**单灯核心（批次 0–2）+ 多灯（批次 3）+ CPU culling（批次 4）跑通**——deferred 逐灯循环（定向+点光、
per-light 颜色、距离衰减）headless 零 VVL error、forward 只读 `lights[0]` diff0 不变（3 灯截图验证：红/青
点光延迟可见、远景建筑出衰减范围不受点光、forward 无点光）；灯数据经反射管线 4 生成 `GpuLight`（部分清偿
Phase 5 `LightDataGpu` 双源债）；CPU frustum 剔除（node-local 球 × modelMatrix 测 Gribb-Harman 6 面）
默认视角 1571/20、背向 0/1591、开剔除图像与不剔完全一致（保守球不误剔）。大头剩余 = 批次 5（GPU culling=7b）
+ 批次 2 三个 refine（deferred IBL、Lua 选路、skybox 合成）+ Phase 8 法线/ORM 贴图采样。

### 目标
支持延迟渲染路径，与 forward rendering 并存。

### 要做的事
- **G-buffer 多 pass**：
  - Position pass、Normal pass、Albedo/Roughness/Metallic pass
  - 需要重建 dynamic rendering 的 attachment 配置
- **Light pass**：
  - 使用 compute shader 或 full-screen mesh shader
  - Light culling（frustum + AABB）
  - **多灯 + 光数据并入反射管线**（清偿 Phase 5 债，见 AGENTS.md 已知问题）：`LightDataGpu` 字节布局现状双源手工一致（`vkresources.cppm` 手写 struct + `static_assert(48)` ↔ `scene.slang` 字面量，无 schema 生成——唯一绕过反射管线的主要 CPU↔GPU struct 契约）；多灯布局重设计时接入反射管线（schema 声明→生成布局常量，同 `model_schema.lua` `constants` 段先例）。灯从 registry context 升格为灯 entity + `LightData` 组件（ECS 规范 3：画不画 = 实体有无；单灯 context 是 Phase 5 显式范围决策，scene.cppm:63 注释）。scene data 头 16B 保留区（`kSceneDataHeaderBytes=64`，vkresources.cppm:42）即为此留的前向兼容余量
- **Final pass**：combining G-buffer + lighting
- **AABB / Frustum Culling**：延迟渲染的前提优化
- **GPU Culling 子系统（`VK_EXT_device_generated_commands`）**：
  - culling compute（frustum + AABB）输出 per-visible-mesh sequence 数据（shader slot 索引 + `DrawArgs` + mesh task count）+ sequence count
  - NVIDIA 路径：`vkCmdPreprocessGeneratedCommandsEXT` → `vkCmdExecuteGeneratedCommandsEXT`，layout = execution-set（shader object 模式，per-sequence 换 material shader）+ push data（per-sequence `DrawArgs`）+ `DRAW_MESH_TASKS` token
  - RADV 回退路径：当前 RADV 仅支持 compute 侧 indirect execution set（graphics shader binding 未开），同一份 culling 输出改走 CPU 侧 `vkCmdDrawMeshTasksIndirectCountEXT`（属 `VK_EXT_mesh_shader`，双驱动可用）
  - 基础设施：`features.cppm` 列表加回扩展（2026-08-22 已移除，A/B 实测零回归）+ `features_config.json` 增加 `deviceGeneratedCommands` feature → 重新生成 `features_generated.cppm`；`DeviceContext` 查询 `VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT` 派生能力位（`supportsGraphicsIndirectCommands`，运行时选路）
  - 新模块 `vk_indirect_commands.cppm`（layout + execution set + preprocess buffer RAII，sync2 barrier）
  - 调研依据（双驱动实测对比、API 表面、驱动不对称、集成点）：`docs/device_generated_commands_research.md`

### 设计决策
- 延迟渲染作为可选渲染路径，不破坏现有 forward 管线
- AABB 组件在 Phase 2（glTF）时就可以开始积累
- GPU culling 是能力门控路径：同一 culling compute 输出驱动两条提交路径（NVIDIA device-generated / RADV CPU IndirectCount），驱动能力升级后自动切换，不改 culling 逻辑

---

## 阶段依赖关系

```
Phase 0 (Reflection)
    ↓
Phase 1 (Camera+Input) ← Phase 2 (glTF) 可并行
    ↓
Phase 3 (Textures)
    ↓
Phase 4 (Multi-Shader)
    ↓
Phase 5 (Transparency+Light)
    ↓
Phase 5.5 (IBL 简化版 + Skybox)
    ↓
Phase 6 (Lua Render Intent)
    ↓
Phase 7 (Deferred Rendering)
```

---

## Legacy 参考映射

| 新功能 | legacy 对应文件 | 复用程度 |
|--------|----------------|---------|
| Shader Reflection | `legacy/ShaderReflection/` | 参考思路，重写代码 |
| Camera | `legacy/app/CameraECS.hpp` | 参考思路，重写代码 |
| Input | `legacy/app/ControllerECS.hpp` | 参考思路，重写代码 |
| Model Import | `legacy/render_core/gl/ModelImporter.hpp` | 参考思路，换 tinygltf |
| Texture | `legacy/render_core/gl/GLTextures.hpp` | 参考思路，适配 descriptor heap |
| Render Queue | `legacy/render_core/gl/RenderQueue.hpp` | 参考思路，适配 ECS |

---

## 注意事项

1. **Reflection 输出是 `.cppm` 而非 `.hpp`**：整个项目用 C++20 modules，生成物必须适配 module 体系
2. **Schema 和 Lua 配置是两套机制**：schema 在校验 shader 契约（build time），Lua 配置描述场景意图（runtime），不要混淆
3. **Phase 3 后删除 legacy/ShaderReflection/**：reflection 功能被新管线替代后清理
4. **imgui 优先级低**：Phase 4 之后考虑加入，用于调试和参数调整
