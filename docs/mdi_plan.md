# Multi Draw Indirect 路线（全面 indirect 化的可行性与阶段）

> **状态**：调研（2026-10-10 立，2026-10-11 加 §7 簇化转换可行性实测，**2026-10-12 簇化轴立项开工**）。
> **命令数轴（阶段 A/B/C/D/E）仍挂起**（交付帧时间零，见 §7.5）；**簇化轴（★0-★3）= 在做的那条**，
> 拍板 P4-P7 见 §0，实施编排见 §9。
> ⚠ **§7 换了本文的轴**：命令数轴（阶段 A/B/C）确认不阻塞但**交付帧时间收益为零**；真正能改帧
> 时间的是簇化轴（§7.5），而它反过来把"bistro 不是 meshlet"变成前置条件。先读 §0 追加块。
> **代码事实一律实测**（spec 锚点 = 本地克隆 `/home/robin/program/Vulkan-Docs`，着色器编译事实 =
> 项目 pin 的 `build/_deps/slang-prebuilt-src/bin/slangc` 2026.16 实跑，限额 = 本机 `vulkaninfo`）。
> **性能数字绑机** = AMD Radeon 8060S / RADV STRIX_HALO / Mesa 26.2.3，换机先复测
> （AGENTS「本机不著书」）。

## 0. 结论速览与拍板

**技术上可行**，且项目的既有障碍已经自己消掉了大半。**P1-P3 = 2026-10-10 定；P4-P7 = 2026-10-12 定（开工放行，实施编排见 §9）**：

| # | 拍板 | 内容 |
|---|---|---|
| P1 | **起手做阴影段** | 阴影段占全帧命令数 **77%**（推算，见 §1.2），且条目按顶点块去重只剩 ~30 → 分桶后命令数断崖式下降；depth-only，重排风险最低 |
| P2 | **先路径 A 后路径 B** | A = mesh-only shader 直接读 `SV_DrawIndex`，**保留** `NO_TASK_SHADER_BIT_EXT`；B = 加 task shader，作为后续增强（groupCount 也进 buffer + per-draw 剔除） |
| P3 | **DGC 的 `@8 PUSH_DATA` 保留不删** | NVIDIA 路靠它取 per-sequence objectIndex，删了就断（根因见 §2.2，这是本线最容易读错的一条） |
| **P4** | **LOD 纳入本计划** | 项目此前**几何 LOD 为零**、mesh shader 是去索引顶点着色器替身（无剔除无选级）。★1/★2/★3 从"可行性"升为**计划内**，本线同时是"让 mesh shader 不再是摆设"那笔账的还账 |
| **P5** | **允许破逐位红线**（大特性放行） | 簇化改的是三角形顺序与顶点执行顺序 ⟹ 逐位相同在数学上不可得。**裁决形态**：🔴 **门关 = 逐位不变仍是硬护栏**（旧路径一行不动，这条免费且必须守住）；**门开的新路径判据 = 闭式解对拍 + 结构判据 + 统计阈值**，红线 A 主动重锚。**债-86 就此结案**（→ `docs/还债总账.md`） |
| **P6** | **测试面收敛 = CSM only** | 阴影只挑 **CSM** 一档做验收（`shadow.lua` `mode="csm"`，IZB 已默认关）；**PCSS / EVSSM / IZB 不在本轮验收面**，簇路径改动**不为其逐位负责**，但**不得编译红/启动红**；**SSR 不作为本轮判据**（在途且画质未定案） |
| **P7** | **GI 路线不开** | 但 ★1 的簇化转换**顺带满足 RT 资产需求**（实测 bistro 132 材质 RT-ready：baseColor/normal/metalRough 齐、TANGENT 已烘、BLEND 0 个、MASK 10 个；本机 `accelerationStructure`/`rayTracingPipeline`/`rayQuery` 全 true）。**别再为"RT 要不要重转 bistro"重开调研** |

**2026-10-11 追加事实（§7 实测，未新增拍板）**：本文通篇在算的是**命令数轴**，而命令数轴在交付
口径的收益是**零**（§1.3 早就写了：交付 GPU-bound 贴地板）。簇化可行性实测把它旁边那条轴的量级
摆出来了——**同一机位提交几何 42.6% → 1.6%（26×）、顶点流量 ≈70×、显存 407.8MB → ≈167MB**。
⟹ 本文的"全面 indirect 化"要拆成两条独立决策读：**MDI 不带簇 = vkCmd 变少（开发体感）**；
**MDI 带簇 = 唯一有资格改交付帧时间的路**（§7.5）。P1「起手做阴影段」在两条轴上含义不同，
动手前须重新确认指的是哪条。

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

