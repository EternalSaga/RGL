# Phase 15 · 屏幕空间反射（含湿面输入面）

> **状态**：现行（2026-10-10 立项）。承接 `docs/phase14_plan.md`（时域线换轨 FSR；其剩余
> 14.7 旋钮调参 / 14.8 VMA 裁决 / 14.9 判据迁移三批**不阻塞本 phase**，作为可插队小账保留）。
>
> **一句话**：deferred 光照只有 IBL 一个反射源，湿面/水面/金属因此只能反射"天空"，反射不出
> **场景内几何**——本 phase 补上屏幕空间这一路反射源，并顺手把湿面从"只加高光不压暗"推到
> 该有的形态。
>
> **为什么现在做**：① 湿面 roughness 已被实测证明进入平台期（§1.2），继续调材质参数是零收益；
> ② 全屏 pass 在本机极便宜（§1.1，tonemap 21µs），帧时不是这件事的瓶颈；③ 反射是本项目
> 少数**能闭式验证**的特性（平面镜倒影位置是几何闭式解），判据档次高、不容易做成"看着像"。

---

## 0. 已定案的前提（拍板记录，别在实现中途重开）

### 0.1 D1 对等豁免：SSR 只走 deferred

**裁决（2026-10-10 用户拍板）**：SSR 是 deferred-only，forward 路径不跟。

理由：交付默认已是 `variant = "deferred"`（`configs/render_graph.lua` 顶层 `variant`）；
`tests/` 里**没有** D1 对等的自动护栏，只有 `tools/qa_scenes.py` 的 `d1_parity` 手工靶；
AO 当年选 depth-only 是**主动兼容**（开阔地面 AO 本来也不需要法线）而不是被红线逼的。

**待同步的旧措辞**（这三处仍把 D1 当硬约束，改完本 phase 才不会有人再查一遍）：
`docs/phase11_plan.md` 的 §2.5「SSR 属后处理 → deferred-only，破 D1 对等」与 §3 决策表
「AO 若走后处理会破 D1 对等」、`docs/next_roadmap.md` 的「D1 两路对等需豁免或 forward 补
G-buffer」。改写口径 = **豁免已给**，不是"待裁决"。另 `rgl/shaders/ao.frag.slang` 头注释
「默认变体是 forward」是过期陈述，就地改。

### 0.2 「成本不设上限」的准确口径

**裁决（同上）**：本 phase 不以 GPU 预算为砍功能的闸门，先看画质。

但这条**不等于不测成本**——本项目最贵的教训就是"没有数就动手"（`docs/还债总账.md` 主轴）。
落地口径固定为：**每批交付照出 per-pass 读数并写进本文 §1.1 的表**，画质判据决定做不做，
成本读数只决定下一批往哪个方向省。读数姿势 = `diagnostics.lua` 的 `profiler = N`（GPU 时间戳
按编译图逐 pass 报），**单实例串行**（skill `rgl-verify` 的测量纪律）。

### 0.3 对 phase11「SSR 非必要」裁决的处置：**收窄，不推翻**

`docs/phase11_plan.md` §2.4 判「反射源（planar reflection / SSR）**降级为非必要**」，依据 =
官方 `Bistro_Exterior_1` 湿地 RGB 饱和度仅 14（近灰白）→ 主要反射明亮天空，IBL 原理上已覆盖。

它**在自己的目标下成立**（复刻官方那一张机位图），但和同文件另外两条实测放在一起，边界就出来了：

| 实测 | 出处 | 含义 |
|---|---|---|
| 湿地"水感"100% 来自 IBL 项（env=0 时 p50 掉到 54 且完全失去水感） | `phase11_plan.md` §2.2 | IBL 供的是**亮度** |
| roughness 0.221 / 0.120 / 0.059 三档，路面带 p90 = 197.0 / 197.3 / 197.0 | 同文件 §8.5 | IBL 供不了**结构** |
| 官方参考图是 Falcor **路径追踪**产物 | 同文件 §2.3 | 图里有建筑/船的镜面倒影，cubemap 给不出来 |

⟹ 准确表述：**"复刻官方那一张图"不需要 SSR，"让湿面/水面在任意机位像水"需要 SSR**。
前者已结案，后者全仓零实现零 spike（grep 仅命中文档裁决句）。本 phase 做后者。

---

## 1. 事实基线（本机核实，作用是别再重查一遍）

### 1.1 GPU 帧时间构成（2026-10-10 实测）

机器 = RADV STRIX_HALO（AMD Radeon 8060S）· 1280×720 · `variant=deferred` · `shadow.mode=csm`
· `profiler=120` 帧窗口 · 单实例。

