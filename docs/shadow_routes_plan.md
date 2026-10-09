# 阴影路线收敛（批 N6）

> **状态**：现行（2026-10-09 立）。本文件 = 债-16 / 债-17 这条线的**批次载体 + 定案记录**。
> 机制深版仍以各 `.cppm` 文件头注释为权威；成本读数在 `hfts_izb_plan.md` §4.4 与
> `evssm_plan.md` §6.2；本文件不重抄它们。

## 0. 一句话

阴影今天有 **4 个平铺布尔 + 3 个被两条路线共用的数值键**，选路判断散在 6 处；本批把它收敛成
**一个枚举 + 每档自己的调参块 + 一个正交的 IZB 修饰**，并把「IZB 出厂到底开不开」三处打架钉成一处。

## 1. 用户拍板（2026-10-09，三项）

| 项 | 定案 |
|---|---|
| 配置落点 | **新建 `configs/shadow.lua`**（不塞进 diagnostics / render_graph） |
| EVSSM | **先收敛配置，修复另开批**（N6c）——不在本批动着色器 |
| IZB 默认值 | **承认出厂开**：改代码默认 + configs 注释 + `hfts_izb_plan.md` 三处，并加对账测试 |

## 2. 配置形状定案

```lua
-- configs/shadow.lua
return {
    mode = "csm",       -- "none" | "csm" | "pcss" | "evssm"（互斥；未知值 = 启动失败，不静默回落）
    izb  = true,        -- 正交修饰：光空间硬影 mask（要求 mode ~= "none"，仅 deferred light pass 消费）
    csm   = { },
    pcss  = { sun_degrees = 0.53, search_radius = 16.0, max_penumbra = 32.0 },
    evssm = { sun_degrees = 0.53, search_radius = 16.0, max_penumbra = 32.0 },
}
```

四条设计红线：

1. **IZB 不进枚举**。交付路径就是 `csm + izb` 同时开（红线 A 那张图），且「IZB 近场细影 + 远场
   软影」混接（`evssm_plan.md` 11.6e）要求两者共存 ⟹ 枚举管**软影滤波轴**，`izb` 管**硬影 mask**，
   两条轴正交。`izb = true × mode = "none"` 由 gate_lint error（与债-32 那条同形同判据）。
2. **旧键 fail-fast，不静默映射**。`shadow_pass` / `shadow_pcss` / `shadow_evssm` / `pcss_*`
   出现在 `diagnostics.lua` = 启动失败并指路新键。理由：本项目最怕的失效形态是「配置写了、
   什么都没发生」；静默映射正是它的制造机。
3. **数值键按档分立**（消掉债-16⑤ 的「两套门共用一套 `pcss_*`」）。
4. **形态决策表住代码，不住文档**。`mode → { 采样变体后缀, 构造哪些图外模块, 消费点, 前置门 }`
   的唯一真相源 = 新模块里的单点查表函数；`gate_lint` / `shader_registry` / `Renderer` 三处从它读。
   本文件**不抄这张表**（抄一次漂一次，先例 = `pass_vocabulary`）。

## 3. 批次拆分与验收

| 批 | 内容 | 验收判据 |
|---|---|---|
| **N6a** | `shadow_config` 新模块（rlbase，无设备依赖）+ `configs/shadow.lua` 落地 + 旧键 fail-fast + 启动回显。**新门与旧门并存，消费点仍读旧门** ⟹ 本批天然不改画面 | `just build` 0 警告 · `just test` 全绿 · 红线 A 逐位 · 新单测：四值解析 / 未知值抛 / 旧键抛 / 缺文件回落 |
| **N6b** | 6 处消费点切到新门 + `gate_lint` 阴影规则重写成「枚举 + izb」+ 删旧键与旧字段 | 红线 A 逐位 · **四臂 sha 对账**（下表）· `just smoke --sync` hazard 0 · `just test` 全绿 |
| **N6c** | IZB 默认值三处对齐 + 「出厂 configs 值 == 代码默认」对账测试 + 本文件收账 | 对账测试变异验红 · 红线 A 逐位 |
| **N6d** | EVSSM 修复批（**判据先行**，见 §5） | 修复批自己定 |

### 3.1 N6a 落地实况（2026-10-09，只记与 §2 定案的差异 + 符号名）

