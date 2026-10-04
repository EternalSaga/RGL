# TAA 历史坐标空间改造（基空间累积）

状态：**现行**（2026-10-04 立，**未开工**）。本文 = 本改造的权威载体与执行清单。
关联：`phase13_plan.md`（TAA 收口 phase，本改造是其 13.1d/13.2 的**续命**）、
`taa_plan.md`（T0–T4 判据收口史）、`还债总账.md` 债-33 / 债-38。

---

## 0. 一句话

**把 TAA 历史附件从「抖动空间」搬到「基空间」**：抖动在**进入累积之前**被消化，
而不是留在累积里每帧穿过历史。这修掉静止机位下「固定频率 + 固定位置」的边缘 shimmer。

用户报障原话：「TAA 我当年玩游戏也用过，根本不记得会有抖动和**固定频率固定位置**的噪点。
我印象中的 TAA 是**静态略模糊，动态下没锯齿**。」——**这个直觉是对的**，
「固定频率 + 固定位置」是实现缺陷的指纹，不是 TAA 的固有代价。

---

## 1. 根因（两侧架构对照，全部已核实到行号）

| | **Filament** | **本项目** |
|---|---|---|
| 历史附件所在空间 | **基空间**（无抖动网格） | **抖动空间**（每帧历史 = 那帧抖动渲染的输出） |
| 抖动在哪步被消化 | **进入累积之前**：输入侧 3×3 Lanczos 重建（注释原话 `also be thought of as "unjittering" the input sample`，`taa.mat:398-405`） | **留在累积里**：本帧色 `colorTex.Sample(linearSampler, input.uv)` 直接进混合（`taa.frag.slang:195`，无任何重建） |
| 重投影用的矩阵 | 两侧**都无抖**（`current.projection` 在 `PostProcessManager.cpp:2918` 存，jitter 到 `:2956` 才写进 CameraInfo） | 两侧**都含抖**（`ecs_impl.cppm:388` 存 `viewProj.proj * viewProj.view`，此刻 `proj` 已含本帧 jitter） |
| **静止相机下历史采样偏移** | **恒等映射 → 点采样，零重采样**（CR 权重退化 `w1=1` 其余 0） | **`δ_t = c_t − c_{t−1}`，闭式均值 0.667px，每帧必重采样** |
| 输出所在空间 | 基空间，下游 bloom/tonemap 看不到抖动 | 抖动空间，靠 `base_space` 在 tonemap 再重采样一次搬回来 |
| 历史重采样 deringing | **有**（clamp 到 5-tap 盒，`taa.mat:308-313`） | **无**（CR 直接 `return acc`，`taa.frag.slang:187`） |
| 抖动序列 | Halton(2,3) 跳 409 项，周期 16（`Options.h:625`） | Halton(2,3) 跳 1 项，周期 16（`taa_jitter.cppm:49-52`）——**周期相同，不是差异项** |

**机制**：静止相机下本项目每帧把历史按 `δ_t` 做一次**亚像素重采样**。Halton 周期 16 ⟹ `δ_t`
只取 16 个离散值，且 `δ` 只依赖相位、不依赖内容 ⟹ 输出对相位是 **16 周期函数**，
而重采样误差只在**高频处**响应 ⟹ 钉死在几何边缘。这就是「固定频率 + 固定位置」。

### 1.1 机制的精确表述（2026-10-04 实施期收紧，别再说错）

⚠ **旧方案历史的「内容对齐」是正确的**——批 12.6 T4 存抖动后 `prevViewProj` 的裁决没错：
历史图内容长在抖动空间，用抖动后矩阵投影，取到的**确实是同一个世界点**。
错的是**采样位置**：静止机位下世界点 `w` 的基位置 `β_t(w) = uv − c_t`（`uv` = 输出像素），
于是 `prevUv = β_{t−1}(w) + c_{t−1} = uv − δ_t` —— **一个随相位变的分数位置**。
⟹ 每帧对**累积图**做一次分数位移重采样：内容对、位置错。两个后果：
① 老贡献被反复低通 = 复利糊化（梯度比 0.671）；② 重采样核随相位变 = shimmer。

