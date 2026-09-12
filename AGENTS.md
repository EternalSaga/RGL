# RGL

Vulkan 现代特性学习项目。Mesh shader 渲染管线，使用 ShaderObject + DescriptorHeap + DynamicState。
核心代码位于 rgl 目录下。
legacy（旧 OpenGL 项目）已于 2026-09-08 删除，历史思路参考见 git history（`legacy/` @ < 2026-09-08）。

## 给大模型
优先使用中文回复用户，除非用户说使用英文回复。
随时思考是否可以拆分子任务给子 agent 执行。主 agent 校验结果。困难任务主 agent 亲自执行。
**文档/代码调研可委派 `researcher` 子 agent**（`.opencode/agents/researcher.md`，robin-nas/MiniCPM5:2B，只读）：适合代码定位（`文件:行号`）、文档定位、上下文简易归纳，行号命中率高、诚实不编造。**主 agent 仍需校验**：它在主动补充额外结论时可能把过期文档/注释当成生成物事实（实测踩过 sizeof 生成物 4B 被它说成 100B）。纯"grep/read 到什么报什么"时可靠，涉及产物布局/大小须自己复核生成物本体。

## 构建

项目使用 `just` 管理构建流程（参见 `Justfile`），支持 Linux 和 Windows 双平台。

### Linux

