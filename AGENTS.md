# RGL

Vulkan 现代特性学习项目。Mesh shader 渲染管线，使用 ShaderObject + DescriptorHeap + DynamicState。
核心代码位于 rgl 目录下。
legacy（旧 OpenGL 项目）已于 2026-09-08 删除，历史思路参考见 git history（`legacy/` @ < 2026-09-08）。

## 给大模型
优先使用中文回复用户，除非用户说使用英文回复。
随时思考是否可以拆分子任务给子 agent 执行。主 agent 校验结果。困难任务主 agent 亲自执行。

## 构建

项目使用 `just` 管理构建流程（参见 `Justfile`），支持 Linux 和 Windows 双平台。

### Linux

```bash
just config              # CMake 配置（默认 clang, Release）；just config gcc Debug 指定编译器/类型
just build               # 增量编译（需先 config）；just build-release / build-debug = 一键配置+编译
just reflect             # 重跑 5 条反射管线（改动 shader 契约/schema/模板后必须；见"着色器编译"）
just run                 # 运行（AMD VGA 存在时自动设 RADV_EXPERIMENTAL=heap）
just run-igpu / run-dgpu / run-intel   # 强制核显 / NVIDIA prime 独显 / Intel anv
just clean / clean-all / all           # 增量清理 / 删 build 目录 / clean+build-release
```

### Windows (NVIDIA + LLVM 22)

前置：LLVM 22、Vulkan SDK（slangc）、just、vcpkg（已 bootstrap）、Git for Windows（`sh.exe` 须在
PATH，just backtick 依赖）。命令与 Linux 同名（`just config/build/reflect/run/clean`）；默认
clang++ + Ninja、triplet `x64-windows-static`；`run-igpu/run-dgpu` 不可用（Linux prime 专用），
NVIDIA 直接 `just run`。

### vcpkg 跨平台依赖

`vcpkg.json` 用 platform expressions 按 OS 启用 features（Linux `sdl3[vulkan,wayland]` /
Windows `sdl3[vulkan]`）；同一依赖可出现两次用 `"platform"` 字段区分。

### 构建系统内部

CMake 无独立构建目录 — 必须从根目录构建（`cmake -B build -S .`）。
产物: `build/rgl/rgl`（Windows 加 `.exe`）。

## 运行要求

- **CWD 必须是 `build/rgl/`**：程序按相对路径读取 `configs/` 和 `generated_shaders/*.spv`，
  其他目录启动会报 "Log config file not found"
- 运行时资源由构建自动同步：`configs/` 增量拷贝（`copy_if_different`，改 config 后 `just build`
  即生效；**新增** config 文件需 `just config`）、SPV 直接编译到 `build/rgl/generated_shaders/`
- `RADV_EXPERIMENTAL=heap` — RADV 下启用 `VK_EXT_descriptor_heap` 必需，`just run`/`run-igpu` 已自动设置
- **运行模式全配置化**（2026-09-08 环境变量→配置文件）：`configs/runtime.lua`（headless /
  headless_size / win_size / show_after / llm_debug+llm_shot）、`configs/diagnostics.lua`
  （no_vvl / no_hdr / no_present / vtx_canary / texel_canary / dump_gbuf / style_dist / tex_dist /
  texture_force_*，全可选默认关）、`configs/render_graph.lua` 顶层 `variant = "forward"|"deferred"|"offscreen"`
  （路线选路，替代旧 RGL_DEFERRED/RGL_OFFSCREEN env）。仅驱动级 env（RADV_EXPERIMENTAL /
  VK_DRIVER_FILES / prime offload）保留——由 `just run-*` 注入，非应用配置
- **headless 模式**：runtime.lua `headless=true` 脱 SDL/compositor（`vkCreateHeadlessSurfaceEXT`），不建 SDL 窗口；
  尺寸 `headless_size={W,H}`（默认 1280×720）；llm_debug 截图直接读 swapchain 后缓冲（免离屏重渲）。
  坑：v357 结构体无 width/height/format 字段、须显式 `VK_KHR_surface`、无 `eImmediate`（用 `eFifo`）
- **agent 冒烟**：改 `build/rgl/configs/runtime.lua` 为 `headless = true`（build 副本临时改，
  下次 `just build` 按源内容比对**自动复原**）→ `cd build/rgl && RADV_EXPERIMENTAL=heap timeout 8 ./rgl`
  （正常应零 `[error]`；deferred 另改 build 副本 render_graph.lua `variant="deferred"`）
