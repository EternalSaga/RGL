# Multi Draw Indirect 路线（全面 indirect 化的可行性与阶段）

> **状态**：调研（2026-10-10 立，2026-10-11 加 §7 簇化转换可行性实测，2026-10-12 簇化轴立项开工，
> **2026-10-13 命令数轴开工 = §19**）。
> **两条轴现在都在做**，但**宿主与形态以 §19 为准**：命令数轴（阶段 A/B/C/D/E）自 2026-10-13 起
> 恢复推进，**起手宿主由"旧 shadow 管线"改判为"阴影段簇路径"**（改判理由与推翻条件 → §19.1）；
> 簇化轴（★0-★3）继续，编排见 §9 / §18。交付帧时间定价不变（命令数轴 = 零，见 §7.5）。
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
| 阴影段 draw | ~~**≈5368（77%）**~~ ⟹ **4517（68%）** | 🔴 **2026-10-14 实测取代推算**（口径见下方「★M-3T-c 后实测取代」块）。旧值是**推算** = `default` 6942 − `shadow-off` 臂 1574，且量在 ★18-B1（阴影段条目塌成 1 条）与 ★18-C（`meshlet.enable` 出厂开）**之前**，两件事都改了分母 |
| 模型段 draw | 1591 primitive | `docs/shader_object_组合成本与优化预留.md` §1 |
| 管线条目（现行 deferred 基线） | ~~376 / 752 句柄~~ ⟹ **479 / 958** | 启动日志 `shader_object baseline:` 行（2026-10-14 本机出厂档）。旧记 376/752 = 2026-10-09，**簇路径条目（mesh 变体 + ★18-B1 阴影簇条目）上线前**的数；口径权威 = 日志本身，别在本文件追平 |
| 模型条目去重数 | 312 | 同文件 §2.5，**那次是 forward + picking 临时开的 2026-09-28 运行** ⟹ deferred 下的真实桶数须重测。⚠ **至今未重测**：启动日志只打**总数**（479），逐段去重数没有读数口 ⟹ 本行仍是 2026-09-28 那个形态的数，**别拿它当 deferred 现行值** |
| 阴影条目去重数 | ~~≈30~~ ⟹ **1** | 🔴 ★18-B1 已把阴影簇条目塌成**全场景 1 条**（启动日志 `main_meshlets.cppm:464`：「条目下标 474 覆盖 prims=1591（全场景 1 条目，簇 buffer 全局单例）」）——去重键 = vertexSlot 的 ≈30 是**旧 shadow 管线**的形态，簇路径阴影段不再有条目轴 |

### ★M-3T-c 后实测取代（2026-10-14，本机 STRIX_HALO，出厂档 + `--set diagnostics:profiler=16 --frames 20`）

| 档 | `draw.count` | `vkcmd.count` | `mdi.count` | `mdi.rows` | `mdi.tasks` |
|---|---|---|---|---|---|
| 门关（`meshlet.mdi_shadow=false`，出厂默认） | 6601 | **19994** | 0 | 0 | 0 |
| 门开（`mdi_shadow=true`） | 6674 | **6452** | 3 | 4590 | **4517** |

三条读法（第 3 条是口径自洽的交叉验证，不是新数）：

1. **阴影段 draw ≈ 4517 = `mdi.tasks`**，且这是**结构式**不是巧合：逐条形态对不可见候选直接
   `continue`（`emitShadowCasters`），⟹ 逐条 draw 数 = 可见候选数 = Σ `gcX` = `mdi.tasks`。
   占门关全帧 6601 的 **68%**，取代上面「77%」那格。
2. **`vkcmd.count` 19994 → 6452（−68%）** = 命令数轴在阴影段的实际收益，与 §1.3「交付路径省不到
   帧时间」不矛盾（省的是 CPU 侧命令录制，不是 GPU）。
3. 🔴 **两档 `draw.count` 之差 6674 − 6601 = 73 恰等于 `mdi.rows − mdi.tasks` = 空行数**（不可见
   候选在 MDI 形态占一行、在逐条形态整条不发）⟹ `draw.count` 的「逻辑行」口径（§19.2 D5）与
   行表锚**互相对上**，这条计数器不是自证。

⚠ **MDI 一次调用只能有一份 shader state**，所以"collapse 到 1 条"不可能：收益上限 =
**去重后的条目数**，不是 1。~~阴影段 ≈ 30×4 = 120（对 ~5368 draw）~~ ⟹ **阴影段实测 = 3 条 MDI
调用**（每生效级联一条，`mdi.count=3`），旧推算作废；**模型段 ≈ 312 条 MDI 调用（对 1591 draw）
仍是推算**（★M 线只做了阴影段宿主，§19.1）。全帧命令数：门关 19994 → 阴影段门开 6452（实测）。

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
>
> 🔴 **状态列 2026-10-14 补齐**（此前 ★0/★1/A/D 全落地了、表上还写"计划内"，读的人会以为
> 一条都没开工）。**"已交付" ≠ "出厂开着"**：出厂档逐行标在表里，键与默认值真相源 =
> `rgl/runtime_config.cppm` 的 meshlet 块；逐批交付记录 → §10.5（簇化轴）/ §19.x（命令数轴）。

| 阶段 | 内容 | 牵动面 |
|---|---|---|
| **0 spike** ✅ 已交付（2026-10-10） | `tests/test_mdi_drawindex.cppm` 七条用例 + 三个探针 shader（§6）+ `multiDrawIndirect` 进 **required** 列表 | **不碰 rgl/ 渲染路径**；实跑结论见 §6.2——本线全部前提坐实，**阶段 C 的孔洞设计也被实测放行** |
| **★0 离线打包工具** ✅ **已交付**（2026-10-12；★0.5-a/b/c 三条判据批同批落地） | `meshlet_pack`：glb → 簇资产（顶点池 + 8bit 簇索引 + 逐簇包围体 + DAG + 层级）；**构建期产物，不进 git**；`--verify` 自检（L0 三角多重集 = 输入、索引可解回、包围体合法） | 依赖 = vcpkg `meshoptimizer` + vendored `clusterlod.h`；**引擎不链 meshoptimizer**（只读打包产物）；产物缺失 → 回落现行路径（⟹ 未打包资产零风险） |
| **★1 簇路径 + 簇剔除** 🔶 **主体已交付**（★1-a0…a2-i + ★1-b；逐批状态与出厂档 → §10.5。可行性实测 §7.1-7.4） | 门 = "该资产有簇资产吗"；新 mesh shader 读簇描述取 ≤128 顶点；层级遍历 → 逐簇可见性；**选级恒 L0** | **G-buffer 提交几何 42.6%→1.30%（33×）、阴影段 36.3%→11.3%**（§7.3/§7.4，离线模拟）；门开破逐位（P5 已放行），门关逐位不变 |
| **★2 DAG cut 选级（相机侧）**（计划内） | 屏幕误差选级（`boundsError` 公式，阈值 = 像素） | 实测在交付机位**免费**（§7.3：与纯簇剔除同量）⟹ 价值在远机位与阴影侧，不在当前机位 |
| **★3 DAG cut 选级（阴影侧）**（计划内，**前置未解**） | 按图集纹素预算选级（各级联各自的纹素） | 🔴 **现成 group 级 cut 实测拿不到收益**（§7.4 带作用域与推翻条件）：先查米级误差离群簇 + 试小 `partition_size` + 逐簇选级 |
| **A 阴影段** ✅ **已交付**（★M-1b + ★M-3T-c，2026-10-13。🔴 **出厂关**：阴影段实测 GPU **净亏** +0.35ms → §19.18。**去向已定案**（2026-10-14 用户拍板：★M-3 / ★M-4 **挂起**，重启条件 → §19.20））（P1，命令数轴）🔴 **2026-10-13 宿主改判 → §19.1** | ~~`shadow.mesh.slang` / `izb_mask.mesh.slang` 走 §3 公式；按 (cascade, vertexSlot) 分桶~~ ⟹ **改为 `shadow_meshlet.mesh.slang`（★18-B1 已把阴影段条目塌成全场景 1 条 ⟹ 分桶维度天然消失、无需测桶数）**；`executePassDraws` 阴影分支走 MDI 分支 | **隔离面**（原判断仍成立且更强）：该 shader 自带 push 块、不 import `material.slang`，阴影 push 不在 5 条反射管线里 ⟹ **不触发条目重建、不碰 DGC 布局、不经 cullDriver**（`shadowCalls` 是独立向量，候选序对账零感知） |
| **B 模型段**（命令数轴） | 同上，但牵动 `material.slang` 的 DrawArgs 语义 + DGC 序列 `@8` 的解释（按 §3：不动布局）；要重测 deferred 下真实桶数 | 牵反射（`just reflect` + 提交生成物） |
| **C 压紧上 GPU**（命令数轴） | compute 剔除直写 args/params（atomic 紧凑），消掉 CPU 读回 | 改 `cull.comp.slang` 输出布局 + 对账契约（§5.3） |
| **D task shader** 🔶 **阴影段宿主已交付**（★M-3T-a/b/c：`shadow_meshlet.task.slang` 上线、`NO_TASK_SHADER_BIT` 改成只在无 task stage 时带；「per-draw 剔除」由 `gcX=0 ⟹ 零 task` 实现）（P2 后段） | 路径 B：groupCount 也进 params、`DispatchMesh(0)` 做 per-draw 剔除 | 去 NO_TASK bit + execution set 加 task 槽（2j/2j+1 → 3 槽约定要重算）。⚠ **没有 ★1 就没有可剔的单元**——台账那条"task 给派发权不给几何单元"的结论没被推翻，被推翻的是它下面"连续三角形无邻接性"的论据（§7.6） |
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
- `GUARD:` ~~待办~~ **判定为不可机械化，降级 `DOC:`**（2026-10-14 收账，别再找这条待办）——
  `docs/DESCRIPTOR_HEAP_SPEC.md` 坑 9 那类"参数序写反但数值相同"的假绿。
  **为什么机械不了**：静态检查能判的是"同一个变量被重复传给相邻两个形参"，而真出事那次是
  **两个不同变量换了位置**（`offset ↔ stride`），名字都不同 ⟹ 静态无从判"谁该在前"；
  要判只能要求"每个调用点的实参**值**两两不等"，那是**激励侧**约束（跑起来才知道值），
  不是可判定的语法性质。
  **现在真正在守它的三件事**（★M-1b 之后比本条挂号时强得多）：① `vkCmdDrawMeshTasksIndirectEXT`
  全项目**只有一处**调用，收在具名入口 `recordMeshTasksIndirectEXT`，形参逐一对应 vkCmd 语义；
  ② 四个实参两两不等（行步长 24 / 实测行数 1530 / 偏移 KB 级 / 命令体 12）⟹ 互换必现形，
  这条"别顺手凑成相等的数"是**用例设计约束**（同 §6.3 上面 `kCols` 从 16 改 12 的处置）；
  ③ `just test-all` 的设备侧自证回路 + A4 阳性对照（参数序错 = 行读错 = 图错，红在实机）。
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

- ~~**簇粒度剔除对阴影段 = 3.2× 几何下降**（36.3%→11.3%）~~
  🔴 **已被引擎内实测推翻（2026-10-12，§5.8 复核完成）**。本条当时自陈"模型盒比引擎真实盒小 ⟹
  绝对值偏乐观，3.2× 待引擎内复核"——复核结果：**那个比值就是模型盒偏小造出来的**。
  实测（默认机位，逐级联投影体数 + 包围球被级联盒子**严格包含**的占比）：

  | 级联 | 投影体数 | 球被严格包含 | 占比 | GPU (ms) |
  |---|---|---|---|---|
  | c0 | 341 | 255 | 74.8% | 0.832 |
  | c1 | **1591**（= 全部候选） | 1567 | **98.5%** | 1.959 |
  | c2 | 1571 | 1545 | **98.3%** | 1.901 |
  | c3 | 1524 | 1486 | **97.5%** | 1.591 |

  **包含性论证（不是跑分）**：投影体包围球被级联盒子严格包住 ⟹ 其**全部簇**必在盒内 ⟹ 簇粒度
  对它一个都剔不掉。故簇剔除最多只能碰"未被包含"那部分，按 GPU 时间加权、且假设边界投影体几何
  能被**全部**删掉（现实中只能删朝外的几簇，所以这是骗人的上界）：
  `0.832×25.2% + 1.959×1.5% + 1.901×1.7% + 1.591×2.5% ≈ 0.31ms = 阴影 6.28ms 的 5%`。
  ⟹ **阴影段的回报不在"剔更多几何"，在"少画几遍"（债-82 时序摊销）与"换粗级几何"（LOD）**。
  根因：`casterInShadowVolume`（`scene_context.cppm:464`）是**光空间 box** 测试且做得没问题，
  远级联盒子本来就罩住大半个场景 ⟹ c1 一个不剔是**物理正确**的，不是剔除器写坏了。
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
| **★0-b1 读取侧** ✅ **已交付**（2026-10-12） | `rgl/meshlet_asset.cppm` + `_impl`（rlbase，**纯 CPU 无设备**）：产物字节流 → 经校验的只读视图；门 `runtime.lua meshlet.enable` **默认 false**（⚠ 本行是**当批**交付记录；🔴 现行默认 = **true**，★18-C 于 2026-10-12 翻的 → §18.11）；`tests/test_meshlet_asset.cppm` 24 用例 | `just test` 304 全绿（无设备）；21 条反向用例**错误串两两不同**（可归因）；主 agent 独立变异两条（8bit 契约判据、段对齐判据）各红 2 条且归因干净；**端到端实测：真读取器读真 184MB 产物通过**（61,518 簇 / 7,596 group / 8,699 节点 / 12 级，临时用例验完即删） | 零（不接渲染路径） |
| **★0-b2 构建期接入** ✅ **已交付**（2026-10-12） | `cmake/RglMeshlets.cmake`（构建期生成，清单驱动，configure 期 git-lfs 防御）+ 装配层加载（`MeshletAssetStore` 与 `AssetTable` 同层） | 门关截图 = 基线锚 `54e40665…`；**门开（真加载 184MB 产物，日志实证门是开的）= 同一哈希** ⟹ 加载路径惰性；第二次 `just build` 不重跑；坏产物 WARNING 回落不砖引擎 | 零（门关 = 一行都不走） |
| **★0.5-a CPU 参考剔除器** ✅ **已交付**（2026-10-12） | `rgl/meshlet_cull.cppm`（rlbase 纯函数）：**线性扫描**全部簇的视锥剔除（故意与 GPU 层级遍历算法不同 = 独立第二实现）+ 21 条契约用例 | 六面各自独立负责（含"摘掉该面就复活"反向锚点）、保守边界取等号、半径缩放三轴各一臂、三角合计 ≠ 条数；主 agent 质检变异 MC（球心乘法次序）**注入时全绿 → 暴露真覆盖缺口 → 补两条含旋转用例后红**；**统计锚：真产物交付机位 1.08% vs spike 1.30%（两条独立实现互证）** | 零 |
| **★0.5-b 图像侧仪器** ✅ **已交付**（2026-10-12；b1 = imgqa `diffblock` 形态判据、b2 = 共面双臂判据，双臂当场抓出 §15 那个 packer 切线 bug） | 簇路径 vs 现行路径的**图像对拍模式** + 差异统计与**统计地板**（类比 `regress floor`：后续批次的差异必须 ⊆ 地板才可归因）+ imgqa 可见性归因接入 | 地板在"门开不剔 + 恒 L0"档建立；人为剔掉一块可见几何必须红（变异红线） | 零 |
| **★1-a 簇路径 + 簇剔除** ✅ **已交付**（2026-10-12/13，拆成 ★1-a0…a2-i 六步逐批入账 → §10.5） | 新 mesh shader（读簇描述 → ≤128 顶点 → 变换）+ GPU 层级遍历 + 逐簇可见性 + 压紧；**选级恒 L0** | 用 ★0.5 仪器判；G-buffer 提交几何应落到 §7.3 的 **1.08-1.30%** 量级（离线模拟 + 引擎 CPU 参考剔除器两个数，作对照不作硬判据）。⚠ 实际交付形态与本页原写**不同**：没做压紧（★1-a2-i 定案走标志位早退，§17.2），且"逐 prim 提交"改成了**原位取代**（§16） | 中，可逐块归因 |
| **★1-b 阴影段接入** ✅ **已交付**（2026-10-12，走 ★18 线 → §18.8 / §18.9） | 阴影 mesh 走簇路径（**只 CSM 档**，P6） | 阴影段 GPU 时间对照 6.28ms 基线；**非默认 cull 档（`gpu_sync`/`gpu_lagged`）单独冒烟 + syncval**（AGENTS 红线：默认档不提交 cull CB） | 中高 |
| **★2 相机侧选级** | 屏幕误差 DAG cut（`boundsError` 公式，阈值 = 像素），🔴 **必须与剔除耦合** | 误差闭式上界不超 T px；**且提交几何不得多于纯 L0 剔除**（§7.3 重测：不耦合时 @1px 反而**多 64%**，旧结论"交付机位免费"已推翻） | 中 |
| **★3 阴影侧选级** | 按图集纹素预算选级 | 🔴 前置 = §7.4 group 级保守问题先破（查米级误差离群簇 / 试小 `partition_size` / 逐簇选级） | 高 |

**★0-a 交付记录（2026-10-12，主 agent 亲验）**：产物 = `tools/meshlet_pack/`（16 文件，最大 292 行）
+ `third_party/{meshoptimizer,vcpkg-ports}/` + `Justfile`（overlay ports / build-tools 各一行）
+ `tools/checks/config.py`（把新工具纳入样式面，**零基线**不许登 baseline）。四条自检 =
L0 三角多重集 / 8bit 索引可解 / 逐簇包围体 / **DAG 结构不变量**。主 agent 独立做两条变异
（**形态与 worker 所验不同**）：**绕序反转** → ①② 红（证明 ① 的"最小顶点在前循环旋转"规范化
**没有把反转一起吞掉**——这是这条断言最容易变成摆设的地方，必须专门验）；**包围球缩 1%** → ③ 红。
`cp` 回滚后产物 sha256 回到基准（当时 `fa189554…`；★0.5-b2 切线修复后基准 = `317ae76e…`，见 §15.5）。

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

> **状态列是 2026-10-14 补的**（此前只有 a0/a1i 打了 ✅，后面四步全落地了表格却没改，
> 读的人会以为 ★1 还没开工）。🔴 **出厂档位一并写在这里**——本线的"已交付"和"出厂开着"
> 是两件事，键与默认值的唯一真相源 = `rgl/runtime_config.cppm` 的 meshlet 块。

| 步 | 内容 | 判据 |
|---|---|---|
| ★1-a0 ✅ **已交付**（2026-10-12） | 4 个簇 buffer 上传 GPU（`meshlet_geometry`，rlgfx） | 门关逐位不变；门开日志报 buffer 大小；`just test-all` 的 gpu 套件加一条**设备侧回读校验和**（合成小产物，不依赖 184MB 文件；无设备必须 FAIL 不 SKIP） |
| ★1-a1i ✅ **已交付**（2026-10-12） | scene data 新增 `ObjectClusterRange` 尾区 + 逐 prim 簇区间扫描（四条不变量当探测器） | 门关逐位不变 + 门关日志零 meshlet 行；门开日志三数 = §10.1 实测（1591/61518/27487）；扫描的四条不变量各配反向用例 |
| ★1-a1ii ✅ **已交付**（2026-10-12，形态三次改道：α 影子叠加 → α″ 去索引发射 → **β-1 原位取代**） | 新 mesh shader + 条目 + 逐 prim 提交 → 最终形态 = **原位取代旧 draw**（§16），逐三角手性精确（§12/§13 的接口上限逼出来的形态） | 判据面 = §11 全套（`diffblock` 形态判据 + 闭式解）；β-1 实测交付 GPU −4.6%（§16.1）。🔴 **出厂关**：`meshlet.switch = false`（开了影子叠加要清空，互斥在装配层 warn） |
| ★1-a2 🔶 **i 已交付 / ii 挂号**（2026-10-13） | **★1-a2-i** = 逐簇视锥剔除（compute 逐 L0 簇保守测试 + mesh shader 标志位早退，§17.2 定案"不做压紧、不做 indirect"）；**★1-a2-ii**（压紧 + GPU 写 groupCount）= §17.3 挂号未做 | 交付记录 → §17.4。🔴 **出厂关**：`meshlet.cull = false`，且**依赖 `enable && switch`**（只作用于已被簇路径接管的 draw；单独开 = 无消费者，装配层 warn 出声） |
| ★1-b ✅ **已交付**（走 ★18 线，2026-10-12） | 阴影段走簇路径 = **★18-B1**（阴影簇条目，全场景 1 条）+ **★18-B2**（接线）+ **★18-A/B** 的 uniform 选级 D（`shadow_lod`）。原表写的"只 CSM 档"仍成立（P6 红线：验收只跑 CSM） | 交付记录 → §18.8 / §18.9。🔴 **出厂开**：★18-C 把 `meshlet.enable = true` + `shadow_lod = 5` 一起翻成出厂档（2026-10-12 用户拍板）⟹ **出厂态已不是"门关态"**，"门关逐位不变"的验收命令必须显式带 `--set meshlet.enable=false --set meshlet.shadow_lod=-1` |

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
| `object_id` 附件 | `r32_uint` | raw id 无损编进 RGB24 | 🔴 **整数无损** | ⚠ **本批不用**：它要 `diagnostics.picking=true`，而那条门**会在编译期追加 object_id pass**（改渲染图）⟹ 拍出来的不是交付那一档。且它给的是**归属**不是**检出**（少画一块已被 albedo 抓到）。要拍走 RPC（`attachment` 字段，无配置键），归 ★1-b |
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
  (c) 3px 粗线 → **`r=1` FAIL、`r=2` PASS**（`U==0 ⟺ 厚度 ≤ 2r`：3 > 2·1 必红、3 ≤ 4 必绿）。
      ⚠ 本条**首稿写反了**（写成 r=1 PASS / r=2 FAIL），是子 agent 发现公式与任务文本矛盾后
      **停下来上报**而不是就地绕过——按错的文本实现，selftest 与算子不可能同时成立；
      修正后的 (c) 才真钉得住「`r` 是半径不是直径」（直径错实现会在 r=1 错误地绿）；
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
| A2 | 门开 `gbuffer_albedo`(0) / `gbuffer_metalrough`(3) 两张与门关**逐字节相同**（object_id 见 §11.1 该行注记，本批不做） | **零** | 少画簇、材质接错、UV 缝顶点位置错 |
| A3 | 门开 `gbuffer_normal` 过 `diffblock`（r=2） | U=0 | 法线/切线（含 `tangent.w` 手性）整块错 |
| A4 | 门开最终截图（csm 档 + 阴影关档）过 `diffblock`（r=2） | U=0 | 用户视角的整块错 |
| A5 | 提交三角数落到 §7.3 的 **1.08-1.30%** 量级（恒 L0 档 = 全 L0 不减，改看**簇数/顶点数**口径） | 量级 | 路径真的在走簇，不是偷偷回落到旧路径 |
| A6 | `just test` / `test-all` / `checks` 全绿 | — | 常规 |

**A2 的采集流程（2026-10-12 已在 ★1-a1i 构建上演练通过，直接照抄）**：
逐通道各跑一次（`dump_gbuf` 档下引擎忽略 `llm_shot.file`，出图名写死 `screenshots/gbuffer_N.png`）：

```bash
for ch in 0 3; do
  python3 tools/smoke.py shot --name g${ch}_off --aa off --set diagnostics:dump_gbuf=$ch
  python3 tools/smoke.py shot --name g${ch}_on  --aa off --set meshlet.enable=true --set diagnostics:dump_gbuf=$ch
  # 各自把 build/rgl/screenshots/gbuffer_$ch.png 存到不同目录再比（同名的两次跑会互相覆盖）
done
```
演练读数：ch0 `ea84ed5e…` / ch3 `7df898fd…` 两臂**逐字节相同**（当时门开不画 ⟹ 本应相同，
这条演练验的是**流程**不是渲染）。

🔴 **A2 是本批的主判据**：它零阈值、无解释空间，且直接覆盖"整块错"的全部形态。
A3/A4 只处理"有损通道上的薄差异"，也就是用户已经认可的那一类。

⚠ **A3/A4 的一条已知盲区（`diffblock` 自己写在判据契约里，别当没看见）**：自然图像**整体平移 1px**
只产生 1-2px 的薄带 ⟹ 形态判据**放行**。所以"几何整体错位"这类整块错**只能由 A2 抓**，
不许拿 `diffblock` 单独当平移闸（合成棋盘靶才会红，那条锚点钉的是仪器不是自然图）。

### 11.5 挂号（★1-a1ii 之前不必做，但别忘）

- **`imgqa selftest` 的断言条数护栏只是文档约定，没有机器执行**：`selftest.h` 里那条计数
  2026-10-12 当天就被加断言改漂过一次（口径还没记录，两个人数出 339/343/344 三个数）。
  已把**唯一口径命令**写进注释；机械化 = 挪进 `tools/checks/` 一条规则（做不掉就先记债）。
  🔴 **2026-10-14 已记债 = 总账 债-117**（挂号到此为止，本批未做；2026-10-14 按该口径实测重数
  = 402，与注释写死的值一致 ⟹ 目前没漂，但"没漂"是运气不是护栏）。⚠ 做的时候**别选"运行期
  计数 vs 写死常量"那条看着最省的设计**：运行期计数会把循环内断言按执行次数计，而循环上界一旦
  随数据变（表长、机位数），护栏就会在合法改动下变红——静态调用点口径虽然土，但它**与激励无关**。
- **报告面要有自己的判据**（本轮两条变异存活换来的）：判据 U 对了不代表报告对——
  分位数统计域漂成全图、连通域排序翻转，两条都不动 U，**全绿而把人引到错的框上**。
  已补 `testDiffBlockReportSurface`；推广：**任何"给人看的定位输出"都当作判据面对待**。
## 12. ★1-a1ii-α 落地与 tangent.w 的接口上限（2026-10-12）

### 12.1 α 的形态选择：影子叠加，把验收留在零阈值

α **不替换**旧路径，而是在同一几何 pass 的 opaque 段之后、用**同一 objectIndex**把 L0 簇再画一遍。
于是"差异多大算可接受"这道判断题**本批完全不需要**——同一几何画两遍 ⟹ 像素必须一位不变。

🔴 但叠加自带一个假绿：**绕序/剔除错 ⟹ 第二遍一个像素都不画 ⟹ 图像同样"逐字节不变"**。
所以阳性对照（`meshlet.debug`：clip.z×0.999 + 翻法线）与位比判据**同等重要，缺一即不成立**。

实测（主 agent 独立复现）：A1 两臂截图 sha256 均 = 基线锚 `54e40665…`；阳性对照与门开非 debug 臂差
**315,802 px = 34.27%**（单一连通域 315,699，bbox 948×572）⟹ 真在画；A2 `gbuffer_albedo`/`metalrough`
两臂**逐字节相同**。

### 12.2 🔴 逐顶点接口装不下逐三角量——近似被量化，β 前必须换 per-primitive

我 §10.3 写的"逐三角重算 `tangent.w`"**必要但不充分**：mesh shader 的**顶点**输出接口
（`ModelVertexOutput.tangentOS.w` 被光栅器插值）决定了共享顶点只能带一个 w。子 agent 发现并**上报**
（取"本簇内第一个引用它的三角形的 w"）而不是静默绕过。

误差面由 `tools/meshlet_w_mixed.py` 精确量出（三条自检锚：L0 三角和 == `l0TriangleCount`、
负手性 == 2,322,319/2,832,120、float32 忠实性；**锚点已变异验证可失败**——本工具首版就是被锚 B
抓出步长口径错）：