**符号面**（`rgl/shadow_config.cppm` 接口 + `rgl/shadow_config_impl.cppm` 实现，均登记
`RGL_BASE_MODULE_SOURCES`）：`ShadowMode` / `shadowModeName` / `parseShadowMode`（未知值
error 并列出四合法值，`cullReplayName` 同口径）/ `ShadowTuning` / `ShadowConfig` /
`ShadowKeyTrace` / `shadowConfigEchoKeys`（回显表 `kShadowEcho` = 键名单一真相源，
`kGateEcho` 同构）/ `describeShadowGates` / `describeAllShadowGates` / `loadShadowConfig`。
旧键 fail-fast = `diagnostics_impl.cppm` 的 `checkMigratedShadowKeys`（9 键逐个点名 +
指路新键路径）。main 接线 = `loadAllConfigs` 内加载 `configPath("shadow.lua")`，失败 =
启动失败，**不回灌**；`tools/smoke.py` 的 `CONFIG_FILES` 已含 `shadow.lua`。

与定案的差异（实现选择，非设计改动）：

1. **`izb` 两形态**：定案的布尔形状（`izb = true|false`）之外**多收一个子表形态**
   `izb = { on, grid, dump_frame }`——N6a 要求把旧 `izb_grid` / `izb_dump_frame` 一起搬进
   shadow.lua，`ShadowConfig` 因此比 §2 草案多 `izbGrid`（clamp [64,2048]）与
   `izbDumpFrame`（负数=0）两字段。其他类型（如字符串）= 报错不静默。
2. **`csm = { }` 预留空块进回显表**（恒 `csm=reserved`，差集语义下永不进 info 行）——
   未知键 lint 的已知集判据 = 回显表键名前缀（diagnostics 同判据），预留块不挂号就会被
   喊成未知键。
3. **旧键「仍被读」但「出现即炸」**：`applyScalarGates` 的 `readBool("shadow_pass")` 等
   读取行**保留**（键轨迹/回显覆盖率护栏的 39 键集合不变），fail-fast 在读取之前拦——
   报错路径永远先于取值路径，不存在「静默映射」的缝隙。
4. **`DiagnosticsConfig::izbShadow` 默认 false→true**（§1 拍板 3 的代码落点；N6c 的
   「三处对齐」还剩 configs 注释与 hfts_izb_plan.md 两处）。连带：`tests/test_gate_lint.cppm`
   四个用例将「全默认」当合法世界替身的臂改为显式 `izbShadow=false`——**lint 规则本体
   （gate_lint_impl）未动**，运行时行为不变（旧 configs 本就写 `izb_shadow=true`，
   forward + 出厂配置改造前同样是 error）。
5. **工具/qa 配置面连带 fail-fast（N6b 收口，本批无法提前迁）**：`qa/configs/{analytic,
   nm_rig,env_band}/diagnostics.lua`、`tools/imgqa/engine.cpp` 生成的 diagnostics.lua、
   `tools/cpu_baseline.py` 的 shadow-off/izb-off 臂仍写旧键 ⟹ 这些入口现在**启动即失败**
   （报错文本指路 shadow.lua）。不能提前把它们迁到 shadow.lua：新门本批不被消费点读取，
   迁了 = 「配置写了、什么都没发生」的静默形态，比响亮失败更糟。
6. 回显三行制（比 diagnostics 多一行）：`shadow: mode=csm izb=true` 路线行**恒打**
   （info，"这台机器跑哪条阴影路"是排障第一问）+ 差集 info（全默认不喊）+ 全量 debug。

**五臂 sha 对账表**（2026-10-09 改造前实测，deferred / 1280×720 / 第 8 帧 / 本机 8060S）——
N6b 改完必须逐臂复现，这是「重构没动语义」的硬锚点，比「看着没坏」强得多：

| 形态 | 旧配置 | 新配置 | 截图 sha256 前缀 |
|---|---|---|---|
| 交付（CSM + IZB） | `shadow_pass=true, izb_shadow=true` | `mode="csm", izb=true` | `fdece9d0` |
| 无影 | `shadow_pass=false` | `mode="none"` | `121e7502` |
| CSM 单独 | `shadow_pass=true, izb_shadow=false` | `mode="csm", izb=false` | `52231a84` |
| PCSS | `shadow_pcss=true, izb_shadow=false` | `mode="pcss"` | `da06ca00` |
| EVSSM | `shadow_evssm=true, izb_shadow=false` | `mode="evssm", izb=false` | `1a6757f3` |

