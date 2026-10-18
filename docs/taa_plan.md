# TAA 时域抗锯齿（批 12.6）——调研与实施计划

状态：**T0–T4 判据收口**（2026-10-02 立项，2026-10-03 判据全绿）。**后续收口与画质 →
`docs/archive/phase13_plan.md`**（本文 = 设计与踩坑账的权威载体，不再是"下一步做什么"）。作业参考 = 本地 Filament
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
  `kGateEcho` + `tests/test_gate_lint.cppm` 覆盖率钉死；分级判据按 `docs/archive/gate_lint_plan.md` §4）。
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
| **T4** | 细线条程序化几何 + 优化项 | 见 §6。**执行状态（2026-10-03）：判据全绿，T4 判据部分收口**。靶 + 7 条几何单测（含常量绊线）+ `imgqa temporal` 四条判据 + `imgqa aa-coverage` 逐杆覆盖率探针全部落地且**可复现**（采集走新增 `/shotburst`）。**顺带查出并修掉 TAA 本体四个真 bug**：① 混合权重反了 ② `prevViewProj` 口径错（→ git:调试历史 2026-10-03 前两条）③ 覆盖率探针抓到的黑晕 ④ **根因 = 跨帧历史每帧被 `VK_IMAGE_LAYOUT_UNDEFINED` 销毁**（③④ → §7 第 11/12 条；修前 TAA 从未累积过任何东西）。修后闭式对拍：中间调 9.13%（预测 ~11%）、抑制幅度 3.99×（预测 √16=4.00×）、逐杆误差 0.180 列。优化项未动 |

> ⚠ **T2/T3 验收的有效性裁决（2026-10-03，根因 bug 修好后补写；重验归 `phase13_plan.md` 批 13.0）**：
> 本节表写在「跨帧历史每帧被 `VK_IMAGE_LAYOUT_UNDEFINED` 销毁」（§7 第 12 条）**未知**的期间，
> 逐条重判如下——
> - **T3 三条全部空洞满足**（「开 spin 无拖影」/「resize 后一帧内恢复」/「相机跳切后无残影」）：
>   **历史是死的 ⟹ 按构造不可能有拖影/残影**，观测值为零不代表性质成立。修后**一条都没重跑**。
> - **T2「静止 20 帧收敛（帧间差单调降到近零）」在死历史下不可能通过**（jitter 开着、无累积，
>   帧间差会停在 jitter 幅度上）。故当时若记为通过，测的必不是这个量（jitter 关闭或别的口径）——
>   这条同样作废重测。
> - **T2「VVL 零报错」与「`imgqa switch` 开关影响域」不是空洞**（历史图仍被读写，barrier 链仍被压；
>   开关两侧画面也确实不同），但**修 barrier 后必须重跑**，且它们**不构成"真的在累积"的证据**——
>   真正证明这一点的是 §6 的 `aa-coverage` 逐杆覆盖率与 `temporal` 的 √N 幅度抑制。
> - **T0/T1 的单测不受影响**（纯函数、无设备、与布局追踪无关），仍然有效。
>
> 教训（→ git:调试历史 2026-10-03 四续条）：**跨帧算子的验收必须在"它真的生效"被独立证明之后才有意义**；
> 而"无拖影/无残影"这类**负向观测**尤其廉价——它同时被"实现正确"与"实现根本没跑"满足。

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

**阳性锚点（硬判据；2026-10-03 执行中改立两次——初稿与第一版替代方案都不成立，理由见
§7 第 7 与第 10 条）**：激励 = **静止机位 + jitter**（平均视点恒定、唯一变化是采样相位），
消融组 = `feedback=1`（输出 = 本帧 ⇒ 只抖不滤）。三条硬判据（`imgqa temporal`，区域来自
objectID，采集走 `/shotburst` 连续帧）：

1. **确定性地板**：TAA 关 + 静止 ⇒ 逐帧差**恰为 0**（非零则一切比值是噪声比噪声）；
2. **闪烁在场**：消融组逐帧差非零（缺它 = 「0 差异」的两种解释堵不住）；
3. **空间抗锯齿在场**（互为阴阳的一对）：TAA 关中间调**恰为 0**（无 MSAA = 二值覆盖，
   实测全图只有 3 个亮度级）+ TAA 开中间调 ≥ 1.0%（闭式预期 ~11%，下限取 1/10）。