| GPU pass | mean ms | 占 `gpu.frame` |
|---|---|---|
| `gpu.frame` | 9.158 | 100% |
| `geometry`（G-buffer，2.8M 三角 / 1591 primitive） | 2.329 | 25.4% |
| `shadow` + `shadow1..3`（CSM 4 级合计） | **6.545** | **71.4%** |
| `light`（deferred 逐灯全屏） | 0.151 | 1.6% |
| `ao` | 0.095 | 1.0% |
| `tonemap` | 0.021 | 0.2% |

**三条推论**：① 全屏 pass 在本机是 21µs–151µs 量级，"再加一个全屏 pass"这件事本身几乎免费，
成本全在**每像素采样次数**上（ray march 的步数）；② 帧时的大头是阴影（71%），真要腾预算那边
有整片可减的地方，不该拿"帧时不够"当不做的理由；③ `geometry` 2.329ms 是**重画一遍场景**的
价格——这条直接决定 §1.3 里 Filament 的 pass 形态为什么不能抄。

⚠ 本表是 profiler 在场、`shadow.mode=csm` 档的读数，**绑机绑档**；跨机/跨档先复测再比。

### 1.2 湿面平台期（已有实测，本 phase 的立项依据）

`docs/phase11_plan.md` §8.5：`_Wet` 材质 effective roughness 0.221 / 0.120 / 0.059 三档，
路面带 p90 = 197.0 / 197.3 / 197.0 —— **0.12 就到顶**。根因原文：「因为没有 SSR，能反射的只有
IBL 高 mip（近似均匀天空辉光），再锐化也反射不出新信息」。

同节另记一条残留偏差：真实湿面会同时压暗漫反射（水填进微观凹凸），这里只加高光不压暗
（AGENTS.md「已知问题」挂着这条），本 phase 批 15.2 结案它。

### 1.3 抄作业：Filament SSR 的可抄面与不可抄面

作业本 = `/home/robin/program/filament` @ `eead6376`。它的 SSR 本体在
`shaders/src/surface_light_reflections.fs`（264 行），pass 编排在
`filament/src/PostProcessManager.cpp::ssr`，参数面在 `filament/include/filament/Options.h`
的 `ScreenSpaceReflectionsOptions`。

**先说它的形态（这一步读错，后面全错）**：Filament 的 SSR **不是后处理 pass**，而是一个
**额外的 mesh render pass**——`Variant::SPECIAL_SSR_VARIANT` 把"声明了 SCREEN_SPACE 反射模式的
物体"**重新画一遍**到 reflections buffer（RGBA16F，premultiplied alpha），ray march 在
structure（深度）图上找命中，命中后从 **`sampler0_ssr` = 上一帧的 SSR 历史**取颜色。

| 维度 | Filament | 本项目 | 判定 |
|---|---|---|---|
| 反射面属性来源 | 重画一遍物体 ⟹ 有精确 position/normal/roughness | **读 G-buffer**（5 通道全 `store`，零重画） | ❌ 形态不可抄 |
| 重画的价格 | 单模型小场景，可接受 | 1591 primitive / 2.8M 三角 = 再一遍 `geometry` = **+2.329ms** | ❌ 同上 |
| 取色源 | 上一帧 SSR 历史（**时域**，因为它在场景渲染中途，当前帧颜色还没画完） | `light` pass 之后 `scene_hdr` 已完整 ⟹ **可用当前帧** | ❌ 不抄，我们的形态更强 |
| 与阴影 | **构造互斥**（`assert_invariant(!isSSRVariant \|\| !hasShadowing)`） | 阴影占 71% 帧时且必须保留；我们的形态天然不冲突 | ❌ 不抄 |
| 分辨率 | 全渲染分辨率（`svp.width/height`） | 起步全分辨率，15.3 再拆半分辨率 | ✅ 同 |
| 粗糙度 | reflections buffer 走 **mip chain**（2D array：layer0=refraction / layer1=reflection，`roughnessLodCount` 级），premultiplied alpha 就是为了能被 mipmap | 同思路 | ✅ 抄 |
| ray march 算法 | **McGuire & Mara, Efficient GPU Screen-Space Ray Tracing, JCGT 2014** 的 DDA（permute / stride / jitter / 半像素深度区间） | 同 | ✅ 抄（见许可） |
| 三个 fade | 边缘 `fadeRateEdge=12`、距离 `fadeRateDistance=4`、**朝向相机 fade** `1-max(0, vsDirection.z)`（背面伪命中） | 三条都要，公式照抄 | ✅ 抄 |
| 抖动 | interleaved gradient noise + stride | 同 | ✅ 抄 |
| 参数四分法 | `thickness`（世界）/ `bias`（世界）/ `maxDistance`（世界）/ `stride`（texel） | 同口径命名，**数值一个不抄** | ✅ 语义抄，数值重标定 |

