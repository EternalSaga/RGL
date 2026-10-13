# RGL

Vulkan 现代特性学习项目。Mesh shader 渲染管线，使用 ShaderObject + DescriptorHeap + DynamicState。
核心代码位于 rgl 目录下。（旧 OpenGL 项目 legacy/ 已删，历史见 git history @ < 2026-09-08。）

## 给大模型

优先使用中文回复用户，除非用户说使用英文回复。
随时思考是否可以拆分子任务给子 agent 执行。**主 agent 必须校验子 agent 结果**（实测踩过：子 agent
把 sizeof 生成物 4B 报成 100B）；**困难任务主 agent 亲自执行**。只读调研（代码/文档定位）可委派
`researcher` 子 agent，涉及产物布局/大小须自己复核生成物本体。

## 项目 Skills（按需加载，命中触发词先 skill 工具读取）

- **`rgl-verify`** — 运行环境、headless 冒烟、llm_debug 截图、BIT-IDENTICAL 回归、**什么能并行**、材质定位（加法闪色）、imgqa 判据（改渲染后验证必用）
- **`rgl-shader-contract`** — 反射管线 / `just reflect` / scalar layout / bindless / 关键契约文件位置表（改 `rgl/shaders/`、`*_schema.lua`、`generated/` 前必读）
- **`rgl-new-module`** — 新建/拆分 `.cppm`、rlstd/GMF 细则、impl partition 分层原则
- **`rgl-test-discipline`** — 写测试硬纪律（阳性锚点/变异红线/沉淀地址/modules 坑；写单测、加用例、收账教训前必读）
- **`rgl-doc-placement`** — 改 AGENTS.md 前的归宿裁决五问 + **抄名单反模式**（**拟向 AGENTS 增删任何段落前必读**）

## 构建

项目使用 `just` 管理构建流程（参见 `Justfile`），支持 Linux 和 Windows 双平台。

```bash
just config              # CMake 配置（默认 clang, Release）；just config gcc Debug 指定编译器/类型
just build               # 增量编译（需先 config）；just build-release / build-debug = 一键配置+编译
just reflect             # 重跑 8 条反射管线（改动 shader 契约/schema/模板后必须，见 skill rgl-shader-contract）
just checks / doc-check / style-check [--strict]  # 源码检查统一入口（每次 just build 已随 source_checks 跑、只警告）
just build-tools         # 构建 imgqa + 跑 checks（imgqa 不进 ALL，`just build` 不编它 ⟹ 它编译失败没人发现）
just run                 # 运行
just smoke / just shot   # agent 一次性 headless 冒烟 / 截图（配置走 RGL_CONFIG_DIR，**不碰 configs 副本**）
just regress ab|floor|burst|selftest  # 逐位回归 + 同构建地板归因（改渲染后验收的默认起手；口径红线 → skill rgl-verify）
just cpu [--arms default,no-vvl --json f.json --compare f.json --list-arms]  # CPU 分布账重测（债-02/20/30 读数；
                       # **单实例串行**，主口径 p50；**CPU 读数必须带 VVL 档**；cull 三档对照 =
                       # `cull-gpu-sync` / `cull-gpu-lagged` 两臂 → tools/cpu_baseline.py 口径五条）
just run-igpu / run-dgpu / run-intel   # 强制核显 / NVIDIA prime 独显 / Intel anv
just test                # 单元测试（tests/，**无 GPU 无窗口**，毫秒级；排除 gpu 套件）
just test-all / test-gpu # 含设备级原语用例（要真设备 + VVL；无设备**FAIL 不 SKIP**）
just gates               # 三道门禁一条命令（build → test → checks --strict，**逐条取退出码**）。
                       # 🔴 别拿 `just checks 2>&1 | grep 汇总` 判绿——管道只返回最后一段的状态
                       # ⟹ 退出码被吞成 grep 的 0，而"0 警告"那行有没有 ERROR 都会被打
just install-hooks       # 装 pre-push 钩子（push 前跑 just test，实测 2s）；🔴 **每台机器 / 每个
                      # worktree 各装一次**，没装 = 回归网只在有人记得敲时才存在（钩子本体在
                      # `tools/hooks/`，接管原 LFS 钩子并在测试通过后接力；`just pre-push` 当命令自检）
just coverage            # 分层测试覆盖量具（rlbase/rlgfx/rlscene/rlframe 谁有 tests/ 消费者；
                      # **只出数不裁决**，口径真相源 = `tools/checks/coverage_layers.py` docstring）
just analytic            # 闭式解探针：算像素「该是多少」并与实机截图逐字节对拍（要 GPU）
just imgqa <子命令>       # 区域归因判据工具箱（子命令与默认值 → `imgqa -h`；判据纪律 → skill rgl-verify）
just fsr-blobs / fsr-check / fsr-smoke   # Phase 14 vendor FSR 3.1.4：重生成排列产物 / SHA1 护栏 /
                     # 最小设备冒烟（要真设备；vendor 改动清单与上游已知问题 → `third_party/fsr/PATCHES.md`）
just clean / clean-all / all           # 增量清理 / 删 build 目录 / clean-all+build-release
```