- **llm_debug 截图流程**（agent 多模态读图）：build 副本 `configs/runtime.lua` 改 `headless=true` +
  `llm_debug=true` → 直接跑二进制 → 延迟 N 帧存 PNG（路径打日志）并退出 → `just build` 复原配置。
  相机位置 = camera.lua 顶层 position/target/fovDegrees
- **截图回归方法学**（2026-09-08）：BIT-IDENTICAL 比对须**同会话双构建**（`git stash` 基线构建 vs 改动构建）；
  跨日/跨全量重建存在 ±1 LSB 叶缘噪声（增量 .o vs 全量 codegen 微差，见 docs/调试历史.md 2026-09-08 条目）

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
vkresources / shader_registry / render_graph / render_graph_config（函数语义注释留接口声明处）。规则与坑：
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

所有导出均在 `namespace RGL` 内。**实现细节以各 `.cppm` 文件头注释为权威**，Phase 演进史见
`docs/next_roadmap.md`。**工具/数据/生成模块 29 行速览表已下沉 `docs/技术概念.md` §模块表**，
此处只留初始化层（构造顺序 = main.cpp 顺序，强导航价值）：

| 模块 | 文件 | 职责 |
|---|---|---|
| `vk_instance` | vkinstance.cppm | Context、Instance、DebugMessenger、apiVersion（headless 换 `VK_EXT_headless_surface`） |
| `vk_device` | vkdevice.cppm | PhysicalDevice 选择、QueueFamilyManager、Device、CommandPools、FeatureChain |
| `vk_swapchain` | vkswapchain.cppm | Surface、Swapchain、ImageView[] format/extent/presentMode |
| `vk_resources` | vkresources.cppm | VmaAllocatorRAII、depth、VmaDescriptorHeap（固定裕量 `kUserSlotMargin(1024)`）、`allocSlot` 统一分配、`ensureSharedSampler/ensureSkySampler/ensureSceneData`（scene data 头 128B：@0 光参数/@48 env/@64 skyInvViewProj/@128 对象矩阵表 ×4096，尾区 @262272 灯数组）、env cubemap 单例、G-buffer RT 缓存（`ensureRtAttachment`/`rtHeapSlot`/`ensureDeferredGBufferSlots`） |
| `vk_renderer` | vkrenderer.cppm | 同步对象、命令缓冲；`drawFrame(const FrameDrawList&)` 盲执行（零 entt）：构造收 `CompiledRenderGraph`，逐 pass = barrier 计划物化 → beginRendering → applyDynamicState → 段内逐 draw → endRendering；blit pass；present/waitStage 图派生；heap barrier + scene staging = 固定段不入图；staging ring；acquire 100ms 超时跳帧 |
| `render_graph` | render_graph.cppm | 渲染意图图（纯 C++ 零 Lua 零 entt）：`RgGraphDesc` → `compileRenderGraph`（校验 + barrier 自动推导 + presentTransition）→ `CompiledRenderGraph`；default/offscreen/deferred 三图变体 + `kGBufferAttachments` 契约表（G-buffer 名单一真相源）+ `renderGraphSelfCheck` |
| `render_graph_config` | render_graph_config.cppm | `configs/render_graph.lua` → `RgGraphDesc`（sol2 沙箱，缺文件回落默认；结构合法性归 compile） |
| `shader_registry` | shader_registry.cppm | 管线表（零 entt）：`PipelineEntry` + `ShaderRegistry`（cube 恒建 + `addModelEntry`（按组合去重）+ `addSkyEntry` + `addLightEntry` + `bindPipelineToAsset`） |

**main.cpp 启动流**：加载 camera/scene/render_graph 配置（先于建窗）+ `compileRenderGraph` +
`renderGraphSelfCheck` → 栈上 `entt::registry`（全部每帧 context）→ 按序构造初始化层（Renderer
收编译图；启动期 push constant/矩阵容量契约检查）→ env 上传（**时序 = 全部模型纹理槽后、
addModelEntry 前**）→ **entities 三段循环**（A1 解析+顶点上传全部模型 → A2 纹理 → C 逐模型
addModelEntry+registerScene；A1/A2 拆分 = heap 布局"顶点槽先于纹理槽"契约）→ `addSkyEntry`/
deferred 装配（G-buffer 槽 + light 条目，**须先于 Renderer 构造**）→ `renderSystem`。

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