4. **覆盖率保真**（`imgqa aa-coverage`，**质量**判据；§7 第 11 条）：投影宽度 w 的黑杆画在
   白板上，正确覆盖混合沿杆方向的**黑度积分** = w 列。单帧、无基线图、与采集时序无关。
   覆盖率由 `1 − aces⁻¹(luma)` 反解（正向复用 `analytic.cpp` 闭式 ACES，`imgqa probe`
   已拿真机 ±2 LSB 验过）。判据 = **判据域（投影宽 ≥2 列）**内 TAA 开最大逐杆误差 ≤ 0.35 列
   **且** TAA 关显著更差（≥3×）。**实测 0.180 列 / 0.866 列 = PASS**。
   **度量自身的有效性**（阳性锚点）：TAA 关实测逐根 = `floor(期望)`（0,1,1,2,2,3,3,4,4,5,6,6
   vs 0.65,1.19,1.72,…）= 点采样二值覆盖的闭式行为；白板亮度对拍闭式 `ACES(1)`。
   ⚠ **判据域不是调阈值调出去的**：邻域裁剪 TAA 对 ≲2 列的细杆**必然欠累积**（错相帧本帧没
   采到杆 ⟹ box5 全白 ⟹ 已累积的黑被判盒外拒掉），单帧值由抖动相位决定，不是稳定质量量。
   窄杆仍打印（实测域外误差 0.373 列），欠累积记债见 §7 第 12 条末。
5. **抑制比 = √N**（`imgqa temporal` 的 `temporal-suppression-sqrtN`，**2026-10-03 换观测
   空间后重立**）：16 相位抖动 = 同一视点 16 个独立采样相位求平均 ⟹ 逐样本估计噪声按
   **√16 = 4.00** 下降（闭式）。观测空间 = **变化像素上的平均幅度**，不是 L1 总量。
   **实测 3.99×**（下限 3.0×）。⚠ 这条与被打反模式的 `temporal-taa-suppression` 唯一差别
   就是观测空间——闭式预测一直是对的，错的是拿它去对总量（L1 对模糊守恒 ⟹ 只给 1.38×）。
   详见 §7 第 12 条与反模式 `screen-space-l1-frame-diff`。

⚠ **抑制比 S1/S2 只报不判**：屏幕空间 L1 逐帧差量不出它——「单像素幅度降」被「变化像素数
涨」抵消（实测 2.04× vs 1.73×，净 1.17×）。工具仍打印这两个分量，是为了让**守恒指纹**可见。
详见反模式 `screen-space-l1-frame-diff` 与 §7 第 10 条。

⚠ 采集窗口必须是**整数个抖动周期**（Halton 周期 16）：周期内的帧间跳变幅度极不均匀
（|h(3)-h(4)|=0.875px 而 |h(8)-h(9)|=0.5px），实测 8 帧对的窗口随预热长度差 3 倍。
工具已把 pairs/warm 一律向上对齐到 16 并在输出里回显。

## 7. 已知边界与记债（收账时写进 AGENTS / 当前任务文档）

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
8. **`/shot` 采不到连续帧（已修：新增 `/shotburst`）**：`/shot` 是「帧首收单、本帧回读」，
   而引擎**自由跑帧**，两次 HTTP 之间经过多少帧取决于墙钟时序 ⟹ 相邻两张根本不是相邻两帧。
   实测旁证：S1（`feedback=1` ⇒ 输出 = 本帧、完全无历史）在 warm 32→64 时自己从 0.0207 变
   0.0152，而它只可能依赖抖动相位；静止机位的 id 轮廓像素数也在两次运行间漂 0.6%。
   **修法 = 新增 RPC `POST /shotburst {dir, n, prefix?, gbuf?}`**：第一张在 drain 里登记，
   之后每张由**帧首结算钩子**自己登记下一张——该钩子先于 `drawFrame`，于是采集落在连续的
   第 N、N+1、N+2… 帧上，连续性由帧循环保证、与 HTTP 时序无关；应答在最后一张写盘后才兑现
   （运输层超时另开一档 120s）。修后实测可复现：warm 32 vs 64 → S1 差 0.03%、S2 差 0.4%。
   ⚠ 残留二阶项：id 面仍是单张 `/shot`（区域不需要连续性），它落在哪个抖动相位会带 ±0.5px
   的 mask 漂移（对均值的贡献 ~0.4%），3px 外扩已吸收。
   ⚠ 没有「微动机位」组：逐帧改机位唯一的配置驱动来源 `SceneSpin` 是 `angle += speed*dt`
   （按帧时长累积）⇒ 每帧角度本身不确定，测不出可复现的数；要做微动得先加「按帧索引驱动」
   的相机运动，而这条判据已登记反模式（第 7 条），不值得。
