# RGL

Vulkan 现代特性学习项目。Mesh shader 渲染管线，使用 ShaderObject + DescriptorHeap + DynamicState。
核心代码位于 rgl 目录下。
legacy（旧 OpenGL 项目）已于 2026-09-08 删除，历史思路参考见 git history（`legacy/` @ < 2026-09-08）。

## 给大模型

优先使用中文回复用户，除非用户说使用英文回复。
随时思考是否可以拆分子任务给子 agent 执行。主 agent 校验结果。困难任务主 agent 亲自执行。
**文档/代码调研可委派 `researcher` 子 agent**（`.opencode/agents/researcher.md`，只读）：适合代码定位
（`文件:行号`）、文档定位、上下文简易归纳；纯"grep/read 到什么报什么"时可靠。**主 agent 仍需校验**：
涉及产物布局/大小须自己复核生成物本体（实测踩过 sizeof 生成物 4B 被它说成 100B）。

## 项目 Skills（按需加载，命中触发词先 skill 工具读取）

- **`rgl-verify`** — 运行环境、headless 冒烟、llm_debug 截图、BIT-IDENTICAL 截图回归（改渲染后验证必用）
- **`rgl-shader-contract`** — 反射管线 / `just reflect` / scalar layout / bindless / 关键契约文件位置表
  （改 `rgl/shaders/`、`*_schema.lua`、`generated/` 前必读）
- **`rgl-new-module`** — 新建/拆分 `.cppm`、rlstd/GMF 细则、impl partition 三条坑、检查工具规则红线

## 构建

项目使用 `just` 管理构建流程（参见 `Justfile`），支持 Linux 和 Windows 双平台。

```bash
just config              # CMake 配置（默认 clang, Release）；just config gcc Debug 指定编译器/类型
just build               # 增量编译（需先 config）；just build-release / build-debug = 一键配置+编译
just reflect             # 重跑 6 条反射管线（改动 shader 契约/schema/模板后必须，见 skill rgl-shader-contract）
just doc-check [--strict]    # Doxygen 注释检查（每次 just build 已随 doc_check 并行跑、只警告）
just style-check [--strict]  # 文件/函数结构检查（红线清单见 skill rgl-new-module）
just run                 # 运行（AMD VGA 存在时自动设 RADV_EXPERIMENTAL=heap）
just run-igpu / run-dgpu / run-intel   # 强制核显 / NVIDIA prime 独显 / Intel anv
just clean / clean-all / all           # 增量清理 / 删 build 目录 / clean-all+build-release
```

**Windows (NVIDIA + LLVM 22)**：前置 LLVM 22、Vulkan SDK（slangc）、just、vcpkg（已 bootstrap）、
Git for Windows（`sh.exe` 在 PATH）；命令同名，默认 clang++ + Ninja、triplet `x64-windows-static`，
`run-igpu/run-dgpu` 不可用（Linux prime 专用），NVIDIA 直接 `just run`。
**vcpkg**：`vcpkg.json` 用 platform expressions 按 OS 启用 features（Linux `sdl3[vulkan,wayland]` /
Windows `sdl3[vulkan]`）；同一依赖可出现两次用 `"platform"` 字段区分。

CMake 无独立构建目录——必须从根目录构建（`cmake -B build -S .`）。产物: `build/rgl/rgl`（Windows 加 `.exe`）。
构建内置检查步骤（均只警告不阻断，与编译并行）：`reflection_check`（反射产物 SHA1 护栏）、
`doc_check`（声明处 `/** */` 注释，`tools/doccheck/`）、`style_check`（八类规则，`tools/stylecheck/`）。
三者内部机制（缓存、并发、已知坑）→ `docs/技术概念.md §构建检查工具机制`；tools/ 工具链说明 → 同文件 §构建工具。

## 运行要点

- **CWD 必须是 `build/rgl/`**（相对路径读 `configs/` 与 `generated_shaders/*.spv`）；RADV 下
  `RADV_EXPERIMENTAL=heap` 必需（`just run` 自动，直接跑二进制须手动带）
