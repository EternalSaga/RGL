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

> ⚠ **第三项已于 2026-10-10 反转**（用户放行换红线）：`izb` 默认 **false**。理由 = 「承认出厂开」
> 只是**记录现状**、不是质量裁决，而实测出两个结构性缺陷（§7）。前两项不受影响。
> 正交形状（红线 1）**保留**——形状判断与"交付该不该同时开"是两件事。

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

1. **IZB 不进枚举**。「IZB 近场细影 + 远场软影」混接（`evssm_plan.md` 11.6e）要求两者共存
   ⟹ 枚举管**软影滤波轴**，`izb` 管**硬影 mask**，两条轴正交。`izb = true × mode = "none"`
   由 gate_lint error（与债-32 那条同形同判据）。⚠ 正交是**形状**上的判断，**不是**"交付该同时
   开"的裁决——2026-10-10 起交付 `izb = false`（§7），`izb = true` 会触发 gate_lint warn。
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
| 交付（CSM + IZB） | `shadow_pass=true, izb_shadow=true` | `mode="csm", izb=true` | `fdece9d0` ⚠ 已退役，见下 |
| 无影 | `shadow_pass=false` | `mode="none"` | `121e7502` |
| CSM 单独 | `shadow_pass=true, izb_shadow=false` | `mode="csm", izb=false` | `52231a84` |
| PCSS | `shadow_pcss=true, izb_shadow=false` | `mode="pcss"` | `da06ca00` |
| EVSSM | `shadow_evssm=true, izb_shadow=false` | `mode="evssm", izb=false` | `1a6757f3` |

**五臂复现（2026-10-09 N6b 改完实测，同机 8060S）**：`fdece9d0` / `121e7502` / `52231a84` /
`da06ca00` / `1a6757f3` —— **逐臂逐位相同**。量法 = 备份 `configs/shadow.lua` → 逐臂改写 →
`just shot --name <臂>` → `sha256sum build/rgl/screenshots/<臂>.png` → trap 还原。

**N6d 改完复现（2026-10-09 通宵，同机同帧）**：`fdece9d0` / `121e7502` / `52231a84` / `da06ca00`
**逐位不变** + EVSSM 臂 **`1a6757f3` → `4d19d128`（故意变）** —— 变的只有 EVSSM：STEP 5 逐抽头
评估 + `min_footprint_texels = 16`。四臂不变就是"没碰交付路径"的硬证据，脚本
`/tmp/opencode/five_arms.py`（本轮未入库，锚点表在此）。

**换红线（2026-10-10，交付关 IZB，用户放行）——现行红线 = `52231a84`**：

| 形态 | 配置 | sha256 前缀 | 说明 |
|---|---|---|---|
| **交付** | `mode="csm", izb=false` | **`52231a84`** | **= 旧「CSM 单独」臂，逐位相同** |
| 无影 | `mode="none"` | `121e7502` | 不变 |
| CSM 单独 | `mode="csm", izb=false` | `52231a84` | 与交付**同一形态**（本来就该相等） |
| PCSS | `mode="pcss"` | `da06ca00` | 不变 |
| EVSSM | `mode="evssm"` | `4d19d128` | 不变 |

**为什么"交付 == CSM 单独"是判据而不是巧合**：这两臂是**同一形态**，所以它们必须逐位相同；
跑出来真相同 ⟹ `izb = false` 之后**什么都不剩**（mask 缓冲不构造、light 条目不选
`IZB_CONSUME` 变体、shadow0 段不再重放）。反过来，只要差 1 个 bit，就说明关掉的 IZB 还有一条
副作用没关干净——那才是要查的东西。旧交付锚 `fdece9d0` 就此退役，**别再拿它对拍**。

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

### 4.2 三实验归因（2026-10-09，主 agent 亲跑；**推翻 §4 的成因结论**）

§4 那条「零方差 ⟹ 只交付接触影」**方向是错的**。复核：`chebyshevUpperBound` 返回的是**受光量**，
零方差时 `pMax → ε/(ε+1) ≈ 0` 落在 LBR 之下 = **判全暗**。所以零方差的症状本该是"过度投影 +
刀锐边缘"，可实测覆盖率反而**低于** CSM ⟹ 丢影另有主因。三个单点变异实验（每个只改一项，
`just imgqa shadow-softness` 当量具）：

| 实验 | 覆盖率 | 带核比 | 读数 |
|---|---|---|---|
| 基线 | 5.89% | 0.201 | — |
| **去快路**（`depth <= moments.x → 1.0` 废掉） | **16.92%** | 1.885 | 读图：整片**朝光**屋顶与墙顶糊影 = 教科书自遮挡 |
| **单抽头**（5 抽头塌成 1 个，r=0） | 5.89% | 0.201 | 与基线**逐位相同**，且太阳角敏感度变成**恰好 0.000%** |