9. **`feedback` 语义是「本帧权重」（现行契约）**：`lerp(clipped, cur, f)` ⇒ f=0.12 = 历史
   0.88（≈19 帧收敛，与 `configs/scene.lua` 文档同序）。推论：f=0 是「只要裁剪后的历史、
   完全丢本帧」的退化反馈环，**不是**「无滤波」；做消融请用 f=1。T4 曾因写反（`lerp(cur,
   clipped, f)`）导致累积窗塌到 ≈1.14 帧、时域滤波名存实亡。
10. **屏幕空间逐帧差这条度量算子本身不能用（两次踩同一个病根）**：第 7 条推翻「微动机位」后
    换成的「静止机位 + jitter」激励，**仍**不能量抑制比。病根在算子不在激励：L1 逐帧差对模糊
    近似**守恒**——jitter 让采样网格每帧动 ±0.5px，「相邻帧的同一屏幕像素」量的是**不同世界点**
    上的估计差 = 梯度 × 网格位移；边缘摊开后梯度变小、作用面积变大，乘积不变。
    实测指纹（`/shotburst` 连续帧、可复现）：单像素幅度 0.380→0.187（降 2.04×）被变化像素
    7.16%→12.41%（涨 1.73×）抵消 ⟹ 净比值 1.17×，与微动版指纹同构。换 L2 只给 √k（同批数据
    算出 1.55×），也够不到闭式。**教训**：闭式 3.95×/11× 是**对齐空间**的预测，两次都拿它对了
    屏幕空间的观测——期望值的**适用域**要和观测的**坐标系**核对，这跟「期望值要有出处」是两回事。
    **现行判据** = §6 第 3 条（空间抗锯齿在场，阴阳一对）。
    **下一步的正确度量** = 单帧**逐杆覆盖率对拍闭式**：投影宽度 w 的杆，正确 AA 的输出亮度
    ≈ w，无 AA 时按相位取 0 或 1。它不需要基线图、不涉及时域差分、可复现，且正是本项目
    `imgqa probe` 那套闭式解探针的路子。前置 = 把靶的杆表（宽度/位置）抽成 `aa_target.cppm`
    与 imgqa **共用的 constexpr 表**（单一真相源；imgqa 不链 rgl 库，不能 import 模块）。
    → 已落地（第 11 条）；「共用 constexpr 表」实际做成了**第二份声明 + 单测绊线**，理由见该条。
11. **逐杆覆盖率探针落地，当场抓到 TAA 本体第三个真 bug：黑晕（未修）**：
    探针 = `imgqa aa-coverage`（`tools/imgqa/aa_coverage.{h,cpp}`，判据 `aa-coverage-fidelity`）。
    **度量自身有效**的两条独立证据：① TAA 关实测逐根 = `floor(期望)`（0,1,1,2,2,3,3,4,4,5,6,6
    vs 闭式 0.65,1.19,1.72,…）= 无 MSAA 点采样的二值覆盖行为；② 白板亮度对拍闭式 `ACES(1)`
    （不是"落在某区间"），且全黑列恰好贡献 1.07 列 ⟹ `1 − aces⁻¹` 反解在真机上闭环。
    **测到的缺陷**：TAA 开时**每根杆两侧各多一列 ~0.87 覆盖率的黑**，凭空多黑 1.409 列/杆；
    最大逐杆误差 2.023 列（TAA 关 0.866）⟹ 判据 FAIL。**消融定位**：`jitter_scale=0` 时黑晕
    几乎不降（1.604 vs 1.853 列）——而静止机位 + 零抖动下 TAA **本应恒等于不开**（历史==本帧
    ⟹ 裁剪恒等 ⟹ 混合是凸组合），实测不等 ⟹ 缺陷与抖动位移量无关，**推翻**了"重采样符号/
    偏移把黑历史搬出杆外"这一最初猜测。
    **机制（本条当时给的机制是错的，见第 12 条的更正）**：当时由「黑晕值 = `f·W` 精确成立」
    （f=0.12→0.12W，f=0.5→0.5W，比值 0.562 vs 预测 0.568）推出「裁剪后历史恒为纯黑」，
    并据此猜是重采样偏差被不动点放大 7.3 倍。**前半对**（裁剪后历史确实恒黑），
    **后半错**（与重采样无关）——真正原因是历史图每帧被 `VK_IMAGE_LAYOUT_UNDEFINED` 销毁。
    教训：`f·W` 这个指纹只约束「裁剪后历史 = 0」，**不区分**「历史本来就是黑」与
    「历史被搬错位置」，我当时把两种都能解释的观测当成了后者的证据。
    **下一步（未做）**：查静止 + `jitter_scale=0` 时 shader 侧 `prevUv` 为何 ≠ `uv`。T1 那条
    「spin 关 + 相机不动 → prevUV == uv 逐位」单测是 **CPU 侧**算的，shader 实际走
    `worldFromDepth(uv, depth)` → `prevViewProj` 的往返——半像素 / y 翻转 / 深度重建精度任一
    有偏，都会被那 7.3 倍放大器变成可见黑晕。
    ⚠ 杆表**没有**做成共用头：`.cppm` 的 GMF include 名字不导出（`tests/` 要 `import aa_target`
    拿这些常量），为一堆常量上 CMake 代码生成不值。实际形态 = `tools/imgqa/aa_coverage.h`
    重抄一份 + `tests/test_aa_target.cppm` 常量绊线（改靶不改探针 → 单测红并点名探针；
    变异实测：改 `kLadderDx` → 恰 1 条失败）。
