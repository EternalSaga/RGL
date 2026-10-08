# Phase 10 实施计划：调试交互（配置热重载 + IMGUI 参数面板 + shader 热重载 + 点选取参）

> **状态**：历史（已归档 2026-09-28）——**批 10.1–10.7 全部落地**。Phase 10 收口；
> 唯一遗留 = 面板 UI 视觉走查（用户窗口验收）+ D12（RPC 通道）延后待翻案条件。
> 批 10.7（2026-09-28 补做，点选高亮 + 选中跳页）见 §10.7 实施记录。
> 现状权威 = AGENTS.md + next_roadmap.md；本文行号/代码引用自归档起漂移，只取设计
> 思路与决策（D1–D12）为历史依据。
> 关键决策 D1–D5 由用户拍板（见 §2），D6–D12 为主 agent 技术决策（可否决）。
> **阶段号消歧**：本 Phase 10 ≠ 2026-09-10 的 Renderer 拆分批（当时批次被旧口径称
> "Phase 10 批次 0"，散见约 19 处历史代码注释——历史注释一律不改）。阴影任务顺移为
> **Phase 11**（见 `next_roadmap.md`）。

## 0. 定位与目标

四子任务合一个 Phase（用户 2026-09-27 拍板占位 Phase 10，阴影顺移 Phase 11）：

1. **配置热重载**——改 `configs/*.lua` 帧边界生效，免重启；
2. **IMGUI 调试参数面板**——窗口模式可交互调 shader/场景参数，**开关本身进配置文件**
   （用户原始需求）；
3. **shader 热重载**——改 `.slang` 运行时重编译换入（L1 深度，见 D2）；
4. **鼠标点选取参**——点中物体，面板显示并可调其参数（GPU id pass，见 D3）。

**总纲 = 单一事实来源（用户拍板硬约束）**：参数面板的字段结构**来自反射生成物**，
注册表只补两样反射永远给不了的东西（写入通道 + 语义层），**不许镜像任何布局**；
现存唯一同类双源债 `ObjectDraw` 本期内修掉（D1）。

Phase 9 衔接：本机判定 CPU-bound（`frame=7.10ms cpu.record=7.03ms gpu.frame=2.98ms`），
**GPU 余量 ≈3.1ms/帧** = 本 Phase 新增 UI/id pass 开销的预算上限；开销记账用现成
`frame_stats`/profiler（单实例串行测量纪律，见 AGENTS 已知问题）。

## 1. 调研定位（2026-09-27 只读调研，已逐项复核）

### 1.1 单一事实来源现状（关键裁决——本计划的出发点）

| 层 | 现状 | 证据 |
|---|---|---|
| **已单源（反射生成，勿动）** | `SceneLight`/`GpuLight`/`ModelVertex`/`DrawArgs` | `generated/scene_forward_types_generated.cppm:11`（48B）、`lights_types_generated.cppm:11`（64B）+ **:25-29 生成常量 `kMaxLights`/`kGpuLightStride`/`kSceneDataLightArrayBase=262272`**、`model_types_generated.cppm:11`、`material_types_generated.cppm:11`；修复提交 `fa69c86`（SceneLight 双源消除，替换手写 `LightDataGpu`）、`27ebec3`（Slang C API 逐字段 offsetof）、`ed38446`（灯数据入反射）；C++ 侧非 generated 同名 struct **零残留** |
| **未单源（本期内修）** | `ObjectDraw`（材质五参数表） | shader 真相源 `shaders/scene.slang:96-110`（totalTriangles/vertexOffset/baseColor/roughnessMetallic/emissive）；C++ 手写 `frame_draw_list.cppm:41` `ObjectDrawGpu` + `:49` sizeof==48 断言"与 readObjectDraw 字面量手工一致"；读取裸字面量 `scene.slang:114`（`524944u + objectIndex*48u`）；`material_schema.lua` 无登记 |
| **半单源** | 标量头 exposure@56/envIntensity@48/envMipRange@52 | C++ `scene_data.cppm:43-56`：`kSceneDataEnvOffset = sizeof(SceneLight)` 派生自反射 + static_assert 链钉 48/64/128；shader 侧 `Load(48)/Load(56)` 字面量无校验 |
| **无单源** | 表基址 @128 矩阵 / @262800 MVP | C++ constexpr 手写 + shader 字面量双向无校验（`scene.slang:11-13` 自述）；@524944 draw 表基址同理（可随 10.2a 像 `kSceneDataLightArrayBase` 一样入 schema） |
| **语义层（任何地方都没有）** | range/默认值/单位 | schema 只有 name/type/offset——**这不是双源，是缺失**，面板需要的新层（§3.3） |

顺带清理的**注释级历史残留**（事实，非幻觉）：`scene.slang:11-13` 点名的 "vkresources
kSceneData\*"（常量已搬 `scene_data.cppm`，vkresources 0 命中）；`shader_registry.cppm:24,30`
"100B push constant"（实况 4B）；`docs/脚本长期规划.md:70,86` 过期 "Phase 8.5 热重载批"指称。

### 1.2 面板骨架数据源（"有几个 shader 就几个下拉子项"——成立）

| 事实 | 定位 |
|---|---|
| **21 条** `rgl_shader()` 调用（实测 2026-09-27）= 下拉子项骨架 | `rgl/CMakeLists.txt:16-54`（另 :14 一条注释） |
| `findSpec` 12 规格词表（scene.lua styles 回落） | `shader_registry.cppm:200`，entries append-only `:278` |
| 20 条反射 JSON（`*_reflection.json`，`just reflect` 产物） | `build/rgl/` 运行时可读 |
| 反射工具能力：`queryStruct`/`queryPushConst`/`standardLayoutInfo` | `tools/slang_reflect.cppm:116-165`、`cmake/RglReflection.cmake:37-44` |
| **红线**：反射是 build tool 不是 runtime component | `next_roadmap.md:17`——运行时扩反射撞线（否决项 §11） |

### 1.3 四子任务地基与缺口

