# HFTS/FTIZB（Irregular Z-Buffer）调研与实施方案

> 状态：**现行** —— Phase 11 批 11.3（spike，带退出条件，非承诺项）的调研结论 + 分阶段执行计划。
> 引用方：`phase11_plan.md` §批 11.3 · `next_roadmap.md` §Phase 11 ③。
> 2026-10-01 调研定稿（四篇论文全文通读 + 开源面查清 + 项目 gap 经主 agent 复核）。
>
> ⚠ **2026-10-10：IZB 交付已关（`shadow.lua` 的 `izb` 默认 true → false，用户放行换红线）**。
> 两个结构性缺陷 = ① 只管 cascade-0 光空间盒、盒外回落 CSM ⟹ 盒边界是一条**随相机在世界表面
> 爬行的接缝**（默认机位实测 1.76% 屏幕判反、亮度差中位 16 LSB / 最高 160 LSB）；② 管辖范围内
> 系统性欠遮挡（`imgqa izb` 的「CSM 遮而 IZB 放」= 405048 px = 占屏 44%）。**阶段 2/3 若开工，
> 缺陷① 是必须先解决的问题**（"扩到全部级联"正是阶段 2 的形态，但按现状扩会把 44% 欠遮挡从
> 近场铺满全屏）。读数、复现配方（三色哨兵一步定死接缝）与裁决全文 →
> `shadow_routes_plan.md` §7。代码路径全留，`izb = true` 会触发 gate_lint warn。
> 本文下方带日期的实测记录（含红线 `fdece9d0…`）是**当时的**读数，不改写；现行红线 = `52231a84…`。

---

## 1. 调研结论

### 1.1 论文族谱

| 文献 | 定位 | 对本项目的关键输入 |
|---|---|---|
| Johnson et al. 2005, "The Irregular Z-Buffer" (ACM TOG) | IZB 原始（硬件提案） | 算法本源：光空间 A-buffer，每个 texel = 一条阴影点链表 |
| **FTIZB 2015**（Wyman/Hoetzlein/Lefohn，I3D 2015 **Best Paper**） | **最重要** —— 把 IZB 从 compute 路线搬进**光栅管线** | 六 pass 流水线 + 全部优化的量化收益表（§1.2） |
| FTIZB 2016 TVCG 扩展版 | 同系统期刊版 | 数据布局细节（节点预分配、地址即像素索引） |
| **HFTS 2016**（Story & Wyman，SIGGRAPH 2016 Talks） | The Division 实装版 | 近场 IZB 硬影 ↔ 远场 PCSS 混合；per-pixel occluder distance 过渡；两 frustum light-space partitioning |
| Tokuyoshi & Mizokuchi 2018（SIGGRAPH Poster，Square Enix） | 性能加速 | 保守 shadow map 预剔除全影节点：**12.4 → 2.12 ms**（其 Fig.3 场景，约 6×）；含可直接移植思想的 HLSL「texel 全覆盖」判定片段（四角重心坐标） |

下载地址（论文 PDF 均可公开获取）。**本地离线副本已落地 `docs/参考资料/`**
（2026-10-01，8 份 PDF + 索引 README + 重下载命令 + 未下载项备案）：
- FTIZB 2015 camera-ready：`research.nvidia.com/sites/default/files/pubs/2015-02_Frustum-Traced-Raster-Shadows/ftizb_cameraReady.pdf`
- FTIZB 2016 TVCG：`cwyman.org/papers/tvcg16_ftizbExtended.pdf`
- HFTS 2016：`cwyman.org/abstracts/sig2016_hfts.pdf`（Story & Wyman，2 页 Talk）
- Tokuyoshi 2018：`jp.square-enix.com/tech/library/pdf/Conservative Z-Prepass for Frustum-Traced Irregular Z-Buffers.pdf`
- 教程式 walkthrough（RTR4 §7.9 摘要，assassin-plus.github.io，2025-03）：概念讲解可用，无实现

### 1.2 FTIZB 六 pass 流水线（核心）

> **术语勘误**：roadmap 原文写"光线步进"。严格说 IZB 不是 raymarching，是
> **frustum tracing**：每个像素（及其 µQuad 子样本）向光源方向对遮挡三角形做
> frustum-triangle 相交测试，由**光空间保守光栅**驱动（三角形生成 fragment → 遍历
> 该 texel 的像素链表），不是逐像素发射步进光线。实现按 frustum tracing 口径。

1. **eye-space z-prepass**（G-buffer）——确定"哪些像素需要阴影查询"；
2. **光空间 scene bounds**（compute，每帧紧贴可见几何拟合光 frustum + cascade 切分）；
3. **IZB 创建**：每像素投到光空间，原子插入对应 grid cell 的链表（grid-of-lists）；
4. **early-Z priming**：光空间深度缓冲——空 texel 写 0、非空写最远节点距离（硬件 z-cull，收益 30–50%）；
5. **核心 pass**：光空间**保守光栅**画遮挡三角形；每 fragment 遍历该 texel 链表，
   逐节点做 frustum-triangle 测试（每条三角形边 = 一个 shadow quad；32 子样本可见性
   走 LUT，三边结果按位与），原子 OR 写 visibility mask；
6. **最终着色**读 mask（替代/混合 shadow map 采样）。

关键数字（GTX980 / 1080p / 2015 硬件）：1 spp 游戏场景 **< 8 ms**、32 spp ≈ 24 ms；
IZB 创建 < 1 ms；主成本 = pass 5 光空间光栅。光空间网格甜点 ≈ 1400²–2500²（1080p 下 2048² 起步可用）。

优化收益表（阶段 2 排期依据，全部出自 FTIZB 2015/2016 实测）：

| 优化 | 收益 |
|---|---|
| early-Z culling（pass 4） | 30–50% |
| IZB 节点移除（像素全遮后从链表摘除，any-hit 语义） | 10–15% |
| N·L≤0 剔除（背对光的像素不建节点） | 10–15% |
| mask 不变不写（去原子竞争） | 最高 14% |
| 软件流水线（提前预取下一节点） | 5–15% |
| 视线方向 1D 采样（每像素 1–8 节点而非 32） | 链表平均长度 ↓4× |
| 1-texel dilation（补偿近似插入的漏光，代价项） | 三角形 fragment 数 +6–40% |

