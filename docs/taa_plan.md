# TAA 时域抗锯齿（批 12.6）——调研与实施计划

状态：**执行中**（2026-10-02 立项，通宵批次）。作业参考 = 本地 Filament
（`/home/robin/program/filament`，本机核到非二手）。目标 = 给 deferred/forward 两条路加
时域抗锯齿，**默认关 = 画面逐位不变**（与 shadow/PCSS/EVSSM/IZB 同款红线）。

## 0. 两条硬约束（用户 2026-10-02 点名）

1. **全部走 descriptor heap**——TAA 的历史图 / 当帧色 / 深度 / 输出**一律 descriptor heap
   槽**（`RenderTargetCache::heapSlot` / `Resources` 运行时段），**零新增 push constant**
   （bindless 原则：push 冻结在纯索引级，见 skill `rgl-shader-contract`）。TAA 参数
   （feedback / jitter / 有效性位）进 **scene data**（binding 3 heap 缓冲区），不进 push。
2. **帧间分析做扎实**——TAA 是本项目第一个**跨帧读**的 feature，跨帧的三件事必须逐条论证：
   ① Vulkan 同队列跨 submission 的内存依赖（§2）；② 历史失效判据（首帧 / resize / 相机跳切）；
   ③ 运动重投影对**本场景实际运动源**（SceneSpin 全场景旋转）的正确性（§3）。

## 1. Filament 作业表（行号本机核实）

| 要素 | Filament 做法 | 出处 |
|---|---|---|
| jitter 序列 | Halton(2,3) **跳过前 409 项**（使均值≈0.5），默认 16 帧周期 `HALTON_23_X16` | `PostProcessManager.cpp:119-130, 2921-2940`、`Options.h:625` |
| jitter 注入 | **投影矩阵第 3 列做 NDC 平移**：`projection[2].xy -= jitter·(2/extent)`；像素单位 ±0.5；Vulkan/Metal y 取反 | `PostProcessManager.cpp:2942-2956`、`PostProcessManager.h:511-514` |
| 作用范围 | 只抖 color pass，structure/depth pass 不抖 | `Renderer.cpp:938-946, 1139` |
| 重投影 | **无 velocity buffer**：`q = prevProj · inverse(curProj) · clip(uv, depth)` | `taa.mat:333-341`、`PostProcessManager.cpp:3086-3091` |
| 邻域裁剪 | 3×3 采样的 box5（中心+4邻）与 box9 各 **50% 混合**；线段 **ACCURATE clip**（Lottes） | `taa.mat:381-396, 463-488, 219-238` |
| blend | 固定 lerp `mix(history, filtered, feedback×confidence)`，**feedback=0.12**（≈19 帧收敛），无 histogram | `taa.mat:492, 517`、`Options.h:564-573` |
| HDR 域 | mix 前 Reinhard `c/(1+max(c))`、mix 后 untonemap（ε=1/65504），**在 tonemap 之前的线性域**做 | `taa.mat:159-178, 510-522` |
| 输入重建 | Lanczos-2 3×3 unjitter + dering；history 用 Catmull-Rom 5-tap | `taa.mat:398-447, 252-315` |
| 色彩空间 | YCoCg 可选（默认关）；**deringing 必须在 RGB 做** | `taa.mat:202-217, 441-462` |
| pass 顺序 | structure(不抖) → SSAO → color(抖) → **TAA** → DoF → Bloom → tonemap | `Renderer.cpp:1142-1545` |
| history 失效 | 无自动检测，靠外部 `View::clearFrameHistory()`；首帧自混 | `View.h:1007-1015`、`PostProcessManager.cpp:3040-3044` |

**Filament 不抄的部分（结论，非遗漏）**：Filament 的 FrameGraph **每帧重建**，历史靠
`resources.detach()` 把本帧输出摘出成下帧输入（`PostProcessManager.cpp:3170-3182`）。
RGL 的图是**启动期编译一次**（Phase 6 契约），这套 detach 搬不过来 → 见 §4 的静态图方案。

## 2. RGL 侧 gap 与逐条设计