**⚠ 红线：世界单位参数一个都不许抄。** Filament 默认 `maxDistance=3.0` / `thickness=0.1` /
`bias=0.01` 是**米制单模型场景**的标定；本项目 bistro 是 `scale = 0.01` 的世界（包围球 r≈1.17），
照抄等于把射线长度调成"比整个场景还长"。参数必须按我们的场景尺度重标定，且**每个参数要有一张
扫描表 + 一次消融证明它真的在动**（阳性锚点，skill `rgl-test-discipline`）。

**⚠ 第二条红线：`maxSteps` 不许抄。** Filament 写 `maxSteps = max(res.x, res.y)`——移动端小
分辨率下的宽松上界。我们 1280×720 桌面，最坏 1280 步/像素不可接受，必须自己限死并用 stride
加速，且这个上界要**可配**（它是画质/成本的主旋钮）。

**许可**：McGuire-Mara 的 DDA 在 Filament 里带 BSD-2-clause 声明（Filament 本体 Apache-2.0）。
本项目**照论文自己实现**，不逐字搬 Filament 的移植；若最终逐字复用其函数体，BSD-2 声明必须
随文件保留（先例 = `third_party/fsr/PATCHES.md` 的 vendor 交代纪律）。

**"Filament 是单模型小场景移动端渲染器"这条心理阴影的具体落点**（下次抄它任何东西都先过一遍）：
① 任何"重画一遍"的方案在它那里便宜、在我们这里贵一个量级；② 它的世界单位是米；
③ 它的屏幕外内容少 ⟹ SSR 未命中的回退质量不重要，我们的大场景**未命中率会很高**，
**回退（与 IBL 混合）的质量比命中的质量更决定观感**；④ 它与阴影构造互斥，我们不许继承这条。

---

## 2. 架构落点（代码指针，逐条复核过本体）

### 2.1 形态 = post-lighting 全屏 pass（这是唯一无环的形态）

`light` pass 写 `scene_hdr`（`rgl/render_graph_builtin.cppm` 的 `deferredRenderGraphDesc`），
所以 `light` **不可能**读同帧的 SSR 结果——图内成环。ray march `scene_hdr` 是唯一无环形态，
也正好是工业主流（Filament 用历史取色是它的 pass 位置逼的，不是算法要求）。

落点 = 一个 `drawLight` 档全屏 pass `ssr`：读 `scene_hdr` + G-buffer（position/normal/metalrough）
+ depth → 写 `ssr_applied`（= `scene_hdr` + 反射增量），消费者（`tonemap`）的色输入换成
`ssr_applied`。

现成模板两处（**照抄这两个，别自己发明**）：
- `decorateVelocityPass`（`rgl/render_graph_decorate.cppm`）= 纯增量装饰器，门关 = 装饰器不被
  调用 = 图里根本没有这个 pass，**结构性**旁路（不是运行时分支）。
- `decorateAoApplyPass`（同文件）= 插 pass + 把消费者输入换成新附件。

换烘焙槽那一处 = `rgl/main_scene.cppm` 的色输入优先级链（现 `FSR > ao_applied > taa_out >
scene_hdr`），加一档。**图上的输入名改写只管 barrier，真正把"读哪张图"换掉的是换烘进管线条目
的堆槽**——两条通道必须同时换，漏一条 = 采到没转布局的图（该文件注释已写死这条教训）。

G-buffer 在 `light` 之后仍可读：5 通道全 `store`（`render_graph_builtin.cppm` 的 geometry
outputs / light inputs 均 `RgStoreOp::store`）。世界坐标重建函数现成：`clipToWorld` /
`worldFromDepth` / `uvFromClip` / `uvFromWorld` / `mulM`（`rgl/shaders/scene.slang` 的
「屏幕 ↔ 世界重建」节，批 14.4 单一来源），消费者先例 ao / taa / velocity。

### 2.2 缺的那颗矩阵：本帧 viewProj 要进 scene data 尾区

**scene data 里没有正向的本帧 viewProj**——只有 @64 的 `skyboxInvViewProj` = inverse(proj·view)。
SSR 要把 ray march 的命中点投回屏幕 uv，必须有正向矩阵。

两条路：shader 里对 @64 求逆（每像素一次 4×4 逆，且 `inverse(inverse(M))` 不保证逐位等于 M，
uv 精度直接进采样位置）／**追加尾区放本帧抖动后 proj·view**。选后者，因为它是项目惯例
（先例 = 批 11.2 阴影光空间 VP 区、批 12.6 `readTaaPrevViewProj` @722080，全部"追加区、
既有偏移逐字节不动 diff0"），且 CPU 侧写正向矩阵**可做机器可验护栏**：

> **护栏（TEST）**：新格与 @64 严格互逆——CPU 侧断言 `newSlot == inverse(skyInvViewProj)`。
> 这条断言同时管住"抖动口径"：两者必须同为抖动后（与深度缓冲配套），错一个口径 SSR 的采样
> 位置会整体偏移而画面"看着还行"，正是本项目反复踩的静默漂移形态。