**五臂复现（2026-10-09 N6b 改完实测，同机 8060S）**：`fdece9d0` / `121e7502` / `52231a84` /
`da06ca00` / `1a6757f3` —— **逐臂逐位相同**。量法 = 备份 `configs/shadow.lua` → 逐臂改写 →
`just shot --name <臂>` → `sha256sum build/rgl/screenshots/<臂>.png` → trap 还原。

### 3.2 N6b 落地实况（2026-10-09，只记与 §2 定案的差异 + 符号名）

**消费点面**（六处一律 `shadowFormPlan(shadowConfig())`，判据不再散写）：
`render_list_impl.cppm`（`collectShadowCasters` / `fillShadowData` 的 `drawAtlas`）、
`shader_registry_impl.cppm`（`softShadowVariantSpec` 的 `spvSuffix` + `addLightEntry` 的
`consumeIzbMask`）、`vkrenderer_offgraph.cppm`（`izbPreFrame`/`izbLightPassPre`/`evssmPreFrame`）、
`main_scene.cppm`（IZB mask 条目）。单例对 = `shadowConfig()` / `setShadowConfig()`
（`main_impl.cppm` 的 `loadAllConfigs` 回灌，同 `diagnostics()` 范式，不穿构造函数参数）。
`DiagnosticsConfig` 的九个旧字段与 `kGateEcho` 九行**已删**（39→30 键），
`checkMigratedShadowKeys` **保留为永久护栏**。

与定案的差异（实现选择，非设计改动）：

1. **scene data 两个软影区按档分立**：`shadowPcss` 区读 `shadowConfig().pcss`、`shadowEvssm`
   区读 `shadowConfig().evssm`，两块都**照旧无条件写**（与改造前同形）。出厂两块同值
   （0.53/16/32）⟹ 交付形态下写出的字节逐位相同（五臂已证）。这是债-16⑤ 的落点。
2. **`ShadowFormPlan` 没有 `tuning` 字段**（主 agent 复核时删掉了子 agent 加的那版）：两个
   scene data 区各读各的块、且**同时被写**，"本档的那一块"在运行期没有消费者——给它写
   断言就是自指绿例（只有测试读它）。且它是 `const ShadowTuning&` 成员，`shadowFormPlan(ShadowConfig{})`
   即悬垂引用（AGENTS 生命周期红线那一类）。调参分立的真护栏改钉在**有消费者的地方**：
   `tests/test_render_list_shadow.cppm` 的「软影两区各读各的调参块」（串档即红）。
3. **`consumeIzbMask` 刻意不掺 `drawAtlas`**：IZB 的构造判据今天就只有 izb 一个
   （`IzbShadow::recordFrame` 无条件清零 = 债-32 的收口，掺上 drawAtlas 就把它拆了）。
   「izb × mode=none 必然一帧阴影也不产」是 gate_lint 的 **error**，不是运行期早退。
4. **gate_lint 阴影规则按枚举重写**（13 行 / 11 门）：`izb × mode="none"`(error)、
   `izb × 非 deferred`(error)、`mode="evssm" × 非 deferred`(error)、`izb.dump_frame × izb 关`
   (warn)、`izb.grid × izb 关`(inertNote)、`pcss.* × mode≠pcss` ×3、`evssm.* × mode≠evssm` ×3
   (inertNote)、`csm`(nullptr)。**「PCSS 让位 EVSSM」那条 warn 整条删除**——枚举下两门同开
   表达不出来，让位链不是被修好，是被消掉（§2 红线 1 的推论）。
5. **`qa/configs/analytic/shadow.lua` 除 `mode="none"` 外还须显式 `izb = false`**（定案表里
   无影臂只写了 `mode="none"`）：N6a 把 izb 的代码默认翻成 true，而该目录是 **forward** 图
   ⟹ izb 开着既撞 gate_lint 的 `izb × 非 deferred` error，又会把 frag 换成 IZB 变体（换 SPV
   = 换被测对象）。nm_rig / env_band 同理。
6. **`tools/cpu_baseline.py` 的 izb-off 臂只能整键替换** `izb = false`（`smoke.patch_lua`
   不支持 `izb.on` 这类嵌套键）——解析器的 izb 两形态（布尔 / 子表）在此派上用场。
