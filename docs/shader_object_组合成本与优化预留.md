# Shader object 组合成本：条目爆炸根因与「砍掉创建期烘焙」定案

> **状态**：现行（2026-09-28 立；**2026-10-14 改写：砍烘焙已定案，定案 + 开工清单在 §5**）。
> 记录管线表条目数的生成机制、实测成本账本，以及 2026-10-14 用户拍板的定案形态与次日开工清单。
> §2/§3 的机制与账本仍然有效；§5 原"优化预留（未实施）"部分已按定案改写，原缓存路线作废
> （理由见 §5.3）。数据基准 = bistro_exterior 场景 + §7 锚点（每次启动由 `logStartupBaseline`
> 自动重打，不必翻旧日志）。

## §1 结论速览

### 1.0 定案（2026-10-14，用户拍板）：砍掉创建期烘焙

**动作**：模型条目的五个 **per-draw** binding——0 顶点 / 1 base / 6 normal / 7 orm / 8 emissive——
从 `VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT` 改为
`VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_EXT`。索引不再烘进 shader object 身份，
改为住在**现有 per-object 表**（`objectDraws`，binding 26）里；push data 只多一个 **device address**
（4B → 16B：`@0` objectIndex 不动、`@8` 放地址）。**五个 mapping 共用同一个 pushOffset（同一个
地址），各按自己的 `addressOffset` 取自己那一项**——一条 12 字节增量喂满五个绑定。
sampler 侧（binding 2/5/13/16，sampler heap）**不动**：全场景共享单值，无 per-draw 变化。

| 项 | 现在（2026-10-04 基线） | 定案后（预期） |
|---|---|---|
| registry 条目 | 376 | **3–7（= spec 数）** |
| VkShaderEXT 句柄 | 752 | ~14 |
| DGC 执行集槽 | 624 | ~14 |
| picking 克隆条目 | 312 | 3 |
| binding mapping 条数 | 4358 | ~30 |
| 启动创建耗时 | 1.13s（3.01 ms/条） | ~20 ms |
| push data | 4B | 16B（+12B，只一个地址） |
| 每帧 draw 数 | 现状（与条目数无关） | **不变**（本刀减的是对象身份，不减 draw） |

🔴 **负知识（别再去找第三条路）**：**任何 per-draw 解析源都从 push data 取值，"push data 零增长"
在这个 API 里不存在。** 本地 `Vulkan-Docs/chapters/descriptorheaps.adoc` 逐个源核过的取值处：

- constant offset = 创建期常量（= 本刀砍掉的对象）；
- push index = `((uint32_t*)pPushData)[pushOffset/4]`，且索引 20bit + sampler 12bit 打包
  （`& 0xFFFFF` / `>> 20 & 0xFFF`）；
- **indirect index = `((VkDeviceAddress*)pPushData)[pushOffset/8]`** —— 索引在 buffer，但
  **buffer 的地址在 push data**（VUID-11260 要求 8 对齐、11261 ≤ `maxPushDataSize − 8`、
  11262 `addressOffset` 4 对齐计数）；
- indirect index **array** 的 `arrayIndex` 项是 **shader 绑定数组下标**
  （`shaderIndex = (Binding − firstBinding) + arrayIndex`），**不是 draw index** ⟹ 全部 11 种
  source 里**没有"按 draw 序号取 buffer"这一档**。

⟹ 选 indirect index 而非 push index：两者都要 push data 增长，前者 +12B（一个地址）且
**五个 binding 复用它**，后者 +16B（四个索引）且带 20bit 打包限制。
**什么实验会推翻本段**：本地 spec 添了"按 draw index 取索引"的新 source；或 indirect index
的 `pushOffset` 出现可省略模式（二者当前都不存在）。

⚠ **RADV 上 `INDIRECT_INDEX` 零先例**：仓内 push-index 先例只在 **storage image** 上跑过
（`mip_gen` / `env_prefilter`，每次启动都跑）；sampled image + indirect index 本机没跑过 ⟹
**开工清单 ① = 最小 indirect-index spike 当第一个 commit**，`just regress floor` 逐位验收。
"机理上必然可行"不构成跳过它的理由——没跑过的东西必须当天上午兑现成数据，而不是赌下午。