- **运行模式全配置化**：`configs/runtime.lua`（headless/llm_debug/窗口）、`configs/diagnostics.lua`
  （诊断开关）、`configs/render_graph.lua`（`variant` 选 forward/deferred/offscreen）；
  仅驱动级 env 保留，由 `just run-*` 注入
- **configs 的 build 端副本会被下次 `just build` 按内容比对静默覆盖复原**——临时调试改
  `build/rgl/configs/`，别改源；被破坏的 build 副本不会自动恢复需手动拷
- 冒烟命令、headless/llm_debug 截图流程、截图回归方法学 → skill `rgl-verify`

## 命名规范

**所有 C++ 符号均在 `namespace RGL` 中**，模块导出包裹 `namespace RGL { ... }`；入口
`main_impl.cppm`（module main:impl，2026-09-14 由 main.cpp 转来）用 `using namespace RGL;`，
跨模块引用统一 `RGL::` 前缀。

## 代码规范

### 模块优先
新代码**必须** C++20 modules（`.cppm`），非 `.hpp/.cpp` 头文件对；入口 `import` 导入，不在模块外
`#include` 项目内部头文件。新建/拆分的完整 checklist → skill `rgl-new-module`。
入口也是模块（2026-09-14 起，纳入 stylecheck/doccheck 全规则）：`main.cppm`（`export module main;`
接口：启动流 helper 声明 + 轻量 struct）+ `main_impl.cppm`（`module main:impl;`：轻函数 +
`main()`，main 合法于分区实现单元）+ `main_scene.cppm`（`module main:scene;`：重函数定义）——
CMake CXX_MODULES FILE_SET 不收纯实现单元，故"接口声明 + 分区实现"形状（同 ecs/ecs_impl）。

### 优先使用 rlstd 代替 #include STL
`.cppm` 一律 `import rlstd;` 取 STL 类型，GMF 不 `#include` STL（例外仅 `<ranges>/<filesystem>/<chrono>`）；
第三方头之外的 Vulcan C++ 头一律 `import rlvk;`；GMF 要收敛（GMF 解析是模块编译耗时大头）。
新加 STL 名先补 `rlstd.cppm`——**rlstd 必须导出"完整可用面"（含自由运算符/begin/end 等，BMI 不带自由
运算符）**，细则与坑 → skill `rgl-new-module`。

### 接口瘦身：大函数体放 partition implementation unit
重模块（函数体 >100 行）拆 `xxx_impl.cppm`（`module xxx:impl;`，CMake 与接口同登记 FILE_SET）：
**接口 = 声明 + 轻量类型 + 文档注释，实现 = 叶子单元**。三条坑（实现单元显式 `import xxx;`、
实现侧不带 `export`、默认实参只写接口）→ skill `rgl-new-module`。

### 函数式 Ranges
优先 C++23 `std::ranges`：视图适配器、`std::ranges::for_each` 替换原始循环、链式组合避免中间容器。

### 成员变量命名
成员自然命名，不加 `_`/`m_` 前缀后缀（`int count;` ✅）。

### ECS 设计规范
场景代码（ecs/scene/model_importer + 相关 system）必须符合五条规范
（逐条处置记录：`docs/archive/ecs_compliance_review.md`）：

1. **registry 是场景唯一数据源**——场景状态不在 registry 外平行维护（每帧/配置状态进 context；资产数据进 `Resources::AssetTable`）
2. **entity 是 ID、component 是纯数据、system 是逻辑唯一载体**——system 间经数据通信，执行顺序显式（显式 system 列表，不用 entt::scheduler）
3. **场景构成数据驱动**——"场景里有什么" = registry 里有什么 entity，不用全局变量/config 字段/构造参数决定构成（画不画 = 实体有无）
4. **分层**——底层 Vulkan 类不依赖 ECS、不认识具体资产（Renderer 零 entt 依赖）；渲染 system 产出抽象 draw list（`FrameDrawList`），执行器只盲执行
5. **资产与场景实例分离**——组件引用稳定资产 ID（`MeshRef.asset`/`MaterialRef.material`），不引侧数组下标；五方一致性（entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args）收敛为资产表一处映射

## 模块结构（C++20 modules, .cppm）