**结论：两个相反的失效同一个根 = 矩的滤波 footprint 跨深度断崖被污染。**

- 污染把 **μ 抬到接收面之上** ⟹ 快路判全亮 = **漏光**（现状：丢 40% 影，读图确认全部贴在
  剪影/细碎结构上——屋顶轮廓、树冠、墙顶、小构件；大片平坦影区两边一致）。
- 污染把 **Var 抬到天文数字** ⟹ Cantelli 值 `Var/(Var+d²)` 塌到 0 = **痤疮**（去快路后糊 7.8% 屏幕）。

所以**快路是数学必需的**，不是可删的优化：只有两个矩时，接收面落在 footprint 均值之前，
`P(z ≥ d)` 的上界**无法**由前两阶矩给出（Cantelli 只界住均值右侧的尾）。快路只是在两种错误里
选了较轻的一种。⟹ 修法不是"改快路"，是**让 footprint 别跨断崖**。

**单抽头实验的另一半**：太阳角敏感度恰好 0.000% ⟹ **mip 链在出厂太阳角 0.53° 下从未启用**
（`penumbraTexels < 1` ⟹ `idealLod = −1` ⟹ `targetLod = 0`），唯一的"半影"效果就是抽头半径。
这独立确证了 §4 成因②，并否掉了"先补方差内容"这条路——**核根本没张开，补什么内容都没用**。

**判据因此补一条痤疮天花板**（J3 从"只出数"改成两侧有界）：上限 1.30，**参照 = 实测痤疮变异
1.720**，不是拍的。没有它，「覆盖率 ≥0.9」会被痤疮直接刷绿——判据的靶子交给它要抓的失效。
两侧都验过：基线 0.599 绿、痤疮变异 1.720 红。

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
| J2 | **接线判据**：`sunSens(evssm, 0→5°)` 的 **any-diff ≥ 0.02%** | **0.082%**（756 px）✅ |
| J3 | **对账，两侧有界**：覆盖率比 ∈[0.70, 1.30]、带核比 ≥0.9（转正门槛 0.9 另列） | **0.736 / 1.000** ✅ |

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

**J2 的口径演变**（本轮改的，理由全文见 §5.2 的判据表）：原设计比 0.53°（"用户看到的就是它"），
实测发现出厂角下 0°↔0.53° **逐位相同** —— 核宽下限（G2，软度的必要来源）必然压住 0.53° 的物理
半影。原判据因此是在要求物理上不存在的软度，**作废重定**为 0→5° 的 any-diff 接线判据。

修法（§4.2 归因后排的序 → **N6d 已落地，结论在下面的 §5.2**；原 F1~F4 作废理由附后）：

- ~~**G1（主修）矩 footprint 的深度一致性闸门**~~ —— **实测净负作用，已删**。实现过（抽点软深度
  相对中心抽点降权/剔除，中心恒保留），扫参 0/1/2/4/8/16 全档：foot=32 时覆盖率比从 **0.845 掉到
  0.526**（gate=2）/ 0.709（gate=8）。原因 = **被闸门剔掉的抽点恰恰是能提供遮挡的那些**：
  剪影处"深度不一致"是**正确证据**，不是噪声。留此段防止有人再去走。
- **G2（软度）滤波核下限** —— **已落地 = `evssm.min_footprint_texels`，默认 16**。出厂 0.53° 下
  mip 链从不参与（§4.2 单抽头实验），软度不可能来自物理半影 ⟹ 必须显式给核下限。
- **G3（次要）方差地板按深度不确定度** —— **实测不是杠杆，未做**。ε 从 0.002 扫到 1e-5：覆盖率比
  0.845 → 0.848（噪声级）。LBR 从 0.1 改 0 反而掉到 0.798。死区闭式（`3√ε/c` = 归一化深度 1.24%）
  算得对，但它**不是当前瓶颈**。
- **N1（真正的修复，本轮新发现）逐抽头评估再平均** —— 见 §5.2，这是唯一把覆盖率从 0.60 抬起来的改动。

### 5.2 N6d 落地实况与**转正不成立**的终局裁决（2026-10-09 通宵，全部实测）

**归因链走到最后推翻了自己两次**，最终由两个**上界探针**定死，不是由推导定死：

