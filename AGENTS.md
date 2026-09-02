# vulkan_practice

Vulkan 现代特性学习项目。Mesh shader 渲染管线，使用 ShaderObject + DescriptorHeap + DynamicState。
核心代码位于 vulkan_practice 目录下。
legacy 目录下的代码是陈旧的 OpenGL 项目已不是当前工作重点，一般不用看，但有些思路可以参考。

## 给大模型
优先使用中文回复用户，除非用户说使用英文回复。
随时思考是否可以拆分子任务给子 agent 执行。主 agent 校验结果。困难任务主 agent 亲自执行。

## 构建

项目使用 `just` 管理构建流程（参见 `Justfile`），支持 Linux 和 Windows 双平台。

### Linux

```bash
just config              # CMake 配置（默认 clang, Release）
just config gcc Debug    # 指定编译器和构建类型
just build               # 增量编译（需先 config）
just build-release       # 一键配置 + 编译 Release
just build-debug         # 一键配置 + 编译 Debug
just reflect             # 重跑 4 条反射管线（改动 shader 契约/schema/模板后必须；见"着色器编译"）
just run                 # 运行程序（自动检测：AMD VGA 存在时设置 RADV_EXPERIMENTAL=heap）
just run-igpu            # 强制核显（AMD 集显原生渲染，无 offload）
just run-dgpu            # 强制独显（NVIDIA prime offload，PROVIDER=3）
just run-intel           # 强制 Intel dGPU（anv 驱动，需 mesa>=26.2 默认开 descriptor heap）
just clean               # CMake 增量清理
just clean-all           # 删除整个 build 目录
just all                 # clean-all + build-release
```

### Windows (NVIDIA + LLVM 22)

**前置要求：** LLVM 22（`clang++`/`llvm-rc`）、Vulkan SDK（`slangc`、头文件/库）、just、
vcpkg（已 bootstrap）、Git for Windows（`sh.exe`，just backtick 求值依赖）。winget 均可装。

```powershell
# just 在 Windows 上需要 sh.exe 在 PATH 中（Git Bash）
$env:PATH = "C:\Program Files\Git\usr\bin;$env:PATH"

just config              # CMake 配置（默认 clang++, Release）
just config Debug        # Debug 构建
just config msvc         # 使用 Visual Studio generator（替代 Ninja）
just build / just build-release / just reflect / just run / just clean / just clean-all
```

Windows 构建 = LLVM 22 clang++ + Ninja（默认）或 VS generator；vcpkg triplet `x64-windows-static`
（静态链接 CRT）。Windows 上 `just run-igpu`/`run-dgpu` 不可用（Linux prime offload 专用），
NVIDIA 直接 `just run`。

### vcpkg 跨平台依赖