### gap ① 渲染图不支持跨帧历史（核心闸门）

代码里已点名预留：

```
rgl/render_graph_impl.cppm:320
// 读前置：本帧尚无写入的 attachment 不可读（跨帧历史复用——TAA 类——骨架版不支持，留后续）
```

**设计**：`RgAttachmentDesc` 加 `bool history = false`（仅 `offscreen` kind 合法）：

- **编译期**：`history=true` 的附件**豁免**读前置校验；其余附件规则逐字不变
  （既有 `"read before write"` 负例继续有效）。
- **历史图必须被写**：图里 `history` 附件若没有任何 pass 写它 → 编译报错
  （否则 = 永远读未初始化图，且这条只能编译期抓）。
- **Vulkan 跨 submission 正确性（帧间分析 ①）**：
  帧首 barrier 现在对追踪型 offscreen 发 `srcStage=TopOfPipe, srcAccess=eNone`
  （`render_graph_impl.cppm:351`）。这对"本帧先写再读"的既有 offscreen 够用，但对
  "本帧读上一帧写的历史图"**不够**——Vulkan spec（`vksync` §Pipeline Barriers）：
  barrier 依赖的是"submitted to the **same queue before it**"的命令，
  `srcStageMask=eAllCommands + srcAccessMask=eMemoryWrite` 的 first scope 覆盖**本队列此前
  所有 submission**，故跨帧 RAW 依赖成立；而 TopOfPipe/eNone 不建立任何依赖。
  另注 spec：同队列 batch **begin in submission order 但可 out-of-order 完成**
  （`vkQueueSubmit`），kFramesInFlight=2 下帧 N 与 N-1 确实可重叠 → 这条不是保险是必需。
  **实现**：`history` 附件的帧首 barrier 用 `srcStage=eAllCommands / srcAccess=eMemoryWrite`
  （保守一档；本帧首用即历史末写，队列同序无额外停顿）。
- **不做**（记债）：帧末用 semaphore 精确等历史写；上面那条保守 barrier 已覆盖正确性。

### gap ② 静态图下的历史缓冲（不 detach 的方案）

同 pass 读写同一张图 = feedback loop（编译期拒 + 物理上非法）。静态图不能逐帧换 role。
**三图 + 帧末 copy**（v1，全用既有 kind，零执行器新码）：

```
[light/opaque...] → scene_hdr ─┐
                               ├─ taa pass(drawLight, state="taa") → taa_out ─→ tonemap 采样
taa_hist(sampleRead, history) ─┘                          └─ copy pass(kind=blit) → taa_hist
```

- `taa pass`：读 `scene_hdr` + `depth` + `taa_hist`，写 `taa_out`（rgba16_sfloat，同 scene_hdr）
- `tonemap`：输入从 `scene_hdr` 换成 **`taa_out`**（装饰器改写，见 §5）
- 帧末 `kind=blit` 的 `taa_commit`：`transferRead taa_out → transferWrite taa_hist`
- 代价 = 一次全屏 blit（RGBA16F ≈ 8B/px，1080p ≈ 16MB，可忽略）；
  **省掉它的双图奇偶交替 = T4 优化项**（要执行器支持按帧奇偶跳过 pass，侵入大，先不碰）。

### gap ③ 运动重投影与本场景运动源（帧间分析 ③）

没有 velocity buffer，纯深度重投影 = 只能修**相机运动**。而 RGL 的运动源不止相机：
`SceneSpin`（`scene_context.cppm:504`）每帧把**全场景**绕轴旋转——纯深度重投影下所有几何
都会拖影（相机静止、物体在动）。

Filament 没这个便宜，**RGL 有**：旋转是"全场景同一矩阵"（`SceneSpin::matrix`），所以
上传一个 `prevSpin`，shader 里做**精确**的物体运动补偿：

```
worldPrev = prevSpin · inverse(curSpin) · worldCur   // 刚体补偿；spin 关 = 单位阵
prevClip  = prevViewProj · worldPrev
```

per-entity 动画（当前无）才需要 velocity buffer → 记债不做。

### gap ④ 词表 / 管线表 / scene data 登记链（机械，照抄批 11.4 SSAO 先例）