12. **TAA 第四个 bug（根因）：跨帧历史每帧被 `VK_IMAGE_LAYOUT_UNDEFINED` 销毁——TAA 从落地
    那天起就没有累积过任何东西（已修）**：
    **症状**：`aa-coverage` 测到每根杆两侧各一列 ~0.87 覆盖率的黑晕；`temporal` 抑制比只有
    1.17×。两条独立排除实验：**① 恒等式检查**（静止 + `jitter_scale=0` + `feedback=1` 时
    TAA 开与关**逐位相同**）⟹ 前向路径干净；**② 预热无关性**（`jitter_scale=0` 下 warm=2
    与 warm=128 **逐位相同**；若历史在累积，0.88²=0.774 vs 0.88¹²⁸≈0 不可能相同）⟹ 历史是死的。
    **定位**：临时把 shader 改成直接输出 `historyTex` → 历史图 99.92% 纯黑；再改成「每帧播种
    纯红 + 绿通道报是否采到红」→ 绿恒 0 ⟹ 提交未落地。而 blit 日志证明 **commit pass 每帧都
    在录、src/dst 句柄与 RT 图像完全一致** ⟹ 问题在布局。布局链日志一锤定音：
    `taa_hist : Undefined -> ShaderReadOnlyOptimal` **每帧**出现。
    **根因**：`ensureOffscreenAttachments` 里 `rtGenerations` 是**每帧归零的局部量**，而
    `RenderTargetCache::ensure` 首次创建即 `generation = 1`（早退只挡「尺寸/格式没变」）
    ⟹ `generation != 0` **恒真** ⟹ 每帧把每个 offscreen 附件的追踪布局抹成 `eUndefined`
    = 声明「内容可丢弃」。本帧先写后读的 RT 无感（那正是帧首丢弃的语义），**唯一跨帧消费者
    `taa_hist` 在被读到之前就被销毁**。修法 = `rtGenerations` 提为 Renderer 成员（跨帧快照）。
    **修后被闭式独立确认**：中间调 2.36% → **9.13%**（闭式 ~11%）；单像素幅度降幅 2.04× →
    **3.99×**（闭式 √16 = 4.00×）；覆盖率凭空多黑 +1.409 → **−0.055 列**；判据域内逐杆误差
    **0.180 列**。红线复测：TAA 关同配置截图修复前后**逐位相同**。
    **可复用判断（三条）**：① 做时域/跨帧算子，先把它自己的**恒等式**做成判据
    （「关掉抖动 + 静止场景 ⟹ 必须等价于不开」），比任何比值判据都便宜且不可辩驳。
    ② 「**预热多少帧结果都不变**」是检测「跨帧状态根本没生效」的通用零成本探针。
    ③ 布局追踪表被抹成 `UNDEFINED` 对「本帧先写后读」的资源完全无害，**只有跨帧消费者会
    中招**——这类 bug 不会被任何现有判据发现，加新资源时要意识到这条不对称。
    **残留记债（不是 bug，是算法类的固有边界）**：投影宽 ≲2 列的细杆**欠累积**（实测域外
    误差 0.373 列，0.652 列的杆单帧只积到 0.076）——错相帧本帧没采到杆 ⟹ 邻域盒全白 ⟹
    已累积的黑被判盒外拒掉。判据因此只在 ≥2 列域内判绝对精度，窄杆只打印不判。
    ⚠ **box9（全 3×3）已试过，无效并回退**：12 根杆的数字与 box5 几乎完全一致（域内最大误差
    0.180 → 0.189）。原因是靶是**竖直**杆，3×3 多出的对角样本落在同行/同列已有取值范围内
    ⟹ min/max 不变 ⟹ **该实验对竖直结构没有区分力**（教训：设计消融要确认被动的量真的会动）。
    由于 box9 有真实代价（鬼影抑制变弱）而无实测收益，不进主干。剩余缓解方向 = variance 或
    YCoCg 扩盒 / Lanczos `filterInput`（与第 5 条同源），**未验证**。

