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
| **★1 簇路径 + 簇剔除**（计划内；可行性已实测 §7.1-7.4） | 门 = "该资产有簇资产吗"；新 mesh shader 读簇描述取 ≤128 顶点；层级遍历 → 逐簇可见性；**选级恒 L0** | **G-buffer 提交几何 42.6%→1.30%（33×）、阴影段 36.3%→11.3%**（§7.3/§7.4，离线模拟）；门开破逐位（P5 已放行），门关逐位不变 |
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
| 现行：primitive 粒度剔除、无 LOD | **42.6%**（748/1591 primitive 通过，Python 模拟） | 1.21 M |
| 簇粒度剔除（无 LOD，`buildMeshlets` 26,316 簇集） | **1.30%**（327 簇 / 36,940 三角） | 36.9 K |
| 同上，**工具 L0 簇集 27,487 + 引擎 CPU 参考剔除器**（★0.5-a，独立第二实现） | **1.08%**（290 簇 / 30,675 三角） | 30.7 K |
| 簇 + DAG cut @1px（逐簇球测试，非层级遍历） | **2.14%**（589 簇 / 60,684 三角） | 60.7 K |

> 🔴 **本表 2026-10-12 全部重跑取原样输出**（旧表的 1.65% / 1.6% 两行**作废**：1.65% 是 spike 里
> `printf` 的**硬编码对照字面量**，被一路当"离线模拟实测"引用进 §4/§7.3/§9 三处；1.6% 是更早
> 配置迭代的读数）。两条独立实现（spike 球测试 1.30% vs 引擎 `meshlet_cull` 1.08%）在同一机位
> 差 0.22pp，簇集不同 ⟹ **互为交叉验证**。

- **33× 提交几何下降**（42.6% → 1.30%；旧写的 26× 是拿作废数算的）。
- 🔴 **推翻本文旧结论"LOD 在这个机位是免费的"**：DAG cut @1px 比纯 L0 剔除**多提交 64% 三角**
  （60.7K vs 36.9K）。机制 = 粗级簇的包围球大 ⟹ 保守视锥测试**通过率上升**，省下的三角被多留下的
  簇吃回去还倒亏。⟹ **★2 的选级必须与剔除耦合**（先剔后选级，或选级时用细级簇的紧包围体做保守
  测试），否则"加 LOD"在近距离机位是**负收益**。这条直接改写 ★2 的判据（见 §9）。
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
三角形 ⟹ 连续三角形**无空间邻接性** ⟹ per-cluster 剔除几乎剔不掉"。**实测为假。**