`vcpkg.json` 用 [platform expressions](https://learn.microsoft.com/vcpkg/concepts/configuration#platform-expressions)
按 OS 条件启用 features：Linux `sdl3[vulkan,wayland]` / Windows `sdl3[vulkan]`。
同一依赖条目可出现两次，用 `"platform"` 字段区分，vcpkg 按当前 triplet 自动选择。

### 构建系统内部

CMake 无独立构建目录 — 必须从根目录构建（`cmake -B build -S .`）。
产物: `build/vulkan_practice/vulkan_practice`（Windows 加 `.exe`）。

## 运行要求

- **CWD 必须是 `build/vulkan_practice/`**：程序按相对路径读取 `configs/`（日志 JSON、相机/场景
  Lua 配置）和 `generated_shaders/*.spv`，其他目录启动会报 "Log config file not found"
- 运行时资源由构建自动同步：`configs/` 增量拷贝（`copy_if_different`，改 config 后 `just build`
  即生效；**新增** config 文件需 `just config`）、SPV 直接编译到 `build/vulkan_practice/generated_shaders/`
- `RADV_EXPERIMENTAL=heap` — RADV 下启用 `VK_EXT_descriptor_heap` 必需，`just run`/`run-igpu` 已自动设置
- **headless 模式**：`RGL_HEADLESS=1` 脱 SDL/compositor（`vkCreateHeadlessSurfaceEXT`），
  不建 SDL 窗口；尺寸 `RGL_HEADLESS_SIZE=WxH`（默认 1280×720）；llm_debug 截图直接读 swapchain
  后缓冲（免离屏重渲）。坑：v357 结构体无 width/height/format 字段（尺寸由 swapchain 定）、
  须显式 `VK_KHR_surface`、无 `eImmediate`（用 `eFifo`）
- **agent 冒烟**：`cd build/vulkan_practice && RGL_HEADLESS=1 RADV_EXPERIMENTAL=heap timeout 8 ./vulkan_practice`
  （VVL 对 edS3 `depthClipEnable` 的报错是已知误报噪声，见「已知问题」）
- **llm_debug 截图流程**（agent 多模态读图）：`configs/camera.lua` 改 `llm_debug=true`（一行开关）
  → `just run` → 离屏渲染延迟 N 帧后存 PNG（路径打日志）并退出 → 复原 `false`。
  相机位置 = camera.lua 顶层 position/target/fovDegrees（无 presets）

## 命名规范

**所有 C++ 符号均在 `namespace RGL` 中。** 每个 `.cppm` 模块的导出均包裹在 `namespace RGL { ... }` 内。
`main.cpp` 使用 `using namespace RGL;` 简化。跨模块引用（类/函数/类型）统一 `RGL::` 前缀。

## 代码规范

### 模块优先
所有新代码**必须**使用 C++20 modules (`.cppm`)，而非 `.hpp/.cpp` 头文件对。
模块声明于 `namespace RGL` 内；入口文件 `import` 导入；不要在模块外 `#include` 项目内部头文件。

### 优先使用 rlstd 代替 #include STL
**所有 `.cppm` 模块必须 `import rlstd;` 获取 STL 类型，而非在 GMF 中 `#include` STL 头文件。**
- GMF 中只保留第三方库头文件（SDL、glm、fmt、sol2 等；**Vulkan C++ 头一律走 `import rlvk;`**，
  仅用 `VK_*` 宏/C 函数的文件可额外 `#include <vulkan/vulkan.h>`——core 量级 ~0.4s）
- **例外：** `<ranges>`（`std::views::` 是 libstdc++ namespace alias，无法 re-export）、`<filesystem>`、
  `<chrono>` 需保留 GMF `#include`
- 新加 STL include 时，先检查是否应补入 `rlstd.cppm` 而非直接加到 GMF。
  **rlstd 是全部 STL 名的唯一来源，且必须导出"完整可用面"**：除类型名外还包括
  namespace 作用域自由运算符/函数（`operator==` 等重载集、`begin/end/size/swap`、
  `make_shared`、`getenv`、`ifstream/ios` 等）与 `__gnu_cxx::operator==/<=>`
  （libstdc++ 容器迭代器比较在 `__gnu_cxx`，C++20 重写机制使 `!=` 自动可用）——
  BMI 只序列化被声明引用的类本体、**不带自由运算符**，缺名时症状是
  "invalid operands to binary expression"（2026-09-02 GMF 掏空后的批量补课，勿再回退）
- **GMF 要收敛**：GMF 头解析 + BMI 序列化（WriteAST）是模块编译耗时大头（-ftime-trace
  实测，codegen 仅 8~11%）——未用的第三方 include 一律删（2026-09-02 编译提速教训）

### 接口瘦身：大函数体放 partition implementation unit
重模块（函数体 >100 行）不把实现写进主接口 `.cppm`，拆 `xxx_impl.cppm`
（`module xxx:impl;`，CMake 与接口同登记 FILE_SET）：**接口 = 声明 + 轻量类型 + 文档
注释，实现 = 叶子单元**。收益：接口 BMI 不含函数体（importer 全部受益）、实现单元
与其他工作并行不上关键路径。已应用：llm_debug / vkrenderer / model_importer /
texture_upload / camera_config / scene_config / render_list / gpu_canary / ecs /
vkresources / shader_registry（函数语义注释留接口声明处）。规则与坑：
- **实现单元必须显式 `import xxx;`**——clang 不给 partition implementation unit
  做隐式主接口 import（标准 [cpp.import.module] 的隐式 import GCC 有、clang 无）
- 默认实参只写接口声明，实现定义去掉；类方法改类外定义（`static`/`[[nodiscard]]`
  只留声明侧）
- 导出实体在实现单元定义时**不得带 `export`**（声明已在接口）
- 新模块沿用同形态；小函数/模板/constexpr 留接口内联无妨

### 函数式 Ranges
优先 C++23 `std::ranges`：`std::views::filter/transform` 视图适配器、`std::ranges::for_each`
替换原始循环、链式组合避免中间容器。

### 成员变量命名
成员变量自然命名，不加 `_`/`m_` 前缀后缀（`int count;` ✅，`_count`/`m_count`/`count_` ❌）。

### ECS 设计规范
场景代码（ecs/scene/model_importer + 相关 system）必须符合五条 ECS 规范
（合规审查与逐条处置记录：`docs/archive/ecs_compliance_review.md`）：

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
`tools/` 下构建工具统一 C++20 modules：`schema_loader`（Schema 结构体 + Lua schema 加载）、
`validator`（reflection JSON ↔ schema 一致性校验）、`generator`（inja 模板生成 `.cppm`，
`renderTemplate()` 支持任意 JSON + 模板渲染）。`reflection_tool` 经 `import rllogger;` 统一日志。
tools 为 `EXCLUDE_FROM_ALL`（编译提速）：日常构建不碰工具链，`just reflect` 才编译。

## 模块结构（C++20 modules, .cppm）

所有导出均在 `namespace RGL` 内。模块职责一行速览；**实现细节以各 `.cppm` 文件头注释为权威**，
Phase 演进历史见 `docs/next_roadmap.md` 对应小节。

### 初始化层（构造顺序 = main.cpp 顺序）

| 模块 | 文件 | 职责 |
|---|---|---|
| `vk_instance` | vkinstance.cppm | Context、Instance、DebugMessenger、apiVersion（headless 时 surface 扩展族换成 `VK_EXT_headless_surface`） |
| `vk_device` | vkdevice.cppm | PhysicalDevice 选择、QueueFamilyManager、Device、CommandPools、FeatureChain |
| `vk_swapchain` | vkswapchain.cppm | Surface、Swapchain、ImageView[] format/extent/presentMode |
| `vk_resources` | vkresources.cppm | VmaAllocatorRAII、depth image+view、VmaDescriptorHeap（容量 = reserved + 8 + `kUserSlotMargin(1024)` 固定裕量，与场景内容解耦）；`allocSlot` 统一 slot 分配器（布局由 schema 生成偏移常量钉死 + 构造期契约断言）；`ensureSharedSampler()`（共享 sampler，幂等）/ `ensureSkySampler()`（sky 专用 mipmap sampler，第二槽）/ `ensureSceneData()`（场景级 bindless 动态 buffer：头 128B = @0 光参数 48B + @48 env 参数 + @64 skyInvViewProj mat4，@128 对象矩阵表 ×`kSceneMatrixCapacity(1024)`，描述符静态、内容每帧 staging copy）；env cubemap 场景级单例（`emplaceEnvTexture/getEnvTexture`，不占 AssetTable 纹理 id 空间）；资产层 `AssetTable` 见 `asset_table` 模块 |
| `vk_renderer` | vkrenderer.cppm | 同步对象、命令缓冲；`drawFrame(const FrameDrawList&)` 盲执行（零 entt 依赖，逐 draw push `DrawArgs` + bind + `drawMeshTasksEXT`）：**三段** `[opaque \| skybox \| transparent]` 段首各 `applyDynamicState`（opaque 写深度关混合 / skybox depthWrite=false+LEQUAL 大三角 z=1 只在空像素通过 / transparent 关深度写+开混合+背面剔除）；staging ring（×MAX_FRAMES_IN_FLIGHT 常驻映射，fence 后 CPU 写 + 录制期 `vkCmdCopyBuffer` + Transfer→Graphics barrier = 每帧动态 GPU 数据通路）；acquire 100ms 超时 eTimeout 跳帧 |
| `shader_registry` | shader_registry.cppm | 管线表（零 entt，只认 slot/资产 id）：`PipelineEntry{name, RenderPipeline, argsSize, pushDataSet}` + `ShaderRegistry`（"cube" 条目恒建 + `addModelEntry`（资产上传期，按 (规格,顶点slot,纹理slot) 组合去重——binding mapping 是 shader object 级状态；恒烘焙 scene data binding 3，有 env 时恒烘焙 binding 4/5）+ `addSkyEntry()`（天空盒单条目）+ `bindPipelineToAsset` 条目下标写回资产表）；`pipelineIndex/entry/pipelineAt` 查询 |

### 工具模块

| 模块 | 文件 | 职责 |
|---|---|---|
| `vulkan_utils` | utils.cppm | 层检查、debug messenger、GPU 选择器、barrier 辅助、present mode |
| `vkerror` | vkerror.cppm | `Error`/`ErrorCode`、`fromVkResult(VkResult,…)`、`vkResultName(VkResult)`（GMF 只含 `vulkan_core.h`，不拖 vulkan.hpp——编译提速关键） |
| `frame_draw_list` | frame_draw_list.cppm | 绘制契约：`DrawCall`/`SceneFrameData`/`FrameDrawList`（三段边界 `skyboxStart`/`transparentStart`）——渲染 system 产出、执行器盲执行的边界（ECS 规范 4） |
| `asset_table` | asset_table.cppm | 资产层纯数据：`MeshAsset`（kind=Model/Cube，共享 `shared_ptr<VmaBuffer>` 拼接顶点 buffer + 块内 `vertexOffset` + `groupCount` 上传期派生 + `pipelineIndex`）/ `TextureAsset`（deque 存储地址稳定 + 描述符指针链 + mipLevels）/ `MaterialAsset`（baseColor/textureId/alphaModeBlend/metallic/roughness/unlit，按 glTF materialIndex 去重）/ `AssetTable` 三容器 + `modelSlotCount` |
| `ecs` | ecs.cppm | `WindowExtent`/`AppExit` context、eventSystem（唯一 `SDL_PollEvent` 调用点：输入 → `InputEvents` 事件流，只产事件不做键位映射——键绑定在 `Camera.keys`；鼠标增量用 `motion.xrel/yrel`，SDL3 陷阱见 `docs/调试历史.md`）/resizeSystem/renderSystem 帧循环（**显式 system 列表** camera→animation→bounds→projection→renderlist，注册顺序 = 数据流契约；llm 截图经 `llmCapture` 回调触发，不依赖 llm_debug 模块） |
| `render_list` | render_list.cppm | `renderListSystem`：ViewProjection + 场景实体 → `FrameDrawList`——单一 `DrawArgs` 超集按资产 id 索引写入（零 AssetKind 分支），mvp = proj·view·spin·world，透明段按相机距离² 降序稳定排序（back-to-front），`env.skybox` 时推 skybox draw，每帧填 `SceneFrameData`（光参数/env 参数/skyInvViewProj） |
| `scene` | scene.cppm | 场景组件（`Transform`/`Relationship` 手写单向 parent/`MeshRef`/`MaterialRef` 真引用/`Bounds`）+ `registerCubeEntity`（cube 一等资产，画不画 = 实体有无）+ 每帧 context（`SceneSpin`/`SceneBounds`/`ViewProjection`）+ `Light`（定向光，main 从 scene.lua light 段 emplace）+ `SceneEnv`（环境 intensity/mipRange/skybox/skyboxPipeline） |
| `scene_config` | scene_config.cppm | `PipelineSpec`/`SceneConfig`（`styleMap` 风格名→管线规格名 + `Light` + `EnvironmentConfig`{dir/faces[6]/intensity/skybox}）+ `loadSceneConfig`（`configs/scene.lua`，sol2 沙箱；styles/light/environment 段可选回落默认；启动期数据，不进每帧路径） |
| `gltf_loader` | gltf_loader.cppm | glTF CPU 解析（tinygltf v3：POSITION/NORMAL/TEXCOORD_0 严格校验 + 去索引展开 + node translation 扁平化 + baseColorFactor/alphaMode/materialIndex/metallic/roughness/unlit（KHR_materials_unlit）+ images 内嵌纹理，输出 `GltfModel` 含包围球） |
| `model_importer` | model_importer.cppm | `uploadModelMeshes`（每模型顶点拼 16MB 块（≤6 块）每块一个 BDA buffer + heap slot，per-primitive 块内偏移 = `MeshAsset.vertexOffset` 进 push constant）+ `registerScene`（场景根挂 `Bounds` + 每 primitive 一个 entity；材质按 materialIndex 去重注册 + 同 materialIndex 字段一致性金丝雀） |
| `texture_pool` | texture_pool.cppm | 纹理上传公共 API 薄壳（自由函数，数据归属资产表——main 局部对象 UAF 教训见 `docs/archive/issue_history.md`）：`loadEmbedded`（stb 解码 + 去重缓存）/ `ensureWhiteTexture`（白 1×1 首纹理槽 + slot 契约断言）/ `uploadModelTextures`（per-primitive 有效纹理选择：有 uv 且图有数据 ? material : 白）/ `loadEnvCubeMapFiles`（六面校验） |
| `texture_upload` | texture_upload.cppm | GPU 上传核心：`uploadTexture`（2D：linear staging 填充 → optimal 拷贝 + UNDEFINED→SHADER_READ_ONLY + DIAG texel 回读 + SAMPLED_IMAGE 描述符入 AssetTable）/ `uploadCubeMap`（6 面直传 e2D+eCubeCompatible + **6 张独立 2D linear staging**（RADV linear 三坑：mipLevels=1 / CUBE_COMPATIBLE 不支持 / linear maxArrayLayers=1）+ mip 链 blit NEAREST + 逐面拷贝金丝雀回读 + env 单例入库 + `ensureSkySampler`） |
| `gpu_canary` | gpu_canary.cppm | `texelCanaryCheck` 帧 N 白纹理回读金丝雀（GPU 侧 UAF 诊断：freed/VA 复用签名；`RGL_TEXEL_CANARY=N` 触发）——诊断与上传逻辑解耦，ecs 不拖纹理上传栈 |
| `vulkan_window` | sdl3window.cppm | SDL3 窗口、surface、HDR/SDR 格式选择 |
| `queue_family_manager` | QueueFamilyManager.cppm | Graphics/Compute/Transfer 队列分配 |
| `features` | features.cppm | FeatureChain 类型、buildRequestedFeatures、validateFeatures、checkDeviceExtensions、自定义异常 |
| `cube` | cube.cppm | 顶点/索引数据 |
| `camera` | camera.cppm | `Camera`（含 `KeyBindings`）/`InputEvents`/`CameraState` context、cameraView/cameraSystem（trackball 绕 target / FPS 第一人称双模式，消费事件即清空） |
| `camera_config` | camera_config.cppm | `CameraConfig` + `LlmDebugConfig` + `loadCameraConfig`（`configs/camera.lua` 含 `keys` 键位 + llm_debug/llm_shot 段，sol2 沙箱 + 可选回落默认）——配置归本模块，执行归 llm_debug |
| `shader` | shader.cppm | SPV 加载器、ShaderEffect、`RenderPipeline`（ShaderObject + 动态状态外移：`RenderState` 含 colorBlend/depthWrite/depthCompareOp/cullMode，`applyDynamicState` 接线；binding mapping 经 `PipelineBindings` 聚合，**顺序填充** running-index——固定下标假设会因组合空洞出错） |
| `llm_debug` | llm_debug.cppm | 一次性截图执行：`captureFrame()` 离屏重渲（8-bit UNORM + D32Sfloat，复用管线表/动态状态/draw list）+ 独立命令缓冲/fence + VMA 回读 + stb PNG（绝对路径打日志）。`llm_debug` 纯开关（true/false）+ `llm_shot` 常驻 `{file,size,delay_frames}`（全可选回落默认）；兼容 `llm_debug=table` 旧格式 |
| `VmaRaii` | VmaBuffer.cppm | VmaBuffer/VmaImage/VmaDescriptorHeap/VmaAllocatorRAII 封装；Buffer/Image 经 `SharedAllocator`（`shared_ptr<AllocatorControlBlock>`）共享 allocator 所有权 |
| `rlstd` | rlstd/rlstd.cppm | C++ 标准库模块封装（libstdc++ 不支持 `import std`：GMF `#include` 全部 STL 头 + `export` 常用名）。STL 名以本模块 `using` 为唯一来源；`std::views::`/`<filesystem>`/`<chrono>` 走 GMF 例外 |
| `rlvk` | rlvk.cppm | Vulkan C++ 头具名模块封装（rlstd 同构）：GMF include `vulkan.hpp`+`vulkan_raii.hpp` 全量编译仅此一次，导出全库实际用词（~120 vk:: + 14 raii + Flags 位运算符）；消费方 `import rlvk`（BMI load ~18ms vs 文本解析 3.1s/TU）。**新用某 vk:: 名报 "not visible" → 到 rlvk.cppm 补一行 `export using`（保持字典序）**；VK_* 宏/C 函数不随模块导出，需要者保留 `#include <vulkan/vulkan.h>` |
| `types_generated` | generated/types_generated.cppm | auto-generated（just reflect 生成、产物已提交；反射管线 1）：`HeapSlotVertexOffset`/`HeapSlotIndexOffset`（相对 reserved range 起点偏移，运行时 slot = `reservedSlots() + offset`） |
| `model_types_generated` | generated/model_types_generated.cppm | auto-generated（just reflect 生成、产物已提交；反射管线 2：`model.slang`+`model_schema.lua`）：`ModelVertex`、`HeapSlotModelVertexBase`、契约常量 `kTrianglesPerGroup`（schema `constants` 段，shader 端字面量必须一致） |
| `material_types_generated` | generated/material_types_generated.cppm | auto-generated（just reflect 生成、产物已提交；反射管线 3：`material.slang`+`material_schema.lua`）：统一 push constant 超集 `DrawArgs`（scalar 布局 100B：mvp/totalTriangles/vertexOffset/baseColor/roughnessMetallic/objectIndex，所有风格共享，static_assert 钉死） |
| `features_generated` | generated/features_generated.cppm | auto-generated（just reflect 生成、产物已提交；`reflection_tool render` + `features_config.json`）：`FeatureChain`、`buildRequestedFeatures()`、`validateFeatures()` |

`main.cpp` 启动流：加载 camera/scene 配置（**先于建窗**：llm/headless 模式影响窗口）→ 栈上
`entt::registry`（持有全部每帧 context）→ 按序构造 5 个初始化层类（Renderer 收管线规格 span，
"cube" 条目恒建；启动期 `maxPushConstantsSize`/资产数 ≤ `kSceneMatrixCapacity` 契约检查）→
env 上传（**时序契约 = 模型纹理槽后、`addModelEntry` 前**；风格契约 = 引用 pbr_ibl 无 env
启动报错）→ CPU 解析模型 + GPU 上传 + `addModelPipeline` + `registerScene` → env 已上传则
`addSkyEntry` + 回填 `SceneEnv` → 按 `showCube` 注册 cube entity + `bindPipelineToAsset` →
`RGL::renderSystem`。`vam_impl.cpp` = `#define VMA_IMPLEMENTATION`，`stb_impl.cpp` =
`#define STB_IMAGE_WRITE_IMPLEMENTATION`。

### 构造依赖图

```
SDLWindow → AppContext → DeviceContext → Swapchain → Resources → Renderer
                └────────────── entt::registry（Viewport context）──────────┘
```

main.cpp 按逆序析构（RAII）。依赖图原则：只允许下层 import 上层，**配置模块不依赖执行模块**
（camera_config 不 import llm_debug）、**ecs 不依赖 llm_debug**（截图经回调）——假边会让
慢模块（llm_debug 拖 vk_renderer 全链）进入本不该等它的编译前置。

### 重要成员生命周期规则

- `vkinstance.cppm`：`engineInfo_`、`instanceCreateInfo_`、`layers_`、`instanceExtensions_` 均为类成员 — `pApplicationInfo` 不能悬空
- `vkdevice.cppm`：`featuresChain_` 为类成员 — `DeviceCreateInfo.pNext` 不能悬空
- 所有 `vk::raii::*` 对象必须通过**成员初始化列表**初始化，绝不在函数体内初始化（默认构造函数已删除）
- 成员声明顺序必须与初始化依赖顺序一致
- **GPU 资源所有权**：长生命周期 GPU 资源（纹理 BO/描述符存储）不得声明在 main() 局部块内
  ——块尾析构 = 第 0 帧起 GPU 采样已释放 VA（UAF 非确定性故障，见 `docs/archive/issue_history.md`）

## 着色器编译

`vulkan_practice/shaders/` 下的 Slang → SPV 输出到 `build/vulkan_practice/generated_shaders/`
（CMake `compile_slang()` 构建时编译；运行时从 `generated_shaders/` 相对路径读取）。

- **反射管线显式触发（2026-09-02 编译提速）**：`*_reflection.json` 与
  `vulkan_practice/generated/*.cppm` + `reflection_manifest.txt` 为**已提交产物**
  （.gitignore 已放行），只在改动契约后 `just reflect` 重生成并随改动提交；
  每次构建开头 `reflection_check` 比对输入 SHA1（`cmake/ReflectionManifest.cmake`），
  忘跑 reflect = 构建 FATAL 指路（产物过期静默错配比构建慢危险）。
  工具链 `EXCLUDE_FROM_ALL`，日常/全新构建不再付 ~14s 串行前缀
- Profile: `sm_6_6+spirv_1_6`，capability: `spvDescriptorHeapEXT`
- **Scalar Block Layout（定案）**：所有 slangc 编译（3 条反射管线 + `compile_slang` 实际 SPV）
  统一 `-fvk-use-scalar-layout`——push constant 用 scalar（C 自然）布局，C++ struct（glm 自然
  对齐 4）与 shader 逐字段偏移天然一致、零 padding（`DrawArgs` 100B）。前提：`scalarBlockLayout`
  feature 已启用（`features_config.json` + `validateFeatures` 校验）。**反射与 SPV 两处 flag
  必须一致**（不一致 = 运行时布局错配的隐蔽 bug）
- **bindless 原则**：push constant 冻结在"mvp + 索引"级别（`DrawArgs` 100B 不再扩）——场景级/
  每帧变化的数据（光照、对象矩阵、env 参数、skybox invViewProj）一律进 descriptor heap
  （`scene.slang` / `Resources::ensureSceneData`）

## 隐式扩展依赖

`features.cppm` 中请求的 `VK_EXT_DEVICE_GENERATED_COMMANDS`、`VK_EXT_DESCRIPTOR_BUFFER` 等扩展
当前无显式使用，但被其他已启用的扩展隐式依赖，移除可能导致驱动层面功能异常，不可随意删除。
`vkinstance.cppm` 中请求的 `VK_EXT_SWAPCHAIN_COLOR_SPACE` 等实例扩展同理。

**KHR/EXT 别名说明：** `VK_KHR_swapchain_maintenance1` promotion 后，
`VkSwapchainPresentFenceInfoKHR` 和 `VkSwapchainPresentFenceInfoEXT` 是同一结构（typedef alias），
sType 值相同。代码使用 EXT 命名形式，功能等价于 KHR。

## 外部依赖（通过父 CMake 链接）

- `rlstd` — `rlstd/` 根目录共享库，C++ 标准库模块封装
- `rllogger` — `logger/` 根目录共享库，`RGL::RLLogger` 单例
- `spdlog` — 通过 rllogger 间接使用
- `glm` — 数学库（强制 depth zero-to-one，弧度制）
- `SDL3` — 窗口/surface
- `VulkanMemoryAllocator` — 内存管理
- `slangc` — **FetchContent 拉 Slang 官方预构建**（`cmake/SlangFetch.cmake` pin v2026.16 + sha256）；
  Windows 上优先用 Vulkan SDK 自带的 slangc（`SlangFetch.cmake` 自动检测）

## 关键文件位置

| 位置 | 说明 |
|---|---|
| `vkrenderer.cppm` `drawFrame()` | ①~⑮ 完整流程：双 fence → acquire（100ms 超时跳帧）→ 录制（barrier/动态渲染/heap barrier+绑定/beginRendering/动态状态/逐 draw pushData→bind→drawMeshTasksEXT/present barrier）→ submit → present（`SwapchainPresentFenceInfoEXT` 挂 `presentInfo.pNext`） |
| `shader.cppm` `RenderPipeline`/`applyDynamicState` | ShaderObject 包装（`DescriptorSetAndBindingMappingEXT` 映射 heap slot，编号运行时动态；`PipelineBindings` 聚合——binding 0/1 顶点索引、3 scene data、4/5 env cubemap+sky sampler，`hasSceneData`/`hasEnv` 开关） |
| `shaders/material.slang` | **统一 push constant 契约**：`DrawArgs` 超集单一真相源 → `material_schema.lua` 校验 → `material_types_generated`；mesh shader 同时 import 本文件与自身契约 |
| `shaders/scene.slang` | **场景级 bindless 数据契约**：`ByteAddressBuffer sceneData`（binding 3）+ `readSceneLight`/`readObjectMatrix`（矩阵表 @128）/`worldPos`/`worldNormal`；字节布局真相源 = vkresources `LightDataGpu`/`kSceneData*`，字面量必须手工一致（无反射端）；仅光照风格 import |
| `shaders/env.slang` / `shaders/sky.slang` | **env cubemap 契约**（binding 4/5 + `readEnvParams()` @48，仅 `model_pbr_ibl`/`skybox.frag` import）/ **天空盒契约**（`skyClipToWorld()` 读 @64 invViewProj 重建世界射线，免 resolution） |
| `shaders/model_pbr.frag` / `model_phong.frag` / `model_pbr_ibl.frag` | 三条 frag 路线：简化 PBR（GGX，世界空间光照）/ 基础 Phong（Blinn）/ PBR+简化 IBL（env 采样替换 ambient）；同 mesh SPV 同 `DrawArgs`，`scene.lua styles.pbr` 一行切换 |
| `shaders/skybox.mesh` + `skybox.frag` | 天空盒：mesh 单 group 输出全屏大三角（NDC z=1=far，不读 buffer，binding 0/1 占位空转）；frag 射线采 envMap |
| `shaders/model_schema.lua` `constants` | 契约常量单一真相源（`kTrianglesPerGroup`）：schema 声明 → 生成 constexpr，`model.mesh.slang` 字面量必须一致（无反射端对应） |
| `shaders/types.slang` | cube 契约：只拥有 buffer slot 段（vertex/index 均 `ByteAddressBuffer`，绝不用 `StructuredBuffer`） |
| `configs/camera.lua` / `configs/scene.lua` | 运行时配置（sol2 沙箱加载，字段可选回落默认）；**与 `shaders/*_schema.lua`（build 时 shader 契约）是两套机制，勿混**；改后 `just build` 生效 |
| `ecs.cppm` `eventSystem` | 中央 SDL 事件轮询（唯一 `SDL_PollEvent`）；鼠标增量用 `motion.xrel/yrel`（SDL3 陷阱，见 `docs/调试历史.md`） |

## 已知问题

> 只列**未解决**项。已解决问题（根因/误诊复盘）见 `docs/archive/issue_history.md`。

- `computeCommandPool`/`transferCommandPool`/`computeQueue`/`transferQueue` 已创建但保留给未来使用
- **edS3 VVL 误报**：VVL 1.4.357 报 `depthClipEnable feature 未启用`——深挖实锤 feature 确实
  启用（`vkCreateDevice` pNext 链末位 flag=TRUE，VVL 认识却未记进"已启用"集）= 非致命噪声，
  app 侧无 bug，保留现状（详见 `docs/vulkan_extensions_2025_调研.md` §十二）
- **VMA + zero-init 扩展冲突（教训）**：VMA 3.4 对 BDA buffer 自动加内部
  `VkMemoryAllocateFlagsInfo(DEVICE_ADDRESS)`，与用户 `pMemoryAllocateNext(ZERO_INITIALIZE)`
  必撞 = spec 违例——`VK_EXT_zero_initialize_device_memory` 已回退；将来真实需求走原生
  `vkAllocateMemory` 单 flagsInfo 绕 VMA（详见 `docs/vulkan_extensions_2025_调研.md` §十二）
- **Phase 5 光照/透明已知近似**：① MASK alphaTest 未实现（`alphaMode=MASK` 暂按不透明，无
  discard）；② 世界法线变换取对象矩阵 mat3 直乘（假定均匀缩放，Phase 7 真实 normalMatrix 再议）；
  ③ 透明排序按 primitive 中心距离² back-to-front（非 per-pixel/OIT，相交透明面会错序）；
  ④ 透明段背面剔除（封闭网格双重混合的修复，非封闭单面几何会被剔掉一面）。均留 Phase 7+
- **Phase 5.5 IBL/天空盒已知近似**：① 简化版 IBL——无 pre-filtered cubemap/BRDF LUT（完整版
  留 Phase 7）；② mipmap blit 用 NEAREST（sRGB blit LINEAR 违 VUID-00239，CPU 生成线性 mip 链
  留作质量接缝）；③ 单环境贴图（全局单例，无 per-material env）；④ 天空盒大三角 z=1 依赖
  depth clear=1.0 + LEQUAL
- **`LightDataGpu` 字节布局双源、无反射**：`vkresources.cppm` 手写 struct（`static_assert`）↔
  `shaders/scene.slang` 字节字面量手工一致——唯一绕过反射管线的主要 CPU↔GPU struct 契约；
  单灯固定布局下风险可控，Phase 7 多灯布局重设计时并入反射管线（schema 声明→生成布局常量，
  同 `model_schema.lua` `constants` 段先例），灯同时升格为灯 entity + 组件（ECS 规范 3）
- `configs/` 增量拷贝（`copy_if_different`）只认"源文件更新"：build 端副本被破坏/截断
  （mtime 更新）不会自动恢复，需手动拷贝或 `just clean`

## 其他文档

> 每篇文档头部带一行状态（`现行`/`历史`/`调研`）。`docs/archive/` = 已归档（Phase 实施计划、
> 设计规划、专题调研；时点快照，行号已漂移，只取设计思路与结论）+ 已解决问题总账
> （`docs/archive/issue_history.md`）。现状权威记录以本文件 + `next_roadmap.md` 为准。

- `docs/next_roadmap.md` — **开发路线图（权威）**：Phase 0–5.5 完成情况 + 偏离注记、Phase 6/7 计划
- `docs/技术概念.md` — 核心 Vulkan 模式、设计决策、shader 缓冲访问规则
- `docs/调试历史.md` — 问题修复时间线（只增不改，已解决争议的结论以此为准）
- `docs/DESCRIPTOR_HEAP_SPEC.md` — descriptor heap 扩展排坑详细记录
- `docs/nvidia_compatibility_report.md` — 跨驱动兼容性排查（三平台验证记录；当前开发机 = Strix Halo 纯核显）
- `docs/device_generated_commands_research.md` — GPU culling（`VK_EXT_device_generated_commands`）双驱动实测调研（Phase 7 依据）
- `docs/present_timing_research.md` — present 时序调研（决策"暂不引入"）
- `docs/vulkan_extensions_2025_调研.md` — 2025+ 新扩展调研 + §十二 实施记录（edS3/headless 落地、zero_init 回退）
- `docs/快捷键配置.md` — Zed 编辑器快捷键/任务/调试配置
- `docs/archive/` — 归档索引（phase 计划、ecs 审查、heap 纹理调研、反射规划；含 issue_history.md 总账）
- 离线 Vulkan 文档（优先于在线检索，`docs.vulkan.org`/`registry.khronos.org` 有 Cloudflare 拦截）：
  本地克隆 `/home/robin/program/Vulkan-Docs`（shallow clone，当前 v1.4.360；`git pull` 更新）。
  **注意**：spec clone 比系统 Vulkan 头/loader（**v341**，实际编译环境）新，部分扩展 API 表面有
  差异（descriptor heap 最典型）——写代码/核对结构体字段以系统头为准，spec 只作语义与 VUID 依据

## min_mesh_crash（最小复现子项目）

独立 CMake 单文件最小复现（2026-08 调查 NVIDIA mesh shader 崩溃时建立）：vertex+fragment 与
mesh shader 的 shader object 测试（GPU 选择写死 vendor 0x10de；强制 RADV 用 `VK_DRIVER_FILES`）。
**根因权威记录见其 README**：mesh shader 对象**必须**带 `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT`
（TASK stage 绑 NULL 时），否则 NVIDIA Xid 13/engine reset；VVL 查不出来是 spec 漏洞。
构建：`cmake -B build -S . && cmake --build build`（运行需 prime offload 环境变量，见其 README）。

## heap_texture_spike（最小验证子项目）

独立 CMake 单文件最小验证程序（2026-08-25，Phase 3 纹理预研）：descriptor heap 中
**SAMPLED_IMAGE 描述符**（`pImage`/`VkImageDescriptorInfoEXT`）+ **STORAGE_BUFFER 描述符**
（`pAddressRange` 裸地址）共存于同一 resource heap；shader object 经 `pEmbeddedSampler` 采样离屏
纹理，`vkCmdCopyImageToBuffer` 回读做 CPU 像素比对（PASS/FAIL 退出码）。**v341 API**（旧 loader
不导出扩展符号 → 全经 `vkGetDeviceProcAddr` 运行时解析）：`pAddressRange`、4 参
`vkWriteResourceDescriptorsEXT`、`pEmbeddedSampler` 在 `sourceData.constantOffset` 内。
结论（2026-08-27 三驱动交叉）：RADV/Intel/NVIDIA × 5 变体全 PASS，语义分离三驱动完整实现；
`imageDescriptorSize`（64/32/64）> `bufferDescriptorSize`（16/16/64）→ 统一步长取 `max`。
权威记录见其 `README.md` + `docs/archive/heap_texture_research.md`。