口径红线（`scene.slang` 已写死，SSR 必须遵守）：`ndc.y = 1 − uv.y·2`、Vulkan 深度 [0,1] 直用、
矩阵是**抖动后** invViewProj。

### 2.3 加一个 `drawLight` 档 pass 的同步面

词表与管线的必须同步位置以 `rgl/pass_vocabulary.cppm` 头注释 + `tests/test_render_graph.cppm`
写死的名单为准（**本文不抄清单**——抄一次漂一次，AGENTS 的抄名单反模式）。要点：两档词表
（`meshPass` / `fullscreenPass`）**不可并成扁平表**；StateKit 有 `static_assert` 拦漏实现；
shader 构建登记在 `rgl/CMakeLists.txt` 的 `rgl_shader(...)`。

### 2.4 湿面 wetness **不进** scene data（修正 phase11 的旧前提）

`docs/phase11_plan.md` §8.5 写「补它需要 per-material 的 wetness 分量进 scene data
（draw data schema 扩列 = 反射管线改动），列 Phase 12 候选」。**这条前提可以绕开**，
是本 phase 调研的实质产出之一。

走 ObjectDraw 扩列的真实代价：stride 48 → 64，而 draw 表尾**推导**出辐照度区基址
（`rgl/scene_data.cppm` 的 `kSceneDataIrradianceOffset`），`rgl/shaders/scene.slang` 与
`rgl/shaders/light.frag.slang` 里 `721552 / 721696 / 721952 / 722016 / 722256` 一串手写
字面量全部后移（shader 侧无编译期反射通道，靠 `static_assert` + 纪律同步）。爆炸半径远大于收益。

改走**程序化 wetness**：世界坐标（G-buffer position 现成）+ 法线朝上程度 + 噪声，在 shader 里
算。这与 phase11 §2.4 已有的裁决同向——那条已把「湿区 albedo 压暗」从"做手工贴图"**改为渲染时
计算**，理由是"压暗应来自法线朝上程度 × AO"。零契约改动、零 `just reflect`。

---

## 3. 批次

| 批 | 内容 | 判据 / 红线 |
|---|---|---|
| **15.0** | **判据与靶先行（零渲染改动）** ✅ **已落地**（落地形态与计划的偏差见 §3.1） | 红线 = `just regress ab` 逐位不变（**已实测：逐位相同**）；判据 = 闭式预测表本身要有**反向用例**（柱不存在时预测区间必须为空，已做成 `ssr_rig_empty` 变体 + 构建期断言 A3） |
| **15.1** | **镜面 SSR 最小可用**（**前半 ✅ 已落地** = `ssr` pass 管道 + 门 + 占位拷贝，落地记录与两个已知缺口 → §3.2；**后半** = 下面的 ray march 本体）：`ssr.frag.slang` 全分辨率 DDA ray march（§1.3 算法面），反射源 = `scene_hdr`，权重全从 G-buffer 现成量算（Schlick fresnel 与 `light.frag` 同式、roughness 门控、背面/边界 fade）。§2.2 的 viewProj 尾区 + 互逆护栏。门走 `configs/runtime.lua` 新块，与 `aa` 三态键**正交**。**末尾必做（§6 风险 1 的量具）**：在 bistro 交付机位上出 SSR 开/关差分覆盖率 = 命中率读数，写进 §1.1 表；过低即触发 §6 的降级路径 | 红线 = 门关逐位 = **红线 A `5ff793bf…`**（权威载体 = `docs/还债总账.md` 的红线声明块；**必须用基线配置**。⚠ 本计划立项时写的是 `fdece9d0`，那个锚 2026-10-10 已因债-76 退役，批 15.0 的 `just regress ab` 实测跑出的就是 `5ff793bf`）；VVL + syncval 零 hazard（新 pass 的布局链，先例债-32）；判据 = 15.0 闭式靶（`just ssr-target`，凸包内必须变亮 = 阳性锚点）+ `imgqa switch`（门关影响域**必须恰为 0**）+ `imgqa attribution` + **`aa` 三档各测一次 SSR 开/关差分**与**装配后 `ssr` pass 下标 < `taa` pass 下标**（这两条堵的是 §3.2 那两个当前全裸的缺口，缺一条 = 后半可以静默交付一个"SSR 只在 aa=off 生效"或"SSR 建在时域滤波之后"的东西） |
| **15.2** | **湿面消费 SSR**（= 原课题「bistro 水膜」三项）：湿区 albedo 压暗 ×0.5–0.7、水洼粗糙度（洼 ≈0.05 / 湿石 0.3–0.5）、水膜法线扰动。wetness 走 §2.4 程序化路线 | 结案 AGENTS.md「湿面只加高光不压暗」那条已知边界（就地删该条）；`imgqa faces` 闭式逐面预测；`imgqa switch --allow-piece ground` 限定湿区影响域 |
| **15.3** | **粗糙度感知模糊 + 伪影治理**：拆半分辨率 march + 全分辨率 apply；反射图走 mip chain（§1.3 抄 Filament 这条）；治深度间隙断裂、屏幕边界淡出、firefly（`docs/还债总账.md` 债-40 那条 HDR firefly 对 SSR 同样成立） | 每条伪影要有**反向用例**，不许"看着好了"收账；per-pass 读数进 §1.1 表 |