> **两条轴，别混着排期**（§7.5）：**命令数轴**（A/B/C/D/E，收益 = 开发体感 + 结构简化，交付帧
> 时间零）与**簇化轴**（★0-★3，收益 = 交付帧时间与显存）。★1 不依赖 A/B/C，A/B/C 也不依赖 ★1。
> **2026-10-12 起簇化轴 = 在做的这条**（P4）。三条贯穿红线：① **门关逐位不变**（P5，硬护栏）；
> ② 门开判据 = 闭式解 + 结构判据 + 统计阈值（P5）；③ 验收只跑 **CSM**（P6）。

| 阶段 | 内容 | 牵动面 |
|---|---|---|
| **0 spike** ✅ 已交付（2026-10-10） | `tests/test_mdi_drawindex.cppm` 七条用例 + 三个探针 shader（§6）+ `multiDrawIndirect` 进 **required** 列表 | **不碰 rgl/ 渲染路径**；实跑结论见 §6.2——本线全部前提坐实，**阶段 C 的孔洞设计也被实测放行** |
| **★0 离线打包工具**（计划内） | `meshlet_pack`：glb → 簇资产（顶点池 + 8bit 簇索引 + 逐簇包围体 + DAG + 层级）；**构建期产物，不进 git**；`--verify` 自检（L0 三角多重集 = 输入、索引可解回、包围体合法） | 依赖 = vcpkg `meshoptimizer` + vendored `clusterlod.h`；**引擎不链 meshoptimizer**（只读打包产物）；产物缺失 → 回落现行路径（⟹ 未打包资产零风险） |
| **★1 簇路径 + 簇剔除**（计划内；可行性已实测 §7.1-7.4） | 门 = "该资产有簇资产吗"；新 mesh shader 读簇描述取 ≤128 顶点；层级遍历 → 逐簇可见性；**选级恒 L0** | **G-buffer 提交几何 42.6%→1.65%、阴影段 36.3%→11.3%**（§7.3/§7.4，离线模拟）；门开破逐位（P5 已放行），门关逐位不变 |
| **★2 DAG cut 选级（相机侧）**（计划内） | 屏幕误差选级（`boundsError` 公式，阈值 = 像素） | 实测在交付机位**免费**（§7.3：与纯簇剔除同量）⟹ 价值在远机位与阴影侧，不在当前机位 |
| **★3 DAG cut 选级（阴影侧）**（计划内，**前置未解**） | 按图集纹素预算选级（各级联各自的纹素） | 🔴 **现成 group 级 cut 实测拿不到收益**（§7.4 带作用域与推翻条件）：先查米级误差离群簇 + 试小 `partition_size` + 逐簇选级 |
| **A 阴影段**（P1，命令数轴） | `shadow.mesh.slang` / `izb_mask.mesh.slang` 走 §3 公式；新增 per-draw 表 + 按 (cascade, vertexSlot) 分桶；`executePassDraws` 阴影分支走桶循环 | **隔离面**：这两个 shader 自带 push 块、不 import `material.slang`，`ShadowDrawArgs` 不在 5 条反射管线里（`cmake/RglReflection.cmake:58-67`）⟹ **不触发 376 条目重建、不碰 DGC 布局** |
| **B 模型段**（命令数轴） | 同上，但牵动 `material.slang` 的 DrawArgs 语义 + DGC 序列 `@8` 的解释（按 §3：不动布局）；要重测 deferred 下真实桶数 | 牵反射（`just reflect` + 提交生成物） |
| **C 压紧上 GPU**（命令数轴） | compute 剔除直写 args/params（atomic 紧凑），消掉 CPU 读回 | 改 `cull.comp.slang` 输出布局 + 对账契约（§5.3） |
| **D task shader**（P2 后段） | 路径 B：groupCount 也进 params、`DispatchMesh(0)` 做 per-draw 剔除 | 去 NO_TASK bit + execution set 加 task 槽（2j/2j+1 → 3 槽约定要重算）。⚠ **没有 ★1 就没有可剔的单元**——台账那条"task 给派发权不给几何单元"的结论没被推翻，被推翻的是它下面"连续三角形无邻接性"的论据（§7.6） |
| **E 路线级（独立决策，别混）** | mapping 换 `HEAP_WITH_PUSH_INDEX` / `HEAP_WITH_INDIRECT_INDEX_ARRAY` ⟹ 条目 376→spec 数、桶数→~3 | `docs/shader_object_组合成本与优化预留.md` §4/§5.4 明写"路线级决策"，且把 per-draw 解析成本带回来。⚠ ★1 的**单一全局顶点池**会自己消掉条目 key 的"30 顶点块"那一维（§7.5 附带红利，推断未实测）⟹ 两条路对条目数的作用重叠，**别重复计收益** |

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
8. **§7 的百分比是离线几何模拟，不是引擎内实测**（口径 = 几何 + 保守包围体测试，机位取
   `configs/camera.lua` 现值）。它能定"量级与方向"，**不能当验收判据**——落地判据必须在引擎内
   重取（draws 计数 + gpu zone 逐 pass），否则就是把模拟读成实测。同理 §7.4 的级联体积是
   "子视锥外接球"近似，比引擎真实光空间盒**小**（盒的 Z 覆盖全部投影体）⟹ 那栏绝对值偏乐观。
