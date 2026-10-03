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

- **`rgl-verify`** — 运行环境、headless 冒烟、llm_debug 截图、BIT-IDENTICAL 截图回归、材质定位（加法闪色）、截 UI（外部截图）、测量纪律（改渲染后验证必用）
- **`rgl-shader-contract`** — 反射管线 / `just reflect` / scalar layout / bindless / 关键契约文件位置表
  （改 `rgl/shaders/`、`*_schema.lua`、`generated/` 前必读）
- **`rgl-new-module`** — 新建/拆分 `.cppm`、rlstd/GMF 细则、impl partition 三条坑、检查工具规则红线
- **`rgl-test-discipline`** — 写测试硬纪律全量（阳性锚点/变异红线/沉淀地址/modules 坑；写单测、加用例、收账调试历史前必读）
- **`rgl-doc-placement`** — 改 AGENTS.md 前的归宿裁决五问（测试/护栏/skill/AGENTS/docs）+ 先补目的地再删源 + 入册前实测（**拟向 AGENTS 增删任何段落前必读**）

## 构建

项目使用 `just` 管理构建流程（参见 `Justfile`），支持 Linux 和 Windows 双平台。

```bash
just config              # CMake 配置（默认 clang, Release）；just config gcc Debug 指定编译器/类型
just build               # 增量编译（需先 config）；just build-release / build-debug = 一键配置+编译
just reflect             # 重跑 7 条反射管线（改动 shader 契约/schema/模板后必须，见 skill rgl-shader-contract）
just checks / doc-check / style-check [--strict]  # 源码检查统一入口（tools/checks/main.py，--only style|doc；每次 just build 已随 source_checks 目标跑、只警告；红线清单见 skill rgl-new-module）
just run                 # 运行
just run-igpu / run-dgpu / run-intel   # 强制核显 / NVIDIA prime 独显 / Intel anv
just test                # 单元测试（tests/，**无 GPU 无窗口**，毫秒级；排除 gpu 套件）
just test-all / test-gpu # 含设备级原语用例（要真设备 + VVL；无设备**FAIL 不 SKIP**）
just analytic            # 闭式解探针：算像素「该是多少」并与实机截图逐字节对拍（要 GPU；
                         #   12.3b 起 = imgqa probe，Python 参考实现已退役）
just imgqa <子命令>       # 区域归因判据工具箱：selftest/attribution/faces/switch/check/probe（判据纪律 → skill rgl-verify）
just clean / clean-all / all           # 增量清理 / 删 build 目录 / clean-all+build-release
```

**Windows 前置（LLVM 22/Vulkan SDK/vcpkg）与 vcpkg platform expressions** → `docs/技术概念.md §构建（Windows 前置与 vcpkg）`。
**工具语言纪律（2026-09-30 用户拍板，三机测试）**：要依赖的工具一律 **C++ + vcpkg**（先例：
inja 弃 jinja、imgqa 链 opencv4）；**留在 Python 的工具只准 stdlib-only**（qa_scenes.py），
不引入任何需要 pip 的依赖。

CMake 无独立构建目录——必须从根目录构建（`cmake -B build -S .`）。产物: `build/rgl/rgl`（Windows 加 `.exe`）、`build/tests/rgl_tests`（单元测试；`tests/` 是 EXCLUDE_FROM_ALL 子目录，故**不进 `just build`**，`just test` 显式构建其 target）。
构建内置检查步骤（均只警告不阻断）：`reflection_check`（反射产物 SHA1 护栏，与编译并行）+ `source_checks`（样式 + Doxygen 单入口 `tools/checks/`；**红线：依赖 rgl 编译完成后才跑——libclang 读半写 BMI 会崩，勿删该 add_dependencies**）。两者内部机制（缓存/并发/已知坑）→ `docs/技术概念.md §构建检查工具机制`；样式规则清单 → `docs/风格检查规则.md`。

## 测试体系（Phase 12 批 12.1 起）

> **下一步做什么** → `docs/phase12_plan.md`（批次史 + 缺口清单 D1–D9 + 计划批次）；本节 = 现状机制描述。

**三层，边界按"要不要 GPU / 要不要窗口"划**：