| 读数 | 值 |
|---|---|
| L0 簇（`refined == -1`） | 27,487 / 61,518，三角和 **恰 = 2,832,120** |
| 负手性三角 | 2,322,319 = **82.00%** |
| 🔴 **混合手性顶点** | **14,347 局部槽 / 14,076 全局顶点 / 1,582 簇（5.76% 的 L0 簇）** |
| 占 L0 顶点引用 | **0.1689%** |
| float32 精度风险 | 27,956 条退化三角中 **0** 条两种精度手性相反 ⟹ 结论对 shader 成立 |

⟹ **β 之前换成 per-primitive 输出**。可行性已实测（slangc 2026.18.3）：
`out primitives POut prims[N]` 可编译，SPIR-V 带 `OpDecorate … PerPrimitiveEXT`，
`spirv-val --target-env vulkan1.3` 通过。爆炸半径比担心的小：**只有 4 个 FS 消费 TBN**
（`deferred_geometry` / `model_light` / `model_pbr_ibl` / `model_vegetation`），
3 个 mesh shader 补 `prims[]`，`normalmap.slang` 改一处签名。
🔴 **两条 mesh 路一起改**（model 路的 w 本就逐三角恒定 ⟹ 语义等价 ⟹ **门关必须仍逐位 = 基线锚**，
这就是该契约改动的零阈值判据）。RADV 运行期接受度是剩余未知量，VVL 会立刻点名。

### 12.3 `format.h` 的 L0 判据口径（2026-10-12 复核改写）

引擎口径 = `refined == -1`（`isL0Cluster`）——它是 `MeshletDiskCluster` 注释里 L0 的**定义**，
且不依赖 group 二次跳转。**两条口径在真产物上是同一个集合**（2026-10-12 直接读产物本体复核）：
`refined == -1` 与 `groups[c.group].depth == 0` 都是 **27,487 簇 / 2,832,120 三角**，
且三角和**恰等于** `header.l0TriangleCount`（闭式锚）。
🔴 本节旧版写的「`depth==0` 只有 2,518 簇」**复现不出来**（口径方向对、那个数字错），已删；
`format.h` 同步改写。

**教训两条（都还在生效）**：

1. 真相源里每条"判据"必须有一条闭式量对上并写进注释，否则错到真正依赖 depth 语义的 ★2/§18
   选级才暴露。
2. **量产物字段的脚本自己要有阳性锚点**。`DiskCluster.refined` 是 **int32**，脚本按无符号 `I`
   读 ⟹ `== -1` 恒假 ⟹ 命中 0，而"0 命中"读起来像"这条口径不成立"而不是"我的脚本坏了"
   （主 agent 2026-10-12 复核时亲手重犯同一个错，靠 `header.l0TriangleCount` 这颗闭式锚才发现）。
   `GUARD: rgl/meshlet_lod.cppm` 的 **invariant 3** 把两条口径的一致性钉成机器判据（分家即报错
   点名 prim）——散文断言不再是唯一防线。
## 13. per-primitive 撞上游缺口 → 改走"簇路径去索引"（2026-10-12 拍板）

### 13.1 α′ 尝试与它撞的墙（结论：slang 发不出 FS 侧装饰，源码级确认）

α′ 按 §12.2 做 per-primitive 手性输出，实现全绿，但**主判据（门关逐位 = 基线锚）红**。根因不是设计错：

- mesh 侧 slang **能**发 `PerPrimitiveEXT`（`out primitives` 三件套限定词）；
- **FS 侧 slang 发不出**（指**用户自定义**逐图元属性）。三重独立证据：
  1. **源码级**（`~/program/slang` @ 2026-09-12）：装饰只在 `kIROp_GLSLPrimitivesRateDecoration` 时发出
     （`slang-emit-spirv.cpp:18299` / `:20090`），该装饰**唯一**添加点是
     `declarator->flavor == meshOutputPrimitives`（`slang-ir-glsl-legalize.cpp:1066`），而
     `GlobalVaryingDeclarator::Flavor` 枚举**只有输出侧三种**（`array` / `meshOutputVertices` /
     `meshOutputIndices` / `meshOutputPrimitives`，`slang-ir-glsl-legalize.cpp:994`）
     ⟹ **输入侧不存在这条路径**。
  2. **行为级双版本对拍**（主 agent 亲做，脚本 `/tmp/opencode/pp_probe.py`，mesh 侧作阳性对照）：
     构建 pin 的 **2026.16** 与系统的 **2026.18.3** 字符串集完全相同、行为完全相同；FS 侧
     `nointerpolation` / `[[vk::perprimitive]]` / `[perprimitive]` / `[perprimitiveEXT]` / 裸语义
     **五种写法装饰数全为 0**（mesh 侧同脚本能查到装饰 ⟹ 判据本身有效）。
     🔴 **且 `[[vk::perprimitive]]` 只报 `warning[E31000]: unknown attribute`、rc 仍为 0**
     ⟹ 只看退出码的构建会**静默产出错着色器**。
  3. **上游**：issue **#7019**（2025-05 开，至今 open，里程碑从 2025Q3 漂到 2026Q4）正文写的复现形态
     与我们逐字相同；修复 PR **#8821** 仍 open 且 reviewer 明确 "the change to the spirv-embedder is
     missing, so this won't work yet"。DXC 那条 `[[vk::ext_decorate(...)]]` 绕法在 slang **根本不存在**
     （全树 `ext_decorate` 零命中）。
- 缺该装饰的后果不是"精度略差"而是**读未写过的 location**（mesh 把 location 4 当逐图元写，
  FS 当普通插值输入读）⟹ 图像 `6dee4f11…` ≠ 基线。子 agent 用 SPIR-V 后置补装饰验证过：
  **补上后门关逐位 = 基线锚**、VVL 干净 ⟹ 设计与旧路径语义等价这条**已被证明**，只是编译器给不出产物。

### 13.2 三条路的取舍（为什么既不修补也不接受近似）

| 路 | 忠实性 | 代价 | 裁决 |
|---|---|---|---|
| A 构建期 SPIR-V 后置补装饰 | ✅ 精确 | 改**编译器产物**（仓库新物种，三驱动都要重验；源码里看不见） | 缓做，留触发条件 |
| B 接受逐顶点近似 | ❌ 0.1689% 顶点引用错 | 零 | 🔴 **拒**：β 的图像地板会烘进一个已知错，以后每次 diff 都有解释不了的成分 |
| C **簇路径去索引发射** | ✅ 精确 | 顶点输出 2.85×（**但等于现行路径**） | ✅ **做** |

**C 的关键事实（本轮量出来才看见的）**：🔴 **现行 mesh 路本来就是去索引的**
（`model.mesh.slang` 每 group 10 三角 / 32 顶点 = 每三角 3 个私有顶点）。簇路径若也去索引发射，
顶点输出 = 3 × 三角数 = **8,496,360**，与现行路径的去索引顶点数**逐位相等** ⟹
**C 相对现状零回归**，只是放弃了"共享顶点"这个**我们本来就没在用的优势**。
收益仍全部来自逐簇剔除（§7.3 的 33×），与顶点发射方式无关。

代价实测：L0 簇中位 **121 三角**、max 128，而 `maxMeshOutputVertices = 256` ⟹ 每任务 ≤ 85 三角，
**71.2% 的 L0 簇需要拆成 2 个任务**（任务数 27,487 → 54,974 上界；空任务早退）。

**C 的额外红利（对 β 价值极大）**：去索引后簇路径的属性语义与旧路径**结构同构**
（都去索引、都逐顶点携带逐三角恒定量）⟹ **β 的验收判据可以从"diffblock 形态地板"升级成
"门开逐位 = 基线锚"的零阈值判据**。位置同位、属性同位、手性现在也精确 ⟹ 没有理由不同。

**A 的重开触发条件（写死，别靠记性）**：当簇路径的**顶点发射**成为可测瓶颈（`just cpu` /
`just analytic` 归因到 mesh 阶段顶点输出），或 slang 上游实现 FS 侧 `perprimitive` 时，
回到 A；届时 §13.1 的三条源码证据与"补装饰后逐位 = 基线"这条实测就是现成的立项材料。
🔴 **但顺序变了**：真到那一天，**先试 §13.4 的路线 D**（`SV_PrimitiveID` 全局下标 + 全局属性表），
它不碰编译器产物；A 降为 D 也不可行时的备胎。

### 13.3 α′ 的产物处置

α′ 的 9 文件契约改动**全部 revert**（旧路径一行不动 = 位锚零风险）。它不是白做：
它证明了 per-primitive 路线可行且旧路语义等价，并把上游缺口钉死到源码级——这些是 §13.1 的来源。

### 13.4 纠正：`SV_PrimitiveID` 是一条**今天就能用**的逐图元通道

§13.1 的结论对**用户自定义**逐图元属性成立，但"此路只能等上游"这句**是错的**。

**① 先记我自己踩的探针陷阱（后来者必踩）**：slang 只在 system-value 语义写成**独立入口参数**时
才映射成 SPIR-V 内置量；写成 **struct 字段**时退化成普通 `Location`。第一版探针把
`SV_Position` / `SV_PrimitiveID` 塞进 `struct FIn`，得到 `SV_Position → Location 0`
（而非 `BuiltIn Position`），**差点据此得出"slang 连 SV_Position 都不映射"**。改独立参数后全对：

| 独立入口参数写法 | 编译出的装饰 |
|---|---|
| `float4 pos : SV_Position` | `BuiltIn FragCoord` |
| `int primitiveId : SV_PrimitiveID` | `BuiltIn PrimitiveId` + `Flat` |
| `bool ff : SV_IsFrontFace` | `BuiltIn FrontFacing` |

旁证：项目**本来就在用** `SV_IsFrontFace`（`deferred_geometry` / `model_light` / `model_pbr_ibl` /
`model_vegetation` 四处，全是独立参数形态）且生产正常。

**② 于是 `SV_PrimitiveID` 两端都通**（用项目原样参数 `-profile sm_6_6+spirv_1_6` 亲验 SPIR-V）：

- mesh 侧 `struct POut { int primitiveId : SV_PrimitiveID; … }` 写进 `out primitives`
  ⟶ `OpDecorate %gl_PrimitiveID BuiltIn PrimitiveId` **+** `PerPrimitiveEXT`；
- FS 侧独立参数 `int primitiveId : SV_PrimitiveID` ⟶ `BuiltIn PrimitiveId`
  ——**它是系统值，不需要那个我们拿不到的装饰**。

⟹ **路线 D**：mesh 把**全局逐图元下标**写进 `SV_PrimitiveID`，FS 读出来索引一张**全局逐图元属性表**。
它只携带一个整数、任意数据靠查表得到 ⟹ **不受上游缺口影响**。
🔴 **需要的产物侧改动**：现有 `ClusterHeader.primBase` 是**该簇首 primitive 在本 primitive 顶点段内
的相对偏移**（shader 里 `vertPool[firstVertex + primBase + localPrim]` 就是它），**不是全局图元序号**；
D 要的是打包期新分配一个全局 per-primitive 序号（`DiskPrim` 加一个字段 + 全局计数即可，
2,832,120 个图元 → 4B/个 ≈ 11MB 索引面，属性表另算）。
🟡 **未验**：驱动（RADV）是否真采用**写入值**而非自动顺序编号——要一次实机实验才能定案，
别把上面这张表读成"运行时也已验证对"。

**③ 对既有裁决的影响**：

- **不改本轮结论**：C（去索引）已落地、相对现状零代价、实测 −4.6% GPU；D 要为 0.1689% 的
  UV 冲突面加一张全局表 + 每片一次查表。**C 仍是对的**。
- **但 §13.2 的"A 的重开触发条件"要改**：将来真需要精确逐图元数据时**先试 D，别先去补 SPIR-V**
  ——D 不碰编译器产物，A 的"仓库新物种 + 三驱动重验"代价直接消失。
- **doubleSided 正解（§15.3 缺口 2）的着色器半边今天就有**：`SV_IsFrontFace` 可用
  ⟹ 背面在 FS 里翻法线即可，不必在产物侧复制副本。
## 14. ★1-a1ii-α″ 去索引发射落地（2026-10-12）

只改 2 个文件（`meshlet.mesh.slang` + `render_list_impl.cppm` 的 taskCount），旧路径一行未动。
每任务 ≤ **85** 三角（3×85 = 255 ≤ `maxMeshOutputVertices` 256），**每簇固定 2 槽**
（`cluster = base + (groupID.x>>1)`、`half = groupID.x & 1`，空槽早退）；
每三角形发射 3 个私有顶点、三个都带**该三角形自己的** `tangentOS.w` ⟹ 插值常量 ⟹ 精确。

主 agent 独立复现：A1 两臂 = 基线锚；A2 `ch0 ea84ed5e…` / `ch3 7df898fd…` 逐字节同；
阳性对照 **M = 315,802 = 34.267%、单一域 315,699、bbox [332,148,948,572]**——与 α **逐位相同**
（去索引只改发射方式，覆盖集一个像素没动，这条本身就是切分没漏画/重画的证据）。

### 14.1 🔴 一条被两条变异钉死的性质：位比判据对叠加形态**全盲**

主 agent 自跑（与子 agent 的 M1/M2/M3 不同形态）：

| 变异 | A1 位比 | 阳性对照 M |
|---|---|---|
| 绕序翻转（角 1/2 交换） | 🟡 **仍绿** | 315,802 → 245,819（−22.2%）✅ 抓到 |
| 半区退化（两槽都画前 85） | 🟡 **仍绿** | 315,802 → 292,554（−7.4%）✅ 抓到 |

**这是设计而非缺陷**：叠加不可见 ⟹ 它画错也改不了像素 ⟹ 位比结构上抓不到。
⟹ **α/α″ 这一族的真判据是阳性对照，位比只是"旧路径没被碰坏"的护栏**。写下来防止后来者
把"A1 绿"当成"簇路径对了"。

### 14.2 绕序正确性的间接证据（β 前唯一的可用论证）

debug 档只把 `clip.z` 拉向相机 0.1%（相对量）。若簇路径画的是**背面**，那些像素的深度在正面之后，
0.1% 的相对量**不可能**赢下深度测试 ⟹ M 会塌到接近 0。实测 M = 34.27% 屏 ⟹
**叠加赢深度的那些像素上，簇几何与最前表面同深** ⟹ 画的是正面。β 会用逐位对拍正面确认。

### 14.3 两处口径修正与一笔账

- **"手性每三角形算一次"这句（我写的）不准确**：实现是同一三角形的 3 条线程各算一次**同一个值**。
  理由成立——`SetMeshOutputCounts` 之后活跃线程数可小于 `numthreads`，mesh stage 跨线程同步是
  发散同步 = UB；而该式是三角形三个 uv 的纯函数 ⟹ 三次求值逐位相同。**语义上仍是"每三角形一个值"**。
- 4 处注释口径（`scene.slang` / `frame_draw_list.cppm` / `scene_data.cppm` / `meshlet_ranges.cppm`）
  仍写 `groupID.x + base`，已按 `base + (groupID.x >> 1)` 重写（`scene.slang` 是反射输入，
  连带 `reflection_manifest.txt` 一起更新）。
- **28.8% 的槽是空任务**（triCount ≤ 85 的簇），任务数 27,487 → 54,974 上界；实测最大单 prim
  1,364 槽 ≪ `maxMeshWorkGroupCount.x` 65535。出口：★1-a2 接逐簇剔除后若这部分固定开销相对放大，
  换前缀和压紧。**别靠调小 85 绕**。
## 15. ★0.5-b2 共面双臂落地，并当场抓出一个产品级 bug（2026-10-12）

### 15.1 这把尺子长什么样

叠加用 `eLess` 且与底共面 ⟹ **全拒、零写入** ⟹ α/α″ 的 A1/A2 位比绿只证明"旧路径没被碰坏"
（§14.1 已用两条变异钉死）。本批利用 **`depthCompareOp` 在本引擎是动态状态**这一事实
（`shader.cppm:291`），只给叠加那几条 draw 换比较函数，造出：

| 档 | 深度比较 | 判据 | 实测 |
|---|---|---|---|
| `off` | `eLess`（现行） | 与门关逐位同（**构造性保证**：nullopt ⟹ 连一条 `setDepthCompareOp` 都不发） | ✅ = 基线锚 |
| `equal` | `eEqual` | 共面像素写回的内容与底逐位同 ⟹ 属性/材质/手性全对 | 🔴 首跑红 → 修后 **U(2)=0 通过** |
| `ne` | `eNotEqual` | 🔴 **不是判据**（见 §15.4，我的设计错） | 恒红 |
| `tint` | （SPV 变体，只把 normal/uv/tangent 写常数、**不动深度**） | 双臂的**阳性对照**：equal+tint 必须变 ⟹ 同时是 equal 臂的**非空洞证明** | ✅ 放行 31.94% 屏 |

四道护栏让"模式静默回落 off"不可能发生：未知值启动 fail-fast + 每臂 INFO 打印生效档 +
tint/debug 同真时 tint 优先并 warn + 门关日志 `grep -ci meshlet` = 0。
**为什么这么重**：主判据是"多臂哈希全同"，一旦键静默回落，所有臂照样全绿 = 整批假绿。

### 15.2 🔴 equal 臂第一次运行就抓到一个真 bug：packer 覆掉了每个顶点的切线 z

首跑 equal 臂 **M = 157,724 px = 17.114%**，最大连通域 128,038 px —— 大块，不是噪声。

**下钻的证据链**（每一步都排除一批假设，值得完整留档）：

1. **哈希不同 ≠ 三通道都错**。逐通道逐像素比（stdlib PNG 读，`/tmp/opencode/png.py`）：
   albedo 差 **64 px / 最大 13**、metalrough 差 **340 px / 最大 26** = 等同；
   🔴 **normal 差 142,289 px（15.4%），平均 5-6 个量化步长**。
   ⟹ 我据"三通道全漂"否掉子 agent 的切线假设是**错的**；它只写了"只有 normal 漂"，是对的。
2. **平均只差 5-6 步**这个量级本身是线索：不是几何错（那会是整面翻转），是**法线贴图扰动没了/变了**。
3. 池里切线**非零**、源 glb **1591 个 primitive 全带 TANGENT** ⟹ 排除"缺属性走 Frisvad 兜底"。
4. 打印原始 float：`tan = (0.7345, 0.2048, 1.0, 0.0)` —— **z 恒 1.0、w 恒 0.0**。
   全量抽样：**z 恰 1.0 = 100.0000%，w 恰 0.0 = 100.0000%**。
5. 对代码：`glb_source.cpp` 写 `reinterpret_cast<float*>(...)[10] = 1.0f`，注释写"tangent.w"。
   但 `kVertexStride` 是**字节**，转 `float*` 后下标 10 = 字节 40 = **tangent.z**；w 是下标 **11**。

**根因**：packer 逐顶点把真实烘焙切线的 z **覆写成 1.0**，而 w 留在零填充的 0。
后果 = 簇路径 TBN 的切线轴方向错 ⟹ 法线贴图扰动不同。`w = 0` 之所以没造成更大事故
（0 会把 bitangent 整个清零），**只因为 α″ 已改成 shader 端逐三角重算手性 —— 纯属运气**。

**修法两层**：
- 值：下标 10 → `kVertTangentW / sizeof(float)` = 11。修后 equal 臂 **M 157,724 → 691（0.075%）**。
- 结构：`format.h` 引入 `struct ModelVertexLayout`，所有字段偏移改由 **`offsetof` 算** +
  `static_assert(sizeof == kVertexStride)`。`float[10]` 这种写法下差一**看不出来**，
  offsetof 下**写不出来**。重构对产物 **no-op**（sha 前后同为 `317ae76e…`）= 它自己的判据。

**修后残留 691 px 的形态**：333 个连通域、最大 16 px、p50 厚度 0.96 px、**U(2) = 0**，
且全部落在 normal 通道 ⟹ 按"整块不行可以、边缘薄差放行"的既定口径**判通过**。

### 15.3 这批真正值得沉淀的三条

1. **判据要能"写回"，不能只"不改变"**。α 族的位比判据结构上抓不到叠加画错（§14.1）；
   一个动态状态开关（`eEqual`）就把"看不见"变成"逐位可验"，成本约等于零。
2. **红了先做通道级定位再归因**。"三通道全漂"和"只有 normal 漂"指向完全不同的根因，
   而 sha256 只告诉你"不同"。**归因错误比不归因更贵**——我自己就先归错过一次。
3. **产物侧的字段错，单测抓不到**（单测在引擎侧，产物是黑盒二进制）。真正能抓的是
   **渲染侧的逐位对拍**——这也是共面双臂存在的理由。哨兵放在"错能发生的那一侧"。

### 15.4 `ne` 臂是我设计错的（记下来别再来一次）

我想用 `eNotEqual` 验"位置逐位同 + 覆盖集同"，前提是"叠加覆盖集 = 旧路径覆盖集"。
**前提错了**：叠加画的是该资产的**全套 primitive，含被遮挡面**，而被遮挡面深度永远 ≠ buffer 里
那个正面的深度 ⟹ NOT_EQUAL 在被遮挡区**恒放行**（实测 29.6% 屏 = 被遮挡面积，链式写到最后画的
primitive 赢）。**叠加形态下它不可能绿。** 配置注释已就地改成"不是判据，只能人工看"并写明出口
（要救得先把叠加限制到前向可见像素）。
"位置逐位同"这条改由 **equal 臂的放行集**承担：实测 **32.74% ≈ debug 覆盖足迹 34.27%**
（差 1.5pp = 颜色恰好相同的那些像素）。

### 15.5 现在的状态

**β 的核心命题——"簇路径 = 旧路径"——已在 G-buffer 层面被证明**：equal 臂 U(2)=0、
albedo/metalrough 逐位同、normal 只剩亚 4px 边缘点。β 剩下的确实是接线。
产物 sha 基准 `fa189554…` → **`317ae76e…`**（切线修复抬的；UV 类锚点
`total=2832120 / negative=2322319 (82.00%)` 逐位不变）。
## 16. ★1-a1ii-β-1 原位取代落地：簇路径第一次接管真实渲染（2026-10-12）

### 16.1 形态与实测

`meshlet.switch`（默认 false）三态：门关 / `enable` 叠加（α 形态，判据全保留）/ `enable+switch`
= **opaque 段内 1:1 原位取代**（同 `objectIndex`、同 `pushData`、同段内位置，只换管线条目与
task 数）。取代点 = `render_list_impl.cppm::makeOpaqueCall` 一条 return。

主 agent 亲验（本机 AMD 8060S / RADV STRIX_HALO，`aa=off`）：

| 判据 | 读数 |
|---|---|
| 门关 | = 基线锚 `54e40665…`，日志 `grep -ci meshlet` = 0 |
| 🔴 **取代档 vs 门关** | **M = 65 px = 0.007%**，50 个连通域，**U(1) = 0**（全是孤立单像素） |
| 阳性对照 tint / debug | 各改 **32.19% / 32.27%** 屏（≈ 接管覆盖，非"切了个寂寞"） |
| 反证 `enable=false + switch=true` | = 基线锚 |
| 确定性 | 连跑三次同哈希 |
| VVL | 默认档 + `gpu_sync` + `gpu_lagged` 三档 errors=0 |
| draws / cmds | **6601 / 19994 两臂逐条相等** ⟹ 命令流形状未变 = 1:1 原位的直接读数 |

🔴 **性能（交付路径 no-vvl，两次独立测量）**：GPU 帧 **8.816 / 8.881 → 8.447 / 8.433 = −4.6%**。
帧墙钟单跑噪声达 3%（9.283 / 9.013），**不足以支撑任何单跑结论**——本批只报 GPU。
这 −4.6% **不含任何剔除收益**（逐簇剔除是 ★1-a2），来源是簇路径顺序读 138MB 顶点池
取代旧路径流式读 408MB 去索引顶点缓冲的内存局部性。**接管本身就是净增益**，剔除是叠加收益。

### 16.2 🔴 主 agent 查出的正确性缺口：doubleSided 的背面副本不在产物里

β-1 报"loader 三角数比产物 L0 多 9%"时我以为是缺几何。逐层查：

1. glb 源三角（Σ `indices.count/3`，1591 prim）= **2,832,120 = 产物 L0，差 0** ⟹ 产物完整。
2. 那 loader 的 3,111,330 多出来的是谁？`duplicateForDoubleSided`（loader:616）给 doubleSided
   材质**追加一份绕序翻转 + 法线取反的背面副本**。
3. 逐位对上：glb 里 **6 个材质** 带 doubleSided，覆盖 **279,210** 三角，
   `2,832,120 + 279,210 = 3,111,330` **恰等于** loader 报的数。
   （引擎从**材质**读 doubleSided，而 glTF 规范把它放在 **primitive** 级——本 glb 由
   `bistro_pack.py` 生成、写在材质上，两边自洽，但换标准 glb 这条口径要重看。）

⟹ **簇产物里没有那 279,210 个背面**，取代它们 = 背面直接消失。当前机位看不见这个洞
（position G-buffer 逐位同），换机位就是洞。

**处置**：取代条件加 `!mat.doubleSided`（`makeOpaqueCall` 与 `auditMeshletSwitch` **同一条判据、
两处必须同步改**），装配期审计把不接管的数量打进 INFO。实测引擎报 **61 个** doubleSided
primitive 不接管，与我从 glb 独立算出的 **61** 逐位吻合；gated = 1530 = 1591 − 61 精确。
加闸后差异从 419 px 掉到 **65 px**——**顺带说明那 419 px 里大部分本来就是这批图元的背面**。

正解（另立批次，别混进 β）：产物侧复制副本，或 shader 端按 `gl_FrontFacing` 翻法线 + 不剔背面
（后者更省，且与 loader 的副本语义等价）。

### 16.3 本批故意不做的边界（都是真缺口，不是遗漏）

- 🔴 **picking / highlight 静默失血**：`replayClonedPass` 逐 draw 调 `cloneEntryFor`，meshlet 条目
  在 `cloneRemap` 里是 `kNoClone` → `continue` ⟹ switch 开时 **1530 条 draw 在 picking/highlight
  pass 里一条都不重放**（表现 = 这些面点不中）。picking 默认关，**VVL 冒烟抓不到它**。归 β-3。
- 阴影 / CSM、IZB mask 重放、透明排序仍走旧路径 ⟹ 同一份几何由两条管线分别画，
  "两条管线深度是否逐位同"尚未测。归 β-2。
- 只在 `cull_replay=cpu` 生效（DGC 重放路走不了簇 draw）；非默认档不切并 warn（已实测三档）。

### 16.4 两个坑（都咬过我，写下来）

1. **`--set diagnostics:cull_replay=gpu_sync` 是无效的**：写进 Lua 是 `cull_replay = gpu_sync`
   （右端是**全局 nil**）⟹ 静默回落默认档。字符串值必须 `--set 'diagnostics:cull_replay="gpu_sync"'`。
   我自己先踩了，导致三条"非默认档"跑其实全是默认档。**`meshlet.coplanar` 有 fail-fast，
   `diagnostics.cull_replay` 没有**——判据关键的键都该 fail-fast（挂号）。
2. **`smoke.py:gbuf_channel()` 对 `dump_gbuf=0` 返回 None**（`n if n > 0 else None`），
   而引擎 0..4 都是合法通道 ⟹ ch0 采集每次报"没拿到截图"。子 agent 发现并绕过，已修。

## 17. ★1-a2 逐簇剔除：定价实测与设计定案（2026-12 拍板）

### 17.1 定价：β-1 的 −4.1% 全部来自主 pass，阴影一分未动

`profiler=120` / `--frames 130` / 单实例串行 / aa 默认档 / 本机 8060S。GPU 时间戳取 `n=117` 那组
（`n=119` 是 CPU zone，两组同名，**读错组会把 CPU 录制时间当成 GPU**）。

| GPU pass | 门关 | 取代档（β-1） | Δ |
|---|---|---|---|
| `geometry`（G-buffer） | 2.322 ms | **1.966 ms** | **−15.3%** |
| `shadow` + `shadow1..3` | 6.608 ms | 6.587 ms | 0（β-1 只切主 opaque 段） |
| `gpu.frame` | 9.212 ms | 8.834 ms | −4.1% |

与 §16 报的 −4.6% 同向（那次是两次独立 GPU 帧读数，本次是 profiler 在场档）。三条推论：