9. **簇化把债-86 从"将来可能"变成"必然"**：簇布局改的是顶点聚类与光栅顺序，截图不可能逐位相同。
   本文不预判裁决（裁决载体 = `docs/还债总账.md` 债-86），但**★2 开工前它必须已经结案**。

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

## 7. bistro 簇化转换可行性（2026-10-11 实测）

> **本节为什么存在**：用户问"阻止 MDI 的下一个问题是 bistro 不是 meshlet 的——这严重吗？要不要
> 重转？"。答案是**分轴的**（§7.5），而回答它需要真转一遍，所以本轮把转换跑完了。
> **口径**：全部数字 = 本机实测（AMD 8060S，CPU 侧耗时绑机）；离线 spike，**未落库**（本轮在
> `/tmp/opencode/mdi/spike/`，落库位置待拍板 §7.8）。重跑配方见 §7.1 末。

### 7.1 家底与依赖裁决（别再重查一遍）

| 项 | 事实 |
|---|---|
| 库 | **meshoptimizer 1.3**（MIT，纯 C++，无 CUDA、无 Python）；**vcpkg 有 port**（1.2）⟹ 满足 AGENTS「工具语言纪律」（C++ + vcpkg），**不需要**放宽 |
| 簇 LOD DAG | **`demo/clusterlod.h`（962 行，同仓 MIT）= BTO 式簇 LOD DAG 的现成参考实现**（`clodBuild` / `clodBuildHierarchy` / `clodLocalIndices`，配置面含 permissive/sloppy 回退、误差合并、植被边界膨胀、逐簇包围体）。⚠ **vcpkg port 不装 demo** ⟹ 要 vendored 进仓（这就是用户放行的"脏依赖"那部分，体量 962 行同仓头文件） |
| 消费侧参考 | `demo/nanite.cpp`（530 行）：屏幕投影误差公式 `boundsError`（0..1，乘屏幕高 = px）+ **BVH cut 遍历**（本文 §7.3 的选级即抄它） |
| 出局项 | **NVIDIA NVMT**：CUDA-only，本机 `which nvcc` 无 ⟹ 三机轮换里只有 NVIDIA 那台能跑，离线工具不能长这样 |
| 耗时 | 全场景（1591 primitive / 2.83M 三角）**纯簇化 0.66 秒 / 含 12 级 LOD DAG 2.8 秒**（本机 CPU，单线程）；spike 编译 4.3 秒 |
| 重跑配方 | `git clone --depth 1 https://github.com/zeux/meshoptimizer` → `clang++ -O2 -std=c++17 -I<mo父目录> spike.cpp <mo>/src/*.cpp` → `dump.py assets/bistro_exterior.glb`（Python stdlib + numpy，出 `prim_count / (vcount,icount,material)[] / pos+idx`）→ `spike bistro.bin cluster`（纯簇化）或 `... lod`（含 DAG 与选级模拟） |

### 7.2 产物布局与数据面账

**布局**（= 目标资产格式的形状）：全局顶点池（源顶点，48B/顶点不变）+ 逐簇描述（包围体 + 材质/索引偏移）
+ 簇内 8bit 三角索引 + 簇内顶点重映射表 + 簇 LOD DAG + 空间层级（`clodBuildHierarchy`）。