### 1.3 HFTS 增量（The Division 实装版）

- **混合**：近接触 = IZB 硬影（alias-free + 接触硬化），远场 = PCSS（软影）；靠
  **per-pixel 最近遮挡体距离**过渡，shifted penumbra filter 防 PCSS 在接触点干扰 IZB。
- **两 frustum light-space partitioning**：替代 cascade，专为 IZB 的不规则负载做均衡
  （比 cascade 更少的固定开销与 CPU↔GPU 通信）。
- **实测成本：HFTS 比 PCSS 贵约 1.4–1.7×**（1080p / GTX980：8.1 vs 5.7、7.9 vs 4.6、
  9.7 vs 5.5 ms 三视图）。
- **推论**：HFTS 的价值是质量（无 texel 走样 + 接触硬化），**不是比 CSM 便宜**；
  预算口径必须按"质量增益 vs 成本"判，不能预设"更省"（见 §4.4）。

### 1.4 开源现状：**无可抄**

| 资源 | 状态 | 价值 |
|---|---|---|
| NVIDIA ShadowWorks（HFTS 商业库） | 闭源，许可不明 | 无 |
| `github.com/strexicious/izb-on` | C#/OpenGL，13 commits，作者自述 finished 不再维护 | 仅算法示意（光空间 binning + 保守光栅 + 链表遍历）；README 自认近处 list 爆炸掉帧 |
| Godot proposal #3965 | 关闭未实现 | 背景 |
| FTIZB 论文附带的 GLSL 示例 | 论文自称在补充材料，未找到公开下载 | 无 |

**结论：必须自己写。**最近的可执行参考 = FTIZB 论文伪代码 + Tokuyoshi 的 HLSL
片段（四角重心坐标判 texel 全覆盖）。

---

## 2. 项目 gap（researcher 摸底 + 主 agent 复核，2026-10-01）

| FTIZB 需要 | 本项目现状 | gap |
|---|---|---|
| eye-space 深度 | **forward 无 z-prepass**（着色与写深度同 pass）；deferred 有 G-buffer | forward 加 prepass；deferred 免费 |
| 保守光栅 | RADV 26.2.1 实测：`OVERESTIMATE` 基线可用、`postDepthCoverage=true`、DynamicState3 可走（`phase11_plan.md` §9） | ✅ **已闭环（2026-10-01 批 1b）**：× mesh shader × EDS3 动态态本机实测全中，护栏 = `tests/test_conservative_mesh.cppm`（gpu 套件）；详见 §4.1 1b 落地记录 |
| IZB 创建 | render_graph 无 compute kind；图外 compute dispatch 有先例（cull_driver / gpu_cull / mip_gen / env_prefilter） | 见 §3：沿用项目惯例放图外 |
| A-buffer（光空间链表 + 全局原子） | 无 | 新 SSBO + barrier |
| 光空间 raster pass | **shadow pass 是现成模板**（drawMesh 零色附件 depth-only + state_kit 条目 + viewport 子矩形） | 新 state `"izb"` + 链表遍历 fragment 变体 |
| mask 消费 | `shadow.slang` 4 抽头 PCF（binding 12/13） | 新 binding + 旁路 |
| **PCSS**（HFTS 混合的另一半） | **不存在**（现在只有 PCF，roadmap 批 11.2 计划的 PCSS 未落地） | 阶段 3 前置任务 |
| 3 ms 预算 | 4 级 CSM 已花 **7.10 ms**，Phase 9 的 3.1 ms 余量已被吃穿（2026-09-29 用户拍板画质优先，gpu.frame 5.05→10.05 ms） | 预算口径重定（§4.4） |

---

## 3. 架构方案

**关键结论：图内 2 个光栅 pass（z-prepass、光空间保守光栅）+ 图外 1 个 compute
dispatch（IZB 创建）——与本项目"compute 全在图外、render_graph 是纯光栅意图图"
的既有架构不变式一致，不需要新增 render_graph pass kind。**

| FTIZB pass | 本项目承载 | 依据 / 先例 |
|---|---|---|
| ① eye-space 深度 | deferred：G-buffer 现成；forward：新增 z-prepass（drawMesh，depth 附件 load=clear、全幅 viewport、零色附件，state 复用 `shadow` 的 depth-only 形态） | shadow pass = 零色附件 drawMesh 先例 |
| ②③④ scene bounds + IZB 创建 + early-Z priming | **图外 compute dispatch**（一次 dispatch 内完成投影 + 链表插入 + priming 深度写） | cull_driver / gpu_cull 编舞先例；帧时序挂在既有 compute 窗口 |
| ⑤ 光空间保守光栅 + 链表遍历写 mask | drawMesh 新 state `"izb"`：DynamicState3 开 `OVERESTIMATE`（不加管线条目）+ 遍历链表 fragment 原子 OR 写 r32ui mask SSBO | shadow pass 模板 + phase11 §9 动态状态结论 |
| ⑥ 消费 | `shadow.slang` 加 izb 分支：读 mask，与 PCF 结果混合/替代 | binding 12/13 旁路先例 |

**门控**：门 = `configs/shadow.lua` 的 `izb`（**出厂关**，代码默认 false）。默认值走过三步：
立计划时关 → 2026-10-09 批 N6a「承认出厂开」改 true → **2026-10-10 实测两个结构性缺陷后关回
false**（用户放行换红线，读数与复现配方 → `docs/shadow_routes_plan.md` §7）。门关 = IZB 相关 pass
零 draw + 采样端旁路 = **画面逐位不变**（`mode = "none"` / `ao.strength=0` 同款纪律，
BIT-IDENTICAL 基线不受扰）。开回来 = `izb = true`，gate_lint 会 warn 把两个读数带在消息里。

**红线**：

