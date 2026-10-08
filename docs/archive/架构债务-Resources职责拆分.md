# 架构债务 · Resources 职责拆分（2026-09-16）

> **状态**（2026-09-19）：已完成。`Resources` 已瘦身为 allocator / depth / two descriptor heaps /
> `SlotSegment` 三段分配器；SceneData、RenderTargetCache、Environment、SamplerLibrary、AssetTable
> 已拆出，allocSlot 区段化完成。`style_check` / `doc_check` 的 libclang crash 另记在
> `git:调试历史` 2026-09-19 续 10。

> 数据来源：教程仓库 `modern_vulkan_tutorial/explore/data/model.json`（`tools/explore/extract.py`
> 五源抽取，clang 文本 AST import 图 + 符号级 uses 边），辅以 `vkresources.cppm` /
> `vkresources_impl.cppm` 源码通读。本文是"我过去修"的依据文档：先给账，再给拆法与验收。
>
> 前作：`架构债务-依赖方向审计.md`（2026-09-08）。债务 1/2 修复时把内容资产从
> `Resources` 构造期剥了出去（allocSlot 领取制），但**领取窗口本身仍全部寄居在
> `Resources` 类上**——本文处理这笔续账：不新增反向 import，而是给 `Resources` 减重。

## 总评：度数第一的枢纽，八种角色一肩挑

explore 全图 723 节点中，`type:vk_resources.Resources` **度数 83，全图第一**
（对比：`DeviceContext` 61、`Renderer` 29）；37 个不同函数、跨 14 个模块 import
`vk_resources`。入边消费者横跨三层：

- **内容上传层**：`builtin_geometry` / `model_importer` / `texture_pool` /
  `texture_upload`（含 `:cubemap` 子模块，本身已是 29 度的次级枢纽）
- **每帧渲染层**：`Renderer` / `render_list` / `frame_session` / `frame_capture` /
  `gpu_cull` / `cull_driver` / `frame_draw_list`
- **启动装配层**：`main` / `main:scene` / `shader_registry`

度数高本身不是罪（allocator/heap 天然人见人用），罪在**它同时持有八种互不相干的
角色**，其中至少四种携带内容/pass 级知识，且一大票 `ensure*` 携带脆弱的**全局时序
契约**（"必须在全部几何槽分配完 + 白纹理首槽断言之后调用"散落在至少 5 个方法注释里）。

## 职责清单（按去留分类）

### 该留：资源内核（角色 1+2）

| 成员/方法 | 说明 |
|---|---|
| `allocator`（VmaAllocatorRAII + BDA flag） | 全项目 buffer/image 创建入口 |
| `descriptorHeap` / `samplerHeap` + `create*Heap` | VK_EXT_descriptor_heap 两块 heap |
| `allocSlot(SlotSegment, name)` / `reservedSlots` / `getReservedRange*` | 三段游标 slot 分配器（现状；原文为单调游标） |
| `depthImage` / `depthImageView` / `createDepthImage` / `recreateDepthImage` / `buildDepthView` | 深度附件生命周期 |

这一层薄、稳定、认识它的人多但认识的内容少——合理枢纽。

### 该搬：六块寄生职责

| # | 职责 | 成员/方法 | 病症 |
|---|---|---|---|
| A | SceneData 字节布局契约 | `kSceneData*` 全部 14 个常量 + `ensureSceneData` + `sceneDataBuffer`/`sceneDataSlot` | shader 布局真相源（与 scene.slang / lights.slang / lights_schema.lua 三方 static_assert 同步），性质同 `lights_types_generated`/`scene_forward_types_generated`，与"资源管理"无关 |
| B | 环境/IBL 单例 | `envAsset` / `envIrradianceSH` / `ensureSkySampler` / `ensureEnvPrefilterReady` / `envPrefilter` | env 是**场景级内容资产**；sky sampler 的 maxLod 烘焙是 env 上传期知识；`vk_resources -> env_prefilter` 是反向 import |
| C | 具体 pass 的 RT 知识 | `ensureDeferredGBufferSlots`（认识 kGBufferAttachments 5 附件名/格式）/ `ensureSceneHdrSlot`（认识 kSceneHdrName/kSceneHdrFormat） | `vk_resources -> render_graph` import 的存在理由就这两个方法——依赖方向倒置：资源层认识了渲染图 |
| D | sampler 内容策略 | `ensureSharedSampler`（REPEAT wrap，注释自曝理由 = "bistro 49.5% primitive UV 越界"）+ `samplerCreateInfo`/`sharedSamplerSlot` 稳定存储 | 演示资产的 UV 分布知识泄漏进内核（同债务 1 的 cube 性质） |
| E | mip/prefilter 基建 slot 供给 | `ensureMipGenReady` / mipGenBound / `mipGen` 成员 | 领槽推进全局游标的"中介"角色；`vk_resources -> mip_gen` 反向 import |
| F | AssetTable 宿主 | `assetTable` 成员 + `getAssetTable` + `export import asset_table` | 2026-09-02 已拆出模块但实例仍寄居；ECS 单一数据源住在资源层 |