| 层 | 位置 | 覆盖 | 入口 |
|---|---|---|---|
| 单元测试层 | `tests/`（doctest，C++20 modules） | **纯函数/契约**：阴影级联几何、相机数学、glTF 解析、四份配置 loader、反射字节布局、内建 cube 几何、**`state_kit` 十段状态表 + `pass_vocabulary` 词表分区**、**`render_graph` 编译（校验/barrier/装饰）**。**无 GPU 无窗口，毫秒级** | `just test` |
| 设备原语层 | `tests/` 的 `TEST_SUITE("gpu")` | **后端原语 golden**：clear 色逐像素、R32_UINT 魔数整数回环、UNORM 量化取整（0.5→128）、image↔buffer 逐字节保真 + 行序 | `just test-all` / `just test-gpu` |
| 闭式解探针 | `tools/imgqa/`（probe 子命令）+ `qa/configs/analytic/` | 「某像素**该**是多少」的独立第二实现（GGX + ACES + 硬件 sRGB 逐式复刻，与引擎共享真机锚点），与实机截图逐字节对拍。**不需要基线图像**（12.3b 接替 analytic_ref.py） | `just analytic` |
| 区域归因判据 | `tools/imgqa/`（C++ + vcpkg，**不进 ALL**） | objectID 反解可见 piece + 闭式解逐面预测 vs 观测亮度簇 + 开关影响域（域外逐位为零）+ 反模式登记（ssim 等有理由的拒绝）。**排查渲染问题的默认起手** | `just imgqa selftest` / `attribution` / `faces` / `switch`（判据晋升与台账纪律 → `tools/imgqa/imgqa.h` 头注释 + skill rgl-verify） |
| 进程级集成层 | `qa/`（批 12.2 起**部分落地**：`configs/analytic/` 探针配置） | 图像/场景级：程序化小场景 + RPC 驱动 + OpenCV 量化 | 场景生成 `tools/qa_scenes.py`；量化 harness 未落地 |

### 分流靠 doctest **测试套件**，不是 tag 也不是链接面（批 12.2d）

本项目 doctest 2.5.3 的 `TEST_CASE` 宏**只接受一个参数**（tag 形态在该构建里不存在）；设备级用例写进
`TEST_SUITE("gpu") { … }`，靠一等公民的套件过滤分流：`just test` = `--test-suite-exclude=gpu`，`just test-all` = 全跑。

**`rgl_tests` 现在链全部四层**（`rlbase rlgfx rlscene rlframe` + Vulkan/VMA/SDL3/imgui）：决策结论 =
「import rlvk」是编译期依赖，「需要一个 VkDevice」才是运行期依赖，两者不等价（`render_graph`/`state_kit` 都 import rlvk 却一个设备都不碰）；代价如实记录 = 测试二进制链了 Vulkan（`nm` 不再是 0），保住的是**运行期**性质（默认子集一条设备路径都不碰，仍是毫秒级）；设备级用例在无设备机器上**必须 FAIL 不 SKIP**——静默跳过 = 假绿。"为什么不只链 rlbase"细节 → `tests/CMakeLists.txt`（链接面注释，头注释有指针）。

### 引擎四库分层（rlstd/rllogger 之后新增四个库）

`rgl` 是**可执行文件**（`main` `main:impl` `main:scene` 三单元含 `main()`，必须留可执行文件；其 CXX_MODULES FILE_SET 只有它自己能 import），故可测模块必须住进库。批 12.1 先用「import 闭包不含 Vulkan」判据，12.2c 改判为**按功能分四层**——那条闭包判据把「import Vulkan」这个**动作**与「能不能不建设备就测」这个**能力**混成一条（实测 13 条切边里 12 条徒劳，vk_resources 是枢纽）。