1. **forward z-prepass 落地后 opaque 的 depth load 由 clear 变 load** → 必须跑
   BIT-IDENTICAL 对拍证明逐位相同（深度值不变则画面应逐位相同——这是断言，不是假设）。
2. **保守光栅只在 izb pass 开**：主 pass 开 `OVERESTIMATE` 会让像素边缘多写深度，
   与 ao pass 读深度重建世界坐标相互作用，属不必要风险面（phase11 §9 同款结论）。
3. **`maxExtraPrimitiveOverestimationSize = 0`（RADV 实测）= 无硬件 fattening**：
   漏光对策走插入侧/光栅侧的 1-texel dilation（软件实现），**不得**依赖硬件外扩。
4. 新增 state `"izb_mask"`（实际落地名，非草案的 `"izb"`）后 state_kit 九段变十段 →
   **口径已收口（2026-10-02）**：状态名词表迁入单一真相源叶子模块 `pass_vocabulary`
   （`izb_mask` = `internal` 档 = 不走图，由执行器硬编码录制路径直接 `stateFor`），
   单测状态名改为由词表派生、`AGENTS.md` 与 `docs/技术概念.md` 口径改十段。
   本条待办的性质也随之作废——不再是"记得同步四处"（当年就漏了四处），而是同步不了：
   StateKit `static_assert`（词表 ↔ 实现同名同数）+ `tests/test_render_graph.cppm`
   的档位分区用例。

---

## 4. 分阶段执行计划

### 4.1 阶段 1 —— 最小可行性 spike【收手判据在这里】

子批次（每批独立可验收）：

**1a `izb_rig` 测试场景**（`tools/qa_scenes.py` 新增第 4 场景）——**2026-10-01 落地**：
- 内容（10 pieces，恰好用满 10 色固定调色板）：地面 ±60 亮黄 + 主柱 r0.5·h6 +
  **厚度梯** bar_thick/mid/thin/hair（截面 0.4/0.15/0.05/0.02 m，同长 6 m 同高 y=2，
  间距 ≥6 m 包络分离）+ **接触探柱** post_contact（0.04 m 底面 y 精确=0）+
  **穿插探杆** bar_mounted（X 向穿主柱，影带横穿柱影）+ **叠影探杆对** stack_pair_lo/hi
  （y=1.0/2.5，影带组成「受光|单叠 6cm|双叠」三分带，构建期精确算出进 manifest）。
- 构建期断言 A1–A8 全部**从几何本体推导**（rect/影包络/y 区间不手抄——手抄双源
  必错，本次实测抄错一次被用户抓获后重构）；声明重叠对必须真相交（排除集不许空转）。
- 引擎冒烟（2026-10-01）：loader 10 primitives / 582 顶点 / 零 error；stack_zoom 机位
  实拍：叠影杆两条 + 梯子档多条影带可见，落向与 h·(2/3, -3/5) 一致（朝 -Z/+X），
  **CSM 影缘锯齿肉眼可见**（= IZB 要打掉的基线缺陷，A/B 有得看）；三分带的分区
  分离度与画面中可见的级联切换接缝未逐项核——留给 1d 用 imgqa + 正射近景定量。
  远景全景机位下强 env 使影很淡属预期，度量走同材质比值与开关对拍。
- 场景生成一致性契约 → §4.5（双跑 byte-identical 已实测）。
- 遗留（→ 1d/1e）：`qa_scenes.py` 的 `--out` 消费路径已验证；imgqa attribution 对
  izb_rig 的 objectID 反解要等 picking 通道在 headless 下可 dump 时再跑（1d 判据批次）。

**1b 风险①先行（成败门）**：保守光栅 × mesh shader × RADV 最小验证
（独立最小 pass，几百行）。**失败 fallback 链**：软件保守光栅（Hasselgren 顶点
外扩）→ 仍不行 = **收手**。——**2026-10-01 门开（fallback 未触发）**：落地为
`tests/test_conservative_mesh.cppm`（`TEST_SUITE("gpu")` 永久护栏，非一次性 spike）
+ `tests/shaders/izb_spike.{mesh,frag}.slang`，本机 RADV（Strix Halo）四条 golden 全中：
- **亚像素三角**（严格在像素 (17,13) 内、不含像元中心）：保守 OFF = 0 像素【负向锚点】
  vs **OVERESTIMATE = 恰 1 像素 @(17,13)**【门】；大三角中心采样 300 → 保守 **325**
  （两值均逐格闭式解，着色器/推导见文件头）。
- **EDS3 动态态双向切换在 mesh 管线上生效**：DISABLED 管线→动态 OVER = 1/325；
  OVER 管线→动态 DISABLED = 0/300（单向测试会漏"管线恒保守"这类坏法）。
- §9 的 vulkaninfo 事实（maxExtra=0）钉成 golden；能力位
  `extendedDynamicState3ConservativeRasterizationMode` 已接进引擎设备链
  （features_config.json optional + EDS3 底座条件与 depthClip 两位共用 + vkdevice 置位）。
- **变异实证**：摘 `rs.pNext = &cons` → 门用例即红（红线声明非空话，已复原）。
- **附带抓获的坐标系事实**：Vulkan NDC y 向下 + 探针 y-up 约定 → 正 viewport 下
  首跑镜像 13↔50；引擎的**负高度 viewport**（`shader.cppm` applyDynamicState）就是
  那唯一一次补偿。1d 光空间 pass 必须继承同款负 viewport，别再补第二刀。

**1c A-buffer + IZB 创建 dispatch**（图外 compute + 原子链表 + priming）。
正确性判据 = 链表长度分布直方图 dump，对照论文 Fig.4（texel 平均节点数个位数，
不出现长 list 爆炸）。——**2026-10-01 落地**（与 1b 同形态 = `TEST_SUITE("gpu")`
永久护栏）：`tests/test_izb_create.cppm` + `tests/shaders/izb_{create,prime}.comp.slang`。
- 数据布局按 2016 TVCG「**地址即像素索引**」：head uint[G²]（NONE 哨兵）+ nodes
  {float lightDepth, uint next} 按像素 ID 索引——一像素至多一节点，**无节点池溢出**；
  创建 = 原子栈压入（InterlockedExchange），priming = 走链取 max（空 cell 恰 0）。
  spike 全 SSBO（输入深度/输出 priming 都不碰纹理载体；1d 换纹理，算法本体不变）。
