# Multi Draw Indirect 路线（全面 indirect 化的可行性与阶段）

> **状态**：调研（2026-10-10 立）。**未立项、不排期**——Phase 15（SSR/湿面）在途，本文只把
> 可行性与形态定下来，动手时机由用户另行拍板。
> **代码事实一律实测**（spec 锚点 = 本地克隆 `/home/robin/program/Vulkan-Docs`，着色器编译事实 =
> 项目 pin 的 `build/_deps/slang-prebuilt-src/bin/slangc` 2026.16 实跑，限额 = 本机 `vulkaninfo`）。
> **性能数字绑机** = AMD Radeon 8060S / RADV STRIX_HALO / Mesa 26.2.3，换机先复测
> （AGENTS「本机不著书」）。

## 0. 结论速览与拍板

**技术上可行**，且项目的既有障碍已经自己消掉了大半。三条拍板（2026-10-10 用户定）：

| # | 拍板 | 内容 |
|---|---|---|
| P1 | **起手做阴影段** | 阴影段占全帧命令数 **77%**（推算，见 §1.2），且条目按顶点块去重只剩 ~30 → 分桶后命令数断崖式下降；depth-only，重排风险最低 |
| P2 | **先路径 A 后路径 B** | A = mesh-only shader 直接读 `SV_DrawIndex`，**保留** `NO_TASK_SHADER_BIT_EXT`；B = 加 task shader，作为后续增强（groupCount 也进 buffer + per-draw 剔除） |
| P3 | **DGC 的 `@8 PUSH_DATA` 保留不删** | NVIDIA 路靠它取 per-sequence objectIndex，删了就断（根因见 §2.2，这是本线最容易读错的一条） |

**必须先纠正的一句旧结论**（两处文档，`device_generated_commands_research.md` §4.3 /
`phase7_devlog.md` 2026-09-09 批次 5）：

> ~~`vkCmdDrawMeshTasksIndirectCountEXT` 不可行：该命令无 per-draw push data 机制，mesh shader
> 拿不到 per-draw DrawArgs~~

**作用域**：该结论**对 DGC 路依然成立**（§2.2：DGC 展开成直接 draw，DrawIndex 恒 0，必须有 push），
**对 MDI 路已失效**——当时 `DrawArgs` 还带 mvp/totalTriangles/vertexOffset/baseColor（36B→100B），
"拿不到 per-draw DrawArgs"是真的；Phase 7b+ bindless 化之后 **DrawArgs 只剩 `objectIndex` 4B**，
而它可以由 `DrawIndex` 从 buffer 取，根本不需要 push。
**仍然要做的部分**：别据此冻结 DGC（两条路按 §3 的统一公式并存，同源）。
**什么实验推翻它**：`tests/test_mdi_drawindex`（本文 §6）——若 RADV 上 DrawIndex 不按 indirect
语义递增，则 MDI 路退回读回重放，本纠正作废。

## 1. 现状事实基线（作用是别再重查一遍）

### 1.1 每 draw 的命令构成

`vkrenderer_passrec.cppm:315-321` `Renderer::drawOne`：

```
pushDataEXT(DrawArgs 4B = 仅 objectIndex) → bindShadersEXT → drawMeshTasksEXT(groupCount,1,1)
```

= **3 条 vkCmd / draw**。`profiler.cppm:150` 的 `kCommandsPerMeshDraw = 3` 就是这个常数，
改 MDI 必然牵动它（§5.4）。

`DrawArgs` 现状（`shaders/material.slang`）：**只剩 `uint objectIndex`**，其余全部住 scene data
per-object 表（`readObjectMvp` / `readObjectDraw`，binding 3）。这是本线能成立的**前提**——
push 里已经没有"数据"，只剩一个"索引"，而索引可以从 DrawIndex 推出来。

### 1.2 规模（数字来源逐条标注，别当成同一批测量）

| 量 | 值 | 出处 |
|---|---|---|
| 全帧 draw | 6942 | `docs/cpu_perf_plan.md` §4.2 `default` 臂（2026-10-09） |
| 全帧 vkCmd | 21036 | 同上（= 6942×3 + 固定段，吻合） |
| 阴影段 draw | **≈5368（77%）** | **推算** = `default` 6942 − `shadow-off` 臂 1574（同文件 §2）；落地前该实测 |
| 模型段 draw | 1591 primitive | `docs/shader_object_组合成本与优化预留.md` §1 |
| 管线条目（现行 deferred 基线） | **376** / 752 句柄 | 同文件 §1 基线重测块（启动日志自动打） |
| 模型条目去重数 | 312 | 同文件 §2.5，**那次是 forward + picking 临时开的 2026-09-28 运行** ⟹ deferred 下的真实桶数须重测 |
| 阴影条目去重数 | ≈30 | `addShadowEntry` 去重键 = vertexSlot，而顶点块 = 30（`shader_object_…` §2.1） |

