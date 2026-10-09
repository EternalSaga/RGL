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

**四臂 sha 对账表**（2026-10-09 改造前实测，deferred / 1280×720 / 第 8 帧 / 本机 8060S）——
N6b 改完必须逐臂复现，这是「重构没动语义」的硬锚点，比「看着没坏」强得多：

| 形态 | 旧配置 | 新配置 | 截图 sha256 前缀 |
|---|---|---|---|
| 交付（CSM + IZB） | `shadow_pass=true, izb_shadow=true` | `mode="csm", izb=true` | `fdece9d0` |
| 无影 | `shadow_pass=false` | `mode="none"` | `121e7502` |
| CSM 单独 | `shadow_pass=true, izb_shadow=false` | `mode="csm", izb=false` | `52231a84` |
| PCSS | `shadow_pcss=true, izb_shadow=false` | `mode="pcss"` | `da06ca00` |
| EVSSM | `shadow_evssm=true, izb_shadow=false` | `mode="evssm"` | `1a6757f3` |

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

## 5. N6d 修复批的候选与前置（本批不做，登记勿丢）

**判据先行**（否则修完不知道有没有修好）：先给 imgqa 加一条 `shadow-softness` 判据
（覆盖率 / 平均变暗 / 带核比 / **太阳角敏感度**四数；阳性锚点 = 现行 EVSSM 的退化指纹
「sun=0 与 sun=0.53 差异 < 0.2% 像素」必须被它抓到）。同时把 §4 的闭式上限落成
`tests/test_evssm_math.cppm` 的用例（零方差矩 + 深度偏移 → 受光量必须 ≈1）。

修法候选（按性价比）：

- **F1 方差地板按深度不确定度给**（不是恒 `ε·pw²`）：σ 由图集纹素对应的世界深度不确定度闭式算出。
  直接补 mip0 零方差这个洞。
- **F2 矩 compute 补有限差分解析方差**（推翻 §4 那条前提后成立），Filament 的解析方差项逐式转写。
- **F3 半影下限 = 1 纹素**：`penumbraSquashed = max(penumbra, k)`，保证至少走宽核；**单独做无意义**，
  必须与 F1/F2 同批（否则只是把 razor-sharp 测试搬到更粗的 mip 上）。
- **F4 调小指数 c**：让指数矩相对带宽变宽，代价是 fp16 下的精度余量与既有 5.5 的定案依据。

**转正裁决的前置**：先解释「为什么远场软影反而更硬」——§4 已给出机制答案，剩下的问题是
F1/F2 修完之后覆盖率与带核比能否**同时**不劣于 CSM。