| 子任务 | 有的 | 缺的 |
|---|---|---|
| IMGUI | 诊断开关先例（`diagnostics.cppm` + `configs/diagnostics.lua`，`profiler=0` 同款形态 `:16`）；帧末钩子位 `vkrenderer_impl.cppm:555` `capturer.record` → `:561` `presentTransition` | imgui 依赖零引入；vcpkg 本地 port `/home/robin/program/vcpkg/ports/imgui` = imgui 1.92.8，features `sdl3-binding`/`vulkan-binding`/`docking-experimental` 齐 |
| 配置热重载 | 配置全走 `loadXxxConfig`；帧边界换入路径蓝图 = `docs/脚本长期规划.md:53`（SPV/config/脚本共用基建） | **运行时零热重载/零文件监视痕迹**；`cmake/RglRuntimeAssets.cmake:28-33` copy_if_different 复原 build 副本（→ 监听对象必须是源，见 D7） |
| shader 热重载 | slangc = `build/_deps/slang-prebuilt-src/bin/slangc`，实测支持 `-reflection-json`；`SLANG_FLAGS` 唯一声明点 `cmake/RglSlang.cmake:22-30`（`SLANG_FLAGS_STR` 派生串，`:13` 红线"新增 slangc 调用点必须引用本变量"）；`RenderPipeline` RAII `shader.cppm:187`（ctor `:213`，binding mapping 烘焙 `:213-296`） | `pipelineIndex` 三方持有（资产表 / GPU cull 静态表已上传 GPU / DGC execution set 按 `entriesView` 全表烘焙 `vk_indirect_commands_impl.cppm:43,105,258`）——**VkShaderEXT handle 依赖 = 最大未知**（10.4 前置精查） |
| 点选取参 | 唯一 `SDL_PollEvent` 点 `ecs_impl.cppm:47`；`MeshRef.objectSlot` = `DrawArgs.objectIndex` = 各表下标（现成锚点）；三套帧内回读先例（`frame_capture` request/record/collect、`gpu_cull` readCull、present_timing collect） | 事件流无绝对坐标（`ecs_impl.cppm:87-94`）；无 id 通道、无 depth 回读先例；`AssetTable` 材质只读无 setter（`asset_table.cppm:207` add / `:224` const get，且**材质去重共享**） |

## 2. 决策记录

| # | 决策 | 结论 | 狀态 |
|---|---|---|---|
| **D1** | 参数数据源 | **反射结构驱动**：控件字段引用反射生成物（offsetof/sizeof/生成常量）；`ObjectDraw` 单源化为材质控件**前置批**；注册表只存写入通道 + 语义层；`scene.slang` 字面量区布局不改 | ✅ 用户拍板（2026-09-27，"坚守单一事实来源就没问题"） |
| **D2** | shader 热重载深度 | **L1 同签名才热换**：运行时 fork slangc 重编译 + `-reflection-json` 签名守卫，binding/push/顶点布局一致才同槽重建；不一致 fail-fast 提示重启；DGC 失败降级读回重放 | ✅ 用户拍板 |
| **D3** | 点选方案 | **GPU id pass**：独立 object-id attachment（默认关不进图保 diff0）+ 帧内回读一个 texel，像素级精确；CPU 射线方案否决 | ✅ 用户拍板 |
| **D4** | 本期范围 | **10.1–10.5 全收**；10.6（反射语义扩产）视前批情况再定 | ✅ 用户拍板 |
| **D5** | 面板边界 | **调参 + profiler 读数 + 点选实体上下文**；不做实体树浏览器/日志窗口 | ✅ 用户拍板 |
| D6 | imgui 插入点 | 帧末独立段：**`capturer.record`（:555）之后、`presentTransition`（:561）之前**——截图不含 UI ⇒ llm_debug 截图回归与 imgui 开关解耦（开/关均不影响截图基线） | 主 agent 定，可否决 |
| D7 | 配置热重载监听对象 | **源 `configs/`**（编译期注入源目录），监听 build 副本会把热重载绑在"被 `just build` 复原"的坑上；源目录不存在（发布态）优雅降级关 | 主 agent 定，可否决 |
| D8 | 截图与 imgui 关系 | 不互斥；且因 D6 顺序，UI 不进截图（"含 UI 基线"无需双记）；headless 天然无窗口无 UI | 主 agent 定，可否决 |
| D9 | imgui 引入方式 | vcpkg `imgui[core,sdl3-binding,vulkan-binding,docking-experimental]` 1.92.8，vcpkg.json platform expression 双平台同款 | 主 agent 定，可否决 |
| D10 | id pass 进图方式 | 由 `diagnostics.picking` 门控、**编译图期**决定（非运行时开关）——默认关 ⇒ 图逐位同 ⇒ diff0；不复用 `kGBufferAttachments` 空通道（会破 BIT-IDENTICAL） | 主 agent 定，可否决 |
| D11 | 运行时重编译 flags 单源 | 运行时 fork slangc 必须用**编译期注入的 `SLANG_FLAGS_STR` 派生串**，禁止内联重复（`RglSlang.cmake:13` 红线延伸到运行时） | 主 agent 定，可否决 |
| D12 | RPC 实时调试通道 | **延后**（非否决）。**用户提出动机（2026-09-28 补录）**：对"不便 LLM 交互"的窗口内操作——摄像机移动、点选物体、修改 shader 参数后即时观察——外部程序发个简单 HTTP POST 即可驱动，替代终端里绕弯的交互。**当前判定**：暂缓——配置热重载（10.3）+ imgui 面板 + headless/llm_debug 截图 + `diagnostics.*` 门控探针（picking_probe 即 10.5 因窗口鼠标注入不可得而生的替代方案）已覆盖大部分分析场景。**用户保留意见**：热重载"可能不太够"（多步交互式排查如"移相机→截图→再点选→读回"文件驱动确实别扭）。**翻案条件**：出现 ≥2 次"因无编程驱动通道而绕路/造专用探针"的实录，即立项轻量 loopback HTTP 命令队列（命令收敛帧边界套用，复用 ConfigReloadHook 模式） | 用户动机 + 主 agent 判定延后 |

## 3. 参数面设计（批 10.2，D1 展开）

### 3.1 `ObjectDraw` 单源化配方（前置，10.2a）

照 `fa69c86` SceneLight 先例，零新发明：

1. `material_schema.lua` 登记 `ObjectDraw`（+ 表基址 `524944`/stride `48` 走 `cppName` 生成常量，
   同 `kSceneDataLightArrayBase` 形态）；
2. 重跑 `just reflect` → 生成物进 `generated/material_types_generated.cppm`（或新 generated 文件），
   逐字段 offsetof static_assert；
3. 删 `frame_draw_list.cppm:41-49` 手写 `ObjectDrawGpu`，消费方（`render_list` 写入侧、
   `cull_driver`/`gpu_cull` 的 DrawArgs 关联处）改用生成物；
4. shader `scene.slang:114` 裸字面量改读生成常量的**对应 shader 侧声明**（或至少三方 static_assert 钉住）；
5. 顺带清 §1.1 注释残留；**shader 契约改动 ⇒ 走 skill `rgl-shader-contract`，生成物随提交**。

验收 = BIT-IDENTICAL（字节布局不变）+ `reflection_check` SHA1 过 + `checks --strict` 0 警告。

### 3.2 参数注册表（`param_registry`，10.2b 起新建）

每条目 = 三元组，**零布局镜像**：

```
结构来源：反射生成物类型 + offsetof/生成常量（编译期引用，改布局自动跟随）
语义层：  label / range / 单位 / widget 形态（slider、color picker、drag）
写入通道：目标 + 时序（下一帧写入链消费）
```

### 3.3 写入通道清单（首批）