- **CMake 无独立构建目录**——必须从根目录构建（`cmake -B build -S .`）。`tests/` 是
  EXCLUDE_FROM_ALL，故**不进 `just build`**（要 `just test` 自己拉）。
- **构建内置两个检查步骤**（均只警告不阻断）：`reflection_check`（反射产物 SHA1 护栏，与编译并行）
  + `source_checks`（样式 + Doxygen 单入口 `tools/checks/`；**红线：依赖 rgl 编译完成后才跑——
  libclang 读半写 BMI 会崩，勿删该 add_dependencies**）。规则参数唯一真相源 = `tools/checks/config.py`，
  规则全景 = `docs/风格检查规则.md`（**别在文档里抄规则清单**）。内部机制 → `docs/技术概念.md §构建检查工具机制`。
- **工具语言纪律**（用户拍板，三机测试）：要依赖的工具一律 **C++ + vcpkg**；**留在 Python 的工具
  只准 stdlib-only**，不引入任何需要 pip 的依赖。
- Windows 前置（LLVM / Vulkan SDK / vcpkg platform expressions）→ `docs/技术概念.md §构建（Windows 前置与 vcpkg）`。

## 测试体系

> **下一步做什么** → `docs/phase15_plan.md`；本节 = 现状机制描述。

**分层，边界按"要不要 GPU / 要不要窗口"划**（覆盖清单以 `tests/` 与 `tools/imgqa/` 本体为准，别在此抄）：

| 层 | 位置 | 需设备 | 入口 |
|---|---|---|---|
| 单元测试层 | `tests/`（doctest，C++20 modules）——纯函数/契约 | **0** | `just test` |
| 设备原语层 | `tests/` 的 `TEST_SUITE("gpu")`——后端原语 golden | 全部 | `just test-all` / `test-gpu` |
| 闭式解探针 | `tools/imgqa/` probe + `qa/configs/analytic/`——「某像素**该**是多少」的独立第二实现，**不需要基线图像** | 全部 | `just analytic` |
| 区域归因判据 | `tools/imgqa/`（**不进 ALL**）——区域来自几何而非图像。**排查渲染问题的默认起手** | 全部 | `just imgqa selftest` / … |
| 进程级集成层 | `qa/`（冻结配置目录 + 追加式调用台账 `qa/log/`）；**量化 harness = imgqa 自身**（RPC 驱动 + 量化 + 台账），不另建第二套 → `docs/phase12_plan.md` D7 | 全部 | 场景生成 `tools/qa_scenes.py` |

- **分流靠 doctest 测试套件**，不是 tag 也不是链接面：本构建 doctest 的 `TEST_CASE` 宏**只接受一个
  参数**（tag 形态不存在），设备级用例写进 `TEST_SUITE("gpu")`，`just test` = `--test-suite-exclude=gpu`。
  **设备级用例在无设备机器上必须 FAIL 不 SKIP**——静默跳过 = 假绿。