⚠ **MDI 一次调用只能有一份 shader state**，所以"collapse 到 1 条"不可能：收益上限 =
**去重后的条目数**，不是 1。模型段 ≈ 312 条 MDI 调用（对 1591 draw），阴影段 ≈ 30×4 = 120
（对 ~5368 draw）。全帧命令数 21036 → 量级约 1000~1500。

### 1.3 收益定价（诚实版：交付路径省不到帧时间）

| 口径 | 读数 | 含义 |
|---|---|---|
| 交付（VVL 关） | frame **10.66** vs gpu.frame **10.30**，cpu 真干 3.70 | **GPU-bound，已贴地板（差 0.36ms）** ⟹ 省 CPU 换不到帧时间 |
| 开发（VVL 开） | VVL = **0.73µs/条 vkCmd** ≈ 15.3ms，占 cpu.record 54% | 命令数 ↓10× ⟹ `default` 臂 15.25ms 逼近 10.7ms 地板 |

⟹ **本线的收益是"开发环境体感提速 + 结构性简化"，不是交付帧时间**。报账时别把 VVL 开销
当成交付收益（`docs/cpu_perf_plan.md` §3.1 的教训正是这个）。

**真正的结构性收益 = 消掉"读回重放"这条路的同步等待**：现行 RADV 路要在录 opaque 段**之前**
拿到可见位 ⟹ 帧周期 = `cpu + gpu` 而非 `max`（债-30 的账，同文件 §3.2 实测 `cull.fenceWait ≈
gpu.frame`）。compute 剔除 + GPU 压紧 + MDI = CPU 全程不需要知道可见性 ⟹ 零读回零等待。
但注意边界：**默认档 `cull_replay=cpu` 根本不提交 cull CB**，债-30 在默认档已经还掉；
MDI 的价值在让 GPU 臂也能不等待。且"背压不消失只搬家"（帧首 fenceWait 6.73ms = GPU-bound 的
合法背压，§4.3）本线改变不了。

## 2. spec 硬依据（逐条锚点）

### 2.1 DrawIndex 在 task / mesh stage 可用

- `interfaces.adoc:2799-2809` **VUID-DrawIndex-04207**：DrawIndex 只准用于
  `Vertex, MeshEXT, TaskEXT, MeshNV, TaskNV` ⟹ **TaskEXT 与 MeshEXT 都在名单里**
- `interfaces.adoc:2780-2782`：**indirect** 命令下 "DrawIndex begins at zero and increments by
  one for each draw executed. The number of draws is given by the drawCount parameter"
- `interfaces.adoc:2793-2797`：task/mesh 同时存在时**只有第一个 active stage 拿得到真值，
  其它 stage 读到 poison**
- `pipelines.adoc:2665-2671` **VUID-pStages-09631**：pipeline 同时含 task+mesh 时，**mesh** 的
  entry point 不得声明 DrawIndex（task 可以）
- `shaders.adoc:228-233` **VUID-vkCreateShadersEXT-pCreateInfos-09632**（**shader object 形态的
  对应条，本项目的权威一条**）：mesh 创建时**未设** `NO_TASK_SHADER_BIT_EXT` ⟹ mesh 不得声明
  DrawIndex；反过来 **设了该 bit ⟹ mesh 可以声明 DrawIndex**

⟹ 两条互斥且都合法的路径：

| 路径 | mesh 创建 flags | 谁读 DrawIndex | 本项目现状 |
|---|---|---|---|
| **A**（P2 先做） | **保留** `NO_TASK_SHADER_BIT_EXT` | mesh 自己 | ✅ flag 已经在设（`shader.cppm:502`）⟹ **零 flags 改动** |
| **B**（后做） | **去掉**该 bit | task（mesh 读 = poison） | 需新增 task shader + `bindShaders` 的 task 槽从 null 变实句柄 |

路径 A 还顺带满足 `VUID-None-08695`（mesh 带 NO_TASK bit ⟹ task stage **不得**绑实句柄，
现行 `bindShaders` 传的正是 null）。

### 2.2 为什么 DGC 的 PUSH_DATA 不能删（P3 的根因）