> **📊 基线已重测（2026-10-04，批 9.5 顺带；下表 312/626/1252/2.3s 是 2026-09-28 那次
> forward + picking 临时开的运行，已过期）**：现行配置（deferred + 4 级阴影 + IZB + AO）实测
> **376 条目 / 376 pipelines / 752 句柄 / 4358 mappings / 纯 `vkCreateShadersEXT` 1.13s
> （3.01 ms/条）/ 30 顶点块**。基线现由 `logStartupBaseline()` **每次启动自动打日志**
> （`shader_object baseline: registryEntries=… handles=… mappings=… createTotal=…`），
> 不必再靠翻旧日志。⚠ **两个数不可直接比**：旧 2.3s 是**日志时间戳差（含 SPV 读盘）**，
> 新 1.13s 是纯创建调用；条目 312→376 的增量来自此后新增的 TAA(binding 4-8) 与
> EVSSM(15/16) 相关条目。**"条目爆炸"这条债的痛点（启动时间）实测比文档记载的轻**——
> 但 376 条目 / 752 句柄 / 752 DGC 槽的结构性成本仍在。⚠ **2026-10-14 起"根因未变"这句只到
> 定案前有效**：根因（§4）**已定案砍掉**（§1.0 / §5），本表读数是**砍之前**的基线，
> 也是开工 ② 的对照起点。全文 → `git:调试历史` 2026-10-04、`docs/还债总账.md` 债-04/债-23。

当前场景（`models/bistro_exterior.glb`，1591 primitive / 132 材质 / 395 图片）的管线表：

| 项 | 数量 | 来源 |
|---|---|---|
| 模型条目 | **312** = 264 pbr + 25 vegetation + 23 light | 六元组去重（§2.4） |
| picking 克隆 | 312（`diagnostics.picking` 开时） | 每 model 条目 × picking.frag 克隆一条 |
| skybox / tonemap | 1 + 1 | 单条目 |
| **条目总数（本次运行）** | **626** | 下标 0–311 模型、312–623 克隆、624 skybox、625 tonemap |
| VkShaderEXT 句柄 | 1252 = 626 × 2 | 每条目 mesh + frag 一次 `createShadersEXT`（shader.cppm:316） |
| 启动创建耗时 | **≈2.3s** = 312 × ~7.4ms | 日志 19:57:31.167 → 19:57:33.486 |

**一句话**：312 = distinct(16MB 顶点块, 材质) 对数 = 132 材质按块跨度展开（§2.5）。纹理维度已按
唯一图去重到最小；放大器是 30 个顶点块（§2.1）。膨胀的根因是架构选择：binding mapping 创建期
烘焙进 shader object 且不可改（§4）。

## §2 312 的生成机制

### 2.1 顶点侧：30 = 30 个 16MB 顶点块槽（不是"30 个模型"）

`model_importer_impl.cppm:116-140`（`kVertexChunkBytes = 16MB`，`computeChunks`）：

- 1591 primitive 去索引展开为 48B/顶点（`ModelVertex` = vec3+vec3+vec2+vec4）；
- 61 个 doubleSided primitive 追加绕序翻转背面副本（顶点 ×2，`gltf_loader_impl.cppm:469-485`）；
- 合计 **9,333,990 顶点 ≈ 427 MiB**（日志 `gltf_loader_impl.cppm:583`），顺序拼进 ≤16MB 块 →
  **30 块 = 30 个 BDA buffer = 30 个几何 heap 槽**（日志 `model: 1591 primitives → 30 块`，
  `model_importer_impl.cppm:243`）。同块 primitive 共享顶点槽，跨块则槽不同。

### 2.2 纹理侧：405 图片资产（四通道独立去重）

`texture_pool.cppm:254-289`：base/normal/orm/emissive **四类各自独立的 image 下标去重缓存**
（同图跨通道会重复上传，sRGB/线性路不同）→ 395 张 glb 图落 405 个纹理资产 + 白纹理 + 中性法线
= 407（日志 `textures: 407 资产`，`texture_pool.cppm:234`）：