| 库 | 内容（模块清单以 `rgl/CMakeLists.txt` 五份清单为唯一真相源） | 需设备 |
|---|---|---|
| `rlbase` | 值类型/数学/四份配置 loader/glTF/图像编解码/反射产物/profiler/RPC 传输/param_registry | **0（整层无设备依赖）** |
| `rlgfx` | `rlvk` `VmaRaii` vulkan_utils vulkan_window queue_family_manager features shader state_kit gpu_profiler vk_instance/vk_device/vk_swapchain/vk_resources render_target_cache sampler_library mip_gen asset_table | 全部 |
| `rlscene` | scene_data render_graph\* render_graph_config environment env_prefilter mip_gen 的消费侧 texture_\* model_importer builtin_geometry render_list frame_draw_list | 全部 |
| `rlframe` | frame_session/frame_capture/picking/cull_driver/gpu_cull/gpu_canary/present_timing/vk_indirect_commands/vk_renderer/imgui_layer/debug_rpc_exec/llm_debug/shader_registry/shader_hot_reload/ecs/param_pages | 全部 |

依赖只向下，出现逆向 import 立刻链接失败，**现在机器可验**：`tools/checks/rules_layers.py` 以那五份清单为唯一真相源，扫全部 `.cppm`（**含分区实现单元**）的 import 边，报「低层 import 高层」为 error 级警告；规则可能静默失效时也报 error（失效不出声 = 「零警告」会被读成「分层干净」）。旧问法「这个模块 import 了 Vulkan 吗」已废。
**新增模块的流程**：加进职责归属正确的那份清单 → configure 期**归属完整性闸门**核对「磁盘上每个 .cppm 恰好登记一次」，漏登记即 `FATAL_ERROR` 点名（刻意不用「GLOB 后减掉已登记」——那会让新文件静默落进 `rgl`，而那里只该有 `main*`）。判据只有一个：**这段逻辑需不需要一个 VkDevice 才能跑**——不需要 → 属 `rlbase`，就该能在 `just test` 里测；需要 → 设备级测试或 `qa/`。

⚠️ 两条踩过的坑（include 面/编译宏归属判据 = 哪个库拥有用它的那个 TU，不是"谁在用它"；勿用 model.json 做依赖分析）→ `rgl/CMakeLists.txt` 头注释已一字不差承载（"model.json"与"include 面/编译宏的归属判据"两个 ⚠️ 段）。

### 写测试的硬纪律（全量十条 + 12.3d 新规 → skill `rgl-test-discipline`）

一句话版：每条断言要有**阳性锚点**（代码从未生效它会绿吗）· guard/早退分支配**反向用例** · 随机性只用 `Lcg` + 写死种子 ·
**辅助函数也要独立判据**（判据噪声远小于信号）· **验证靠变异测试**（"测试通过"≠"测试有效"）· 每条测试带**「变异红线」声明**（12.3d）·
调试历史条目带**「沉淀地址」** `TEST`/`GUARD`/`DOC` 三选一（12.3d）· 断言**照源码写**不照记忆、期望值出处**独立于实现**（数学契约/闭式解/实机实测，12.3d）·
判据工具自己要有自检 · 单测结果以 `just test` 为准（先构建；直接跑二进制可能拿陈旧二进制）。

## 运行要点

- **工作机器不著书**（2026-10-02 用户拍板：三台机器轮换，一台干完换一台测）：开工先认设备
  （启动日志 deviceName / vkinfo）；本文与 docs 里的性能数字均**绑特定机器**，跨机先复测再对比。

- **CWD 必须是 `build/rgl/`**（相对路径读 configs/ 与 spv）· 运行模式全配置化（runtime/diagnostics/render_graph.lua）· build 端 configs 副本 = `copy_if_different`（**只源更新才覆盖**；改副本调试不会自动复原——复原靠 `cp configs/*.lua build/rgl/configs/` 或 touch 源，2026-10-02 实测纠正旧表述"静默覆盖复原"）；
  运行环境细节、冒烟命令、headless/llm_debug 截图流程、截图回归方法学 → skill `rgl-verify`
- **RPC 调试通道**（端点速查 / 线程时序 / cameraAimAt 反推与启动探测 → `docs/技术概念.md §RPC 调试通道`）：
  门 `diagnostics.rpc` **默认关** + `rpc_port = 8788`（只绑 127.0.0.1；与 imgui 门独立 → headless 可用）；起跑前 `pgrep -cx rgl` 确认无残留