| 参数组 | 结构来源 | 写入通道 | 生效时序 |
|---|---|---|---|
| 主光/灯（前向 `SceneLight` + 灯数组 `GpuLight[8]`） | 反射生成物 | `Light` 组件 / render_list 写入侧（`render_list_impl.cppm:87-173` 链上游） | 下一帧 |
| 曝光 / env 强度 / env mip | `scene_data.cppm` 派生常量（半单源，10.2a 后更紧） | SceneEnv ctx 同链 | 下一帧 |
| 材质五参数（baseColor/roughnessMetallic/emissive/alphaCutoff） | **10.2a 生成物** | `AssetTable` 新增 setter（注意材质去重共享：面板提示"共享该材质的实体数"） | 下一帧 |
| shader 下拉子项 | 21 条 `rgl_shader` 骨架 + `findSpec` 词表 | 子项 = 上面参数组按 spec 归组的视图 | — |

面板形态（D5）：shader 下拉（21 子项）→ 该 spec 参数组控件；profiler 读数区
（`CounterAgg`/stats 现成数据，只读）；点选实体上下文区（§7）。

## 4. IMGUI 骨架（批 10.1）

1. vcpkg 引入（D9）+ CMake 链接，新模块 `imgui_layer`（`_impl` partition，走 skill `rgl-new-module`）；
2. **输入链扩展**：`ecs_impl.cppm` 唯一 poll 点加绝对坐标字段 + 先喂
   `ImGui_ImplSDL3_ProcessEvent`、`WantCaptureMouse/Keyboard` 时不再进 InputEvents
   （保证默认关时行为逐位不变）；
3. **帧末 UI 段**（D6 位）：独立 rendering 段画 UI，位于 `:555` 与 `:561` 之间；
4. `diagnostics.lua` 加 `imgui = false` 总开关（§8）；
5. 面板骨架 = profiler 读数 + 参数占位页（控件 10.2 落）。

## 5. 配置热重载（批 10.3）

- `hot_reload` 模块：mtime 轮询放在**帧边界**（与 `脚本长期规划.md:53` 蓝图共用基建，
  未来 SPV/脚本热换同一入口），变更 → 重跑 `loadXxxConfig` → 帧边界套用；
- **T1 本批必做**：`scene.lua`（spin/exposure 等逐帧值）、`camera.lua`、`diagnostics.lua`
  运行时可热字段、`log_level.json`；
- **T2 定义但可选**：`render_graph.lua` variant（需 `compileRenderGraph` + Renderer 图成员
  重建，帧边界换入）、窗口尺寸/present mode（走 swapchain recreate）；
- **T3 不可热载**：headless/backend 选择（日志警告忽略）；
- 监听源 `configs/`（D7），CMake 编译期注入源目录，缺失优雅降级关。

## 6. shader 热重载（批 10.4，L1 = D2）

流程（全部在帧边界执行，不中断录制中 CB）：

```
watch rgl/shaders/ 全部 .slang（mtime；v1 全量重编 unique 源 = 每源 1 次 fork，共享模块去重）
  → fork slangc（注入 SLANG_FLAGS D11 + slangc 路径 + 输出临时路径；std::system 引号包路径）
  → 守卫 = **运行时 SPV 二进制解析**接口签名（2026-09-28 偏差：不用计划的 -reflection-json，
    见 §6.1）与基线比对；SPV 字节 hash 无变 = 先跳过（免签名比对）
  ├─ 同签名 → 同槽重建 RenderPipeline（pipelineIndex 不变 → 资产表/cull 静态表零改动）
  │           + DGC 两路句柄刷新（IES execution set / 静态 shader 对；先 waitForIdle）
  │           → commit（旧句柄在 set 指向新句柄**之后**才销毁）→ 下一帧生效
  └─ 不一致 / 编译失败 → warn + 保留旧 SPV 继续跑（提示"契约变化请重启"；编译失败等下次保存）
```

### 6.1 守卫实现偏差：SPV 二进制解析 替代 -reflection-json（2026-09-28）

- 计划原案 = fork slangc 带 `-reflection-json` 比对布局。实做改为直接解析**已编译 SPV
  字节**（`shader` 模块 `spvInterfaceSignature`，实现拆在 `shader:impl` 分区）：同数据面、
  少一次 fork、无 JSON 归一化痛点。
- 签名 = 排序 token 集：ExecutionMode（mode+字面量，**entry id 排除**——跨编译漂移）+
  OpEntryPoint（model+名字，**interface id 列表排除**）+ 每接口变量（storage class +
  原始 decoration 集全量含操作数 + 指向类型递归展开到字面量）。同型成员重排的可见性
  来源 = member Offset decoration 配对集合；类型 id 漂移由递归展开吸收。
- **实测踩坑两枚**（守卫假阳根因，均修）：① SPIR-V opcode 15/16 = OpEntryPoint/
  OpExecutionMode（与直觉相反易写反——写反则 EntryPoint 的 interface id 列表泄进签名
  → 跨编译必假阳）；② 类型指令结果 id 在字 1（常量在字 2）——索引错格 = defs 查表全
  miss → 原始 id 泄进签名。判据 = 29 源 fresh 编译 vs build 产物全量交叉 **21/21 SIG
  IDENTICAL**（同源码跨 slangc 版本产物字节异但接口签名稳）。

## 7. 点选取参 = GPU id pass（批 10.5，D3）

1. **输入侧**：点击事件带绝对坐标（10.1 扩的字段）；
2. **id pass**（`diagnostics.picking` 门控进图，D10）：object-id attachment（r32ui，
   独立附件，**不复用 G-buffer 空通道**），复用 mesh 顶点路 + id 片元写
   `objectSlot`（+depth 测试得可见层）；
3. **帧内回读**：copy 1 texel → map 读回（复用 frame_capture 三步曲先例的回读通路）；
4. **锚点反查**：`objectSlot` → `MeshRef` → entity → 组件（registry 唯一数据源，
   不平行维护选中状态——选中 entity 存 registry context）；
5. **面板绑定**：点中 → 面板实体上下文区显示该实体材质/灯参数（即 §3.3 控件实例化到该实体）。

## 8. 配置开关（用户需求：开关进配置文件）

全部进 `configs/diagnostics.lua`（缺文件/缺段 = 全关，与现开关同语义，启动日志汇总行）：

| 开关 | 默认 | 控制 |
|---|---|---|
| `imgui = false` | 关 | IMGUI 面板总开关（含输入拦截；headless 无窗口天然 no-op） |
| `config_reload = false` | 关 | 配置热重载（§5） |
| `shader_reload = false` | 关 | shader 热重载 L1（§6；slangc/flags 缺失再降级关并日志） |
| `picking = false` | 关 | GPU id 点选取参（§7；编译图期门控，D10） |

profiler 面板读数复用现成 `profiler = 0`（不开 profiler 时面板显示占位，不开新门）。

## 9. 分期批次

