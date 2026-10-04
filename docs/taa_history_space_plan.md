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

### 1.1 这一条解释了全部五条独立实测（这是它作为根因的主要证据）

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
2. **⚠ 批 12.6 T4 的裁决要反转，且必须在注释里写清「当时对、现在错」。**
   `scene_data.cppm:241-243` 记载：曾长期存**未抖** `prevViewProj`，被改判为错
   （「把『像素中心对齐』误读成『在未抖网格里』」，后果 = 采样点系统性偏 `-jitter_prev`，
   静止机位 S1/S2 = 0.48×）。**那个裁决在「历史住抖动空间」的前提下是正确的。**
   本改造把历史搬到基空间后，未抖口径重新成立。
   ⟹ 改注释时必须写成**条件式**（「历史在基空间 ⟹ 用无抖矩阵；若历史回到抖动空间则必须回到抖动矩阵」），
   否则未来某个会话会照 12.6 T4 把它改回去。这是本改造最大的**回归风险**。
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
- 重投影修正：现有 `prevUv` 落在上一帧的**抖动**网格上，需搬到基网格
  ⟹ `prevUv_base = prevUv_jittered ± δ_t`，`δ_t = c_t − c_{t−1}`（闭式均值 0.667px）。
  - **⚠ 符号必须实测钉，不许推导。** 项目自己的红线在 `render_list_impl.cppm:261-274`
    （「逐轴符号不同，勿凭直觉写」，x/y 逐轴符号不同、且经 `clip.w = −v.z` 翻号）。
    护栏 = `imgqa` 的 `temporal-jitter-transfer` 闭式对拍 + 新增「δ 取反必须更差」反向用例。
  - CPU 侧需在 `TaaState` 加一份 `c_{t−1}` 簿记，与 `prevViewProj` 同处推进（`ecs_impl.cppm:388` 附近）。
- `δ_t` 载荷：**复用 `@722224`**（Step 3 删 `base_space` 后空出的 16B 槽）。

**Step 3 — 删 `base_space`（事后补救不再需要）**
- `rgl/shaders/tonemap.frag.slang:75-87` `baseSpaceUv` 删除；`sceneHdrTex` 与 `aoTex` 回到 `input.uv`。
- 门 `taa.base_space` 申报删除 —— **不删会被 gate_lint 的死门护栏点名**（38 门覆盖率护栏，
  见 `docs/gate_lint_plan.md`）。同步删 `scene_config` 读取与 `configs/scene.lua` 键。
- 判据 `temporal-base-space` **退役**（它验「门开 vs 门关」，门没了）⟹ 改造成
  「静止机位交付游动 ≤ 阈值」的**绝对**判据，别直接删掉不留替代。

### 3.2 载荷总结：**零扩列**

| 偏移 | 现义 | 新义 |
|---|---|---|
| `@722208.zw` | `jitterPx`（**shader 从未使用**） | 内容位移 `c_t` |
| `@722224` | `base_space` 偏移（Step 3 删） | `δ_t` |

⟹ **不动 `scene_data.cppm` 偏移表、不新增 static_assert、预计不触发 `just reflect`**
（TAA 载荷偏移是 `scene_data.cppm:253-255` 手写常量，不在 5 份 `*_schema.lua` 里）。
但 `reflection_check` SHA1 护栏会告诉我们是否判断有误——**以护栏为准，别信本文这句**。

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
| `rgl/shaders/taa.frag.slang` | Step 1 输入重建；Step 2 `prevUv` 修正；`readTaaParams` 语义改 `c_t`；新增读 `δ_t`；**头注释数据流图重写**（现注释仍写「双线性采历史」，实为 CR，已漂移） |
| `rgl/ecs_impl.cppm` | `TaaState` 加 `c_{t−1}` 簿记；`prevViewProj` 口径注释改**条件式**（§2.2）；`projUnjittered` 的去留裁决 |
| `rgl/scene_context.cppm` | `TaaState` 新字段；`projUnjittered` 注释（现称「供重投影用」= 谎言行） |
| `rgl/render_list_impl.cppm` | `fillTaaData`：`@722208.zw` 改写 `c_t`、`@722224` 改写 `δ_t`；符号推导注释更新 |
| `rgl/frame_draw_list.cppm` | `taaParams` / `taaResolveOffset` 字段名与注释（名字要不要跟着改，倾向改：`taaContentShift` / `taaHistoryShift`） |
| `rgl/scene_data.cppm` | 偏移值不动，**注释必须重写**（`:234-252` 整段是 12.6 T4 口径） |
| `rgl/shaders/tonemap.frag.slang` | Step 3 删 `baseSpaceUv` |
| `rgl/scene_config*.cppm` + `configs/scene.lua` | 删 `taa.base_space` 门与键 |
| `docs/gate_lint_plan.md` | 门申报表同步（少一门） |
| `tools/imgqa/` | 新判据 `taa-noise`（§5） |