**实现细节以各 `.cppm` 文件头注释为权威**，Phase 演进史见 `docs/next_roadmap.md`；
工具/数据/生成模块速览 → `docs/技术概念.md §模块表`；下表深版职责 → 同文件 §核心运行时模块表（深版）。
此处只留初始化层骨架（构造顺序 = main 启动流顺序）：

| 模块 | 文件 | 职责（一行） |
|---|---|---|
| `vk_instance` | vkinstance.cppm | Context、Instance、DebugMessenger（headless 换 `VK_EXT_headless_surface`） |
| `vk_device` | vkdevice.cppm | PhysicalDevice 选择、QueueFamilyManager、Device、CommandPools、FeatureChain |
| `vk_swapchain` | vkswapchain.cppm | Surface、Swapchain、ImageView[] format/extent/presentMode |
| `vk_resources` | vkresources.cppm | VMA 分配器、depth、VmaDescriptorHeap、scene data 单例（头 128B + 尾区灯数组/per-object 表，字节布局契约见 scene.slang）、env cubemap、G-buffer RT 缓存 |
| `vk_renderer` | vkrenderer.cppm | **盲执行器**：`drawFrame(CompiledRenderGraph)`（零 entt）= beginFrame→writeSceneStaging→cullDriver→recordFixedDataPath→pass 循环（SlotResolver/stateKit/executePassDraws 三路分发）→capture→present→submit；构造期 `validateRenderGraphForExecutor` fail-fast |
| `frame_session` | frame_session.cppm | 帧生命周期会话：双 fence 同步 + 每帧 CB + beginFrame/submit/present/advance + swapchain/depth 重建 |
| `frame_capture` | frame_capture.cppm | 帧内回读捕获器（llm_debug 截图载体）：request/record/collect 三步曲 |
| `cull_driver` | cull_driver.cppm | GPU culling 帧编舞：`runFrame`（dispatch→preprocess→同帧等待→readCull→reconcile）+ `executeOpaqueSegment` 三路分发（IES DGC / 静态 DGC / 读回重放） |
| `state_kit` | state_kit.cppm | pass 动态状态表：四段 RenderState（opaque/transparent/skybox/light）+ `stateFor(name)` |
| `render_graph` | render_graph.cppm | 纯 C++ 渲染意图图：`compileRenderGraph`（校验 + barrier 推导 + presentTransition）→ `CompiledRenderGraph`；三变体 + `kGBufferAttachments` 契约表 |
| `render_graph_config` | render_graph_config.cppm | `configs/render_graph.lua` → `RgGraphDesc`（sol2 沙箱，缺文件回落默认） |
| `shader_registry` | shader_registry.cppm | 管线表（零 entt）：addModelEntry/addCubeEntry/addSkyEntry/addLightEntry/bindPipelineToAsset，条目全按需建 |
| `gpu_cull` | gpu_cull.cppm | GPU culling 数据通路：CullEntry/序列/count buffer + seqStaging ring×2；recordCullPass/readCullResult（字节布局 → 深版表 + skill rgl-shader-contract） |
| `vk_indirect_commands` | vk_indirect_commands.cppm | DGC RAII（能力门控）：`DgcCommands`（NVIDIA IES 路）/ `DgcStaticCommands`（RADV 静态路，默认关）；7 入口点全走 `vkGetDeviceProcAddr` |

**main 启动流**（main_impl.cppm；完整版 → `docs/技术概念.md §main 启动流`；析构 = 构造逆序 RAII）：
加载四类配置 + `renderGraphSelfCheck` → 建窗（headless 不建）→ 栈上 `entt::registry` →
AppContext→DeviceContext→Swapchain→Resources→ShaderRegistry → `compileRenderGraph`（建窗后）→
entities 循环：A0 内建几何→A1 模型顶点→A2 纹理→env 上传→C 模型注册+C2b 灯 entity →
C3 候选表+`GpuCull` → addSkyEntry/deferred 装配 → DgcCommands/DgcStaticCommands 条件构造 →
`Renderer` → `renderSystem`。
关键时序契约：heap 布局"几何槽先于纹理槽"（A0/A1/A2 拆分，见 `AssetTable::expectedGeometrySlot`）；
env 上传 = A2 后、addModelEntry 前；候选表迭代序与 render_list CPU 路径完全一致；
deferred 装配与 DGC 构造**先于 Renderer**。