1. **★1-a2 在主 pass 上的绝对天花板 = 1.966 ms**。提交几何砍 39×（§7.3 的 42.6%→1.08%）后
   屏幕上该画的还得画，现实预期 **1.3–1.7 ms（帧的 15–19%）**。
2. ~~**阴影 6.587 ms 是主 pass 的 3.4 倍 ⟹ β-2 的期望收益高于 ★1-a2**~~
   🔴 **该推论已被 §7.4 的引擎内复核推翻**（2026-10-12）：它的前提是"§7.4 给阴影 3.2× 几何下降"，
   而那 3.2× 是模型盒偏小的产物。**簇粒度剔除进阴影段的可证上界只有 0.31ms（阴影的 5%）**
   ⟹ **β-2 按原形态（逐簇剔除）不做**。
   阴影上真正的两条杠杆（用户 2026-10-12 拍板先做 ①）：
   - **① 远级联 LOD**（★1 的另一轴）：不是剔几何而是**换粗级几何**，不受包含性限制。
     uniform 整级上界 = depth4 全三角的 5.8% / depth7 0.5%（§7.2）。§7.4 那条"group 级 DAG cut
     退化到 L0"的负知识否的是 **group 粒度选级**，**不否 LOD 本身**（它自己写了"逐簇选级仍要做"）。
     ⚠ 前置：阴影段目前**还没走簇路径**（β-1 原位取代只覆盖主 opaque 段，阴影仍用
     `asset.shadowPipelineIndex` 的旧 mesh 管线）⟹ 本条需要先补"簇路径进阴影段"这块基础设施。
   - **② 债-82 时序摊销**：c1..3 = 5.45ms = **阴影 87%**，且每帧全量重画。不剔几何、**少画几遍**，
     不需要簇路径；代价 = 远级影缘时序模糊（需单独画质判据）。
3. 本表推翻了"先做完 ★1 再测 §7.4"里隐含的排序假设——阴影侧的账现在就有 per-pass 读数。
   （债-82 的级联体数**已于 2026-10-12 实测完毕**，见 §7.4 表；它当时被写作"另一件事"是误判——
   那正是本推论的分母。）

### 17.2 设计定案：标志位早退（★1-a2-i），不做压紧、不做 indirect

**为什么不需要 indirect**：每个 primitive 本来就是**独立 draw、taskCount 由 CPU 给**
（`meshletTaskCount(l0Count)` = 2×L0 簇数）。所以"少画"有两种实现——CPU 改 taskCount（要 GPU→CPU
读回 = 卡帧）或 GPU 让空任务自己早退。选后者。

**★1-a2-i 的形态**（本批）：

| 件 | 内容 |
|---|---|
| 剔除项表 | 启动期一次性上传，**object 主序**逐 L0 簇一项：`{clusterIdx u32, objectSlot u32}`（8B × 27,487 ≈ 220 KB）。同 `cullTable` 的建法 |
| `ObjectClusterRange` | 复用现成的 `reserved` 字段承载 **`cullBase`**（该 object 首剔除项的全局下标 = l0Count 的 object 前缀和）。🔴 **不用 `base + local` 做下标**：`base` 是全局簇下标，同资产被两个 object 复用时两者撞同一槽 ⟹ 一个对象的可见性替另一个决定 = **几何凭空消失**。16B 步长不变 = scene data 布局不动 |
| 剔除 compute | 新 `meshlet_cull.comp.slang`，一线程一剔除项。球变换与 6 面保守测试**逐式复用** `cull.comp.slang`（`wc = m0·c.x+m1·c.y+m2·c.z+m3`、`scale = max(三列长度)`、`dot+n.w < -r` 才剔），视锥走 push。写 `RWByteAddressBuffer` 的 u32 标志 |
| dispatch 位置 | 主 CB 内、`recordFixedDataPath` **之后**（scene staging 已拷完 + 屏障已过）、pass 循环**之前**。渲染图**没有 compute pass 概念**（`ao` 是全屏光栅不是 compute）⟹ 手插图外数据通路段 |
| mesh shader | `if (flag[range.cullBase + localCluster] == 0) { SetMeshOutputCounts(0,0); return; }` |
| 门 | `meshlet.cull`（默认 false）。门关 = 不 dispatch、mesh 不读 = 与 β-1 逐位同 |

**确定性**：标志位方案**无 atomic、无顺序依赖** ⟹ 天然逐帧确定（压紧方案要 atomicAdd，
会引入提交序抖动，那是 ★1-a2-ii 才付的代价）。

**🔴 本批最值钱的判据**：保守视锥剔除**按构造不可能改变交付图像**——被剔的簇整个在视锥外，
一个像素都贡献不了。所以主判据是零阈值的

> **`cull=on` 截图 逐位 = `cull=off` 截图**（不是"差异小于地板"）

配套三条，缺一条就是假绿：
- **阳性对照**：一个"强制全剔"的调试档必须把主 opaque 段整个抹掉（否则"剔除器根本没跑"也满足上式）；
- **反向对照**：`meshlet.enable=false` 仍 = 门关基线锚 `54e40665…`；
- **收益判据**：`geometry` pass 的 GPU 时间戳必须降（保守剔除若没剔掉任何东西，图像也"逐位不变"，
  上式照样绿 ⟹ 图像判据对"剔了个寂寞"全盲，必须配性能读数）。

### 17.3 ★1-a2-ii（挂号，本批不做）：压紧 + GPU 写 groupCount

标志位方案仍要发射 2×27,487 = 55K 个任务，其中被剔的那些只是早退。若实测早退开销吃掉收益，
再做压紧：`cull.comp.slang` 已经在**逐 candidate 覆盖 `meshCmd.x`（groupCount）**
（不可见 → 0 = no-op draw），所以 GPU 写 taskCount 的通路**现成**。缺的只是**顺序稳定的压紧**
（两趟：先 per-object 计数、再前缀和写紧凑簇下标表）——不能用裸 atomicAdd，那会让同 object 内
可见簇的顺序逐帧抖，深度平局像素跟着抖。

### 17.4 ★1-a2-i 交付记录（2026-10-12，两批；子 agent 实现 + 主 agent 亲验）

**补记说明**：这一节是 2026-10-14 收账时补的——★1-a2-i 当天就入账了（`dc3051c` 契约/CPU 半 +
`aa8c9c5` GPU 侧接线），但 §17 只写了定案（§17.2）没写交付，读的人会以为还停在"设计定案"那一步。
以下逐条来自两次的 commit message 与主 agent 亲跑读数，不是转述。

**形态照 §17.2 定案做**：标志位早退，不压紧、不 indirect。剔除项表 8B × 27,487（host 映射直写）
+ 逐簇可见标志 + `meshlet_cull.comp.slang` 一线程一项保守视锥球测试 + mesh 侧
`SetMeshOutputCounts(0,0) + return` 早退（新 binding 21）。dispatch 手插主 CB（渲染图无 compute
pass 概念）：`recordFixedDataPath` 之后、pass 循环之前。

四条值得留的：

1. 🔴 **compute 的布局契约也走反射**（用户红线，本批是全仓**首个** compute 反射样板 = 第 8 条
   管线）。为此给反射工具加了定长数组支持（`Kind::Array → std::array<T,N>`；schema 侧认
   `"T[N]"`，返回类型从 `const char*` 改 `std::string`——数组串是运行期拼的，前者只能悬空）。
   回归红线 = **既有 6 条管线生成物逐字节不变**（亲验）。仓库里 `cull.comp.slang` + `gpu_cull.cppm`
   那套「手写字节偏移 + static_assert」是**遗留形态，勿效仿**（调研子 agent 一度把它当样板推荐）。
2. 🔴 **下标空间红线**：标志下标**不能**用 `base + 簇内序`——同资产被两个 object 复用时 `base`
   相同 ⟹ 撞同一槽 ⟹ 一个实例的可见性替另一个决定 = **几何凭空消失**。改由
   `ObjectClusterRange.reserved` 转正承载 `cullBase`（object 前缀和），16B 步长不变 = 布局不动。
   变异 B（`cullBase = base`）与变异 A（前缀和误用 count）各被 3 条用例抓住，含专杀 B 的反向用例。
3. **图像判据对"保守剔除"天然是零阈值**：`cull=on` 逐位 = `cull=off` = 取代档锚
   `0233baacea7b2654…`（保守剔除按构造不可能改图）。⟹ 必须配**阳性对照**才有意义：把 compute 的
   Store 强制写 0 → sha `734124f3c9d063af…`，且与子 agent 独立跑出的**同值**（两次不同实现同 sha
   = 既证明标志真驱动渲染，也证明确定性）。反向对照 `enable=false` = 门关基线 `54e40665…`。
   门真开的实证 = 日志「生效档 = gpu」+ items=27487（**恰等于 canonical L0 簇数**）+ dispatch 430 组。
4. **门关零新命令**是量出来的不是推的：`vkcmd.count` 每帧 **19994** == `enable=false` 的 19994；
   `cull=true` → 19999（+5 与 `recordDispatch` 实发的 5 条 vkCmd 逐条对上）。
   ⚠ 这个 19994 与 §1.2 今天（2026-10-14）门关档读到的 19994 同值——两天两档互证。

**收益读数（口径带作用域）**：子 agent 逐帧配对（n=117）在**部分可见区间** geometry **−21.7%**
（单帧最高 −66%），转出单调加深、转回镜像衰减。🔴 **默认机位下建筑全在视锥内，读数落在噪声内**
⟹ 收益判据必须配**旋转激励**口径，别拿默认机位的"没差别"当"没收益"。

**顺手撞出来的两条**（都改了别的文档，这里留指针）：① 试 `StructuredBuffer` 类型化三张表，
实测全绿、逐位不变（`ArrayStride` = 8/44/4），但**当时** R3 红线（任何 `.slang` 出现
`StructuredBuffer` = ERROR，无豁免清单）未撤销 ⟹ 回退 `ByteAddressBuffer` + `Load<T>`；
② 更该记的一条：**spike 全程用的就是 `StructuredBuffer`，它没触发 R3 只因为 R3 只扫 `rgl/`
树、不扫 `tests/shaders/`——spike 是绕着红线跑的，不是红线认可了它**。

🔴 **① 的回退理由当天就过期了，别照本记录去理解现行红线**：R3 已于 2026-10-12 由用户拍板
**窄化放行**（从"出现即 ERROR"改成"**元素类型未走反射契约**才 ERROR"，配套 R4 stride 对账护栏 +
设备端 stride 往返用例）→ `docs/constantbuffer_迁移计划.md` §8（经过与那条疏漏在同文 §8.2）。
⚠ 但**"放行"不等于"已迁移"**：`meshlet_cull.comp.slang` / `meshlet.mesh.slang` 挂着逐位锚点，
改回类型化载体是**单独一批**（要跑逐位对拍），至今未做。

## 18. 远级联 LOD：阴影段的主杠杆（2026-10-12 立项）

用户拍板：β-2 原形态（簇粒度剔除进阴影段）被 §7.4 复核否掉后，**阴影走 LOD**。本节 = 该线的
设计输入与事实基线。

### 18.1 为什么是"换粗级几何"而不是"剔更多几何"

口径见 §7.4 的推翻记录：簇剔除在阴影段的**可证上界只有 0.31ms（阴影 6.28ms 的 5%）**，根因是
远级联盒子本来就罩住大半个场景（c1 收下 100% 候选、98.5% 的投影体包围球被盒子严格包含）。
LOD 不剔几何、**换粗一级**，不受包含性限制。

### 18.2 产物 LOD 结构事实基线（**别再重查一遍**）

- **簇表排序 = (primitive, DAG 级, group 序)**，每个簇恰好在其出生级被输出一次
  （`packer.cpp:140-178` + `clusterlod.h:797-857` 主循环）。⟹ **L0 是每 object 区间的前缀，
  每一级在同一 object 内也是连续子区间**。真产物 1591/1591 复核（含"每 prim 内 group depth
  单调不减"）。
  ⚠ 但引擎读侧四条常驻不变量（`meshlet_ranges.cppm:18-30`）**不含"级内连续"**——第一刀若依赖它，
  要在扫描侧补第五条不变量（失效 = 工具口径变了，同现有四条的可归因错误串纪律）。
- `DiskCluster.group` = 本簇所属 **group 的全局下标**；`.refined` = 产出本簇的**更细 group** 下标
  （DAG 边，-1 = L0）。**不是 partition id、不是父簇下标**（`format.h:126-137`）。
  🔴 L0 口径取 `refined == -1`（`MeshletDiskCluster` 注释里的定义，不依赖 group 跳转）；
  `group.depth == 0` 在真产物上与之**同一集合**（27,487 簇 / 2,832,120 三角，2026-10-12 独立
  探针复核）。旧版「depth==0 只 2,518 簇」是量错的读数，已删 → §12.3。引擎口径 =
  `isL0Cluster`（`meshlet_ranges_impl.cppm:27-29`），两条口径的一致性现由 `meshlet_lod`
  invariant 3 机器钉住。
- **各级实测**（`meshlet_pack --stats` 亲跑复核，与 §7.2 表逐项相等）：

  | depth | 0(L0) | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|
  | 簇 | 27487 | 15525 | 8588 | 4787 | 2573 | 1354 | 694 | 291 | 146 | 62 | 9 | 2 |
  | 三角 | 2832120 | 1402098 | 690188 | 334293 | 162928 | 77556 | 35693 | 13935 | 6059 | 2432 | 389 | 60 |

- 🔴 **回落级才是成本主导项，比 D 选几更重要**：调研实测 maxdepth 直方图显示
  **561/1591 个 object 的 DAG 停在 L0**，且 **1097/1591（69%）的 maxdepth < 4**（这些是小物体，
  人均约 55 三角）。所以"uniform 整级上界 = depth4 的 5.8%"那行**只覆盖有该级的 object**，
  做预算不能直接引。按"每 object 画 `min(D, maxD_自己)`"重算：

  | D | 画簇数/级 | 三角/级 | 占 L0 | 若回落改成"回落 L0" |
  |---|---|---|---|---|
  | 3 | 5,791 | 388,677 | 13.72% | — |
  | **4** | 3,670 | 223,130 | **7.88%** | 11.83% |
  | **5** | 2,566 | 144,448 | **5.10%** | — |
  | 7 | 1,759 | 94,072 | 3.32% | **34.97%（比现状全量重画还贵）** |

  ⚠ 这张表原本是**调研单次实测**。2026-10-12 用**独立第二实现**（Python 直接读产物本体，不 import
  任何被测码）逐行复算：**全部逐项相等**，含 maxdepth 直方图（`D=5 deepest` 的选中级直方图
  `[561,219,224,93,115,379,0×6]` ⟹ 停在 L0 的 561、maxdepth<4 的 561+219+224+93 = **1097**，
  与旧读数逐位吻合）。`rgl/meshlet_lod.cppm` 的 `costTable` 是它的**常驻机器版**（启动日志打，
  §18.6 判据 6），不再停在一次性读数。

### 18.3 可寻址性：能，且第一刀不用扩任何表

每 object 的第 D 级 = 单一连续区间，`(base, 级内起始偏移, 簇数)` 可精确表达（`DiskGroup.
clusterOffset/clusterCount` 直接可算）。三条扩法按改动量升序：① **偏移走 push constant**
（选它：零 scene data 变更、零反射重跑、旧 shadow 管线一字节不动）② 追加 scene data 尾区
③ 扩 `ObjectClusterRange` stride 16→24（最大，且该 struct 的 `reserved` 已转正为 `cullBase`，
`scene.slang:141-146` 明写"★2 若要选级请另开字段/另表，勿与此争用"）。

### 18.4 uniform 整级**不受** §7.4 那条负知识影响（已确认）

§7.4 否的是 group 粒度**自适应**选级，根因链第一环是"选级发生在 group 粒度、要读 group 误差
（= 分区内 max，`clusterlod.h:244-254`）与阈值比"。uniform 整级 = 画 `depth == D` 的全部簇：
**不读 error、不比阈值、不做 per-group 判决** ⟹ 长尾离群簇根本进不了判决路径。

### 18.5 阴影段走簇路径的前置清单（现状：零交集）

阴影现在走 `shadowPipelineIndex` 旧 mesh 管线（`addShadowEntry`，去重键 = vertexSlot，bistro ≈8
条目），`collectShadowCasters`/`emitShadowCasters` 不查任何簇表。要新建：`kTailSpecs` 加
`"shadow_meshlet"` + `addShadowMeshletEntry`（**全场景 1 条目**，簇 buffer 是全局单例；
bindings 3 + 17-20，depth-only 不烘 texture/sampler）+ 资产字段 `shadowMeshletPipelineIndex` +
新 gate 默认关 + doubleSided/blend 排除（§16.2 缺口：产物无 61 个背面副本）+ 装配期对账
（`auditMeshletSwitch` 形态，半切状态启动期机器查掉）。
**不需要动**：渲染图（4 个 shadow pass 已存在）、执行器（`drawOne` 对 push 大小通用）、
采样端、c0 pass、scene data 布局（偏移走 push）。簇四段 GPU buffer + 描述符槽**已存在**
（`wireMeshletBindings`，binding 17-21 全局单例），直接复用槽号。

### 18.6 第一刀形态与验收

**形态**：c0 保持旧路径（**零改动**）；c1..c3 统一切簇路径，每 object 画 `min(D, maxD_自己)`，
D 是单枚配置旋钮。改动面 = CPU 启动期扫描加一个纯函数（rlbase，`just test` 可钉）+ 每帧
`emitShadowCasters` 换 task 数与 push + 新 shader 里 `cluster = base + levelDStart + (groupID.x>>1)`
**一个偏移量**。`meshletTaskCount` 函数体零改（入参语义从"L0 簇数"放宽为"本 draw 要画的簇数"，
🔴 那条"α 叠加段与 β-1 取代段必须同式"的漂移红线注释要扩到第三个消费者 = 阴影段）。

**验收（阴影图必变 ⟹ 逐位只保留在"不该变的地方"）**：
1. **构造性逐位**：c0 阴影图逐位、主 pass 输出逐位；终图 diff 掩码 ⊆ 阴影区（+小膨胀）。
2. 🔴 **阳性对照 L0 臂**（先于信任 level D）：同一新管线配 `levelDStart=0, count=l0Count` 跑
   c1..3 ⟹ 与旧路径"同几何"（深度内容非逐位，光栅序不同 = 债-86 口径；但逐级阴影覆盖像素数
   在容差内）。**先证明管线画的是对的几何，再谈 LOD。**
3. **区域归因**（imgqa 默认起手）：终图 diff 非空（防"没画成假绿"）。
   🔴 **原判据「diff ⊆ 阴影区」写错了，2026-10-12 实测推翻并改写**：以「关掉全部阴影」定义
   被影响区 `S = {p : 有影 != 无影}`，则**任何让影子变大的改动，新增的影缘按构造就在 S 之外**
   ——这条判据对 LOD 永远不可能成立，拿它当门会把正常实现判成 bug。改写成两条有判别力的：
   ① **越界像素到 S 的距离分布**（贴边 = 影缘生长；散落全屏 = 画错东西）；
   ② **diff 规模随 D 单调不减且随成本表饱和**（LOD 轮廓误差的性质；漏画/区间错位会让这条失稳）。
   实测表与那处 blob 的归因 → §18.9。
4. **覆盖稳定性**：逐级"被阴影像素数"新旧在容差内（允许影缘微缩，不允许影子大面积消失）。
5. **性能实测记账**：逐级 GPU 时间（基线 c1 1.959 / c2 1.901 / c3 1.591ms）降到与三角比一致的
   量级；draw 数不变（每级 1591/1571/1524）⟹ 省的全是光栅 + 顶点处理。
6. **启动期机器对账**：Σtask = 2×Σ簇数、per-object 最大 task < 派发上限、doubleSided/blend
   排除数 == 已知值；**并把 §18.2 那张成本表打成日志**（把一次性读数变常驻输出）。

### 18.7 待拍板与已记欠账

- ✅ **D 已定（2026-10-12 用户拍板「做成配置」）**：D 与回落策略**都是配置旋钮**，不写死。
  `meshlet.shadow_lod`（int，**-1 = 关** / **0 = 阳性对照 L0 臂** / **N≥1 = uniform 选级 D=N**）
  + `meshlet.shadow_lod_fallback`（`"deepest"` 默认 / `"l0"`，未知值 fail-fast）。
  出厂默认 = **关**（P5 门关逐位不变红线）。数据推荐档 **D=5 + deepest**（5.10% L0 三角，
  比现状降 ~19×），画质取舍随时改键复测，不必再回来改代码。
- 欠账 ①：逐 c1..c3 分立 D（同一张表加列即可）。
- 欠账 ②：🔴 主 opaque 段若将来也切 depth-D，`clusterVisible.Load((cullBase + 簇内序)*4)` 的
  剔除标志下标体系会错——剔除项表按 **L0** 铺（`items.size() == Σ l0Count`）。第一刀只动阴影段
  （阴影管线无 binding 21、不读该表）不受影响，但推广到主段前必须按所选级重建下标空间。

### 18.8 ★18-A 交付记录（2026-10-12，子 agent 实现 + 主 agent 亲验）

交付 = `rgl/meshlet_lod.cppm`(+`_impl`，**rlbase** 纯 CPU)：`scanMeshletLod(asset, ranges, D,
fallback)` → 逐 prim `(start, count, level, triangles)` + 聚合锚 + **`costTable`（§18.2 那张表的
常驻机器版）**。`start` 是**相对 `MeshletPrimRange::base` 的偏移**（批 B 原样进 push，shader 侧
`cluster = base + start + (groupID.x>>1)`）。🔴 本模块**不乘 task 槽数**——`2×` 的唯一真相源是
`meshletTaskCount`，不在第三处复述。五条不变量各带两两不同的 `meshlet lod: invariant N` 错误串，
其中 **invariant 3 = 两条 L0 口径（`refined==-1` 与 `depth==0`）必须同数**、**invariant 2 =
级内连续**（§18.2 点名要补的那条）。

主 agent 亲验（本机 AMD 8060S）：

1. **数字自己复核，不采信汇报**：另写独立 Python 探针**直接读产物本体**（不 import 被测码），
   §18.2 成本表 D=3/4/5/7 × 两策略**逐行逐项相等**（D=5 deepest = 2,566 簇 / 144,448 三角 /
   5.100%），逐级直方图与 maxdepth 直方图（561 / 1097）同样吻合。
2. 🔴 **本批最值钱的一条：探针自己先错了一次，而它恰好复现了文档里那条错读数。** 主 agent 第一版
   把 `DiskCluster.refined`（**int32**）按无符号 `I` 读 ⟹ `== -1` 恒假 ⟹ 命中 **0**，看起来像
   "这条口径不成立"而不是"脚本坏了"；靠 `header.l0TriangleCount` 这颗闭式锚才发现。修正后两条口径
   **同一集合**（各 27,487 簇 / 2,832,120 三角）⟹ **§12.3 与 `format.h` 旧写的「depth==0 只
   2,518 簇」是量错的读数，已就地删除改写**（口径方向对、数字错）。教训：
   **量产物字段的脚本自己要有阳性锚点**——"0 命中"永远要先怀疑量法，不是先怀疑结论。
   `GUARD:` 该一致性从此是 invariant 3，不再靠散文。
3. **质检变异与子 agent 形态不同**（子 agent 13 条 M1-M13 已全捕获）：主 agent 另做
   **MA** 回落取 `≤D` 的**首**段而非末段 → 红 5 条；**MB** `costTable` 丢掉 fallback 策略写死
   deepest → 红 2 条；**MC** L0 锚聚合移进 `if (selected)` → **全绿，判定为等价变异**
   （invariant 5 保证 `selected ⟺ count>0`，而 `depth0Len>0 ⟹ segs 非空`）。
   🔴 **等价变异不当缺口，但也不能只凭"看起来等价"收工**：另补三条非等价替代 **MD1/MD2/MD3**
   （L0 簇锚换成被选簇数 / L0 三角锚换成被选级三角 / invariant 3 弱化 `<`）**全部红**，
   才确认 L0 锚与 invariant 3 的两个半边各自有判据。
4. 验收：`just build`（style/doc-check 零新增警告）、`just test` **367 passed / 0 failed /
   27 skipped**（skipped = gpu 套件既有基线）、`just checks` 零新增。渲染路径**零改动**（本批不碰）。

### 18.9 ★18-B1/B2 交付记录（2026-10-12，第一刀落地；子 agent 实现 + 主 agent 亲验）

**B1 = 管线侧基础设施**（`shadow_meshlet.mesh.slang` + `addShadowMeshletEntry` 全场景 1 条目 +
`shadowMeshletPipelineIndex` + 两枚配置键），**B2 = 选级接线**（`MeshletShadowLodTable` ctx +
装配层建表/对账/常驻日志 + `collect/emitShadowCasters` 按级换管线）。B2 顺带把建表半区拆成
`main_meshlet_tables.cppm`（`main_meshlets.cppm` 已破 500 行红线）。

**主 agent 亲验（本机 AMD 8060S，全部自己重跑，不采信汇报）**：

1. **SPV 自己 dis**：新管线只声明 binding `3/17/18/19/20`，**无 21、无 0/1**；push = 3×uint；
   输出 256×`v4float` / 85×`v3uint`。
2. **B1 主判据（基础设施必须可证明惰性）**：门关 = 基线锚 `54e40665…` 且 `grep -ci meshlet` = 0；
   门开（日志实证条目 474 已建、覆盖 1591 prim）截图**仍逐位 = 同一锚**。
3. **成本表三方对账**：引擎启动日志的 `costTable` 与主 agent 的独立 Python 探针**逐行相等**
   （D=3 5791/388677/13.724% · D=4 3670/223130/7.879% · D=5 2566/144448/5.100% · D=7 1759/94072/3.322%），
   `l0Clusters=27487 / l0Triangles=2832120` 命中闭式锚，`回落 prims=1212` 与探针直方图算出的
   `1591 − 379 = 1212` 相等。单 draw 最大簇数 34 → task 68 ≪ `maxMeshWorkGroupCount.x` 65535。
   可切图元 1530 / doubleSided 不切 61 / 无簇产物 0。
4. **阳性对照 L0 臂（判据 2）比预期更强**：`shadow_lod=0` 的终图与门关**逐位相同**（不是"容差内"）。
   理由：depth-only pass 的每个纹素深度 = 覆盖它的所有图元的 min，**与图元序无关** ⟹ 同几何必逐位同。
   而它**确实在画**：逐级 GPU 时间 1.437/1.396/1.156ms（= 门关的 0.73×，非零且可观）。
   🔴 这条同时是**寻址正确性的证明**：D=0 与 D≥1 唯一的差别是画哪一级，故 D=0 逐位同 ⟹
   base/偏移/binding/push/级联索引/图集分块全对，任何 D>0 的差异只能来自 LOD 面本身。