| 通道 | 独立图片 | 回落 | 条目内不同取值 |
|---|---|---|---|
| base | 132（132 材质各一张） | 白 | 132 |
| normal | 132（132 材质各一张） | 中性法线 | 132 |
| orm | 131（11 primitive 无 ORM） | 白（4096） | 132 = 131 + 白 |
| emissive | 10（102 primitive 引用） | 白（4096） | 11 = 10 + 白 |

### 2.3 spec 侧：材质名连连看（spec 是材质的纯函数）

`main_scene.cppm:150-168` + `configs/scene.lua material_styles`：材质名含 `Emissive` → light
（**12** 材质，scene.lua 注释写 10 已漂移）、含 `Foliage` → vegetation（10）、其余 → pbr（110）。
forward 经 `styles` 表落规格名：`model_light` / `model_vegetation` / `model_pbr_ibl`。

### 2.4 去重入口：六元组（不是五元组）

`shader_registry_impl.cppm:25-29`：

```cpp
const auto key = std::make_tuple(specName, vertexSlot, textureSlot, normalSlot, ormSlot, emissiveSlot);
```

每 primitive 调一次 `addModelEntry`（`main_scene.cppm:169-180`），同组合返回既有条目。sampler 槽
不进 key（全场景单值 `ensureShared`）；scene data / env 槽同理恒等。条目名 =
`{spec}.v{v}.t{t}.n{n}.o{o}.e{e}`（`shader_registry_impl.cppm:65`），逐条 debug 日志
（`:74`）可直接统计。

> 已知文档债：`shader_registry.cppm:58-59` 类注释仍写"五元组"（Phase 8 ③ 加 emissive 槽前的
> 残留）；`addModelEntry` 接口注释（`:178`）与 impl（`:27`）均为"六元组"，以后者为准。

### 2.5 312 = 块跨度展开

条目数 = distinct (块, 材质) 对数（spec 是材质纯函数、每材质纹理四元组固定，均不再分裂）。
132 材质的块跨度分布（raw glb 模拟，附录 A）：

| 跨块数 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 材质数 | 68 | 27 | 11 | 9 | 5 | 2 | 3 | 3 | 2 | 1 | 1 |

68×1 + 27×2 + 11×3 + 9×4 + 5×5 + 2×6 + 3×7 + 3×8 + 2×9 + 1×10 + 1×11 = **312**。
按 spec 切：110 pbr 材质 → 264、10 vegetation → 25、12 light → 23。

## §3 成本账本（付了什么 / 换来了什么）

**付（启动期，一次性；shader 不变不重复）**：

- 创建耗时 ≈2.3s。312 条均匀 ~7.4ms/条——**不随 SPV 重复出现递减**（全部条目仅共享
  1 mesh SPV + 3 frag SPV），说明驱动按 (SPV, mapping) 逐条做创建期工作（§6 尽调项 ③）。
  SPV 读盘可忽略（6.3KB + 6~20KB × 624 次 ≈ 8MB，page cache）；模型路径 `loadShaderEffect`
  无 SPV 缓存（每条重读 2 文件），picking 路径有 `meshCache` 先例（`shader_registry_impl.cppm:88-103`）。
- DGC execution set 容量 = 2 × 条目数 = 624 slots（`vk_indirect_commands.cppm:38-39`）。
- GPU 显存：spec 保证同 SPIR-V 不同 mapping 的编译二进制相同（`VkShaderCreateInfoEXT`
  VU 注，本地 spec v1.4.360）→ 312 条目共享 3 份编译产物，增量只是句柄 + mapping 元数据。

**换（每帧）**：1591 draw × 3 条命令（`pushDataEXT` 4B + `bindShadersEXT` +
`drawMeshTasksEXT`，`vkrenderer_impl.cppm:424-430`），零描述符 update/bind 操作；heap 帧首绑一次
（`:385-397`）。无 VkPipeline、无 descriptor set/pool。

## §4 为什么"多一个纹理槽 = 多一个 shader object"

三层结构（纹理槽存在哪）：

```
堆槽（启动期写完，终生不变）        slot N = {SAMPLED_IMAGE, 某 imageView}
shader object mapping（创建期烘焙） binding 1 → heapOffset = N × descriptorSize
draw（3 条命令，无任何绑纹理步骤）   换纹理 = 换 shader object 句柄
```