13. **TAA 第五个 bug：Catmull-Rom 历史核写成奇函数（已修）+ 「游动」定案为机制而非缺陷**：
    **症状**：真实 bistro 上「边缘疯狂闪烁 + 整幅游动 1~2 像素」，且 phase13 §1.7 量到 98.3%
    像素每帧拒绝历史。**根因**：`sampleHistoryCatmullRom` 三次项用带符号 `a³`，而核关于到纹素
    中心的距离是**偶函数** ⟹ `t=0` 权重从 `(0,1,0,0)`（Σ=1）变成 `(0,1,−3,8)`（**Σ=6**）⟹ 2D
    直流增益 **36×** ⟹ 历史被 `saturate` 顶界后被 AABB 判越界 ⟹ **恒拒**（= §1.7 那个「盒扩张
    1.5× 纹丝不动」，不是域口径问题）。抓它的仍是第 12 条判断①那条**恒等式**（`jitter_scale=0`
    + 静止 ⟹ TAA 开 ≡ 关）：修复前离零抖动基图 1.337/255，修复后 0.016/255。
    **可复用判断**：① 「**采样点对齐纹素中心时重采样必须等于直接取值**」是任何重采样算子的第一条
    判据，比任何端到端指标都便宜；② **改完核必须回验最终着色器**——批 13.1a 当时的 Python 交叉
    检查只否掉了「双线性折叠」那个替代方案，从未回验 Slang 核本身，于是把 bug 当「精确 CR」交付了
    一批；③ 时域算子的**游动要用像素量**（相邻帧整幅位移），不要用观测域逐帧差——后者说不清「几像素」。
    **游动定案**（`imgqa taa-swim` 绝对口径 + 合成锚点标定）：`fb=.12 js=1` 相邻帧位移 **0.717px
    均值 / 1.051px 最大**（闭式期望 0.667px），而 `fb=1` 完全不累积时是 0.674px ⟹ **累积压不掉它**；
    机制 = 重投影把历史对齐在基空间、输出采样位置仍在本帧抖动位置（`out_t(uv) = F(uv − j_t)`）。
    ⟹ **修 CR 核治闪烁，不治游动**；游动消不消是产品决策（详见 `phase13_plan.md` §1.9）。
    ~~顺带量出坐标口径：图像位移 = `scale·(−jx, +jy)`（…）——此前文档与探针都按「+jitterPx」记，
    是错的。~~ **⚠ 2026-10-04 更正：这句方向写反了。** 真图像内容位移 = `scale·(+jx, −jy)`
    （= 当初被说成「错的」那个记法）；探针 `jitterDeltaPx` 返回的是它的**相反数**（`freeShift`
    的 `frame(p) = ref(p + d)` 口径），数值读数全部有效。全案 → 下面第 15 条。