| 面 | 改动 |
|---|---|
| `pass_vocabulary.cppm:62` | `kAllPassStates` + `{"taa", fullscreenPass}`（**两档不可并表**的红线不破） |
| `state_kit.cppm` | 加 `taaState` 成员 + ctor 赋值（全屏终图，与 tonemap/ao 同形：depthTest/Write 关、cull None）+ `kStateSlots` 一行（漏 = static_assert 红） |
| `render_graph_builtin.cppm` | 加 `taaPass()` / `taaCommitPass()` 速构 + `tonemapPass()` 输入换 `taa_out` + `renderGraphSelfCheck` 结构断言更新 |
| `render_graph_decorate.cppm` | `decorateTaaPass(desc)`（按角色反查，三变体通用，仿 decorateShadowPass） |
| `render_graph_rtslot.cppm` | `ensureTaaSlot()`（名字/格式契约常量 `kTaaName/kTaaFormat`） |
| `shader_registry.cppm` | `findSpecBase` 加 `"taa"`（mesh 复用 `light.mesh.spv` 全屏大三角）+ `addTaaEntry()`（仿 `addAoEntry`） |
| `main_scene.cppm` | `assembleHdrAoChain` 里领 `taa_out`/`taa_hist` 槽 → `addTaaEntry`（**必须在烘 binding 之前领槽**，既有契约） |
| `rgl/CMakeLists.txt` | `rgl_shader(taa.frag TAA_FRAG_SPV scene)` |
| scene data 尾区 | `kSceneDataTaaOffset = 722080`（紧接 EVSSM const @722064+16）：`prevViewProj` mat4 + `prevSpin` mat4 + `jitter/frameIndex/valid` vec4；`scene.slang` 字面量手工同步 + 两侧 `static_assert` 钉死（既有纪律） |

## 3. jitter 注入点（决策）

`ViewProjection`（`scene_context.cppm:514`）由 `projectionSystem`（`ecs_impl.cppm:255`）写。
下游消费：CPU 算的 objectMvps（`@262800`）、`skyInvViewProj = inverse(proj·view)`（@64）、
cull 候选、picking、IZB、shadow 级联 near/far。

- **抖动加在 `ViewProjection::proj` 上**（下游自动传染，与 jittered 深度自洽），
  同时 `ViewProjection` 保留 **未抖的 `projUnjittered`** 供重投影与判据；
  `skyInvViewProj` 继续 = `inverse(jittered proj · view)`（AO/tonemap/IZB 采 jittered
  深度必须用配套逆矩阵重建，否则重建世界坐标偏 0.5px）。
- **不抖**：shadow 光空间矩阵（本就不同源）；`near/far` 自适应与级联切分（用未抖 proj 判深度域）。
- **已知偏差（记账不修）**：picking 的 1-texel 回读、cull reconcile 对账基线会带 ≤0.5px
  亚像素偏移——picking 是 id 归属（亚像素不改变命中的 primitive），cull 是包围球对账
  （抖动改的是投影不是可见性判据的位级结果？→ **T1 实测确认**，若对账报警则 cull 用未抖 proj）。

## 4. 配置与门（红线）

- **默认全关**：`configs/diagnostics.lua` 加 `taa = false`（38→39 门：`kGateRules` +
  `kGateEcho` + `tests/test_gate_lint.cppm` 覆盖率钉死；分级判据按 `docs/gate_lint_plan.md` §4）。
- 关 = **零 jitter + 图里没有 taa pass/commit pass + TAA 管线不建**（装饰器不调用）
  → 与基线**逐位不变**，走同会话双构建 BIT-IDENTICAL 验收。
- 运行参数进 `configs/scene.lua` 的 `taa` 段（仿 `ao.strength`，非门）：
  `feedback`（默认 0.12）/ `jitterScale`（默认 1.0）/ `motionCompensation`（默认 true）
  / `filterHistory`（Catmull-Rom 开关，默认 true）。
- imgui 参数页加 feedback 滑条（`param_pages` + `param_semantics.json`）。

## 5. 批次拆分与验收