- `rgl_tests` 链全部四层库：决策 = 「import rlvk」是编译期依赖，「需要一个 VkDevice」才是运行期依赖。
  代价 = 测试二进制链了 Vulkan；保住的是**运行期**性质（默认子集不碰设备路径）→ `tests/CMakeLists.txt`。

### 引擎四库分层

可测逻辑必须住进库（不能留在 `main` 里）。`rlbase` = **整层无设备依赖**；`rlgfx` / `rlscene` /
`rlframe` = 设备级。

- **模块清单以 `rgl/CMakeLists.txt` 五份清单为唯一真相源**（本文件不抄模块名——抄一次漂一次）。
- **新增模块的流程**：加进职责归属正确的那份清单 → configure 期**归属完整性闸门**核对「磁盘上每个
  `.cppm` 恰好登记一次」，漏登记即 `FATAL_ERROR` 点名。判据只有一个：**这段逻辑需不需要一个
  VkDevice 才能跑**——不需要 → 属 `rlbase`，就该能在 `just test` 里测。
- 依赖只向下，逆向 import 立刻链接失败，**机器可验**：`tools/checks/rules_layers.py` 以那五份清单为
  真相源扫 import 边；规则可能静默失效时也报 error（失效不出声 = 「零警告」会被读成「分层干净」）。
- 两条踩过的坑（include 面/编译宏归属判据、勿用 model.json 做依赖分析）→ `rgl/CMakeLists.txt` 头注释承载。

### 写测试的硬纪律

全量十条 + 新规 → **skill `rgl-test-discipline`**（写单测、加用例、收账教训前必读）。

## 运行要点

- **工作机器不著书**（用户拍板：三台机器轮换）：开工先认设备（启动日志 deviceName / vkinfo）；
  本文与 docs 里的性能数字均**绑特定机器**，跨机先复测再对比。
- **CWD 必须是 `build/rgl/`**（相对路径读 configs/ 与 spv）· 运行模式全配置化 · build 端 configs
  副本 = `copy_if_different`（**只源更新才覆盖**，改副本调试不会自动复原）。
  运行环境细节、冒烟/截图流程、回归方法学、**并行红线** → skill `rgl-verify`
- **RPC 调试通道**（端点速查 → `docs/技术概念.md §RPC 调试通道`）：门 `diagnostics.rpc` **默认关** +
  `rpc_port = 8788`（只绑 127.0.0.1；与 imgui 门独立 → headless 可用）；起跑前 `pgrep -cx rgl` 确认无残留
- "哪个材质占这块屏幕"用**加法闪色**（改 emissive 不改 baseColor）· "截 UI"验收**必须外部截图** → skill `rgl-verify`

## 命名与代码风格

**所有 C++ 符号均在 `namespace RGL` 中**（模块导出包裹 `namespace RGL { ... }`，跨模块引用统一
`RGL::` 前缀，入口用 `using namespace RGL;`）。优先 C++23 `std::ranges`：视图适配器、
`std::ranges::for_each` 替换原始循环、链式组合避免中间容器。

> 成员命名（不加 `_`/`m_`）、Doxygen 块、模块结构、现代 C++ 禁用项等**由 source_checks 点名到
> 文件:行号，本文件不重述**——`just build` 每次自动跑。

## 代码规范

- **模块优先**：新代码**必须** C++20 modules（`.cppm`），不在模块外 `#include` 项目内部头文件。
- **rlstd / rlvk 优先**：`.cppm` 一律 `import rlstd;` 取 STL 类型、`import rlvk;` 取 Vulkan C++ 头，
  GMF 不 `#include` STL（例外仅 `<ranges>`/`<filesystem>`/`<chrono>`）。**缺名去补 `rlstd.cppm` /
  `rlvk.cppm` 的 `export using`，而不是在本文件加头**——这是唯一不显然的一步。