- "哪个材质占这块屏幕"用**加法闪色**（改 emissive 不改 baseColor）· "截 UI"验收**必须外部截图** → skill `rgl-verify`

## 命名规范

**所有 C++ 符号均在 `namespace RGL` 中**，模块导出包裹 `namespace RGL { ... }`；入口
`main_impl.cppm`（module main:impl，2026-09-14 由 main.cpp 转来）用 `using namespace RGL;`，
跨模块引用统一 `RGL::` 前缀。

## 代码规范

- **模块优先**：新代码**必须** C++20 modules（`.cppm`），非 `.hpp/.cpp` 头文件对；入口 `import` 导入，不在模块外 `#include` 项目内部头文件。入口也是模块三件套：`main.cppm`（接口：启动流 helper 声明 + 轻量 struct）+ `main_impl.cppm`（`module main:impl;`，含 `main()`）+ `main_scene.cppm`（`module main:scene;` 重函数）。新建/拆分完整 checklist → skill `rgl-new-module`。
- **优先使用 rlstd 代替 #include STL**：`.cppm` 一律 `import rlstd;` 取 STL 类型，GMF 不 `#include` STL（例外仅 `<ranges>/<filesystem>/<chrono>`），第三方头之外的 Vulcan C++ 头一律 `import rlvk;`（GMF 要收敛，解析是模块编译耗时大头）；新加 STL 名先补 `rlstd.cppm`——**rlstd 必须导出"完整可用面"（含自由运算符/begin/end 等，BMI 不带自由运算符）**。细则与坑 → skill `rgl-new-module`。
- **接口瘦身：大函数体放 partition implementation unit**：重模块（函数体 >100 行）拆 `xxx_impl.cppm`（`module xxx:impl;`，CMake 与接口同登记 FILE_SET）：**接口 = 声明 + 轻量类型 + 文档注释，实现 = 叶子单元**。三条坑（实现单元显式 `import xxx;`、实现侧不带 `export`、默认实参只写接口）→ skill `rgl-new-module`。

### 函数式 Ranges
优先 C++23 `std::ranges`：视图适配器、`std::ranges::for_each` 替换原始循环、链式组合避免中间容器。

### 成员变量命名
成员自然命名，不加 `_`/`m_` 前缀后缀（`int count;` ✅）。

### ECS 设计规范
场景代码（ecs/scene/model_importer + 相关 system）必须符合五条规范
（逐条处置记录：`docs/archive/ecs_compliance_review.md`）：

1. **registry 是场景唯一数据源**——场景状态不在 registry 外平行维护（每帧/配置状态进 context；资产数据进 main 装配层持有的 `AssetTable`）
2. **entity 是 ID、component 是纯数据、system 是逻辑唯一载体**——system 间经数据通信，执行顺序显式（显式 system 列表，不用 entt::scheduler）
3. **场景构成数据驱动**——"场景里有什么" = registry 里有什么 entity，不用全局变量/config 字段/构造参数决定构成（画不画 = 实体有无）
4. **分层**——底层 Vulkan 类不依赖 ECS、不认识具体资产（Renderer 零 entt 依赖）；渲染 system 产出抽象 draw list（`FrameDrawList`），执行器只盲执行
5. **资产与场景实例分离**——组件引用稳定资产 ID（`MeshRef.asset`/`MaterialRef.material`），不引侧数组下标；五方一致性（entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args）收敛为资产表一处映射

## 模块结构（C++20 modules, .cppm）

**实现细节以各 `.cppm` 文件头注释为权威**，Phase 0–7 演进史见 `docs/next_roadmap.md`（Phase 8+ 见各 `phase*_plan.md`）；
工具/数据/生成模块速览 → `docs/技术概念.md §模块表`；下表深版职责 → 同文件 §核心运行时模块表（深版）。
此处只留初始化层骨架（构造顺序 = main 启动流顺序）：