> **口径**（2026-10-12 重测钉死，之前两处数字都没写口径）：逐 primitive 量
> 「**primitive 包围盒对角线 ÷ 连续 10 三角包围盒对角线的中位数**」，取按三角数排序的 top12 primitive。
>
> - **实测 19-254×，中位 38×**（体积比口径则是 24× ~ 1.32M×，中位 38 万×——**别混**）。
>   12 个里有 **1 个只 2×** ⟹ 相干性是**统计性质不是逐 primitive 保证**。
> - 🔴 **旧写的"比 1024³ Morton 排序后的同级还紧 3-10×"作废**：重测**源序 ≈ Morton 序**
>   （紧度比中位 **1.00×**，范围 0.33-1.75×；那个 2× 的 primitive 源序还**更差**）。
>   ⟹ 相干性是真的，但它**不是源序独有的恩赐**：meshopt 的 `partition_spatial` 本来就能排到同等紧。
>   这**反而加强**对本条的推翻——"要相干性就得重做几何、是以月计的工具链工作"这条推论更站不住。

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
| **★0-b1 读取侧** ✅ **已交付**（2026-10-12） | `rgl/meshlet_asset.cppm` + `_impl`（rlbase，**纯 CPU 无设备**）：产物字节流 → 经校验的只读视图；门 `runtime.lua meshlet.enable` **默认 false**；`tests/test_meshlet_asset.cppm` 24 用例 | `just test` 304 全绿（无设备）；21 条反向用例**错误串两两不同**（可归因）；主 agent 独立变异两条（8bit 契约判据、段对齐判据）各红 2 条且归因干净；**端到端实测：真读取器读真 184MB 产物通过**（61,518 簇 / 7,596 group / 8,699 节点 / 12 级，临时用例验完即删） | 零（不接渲染路径） |
| **★0-b2 构建期接入** ✅ **已交付**（2026-10-12） | `cmake/RglMeshlets.cmake`（构建期生成，清单驱动，configure 期 git-lfs 防御）+ 装配层加载（`MeshletAssetStore` 与 `AssetTable` 同层） | 门关截图 = 基线锚 `54e40665…`；**门开（真加载 184MB 产物，日志实证门是开的）= 同一哈希** ⟹ 加载路径惰性；第二次 `just build` 不重跑；坏产物 WARNING 回落不砖引擎 | 零（门关 = 一行都不走） |
| **★0.5-a CPU 参考剔除器** ✅ **已交付**（2026-10-12） | `rgl/meshlet_cull.cppm`（rlbase 纯函数）：**线性扫描**全部簇的视锥剔除（故意与 GPU 层级遍历算法不同 = 独立第二实现）+ 21 条契约用例 | 六面各自独立负责（含"摘掉该面就复活"反向锚点）、保守边界取等号、半径缩放三轴各一臂、三角合计 ≠ 条数；主 agent 质检变异 MC（球心乘法次序）**注入时全绿 → 暴露真覆盖缺口 → 补两条含旋转用例后红**；**统计锚：真产物交付机位 1.08% vs spike 1.30%（两条独立实现互证）** | 零 |
| **★0.5-b 图像侧仪器**（P5 落地，🔴 ★1-a 前置） | 簇路径 vs 现行路径的**图像对拍模式** + 差异统计与**统计地板**（类比 `regress floor`：后续批次的差异必须 ⊆ 地板才可归因）+ imgqa 可见性归因接入 | 地板在"门开不剔 + 恒 L0"档建立；人为剔掉一块可见几何必须红（变异红线） | 零 |
| **★1-a 簇路径 + 簇剔除** | 新 mesh shader（读簇描述 → ≤128 顶点 → 变换）+ GPU 层级遍历 + 逐簇可见性 + 压紧；**选级恒 L0** | 用 ★0.5 仪器判；G-buffer 提交几何应落到 §7.3 的 **1.08-1.30%** 量级（离线模拟 + 引擎 CPU 参考剔除器两个数，作对照不作硬判据） | 中，可逐块归因 |
| **★1-b 阴影段接入** | 阴影 mesh 走簇路径（**只 CSM 档**，P6） | 阴影段 GPU 时间对照 6.28ms 基线；**非默认 cull 档（`gpu_sync`/`gpu_lagged`）单独冒烟 + syncval**（AGENTS 红线：默认档不提交 cull CB） | 中高 |
| **★2 相机侧选级** | 屏幕误差 DAG cut（`boundsError` 公式，阈值 = 像素），🔴 **必须与剔除耦合** | 误差闭式上界不超 T px；**且提交几何不得多于纯 L0 剔除**（§7.3 重测：不耦合时 @1px 反而**多 64%**，旧结论"交付机位免费"已推翻） | 中 |
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

**★0-b1 交付记录（2026-10-12，主 agent 亲验）**：读取侧 = `meshlet_asset`（rlbase）+ 24 条用例 +
门 `meshlet.enable`（默认 false，真相源 = `runtime_config.cppm` 成员初始化）。三条要记的：

1. **worker 实测抓出一条真设计 bug，主 agent 独立复核为真**：写侧段间 16B 对齐填充 ⟹ "段字节数
   == Σ 计数"对**真产物**不成立（实测 Tris 段多 11B、Remap 段多 8B），判据必须是
   `Σ ≤ 段字节 < Σ + 16`。**若当初写死严格相等，★0-b2 第一次加载就炸**。
2. **两侧格式一致性**是裁决 2（引擎不 include `tools/`）留下的已知缺口，本批用**一次性端到端用例**
   补了证据（真读取器读真 184MB 产物：簇/group/节点/顶点/级数全对，验完即删——**不留依赖 `/tmp`
   产物、会静默 skip 的用例**）。常驻证据改由 ★0-b2 的构建期端到端冒烟承担（产物那时在 build 目录里）。
3. **两条已知无护栏项**（★0-b2 或后续要收）：① 写侧抬 `kToolVersion` 会被引擎拒读，
   联动只写在注释里没有机器护栏；② `MeshletAsset::image` 是公开成员（护栏禁 friend 挡了封装写法），
   parse 后改它会让各 span 悬空，目前只有注释红线。

**★0-b2 交付记录（2026-10-12，主 agent 亲验）**：产物 = `cmake/RglMeshlets.cmake` + 装配层
`loadMeshletForModel` + `MeshletAssetStore`（与 `AssetTable` 同层持有，**不进 `UploadedAssets`**
——那是"已上传 GPU 的东西"，本批零上传）。亲验到的四件事：

1. **门开仍逐位不变是"实证"不是"推断"**：`--set meshlet.enable=true` 后引擎日志确有
   `meshlet: … 已加载 … clusters=61518 groups=7596 nodes=8699 levels=12`，而截图哈希与门关
   **同为基线锚 `54e40665…`**。先证门开了，位比才有意义（否则测的是两次门关）。