改造后的目标态：`prevUvBase = uv`（**整数**）⟹ CR 权重退化成 `(0,1,0,0)` = 点采样，
两个后果一起消失。这才是「历史住基空间」的算术含义。

### 1.2 残余地板（验收目标据此定，别指望 0）

输入侧 un-jitter 本身 `cur = I_t(uv + c_t)` **也是相位相关的亚像素重采样**——它不作用于
累积图（每帧只重采样当帧一次、不复利），但输出是「一串相位各异的重采样」的加权和，
所以**不会归零**。Filament 同构（它的输入侧 Lanczos 同样随相位变）。
⟹ 验收判据是**大幅下降**，不是归零；`jitter_scale=0` 才是真 0（`c_t ≡ 0` ⟹ 两处重采样同时消失）。

### 1.3 这一条解释了全部五条独立实测（这是它作为根因的主要证据）

| 症状 | 机制 | 实测 |
|---|---|---|
| 固定频率 16 | `δ_t` 是 16 周期序列 | lag-16 同相位差 0.000% |
| 固定位置 | `δ` 与内容无关，只在边缘响应 | 9.22% 像素 >2LSB，集中边缘 |
| 比理想 16 相位超采样还糊 1.5× | 老贡献被**反复**亚像素重采样（复利低通） | 梯度比 0.671（`phase13_plan` §1.5） |
| `jitter_scale=0` 完全静止 | `δ=0` → 历史退化为点采样 | 0.000% |
| `base_space` 有效 10.8× 但没治好 | 它只搬**显示**变换，累积内部相位依赖一点没动 | 游动 0.643→0.060px，交付仍 9.22% |
| 换 Catmull-Rom 治好糊、没治抖 | CR 只降低单次重采样的低通量；**负瓣带来随相位翻号的振铃** | 糊度对历史年龄依赖消失，shimmer 仍在 |

**内部一致性佐证**：批 13.2 自己因「CR 负瓣振铃 + 列退化」**实测否决过** CR 做 tonemap resolve
核（`phase13_plan` §1.10）——同一条反对理由，在历史重采样那条路上成立得更强（那边还叠加）。

---

## 2. 与既有结论的关系（三条，别重复推翻）

1. **`phase13_plan` §1.9（批 13.1d）「输出在抖动空间」的定案是对的，但只修了一半。**
   §1.10 的 `base_space` 修的是**显示**那一半；**累积**那一半（历史每帧被 `δ_t` 重采样）从未动过。
   本文不是撤回 13.1d，是把它做完。
2. **⚠ 批 12.6 T4 的裁决**——**立文档时我判断"要反转"，实施时核实为「不反转，但多一条成对约束」**。
   `scene_data.cppm` 记载：曾长期存**未抖** `prevViewProj`，被改判为错
   （「把『像素中心对齐』误读成『在未抖网格里』」，后果 = 采样点系统性偏 `-jitter_prev`，
   静止机位 S1/S2 = 0.48×）。**该裁决在本改造后仍然成立**：`prevViewProj` 的作用是把世界点
   投到**上一帧的抖动网格**上，这一点没变；变的是下游——历史搬到基空间后，要在 shader 里
   **减一帧内容位移 `c_{t−1}`** 才落到基网格。
   ⟹ 两处**成对、二选一**：「本字段抖动后 + shader 减 c_{t−1}」（现行）或
   「本字段未抖 + shader 不减」（Filament 形态）。**都做 = 多减一次，都不做 = 少减一次**，
   两种都让 0.667px 游动原样回来。注释已写成这个条件式（`scene_data.cppm` / `scene.slang` /
   `ecs_impl.cppm` 三处），单测反向 ② 专门钉「都做」这个失败形态。
3. **`ViewProjection::projUnjittered`（`ecs_impl.cppm:352`）全仓零消费者**——它当年就是为这件事加的，
   从来没接上。本改造可能用到，也可能不需要（见 §3 载荷裁决）。

---