| 批 | 内容 | 主要改动 | 验收 |
|---|---|---|---|
| **10.1** imgui 骨架 **✅ 2026-09-28** | vcpkg 引入、`imgui_layer` 模块、输入链绝对坐标 + WantCapture 门、帧末 UI 段（D6 位）、`imgui` 开关、面板骨架（profiler 读数 + 占位页） | vcpkg.json、新 `.cppm`、`ecs_impl.cppm` 事件、`vkrenderer_impl.cppm` 帧末段、`diagnostics.cppm` + `configs/diagnostics.lua`、CMake | 默认关 BIT-IDENTICAL + `checks --strict`；开 = 窗口面板显示、profiler 读数跳动、headless no-op |
| **10.2a** `ObjectDraw` 单源化 **✅ 2026-09-28** | §3.1 五步：schema 登记 + 表基址常量 → `just reflect` → 删手写 `ObjectDrawGpu` → 字面量钉住 → 清注释残留 | `scene_forward_schema.lua`（实际落点，见 10.2a 实施记录）、`generated/*`（随提交）、`frame_draw_list.cppm`、消费方、`scene.slang`/`shader_registry.cppm` 注释 | **BIT-IDENTICAL** + `reflection_check` 过 + `checks --strict`（走 skill `rgl-shader-contract`） |
| **10.2b** 灯/曝光控件 **✅ 2026-09-28** | `param_registry` 模块 + 首批条目（§3.3 前两行）+ 灯/曝光控件接面板 | 新 `.cppm`、`ecs.cppm/_impl.cppm`（build 函数落点修正：render_list 无 registry ctx 全链，装配归 ecs）、`imgui_layer` 消费 | 默认关 diff0；开 = 拖动下一帧画面变化 |
| **10.2c** 材质控件 **✅ 2026-09-28** | `AssetTable` setter + 材质条目（吃 10.2a 生成物）+ 共享实体数提示 | `asset_table.cppm`、`param_registry` | 同上 + 共享提示正确 |
| **10.3** 配置热重载 **✅ 2026-09-28** | §5：`hot_reload` 模块 + 帧边界换入基建 + T1 清单；（T2 可选不做） | 新 `.cppm`、`main.cppm/_scene.cppm`（wireHotReload）、`ecs.cppm/_impl.cppm`（ConfigReloadHook）、`rllogger`、CMake 注入源目录 | 默认关 diff0；开 = 改源 `configs/` 下一帧生效且 build 副本不被污染；缺源目录降级关 |
| **10.4** shader 热重载 L1 **✅ 2026-09-28** | §6：watch + fork slangc + SPV 二进制签名守卫（§6.1 偏差）+ 两阶段同槽重建 + DGC 两路句柄刷新 | 新 `.cppm`×2 + `shader_impl`、`shader.cppm` 签名解析、`shader_registry` build/commitReplacement、`vk_indirect_commands` update 入口、CMake 三宏注入 | 默认关 diff0；开 = 改注释重载无视觉变化、改逻辑下一帧生效、**改契约被拦且旧管线继续跑** |
| **10.5** 点选取参 **✅ 2026-09-28** | §7：点击事件 + id pass 进图 + 1 texel 回读 + objectSlot 反查 + 面板实体绑定 | 新 `.cppm`×2（picking）、`decoratePickingPass`、`addPickingEntries` 克隆、执行器分支、`PickingHook`/`SelectedObject` ctx、面板"选中"页、`picking_probe` | 默认关 diff0；开 = 点击命中正确（含遮挡层序）、面板显示并可调该实体参数 |
| **10.6**（可选，D4 视情况）**✅ 2026-09-28** | 反射语义扩产：range/默认值/单位入 schema → 生成语义层，注册表薄化 | `rgl/param_semantics.json`（新第 7 管线数据源）+ inja 模板 + `param_semantics_generated`；面板构建拆出 `param_pages`(+impl) | 10.2b/c 行为不变（仅数据源换薄）✅ |
| **10.7** 点选高亮 + 选中跳页 **✅ 2026-09-28** | §7 续：`highlight.frag` 克隆管线 + 高亮 pass（写既有色附件、插消费 pass 前）+ **按材质** CPU 过滤 draw + 面板选中跳页 + Esc 取消 | `rgl/shaders/highlight.frag.slang`（新，走 `rgl_shader` SPV 管线）、`shader_registry` 克隆泛化（`ClonePurpose`）、`decorateHighlightPass`、`state_kit.highlightState`、`Renderer::replayClonedPass`、`RenderListState.materialBySlot`、`SelectedObject` 迁 `scene_context` + 加 `materialId`、`DebugPanelState.pendingSelectJump` + `ParamPage.selectOnPick` | `picking=false` **BIT-IDENTICAL** ✅；开 = 点中物体琥珀高亮（同材质全部实体一起亮）+ 面板自动切"选中"页 + 共享实体数 + Esc 取消 |

模块新建一律走 skill `rgl-new-module`；每批验收走 skill `rgl-verify`（headless 冒烟 +
BIT-IDENTICAL 截图回归，基线 `10b00c78c4a2d644479bb56eb35ac796bae90ada6433178fecd3dc21f2e72d14`）。

### 10.1 实施记录（2026-09-28）

- **输入过滤 = `InputFilter.consume(const void*)`**：`void*` 类型擦除（ecs 接口不 GMF
  include SDL，且不在 purview 前置声明 `SDL_Event`——GMF 全局模块实体 vs purview 模块实体
  同名双声明地雷）；eventSystem 循环先手调过滤器（WantCapture 门），空 = 逐位不变。
  InputEvent 新增 `pos`（鼠标绝对坐标，10.5 点选复用）。
- **imgui 依赖面收口**：imgui.h/backends 只 include 于 `imgui_layer_impl` GMF；接口只暴露
  `ImGuiLayer` 四方法。后端 = 1.92.8 dynamic rendering 路（swapchain color format 建主
  pipeline，descriptor pool 后端自建与项目 heap 无关）。rlvk 补导出 `vk::ImageView`/`::VkFormat`。
- **Renderer 帧末扩展段 = `setFrameEndHook` + `emitFrameEndOverlay`（⑪.6 位）**：执行器负责
  swapchain 任意帧末布局（CAO/TransferDst/捕获帧 TransferSrc）→ CAO barrier + 追踪表更新，
  hook 契约 beginRendering(load)；空 hook 零调用零 barrier。
- **profiler 新增只读 API `profilerSnapshotNow()`**（面板读数：帧墙钟 last/avg +
  drawFrame/gpu.frame 窗口均值；profiler=0 = enabled false）。
- **验收全过**：同会话双构建 BIT-IDENTICAL（改后 = 基线 = `10b00c78…`）；`checks --strict`
  0 警告（85 cppm）；窗口 `imgui=true` 10s VVL 0 error（含面板就绪日志）；offscreen 变体
  （blit→TransferDst→CAO overlay）barrier 分支 12s VVL 0 error。