7. **`shadow_config_impl.cppm` 的 `loadShadowConfig` 局部量必须叫 `root`**：imgqa 的键名
   校验器（`knownRootKeys`）按 `root["键"]` 正则从源文件抽「解析器认识的键」，叫 `table`
   就抽不到 `mode`，校验器反过来拒绝合法的 shadow.lua。

## 4. EVSSM 生效性实测结论（2026-10-09，本机 8060S / Mesa 26.2.3）

**结论：EVSSM 生效（构造 + 改画面 + 零 error），但它改的不是「软」——半影机制实测近乎恒等。**

| 臂 | 影覆盖 | 平均变暗 | 过渡带/深本影 |
|---|---|---|---|
| CSM（现行） | 9.72% | 51.5 LSB | 1.021 |
| EVSSM 默认参数 | 5.87% | 74.5 LSB | 0.185 |
| EVSSM 太阳 5°（物理值 ×9.4） | 6.00% | 71.6 | 0.229 |
| EVSSM 太阳 **0°**（半影恒 0） | 5.87% | 74.6 | 0.184 |
| EVSSM 搜索/半影上限 16/32→64/64 | 与默认臂**逐位相同** | | |

- 太阳角 0° 与 0.53° 全图只差 **1245 / 921600 像素**（>8LSB 仅 105 像素）⟹ 半影项不承重。
- 搜索半径与半影上限提到 64/64 **逐位不变** ⟹ 两个键在默认配置下根本不承重。

**成因（闭式，两条互锁）**：

1. `evssm_moments.comp.slang` 只写 `(z, z², −1/z, 1/z²)` ⟹ mip0 处 `m2 − m1² ≡ 0`，方差只剩地板
   `ε·pw²`（ε = 0.002，与 Filament 同值）。`pMax > lbr(0.1)` 要求 `exp(2c·Δ) > 0.866`
   ⟹ **遮挡体必须落在本级联归一化深度 1.31% 以内**，否则判全亮。级联 0 深度跨度 ~10m
   ⟹ 13cm 以外全亮 = **只交付接触影**。这就是覆盖率 9.72%→5.87% 的成因。
2. 抬 mip 的唯一入口是半影：`penumbraTexels = Δz·tan(θ/2)/texelWorld`，θ=0.53° 时要
   Δz > 215×texelWorld（本场景 ≈4.3m）才够 1 纹素。而 4.3m 早已越过第 1 条的亮区
   ⟹ **中间那段既不被半影软化、也不被 EVSM 判成影 = 直接丢影**。

**推翻一条写在 shader 头注里的前提**：`shadow.slang` 的 `EVSSM_SOFT` 头注称「解析方差**不加**——
矩在 compute 侧生成无导数可用」。**compute 读邻 texel 做中心差分就是解析方差**，Filament 用
屏幕导数只是它的便利形式。该前提不成立 ⟹ 修复有路。

### 4.1 两条独立确证 + 一条量具边界（2026-10-09 批 N6d 步 0 排查）

- **确证 A（大 Δz 直接不产影）**：自建「上下两层岛板、上层下缘距下层地板 88m」的合成靶
  （理论半影 ~10m）实测 `none` vs `evssm` **逐位相同**、`none` vs `pcss` 亦逐位相同
  ⟹ 大 Δz 下软影两档**完全不产影**。这不是"半影算错了"，是 §4 成因①的**直接显形**：
  遮挡体落在本级联归一化深度窗外 = 判全亮。**覆盖率丢到 0 而不是丢到"糊"** = 窗函数而非衰减。
- **确证 B（PCSS 也丢覆盖，但机制不同）**：交付场景同机位六臂（1280×720 / deferred / `izb=false`）
  影覆盖 `csm` **9.84%** > `pcss` **6.89%** > `evssm` **5.89%**。PCSS 不走 EVSM 方差窗，
  它丢的 30% 来自 `blockerCnt == 0` 的全亮早退（搜索半径内没找到 blocker 就当没影）。
  ⟹ **"软影路线覆盖率低于 CSM"不是 EVSSM 独有**，N6d 验收时拿 CSM 当唯一基准会冤枉人；
  但 PCSS 的丢法是**可解释的搜索失败**、EVSSM 的丢法是**深度窗外**，两者可分。