| 实验 | 覆盖率比 | 带核比 | 结论 |
|---|---|---|---|
| 基线（改造前） | 0.599 | 0.209 | — |
| G1 一致性闸门（foot=4） | 0.579 | 0.206 | **有害/无效** |
| 干净锚点单纹素快路 | 0.598 | 0.206 | 零效果 ⟹ 漏光不在快路那一行 |
| ε = 1e-5 | 0.585 | 0.182 | 零效果 ⟹ 死区不是瓶颈 |
| **阳性对照**：零滤波 + 单纹素纯 EVSM | 0.608 | 0.195 | **缺陷在 Chebyshev 判据本身** |
| **上界探针**：逐抽头判据换成与 CSM 完全相同的裸深度比较 | 0.626 | 0.111 | **Chebyshev 也无罪 ⟹ 是采样结构** |
| **CSM 改单抽头**（对照，不是 EVSSM） | — | **0.152** | **CSM 的"软"就是那 4 抽头 PCF** |

**决定性的一条**：把 CSM 自己改成单抽头，它的覆盖率从 9.84% 掉到 8.02%、带核比从 0.978 塌到
0.152 —— 也就是 **CSM 落到 EVSSM 的位置**。所以"EVSSM 比 CSM 丢 40% 覆盖、硬 5 倍"这句话，
绝大部分是**拿 EVSSM 跟 PCF 的盒式模糊比**，不是 EVSSM 的缺陷。真正的 EVSSM 净损失是
0.783（CSM 单抽头为分母）那一段。

**真正的修复 = N1 逐抽头评估再平均**（`shadow.slang` STEP 5）。原先"先把 5 个抽点的矩平均、
再评估一次 Chebyshev"在跨剪影 footprint 上必然退化（双峰分布 ⟹ Var 爆量级、μ 被远侧抬走 ⟹
上界趋 0 或趋 1，两侧都错）。改成**每个抽点各自评估、再平均受光量** = PCF 的结构。
配套的两条实测教训（都写在代码注释里，别再改回去）：

1. **快路参照必须逐抽点各用自己的矩**，不能共用中心锚点。共用时中心纹素判"接收面在其之前"
   会让 5 个抽点全部返回全亮，核再宽也加不回影（实测 foot 扫到 8 只从 0.598 涨到 0.620）。
   集合对拍实测：CSM 影域里 **37.9%** 的像素正是"中心锚点判在前、CSM 判影"。
2. **核宽是「覆盖 ↔ 不透明度」的单调权衡**，不是越宽越好。权衡表（交付配置 / 第 12 帧 / 比 CSM
   同臂）在 `rgl/shaders/shadow.slang` STEP 2.5 注释，四列 = 覆盖率比 / 黑度比 / 带核比 /
   每影像素深度。**foot=16 是唯一同时贴上 CSM 形态的点**（带核比 1.000、每影像素深度 50 vs 52）。

**终局裁决：EVSSM 转正不成立，封存为 `mode="evssm"` 一档。** 依据（不是意见，是数）：
在"软度与不透明度同时贴上 CSM"的唯一工作点（foot=16），覆盖率比 = **0.736**、黑度比 = **0.708**
—— 即 **EVSSM 的影"和 CSM 一样好，但只有它的七成"**。想再要覆盖就得把影冲淡（foot=32：覆盖率比
0.845 但黑度比 0.608）。而它付出的常驻成本是：矩 compute pass + 8 级 mip 链 + RGBA16F 图集。
**结构性原因**：EVSSM 唯一的 O(1) 宽滤波机制是矩 mip 链，而盒式平均的指数矩跨深度断崖是空洞的
（双峰 ⟹ Chebyshev 上界退化）；绕开它的办法（逐抽头 + 大核）就是在重造 PCF，只是更贵。
CSM 之所以对，正因为它**永远只采 mip0**（深度图不能做盒平均）。

**判据随之重定（不是放宽，是换靶子）**：

| 判据 | 旧 | 新 | 为什么 |
|---|---|---|---|
| J2 | `sunSens(evssm, 0→0.53°) ≥ 0.5%`（>8LSB） | `sunSens(evssm, 0→5°)` 的 **any-diff ≥ 0.02%** | 出厂角下 0°↔0.53° **逐位相同**（核下限 16 纹素 ≫ 0.53° 物理半影）⟹ 原判据的靶子是**物理上不存在的软度**。5° 实测 756 px any-diff / 0 px >8LSB = 接线但不可见；可见性交给 J3 带核比 |
| J3 | 只判痤疮上限 1.30 | 覆盖率比 **∈[0.70, 1.30]**、带核比 **≥0.9** | 下限 0.70 = 工作点 0.736 的 0.95 倍、把改造前 0.599 判红（变异实测：核宽设 0 → 0.600/0.208 红）。**0.9 是转正门槛不是判据线**——钉在够不到的数 = 永久红灯 = 没有信号的红灯 |

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

