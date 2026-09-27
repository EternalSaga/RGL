# Phase 10 实施计划：调试交互（配置热重载 + IMGUI 参数面板 + shader 热重载 + 点选取参）

> **状态**：现行——**批 10.1 imgui 骨架已落地（2026-09-28）**，其余批次未开工。
> 关键决策 D1–D5 已由用户拍板（见 §2），D6–D11 为主 agent 技术决策（可否决）。
> 批次 10.1–10.6 见 §9。
> 依据 = 2026-09-27 只读调研（worker 子 agent A–F 六节 + 主 agent 逐项复核）+ 方向讨论。
> **阶段号消歧**：本 Phase 10 ≠ 2026-09-10 的 Renderer 拆分批（当时批次被旧口径称
> "Phase 10 批次 0"，散见约 19 处历史代码注释——**历史注释一律不改**，仅本文件与
> roadmap 新增消歧行）。原计划中的阴影任务整体顺移为 **Phase 11**（`next_roadmap.md`
> 阴影段 :212-239 改号，见 §12）。
> 完成后本文件转 `archive/`，进度回写 next_roadmap。

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
watch rgl/shaders/*.slang（mtime；导入闭包按 import 语句递归求，v1 可全量重编 21 条）
  → fork slangc（注入 SLANG_FLAGS_STR D11 + slangc 路径 + 输出临时路径）
  → -reflection-json 与在役 pipeline 的绑定/顶点/push 布局比对（守卫基线 = 运行时已装载状态）
  ├─ 同签名 → 同槽重建 RenderPipeline（pipelineIndex 不变 → 资产表/cull 静态表零改动）
  │           + DGC execution set 重建（VkShaderEXT 换新；重建失败 → 降级读回重放，不中断）
  │           → 下一帧生效
  └─ 不一致 / 编译失败 → 日志 fail-fast + 保留旧 SPV 继续跑（提示"契约变化请重启"）
```

**前置精查（10.4 开工前，只读）**：`vk_indirect_commands` 的 VkShaderEXT handle 生命周期
依赖（execution set 容量/条目稳定性、swap 时序）——worker 二期只读调研。

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
| **10.2c** 材质控件 | `AssetTable` setter + 材质条目（吃 10.2a 生成物）+ 共享实体数提示 | `asset_table.cppm`、`param_registry` | 同上 + 共享提示正确 |
| **10.3** 配置热重载 | §5：`hot_reload` 模块 + 帧边界换入基建 + T1 清单；（T2 可选） | 新 `.cppm`、各 `loadXxxConfig` 调用点、CMake 注入源目录 | 默认关 diff0；开 = 改源 `configs/` 下一帧生效且 build 副本不被污染；缺源目录降级关 |
| **10.4** shader 热重载 L1 | §6：前置精查（只读）→ watch + fork slangc + 签名守卫 + 同槽重建 + DGC 降级 | 新 `.cppm`、`shader.cppm` 重建入口、`vk_indirect_commands` 重建/降级、编译期注入 flags | 默认关 diff0；开 = 改注释重载无视觉变化、改逻辑下一帧生效、**改契约被拦且旧管线继续跑** |
| **10.5** 点选取参 | §7：点击事件 + id pass 进图 + 1 texel 回读 + objectSlot 反查 + 面板实体绑定 | 图配置/id shader（新 `rgl_shader` 条目）、回读通路、`ecs` 选中 context、面板区 | 默认关 diff0；开 = 点击命中正确（含遮挡层序）、面板显示并可调该实体参数 |
| **10.6**（可选，D4 视情况） | 反射语义扩产：range/默认值/单位入 schema → 生成语义层，注册表薄化 | schema/生成器 | 10.2b/c 行为不变（仅数据源换薄） |

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
- 实施期回写：`phase10_plan.md` 各批实施记录、`调试历史.md` 新条目（只增）、完成后本文件转
  `archive/`，`AGENTS.md` 已知问题/模块表回写。