**批次顺序的理由**：15.0 先做是因为 SSR 是本项目少数能闭式验证的特性，尺子必须比实现先到位
（skill `rgl-test-discipline` 的"程序化靶是判据成立的前提"）。

### 3.1 批 15.0 落地记录（2026-10-10）

**与计划的三处偏差，都是"计划里的做法会破坏既有护栏"**：

| 计划原话 | 实际做法 | 为什么 |
|---|---|---|
| 给 `env_band` rig 加一根柱 | **新造 `ssr_rig` 场景**（`tools/qa_scenes.py` 场景 8） | `env_band` 自己的 **A5 单变量断言**（四 piece 同色板位、同几何、metallic 全 1，只有 roughness 不同）会被加柱直接违反；`nm_rig` 当年是同一个理由另造的靶，先例现成 |
| 预测"倒影**行号区间**" | 预测**倒影区域的世界凸包** + 其屏幕投影 | 闭式解比"行区间"更强：平面 N=+Y 下反射射线是 `Q(t)=(x(1+t), ht, z(1+t))`，令 `s=1+t` 得**水平 = s·(x,z)、高度 = h(s−1)**，于是命中竖直柱 ⟺ `∃s∈[1,1+Hc/h] : s·(x,z) ∈ C`，即 **倒影区 = 柱底面关于相机原点的缩放并集 `⋃_{λ∈[h/(h+Hc),1]} λ·C`**。它与方位角无关、与柱形状无关（矩形时就是 8 角点的凸包），所以是二维掩码而不是一维区间 |
| 「固化一个天空被建筑挡死的 bistro 机位」 | **改成合成靶 + 黑天**（bistro 机位勘察降级为 15.1 的附带项） | 真机位下 SSR 的影响域 = **所有能反射到屏幕内容的镜面像素**，远大于柱倒影区，"掩码外逐位不变"这条因果泄漏恒等式**根本不成立**（反射天空会让整片地面变亮）。黑天合成靶把影响域收敛到恰好等于倒影区，判据信噪比从"统计"升到"闭式" |

**闭式解当场被实测验证**：靶渲出来后柱身实测屏幕行区间 = 闭式预测 `[251.2, 488.7]`
逐位吻合；地面远端（z=−25）实测上边界 row≈291 = 闭式 `360−(8−3.66)×16`。
即 `ssr_dir_to_pixel` 这套投影闭式与引擎的 `glm::perspective` 口径一致——**这是整个判据的地基**。

**判据基线（SSR 关，本机实测）**：

| 区域 | 像素数 | p50 | p95 | 非零占比 |
|---|---|---|---|---|
| 倒影判据区（凸包剥掉柱身） | 40076（4.35% 画面） | 0.0 | **0.0** | 0.47%（柱底边缘，SSR 开关两侧同亮、差分后为 0） |
| 柱身（阳性锚点） | 37708 | 76.0 | 78.3 | **100%** |

可用像素 4.35% 画面（`env_band` 当年只有 49 行）；背景纯黑 ⟹ **SSR 一开就是信噪比 ∞ 的差分**。

**落地产物**：
- `tools/qa_scenes.py` 场景 8 `ssr_rig` / `ssr_rig_empty`（负对照）+ 构建期断言 A0–A5。
  **A0 是闭式实现自身的阳性锚点**：`ssr_dir_to_pixel` 必须是 `nm_ray_dir` 的逆（往返误差
  实测 2.4e-13 px）。A0 不成立时后面所有断言都在自欺——投影写错，"凸包在画面内"毫无意义。
- `qa/configs/ssr_rig/`（靶配置）+ `just ssr-target`（生成靶 + 出图一条命令）。
- `tools/smoke.py --config-base <dir>`：让仓库里的靶配置走**同一套** staging 与
  headless 生效核对，不碰 `configs/` 副本。

**⚠ 本批踩到的引擎事实（写进靶配置头注释，也记债）**：deferred 下天穹**既不看
`environment.skybox` 也不乘 `intensity`**——`light.frag` 的天空分支是
`if (env.mipRange > 0.0) return float4(envSky, 1.0)`，env mip0 **原值直出**。想要黑天
**只能 `mipRange = 0`**，也就是**整段 environment 缺失**；而 `environment` 一缺，
`validateEnvironmentContract` 又要求 styleMap 里不能出现 `model_pbr_ibl` /
`model_vegetation` / `model_light`（它们真会未映射采样 env，这个契约是对的）。
⟹ 靶必须用非 IBL 风格。`skybox = false` 在 deferred 下是**空开关** → 债-80。