5. **性能记账**（`cpu_baseline` 量具口径：单实例串行 / RPC `/quit` / profiler 窗口 120 / VVL 关）：

   | GPU 段 | 门关 | 仅 enable | L0 臂 | **D=5** | D=5+switch |
   |---|---|---|---|---|---|
   | shadow（c0，恒旧路径）| 0.772 | 0.764 | 0.766 | **0.765** | 0.769 |
   | shadow1 | 1.949 | 1.954 | 1.437 | **0.507** | 0.507 |
   | shadow2 | 1.889 | 1.888 | 1.396 | **0.498** | 0.498 |
   | shadow3 | 1.580 | 1.586 | 1.156 | **0.361** | 0.356 |
   | **gpu.frame** | 8.752 | 10.341 | 8.907 | **6.290** | **4.335** |

   c0 一列四档不动 = 「c0 零改动」的独立读数。c1..c3 合计 5.418 → 1.366ms（**0.252×**）。
   ⚠ **`enable` 一列是混淆项**：`meshlet.enable=true` 会连带打开 α 影子叠加段（+1.59ms），
   所以 D=5 的 6.290 是**背着这笔 overhead** 读到的；`+switch` 档（叠加段清空）才是簇特性全开的
   真实形态 = **4.335ms，比门关快 2.02×**。跨档比较必须带这一列，否则会把 D 的收益读低。

   **判据 5 的「draw 数不变」用运行期计数器钉**（`stats_*.json` 的 `counters`，不是 `zones`）：

   | 臂 | 门关 | 仅 enable | L0 臂 | **D=5** | D=5+switch |
   |---|---|---|---|---|---|
   | `draw.count` / 帧 | 6601 | 8172 | 8172 | **8172** | 6601 |
   | `vkcmd.count` / 帧 | 19994 | 24707 | 24707 | **24707** | 19994 |

   D=5 与 L0 臂、与仅 enable 档**逐条相等** ⟹ 阴影 LOD 一条 draw 都没增减（「原位换管线」成立）；
   8172 − 6601 = **1571** 恰是 α 叠加段那批（vkcmd 差 4713 = 3 × 1571，每条 draw 三条 vkCmd），
   `+switch` 清掉叠加段后总数回到 6601。四档 min == max == last ⟹ 逐帧恒定，不是均值掩盖。
   🔴 这条比「逐级段计数器」强：段计数器要另加测点，而总数相等 + 单测钉住形状（用例
   「切与不切的 draw 条数逐条相等」）两边夹就够了。
6. **区域归因**（判据 3 已按实测改写，见上）：`|S| = 66114`（7.174% 全幅）。

   | D | 0 | 1 | 2 | 3 | 4 | **5** | 7 | 9 | 11 |
   |---|---|---|---|---|---|---|---|---|---|
   | diff px | **0** | 104 | 637 | 1908 | 2461 | **3655** | 5597 | 6184 | 6184 |
   | 其中 ⊄S(>16px) | — | 0 | 1 | 101 | 101 | **101** | 101 | 101 | 101 |

   单调不减 ✓、随成本表在 D≥9 饱和 ✓。D=5 覆盖 `|S|` 66114 → 66103（**−0.017%**，判据 4 PASS）；
   方向 = 变亮 2436 / 变暗 1219（影缘双向微缩），**越界那 101 px 100% 变暗**（新影子出现）。
   🔴 **那 101 px 不是 bug，两条独立证据**：① 它们挤在一个 56×35 的连通块里（x[918..973]
   y[573..607]），且**门关 == 无影**（基线该处本就不受阴影影响）⟹ 是"LOD 面凸出多挡了一点"；
   ② 它们在 D=3/5/11 **逐字节相同**而同期全图 diff 从 1908 涨到 6184 ⟹ 该遮挡体被
   `min(D, 自身最深级)` 钳位（探针直方图里有 93 个 object 的 maxD 恰为 3）。
7. **质检变异与子 agent 形态不同**（子 agent 13 条已全捕获）：主 agent 另做 **MA** 级联边界平移
   `c>=1`→`c>=2`（c1 静默退回旧路径）→ 红 3；**MB** `start`↔`count` 字段互换 → 红 1（只被
   collect 侧抓到，emit 侧用例吃手搭候选 = 测试文件已明写的缺口）；**MC** `objectIndex`
   off-by-one → 红 3；**MD** task 数取错字段（拿旧 `groupCount` 当簇数）→ 红 2。四条还原后基线全绿。
8. **cull 三档红线**（门形与"非默认档必须单独冒烟 + syncval"的口径 → `docs/cpu_perf_plan.md` §4.1/§4.4）：`cull_replay` 三档（`cpu`/`gpu_sync`/`gpu_lagged`）× `--sync` syncval 全部
   errors=0 / hazard=0。`just build` / `test`（376 passed）/ `checks --strict` / `shader-dep --strict`
   零新增。
9. 🔴 **本批抓到一条静默失效（已修）**：`meshlet.shadow_lod = 5` 而 `meshlet.enable = false` 时
   **errors=0 / warnings=0 / 日志零行**——用户会以为功能开着。已在 `wireClusterRangeTable` 门关
   早退处就近告警（只在偏离默认时喊 ⟹ 门关 `grep -ci meshlet` 仍 = 0）。
   根因是 **gate_lint 的 `Ctx` 只有 `DiagnosticsConfig` + `ShadowConfig`，`runtime.lua` 的整个
   `meshlet` 块都不在射程内**（`debug`/`switch`/`cull` 与 `enable` 的孤立组合同样静默）
   ⟹ 记欠账 ③，别为一个键去改 Ctx 做成特例。

**画质/成本对照表（D 是配置旋钮，出厂默认仍 = 关；要改默认看这张表）**：

| D | 画簇数 | 三角占 L0 | 终图 diff（px / 占阴影区）| c1..c3 合计 |
|---|---|---|---|---|
| 0（对照臂）| 27487 | 100% | **0** | 4.007ms（0.74×）|
| 3 | 5791 | 13.72% | 1908 / 2.89% | — |
| 5（数据推荐）| 2566 | 5.10% | 3655 / 5.53% | 1.366ms（**0.252×**）|
| 7 | 1759 | 3.32% | 5597 / 8.47% | — |
| 11 | 1591 | 3.00% | 6184 / 9.35% | — |

### 18.10 欠账 ③：让 gate_lint 接管 `runtime.lua` 的 `meshlet` 块

§18.9 第 9 条。现状 = `Ctx` 只吃 `DiagnosticsConfig` + `ShadowConfig`，`meshlet` 六枚既有键 +
本批两枚新键的**孤立组合**（子键开、`enable` 关）全部静默。修法 = `Ctx` 加 `RuntimeConfig` +
它的 defaults、`test_gate_lint.cppm` 的覆盖率闸门同步扩、一次覆盖整块（**别只挑新键**，
只接管一个键会让「申报表 = 门的全集」这条覆盖率判据失去意义）。

### 18.11 ★18-C 翻默认：`meshlet.enable = true` + `shadow_lod = 5` 出厂（2026-10-12 用户拍板）

问「D 取几」时用户答「为什么不能做成配置」，配置面在 B1 落地后追问「出厂默认怎么定」→
**「默认开 D=5」**；把耦合代价摆出来再问一次 → **「就要现在翻默认（overlay 一起开）」**。
本小节记这次翻动与**翻默认这件事本身的方法论**（比这次翻哪个值更值得留）。

#### 翻了什么

`RuntimeConfig::meshletEnable` `false → true`、`meshletShadowLod` `-1 → 5`（成员初始化 =
唯一真相源），`configs/runtime.lua` 同步，`readMeshletBlock` 头注释同步（它当时还写着
「默认 false = 门关护栏」——**注释说反了比没注释更坏**）。两键必须同时翻：只翻 `shadow_lod`
会正好落进 §18.9 第 9 条那条孤立门告警。

#### 两个锚（引用锚的判据必须同时写明是哪一态）

| 态 | 怎么得到 | 截图锚 |
|---|---|---|
| 🔴 **出厂默认** | 什么都不给 | `95edc8cc…` |
| **门关**（P5 护栏态）| `--set meshlet.enable=false --set meshlet.shadow_lod=-1` | `54e40665…` |

🔴 **P5 没有被废掉，被改掉的是「门关 = 出厂态」这个前提。** 实测显式门关**精确复现**
`54e40665…` ⟹ 护栏仍可逐位到达。但每批「门关逐位不变」的验收命令从此**必须带上那两枚
`--set`**，写「出厂默认」就等于没跑这条判据。

⚠ **只给 `enable = false` 会看到一条 WARNING**（`meshlet.shadow_lod = 5 但 enable = false ——
不会生效`，§18.9 第 9 条那条）。翻默认后这条从"罕见误配"变成"最常见的关闭路径"，**不是 bug**：
它说的就是事实，且告警文案自带解法（两枚一起给）。别为了让它闭嘴而把告警删掉或降级——
那等于把 §18.9 抓到的静默空开关重新放出来。

#### 已明写接受的三条代价（读数绑本机 AMD 8060S）

显存 **+175.5 MiB**（4 段簇 buffer，顶点池 138MB 占大头）· 启动 **+1.2s**（11.05→12.25s）·
🔴 **连带打开 α 影子叠加段 = 白付 1.59ms/帧**。第三条是设计债不是实现细节：
`collectMeshletOverlay` 的激活条件只有「有簇管线条目 + 区间表存在」，**没有自己的门**；
α 是 ★1-a1ii-α 的验证脚手架（同批几何在 opaque 之后重画一遍、靠共面 + eLess 一个像素不写），
β-1 的 `switch` 档会把整段清空 ⟹ **它已完工却仍绑死产品路径的开关**。
⟹ **欠账 ④：给 α 叠加段一枚自己的门 `meshlet.overlay`（默认 false）**，做完净收益才回到全额
（现在 2.46ms）。⚠ **两个口径别混**（2026-10-12 记账时对产物重算纠正）：**阴影段**全额 = 5.419 → 1.359
= **4.06ms**；**帧级**全额 = 8.751 → 4.336 = **4.42ms**（`gpu.frame`）。原文写的"4.05ms 全额"是阴影段口径。顺带：阴影只要位置，顶点池 48B/顶点里 36B 是浪费，
产物加一份 12B position-only 池可把 138MB 降到 34.6MB（总显存 175→71MiB）——记欠账 ⑤，
比独立门重（动产物格式）。

#### 🔴 翻默认的纪律：**默认翻方向，探针就得翻方向**

翻默认最坏的失效不是断言写错，而是**探针值恰好等于新默认** ⟹「键被读到」与「键被静默忽略」
得到同一个结果 ⟹ 用例失去判别力却照样绿。本次两条扁平键用例（`meshlet_enable` /
`meshlet_shadow_lod`）与「显式命中」用例的探针原本都取 `true` / `5`，翻默认后必须改成
`false` / `7`。

**实测坐实（`/tmp/opencode/mutate_18c.py`，主 agent 亲跑）**：

| 变异 | 结果 |
|---|---|
| M1 `meshletEnable` 默认翻回 false | 红 1（缺块 + 成员初始化，同用例）|
| M2 `meshletShadowLod` 默认翻成 0 | 红 1 |
| M3 解析器键名打错 `enable → enable_x`（键被静默忽略）| 红 2 |
| M4 解析器键名打错 `shadow_lod → _x` | 红 1 |
| **M5** = M3 + 把「显式关门」探针方向**改回** `enable = true` | `meshlet` 块用例**整条绿** |

M5 是这条纪律的证据：探针不翻 ⟹ 键被静默忽略时**该用例自己抓不到**，全场只剩
`meshlet.switch` 一条红——而它红是因为**碰巧**拿 `enable = "on"` 当严格类型探针的载体。
兜住假绿的是一条无关用例，正是 `tools/smoke.py` 自己写过的「**那是运气不是护栏**」。

### 18.12 欠账 ↔ 还债总账挂号对照（2026-10-12 记账）

§18.10 / §18.11 里散记的 ①②③④⑤ 已按总账口径挂号。**债号是唯一句柄，细节仍以本文件为权威载体**
——总账只留一行 + 指针指回这里；两边都写细节 = 两边一起漂（本仓漂过两次的那条老路）。

| 本文件 | 总账 | 一句话 | 严重度 |
|---|---|---|---|
| ④ | **债-105** | 给 α 叠加段一枚自己的门 `meshlet.overlay`（默认 false） | 中-高（**本账性价比最高**：纯拆开关拿回 1.59ms） |
| ③ | **债-106** | gate_lint 的 `Ctx` 接管整块 `meshlet`（**别只挑新键**） | 中（护栏缺口，挡在"再加诊断门"前面） |
| ① | **债-107** | 逐 c1..c3 分立 D（同表加列） | 低-中（同成本换画质） |
| ⑤ | **债-108** | 产物加 12B position-only 顶点池（175 → 71MiB） | 低（动产物格式，比 ④ 重） |
| ② | **债-109** | opaque 段若切 depth-D，`clusterVisible` 下标空间须按所选级重建 | 低（**无触发**，动手前必读） |
| ⑥ | **债-110** | D 扫描画质判据是一次性脚本、未入库 ⟹ 画质读数不可一键复现 | 中（出厂默认 D=5 的直接依据） |
| — | **债-111** | 文档指针护栏不查反引号内的文件名 / 符号名 | 中（本次抓到 4 个编造名字） |

#### 🔴 记账时的自查：凭记忆写指针 = 编造（2026-10-12 当场抓到 4 个）

给本节挂号时我**凭记忆**写出处指针，`just checks` 全程零警告地放过了 **4 个根本不存在的名字**：
工具 `mdi_selftest --check`、目录 `tools/meshlet_gen/`、产物 `meshlet_cluster_data.generated`、
函数 `addAlphaShadowEntry`；外加一个不存在的 imgqa 子命令 `shadow_lod`（此前 §18.6 判据里也这么写）。
同时**性能逐级拆分记错了**（记 c0 0.135 / c1 1.043 / c3 2.550，产物真值 0.772 / 1.950 / 1.579），
而**总和恰好对得上**——和对了不代表拆分对了。

**三条可复用的判据**：

1. **写指针前先 `grep -r` / `ls` 一次**。"我记得这个工具叫 X"不是指针，是愿望。
2. **对产物重算，不对摘要复述**。摘要里的 8.752 / 6.290 / 5.418 事后在 `build/rgl/profiles/stats_*.json`
   里逐位复现（8.751 / 6.287 / 5.419）——但同一目录里还躺着**另一轮四臂**的 JSON（9.332 / 6.649），
   两个数量级混在一起就会写出"和对了、拆错了"的表。**挂号必须点名是哪一个文件**。
3. **同名 zone 有 cpu / gpu 两侧**：`shadow1` 的 cpu 侧 3.281 vs gpu 侧 1.950，取错侧不报错只出歪数。

⟹ 已把"护栏不查反引号内文件名"记成 **债-111**；把画质表标注成"当时实测、当前不可一键复现"（**债-110**）。

§18 第一刀四批本身 = 结案账目 **债-104**（数据 → `docs/还债实测数据.md` §4；形态与两个截图锚 →
本文件 §18.6 / §18.11）。同货币的 **债-82**（阴影段无时序摊销）**未结案**——本刀是"减几何量"，
"改更新率"那条独立可叠加的收益原样还在。

## 19. 命令数轴开工：真 MDI 的落地计划（2026-10-13）

> **本节为什么存在**：§7.8 拍板 1 把命令数轴"挂起不删"，簇化轴 ★18 第一刀落地后它的前置变了。
> 2026-10-13 恢复推进。**代码事实全部当日读本体核对**（`executePassDraws` / `segmentSpan` /
> `emitShadowCasters` / `addShadowMeshletEntry` / `rgl_shader_defined` / profiler 计数口径）。

### 19.1 宿主改判：起手做"阴影段簇路径"，不是 §4 原写的旧 shadow 管线

三条硬依据（都是 ★18 落地之后才存在的事实）：

| 依据 | 事实（当日核对） | 后果 |
|---|---|---|
| **条目已塌成 1** | ★18-B1 `addShadowMeshletEntry` = **全场景 1 条目**（启动日志：`条目下标 474 覆盖 prims=1591（全场景 1 条目，簇 buffer 全局单例）`） | §1.2 给阴影段算的"≈30 桶 ×4 = 120 条 MDI 调用"**作废**：簇路径下每级联 **1 次调用**，收益上限是**确定数** ⟹ 阶段 A 不再需要先测桶数（那是阶段 B 的前置） |
| **不经 cullDriver / DGC** | 阴影段走 `executePassDraws` 的通用 `segmentSpan + drawOne` 分支；`shadowCalls` 是**独立向量**（`frame_draw_list.cppm:245-250` 注释明写"不拼进 calls 是为了不打破 GPU culling 候选序对账"） | 🔴 **P3 的 `@8 PUSH_DATA` 红线（§2.2）在本刀根本够不着**；候选序对账 / `checkCullAlignment` / DGC 序列布局一字节不动 |
| **depth-only ⟹ 逐位不变近乎构造性** | 图集每纹素深度 = 覆盖它的所有图元的 min，**与图元序无关**（§18.9 判据 4 已用同一条论证把 D=0 臂证成逐位同） | 主判据可以是**零阈值逐位**，不必先付 P5 那套"闭式解 + 结构判据 + 统计阈值"的重仪器。**模型段拿不到这条**（deferred 写 5 张 G-buffer，深度平局有先后歧义，§5.5） |

⚠ **改判作用域**：只改"起手宿主"，§4 的阶段划分本身（B/C/D/E）原样。
**什么实验推翻本改判**：行序不变的前提下，若 MDI 形态的阴影图**不**逐位等于非 MDI 形态
⟹ 说明 RADV 的 MDI 展开与逐条 draw 在光栅/深度上不等价 ⟹ 第三行作废，阴影段退回"结构判据"档，
且阶段 B 的定价要重做（它原本指望复用这条推理）。

### 19.2 设计定案（实现时别再讨论）

**D1 行布局 = 24B**

```
行 i = [ gcX | 1 | 1 | objectIndex | clusterStart | 0 ]     共 24B（**6 个 word**）
        └──── 前 12B 恰是 VkDrawMeshTasksIndirectCommandEXT ────┘└ word[3] ┘└ word[4] ┘└ w[5] ┘
```

- VUID-07088：stride 必为 4 的倍数且 ≥ `sizeof(VkDrawMeshTasksIndirectCommandEXT)`=12 ⟹ 24 合法
  （spec 原文与 `sizeof`/`offsetof` 均当日实测，见 §19.5 末；spike 的 `per_draw_groupcount` 已实测
  GPU 逐行读自己的 x）。
- 🔴 **本节原图是计划作者写错的**：图里画了 8 个 word（= 32B）却标"24B"，"留 3 个保留 word"跟着一起错。
  ★M-0 执行者**发现后上报而不是照图硬填**，落地形态 = 前 5 个 word 已占用 ⟹ **只剩一枚** `reserved0`。
  可复用的教训：**ASCII 布局图必须能反向数回字节数**——散文写 24、图数出 32，只有逐字段量一遍才暴露。
- **为什么 24 不是 20**：`clusterStart` 逐 object 不同（`min(D, 自身最深级)` 回落）⟹ 16B 的单个 word[3]
  装不下两个索引；24 = 8 的倍数（对齐友好）+ 那一枚保留 word 给 ★M-3（模型段要加材质/顶点块那一维）。
  代价 = 比 20B 多 6KB。
- 🔴 **`cascadeIndex` 不进行、走 push**：一次 MDI 调用 = 一个级联 = 一个常量。push 结构体**尺寸仍是 12B**
  （`{rowBase, cascadeIndex, reserved}`）⟹ `static_assert(sizeof(ShadowMeshletDrawArgs)==12)` 不动、
  执行器 `drawOne` 的通用 push 路径不动。
- **§3 的统一公式在此就是 word0 的语义切换**：非 MDI 路 word0 = objectIndex（drawID 恒 0）、
  MDI 路 word0 = **行基址**（`slot = rowBase + SV_DrawIndex`）。DGC 路（§2.2）不受影响——阴影段不在 DGC 里。

**D2 门 = 新键 `meshlet.mdi_shadow`（默认 false）+ 新 SPV 变体，不是运行时 flag**

- 纪律出处 = §12/§13"门 = 选 SPV 变体 / 选管线条目"（2026-10-02 slang fp 重排教训）。
- 编译条目 = `rgl_shader_defined(shadow_meshlet.mesh SHADOW_MESHLET_MDI_MESH_SPV MDI_SHADOW scene scene_tables)`
  （先例 = `rgl/CMakeLists.txt:25` 的 `MESHLET_DEBUG_OFFSET`）。
- ⟹ **门关用的还是今天那份 SPV 与那条条目，一字节不动** = P5 硬护栏的构造性成立。
- 挂号：本键落进 `meshlet` 块 ⟹ **债-106**（gate_lint 的 `Ctx` 不认整块）射程内，**别为它做特例**。

**D3 执行器形态 = 给 `DrawCall` 加两字段，不引入 variant**

```cpp
uint32_t argsOffset = 0;   // 行表字节偏移
uint32_t drawCount  = 1;   // >1 = 一次 MDI 调用（此时 groupCount 忽略，GPU 从行表读）
```

默认值 = 现行行为 ⟹ 既有全部 `DrawCall{pipeline, pushData, groupCount}` 聚合构造点**零改动**。
执行器分支 = `pushDataEXT → bindShaders → drawMeshTasksIndirectEXT(buffer, offset, drawCount, 24)`，仍 3 条 vkCmd。
🔴 **参数序红线**：真实签名是 `(..., offset, drawCount, stride)`（`vulkan_core.h:27021`）。spike §6.3 的 M4
正是栽在"两个数相等 ⟹ 互换测不出东西"。⟹ 本刀必须把这条调用收进**唯一具名入口**，且行步长（24）与
任何用例里的行数/偏移**都不相等**。

**D4 行表载体 = 每帧 CPU 填 + ring×2 host 映射**

- CPU 侧行表住 `FrameDrawList`（帧局部，与 `pushData` 同风格，执行器保持零 entt）；GPU 侧由新服务
  `MeshletShadowArgsService`（rlscene）持有 **ring×2**（`kFramesInFlight=2`）host-visible|coherent buffer，
  usage = `INDIRECT_BUFFER | STORAGE_BUFFER`（VUID-buffer-02709）。
- 🔴 **为什么 ring×2 而不是 `MeshletCullService` 的"单份 + 写前依赖 barrier"**：那套的写者是 **GPU**
  （dispatch 前一条 barrier 排开上一帧的读），本表写者是 **CPU**——barrier 排不开主机写与 GPU 读，
  只有分帧轮转（或主机 fence，那是把同步点搬回来）。
- 每帧量 ≈ 候选数 × 24B × 3 级联 ≈ 114KB，一次 memcpy（scene data 每帧拷 787KB，同量级）。
- **不可见行写 `gcX = 0` 而不是从行表里删掉**：行序 = 候选序恒定 ⟹ 逐位不变的前提成立；且 spike 的
  `zero_groupcount_noop` 已实测 **0 计数不跳号** ⟹ 孔洞流合法，**不需要压紧、不需要 atomic**
  （§17.3 那笔代价本刀不付）。

**D5 profiler 口径（§5.4 点名的坑，本刀一起收）**

- `draw.count` 口径改为"**逻辑 draw 条数（含 `gcX=0` 空行）**"；新增 counter `mdi.count`（MDI 调用次数）/
  `mdi.rows`（经 MDI 提交的行数）。`vkcmd.count` 对一次 MDI 调用记 **3 条**（不是 3N）。
- 不给新口径就不许报"命令数下降"——否则就是把 vkcmd 下降读成 draw 下降。

**D6 本刀顺手还掉债-81 的一半**

spike 未覆盖两处之一 = "探针走经典 descriptor set + `vkCmdPushConstants`，引擎走 heap + `vkCmdPushDataEXT`"。
★M-1 的 A3/A4 判据（**heap + pushDataEXT + shader object + MDI 四者同时在场**且逐位/阳性对照成立）
**就是那一半的结案证据** ⟹ 交付时同步改 `docs/还债总账.md` 债-81 行（剩一半 = NVIDIA / Intel anv，换机收）。

### 19.3 批次与判据

| 批次 | 内容 | 判据（入账条件） | 渲染风险 |
|---|---|---|---|
| **★M-0 行表构建器** | `rgl/mdi_rows.cppm`(+`_impl`，**rlbase** 纯 CPU)：候选 + LOD 选级 + 逐级可见性 → 24B 行数组 + 聚合锚（Σ行 / Σtask / 空行数）；门 `meshlet.mdi_shadow`（默认 false）；**零消费者** | `just test` 全绿；行大小与"前 12B 恰是 `VkDrawMeshTasksIndirectCommandEXT`"由 `static_assert` + 用例**双钉**；stride 合法性（4 的倍数 ≥12）成断言；行序 = 候选序（打乱输入必须红）；不可见行 `gcX=0` **且其余字段照写**（否则红了无法定位是哪一行）；出厂/门关两锚不变（本批不碰渲染） | **零** |
| **★M-1 GPU 载体 + shader 变体 + 执行器** | `MeshletShadowArgsService`（rlscene，ring×2 + heap 槽）；`shadow_meshlet.mesh.slang` 的 `MDI_SHADOW` 变体（binding 22 读行 + `SV_DrawIndex`）；`addShadowMeshletMdiEntry`（多烘一槽）；`DrawCall` 两字段 + `drawOne` MDI 分支；`emitShadowCasters` 每级联改发 1 条 MDI；profiler 新口径 | A1 门关 = `54e40665…`；A2 出厂默认 = `95edc8cc…`；🔴 **A3 主判据 = `mdi_shadow=true` vs 不给，终图逐位相同**；A4 阳性对照（强制全剔 ⟹ c1..c3 影子消失）；A5 收益判据（`mdi.count` = 生效级联数、`mdi.rows` = 启动对账给的闭式锚、`vkcmd.count` 相应下降）；A6 `cull_replay` 三档 + `--sync` syncval errors=0；A7 build/test/test-all/checks --strict/reflect 全绿。🔴 **A3 在 STRIX 上本批不可达**（§19.9 毒化），由插队的 ★M-3T-a 转绿 → §19.16 | 中（门关构造性不动） |
| **★M-2 三机结案** | 换 NVIDIA / Intel anv 各跑一遍 ★M-1 判据 + 债-81 剩余半 | 三机 `just test-gpu` 绿 + A3 逐位；台账债-81 改"三机已验" | 零 |
| **★M-3T-a task 化**（✅ **已落地 §19.16**，2026-10-13） | 为绕 §19.9 的 gfx1151 毒化而插队：`SV_DrawIndex` 声明处从 mesh 搬进 task + `shadow_meshlet.task.slang` 纯中继（payload 转交 drawID/slot）+ 引擎侧可选 task stage（`PipelineSpec::taskSpv` / `RenderPipeline` 第三句柄 / `NO_TASK_SHADER_BIT` 条件化）。🔴 `gcX` **单位**变 task 数、**数值不变** ⟹ 行表/执行器/采集侧/A5 锚零改动 | **A3 在出事机由红转绿**（门开 = 门关 = `95edc8cc…`）；门关 mesh SPV 与 HEAD 重编逐字节同；task 产物两 builtin 计数 = 2；A5/A6/A7 全中；shader 侧两条变异各落在 §19.15 已记锚上；`TEST: tests/test_shader_mapping.cppm` | 中（门关构造性不动，已 `cmp` 钉住） |
| **★M-3T-b mesh 循环化 ≤128**（✅ **已落地 §19.17**，2026-10-13） | 两枚 meshlet mesh 的 `[numthreads(256)]`→128（`numthreads` 吃 schema 常量）+ 两段输出 `if` 改 strided 循环 + schema `kMeshletMaxWorkGroupInvocations` 256→128 + 新护栏「任一 `.mesh.slang` 的 `numthreads` ≤ 设备下限（三条失效路径出声）」 | 实测**三锚全不变**（出厂 `95edc8cc…` / 红线 A `54e40665…` / 门开 = 出厂）⟹ **无需重锚**；五枚 SPV 全 `LocalSize 128 1 1`；同会话 A/B 代价 = 0。🔴 但"NV 上能跑"仍是推定（本机不是 NV）→ 债-81 | 中（动主 pass，但实测零漂移） |
| **★M-3T-c task 真干活**（✅ **已落地 §19.18**，2026-10-13） | task 读行表（word[3]/[4]/[5]）拿身份 + mesh 组数，一次 `DispatchMesh(meshGroupCount,1,1)` 发完整批；`gcX` 单位改 task（可见 ? 1 : 0）；word[5] `reserved0`→`meshGroupCount`（🔴 用掉 ★M-3 那枚保留 word）；mesh 派发序号回到 `groupID.x`、不再声明 binding 34 | 三锚实测**全部不变**（出厂 `95edc8cc…` / 红线 A `54e40665…` / 门开 = 出厂）；task 数 13684 → 4517（**3.03×**）；syncval 三档 hazard 0。🔴 **负收益如实记**：shadow 段 2.01 → 1.86，残余 +0.35ms 不随 task 数缩放（= amplification 依赖级本身），门保持默认关 | 中 |
| **★M-3 模型段（阶段 B）** | 🔴 **前置 = 先测 deferred 下真实桶数**（§1.2 的 312 是 2026-09-28 forward+picking 临时档读数，作废待重测）；分桶后按 §3 改 `material.slang` 的 DrawArgs 语义（**不动 DGC 布局**，P3） | 破逐位（P5 门开档）：闭式解对拍 + `diffblock` 形态判据 + 红线 A 主动重锚；门关逐位不变每批重跑 | 高（deferred 5 张 G-buffer，深度平局歧义，§5.5） |
| **★M-4 GPU 写行表（阶段 C）** | 级联体积测试进 compute、直写 `gcX`（消 CPU 侧 `casterInShadowVolume`）；需要时才付压紧/前缀和那笔（§17.3） | 与 CPU 构建器**逐元素对账**（照 `meshlet_hand` 的"序，不是计数"口径） | 中高 |

