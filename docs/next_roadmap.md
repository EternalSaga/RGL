# vulkan_practice 路线图

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

---

## Phase 2：glTF 加载

### 目标
可以加载外部 3D 模型，替代硬编码的 cube。

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

---

## Phase 3：纹理管理

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

### 目标
支持多种 shader 风格（unlit、pbr），通过 Lua 描述渲染意图。

### 要做的事
- **types.slang 扩展**：加入 `Material` struct（baseColor、roughness、metallic 等）
- **generate_layout 扩展**：为 Material 生成对应的 C++ struct
- **schema 扩展**：校验 Material struct 的字段和布局
- **ShaderRegistry 模块**：
  - 注册不同 shader 风格（cube_unlit、cube_pbr 等）
  - 每个 shader 有独立的 `.cppm` 生成物
  - 渲染时根据 material 类型选择 shader
- **Lua 配置**（初步）：
  - 描述场景使用的 shader、entity 列表、渲染参数
  - 启动时读取，构建 renderer state
- **imgui** 集成（优先级不高，后期加）：
  - 用于调试 overlay、参数调整
  - 不影响渲染管线

### 设计决策
- 每个 shader 风格有独立的 `schema.lua` 和生成模块
- `shader.cppm` 支持多 pipeline，通过 shader registry 切换
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

### 设计决策
- 延迟渲染作为可选渲染路径，不破坏现有 forward 管线
- AABB 组件在 Phase 2（glTF）时就可以开始积累

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
