# vulkan_practice

Vulkan 现代特性学习项目。Mesh shader 渲染管线，使用 ShaderObject + DescriptorHeap + DynamicState。  
核心代码位于vulkan_practice目录下。  
legacy目录下的代码是陈旧的OpenGL项目已经不是当前工作重点，一般不用看，但是有些思路可以进行参考。  

## 给大模型
优先使用中文回复用户，除非用户说使用英文回复。

## 构建

项目使用 `just` 管理构建流程（参见 `Justfile`），支持 Linux 和 Windows 双平台。

### Linux

```bash
just config              # CMake 配置（默认 clang, Release）
just config gcc Debug    # 指定编译器和构建类型
just build               # 增量编译（需先 config）
just build-release       # 一键配置 + 编译 Release
just build-debug         # 一键配置 + 编译 Debug
just run                 # 运行程序（自动检测：AMD VGA 存在时设置 RADV_EXPERIMENTAL=heap）
just run-igpu            # 强制核显（AMD 集显原生渲染，无 offload）
just run-dgpu            # 强制独显（NVIDIA prime offload，PROVIDER=3）
just run-intel           # 强制 Intel dGPU（anv 驱动，需 mesa>=26.2 默认开 descriptor heap）
just clean               # CMake 增量清理
just clean-all           # 删除整个 build 目录
just all                 # clean-all + build-release
```

### Windows (NVIDIA + LLVM 22)

**前置要求：**
- **LLVM 22**（`winget install LLVM.LLVM`，提供 `clang++`、`llvm-rc`）
- **Vulkan SDK**（`winget install LunarG.VulkanSDK`，提供 `slangc`、Vulkan 头文件/库）
- **just**（`winget install Caseycasey.Just`）
- **vcpkg**（`git clone https://github.com/microsoft/vcpkg`，已 bootstrap）
- **Git for Windows**（提供 `sh.exe`，just backtick 求值依赖）

```powershell
# just 在 Windows 上需要 sh.exe 在 PATH 中（Git Bash）
$env:PATH = "C:\Program Files\Git\usr\bin;$env:PATH"

just config              # CMake 配置（默认 clang++, Release）
just config Debug        # Debug 构建
just config msvc         # 使用 Visual Studio 18 2026 generator（替代 Ninja）
just build               # 增量编译
just build-release       # 一键配置 + 编译 Release
just build-debug         # 一键配置 + 编译 Debug
just run                 # 运行程序
just clean               # CMake 增量清理
just clean-all           # 删除整个 build 目录
```

Windows 构建使用 LLVM 22 clang++ + Ninja（默认）或 Visual Studio generator（`just config msvc`）。vcpkg triplet 为 `x64-windows-static`（静态链接 CRT）。

> **注意：** Windows 上 `just run-igpu`/`run-dgpu` 不可用（Linux prime offload 专用）。Windows NVIDIA 直接 `just run` 即可。

### vcpkg 跨平台依赖