- **v1 已知限制**：swapchain recreate 若改 format/imageCount 需重启（本机 resize 不改这两项）；
  面板仅 dock 主窗，多视口不做。

### 10.2a 实施记录（2026-09-28）

- **schema 落点修正**：登记进 **`scene_forward_schema.lua`**（非计划原文的 material_schema——
  ObjectDraw 是 scene.slang 模块级 struct，scene_forward 管线的 slang 输入正是 scene.slang，
  C API queryStruct 可直接命中；material 管线只编 material.slang 看不到它）。
- **shader 端一步到位**：`ObjectDraw` 字段**声明序改为内存序**（roughnessMetallic 移到
  baseColor 前 → scalar layout 0/4/8/16/32 无 padding 恰等于原手工 buffer 布局），
  `readObjectDraw` 从手写 Load4×3 + asuint 链改为 **`Load<ObjectDraw>` 整读**——shader 内部
  布局知识也收敛为 struct 声明一处（原手写链与声明是双份知识，字段对调即静默错位）。
- **工具侧**：`schema_loader.cppm` 的 vertexAttrSize/vertexAttrCppType 补 `uint`/`int`
  标量映射（与 slang_reflect typeToCppType 标量名对齐；首个整型顶点属性先例）。
- **C++ 侧**：手写 `ObjectDrawGpu` 删除 → 生成 `ObjectDraw`（逐字段 offsetof assert 随生成物）；
  消费方 = frame_draw_list span / render_list 写链（`roughnessMetallic = {r, m}`）/
  vkrenderer sizeof。`scene_data.cppm` 新增双钉：`kSceneDataDrawTableOffset == 524944`
  （shader 字面量）+ `kSceneDrawStride == sizeof(ObjectDraw)`。
- **顺手清注释残留**：`scene.slang:11-13` vkresources 过时指称改 scene_data.cppm +
  draw 表 assert 强度描述更新。
- **resize 语义微差留档**：旧 `ObjectDrawGpu` 有 NSDMI（baseColor 默认 1.0），生成类型
  无 → `resize()` 零初始化。无影响 = 每 entity 的 objectSlot 在渲染系统写链中全字段覆写，
  未写槽位无 reader（objectIndex 只来自存在的 entity）。
- **验收全过**：`just reflect` 5 管线 PASS（ObjectDraw size=48 fields=5 交叉校验）；
  forward 截图 = 基线 `10b00c78…`；deferred/offscreen 冒烟 0 error；`checks --strict` 0 警告。

### 10.2b 实施记录（2026-09-28）

- **条目契约 = 回调对 + 语义层（零布局镜像）**：`param_registry` 纯数据类型模块
  （Slider/Color3/Drag3 Entry + ParamPage），字段结构由回调两端各自引用源头类型
  （ECS 组件 / SceneEnv ctx），注册表本体零 GPU 布局副本（D1 硬约束）。
- **装配落点修正**：`buildDebugParamPages(entt::registry&)` 声明在 **ecs.cppm**、定义在
  ecs_impl（计划原文写"render_list 写入侧"——renderListSystem 只消费 ctx，装配页需要
  registry 全貌 + Light 组件可变 view，归 ecs 更顺）；imgui_layer 经 `setPageBuilder`
  回调注入，**不认识 ECS/registry**（分层与 llm_debug 回调先例同构）。
- **页内容**：「渲染」= exposure / env 强度 / env mip 范围（SceneEnv ctx）；「灯光」=
  每灯强度滑条 + 光色 + 方向 drag3（按 index 升序，index 0 主光附共享 ambient）。
  写回 = 改组件/ctx 本体，renderListSystem 下一帧消费（= 计划"下一帧"时序）。
- **渲染模式**：get 每帧跑（热重载/外部改动自动反映）→ widget → 编辑才 set；
  label 帧间稳定 = imgui id 稳定。
- **验收**：默认关 forward 截图 = 基线 `10b00c78…`；窗口 `imgui=true` 12s VVL 0 error
  （面板就绪 + TabBar 渲染）；`checks --strict` 0 警告。**视觉验收留给用户**
  （llm 截图在 UI 段之前，不含面板画面）。

### 10.2c 实施记录（2026-09-28）

- **材质页 = 选择器 + 动态条目**：`param_registry` 扩 `SliderIntEntry`（材质 id 选择）+
  `TextEntry`（只读提示）+ `DebugPanelState`（registry ctx 持久 UI 态——选择态不进
  每帧重建的条目）；页内容 = 选中材质 baseColor(rgb+α)/roughness/metallic/emissive/
  alphaCutoff + "共享该材质的实体数"提示（view\<MaterialRef\> 计数）。
- **写入通道 = `AssetTable::getMaterialMut(id)`**（新增可变访问）：renderListSystem 每帧
  读材质表写 ObjectDraw → 编辑下一帧生效；语义限制已注释在接口（值去重不重归并、
  无回滚、编辑仅本会话——重启回 config 值）。
- **验收**：默认关 forward 截图 = 基线 `10b00c78…`；窗口 `imgui=true` 12s VVL 0 error；
  `checks --strict` 0 警告。

### 10.3 实施记录（2026-09-28）

- **机制/语义分层**：`hot_reload.HotReloader` = 纯机制（登记 path+回调，poll 比 mtime，
  先更戳后回调 = 半写文件失败不重试风暴；回调异常吞掉 warn）；语义全在装配方
  `main.wireHotReload`（main:scene）。帧边界挂点 = `ConfigReloadHook`（ecs ctx）由
  renderSystem 帧首（eventSystem 后、systems/draw 前）调 `configReloadTick`——上一帧
  已 submit、本帧未录制，套用无撕裂。
- **门** = `diagnostics.config_reload`（默认 false = wireHotReload 直接返回，零行为
  变化）；监视源 = **源树** configs/（D7；编译期定义 `RGL_SOURCE_CONFIG_DIR` 注入，
  目录缺失 warn 降级关）。
- **T1 套用面**：scene.lua → SceneSpin.speed / SceneEnv.exposure / SceneEnv.intensity
  （entities/灯/资产不热载 = 重注册牵动 heap 槽，超 T1）；camera.lua → fov/near/far/
  fpsSpeed/灵敏度/键位（**姿态与 mode 不套**——相机运行中已漂移，覆盖=瞬移）；
  log_level.json → `RLLogger::reloadLevels`（新增：只改两 sink level，不重建 file sink
  ——truncate=true 重建会清日志）。
- **实测踩坑留档**：验证时"scene.lua 改了没反应"实为 **stdout 重定向全缓冲 + 测试
  中途把 console_level 切 warn 隐藏了 info 行**——热重载本身 touch 后 3ms 内套用；
  观察热重载日志别在测试中途调低 console_level。
- **验收**：默认关 forward 截图 = 基线 `10b00c78…`（0 hot_reload 日志）；窗口开
  config_reload 实测 camera/log_level/scene 三链套用日志 + 恢复；`checks --strict`
  0 警告（HotReloader::poll 与 renderSystem 认知复杂度告警各拆一 helper 消解）。