**开发期 bug 记录（防止下次重踩同一个"闭式代码看起来完全正确"的坑）**：投影函数喂
**未归一化**方向时，`asin(dot(d,û))` 会撞上 clamp 边界 → `tan(90°)` 发散出 1e16 量级的
假行号，症状是"倒影跑到画面外"，而闭式代码逐行看都对。归一化 + A0 往返断言一起才是防线。

**变异验红抓出了判据自身两个问题**（7 条变异 7/7 变红，但过程比结果有用）：

1. **A0 原本有盲区**：`nm_ray_dir` 返回的本就是单位向量，只拿它做往返 ⟹ **永远测不到**
   「投影函数自己会归一化」这条契约。实测：去掉归一化时原 A0 **原样绿**。补了"喂
   0.001×/7×/1e5× 非单位方向"的第二段才红。
   ⟹ 教训：**阳性锚点要覆盖契约的每一个前提**，"输入恰好满足契约"的自检等于没测。
2. **断言顺序决定判据的可用性**：第一版把"屏幕区间出画"这类粗判据排在前面，于是
   λmin 写倒数这种**几何**错误只报出"出画"，归因要重做。改成「闭式几何 → 屏幕投影 →
   场景装配」的顺序后，同一条变异直接报"倒影点半径 12.79 越过柱底最远角 6.63"。
   顺带一条：**永不触发的断言 = 装饰**，第一版的 A6 被遮蔽到从未触发，靠变异才暴露。
3. 按记忆写"哪条变异该红哪条断言"，**四条里错了三条**。已把表改成实测输出（纪律
   「断言要照实测写，不照记忆写」的又一次实例）。



---

### 3.2 批 15.1 落地记录（进行中）

**已完成子步：SSR 正向矩阵进 scene data 尾区（零渲染改动）**

`kSceneDataSsrOffset = 722272`（= TAA 区尾 722080+192），`kSceneDataSsrBytes = 64`，
`kSceneDataBytes` → **722336**。填值 = `viewProj.proj * viewProj.view`，与 @64
`skyInvViewProj` **同一表达式不取逆**（`fillSsrViewProj` 接缝，12.3d `fillShadowData` 先例）。
消费端 `scene.slang readSsrViewProj()` 已备好，**本步无调用点** ⟹ 红线 = 画面逐位不变（已实测）。

`tests/test_ssr_viewproj.cppm` 两条用例，设计上有三处值得记（都是"否则就是假绿"的形态）：
- **两源结构**：写入侧偏移用 C++ 常量、**读回侧用字面量 722272**（镜像 slang 的 `Load4`）。
  两侧同用一个常量的话，常量整体漂移测试永远绿。
- **刻意不用 identity view**：`view = 单位阵`时 `inverse` 的两种乘法顺序恰好同值（5a-8c 教训），
  转置/顺序错会被完全掩盖。另钉了"矩阵非对称"阳性锚点——对称矩阵下 transpose 判据失去区分力。
- **闭式差量当独立预言机**：抖动注入的期望差量从 `applyProjectionJitter` 的注入式推
  （只动 proj 第 3 列 ⟹ 行 0 差 = `(−2jx/W)·view[c][2]`、行 2/3 逐位等），**不是再算一遍
  `proj·view`**。

**主 agent 质检变异（形态与实现者所验三条不同）**：

| 变异 | 结果 | 证明了什么 |
|---|---|---|
| 乘法顺序颠倒 `view * proj` | 两条用例**全红**（282 断言） | 5a-8c 那条防线真的有区分力 |
| 抖动注入**符号**翻转（`-=`→`+=`） | **只红闭式差量那条**用例（56 断言），(a) 保持绿 | 闭式差量是**独立预言机**，不是同义反复；两条用例分工清晰 |

**⚠ 已知覆盖缺口（别把这块读成"全链已护栏"）**：单测是**复刻** `writeSceneStaging` 的两条
memcpy，不是调用它 ⟹ 若有人把 `vkrenderer_impl.cppm` 里的**目标常量写错**（比如误用
`kSceneDataTaaOffset`），单测**不会红**（常量本身有 static_assert 钉值，但"用哪个常量"没有）。
这与 `test_shadow_forward` 是同一形态的取舍。**真正的端到端护栏 = 本批的闭式反射靶**：
SSR 一旦消费这颗矩阵，`just ssr-target` 的闭式倒影凸包就会把「偏移错 / 口径错 / 提取链错」
一起现形（矩阵错 ⟹ 倒影不出现在预测位置 ⟹ 判据红）。所以 15.1 的接线顺序刻意是
「先有靶、再接线」，不是「先接线再补判据」。