| 模块 | 文件 | 职责（一行） |
|---|---|---|
| `vk_instance` | vkinstance.cppm | Context、Instance、DebugMessenger（headless 换 `VK_EXT_headless_surface`） |
| `vk_device` | vkdevice.cppm | PhysicalDevice 选择、QueueFamilyManager、Device、CommandPools、FeatureChain |
| `vk_swapchain` | vkswapchain.cppm | Surface、Swapchain、ImageView[] format/extent/presentMode |
| `vk_resources` | vkresources.cppm | VMA 分配器、depth、resource/sampler VmaDescriptorHeap、descriptor heap 用户槽三段分配器（geometry / texture / runtime） |
| `vk_renderer` | vkrenderer.cppm / _impl / _passrec | **盲执行器**：`drawFrame(CompiledRenderGraph)`（零 entt）= beginFrame→writeSceneStaging→cullDriver→recordFixedDataPath→pass 循环（SlotResolver/stateKit/executePassDraws 三路分发）→capture→present→submit；构造期 `validateRenderGraphForExecutor` fail-fast |
| `frame_session` | frame_session.cppm | 帧生命周期会话：双 fence 同步 + 每帧 CB + beginFrame/submit/present/advance + swapchain/depth 重建 |
| `frame_capture` | frame_capture.cppm | 帧内回读捕获器（llm_debug 截图载体）：request/record/collect 三步曲 |
| `cull_driver` | cull_driver.cppm | GPU culling 帧编舞：`runFrame`（dispatch→preprocess→同帧等待→readCull→reconcile）+ `executeOpaqueSegment` 三路分发（IES DGC / 静态 DGC / 读回重放） |
| `state_kit` | state_kit.cppm | pass 动态状态表：十段 RenderState + `stateFor(name)`（表驱动；名字与分档的真相源 = `pass_vocabulary`，值语义在此） |
| `render_graph` | render_graph.cppm | 纯 C++ 渲染意图图：`compileRenderGraph`（校验 + barrier 推导 + presentTransition）→ `CompiledRenderGraph`；三变体 + `kGBufferAttachments` 契约表 |
| `render_graph_config` | render_graph_config.cppm | `configs/render_graph.lua` → `RgGraphDesc`（sol2 沙箱，缺文件回落默认） |
| `shader_registry` | shader_registry.cppm | 管线表（零 entt）：addModelEntry/addCubeEntry/addSkyEntry/addLightEntry/bindPipelineToAsset，条目全按需建 |
| `picking` | picking.cppm / _impl | GPU id 点选服务（批 10.5）+ **高亮 pass 装饰与按材质过滤（批 10.7）**：1-texel r32ui 回读 + `SelectedObject` ctx；highlight pass 写既有色附件插 tonemap 前 |
| `gpu_cull` | gpu_cull.cppm | GPU culling 数据通路：CullEntry/序列/count buffer + seqStaging ring×2；recordCullPass/readCullResult（字节布局 → 深版表 + skill rgl-shader-contract） |
| `vk_indirect_commands` | vk_indirect_commands.cppm | DGC RAII（能力门控）：`DgcCommands`（NVIDIA IES 路）/ `DgcStaticCommands`（RADV 静态路，默认关）；7 入口点全走 `vkGetDeviceProcAddr` |

**main 启动流**（main_impl.cppm；完整版 → `docs/技术概念.md §main 启动流`；析构 = 构造逆序 RAII）：
加载四类配置 + `renderGraphSelfCheck` → 建窗（headless 不建）→ 栈上 `entt::registry` → AppContext→DeviceContext→Swapchain→Resources→ShaderRegistry → `compileRenderGraph`（建窗后）→ entities 循环（A0 内建几何→A1 模型顶点→A2 纹理→env 上传→C 模型注册 + C2b 灯 entity）→ C3 候选表 + `GpuCull` → addSkyEntry/deferred 装配 → DgcCommands/DgcStaticCommands 条件构造 → `Renderer` → `wireDebugRpc` → `renderSystem`。
关键时序契约：heap 用户槽按用途段分配（几何段仍由 `AssetTable::expectedGeometrySlot` 校验，纹理首槽 = 白 = 纹理段首槽）；env 上传 = A2 后、addModelEntry 前；候选表迭代序与 render_list CPU 路径完全一致；deferred 装配与 DGC 构造**先于 Renderer**。

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