- Slang 的 `uniform texture2d<float> : register(t1)` 只有 binding 号；"哪张图"只存在于 mapping。
- mapping 随 `vkCreateShadersEXT` 冻结（`shader.cppm:244-258`，
  `VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT`），**无创建后修改 API**。
  新纹理槽 = 新 mapping 组合 = 新对象（若组合未见）。
- 对比传统路线：纹理在 descriptor set 里 per-draw 供给，加纹理不加管线；本路线把 per-set 的
  "指哪张图"摊到了 per-shader-object。

**这不是 spec 强制**：`VkDescriptorSetAndBindingMappingEXT::sourceData` 是 union（本地 spec
v1.4.360 与系统头 v357 均含全部取值）：

| source | 槽怎么定 | 效果 |
|---|---|---|
| `HEAP_WITH_CONSTANT_OFFSET`（本项目，shader.cppm:253） | 创建期常量 | 纹理 = 对象身份 → 组合枚举（**2026-10-14 定案砍掉**） |
| `HEAP_WITH_PUSH_INDEX` | `base + *(pushData + pushOffset)`，per-draw | 一个对象服务所有纹理（本机先例只在 storage image） |
| `HEAP_WITH_INDIRECT_INDEX(_ARRAY)` | **索引在 buffer**，buffer 地址在 push data（`(VkDeviceAddress*)pPushData)[pushOffset/8]` | 同上，且 5 个 binding 共用一个地址 ⇒ **定案选此路**（§1.0） |

类比：constant offset = 静态链接（每纹理组合一个"二进制"，运行期零解析）；push index =
动态链接/PLT（draw 时查一次）。选 constant offset 的理由：本机 CPU-bound（Phase 9 基线
frame 7.10ms ≈ cpu.record 7.03ms，CPU 侧零余量，`docs/archive/phase9_plan.md` §10），per-draw 索引解析
方向相反；descriptor heap 是 RADV 实验特性，变体 source 更缺测试（踩洞史见
`docs/DESCRIPTOR_HEAP_SPEC.md`、`heap_texture_spike`）；DGC 路径下执行集合绑定完全静态，
push-index 会扩大 GPU 生成序列的正确性面。

**2026-10-14 定案时这三条理由的现状**（就地改写，不留"旧结论 + 更正"双层）：
① **CPU-bound 零余量已推翻**——此后 CPU 负载持续压缩（`docs/cpu_perf_plan.md`、实测
`drawFrame.dataPath` 0.025ms 级），这条不再是"方向相反"而是"有待价"；② **RADV 实验特性，
已部分消解**：push-index 在 storage image 上有本机先例（mip_gen / env_prefilter），
但 sampled image + indirect index 仍零先例 ⟹ 转成开工清单 ①（§5.2）；③ **DGC 绑定静态，
仍成立且升级为唯一待决策项**：NVIDIA DGC 对 push constant 增长有潜在 bug（用户持有细节），
本机 RADV 不走该路 ⟹ 节点见 §5.5。

## §5 定案与开工清单（2026-10-14）

> 本节原是"优化方向（预留，未实施）"，触发条件是"启动时间成为痛点"。
> **2026-10-14 用户拍板砍掉创建期烘焙**，动机换成结构性判据：**per-draw 数据被烘成对象身份**
> （场景 1591 个 primitive 的 draw 只分属 3–7 个 spec 程序，却为它们造了 376 个对象身份）
> 本身是债，不再按启动时间定价。原缓存路线（L0/L1）随定案作废（§5.3）。
> §2/§3 的机制与账本不变，仍是 ② 的对照基线。

### 5.1 定案形态

1. **五个 binding 换源**：0（顶点）/ 1（base）/ 6（normal）/ 7（orm）/ 8（emissive）从
   constant offset 改 `HEAP_WITH_INDIRECT_INDEX`。**sampler 侧不动**（binding 2/5/13/16 走
   sampler heap constant offset）：全场景共享单值，无 per-draw 变化，换了纯风险零收益。