**已完成子步：`ssr` pass 管道 + 占位拷贝 shader（门默认关）**

拆出这一步的唯一理由 = 拿到一条比"门关逐位不变"更强的判据：**占位 shader 零算术
（`Load` 整数纹素原值拷贝），所以「门开 = 门关逐位相同」只可能因**管道**接错而破，不可能因
算法而破**。实测：门关 `5ff793bf…` = 门开 `5ff793bf…`（`cmp` 完全相同），而启动日志
`ssr (RGBA16F, …): 1280x720 slot 8252` 证明 RT 真领了、pass 真跑了 —— 于是"接错"与"算错"
这两种在本批极易混在一起的失效被彻底分开，后半只剩算法一种嫌疑。

落点：`decorateSsrPass`（`decorateAoApplyPass` 同构三件套）插在 shadow 之后、velocity/taa/
ao_apply **之前**；跳过两条 = 无采色上屏消费者 / 图无 `gbuffer_position`（= deferred-only
边界，即 §0.1 的 D1 豁免在代码里的落点）。色输入优先级链成 **`ao_applied > taa_out >
ssr_applied > scene_hdr`**（FSR 仍整条顶掉）。装饰器**可复合性**是这套设计成立的前提：每个
装饰器都现搜「消费者当前的色附件」，SSR 先插 ⟹ 下游自动读到 `ssr_applied` ⟹ 链自己接成
`scene_hdr → ssr_applied → [taa_out | ao_applied] → tonemap`；这条性质有单测钉（含 barrier 成立）。

验收：`just regress ab --expect 5ff793bf` 逐位相同 · 门开/门关两图 SHA 相同 · VVL + syncval
`errors=0` · `--ssr` × `aa` 三档各冒烟 PASS · `--ssr --variant forward` 走"图形态不支持"warn
优雅跳过（D1 边界的运行时证据）· `just test` 260 → **263 例** · style/doc-check 0 警告。

**主 agent 质检变异（形态与实现者所验不同）—— 两条红、两条绿，绿的那两条是缺口**：

| 变异 | 结果 | 含义 |
|---|---|---|
| `ssr` pass 的 `kind` 改成 `drawMesh`（kind 与 state 串味） | **红**（2 例） | 图校验承认可复合性 |
| 跳过条件 2 的谓词取反（forward 边界） | **红**（1 例） | deferred-only 边界有护栏 |
| `main_scene` 里 **taa 条目的色输入退回 `hdrSlot`** | **绿**（275 例全过） | ⚠ 缺口 1 |
| **`decorateSsrOrWarn` 调用挪到装饰序列最后** | **绿**（275 例 + 冒烟 + syncval 全过） | ⚠ 缺口 2 |

**⚠ 两个已确认、当前无任何检查覆盖的缺口（15.1 后半必堵，别当"已护栏"）**：

1. **「烘进管线条目的堆槽」这条通道全裸**。把 `addTaaEntry` 的色输入退回 `hdrSlot`（= 图上写
   `ssr_applied`、描述符指 `scene_hdr`）后：单测全绿、`--ssr --aa taa --syncval` `errors=0`、
   出图正常。原因是两张图都在 `eShaderReadOnlyOptimal`，VVL 无从分辨；而占位拷贝下像素又
   恰好相同，逐位判据也抓不住。**真 shader 下的症状 = SSR 在 `aa=taa` / `aa=fsr` 档完全无效**，
   而 §3 给 15.1 定的判据（闭式靶）只在 `aa=off` 跑 ⟹ 抓不到。
   **后半必加的护栏 = SSR 开/关差分在 `aa` 三档各测一次**：`aa=taa` / `aa=fsr` 两档
   SSR-on 与 SSR-off **必须不同**（不同 = 那条通道的描述符真的指向了 SSR 产物）。
2. **装饰器调用顺序这条不变量只由注释承载**。把 `decorateSsrOrWarn` 挪到 taa/ao_apply 之后，
   图**仍然自洽**（能编译、无 hazard、无 warning）——只是 SSR 落到时域滤波之后（抖动网格不匹配
   + SSR 不被滤波）。差分覆盖率那条护栏**抓不到它**（SSR 仍然生效、只是位置不对）。
   **后半必加的护栏**：把装饰序列收进一个可测函数，对 `aa × ssr` 全组合断言**装配后图里
   `ssr` 的 pass 下标 < `taa` 的 pass 下标**（顺序错了即红）。现在做不到是因为序列内联在
   `compileGraphOrThrow` 里，测试不可达。

## 4. 红线与纪律


- **门关 = 逐位不变**，且必须是**结构性**旁路（装饰器不被调用 = 图里没这个 pass），不是运行时
  分支——运行时分支挡不住 slang 对源码差异的 fp 重排（`light.frag.slang` 的 IZB_CONSUME 注释里
  那条 105401 像素漂移教训）。