- **量具边界（重要，别再走一遍）**：`shadow_rig` **量不出半影**。它的存在理由就是跨级联
  （距离梯 12→1025m、岛台撑到 radius≈831m）⟹ 级联 `texelWorld` ≈ **0.29 / 0.70 / 1.14 / 2.63 m**，
  而全部遮挡体 Δz ≤ 12m ⟹ `penumbraTexels = Δz·tan(θ/2)/texelWorld < 1` **对所有档恒成立**
  （shader 结构上零模糊）。实测 `near_anchor` 臂 sun0↔sun5 差 **6 px**、`far_sweep` 臂**逐位相同**。
  `--size` 放大与换机位**都无效**（信号与阈值同比缩放，量化发生在 2048² 图集里）。
  ⟹ 半影类判据的场景必须**小尺度**（交付场景 bistro 的级联 0 texelWorld ≈ 2cm，够用）；
  而 `shadow_rig` 的既有判据（`faces` / `switch` / `attribution`）不受影响、**不要为了量半影去改它**。
- **量具必须钉帧号**：同配置第 8 帧与第 12 帧的影覆盖差 0.12pp（场景 `spin` 在转）⟹
  判据的 `delay_frames` 是量具的一部分，写死、别用默认值。

## 5. N6d 修复批的候选与前置（本批不做，登记勿丢）

**判据先行**（否则修完不知道有没有修好）：imgqa 判据 `shadow-softness` 的设计定案见 §5.1。
同时把 §4 的闭式上限落成 `tests/test_evssm_math.cppm` 的用例（零方差矩 + 深度偏移 → 受光量必须 ≈1）。

### 5.1 `shadow-softness` 判据设计定案（2026-10-09，含一次设计自纠）

**场景 = 交付配置**（`ProfileOpts::deliveryConfigDir = configs/`，只覆写 `shadow.lua`），
**不是 `shadow_rig`**——理由见 §4.1 量具边界。**`izb` 必须覆写成 false**（IZB 硬影 mask 乘在
软影之上会吞掉半影：实测带核比 0.097 开 / 0.185 关）。本判据测的是**软影滤波轴本身**，
交付形态（IZB 开）的读数是 `izb` 命令的事。

**五臂**：`none`（参照）· `csm` · `evssm`（默认 sun 0.53）· `evssm_sun0` · `pcss_sun0` / `pcss_sun5`
（后两臂只服务判据 1）。

**度量**（全部是**臂间差分统计**，不需要几何区域、不需要基线图）：影覆盖率（相对 `none`
变暗 >8LSB）、平均变暗、带核比（`frac(8<Δ≤40)/frac(Δ>40)`）、太阳角敏感度
（`frac(|L_sun0 − L_sunX| > 8LSB)`）。

**三条判决线（跑前写死）**：

| | 判据 | 现状读数 |
|---|---|---|
| J1 | **量具有效**：`sunSens(pcss, 0→5°) ≥ 1.0%` | 1.955% ✅ |
| J2 | **现状缺陷**（现在必须红、修完必须绿）：`sunSens(evssm, 0→0.53°) ≥ 0.5%` | **0.013%**（>8LSB 口径，120 px）❌ |
| J3 | **对账出数**（advisory）：`coverage(evssm)/coverage(csm)`、`bandCore(evssm)/bandCore(csm)` | 0.60 / 0.18 |

**口径别混**（§4 与本判据的数不同源，都对）：§4 的 **0.14%（1245/921600）= any-diff**（任一字节不同）、
第 8 帧；J2 的 **0.013%（120/921600）= >8LSB**、第 12 帧。同一次运行里两个口径同时打印
（any-diff 0.183% = 1686 px），换口径不改变"远低于 0.5%"这个结论。J1 两侧同是 >8LSB 口径
（定案探针 1.955% / 命令首跑 1.928%，差 0.03pp 来自 profile 构造路径不同）。

**J1 为什么用 PCSS 而不是 EVSSM 自己**（这是本设计自纠的一处，别改回去）：原设计拿
「EVSSM sun0 vs sun5」当阳性锚点，**那是循环论证**——被测对象坏了，锚点必然也红，
就分不清"量具瞎了"还是"缺陷还在"。PCSS 与 EVSSM **共用同一条 `2·tan(θ/2)·Δz` 半影公式**，
但它的深度比较是 `saturate((d−refZ)/w+0.5)` 直采，**不经过 EVSM 方差窗** ⟹ 是一条独立的
"太阳角→半影"实现。量具能测出 PCSS 的响应（实测 1.955%）就证明它不是瞎的，
EVSSM 的 0.14% 就是缺陷而不是测量失败。