2. **索引住在现有表**：per-object 索引表并入 `objectDraws`（binding 26，
   `StructuredBuffer<ObjectDraw>`，容量 `kSceneMatrixCapacity` = 4096）——每对象 +5 个 uint
   = 20B，@4096 = +80KB，可忽略。**不新开 buffer、不领新槽**：正是"加到现有 constant buffer 里"。
3. **push data 4B → 16B**：`@0` objectIndex 不动，`@8` 放索引表 device address
   （VUID-11260 要求 8 对齐、11261 ≤ `maxPushDataSize − 8`；双驱动实测 256B，
   `vkdevice.cppm:154-161`）。五个 mapping **共用同一个 pushOffset**（同一个地址），各按
   `addressOffset` = `offsetof(ObjectIndexRow, item)` 取自己那一项（VUID-11262 要求 4 对齐计数）。
4. **去重 key 塌成 spec**：`addModelEntry` 的六元组（`shader_registry_impl.cppm:43`）退化为
   `specName`；条目名退化为 spec 名；`modelEntryBySlot` 键随之退化（可整体换成 `map<string,uint32_t>`）。
5. **接线随身份塌缩**：`addCloneEntries` 每 model 条目一条克隆 → 每 spec 一条（312 → 3）；
   `dgcStatic` 每管线条目一次 Execute（376 → 3 次）；`izbMaskEntryFor(vertexSlot)` 的键从
   "顶点槽"改成 spec（vertex 索引进表后它不再是身份）。

🔴 **一条必须写进表契约的 spec 约束**：`indirectAddress` 的读被定义为 **non-volatile uniform
buffer read**，且**"值在飞行中必须保持静态，否则数据竞争"**（`descriptorheaps.adoc` indirect-index
节末段）。scene data device buffer 是跨帧单份、每帧整块重写，现有解法 = `recordFixedDataPath` 的
**写前依赖 barrier** 兜 RAW/WAW（`vkrenderer_passrec.cppm:263-275`）⟹ 沿用同一机制即可，但这条
必须显式写进索引表注释——否则它是第三条隐式依赖（前两条 = heap barrier 与 scene data 拷贝）。

### 5.2 开工清单（明天动工；每步判据同条写死，不绿不进下一步）

| # | 动作 | 验收判据 |
|---|---|---|
| ① | **最小 INDIRECT_INDEX spike = 第一个 commit**：单 binding（base 纹理）、单 spec、push data 16B | `just regress floor` **逐位**（本机 sampled image + indirect index 零先例，这一步证伪整条路线就当天上午改道，不损失下午）；VVL/syncval 0 条（含 VUID-11260/11261/11262） |
| ② | 五 binding 全换 + 索引表字段 + push data 16B + 去重 key 塌成 spec | `just regress floor` 逐位；`logStartupBaseline` 新读数 = 条目 ≈ spec 数 / 句柄 ≈ 2×条目 / mappings ≈ 30（对照 §1 基线表）；**条目数断言进 `tests/`**：换材质/换块不新增条目，且"新增 spec 组合不新增条目"可变异 |
| ③ | emit 侧全部路径（opaque / skybox / transparent / shadow / meshlet / MDI）+ `list.cullArgs[i]` 与 draw 同值（BIT-IDENTICAL 的既有真相源） | `just regress floor/burst` 逐位；`just test-all`（设备层 + DGC 路） |
| ④ | DGC 两条路：NV 序列 stride 与 PUSH_DATA token 4B → 16B（`vk_indirect_commands.cppm:26`，`DRAW_MESH_TASKS` 从 @108 后移）+ fanout 把索引表地址写进流；RADV 静态 shader 路执行次数随条目塌缩 | `just test-all`；dgcStatic Execute 次数 = 条目数；⚠ 本机走重放路 ⟹ ③ 的逐位**已是本机的实际验收**，④ 的正确性靠单测/设备层用例兜 |
| ⑤ | 收账：债-04/债-23 结案 + 本文档基线读数改写 + `还债总账` §3 翻案行改终态 | `just gates` 全绿 |

**变异红线（开工当天配好，别等 ⑤ 补）**：把索引表里任一槽索引改错、把某 binding 的
`addressOffset` 改成相邻字段、把 push data 的地址写成上一行的表基址——**必须**有东西红
（逐位回归 + 条目数断言 + VVL 三件套三者至少各一）。它们的共同性质是**错法不崩、VVL 大多
不报、画面静默错**，正是本仓反复堵的形态。