- 判据（合成场景，确定性零 RNG，闭式手工锚点）：每 cell 深度**多重集**（顺序无关，
  原子链顺序天然不确定）、**环检测**、可达像素 ID 集合不重不漏、priming = max/0 +
  「非空格计数」阳性锚点、反向 = 空哨兵/越 UV 像素 ID 全表不可达。A 组长链锚点
  {21.125, 19.875, 18.625} 系 `20−y` 闭式独立字面量。直方图 dump `0:56 1:7 3:1`
  （合成形状是设计出来的；**论文 Fig.4 统计口径推迟 1d 真场景**，此处只验机制）。
- **变异实证 ×2**：摘链（next 不接旧表头）→ 长度/成员/priming 三判据红；priming
  取 min → priming 对比组红（均已复原）。
- 首跑抓获的四个坑（1d/1e 集成必读，均已注释钉在代码处）：
  ① slang `ByteAddressBuffer.Load4` 返 int4，**必须 asfloat 位转**（直接赋 float4 =
  数值转换，矩阵变垃圾——引擎 cull/ao 全部 asfloat 包一层，勿因"引擎有先例"而侥幸）；
  ② `InterlockedExchange` 首参是**字节偏移**（传元素索引 = 写入缩到 word idx/4，
  全表错位 4 倍且 **VVL 不出声**——本坑靠 dbg 仪表 buffer 逐像素对拍定位）；
  ③ `l3 * w`（float4×float4）= **分量乘**，齐次列组合要写 `l3 * w.w`——本坑里
  lightView 平移列 x/y 恰为 0，uv 幸存、只有深度坏，**局部症状掩盖根因**；
  ④ host SSBO `unmapMemory()` 后再经映射指针读 = 悬空（首跑 SEGV）；正确姿势 =
  保持映射到生命周期尾（coherent 直读，free 时隐式 unmap 合法）。
- **描述符红线（2026-10-01 用户拍板）**：本测试用老式 DescriptorPool+set =
  **仅 spike 放宽**（fixture 无 VMA/Resources）；**1d 进引擎必须走 `vk_resources`
  全量 descriptor heap**（用户槽 runtime 段），老式 bindless 引擎禁入 →
  `docs/DESCRIPTOR_HEAP_SPEC.md`；已钉 `test_izb_create.cppm` 文件头。

**1d 前半（2026-10-01 深夜落地）：IZB 创建/priming 进引擎（rlgfx `izb_shadow` 模块）**。
- 形态：图外 compute sandwich，录在 **deferred 图 `drawLight "light"` pass 头部**
  （深度已由 geometry pass 写完）；深度 layout 进出同值 = 渲染图布局追踪表零漂移。
  门 = `configs/shadow.lua` 的 `izb`（**出厂关**，代码默认 false，理由见上方门控段）：**门关 = IzbShadow 恒不构造**
  （零分配零命令）；forward 图无 drawLight pass 天然不触发（z-prepass 两路接 = 1e）。
- 数据 = `vk_resources` 全量 heap Runtime 段 3 槽（izb.head/nodes/primed）+
  **深度采样描述符与 SSAO 共用 `ensureDepthSampleSlot`**（同 view 同 RO 布局）；
  shader object = heap mapping（gpu_cull 同款）；每帧 `vkCmdFillBuffer` 清 head。
- 光空间 = **cascade-0 `shadowViewProj`**（正交 → 窗口 z 天然线性、深度比较仿射不变
  → 无需 near/far；与 CSM 光向同源）。**v1 语义 = 近场 IZB**：cascade-0 范围外像素
  不建节点（HFTS 远场分区 = 阶段 3）。节点 payload 收编 uv：16B = (u, v, lightDepth,
  next)（mask pass 的 edge 判定直读，spike 的独立 pixelUV SSBO 取消）。
- 实测（bistro deferred 1280×720，`izb_dump_frame` 阳性锚点）：G=512 →
  cells 262144：empty 239584 / n1 1515 / n2 989 / n3+ 20056，**maxChain 162，
  nodes 304122**，无坏链；G=64 对照 → 同 nodes、maxChain 2704（grid 参数真生效，
  节点数几何不变）。VVL 零错误；**门开截图与门关 byte-identical**（mask 消费未接
  = 画面不变红线实证，llm_debug 帧 8 对拍）。
- 首跑 VVL 三连坑（均已注释钉 `izb_shadow_impl.cppm`，git:调试历史 2026-10-01 收账）：
  ① BDA 必须 `createBufferWithAddress`（普通 `createBuffer` 的 `getDeviceAddress()`
  恒 0 = 写描述符 "address is zero"）；② `vkCmdFillBuffer` 要 `eTransferDst`；
  ③ `VkBufferCopy.size` 不吃 `VK_WHOLE_SIZE`（要显式字节数）+ nodes 是 dump 拷贝
  **源**（`eTransferSrc`，注释曾写反）。
- 直方图对论文 Fig.4 口径：bistro/cascade-0 的 n3+ 占非空 cell 92% ≫ 论文理想值——
  因 cascade-0 半径小（~数米）把大量近场几何压进 512² 同 cell；**结论 = v1 的
  cascade-0 网格密度不够或范围该放大**，mask pass 落地后用 §4.4 预算口径一起定参。

**1d 后半（待做）mask pass（光空间保守光栅）实现 checklist**：
1. 管线 = mesh（光空间 MVP）+ frag（走链判定）+ `RASTERIZATION_MODE_OVERESTIMATE`
   （EDS3 动态态，能力位 1b 已接）；shadow pass 的 depth-only 段是形态先例
   （`ShadowDrawArgs` / 图集子矩形 viewport / drawSource "shadow*"）。