- **红线比对算子固定为逐位相同**，不得换 PSNR/SSIM（用户拍板，skill `rgl-verify`）。
- **带时域历史的档不立位比闸门**（本 phase 不做时域 SSR，这条是给 15.3 之后的人）。
- **非默认档必须单独冒烟 + syncval**：SSR 开 + `aa` 三档各一，默认档不覆盖它们。
- **参数数值全部本机重标定**（§1.3 红线），每个旋钮一张扫描表 + 一次消融阳性锚点。

## 5. 明确不做

- **planar reflection**（单平面镜像相机重渲场景）：成本 ≫ SSR（至少再一遍 `geometry` = 2.329ms），
  而它唯一的优势是对 forward/deferred 两路友好——§0.1 豁免之后这个优势不存在了。
- **时域 SSR 去噪**：Phase 13 刚因「可行域为空」冻结自研 TAA（`phase14_plan.md` §0.2），SSR 的
  时域是同一个坑，要做单独立项，不许在 15.x 里顺手加。
- **bloom**：排本 phase 之后。它跟刚接完的 FSR 抢 tonemap 前的位置，`docs/taa_plan.md` §7 那条
  「bloom 须排在 TAA 之后」在新 `aa` 三态键下需要重新裁决；本 phase 会先把"tonemap 之前插 pass +
  换色输入烘焙槽"这条基建趟平，bloom 排后面省一半事。
- **anisotropic SSR**：Filament 的 `getAnisotropyPixelParams` 那条路，我们没有各向异性材质。

## 6. 风险清单（按"会让本计划作废"排序）

1. **未命中率过高**（§1.3 心理阴影③）：大场景屏幕外内容多，若湿面大部分反射落在屏幕外，SSR
   交付的是"大部分像素回退 IBL"，观感提升可能低于预期。**缓解（2026-15.0 改期，见下）** =
   未命中率**在 15.1 末尾直接量**，未命中率过高则本 phase 降级为"只做湿面三项（15.2）+
   结案 SSR 不可行"，这仍是有效结论。
   > **为什么从 15.0 挪到 15.1**（原计划写的是"15.0 先出读数"）：15.0 期间能拿到的只有
   > **CPU 代理**——要么给 bistro 网格写一套 CPU 光线投射（另一个小项目），要么拿
   > "屏幕上半部几何覆盖率"当代理，而 `dump_gbuf` 的 position 通道是 `toRgba8` 线性 clamp，
   > 负坐标与天空**同为纯黑**，连几何掩码都拿不干净（15.0 实测确认）。反过来，15.1 一旦存在，
   > **命中率 = SSR 开/关差分的覆盖率本身**，用本批刚建好的 `--config-base` + `imgqa switch`
   > 零额外成本就能量，且是精确值而非代理。**降级路径不变**，只是量具换到更准的那一个。
2. **ray march 成本失控**：全分辨率 + 步数上限给松了会直接吃掉几 ms。**缓解** = `maxSteps`
   可配 + 15.1 就出 per-pass 读数（§0.2 口径），超预算降 stride/步数而不是砍功能。
3. **深度间隙与背面伪命中**：SSR 的经典伪影。三条 fade 抄齐（§1.3），15.3 专项治理。
4. **与 FSR/TAA 的相互作用**：SSR 读 `scene_hdr`（抖动网格），`aa=fsr/taa` 时采样位置口径要
   与 §2.2 的互逆护栏一致。**缓解** = 三档各冒烟；护栏是 TEST 不是文档。
5. **`imgqa` 没有现成的 SSR 判据**：`env_band` rig 是现成的**靶**，但"倒影出现在预测区间"这条
   判据要新写。**缓解** = 15.0 整批就是干这件事，判据落进 `tools/imgqa/` 注册表（**不在文档里
   抄判据清单**）。

## 7. 记债继承（本 phase 的账，不自动清偿）

| 号 | 内容 | 触发条件 |
|---|---|---|
| 债-77 | SSR 时域去噪（15.3 明确不做的那条） | 空间去噪（15.3）把伪影治到仍不可接受时才开 |
| 债-78 | forward 路径的反射源（§0.1 豁免掉的账） | forward 重新成为交付路径，或有人要拿 forward 出图 |
| 债-79 | bloom 与 FSR 在 tonemap 前的先后裁决（§5 排后的账） | bloom 立项时 |
| 债-80 | **`environment.skybox = false` 在 deferred 下是空开关**：`light.frag` 天空分支只看 `env.mipRange`，既不看这个键也不乘 `intensity`（forward 的 skybox pass 才吃它）。即"deferred 下关不掉天穹"，且配置写了不生效 = 典型的静默门 | 有人要拿 deferred 出无天穹的图，或 gate_lint 扩这一类"键写了但执行侧不读"的检查时 |