Slang → SPV 构建时编译到 `build/rgl/generated_shaders/`。硬规则：**改 shader 契约/schema/模板后必须 `just reflect` 并随改动提交生成产物**（`generated/*.cppm` 是已提交产物，构建开头 SHA1 护栏比对）。
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

> 只列**未解决问题**与标「现行」的**契约**。已解决问题（根因/误诊复盘）见 `docs/archive/issue_history.md`。

- **Phase 9 性能基线与判定（2026-09-27 收口）**：本机 forward 1280×720 = **CPU-bound**（profiler 判定行 `frame=7.10ms cpu.record=7.03ms gpu.frame=2.98ms`）→ ④c"cull 读回延后一帧"**否决**、⑤"帧率上限"**当前不需要**（CPU 限速 ≈141 fps 低于 165 Hz，`drop` 双零）；给阴影（Phase 11）的硬基线 = **GPU 侧余量 ≈3.1 ms/帧，CPU 侧无余量**。未收口：`drawFrame` 内 ~93% CPU 时间无子 zone（提帧率下一步 = 补子 zone，纯插桩，可选批 9.5）、present_timing 跨机（NVIDIA / Intel anv）复测未做。全文 → `docs/phase9_plan.md` §10；present_timing 本机形态限制（只 advertise stage-local 域）→ `docs/调试历史.md` 2026-09-27 条目。
- `computeCommandPool`/`transferCommandPool`/`computeQueue`/`transferQueue` 已创建但保留给未来使用
- **VMA + zero-init 冲突**：VMA 对 BDA buffer 自动加内部 DEVICE_ADDRESS flags，与用户 ZERO_INITIALIZE pNext 必撞——该扩展已回退，真实需求走原生 `vkAllocateMemory` 绕 VMA（`docs/vulkan_extensions_2025_调研.md` §十二/§13.1）
- **Phase 5 光照/透明近似**：世界法线 mat3 直乘（假定均匀缩放）；透明排序 per-primitive 距离²（非 OIT）且开背面剔除（非封闭几何会剔掉一面）。留 Phase 7+
- **Phase 5.5 IBL/天空盒近似**：BRDF LUT 仍无（prefiltered 已 Phase 8 ④b GGX 预滤波落地，`env_prefilter.cppm`）；mip blit NEAREST（sRGB LINEAR 违 VUID-00239）；单环境贴图；大三角 z=1 依赖 depth clear + LEQUAL；**light/env 两个 intensity 语义不对称**（light CPU 预乘、env shader 端乘，亮度跨路线不可直接对比）。简化 IBL forward/deferred 逐式相同；skybox 背景在 deferred light pass 内合成（非独立 pass）
- **Phase 6 渲染图边界**：线性 pass 序（无拓扑排序/fusion/aliasing）；两个图字符串字段的
  词表单一真相源 = **`pass_vocabulary`**（`kAllPassStates` 按 scope 分档：drawMesh 认
  meshPass 档 6 名、drawLight 认 fullscreenPass 档 3 名 = `kFullscreenPassStates`，
  `izb_mask` 属 internal 档不走图；段名 = `kAllDrawSources`，shadow 是前缀档）——
  **两档不可并成扁平表**（并集会让 kind 与 state 串味的图放行到执行器深处）；
  单 swapchain attachment；多线程录制不做
- **Phase 11 批 11.4 SSAO 三条固有边界**：① **depth-only → 法线贴图细节看不见**（鹅卵石缝
  是法线贴图，深度图上是一整片平面 → 开阔地面 AO 恒 1.000，不是 bug）；② 屏幕空间螺旋采样
  无投影 range check（scene data 头只有 `invViewProj`）→ 前景轮廓外一圈 AO 光晕；
  ③ 施加点在 tonemap 合成处，**连直接光一起压**（forward 无 G-buffer 的唯一对等位置）。
  AO pass 常驻图内、`ao.strength=0` 逐位旁路（`lerp(1,x,0)≡1.0`），故不产生额外基线成本