## 3. 目标架构与载荷裁决

### 3.1 三步（**必须同一个 commit**，见 §3.5）

**Step 1 — 输入侧 un-jitter（`rgl/shaders/taa.frag.slang`）**
- 本帧色不再 `colorTex.Sample(linearSampler, input.uv)`（`:195`），改为按本帧内容位移 `c_t`
  做亚像素重建，把抖动网格的采样搬到基网格像素中心。
- **重建核 = 双线性 4-tap 起步**。理由：输入侧重建**每帧只对当帧做一次、不叠加** ⟹ 低通不复利，
  而 CR/Lanczos 的**负瓣振铃会进历史并在历史里累积**。⚠ 选型待实测（§5 判据）。
- 天空像素（`depth ≥ 1.0`）同样重建——天空也随 jitter 移动。
- `c_t` 从哪来：**`@722208.zw` 已经传了 `jitterPx` 且 shader 从未使用**（`taa.frag.slang:194` 只读 x/y）。
  ⟹ 把该槽语义从 `jitterPx` 改成**内容位移 `c_t`**（CPU 单点换算，shader 不再自己判符号），
  **零载荷扩列**。

**Step 2 — 历史基空间 + 重投影修正**
- 输入 un-jitter 后，混合结果天然落在基空间 ⟹ `taa_out` / `taa_hist` 自动变基空间，
  `taa_commit` 的 blit **不用改**。
- 重投影修正：现有 `prevUv` 落在上一帧的**抖动**网格上（`= β_{t−1}(w) + c_{t−1}`），需搬到基网格
  ⟹ `prevUv_base = prevUv − c_{t−1}`。
  - 🔵 **实施期修正（立文档时写的是 `± δ_t`，错）**：修正量是**上一帧自己的内容位移
    `c_{t−1}`**，不是 `δ_t = c_t − c_{t−1}`。静止机位下两者都让结果落回本像素（`δ` 只是
    `c_{t−1}` 与本帧 `c_t` 的差），**但运动场景只有 `c_{t−1}` 正确**——历史图是按 `c_{t−1}`
    长出来的，与本帧抖了多少无关。写 `δ_t` 会在场景自旋时偏一个 `c_t`（≤0.5px）。
  - **⚠ 符号必须实测钉，不许推导。** 项目自己的红线在 `taa_jitter.cppm::contentShiftPx`
    （逐轴符号推导 + 「反了游动翻倍到 1.33px」的实测背书）。护栏 = 单测
    `test_taa_jitter`「静止机位不变量」三条反向 + `imgqa temporal-jitter-transfer` 闭式对拍。
  - CPU 侧需在 `TaaState` 加一份 `c_{t−1}` 簿记，与 `prevViewProj` **同处推进**（成对字段）。
- 🔵 **实施期发现的第三个 uv 陷阱（不修则白改）**：重建世界坐标的 uv **也必须换成 `baseUv`**。
  混合用的颜色取自 `baseUv` ⟹ 输出像素代表基位置 `input.uv`；而抖动投影下「基位置 β 的世界点」
  出现在像素 `β + c_t`，故必须喂 `baseUv = input.uv + c_t` 才解出正确世界点。喂 `input.uv`
  会解出基位置 `input.uv − c_t` 的世界点 ⟹ 历史整体偏 `−c_t`（随相位变）⟹ shimmer 换个形式留着。
  **主 agent 实施时正是先写单测才发现自己踩了这个。** 深度仍按 `input.uv` 取（那是本像素的表面）。
- `c_{t−1}` 载荷：**复用 `@722224`**（Step 3 的 tonemap 偏移删除后空出的 16B 槽）。

**Step 3 — tonemap 偏移删除 + 门 `base_space` 改义（不删）**
- `rgl/shaders/tonemap.frag.slang` `baseSpaceUv` / `sampleHdrBaseSpace` 删除；回到 `input.uv`。
  **留着 = 双重 un-jitter，比改前更糟。**