**口径**（2026-10-12 起以工具为准）：下表 = `tools/meshlet_pack/`（★0-a 落地）对 `assets/bistro_exterior.glb`
的实测，配置钉死 `clodDefaultConfig(128)` 全默认，库 = vcpkg `meshoptimizer 1.3`（overlay port）。
早期数字来自 spike 的 `-O2` 源码构建，**深层与 §7.2.1 的浮点标志敏感性一并解释**。

| 量 | 实测（工具口径） |
|---|---|
| L0 簇 | **27,487**（`clodBuild` 的 depth0）；平均 **103.0 三角/簇**，中位填充率 **94.5%**。对照：`buildMeshlets(128v/128t)` 单独跑 = 26,316 簇 / 107.6 三角 / 填充 96.9%（**两个口径别混**，半径行也是分开的） |
| L0 簇包围球半径 | p50 **0.38m** / p90 1.52m / p99 7.92m / max 32.11m（世界单位，L0 口径）。`buildMeshlets` 单独跑口径：p50 0.39 / p90 1.66 / p99 8.49 / max 39.58 |
| DAG | **12 级**，**61,518** 簇，**7,596** group，层级 **8,699** 节点（`clodBuildHierarchy` node_width=8） |
| 各级三角（精确） | 2,832,120 / 1,402,098 / 690,188 / 334,293 / 162,928 / 77,556 / 35,693 / 13,935 / 6,059 / 2,432 / 389 / 60（≈每级减半） |
| **L0 无损** | 不变量自检（= `--verify ①`）：L0 三角多重集 = 输入多重集，**2,832,120 逐位一致 = 100.00%**；误差预算 = ∞ → 85,138（最粗级） |
| **LOD 不增顶点** | 简化只在**原顶点集上产索引表** ⟹ **全部 12 级共用同一份顶点池**（2,883,709 顶点，1:1 源顶点），12 级索引合计 16.7MB |
| 数据面合计 | 顶点池 138.4 + L0 索引 8.5（8bit）+ L0 重映射 11.9（32bit）+ 其余级索引 8.2 = **167.0MB**（旧口径）；**全量含其余级重映射 14.3MB = 181.3MB**，vs 现行去索引 **407.8MB** = **2.25× 显存下降**（旧写的 2.4× 是漏算其余级 remap） |
| UV 手性 | **逐三角 82.00% 为负**（2,322,319 / 2,832,120，与 loader `uvTriangleHandedness` 同式）。🔴 顶点池存的是 `tangent.w = +1` 占位 ⟹ **★1 的 mesh shader 必须逐三角重算手性**，否则 82% 的面法线翻错（`normalmap.slang` 消费 `worldT.w`） |

#### 7.2.1 🔴 簇 DAG 对构建浮点标志敏感（实测，影响测试怎么写）

同一份源、同一份 `clusterlod.h`、同一份钉死配置，**只改 meshoptimizer 自身的构建标志**：

| meshoptimizer 构建 | 总簇 | group | 节点 | depth1 簇 | depth5 三角 |
|---|---|---|---|---|---|
| 全部 `-O2`（spike 早期数字的来源） | 61,547 | 7,598 | 8,705 | 15,564 | 77,820 |
| **工具的组合**：调用方 `-O2` + vcpkg 库 `-march=native` | **61,518** | **7,596** | **8,699** | 15,525 | 77,556 |
| 全部 `-O2 -march=native` | 61,604 | 7,601 | — | — | — |

- **L0 完全稳定**（27,487 簇 / 2,832,120 三角 = 输入 100% / 12 级 / 数据面体积都不动）；
  **depth1 起每级都有抖动**，量级 ≤0.35%（早期文档写"只差最深两级"是被显示精度盖住了，已纠正）。
- 根因 = 深层 simplify 的误差比较出现 ULP 级平局（`-march=native` 的 FMA 收缩 + 自动向量化）。
- **对测试的直接后果**（写断言前必读）：**任何"总簇数 / 各级三角数 == 常数"的断言都必须带容差**，
  只有 **L0 不变量**（多重集逐位 = 输入）可以写死。判据的数值噪声必须远小于它要检测的信号——
  这里噪声是 0.35%，拿它去检测"我改坏了多少"是可行的，检测"是不是恰好等于 X"是不可行的。