```bash
just config              # CMake 配置（默认 clang, Release）；just config gcc Debug 指定编译器/类型
just build               # 增量编译（需先 config）；just build-release / build-debug = 一键配置+编译
just reflect             # 重跑 5 条反射管线（改动 shader 契约/schema/模板后必须；见"着色器编译"）
just doc-check [--strict]  # Doxygen 注释检查（每次 just build 已随 doc_check 并行跑、只警告；此处单独跑+透传参数）
just style-check [--strict]  # 文件/函数结构检查（文件>500/函数>120 代码行、认知复杂度>15，libclang 多进程，见 tools/stylecheck）
just run                 # 运行（AMD VGA 存在时自动设 RADV_EXPERIMENTAL=heap）
just run-igpu / run-dgpu / run-intel   # 强制核显 / NVIDIA prime 独显 / Intel anv
just clean / clean-all / all           # 增量清理 / 删 build 目录 / clean-all+build-release
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

- `doc_check`（ALL，与编译并行，2026-09-09）：复用 `build/compile_commands.json` 对 `rgl/*.cppm`
  做 clang `-ast-dump`（构建链同一 LLVM，纯标准库 Python，零 pip 依赖），检查类/枚举/函数/方法
  声明正上方有非空 `/** */` 块。实现在 `tools/doccheck/`（doccheck.py + vendored astparse.py，
  源自姊妹教学项目 modern_vulkan_tutorial 的 AST 管线）。AST 缓存 `build/doccheck_cache/`
  （键 = 源+命令 sha256）：增量只重 dump 改动文件（秒级），全量 ~1min。规则细节：注释真相源 =
  接口声明处（impl 里接口已声明函数的定义不重复要求）；重载组共用最上方一个块；前向声明与
  定义间可夹空行（Renderer 先例）；只警告不阻断（`--strict` 才 exit 1），默认构建含 `--methods`。
- `style_check`（ALL，与编译并行，2026-09-11，2026-09-12 重构为多模块小包 + 规则升级）：
  `rgl/*.cppm` 八类规则——前五项 libclang 解析
  （代码行 = 去注释空行，token 级精确）：①文件 >500 提示拆文件、②函数 >120 提示拆函数、
  ③认知复杂度 >15（SonarSource 风格：if/for/while/do/catch/三元 结构 +1+嵌套，
  switch 与 else/else-if +1 不递增嵌套，一段连续 &&/|| 整段 +1；**lambda 独立计分**
  ——宿主不叠加 lambda 体、lambda 各自成条目从 nesting 0 算，2026-09-12 修正；替换旧
  "嵌套深度>5"，缓存版本随之 v2→v3→v4→v5，v5 修正 `TOK_COMMENT` 枚举值 5→4——注释
  token 此前未过滤、每条注释首行误计入代码行致行数类规则大面积虚高）提示拆函数、
  ④接口瘦身（非 _impl 文件函数体 >100 → 迁入 xxx_impl.cppm）、⑤成员命名（_ / m_ 前缀、
  _ 后缀 → 自然命名）；后三项纯文本：⑥分层/模块优先（底层零 entt = 不 import ecs 且
  GMF 不 include <entt>，名单 `ZERO_ENTT_MODULES`；点名禁止 import 边 `FORBIDDEN_EDGES`；
  rgl/ 禁 .h/.hpp/.hh、.cpp 仅 main.cpp + 三个单头库实现单元、禁内部头 #include "）、
  ⑦GMF STL 白名单（#include <STL> 只许 ranges/filesystem/chrono，余走 import rlstd）、
  ⑧禁用旧式 Vulkan API（剥注释后按行匹配 `FORBIDDEN_VULKAN_APIS`：经典 descriptor pool /
  pipeline object / render pass / push constants——项目已全面 descriptor heap / shader
  object / dynamic rendering，回归护栏；标识符边界匹配防 bindPipelineToAsset 误报）。
  实现在 `tools/stylecheck/` 包（stylecheck.py CLI 入口 + config/clangbind/cmdclean/
  metrics/worker/rules 各司其职；libclang ctypes 进程内解析，纯标准库 Python；
  **ctypes 持 GIL → ProcessPoolExecutor 一进程一文件**，默认 min(cpu,4) worker、每进程
  ~550MB）。缓存 `build/stylecheck_cache/`（键 = 源+命令 sha256 + 缓存版本）：增量秒级，
  全量 69 文件 ~12s。**产物预检**：@modmap/BMI 是构建产物，clean 后首次构建（style_check 与
  编译并行时尚未生成）未就绪文件整体跳过 libclang 规则、只跑纯文本规则（汇总"产物未就绪跳过 N"
  = 正常非故障，编译完成重跑即全量）。警告文案直接给 LLM 行动指令。坑（详见文件头，勿回退）：C API 不展开
  @.modmap（须内联展开 + BMI 路径转绝对）、输入文件/驱动路径不能进 argv、Gentoo 须显式
  `-resource-dir`、`export module`+import+**BMI 加载失败** → parse 返回 NULL（libclang 缺陷，
  按跳过+警告处理）、LLVM 22 结构体布局（CXCursor 32B / Location/Range 24B / CXToken 24B
  值类型）
  与 token API 全异于旧例程。

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
  尺寸 `headless_size={W,H}`（默认 1280×720）；llm_debug 截图 = drawFrame 帧内回读 swapchain 后缓冲
  （present 前拷贝，真实上屏内容；headless+llm 时 main 契约令 headless_size 跟随 llm_shot.size）。
  坑：v357 结构体无 width/height/format 字段、须显式 `VK_KHR_surface`、无 `eImmediate`（用 `eFifo`）
- **agent 冒烟**：改 `build/rgl/configs/runtime.lua` 为 `headless = true`（build 副本临时改，
  下次 `just build` 按源内容比对**自动复原**）→ `cd build/rgl && RADV_EXPERIMENTAL=heap timeout 8 ./rgl`
  （正常应零 `[error]`；deferred 另改 build 副本 render_graph.lua `variant="deferred"`）
- **llm_debug 截图流程**（agent 多模态读图）：build 副本 `configs/runtime.lua` 改 `headless=true` +
  `llm_debug=true` → 直接跑二进制 → 延迟 N 帧存 PNG（路径打日志）并退出 → `just build` 复原配置。
  相机位置 = camera.lua 顶层 position/target/fovDegrees；截图尺寸 = swapchain extent（headless 跟随
  llm_shot.size，见 headless 模式条）；诊断直读 G-buffer 通道 = diagnostics.lua `dump_gbuf=0..3`
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
vkresources / shader_registry / render_graph / render_graph_config / gltf_loader
（函数语义注释留接口声明处）。规则与坑：
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
`validator`（reflection JSON ↔ schema 一致性校验，JSON 路径遗留）、`generator`（inja 模板生成
`.cppm`，`renderTemplate()` 支持任意 JSON + 模板渲染；`generateCppmFromSlang` 走 C API 路径）、
`slang_reflect`（Slang C API 封装：链接预构建包 `libslang.so`，`SlangContext` 查非资源 struct
实际布局，见 `docs/slang_api_reflection_research.md`）。`reflection_tool` 经 `import rllogger;`
统一日志，`slang` 子命令 = C API 路径（4 条 slang 管线主用），`validate/generate` = JSON 路径（遗留）。
tools 为 `EXCLUDE_FROM_ALL`（编译提速）：日常构建不碰工具链，`just reflect` 才编译。

## 模块结构（C++20 modules, .cppm）

所有导出均在 `namespace RGL` 内。**实现细节以各 `.cppm` 文件头注释为权威**，Phase 演进史见
`docs/next_roadmap.md`。**工具/数据/生成模块 32 行速览表已下沉 `docs/技术概念.md` §模块表**，
此处只留初始化层（构造顺序 = main.cpp 顺序，强导航价值）：

| 模块 | 文件 | 职责 |
|---|---|---|
| `vk_instance` | vkinstance.cppm | Context、Instance、DebugMessenger、apiVersion（headless 换 `VK_EXT_headless_surface`） |
| `vk_device` | vkdevice.cppm | PhysicalDevice 选择、QueueFamilyManager、Device、CommandPools、FeatureChain |
| `vk_swapchain` | vkswapchain.cppm | Surface、Swapchain、ImageView[] format/extent/presentMode |
| `vk_resources` | vkresources.cppm | VmaAllocatorRAII、depth、VmaDescriptorHeap（固定裕量 `kUserSlotMargin(1024)`）、`allocSlot` 统一分配、`ensureSharedSampler/ensureSkySampler/ensureSceneData`（scene data 头 128B：@0 光参数/@48 env/@64 skyInvViewProj/@128 对象矩阵表 ×4096，尾区 @262272 灯数组 / @262800 per-object MVP 表 / @524944 per-object 材质表——Phase 7b+ DrawArgs bindless 化，见 scene.slang）、env cubemap 单例、G-buffer RT 缓存（`ensureRtAttachment`/`rtHeapSlot`/`ensureDeferredGBufferSlots`） |
| `vk_renderer` | vkrenderer.cppm | **盲执行器骨架**（Phase 10 批次 0 拆四子系统，见下四行；手术 6 续拆 pass 循环节点，BIT-IDENTICAL 全程维持）：`drawFrame(const FrameDrawList&)`（零 entt）构造收 `CompiledRenderGraph`，流程 = `session.beginFrame`（①②③）→ `writeSceneStaging`（③.5）→ `cullDriver.runFrame`（④c）→ `recordFixedDataPath`（⑥⑥.5⑦ heap barrier/copy/bind，私有方法）→ pass 循环（`ensureOffscreenAttachments` 帧首 ensure / `SlotResolver`（文件内）解析 slot→image/view + 物化 barrier / `beginPassRendering`（文件内）/ `stateKit.stateFor`+applyDynamicState / `executePassDraws` 三路分发（drawLight/GPU-cull opaque/逐 draw））→ `capturer.record`（⑪.5）→ presentTransition（⑫）→ `session.submit/present/advance`（⑬⑭⑮）；公开 `requestFrameCapture/takeFrameCapture/hasFrameCapture`（转发 capturer）+ `waitForIdle` + `getGraphicsCommandPoolRef`（金丝雀）；构造期 `validateRenderGraphForExecutor` fail-fast |
| `frame_session` | frame_session.cppm | 帧生命周期会话（手术 4 自 drawFrame 摘出）：四套同步对象（双 fence，VUID-00067）+ 每帧命令缓冲 + `beginFrame`（①双 fence wait/reset + ②acquire 100ms 超时三态 began/skipped/recreated + ③CB begin）/`submit`（⑬裸 vkQueueSubmit）/`present`（⑭presentFence + out-of-date 重建）/`advance`（⑮）；`recreateSwapchainAndDepth`（swapchain+depth extent 同步重建）；`kFramesInFlight=2`；noPresent 诊断分支保留 |
| `frame_capture` | frame_capture.cppm | 帧内回读捕获器（手术 2 自 Renderer 摘出，llm_debug 截图载体）：`FrameCapture` 产物（width/height/format/gbufChannel/bytes/pixels）+ 三步曲 `request`（⑫前挂）/`record`（后缓冲 or GB 通道回读拷贝，读写 attachmentLayouts 布局追踪）/`collect`（⑬后等本帧 fence 搬 mapped 字节）；captureStaging GPU_TO_CPU 按需增长；acquire 跳帧时请求保持 pending |
| `cull_driver` | cull_driver.cppm | GPU culling 帧编舞（手术 3 自 drawFrame ④c + opaque 段摘出，持 GpuCull*+DgcCommands*+DgcStaticCommands*）：`runFrame`（cull CB 录制 dispatch+DGC preprocess → 提交 → 同帧同步等待 → readCull → `reconcile` objectSlot 多重集 + cullArgs↔entries 对齐自检）+ `executeOpaqueSegment`（IES DGC execute / 静态 DGC / 读回重放**三路分发**，能力位 + 门控切换；默认 AMD = 重放）；inactive = 全路径不碰 culling |
| `state_kit` | state_kit.cppm | pass 动态状态表（手术 1 自 Renderer 摘出）：四段 RenderState（opaque/transparent/skybox/light，值语义含透明段剔背面防 alpha 翻倍、天空盒 z=1 LEQUAL、light 全屏终图不测深）+ `stateFor(name)`（词表与 validateRenderGraphForExecutor 同步）+ edS3 depthClip 能力门；Phase 10 阴影新增状态集中改此一处 |
| `render_graph` | render_graph.cppm | 渲染意图图（纯 C++ 零 Lua 零 entt）：`RgGraphDesc` → `compileRenderGraph`（校验 + barrier 自动推导 + presentTransition）→ `CompiledRenderGraph`；default/offscreen/deferred 三图变体 + `kGBufferAttachments` 契约表（G-buffer 名单一真相源）+ `renderGraphSelfCheck` |
| `render_graph_config` | render_graph_config.cppm | `configs/render_graph.lua` → `RgGraphDesc`（sol2 沙箱，缺文件回落默认；结构合法性归 compile） |
| `shader_registry` | shader_registry.cppm | 管线表（零 entt）：`PipelineEntry` + `ShaderRegistry`（条目全按需建，无构造期恒建：`addModelEntry`（按组合去重）+ `addCubeEntry`（内建几何，按资产 slot 烘焙）+ `addSkyEntry` + `addLightEntry` + `bindPipelineToAsset`） |
| `gpu_cull` | gpu_cull.cppm | GPU culling 数据通路：CullEntry 表（60B）+ 序列 buffer（120B：execSlotFrag/Mesh + DrawArgs@8(4B) + meshCmd@108）+ count buffer（64B：visible/total）+ seqStaging ring×2（MAPPED，CPU 预写 args = 仅 objectIndex）；`writeSequenceStaging`（CPU args）+ `recordCullPass`（staging 拷贝 + dispatch）+ `readCullResult`；GPU 只覆盖可见性（meshCmd.x），mvp 走 scene data per-object 表（Phase 7b+，CPU 每帧写位级一致，见 scene.slang readObjectMvp） |
| `vk_indirect_commands` | vk_indirect_commands.cppm | DGC RAII（能力门控，两类）：**DgcCommands**（NVIDIA IES 路）= indirect commands layout（3 token：EXECUTION_SET@0 每 stage-bit 一位 / PUSH_DATA@8 DrawArgs(4B) / DRAW_MESH_TASKS@108，stride 120B）+ shader-object execution set（每条目 2 slot：2j=frag/2j+1=mesh，初始态=首条目 frag+mesh 建后 update 全槽）+ preprocess buffer（`vkGetGeneratedCommandsMemoryRequirementsEXT` + 原生 `vkAllocateMemory`，按帧槽 ×framesInFlight）；**DgcStaticCommands**（RADV 静态 shader 路，spike 门控 `dgc_static` 默认关——功能通/性能亏，见 `docs/radv_static_dgc_spike.md`）= 2 token layout（去 EXECUTION_SET）+ pNext 静态 `VkGeneratedCommandsShaderInfoEXT` 逐条目 Execute + preprocess 槽 N×framesInFlight（execute 前四阶段 bindShadersEXT，mesh 管线 vertex/task 须显式 null 绑——VVL 独有）；**7 入口点全走 `vkGetDeviceProcAddr`**（系统 loader 不导出 DGC 符号，v357 无 `vk::` wrapper；隐式 preprocess 不含 preprocess 入口点）；`bindInitialShaders`/`recordExecute` |

**main.cpp 启动流**：加载 camera/scene/runtime/diagnostics 配置（先于建窗）+ `renderGraphSelfCheck`
→ 建窗（headless 不建）→ 栈上 `entt::registry`（全部每帧 context）→ 按序构造初始化层
AppContext→DeviceContext（push constant 契约检查）→Swapchain→Resources→ShaderRegistry →
`loadRenderGraphConfig` + `compileRenderGraph`（**建窗后**）→ **entities 四段循环**（A0 内建
几何上传（cube，若有 cube entity）→ A1 解析+顶点上传全部模型 → A2 纹理 → **env 上传**（时序
= A2 后、addModelEntry 前）→ C 逐模型 addModelEntry+registerScene + 容量契约检查 + cube
entity/条目 + **C2b 灯 entity**（`sceneConfig.lights` → 每灯一 entity + `Light` 组件，`index`=
列表位正序契约，画不画 = 实体有无）；A0/A1/A2 拆分 = heap 布局"几何槽先于纹理槽"契约，见
`AssetTable::expectedGeometrySlot`）→ **C3 GPU culling 候选表**（`view<MeshRef, MaterialRef,
CullSphere>` 迭代序+守卫与 render_list CPU 路径完全一致；candidate = 不透明 primitive）+
`GpuCull` 构造 → `addSkyEntry`/deferred 装配（G-buffer 槽 + light 条目，**须先于 Renderer 构造**）
→ `DgcCommands` 条件构造（graphicsIndirect && cull active，异常回落 null）→
`DgcStaticCommands` 条件构造（非 graphicsIndirect && staticGraphicsIndirect &&
`diagnostics.dgcStatic`，spike 门默认关；流数 N 与 `GpuCull` 构造期同取 registry 条目数）→
`Renderer`（收编译图 + &gpuCull + dgc + dgcStatic）→ `renderSystem`。

### 构造依赖图

```
SDLWindow → AppContext → DeviceContext → Swapchain → Resources → Renderer
                └────────────── entt::registry（WindowExtent/ViewProjection 等每帧 context）──────────┘
```

main.cpp 按逆序析构（RAII）。依赖图原则：只允许下层 import 上层，**配置模块不依赖执行模块**
（camera_config 不 import llm_debug）、**ecs 不依赖 llm_debug**（截图经回调）——假边会让
慢模块（llm_debug 拖 vk_renderer 全链）进入本不该等它的编译前置。

### 重要成员生命周期规则

- `vkinstance.cppm`：`engineInfo`、`instanceCreateInfo`、`layers`、`instanceExtensions` 均为类成员 — `pApplicationInfo` 不能悬空
- `vkdevice.cppm`：`featuresChain` 为类成员 — `DeviceCreateInfo.pNext` 不能悬空
- 所有 `vk::raii::*` 对象必须通过**成员初始化列表**初始化，绝不在函数体内初始化（默认构造函数已删除）
- 成员声明顺序必须与初始化依赖顺序一致
- **GPU 资源所有权**：长生命周期 GPU 资源（纹理 BO/描述符存储）不得声明在 main() 局部块内
  ——块尾析构 = 第 0 帧起 GPU 采样已释放 VA（UAF 非确定性故障，见 `docs/archive/issue_history.md`）

## 着色器编译

`rgl/shaders/` 下的 Slang → SPV 输出到 `build/rgl/generated_shaders/`
（CMake `compile_slang()` 构建时编译；运行时相对路径读取）。

- **反射管线显式触发（C API 路径，2026-09-10 起）**：`generated/*.cppm` +
  `reflection_manifest.txt` 为**已提交产物**，只在改契约后 `just reflect` 重生成并随改动提交；
  构建开头 `reflection_check` 比对输入 SHA1，忘跑 = FATAL 指路。共 **5 条管线**（types/model/
  material/lights 反射生成 + features 模板生成），Profile `sm_6_6+spirv_1_6` + capability
  `spvDescriptorHeapEXT`。**4 条 slang 管线走 `reflection_tool slang` 子命令**（链接 libslang.so
  直接查 C API，**不产 JSON 中间层**）：struct 布局 = 编译器实查真值（逐字段 offsetof
  static_assert），schema 降为提取清单 + 交叉校验期望值。详见 `docs/slang_api_reflection_research.md`
- **Scalar Block Layout（定案）**：所有 slangc 编译（反射 + SPV 两处 flag **必须一致**）统一
  `-fvk-use-scalar-layout`——C++（glm 自然对齐）与 shader 逐字段偏移天然一致零 padding
  （`DrawArgs` 4B）。前提 `scalarBlockLayout` feature（features_config.json + validateFeatures）
- **bindless 原则**：push constant 冻结在"纯索引"级别（Phase 7b+ 起 `DrawArgs` 仅 `objectIndex`
  4B，不再扩）——场景级/每帧变化的数据（光照、对象矩阵、MVP、材质）一律进 descriptor heap（scene data）
  每帧变化的数据（光照、对象矩阵、env 参数、灯数组）一律进 descriptor heap（scene data）

## 隐式扩展依赖

`features.cppm` 请求的 `VK_EXT_DESCRIPTOR_BUFFER`、`VK_KHR_SHADER_UNTYPED_POINTERS` 等
（`kDeviceExtensions()` 9 项，features.cppm L27-36）除 `VK_EXT_DEVICE_GENERATED_COMMANDS`
（Phase 7b 批次 5 起**显式使用**：`gpu_cull`/`vk_indirect_commands`，GPU culling 默认路径）外
均无显式使用，但被其他已启用扩展隐式依赖，不可随意删除；`vkinstance.cppm` 的
`VK_EXT_SWAPCHAIN_COLOR_SPACE` 同理。**KHR/EXT 别名**：`VkSwapchainPresentFenceInfoKHR/EXT` 是同一结构（typedef alias），代码用 EXT。

## 外部依赖（通过父 CMake 链接）

`rlstd`（根目录 STL 模块库）、`rllogger`（根目录日志库，`RLLogger::getInstance()`）、spdlog（经
rllogger）、glm（depth zero-to-one + 弧度制）、SDL3、VulkanMemoryAllocator、
slangc（`cmake/SlangFetch.cmake` FetchContent pin v2026.16 + sha256；Windows 优先 Vulkan SDK 自带）

## 关键文件位置（契约与接缝）

| 位置 | 说明 |
|---|---|
| `shaders/material.slang` | **统一 push constant 契约**：`DrawArgs`（Phase 7b+ 仅 `objectIndex` 4B）单一真相源 → schema 校验 → `material_types_generated` |
| `shaders/scene.slang` | **场景级 bindless 契约**：`sceneData`（binding 3）+ `SceneLight`（@0 前向头，**C API 反射生成** `scene_forward_types_generated`，双源已消）+ `readSceneLight`/`readObjectMatrix`/`readObjectMvp`/`readObjectDraw`/`worldPos`/`worldNormal`/`clipPos`；@0-128 头 + 各 per-object 表字节布局 = shader 字面量（per-object 表仍手工一致，见已知问题） |
| `shaders/env.slang` / `sky.slang` | env 采样契约（binding 4/5 + @48）/ 天空盒契约（@64 invViewProj 重建射线） |
| `shaders/lights.slang` | **多灯契约**（反射管线 4）：`GpuLight` + `readLightCount/readGpuLight` 读尾区 @262272；`light.frag` import，forward 不 import |
| `shaders/model_pbr/model_phong/model_pbr_ibl.frag` | 三条 frag 路线（GGX / Blinn / GGX+简化 IBL），`scene.lua styles` 一行切换 |
| `shaders/deferred_geometry.frag` + `light.mesh/frag` | deferred 路线：4-MRT G-buffer 写入（oct 法线）/ 全屏大三角逐灯 GGX + **简化 IBL**（env 采样替换常 ambient，与 model_pbr_ibl 逐式相同）+ **skybox 背景合成**（天空像素 = position 清 0，重建射线采 env）；env 走 light pass binding 9/10（4/5 被 G-buffer 占）；`kGBufferAttachments` 表定格式 |
| `shaders/model_schema.lua` `constants` | 契约常量单一真相源（`kTrianglesPerGroup` 等 → 生成 constexpr）；`HeapSlotGeometryBase` = 用户槽区起点 = 几何段基址 |
| `rgl/builtin_geometry.cppm` | **内建几何契约**（架构债务 1 方案 B）：cube 等程序化几何的上传/注册，与 gltf 同路；`vk_resources` 不认识任何内容 |
| `shaders/cull.comp.slang` + `rgl/gpu_cull.cppm` | **GPU culling 契约**（Phase 7b）：CullEntry 60B / 序列 120B（execSlot 2j/2j+1 + DrawArgs@8(4B) + meshCmd@108）/ CullPush 100B（planes[6]+sequenceCount）/ count 64B（visible@0 total@4）；CPU 算 mvp 进 scene data per-object 表、GPU 只写可见性（保 forward diff0）；DGC 序列契约见 `vk_indirect_commands.cppm` + `docs/device_generated_commands_research.md` §4.4。静态 DGC 路追加 `shaders/stream_fanout.comp.slang`（per-entry 满槽孔洞流扇出，`dgc_static` 门控，见 `docs/radv_static_dgc_spike.md`） |
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
  语义不对称**（light CPU 预乘、env shader 端乘，亮度跨路线不可直接对比）。**简化 IBL 已扩展
  到 deferred light pass（批次 2 refine，2026-09-10）**：forward（model_pbr_ibl）与 deferred
  （light.frag）逐式相同，mipRange=0 回落常 ambient；skybox 背景亦在 deferred light pass 内
  合成（天空像素采 env），非独立 pass
- **Phase 6 渲染图边界**：线性 pass 序（无拓扑排序/fusion/aliasing）；offscreen 显式格式已落地
  （词表 12 项）；sampleRead RT→heap 接线已落地（D1）；执行器 state/drawSource 名字表认
  opaque/skybox/transparent/light 四个；单 swapchain attachment；多线程录制不做（7b+ 议）
 - **前向头 `SceneLight` 已入反射（2026-09-10，双源消除）**：forward 头 `@0-48` 原手写
   `LightDataGpu` ↔ `scene.slang` 手工一致 → 现经 C API 反射管线（第 6 条 `scene_forward`）生成
   `SceneLight`（`scene_forward_types_generated`，逐字段 offsetof 断言），替换 4 处手写引用
   （vkresources/frame_draw_list/render_list_impl/vkrenderer_impl）。灯数组 @262272 走反射管线 4
   （`GpuLight`）。**灯已 entity 化**：`Light` 组件 + `index` 正序契约（画不画 = 实体有无，forward
   读 index 0 = 主光，延迟按 index 落槽，见 main C2b + render_list）
- **GPU culling（Phase 7b）边界**：DGC 路径（`vkCmdExecuteGeneratedCommandsEXT`）本机 RADV
  graphics 侧不可用（`supportedIndirectCommandsShaderStagesShaderBinding` 仅 COMPUTE，26.2.1 复测
  graphicsIndirect=false）→ 默认走**读回重放**。**注意：本机核显 = 9950X Raphael（RDNA2 小核），
  非 Strix Halo**——graphicsIndirect=false 疑为驱动按硬件代际 gate，待真 Strix Halo 复测（若 true
  即走与 NV 同一条 DGC execute 路径；启动期选路 log `DGC path: device=... -> ...` 直接可验）。
  **2026-09-10 静态模式 spike**：Strix Halo（8060S）复测 graphicsIndirect 仍 false（代际 gate
  假说不成立，binding 子集两代同硬编码）；但 RADV graphics DGC 执行路径实为已实现，**静态
  shader 模式**（2 token layout + pNext ShaderInfo，绕开 binding 子集）功能验证通过
  （VVL 零 error + 1px tie 级回归，`docs/radv_static_dgc_spike.md`）——满槽实现 preprocess
  ×N 慢重放 1.85× → **默认不开**，`diagnostics.dgc_static` 门控保留（详见 调试历史 2026-09-10 续 3）；
  NVIDIA 侧 graphicsIndirect=true 走 DGC execute，2026-09-10 终验跑帧零 error/warning 石锤
  （曾黑屏 = NV 驱动 push 100B 挂死 bug，官方 sample 复现，规避 = DrawArgs 压 4B bindless 化；
  曾报 `vkCreateIndirectExecutionSetEXT: ErrorOutOfDeviceMemory` = 运行环境瞬时显存干扰**假警报**，
  干净环境重跑即通，非驱动限制——根因全程见 `docs/调试历史.md` 2026-09-10 条目）；Intel Arc A310
  （DG2/anv 26.2.1）graphicsIndirect=false computeIndirect=true（binding 子集 COMPUTE+RT、无
  MESH/FRAGMENT，与 RADV 同画像）→ 走读回重放，零 error、截图与 RADV 一致（至此三驱动全验证）。
  culling compute 三驱动都跑（默认路径无开关）；CPU 剔除保留为兜底+
  每帧对账基准（`reconcileGpuCull` 多重集，不一致
  告警不中断）；mvp 由 CPU 算进 scene data per-object 表（GPU float4x4 乘法 FMA 收缩序与 glm 无
  位级契约，保 forward diff0）
- `configs/` 增量拷贝只认"源文件更新"：build 端副本被破坏（mtime 更新）不会自动恢复，需手动拷贝；
  且 `copy_if_different` 按内容比对——**build 端临时改配置会被下次 build 静默覆盖**（截图调试后记得
  改的是 build 副本还是源）

## 其他文档

> 每篇文档头部带状态（`现行`/`历史`/`调研`）。现状权威 = 本文件 + `next_roadmap.md`。

- `docs/next_roadmap.md` — **路线图（权威）**：Phase 0–6 一段话结论 + Phase 7 进度表 + 剩余计划
- `docs/技术概念.md` — 核心 Vulkan 模式、设计决策、**§模块表（工具/数据/生成模块速览）**
- `docs/调试历史.md` — 问题修复时间线（只增不改，争议结论以此为准）
- `docs/DESCRIPTOR_HEAP_SPEC.md` — descriptor heap 排坑记录（活文档）
- `docs/nvidia_compatibility_report.md` — 跨驱动兼容性（三平台记录；当前机 = 9950X Raphael RDNA2 核显，
  非 Strix Halo）
- `docs/device_generated_commands_research.md` — GPU culling 双驱动实测（Phase 7b 依据）
- `docs/radv_static_dgc_spike.md` — RADV 静态 shader DGC spike（历史：功能通、满槽实现慢重放
  1.85× → 默认不开；翻盘 = 紧凑分桶 + 去同步等待，以本文为设计底稿重启）
- `docs/vulkan_extensions_2025_调研.md` — 新扩展调研 + §十二/十三 实施记录（活文档）
- `docs/phase7_devlog.md` — Phase 7 滚动开发日志（完结后归档）
- `docs/archive/` — 归档索引（phase 计划含 **phase6_record.md**、ecs 审查、issue_history 总账、
  present_timing/教程拆解等已结题调研）
- 离线 Vulkan 文档（优先在线检索被 Cloudflare 拦截时）：本地克隆 `/home/robin/program/Vulkan-Docs`
   （v1.4.360）。**注意**：spec 比系统头/loader（v357）新，结构体字段以系统头为准，spec 只作语义依据
- 最小复现/验证子项目（独立 CMake，结论已吸收进主工程与 archive）：`min_mesh_crash/`（NVIDIA mesh
  shader 必带 `NO_TASK_SHADER_BIT_EXT` 根因，见其 README）、`heap_texture_spike/`（heap 纹理三驱动
  PASS + 步长取 max，见其 README + archive/heap_texture_research.md）
