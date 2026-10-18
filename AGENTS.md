# RGL

Vulkan 现代特性学习项目。Mesh shader 渲染管线，使用 ShaderObject + DescriptorHeap + DynamicState。
核心代码位于 rgl 目录下。（旧 OpenGL 项目 legacy/ 已删，历史见 git history @ < 2026-09-08。）

## 项目哲学（切分形态的裁决依据）

四条，冲突时按序号让位。**「文档只记录不阻止」对这一段本身同样成立**：每条都点名了为它红过的
机器；写不出机器判据的「必须」当场开债，不许只留注释。本项目反复实测的真正对手不是"缺规则"，
是**看起来已完工的半吊子**——`Load<T>` 像类型化、躺在仓库里的反射产物像单一来源、注释里的
"必须同步"像护栏。

1. **单一来源不承重，「访问路径必须消费那个来源」才承重。** 反射算得出偏移、`static_assert` 也在，
   只要访问路径吃的是抄下来的 `108u`，那份反射产物就只是**一份写得很好的副本**。
2. **人不算偏移。** C++ ↔ shader 之间的布局事实只准由反射生成物承载：契约 struct 住**可 import**
   的 `.slang`（声明在入口单元的类型对 R4 探针**结构性不可达**）→ schema 登记 → 两侧吃生成物。
   样板 = `rgl/shaders/cull_tables.slang`；闸门 = `check_shader_deps.py` 的 R5/R6/R7（R7：**新增
   `ByteAddressBuffer` 声明即 ERROR**，RT 线开工前它就在那儿）。
   ⚠ 已知漏洞：调用点用**位置聚合初始化**时生成物重新失去保护（债-118，**无机器判据**）——
   跨侧布局的 struct 一律指定初始化（一个约定同时堵改名与换序两个洞）。
3. **申报 ≠ 发射权**（plan-only 红线）：申报面只拿"计划 + 校验"，barrier 与真实布局仍由模块给。
   否则 `layoutDrift` 那类对账**变成恒真**——这是本项目唯一一类"加了东西反而让判据失效"的形态。
4. **让错的东西编不过 > 让错的东西被看见。** 穷尽 switch 不写 `default`（`-Wcovered-switch-default`
   已第一方启用）、布局断言由生成物自带（`sizeof` / 逐字段 `offsetof`，期望值**不抄生成物**）；
   注释与 code review 是最后手段，不是防线。

## 给大模型
- 随时思考是否可以拆分子任务给子 agent 执行。**主 agent 必须校验子 agent 结果**。
- 只读调研（代码/文档定位）可委派`researcher` 子 agent，主agent简要查验结果。
- 优先分解执行任务给`worker`子agent执行。

## 项目 Skills（按需加载，命中触发词先 skill 工具读取）

- **`rgl-verify`** — 运行环境、headless 冒烟、llm_debug 截图、BIT-IDENTICAL 回归、**什么能并行**、材质定位（加法闪色）、imgqa 判据（改渲染后验证必用）
- **`rgl-shader-contract`** — 反射管线 / `just reflect` / scalar layout / bindless / 关键契约文件位置表（改 `rgl/shaders/`、`*_schema.lua`、`generated/` 前必读）
- **`rgl-new-module`** — 新建/拆分 `.cppm`、rlstd/GMF 细则、impl partition 分层原则
- **`rgl-test-discipline`** — 写测试硬纪律（阳性锚点/变异红线/沉淀地址/modules 坑；写单测、加用例、收账教训前必读）
- **`rgl-doc-placement`** — 改 AGENTS.md 前的归宿裁决五问 + **抄名单反模式**（**拟向 AGENTS 增删任何段落前必读**）
- **`rgl-glossary`** — 项目自造简写的白话对照表；**向用户解释任何项目概念前先查它**，命中就用
  表里的白话说法（术语名单以那张表为唯一真相源，本行不抄；对话语域 → 全局 AGENTS）

## 构建

项目使用 `just` 管理构建流程（参见 `Justfile`），支持 Linux 和 Windows 双平台。