- **湿面只加高光不压暗**（批 11.0 补修）：`_Wet` 材质 `roughnessFactor=0.163` → effective 0.12
  （水膜）。实测 roughness 0.12 是**平台期**（无 SSR，只能反射 IBL 高 mip 的均匀天空辉光），
  且真实湿面应同时压暗漫反射而这里没做 → 湿区略"亮而奶"。补它需 per-material wetness 分量
  进 scene data（draw data schema 扩列 = 反射管线改动），列 Phase 12 候选
- **灯契约（现行）**：`Light` 组件 + `index` = 列表位正序（画不画 = 实体有无；forward 读 index 0 = 主光，延迟按 index 落槽）；`SceneLight`（前向头）/`GpuLight`（灯数组）布局均走 C API 反射生成，无手写双源
- **GPU culling（Phase 7b）现状（现行）**：默认 = compute culling + **读回重放**（三驱动均跑 culling compute，无开关）；DGC execute 仅 NVIDIA graphicsIndirect=true 可走，RADV 与 Intel anv 实测均 false（机器归属见 nvidia_compatibility_report）→ 重放；RADV 静态 shader 模式 spike 功能通但性能亏 1.85× → `diagnostics.dgc_static` 门默认关。
  CPU 剔除保留为兜底 + 每帧对账基准（reconcile 不一致告警不中断）；mvp 由 CPU 算进 scene data per-object 表（GPU float4x4 FMA 收缩序与 glm 无位级契约，保 forward diff0）。
  驱动级细节/历史 → `docs/archive/radv_static_dgc_spike.md` / `docs/nvidia_compatibility_report.md` / 调试历史 2026-09-10

## 其他文档

> 每篇文档头部带状态（`现行`/`历史`/`调研`）。现状权威 = 本文件；下一步路由 → 各 phase 计划文件（当前 `docs/phase13_plan.md`；测试体系账本仍看 `docs/phase12_plan.md`）。