14. **TAA 第六个 bug：进 Reinhard 域前的 `saturate` 把线性 HDR 亮部顶平（批 13.1e，已修）**：
    **症状**：`temporal-identity` 在合成 profile 绿（0.016/255），换到**交付配置 + 报障机位**红 75 倍
    （mad 1.688、max 37、10.37% 像素 dev≥2）。**根因不在邻域盒**（13 个盒修法变体在压暗区逐位
    不改；2×2 因子实验把因果钉死），在取值域闸门：本 pass 在 tonemap **之前**，通道 ≫1 是常态，
    `saturate` 把亮部顶平到 1 ⟹ 时域不动点从 `X = cur` 变成 `X = (1−α)·1 + α·cur` ⟹ 白屋顶
    8.83% 像素系统性压暗 −18.5 LSB（fb 依赖与公式逐条吻合）。修 = `clampHdr` 只夹下界
    （Reinhard 定义域本就 `[0,∞)` 管上界；下界仍要夹：CR 负瓣 + `clipToAabb` 要求非负）。
    **盒退化是下游症状**：顶平区 5 抽头全 = 1 ⟹ 盒必退化；修取值域后塌回 centroid 9.799%→0.381%。
    **可复用判断**：① **判据测不到的东西不能拿它当修复的验收**（js=0 稳态与裁剪算子无关——把裁剪
    整个关掉 mad 仍是 1.688）；② 差值**单号**（mean = −mean|d|）= 偏置不是噪声，空间 localize 后
    顺着「不动点方程的 fb 依赖」一条链就摸到根因；③ 合成档绿 ≠ 绿——**判据必须跑在交付配置 +
    报障机位上**（本轮 mad 差 75 倍）。

15. **基空间输出落地（批 13.2，门 `taa.base_space` 默认开）+ 符号口径更正 + 两条量具自检重标**：
    **机制**：`O_t(x) = F(x − c_t)` ⟹ 消费端按 `+c_t` 采样落回基网格。折进 **tonemap 的采样坐标**
    （pass 序上它本来就是 `taa_out` 的唯一 1:1 最终消费者 ⟹ 零额外 pass/纹理）；CPU 单点写
    `taaResolveOffset = (+jx, −jy, 门, 0)`，门关写全 0 且 shader 走**原样 `return uv`** 分支。
    **重采样核 = 双线性**：CR 更锐（梯度 +6.2%）但在有闭式的 `aa-coverage` 上 0.119→0.181 列退化
    + 负瓣振铃（黑晕翻负号）⟹ 否决。**梯度能量这条无闭式，单看它会翻案**（§7 第 10 条同课）。
    **验收**：门开游动 0.0598px / 门关 0.643 / 闭式 0.667（降 10.8×），阳性锚点 = 同次运行门关态；
    红线同会话双构建逐位（HEAD tonemap spv `4c7f991f…` vs 当前 `05cf91fd…`，截图同 sha `c186ce74…`）。
    **符号口径更正**：真内容位移 `c_t = scale·(+jx, −jy)`（glm 列主序 `proj[2][0]` = clip.x 的
    v.z 系数，`clip.w = −v.z` 翻号 ⟹ Δndc = +2j/W；y 再被**负高视口**翻一次）；`jitterDeltaPx`
    返回 −c_t，与 `freeShift` 的 d（`frame(p) = ref(p + d)`，warpAffine 方向）同口径匹配。
    最终裁决 = 门开游动**归零**（符号若反 ⟹ 翻倍 ~1.33px，20 倍差）。**可复用判断**：符号口径
    必须绑定算子 warp 方向命名，「图像位移」这种自然语言在同一工具里能容纳两种相反约定而数值
    不报错（模长与符号无关，历次读数全对，只有注释在撒谎）。
    **量具自检重标（两条红，都验证过与引擎改动无关：锚点只依赖 T0、③ 只依赖 fb=1 的 T4）**：
    ① `temporal-fitter-anchor` 红在 **phaseCorrelate 半整数奇点**（14 组已知位移实测：半整数处
    误差恒 −0.320±0.002px，四分之一整数 ≤0.002px），旧锚点 x=3.5 正踩奇点 ⟹ **换探针位移
    (3.25,−2.125) 出奇点，阈值 0.25px/10% 一字未动**，加零位移反向用例（堵拟合器卡常数）；
    ② `temporal-jitter-transfer` ③ 红在**场景档**（覆盖翻转地板：合成 profile 18.9% vs 交付
    场景 36.7%）⟹ 阈值 30%→50%，失败模式 92~100% 两侧余量 ≥13pp，区分力由
    `temporal-swim-discriminative` 反向守。**教训：标定锚点不要放在估计器的已知奇点上；阈值
    随场景内容变的量（插值失配/覆盖翻转地板）必须写明场景档。**