**串行推进，一步一验收**（同 §9 纪律）：一次只派一个子 agent，主 agent **亲验产物本体**；每批一次 commit。

⚠ **A3 的诚实边界**：门关逐位是**构造性**的（那份 SPV 一字节没动）；门开逐位是**强判据而非构造**——
MDI 变体是另一次编译，位级不漂移是"实测出来的"。若 A3 红：先按 §15.3 做**通道级定位**，再区分
"MDI 语义与逐条 draw 不等价"（推翻 §19.1 第三行）与"变体编译漂移"（改判成立、换结构判据）。

### 19.4 与既有账的关系

- **不新立债**：★M-3 的前置（桶数）与 ★M-2（三机）分别就是 §1.2 与债-81 已记的账。
- **债-86 已结案**（P5）在本刀**用不到**——阴影段主判据是零阈值逐位（§19.1 第三行），不破逐位。
- **债-105**（α 叠加段没有自己的门，白付 1.59ms）与本刀**正交**，但它比整条命令数轴便宜一个数量级
  且收益在交付口径 ⟹ 建议排在 ★M-1 之后、★M-3 之前插一刀（用户拍板）。
- **§8「不在阶段 A 顺手做 E」**继续有效：本刀不碰 mapping / push-index。

### 19.5 ★M-1 接缝实测（2026-10-13 主 agent 当日读本体，实现前照做，别再重查）

| 接缝 | 事实（文件:行） | 对 ★M-1 的后果 |
|---|---|---|
| **SPV 变体机制** | `addMeshletEntry` 用**三张平行表**拼变体：`kVariantSpv` / `kVariantSuffix` / `kVariantSpecSuffix`（`shader_registry_impl.cppm:189-193`），并明写"变体名/后缀表 = **唯一拼写点**，别在别处再拼一遍" | 阴影段的 MDI 变体**照同一形态**加，不在执行器里拼字符串。`rgl_shader_defined` 先例在 `rgl/CMakeLists.txt:25/29` |
| **领槽 API** | `resources.allocSlot(SlotSegment::Runtime, "<名字>")` + `writeMeshletSectionDescriptors(...)`（`main_meshlets.cppm:218-229`） | args buffer 走同一条路；名字带 `meshlet.gpu.` 前缀的既有命名法 |
| **声明 / mapping 必须同向** | 条目侧多烘的 binding 若 SPV 未声明，会被"批 M 过滤"裁掉；**反过来（声明了未 mapping）是 VVL 错误**（`addShadowMeshletEntry:439-455` 注释） | 🔴 MDI 变体**声明** binding 22 的那一侧，条目必须**同时**给 22 领槽；旧 `shadow_meshlet` 条目**不声明也不烘** 22（两侧同向） |
| **push 尺寸是条目属性** | `PipelineEntry::argsSize = 12` 与 `ShadowMeshletDrawArgs` 的 `static_assert` 同源 | D1 的 push 不改尺寸 ⟹ `argsSize` 不动、执行器通用 push 路径不动 |
| **执行器已知 frameSlot** | `executePassDraws(cmd, pass, list, frameSlot)` | 见下方 ring×2 的免第二槽解法 |

🔴 **ring×2 不需要第二个描述符槽（本轮设计收获）**：`MeshletCullService` 用"单份 + 写前依赖 barrier"
是因为它的写者是 GPU；本表写者是 CPU，必须分帧轮转。但**不必**领两槽、也不必逐帧改描述符：

- 一张 buffer = `kFramesInFlight` 个等长 slice 首尾相接，描述符覆盖**整张**；
- GPU 侧 `argsOffset` 与 shader 侧 `rowBase` **都由同一个 frameSlot 推出来**
  （`argsOffset = frameSlot*sliceBytes + 级联段起点`、`rowBase = frameSlot*sliceRows + 段起点行号`）；
- ⟹ 分帧轮转落在**偏移**上，不落在句柄上，管线条目与 shader object 的启动期烘焙一次不动
  （这正是 `MeshletCullService` 注释里骂过的"ring×2 要把条目表翻倍"那个代价，本形态绕开了）。

⚠ 前提是 `argsOffset` 与 `rowBase` **必须同源于同一个 frameSlot** ——两处各算一遍迟早漂，
收进一个具名入口（同 D3 把 vkCmd 参数序收进唯一入口的那条纪律）。

**★M-1 的 A5 收益判据要有闭式锚**：启动对账打印 `Σrows`（🔴 **容量取上界口径 = 级联数 × objectSlot 数**，
现值 `3 × 1591 = 4773`，★M-1a 实测定稿。理由：上界口径**不复述** `collectShadowCasters` 的四条判据
⟹ 不会变成第三处真相源，且结构上不可能越界；代价 = 多 61×3 行恒 `gcX=0` 的空转行。
**收益读数**则按实际可切数（1530）算，两个口径别混）与
`Σtask`，并打印"预期 vkcmd 下降 = 3 × (可见 meshlet 行数 − 生效级联数)"。
没有闭式锚的性能读数会被读成"大概降了"，spike §6.3 那条 M4 教训的同族形态。
🔴 **★M-1 的 buffer 尺寸另有两条我定案时没写的 VUID**（当日读 spec 原文 `drawing.adoc:2592-2606`）：
`drawCount-07089`（`drawCount == 1`：`offset + 12 ≤ buffer 大小`）与
`drawCount-07090`（`drawCount > 1`：`stride × (drawCount−1) + offset + 12 ≤ buffer 大小`）。
违例的症状**不是崩溃也不是画面错**，而是 VU 违例——门关档不跑 VVL 就不现形。⟹ slice 尺寸必须由**一个
具名公式**算（`sliceBytes = rows × kMdiShadowRowStride`，且 `argsOffset` 与它同源），不许各处加加减减。

### 19.6 ★M-0 交付记录（2026-10-13，子 agent 实现 + 主 agent 亲验）

交付 = `rgl/mdi_rows.cppm`(+`_impl`，**rlbase**) + `tests/test_mdi_rows.cppm`（10 条用例）+
`runtime_config` 门 `meshlet.mdi_shadow`（默认 false）+ `configs/runtime.lua` + 两处 CMake 登记。
🔴 **零消费者、渲染路径零改动**（`rgl/shaders` / `render_list` / `vkrenderer` / `frame_draw_list` 一行未动）。

**主 agent 亲验（全部自己跑，不采信汇报）**：

1. **布局我自己量**（不读它的断言、独立编一份）：`sizeof(VkDrawMeshTasksIndirectCommandEXT)` = **12**、
   `offsetof(X/Y/Z)` = **0/4/8** ⟹ 与 `MdiShadowRow` 前 12B 同；`sizeof(MdiShadowRow)` = 24 = **6 word**。
2. **基线**：`just test` = **395 passed / 0 failed / 27 skipped，92732 断言**（它的终态读数，我复现）。
3. **三档截图锚我自己跑**：出厂默认 `95edc8cc…` / 门关（`--set meshlet.enable=false --set
   meshlet.shadow_lod=-1`）`54e40665…` / `--set meshlet.mdi_shadow=true` = `95edc8cc…`（零消费者 = 必须不变）。
4. **我的两条独立变异（形态与它的 13 条不同）**：
   - **MA** = `validateMdiShadowRows` 里 `checkRowCount` 的**判决被丢** → 红 **2 条**（归因 = 全部坏形态
     + 十条两两不同，与它 M06 的终态红名单一致）✅
   - **MB** = `rowFor` 显式写 `groupCountY/Z = 0`（"每行 gcX 有值而 GPU 一条都不画、三个锚却全自洽"
     这种静默灾难形态）→ 红 **5 条**（正向锚点逐字段 / 行序=候选序 / 不可见行身份 / 前 12B 对账 …）✅
   还原后 sha256 与进场快照逐字节相同（`40ef4a5a92023266…`），基线复绿，`just checks --strict` **0 警告**。
5. **它的 13 条变异终态**（`/tmp/opencode/mutation_m0_final2.txt`，对**此刻这棵树**跑）：13/13 全部捕获。
   两条曾被误读的要紧账：**M06**（`!=`→`>`）首版红 **0** 条 = 真缺口，补 `badRowDroppedHonestAnchors`
   后红 2 条；**M11**（配置键名打错 = 键被静默忽略）首版是**构建红**（`// MUTANT` 注进了调用括号里，
   语法崩）= 读数作废，改成纯字面量替换后红 1 条、靠"探针取 `true` ≠ 默认 `false`"才有牙。

**三条要记住的**：

1. 🔴 **两个写入者都假设自己是唯一的，双方各错一次**。它的 `finally` 按进场快照逐字节写回，
   **抹掉过我注入的 MA/MB**；而我据此断言"它汇报的改名没落地"——实际我读到的是被回滚的旧版
   （`476 行"九条" vs 现树 499 行"十条"`）。**两边都是真错**：它的 harness 缺并发防护，
   我没跑一次就急着下结论。防线已落进 harness（进场记 sha256 / 进场即含 `MUTANT` 拒跑 /
   每次还原后逐字节复核，不符即抛"现场污染"作废读数）。
   ⟹ **★M-1 起写成硬前置**：主 agent 跑独立变异前必须 `pgrep -af "ninja|rgl_tests|mutate_"` +
   `grep -rn MUTANT rgl/ tests/` **双检**；子 agent 的 harness 一律带还原后复核闸门。
2. **计划的图要能反向数回字节数**（§19.2 D1 那条 32B/24B 就是我画错的，见上）。
   执行者**上报而不是照图硬填**，这条是本批最值钱的执行侧判断。
3. 🔴 **`rowFor` 是构建器与对账器共用的唯一映射公式**（刻意只留一份，防两侧各写一遍漂成
   "构建器写 gcX=0 而对账器期望 gcX=taskCount"）。代价已被 MB 精确暴露：**共用公式 ⟹ 任何发射
   形态的漂移两侧同变，`validate` 那条链结构上抓不到**，信号只剩钉死字面量的用例。
   ⟹ 对 **★M-4**（GPU 行表 vs CPU 构建器逐元素对账）有直接后果：**对账伙伴若与生产者共用公式，
   就不是独立第二实现**（同 `meshlet_cull` 故意用最笨的线性扫描那条纪律）。★M-4 必须显式分开。

**本批挂号（不阻塞 ★M-1，别忘）**：

- ✅ ~~🔴 "多一行"（`rows > candidates`）方向无人钉~~ **已收（2026-10-14 → §19.19 ①）**：
  `checkRowCount` 现在是 `!=`（两个方向都红），
  但**没有用例专门钉 `<` 半边**（`!=`→`<` 的漂移只挡少行、放过**多行**）。而那里藏着一个比漏检更硬的
  东西：`checkRowOrder` 的循环把"行数已相等"当前提，一旦放过 `rows > cands`，它就是 **`std::span`
   上的越界读（UB，无边界检查）**。修法 = 补"锚诚实 + 多一行"用例 + 把该前置写成显式契约。
  ⚠ 注意"多一行"**打不出 M06 的 `>` 半边**（`6 > 5` 仍真）——它钉的是另一条边，别当成同一条账。
  **收账结论**：两条都做了；实测 ㉔（`!=`→`<`）红 1 条 = 旧判据面对这条漂移**完全静默**，
  而 ㉗（顺序调换 + 前置摘掉）的错误串实测把「行数不匹配」报成「行序不对 + 越界读来的垃圾数值」
  ⟹ 前置的价值是**归因**不是检出（旧红线 ⑥ 的读数因此 2 → 1，检测力未变）。
- **`meshlet.mdi_shadow` 是 `meshlet` 块里第一枚"生效条件不依赖 `enable`"的键**（阴影段旧管线条目
  恒在 ⟹ `enable=false` 时它照样有合法宿主，本批刻意没加"必须同时开 `enable`"的隐含耦合）。
  ⟹ `shadow_lod` 那条"孤立门就近 WARNING"对它**不适用**；而 gate_lint 仍不认整块（**债-106**）。
  ★M-1 若给 binding 22 的槽号从 `enable` 的 `nullopt` 链取，**必须回头改这条注释**——
  否则"`= false` 时零消费者"会变成"`= false` 时静默不生效 + 无人出声"。

### 19.7 债-105 交付记录（2026-10-13，插在 ★M-1 前，用户拍板"先还债"）

**这批不在命令数轴上**，但它是 ★M-1 的干净前提：叠加段留在出厂路径里，任何 MDI 收益读数都得先减掉它。

交付 = `meshlet.overlay`（默认 **false**，严格 bool）+ ctx 标记 `MeshletOverlayOn`（与 `MeshletSwitchOn` 同构）
+ `collectMeshletOverlay` 的第五枚判据**进函数**（不再只写在调用点）+ `pickMeshletMeshVariant`
（overlay 关 ⟹ 变体一律 base）+ `warnMeshletVariantInstruments` + `tests/test_meshlet_overlay.cppm`（9 用例）。

**读数 → 数据 §4.6**（两臂 counters 6601/19994 vs 8172/24707、同批差 **1.541ms**、三锚逐位命中）。
台账债-105 已移进 §1 结案表。

**四条要记住的**：

1. 🔴 **"关 overlay" ≠ "关条目"**。叠加条目同时是 **β-1 原位取代的宿主**（`makeOpaqueCall` 用
   `asset.meshletPipelineIndex`）⟹ 把 `bakeMeshletEntries` 一起挪到 overlay 门下 = **switch 静默失去宿主**
   （取代变零、图像回到旧路径、而且没有任何错误信号）。烘焙条件仍只挂 `enable` 的 `slots`，
   overlay 只管**采不采 draw**，此约束已在 `bakeMeshletEntries` 头注释钉死。端到端证据：
   `switch=true`（overlay 关）仍取 `dc846396…` 且日志 `取代图元 1530 个`。
2. 🔴 **叠加仪器是"条目级"SPV 变体位 ⟹ 它本来就能改掉产品取代路的几何**。`debug` 变体把 clip.z 拉向相机，
   若 overlay=false 而 `debug=true` 仍烘进条目，则 `switch` 臂画的是**错位几何**——一条"诊断开关污染产品路"
   的通路，此前没人堵。本批处置 = overlay 关时**强制 base 变体 + 告警**（`A6b`：图 = 出厂锚且 warnings=1）。
   ⟹ 这条是 ★M-1 的现成类比：**凡是"条目级"的位，都要问它的宿主被谁共用**。
3. **我（计划作者）的 A6 判据写错了一格**：我要求 `overlay=true + tint=true` 必须变图。物理上不可能——
   TINT 变体 clip 一 bit 不动、共面、`coplanar=off` 的 `eLess` ⟹ **零像素写入**（`runtime.lua` 与
   `meshletTint` 注释都明写"coplanar=off + tint 必须 = 基线"）。执行者**没有为凑绿放宽判据**，
   而是上报并换正确口径：`overlay=true + tint=true + coplanar=equal` → `1262cfbc…`（变图，且与改前逐位同）。
   ⟹ **记进本文件防止重犯**：给"叠加形态"立阳性对照判据时，先问该变体改不改深度；`debug` 改深度 ⟹ 单用即变图，
   `tint` 不改深度 ⟹ 必须配 `coplanar=equal` 臂才变图（同 §14.1"位比判据对叠加形态全盲"一族）。
4. **主 agent 独立变异 MA 的前两次注入都是"构建红不是判据红"**（`//` 注释吃掉同行后半截条件；
   摘掉判点后形参变 unused）。第三次改成 `if ((!overlayActive && false) || meshletInPlace || …)` 才有效：
   红 3 条且归因干净（主判据 / 真值表 / Lcg）。⟹ **变异必须"能编过、只坏语义"**；"构建红"不是抓到了，是没测。
   另外我自己的测量脚本第一版把两臂跑成了同一档（`$2` 恒空）⟹ **"两臂数字全同"要先怀疑量具**。

**★M-1 接线前的三条新前置**（本批挣到的，已并入 §19.3/§19.5 的执行注意）：

- 🔴 **新增管线条目必须纯追加**（`addShadowMeshletMdiEntry` 只能排在旧条目之后）。若插到旧条目之前，
  `asset.shadowMeshletPipelineIndex` 指向的条目就变了 ⟹ **"门关用的还是今天那份 SPV"这条护栏静默破**
  （与第 2 条同族失效形态，且截图未必立刻红）。入账判据里加一条"旧条目下标不变"的启动对账。
- `renderListSystem` 现贴着 **120 行红线**（本批靠把两枚 ctx 读点收成 `meshletActivations(rg)` 才腾出位置）。
  ★M-1 若还要往 draw 采集里塞第三枚生效标记，**扩 `meshletActivations` 的返回或再抽函数，别直接加行**。
- ~~`tests/test_shadow_lod_emit.cppm` 的"已知覆盖缺口"里"`meshletTaskCount` 在匿名 namespace 不可直连"~~
  **本批已把它抬成导出接缝**，该句过时；已就地更正为"用**字面 2** 是刻意的独立期望（不取被测函数，
  免得把 ×2 抄进第二处），另一端 `kSlotsPerCluster == 2` 另钉"。

### 19.8 ★M-1a 交付记录（2026-10-13，子 agent 实现 + 主 agent 亲验）

交付 = `rgl/meshlet_shadow_args.cppm`(+`_impl`，**rlscene**) 环层 + 服务层两格 + `mdi_rows.cppm` 补四个纯算式
（`mdiShadowSliceBytes` / `ArgsCapacityBytes` / `TailBytes` / `RangeFits` / 🔴 `RowLocation` = 同源 pair 唯一产地）
+ 设备用例 5 条（`TEST_SUITE("gpu")`）+ 测试专用 SPV `tests/shaders/mdi_shadow_rows.mesh.slang`
+ 装配层 `MeshletAssembly` 两格持有者。**零消费者**：一条 draw 都没接。

**主 agent 亲验（自己跑，不采信汇报）**：

1. **基线**：`just test` = **408 / 0 failed / 32 skipped，93483 断言**（+4 用例、+611 断言）。
   skipped 27→32 = 新增 5 条 gpu 用例被默认排除，**不是新跳过**。
2. **我的独立变异（与它的 15 条形态不同）**：只让**一侧**做环层轮转
   （`argsOffset` 带 `frameSlot` 而 `rowBase = row − frameSlot*sliceRows`）⟹
   单元侧红 **2** 条（同源 pair + Lcg 属性组二），**设备侧红 4 条**（载体账 / 设备往返 / MDI 自证回路 / 阳性对照）。
   ⟹ 同源契约两侧都有牙。还原后 sha 与进场逐字节同（`fadb62f2…`）、408 复绿。
3. **三档锚我自己跑**：出厂 `95edc8cc…` **且有 INFO**（先证"建了"，位比才有意义）、门关 `54e40665…`
   且 `grep -ci meshlet` = **0**（门关连号都不领）、`mdi_shadow=true` 仍是 `95edc8cc…`（门开也无消费者）。
4. **INFO 里的闭式数我逐个重算**：`rows/slice=4773 × stride 24 × slices 2 = 229104` ✓；
   `末行 offset+12 = 229104 − 24 + 12 = 229092 ≤ 229104` ✓；`4773 = 1591 × 3` ✓（上界口径见 §19.5 修正）。

**四条要记住的**：

1. 🔴 **它把我 §19.5 的口径纠正了**（已就地改 §19.5）：容量按**上界** `3 × 1591`，收益读数按**实际可切** 1530。
   我原写的"现值 3 × 1530"会把容量算子绑到 `collectShadowCasters` 的四条判据上 = **第三处真相源**。
2. **07089 与 07090 在 `drawCount == 1` 时重合** ⟹ 一条闭式判据同时守两个 VUID；且 `drawCount == 0`
   必须**早退取起点**，否则 `drawCount − 1` 无符号回绕成 `2^32−1` 个 stride，"容量够不够"会读成永远不够。
   这两点我定案时都没写。
3. **设备侧的"自证回路"不等于债-81 结案**：用例 8 真发了一次 `vkCmdDrawMeshTasksIndirectEXT`
   （字节级往返对"分家"是**恒真**的——buffer 里没有"行号"这个量，不画一次就测不到，这条判断是对的），
   但它走的是**测试自己的**描述符形态，不是引擎的 heap + `vkCmdPushDataEXT`。
   ⟹ 🔴 **债-81 的 heap 那一半仍归 ★M-1b 的 A3/A4 判据**，别把这条绿读成它结案了。
4. 🔴 **本批留下的唯一新坑 = 启动时序接缝**：`argSlot()` 要等**注册之后**（`wireMeshletShadowArgs` 在第④步）
   才有，而条目烘焙在注册**之前**（`wireMeshletOverlay`）。★M-1b 的正确解法是**领槽前移、buffer 与描述符留后**
   （已写进 `wireMeshletShadowArgs` 头注释），**别为了凑槽号去猜启动期算不出的行数**。
   另：本批在 `assembleBackEnd` 之前领了一枚槽 ⟹ 其后槽号整体 +1；拿槽号做日志对照的批次按 `8258` 读，
   旧槽 `8252/8253/8254/8255/8257` 与条目 `474` 未动（**纯追加**这条本批没埋雷）。

**两条诚实存活（不当判据用，已声明为缺口）**：N8（`RangeFits` 哨兵分支在合法入参下结构上不可达）、
N11（`row ≤ UINT32_MAX` 闸门要 ~100GB 行表才有激励）。

**挂号**：

- ✅ ~~🔴 新债（待落总账）~~ **已收账（2026-10-14，→ §19.19 ②）**：
  `tests/test_device_primitives_impl.cppm` 的 `ai.allocationSize = bytes`
  取的是**入参**而不是 `buf.getMemoryRequirements().size` ⟹ 非 64 倍数的附件尺寸会踩 bind VU，
  而症状是**回读全 0**（"没画到"与"量具坏"分不开）。我已核到定义体（子 agent 实测撞见，改 RT 尺寸绕开，
  没动共享夹具）。修法 = 用 `req.size` 分配 + 断言 `allocationSize ≥ bytes`；影响所有共用该夹具的 gpu 用例。
  ⚠ 原写"⚠ 此刻总账有另一条线在途改动，树静时落总账行"——**实际没落成债**：收账时普查发现
  **同形态有 4 处不是 1 处**（三个 izb 夹具同样拿着 `req` 却按 `bytes` 分配），四处同批改完，
  未编号即结案（数据 → `docs/还债实测数据.md` §4.7）。
- ✅ ~~§19.6 的"多一行 `<` 半边 + `checkRowOrder` 的 span 越界读前置"仍未做~~ **已做**
  （2026-10-14，→ §19.19 ①：新坏形态 + 显式前置 + 四条变异实测，含一条**归因指错**的立项证据）。

**并发这件事本轮又发生了一次（且这次它自己发现了）**：它的变异 harness 被我双启，两个进程同时写同一批文件，
留下 `mdi_rows.cppm` 处于变异态；它的解法是加两道闸门（**进场即校验所有被改文件 == 基线快照，不符拒跑** +
**检测到多实例就拒跑**）。这两道比我上一批记的"只加还原后复核"更对——**污染要在进场时拦，不是在还原时发现**。

### 19.9 ★M-1b 交付记录与**根因**：`SV_DrawIndex` 在 mesh stage 毒化后续 mesh draw（2026-10-13，主 agent 接手定位）

**状态：代码面六件全部落地且构建/检查全绿，但 A3 主判据红 —— 根因已定位到驱动，本批不能收线。**

#### 接手现场（记一笔流程账）

第二位执行者做到变异阶段被取消，**把一个变异体留在了树上**且构建是绿的（只坏语义不坏编译）：
`vkrenderer_passrec.cppm` 里 `if (true) continue;  // MUTANT(kappa): 跳过 upload`。
它被取消时 A3 已经红、正在做通道级定位，**顺序反了**（A3 未结清就去做变异 = 在坏地基上盖楼）。
⟹ 收线三查里 `grep -rn MUTANT rgl/ tests/` 这一条**救了一次**，别省。

#### 已实测为**正确**的部分（下一个人别再查这些）

| 面 | 读数（启动期打点实测） |
|---|---|
| 条目纯追加 | 新条目 **475**、旧 `shadow_meshlet` 仍 **474**、`entries_total=476`；旧簇槽 8252-8255 + 可见标志 8257 未动，行表槽 8258 |
| 段与行 | `segments=3`，c1/c2/c3 各 `rowCount=1530`，`row0{gcX=2 oi=1590 cs=11}`（真值非零） |
| 同源 pair | `argsOffset = rowBase × 24` 三级联全对（0 / 36720 / 73440） |
| 上传 | `prepareMdiShadowRows` 每帧调用、`upload` 返回无错、`mdiArgsPtr=true` |
| 命令数口径 | 门关 `vkcmd=19994` = 6601×3+191；开档 `6452` = (2084 逐条 + 3 MDI)×3+191；`draw.count 6674` = 6601−4517+4590 ⟹ **D5 口径自洽** |
| 门关逐位不变 | A1 `54e40665` ✓ / A2 出厂 `95edc8cc` ✓（多次复跑逐位同） |

#### 根因：单变量二分矩阵（同一次构建，`sha256` 前 16 位）

| 臂 | 图像 | bistro |
|---|---|---|
| 出厂（门关） | `95edc8cc…` | 在 |
| 门开 + **不发** MDI 命令（条目/push/bindShaders/上传全在） | `da768bfb…` | **在** |
| 门开 + MDI 命令，`drawCount` 截到 **1** 行 | `b1c6afca…` | **没了** |
| 门开 + MDI 命令，全 1530 行 | `b1c6afca…` | 没了 |
| 门开 + 所有行强制 `gcX=0`（展开**零任务**） | `b1c6afca…` | 没了 |
| 门开 + 同条目改走**直接** `drawMeshTasksEXT` | `b1c6afca…` | 没了 |
| 门开 + 同条目只把 mesh SPV 换回**非 MDI** 那份 | `da768bfb…` | **在** |
| 门开 + MDI SPV 但**入口不声明** `SV_DrawIndex` | `da768bfb…` | **在** |
| 门开 + MDI SPV 声明 `SV_DrawIndex` 但行号 `min(…,4772)` 夹住 | `b1c6afca…` | 没了 |

**结论（一句话）**：在 **AMD Radeon 8060S Graphics (RADV STRIX_HALO) / Mesa 26.2.3** 上，
mesh shader 入口**一声明 `DrawIndex` 这个 system value，同一提交里后续所有 `vkCmdDrawMeshTasksEXT`
直接 draw 就不产出任何像素**——主视图那栋 bistro 整块消失（`diffblock`：34.27% 像素、单个 315692px
连通域、最厚 150px = "整块没了"，不是边缘漂移）。

🔴 **不是我们的用法错**：`DrawIndex` 在 mesh/task 阶段的内置清单里（本地 spec
`appendices/VK_EXT_mesh_shader.adoc:80`，ChangeLog 亦记 "Add … VUs for DrawIndex BuiltIn
decorations with Task and Mesh shader stages"）；`maxDrawIndirectCount = 4294967295`；
`vkCmdDrawMeshTasksIndirectEXT` 的 drawCount 相关 VUID 只有 07088/07089/07090（§19.5 已核）。
也**不是越界读**：夹住行号无效，且 `rowBytes` 最大 73452 « capacity 229104。
⟹ **驱动缺陷**（RDNA3 无硬件 mesh shader，走仿真路；`gl_DrawID` 在 mesh 入口的接线大概没做）。