- 🔵 **门的裁决改了（实施期）**：原计划「删门」，实际**改义保留**。理由：牵连面比估的大
  （`tools/imgqa/{engine,temporal,aa_coverage,judge}.cpp` + 测试都在传 `baseSpace`），
  而改义**净门数不变**、imgqa 管道原样可编译，更重要的是——
  **门开/门关 = 新/旧架构，同一构建内直接 A/B**（比批 13.2 的「同会话双构建对比」强得多）。
  新义 = **基空间累积开关**；门关 ⇒ 两个载荷恒 0 ⇒ 逐位回到改前 `base_space = false` 形态。
  门判定只有一处 = `render_list_impl.cppm::fillTaaData`（shader 侧无门逻辑，红线构造性成立）。
  ⟹ gate_lint 门数不变、无需改申报表；`temporal-base-space` 判据**结构仍成立**（门开 vs 门关），
  但其阈值（`kBaseSpaceSwimCap = 0.35`）是旧语义下定的，**须重测后回填**。
- ⚠ `SceneEnv::taaBaseSpace` 默认值原写 `false` 而 `SceneConfig` 写 `true`、main 总覆盖
  = **谎言行**（直接构造 `SceneEnv` 的测试拿到 false、生产拿到 true）。已对齐为 `true`。

### 3.2 载荷总结：**零扩列**

| 偏移 | 现义 | 新义 |
|---|---|---|
| `@722208.zw` | `jitterPx`（**shader 从未使用** = 死载荷） | 内容位移 `c_t`（**真读**） |
| `@722224` | `base_space` 偏移（tonemap 消费，已删） | 上一帧内容位移 `c_{t−1}` |

⟹ **不动 `scene_data.cppm` 偏移表、不新增 static_assert**（TAA 载荷偏移是手写常量，
不在 5 份 `*_schema.lua` 里）。

🔵 **但「预计不触发 `just reflect`」这句判断错了**——`reflection_check` SHA1 护栏**开火了**：
`scene.slang` 本身在反射清单里（我只改了注释 + 重命名一个 reader，哈希就变了）。
跑 `just reflect` 后 **`git status` 只有 `reflection_manifest.txt` 一个文件变**，
没有任何布局产物变 ⟹ 这恰好是「**零扩列**」的机器证明。
**教训入册**：写「预计不需要 X」这种话没用，护栏会说话——本次护栏就是对的那个。

### 3.3 本次**不做**（避免同时改两处导致无法归因）

- **AO 搬进 TAA 之内**：Filament 的 AO 在 TAA 输入内被时域滤波（`Renderer.cpp:1370-1376`），
  本项目在 TAA **之后**施加（`taa.frag.slang:34` 自述「AO 噪声不被时域滤波」）。
  这是**独立的第二条**根因候选，要改 pass 序，体量大 ⟹ 另立债（`还债总账` 债-39），本批不碰。
- **variance clipping / flicker filtering**：Filament 有但**默认也关**（`Options.h:621, 629`），
  属调优不属正确性 ⟹ 留 Step 4 独立批次。
- **历史重采样补 deringing clamp**：Filament 有（`taa.mat:308-313`）。
  本改造后静态相机下历史变点采样，deringing 的必要性下降 ⟹ 留 Step 4 实测后再定。

### 3.4 门与旁路纪律（红线，不许破）

- `historyValid = 0`（首帧 / resize / 相机跳切）时 shader **逐位旁路** = 输出本帧。
  改造后「本帧」= 本帧 un-jitter 版，**与 TAA 关不再逐位相同**（un-jitter 本身是重采样）。
  ⟹ 门红线的口径是「**TAA 关**时逐位不变」，**不覆盖**「TAA 开的首帧」。写测试时别写错口径。
- `jitter_scale = 0` 时 `c_t = 0` ⟹ un-jitter 退化为恒等采样、`δ_t = 0` ⟹ 历史点采样。
  **这条是本次改造的阳性锚点**，必须逐位 = 现状 `jitter_scale=0` 的结果。

### 3.5 执行顺序（为什么 1+2+3 原子）