- `docs/next_roadmap.md` — **路线图（历史）**：Phase 0–7 结论存档（Phase 8+ 执行记录见各 `phase*_plan.md`；下一步路由 → 当前 `docs/phase13_plan.md`）
- `docs/技术概念.md` — 核心 Vulkan 模式、设计决策、§构建（Windows 前置与 vcpkg）/ §模块表 / §构建检查工具机制 / §核心运行时模块表（深版）/ §RPC 调试通道（Phase 10b，端点速查）/ §main 启动流（完整版）
- `docs/调试历史.md` — 问题修复时间线（只增不改，争议结论以此为准）
- `docs/phase8_plan.md` — Phase 8 Bistro 打磨（HDR / HDR env / 纹理接缝 / 灯光）实施计划与验收
- `docs/phase9_plan.md` — Phase 9 性能观测（Profiler）批次记录；§10 = 性能判定全文（CPU-bound 结论）、§11 = 跨机复测遗留
- `docs/phase11_plan.md` — Phase 11 阴影 + 前置修复批（11.0 光照质感 / 11.2 阴影 / 11.4 SSAO）计划与执行记录
- `docs/phase12_plan.md` — Phase 12 测试体系账本（批次史 + 缺口 D1–D9 + 12.3d 调试历史全量裁决执行记录）
- `docs/phase13_plan.md` — **Phase 13 TAA 收口（现行，下一步从这里读）**：为什么还要一个 phase（T2/T3 验收被"历史每帧销毁"bug **空洞满足**、判据全跑在合成靶 + env 关闭、默认关无性能账）/ 批次 13.0–13.4 + 每条验收判据 / 记债继承表（AO 在 TAA 后、无 velocity buffer、YCoCg 盒）
- `docs/taa_plan.md` — **TAA 时域抗锯齿（T0–T4 判据收口 2026-10-03；收口与画质 → `phase13_plan.md`）**：两条硬约束（§0，全走 descriptor heap + 跨帧分析三件事）/ Filament 作业表 / 四个 gap 逐条设计（跨帧历史附件是核心闸门）/ jitter 注入点决策 / §7 = 机制与踩坑账（第 11/12 条 = 根因）
- `docs/gate_lint_plan.md` — **门配置自检（现行机制的权威载体）**：38 门申报规则表 + 回显派生 + 覆盖率护栏；加诊断门/改门语义前看（§4 分级判据 + §5 三条防腐条款 + §8 执行偏差）
- `docs/hfts_izb_plan.md` — HFTS/FTIZB 调研与实施计划（批 11.3 权威载体：论文结论 / 开源现状 / 项目 gap / 分阶段 1a–1e + 收手判据；§4.3 = 2026-10-02 路线改道记录）
- `docs/evssm_plan.md` — **EVSSM 软阴影（批 11.6，现行）**：Filament EVSSM 作业清单 / 分轴质量对比 / 三只拦路虎 / spike 批次拆分（远场软影权威路线，HFTS 已降级 backlog）
- `docs/DESCRIPTOR_HEAP_SPEC.md` — descriptor heap 排坑记录（活文档）
- `docs/nvidia_compatibility_report.md` — 跨驱动兼容性（三平台记录；**不写"本机"是哪台**——三机轮换，开工前看启动日志 deviceName 确认）
- `docs/device_generated_commands_research.md` — GPU culling 双驱动实测（Phase 7b 依据）
- `docs/device_fault_调研.md` — VK_(EXT|KHR)_device_fault 的 device lost 诊断调研（**未实施**）
- `docs/archive/radv_static_dgc_spike.md` — RADV 静态 shader DGC spike（功能通、慢 1.85× → 默认不开；翻盘设计底稿）
- `docs/vulkan_extensions_2025_调研.md` — 新扩展调研 + §十二/十三 实施记录（活文档）
- `docs/phase7_devlog.md` — Phase 7 滚动开发日志（完结后归档）
- `docs/archive/phase10_plan.md` — Phase 10 调试交互计划（**2026-09-28 批 10.1–10.7 全落地归档**；imgui 面板/热重载/点选/**点选高亮+选中跳页**；决策表 D1–D12 含 D12 RPC 延后；遗留 UI 视觉走查）
- `docs/phase10b_plan.md` — **Phase 10b 增补批（现行，2026-09-28 全落地）**：11b.1 imgui 中文字体（系统字体探测链，MiSans Light 首选）+ 11b.2 loopback HTTP POST RPC（D12 翻案）；协议表/依赖方向/验收清单（已全绿，除 Windows 非门项）
- `docs/shader_object_组合成本与优化预留.md` — 管线表 312 条目（bistro 基准）生成机制 + 成本账本 + 优化预留（L0 SPV 缓存 / L1 磁盘二进制缓存 / 路线级 push-index）；**优化未实施**，触发条件见其 §5.1
- `docs/风格检查规则.md` — tools/checks 两轮扩展设计（partition/docs/现代 C++/expected 禁 throw）；工具 Python ≥3.14，规则拆 `rules_{metrics,text,doxygen}.py`
- `docs/脚本长期规划.md` — 脚本长期线（独立于 Phase 编号；P0 spike 已通，`script_spike/`）
- `docs/快捷键配置.md` — Zed 编辑器快捷键/任务/调试配置说明（与代码无关）
- `docs/archive/静态审查升级路线.md` — LLVM 23 + `-Wlifetime-safety` 两阶段落地计划（vs Rust 借用检查差距结论；clangbind ABI 回归是阶段 1 最高风险）
- `docs/archive/阴影失效排查.md` — Phase 11 批 11.2 阴影失效排查交接日志（**已解决归档**，权威结论 = 调试历史 2026-09-29 条目）
- `docs/archive/` — 归档索引（phase 计划含 **phase6_record.md**、ecs 审查、issue_history 总账、present_timing/教程拆解等已结题调研）
- 离线 Vulkan 文档（优先在线检索被 Cloudflare 拦截时）：本地克隆 `/home/robin/program/Vulkan-Docs`（v1.4.360）。**注意**：spec 比系统头/loader（v357）新，结构体字段以系统头为准，spec 只作语义依据
- 最小复现/验证子项目（独立 CMake）：`script_spike/`（脚本长期线 P0，见 `docs/脚本长期规划.md`）；`min_mesh_crash/` 与 `heap_texture_spike/` 已删除（`516cf5a`，墓碑 = `docs/archive/*_已删除.md`；根因/三驱动结论在 `docs/archive/issue_history.md` / `heap_texture_research.md`，代码见 git history）