- **对资产可复现性的后果**：产物是**构建期生成、不进 git**（§7.8 拍板 3）⟹ 跨机不逐位**不构成问题**；
  若将来要把 `.meshlets.bin` 提交进 LFS，再回来钉 port 的构建标志（PATCHES.md 记了这个杠杆）。


⚠ **一条限制地板的资产事实**：bistro 源网格 **2,883,709 顶点 / 2,832,120 三角 = 1.02 顶点/三角**
（摄影测量 + UV 缝拆分 ⟹ 拓扑共享本来就差；去索引后是 8,496,360 顶点 = 2.95×）。⟹ "再按位置焊接
顶点还能省"这条路**不成立**，138.4MB 就是地板；2.4× 全部来自"停止去索引"，不来自焊接。

### 7.3 运行时定价：几何与带宽（离线几何模拟）

机位 = `configs/camera.lua` 现值（eye (1.1,0.75,1.05) / target (0.14,0.12,0.02) / fov45 / 1280×720），
保守包围体测试（primitive 用 AABB，簇/DAG 用包围球）。⚠ **三行的聚类口径不同源**：第 1 行 = 现行
去索引布局（Python 模拟），第 2 行 = `buildMeshlets(128v/128t)` 的 26,316 簇，第 3 行 = `clodBuild`
的 27,487 簇（= §7.2 的 L0）。三者都是"提交多少三角"的同一口径，但**别把 417 簇与 27,487 簇读成同一次聚类**。

| 提交几何 | 占全场景三角 | 绝对量 |
|---|---|---|
| 现行：primitive 粒度剔除、无 LOD | **42.6%**（748/1591 primitive 通过） | 1.21 M |
| 簇粒度剔除（无 LOD） | 1.65%（417 簇） | 46.6 K |
| 簇粒度 + DAG cut @1px | **1.6%**（按 depth 分摊：d0 22.5K / d1 14.2K / d2 9.0K） | 45.6 K |

- **26× 提交几何下降**，且**LOD 在这个机位是免费的**（选级落在 depth 0-2，与纯簇剔除同量）。
- **带宽账**：现行每三角取 3 顶点 × 48B = **144B**；簇布局实测 113 顶点 / 107 三角 ⟹ **50.7B/三角**
  （2.8×）。与 26× 相乘 ≈ **70× 顶点流量下降**。
- **交叉印证（这条才是重点）**：`scale-half` 臂像素 ÷4 只让 `gpu.frame` 10.78→9.98（−11%，
  `docs/cpu_perf_plan.md` §2 表）⟹ 帧**不绑像素绑几何**，与本定价同向。**本线第一次有交付帧时间
  可谈**（§1.3 那句"收益是开发环境体感"只对命令数轴成立）。

### 7.4 阴影段定价（近似，方向性；阴影 = GPU 6.28ms = 帧 61%）

模型：级联体积 = **子视锥外接球**（引擎 `frustumBoundingSphere` 拟合 → 光空间盒 XY 收紧到球半径、
Z 覆盖全部投影体）；图集 `kShadowMapSize` 2048 / 2×2 = 1024/级 ⟹ 纹素 = 2R/1024。

| 级 | 切片(m) | R(m) | primitive 粒度 | 簇粒度 | 簇+LOD@1texel |
|---|---|---|---|---|---|
| 0 | 0-25.0 | 12.5 | 25.5% | **11.2%** | 99.6% ⚠ |
| 1 | 25-50.1 | 22.1 | 8.8% | **0.1%** | 98.7% ⚠ |
| 2 | 50-76.0 | 32.8 | 1.9% | 0.0% | 97.6% ⚠ |
| 3 | 76-200 | 90.0 | 0.2% | 0.0% | 85.5% ⚠ |
| **合计** | | | **36.3%** | **11.3%** | — |

- **簇粒度剔除对阴影段 = 3.2× 几何下降**（36.3%→11.3%）。实测锚：现行 5368 draws / 6.28ms
  （`docs/还债实测数据.md` §2.2）——本文模型比引擎真实盒**小**（盒的 Z 覆盖全部投影体）⟹ 绝对值
  偏乐观，**3.2× 这个比值待引擎内复核**（§5.8）。