Step 1 单独落地 = 输入在基空间、历史仍在抖动空间 ⟹ 错位 `c_{t−1}`，**比现状更糟**。
Step 2 依赖 Step 1。Step 3 若晚于 Step 2 = 双重 un-jitter，**比现状更糟**。
⟹ **Step 1+2+3 同一个 commit**，中间态不可测、不可跑。Step 4 独立。

---

## 4. 改动面清单（文件级）

| 文件 | 改什么 |
|---|---|
| `rgl/shaders/taa.frag.slang` | ✅ Step 1 输入重建（`baseUv` + **saturate**）；Step 2 `prevUvBase` 修正；**重建 uv 也换 `baseUv`**（§3.1 陷阱）；`readTaaParams` 语义改 `c_t`；新增读 `c_{t−1}`；**头注释数据流图重写**（原注释仍写「双线性采历史」，实为 CR，已漂移） |
| `rgl/ecs_impl.cppm` | ✅ `TaaState` 加 `prevContentShift` + `payloadPrevContentShift`；`prevViewProj` 注释加**成对约束**（§2.2，非反转） |
| `rgl/scene_context.cppm` | ✅ `TaaState` 两个新字段；`SceneEnv::taaBaseSpace` 默认值 `false`→`true`（消谎言行） |
| `rgl/taa_jitter.cppm` | ✅ **新增 `contentShiftPx`**：把散在注释里的逐轴符号收成**一个可测纯函数**（符号只有一个出处），实测背书随函数走 |
| `rgl/render_list_impl.cppm` | ✅ `fillTaaData`：`@722208.zw` 改写 `c_t`、`@722224` 改写 `c_{t−1}`；门判定收在此处；推导注释删重（指向 `contentShiftPx`） |
| `rgl/frame_draw_list.cppm` | ✅ `taaResolveOffset` → **`taaPrevContentShift`**（改名，旧名已无意义）；`taaParams` 注释改义 |
| `rgl/vkrenderer_impl.cppm` | ✅ memcpy 常量/字段改名 |
| `rgl/scene_data.cppm` | ✅ 偏移值不动，**注释整段重写**（原 `:234-252` 是 12.6 T4 口径 + 自相矛盾）；常量改名 `kSceneDataTaaPrevContentShiftOffset` |
| `rgl/shaders/scene.slang` | ✅ 尾区注释重写（原 `:274` 写「投回上一帧用**未抖**矩阵」与 `:268` 直接矛盾）；`readTaaResolveOffset` → `readTaaPrevContentShift` |
| `rgl/shaders/tonemap.frag.slang` | ✅ Step 3 删 `baseSpaceUv` + `sampleHdrBaseSpace`；CR 选型实测数据**搬**到 taa.frag（先补目的地再删源） |
| `rgl/scene_config*.cppm` + `configs/scene.lua` + `main_impl.cppm` | ✅ 门 `base_space` **改义**（不删）+ 注释重写 |
| `tests/test_taa_jitter.cppm` | ✅ 新增「静止机位不变量」用例（主断言 + 三条闭式反向）；新增 `enginePixelFromNdc` helper |
| `docs/gate_lint_plan.md` | ➖ **不用改**（门数不变，§3.1 Step 3 改义裁决的直接收益） |
| `tools/imgqa/` | ⏳ 新判据 `taa-noise`（§5，**待做**）；`temporal-base-space` 阈值待重测回填 |

---

## 5. 验收判据

### 5.1 主判据（**必须先晋升成判据再动代码**）

**交付相邻帧 >2LSB 像素占比**（2560×1440 用户机位、静止机位、连拍 32 帧、中央 70% 裁剪、
逐对相邻比较；探针 `/tmp/opencode/{noise_probe,taa_ab,taa_ab_after}.py`）。

**基线（2026-10-04 改造前实测，同仪器同机位）**：