```bash
just config              # CMake 配置（默认 clang, Release）；just config gcc Debug 指定编译器/类型
just build               # 增量编译（需先 config）；just build-release / build-debug = 一键配置+编译
just reflect             # 重跑 11 条反射管线（改动 shader 契约/schema/模板后必须，见 skill rgl-shader-contract）
just checks / doc-check / style-check [--strict]  # 源码检查统一入口（每次 just build 已随 source_checks 跑、只警告）
just tools-check         # 编 imgqa + 跑 imgqa selftest + 编 meshlet_pack（无引擎无设备；**已并进 just gates**）
just build-tools         # tools-check + 一遍非严格 checks（改完 tools/ 想立刻知道编不编得过）
just run                 # 运行
just smoke / just shot   # agent 一次性 headless 冒烟 / 截图（配置走 RGL_CONFIG_DIR，**不碰 configs 副本**）
just regress ab|floor|burst|selftest  # 逐位回归 + 同构建地板归因（改渲染后验收的默认起手；口径红线 → skill rgl-verify）
just cpu [--arms default,no-vvl --json f.json --compare f.json --list-arms]  # CPU 分布账重测（债-02/20/30 读数；
                       # **单实例串行**，主口径 p50；**CPU 读数必须带 VVL 档**；cull 三档对照 =
                       # `cull-gpu-sync` / `cull-gpu-lagged` 两臂 → tools/cpu_baseline.py 口径五条）
just run-igpu / run-dgpu / run-intel   # 强制核显 / NVIDIA prime 独显 / Intel anv
just test                # 单元测试（tests/，**无 GPU 无窗口**，毫秒级；排除 gpu 套件）
just test-all / test-gpu # 含设备级原语用例（要真设备 + VVL；无设备**FAIL 不 SKIP**）
just gates               # 四道门禁一条命令（build → test → tools-check → checks --strict，**逐条取
                       # 退出码**）。第四步是债-133 补的：imgqa 是 EXCLUDE_FROM_ALL，而它的
                       # selftest 一异常退出会**连带其后测试一起不跑**——实测坏过 4 个提交而三道全绿。
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
- **构建内置两个检查步骤，阻断性不同**（原写「均只警告不阻断」是错的，2026-10-14 实测改准）：
  `reflection_check`（反射产物 SHA1 护栏，与编译并行）**输入变了即 FATAL 阻断构建**——所以临时改
  shader 做变异之后，要么 `just reflect`，要么连 `rgl/generated/reflection_manifest.txt` 一起还原，
  只还原 `.slang` 与生成物会剩一条建不过去的构建；`source_checks`（样式 + Doxygen 单入口
  `tools/checks/`；**红线：依赖 rgl 编译完成后才跑——
  libclang 读半写 BMI 会崩，勿删该 add_dependencies**）只警告不阻断。规则参数唯一真相源 = `tools/checks/config.py`，
  规则全景 = `docs/风格检查规则.md`（**别在文档里抄规则清单**）。内部机制 → `docs/技术概念.md §构建检查工具机制`。
- **工具语言纪律**（用户拍板，三机测试）：要依赖的工具一律 **C++ + vcpkg**；**留在 Python 的工具
  只准 stdlib-only**，不引入任何需要 pip 的依赖。
- Windows 前置（LLVM / Vulkan SDK / vcpkg platform expressions）→ `docs/技术概念.md §构建（Windows 前置与 vcpkg）`。

## 测试体系

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

## 其他文档

> 每篇文档头部带状态（`现行`/`历史`/`调研`）——**状态只住那里**。本节写日期 / 批次号 /"已落地·
> 挂起·冻结"= 给文档状态开第二份副本，文档一动它就静默变错（实测抓到两条已漂，方向是误导）。
> 本节**不是文档索引**（用户拍板：索引在人脑里，`ls docs/` 是真相源）——这里只挂**每次开工都可能
> 需要路由**的几篇。护栏因此只做单向反查：本节写出的 `docs/**.md` 磁盘上须存在，悬空即 error
> （规则 `agents_doc_index_checks`，§1.6；原"每篇都得挂号"的正查已随此决定退役）。
>
> **教训写哪（2026-10-08 换轨：全局调试历史已删除，不再重建）**：能落成测试 / 护栏的**必须**落成
> （`TEST:` / `GUARD:`）；写不出断言的教训**就近写当前任务文档**，且**可改写**——被实测推翻的旧教训
> **直接删**，只留正确做法，发现矛盾以新发现为准就地改（错误版本靠 git 沉淀，不留"旧结论 + 更正"双层
> 误导后来者）。引用已删文档的旧条目写 `git:调试历史 <日期>`。写法与裁决树 → skill `rgl-doc-placement`；
> 护栏 = `doc_pointer_checks`（悬空文档指针 / 退役文档名复发，规则 §1.7）。
- **台账 / 现状**：`docs/还债总账.md`（**全项目技术债唯一台账**；结案账目只留一行终态 + 指针）·
  `docs/还债实测数据.md`（结案账目的实测数据 + 主 agent 亲验记录）· `docs/cpu_perf_plan.md`
  （CPU/GPU 读数与还账方案；`just cpu` 口径 + 基线表）· `docs/技术概念.md`（机制深版：
  §核心运行时模块表 = **唯一模块职责表** / §main 启动流 / §RPC 调试通道 / §构建检查工具机制 /
  §构建 Windows 前置）· `docs/gate_lint_plan.md`（**加诊断门前看**）
- **调研 / 活文档**：`docs/mdi_plan.md`（Multi Draw Indirect **可行性定案载体**；**动 indirect 前读，
  别重推一遍**）· `docs/compute_调研.md`（图外 compute 全景 / 渲染图**真实边界** / 队列归属事实基线 /
  **扩渲染图形态的定案载体**；**动 compute 或扩渲染图前读**，§9 负知识别再重查）
- **离线参考**：本地克隆 `/home/robin/program/Vulkan-Docs`（在线检索被 Cloudflare 拦截时用）。
  ⚠ spec 比系统头/loader 新，**结构体字段以系统头为准**，spec 只作语义依据