## 拆法（五个独立可编译的步骤，按伤筋动骨排序）

### 步骤 1（最机械）：`scene_data.cppm` 承接 A

- 新建 `rgl/scene_data.cppm`：14 个 `kSceneData*` 常量 + 全部 static_assert +
  SceneData 头注释整体搬入；`import lights_types_generated` 等 generated 模块随迁。
- `Resources::ensureSceneData` 改名语义为"向内核领 buffer+slot"，或整体搬进
  `SceneDataService { ResourceKernel&, VmaBuffer, uint32_t slot }`（推荐后者，
  `sceneDataBuffer`/`sceneDataSlot`/`sceneDataReady` 三成员一并走）。
- 消费方（`render_list_impl`、`shader_registry_impl`、`vkrenderer_impl`、scene.slang
  字面量注释里的"同值"引用）**常量名不变**，只改 import——机械替换，行为零变化。
- 验收：`vkresources.cppm` 不再 import `lights_types_generated`；启动日志/截图 diff 为零。

### 步骤 2：`RenderTargetCache` 承接 C 的通用部分

- 新建 `rgl/render_target_cache.cppm`：`RtAttachment` 结构 + `ensureRtAttachment` /
  `findRtAttachment` / `rtHeapSlot` / `rtAttachments` map 整体搬入；构造吃
  `VmaAllocatorRAII& + DeviceContext& + descriptorHeap& + allocSlot 回调（或 kernel 引用）`。
- `ensureDeferredGBufferSlots` / `ensureSceneHdrSlot` **不进缓存**——它们是
  "deferred 图的装配函数"，搬去 `render_graph`（或其 impl partition，契约表
  kGBufferAttachments 本就在那里，搬完后 `kGBufferAttachments` 引用不出 render_graph 模块），
  由 `main_scene` 装配期调用。
- 验收：**删除 `vk_resources -> render_graph` import 边**（impl 侧的 `import render_graph`
  随两个方法迁走自然消失）；explore 重跑抽取后该边不复存在。

### 步骤 3：`Environment` 服务承接 B+E（env 部分）

- 新建 `rgl/environment.cppm`：`envAsset` / `envIrradianceSH` / sky sampler（含
  create info 稳定存储与 `ensureSkySampler`）/ `EnvPrefilter` 实例 + 其 slot 供给
  （`ensureEnvPrefilterReady` 的逻辑原样搬，allocSlot 经 kernel 领）。
- 消费者改名最小：`texture_upload:cubemap` 本来就是 env 上传的唯一生产者（度 29 的
  模块，它与 env 同语境）；`main_scene` 装配点、`shader_registry_impl` 烘焙 binding 4/5
  处、`render_list_impl` 读 irradianceSH 处改取 `Environment&`。
- 验收：`vk_resources` 不再 import `env_prefilter`；`getEnvTexture`/`setEnvIrradianceSH`
  从 Resources 消失。

### 步骤 4：`SamplerLibrary` 承接 D+E（mip 部分）

- 新建 `rgl/sampler_library.cppm`：共享 sampler（含 REPEAT 决策及其 bistro 注释）+
  create info 稳定存储；sky sampler 若步骤 3 后仍想归拢，可放这里由 Environment 引。
- `MipGen` 实例 + `ensureMipGenReady` 的 slot 供给逻辑同性质，搬去 `mip_gen` 侧的
  装配函数（吃 kernel 引用），或并入 SamplerLibrary 所在的内容服务层。