| 格 | 含义 | 相邻帧均值 | **>2LSB 占比** | lag-16 同相位 >2LSB |
|---|---|---|---|---|
| `base` | TAA 开·现行（= 改前 `base_space=true`） | 0.8363 | **9.070%** | **0.000%** |
| `anchor_js0` | `jitter_scale=0`（阳性锚点） | 0.0011 | **0.000%** ✅ | 0.013% |
| `anchor_off` | TAA 关（阴性锚点） | 0.0000 | **0.000%** ✅ | 0.000% |
| `rev_fb1` | `feedback=1.0`（反向：无累积） | 2.8000 | **21.875%** ✅ | 0.000% |

- 与历史记录的 9.22% / 21.71% 吻合（不同裁剪/解码细节），**仪器复现了已知基线**。
- **`lag-16 同相位 >2LSB = 0.000%` 精确成立** ⟹ 逐帧变化 **100% 由抖动相位索引决定**，
  不含任何随机成分。这正是「相位相关重采样」的指纹（若是随机噪点，同相位帧不会逐位相同）。
  ⟹ 这条同时**排掉**了「AO hash 噪声」作为主因（hash 与相位无关，会在 lag-16 留下残差）。
- 目标：**< 3%**（先立门槛，实测后按数据收紧并回填）。
  ⚠ 据 §1.2，**地板不是 0**（输入侧 un-jitter 本身相位相关），别把目标定成 0 然后误判失败。
- ⚠ 该统计目前只在 `/tmp/opencode/`（Python + PIL，一次性诊断），**未入仓**。
  ⟹ **晋升为 `imgqa taa-noise` 子命令，C++ + opencv4**（工具语言纪律：进仓库的工具必须 C++ + vcpkg）。
  带阳性锚点 + 反向用例 + 变异红线声明 → skill `rgl-test-discipline`。
- **阳性锚点**：`jitter_scale = 0` 必须 **0.000%**。
- **反向用例**：`feedback = 1.0`（无累积）必须显著更差（现状 21.71%）。
- **⚠ 现有判据的设计缺陷（记档）**：`jitter_scale=0` 这个锚点**区分不了**「历史重采样」
  「AO 未被滤波」「裁剪盒随相位变」三条机制——三条在 `js=0` 时一起死。
  上一轮据此结案为「TAA 自身交付时域不稳定」，**判据不足以支撑该归因**。新判据必须能单独开关每条机制。

### 5.2 糊度（防止「不抖了但糊成一团」）

- 梯度能量 vs `feedback=1.0` 基线；并与「理想 16 相位超采样」参考比（`phase13_plan` §1.5 的方法）。
- 现状：梯度比 **0.671**（比自身应收敛到的还糊 1.5 倍）。目标：**≥ 0.85**。

### 5.3 红线

- **TAA 关时截图 sha256 逐位不变**（做法先例 = 批 13.2「同会话双构建对比」）。
- `just test` 全过；`just checks` / `doc-check` 0 警告。
- 跑批 0 error 0 warning（`just smoke`）。

### 5.4 判据资产现状（改造会打到哪些）

| 判据 | 影响 |
|---|---|
| `imgqa taa-swim` | 保留，语义仍成立（量交付游动） |
| `temporal-jitter-transfer` | 保留，**符号护栏之一**（§3.1 Step 2）；主护栏是单测「静止机位不变量」 |
| `temporal-base-space` | 🔵 **不退役**（门改义而非删除，§3.1 Step 3）：结构「门开 vs 门关」仍成立，但阈值 `kBaseSpaceSwimCap = 0.35` 是旧语义下定的 ⟹ **须重测回填**，否则是一条阈值失准的判据 |
| `tools/imgqa/temporal.cpp` | 只量 swing，量不到本症状（债-34 缺口）⟹ 新子命令补上 |

---

## 6. 风险

1. **符号写反** → 游动翻倍而非归零。护栏 = 单测三条闭式反向（§4）+ `temporal-jitter-transfer`
   闭式对拍。**不许靠推导定符号**；符号唯一出处 = `taa_jitter.cppm::contentShiftPx`。
2. **输入双线性重建偏糊** → 实测；不可接受再试 Lanczos-2 并**同时测振铃**（13.2 的教训：
   单看梯度会翻案，必须联合列退化看）。