| 批 | 内容 | 验收判据 |
|---|---|---|
| **T0** | 渲染图历史附件：`history` 标志 + 编译校验（豁免读前置 / 必须有写 / 只准 offscreen）+ 跨帧 barrier src 修正 + `decorateTaaPass` 骨架 | `just test`：`test_render_graph.cppm` 加**正例**（history 图编译过 + barrier src 断言）与**反向用例**（非 history 图读前置仍拒；history 无写方被拒；history 用在 depth kind 被拒），每条带变异红线声明 |
| **T1** | jitter 注入（Halton 表 + frameIndex）+ scene data 尾区 TAA 段 + `ViewProjection::projUnjittered` | 单测：Halton 序列闭式（前 16 项手算锚点 / 均值≈0.5 / 值域 ±0.5）；重投影矩阵闭式（spin 关 + 相机不动 → prevUV == uv 逐位；纯平移 → 手算 uv 偏移对拍）；scene data 偏移 static_assert；实机截图看静止边缘逐帧抖动幅度 <1px |
| **T2** | `taa.frag.slang`（box5/box9 混合 + Lottes 线段 clip + HDR 域 mix + Catmull-Rom history）+ 三图结构（taa/commit/tonemap 换输入）+ 词表/状态/管线表/main 装配全链 | `just reflect`；VVL 零报错；`imgqa switch`（开关影响域：域外逐位为零 + 域内确实变了）；静止 20 帧收敛（帧间差单调降到近零）；门关 BIT-IDENTICAL |
| **T3** | spin 补偿（prevSpin 上传 + shader 刚体重投影）+ 历史失效（首帧 / resize / 相机跳切阈值 → 一帧 feedback=1） | 开 spin 无拖影（逐帧截图差分峰值 vs 不补偿显著下降）；resize 后一帧内恢复；相机跳切后无残影 |
| **T4** | 细线条程序化几何 + 优化项 | 见 §6。**执行状态（2026-10-03）**：靶 + 6 条几何单测 + `imgqa temporal` 判据已落地；顺带查出并修掉 TAA 本体两个真 bug（混合权重反了 / `prevViewProj` 口径错，→ 调试历史 2026-10-03）。**未收口**：判据本身先要修采集（§7 第 8 条），优化项未动 |

## 6. 细线条程序化几何（TAA 的阳性锚点）

现有内建几何只有 cube（`cube.cppm`，8 顶点大平面）——**大平面 + 深背景对 TAA 几乎无信号**，
静止时截图几乎无差 → TAA 有效性没法用像素判据证明（"0 差异"的两种解释都堵不住）。
故补一个**程序化细线条靶**（内存合成 `GltfModel`，走 `uploadModelMeshes`+`registerScene`
现成路，零新 shader、零文件依赖）：

- **对角细杆阵列**：宽 0.002–0.02 的细长 quad 条，斜 45° 排布（亚像素宽度 → 无 AA 时闪烁最明显）
- **透视收缩栅栏**：等间距细杆沿 +Z 延伸（远处自然跌破 1px → 时域累积后应稳定）
- **同心细环 + 放射线**（覆盖各角度边缘，检验各向异性）
- 黑白高对比 baseColor（AA 效应对比最大化），走 unlit 或现有 pbr 皆可
- `scene.lua` 开关（`entities = { {type = "aaTarget"} }` 或独立 `aa_target = true`）

**阳性锚点（硬判据，2026-10-03 执行时按闭式重设计过——初稿写法不成立，理由见 §7 第 7 条）**：
激励 = **静止机位 + jitter**（平均视点恒定、唯一变化是采样相位），消融组 = `feedback=1`
（输出 = 本帧 ⇒ 只抖不滤）。三条硬判据（`imgqa temporal`，区域来自 objectID）：

1. **确定性地板**：TAA 关 + 静止 ⇒ 逐帧差**恰为 0**（非零则一切比值是噪声比噪声）；
2. **闪烁在场**：消融组逐帧差非零（缺它 = 「0 差异」的两种解释堵不住）；
3. **时域累积抑制**：消融/滤波两组比值 ≥ 3.0×（两组输入统计完全相同，唯一差别是一阶
   IIR 的权重，故可对拍闭式；白噪声近似预测 ≈11×，下限只取 3.0 = 给邻域裁剪留余量）。