**J2 为什么比 0.53° 而不是 5°**：0.53° 是出厂值，**用户看到的就是它**。修复必须让
**默认配置**对太阳角有响应，而不是让一个极端值有响应。

修法候选（按性价比）：

- **F1 方差地板按深度不确定度给**（不是恒 `ε·pw²`）：σ 由图集纹素对应的世界深度不确定度闭式算出。
  直接补 mip0 零方差这个洞。
- **F2 矩 compute 补有限差分解析方差**（推翻 §4 那条前提后成立），Filament 的解析方差项逐式转写。
- **F3 半影下限 = 1 纹素**：`penumbraSquashed = max(penumbra, k)`，保证至少走宽核；**单独做无意义**，
  必须与 F1/F2 同批（否则只是把 razor-sharp 测试搬到更粗的 mip 上）。
- **F4 调小指数 c**：让指数矩相对带宽变宽，代价是 fp16 下的精度余量与既有 5.5 的定案依据。

**转正裁决的前置**：先解释「为什么远场软影反而更硬」——§4 已给出机制答案，剩下的问题是
F1/F2 修完之后覆盖率与带核比能否**同时**不劣于 CSM。

## 6. 收账（2026-10-09）

**已完成**：N6a（`configs/shadow.lua` + `shadow_config`，只回显不回灌）· N6b（六处消费点切
`shadowFormPlan()`、删九个旧字段、gate_lint 按枚举重写、工具面修复）· N6c（四篇现行文档口径
对齐 + 本文件与总账勾账）。commit：`d31fd91` / `218f35c`。

**量法（本轮所有像素读数的复现路径）**：五臂 sha = `just shot --name <臂>` + `sha256sum`，
配置走 `RGL_CONFIG_DIR`（`tools/smoke.py` 的 staging，不碰配置副本）。**但 §4 的参数扫描
（太阳角 0°/0.53°/5°、搜索/半影上限 16/32→64/64、覆盖率与带核比）用的是一次性脚本，未入库**
——这正是 CPU 线踩过的坑（量债-02 的脚本没进仓库 ⟹ 重测基线的成本 = 重写采集层）。
**所以 N6d 的第一步必须是把这条判据落成 imgqa 子命令**（§5 的"判据先行"不是流程洁癖：
没有它，F1/F2 修完只能靠肉眼说"看起来软了"）。

**剩余**：债-16②（两套执行世界观，"收进图"已否决 → 只剩清晰度）· 债-16⑤ 另两半（pass 名
`shadow/shadow1..3` vs 段名 `shadow0..3` 的编号口径、`light.frag` vs `model_light.frag`）·
N6d EVSSM 修复批（要改着色器，**需用户放行**）。

## 6.1 N6d 步 0 已落地：`imgqa shadow-softness`（2026-10-09）

§6 那条负账（判据是一次性脚本）已清。命令**零 flag**（场景/臂/帧号/阈值都是量具的组成部分，
**能调 = 能调绿**），首跑：J1 PASS 1.928% · **J2 FAIL 0.013%（预期红 = 如实报告缺陷）** ·
J3 出数 0.599 / 0.207 · 确定性 0 px。**rc=1 是现状的正确结果**，别把它当工具坏了。

两处值得记的设计事实：

- **量具自证用 PCSS 不用 EVSSM**（§5.1）——原设计是循环论证，子 agent 的 BLOCKED 报告把它逼了出来。
- **`shadow_rig` 结构性量不出半影**（§4.1）——它为了跨级联把包围球撑到 radius≈831m，
  `texelWorld` 0.29~2.63 m ⟹ 任何档 `penumbraTexels < 1`。**别为了量半影去改它**，
  它的既有判据（`faces` / `switch` / `attribution`）不受影响。

主 agent 复核时改回一处：覆写器的写后自检原用「全文搜 `on = true` 判旧值离场」——今天能过
只因为注释里恰好没这个字面量，改成**读回解析后的值**（`topLevelValueOf`），并补
`selftest` 面 D 注释陷阱靶（变异实测：换回旧搜法 → 2 条红）。