`vcpkg.json` 使用 [platform expressions](https://learn.microsoft.com/vcpkg/concepts/configuration#platform-expressions) 按 OS 条件启用 features：
- **Linux**: `sdl3[vulkan,wayland]`（wayland 仅 Linux 可用）
- **Windows**: `sdl3[vulkan]`（无 wayland）

同一依赖条目可出现两次，用 `"platform"` 字段区分。vcpkg 按当前 triplet 自动选择匹配的条目。

### 构建系统内部

CMake 无独立构建目录 — 必须从根目录构建（`cmake -B build -S .`）。

产物: `build/vulkan_practice/vulkan_practice.exe`（Windows）/ `build/vulkan_practice/vulkan_practice`（Linux）

> **运行要求：** 必须从 `build/vulkan_practice/` 目录运行（CWD），`just run` 已自动 cd 并检查资源完整性。

## 运行要求

- **CWD 必须是 `build/vulkan_practice/`**：程序按相对路径读取 `configs/`（日志 JSON、相机 Lua 配置）和 `generated_shaders/*.spv`，在其他目录启动会报 "Log config file not found"
- 运行时资源由构建自动同步到 build 目录：`configs/` 增量拷贝（`copy_if_different`，修改 config 后下次 `just build` 即生效，无需 relink；新增 config 文件需 `just config`）、SPV 直接编译到 `build/vulkan_practice/generated_shaders/`
- `RADV_EXPERIMENTAL=heap` — RADV 驱动下启用 `VK_EXT_descriptor_heap` 必需，`just run`/`run-igpu` 已自动设置

## 命名规范

**所有 C++ 符号均在 `namespace RGL` 中。** 每个 `.cppm` 模块的导出均包裹在 `namespace RGL { ... }` 内。`main.cpp` 使用 `using namespace RGL;` 简化写法。

初始化层类之间的跨模块引用（如 `DeviceContext` → `AppContext`）使用 `RGL::` 前缀，因为每个类本身已在命名空间内。调用工具模块的函数/类型（如 `RGL::pickPhysicalDevice()`、`RGL::QueueFamilyManager`、`RGL::FeatureChain`）同样使用 `RGL::` 前缀。

## 代码规范

### 模块优先

所有新代码**必须**使用 C++20 modules (`.cppm`)，而非传统的 `.hpp/.cpp` 头文件对。
- `.cppm` 模块声明于 `namespace RGL { ... }` 内
- `main.cpp` 等入口文件通过 `import` 导入模块
- 不要在模块外部使用 `#include` 引入项目内部头文件

### 优先使用 rlstd 代替 #include STL

**所有 `.cppm` 模块必须 `import rlstd;` 获取 STL 类型，而非在 GMF 中 `#include` STL 头文件。**

- GMF 中只保留第三方库头文件（Vulkan、SDL、glm、fmt 等）
- 所有 STL 类型（`std::vector`、`std::string`、`std::map` 等）通过 `import rlstd;` 获取
- **例外：** `<ranges>` 的 `std::views::` 因 libstdc++ 定义为 namespace alias 无法 re-export，需保留 `#include <ranges>`；`<filesystem>` 同理
- 新加 STL include 时，先检查是否应补入 `rlstd.cppm` 而非直接加到 GMF

### 函数式 Ranges

优先使用 C++23 `std::ranges` 进行函数式编程：
- 使用 `std::views::filter`、`std::views::transform` 等视图适配器
- 使用 `std::ranges::for_each` 等算法替换原始循环
- 链式组合视图操作，避免中间容器

### 成员变量命名

成员变量使用自然命名法，不加 `_` 或 `m_` 前缀后缀：
```cpp
class Foo {
    int count;      // ✅
    int _count;     // ❌
    int m_count;    // ❌
    int count_;     // ❌
};
```

### ECS 设计规范

场景代码（ecs/scene/model_importer + 相关 system）必须符合五条 ECS 规范
（合规状态与逐条处置记录：`docs/ecs_compliance_review.md` §ECS 设计规范，权威记录）：

1. **registry 是场景唯一数据源**——场景状态不在 registry 外平行维护
   （每帧/配置状态进 context；资产数据进 `Resources::AssetTable`）
2. **entity 是 ID、component 是纯数据、system 是逻辑唯一载体**——system 间经数据通信，
   执行顺序显式（显式 system 列表；不用 entt::scheduler——本地 3.16 是 process 调度器，
   执行顺序无保证）
3. **场景构成数据驱动**——"场景里有什么" = registry 里有什么 entity，
   不用全局变量 / config 字段 / 构造参数决定场景构成（画不画 = 实体有无）
4. **分层**——底层 Vulkan 类不依赖 ECS、不认识具体资产（Renderer 零 entt 依赖）；
   渲染 system 产出抽象 draw list（`FrameDrawList`），执行器只盲执行
5. **资产与场景实例分离**——组件引用稳定资产 ID（`MeshRef.asset` / `MaterialRef.material`），
   不引侧数组下标；五方一致性（entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args）
   收敛为资产表一处映射

### 构建工具模块

`tools/` 下的构建工具也统一使用 C++20 modules：

| 模块 | 文件 | 用途 |
|---|---|---|
| `schema_loader` | tools/schema_loader.cppm | Schema 结构体 + Lua schema 加载 |
| `validator` | tools/validator.cppm | 将 reflection JSON 与 schema 校验一致性 |
| `generator` | tools/generator.cppm | 使用 inja 模板生成 `.cppm`，接入 `rllogger`。新增 `renderTemplate()` 支持任意 JSON + inja 模板的渲染 |

`reflection_tool` 通过 `import rllogger;` 使用项目统一日志库，替代 `std::cout`/`std::cerr`。

## 模块结构（C++20 modules, .cppm）

所有导出均在 `namespace RGL` 内。

### 初始化层（构造顺序 = main.cpp 顺序）

| 模块 | 文件 | 用途 |
|---|---|---|
| `vk_instance` | vkinstance.cppm | Context、Instance、DebugMessenger、apiVersion |
| `vk_device` | vkdevice.cppm | PhysicalDevice 选择、QueueFamilyManager、Device、CommandPools、FeatureChain |
| `vk_swapchain` | vkswapchain.cppm | Surface、Swapchain、ImageView[] format/extent/presentMode |
| `vk_resources` | vkresources.cppm | VmaAllocatorRAII、depth image+view、VmaDescriptorHeap（heap 容量 = reserved + 8 + `kUserSlotMargin(1024)` 固定裕量，与场景内容解耦）、`ensureSharedSampler()`（共享 sampler 描述符，幂等，slot 归 Resources 成员）、统一 `allocSlot` 分配器（布局由 schema 生成偏移常量钉死 + 构造期契约断言）、**`AssetTable`**（资产层三容器：`MeshAsset{kind, shared_ptr<VmaBuffer> vertexBuffer, heapSlot, triangleCount, vertexOffset, groupCount, pipelineIndex, 可选 indexBuffer/indexHeapSlot}`（kind=Model/Cube，**vertexBuffer 拼接顶点 buffer 的 shared_ptr**（Phase 4 批次 2：同模型 N 个 primitive 共享，**vertexOffset** = 块内字节偏移进 push constant）+ **groupCount 上传期按 shader 契约派生**（Phase 4 D6：model=ceil(tri/kTrianglesPerGroup)、cube=1 固定、0=不渲染，renderListSystem 每帧零推导）；cube buffer 构造体 move 进记录经 `cubeAssetId()` 访问）/ `TextureAsset{VmaImage, view, heapSlot, 描述符指针链}`（deque 存储，元素地址稳定 = 指针链稳定）/ `MaterialAsset{baseColor, textureId, alphaModeBlend, metallic, roughness, unlit}`（按 glTF materialIndex 去重，**Phase 4 批次 3 +metallic/roughness/unlit 风格字段**）；资产表 +`modelSlotCount` 计数（Phase 4 批次 2 拼接 buffer 后模型顶点 slot 数，白纹理契约用）；registry entity 经 `MeshRef{asset}`/`MaterialRef{material}` 引用，五方一致性收敛为本表一处映射） |
| `vk_renderer` | vkrenderer.cppm | `DrawCall`/`FrameDrawList` 绘制契约 + 同步对象、命令缓冲；持 `ShaderRegistry&`（Phase 4 批次 2：管线表迁出，零管线表成员）；`pipelineAt()`/`pipelineIndex()` 委托 registry；`drawFrame(const FrameDrawList&)` 盲执行单循环（不认识具体场景资产，零 entt 依赖，逐 draw 经 `entry.pushDataSet` 显式 push set + `entry.argsSize` push `DrawArgs`）。push constant 统一 `DrawArgs` 超集（scalar 布局 96B，见 `material_types_generated`） |
| `shader_registry` | shader_registry.cppm | 管线表（Phase 4 批次 2 从 Renderer 迁出，零 entt——只认 slot/资产 id，不认场景实体）：`PipelineEntry{name, RenderPipeline, argsSize=96B, pushDataSet=0}` + `ShaderRegistry`（构造即建 "cube" 条目恒建 + 收 `PipelineSpec` span（scene.lua 风格名→SPV 规格）；`addModelEntry(specName, vertexSlot, textureSlot, samplerSlot)` 资产上传期动态加入模型条目，**按 (规格,顶点slot,纹理slot) 组合去重**——binding mapping 是 shader object 级状态，同组合共享一条条目（392 primitive → 12 条）；`bindToAsset`/`bindPipelineToAsset` 条目下标写回资产记录（启动期一次）；`pipelineIndex(name)` name→下标、`entry(index)`/`pipelineAt(index)` 下标→条目/管线（llm_debug 离屏重渲用）） |

### 工具模块

| 模块 | 文件 | 用途 |
|---|---|---|
| `vulkan_utils` | utils.cppm | 层检查、debug messenger、GPU 选择器、barrier 辅助、present mode |
| `vkerror` | vkerror.cppm | `Error`/`ErrorCode` 类型定义、`fromVkResult` |
| `ecs` | ecs.cppm | `WindowExtent`/`AppExit` context、eventSystem（中央 SDL 事件轮询：窗口/退出 → `WindowExtent`/`AppExit`，输入 → `InputEvents` 事件流（只产事件，不做键位映射——键绑定在 `Camera.keys`））/resizeSystem（extent 比较重建 swapchain+depth）/renderSystem 帧循环（**显式 system 列表**：camera→animation→bounds→projection→renderlist，注册顺序 = 数据流契约；未用 entt::scheduler——本地 3.16 版是 process 调度器、执行顺序无保证）+ `renderListSystem`（**Phase 4 归一**：单一 `DrawArgs` 超集按资产 id 索引写入，零 AssetKind 分支，H1 解决） |
| `scene` | scene.cppm | 场景组件（`Transform`/`Relationship`（手写单向 parent，entt 3.16 无内置；children 由 view 过滤派生）/`MeshRef`（资产 id，引用 `Resources::AssetTable`）/`MaterialRef`（**真引用** `{material}` 资产表下标，批次 2，E5.3）/`Bounds`）+ `registerCubeEntity`（批次 3：cube 一等资产——Transform(identity)+Bounds(顶点半径派生)+MeshRef+MaterialRef+Relationship(根)，画不画 = 实体有无）+ 每帧 context（`SceneSpin`/`SceneBounds`/`ViewProjection`） |
| `scene_config` | scene_config.cppm | `PipelineSpec`/`SceneConfig`（+`styleMap` 风格名→管线规格名，Phase 4 批次 3）+ `loadSceneConfig`（`configs/scene.lua`，sol2 沙箱同 camera 模式；styles 表可选回落默认 {unlit,model/pbr,model_pbr}；启动期数据，main 转成 registry 数据后不再进每帧路径） |
| `gltf_loader` | gltf_loader.cppm | glTF CPU 解析（tinygltf v3：POSITION/NORMAL/TEXCOORD_0 严格校验 + 去索引展开 + node translation 扁平化 + baseColorFactor/alphaMode/**materialIndex**（材质身份，-1 = 无材质）/**metallic/roughness/unlit**（Phase 4 批次 3：pbrMetallicRoughness.metallicFactor/roughnessFactor 缺省 1.0 + KHR_materials_unlit 扩展存在性）/images 内嵌纹理数据，输出 `GltfModel` 含包围球） |
| `model_importer` | model_importer.cppm | `uploadModelMeshes`（**Phase 4 批次 2 分块拼接**：每模型顶点拼成 16MB 块（≤6 块）每块一个 BDA buffer + 描述符写 heap slot `reserved+2+块号`（`HeapSlotModelVertexBase`，统一 `allocSlot` + 契约断言），per-primitive 块内字节偏移 = `MeshAsset.vertexOffset` 进 push constant；注册进 `Resources::AssetTable`，返回资产 id 列表）+ `registerScene`（场景根 entity 挂 `Bounds` + 每 primitive 一个 entity，`MeshRef{asset}` + `MaterialRef{material}` 真引用；**材质资产按 materialIndex 去重注册**（+metallic/roughness/unlit）+ 同 materialIndex 字段一致性金丝雀（违例 = loader bug 启动期报错）） |
| `texture_pool` | texture_pool.cppm | 纹理上传**自由函数**（批次 1：TexturePool 类删除，数据归属资产表——main 局部对象 UAF 教训，docs/heap_texture_research.md §12.6/§12.7）：`loadEmbedded`（CPU 解码 + 去重缓存同图同资产）/ `ensureWhiteTexture`（白 1×1 首纹理槽，契约断言 slot = reserved + HeapSlotModelVertexBase + 模型顶点槽数）/ `uploadModelTextures`（每图一个 VmaImage + SAMPLED_IMAGE 描述符写 heap slot + 注册 `TextureAsset`；**per-primitive 有效纹理选择**：有 uv 且图有数据 ? material.textureId : 白——返回值 `primitiveTextureIds` 供管线烘焙） |
| `vulkan_window` | sdl3window.cppm | SDL3 窗口、surface、HDR/SDR 格式选择 |
| `queue_family_manager` | QueueFamilyManager.cppm | Graphics/Compute/Transfer 队列分配 |
| `features` | features.cppm | FeatureChain 类型定义、buildRequestedFeatures、validateFeatures、checkDeviceExtensions、自定义异常 |
| `cube` | cube.cppm | 顶点/索引数据 |
| `camera` | camera.cppm | `Camera`（含 `KeyBindings` 键位绑定）/`InputEvents`（输入事件流：key/mouse button/delta/wheel）/`CameraState`（cameraSystem 自管 level 状态）context、cameraView/cameraSystem（trackball 绕 target 轨道 / FPS 第一人称双模式，消费事件即清空）、loadCameraConfig（读 `configs/camera.lua` 含 `keys` 键位字段，sol2 沙箱 + 可选回落默认） |
 | `shader` | shader.cppm | SPV 加载器、ShaderEffect、RenderPipeline（ShaderObject + 动态状态） |
 | `llm_debug` | llm_debug.cppm | llm_debug 一次性截图（`configs/camera.lua`，agent 自主工作用）：`LlmDebugConfig` + `captureFrame()`——离屏重渲（8-bit UNORM 目标 + D32Sfloat 深度，复用管线表/动态状态/draw list）+ 独立命令缓冲/fence + VMA 回读 + stb PNG（绝对路径打日志）；**`llm_debug` 纯开关（true/false，截图只改这一行）+ `llm_shot` 常驻截图配置**（`{file, size, delay_frames}` 全可选回落默认，改文件名/尺寸只动 llm_shot）；向后兼容 `llm_debug=table` 旧格式（启用+细字段，覆盖 llm_shot）；相机位置无额外字段，直接用 camera.lua 顶层 position/target/fovDegrees |
| `VmaRaii` | VmaBuffer.cppm | VmaBuffer、VmaImage、VmaDescriptorHeap、VmaAllocatorRAII 封装。VmaBuffer/VmaImage 通过 `SharedAllocator` (`std::shared_ptr<AllocatorControlBlock>`) 共享 allocator 所有权 |
| `rlstd` | rlstd/rlstd.cppm | C++ 标准库模块封装。libstdc++ 不支持 `import std`，因此通过 GMF `#include` 所有 STL 头文件并 `export` 常用名称。所有项目模块通过 `import rlstd;` 获取 STL 类型，而非在 GMF 中 `#include` STL 头。注意：`std::views::` 因 libstdc++ 将其定义为 namespace alias 而无法通过模块导出，仍需在 GMF 中 `#include <ranges>` |
| `types_generated` | generated/types_generated.cppm | auto-generated（由 `reflection_tool` + inja 模板生成），导出 `HeapSlotVertexOffset`、`HeapSlotIndexOffset`（相对 heap reserved range 起点的偏移，运行时 slot = `reservedSlots() + offset`）。push constant 契约 Phase 4 移至 `material.slang` |
| `model_types_generated` | generated/model_types_generated.cppm | auto-generated（第二条反射管线：`model.slang` + `model_schema.lua`），导出 `ModelVertex`、`HeapSlotModelVertexBase`、契约常量 `kTrianglesPerGroup`（schema `constants` 段生成，shader 端字面量必须一致）。push constant 契约 Phase 4 移至 `material.slang` |
| `material_types_generated` | generated/material_types_generated.cppm | auto-generated（第三条反射管线：`material.slang` + `material_schema.lua`，Phase 4），导出统一 push constant 超集 `DrawArgs`（**scalar 布局 96B**：mvp/totalTriangles/vertexOffset/baseColor/roughnessMetallic，所有 shader 风格共享，static_assert 钉死）。scalar 布局策略见"着色器编译"节 |
| `features_generated` | generated/features_generated.cppm | auto-generated（由 `reflection_tool render` + `features_config.json` 生成），导出 `FeatureChain`、`buildRequestedFeatures()`、`validateFeatures()` |

 `main.cpp` 先加载 `configs/camera.lua` / `configs/scene.lua`（**先于建窗**：llm_debug 模式影响窗口——离屏隐藏窗 + 配置尺寸），创建栈上 `entt::registry`（持有 `WindowExtent`/`AppExit`/`Camera`/`InputEvents`/`CameraState`/`SceneSpin`/`SceneBounds`/`ViewProjection`/`FrameDrawList` context），按顺序构造 5 个初始化层类（`Renderer` 收管线规格 span，"cube" 条目恒建；heap 容量为固定裕量，与场景内容解耦），CPU 解析模型（Vulkan 初始化之后）+ GPU 上传（写资产表）+ `addModelPipeline` + `registerScene`，按 `showCube` 注册 cube entity + `bindPipelineToAsset`，然后调用 `RGL::renderSystem(registry, vkRenderer, llmEnabled ? &llmCfg : nullptr)`。`vam_impl.cpp` 包含 `#define VMA_IMPLEMENTATION`，`stb_impl.cpp` 包含 `#define STB_IMAGE_WRITE_IMPLEMENTATION`（llm_debug PNG 写出）。

### 构造依赖图

```
SDLWindow → AppContext → DeviceContext → Swapchain → Resources → Renderer
                └────────────── entt::registry（Viewport context）──────────┘
```

main.cpp 按逆序析构（RAII，无需手动清理）。

### 重要成员生命周期规则

- `vkinstance.cppm`：`engineInfo_`、`instanceCreateInfo_`、`layers_`、`instanceExtensions_` 均为类成员 — `pApplicationInfo` 不能悬空
- `vkdevice.cppm`：`featuresChain_` 为类成员 — `DeviceCreateInfo.pNext` 不能悬空
- 所有 `vk::raii::*` 对象必须通过**成员初始化列表**初始化，绝不在函数体内初始化（默认构造函数已删除）
- 成员声明顺序必须与初始化依赖顺序一致

## 着色器编译

`vulkan_practice/shaders/` 下的 Slang 着色器 → SPV 输出到 `build/vulkan_practice/generated_shaders/`。

- CMake `compile_slang()` 函数在构建时处理编译
- Profile: `sm_6_6+spirv_1_6`，capability: `spvDescriptorHeapEXT`
- **Scalar Block Layout（Phase 4 定案）**：所有 slangc 编译（3 条反射管线 + `compile_slang` 实际 SPV）统一加 `-fvk-use-scalar-layout`——push constant 用 scalar（C 自然）布局，C++ struct（glm 自然对齐 4）与 shader 逐字段偏移天然一致、零 padding（`DrawArgs` 96B）。前提：`scalarBlockLayout` feature 已启用（`features_config.json`，`validateFeatures` 校验）。**反射与 SPV 两处 flag 必须一致**（不一致 = 运行时布局错配的隐蔽 bug）；generator 的 `standardLayoutInfo` 已同步为 scalar 对齐（`tools/generator.cppm`）
- 运行时从 `generated_shaders/` 相对路径读取 SPV（输出直接在 build 目录，从 `build/vulkan_practice/` 运行即可，见"运行要求"）

## 隐式扩展依赖

`features.cppm` 中请求的 `VK_EXT_DEVICE_GENERATED_COMMANDS`、`VK_EXT_DESCRIPTOR_BUFFER` 等扩展当前无显式使用，但被其他已启用的扩展隐式依赖。移除它们可能导致驱动层面功能异常，不可随意删除。

`vkinstance.cppm` 中请求的 `VK_EXT_SWAPCHAIN_COLOR_SPACE` 等实例扩展同理。

**KHR/EXT 别名说明：** `VK_KHR_swapchain_maintenance1` promotion 后，`VkSwapchainPresentFenceInfoKHR` 和 `VkSwapchainPresentFenceInfoEXT` 是同一结构（typedef alias, `vulkan_core.h:18411`），sType 值相同。代码使用 EXT 命名形式，功能等价于 KHR。

## 外部依赖（通过父 CMake 链接）

- `rlstd` — `rlstd/` 根目录共享库，C++ 标准库模块封装
- `rllogger` — `logger/` 根目录共享库，`RGL::RLLogger` 单例
- `spdlog` — 通过 rllogger 间接使用
- `glm` — 数学库（强制 depth zero-to-one，弧度制）
- `SDL3` — 窗口/surface
- `VulkanMemoryAllocator` — 内存管理
- `slangc` — 着色器编译器：**FetchContent 拉 Slang 官方预构建**（`cmake/SlangFetch.cmake` pin v2026.16 + sha256，解压 `build/_deps/slang-prebuilt-src/bin/slangc`）；2026-08-27 起替代 vcpkg `shader-slang` port（port 停 2026.7.1，`vcpkg.json` 已移除该依赖）。**Windows 上优先使用 Vulkan SDK 自带的 slangc**（`C:/VulkanSDK/<ver>/Bin/slangc.exe`，由 `SlangFetch.cmake` 自动检测）

## 关键文件位置

| 位置 | 说明 |
|---|---|
 | `vkrenderer.cppm` `drawFrame()` | ①~⑮ 完整流程：双 fence → acquire（**100ms 超时 + eTimeout 跳帧**：compositor 停发 present 时保帧循环存活）→ 录制（barrier/动态渲染/heap barrier+绑定/beginRendering/动态状态/**逐 draw：pushData→bind→drawMeshTasksEXT**/present barrier）→ submit → present（`SwapchainPresentFenceInfoEXT` KHR 别名挂 `presentInfo.pNext`） |
 | `llm_debug.cppm` `captureFrame()` + `configs/camera.lua` `llm_debug`/`llm_shot` | 一次性截图（agent 自主工作，批处理语义无 mailbox）：隐藏窗（`SDL_WINDOW_HIDDEN`）离屏渲染 → 延迟 N 帧 → 离屏重渲 + PNG（绝对路径打日志）→ 退出；相机位置 = camera.lua 顶层字段（无 presets）；**依赖 compositor 存活**（息屏可，suspend 不行）；agent 流程：改 `llm_debug=true`（一行开关，`llm_shot` 常驻 file/size）→ `just run` → 多模态读 PNG |
| `vkrenderer.cppm` `DrawCall`/`FrameDrawList`/`pipelineIndex()` | 绘制契约（2026-08-24 批次 2）：渲染 system 产出 draw list，`drawFrame` 盲执行单循环；管线 name→下标映射构造时建一次，录制路径零字符串查找 |
 | `ecs.cppm` `renderSystem` | 帧循环 + 显式 system 列表（camera/animation/bounds/projection/renderlist，注册顺序 = 数据流契约；eventSystem/resizeSystem/drawFrame 留循环体内直调——持有控制流）+ `LlmDebugConfig*`（可空）一次性截图计数：第 N 帧后 `captureFrame` + 置 `AppExit` |
| `vkrenderer.cppm` `addModelPipeline()` | 模型管线条目（2026-08-25 批次 3，资产上传期每 primitive 一次）：binding mapping 显式烘焙顶点 slot + 条目下标写回资产表，`renderListSystem` 经资产表解析管线（无 `"model_i"` 字符串约定） |
| `ecs.cppm` `renderListSystem` | ViewProjection + 场景实体 → `FrameDrawList`：**Phase 4 归一**——单一 `DrawArgs` 超集按资产 id 索引（`state.args[assetId]`），零 AssetKind 分支（cube/model 共用同一路径，各风格只消费自己字段）；mvp = proj·view·spin·world，资产 id → 资产表取三角形数/`groupCount`（上传期派生）/管线下标，baseColor 经材质资产解析（H1 解决） |
| `vkresources.cppm` `writeDescriptors()` | cube 顶点/索引描述符写入（`vkWriteResourceDescriptorsEXT`，构造期）；模型描述符在 `model_importer.cppm` `uploadModelMeshes` |
| `shader.cppm` `RenderPipeline`/`applyDynamicState` | ShaderObject 包装（`DescriptorSetAndBindingMappingEXT` 映射 heap slot，编号运行时动态）+ 动态状态外移（Phase 7 多 pass 接缝） |
| `shaders/model_schema.lua` `constants` | 契约常量单一真相源（`kTrianglesPerGroup`）：schema 声明 → 生成 `model_types_generated` constexpr，`model.mesh.slang` 字面量必须一致（同 `vertexLayouts` 模式，无反射端对应） |
 | `shaders/material.slang` | **统一 push constant 契约（Phase 4）**：`DrawArgs` 超集（所有 shader 风格共享，scalar 布局 96B）单一真相源；`material_schema.lua` 校验 → `material_types_generated`。mesh shader 同时 import 本文件与自身契约（cube.mesh = import types + material） |
 | `shaders/model_pbr.frag.slang` | **pbr 占位 frag（Phase 4 批次 3）**：输出与 model.frag 完全一致（`drawArgs.baseColor.rgb × texel.rgb`），baseColor 已接线、`roughnessMetallic` 契约字段 CPU 侧写占位 0（mat.roughness/metallic 接线留 Phase 5 真实光照）；mesh 复用 model.mesh.spv，CMake 独立 compile_slang 行 |
 | `shaders/types.slang` | cube 契约：只拥有 buffer slot 段（vertex/index 均 `ByteAddressBuffer`，绝不用 `StructuredBuffer`）；push constant 已移至 material.slang |
 | `shaders/cube.mesh.slang:19` | `mul(drawArgs.mvp, ...)` MVP 变换 |
 | `configs/camera.lua` / `configs/scene.lua` | 运行时配置（`camera.cppm`/`scene_config.cppm` 经 sol2 沙箱加载，字段可选回落默认值；scene.lua `styles` 表 = 风格名→管线规格名映射，Phase 4 批次 3）；**与 `shaders/*_schema.lua`（build 时 shader 契约）是两套机制，勿混**；改后 `just build` 生效 |
| `ecs.cppm` `eventSystem` | 中央 SDL 事件轮询（唯一 `SDL_PollEvent` 调用点）：输入 → `InputEvents` 事件流（只产事件，键位映射在 `Camera.keys`）；鼠标**增量用 `motion.xrel/yrel`**（`x/y` 是窗口绝对坐标，SDL3 陷阱，见 `docs/调试历史.md`） |

## 已知问题

- `computeCommandPool`、`transferCommandPool`、`computeQueue`、`transferQueue` 已创建但保留给未来使用
- `configs/` 增量拷贝（`copy_if_different`）只认"源文件更新"：build 端副本被破坏/截断（mtime 更新）不会自动恢复，需手动拷贝或 `just clean`
- **ECS 已知债**（2026-08-28 重构批次 1-3 后剩余：资产表/MeshRef 资产 ID/构造参数去耦合/输入事件化/Relationship 单向化/材质资产表/cube 实体化已落地，详见 `docs/ecs_compliance_review.md`）：
  - ~~cube 尚非 ECS entity——DrawCall 过渡特例 + `showCube` 构造期 gating~~（**2026-08-28 重构批次 3 已解**：cube 一等资产 + `registerCubeEntity`，画不画 = 实体有无；"cube" 管线条目恒建）
  - ~~`MaterialRef` 名为 Ref 实为值拷贝~~（**2026-08-28 重构批次 2 已解**：`MaterialRef{material}` 真引用 + `MaterialAsset` 资产表，E5.3）
  - ~~push constant 布局未归一：`renderListSystem` 留 `AssetKind` 分支（Cube→`DrawArgs` 64B / Model→`ModelDrawArgs` 96B）~~（**Phase 4 批次 1 已解**：统一 `DrawArgs` 超集（scalar 布局 96B，`material_types_generated`）+ `renderListSystem` 单一写入路径零 AssetKind 分支 + `PipelineEntry.pushDataSet` 显式化（L5）；scalar 布局策略见"着色器编译"节，H1 注记解决，见 `docs/ecs_compliance_review.md`）
  - ~~管线表 per-primitive 条目（392 条）未去重~~（**Phase 4 批次 2 已落地 2026-08-28**：`shader_registry.cppm` 组合去重 + 分块拼接顶点 buffer（16MB×6 块）+ per-draw `vertexOffset`，392 primitive → 12 条目 / 6 heap slot）。~~**cottage 偶发 artifact（接受为已知问题推进，决策 2026-08-28 晚）**~~（**2026-08-30 已解，根因 = 两个自 Phase 2 就存在的共享代码越界 bug，Windows/NVIDIA 移植暴露**）：先前（08-28）"非确定性驱动竞态、复现率 1/13"的定性是**误诊**。真根因两处：① **`gltf_loader.cppm` uv 越界读**——去索引展开写 `uv[idx*3+0/1]`，但 uv 是 vec2（步长应为 2，正确 `uv[idx*2+…]`），idx > 2V/3 即越界读相邻堆垃圾（非确定性）→ 特定区域纹理采样为黑 = 741px artifact 根因（"偶发"= 相邻内存内容随机）；Windows 大模型越界读跨页 = AV 崩溃（0xC0000005，"灰屏/无响应"直接原因）。② **`model.mesh.slang` triangles 索引越界**——`base=(firstTriangle+ti)*3` 是全局顶点索引，但 out indices 必须引用本组 `verts[0..vertexCount-1]`（≤29），正确 `base=ti*3`；group≥1 越界引用 → 几何崩形（拉长条）。`firstTriangle*3=30·groupIndex` 恰为每组 30 顶点的整数倍，RADV 对越界 index 做 mod 回绕恰好绕回正确值 → Linux 几何"看似正常"；NVIDIA 越界 = clamp/垃圾读 → 暴露。三个待查项重新归因：① 根因 = 上述 ①②（非驱动竞态）；② VVL heap 双写 = 排查扰动因素，非根因；③ lavapipe ICD 崩溃 = 独立环境问题，与 artifact 无关。教训记录见 `docs/phase4_plan.md` 批次 2 定案注记（2026-08-30 重写）
  - ~~无统一 slot 分配器：cube 固定 0/4、model 固定 base 8+i~~（2026-08-28 勘误：统一 `allocSlot` 已落地，布局 = cube 顶点 0/索引 1、模型 base 2+i、纹理其后，全由 schema 生成偏移常量钉死 + 构造期契约检查；`HeapSlotModelVertexBase=2` 等见 `generated/model_types_generated.cppm`，管线 mapping 显式传 slot）
- ~~**RADV 4K 大纹理黑屏（驱动限制，2026-08-27 定位，未解）**~~：Strix Halo APU 上 **64MB（4096²）sRGB 纹理 + 渲染目标 <2048 → 全黑 / GPUVM TCP fault**；目标 ≥2048 或纹理 ≤16MB 均正常（真实纹理 256×512 级不受影响，Phase 3 M3 可用）。~~根因 = 56 位 VA 截断~~ **2026-08-28 修正**：GPU VA 实际 **48 位**（`vm_size=-1` auto，boot 日志 `vm size is 262144 GB`；`high_va_offset` 只是固件能力值），fault 地址是 48 位内合法高区地址 + **PTE 权限错**（PERMISSION_FAULTS=3）= 内核页表状态 bug，非编码问题。~~已排除 VVL/分配时序/VMA 子分配/内存池/heap 容量/shader 变体/swapchain 扩展~~ **2026-08-28 已解（定性 = 同根 UAF 复用）**：~~（驱动限制，未解）~~ §十 全部观测在 UAF 修复前构建上取得（TexturePool 块内声明，纹理 BO 第 0 帧前释放，见上条"已解"项）——"high VA BO"= 已释放 VA 复用后的分配；**2026-08-28 傍晚遗留验证 PASS**：master 上真实 4K 材质模型（cottage 257MB / 392 primitive / 11 张 4096² 64MB 内嵌纹理）+ §十 精确配置（llm 1280×720 目标 <2048）+ 可见窗两路复跑均正常零 fault → §十"内核 gpuva high VA + 48 位编码"定性整体改写为 UAF 复用，**上游 bug report 不发**。完整记录见 `docs/heap_texture_research.md` §十（12.6 勘误注）+ **§12.6/§12.7**
- ~~**交互模式 GPUVM fault（状态漂移 + per-process，2026-08-28 定位，未解）**~~ **已解（2026-08-28 下午，根因 = TexturePool 生命周期 UAF）**：e2015c6 及之前（7e1abce/afe2ff0 同款）`TexturePool` 声明在 main() 的 if 块内，块尾析构释放**纹理 BO + 描述符存储** → 第 0 帧起 GPU 采样已释放 VA；"状态漂移/分钟级翻转/per-process" = **UAF 非确定性**（VA 未复用 = 读旧页渲染正常"好状态"，被复用为权限不同 BO = TCP fault"坏状态"）；**2de6731 把声明提出 if 块即修复**（commit message 误标 slot 顺序）。m3-scene 分支现场复现 + 最小修复单变量实锤（e2015c6 原样可见窗确定性 fault 0x8001000fa000/fb000 = §十一签名同模式；仅生命周期修复 → 3 轮正常）；group 数嫌疑失去意义。完整记录见 `docs/heap_texture_research.md` §十一（勘误注）+ **§12.6**

## 其他文档

> **状态标记**：每篇文档头部带一行状态（`现行` / `历史` / `调研`）。历史文档为时点快照（行号已漂移），现状权威记录以本文件 + `next_roadmap.md` 完成情况为准；引用历史文档只取设计思路，勿按行号定位代码。

- `docs/技术概念.md` — 核心 Vulkan 模式、设计决策、shader 缓冲访问规则
- `docs/调试历史.md` — 灰屏问题修复时间线、已解决的争议
- `docs/DESCRIPTOR_HEAP_SPEC.md` — 有关DescriptorHeap 扩展的排坑详细记录
- `docs/next_roadmap.md` — 后续开发计划
- `docs/phase2_coupling_review.md` — 【历史】Phase 2 耦合审查（2026-08-23 问题清单 + 修复方向；条目处置权威记录在 ecs_compliance_review §处置记录，行号已漂移）
- `docs/ecs_compliance_review.md` — **ECS 设计规范（五条参照系，场景代码设计合规判据）** + 合规审查与修复方向（2026-08-24 审查；批次 2/3 + 2026-08-28 重构批次 1-3 已落地，见 §处置记录；规范摘要在 AGENTS.md 代码规范 §ECS 设计规范；剩余债（push constant 归一 / 管线去重）归 Phase 4）
- `docs/shader_reflection_plan.md` — 【历史】shader 反射设计规划（Phase 0 实现前；实现现状以本文件模块结构表为准）
- `docs/nvidia_compatibility_report.md` — 跨驱动兼容性排查（heap 对齐、Aftermath 集成、mesh shader 崩溃根因；2026-08-27 补 NVIDIA/Intel/AMD Strix Halo 三平台验证记录，当前开发机为 Strix Halo 纯核显机）
- `docs/device_generated_commands_research.md` — GPU culling（VK_EXT_device_generated_commands）双驱动实测调研（Phase 7 依据）
- `docs/heap_texture_research.md` — descriptor heap 纹理采样预研（Phase 3 依据）：spec 条款（SAMPLED_IMAGE 专用 / 内嵌 sampler / 对齐）+ **v341 头与 v1.4.360 spec 的 API 差异**（pAddressRange vs 句柄、4 参 vs 6 参写描述符、pEmbeddedSampler 位置）+ `heap_texture_spike/` 三驱动实测（2026-08-27：RADV/Intel/NVIDIA × 5 变体全 PASS，语义分离三驱动完整实现；Q1 统一步长 `max(buffer,image)`；§九 误诊复盘：标量 `Texture2D<float>` R 通道 splat + `samplerHeapOffset` 字段误用，推翻早期"RADV 只认组合绑定"定性；主工程待改 `model.slang` float4 + `shader.cppm` 映射字段）
- `docs/present_timing_research.md` — present 时序调研（校准时间戳扩展）
- `docs/快捷键配置.md` — Zed 编辑器快捷键、任务和调试配置说明
- 离线 Vulkan 文档（优先于在线检索） - `docs.vulkan.org` / `registry.khronos.org` 有 Cloudflare 拦截，在线抓取不可靠。已在本地磁盘克隆官方 spec 源仓库：
	**位置**：`/home/robin/program/Vulkan-Docs`（本地 NVMe，shallow clone，约 37MB，**当前 v1.4.360**；更新：`git pull` 或重新 `git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Docs.git`）
	**注意**：spec clone 版本（v1.4.360）比系统 Vulkan 头/loader（**v341**，主工程实际编译环境）**新**，部分扩展 API 表面有差异（descriptor heap 最典型，见 `docs/heap_texture_research.md` §二）——写代码/核对结构体字段以系统头为准，spec 只作语义与 VUID 依据；布局：`chapters/` 为顶层目录，`appendices/` 多为 stub

## min_mesh_crash（最小复现子项目）

独立 CMake 的单文件最小复现程序（2026-08 调查 NVIDIA mesh shader 崩溃时建立）：

- `main.cpp` — vertex+fragment 与 mesh shader 的 shader object 测试（GPU 选择写死 vendor 0x10de；强制 RADV 用 `VK_DRIVER_FILES`）
- `shaders/` — `mesh.glsl`（mesh-only，无 task）、`vert.glsl`、`frag.glsl`
- `README.md` — 根因权威记录：mesh shader 对象**必须**带 `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT`（TASK stage 绑 NULL 时），否则 NVIDIA 上 Xid 13/engine reset；VVL 查不出来是 spec 漏洞（VUID 08694 需 taskShader+meshShader 双 feature）
- 构建/运行：`cmake -B build -S . && cmake --build build`，运行需 prime offload 环境变量（见其 README）

## heap_texture_spike（最小验证子项目）

独立 CMake 单文件最小验证程序（2026-08-25，Phase 3 纹理预研）：

- `main.cpp` — descriptor heap 中写 **SAMPLED_IMAGE 描述符**（`pImage`/`VkImageDescriptorInfoEXT`，pView = image view create info）+ **STORAGE_BUFFER 描述符**（`pAddressRange` 裸地址，与主工程一致）共存于同一 resource heap；mesh+frag shader object 经 `pEmbeddedSampler`（内嵌 sampler，无独立 sampler heap）采样离屏纹理，`vkCmdCopyImageToBuffer` 回读做 CPU 像素比对（PASS/FAIL 退出码）
- **v341 API**（与主工程同一编译环境，系统头/loader）：buffer 描述符走 `data.pAddressRange`、`vkWriteResourceDescriptorsEXT` 4 参（`VkHostAddressRangeEXT`）、`pEmbeddedSampler` 在 `sourceData.constantOffset` 内、`VkBindHeapInfoEXT` 单 `heapRange`；**旧 loader 不导出扩展符号** → 全部经 `vkGetDeviceProcAddr` 运行时解析（主工程经 Vulkan-Hpp dispatcher 同理）
- 结论（2026-08-27 三驱动交叉）：RADV/Intel/NVIDIA × 5 变体（组合 / 分离+sampler heap / 分离+内嵌 / 无 sampler 对照）**全 PASS**——descriptor heap 纹理/sampler 语义分离三驱动完整实现；`imageDescriptorSize`（RADV 64 / NVIDIA 32 / Intel 64）> `bufferDescriptorSize`（16/16/64），Phase 3 统一步长改 `max`（运行时查询）；变体 `SPIKE_SHADER=glsl|slang|slang_ctrl`、`SPIKE_SAMPLER=embedded|heap`、`SPIKE_SAMPLER_FIELD`（旧字段对照），一键矩阵 `cross_test.sh`；slangc 经 `cmake/SlangFetch.cmake` FetchContent 拉官方预构建（与主工程同源，`$ENV{SLANGC_DIR}` 可覆盖，不查系统 PATH，见调研文档 §九.4）。权威记录见 `README.md` + `docs/heap_texture_research.md`
- 构建/运行：`cmake -B build -S . -G Ninja && cmake --build build`；RADV 用 `RADV_EXPERIMENTAL=heap`，NVIDIA 用 prime offload 环境变量（见其 README）