`generatedcommands.adoc:1299-1303` 的参考展开：

```cpp
case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_EXT:
    vkCmdDrawMeshTasksEXT(cmd, data->groupCountX, data->groupCountY, data->groupCountZ);
```

= **直接 draw，不是 Indirect**。配合 §2.1 那条"直接 draw 的 DrawIndex 恒为 0"
（`interfaces.adoc:2790`）⟹ **DGC 每条 sequence 的 DrawIndex 都是 0，`@8` 那 4B PUSH_DATA 是它
唯一的 per-sequence 数据源**。删掉它 NVIDIA 路全瞎。

**这条是本线最容易读错的地方**：MDI 与 DGC 的"per-draw 数据从哪来"**机制不同**
（MDI 靠 DrawIndex 递增，DGC 靠 push），必须用 §3 的统一公式让一份 shader 同时满足两边。

### 2.3 MDI 的命令面约束

`drawing.adoc:2592-2604`：
- **VUID-drawCount-02718**（`draw_indirect_drawcount.adoc`，被 mesh indirect 命令 include）：
  **`multiDrawIndirect` feature 未启用 ⟹ drawCount 只能 0 或 1** ← **当前就是这条卡着**
- **VUID-drawCount-07088**：drawCount>1 时 `stride` 必为 4 的倍数且 ≥ `sizeof(VkDrawMeshTasksIndirectCommandEXT)`
  = **12B** ⟹ 允许 16B 步长（§3 的方案靠这一点）
- **VUID-buffer-02709**：args buffer 必带 `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`
- **VUID-MeshEXT-07091**：绑定的 graphics pipeline/shader 必须含 MeshEXT stage

`groupCountX = 0`：**没有** "必须 > 0" 的 VU（`draw_mesh_limits_common.adoc` 只有上限条；
maintenance5 与此无关，别去找它）⟹ 0 workgroup = 空转在 spec 层合法，驱动实现是否干净归 §6 实测。

### 2.4 本机限额与 feature 现状（实测）

`vulkaninfo`（本机，2026-10-10）：

```
multiDrawIndirect      = true
maxDrawIndirectCount   = 4294967295      ← 限额完全不是瓶颈
maxMeshWorkGroupCount  = 65535 × 3,  total = 4194304
maxMeshWorkGroupInvocations = 256
```

**但 `rgl/features_config.json` 没启用 `multiDrawIndirect`**（core 位，`features` 列表里只有
robustBufferAccess / shaderInt64 / fragmentStoresAndAtomics）⟹ 按 §2.3 第一条，不加它收益归零。
这是**硬前置**。已具备的相邻条件：`taskShader` ✅（`features_generated.cppm:113`）、
`shaderDrawParameters` ✅（:94，且有 validate）、`maintenance5` ✅、`robustBufferAccess` ✅。

## 3. 统一形态：`push.base + DrawIndex`（一份 shader 喂两条路）

这是本线的设计核心，也是 P3「双路同源」的**构造性**实现——不是纪律承诺，是公式本身让两边
读到同一份数据。

**每帧一张 per-draw 参数表**，16B 行，**同时是 MDI 的 args buffer**：

```
行 i = [ groupCountX | 1 | 1 | objectIndex ]     ← 前 12B 恰是 VkDrawMeshTasksIndirectCommandEXT
          └── GPU 读（§2.3 VUID-07088 允许 stride=16）──┘└── shader 读 word[3] ──┘
```

**shader 侧取 objectIndex 的公式（两条路共用）**：

```hlsl
uint slot  = drawArgs.objectIndex + drawID;   // drawID：路径 A 在 mesh 读、路径 B 在 task 读后经 payload 传
uint objIdx = drawParamsBuffer.Load(slot * 16u + 12u);
```

| 路 | `drawArgs.objectIndex` 的含义 | `drawID` | 结果 |
|---|---|---|---|
| **MDI** | 本次调用的**桶基址**（该 pipeline 条目第一个 draw 的行号） | 0..drawCount−1 | 命中桶内第 drawID 行 |
| **DGC（NVIDIA）** | 本 sequence 的行号（逐 sequence push） | **恒 0**（§2.2） | 命中那一行，一个字节不用改 |
| CPU 逐 draw（现行/重放） | 本 draw 的行号 | 恒 0 | 同上 |