2. 负 viewport 口径必须继承 `shader.cppm` `applyDynamicState` 的 `vk::Viewport(x,
   yTop, w, -h)` 唯一补偿（1b 结论：mesh 输出 clip 是 D3D12 口径 +Y 向下）——
   tests spike 已按同款验证过（`test_izb_mask.cppm` 判据③' 钉住）。
3. mask 载体 = SSBO 原子（bit = 三角），pass 零附件 beginRendering（0 color）或 1
   texel 占位；viewport = 光网格 G×G。
4. frag 判定语义 golden = `test_izb_mask.cppm`（2D 棱柱三边同号 + texel 中心插值
   深度；OVER/DISABLED 对拍已实证保守专属正对与 edge 拒斥）。
5. 投影体来源 = opaque 段 DrawArgs 全量重放（同 shadow casters 思路，cullMode 关或
   双面）；级联 = v1 仅 cascade 0。
6. 消费（1e）：deferred light.frag 读 mask bit → 乘到该灯贡献；forward 走 z-prepass。

**1d 后半（2026-10-02 凌晨落地）：mask pass（光空间保守光栅）——render_list 与
render_graph 零改动版**。
- 形态：sandwich 内 **create/prime 之后接 mask pass**——`beginRendering`（**零附件**、
  renderArea = G×G）+ `izb_mask` 动态态（OVERESTIMATE 保守/双面/无深度）+ **shadow0
  段逐 draw 重放**（投影体与 push 复用阴影段，尾缀 gridDim = 12B push）。段空 =
  warn-once（mask 依赖 shadow_pass 开启）。
- 管线：`izb_mask.mesh.slang`（shadow.mesh 镜像 + **平摊三顶点 (uv, lightDepth)**
  ——mesh 顶点逐三角独立，三顶点写同一份整三角数据）+ `izb_mask.frag.slang`
  （FTIZB 判定：节点 uv 的 2D barycentric inside + **节点位置插值光深度** 严格 >
  + bias 2e-4；bit = 屏幕像素 ID「地址即像素」，`InterlockedOr` 位图）。
- 描述符：`Resources::ensureIzbSlots()` **四槽号终身稳定**（管线条目启动期 bake，
  跨 IzbShadow 重建不变）；条目 `addIzbMaskEntry`（vertexSlot 去重，extra 5/6/7）。
- 保守光栅：EDS3 动态位（`conservativeModeDynamicSupported` → StateKit 门 →
  `applyDynamicState` 第 6 参；**能力在场时每 pass 显式写值防 sticky 泄漏**）。
  核心 feature `fragmentStoresAndAtomics` 补进 features_config.json（reflect）。
- **实测阳性锚点**（bistro deferred G=512）：`shadowed=33707 / nodes=232820`
  （14.5% 被遮，maxChain=170）；VVL 零错误；**门开（mask 已产出）截图与门关
  byte-identical**（消费未接红线，重构前后双验）。
- **w 除法坑（首挂真凶，git:调试历史收账）**：izb_create 反投影 `invVPM·ndc` 未
  `w/=w.w` → 节点 uv/深度落进沿视射线缩放的假空间，与 mask mesh 的真实 world
  不同空间 → inside 判据全灭。判别路径三层假象：①「head 置位 221」证明管线通
  但掩盖判据问题；② 三态计数把 occl/inside 写进**相邻像素位**（`2u<<(h&31)`）
  互相污染读出 154483 假命中；③ 深度-only「221」也是巧合集。**教训：判别位必须
  用独立 counter（固定字），别复用位图挪位**。修后 nodes 304122→232820
  （uv 过滤更准）。
- **1d 收口状态**：create/prime/mask 全通 + dump 判据；**1e = 消费接线**
  （light.frag 读 mask 位 → 该灯贡献衰减 + forward z-prepass + D1 对拍 +
  imgqa attribution 影响域）。

**1e 消费接线（2026-10-02 凌晨落地，deferred 端到端；终态 = 变体编译，见下）**：
- **红线终态 = shader 变体编译（重要方法论定案）**：`light.frag.slang` 单源双 SPV——
  无 -D 原版 + `-DIZB_CONSUME` 变体（`rgl_shader_defined`），registry 按 `izb_shadow` 门
  选 spec（`findLightSpec`），binding 14 仅变体绑定。**首版运行时 flag 分支被实测推翻**：
  flag=0 → 原样 `sampleShadow1Tap` 语义等价，但 slang 对源码差异（函数边界/SSA）的
  fp 重排让门关输出对老基线漂移 **105401 像素**——当时的「门关=基线 PASS」是
  **同 build 自比同真**（izb_1e_off 与 iz_b_v1_off 同二进制），跨二进制二分
  （5b843e3 checkout 复跑 = 老基线可信 → 回退 light.frag 消费点即恢复）才定位真凶。
  **定案：shader 门控唯一正确形态 = 变体编译；「门关逐位」红线必须跨二进制对老基线。**
- 消费语义 = **替换语义 v1**（变体内）：flag（scene data @722016，首帧时序回落 CSM）
  → cascade-0 IZB mask 接管方向光阴影（范围外 = 1.0 无影）。**相乘方案被实测否决**：
  同一太阳下 IZB 遮挡集 ⊆ CSM 遮挡集 → 相乘画面零差异；补集判别 +177815 像素反证
  通路全通。
- 消费区：scene data 尾 `kSceneDataIzbOffset @722016`（flag,W,H,r），
  `writeSceneStaging` 按 `izbShadow` 门写；light.frag 字面量由 static_assert 钉死。
- **binding 14 仅变体声明+绑定**（原版无此 binding = 不占槽）；Resources 发号时仍写
  16B 零 fallback 到 mask 槽（防门开首帧 IzbShadow 构造前的时序坑，门关零成本）。
  判别技巧（fallback 全 1 探针等）见git:调试历史 2026-10-02。