2. **坏产物不砖引擎**（实测把产物截断到 2MB）：`WARNING … section table inconsistent: Clusters
   offset … beyond file size …—— 回落现行路径`，退出码 0。产物是可选加速资产，这条是 §7.8 拍板 3
   「缺失 → 回落」的**坏产物版**。
3. **git-lfs 防御必须在 configure 期**：新克隆没拉 LFS 时 glb 是 ~135B 指针，喂给工具 = **构建红**，
   比运行期失败更糟（可选资产不该挡干净 checkout 的构建）。configure 期按文件大小跳过并 WARNING。
4. **主 agent 顺手补的护栏**：`tools/smoke.py --set` 只支持扁平键，worker 是靠裸键 `enable`
   **恰好唯一命中**才跑通门开的——那是运气不是护栏（新门一律是嵌套块 `meshlet`/`wet`/`fsr`/`taa`）。
   已让 `--set` 支持点号嵌套键（`meshlet.enable`），先定位块再匹配子键，块或子键命中 ≠1 一律红。

**已知无护栏项**（★1-a 收）：① 缓存键实现 = ninja 文件级依赖（glb mtime + 工具二进制），
拍板 3 说的"源 SHA1 + 工具版本"里**工具改了没重编**这类形态不设防；② 产物路径约定
（CMake 输出位置 ↔ 引擎"模型路径 + `.meshlets.bin`"）是**注释级同步**，模型若改用绝对路径会
静默找不到产物（好在 INFO 会出声）。

**★0.5-a 交付记录（2026-10-12，主 agent 亲验）**：`meshlet_cull`（rlbase）= GPU 层级遍历版的
**对账伙伴**，故意用最笨的线性扫描（伙伴若也走层级 = 用同一个 bug 验自己）。两条约定在本批钉死、
GPU 版必须继承：视锥 = 复用 `extractFrustum`/`sphereInFrustum`（保守：`d < −r` 才剔）；坐标系 =
产物球是**源 glb 单位** → `objectMatrix` 变换，半径缩放取**三轴长度上界**（低估 = 误剔 = 几何凭空消失）。

三条要记的：

1. 🔴 **本轮最贵的流程教训：spike 里 `printf` 的硬编码对照字面量，一路被当"离线模拟实测"引用。**
   `1.65%` 是 spike 第 302 行 `printf` 里的**字面量**（我手抄的旧读数），进了 §4 阶段表、§7.3 定价表、
   §9 批次判据三处，并被写成"★1 提交几何应落到的量级"。**规则：spike 里不许打印非计算得出的对照
   常量；引用 spike 数字必须重跑并贴原样输出。** 同轮重跑发现 §7.3 整表都是旧配置迭代读数（全部作废重测）。
2. **重测推翻一条本文结论**：`LOD 在交付机位是免费的` → 实为 **DAG cut @1px 比纯 L0 剔除多提交
   64% 三角**（粗级簇包围球大 ⟹ 保守视锥通过率上升）。结论被推翻的同时给出 ★2 的新判据
   （**选级必须与剔除耦合**，否则近距离机位加 LOD 是负收益）。§7.4 阴影侧定价用同一套 cut 机器，
   **★3 开工前必须重测**（它那条"group 级 cut 退化到 L0"可能是同一机制的产物，不是资产性质）。
3. **变异存活 ≠ 判据松**：主 agent 的质检变异 MC（球心乘法次序翻转）注入后 323 条全绿。根因是
   **输入形态缺维度**——其余用例的 `objectMatrix` 全是纯缩放/`translate·scale` 且球心取源原点，
   而 `w=1` 向量在两种乘法次序下都取第 3 列 ⟹ 恒等（glm 一般矩阵下确实不同，已探针验证）。
   补两条含旋转用例后 MC 红。**先问"输入有没有这个维度"，再问"判据是不是松"。**