⟹ **DGC 序列布局（120B stride、@8 PUSH_DATA、@108 DRAW_MESH_TASKS）完全不动**；改的只是
"push 那 4B 现在解释成行号"，而它本来解释成的也是行号级别的索引（objectIndex），**尺寸不变 ⟹
NV 驱动那个"push 字节数 × 序列数挂死"的红线（`material.slang` 头注释实测的 84B 阈值）不受影响**。

**桶基址为什么必须走 push**：一次 MDI 调用内 push 区是共享的（这正是当初否掉 IndirectCount 的
理由），但"基址"在一次调用内**本来就是常量**——变化的部分交给 DrawIndex。所以障碍不是
"per-draw push 不存在"，而是"per-draw 里只有 drawID 那一项，而 DrawIndex 免费给了它"。

## 4. 阶段划分

| 阶段 | 内容 | 牵动面 |
|---|---|---|
| **0 spike** ✅ 已交付（2026-10-10） | `tests/test_mdi_drawindex.cppm` 七条用例 + 三个探针 shader（§6）+ `multiDrawIndirect` 进 **required** 列表 | **不碰 rgl/ 渲染路径**；实跑结论见 §6.2——本线全部前提坐实，**阶段 C 的孔洞设计也被实测放行** |
| **A 阴影段**（P1） | `shadow.mesh.slang` / `izb_mask.mesh.slang` 走 §3 公式；新增 per-draw 表 + 按 (cascade, vertexSlot) 分桶；`executePassDraws` 阴影分支走桶循环 | **隔离面**：这两个 shader 自带 push 块、不 import `material.slang`，`ShadowDrawArgs` 不在 5 条反射管线里（`cmake/RglReflection.cmake:58-67`）⟹ **不触发 376 条目重建、不碰 DGC 布局** |
| **B 模型段** | 同上，但牵动 `material.slang` 的 DrawArgs 语义 + DGC 序列 `@8` 的解释（按 §3：不动布局）；要重测 deferred 下真实桶数 | 牵反射（`just reflect` + 提交生成物） |
| **C 压紧上 GPU** | compute 剔除直写 args/params（atomic 紧凑），消掉 CPU 读回 | 改 `cull.comp.slang` 输出布局 + 对账契约（§5.3） |
| **D task shader**（P2 后段） | 路径 B：groupCount 也进 params、`DispatchMesh(0)` 做 per-draw 剔除 | 去 NO_TASK bit + execution set 加 task 槽（2j/2j+1 → 3 槽约定要重算） |
| **E 路线级（独立决策，别混）** | mapping 换 `HEAP_WITH_PUSH_INDEX` / `HEAP_WITH_INDIRECT_INDEX_ARRAY` ⟹ 条目 376→spec 数、桶数→~3 | `docs/shader_object_组合成本与优化预留.md` §4/§5.4 明写"路线级决策"，且把 per-draw 解析成本带回来 |

## 5. 风险与坑（逐条带出处）

1. ~~**驱动是否真按 indirect 语义填 DrawIndex**~~ **本机已结案**（2026-10-10，§6.2）：RADV
   STRIX_HALO 上递增正确、0 计数不跳号、shader object 形态同样成立，全部成断言
   （`TEST: tests/test_mdi_drawindex.cppm`）。
   **仍然欠的部分**：**NVIDIA 与 Intel anv 一台没跑**——本断言只在起得来的机器上绿
   （`just test-gpu` 换机即真判），阶段 A 上三机冒烟时才算全结案。别把这条读成"驱动无关"。
2. **NVIDIA Xid 13 家族（阶段 D）**：mesh 不带 NO_TASK bit + task 绑 null → 驱动查空指针 →
   engine reset（`docs/nvidia_compatibility_report.md` §十 根因）。当年 VVL 查不出是**因为
   `taskShader` feature 没启用**（VUID-08694 要 taskShader+meshShader 双位才生效）；
   **现在该位已启用 ⟹ 这条 VVL 会管**，但仍须三平台实跑。
3. **对账契约要跟着改（阶段 C）**：`checkCullAlignment`（`cull_driver_impl.cppm:40-49`）是
   **顺序**比对（candidate 序 = CPU opaque 段序）。GPU 压紧后顺序变 ⟹ 该自检得降级成多重集
   （`reportReconcile` 本来就是多重集，只有 alignment 那条是顺序的）。
4. **`profiler.cppm:150` 的 `kCommandsPerMeshDraw = 3` 与 draw.count 口径**会变——批 9.5 的
   CPU 账本靠它，改完不更新就是把"命令数下降"读成"draw 数下降"。