- **跨 render pass barrier（首挂真凶）**：mask frag atomic 写 → light.frag 读之间
  无隐式依赖，**VVL 不报错但 light 读恒 0**（现象 = 门关门开逐位一致、flag 硬置
  判别却生效）。`endMaskPass` 内补 allGraphics/shaderWrite→shaderRead；
  `beginMaskPass` 一并补 head/nodes 的 compute 写→frag 读 + fill 前 WAR（src 含读）。
- pid 口径：mask bit = izb_create 的 py·W+px（fb 行）；light.frag 用 uv 反解
  （light.mesh uv 契约 = fb 归一化 y 上→下）**免翻转**，且零 mesh 契约改动。
- 实测（bistro deferred）：门开 = IZB-only 硬影**位置与 CSM 一致**（建筑暗面/墙面
  大块/檐下；树冠与广场 IZB 更aggressive = 逐像素判定对细几何的特性，v1 已知）；
  门关 = 基线逐位；默认 forward 门关闭零 izb 构造、VVL 全程零错误。
- **性能记录（§4.4 口径，2026-10-02 bistro deferred 720p，profiler window 5，未优化 v1）**：
  `gpu.frame` 9.47 → 10.63 ms（**IZB 总成本 +1.17 ms**）；light zone 0.27 → 1.64 ms
  （sandwich = create 921600 线程 + prime G² + **mask 重放 shadow0 段全量 meshlet**
  ——重放是成本大头，正是论文预剔除优化的目标位；shadow 各级 zone 无扰动）。
  不设 ms 门（用户拍板），数字供阶段 2 优化基线。
- **1e 未完项**：forward z-prepass（需条件性图插 pass + draw 代价×2，列白天批次）；
  izb_rig 三分带闭式对拍；imgqa attribution 影响域；bias/grid 调参。
- **1e 已知异常（干净二进制实测，hair 贴脸机位，02:55 收账 = 观察确凿、根因待昼查）**：
  mask = **上半屏全宽置位**（453274 ≈ y<340 全排，边界锐利横带）+ 下半全零 +
  无杆影带结构；IZB 画面该带不黑（light 读该 mask 却不见对应影 = 天空行走
  skybox 分支不受 shadowFactor，带内地面部分应黑未黑 = 消费时序/pid 还需查一层）。
  候选机制（未逐一证伪）：① 120m 大地面三角（rig 独有；bistro 地面=meshlet 小片
  无此象）在 cascade-0 外的投影/重心外推令 inTri&&occl 大面积误真 ② nodeD 与
  三角 dF 的 ndc/光空间深度口径在远端分叉 ③ 消费侧 pid/帧时序。**ladder 机位
  （同一 rig 场景、无大地面视角）A/B 与 CSM 逐位一致** = 主几何通路正确的强交叉
  验证，异常局限于贴地掠射视角。**阳性对照（同夜后补，stack 贴脸俯视 3m）**：双杆硬影
  清晰出影（方向/偏移正确，锯齿边 = 屏幕像素级硬影），**IZB 独有暗 306151 / CSM 独有
  仅 479** = CSM 全丢的细杆影 IZB 画出（卖点实证）；量级偏宽 = bias/精度
  over-estimation（v1 调参域，`izb_mask.ppm` 直接看遮带宽度）。
  **判据工具新增**：dump 帧写 `izb_mask.ppm`（白=未遮 黑=遮，fb 行序）——mask
  空间结构 vs 截图目检直接对位，比 count 数字锐利得多。
- **补账结案（2026-10-02 上午，用户复测推动）**：「门开无影」三重根因全修毕——消费端
  漏 `asfloat()`（ConvertUToF 陷阱）+ 字节偏移口径（`Load(pid>>5)` 缺 ×4）、生产端
  izb_create 屏幕 ndc.y 走错口径（世界重建上下镜像）。**「hair 掠射上半屏全遮异常」
  = ndc 镜像的产物，随修消解**（stack「阳性」= 稠密假绿，教训入git:调试历史）。守卫就位：
  `test_izb_engine_contract`（引擎真 SPV 闭式，变异验红×2）+ imgqa `izb`（mask↔画面
  三判据）+ imgqa `swim`（影场世界不变量）+ `slang_load_bitcast_checks`（静态规则）。
  **「影子随相机游动」结案 = 非 5a-7 回归**：swim 实测 CSM·rig·0.06m 漂移 0/63835、
  IZB·rig·2m 漂移 1/55614（机制世界稳定）、IZB·bistro 街景·0.06m 影区重心滑 43%——
  即 v1「cascade-0 近场」语义的 patch 边界扫过（街景 c0 仅 r=0.25 光盒贴相机）。
  修法待拍板：覆盖外回落 CSM（推荐）/ 换挂 c3 / 阶段 2 多网格。

**债-32 收口（2026-10-09：mask 永不清零 = 着色器读未初始化显存）**

- **根因（一句话）**：mask 的 `FillBuffer` 清零写在 `IzbShadow::beginMaskPass` 之内，而 mask
  pass **会被调用点整段跳过**（`Renderer::replayIzbMaskSegment` 在 shadow0 段为空时早退 =
  `shadow_pass=off` 或无投影体）；消费侧 `light.frag` 的 `izbShadowFactor` 只看 izb flag
  （= `izbShadow` 已构造，**不判 shadow_pass**）照常被读 ⟹ 该组合下整帧无人写过 mask，shader
  读未初始化显存。本机此前与 shadow-off 逐位相同 = 「新页读零」×「bit=0 语义恰为无遮挡」两条
  侥幸叠乘，换驱动 / 分配器复用脏页即翻车。
- **修法 = 搬走清零，不是给调用点加纪律**：清零移到 `IzbShadow::recordFrame` 第 ⑧ 步（每帧
  无条件；`izbLightPassPre` 逐 pass 无条件调它）。此后「izb flag=1 ⟹ mask 本帧被写过」**由结构
  成立**，调用点再怎么早退都跳不掉。两条位置/同步红线：① 必须在 dump 拷贝**之后**——dump 读的是
  **上一帧**的 mask（`imgqa izb` 吃它），清在前面 `shadowed` 恒 0；② fill 的 dst barrier 补
  `ShaderRead`——段空时没有 `endMaskPass` 那条写→读 barrier，light.frag 的读得由 fill 排序。