扩展名、Doxygen、>100 行该拆 impl、分层 import 方向**由 source_checks 点名**，此处不写。
完整 checklist 与 impl partition 细则 → skill `rgl-new-module`。

### ECS 设计规范
场景代码（ecs/scene/model_importer + 相关 system）必须符合五条规范（逐条处置记录：`docs/archive/ecs_compliance_review.md`）：

1. **registry 是场景唯一数据源**——场景状态不在 registry 外平行维护（每帧/配置状态进 context；资产数据进 main 装配层持有的 `AssetTable`）
2. **entity 是 ID、component 是纯数据、system 是逻辑唯一载体**——system 间经数据通信，执行顺序显式（显式 system 列表，不用 entt::scheduler）
3. **场景构成数据驱动**——"场景里有什么" = registry 里有什么 entity，不用全局变量/config 字段/构造参数决定构成（画不画 = 实体有无）
4. **分层**——底层 Vulkan 类不依赖 ECS、不认识具体资产（Renderer 零 entt 依赖）；渲染 system 产出抽象 draw list（`FrameDrawList`），执行器只盲执行
5. **资产与场景实例分离**——组件引用稳定资产 ID（`MeshRef.asset`/`MaterialRef.material`），不引侧数组下标；五方一致性（entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args）收敛为资产表一处映射

## 模块结构（C++20 modules, .cppm）

**职责表不在本文件**——唯一模块职责表 = `docs/技术概念.md §核心运行时模块表`（按构造顺序排列，
新增运行时模块插对应位置）；**实现细节仍以各 `.cppm` 文件头注释为权威**。工具/数据/生成模块速览
→ 同文件 §模块表；深版启动流 → 同文件 §main 启动流。

**构造顺序骨架**（= main 启动流顺序 = import 依赖方向）：

```
SDLWindow → AppContext → DeviceContext → Swapchain → Resources → ShaderRegistry
          → compileRenderGraph → entities 循环（内建几何→顶点→纹理→env 上传→模型注册+灯）
          → 候选表 + GpuCull → addSkyEntry/deferred 装配 → DGC 条件构造
          → Renderer → wireDebugRpc → renderSystem
                └──── entt::registry（WindowExtent/ViewProjection 等每帧 context）────┘
```

- **关键时序契约**（heap 用户槽分段 / env 上传时机 / 候选表迭代序 / DGC 先于 Renderer）→
  `docs/技术概念.md §main 启动流`（完整版，本文件不维护副本）
- **依赖图原则**：只允许下层 import 上层，**配置模块不依赖执行模块**（camera_config 不 import llm_debug）、**ecs 不依赖 llm_debug**（截图经回调）——假边会让慢模块进入本不该等它的编译前置。

### 生命周期红线（编译器**不管**的那两类）

- **CreateInfo 的 `pApplicationInfo` / `pNext` 引用的结构必须是类成员**，不能是构造期栈上临时量——
  悬空指针不报错，实例化时才炸（先例 `vkinstance` / `vkdevice`）。
- **GPU 资源所有权**：长生命周期 GPU 资源（纹理 BO/描述符存储）不得声明在 main() 局部块内——
  块尾析构 = 第 0 帧起 GPU 采样已释放 VA（UAF，见 `docs/archive/issue_history.md`）。

> `vk::raii::*` 默认构造已删除、成员初始化顺序（`-Wreorder`）**编译器直接报错**，本文件不写。

## 着色器编译

Slang → SPV 构建时编译到 `build/rgl/generated_shaders/`。硬规则：**改 shader 契约/schema/模板后必须
`just reflect` 并随改动提交生成产物**（`generated/*.cppm` 是已提交产物，构建开头 SHA1 护栏比对）。
完整流程、bindless 原则、关键契约文件位置表 → skill `rgl-shader-contract`。

## 隐式扩展依赖