5. **重排的图像安全性**：阴影段 `depthWrite=true` + `CompareOp=eLess`（`state_kit.cppm:107-108`）
   ⟹ 只存最近深度，draw 顺序不改图集结果；IZB mask 是置位（单调）⟹ 也不改。**模型 opaque 段
   则不是纯 depth-only**（deferred 写 5 张 G-buffer），重排在**深度恰好相等**的片元上有先后歧义
   ⟹ 阶段 B 必须实测 regress，别套用阴影段的推理。
6. **heap 绑定会被 `vkCmdBindDescriptorSets` 作废**（`docs/DESCRIPTOR_HEAP_SPEC.md` 坑 9，
   FSR 现例）⟹ 每帧 FSR dispatch 之后必须重绑，MDI 调用同样依赖 heap 已绑。
7. **anv 未实测**：`MultiDrawIndirectCount=false`（`docs/device_generated_commands_research.md`
   :132）那是 **DGC 的 count token**，与 §2.3 的 `multiDrawIndirect` core 位不是一回事；
   普通 mesh MDI 在 Intel 上未跑过。

## 6. spike：`tests/test_mdi_drawindex.cppm`（`TEST_SUITE("gpu")`）

**为什么放这里而不是新立目录**：项目体量已不允许从头手写 instance→device→heap 引导
（`tests/test_device_primitives.cppm:26-38` 已记过一次改主意：自造裸引导 = 把引擎已经做对的
结论重新赌一遍，且"测的就不是引擎那条路了"）。`tests/` 的 gpu 套件链**全部四层库**、有真设备
夹具 `testing::gpu::Fixture`、自带探针 SPV 目录 `tests/shaders/`（`compile_slang` 复用
`cmake/RglSlang.cmake`，与引擎产物分目录）、且遵守"无设备 FAIL 不 SKIP"。先例形态 =
`test_conservative_mesh.cppm`（同样是"某驱动行为从未被实测过 ⟹ 把它变成可重跑的断言"）。

**探针设计（闭式解，不依赖基线图像）**：RT = 12 列 R32_UINT（整数回环已由
`test_device_primitives` 担保）。args buffer **16B 行** = `[groupCountX | 1 | 1 | tag]`，
前 12B 恰是 `VkDrawMeshTasksIndirectCommandEXT`、word[3] 是只有探针会读的载荷 ⟹
**一张 buffer 两个读者**（GPU 读前 12B、shader 读 word[3]），这正是 §3 手法本身。
两个探针文件：`mdi_spike.mesh.slang`（一 draw 一列，列 = `rowBase + DrawIndex`、值 = `tag`）、
`mdi_groups.mesh.slang`（一 draw 多列，列 = `row*3 + gid.x`、值 = `tag + gid.x`）。

### 6.1 七条用例（实跑形态）

| 用例 | 判据 | 角色 |
|---|---|---|
| `capability` | `multiDrawIndirect` 真进 required 链 + 限额 ≥ 12 | 前置（缺位症状是 VU 打断而非用例红，先点名） |
| `control_drawCount1` | 只第 0 列 = `tag_0`，其余保持哨兵 | **harness 阳性锚点** |
| `drawindex_increments` | 12 列逐一 = `tag_0..11` | **本线成败门** |
| `base_plus_drawindex` | `push.rowBase=6` + drawCount=6 ⟹ 只有列 6..11 有值 | §3 公式里"均匀基址 + 递增"那一步 |
| `zero_groupcount_noop` | 行 5 的 x=0 ⟹ 列 5 保持哨兵，**且列 6..11 不错位** | 阶段 C 孔洞流前提 |
| `per_draw_groupcount` | 四行 x = 1/2/3/1 ⟹ 每 draw 只覆盖自己那 n 列、第 i 列值 = `tag+i` | 阶段 A 的"1591 种 groupCount" |
| `shader_object_variant` | 引擎绑定形态（shader object + `applyDynamicState` + NO_TASK flag）重跑门判据 | 补"测的不是引擎那条路" |

原设计的 `stride12_vs_16` 对照用例**没实现**：换 stride 就没有 word[3] 可放载荷，两形态无法
共用一份判据；而"GPU 逐行读自己的 x"这件事被 `per_draw_groupcount`（x=1/2/3/1 四种数量）与
`zero_groupcount_noop`（x=0）**更强的形式**覆盖住了，不需要那条。

### 6.2 实测结果（本机 RADV STRIX_HALO / Mesa 26.2.3，2026-10-10）