- **护栏**：gate_lint 新增 `izb × mode = "none"` error（与 `mode = "evssm"` × 非 deferred 同判据
  同形；N6b 起阴影侧规则表整体按枚举重写，现行口径 → `docs/shadow_routes_plan.md` §3.2）。
  规则表**首次一门两行**（两个根因各一条消息，合并 = 丢归因）⟹ 覆盖率护栏改钉**去重后的门数**。
- **判据（本机 8060S / RADV，1280×720 deferred）**：① 红线 = 默认配置截图 SHA256 `fdece9d0…`
  复现（正常路径命令数不变：三条 mask 命令只是换了宿主函数。7 次里 6 次逐位、1 次 1 像素
  ±1 LSB，而**改动前**二进制同档连拍也漂 1 次 1 像素 ≤4 LSB ⟹ 本机既有 nondeterminism，
  非本批引入 → 总账债-09）；② `just smoke --sync` hazard 0；③ `just test` 242 全绿；
  ④ **位置红线的直接复验**：`izb_dump_frame=3` 跑通 → `shadowed=109294` > 0（清零若搬到 dump
  之前该值恒 0），该档再开 syncval 仍 errors=0；⑤ **阳性锚点（变异）**：把 ⑧ 的 fill 值改成
  `0xFFFFFFFF` 跑 `shadow off × izb on` → 画面整片失去直射光（消费通路是活的、fill 真落地）
  ⟹ 修前该组合「无人写 mask」不是纸面推理；⑥ 修后 `shadow off × izb on` 与
  `shadow off × izb off` 逐位相同（`121e7502…`）= 全 0 mask 的「无遮挡」与 shadow-off 本应
  给出的画面重合，且这次**由构造**；⑦ lint 三条变异各自验红（谓词恒假 → 正例红 / 谓词去掉
  `shadowPass` → 反向红 / 整行删掉 → 覆盖率与「两条独立规则」计数红）。
- **教训（`TEST:` `tests/test_gate_lint.cppm` + `GUARD:` gate_lint 规则表）**：**每帧必达**的写
  （清零 / 初始化 / 上传）宿主必须选在**每帧必到**的编舞里。挂在「可选 pass 的 begin」上 = 把
  正确性押在「调用点不会跳过这个 pass」这条没人守的约定上，而这一维 VVL / syncval **根本不建模**
  （债-75 同族：shader 侧发起的访问它无从归因）——所以这类洞只有结构能堵，日志和层都指望不上。

**1d 光空间保守光栅 pass + mask 写入**：✅ 已完成（上文）；「izb_rig 出硬影」= 1e 消费后。

**1e 消费接入 + forward z-prepass**，D1 对拍（forward vs deferred 的 izb 阴影），
BIT-IDENTICAL 对拍（红线 1）。

**验收判据（阶段 1）**：
1. **视觉**：izb_rig 上细横杆影边缘无 texel 走样（与 CSM 对拍肉眼可辨）、接触阴影
   正确、无漏光；另在 Bistro 上跑一组目检对比（不做 ms 门）。
2. **判据工具**：imgqa attribution（开关影响域域外恰为 0）+ 开关对拍。
3. **性能**：**不设 ms 门**（2026-10-01 用户拍板：阶段 1 只验正确性）；各 pass 成本
   用 gpu profiler zone + cpu.record 量化，记录回本文档 §4.4。
4. **基线**：`izb_shadow=false` 帧与现有基线逐位相等。

### 4.2 阶段 2 —— 性能（阶段 1 结论"做"才开）

按论文收益表排期：**Tokuyoshi 保守 shadow map 预剔除**（最大单项，约 6×）→
early-Z priming 精细化 → 节点移除 → N·L 剔除 → unchanged-mask 去竞争。
实测后**定预算口径**（§4.4），定案记录回本文档。

### 4.3 阶段 3 —— HFTS 混合（阶段 2 预算口径达标才开）

前置：**PCSS**（本项目现在只有 PCF，roadmap 批 11.2 计划的 PCSS 未落地——阶段 3
第一个任务）→ occluder distance 提取 → IZB/PCSS 过渡（shifted penumbra）→
两 frustum 分区（可选，视负载数据定）。

**半影裁决（2026-10-02 用户拍板，街景路灯机位实看后）**：v1 全程锐利硬影
（零半影）接受为阶段 1 语义，不为画质提前开工——「底部清晰尖端模糊」=
太阳角直径 0.53° 的物理半影（宽度 = Δz × 0.0093，3m 杆顶 ≈ 2.8cm），
CSM 固定 PCF 半径与 IZB v1 二值 mask 均给不了，属已知设计缺口非 bug。
**阶段 3 开工免重推导的调研结论**：现有 `izb_mask.frag` 判定式
`occl = nodeD > dF + bias` 两操作数之差 = 遮挡体→接收面光深度距离（正交
= 线性真实距离），半影半径**免费可得**；除原 PCSS 混合路线外存在备选路线 =
mask bit→R8 覆盖率 + 太阳盘 5 采样（中心 + 四向 r 偏移，多遮挡体取 min），
r→0 自动退化回 v1 硬影，不依赖 PCSS 也不依赖阶段 2 预算达标，闭式判据可锚
（半影宽度梯度 = Δz×0.0093）。两路线届时二选一。