### 5.3 原 L0 / L1 缓存路线：作废

根因（376 条目）已除 ⟹ 3–7 条目 × 3.01 ms ≈ 20 ms，**缓存没有可省的**。原 §5.2/§5.3
（运行内 SPV 字节缓存 / 磁盘二进制缓存 + key 设计 + "二启省 ~2s"）与 §6 三条尽调项**全部作废**。
原文保留仅为历史：留着标注比删干净更能防止"有人再提二进制缓存"这一种反复。

### 5.4 原路线级两条的现状

- **16MB 顶点块调大**（`kVertexChunkBytes` → 256MB ≈ 2 块）→ 仍可做，但与 5.1 **收益重叠**：
  vertex 索引进表后 chunk 维度本就不再分裂条目，调大块只省 BDA buffer 条数与 allocator 压力。
  ⚠ `docs/mdi_plan.md` §E 路线级原把这条与 mapping 替换记成"别重复计收益"——现在那一轴被 5.1
  消掉，**该告警的对象已变，以本节为准**。
- **push-index mapping** → 不选：+16B（四个索引）且带 20bit 打包限制，而 indirect index 用
  +12B（一个地址）喂满五个 binding。理由与公式 → §1.0。

### 5.5 待用户决策（明天开工前定，全案只有这一条没有数据）

**NVIDIA DGC 的 push constant 增长 bug**（细节由用户持有，本机 RADV 不触发该路）：④ 的 NV 路
PUSH_DATA token 需 +12B。两个分支：

- **(a) fanout 把索引表地址一起写进流**——地址 = 表基址 + `objectIndex × stride`，compute 侧
  一行；NV 路与 CPU/RADV 路形态统一（都靠 indirect index）。
- **(b) NV 路保留 constant offset 旁路**——不碰那个 bug，但等于**没砍干净**：条目数仍是
  (spec × chunk × 材质) 量级，且多一条"两条路径两种映射源"的验证面。

**默认按 (a)**，用户确认后开工。若选 (b)，②的条目数断言要分路径写（NV 档不塌缩），
`logStartupBaseline` 的读数也要按当前路径解释。

## §6 实施前尽调项 ——【作废，2026-10-14】

> 以下三条是 **L1 磁盘二进制缓存**开工前的尽调项；该路线已随定案作废（§5.3）⟹ 三条一并作废。
> 保留原文，防止"再提一次二进制缓存"时不记得它们曾经为什么被写下来。**唯一仍然有效的教训**
> = 第 2 条的精神：新机制上机前先确认驱动**自己**有没有缓存，别把驱动白送的算成自己的收益
> （INDIRECT_INDEX 的对应问题 = RADV 对 indirect index 有没有内部去重，开工 ① 的顺带观察项）。

1. **linked object 的二进制语义（第一风险）**：本项目 mesh+frag 一次 linked 创建
   （`mesh.nextStage = FRAGMENT`）。`vkGetShaderBinaryDataEXT` 返回 per-stage 二进制还是 linked
   程序一份？从二进制重建时链上两个 createInfo 各自怎么给码？spec 此处含混 → VVL 下小实验确认。
2. **RADV 自带缓存行为**：是否落盘 ISPIR-V 缓存（删缓存目录 / 换驱动版本观察创建耗时）。
   若驱动已有 on-disk cache，L1 增量收益缩水。
3. **成本构成 spike**："同 (SPV, mapping) 连建 N 次" vs "每次换 mapping"，分离
   SPIR-V 编译 / 校验 / handle 分配。若大头是校验 + handle，二进制缓存收益有限。

## §7 代码与日志锚点