---

## 5. 验收判据

### 5.1 主判据（**必须先晋升成判据再动代码**）

**交付相邻帧 >2LSB 像素占比**（2560×1440 用户机位、静止机位、连拍 16 帧、逐对相邻比较）。

- 现状基线：**9.22%**
- 目标：**< 3%**（先立门槛，实测后按数据收紧并回填本文）
- ⚠ 该统计目前只在 `/tmp/opencode/noise_probe.py`（Python + PIL，一次性诊断），**未入仓**。
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
| `temporal-jitter-transfer` | 保留，**符号护栏靠它**（§3.1 Step 2） |
| `temporal-base-space` | **退役**，改造成绝对游动判据（§3.1 Step 3） |
| `tools/imgqa/temporal.cpp` | 只量 swing，量不到本症状（债-34 缺口）⟹ 新子命令补上 |

---

## 6. 风险

1. **符号写反** → 游动翻倍而非归零。护栏 = `temporal-jitter-transfer` 闭式对拍 +
   「δ 取反必须更差」反向用例。**不许靠推导定符号。**
2. **输入双线性重建偏糊** → 实测；不可接受再试 Lanczos-2 并**同时测振铃**（13.2 的教训：
   单看梯度会翻案，必须联合列退化看）。
3. **12.6 T4 被未来改回**（§2.2）→ 注释写成条件式，且加一条单测钉住「历史在基空间」的口径。
4. **测量口径已变**：2026-10-04 HPD 修复后帧缓冲是真实 1:1 像素，
   **此前所有 2560×1440 噪点数字对应的是 kwin 100% 缩放时的画面**。跨批对比必须重跑基线。
5. **首帧/resize 旁路口径**（§3.4）→ 测试别把「TAA 开首帧」写成「与 TAA 关逐位相同」。

---

## 7. 回滚

Step 1+2+3 单 commit ⟹ `git revert <sha>` 一步回滚。
配置门 `taa.base_space` 的删除也在同一 commit 内（回滚后自动复活，无残留死门）。

---

## 8. 执行清单

- [ ] **0** 晋升 `imgqa taa-noise`（C++ + opencv4），跑出现场基线表（含 `js=0` 阳性锚点、`fb=1.0` 反向）
- [ ] **1** Step 1+2+3 一个 commit：输入 un-jitter / 历史基空间 + `δ_t` 修正 / 删 `base_space`
- [ ] **2** 符号实测：`temporal-jitter-transfer` 闭式对拍 + 「δ 取反必须更差」反向用例
- [ ] **3** 跑 §5 全部判据（主判据 + 糊度 + 门红线），回填实测数字到本文 §5.1
- [ ] **4** 注释面：`scene_data.cppm:234-252` 重写为条件式；`taa.frag.slang` 头注释数据流图重写
- [ ] **5** 判据退役处理：`temporal-base-space` 改绝对判据；gate_lint 门表同步
- [ ] **6** 文档收账：`调试历史.md` 条目（含「`js=0` 锚点区分不了三机制」这条负知识）、
      `还债总账.md` 债-38 结案 + 债-33 归因更正、`phase13_plan.md` 批次登记
- [ ] **7** Step 4 独立批次（deringing / variance clipping / flicker）——**实测后再决定要不要做**
- [ ] **8** 另立债-39：AO 搬进 TAA 之内（独立根因候选，本批不碰）