### 10.4 实施记录（2026-09-28）

- **模块**：`shader_hot_reload`（接口 = ShaderHotReloader + 私有 StageSource/Candidate，
  实现全在 `shader_hot_reload:impl`）。复用 10.3 `hot_reload.HotReloader` 做 .slang mtime
  watch（回调只置 dirty = 同帧多文件变更合并一次重载）；挂点复用 `ConfigReloadHook`
  （config_reload 已开 = 组合追加、未开 = 独占 emplace；reloader 所有权在闭包）。
  门 = `diagnostics.shader_reload`（独立于 config_reload）。
- **编译期注入三宏**（rgl/CMakeLists.txt）：`RGL_SLANGC_PATH`（同构建用 slangc）、
  `RGL_SLANG_FLAGS`（= SLANG_FLAGS 列表拼串，D11 单源）、`RGL_SOURCE_SHADER_DIR`（源树
  shaders/，同 D7 语义）；任一缺失/不可用 = init 降级关并 warn。temp SPV 目录 =
  tmp + hash(源目录)^chrono 熵，析构清。
- **两阶段重建**（管线表扩口）：`PipelineEntry` 增存 spec + 绑定语义（5 个 add* 构造点
  designated init）→ `buildReplacement(index, effect)` 用同参数新建管线（**旧管线仍活**）
  → 全部候选建完后 `waitForIdle` → DGC 两路刷句柄（IES `updateShaderPair` / 静态
  `updateEntryShader`，槽约定 2j=frag、2j+1=mesh；entry 0 同步 initialShaders 保
  VUID-11053；容量启动期固化 = 越界槽忽略不报错）→ `commitReplacement`（旧句柄此刻
  才销毁）。同槽 = pipelineIndex 不变 → 资产表/cull 序列表/heap 槽零改动。单条目
  build 抛异常 = 该条目保留旧、其余继续。**mesh LocalSize 变化由 ExecutionMode token
  拦住**（kTrianglesPerGroup 类契约改 = 重启）。
- **守卫基线** = init 逐条目读在役 SPV（spec 路径）建 hash + 签名；commit 后写回候选
  的新 hash/签名 = 下轮守卫判据是**最近一次已接受状态**（非启动快照，连续编辑可叠加）。
  编译失败 = 全量保留旧、等下次保存（slangc 报错输出吞进日志 warn 不刷屏）。
- **热重载暴露并修复存量 CMake 依赖缺口**：`rgl_shader()` 的 DEP 表按"手写 import 清单"
  维护，9+2 处漏登（model.mesh/model.frag 漏 scene、model_pbr_ibl/vegetation/light.frag
  漏 lights、cube.mesh/light.frag/tonemap 漏 scene 等）。后果实测：scene.slang 被 10.2a
  改动后 `model.frag.spv` **未重编**（build 里 09-13 旧产物，ObjectDraw 声明序与源码
  脱节；因 scalar layout offset 未变渲染仍对，故此前 BIT-IDENTICAL 没暴露）。已全部补登
  + 重编后 forward 截图仍 = 基线（内存布局等价实证）。教训：**DEP 表 = 人工同步债**，
  未来可考虑 slangc `-M` 自动生成。
- **v1 已知限制**（注释在接口头）：新文件不追登 watch（新增 .slang 需重启）；全量重编
  29 源 ≈5s 阻塞帧循环（slangc 0.19s/次，改哪个都要等全量）；DGC 容量外条目（sky/light
  等启动后增条目）update 越界忽略 = 其句柄不刷新（本机 RADV 静态路 entry 容量按构造期
  条目数固化，NVIDIA IES 路同理）。
- **验收**：默认关 forward 截图 = 基线 `10b00c78…`（0 hot_reload 日志）；开 = ①touch 无
  改 → 全量重编 → 字节无变全跳（"无接口兼容的可热换变更"）②tonemap 输出乘 0.5（同签名
  功能改）→ "已重建 1 条管线（pipelineIndex 不变，下一帧生效）"，复原同理 ③加
  `[[vk::binding(30)]]` 真采样 = 契约改 → "条目 313（tonemap）接口契约变化——保留旧
  管线" warn，程序继续跑；全程 VVL 0 error。`checks --strict` 0 警告（守卫解析器拆
  shader:impl 分区 + spvAbsorb/EMD 拆 helper；rlstd 补导出 sort/unordered_map/size_t/
  system）。

### 10.5 实施记录（2026-09-28）

- **id pass = 克隆管线重放**（非新 draw source）：`addPickingEntries` 把 model 族条目
  逐个克隆（mesh = 原 SPV、frag = `picking.frag`（写 `DrawArgs.objectIndex+1` 进
  r32ui）、bindings 原样 = 贴图映射空转无害），remap 表反查，非 model 条目（cube 等）
  跳过。执行器 `executePassDraws` 第〇分支逐 opaque 段 draw 用克隆条目重放——**不走
  cull/DGC**（遮挡由 depth 测试保证，与剔除无关），heap 全 bindless 已就位，per-draw
  只差 push 4B。id 值 = objectSlot+1（0 = 未命中，attachment clear 0）。
- **图装饰 = 装配层编译期门控**（D10）：`decoratePickingPass(desc)` 在
  `compileGraphOrThrow` 内 `diagnostics.picking` 时调用——object_id r32_uint offscreen
  附件 + picking pass 插首个带 depth 的 opaque pass 之后（depth load **只测不写**：
  StateKit picking 段 `depthWrite=false`，不污染 transparent 段；clear 必须显式全零 =
  VkClearValue union 下 r32ui 会拿 float 默认值 reinterpret 成垃圾）。
- **回读三步曲**（`picking` 模块，零 entt）：frame_capture 先例——request（坐标）→
  record（帧末 capturer 之后录 1-texel copy，CAO→TS 并保持，下帧 pass barrier 自动衔接）
  → collect（等本帧 fence 读 u32，raw−1 = objectSlot）。
- **输入与反查**（函数桥零依赖）：eventSystem 左键按下（已过 InputFilter = imgui 控件区
  点击天然吞掉）→ `PickingHook.requestPick`（registry ctx，main wirePicking 装配）；
  `pickingTick` 帧边界消费 take() → 扫 `view<MeshRef>` 反查 entity（点击触发 O(n) 一次，
  不平行维护映射表）→ `SelectedObject` ctx → 面板"选中"页（材质五参数控件
  `appendMaterialControls` helper 与"材质"页共用，10.2c 抽取）。
- **实测踩坑（根因级）**：picking pass 首版回读恒 0——RenderState 默认
  `depthCompareOp=eLess`，而 picking 重绘与 scene depth **同几何等值** → 全片元被深度
  拒掉；修 = picking 段 `eLessOrEqual`（skybox z=1 同先例）。定位法 = frag 写常量 7
  绕 push/接口 + 临时关 depthTest 二分。