⚠ 采集窗口必须是**整数个抖动周期**（Halton 周期 16）：周期内的帧间跳变幅度极不均匀
（|h(3)-h(4)|=0.875px 而 |h(8)-h(9)|=0.5px），实测 8 帧对的窗口随预热长度差 3 倍。
工具已把 pairs/warm 一律向上对齐到 16 并在输出里回显。

## 7. 已知边界与记债（收账时写进 AGENTS/调试历史）

1. **AO 时序性**：RGL 的 AO 施加在 tonemap shader 内（不是 Filament 的"AO 烘进 color 再 TAA"）
   → TAA 在 AO 之前，**AO 的螺旋采样噪声不被时域累积**。v1 接受；把 AO 施加挪进 TAA pass
   输入 = 后续批次（要拆 tonemap 契约，牵动 strength=0 逐位旁路基线）。
2. **无 velocity buffer**：per-entity 动画会拖影（当前场景无 per-entity 动画）。
3. **picking / cull 亚像素偏移**：见 §3，T1 实测定性。
4. **双图交替省 copy**：T4 优化项（要执行器按帧奇偶跳 pass）。
5. **Lanczos-2 输入重建**：v1 用双线性 + box clip 起步，Lanczos 重建（Filament 默认开）
   在静止画质上更锐，实测若"糊"再上（`Options.h:615` filterInput）。
6. **TAA 与 DoF/Bloom**：本项目无这两个 pass，不涉及；将来加 Bloom 须排在 TAA 之后
   （Filament 同序，否则 bloom 的 HDR 高频会被时域拖尾）。
7. **plan 初稿的 §6 硬判据不成立（已按闭式重设计，别再走回头路）**：「匀速微动机位下
   逐帧差降一个量级」对**完全正确**的 TAA 也几乎测不出来，两条独立理由：① 时域滤波压的是
   「同视点不同采样相位」的估计差，**压不掉**「视点真的动了」的正确画面变化；② L1 逐帧差
   对模糊近似**守恒**——边缘摊开会让更多像素各差一点，均值几乎不变（实测：变化像素
   3.10%→5.54% 而幅度 0.53→0.245，乘积不变）。已登记为 imgqa 反模式
   `moving-camera-l1-ratio`，微动两组（M0/M1）降级为只报不判的参考值。
8. **`/shot` 采不到连续帧 ⟹ T4 时域判据全部悬空（本批未收口的真因）**：`/shot` 是「帧首
   收单、本帧回读」，而引擎**自由跑帧**，两次 HTTP 之间经过多少帧取决于墙钟时序。实测同一
   配置把 warm 从 32 改到 64：S1/S2 从 1.58× 跳到 0.95×，而 S1（`feedback=1` ⇒ 输出 = 本帧、
   完全无历史）自己也从 0.0207 变 0.0152——它只可能依赖抖动相位，说明**采到的帧序列相位不
   确定**（旁证：静止机位的 id 轮廓像素数在两次运行间漂 0.6%）。
   **收口前置**：把采集换成「连续 N 帧」通道——复用 `dump_gbuf` 的「每帧重新 request」连续
   直读语义，或把 llm_debug 一次性模式扩成「连续截 M 张后退出」（完全不依赖 HTTP 时序，
   首选）。在此之前任何 TAA 时域数字都是时序噪声，**不许**拿来当验收依据。
9. **`feedback` 语义是「本帧权重」（现行契约）**：`lerp(clipped, cur, f)` ⇒ f=0.12 = 历史
   0.88（≈19 帧收敛，与 `configs/scene.lua` 文档同序）。推论：f=0 是「只要裁剪后的历史、
   完全丢本帧」的退化反馈环，**不是**「无滤波」；做消融请用 f=1。T4 曾因写反（`lerp(cur,
   clipped, f)`）导致累积窗塌到 ≈1.14 帧、时域滤波名存实亡。