- **未拿到的收益（负知识，带三件套）**：按图集纹素预算做 **group 级** DAG cut，实测**退化到 L0**
  （上表 ⚠ 列）。根因 = 选级发生在 group（partition，~16 簇）粒度、group 误差取分区内**最大值**，
  而簇误差是长尾分布（depth4 p50 2.4cm / **p90 18cm** / max 1.6m；个别簇 2.1m）。
  **作用域**：只否"拿现成 group 级 cut 直接换远级联 LOD 收益"，**不否 LOD 本身**——潜力仍在，
  uniform 整级上界 = depth4 5.8% / depth7 0.5%（§7.2 各级三角表）。
  **仍然要做**：查那批米级误差离群簇（`nanite.cpp` 有 "stuck clusters" 计数可抄）、试小
  `partition_size`、以及逐簇（而非逐 group）选级。
  **什么实验推翻本条**：上述任一改动后按纹素预算重跑，若 c1-c3 落到 uniform 上界附近 ⟹ 本条作废。

### 7.5 谁阻塞谁（正面回答"bistro 不是 meshlet 严重吗 / 要不要重转"）

| 轴 | bistro 非簇布局阻塞吗 | 硬依据 |
|---|---|---|
| **命令数轴**（阶段 A/B/C/D 的 MDI 语义） | **完全不阻塞** | 最大 primitive 82,015 三角 → `groupCount` **8,202 ≪ maxMeshWorkGroupCount.x 65,335**；1591 个 primitive 全在限额内（最大的两个各 82,015 三角，其余更小）⟹ **没有一个需要拆成多行**；行/孔洞/DrawIndex 语义已由 `tests/test_mdi_drawindex.cppm` 放行 |
| **簇化轴**（★2/★3：GPU-driven 剔除 + 选级 + 压紧） | **这才是阻塞**，而它才是唯一能改交付帧时间的轴 | §7.3/§7.4 的量级全部以"簇 = 可剔除/可选级的几何单元"为前提；primitive 粒度下 G-buffer 只能剔到 42.6%、阴影只能剔到 36.3% |

**一句裁决**：**没有簇，MDI 只是 vkCmd 变少；有了簇，MDI 才第一次有资格改交付帧时间。**

**要不要重转 bistro**：要，但**正确单位不是"这个资产"而是"这条管线"**——转换是 2.8 秒的事，
kyoto / cottage 同理，所以形态 = 通用离线工具 + 新资产格式（bistro 只是第一个靶）。且它可以
**与旧路径并存、门切换**（逐位红线只在门开时失效）。

**附带红利（推断，未实测）**：单一全局顶点池会消掉管线条目 key 的"30 个 16MB 顶点块"那一维
（`docs/shader_object_组合成本与优化预留.md` §2.1 写的条目爆炸根因）⟹ 条目 376 → ≈132-180、
启动创建 1.13s 同降。**标推断**：条目数由 binding mapping 的实际组合决定，要等 ★1 的绑定形态定了才能测。

### 7.6 纠正一条台账论据（推翻即改，不留双层）

`docs/还债总账.md` 第 3 节"上 task shader ⟹ 立刻补足 LOD"那条，论据写的是"每 group = 连续 10 个
三角形 ⟹ 连续三角形**无空间邻接性** ⟹ per-cluster 剔除几乎剔不掉"。**实测为假**：bistro 连续 10 三角
的包围盒比其 primitive 紧 **20-254×**（按三角数取 top12 primitive 实测中位数），且比 1024³ 网格
Morton 排序后的同级还紧 **3-10×**——摄影测量资产的重建面序**本身就是相干的**。

- **作用域**：只对 bistro 成立（摄影测量）；手工/程序化几何不保证。
- **仍然成立的部分**：缺的仍是"逐单元包围体 + 低于 primitive 的剔除粒度"，而这两样**在有簇索引格式
  之后才划算**（簇 = 天然的单元，且顺带带来 8bit 索引与共享顶点池）；被推翻的只是"重做几何才有
  相干性"这条推论，以及由它推出的"这是以月计的工具链工作"——**离线那半实测 2.8 秒 + 现成 MIT 实现**。
- **什么实验推翻本纠正**：对 kyoto / cottage 跑同一测量若不相干，则把作用域收窄到 bistro 并注明。
- 台账已就地改写（2026-10-12 随 P4 放行）。

### 7.7 风险（按会不会翻车排序）

1. **属性感知简化未测**（头号画质风险）。本轮只喂位置。bistro 有 UV 缝 + 法线贴图，不配
   `clodMesh::vertex_attributes` + `attribute_protect_mask` 保护缝，简化就会跨缝滑顶点 ⟹ 着色撕裂。
   API 有，但**要一次真 A/B 才能立项**。