#### 连带发现（对判据设计有直接影响，别再当新问题查）

`--set meshlet.shadow_lod=-1`（c1..c3 退回旧 shadow 管线）与出厂**只差 3670 px = 0.398%**，
600 个碎连通块、无整块 ⟹ **交付机位下 c1..c3 簇阴影的可见足迹就这么大**。
所以：① A3「逐位不变」**不是空判据**（正确实现必须逐位复现这 3670 px）；② 但它被 bistro
整块消失（34%）完全淹没，A3 红的时候**看不出阴影对不对**；③ A4 阳性对照（强制全剔）在
本批**必然只出 3670 px 量级的差**，做的时候别拿整块当预期。

#### 对 §19.1 / §19.2 的处置

- **§2.1 路 A（mesh stage 直读 `SV_DrawIndex`）在本机不可用** ⟹ §19.1 第三行"同几何同行序
  ⟹ 逐位不变"的前提**不是被证伪，而是被驱动挡在门外**：MDI 展开根本没跑到，谈不上等价性。
- **D2 的 binding 号 22 → 34**（`import scene` 把 `sceneHeader` UBO 带进同一 SPV，22 重复声明
  = `vkCreateShadersEXT` mapping 冲突，实测首跑现形）。已接受，shader 头注释与启动 INFO 都记了。
- **§19.8 的"领槽前移"接缝被第⑤步形态消解**（条目烘焙排在载体建成之后、仍在 `assembleBackEnd`
  之前 ⟹ DGC/GpuCull 的 `entries.size()` 快照天然含新条目，零槽序扰动）。雷 1/雷 2 的时序论证
  已在 `wireMeshletMdiShadowActivation` 头注释逐条给（`cloneEntryFor` 有界内守卫、hot_reload
  基线在之后挂）；**NVIDIA DGC execute 路本机不可测，仍归 ★M-2**。
- **下一步三选一**（待拍板）：① 门保持默认关 + 给 Mesa 提 issue + 等 ★M-2 在 NVIDIA/Intel
  上验（代码已就位，成本最低）；② 改走 **task shader 路**（task 读 `DrawIndex` 经 payload 传给
  mesh）——spec 正统形态，但要新增一个 stage；RADV 的 task 路当日**未验证**
  ⟹ **现已判毕（§19.15）：gfx1151 上 task 入口声明 `DrawIndex` 不毒化 ⟹ ② 成立**；
  ③ 换宿主（§19.1 的原始宿主 = 旧 shadow 管线，它不用 mesh shader，绕开整件事）。

### 19.10 ★M-1b ANV 验证（2026-10-13，机器 = Ryzen 9950X 主机 + Intel Arc Pro B50（BMG G21）/ Mesa 26.2.3，`VK_DRIVER_FILES` 只留 intel ICD）

**结论：M-1b 在 ANV 上工作 —— RADV 那条红坐实为驱动缺陷，代码面没有机器相关的潜在问题。**
★M-2 三机结案进度 = Intel anv 已验（本节），NVIDIA 待验；债-81 的 heap 那一半**仍未结案**（等三机齐）。

判据逐条（同一次构建 `96887ba`，双臂同会话）：

| 判据 | 读数 | 判定 |
|---|---|---|
| A3 主判据 | 门关 vs `mdi_shadow=true` 终图 **逐位相同**（`56f2ff18…` = `56f2ff18…`） | ✓ |
| A5 读数 | 关 `vkcmd=19994 mdi.count=0`；开 `vkcmd=6452 mdi.count=3 mdi.rows=4590 draw.count=6674` —— **与 §19.9（8060S 机）逐字同** | ✓ MDI 路径真在跑 |
| A4 形态 | 门开 vs `shadow_lod=-1`（回旧管线）= **46 px / 0.005% 碎差、无整块** | ✓（8060S 为 3670 px，绝对值绑机，只形态跨机可比） |
| A6 syncval | 门开 + `VK_LAYER_VALIDATE_SYNC=1` errors=0 | ✓ |
| `just test-gpu`（ANV 设备） | 32/32、24850 断言（含 M-1a 的 MDI 自证回路在 anv 上真发 `vkCmdDrawMeshTasksIndirectEXT`） | ✓ |

启动对账三元也与 §19.9 同构：新条目 475 / 旧 474 / `entries_total=476`；DGC `graphicsIndirect=false` ⟹ 重放路（与 RADV 臂同形态，NVIDIA 的 execute 路仍只归 ★M-2 的 5090 那半）。

**两条机器绑定读数（拿槽号/像素数做跨机对照的批次注意）**：

1. 行表 heap 槽 = **8234**（8060S 机 = 8258，绝对槽号随设备限额/分段走；相对结构同 = 可见标志槽 +1，
   旧簇槽 8228-8231 + 标志 8233 未动）。**文档里的槽号一律只作单机对照锚，不是跨机契约。**
2. c1..c3 的 LOD 细节足迹本机 = 46 px（8060S = 3670 px）——LOD5 vs 全 mesh 的深度差经不同驱动
   光栅化/量化后差两个量级很正常；**跨机只比形态（碎差无整块），别比绝对像素数**。

（环境账：本机 clang 22 不在 PATH、vcpkg registry fetch 网络抖动——构建由用户手工完成，与 M-1b 无关。）

### 19.11 ★M-1b NVIDIA 验证（2026-10-13，同机 RTX 5090 / driver 615.71.09）：机制绿、**引擎路被 256 线程超规结构性挡住**

**两条相互独立的结论，别混：**

1. **MDI 状态在 NVIDIA 上工作**：`--test-case="*mdi*,*shadow_args*,*MDI*"` = **12/12 passed**
   （含 `shadow_args_mdi_expansion` 自证回路真发 `vkCmdDrawMeshTasksIndirectEXT`、DrawIndex 递增 /
   push rowBase / 每 draw groupCount / shader object 绑定形态全部正确）。⟹ NV 驱动没有 RADV 那味
   `SV_DrawIndex` 毒化；**M-1b 机制无机器相关的潜在问题**（与 §19.10 互证）。
2. **引擎级 A3 在 5090 上此刻跑不了，且与上屏/离屏无关**：`meshlet.mesh.slang` / `shadow_meshlet.mesh.slang`
   声明 `[numthreads(256)]`，而 NV `maxMeshWorkGroupInvocations = maxMeshWorkGroupSize[0] = 128`
   （ANV=1024、RADV 宽松，故此前两机从未现形）⟹ `vkCreateShadersEXT` 被 VVL 点名
   VUID-RuntimeSpirv-MeshEXT-07295/07298 **真超规**，调用照进 ICD 后在 `libnvidia-gpucomp.so` 内
   SIGSEGV（驱动对非法入参未干净失败，行为未定义归它，超规归我们）。窗口模式（KWin wayland 隐藏窗
   llm_debug，实测建窗与设备创建全通过）与 headless 一样死在建 shader 那一步——**崩点在任何 draw 之前**。

**连带定案两笔**：

- **`VK_EXT_headless_surface` NVIDIA ICD 不提供**（NV-only `VK_DRIVER_FILES` 下 createInstance 直接
  ExtensionNotPresent；3-ICD 清单里那条来自 Mesa）⟹ **5090 上离屏冒烟不可用，验证只能上屏**
  （WAYLAND_DISPLAY=wayland-0 的隐藏窗 llm_debug 路实测可起跑）；test-gpu 则靠多 ICD 并集过 instance、
  评分器仍选 5090（score=1024810）。
- **NV 的 DGC `graphicsIndirect=true`**（三机唯一，RADV/ANV 均 false）⟹ ★M-2 execute 路的前提成立，
  但被本节的 256 线程墙压在后面。

**5090 的 `just test-gpu` = 28/32，红 4 条全部是既有 Mesa golden 换机（无一条 MDI 族）**：
`izb_1b 能力面`（`maxExtraPrimitiveOverestimationSize==0` golden）、`UNORM 取整`（0.5→128 的 tie
舍入 NV 不同）、`dheap 0`（heap 通路 `szUbo==szSsbo` 等式 NV 不成立）、`izb_1d`（保守光栅 mask 参考值）。
⟹ ★M-2 的「三机 test-gpu 绿」要先把这四条改写成**设备相对断言**（读能力报差异、不钉 Mesa 数值）。

**新挂号（★M-2 NVIDIA 半的前置）**：簇 mesh shader 若要上 NV 需 **≤128 线程变体**——现设计
1 三角形/线程、`256 ≥ 3×85` 两类线程都排得下，128 装不下 ⟹ 循环化改造（每线程多顶点），独立小批、
待拍板；不做则 NV 半只认机制级（本节的 12/12）。

### 19.12 四机矩阵与 §19.9 根因**修正**：RADV 毒化 = STRIX_HALO 特有，不是 RADV 仿真路通病（2026-10-13 实测）

本机 9950X 核显 **RAPHAEL_MENDOCINO（gfx1036，Mesa 26.2.3，与出事机 STRIX_HALO/gfx1151 同版本）
上 ★M-1b 逐位绿**：门关 vs 门开终图 diff = **0 px**，且 MDI 真在跑（`mdi_shadow 生效` INFO、
`vkcmd=6452 / mdi.count=3 / mdi.rows=4590` 与 §19.9、§19.10 读数逐字同）。

> 🔴 **能力列（mesh/task 上限、granularity、`graphicsIndirect`）的唯一现读口** =
> `python3 tools/smoke.py smoke` 摘要里的 `设备规格` 行（本体 = 启动日志
> `Mesh/task device caps:`，`rgl/utils.cppm::logMeshTaskDeviceCaps`：选完设备**无条件**打、
> 与配置门无关，描述引擎**实际选中**的那台）。下表是 2026-10-13 各机快照，**别拿它当权威**
> ——这类数字换机器就漂（三机轮换 + `run-igpu/dgpu/intel`），漂了还会被下一个会话平铺直叙地读。

| 设备 | mesh/task 上限 | M-1b 引擎级 | 机制级（test-gpu MDI 12 条） | DGC graphicsIndirect |
|---|---|---|---|---|
| STRIX_HALO（8060S 机，RDNA3.5） | 现读口补（此前未查）：mesh 硬上限 256 但 **preferred 只有 128** ⟹ 组合手术取 128 线程**不只迁就 NV**，RADV 自己也推荐这一档 | 🔴 红（mesh 入口声明 DrawIndex 毒化后续 draw） | 未单测 | false |
| **RAPHAEL（本机核显，RDNA2）** | task 1024 / **mesh 256** | ✅ **逐位绿** | —（可补） | false |
| ANV BMG G21 | 1024 / 1024 | ✅ 逐位绿（§19.10） | ✅ 全绿 | false |
| NV RTX 5090 | **128 / 128** | ⛔ 不可达 = 本文件 mesh 声明 256 **超 NV 上限**（07295/07298），与上屏/离屏无关；驱动在建 shader 时 SIGSEGV | ✅ **12/12 绿**（含 MDI 自证回路） | **true**（三机唯一） |

⟹ **§19.9「驱动缺陷（RDNA3 无硬件 mesh shader 走仿真路）」定性过宽，修正为：同版本 Mesa 在
gfx1036 仿真路上正确、gfx1151 上毒化 = STRIX_HALO 特有的仿真路径缺陷**。给 Mesa 提 issue 时
**靶要钉 gfx1151 + DrawIndex-in-mesh**，并附 RAPHAEL 绿作对照（同 Mesa 版本、同仿真机制、不同 SI 家族）。

**对「走 task shader 传统路（§19.9 选项②）能否三家都没问题」的分析**（Slang `task` stage 实测可编译
`OpExecutionModel TaskEXT`）：

- **不能只靠 task**：NV 的墙是 **mesh 线程数 256>128**（task 上限也是 128，且与 mesh 墙正交）——
  不改 mesh 线程数，task 路在 NV 上照样建不出来。ANV/RAPHAEL 已绿，无需救。
- **真正要救的只有 STRIX**，而 task 路读 DrawIndex 是否绕开毒化 = **本机不可测**（RAPHAEL 不复现）
  ⟹ 只能带探针去 8060S 机。**✅ 已判毕（§19.15）：不毒化。**
- ⟹ 唯一可能"三家都过"的形态 = **task 化 + mesh 循环化 ≤128**（两件事）。顺带得到 D3D12 便携形态
  （128 线程 / 32 粒度；注意 NV `meshOutputPerVertex/PrimitiveGranularity=32`、`maxMeshOutputVertices=256`），
  并解锁 NV 的 DGC execute 路。
  **🔴 原本写"一次重做两件事"是错的，已拆两批（2026-10-13 落地时拍板）**：两件事**耦合点为零**
  （task 化不需要 ≤128，≤128 也不需要 task），而 ≤128 的影响面大一个量级——`meshlet.mesh.slang`
  （主 pass，红线 A `54e40665…`）当时同样声明 `[numthreads(256)]`，且 `kMeshletMaxWorkGroupInvocations`
  在 `rgl/shaders/meshlet_cull_schema.lua` 是**两条管线共用**的唯一真相源（当时 256，★M-3T-b 已降 128）
  ⟹ 降 128 必然动主 pass + schema + 闸门。捆在一起 = 把"能转绿的判据"和"会破红线 A 的改动"
  混进同一次验收。拆分后两批都已落地：
  - **★M-3T-a = task 化**（✅ §19.16，本机 A3 由红转绿，逐位锚零改动）——解 STRIX 毒化。
  - **★M-3T-b = mesh 循环化 ≤128**（✅ §19.17，三锚实测**全部不变**、代价 0，无需重锚）——解 NV 超规墙。
- **拍板前建议的廉价前置**（本机半日）：写机制探针（task 读 DrawIndex → payload → mesh≤128，
  经 `vkCmdDrawMeshTasksIndirectEXT`，复用 mdi 测试 rig 形态）先在本机三设备跑绿；然后去 STRIX 机
  跑同一探针决定 task 路对 gfx1151 是否有救。

**前置①已完成（2026-10-13 当晚）**——`tests/test_mdi_task_probe.cppm` + 三枚 SPV
（`mdi_task_probe.task/.mesh/_guard.mesh.slang`，🔴 task/mesh 分模块编译规避 slang#10123 的
NV 越界读坑；`[numthreads(1,1,1)]` 全档 ≤ NV 的 128）：

- 四条用例 × 三设备（NV 5090 / ANV B50 / RADV RAPHAEL，VVL 常开）= **12/12 绿**。
  判据骨架 = task 侧 DrawIndex 递增 + payload 通路同源自证 + 孔洞不跳号 + **守卫探测器**
  （task-MDI 之后同提交直接 mesh draw，守卫列必须出像素 = 毒化检测器本体）。
- 变异验红（主 agent 亲做）：M1 `rowBase + 0u*drawID` ⟹ increments/zero/poison 数据列红、
  control 绿（drawCount=1 无信息，同旧 rig 纪律）；M2 task 侧 col/value 互换 ⟹ **control 也红**
  且守卫列不假阳性（payload 坏与毒化坏签名可分辨）。还原后基线复绿 + 全量 `just test` 93698 断言绿。
- **证据边界（别读宽）**：本机三设备的守卫绿只证"机制无障碍"；**毒化问题对 STRIX 的裁决必须带
  `task_probe_poison_guard` + 它的 control 去 8060S 机重跑**（RAPHAEL 连 mesh 路都不毒化，
  它绿不能外推 STRIX 的 task 路也绿）。重跑 =
  `VK_DRIVER_FILES=<radeon_icd.json> ./build/tests/rgl_tests --test-suite=gpu --test-case="task_probe*"`
  （CWD = 仓库根，先 `cmake --build build --target rgl_tests`）。

**STRIX 裁决（现行 = §19.15 引擎级探针实测；rig 层那张卡已删——§19.13 已证它测不出这种毒化，
留着只会诱导别人去跑一个空判据）**：

| 问题 | 裁决 | 依据 |
|---|---|---|
| rig 层 `task_probe_poison_guard` 能不能判 task 路有没有救 | **不能**（守卫"绿"与"rig 根本测不出"不可分辨） | §19.13 |
| `DrawIndex` 声明在 **task** 入口是否也毒化同提交后续 mesh draw（gfx1151） | ✅ **不毒化** ⟹ **选项② 成立** | §19.15（G-buffer ch0/ch3 逐位相同 + 探测器同 session 会响） |
| task→mesh payload 通路 + `SV_DrawIndex` 是否 = 段内行号（gfx1151） | ✅ **通**（阴影几乎复原，残差仅 2721 px） | §19.15 附加实验 |
| 下一步 | ~~立"task 化 + mesh ≤128 循环化"组合手术批~~ → **已拆两批、第一批已落地**：★M-3T-a task 化 ✅（本机 A3 转绿）、★M-3T-b mesh ≤128 未开工（NV 的 256>128 墙那一批解） | §19.16 |

**提 Mesa issue 的靶（§19.12 修正后）**：gfx1151（Strix Halo）+ mesh 入口声明 `DrawIndex` 毒化
同提交后续 mesh draw；**同版本 Mesa 的 RAPHAEL（gfx1036）逐位绿 = 现成对照证据**。
### 19.13 STRIX 复跑裁决：`task_probe_poison_guard` 的绿是**空的**（rig 层测不出这种毒化）（2026-10-13 实测，机器 = 出事机 8060S / STRIX_HALO）

**裁决：§19.12 裁决卡第一行不成立。** task 路对 gfx1151 是否有救 = **未知，rig 层不可判**，
不能据此立"task 化 + mesh ≤128 循环化"那一整批。

#### 复跑读数（`git pull` 到 `9d0ab76` 后本机重编重跑）

```
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json \
  ./build/tests/rgl_tests --test-suite=gpu --test-case="task_probe*"
→ 4 | 4 passed（含 task_probe_poison_guard）
```

按卡面这就是"task 路有救"。**但它成立的前提从没被量过**：守卫绿只说"这次提交里后面的 draw
画出来了"，而 §19.9 的毒化是在**引擎**里复现的——**这个 rig 到底测不测得出这种毒化，从没验过**。
若它复现不出，"绿"与"失明"是同一个读数。

#### 两条阳性对照（新增，长在 `test_mdi_drawindex.cppm`）

不放进 task 探针文件：那里的 rig 用经典 descriptor set + 1 线程，而 `mdi_*` 的 rig 用的正是
**§19.9 的触发形态**（`mdi_spike.mesh.slang`：mesh 入口声明 `SV_DrawIndex`）且**已证在 STRIX 能画出来**
（`mdi_drawindex_increments` 等 13 条本机全绿）。探测器要判"毒化没发生"，触发必须先被证明"发生了"。

不必新写守卫 shader：同一条管线 push 换 `rowBase` 再**直接** draw 一次，画的就是另一列。

| 用例 | 形态 | STRIX 读数 |
|---|---|---|
| `mdi_poison_control` | 同提交同 pass：MDI(行 0..5) → 直接 draw(行 6) | 列 0..5 对 **且列 6 = 1006** ⟹ 复现不出 |
| `mdi_poison_control_two_pass` | **跨 render pass**（贴引擎 shadow pass → deferred pass 的几何） | pass1 列 0..5 对、pass2 列 6 = 1006 ⟹ 复现不出 |

两条的守卫读数走 `MESSAGE` 不走 `CHECK`（两种极性都合法，取决于设备）；`CHECK` 只钉"触发真的跑过"。

#### 四条"为什么 rig 不复现"的结构假设，逐一排除

| 假设 | 探针（临时改，已还原） | STRIX 读数 |
|---|---|---|
| 经典管线不毒化，要 shader object | 给 `mdi_shader_object_variant`（shader object + 引擎动态态配方）追加一枚直接 draw | 列 6 = 1006，复现不出 |
| 1 线程不毒化，产品是 `[numthreads(256)]` | `mdi_spike.mesh.slang` 改 `numthreads(256,1,1)` | 复现不出 |
| 同 pass 不毒化，引擎跨 pass | `mdi_poison_control_two_pass` | 复现不出 |
| 同管线不毒化，引擎换管线 | `task_probe_poison_guard` 本来就换 guard 管线 | 复现不出 |

**唯一没量的结构差 = descriptor heap**（rig 用经典 descriptor set；`test_mdi_drawindex.cppm`
文件头自己划过这条界："本条补的是 shader object 绑定这一半，**不含** descriptor heap"）。
补它基本等于把引擎机制搬进测试，不划算。

#### 纪律沉淀（跨用例形态的阳性锚点）

**守卫型用例的"绿"必须配一条"本设备测得出"的阳性对照**，否则"绿"与"失明"不可分辨。
这是 `rgl-test-discipline` 的阳性锚点在**跨用例**下的版本：锚点不能只锚"数据通路通"
（`task_probe_control` 锚的是这个），还要锚"**探测器会响**"。写不出反向断言时（本例两种极性
都合法），至少把读数 `MESSAGE` 出来并把"绿不等于通过"写进用例头注释与裁决卡。

#### 下一步：唯一能真判的形态，比组合手术便宜一个量级

**引擎级最小 task 探针**——给产品阴影 MDI 管线加一枚**平凡 task stage**（task 声明 `DrawIndex`、
只发 1 个 mesh group、行表先不接），mesh 侧**停止声明** `DrawIndex`。**不看阴影对不对，只看
bistro 在不在**（§19.9 的九臂矩阵里"bistro 在不在"就是现成的二值判据，成本 = 一次截图 + 一次哈希）：

| 读数 | 结论 | 后续 |
|---|---|---|
| bistro 在（`95edc8cc` 形态） | DrawIndex 换 stage 即绕开毒化 | 组合手术的 STRIX 收益**成立**，可立批 |
| bistro 仍没 | 毒化与 stage 无关（"这条提交里声明过 DrawIndex"就够） | **选项② 直接作废** ⟹ 回选项①（Mesa issue + 门默认关） |

比"task 化 + mesh ≤128 循环化"省掉：行表接入、`gcX` 语义换成 task 数、A5 闭式锚重算、
逐位锚各机重立。它只回答那一个会决定一整批去向的问题。
### 19.14 ★M-3T 引擎级最小 task 探针（**实现就绪规格**，2026-10-13 写；✅ 当日已在出事机执行完毕 ⟹ 结论 **§19.15**）

**唯一要回答的问题**：`DrawIndex` 声明在 **task** stage 是否也毒化后续 mesh draw？
**唯一判据**：主视图那栋 bistro **在不在**（不是逐位相等——阴影在这个探针里是故意错的）。

#### 为什么这个形态是"最小"的（别把它做成手术）

task **恒发 1 个 mesh group、不读行表**；mesh 变体**不声明** `DrawIndex`、继续读 push 的 `rowBase`。
⟹ **行表 / 执行器 / 采集侧 / profiler / A5 闭式锚 全部零改动**。阴影必然画错，**故意不管**——
它只用来确认"task stage 真的在管线里跑了"，不用来判断对错。

#### 改动清单（6 处，位置已核过本体）

1. **新** `rgl/shaders/shadow_meshlet_task.task.slang`：`uint drawID : SV_DrawIndex`
   + **非可折叠守卫** `if (drawID != 0xFFFFFFFFu) DispatchMesh(1u, 1u, 1u, p);`
   - 🔴 **本规格原写的 `[shader("task")]` + `EmitMeshTasksEXT` 是错的**（执行当日实测，
     pinned slangc 2026.16 与系统 2026.18.3 **同结论**）：Slang 的 HLSL 侧没有 `EmitMeshTasksEXT`
     这个名字（`error[E30015]: undefined identifier`）。Slang 走 D3D12 拼写
     **`[shader("amplification")]` + `DispatchMesh`**，产物仍是 `OpEntryPoint TaskEXT` +
     `OpDecorate … BuiltIn DrawIndex`（= Vulkan 侧 task stage 同一物）。先例 =
     `tests/shaders/mdi_task_probe.task.slang`（§19.12 前置① 已在该形态上三机跑绿）。
     ⟹ **不必换系统 slang**（换了反而作废 `95edc8cc…` / `54e40665…` 两个逐位锚）。
   - 🔴 **规格没写的一条硬约束**：`DispatchMesh` 的 payload 是**必填泛参**（不传 =
     `error[E30439]: generic parameter could not be inferred`），而 slang 把 mesh 侧**没读过**的
     payload 从入口接口里**整个裁掉**（实测：入口接口只剩 position/indices）⟹ mesh 侧必须
     **真读一次**，否则 task 有 payload 输出、mesh 无 payload 输入 = 两侧不同向。
   - 🔴 **DCE 陷阱（本探针唯一的假绿来源）**：`drawID` 若只喂给一个死存储/死 payload，
     Slang 会把 builtin 整个删掉 ⟹ 触发源消失 ⟹ "bistro 在"变成**必然**而不是结论。
     守卫必须让值参与**运行时判定**（上面那句编译器无法证伪）。
   - 🔴 **验收不看"编译过了"**：必须确认产物里真有 DrawIndex builtin，否则触发源不存在。
     已实测可用的命令（`spirv-dis` 在 `/usr/bin`，token 是 `BuiltIn.DrawIndex` 不是 `DrawIndex`）：
     ```bash
     spirv-dis build/rgl/generated_shaders/shadow_meshlet_task.task.spv | grep -c BuiltIn.DrawIndex
     ```
     **必须 ≥ 1**。对照基线（本机已量）：`shadow_meshlet.mesh.spv` = 0、
     `shadow_meshlet.mesh_MDI_SHADOW.spv` = 1（同一命令顺带可验非 MDI 那份仍恒 0 = 逐位不变的 SPV 面证据）。
   - `compile_slang` 不传 `-stage`（slangc 从 `[shader(...)]` 属性推阶段，见
     `cmake/RglSlang.cmake:37-63`）⟹ **CMake 侧零门槛**，rgl/shaders 里此前没有 `.task` 先例，
     这是第一枚（产物名 = 把 `.slang` 换 `.spv` ⟹ `shadow_meshlet_task.task.spv`，双扩展名天然不撞）。
2. `rgl/shaders/shadow_meshlet.mesh.slang`：加第三档 `MDI_SHADOW_TASK` = 现有 `MDI_SHADOW` 分支
   但 `drawID` 恒 `0u`（= §19.9 已证的"入口不声明 `SV_DrawIndex`"那一臂，已知单独不毒化）。
3. `rgl/CMakeLists.txt`：task 用**无 define** 的 `rgl_shader(shadow_meshlet_task.task SHADOW_MESHLET_TASK_SPV scene scene_tables)`
   （`rgl_shader_defined` 第三参是 DEFINE 不是 DEP，别误用）；mesh 第三档用已有的
   `rgl_shader_defined2(shadow_meshlet.mesh SHADOW_MESHLET_MDI_TASK_MESH_SPV MDI_SHADOW MDI_SHADOW_TASK scene scene_tables)`
   ⟹ 产物 `shadow_meshlet.mesh_MDI_SHADOW_MDI_SHADOW_TASK.spv`（define 拼接序 = 参数序，
   与 registry 侧路径拼接约定一致，见 `cmake/RglSlang.cmake:110-125`）。
   这样 shader 侧只需把入口那段的条件从 `#ifdef MDI_SHADOW` 改成
   `#if defined(MDI_SHADOW) && !defined(MDI_SHADOW_TASK)`，行表读取块两档共用，改动面最小。
4. `rgl/scene_config.cppm::PipelineSpec`（:19）：`+ std::string taskSpv;`（**空 = 无 task stage**，
   其余条目零影响）。
5. `rgl/shader.cppm::ShaderEffect`：`+ std::vector<uint32_t> taskStageSpv;`；`loadShaderEffect`
   按 `spec.taskSpv` 非空可选读入。