`features.cppm` 的 `kDeviceExtensions()` 9 项里，除 `VK_EXT_DEVICE_GENERATED_COMMANDS`（gpu_cull /
vk_indirect_commands 显式使用）外**均无显式使用但被其他已启用扩展隐式依赖，不可随意删除**；
`vkinstance.cppm` 的 `VK_EXT_SWAPCHAIN_COLOR_SPACE` 同理。**KHR/EXT 别名**：
`VkSwapchainPresentFenceInfoKHR/EXT` 是同一结构（typedef alias），代码用 EXT。

## 已知问题

> 只列**未解决问题**、标「现行」的**契约**、以及**已结案但防止重复排查的结论**（如 AO 噪点）。
> 已解决问题的根因/误诊复盘见 `docs/archive/issue_history.md`。性能数字一律标机器、结论全文放
> phase 计划，本节只留"每次开工都得知道"的边界。

- **本机 1280×720 帧时间构成（2026-10-09 重测 + 债-30 还账后，绑机；旧结论"CPU 侧无余量"已作废）**：
  交付路径（VVL 关）= **frame 10.66ms vs GPU 10.30ms ⟹ GPU-bound 且已贴地板（差 0.36ms）**；
  开 VVL 后 CPU 录制翻到 15ms（VVL ≈ 0.73µs/条 vkCmd）⟹ **"CPU-bound" 只在开发环境成立**。
  **CPU 侧读数必须带 VVL 档**，且"CPU 真干"要减掉**两处**主机等待（cull 读回 + 帧首 fence）——
  债-30 还掉后背压不消失只**搬家**（实测从 cull 等待 10.78ms 搬到帧首 6.73ms）。
  读数/口径/方案 → `docs/cpu_perf_plan.md`；重测 = `just cpu`。present_timing 跨机复测仍未做（债-07）
- **渲染近似（现行边界）**：世界法线 mat3 直乘（假定均匀缩放）；透明排序 per-primitive 距离²（非 OIT）
  且开背面剔除；BRDF LUT 仍无；mip blit NEAREST（sRGB LINEAR 违 VUID-00239）；单环境贴图；大三角 z=1
  依赖 depth clear + LEQUAL；**light/env 两个 intensity 语义不对称**（light CPU 预乘、env shader 端乘，
  亮度跨路线不可直接对比）；skybox 背景在 deferred light pass 内合成（非独立 pass）
- **渲染图边界（现行）**：线性 pass 序（无拓扑排序/fusion/aliasing）；单 swapchain attachment；多线程录制不做。
  两个图字符串字段的词表单一真相源 = **`pass_vocabulary`**（按 scope 分档），**两档不可并成扁平表**
  （并集会让 kind 与 state 串味的图放行到执行器深处）
- **SSAO 三条固有边界**：① depth-only → **法线贴图细节看不见**（开阔地面 AO 恒 1.000，**不是 bug**）；
  ② 螺旋采样无投影 range check → 前景轮廓外一圈光晕；③ 施加点在 tonemap 合成处，**连直接光一起压**。
  `ao.strength=0` 逐位旁路，无常驻成本。**AO 噪点已量化结案：别再当"噪点闪烁"的嫌疑**（交付颗粒
  < 1 量化步长 = 不可见；TAA 关时 AO 逐帧静止）；可见形态是**屏幕锚定的相机移动爬行** → `git:调试历史` 2026-10-04
- **湿面（15.2 已落地，旧边界「只加高光不压暗」已结案）**：wetness 身份 = roughness 通道
  （与 SSR 粗糙度门控同源）⟹ **跨场景光滑介质会被误判成湿**，需材质白名单；三项调制
  （压暗/水洼粗糙度/水膜法线扰动）在 deferred_geometry 的 G-buffer，法线扰动**同时进
  NdotL/AO/SSR**（不只反射项）；参数 = runtime.lua `wet` 块，`strength=0` = 逐位旁路
  （湿面关锚 `5ff793bf…`）→ `docs/phase15_plan.md` §3.3
- **灯契约（现行）**：`Light` 组件 + `index` = 列表位正序（画不画 = 实体有无；forward 读 index 0 = 主光，
  延迟按 index 落槽）；`SceneLight`/`GpuLight` 布局均走 C API 反射生成，无手写双源
