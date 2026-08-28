# vulkan_practice 路线图

> **状态**：现行（开发路线图；各 Phase 完成情况随提交更新，当前节点 = Phase 3 纹理管理）

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

> **实施计划**：见 [phase2_plan.md](phase2_plan.md)（2026-08-23 规划评审，含已确认决策、
> 事实核查、可执行分步、文件变更清单与风险）。
>
> **ECS 设计规范（参照系）**：本 Phase 起的场景代码必须符合五条 ECS 规范
> （registry 唯一数据源 / 纯数据组件 / 数据驱动场景构成 / 分层 / 资产-实例分离）——
> 摘要在 `AGENTS.md` 代码规范 §ECS 设计规范，合规状态与逐条处置记录权威记录在
> [ecs_compliance_review.md](ecs_compliance_review.md) §ECS 设计规范。

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
> 标准布局 padding、构建内存）见 [phase2_plan.md §7](phase2_plan.md)。

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
> 三驱动验证见 [heap_texture_research.md](heap_texture_research.md)；真实 4K 纹理模型
> cottage 257MB/392 primitive/11 张 4096² 复验 PASS）。
> 计划-实现偏离注记（实现为准，AGENTS.md 模块表为现状权威记录）：
> - 纹理数据归属 `Resources::AssetTable`（`TextureAsset`/`MaterialAsset` 容器），
>   **TexturePool 类未落地**——2026-08-28 重构批次 1 把 main 局部 TexturePool 对象
>   （GPUVM fault UAF 根因，heap_texture_research.md §12.6/§12.7）整体降为自由函数
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
> 完整记录见 [heap_texture_research.md](heap_texture_research.md)。

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

> **状态（2026-08-28）**：**已完成**（4 批次，详见 `docs/phase4_plan.md`）。前置债已清偿：
> ① 管线表 per-primitive 条目去重（392→按 (规格, 顶点 slot, 纹理 slot) 组合 = 12 条）；
> ② `renderListSystem` AssetKind 分支归一（统一 `DrawArgs` 超集，零 AssetKind 分支）。
> ShaderRegistry 是两者的统一落点。

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
- **pbr 占位 frag**：`model_pbr.frag.slang` 输出与 model.frag 完全一致（material 字段已接线
  进 `DrawArgs.roughnessMetallic`，光照留 Phase 5）——非真 PBR 光照
- **Lua = 风格映射**（非渲染意图）：scene.lua `styles` 表只做"风格名→管线规格名"映射，
  非 Phase 6 的渲染意图描述（entity 列表/pass 序列）
- **Material 走 push constant 超集**（非独立 binding）：材质字段（baseColor/roughness/
  metallic）进统一 `DrawArgs` 超集（96B scalar），非独立 descriptor binding
- **顶点拼接 D5 超计划新增**：分块拼接 buffer（16MB×6 块）+ per-draw vertexOffset 是
  计划外新增（为去重服务的 heap slot 收敛）
- **cottage 偶发 artifact（已知问题）**：特定区域纹理偶发采样黑（~741px，非确定性驱动
  竞态，复现率 1/13，非批次 2/非 frag 特有，grass 正常）——接受推进，三个待查项见
  AGENTS.md 已知问题 + phase4_plan.md 批次 2 定案注记

### 设计决策
- 每个 shader 风格有独立的 `schema.lua` 和生成模块（实际：material.slang 统一契约 +
  各风格 frag 复用）
- `shader.cppm` 支持多 pipeline，通过 shader registry 切换
- **前置依赖**：先完成 Phase 2 的"前置：Shader/Renderer 解耦"两步
  （动态状态外移 + 管线表），ShaderRegistry 在其接缝上注册
- Lua 配置和 shader reflection 是两套独立机制，不要混淆

---

## Phase 5：透明排序 + 简单光照

### 目标
渲染透明物体和基础光照，提升视觉效果。

### 要做的事
- **透明排序系统**：
  - 每帧根据 camera 位置对透明 entity 按距离排序
  - 排序结果暂存，renderSystem 按排序顺序渲染透明队列
- **Directional Light**：
  - 加入 `LightComponent`（direction、color、intensity）
  - 至少支持 ambient + diffuse
  - push constant 或 descriptor heap 传递 light 数据
- **Shadow**：暂不做（属于延迟渲染或后处理范畴）

### 设计决策
- 透明排序在 `ecs.cppm` 的 renderSystem 中做，不单独建 system
- 光照数据通过 push constant 传递（轻量），后续可改为 descriptor heap

---

## Phase 6：Lua 渲染意图（架构升级）

### 目标
用 Lua 描述渲染意图，renderer system 将其转换为可执行序列。

### 要做的事
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

---

## Phase 7：延迟渲染（独立分支）

### 目标
支持延迟渲染路径，与 forward rendering 并存。

### 要做的事
- **G-buffer 多 pass**：
  - Position pass、Normal pass、Albedo/Roughness/Metallic pass
  - 需要重建 dynamic rendering 的 attachment 配置
- **Light pass**：
  - 使用 compute shader 或 full-screen mesh shader
  - Light culling（frustum + AABB）
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