6. `rgl/shader.cppm::RenderPipeline` 构造（:593-628）——**本探针真正的成本所在**：
   - `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT`（:598 现无条件带）**改为条件**：有 task stage 时**不能带**
     （VUID-…-08695 的反向要求；`tests/test_mdi_drawindex.cppm` 的 shader-object 变体注释记过这条，
     漏 flag 的历史代价 = NVIDIA Xid 13，见 `docs/nvidia_compatibility_report.md` §十）。
   - `createInfos` 由 2 条变 3 条，**nextStage 链要重接**：`task.nextStage = MESH`、`mesh.nextStage = FRAG`
     （现在只有 mesh→frag）。
   - task 的 createInfo 同样要 `DESCRIPTOR_HEAP_BIT | INDIRECT_BINDABLE_BIT` ＋ **同一个 `mappingInfo`**
     （照 mesh 那份写，binding 映射是条目级的，不分 stage）。
   - `bindShaders` 第 5 槽（`eTaskEXT`）从 `nullptr` 换成 task 句柄。
   - `stats.handles += 2`（:622 注释"mesh + frag"）要跟着变——**否则 shader object 句柄计数从此是假的**。
7. `rgl/shader_registry_shadow.cppm`：探针期**只改一行**——`addShadowMeshletMdiEntry` 里
   `findSpec("shadow_meshlet_mdi")` → `"shadow_meshlet_mdi_task"`（spec 表新增该条：
   `meshSpv` 指向 `_MDI_SHADOW_TASK` 产物、`taskSpv` 指向新 task 产物、`fragSpv` 复用 `shadow.frag`）。

#### 判据（三态，含两条前提自检）

| 读数 | 结论 | 后续 |
|---|---|---|
| bistro **在** | DrawIndex 换 stage 即绕开毒化 | 选项②成立 ⟹ 可立"task 化 + mesh ≤128 循环化"组合手术批 |
| bistro **没了** | 毒化与 stage 无关（"这条提交里声明过 DrawIndex"就够） | **选项②作废** ⟹ 回选项①（Mesa issue + 门默认关） |
| 图与 `b1c6afca` **逐位相同** | task stage 根本没生效 | **判据不成立**，先查 1/3/6 步（十有八九是 DCE 把 builtin 删了） |

前提自检（任一不过就别读结论）：① SPV 里有 `BuiltIn DrawIndex`；② `mdi.count=3 / mdi.rows=4590`
照旧；③ 启动 INFO 新条目 475 / 旧 474 / `entries_total=476` 照旧；④ 图**必须不同于** `b1c6afca`
（task 恒发 1 group ⟹ 阴影形态必变）。

#### 命令（本机 = STRIX_HALO，出事机）

```bash
just reflect && just build
python3 tools/smoke.py shot --name t_factory --frames 8
python3 tools/smoke.py shot --name t_task    --frames 8 --set meshlet.mdi_shadow=true
./build/tools/imgqa/imgqa diffblock --a build/rgl/screenshots/t_factory.png \
                                    --b build/rgl/screenshots/t_task.png
```

判读**不用哈希相等**（阴影故意错），用 `diffblock` 的连通域签名：bistro 消失的签名 = 单个
~31.6 万 px 巨型连通域 + `bbox≈[332,148,948,572]` + 最厚 ~150px（§19.9 实测）。
**没有**这种整块 = bistro 在。

#### 红线

- **别为了"让阴影对"去接行表 / payload** ⟹ 那就成了手术，本探针的价值恰恰是便宜。
- **别拿逐位相等当判据**（阴影必错，必然红，会把人误导成"毒化"）。
- 探针**不入库**：spec 表那条 `shadow_meshlet_mdi_task` 与 `findSpec` 的改行属临时物，收线时删掉；
  若 task stage 支持（第 6 步）判为要留，就单独一笔提交并写明"零消费者"（★M-1a 的先例）。
- 结论写 **§19.15**，并**就地改写 §19.12 裁决卡**（别新开"更正"段留两层误导后来者）。

#### 顺带：给 Mesa issue 补的一条新证据

§19.13 的"rig 层测不出"不是坏消息——它把触发条件从"mesh 入口声明 DrawIndex"**收窄**了：
经典管线 / shader object / 1 与 256 线程 / 同 pass 与跨 pass 全部不复现，
**唯一未排除的结构差 = `VK_EXT_descriptor_heap`**（引擎的 descriptor 来自 heap 烘焙偏移）。
issue 里该点明这一条，它比"mesh+DrawIndex 就坏"精确得多，也更好复现（引擎九臂矩阵即最小复现骨架）。
同 Mesa 26.2.3 在 gfx1036（RAPHAEL）逐位绿、gfx1151（STRIX_HALO）毒化 = 现成的家族对照。
**§19.15 又收窄一层**（task 入口声明不触发），现行靶以 §19.15 末那条为准。

---

### 19.15 ★M-3T 执行记录与裁决：**task 入口声明 DrawIndex 不毒化** ⟹ 选项② 成立（2026-10-13 实测，机器 = 出事机 8060S / RADV STRIX_HALO / Mesa 26.2.3）

**裁决（一句话）**：`DrawIndex` 声明在 **task** 入口（mesh 入口不声明），同提交后续 mesh draw
正常产出像素，主视图 G-buffer **逐 bit 未动** ⟹ §19.9 的毒化收窄到"**在 mesh 入口**声明
`DrawIndex`"，**选项② 成立**，可立"task 化 + mesh ≤128 循环化"组合手术批。

#### 前提自检（四条 + 两条顺带，全过；任一不过则本节结论不成立）

| # | 自检 | 读数 |
|---|---|---|
| ① | task SPV 里有 DrawIndex builtin（🔴 **本探针唯一的假绿来源 = DCE**） | `shadow_meshlet_task.task.spv`：`OpEntryPoint TaskEXT %main "main" %17 %__EmitMeshTasks_Payload` + `OpDecorate %17 BuiltIn DrawIndex`，`grep -c BuiltIn.DrawIndex` = **1** |
| ①′ | 三枚 mesh SPV 的 builtin 面没漂 | `shadow_meshlet.mesh.spv` = **0**（非 MDI 那份没动）· `_MDI_SHADOW.spv` = **1** · `_MDI_SHADOW_MDI_SHADOW_TASK.spv` = **0** 且 `%taskPayload` **在入口接口里**（payload 活着、没被 slang 裁掉） |
| ② | MDI 真在下发 | `mdi.count=3` / `mdi.rows=4590`（与 §19.9 / §19.12 读数逐字同） |
| ③ | 条目序没漂 | 启动 INFO 新条目 **475** / 旧 `shadow_meshlet` **474** / `entries_total=476`；行表槽 8258 |
| ③′ | **task 句柄真建了**（机器可读，比"编译过了"硬） | `handles` 出厂 **958 = 2×479** → 门开 **961 = 2×480 + 1**（多的一枚正是 task） |
| ④ | 图必须不同于 `b1c6afca` | 探针臂 = **`da768bfb…`** ≠ `b1c6afca…` ✅ |

#### 判据读数（三态判据落在第一格）

- 探针臂终图 = **`da768bfb…`** —— 与 §19.9 九臂矩阵"门开 + **不发** MDI 命令"那一格**逐位相同**，
  而那一格记录的正是 **"bistro 在"**。成因自洽：task 恒发 1 组 + 🔴 `SV_GroupID` 在 task 之下是
  **本 task 内的局部 id**（spec：mesh 阶段 WorkgroupId = "position within the current mesh task"）
  ⟹ 恒 0 ⟹ 每 draw 只画第一个簇 ⟹ c1..c3 阴影对终图的贡献塌到 0 px，与"整段不发"同图。
- `diffblock(factory vs 探针)`：M = 50,161 px / 2202 个连通域 / 最大 27,098 px @
  `bbox=[434,311,312,409]` / 最厚 **30.56px**。**没有** bistro 消失签名
  （315,692 px / `bbox=[332,148,948,572]` / 最厚 150.16px）。
- 🔴 **探测器会响（§19.13 的教训就地补上，否则这条绿又是空的）**：同 session 用**改前构建**复现
  `b1c6afca`，`diffblock` 打出的正是 §19.9 记录的三个数（315,692 / `[332,148,948,572]` / 150.16）
  ⟹ 同一台机、同一 Mesa、同一判据，**响与不响可分辨**。
- **零阈值仪器定案（比 diffblock 强一档，判"主视图几何动没动"该用它）**：
  `gbuffer_albedo`(ch0) = `ea84ed5e…`、`gbuffer_metalrough`(ch3) = `7df898fd…`，出厂臂与探针臂
  **逐字节相同**（且与 §11.4 演练读数同）。G-buffer 由**主视图几何 pass** 写、天然不带阴影
  ⟹ 主视图几何（含 bistro）**一 bit 未动**。bistro 若被毒化，ch0 会有 34% 屏幕归零，
  零阈值逐位对拍直接抓走，不需要"看连通域像不像整块"这种解释。
- **task 真跑了的阳性对照**（排掉"task 建了但没执行"这一假绿）：shadow1/2/3 GPU 时间戳 =
  **0.774 / 0.645 / 0.512 ms**，**高于**同次 profiler 会话的出厂对照 0.512 / 0.500 / 0.359 ms
  （窗口 120 / warmup 60 / `no_vvl` 臂）。两臂 mesh 组数同为 ΣgcX，多出来的正是每 mesh 组一次
  task 调用。
- VVL 常开冒烟 + `--sync`：**errors=0 / hazard=0**。

#### 附加实验（**超出 §19.14 规格**，主判据不受它污染）

主判据出完之后追加一格：把 mesh 的 `drawID` 从恒 `0u` 换成 payload 里的值（守卫同时改成按
`mdiShadowArgs.GetDimensions` 派生的行表上界兜底，不给"越界读行表"的机会）：

- 终图 = **`0d0a17fc…`**，与出厂只差 **2721 px = 0.295%**、最厚 **5.73px**、363 个细碎连通域
  （对比 `drawID` 恒 0 那臂的 50,161 px / 最厚 30.56px）。G-buffer ch0/ch3 仍逐位相同。
- ⟹ **task→mesh 的 payload 通路在 gfx1151 上通，且 task 侧 `SV_DrawIndex` 给的是正确的段内行号**
  ——行寻址 `rowBase + drawID` 几乎已把阴影复原，残余只来自"每 task 只发 1 组 ⟹ 每对象只画第一个簇
  的前 85 三角"这一条**探针故意留下**的错误。
- ⚠ **证据边界**：这**不是** A5 级闭式判据（"每对象只画第一簇"本身没有闭式锚），它只证"行号对到
  让阴影几乎复原"。若 drawID 是"变化但错"的值，行会散到别的对象上，差值不可能只剩 0.295%。
  严格逐行相等要等组合手术批的 A5 锚。
- 🔴 **这笔流程账要记**：§19.14 红线写的是"别为了阴影对去接 payload"。实际执行顺序 =
  **先按规格出主判据（`drawID` 恒 0 那臂 + G-buffer 逐位）→ 再追加实验**，主判据没有被追加实验
  回头改写，追加实验也没入库（随探针一起 revert）。红线当时防的是"把探针做成手术"——
  判据先定案、实验后追加，这个顺序是这条红线能不能这么用的关键，别只记结论。

#### 成本读数（给下一批拍板用，**非干净 A/B**）

| 臂 | shadow 组 GPU 合计 | frame p50 |
|---|---|---|
| 出厂（c0 旧路 + c1..c3 簇 LOD5） | 2.149 ms | 4.790 ms |
| task 探针（同 mesh 组数 + 每 mesh 组一次 task） | 2.685 ms | 5.475 ms |

⟹ 本形态下 task 阶段在 shadow 组上的量级 ≈ **+0.54ms**（+25%~+50%/级联）。🔴 **别当纯 task 开销读**：
两臂 mesh 组数相同但**内容不同**（探针臂每对象只画第一簇）。真要定 task 化净成本，等组合手术批
拿正确形态（task 读行表决定 `DispatchMesh(2*count, 1, 1)`）再量。

#### 与规格的两处偏差（都朝"更小"，不是扩大形态）

1. task 的 `rgl_shader` **不带 DEP**（规格第 3 步写了 `scene scene_tables`）：本 task 入口不 import
   任何模块，R1 查的是"import 闭包 ⊆ DEP"，无 import ⟹ 空 DEP 才正确（挂了反而给 task 加一条
   假编译前置）。
2. mesh 侧 payload **必须真读一次**（规格没写）：见 §19.14 第 1 步补的那条。读出来喂一条
   "正常值下恒不触发"的守卫，几何与 `MDI_SHADOW` 档同。
3. 另记一条构建面事实：`PipelineSpec` / `ShaderEffect` 的新字段**必须带默认成员初始化器 `= {}`**——
   本构建开 `-Werror,-Wmissing-field-initializers`，不带 DMI 则全部 20 处三元素 brace-init
   （含 `shader_hot_reload_impl.cppm` 的 `ShaderEffect{mesh, frag}`）当场编不过。

#### 给 Mesa issue 的再收窄（接 §19.14 末段，现行靶）

gfx1151（Strix Halo）+ **mesh 入口**声明 `DrawIndex` 毒化同提交后续 mesh draw；
**同一提交里改由 task 入口声明 `DrawIndex`（mesh 入口不声明）⟹ 不触发**（§19.15）。
⟹ 排除"`DrawIndex` builtin 本身在 gfx1151 仿真路上坏了"，嫌疑钉在 **mesh 阶段的 DrawIndex 取值
路径**；再叠加 §19.13 的"经典管线 / shader object / 1 与 256 线程 / 同 pass 与跨 pass 全部不复现，
唯一未排除结构差 = `VK_EXT_descriptor_heap`"。同 Mesa 26.2.3 的 gfx1036（RAPHAEL）逐位绿 = 家族对照。

#### 下一批（组合手术）的输入事实

| 事实 | 状态 |
|---|---|
| task 路在 gfx1151 无毒化 | ✅ 本批 |
| task→mesh payload 通、`SV_DrawIndex` = 段内行号 | ✅ 本批（附加实验） |
| shader object 侧 task 接入形态（NO_TASK bit 条件化 / nextStage 链 / mapping 共用 / `bindShaders` 第 5 槽 / `stats.handles` 口径） | ✅ 本批跑通，**形态记在 §19.14 第 6 步**（代码不入库，见下） |
| NV 的墙：mesh 256 > 128（task 上限亦 128，两墙正交） | ⛔ 未解，那一批一并解（§19.11） |
| ANV BMG G21 / RAPHAEL 已绿 | ✅ §19.10 / §19.12 |
| 待做 | mesh 循环化 ≤128 + task 读行表定 `DispatchMesh(2*count,1,1)` + `gcX` 语义从 mesh 组数换 task 数（`kSlotsPerCluster=2` 是 mesh-only 产物）+ A5 闭式锚重算 + 逐位锚各机重立 + STRIX 复测收线 |

#### 探针处置（用户拍板：全部 revert，知识只留文档）

`shadow_meshlet_task.task.slang`、CMake 两行、spec 表那条 `shadow_meshlet_mdi_task`、`findSpec`
改行、mesh 的 `MDI_SHADOW_TASK` 档、以及第 4/5/6 步的引擎侧 task stage 支持 **全部 revert**
（`git status` 回干净、出厂锚 `95edc8cc…` 复现、`handles` 回 958）。
理由：下一批尚未拍板，留一套无消费者、无测试、且已把 `RenderPipeline` 构造顶过样式阈值
（129>120 行 / 复杂度 21>15）的死代码不划算；**可重做的知识**（含 slang 拼写、payload 两侧真读、
DMI、NO_TASK 反向 VUID、nextStage 链）全部落在 §19.14 + 本节，那是一份**已执行过且已纠错**的规格。

#### 顺带：设备能力现读口（用户提的"别写死"）

§19.12 四机矩阵此前把 mesh/task 上限与 granularity **手抄进表**，而引擎侧的查询面撑不起它：
`maxMeshOutputVertices` 等只在 `checkMeshletDeviceFit` **不够用时**才 warn，且调用点排在
`wireMeshletSwitch` 的门关早退**之后** ⟹ 门关就查不到；task 侧上限与两个 granularity 全仓从未查询。
（`graphicsIndirect` 反倒是够的——`DGC path:` 三条分支都带，只是 smoke 摘要没打这个字段。）

落地 = `rgl/utils.cppm::MeshTaskDeviceCaps` + `logMeshTaskDeviceCaps`，紧跟
`Selected rendering GPU` **无条件**打一行；smoke 摘要多一行 `设备规格`（含从 `DGC path:` 行拼来的
`graphicsIndirect`）。**现读口 = `python3 tools/smoke.py smoke` 的 `设备规格` 行。**

🔴 一条不显然的口径：能力必须描述**引擎实际选中的那台**设备。`pickPhysicalDevice` 是打分制选一台，
本项目又跑 `run-igpu/dgpu/intel` ⟹ **独立 vkinfo 式工具会报"枚举到的某台"，与引擎选路可以不一致**。
这就是现读口长在选设备之后、而不是做成 `tools/` 可执行的原因。

现读口第一次跑就补上了表里的空格，并给出一条**改变设计取向**的结论：本机 STRIX
`maxMeshWorkGroupInvocations=256` 但 `maxPreferredMeshWorkGroupInvocations=`**`128`**
（`maxPreferredTaskWorkGroupInvocations=64`、granularity 1/1、`maxTaskPayloadSize=16384`）
⟹ 组合手术把 mesh 降到 128 线程**不只是迁就 NV 的硬上限**，RADV 自己的推荐档也是 128。
其余数值（含 payload 预算 16KiB）不再抄进文档——按上面的现读口读。

---

### 19.16 ★M-3T-a 落地：**task 化上线，A3 主判据在出事机由红转绿**（2026-10-13 实测，机器 = 出事机 8060S / RADV STRIX_HALO / Mesa 26.2.3）

**一句话**：把 `SV_DrawIndex` 的声明处从 **mesh** 入口搬进 **task** 入口之后，`meshlet.mdi_shadow=true`
与门关**终图逐位相同**（`95edc8cc…`）——§19.9 那张"门开 = 主视图 bistro 整块消失"的红判据转绿。
一条零阈值判据同时证明两件事：**没毒化**（主视图在）+ **MDI 语义等价**（阴影逐位对）。

#### 形态：`gcX` 的**单位**变了、**数值**没变 ⟹ 行表/执行器/采集侧/A5 锚零改动

| | 改前 | 改后 |
|---|---|---|
| MDI 行 `gcX` 的单位 | mesh 组数 | **task 数** |
| 数值 | 2 × 选级后簇数（每簇固定 2 槽） | **同一个数**（每 task 恰好发 1 个 mesh group） |
| mesh 拿行号 | 入口 `SV_DrawIndex` | `payload.drawID`（task 转交） |
| mesh 拿派发序号 | 入口 `SV_GroupID.x` | `payload.slot`（task 转交） |

🔴 **task 不是"多余的一层"**：spec 里 mesh 阶段的 `WorkgroupId` = "position within the **current mesh
task**"，而本管线每 task 只发 1 组 ⟹ mesh 侧 `groupID.x` **恒 0**。旧派发契约里它承担的那一维
（半区号 + 簇内偏移）**必须**由 task 的 `SV_GroupID`（= 本 draw 内 task 序号，取值域恰好等于旧
`groupID.x`）经 payload 转交，否则每个 draw 只画第一个簇。§19.15 探针"阴影故意画错"的根因就是这条。

改动 7 处：新增 `rgl/shaders/shadow_meshlet.task.slang`（纯中继，1 线程）·
`shadow_meshlet.mesh.slang` 的 `MDI_SHADOW` 分支改收 payload（**非 MDI 分支逐 token 未动**）·
`rgl/CMakeLists.txt` 加 task target（🔴 DEP 为空是对的：纯中继不 import，挂了就是假编译前置）·
`PipelineSpec::taskSpv` / `ShaderEffect::taskStageSpv`（均**末尾追加 + `= {}`**）·
`loadShaderEffect` 非空才读、读不到直接抛 · `RenderPipeline` 可选第三句柄 + `NO_TASK_SHADER_BIT`
条件化 + `bindShaders` 第 5 槽 · 管线表 `shadow_meshlet_mdi` 加第四元素。
另：`shader_hot_reload_impl.cppm` **跳过带 task 的条目**并出声（本工具只认 mesh+frag，重建会丢
task ⟹ mesh 在等的 payload 无生产者 = 静默画错；热重载与 `mdi_shadow` 双双默认关，但"静默错"
不在可接受形态里）。

#### 判据结果（§19.3 的 A1–A7 全中）

| 判据 | 读数 |
|---|---|
| A1/A2 门关 / 出厂 | `95edc8cc…` ✅ 不变 |
| **A3 主判据** | 门开 = `95edc8cc…` **逐位相同** ✅（改前 = `b1c6afca…` 毒化） |
| 门关 SPV 未被污染 | 与 `git HEAD` 版源码重编**逐字节相同**（`cmp` 过）✅ |
| task 产物接口面 | `spirv-dis` 里 `BuiltIn DrawIndex` + `BuiltIn WorkgroupId` 计数 = **2** ✅ |
| A4 阳性对照 | **构造性成立**：task 若没跑就没有 mesh 组 ⟹ c1..c3 影子消失 ⟹ 必不等于 `95edc8cc…`。A3 逐位绿本身即"task 真跑了且画对了" |
| A5 收益 | `mdi.count=3`、`mdi.rows=4590`（= 启动对账闭式锚）、`vkcmd.count` **19994→6452**、`draw.count` 6601→6674（含空行，口径 D5） |
| A6 | `cull_replay` 三档（`cpu`/`gpu_sync`/`gpu_lagged`）× `--sync` = **hazard 0 / errors 0** ✅ |
| A7 | `just test` 419 例 / 93746 断言 · `just test-all` 457 例 / 118713 断言 · `checks --strict` 0 警告 · `reflect` 幂等 ✅ |
| 句柄数 | `handles` 960 → **961**（多的那枚正是 task；口径已同步进 `ShaderObjectStats::handles` 注释） |

#### 变异验红（shader 侧两条，各自落在 §19.15 **已记过**的锚上）

| 变异 | 结果 | 含义 |
|---|---|---|
| `p.slot = 0u`（task 只搬 DrawIndex、不转交派发序号） | `0d0a17fc…` | = §19.15「payload 寻址」那一格 |
| `p.drawID = 0u`（行号不转交） | `da768bfb…` | = §19.15「task 探针 drawID 恒 0」那一格 |

两个维度各退化成一个**此前已独立观测到**的形态，比"随便红一下"强：说明实现路径没有第三条歧途。
还原后复跑 A3 复绿。

#### 代价读数（A3 逐位相同 ⟹ 内容完全一致，差异只可能来自派发形态）

🔴 **口径（当天稍后由 ★M-3T-b 实测修正 → §19.17）**：本机 `frame_p50` **同一份代码连续两跑能差
0.69ms**（实测四次：5.729 / 5.712 / 5.338 / 5.036），而 GPU zone 四次波动 0.014ms ⟹ **A/B 判据只能用
直接消费者的 GPU zone，`frame_p50` 在本机不可用**。故本节只报 `shadow` 口径：门关 `shadow1/2/3`
= 0.516/0.504/0.363（合计 1.383）→ 门开 = 0.777/0.654/0.520（合计 1.951）= **+0.57ms**；★M-3T-b
当天**同会话**复测得 +0.50ms（1.51 → 2.01）⟹ 方向与量级复现。即：**本形态买到的是"能用"，
不是"更快"**——命令数收益（`vkcmd` −68%）在 CPU 侧，而这台机器 GPU-bound，于是 GPU 侧多付的
task 开销直接进 GPU 时间。
**下一批（★M-3T-c）的量化目标因此明确**：task 从"每 mesh 组一个"改成"每 draw 一个"（task 读行表拿
簇数、发 `DispatchMesh(2×簇数,1,1)`），每级联 task 数 5132 → 1530（3.4×），代价应大部分收回。
代价 = `gcX` 语义真变成 task 数（行表构建器 + 采集侧 + A5 闭式锚要跟着重算，簇数得挪进保留 word）。

#### 三条实测纠错 / 教训（带沉淀地址）

1. **`§19.14` 那条"必须加不可折叠守卫防 DCE"是多余的**。实测：task 里 `p.drawID = drawID` 这类
   **payload 输出存储**本身就足以让 `DrawIndex` 与 `WorkgroupId` 都留在接口里（pinned slangc 2026.16，
   无守卫直接编出两个 `OpDecorate ... BuiltIn`）。原守卫防的是"只喂**死**存储"，而 task payload 是
   输出接口、slang 看不见 mesh 侧就不敢裁。生产形态因此是干净中继。`DOC:`（本条）
2. **"没有这个 stage"和"这个 stage 解析失败"不能用同一个值表示**。落地时两者都写成 `std::nullopt`
   ⟹ 全部 **479 条**条目被误判成解析失败、退回全烘 + 启动刷屏，而**终图逐位不变**（全烘是"恒正确"
   的失效方向）⟹ **A3 照样绿，唯一信号是人眼扫到 `warnings=479`**。修法 = 不存在的 stage 根本不进
   列表（列表里没有 = 不存在，无需第三种状态）。
   `TEST: tests/test_shader_mapping.cppm`（用例 1 直接钉这一格；五条变异实测全红）。
   **可复用的判据学结论：逐位图像判据对"启动面/性能回归"和"朝正确方向失效的过滤"天然失明 ⟹ 验收
   必须同时看 smoke 的 `warnings` 计数，不能只看哈希。**
3. **纯函数就该在 `just test` 里测**：`declaredBindingsUnion` / `bakeBindingMappings` 不要 VkDevice，
   按分层判据（"这段逻辑需不需要一个 VkDevice 才能跑"）从 `RenderPipeline` 构造函数抽成
   `shaderDetail::` 具名函数并导出（顺带把构造函数从复杂度 28 / 103 行降到阈值内，样式 0 警告）。
   判据产物 `tests/shaders/mapping_task.task.slang` 的唯一职责 = 声明一个 tests 里所有 mesh/frag
   都不声明的 binding 3——**否则"task 的声明进不进并集"两种实现给出同一个集合，测不出来**（仓内既有
   task 产物声明的 binding 0 与每个 mesh 产物重合，拿它是假绿）。

#### 一条未归因的单像素离群（`DOC:`，别再当新账）

门开首跑出现过一次 `c7d5ddf3…`：与基准差 **1 个像素**（`imgqa diffblock` bbox=`[697,319,1,1]`，
仅 1 字节不同、不足 1 个 sRGB 亮度台阶，`U(2)=0` 判据 PASS）。此后 **16 次门开 + 4 次门关**
（含 3 次紧跟 `just test-all` 的同前提复跑）**全部**回到 `95edc8cc…`，未再现。
不是阴影语义差（那会是成片区域）。**处置**：A3 作为闸门若撞哈希不符，先 `imgqa diffblock` 看厚度
分布再定性——单像素亚台阶属未归因环境级离群，重跑即可；成片/成域差异才算本批判据的真红。
不要为这一条立债：它既不可复现、也不在本批改动面上（门关路径同样可能中招，只是尚未观测到）。

#### ★M-3T-b（mesh ≤128）的输入事实 → **已落地，见 §19.17**

---

### 19.17 ★M-3T-b 落地：两枚 meshlet mesh 线程数 256→128 + strided 循环（2026-10-13，机器 = 8060S / RADV STRIX_HALO）

**一句话**：`meshlet.mesh.slang` 与 `shadow_meshlet.mesh.slang` 的 `[numthreads(256)]` 降到 **128**
并把两段输出 `if` 改成 strided 循环，schema 常量同步降档 ⟹ NV 的 `vkCreateShadersEXT` 超规墙
（VUID-RuntimeSpirv-MeshEXT-07295/07298）拆除，而**三枚逐位锚全部不变**。

#### 为什么拆两批跑（b1 阴影 / b2 主 pass）

两枚 shader 的改法同构，但风险面差一个量级：主 pass 是出厂默认主视图路径。b1 只改阴影那一枚
（schema 不动 ⟹ 闸门不变 ⟹ 对 NV 兼容性零贡献），**唯一目的**是用最小面把"128 线程循环化会不会
引起 slang fp 重排漂移"这个问题隔离实测。b1 三锚全中 ⟹ b2 照抄改法的风险已被前置证明压低。
这个拆法是对的：b1 若漂，b2 就要换方案（例如降 `kTaskTris` 让线程数 ≥ 输出元素数），而 b1 没漂。