- **GPU culling（现行）**：三态门 **`diagnostics.cull_replay`，默认 `cpu`** = opaque 段由 **CPU 剔除**驱动、
  GPU compute culling 退为**异步对账验证器**（债-30 还账，实测逐位不变）；`gpu_sync` = 旧的"compute culling +
  **读回重放**"同帧形态，`gpu_lagged` = GPU 集照跑但滞后一帧对账。**非默认档必须单独冒烟 + syncval**——
  默认档不提交 cull CB，fence 生命周期与跨帧依赖错误只在两 GPU 臂现形（`just cpu` 的 `cull-gpu-*` 两臂）。
  DGC execute 仅 NVIDIA graphicsIndirect=true 可走，RADV 与 Intel anv 实测均 false → 重放；RADV 静态 shader
  模式功能通但性能亏 1.85× → `diagnostics.dgc_static` 门默认关（`cull_replay≠gpu_sync` 时该路根本走不到，
  gate-lint 会喊）。CPU 剔除保留为兜底 + 每帧对账基准（不一致告警不中断）；
  mvp 由 CPU 算进 scene data（GPU float4x4 FMA 收缩序与 glm 无位级契约，保 forward diff0）。
  驱动级细节 → `docs/nvidia_compatibility_report.md` / `docs/archive/radv_static_dgc_spike.md`
- **VMA + zero-init 冲突**：VMA 对 BDA buffer 自动加内部 DEVICE_ADDRESS flags，与用户 ZERO_INITIALIZE
  pNext 必撞——该扩展已回退，真实需求走原生 `vkAllocateMemory` 绕 VMA（`docs/vulkan_extensions_2025_调研.md` §十二/§13.1）

## 其他文档

> 每篇文档头部带状态（`现行`/`历史`/`调研`）。现状权威 = 本文件；**下一步路由 → `docs/phase14_plan.md`**。
> 本节与 `docs/` 由 source_checks **双向对账**（docs 漏索引 / 本节悬空指针即警告，规则
> `agents_doc_index_checks`）——新建 docs 不在此挂号会被构建点名。**本节只做索引，不写摘要**（摘要会漂）。
>
> **教训写哪（2026-10-08 换轨：全局调试历史已删除，不再重建）**：能落成测试 / 护栏的**必须**落成
> （`TEST:` / `GUARD:`）；写不出断言的教训**就近写当前任务文档**，且**可改写**——被实测推翻的旧教训
> **直接删**，只留正确做法，发现矛盾以新发现为准就地改（错误版本靠 git 沉淀，不留"旧结论 + 更正"双层
> 误导后来者）。引用已删文档的旧条目写 `git:调试历史 <日期>`。写法与裁决树 → skill `rgl-doc-placement`；
> 护栏 = `doc_pointer_checks`（悬空文档指针 / 退役文档名复发，规则 §1.7）。

- **台账 / 现状**：`docs/还债总账.md`（**全项目技术债唯一台账**；勾账三态 = 结案/否决/降级；
  **结案账目只留一行终态 + 指针，数字在 `docs/还债实测数据.md`**）·
  `docs/还债实测数据.md`（结案账目的**实测数据 + 主 agent 亲验记录**载体；标了「权威载体」的小节以那里为准）·
  `docs/cpu_perf_plan.md`（**CPU/GPU 性能读数与还账方案**：量具 `just cpu` 的口径 + 基线表 +
  债-30 三档方案与**落地实测/验收**；本文件"帧时间构成"那条结论的全文在这里）·
  `docs/技术概念.md`（机制深版：§模块表 / §核心运行时模块表 = **唯一模块职责表** / §main 启动流 /
  §RPC 调试通道 / §构建检查工具机制 / §构建 Windows 前置）· `docs/gate_lint_plan.md`（**门配置自检的权威载体**；加诊断门前看）