3. **12.6 T4 被未来改回 / 成对约束被破**（§2.2）→ 注释写成**成对条件式**（三处），
   单测反向 ② 钉「prevViewProj 改未抖 + shader 仍减 c」这个失败形态。
4. **测量口径已变**：2026-10-04 HPD 修复后帧缓冲是真实 1:1 像素，
   **此前所有 2560×1440 噪点数字对应的是 kwin 100% 缩放时的画面**。跨批对比必须重跑基线。
   （headless 定分辨率探针不受 HPD 影响，实测基线与历史吻合，已验证。）
5. **首帧/resize 旁路口径**（§3.4）→ 测试别把「TAA 开首帧」写成「与 TAA 关逐位相同」。
6. ✅ **已堵：共享 sampler 的 wrap 是 REPEAT**（`sampler_library.cppm` 平铺 UV 定案）。
   `baseUv = input.uv + c_t·texel` 最多越界半纹 ⟹ 最外一圈采到**对侧**像素（1px 宽假边）。
   ⟹ 必须 `saturate()`。此坑原载于批 13.2 tonemap resolve 注释，**随代码搬进 taa.frag 才没丢**
   ——若当初直接删 tonemap 那段注释不搬，这就是一个跟着上线的 1px 假边。
7. ✅ **已堵：三个 uv 必须分清**（`input.uv` 取深度 / `baseUv` 重建世界 / `prevUvBase` 采历史）。
   混用任两个都「画面仍像 TAA」，只有静止机位不变量单测能抓（本次真的抓到了，见 §3.1）。

---

## 7. 回滚

Step 1+2+3 单 commit ⟹ `git revert <sha>` 一步回滚。
门 `taa.base_space` **保留**（改义不删）⟹ 回滚后旧语义自动恢复，无残留死门；
且**不回滚也能回退**：`base_space = false` 即逐位回到改前 `base_space=false` 形态。

---

## 8. 执行清单（2026-10-04 执行进度）

- [x] **0** 基线跑完（§5.1 表）。⏳ **晋升 `imgqa taa-noise`（C++ + opencv4）仍未做**
      ——当前判据跑在 `/tmp`（Python + PIL），**收账前必须晋升**，否则这条主判据不在仓库里。
- [x] **1** Step 1+2+3 落地（一个 commit）：输入 un-jitter / 历史基空间 + `c_{t−1}` 修正 /
      tonemap 偏移删除 + 门改义
- [x] **2** 符号护栏：新增单测「静止机位不变量」（主断言 + 三条闭式反向），
      **变异实测**：`cPrev` 取反 → 62 红；重建改喂 `uv` → 93 红（非空转）
- [x] **3a** 主判据改造后：**9.070% → 6.908%**（门开），门关 24.787%（复现历史 24.99%）
      ⟹ **有改善但没到 <3% 目标**，残余归因见 §5.1b
- [x] **3b** `just test` 189 全过、`just checks` / `doc-check` 0 警告、`just reflect` 已跑
      （仅 manifest 哈希变 ⟹ 零布局变更的机器证明）
- [x] **4** 注释面全部重写（`scene_data.cppm` / `scene.slang` / `taa.frag.slang` /
      `tonemap.frag.slang` / `ecs_impl.cppm` / `scene_config*.cppm` / `configs/scene.lua`）
- [ ] **3c** 糊度（梯度能量）与门红线（TAA 关截图逐位不变）——**待跑**
- [ ] **5** `temporal-base-space` 阈值按新语义重测回填（门保留，判据结构不退役）
- [ ] **6** 文档收账：`调试历史.md` 条目（含「`js=0` 锚点区分不了三机制」这条负知识 +
      「先写单测才发现 shader 写错」这条正知识）、`还债总账.md` 债-38 三态勾账、
      `phase13_plan.md` 批 13.5 登记
- [ ] **7** Step 4 独立批次（deringing / variance clipping / flicker）——**实测后再决定**
- [ ] **8** 债-39（AO 进 TAA 之内）按 §5.1b 归因结果决定优先级