**路线改道（2026-10-02 深夜用户拍板，权威）**：同日夜发现 Filament 现行
ShadowType::PCSS 已于 2026-07 整体重写为 **EVSSM**（指数矩 + 8 级 mip 的
O(1) blocker 搜索 + LOD quincunx，生产级作业），经典 poisson PCSS 被其官方
替换；批 11.5 经典 PCSS 全套机制已建成但**冻结不转正**（`af5811a`，门关红线
实证，降级零内存 fallback 候选）。阶段 3 远场软影目标改为 **EVSSM**
（批 11.6 spike 先行 → **docs/evssm_plan.md** 为权威载体）；**HFTS 研究降级
backlog**——分轴对比（同文件 §1.3 + evssm_plan）显示 HFTS 无可替代的
height field 地形自影轴在本项目建筑场景基本无用，而 EVSSM 的 mip 稀释弱点
恰是 IZB 实测强项（细杆锐影 CSM 全丢、IZB 独有 4705px）——**IZB（近场细影）
+ EVSSM（远场软影）= 本路线对 HFTS 混合本意的等价实现**。

### 4.4 预算口径（2026-10-01 拍板）

- roadmap 原 `X = 3 ms` 是 Phase 9（无阴影）时代的余量；现 4 级 CSM 已花 7.10 ms，
  原口径失效。
- **阶段 1：无 ms 门，只量化**（各 pass GPU zone + CPU record 记入下表，落地后填）：

  | pass | GPU (ms) | CPU (ms) | 备注 |
  |---|---|---|---|
  | z-prepass（forward） | **0.000** | **0.000** | **实测 = 零成本**：`izbPreFrame` 判 deferred 才构造 ⟹ forward 下 `IzbShadow` 恒空、零 dispatch。gate_lint R-02 如期 error（但 rc=0 不阻止启动） |
  | IZB 创建（图外 dispatch） | **+0.098** | **+0.009** | 含 bounds + priming。分离实验口径：`shadow_pass=off` 时 mask 段空 ⟹ 增量只剩 create+prime |
  | 光空间保守光栅 + mask | **+1.478** | **+0.699** | **主成本确实在这**（占 IZB 总增量 94%）；重放 shadow0 段 = **341 draws**（= shadow0 全量，独立印证"mask 重放 shadow0"） |
  | 合计 vs 现状 CSM 7.10 ms | **+1.576**（light zone 内）/ Δgpu.frame **+1.64** | 净录制 **+0.993** | 本机 AMD 8060S / Mesa 26.2.3、1280×720 headless、bistro、150 帧窗。⚠ `ΔdrawFrame` 是 +2.70，**多出的 +1.71 是 `cull.fenceWait` 反压不是 IZB 自身成本**（见 `git:调试历史` 债-30 段） |

  > **填表记录（2026-10-04）**：四行"待填"由本轮实测清偿（worker 跑分离实验 + 主 agent 独立
  > 跑配置轴复核：izb on/off → draw 6942/6601、gpu.frame 10.94/9.30、cpu.record 27.03/24.46，
  > 逐项吻合）。**IZB 显存增量 +46.51 MiB**（`vma[first_frame]` 新测点；`vma[startup]` 量不到，
  > 因 `IzbShadow` 首帧懒构造）。`izb_grid` 扫 256/512/1024/2048 **非单调**（512 与 1024 相近、
  > 256 最差），且 draw/vkcmd 完全不随 grid 变 ⟹ 差异是 GPU 侧执行噪声，**不构成调参结论**。
  > 全文 → `git:调试历史` 2026-10-04、`docs/还债总账.md` 债-05。

- **阶段 2 定案候选口径**：① 若 IZB 定位**替换** CSM：总成本 ≤ 现状 7.10 ms；
  ② 若定位**叠加**：增量 ≤ 实测可接受值。口径在阶段 2 实测后拍板并记录于此，
  同步回 `next_roadmap.md` ③。

### 4.5 `izb_rig` 场景生成一致性契约

- **单一真相源 = manifest**（几何 / 颜色 / piece / 相机 / cascadeReference）：
  harness 与 imgqa 判据**只读 manifest，不重实现几何**。
- **字节可复现**：连跑两次 md5 一致（qa_scenes.py 既有纪律：stdlib-only，无 pip
  依赖，float 只经 IEEE754 float32 落盘，min/max 从已 round 字节反读）。
- **生成器内置自检**：坏文件不落盘（头/chunk 对齐 / accessor 边界 / POSITION
  min-max 与真实顶点精确相等 / 绕序⇔法线 / 影包络断言——全部沿用 shadow_rig 先例）。
- **与 12.5（qa_scenes C++ 重写）的关系**：一个场景 = 一个生成源，不并行两套。
  C++ 重写先落地 → izb_rig 直接进 C++ 生成器；未落地 → 先加 Python 版，12.5 时
  整体搬运（场景函数是纯几何构造，无引擎依赖，搬运面 = 几何基元函数）。
  两路 manifest schema 不变，harness 不关心 glb 是谁写的。

---

## 5. 风险清单

| # | 风险 | 对策 |
|---|---|---|
| 1 | ~~保守光栅 × mesh shader × RADV 未实测~~ **已闭环（批 1b 门开）**：OVERESTIMATE 与 EDS3 动态态在 mesh 管线上逐像素闭式解验证通过 | 无 fallback 需求；护栏常驻 gpu 套件防回归 |
| 2 | 无硬件 fattening（`maxExtraPrimitiveOverestimationSize=0`） | 插入/光栅侧软件 1-texel dilation（红线 3） |
| 3 | fragment/compute 全局原子在 RADV 的吞吐未知（IZB 创建是论文里方差最大的 pass） | 1c 直方图 dump + profiler 量化 |
| 4 | 本机 CPU-bound，无 CPU 余量：新增录制/编舞开销落 CPU 侧 | 阶段 1 同步记 cpu.record（§4.4 表） |
| 5 | forward z-prepass 破 BIT-IDENTICAL | 红线 1 的逐位对拍断言兜底 |
| 6 | HFTS 成本天然 ≥ PCSS 1.4×（论文实测） | §4.4 口径按"质量增益 vs 成本"判，不预设"更省" |

---

## 6. 收手判据（沿用 roadmap）

阶段 1 视觉不正确（细影走样/漏光且 fallback 链修不动），或阶段 2 预算口径定案后
冲不进 → **收手**：CSM+PCF 为最终交付；本文档头部状态改「收手」+ 记录收手结论，
`phase11_plan.md` §8 落地记录同步。