- 验收：`vk_resources` 不再 import `mip_gen`；kernel 只剩"写描述符进 heap 槽"这类
  内容中性操作。

### 步骤 5（根治时序契约，最后做）：allocSlot 区段化

现状 = 全局单调游标 + 注释约定顺序，任何新上传点都要读别人的注释才敢领槽。改法：

- kernel 一次性把用户槽区划成命名区段：`geometry / texture / runtime`（scene data、
  RT 采样槽、mipgen/envpf 槽各归 runtime 或各自区段），每区段独立游标 + 独立容量上限。
- "白纹理首槽 = 几何段尾 +1"这类契约降级为**纹理区段内断言**，跨区段顺序不再有意义；
  时序注释可以删掉一大片。
- heap 容量 = reserved + Σ 各区段容量（各区段初值按现状 bistro 用量 + 裕量定，
  总数仍是 kUserSlotMargin 量级，4096 槽 MB 级开销不变）。
- 验收：`ensureMipGenReady`/`ensureDeferredGBufferSlots` 等注释中的"必须在……之后
  调用"契约条款全部删除；`expectedGeometrySlot` 断言改为区段内成立。

### 附：AssetTable（F）的去向

不单列步骤：所有权顺势搬去内容层（`main_scene` 装配期构造，或 gltf/model_importer
语境的服务对象），Resources 连 `getAssetTable` 一起摘除。放步骤 3/4 之间顺手做即可。
`export import asset_table` 这个"接口透传"补丁随 `getAssetTable` 消失——消费者直接
`import asset_table`。

## 拆完的账（预期）

- `Resources`（或改名 `ResourceKernel`）成员从 ~20 个瘦到 6 个左右：三个引用 +
  allocator + 深度图两件套 + 两 heap + 区段游标。认识 light/sky/env/G-buffer/tonemap/
  bistro 任何一词的代码全部离场。
- import 反向边消失三条：`vk_resources -> mip_gen` / `-> env_prefilter` / `-> render_graph`
  （后者在债务审计文档里本就被记过一笔）。
- explore 图预期：`type:vk_resources.Resources` 度数大幅下降（kernel 只被"要内存/槽"
  的人用），新枢纽变成各自语境内的 `Environment` / `RenderTargetCache`——度数转移
  到语义正确的地方。复现：改完重跑 `extract.py + audit.py + gen.py`，对比度数表。

## 总验收（每步之后都跑）

1. RGL 构建通过 + 启动 bistro 场景截图与拆前逐像素一致（`heap_texture_spike` 流程 /
   教程仓库 `make_shots.sh` 的引擎运行时）。
2. descriptor heap 槽号日志（allocSlot 的 debug 行）：步骤 1–4 应完全不变；步骤 5
   允许槽号重排，但白纹理契约断言、light 管线 baked 槽、tonemap binding 4 全部命中。
3. `tools/explore/extract.py && audit.py` 全绿，度数表对比记录进本文"修复记录"节。

## 修复记录（2026-09-19）

- 步骤 1–5 + F 已落地：`scene_data.cppm`、`render_target_cache.cppm`、
  `render_graph_rtslot.cppm`、`environment.cppm`、`sampler_library.cppm` 已拆出；
  `AssetTable` 实例改由 main 装配层持有；`Resources` 不再持有内容级 `ensure*` /
  `getAssetTable` / `export import asset_table`。
- `Resources` 当前成员收敛为三个引用 + allocator + depth image/view + resource/sampler heap +
  `SlotSegment` 三段游标；`vk_resources` 不再 import `mip_gen` / `env_prefilter` /
  `render_graph` / `asset_table` / `lights_types_generated`。
- allocSlot 区段化：`SlotSegment::Geometry / Texture / Runtime` 三段独立容量，
  白纹理契约从"几何段尾动态邻接"改为"纹理段首槽"；runtime 辅助槽不再插入几何/纹理契约段。
- 本次验证：`just build` 通过（libclang style_check crash 另记）；headless forward /
  deferred 冒烟无 `[error]`。未跑 BIT-IDENTICAL 截图回归；步骤 5 本身允许 slot 号重排。