- **窗口鼠标注入不可得**（本环境）：SDL x11 后端在 XWayland 下卡死（picking=false 亦
  复现 = 与本批无关）、Xvfb 无 DRI（surface 不支持）、uinput 无窗口定位手段 → 加
  `diagnostics.picking_probe = {frame,x,y}` 配置化探针做 **headless 确定性验证**
  （D12 翻案实录 #1/#2：探针一次启动只发一次 + 摄像机无编程通道）。
- **D12（RPC 通道）= 延后**：用户动机补录进 §2（摄像机移动/点选/改 shader 参数的
  HTTP POST 驱动）；本轮不实现，翻案条件 = ≥2 次绕路实录。
- **v1 已知限制**：miss 不清空上次选中（take() 契约 = nullopt 兼"无新结果/未命中"）；
  probe 只触发一次（扫点位需重启）；picking pass 开启态 = opaque 段每帧双份录制（CPU
  记账：本机 CPU-bound，开点选时帧录制开销 ≈ ×1.3，调试功能可接受）。
- **验收**：默认关 forward 截图 = 基线 `10b00c78…`（0 picking 日志）；开 = probe
  (900,600) → raw 1495 → objectSlot 1494 → entity 1495（bistro 桌面命中，反查日志
  齐全）、probe (640,120) 天空 → raw 0 miss 保持选中；全程 VVL 0 error；
  `checks --strict` 0 警告。面板"选中"页视觉走查留用户（窗口注入不可得）。

### 10.6 实施记录（2026-09-28）

- **语义层 = 第 7 条反射管线（模板形态，同 features 管线）**：`rgl/param_semantics.json`
  （label/range/speed 唯一权威，13 条控件元数据 = 8 sliders + 4 colors + 1 drag3）→
  `tools/templates/param_semantics_cppm.inja` → `reflection_tool render` →
  `generated/param_semantics_generated.cppm`（`RGL::semantics::kXxx` constexpr 表）。
  管线注册在 `cmake/RglReflection.cmake`（SHA1 护栏 REFLECTION_PAIRS 自动纳入 json+模板）。
- **D1 边界落定**：**语义是数据、通道是代码**——schema 只装反射/配置给得了的静态语义
  （label/min/max/speed）；get/set 写通道（闭包捕获 ctx/组件/资产指针）留消费侧。灯页
  逐实例 label = meta 前缀 + index tag 运行时拼接（json 存"强度 L"，面板显"强度 L3"）。
- **与原计划偏差（记录）**：**默认值/单位未入表**——当前面板无消费场景（无"重置默认"
  按钮、控件不带单位后缀），入表即死数据；将来做重置功能时再扩 json 字段即可。
- **附带拆模块**：切薄后 `ecs_impl.cppm` 代码行 504 > 500 红线 → 面板页构建拆出独立
  模块 `param_pages`(+`param_pages:impl`)（与帧循环 system 职责本不同源）。依赖方向：
  param_pages → ecs/scene_context（认识 ECS 搭控件）；imgui_layer 仍零认识 ECS
  （builder 回调契约不变）；ecs 不 import param_pages（无环）。踩坑：模板生成物
  `inline constexpr` 常量漏 `export`（struct 导了实例没导，症状 = "must be imported
  from module before it is required"，同 rlstd "完整可用面"教训）。
- **验收**：`just reflect` 产物 13 常量全导出；默认关 forward 截图 = 基线
  `10b00c78…`；窗口 + imgui 面板（pageBuilder 每帧跑）25s 无崩溃 0 error；
  `checks --strict` 0 警告（style + doc）。

### 10.7 实施记录（2026-09-28）

**起因**：用户报"phase10 点选似乎没生效"。排查结论 = **点选链路一直是通的**（headless
探针 + 窗口合成左键两轮实测均命中：`回读 raw id → objectSlot → entity`），真正原因是
① `diagnostics.picking` 默认关；② **选中后画面零视觉反馈**——唯一反馈是面板"选中"页
里的一行文字，而 Tab 默认停在"渲染"页，用户看不到。

- **高亮 frag 走 SPV 编译管线，不新增反射管线**：`rgl/shaders/highlight.frag.slang` 经
  `rgl_shader(highlight.frag HIGHLIGHT_FRAG_SPV model material scene)` 登记（同
  `picking.frag` 先例）。它只 import 既有契约（model/material/scene）、**不定义新
  struct**，故 7 条反射管线与 `generated/` 产物零变化（实测 `just reflect` 后
  `git status` 无生成物 diff）。用户"所有 shader 都要走反射"的要求 = 每个 shader 都经
  slangc 编译进 SPV，这一条满足。
- **克隆机制泛化**：`addPickingEntries` → `addCloneEntries(ClonePurpose, fragSpv)`，
  `pickingRemap` → `cloneRemap[用途][条目]`（`ClonePurpose{picking, highlight, kCount}`）。
  新增用途只需加枚举值 + 一次调用，不动既有表与下标语义。
- **高亮 pass 不新增附件**：写**既有**场景色附件（`loadOp=load` 保留成片，alpha 混合叠加）
  + 既有 depth（`loadOp=load`，`state_kit.highlightState` 关 depthWrite）。
  **插入点 = 首个「采样色附件的消费 pass」之前**（forward/deferred = tonemap 前，offscreen =
  composite blit 前），附件名由该 pass 的 `sampleRead` 输入反查（不硬编码，三变体自动跟随）。
  两个踩坑：① 最初按"图末"插 → 落在 tonemap **之后**，此时色附件已无人采样，高亮画了
  也白画（图能编译过、pass 也在跑，纯视觉无效）；② 借 depth 前须把上一个写 depth 的
  pass 的 `storeOp` 从 `dontCare` 改 `store`（forward 的 transparent 段按「depth 后续
  无人用」声明 dontCare），否则 compile 报 "written with DontCare store but used later"。
- **按材质过滤（方案 A，用户拍板）**：`RenderListState.materialBySlot`（按 objectSlot 存
  材质 id，**纯 CPU 侧不进 scene data**）+ `FrameDrawList{hasSelection, selectedMaterial}`。
  执行器 `replayClonedPass` 逐 draw 读 `pushData` 里的 `objectIndex` 查表比对（同
  `cull_driver` 的 reinterpret 手法），命中才录。**刻意不进 GPU 契约**——避免为
  「哪个物体被选中」新增 per-object 字节字段、触发 `ObjectDraw` 布局变更与 reflect
  重生成。选中范围 = 同 `MaterialRef.material` 的**全部实体**（材质是共享资产，ECS 规范 5）。
  执行器保持零 entt（选中态经 `FrameDrawList` 单通道传递，ECS 规范 4）。