### 构造依赖图

```
SDLWindow → AppContext → DeviceContext → Swapchain → Resources → Renderer
                └────────────── entt::registry（WindowExtent/ViewProjection 等每帧 context）──────────┘
```

依赖图原则：只允许下层 import 上层，**配置模块不依赖执行模块**（camera_config 不 import llm_debug）、
**ecs 不依赖 llm_debug**（截图经回调）——假边会让慢模块进入本不该等它的编译前置。

### 重要成员生命周期规则

- `vkinstance.cppm`：`engineInfo`、`instanceCreateInfo`、`layers`、`instanceExtensions` 均为类成员——`pApplicationInfo` 不能悬空
- `vkdevice.cppm`：`featuresChain` 为类成员——`DeviceCreateInfo.pNext` 不能悬空
- 所有 `vk::raii::*` 对象必须通过**成员初始化列表**初始化，绝不在函数体内初始化（默认构造函数已删除）
- 成员声明顺序必须与初始化依赖顺序一致
- **GPU 资源所有权**：长生命周期 GPU 资源（纹理 BO/描述符存储）不得声明在 main() 局部块内——块尾析构 = 第 0 帧起 GPU 采样已释放 VA（UAF，见 `docs/archive/issue_history.md`）

## 着色器编译

Slang → SPV 构建时编译到 `build/rgl/generated_shaders/`。硬规则：**改 shader 契约/schema/模板后必须
`just reflect` 并随改动提交生成产物**（`generated/*.cppm` 是已提交产物，构建开头 SHA1 护栏比对）；
slangc 反射与 SPV 两处 scalar layout flag 必须一致；push constant 冻结在纯索引级别。
完整流程、bindless 原则、关键契约文件位置表（DrawArgs/scene.slang 字节布局等）→ skill `rgl-shader-contract`。

## 隐式扩展依赖

`features.cppm` 请求的 `VK_EXT_DESCRIPTOR_BUFFER`、`VK_KHR_SHADER_UNTYPED_POINTERS` 等
（`kDeviceExtensions()` 9 项）除 `VK_EXT_DEVICE_GENERATED_COMMANDS`（gpu_cull/vk_indirect_commands
显式使用）外均无显式使用，但被其他已启用扩展隐式依赖，不可随意删除；`vkinstance.cppm` 的
`VK_EXT_SWAPCHAIN_COLOR_SPACE` 同理。**KHR/EXT 别名**：`VkSwapchainPresentFenceInfoKHR/EXT` 是同一结构（typedef alias），代码用 EXT。

## 外部依赖（通过父 CMake 链接）

`rlstd`（根目录 STL 模块库）、`rllogger`（根目录日志库，`RLLogger::getInstance()`）、spdlog（经
rllogger）、glm（depth zero-to-one + 弧度制）、SDL3、VulkanMemoryAllocator、
slangc（`cmake/SlangFetch.cmake` FetchContent pin v2026.16 + sha256；Windows 优先 Vulkan SDK 自带）

## 已知问题

> 只列**未解决**项。已解决问题（根因/误诊复盘）见 `docs/archive/issue_history.md`。

- `computeCommandPool`/`transferCommandPool`/`computeQueue`/`transferQueue` 已创建但保留给未来使用
- **VMA + zero-init 冲突**：VMA 对 BDA buffer 自动加内部 DEVICE_ADDRESS flags，与用户 ZERO_INITIALIZE
  pNext 必撞——该扩展已回退，真实需求走原生 `vkAllocateMemory` 绕 VMA（`docs/vulkan_extensions_2025_调研.md` §十二/§13.1）
- **Phase 5 光照/透明近似**：MASK alphaTest 未实现；世界法线 mat3 直乘（假定均匀缩放）；透明排序
  per-primitive 距离²（非 OIT）且开背面剔除（非封闭几何会剔掉一面）。留 Phase 7+