| 内容 | 位置 |
|---|---|
| 顶点 16MB 分块 | `rgl/model_importer_impl.cppm:116-140` |
| doubleSided 背面副本（顶点 ×2） | `rgl/gltf_loader_impl.cppm:469-485` |
| 纹理四通道独立去重 | `rgl/texture_pool.cppm:220-289` |
| 六元组去重 + 条目名 + 逐条日志 | `rgl/shader_registry_impl.cppm:22-78` |
| 去重表成员（六元组） | `rgl/shader_registry.cppm:352`（过期"五元组"注释 :58-59） |
| spec 派生 + per-primitive 建条目 | `rgl/main_scene.cppm:149-180` |
| mapping 烘焙（constant offset） | `rgl/shader.cppm:236-319` |
| 帧首绑 heap + draw 3 命令 | `rgl/vkrenderer_impl.cppm:376-398, 424-430` |
| DGC 容量 2×条目 | `rgl/vk_indirect_commands.cppm:38-39` |
| 材质风格规则 | `configs/scene.lua`（`material_styles` / `styles`） |
| 基准日志 | `build/rgl/logs/gllog.txt`（2026-09-28 19:57 运行；build 侧 picking 临时开） |

**本刀要动的符号本体**（2026-10-14 定案；行号会漂，改前以 grep 为准）：

| 符号 | 位置 | 动什么 |
|---|---|---|
| `bakeBindingMappings` | `rgl/shader.cppm` | 五个 binding 的 `m.source` 与 `sourceData`（constant offset → indirect index，共用 pushOffset、各带 addressOffset） |
| `PipelineBindings` + `b.extra`（binding 6/7/8） | `rgl/shader.cppm` / `shader_registry_impl.cppm:81-83` | per-draw 槽字段从"身份"降为"表字段引用" |
| `addModelEntry` 六元组去重 + 条目名 | `rgl/shader_registry_impl.cppm:43,102-103` | key 塌成 `specName`；条目名退化为 spec |
| `addCloneEntries` | `rgl/shader_registry_impl.cppm` | 每 model 条目克隆 → 每 spec 一条 |
| `izbMaskEntryFor(vertexSlot)` | `rgl/shader_registry.cppm` | 键从顶点槽改 spec |
| `DrawArgs`（push data 4B） | `rgl/generated/material_types_generated.cppm` + 各 emit 点 | 4B → 16B（`@8` 索引表地址）；mip_gen/env_prefilter 侧不变（它们仍用 push-index，`kPushIndexByte = 20`） |
| `objectDraws`（binding 26） | `scene_data` 子范围载体 | 每对象 +5 uint 索引列 |
| DGC 序列 stride + PUSH_DATA token | `rgl/vk_indirect_commands.cppm:26`（`@8` 4B → 16B，`DRAW_MESH_TASKS` 后移） | NV 路；fanout 把地址写进流 |
| `logStartupBaseline` 读数 | `rgl/shader.cppm`（`ShaderObjectStats`） | ②⑤ 的验收数字（条目 / 句柄 / mappings / createTotal） |
| spec 公式与 VUID | `Vulkan-Docs/chapters/descriptorheaps.adoc`（indirect-index 节：11260/11261/11262 + "值在飞行中保持静态"段） | 定案依据；别靠记忆写参数 |

## 附录 A：312 的独立复现（raw glb + 项目算法，2026-09-28 逐项吻合）

纯 Python 重放（无项目依赖）：解析 glb JSON → 按 mesh/primitive 序逐 primitive：
顶点数 = indices 数（无 indices = POSITION 数）× (doubleSided ? 2 : 1)，字节 = ×48B，
顺序拼 16MB 块（`off>0 && off+b > 16MB` 进新块）；纹理键 = (base/normal/orm/emissive 的
texture index 直接当 image index——loader 假设 texture 下标 == image 下标，
`gltf_loader_impl.cppm:265-273`；-1 = 回落)；spec = 材质名含 Emissive/Foliage 连连看。
条目 = distinct (spec, 块, 四键)。

```
primitives: 1591 (log 1591)   total vertices: 9333990 (log 9333990)
chunks: 30 (log 30)           materials: 132 (log 132)
entries: 312 (log 312)        pbr 264 / vegetation 25 / light 23 (log 同)
distinct per dim: 3/30/132/132/132/11 (log 同)
```

复现时注意：loader 对 texture 的 `index` **不查 textures 数组**、直接当 image index 用
（越界 = -1）；`just reflect` 或资产变更后数字会变，本文档数据以 §7 基准日志为准。