**★1-a0 交付记录（2026-10-12，子 agent 实现 + 主 agent 亲验）**：`rgl/meshlet_geometry.cppm`(+`_impl`，
**rlgfx**——要 VkDevice，且与 `Resources`/描述符堆一概无关，放 rlscene 会把"要设备"读成"要场景层"）。
四段（顶点池/簇表/Tris/Remap）各一个 storage buffer，**Groups/Nodes/PrimTable 本批不上传**（选级与
层级遍历才要，§10.5 定死的交付面不多做）。门 `meshlet.enable` 关 = 一行不执行。

四条要记的：

1. **上传路径照抄 `uploadChunk`（映射直写、无 staging，峰值 1× 而非 2×）**，但**补了先例的两个洞**：
   ① usage 加 `eTransferSrc`——先例的 canary 缺它，所以它的回读**从没在 VVL 下跑过**；
   ② 写完 `vmaFlushAllocation`——先例没刷，三台机器恰好都 coherent 所以没症状，非 coherent 驱动上
   会**静默读到脏数据**。coherent 内存上这次调用是 VMA 内部提前返回，代价 0。
2. 🔴 **判据形态上的一个坑（本批最有普适价值的一条）**：**映射指针回读在统一内存的机器上是恒真判据**
   （CPU 和 GPU 看同一块内存，"回读一致"什么都没证明）。主判据必须是**设备侧往返**
   （`vkCmdCopyBuffer` 让 GPU 读源 buffer 写目标 buffer），映射回读只作辅助锚点（它红 = 上传写错；
   它绿而设备回读红 = harness 坏）。**任何"上传后回读"类测试都适用**。
3. **主 agent 变异（与子 agent 的 6 条不同形态）抓到一条真缺陷**：MA「整段漏传」注入后，用例
   `CHECK` 完**继续拿 `VK_NULL_HANDLE` 去 `vkCmdCopyBuffer`** → SIGSEGV **打断整个测试二进制**，
   实测其余 **214 条用例静默不跑**——"全绿"面被扩大成假绿。修法 = 两条用例各加
   `REQUIRE(section->deviceAddress != 0)`（真实分配的 BDA 恒非 0，比 `allocBytes>0` 更直接）。
   修后 MA 红得干净（346 条全跑、2 红、无崩溃），MB「传完即被覆盖」同样红。**教训**：设备级用例里
   任何"句柄可能为空还进 vkCmd*"的路径，前置条件一律 `REQUIRE` 不是 `CHECK`——VVL 只报错不拦。
4. **验收证据**（本机 AMD 8060S / RADV STRIX_HALO）：`just build`/`test`/`test-all`/`checks` 全绿
   （346 条含 gpu 套件、0 skipped）；**门关与门开截图同为基线锚 `54e40665…`**，门开有日志实证
   （`clusters=61518 vertices=2883709 | vertexPool=138418032B clusters=2706800B tris=16673264B
   remap=26178352B`，与 `meshlet_pack --stats` 口径对得上）。

**★1-a1 挂号两条**（本批不做，别忘）：① 装配层接线**别再往 `main_scene.cppm` 里塞**（已 840 行，
本批的 `loadMeshletForModel` 是最后一块）——走独立 helper 模块；② 4 个簇 buffer 进 shader 走
**固定 binding 下标**（§10.2），不进 per-entry 描述符。

**★1-a1i 交付记录（2026-10-12，子 agent 实现 + 主 agent 亲验）**：`ObjectClusterRange` 尾区 +
`meshlet_ranges` 扫描（rlbase）。**本批零消费者**（读取点归 ★1-a1ii 的 mesh shader）。

三条要记的：

1. **"加一张表"的真实爆炸半径**（下次加 scene data 尾区照此清单走，别现场发现）：schema 登记 +
   `scene.slang` 结构声明（仅声明）+ `just reflect` 生成物（**纯追加**，`ObjectDraw` 逐字节不动）+
   `scene_data` 尾区常量与 `kSceneDataBytes` + `SceneFrameData` 镜像字段 + `mirrorObjectTables`
   （`renderListSystem` 撞 120 行红线被迫抽函数）+ `writeSceneStaging` 调用点 +
   🔴 **`tests/test_wet_params.cppm` 两条"湿面区是最后一个尾区"的 static_assert 必然编不过**
   （追加尾区的必然后果，改钉"相邻关系"，末尾性移交本批测试的 `static_assert`）。
2. **可测性优先的一次主动重构**：尾区写入本体从 `Renderer` 私有成员提为
   `scene_data::writeClusterRangeRegion`（纯函数、无 VkDevice）。原形态单测**根本调不到**，
   于是"门关 = 整区全 0"与"有效段之外清零"两条只有代码阅读在担保——**截图哈希看不见 scene data
   缓冲区**，这类"数据面而非图像面"的安全性质尤其容易变成假担保。
3. 🔴 **主 agent 变异 MB 抓到一条真覆盖缺口，形态与以往不同**：把铺满判据 `base != expect`
   弱化成本地正确的 `base > expect`（只挡空洞、放过重叠），**子 agent 的 11 条变异全绿时它也全绿**。
   补 `badOverlap` 后红。两条教训：**(a) 判据"短路"≠ 判据"弱化"**——只测"摘掉整条判据"会漏掉
   "判据写反一半"，而后者才是 `!=` 比较最真实的漂移形态；**(b) 反向用例要按判据负责的每一半边
   构造**——一条"空洞"用例会被后来人读成"铺满已验"。另注意它**不是**"完全没响"：重叠仍会被末尾
   总量判据抓到，但**不点名是哪个 prim** ⟹ 这条用例真正钉的是**归因**（合成数据要满足读取侧
   `clusterCount == Σ group.clusterCount`，主 agent 第一版因簇数取 4 而非 5 被 parse 先拒）。

**验收证据**：`just config`/`build`/`reflect`（幂等）/`test`/`checks` 全绿；门关与门开截图同为
`54e40665…`；门关日志 `grep -ci meshlet` = **0**（"门关 = 一行都不执行"的唯一可观测护栏，
MC 类"门失效"变异只能靠它，图像看不见）；门开 INFO 报 `prims=1591 clusters=61518 l0Clusters=27487
/ objectSlot 填充 1591/1591`，与 §10.1 实测三数一致。

**★1-a1ii 挂号**（本批不做）：① 每帧 scene data 多拷 64KB（722400→787936，+9%）——接消费者时
重算这笔账（scene data 是单 buffer 整区 `vkCmdCopyBuffer`，成本按容量线性）；② 装配层两道对账
（prim 数 / material 身份）住在 app 模块**不在单测面**，护栏只有"启动日志无 WARNING"；
③ `writeClusterRangeRegion` 不校验 `staging.size() >= kSceneDataBytes`（本区是尾区，越界写 buffer 外）。

**★0.5-c 交付记录（2026-10-12，子 agent 实现 + 主 agent 亲验）**：`rgl/meshlet_hand.cppm`(+`_impl`，
rlbase) = **★1-a1ii 那两件"错了不报错、只是图像静默错"的事的 CPU 对账伙伴**：① 沿簇寻址三角形
（`簇 → Tris[triOffset+3k]` 8bit 局部索引 `→ Remap[remapOffset+局部] →` 全局顶点池）；
② 逐三角重算 UV 手性（`gltf_loader_impl.cppm:365` 同式，`det < 0 → −1`，含 `det == 0`/NaN 走 +1）。

1. 🔴 **本批最硬的证据 = 真产物双锚同时命中**：门开 INFO
   `total=2832120 negative=2322319 (82.00%) l0TriangleCount=2832120`。
   `total` **恰等于 header.l0TriangleCount** ⟹ 遍历对 L0 不重不漏；`negative` **逐位等于 packer 在
   源 glb 上独立量的 2,322,319** ⟹ 一条链同时验通三件事：**寻址正确 + 顶点池里的 UV 忠实于源 +
   公式照抄无误**。这三件任何一件错，第三个数都不会对上（§10.3 那条"82% 面法线翻错"的前置据此
   从"packer 自报"升级为"引擎侧独立复现"）。
2. **对账契约 = 序，不是计数**：GPU 版将来按 `(base, count, kL0Only)` 跑，与本模块 `perTriangle`
   **逐元素**比 ±1（遍历序 = 簇升序 × 簇内三角升序，被滤簇不产条目）。统计锚是第二形态，**不替代**它。
3. 🔴 **主 agent 两条对账契约面变异，第一条就抓到缺口**（子 agent 的 M1-M13 全绿时它们也全绿）：
   - **M14 逐三角载体改前插（全局逆序）** → 红 3 条 ✅。序错时 `total`/`negative` **完全不变**，
     若没有逐序列断言，★1-a1ii 的对账会**逐元素静默错位**还一路"对上了"。
   - **M15 删掉 `if (wantPerTriangle)`（false 也填）** → **首跑存活**。根因：原有那条
     `perTriangle.empty()` 长在 **count = 0** 的空区间上 = **恒真**，钉不住契约。补
     `total > 0` 前提 + `stats->perTriangle.empty()` 后红。**教训与 ★1-a1i 的 ⑫ 同源：断言要长在
     "这条契约能被违反"的输入形态上**——同一类错（判据存在但输入形态不给它违反的机会）今晚第二次犯。
4. **验收证据**：`just config`/`build`/`test`（348 条）/`checks` 全绿；门关与门开截图同为 `54e40665…`；
   门关日志 `grep -ci meshlet` = 0。

**挂号**：① 门开时启动期多一次全表 L0 遍历（2.83M 三角，实测 ~30ms 量级，11.3s 启动内不可辨）——
**★1-a2 若把它挪进逐帧路径必须先重算**；② M8/M11 类"越界读"变异会让进程先死（红名单不完整），
本批按红线不重复校验读取侧职责，GPU 对账批若要吃这条需给遍历加显式"读窗口有界"前置。

**贯穿纪律**：

- **验收档固定**（P6）：`shadow.lua mode="csm"` + `aa=off` + TAA off + SSR off，走 `RGL_CONFIG_DIR`
  副本；**PCSS / EVSSM / IZB 不为其逐位负责，但不得编译红/启动红**。
- **门关逐位不变**（P5）是每批都要重跑的护栏，不是只测一次。
- 每批交付的测试由主 agent **当场变异验红后才入账**（skill `rgl-test-discipline`）。
- 顶点格式压缩（48B → 量化）**是另一条轴**，不混进簇化批次；簇化之后可再拿 2-3×。

## 10. ★1-a 设计定案（2026-10-12 主 agent 拍板，实现前照做）

★1-a 是本轮唯一还没做设计的大块（要动 shader 契约 + 表布局）。四个决定，实现时**别再重新讨论**：

### 10.1 per-draw 的"我这个 primitive 的簇区间从哪来"

产物里簇是**全局一张表**，而 ★1-a1 沿用现行的**逐 primitive 提交**（材质绑定原样不动）。mesh shader
需要 `clusterBase`（`groupID.x + clusterBase` = 全局簇下标）。三个选项：

| 方案 | 代价 | 门关逐位不变 | 裁决 |
|---|---|---|---|
| A. `ObjectDraw` 加两字段（stride 48 → 56） | 最省一张表 | **靠"没人读新字段"保证**（弱）——它改的是**所有** shader 热路径整读的结构 | ❌ |
| **B. 新增独立表 `ObjectClusterRange{uint base; uint count;}`（stride 8，追加在 scene data 末尾）** | 多一张表 + 一个 offset | **构造性成立**：现有结构逐字节不动 | ✅ **采用** |
| C. push constant 加 `clusterBase` | 零表改动 | 成立 | ❌ 把 per-draw CPU 成本带回来，与"交付 GPU-bound、CPU 有余量"反着走（§8 对 push-index 已表过态） |

- 🔴 **产物结构不变量（2026-10-12 主 agent 在真产物上实测，★1-a1 直接依赖，别再重推）**：
  | 量 | 实测 | 含义 |
  |---|---|---|
  | group 表按 prim 的分段数 | **1591 = primCount** | 每个 prim 的 group 是**一段连续区间**（`clodBuild` 逐 prim 跑的直接后果） |
  | 每 prim 的**簇**区间被拆成多段的 prim 数 | **0 / 1591** | ⟹ 一个 prim 的全部簇 = **单一 `[base, count)`** |
  | 按 prim 序拼出的区间是否铺满 `[0, 61518)` | **是，0 重叠、0 空洞** | ⟹ 表就是"按 prim 序的连续分块"，★1-a2 的分段压紧天然成立 |
  | 每 prim 的 **L0 簇**是否其区间**前缀** | **1591 / 1591 = 100%** | 🔴 **恒 L0 档只要 `(base, l0Count)`**，**不必上传 Groups、shader 侧不必选级过滤**（全局 L0 不是 `[0,27487)` 前缀，是逐 prim 前缀的并 —— 别拿全局前缀偷懒） |
- **per-prim 区间由引擎加载时扫一遍算出**（61,518 簇 / 7,596 group，一次线性扫，微秒级）。
  **不抬 `kFormatVersion`、不改工具**——`DiskGroup{prim, clusterOffset, clusterCount, depth}` 足够。
  上表四条**同时是这条扫描的断言**：任何一条不成立 = 工具漂了，加载期直接报错回落（不砖引擎）。
- **表布局据此定为 `ObjectClusterRange{ uint base; uint count; uint l0Count; uint _reserved; }`
  stride 16**（三个数都要：`count` 给全级档、`l0Count` 给恒 L0 档；补齐位留给 ★2 的选级预算）。
- 表写入走 `scene_data` 现有生成路径（`just reflect` 重跑，生成物随改动提交）。

### 10.2 管线条目与绑定

- 现行条目按 **(顶点块, 纹理集)** 键控（376 条）。簇路径**只有一份顶点池** ⟹ 条目退化成按纹理集键控，
  **不要**给每个簇再开条目。
- 4 个簇 buffer（顶点池 / 簇表 / Tris / Remap）是**全局单例** ⟹ 放**固定 binding 下标**（像 scene data
  那样），**不进** per-entry 描述符，避免条目数再涨。
- 新 mesh shader 只多读这三样 + 现有 `ObjectDraw`/材质绑定；MVP 仍从 scene data 读（`readObjectMvp`）。

### 10.3 🔴 两条硬前置（漏一条 = 图像静默错，不是编译错）

1. **`tangent.w` 必须逐三角重算**：产物顶点池存的是 `w = +1` 占位，而 bistro **逐三角 82.00% 为负**，
   `normalmap.slang` 消费 `worldT.w`（`B = cross(N,T) * w`）⟹ 不重算 = 82% 的面法线翻错。
   公式与 loader **同式**（`uvTriangleHandedness`：`s1x·s2y − s2x·s1y < 0 → -1`），mesh shader 里
   每三角 ~10 FLOP，可忽略。**别存逐顶点 w**：共享池里一个顶点被多三角引用，逐顶点存是**近似**，
   逐三角重算是**忠实**于现行语义的。
2. **`triCount ≤ 128` 的 8bit 契约**在 shader 侧要有断言形态（`numthreads` / `out indices` 上限
   与产物契约对账），否则越界产物会静默画错（读取侧已拦，shader 侧是第二道）。

### 10.4 剔除接法（★1-a2，先定形态再写码）

- **★1-a2 采用"逐 primitive 分段压紧"**：可见簇按 primitive 分段压紧（稳定划分），每 prim 一个
  indirect args（`groupCount` = 该段簇数）⟹ **材质绑定与提交结构与今天完全一致**，改动面最小。
- **单 draw + bindless 材质**（一条 mesh draw 画完全场景可见簇）是真正的奖品（6942 draw → 1），
  但它要 bindless 材质表 + 逐簇材质解析 ⟹ **归 ★1-b/★3 之后**，且那正是命令数轴（§4 A-E）与本轴
  合流的地方——**别在 ★1-a2 顺手做**。
- 对账伙伴 = ★0.5-a 的 `cullClustersLinear`（**线性扫描**，与 GPU 层级遍历算法不同）。
  GPU 侧另加一条诊断：逐帧比对两个可见集，不一致**告警不中断**（照抄 primitive 路径的既有口径）。
- 🔴 **★2 的选级必须与剔除耦合**（§7.3 重测：不耦合时 @1px 反而多提交 64% 三角）。

### 10.5 批次切法（把 ★1-a 再切细，每步都能单独验收）

| 步 | 内容 | 判据 |
|---|---|---|
| ★1-a0 ✅ **已交付**（2026-10-12） | 4 个簇 buffer 上传 GPU（`meshlet_geometry`，rlgfx） | 门关逐位不变；门开日志报 buffer 大小；`just test-all` 的 gpu 套件加一条**设备侧回读校验和**（合成小产物，不依赖 184MB 文件；无设备必须 FAIL 不 SKIP） |
| ★1-a1i ✅ **已交付**（2026-10-12） | scene data 新增 `ObjectClusterRange` 尾区 + 逐 prim 簇区间扫描（四条不变量当探测器） | 门关逐位不变 + 门关日志零 meshlet 行；门开日志三数 = §10.1 实测（1591/61518/27487）；扫描的四条不变量各配反向用例 |
| ★1-a1ii | 新 mesh shader + 条目 + 逐 prim 提交，**不剔不选级（恒 L0，用 `l0Count`）** | **建立"簇路径 vs 现行路径"的图像统计地板**（★0.5-b）；差异必须可归因（预期集中在轮廓/UV 缝边缘，不是整块错） |
| ★1-a2 | 层级遍历 + 逐簇剔除 + 分段压紧 | 与 `cullClustersLinear` 逐帧对账；提交三角落到 §7.3 的 1.08-1.30% 量级 |
| ★1-b | 阴影段走簇路径（**只 CSM 档**，P6） | 阴影段 GPU 时间对照 6.28ms 基线；**非默认 cull 档单独冒烟 + syncval**（AGENTS 红线） |

## 11. ★1-a1ii 验收判据定案（2026-10-12 主 agent 拍板）

★1-a1ii 是 P5 放行破逐位后**第一个要判"差异可不可接受"的批次**。这一节把判据做成机制，
不留"主 agent 看一眼觉得没问题"的环节。

### 11.1 先立一条被忽略的事实：G-buffer 各通道的**精确性不同**

`dump_gbuf=N`（N = `kGBufferAttachments` 下标）与 `attachment=object_id` 的落盘路径
（`llm_debug_impl.cppm::toRgba8` 逐 case 实测）：

| 源 | RT 格式 | 落盘处理 | 精确性 | 能当什么判据 |
|---|---|---|---|---|
| `gbuffer_albedo` (0) | `rgba8_srgb` | **原样 memcpy** | 🔴 **逐字节精确** | **零阈值逐字节对拍** |
| `gbuffer_metalrough` (3) | `rgba8_unorm` | 线性 clamp（值域本就 0..1） | 🔴 **逐字节精确** | **零阈值逐字节对拍** |
| `object_id` 附件 | `r32_uint` | raw id 无损编进 RGB24 | 🔴 **整数无损** | **零阈值逐字节对拍**（覆盖归属） |
| `gbuffer_normal` (1) | `rg16_snorm` | oct (x,y)∈[-1,1] → 8bit | ⚠ 8bit 有损 | 只能做**形态**判据（§11.2） |
| `gbuffer_position` (2) | `rgba32_sfloat` | **世界坐标 clamp 显示** | ❌ 不可比对 | 弃用（要精确位置走别的路） |
| 最终截图 | swapchain sRGB | — | 8bit | 用户视角，形态判据 |

**两条直接结论**：

1. 🔴 **"整块错"根本不需要判** —— 少画一个簇 / 材质接错 / 覆盖归属错，在 albedo、metalrough、
   object_id 这三条**逐字节精确**的通道上就是**精确不等**，零阈值、零解释。
   **`sha256sum gbuffer_0_A.png gbuffer_0_B.png` 就是判据本身。**
2. **"边缘差异可接受"只在两条有损通道上成立**（normal 与最终图像）——因为 UV 缝顶点的法线/切线
   差异本来就只出现在缝的两侧若干像素内。⟹ 需要一条**形态判据**，见 §11.2。

### 11.2 形态判据：腐蚀测"有没有内部"（imgqa `diffblock`）

用户口径"UV 缝边缘有差异我能接受，整块不行"翻译成几何语言 = **差异集是否处处薄**。
薄 = 没有内部 = 腐蚀后消失。

```
M = { p | |luma(A) − luma(B)| ≥ τ }            τ 默认 = 1 个 sRGB 量化步长（并报 τ=0 的计数）
U = area( erode( M, 圆盘半径 r ) )              默认 r = 2
判据：U == 0                                     ⟺ 差异处处厚度 ≤ 2r 像素
```

- **为什么是腐蚀而不是"到边缘的距离"**：腐蚀**自洽**（只吃两张图，不需要 G-buffer 边缘图当第二
  判据源，也就没有"边缘图自己算错了怎么办"的回归），且 `r` 的语义直接可读——
  **"差异必须处处不超过 4 像素厚"**就是用户那句话的形式化。
- **必须同时输出**（让阈值是看得见的，不是藏起来的）：① 差异像素到最近非差异像素的**厚度分布**
  （p50/p90/p99/max）；② `U(r)` 随 `r = 0,1,2,4` 的曲线（若 `r=4` 才归零，说明判据被稀释）；
  ③ 每个连通域的 `area / bbox / 厚度`，按面积降序打印前 K 个（人只需看这几个框）。
- **阳性锚点（`imgqa selftest`，缺任一条本算子不许入账）**：
  (a) 沿边缘画 1px 线 → **PASS**；(b) 平坦区 20×20 实心块 → **FAIL 且报出 bbox**；
  (c) 3px 粗线 → `r=1` PASS、`r=2` FAIL（钉住 `r` 的语义，防实现把半径当直径）；
  (d) 对角 1px 阶梯线 → **PASS**（8-连通/腐蚀结构元的经典坑）；
  (e) **整幅平移 1px** → FAIL（"整块错"最真实的形态之一：几何整体错位）。
- **解释（不是判据）**：通过形态判据后，才用 G-buffer 边缘图（normal/albedo 梯度）给薄差异**归类**
  ——落在深度边 = 轮廓、落在法线边 = 缝/折边、落在材质边 = 材质边界。归类只写进报告，不参与 PASS/FAIL。

### 11.3 混淆项隔离：几何段判据天然不带阴影

★1-a1ii 只改**几何段**（G-buffer 的产出），阴影在光照段 ⟹ **G-buffer 对拍天然无阴影混淆**。
最终图像那一路则跑**两档**：`shadow.mode="csm"`（P6 验收档）与**阴影关**各一次：

- 两档都 PASS → 差异与阴影无关，收工。
- 只有 csm 档 FAIL → 差异在阴影路径。**本批不修**（阴影段走簇路径是 ★1-b 的事），
  但必须落一条账：软阴影的历史问题（用户确认：一直没彻底解决）会在这里现形，
  **不许**用"历史问题"当免检理由——要么证明与簇路径无关（门关逐位 + 门开阴影档差异 ⊆ 已知集），
  要么挂号到 ★1-b。

### 11.4 判据清单（★1-a1ii 交付必须逐条给证据）

| # | 判据 | 阈值 | 抓什么 |
|---|---|---|---|
| A1 | 门关截图 = 基线锚 `54e40665…` | 逐字节 | 旧路径一行没动 |
| A2 | 门开 `gbuffer_albedo` / `gbuffer_metalrough` / `object_id` 三张与门关**逐字节相同** | **零** | 少画簇、材质接错、覆盖归属错、UV 缝顶点位置错 |
| A3 | 门开 `gbuffer_normal` 过 `diffblock`（r=2） | U=0 | 法线/切线（含 `tangent.w` 手性）整块错 |
| A4 | 门开最终截图（csm 档 + 阴影关档）过 `diffblock`（r=2） | U=0 | 用户视角的整块错 |
| A5 | 提交三角数落到 §7.3 的 **1.08-1.30%** 量级（恒 L0 档 = 全 L0 不减，改看**簇数/顶点数**口径） | 量级 | 路径真的在走簇，不是偷偷回落到旧路径 |
| A6 | `just test` / `test-all` / `checks` 全绿 | — | 常规 |

🔴 **A2 是本批的主判据**：它零阈值、无解释空间，且直接覆盖"整块错"的全部形态。
A3/A4 只处理"有损通道上的薄差异"，也就是用户已经认可的那一类。