#### 改法要点

- **`numthreads` 直接吃常量**：`static const uint kMeshletMaxWorkGroupInvocations = 128u;` +
  `[numthreads(kMeshletMaxWorkGroupInvocations, 1, 1)]`（实测 pinned slangc 2026.16 支持，产物
  `OpExecutionMode ... LocalSize 128 1 1`）⟹ 现有 `meshlet_spec_checks` 规则 1 自动把
  schema ↔ shader 钉成相等，**不需要为"值写错"再加护栏**。
- **strided 循环**：`for (uint v = ti; v < 3u*nTri; v += 线程数)`，体内 `ti` 全换 `v`（主 pass 那枚
  还要换 `#if MESHLET_TINT` / `#else` 分支里的 `verts[ti]`，**漏一个就是画错**——亲验全文 `ti`
  只剩入口声明 + 两个循环初值共 3 处）。索引段今天 `nTri ≤ 85 < 128` 恒跑一轮，改循环是为了让
  "线程数 < 输出元素数"这类未来漂移不会静默少写。
- 输出数组 `verts[256]` / `triangles[85]` 与 `kTaskTris` **未动**——那是 `maxMeshOutputVertices`
  的约束（NV 也是 256），不是本批的墙。
- 🔴 **中途一度必须两个名字**：b1 期间 schema 还是 256、shader 要 128，若沿用 schema 常量名会被
  规则 1 当场判成规格漂移（规则 1 的语义是"同名必须等值"）。b2 schema 降档后才并回同一个名字。
  教训已写进 shader 注释：**两个量只要此刻不是同一个量就必须分开名字，确认是同一个量之后就应当
  并回**，否则留下一份没人对账的平行拼写。

#### 判据（主 agent 亲跑，非采信子 agent 读数）

| 判据 | 实测 |
|---|---|
| 出厂默认 | `95edc8cc…` ✅ **逐位不变**（主 pass 簇路径是它的直接生产者 ⟹ 循环化零漂移） |
| 门关旧路径（红线 A） | `54e40665…` ✅ 不变 |
| 门开 MDI | = 出厂 ✅（§19.16 的 A3 仍绿） |
| 五枚 SPV 接口面 | `meshlet.mesh` / `_MESHLET_DEBUG_OFFSET` / `_MESHLET_TINT` / `shadow_meshlet.mesh` / `_MDI_SHADOW` **全部** `LocalSize 128 1 1` |
| 反射 | 生成物 = 128，`just reflect` 幂等（9/9 生成物哈希不变） |
| 测试 | `just test` 419 例 / 93746 断言 · `just test-all` 457 例 / 118713 断言 · `checks --strict` 0 警告 |
| smoke | 三次均 `errors=0 warnings=0` |

#### 新增构建期护栏：`numthreads` ≤ 设备下限

**为什么规则 1 不够**：它的正则只在"shader 里真出现了那个常量"时才生效。把常量删掉、`numthreads`
写回 `256` 是一条**全绿**路径（规则 3 的死常量判据也救不了——`checkMeshletDeviceFit` 恒是该常量的
消费者），而后果正是本批在修的那个 bug 的再武装。新判据 = 任一 `rgl/**/*.mesh.slang` 的
`[numthreads(N,…)]` 之 **N ≤ `kMeshletMaxWorkGroupInvocations`**（= 三机最小值 = NV 硬上限）。
用"≤"而非"="：`cube/light/skybox` 是 3、`model/shadow/izb_mask` 是 32，本来就远低于上限。
🔴 **三条失效路径全部出声**（schema 缺常量 / 某枚 mesh shader 解析不到 `numthreads` / 线程数是
还原不出数值的标识符），并把原有那条"constants 块整块解析不到就静默 return"也改成出声——
四条判据空跑却零输出，同属"失效不出声"。
**阳性自证（主 agent 亲做）**：把 `cube.mesh.slang` 的 32 临时改 256 ⟹ `just checks --strict`
出 1 条点名 VUID-07295/07298 的警告并 exit 1；`cp` 还原后归零。

#### 🔴 本批最重要的产出：本机 `frame_p50` 不能当 A/B 判据

同会话、同一台机器、**同一份代码**的 `frame_p50` 四次读数 = **5.729 / 5.712 / 5.338 / 5.036**
（跨度 **0.69ms**），而同期 `shadow` 合计四次 = 1.512 / 1.507 / 1.521 / 1.512（跨度 **0.014ms**）、
`geometry` 四次跨度 0.019ms。同会话 A/B（只退 `meshlet.mesh.slang` 一份 shader、只重编 shader）：

| 态 | shadow 合计 | geometry gpu |
|---|---|---|
| 主 pass 128（本批） | 1.512 / 1.507 | 2.341 / 2.331 |
| 主 pass 256（对照） | 1.513 / 1.514 | 2.337 / 2.344 |

⟹ **降档 + 循环化的性能代价 = 0**（两个直接消费者指标一致到 ±0.01ms）。
连带两条必须记住的账：① 本批之前记在 §19.16 / commit 里的"frame_p50 +0.14ms""4.849 基线"
**是噪声，已就地改掉**；② 任何跨会话的 `frame_p50` 对比（含 `just cpu` 的历史基线表）在本机
**都不可直接引用**，要 A/B 就同会话跑、且优先读直接消费者 zone → 口径已写进
`docs/cpu_perf_plan.md`。

#### 边界（别读宽）

**本批只让 shader 在 NV 的规格内**，本机不是 NV ⟹ "NV 上现在能跑 MDI"仍是**推定**，
要收线必须去 NV 机跑 `--set meshlet.mdi_shadow=true` 的 A3（债-81 的换机面）。
---

### 19.18 ★M-3T-c 落地：task 从"每 mesh 组一枚"收成"每 draw 一枚"，行寻址整体搬进 task（2026-10-13，机器 = 8060S / RADV STRIX_HALO）

**一句话**：task shader 不再只当中继（★M-3T-a 的"残废形态"），它现在**读行表**拿身份与 mesh 组数、
一次 `DispatchMesh(meshGroupCount,1,1)` 发完整批 ⟹ 每级联 task 数 **4724 → 1530（3.03×）**，
而**三枚逐位锚全部不变**（门开 MDI 仍 = 出厂 `95edc8cc…`）。

#### 形态（三处协同，中间态是坏的，故一批做完）

| 处 | 改动 |
|---|---|
| 行表 word[0] `gcX` | 🔴 **单位从 mesh 组变成 task**：`可见 ? 1 : 0` |
| 行表 word[5] | `reserved0` → **`meshGroupCount`**（= `meshletTaskCount(选级后簇数)` 的成品数，**与可见性无关照写**）。🔴 占用了 §19.2 D1 留给 ★M-3 的那枚保留 word ⟹ ★M-3 真要加"材质/顶点块"那一维得把行扩到 32B |
| task | 读 `row = rowBase(push.word0) + SV_DrawIndex` 的 word[3]/[4]/[5]，payload 只带 `{objectIndex, clusterStart}`，`DispatchMesh(meshGroupCount,…)`。**不再声明 `SV_GroupID`**（每 task 发多组后它恒 0，留着就是死输入） |
| mesh | 派发序号**回到 `groupID.x`**（spec 语义 = "position within the current mesh task"，取值域 `[0, meshGroupCount)` **逐值等于**旧 mesh 组号）；身份从 payload 取；**不再声明 binding 34** |

两个设计收益比原计划更干净：① mesh 的 `groupID.x` 复活 ⟹ **MDI 与非 MDI 两条路径的算式完全同形**
（只差身份从 push 还是 payload 来），`half` 那两段 `#ifdef` 直接收成一行；② mesh 不认识行表 ⟹
**行表契约的 shader 侧拼写从两份收成一份**，§19.16 记的那笔"契约两份拼写"重复顺带消掉。
🔴 ×2（每簇 2 槽）**仍只拼在 C++ 侧**（`meshletTaskCount`）——行里直接带成品数，task 里不出现 ×2，
否则就是第三个真相源。

#### 判据（主 agent 亲跑）

| 判据 | 实测 |
|---|---|
| 出厂默认 / 门关旧路径（红线 A）/ 门开 MDI | `95edc8cc…` / `54e40665…` / **= 出厂** ✅ 三锚全不变，三次 smoke 均 `warnings=0` |
| task SPV 接口面 | `BuiltIn DrawIndex` = **1**、`BuiltIn WorkgroupId` = **0**（本批改判据：★M-3T-a 是 2）、`OpEmitMeshTasksEXT` = 1、binding 34 在此侧 |
| mesh MDI SPV | binding 34 引用 = **0**、入口含 `gl_WorkGroupID` + `mdiPayload` |
| 测试 | `just test` **422 例 / 94022 断言**（+3 用例）· `just test-all` **460 / 118990，0 skipped** · `checks --strict` 0 警告 |
| syncval 三档 `cull_replay`（cpu / gpu_sync / gpu_lagged）× 门开 | **hazard 0 条**，三档全 PASS |

#### 收益实测（探针取数，见下）

每级联（真产物 bistro 场景，`mdi.count=3`）：

| 级联 | 行数 | task 数（= 可见行） | 可见行 mesh 组总数 |
|---|---|---|---|
| c1 | 1530 | **1530** | 4724 |
| c2 | 1530 | **1511** | 4636 |
| c3 | 1530 | **1476** | 4324 |
| Σ | 4590 | **4517** | **13684** |

⟹ task 数 **13684 → 4517 = 3.03×**（★M-3T-a 形态下 task 数 = mesh 组数）。
`sumMeshGroupCount`（13684）是**跨形态对账量**：逐条 draw 形态的 Σ groupCount 必须等于它
（A3 逐位相同是它的图像侧证据）。

#### 🔴 负收益：task 化在 GPU 侧仍是净亏，且残余不随 task 数缩放

同会话、直接消费者 GPU zone（`shadow1/2/3` 合计，口径 → `docs/cpu_perf_plan.md` §1 条 4）：

| 形态 | shadow 合计 | 相对逐条 |
|---|---|---|
| 门关（逐条 draw，无 task） | 1.519 / 1.504 | — |
| 门开 ★M-3T-a（每 mesh 组一枚 task） | ≈2.01（当天早先同机读数） | **+0.50ms** |
| 门开 ★M-3T-c（每 draw 一枚 task） | **1.851 / 1.864** | **+0.35ms** |

**task 数降 3.03× 只收回 0.15ms**（0.50 → 0.35）。⟹ 残余 0.35ms **不是 task 数量的开销**，而是
amplification 阶段本身插进了一条**依赖级**（mesh 组必须等它的 task 跑完才能开始），那部分延迟不随
task 数缩放。结论落定：**本机（GPU-bound）上 MDI+task 的阴影段比逐条 draw 慢 0.35ms**，命令数收益
（`vkcmd` −68%）只在 CPU-bound 的开发环境（开 VVL）里才可能兑现。
**门 `meshlet.mdi_shadow` 保持默认关的理由，从"没量过所以不敢开"变成"量过了：GPU 侧净亏"**。
★M-3T-c 原定的量化目标（"代价应大部分收回"）**没达成**，如实记这条，别读成"再优化一下就好"。

#### 两条被当场抓出的错（都不是渲染错，但都会骗过逐位判据）

1. **`sumMeshGroupCount` 口径写错**：第一版按**全部行**求和。探针一跑发现三档级联这个数一位不变
   （恒 4724）——因为不可见行的 word[5] 照写。而逐条 draw 形态**不为不可见候选发 draw**，
   全行求和就不是"与逐条形态相等"的那个对账量了（一个不随可见性动的死数 = 对账价值为零）。
   改成只累加可见行后读数变成 4724/4636/4324（随可见性动）⟹ 口径修正。**教训**：新加聚合锚
   必须当场看它**会不会随它声称要反映的那个量动**，不动就是死数。
2. **收益没有运行期计数器**：`mdi.count` / `mdi.rows` 有，**task 数没有**。本批的头条收益
   （13684 → 4517）只能靠一次性探针（插日志 → 跑 → 还原）取数。已挂 **债-113**。

#### 测试面（子 agent 执行、主 agent 亲验）

- `just test` 编译不过 = 本批语义改动的**第一个信号**（`reserved0` / `taskCount` 都不存在了）。
  改名之外真正的语义重写：`reserved0 == 0` 那类"保留位必须为 0"的断言在新语义下是**错的**，
  换成"word[5] = 候选的 meshGroupCount 且不可见行照写"。
- 三条新用例：`Σ gcX == rowCount − emptyRows`（3b）· `sumMeshGroupCount` 只算可见行 ·
  **只换 word[5] 的两行互换**必须红 `invariant 4`（不加这条，"meshGroupCount 进逐元素比对面"
  这件事没有任何测试守着）。
- 设备自证回路（`tests/shaders/mdi_shadow_rows.mesh.slang`）自证面从 word[3]/[4] **扩到 word[5]**
  ——本批起 word[5] 是真载荷（真机上 task 拿它当 `DispatchMesh` 的 X）。
- 🔴 **tag 公式的单射性不是天生的**：`obj×16 + start×4 + groups` 会进位（groups ≥ 4 吃 start 的位）。
  主 agent 补机器断言时**当场撞出一格碰撞**：行 A 的 `(obj, groups, obj) = (37,5,37)` 与正确 tag
  同为 649。那是**两字段同时错位**（不可能是任何单一 bug 的形状），故接受覆盖面 = 单字段错位 +
  整体错一格 word，并把这条边界**写进断言注释**（换用例取值时断言会响，不会静默）。
- 变异红线 24 条实跑。**🔴 一条如实申报的存活**：M23（3b 判据短路）红 **0** 条——`validate` 路径上
  逐行对账（invariant 4）先跑，任何来自候选的表都到不了 3b ⟹ **3b 目前是哨兵不是判据**，
  专属信号只在构建器自检路径（M20「×2 乘回 gcX」时逐字报 3b）。组合变异 M24（M23+M20）红 10 条
  ⟹ 哨兵短路不静默。保留 3b 的理由：它是"锚自洽"类判据，若以后有人把 gcX 从逐元素比对面摘掉，
  它是唯一的兜底。

#### 边界（别读宽）

本机不是 NV ⟹ "NV 上现在能跑 MDI"仍是**推定**（★M-3T-b 拆掉了超规墙，但没人替 NV 跑过 A3）→ 债-81。

### 19.19 收尾批交付记录（2026-10-14：收 §19.6 / §19.8 两条挂号 + 债-113 计数器 + 文档纠状态）

这一批不动渲染，做的三件事：
把 ★M-0 / ★M-1a 交付时**挂号没做**的两条小修收掉、给 ★M-3T-c 的头条收益补上运行期计数器
（债-113）、把这份计划里**状态漂了的表格与推算数**纠回来。

**硬护栏（本批改了 `prepareMdiShadowRows` 与 profiler，必须实测"没动渲染"）**：三锚**逐字复现**
——出厂 `95edc8ccd7a4…`、门关（`enable=false` + `shadow_lod=-1`）`54e406655f16…`、
🔴 **A3 主判据 `mdi_shadow=true` 与出厂逐位相同**（同一 sha）。另加一条计数器口径级证据：
出厂臂 `draw.count` / `vkcmd.count` = **6601 / 19994**，与 §4.6（2026-10-13，债-105 还账）
逐字相同 ⟹ 交付路径的命令面也一字节没变（读数 → `docs/还债实测数据.md` §4.7）。

#### ① §19.6 挂号：不变量 1 的「多一行」半边 + `checkRowOrder` 的前置写成显式契约

- 新增坏形态 `badRowExtraHonestAnchors`（多一行 + 四个锚按 6 行**诚实重算**），表 13 → 14 条。
  needle 带 `holds 6 rows but the candidate list holds 5` 两个数 ⟹ 比较符被写反也红。
- `checkRowOrder` 的循环以「行数已等于候选数」为前提而它读 `std::span`（**无边界检查**）。
  该前置此前只在注释里，现改成**显式出声**（错误串前缀 `mdi rows: precondition`，刻意不含
  `invariant`）。正常路径不可达 ⟹ 它是**哨兵**，价值不在多一条判据，见 ㉗。
- 变异实测（harness `/tmp/opencode/mutate_mdi_rows_extra{,2,3,4}.py`，进场基线 sha `0af3451f…`）：
  **㉔ `!=`→`<` 红 1**（旧判据面全是**少行**形态，这条漂移旧判据**完全静默**）；
  **㉕ validate 顺序调换 红 1**（钉前置本身）；**㉖ 前置短路 红 0 = 存活**（不变量 1 完好时结构
  上不可达 ⟹ 等价变异，如实申报，同 ㉓ 同族）；
  **㉗ 顺序调换 + 前置摘掉 红 2，但归因是错的**——实测错误串原文：
  `invariant 4: row 5 holds {…} but candidate 5 maps to {gcX 0, objectIndex 137, clusterStart 529, …}`，
  而 `137 / 529` 是**越候选尾读来的堆上邻接字节**（用例数据里没有这两个数）。⟹ 没有前置时同一
  漂移会把「行数不匹配」报成「行序不对 + 数值不等」，堆布局一变还可能**恰好相等而彻底静默**。
  这就是前置的立项证据 = §15.3「归因错误比不归因更贵」的机器化。
- 🔴 **连带改了一个旧读数**：旧红线 ⑥（`!=`→`>`）由 **2 条变 1 条**。不是检测力变弱，是**失效
  形态变了**——少行的表现在前置处出声，「两两不同」那条用例不再因 `REQUIRE(has_value)` 红。红仍是红。

#### ② §19.8 挂号：`allocationSize = bytes` → `req.size`，实测**有 4 处不是 1 处**

挂号只写了 `test_device_primitives_impl.cppm` 一处。普查 `allocationSize` 发现
`test_izb_engine_contract` / `test_izb_mask` / `test_izb_create` 三处**同一形态**（都已经在手边
拿着 `req` 却拿请求的 `bytes` 去分配），一并改。**为什么一直没出声**：本机四个夹具 **42 次分配
实测 `req.size ≡ bytes`**（对齐 16、请求值全是 16 的倍数）——这句是**插桩量出来的**，不是推断
（量具 `/tmp/opencode/probe_req_size.py`，跑完还原并逐字节复核 sha）。**也不是假想风险**：
**同一后端**在别的夹具咬过，`test_meshlet_shadow_args.cppm` 文件头留着原文（56B staging 实测报
`requirements size 64 vs allocationSize 56`，且回读全 0 = 判据假空）。

⚠ 顺带纠一条我自己写下的错：注释初稿引了一个**未核实的 VUID**。本地 `/home/robin/program/Vulkan-Docs`
是**源克隆**，VU 编号是生成期才赋的，源里根本没有 ⟹ 改成引用驱动**实测报出**的那条编号。
**「看着像 spec 编号」不等于「核过 spec」**，这一格下次直接引驱动报错原文。

#### ③ 债-113：`mdi.tasks` 第五条计数器（★M-3T-c 的收益从此可回归）

**入账点是这批唯一的设计决策**，三个候选里选第三个：

| 候选 | 判定 |
|---|---|
| `drawOne` 的 MDI 分支（`mdi.count`/`mdi.rows` 的入账处） | ❌ 盲执行器只认 `drawCount`，让它算 task = 把行表语义漏进 `DrawCall`（ECS 规范 4，债-113 原文明令禁止） |
| 采集侧 `render_list:mdi`（锚就在这里，最省） | ❌ 那里算得出同一个锚，但 `mdi.count`/`mdi.rows` 是**录制期**入账 ⟹ 四条同源计数跨两个阶段。今天容量/轮转预检都在采集期，将来录制期一旦新增回落分支，tasks 就会在"没提交"的帧上报非零——而这条计数器的意义正是"收益会不会静默回退" |
| ✅ `Renderer::prepareMdiShadowRows`（行表真的写进 ring 之后） | 与 `mdi.count`/`mdi.rows` 同阶段口径；`drawOne` 一字节不动。上游读数 = `MdiShadowSegment::taskCount`，**唯一产地 = 构建器锚 `sumGroupCountX`**（录制侧只搬运不重算 = 只准有一个出处） |

**验收三条全绿**（读数 → `docs/还债实测数据.md` §4.7）：同配置连跑两次 `mdi.tasks` 逐次相同；
门关档恒 0；🔴 **且 = ★M-3T-c 当天一次性探针的闭式读数**（4517）⟹ 计数器**取代探针**成立，
以后改 task 形态不必再搓探针。附带一条口径自洽交叉验证：两档 `draw.count` 之差恰等于
`mdi.rows − mdi.tasks`（空行数）。

**护栏**：`tests/test_mdi_shadow_emit.cppm` 新增「债-113 taskCount」用例（期望 2/2/3 = 文件头闭式
可见集手算，🔴 不读被测锚；另钉 `taskCount < rowCount` 给"把行数当 task 数"唯一的信号格）。
变异实测 **M11 / M12 / M13 各红 1 条**；连带 **M10 由 1 → 3 条**。⚠ 这批在这里犯了一次
**先写后测**：初稿凭推断写「M10 → 2 条」，实测是 3 条（漏了「全不可见级联」也断言 `gcX == 0`），
已按实测改对并把这次犯错留在用例文件头。

**护栏拦了一次正确的改动**：R-EC4（ECS 全局可变状态棘轮）把新增的第 5 个 namespace 作用域累加器
报成 error，而基线**只准降**。处置不是要额度，是把五个累加器**收成一个 struct** ⟹ 棘轮 **4 → 1**，
且顺带堵掉一格真风险：旧 flush 逐字段手写清零，新增计数器漏清零 = 该计数器跨帧累加、读数一路涨
且零信号（观察者自己造出静默漂移）。重构后**五个 counter 逐字复测不变**（6674/6452/3/4590/4517
与门关 6601/19994/0/0/0），"只是搬运"是量出来的。

#### ④ 文档纠状态（本文件）

- **§1.2**：两个推算数被实测取代（阴影段 draw 77% → **68%**；阴影条目 ≈30 → **1**，★18-B1 塌成
  全场景 1 条 ⟹ 「阴影段 ≈120 条 MDI 调用」作废，实测 **3** 条）；条目基线 376/752 → 479/958
  （指针指向启动日志，不在本文件追平）；🔴 **模型段去重数 312 至今未重测**——启动日志只打总数，
  逐段去重数没有读数口，已就地标注"别拿它当 deferred 现行值"。
- **§4 / §9 / §10.5**：批次表状态补齐（★0 / ★0.5-b / ★1-a / ★1-b / A / D 全落地了表上还写
  "计划内"）。🔴 新增一条区分：**"已交付" ≠ "出厂开着"**，出厂档逐行标出（键与默认值真相源 =
  `runtime_config.cppm`）。顺带纠 ★0-b1 行里"默认 false"那句（当批记录，现行默认 = true，★18-C 翻的）。
- **§17.4**：补 ★1-a2-i 交付记录（当天入账但 §17 只有定案没有交付）。🔴 其中 R3 那条回退理由
  **当天就过期了**：R3 已窄化放行（→ `docs/constantbuffer_迁移计划.md` §8），但**"放行"不等于
  "已迁移"**——两张挂逐位锚点的表至今没改回类型化载体。
- **§6.3 `GUARD:` 待办判定为不可机械化，降级 `DOC:`**（静态判不了"两个不同变量谁该在前"，
  而"实参值两两不等"是激励侧约束）；同时写清现在真正在守它的三件事，别再找这条待办。
- **§11.5** `imgqa selftest` 断言条数护栏机械化 → **记债-117**（本批未做）。

### 19.20 命令数轴去向定案（2026-10-14 用户拍板：★M-3 / ★M-4 挂起）

**裁决**：阴影段 MDI（★M-0 / ★M-1 / ★M-3T-*）**已交付的部分保留**，门 `meshlet.mdi_shadow`
**保持默认关**；**★M-3（模型段 MDI）与 ★M-4（GPU 写行表）挂起**，不排期；Mesa issue 暂缓。

**🔴 定案依据不是"bistro 上 GPU 净亏 0.35ms"** —— 用户明确**不按 bistro 帧时间计价**，在意的是
「这套架构能否适配更复杂模型」。所以 §19.18 那条净亏读数**保留为事实**，但它**不再是冻结理由**
（它只是"本机 + 本场景 + 本档"的一个点）。后来者别拿它当定案依据重推一遍——真正的理由是下面两条。

**为什么"再切切 task"救不回来**（本批讨论定案，防"那就优化一下"的误读）：task 这一级要赚，必须

```
它砍掉的 mesh 组发射数 × 每组发射成本  >  它插进的那条依赖级 + 它自己的 ALU
```

左边两个乘数：**剔除率**与 **fan-out**。本宿主分别是 **0**（CPU 早剔完，mesh 组一个不少）与
**3.03**（13684 mesh 组 ÷ 4517 候选；远级联走粗级几何 ⟹ 每候选本就只有一几个簇）——两个乘数同时
在最坏端。实测也封死了这条路：**task 数砍 3.03× 只收回 0.15ms**（0.50 → 0.35），而两个变体之间
**唯一没变的量是 mesh 组总数 13684** ⟹ 常数残差最一致的解释是「mesh 组发射从前端走改成从
amplification 走，每次更贵」，与 task 数无关。⚠ ①依赖级延迟 / ②逐 mesh 组发射成本 **未用实验分开**
（只有两个数据点）；要分开加一臂「门开 + 强制全不可见」（mesh 组 ≈ 0、task 照发）即可。

**"GPU 自己决定少发射 mesh 组"这个真需求，载体不是 task shader**：task 把依赖级插进 mesh 流水线
**内部**，而 **compute 写 indirect args** 把依赖留在命令流层面（前端照旧直接发射 mesh 组）。
后者**已经建了一半**——`cull.comp.slang` 现在就在逐 candidate 覆盖 `meshCmd.x`，缺的只是顺序稳定的
压紧（§17.3 的 **★1-a2-ii**）。⟹ 将来要做这件事，起手是 ★1-a2-ii，不是 ★M-3。
task shader 在本项目的正确身份 = **绕 gfx1151 `DrawIndex` 毒化的临时代价**，不是要发展的能力。

**四条挂起项与各自的重启条件**

| 挂起项 | 重启条件（满足才重评） |
|---|---|
| ★M-3 模型段 MDI | 出现 **CPU-bound** 的场景（模型段命令收益只在 CPU 侧兑现；GPU 同比变紧则仍亏）。用户侧口径：「等想起来再说」 |
| ★M-4 GPU 写行表 | 同上，且**先有规模压力档读数**（`casterInShadowVolume` 那条 O(候选 × 级联) 逐帧扫描在 N 份场景下的增长斜率）——没有这个数，它的定价仍是空的 |
| Mesa issue（gfx1151 mesh 入口 `DrawIndex` 毒化） | 它是**唯一能拆掉那 0.35ms 依赖级**的动作（修好 ⟹ mesh 直读 `DrawIndex`、task 整级可摘）。要提需先写最小复现（spike 在 `/tmp/opencode/mdi/spike/`，落库位置待拍板 §7.8） |
| ★M-2 三机结案（债-81 剩余半） | NV 引擎级 A3 **从未跑过** ⟹ 它同时是"门该不该开"的唯一翻盘证据（NV 的 amplification 实现可能不同）。用户自己跑，跑前需要清单 |

**优先级换轨的触发点**：普查规模上限发现三堵写死的墙（对象矩阵表 4096 / bistro 用掉 39% ⟹
**最多装 2 个 bistro**；纹理段 4096 与对象表 4096 两处常量**注释自陈是按 bistro 反推的**）。
用户裁决 = **不处理**（"直接 throw 是最好查的"）——撞上是启动期报错，不是静默错，符合本仓
"宁抛不猜"的取向。登记为**降级项**（触发条件见总账 §3「规模上限清单」条），并把优先级让给
**把 compute shader 纳入声明式渲染图**（当前多处图外 dispatch，立项文档随该批建）。