## 7. IZB 交付关闭的记录（2026-10-10，两个结构性缺陷 + 复现配方）

> 触发：用户透露「IZB 的覆盖与移动一直有问题，只在 bistro 默认机位向中心移动时察觉」。
> 静态交付图里**肉眼看不出**，所以它出厂开了很久。

**缺陷① 随相机在世界表面爬行的接缝（结构性）**。`izb_create.comp.slang` 头注自陈「v1 只覆盖
split0 范围 = 近场 IZB，HFTS 分区留阶段 2」⟹ IZB 只管 **cascade-0 光空间盒**，盒外由
`light.frag.slang` 的 `izbShadowFactor` 回落 `sampleShadow1Tap`（CSM 4 抽头 PCF）。**两套算法在
盒边界两侧给出不同答案**，边界因此成了一条画在世界表面上的线：相机一动，线就在地上爬。

- 量法 = `cam_sweep`（沿 rig 机位插值 t=0→0.8，逐 t 与同机位 CSM 臂对拍）：判反像素
  t=0.00 **少投 13405 / 多投 2791 = 占屏 1.76%**、t=0.40 0.82%、t=0.70 0.70%。
- 反差 = 判反像素亮度差**中位 16–19 LSB**、p95 41–75、max 131–160（8 LSB 是本仓"刚好可觉"口径）。
- 形态 = 最长连续游程仅 15–61 px ⟹ 断开的中等反差线，不是干净直线（所以静态图里看不出来）。

**复现配方（三色哨兵，一步定死，比读代码快）**：临时把 `izbShadowFactor` 改成「盒内
`return 0.0` / 盒外 `return 1.0`」再截图——黑白边界与 izbdiff 的蓝色裂缝**完全重合**（默认机位
下是 x≈920 贯穿全高的竖线带顶折角）。这条配方是 skill `rgl-verify` 阴影验收第 3 条
（三色哨兵 > 数值日志 > 推导）的又一次兑现。

**缺陷② 管辖范围内系统性欠遮挡**。`just imgqa izb` 自己打印的「变亮（CSM 遮而 IZB 放）」=
**405048 px = 占屏 44.0%**（rig，2026-10-10 复现逐位同旧读数）。此前它被登记成 INFO 放过去，
理由写的是「v1 近场语义属预期」——**判据记录了缺陷又放它过**。三条硬子判据只测 mask↔画面
**寻址一致性**，对"遮得够不够"完全失明。现已把定性写回 `imgqa izb` 的输出与判据注册文本。

**为什么"关掉"而不是"修"**：三种修法（关 / 盒边界羽化 / 扩到全级联）都会改交付图。羽化只把
硬跳变摊成渐变，爬行不消失、欠遮挡原样保留；扩到全级联会把 44% 的欠遮挡从近场铺满全屏
（覆盖更差），且是数小时高风险改动。关掉是**测出来的**：CSM 单独一档已被 `imgqa swim` 证明
影场世界不变（零位移失配 **0/63835**），且 `light.frag.slang` 头注记着「同一太阳下 IZB 遮挡集
⊆ CSM 遮挡集」⟹ CSM 单独作源不丢遮挡（它遮的是超集）、只丢那条缝。

**护栏**：`gate_lint` 对 `izb = true` 出 **warn**，消息里带上面两个读数 + 本节指针（实测启动
日志会打出来）。IZB 代码路径**一律保留**（`mode` 与 `izb` 正交，开回来仍可用），warn 的作用是
让"顺手打开"的人先看见数。落点：`rgl/shadow_config.cppm`（默认值 + 整段理由注释）、
`configs/shadow.lua`、`rgl/gate_lint_impl.cppm`。

**顺带结案一条旧悬案**：`imgqa faces --shadow` 长期 FAIL（观测簇 0.1273/0.1936 vs 预测
0.2376/0.2885）**与 IZB 无关**——faces 走合成 profile，`ProfileOpts::izbShadow` 恒 false。
真因 = **闭式解只含直接光项**（`predictFaces` 无阴影项）。同机位两跑实测：`--shadow` 关时观测簇
0.2214/0.2904 ≈ 预测 +Z 0.2221/+Y 0.2885（**PASS**），开时同两簇各 ×0.575 / ×0.667——两簇系数
不同 = 逐像素阴影因子，正是模型缺的那一项。靶子里没有这一项 ⟹ **作废求值**（改成 SKIP，
`--env` 同口径统一），不是调阈值凑绿。常亮的红灯等于没有红。