`rgl/shaders/` 下的 Slang → SPV 输出到 `build/rgl/generated_shaders/`
（CMake `compile_slang()` 构建时编译；运行时相对路径读取）。

- **反射管线显式触发**：`*_reflection.json` + `generated/*.cppm` + `reflection_manifest.txt` 为
  **已提交产物**，只在改契约后 `just reflect` 重生成并随改动提交；构建开头 `reflection_check`
  比对输入 SHA1，忘跑 = FATAL 指路。共 **5 条管线**（types/model/material/lights 反射生成 +
  features 模板生成），Profile `sm_6_6+spirv_1_6` + capability `spvDescriptorHeapEXT`
- **Scalar Block Layout（定案）**：所有 slangc 编译（反射 + SPV 两处 flag **必须一致**）统一
  `-fvk-use-scalar-layout`——C++（glm 自然对齐）与 shader 逐字段偏移天然一致零 padding
  （`DrawArgs` 100B）。前提 `scalarBlockLayout` feature（features_config.json + validateFeatures）
- **bindless 原则**：push constant 冻结在"mvp + 索引"级别（`DrawArgs` 100B 不再扩）——场景级/
  每帧变化的数据（光照、对象矩阵、env 参数、灯数组）一律进 descriptor heap（scene data）

## 隐式扩展依赖

`features.cppm` 请求的 `VK_EXT_DEVICE_GENERATED_COMMANDS`、`VK_EXT_DESCRIPTOR_BUFFER` 等当前无
显式使用，但被其他已启用扩展隐式依赖，不可随意删除；`vkinstance.cppm` 的 `VK_EXT_SWAPCHAIN_COLOR_SPACE`
同理。**KHR/EXT 别名**：`VkSwapchainPresentFenceInfoKHR/EXT` 是同一结构（typedef alias），代码用 EXT。

## 外部依赖（通过父 CMake 链接）

`rlstd`（根目录 STL 模块库）、`rllogger`（根目录日志库，`RLLogger::getInstance()`）、spdlog（经
rllogger）、glm（depth zero-to-one + 弧度制）、SDL3、VulkanMemoryAllocator、
slangc（`cmake/SlangFetch.cmake` FetchContent pin v2026.16 + sha256；Windows 优先 Vulkan SDK 自带）

## 关键文件位置（契约与接缝）

| 位置 | 说明 |
|---|---|
| `shaders/material.slang` | **统一 push constant 契约**：`DrawArgs` 超集单一真相源 → schema 校验 → `material_types_generated` |
| `shaders/scene.slang` | **场景级 bindless 契约**：`sceneData`（binding 3）+ `readSceneLight`/`readObjectMatrix`/`worldPos`/`worldNormal`；@0-128 头字节布局 = vkresources 手写 ↔ shader 字面量手工一致（无反射端，见已知问题） |
| `shaders/env.slang` / `sky.slang` | env 采样契约（binding 4/5 + @48）/ 天空盒契约（@64 invViewProj 重建射线） |
| `shaders/lights.slang` | **多灯契约**（反射管线 4）：`GpuLight` + `readLightCount/readGpuLight` 读尾区 @262272；`light.frag` import，forward 不 import |
| `shaders/model_pbr/model_phong/model_pbr_ibl.frag` | 三条 frag 路线（GGX / Blinn / GGX+简化 IBL），`scene.lua styles` 一行切换 |
| `shaders/deferred_geometry.frag` + `light.mesh/frag` | deferred 路线：4-MRT G-buffer 写入（oct 法线）/ 全屏大三角逐灯 GGX；`kGBufferAttachments` 表定格式 |
| `shaders/model_schema.lua` `constants` | 契约常量单一真相源（`kTrianglesPerGroup` 等 → 生成 constexpr） |
| `configs/*.lua` | 运行时配置（sol2 沙箱，字段可选回落默认）；**与 `shaders/*_schema.lua`（build 时契约）是两套机制，勿混** |

## 已知问题

> 只列**未解决**项。已解决问题（根因/误诊复盘）见 `docs/archive/issue_history.md`。