2. **债-86 正面撞上**（§5.9）：簇化改顶点聚类与光栅顺序 ⟹ 截图不可能逐位相同。
3. **MASK 抠花 / 植被 / doubleSided**：需逐材质配置（`simplify_dilate_borders` 文档明写"foliage
   最有用"）；61 个 doubleSided primitive 现在的顶点绕序副本应换成按 draw 关背面剔除。
4. **group 级选级保守 + 簇误差长尾**（§7.4，含推翻条件）。
5. **运行时才是工作量主体**：GPU 层级遍历 + DAG cut + 压紧 + 新 mesh shader（读簇描述 → 取 ≤128
   顶点 → 变换 → 输出）+ 新资产格式 + importer。**离线工具是简单的那 10%。**

### 7.8 拍板结果（2026-10-12）与剩余待拍板

| # | 原待拍板 | 结果 |
|---|---|---|
| 1 | 先做哪条轴 | **簇化轴**（P4 + "然后开干"）。命令数轴 A-E **挂起不删**，交付帧时间零这条不变 |
| 2 | 工具落库位置/形态 | `tools/meshlet_pack/`（C++ + vcpkg），**必须进 `just build-tools`**——AGENTS 已点名"不进 build-tools 的 C++ 工具 = 编译红没人发现"（imgqa 教训）。`clusterlod.h` **vendored** 进 `third_party/meshoptimizer/`（vcpkg port 不装 demo），带 PATCHES.md 记来源与改动 |
| 3 | 资产格式与门形态 | 门 = **逐资产**"有没有簇产物"；产物 = **构建期生成、不进 git**（转换 2.8s，缓存键 = 源 glb 的 SHA1 + 工具版本）；🔴 **引擎不链 meshoptimizer**（只读打包产物，依赖面可验）；产物缺失 → **回落现行路径** ⟹ 未打包资产零风险 |
| 4 | 债-86 判据分区 | **已结案**（P5）：门关逐位不变 = 硬护栏；门开用闭式解 + 结构判据 + 统计阈值 |
| 5 | 逐级联真实投影体数 | **推迟到 ★3 前**（它是阴影侧选级的输入，不是 ★0/★1 的前置） |

**剩余待拍板（不阻塞 ★0/★1 开工）**：① 属性感知简化的画质验收口径（★2 前置，§7.7 第 1 条）；
② MASK / 植被 / doubleSided 的逐材质配置表（bistro 实测 MASK 10 个、doubleSided 6 个）。

## 8. 明确不做

- **不给带历史时域档立位比闸门**（skill `rgl-verify`：TAA/FSR-temporal 不逐位确定）——
  阶段 A/B 的 regress 只在 `aa=off` 档做。
- **不把 VVL 关掉当基线报**（`docs/cpu_perf_plan.md` §6）。
- **不在阶段 A 顺手做 E**（push-index mapping）：那是条目数的根因，但会把 per-draw 解析成本
  带回来，与"交付 GPU-bound、CPU 有余量"的现状正好相反方向，须独立拍板。

## 9. 实施编排（2026-10-12 开工，簇化轴）

**串行推进，一步一验收**：一次只派一个子 agent，主 agent **亲验产物本体**（生成物数字自己复核，
不采信汇报）。每批一次 commit（任何一步可单独回滚）。**"判据"列是入账的唯一标准**——判据不绿就不进下一步。