- **`SelectedObject` 从 `ecs.cppm` 迁到 `scene_context.cppm`**：`renderListSystem`（机制层）
  每帧要读它填高亮通道，而 `ecs_impl` 已 import `render_list`——留在 ecs 会造成
  `ecs ↔ render_list` 循环 import。`scene_context` 是叶子值类型层（`Light`/`MeshRef`/
  `MaterialRef` 都在那），本就是它的位置。同时加 `materialId` 字段。
- **面板跳页**：`ParamPage.selectOnPick`（构建器声明跳转目标）+ `DebugPanelState.
  pendingSelectJump`（一次性请求位）。`imgui_layer` 传 `ImGuiTabItemFlags_SetSelected`
  触发切换，**不认页名字符串**（跳转目标由构建器声明，渲染层零业务知识）。只在**真的
  命中实体**时置位——miss（点空处）不发跳转，否则会因「点空」把用户从灯光页弹到空的
  "选中"页。消费即清零，不会反复弹回。
- **Esc 取消选中**：`eventSystem` 轮询循环内 `handleEscapeDeselect` 置
  `SelectedObject.entity = entt::null` 即整链收口（高亮 pass 见 `hasSelection=false`
  走零 draw；面板回落未选中提示）。与 picking 服务无关，纯 CPU 复位。
  **位置硬约束 = 必须在 `InputFilter` 判定之前**：imgui 的 `WantCaptureKeyboard` 是
  粘性状态（点过面板后恒为真，见 `git:调试历史` "面板吞键盘"事故），放到 filter
  之后 Esc 会被 imgui 吞掉。实现用 `if (!isEscape && filter && ...)` 单条件短路
  （不二次调用有副作用的 helper），并把 ESC 判定抽 helper 以压低 `eventSystem`
  认知复杂度（style-check 15 上限）。
- **两个实测踩坑（高亮不可见）**：
  1. **混合语义写错**：首版把 `rim` 同时乘进 rgb 与 a，而混合是标准
     `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`（`shader.cppm` colorBlend），实际贡献退化为
     `rim²·intensity`——power=4 时除轮廓线外 rim≈0.05、rim²≈0.0025，实测点亮 1.8% 像素
     但通道最大差仅 17/255，肉眼不可见。改 `rgb = 纯高亮色`、`a = saturate(rim)·intensity`
     （贡献对 rim 线性）。
  2. **纯菲涅尔只勾轮廓**：建筑墙面这类**正对相机的大平面** NdotV≈1 → rim≈0，点中后
     正面完全无变化。加 `HIGHLIGHT_BASE_WEIGHT=0.35` 恒定基色权重（"点中即染色"），
     轮廓光只作强化。修正后最大通道差 46 → **112/255**，肉眼清晰可见。
- **验收**：`picking=false` **BIT-IDENTICAL**（`git stash` 基线 vs 改动同会话双构建，
  截图逐字节相同）；开 = 差异 1.82% 像素、最大通道差 112、分布集中在点选点所在物体
  （`match=1`，bistro 132 材质基本一对一）；headless 探针 + 窗口合成左键两轮均命中；
  跳页链路日志验证 `probe 第 5 帧 → objectSlot 711 → entity 712 → 跳页请求被构建器消费`；
  `checks --strict` 0 警告。
- **补修（2026-09-28，用户实测后）**：用户报「ESC 无效 + 跳页无效」，逐条 grep 复核 11
  个改动点后确认**两处编辑静默丢失**（编译器不报错——被替换的旧代码本就合法）：
  `imgui_layer_impl` 的 `BeginTabItem` 未传 `SetSelected` flags、ESC 分支根本没写进
  `eventSystem`。补齐后：窗口模式注入 Esc → 日志打出 `picking: Esc 取消选中`；
  `picking=false` 仍 BIT-IDENTICAL；`checks --strict` 0 警告。详见
  `git:调试历史` 2026-09-28 补修条目。
- **未做/遗留**：① 面板"选中"页**视觉走查仍未做**——本机窗口截图只能抓到前台窗口
  （`spectacle -a` 反复抓到终端/浏览器），RGL 窗口无法置前，llm_debug 截图又在 imgui
  绘制**之前**（AGENTS 已知限制）。跳页逻辑已用日志验证，但"看起来切过去了"需用户窗口
  确认。② 阴影仍为 Phase 11。③ 高亮在 offscreen 变体未实测。④ 开 `picking` 时克隆条目
  312 → 624（本机实测两条 `clone[...]` 日志），启动开销未专门计时——延后优化（见
  `git:调试历史` 2026-09-28 条目）。

## 10. 验收红线（汇总）

1. **默认关 = BIT-IDENTICAL** 截图回归 + `checks --strict` 0 警告（每批必过）；
2. **headless no-op**：`imgui`/`picking` 无窗口天然关；`config_reload` headless 可用；
3. **单一事实来源**：注册表不镜像布局（字段只引用反射生成物/派生常量）；
   运行时不做 slang 反射扩产（`next_roadmap.md:17` 红线）；
4. 改 shader 契约（10.2a、10.4 涉及）必走 skill `rgl-shader-contract`：`just reflect` +
   生成物随改动提交；`SLANG_FLAGS` 运行时用注入派生串（D11）；
5. 每批一提交（中文提交信息），提交前 `checks --strict`；
6. **帧边界纪律**：config/SPV 换入只在帧边界套用，绝不在录制中改当前 CB；
7. 开销记账：imgui/id pass 开启态跑 `frame_stats`/profiler 前后对比，控制在 Phase 9 GPU
   余量 3.1ms/帧 + CPU 增量合理内；性能测量**单实例串行**（AGENTS 测量纪律）；
8. id pass 默认关 ⇒ render graph 与现三变体逐位同（D10）。

## 11. 否决项（记录在案，防止回头重议）

- 运行时反射扩产 / 注册表镜像布局（红线 3 + D1）；
- L2 深度：binding 集变化热换、全契约热换（D2 选 L1）；
- CPU 射线选点 v1（D3 选 id pass；射线方案留作 id pass 失败时的降级备胎）；
- 复用 `kGBufferAttachments` 空通道当 id 通道（破 diff0，D10）；
- 实体树浏览器/日志窗口（D5）；
- 监听 build 副本 `build/rgl/configs/`（D7）；
- 含 UI 双截图基线（因 D6 顺序 UI 不进截图，无需双基线，D8）。

## 12. 文档回写清单（实施时逐项做）

- ~~`next_roadmap.md` 阴影段 10 → 11 顺移~~ **✅ 已完成（2026-09-27 顺移提交）**：阴影段改号 +
  新 Phase 10 块插入 + :241 imgui 远期候选标注升格 + 头注消歧；AGENTS 基线行/文档表、
  技术概念 :106/:268/:272、脚本长期规划 P1/P2/§6 指称同步改毕；
- 实施期回写：`phase10_plan.md` 各批实施记录、`git:调试历史` 新条目（只增）、完成后本文件转
  `archive/`，`AGENTS.md` 已知问题/模块表回写。