- **现行计划**：`docs/phase15_plan.md`（**下一步从这里读**：屏幕空间反射 + 湿面输入面；§0 = D1
  豁免与「成本不设上限」两条拍板；§1.3 = Filament SSR 的可抄/不可抄面，**别再重读一遍 filament**）·
  `docs/phase14_plan.md`（时域线换轨 = 逐像素速度场 + vendor
  FSR 3.1.4 的 VK 后端；§1 = 本机核实的 FSR 事实基线，**别再重查一遍**；剩余 14.7/14.8/14.9
  不阻塞 Phase 15）·
  `docs/phase13_plan.md`（TAA 收口，**2026-10-07 冻结**；§1.22 = 冻结立论）·
  `docs/taa_history_space_plan.md`（**批 13.5 已落地** = 历史住基空间；批 12.6 T4 的「未抖
  prevViewProj 是错的」裁决**已反转**）·
  `docs/taa_plan.md`（TAA 机制与踩坑账，§7 第 11/12 条 = 根因）· `docs/evssm_plan.md`（远场软影
  权威路线，HFTS 已降级 backlog）· `docs/shadow_routes_plan.md`（**阴影路线形态唯一定案载体**：`configs/shadow.lua` 的 `mode` 枚举形状与四条红线；改阴影配置键/门形前看）·
  `docs/constantbuffer_迁移计划.md`（scene data 类型化迁移的**唯一计划载体 + spike 已定案事实基线**；
  **动 scene data 访问形态前看**，§1 别再重查、§6.2 是分支拓扑拍板、✅ §8 = R3 红线已窄化放行）· `docs/hfts_izb_plan.md` · `docs/shader_object_组合成本与优化预留.md`
  （优化**未实施**）· `docs/脚本长期规划.md` + `script_spike/`（脚本长期线，独立于 Phase 编号）
- **调研 / 活文档**：`docs/mdi_plan.md`（Multi Draw Indirect 路线**可行性定案载体**，**2026-10-12 簇化轴
  立项开工（§0 拍板 P4-P7、§9 编排）**、命令数轴挂起：
  DGC 的 `@8 PUSH_DATA` 为何不可删 / `push.base + DrawIndex` 统一公式 / 阶段划分与风险；
  动 indirect 前先读，别重推一遍） · `docs/DESCRIPTOR_HEAP_SPEC.md` · `docs/FSR输入契约.md`（Phase 14 批 14.2：
  FSR3-upscaler 四个每帧输入的**口径权威载体**——单位/坐标系/符号/采样时刻 + 逐条出处；
  接 FSR 前必读，别靠读源码"认为一致"） · `docs/nvidia_compatibility_report.md`
  （三平台；**不写"本机"是哪台**）· `docs/device_generated_commands_research.md` ·
  `docs/vulkan_extensions_2025_调研.md` · `docs/device_fault_调研.md`（**未实施**）·
  `docs/风格检查规则.md`（规则全景；参数真相源 = `config.py`）· `docs/快捷键配置.md`（与代码无关）
- **历史 / 归档**：`docs/next_roadmap.md`（Phase 0–7 结论存档）· `docs/phase8_plan.md` ·
  `docs/phase9_plan.md`（§10 性能判定全文）· `docs/phase11_plan.md` · `docs/phase12_plan.md`
  （测试体系账本）· `docs/phase7_devlog.md` · `docs/phase10b_plan.md` · `docs/archive/`
  （phase6_record、ecs 审查、**issue_history 总账**、radv_static_dgc_spike、阴影失效排查、静态审查升级路线等）
- **离线参考**：本地克隆 `/home/robin/program/Vulkan-Docs`（在线检索被 Cloudflare 拦截时用）。
  ⚠ spec 比系统头/loader 新，**结构体字段以系统头为准**，spec 只作语义依据
- **已删除的最小复现子项目**：`min_mesh_crash/`、`heap_texture_spike/`（墓碑 = `docs/archive/*_已删除.md`；
  根因/三驱动结论在 `docs/archive/issue_history.md` / `heap_texture_research.md`，代码见 git history）