| 批次 | 内容 | 判据（入账条件） | 渲染风险 |
|---|---|---|---|
| **★0-a 打包器** ✅ **已交付**（2026-10-12） | vcpkg 加 `meshoptimizer`；`third_party/meshoptimizer/`（vendored `clusterlod.h` + LICENSE + PATCHES.md）；`tools/meshlet_pack/`（glb → 簇产物 + 统计表 + `--verify`）；CMake 与 `just build-tools` 接入 | ① `just build-tools` 与 `just build` 双绿，且**引擎链接面无 meshoptimizer**（`nm -C build/rgl/rgl \| grep -c meshopt` = **0**，工具二进制 = 101）；② bistro 统计表与 §7.2 **逐项对拍**（配置钉死 = `clodDefaultConfig(128)` 全默认：max_vertices/max_triangles=128、min_triangles=42、partition_size=16、simplify_ratio=0.5、threshold=0.85、permissive=true）；③ `--verify` 四条自检绿 | **零**（不碰运行时） |
| **★0-b 格式 + 构建期接入** | 二进制格式（magic/version/源 SHA1 缓存键）、构建期生成规则、importer 读入、门 = "该资产有簇产物吗" | **门默认关时截图逐位不变**（`just regress ab`）；产物缺失/存在两种情况启动均不红 | 零（门关 = 旧路径） |
| **★0.5 验收仪器**（P5 落地，🔴 ★1 前置） | 闭式解探针与 imgqa 判据扩展到簇档：**可见性归因**（被剔的每块都该剔）+ **逐像素误差上界**断言 | 仪器在**现行路径**上跑必须全绿（阳性锚点）；人为剔掉一块可见几何必须红（变异红线） | 零 |
| **★1-a 簇路径 + 簇剔除** | 新 mesh shader（读簇描述 → ≤128 顶点 → 变换）+ GPU 层级遍历 + 逐簇可见性 + 压紧；**选级恒 L0** | 用 ★0.5 仪器判；G-buffer 提交几何应落到 §7.3 的 **1.65%** 量级（离线模拟数，作对照不作硬判据） | 中，可逐块归因 |
| **★1-b 阴影段接入** | 阴影 mesh 走簇路径（**只 CSM 档**，P6） | 阴影段 GPU 时间对照 6.28ms 基线；**非默认 cull 档（`gpu_sync`/`gpu_lagged`）单独冒烟 + syncval**（AGENTS 红线：默认档不提交 cull CB） | 中高 |
| **★2 相机侧选级** | 屏幕误差 DAG cut（`boundsError` 公式，阈值 = 像素） | 误差闭式上界不超 T px；远机位收益实测（交付机位实测免费，§7.3） | 中 |
| **★3 阴影侧选级** | 按图集纹素预算选级 | 🔴 前置 = §7.4 group 级保守问题先破（查米级误差离群簇 / 试小 `partition_size` / 逐簇选级） | 高 |

**★0-a 交付记录（2026-10-12，主 agent 亲验）**：产物 = `tools/meshlet_pack/`（16 文件，最大 292 行）
+ `third_party/{meshoptimizer,vcpkg-ports}/` + `Justfile`（overlay ports / build-tools 各一行）
+ `tools/checks/config.py`（把新工具纳入样式面，**零基线**不许登 baseline）。四条自检 =
L0 三角多重集 / 8bit 索引可解 / 逐簇包围体 / **DAG 结构不变量**。主 agent 独立做两条变异
（**形态与 worker 所验不同**）：**绕序反转** → ①② 红（证明 ① 的"最小顶点在前循环旋转"规范化
**没有把反转一起吞掉**——这是这条断言最容易变成摆设的地方，必须专门验）；**包围球缩 1%** → ③ 红。
`cp` 回滚后产物 sha256 回到基准（`fa189554…`）。

三条要记住的：

1. **§7.2.1 的浮点标志敏感性**是本轮最意外的发现，它直接改写测试口径：**只有 L0 不变量能写死，
   簇数/各级三角数断言必须带容差**（噪声 0.35%）。
2. **"三条自检"是开批次时开少了**：把 depth>0 的产出整段丢掉时 ①②③ 全绿（① 只保 L0）——
   必须补**结构不变量**（④：簇顶点 ⊆ refined group 顶点集、`refined` 指向更浅 group）才验得红。
   **教训：无损性断言只覆盖 L0 时，DAG 侧必须另配一条结构断言**，否则"最粗那级整个不产出"是绿的。
3. **`tangent.w` 逐三角 82.00% 为负**（§7.2 表）= **★1 硬前置**：mesh shader 不逐三角重算手性
   = 82% 的面法线翻错。同一条实测顺手把 `gltf_loader_impl.cppm` 里"92%"那条**没写口径**的旧注释
   钉死了（逐三角 82.00% / 逐 prim 多数 93.5% / 逐材质多数 94.7%）。

**贯穿纪律**：

- **验收档固定**（P6）：`shadow.lua mode="csm"` + `aa=off` + TAA off + SSR off，走 `RGL_CONFIG_DIR`
  副本；**PCSS / EVSSM / IZB 不为其逐位负责，但不得编译红/启动红**。
- **门关逐位不变**（P5）是每批都要重跑的护栏，不是只测一次。
- 每批交付的测试由主 agent **当场变异验红后才入账**（skill `rgl-test-discipline`）。
- 顶点格式压缩（48B → 量化）**是另一条轴**，不混进簇化批次；簇化之后可再拿 2-3×。