- **Phase 5.5 IBL/天空盒近似**：无 prefiltered/BRDF LUT；mip blit NEAREST（sRGB LINEAR 违 VUID-00239）；
  单环境贴图；大三角 z=1 依赖 depth clear + LEQUAL；**light/env 两个 intensity 语义不对称**
  （light CPU 预乘、env shader 端乘，亮度跨路线不可直接对比）。简化 IBL forward/deferred 逐式相同；
  skybox 背景在 deferred light pass 内合成（非独立 pass）
- **Phase 6 渲染图边界**：线性 pass 序（无拓扑排序/fusion/aliasing）；执行器 state/drawSource 只认
  opaque/skybox/transparent/light 四个名字；单 swapchain attachment；多线程录制不做
- **灯契约（现行）**：`Light` 组件 + `index` = 列表位正序（画不画 = 实体有无；forward 读 index 0 = 主光，
  延迟按 index 落槽）；`SceneLight`（前向头）/`GpuLight`（灯数组）布局均走 C API 反射生成，无手写双源
- **GPU culling（Phase 7b）现状**：默认 = compute culling + **读回重放**（三驱动均跑 culling compute，
  无开关）；DGC execute 仅 NVIDIA graphicsIndirect=true 可走，本机 RADV（9950X Raphael RDNA2）与 Intel
  anv 均 false → 重放；RADV 静态 shader 模式 spike 功能通但性能亏 1.85× → `diagnostics.dgc_static` 门默认关。
  CPU 剔除保留为兜底 + 每帧对账基准（reconcile 不一致告警不中断）；mvp 由 CPU 算进 scene data per-object
  表（GPU float4x4 FMA 收缩序与 glm 无位级契约，保 forward diff0）。驱动级细节/历史
  → `docs/radv_static_dgc_spike.md` / `docs/nvidia_compatibility_report.md` / `docs/device_generated_commands_research.md` / 调试历史 2026-09-10

## 其他文档

> 每篇文档头部带状态（`现行`/`历史`/`调研`）。现状权威 = 本文件 + `next_roadmap.md`。

- `docs/next_roadmap.md` — **路线图（权威）**：Phase 0–6 一段话结论 + Phase 7 进度表 + 剩余计划
- `docs/技术概念.md` — 核心 Vulkan 模式、设计决策、§模块表 / §构建检查工具机制 / §核心运行时模块表（深版）/ §main 启动流（完整版）
- `docs/调试历史.md` — 问题修复时间线（只增不改，争议结论以此为准）
- `docs/DESCRIPTOR_HEAP_SPEC.md` — descriptor heap 排坑记录（活文档）
- `docs/nvidia_compatibility_report.md` — 跨驱动兼容性（三平台记录；当前机 = 9950X Raphael RDNA2 核显，非 Strix Halo）
- `docs/device_generated_commands_research.md` — GPU culling 双驱动实测（Phase 7b 依据）
- `docs/radv_static_dgc_spike.md` — RADV 静态 shader DGC spike（功能通、慢 1.85× → 默认不开；翻盘设计底稿）
- `docs/vulkan_extensions_2025_调研.md` — 新扩展调研 + §十二/十三 实施记录（活文档）
- `docs/phase7_devlog.md` — Phase 7 滚动开发日志（完结后归档）
- `docs/archive/` — 归档索引（phase 计划含 **phase6_record.md**、ecs 审查、issue_history 总账、present_timing/教程拆解等已结题调研）
- 离线 Vulkan 文档（优先在线检索被 Cloudflare 拦截时）：本地克隆 `/home/robin/program/Vulkan-Docs`（v1.4.360）。**注意**：spec 比系统头/loader（v357）新，结构体字段以系统头为准，spec 只作语义依据
- 最小复现/验证子项目（独立 CMake，结论已吸收进主工程与 archive）：`min_mesh_crash/`（NVIDIA mesh shader 必带 `NO_TASK_SHADER_BIT_EXT` 根因，见其 README）、`heap_texture_spike/`（heap 纹理三驱动 PASS + 步长取 max，见其 README + archive/heap_texture_research.md）