- `computeCommandPool`/`transferCommandPool`/`computeQueue`/`transferQueue` 已创建但保留给未来使用
- **VMA + zero-init 冲突**：VMA 3.4 对 BDA buffer 自动加内部 DEVICE_ADDRESS flags，与用户
  ZERO_INITIALIZE pNext 必撞——该扩展已回退，真实需求走原生 `vkAllocateMemory` 绕 VMA
  （详见 `docs/vulkan_extensions_2025_调研.md` §十二/§13.1；robustness2 已按 optional 引入 §13.3）
- **Phase 5 光照/透明近似**：MASK alphaTest 未实现；世界法线 mat3 直乘（假定均匀缩放）；透明排序
  per-primitive 距离²（非 OIT）；透明段开背面剔除（非封闭几何会剔掉一面）。留 Phase 7+
- **Phase 5.5 IBL/天空盒近似**：无 prefiltered/BRDF LUT；mip blit NEAREST（sRGB LINEAR 违
  VUID-00239）；单环境贴图；大三角 z=1 依赖 depth clear + LEQUAL；**light/env 两个 intensity
  语义不对称**（light CPU 预乘、env shader 端乘，亮度跨路线不可直接对比）
- **Phase 6 渲染图边界**：线性 pass 序（无拓扑排序/fusion/aliasing）；offscreen 显式格式已落地
  （词表 12 项）；sampleRead RT→heap 接线已落地（D1）；执行器 state/drawSource 名字表认
  opaque/skybox/transparent/light 四个；单 swapchain attachment；多线程录制不做（7b+ 议）
- **`LightDataGpu` 前向头双源**（灯数组已入反射）：forward `LightDataGpu@0-48` 仍手写 struct ↔
  `scene.slang` 字面量手工一致（为 forward diff0 未并反射）；灯数组 @262272 已入反射管线 4
  （static_assert 三方同步）。灯未 entity 化（经 `SceneLights` context，lights[0]=主光）
- **captureFrame 未图化**（Phase 7 D1 债）：llm_debug 截图的 deferred 分支 = 自包含手写 2-pass
  （绕过 `CompiledRenderGraph`，与 drawFrame 平行维护；G-buffer 名单已并入 `kGBufferAttachments`
  单源，pass 逻辑仍双份）。正解 = deferred 下截图走 swapchain 回读不重渲，留后续
- `configs/` 增量拷贝只认"源文件更新"：build 端副本被破坏（mtime 更新）不会自动恢复，需手动拷贝；
  且 `copy_if_different` 按内容比对——**build 端临时改配置会被下次 build 静默覆盖**（截图调试后记得
  改的是 build 副本还是源）

## 其他文档

> 每篇文档头部带状态（`现行`/`历史`/`调研`）。现状权威 = 本文件 + `next_roadmap.md`。

- `docs/next_roadmap.md` — **路线图（权威）**：Phase 0–6 一段话结论 + Phase 7 进度表 + 剩余计划
- `docs/技术概念.md` — 核心 Vulkan 模式、设计决策、**§模块表（工具/数据/生成模块速览）**
- `docs/调试历史.md` — 问题修复时间线（只增不改，争议结论以此为准）
- `docs/DESCRIPTOR_HEAP_SPEC.md` — descriptor heap 排坑记录（活文档）
- `docs/nvidia_compatibility_report.md` — 跨驱动兼容性（三平台记录；当前机 = Strix Halo 核显）
- `docs/device_generated_commands_research.md` — GPU culling 双驱动实测（Phase 7b 依据）
- `docs/vulkan_extensions_2025_调研.md` — 新扩展调研 + §十二/十三 实施记录（活文档）
- `docs/phase7_devlog.md` — Phase 7 滚动开发日志（完结后归档）
- `docs/archive/` — 归档索引（phase 计划含 **phase6_record.md**、ecs 审查、issue_history 总账、
  present_timing/教程拆解等已结题调研）
- 离线 Vulkan 文档（优先在线检索被 Cloudflare 拦截时）：本地克隆 `/home/robin/program/Vulkan-Docs`
  （v1.4.360）。**注意**：spec 比系统头/loader（v341）新，结构体字段以系统头为准，spec 只作语义依据
- 最小复现/验证子项目（独立 CMake，结论已吸收进主工程与 archive）：`min_mesh_crash/`（NVIDIA mesh
  shader 必带 `NO_TASK_SHADER_BIT_EXT` 根因，见其 README）、`heap_texture_spike/`（heap 纹理三驱动
  PASS + 步长取 max，见其 README + archive/heap_texture_research.md）