**7 条全绿，115 断言，VVL 零 error；全 gpu 套件 19/19、无设备子集 274/274 无回归。** 逐条结论：

| 问题 | 答案 | 对本线的意义 |
|---|---|---|
| MDI 下 DrawIndex 是否逐 draw 递增 | **是** | §3 公式成立，Phase A 前提坐实 |
| `groupCountX=0` 是否干净空转 | **是** | 0 workgroup 合法（无"必须>0"的 VU，§2.3） |
| 零计数 draw 会不会让 DrawIndex **跳号** | **不跳**（列 6..11 逐位对位） | **阶段 C 的"满槽孔洞流"设计成立**，不必改紧凑流 + atomic 前缀和 |
| 16B stride 的 indirect buffer 能否被 shader 当 SSBO 同读 | **能** | 一张 buffer 两个读者，无需第二个 params buffer |
| 均匀 push 基址 + DrawIndex 相加 | **成立** | 一桶一次 MDI 调用的形态可用（Phase A 直接依赖） |
| 每 draw 是否读自己那行的 groupCountX | **是** | 1591 draw 各自数量不同的现实形态可用 |
| shader object（引擎形态）下以上是否同样成立 | **是** | 不是只有管线形态能用 |

**未覆盖的两处，别把这次的绿读成"整条引擎路径已验"**：① **descriptor heap**——探针走经典
descriptor set + `vkCmdPushConstants`，而引擎走 heap + `vkCmdPushDataEXT`（自建 heap 等于把
`Resources` 重做一遍，不在 spike 里假装覆盖）；② **只有本机一个驱动**，NVIDIA / Intel anv 未跑。
这两条由阶段 A 集成时的 `just regress` 逐位红线 + 三平台冒烟兜。

### 6.3 变异验收（六条全部实测，不是声明）

`M1` row 丢 `drawID` → 门/基址/孔洞/shader-object 四条红、control 仍绿 ✅
`M2` tag 偏移 `+12→+0` → control 立刻红 ✅ `M3` 孔洞行 x 改回 1 → 只有孔洞用例红 ✅
`M4` drawCount/stride 互换 → control + 孔洞 + groupcount 红 ✅
`M5` workgroup 间无差异（去 `+gid.x`）→ 只有 groupcount 用例红 ✅
`M6` 多 workgroup 挤同一列 → 同上 ✅ 回滚后 7/7 复绿 ✅

⚠ **`M4` 这轮先"测不出东西"，暴露了一个真陷阱**（记在这里防止重犯）：首版
`kCols == kRowBytes == 16`，而我把 `drawMeshTasksIndirectEXT` 的参数序写成
`(buffer, offset, stride, drawCount)`（**真实签名是 `..., drawCount, stride`**，
`vulkan_core.h:27021`）。因为两个数都是 16，**互换后数值相同 ⟹ 核心门用例假绿**，
抓到它纯属 control 恰好用了一个不等的数。两处修法：① 参数序收进唯一的 `drawMdi()` 具名入口；
② `kCols` 改成 12，**让任何用例的两个数都不相等**。文件头有"别顺手凑回 16"的红线注释。

**过程教训（沉淀地址）**：
- `TEST: tests/test_mdi_drawindex.cppm` — DrawIndex 语义、0 计数不跳号、stride-16 双读者、
  per-draw groupCount，全部成断言。
- `GUARD:` 待办 — `docs/DESCRIPTOR_HEAP_SPEC.md` 坑 9 那类"参数序写反但数值相同"的假绿，
  目前只靠用例里的 `kCols ≠ kRowBytes` 注释挡着，没有机械护栏。
- `DOC:` **`just regress ab` 会 `git stash -u` 扫走并发会话的在途工作**（含新建的未跟踪文件）。
  本轮实测两次：我的四个文件两次被卷进 `stash@{0}`，靠 regress 的 `finally` 弹回才没丢。
  ⟹ 并发期间"只新建文件就不干扰人"这个判断**不成立**；真要并行得写到仓库外，或等对方收线。

## 7. 明确不做

- **不给带历史时域档立位比闸门**（skill `rgl-verify`：TAA/FSR-temporal 不逐位确定）——
  阶段 A/B 的 regress 只在 `aa=off` 档做。
- **不把 VVL 关掉当基线报**（`docs/cpu_perf_plan.md` §6）。
- **不在阶段 A 顺手做 E**（push-index mapping）：那是条目数的根因，但会把 per-draw 解析成本
  带回来，与"交付 GPU-bound、CPU 有余量"的现状正好相反方向，须独立拍板。
