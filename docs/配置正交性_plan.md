# 渲染能力配置 × 渲染图 —— 正交化设计与施工顺序

> **状态**：`现行`（2026-10-16 立，同日 §8 二改开工 → 三改定案 O3 → **四改取消 S1b**）。
> **设计已定稿、拍板点已全部关闭**：A / C 由 §8.0 定案、**B 裁 (b)**（含需求申报 = O3）、
> D 排到 S4（默认不做）、**E 已认**（O3 射程钉 image 侧 **且**不接设备能力轴，与 债-127 并行）。
> ✅ S1a 已落地（并顺带修掉一条先于它的色输入链漏档 `ssr_applied`）；🔴 **S1b 已取消**
> （「装饰器返回带内容结果」当期零消费者 → 先建后找，四条否证见 §8.2.0）；**S2 起改配置面仍需放行**。
> 开工先读 §8.2（四阶段 + S1c 清理刀 + 图事实粒度），再读 §7；**§8 取代初稿的「只做一件事」结论**
> （理由在 §8 开头）。
> 一句话：配置层的依赖图 ≠ 能力本身的依赖图，中间差的那截是**实现细节泄漏**；
> 治法是「一个事实只在它的提供方那里申报」（§5.2 裁决句），不是往 gate_lint 加行。
> 与 债-127 同根因（§5.1），与 `docs/compute_调研.md` §6 D6 的 plan-only 红线**不冲突**（§4）。

任务：为「渲染能力的配置，不正交」做一次设计推演（推演阶段不写代码、不改仓库）。
事实基础 = 同轮只读调研清单（临时产物，**其耐久内容已全部吸收进本文 §1 / §2.2 / 附录 A / 附录 C**，
原始清单不再保留）。**本文所有数字均逐条 grep/read 复算**，复算与清单不一致处
在 §1.2 与附录 A 点名。行号以 2026-10-16 工作区为准。

---

## 0. 问题陈述（项目主人原话，未改写）

> 「渲染能力的配置，不正交。有些很明显互斥的，各种散落，有些不应该依赖的，反而成了依赖。」
> 「之前三种阴影散落的时候我就发现了，但是没想到那么严重。」
> 「我建议基于现有渲染图形状，结合设计下配置。渲染图已经提供了基础能力了，所以利用起来。」
> 「说到底，和 127 也是一个症状。」
> 「plan only 的出厂设置，纳入动态渲染图是否有必要。」

---

## 1. 基线盘点（全部复算，复算方法见附录 B）

### 1.1 现状数字

| 事实 | 数 | 复算方法 |
|---|---|---|
| gate_lint 申报表 | **60 行 / 53 门**（diagnostics 30 行 30 门 + shadow 16 行 13 门 + runtime.meshlet 块 14 行 10 门） | `awk '/const GateRule kGateRules\[\]/,/^};/' rgl/gate_lint_impl.cppm \| grep -c '{"'` → 60；去重门名 → 53 |
| 装饰包装层出声点 | **7 处**（velocity/taa/ssr warn + ao_apply **debug** + picking-object_id warn + highlight warn + shadow 无条件 INFO），全在 `main_impl.cppm` 的 `compileGraphOrThrow`（:398-511） | 清单写「6 处」——picking 与 highlight 是两个独立 warn 点（:483-487、:498-501），按 7 处计；若「picking 一枚键控两个 pass」算一处则 6，口径歧义 |
| meshlet 装配层 warn 点 | **≥10 处**（配对表 11 个根因，其中 meshlet 块占 7 个） | `rgl/main_meshlets.cppm` 十个 wire*/bake* 函数逐数（:105-110 / :216-221 / :225-231 / :240-242 / :246-250 / :278-284 / :421-429 / :499-506 / :566-573 / :695-714） |
| 工具层「参数无宿主」守卫 | **4 处**（`tools/smoke.py:560-566` ×3：--clip 要 aa=taa / fsr 子选项要 aa=fsr / jitter 要 temporal；`tools/imgqa/engine_profile.cpp:357-363` ×1：checkFsrOpts） | 逐处读 |
| 「图里有没有 X」现查点 | **6 处代码位置**（initFsr 内 **5** 个调用点，清单写 4——见 §1.2 差异 ①） | `main_scene.cppm:634-639`（has() lambda）；`vkrenderer_offgraph.cppm:299-307`（findAttachmentSlot×5）；`vkrenderer_offgraph.cppm:100-101`（izbPreFrame depth find_if）；`picking_impl.cppm:22`（object_id）；`frame_capture_impl.cppm:51-52`（swapchain kind）+ `:90-91`（slotOfName） |
| **两处判据对（条件集不同、无测试对账）** | **1 对**：`computeFsrUsable`（main_scene.cppm:632-649，**4 条件**：门+velocityDecorated+shader16+inputsReady）× `initFsr`（vkrenderer_offgraph.cppm:289-317，**3 条件**：只查三个输入附件） | 注释自认「两处判据必须一致，漂移的代价是垃圾画面且不指名」（:625-627）；`grep -rn computeFsrUsable tests/` 无对账用例（仅 smoke.py 正则读引擎日志行做工具侧核对） |
| 图外同步 | **29 处 `.emit(` 全在模块侧**（evssm 3 + gpu_cull 8 + izb 14 + meshlet_cull 4）；边表 **28 条 / 7 道门** | `grep -rn "\.emit(" rgl/*.cppm \| wc -l` → 29；`rgl/offgraph_sync.cppm:148` kSyncGateCount=7；RgSyncEdgeId 枚举 3+13+8+4=28 |
| buffer 附件 | 注册 **4 个名字**（meshletVisibleFlags/meshletCullItems/izb_head/izb_nodes），**进图 3 个**（meshletCullItems 只在注册表），图内真消费者 **1 个**（kAttachmentSyncEdges 恰 1 条 = meshletVisibleFlags） | `grep -rn "registerBufferAttachment(" rgl/`（4 调用点）；`offgraph_sync_impl.cppm:488` `std::array<RgAttachmentSyncEdge, 1>` |
| 图对 buffer 附件零发射 | `UsageInfo::layout = nullopt` 结构性早退（render_graph_usage.cppm:87-90，storageRead/Write 两档） | 读 + 注释 :29-35（planBarrier/planBeginRenderingEntry 拿 nullopt 必须早退 = plan-only 红线做成类型） |
| 图事实谓词（导出面） | `graphHasDeferredLight` 是 render_graph **唯一**导出的图事实谓词（render_graph.cppm:650），生产侧问者 **3 个**（gate_lint_impl.cppm:318、vkrenderer_offgraph.cppm:99 izbPreFrame、:246 evssmPreFrame）；`OffGraphSchedule::isSchedulable`（offgraph_contract_impl.cppm:183）**生产侧零调用者**（只有 tests/test_offgraph_contract.cppm 调） | `grep -rn graphHasDeferredLight rgl/`；`grep -rn isSchedulable rgl/ tests/` |
| RT 槽 | **9 个 ensure 函数**（render_graph_rtslot.cppm 7 个 + vkresources.cppm 的 ensureDepthSampleSlot/ensureFsrSampleSlot），名字与格式吃 render_graph.cppm 的 constexpr 契约常量 | `grep -c "std::expected<uint32_t, Error> ensure" rgl/render_graph_rtslot.cppm` → 7 |
| 校验面 | 附件级 **8 条**（空名/重名/swapchain 计数/格式/尺寸/缩放/历史/buffer 注册，render_graph_impl.cppm:179 注释）+ pass 级 **5 个** precheck 函数（precheckReadUsages/precheckDrawPass/precheckFullscreenPass/precheckBlitPass/precheckPass） | 读 render_graph_impl.cppm:179-316 |
| 动态面 | `DynamicRenderGraph` = markDirty/isDirty/recompile + declareBufferProductions/Consumptions + declarePass/redeclarePass/adoptPass/withdrawPass（dynamic_render_graph.cppm:143-237）；B2-5 单次重编译实测中位数 **4.5µs**（compute_调研.md:338-341，AMD 8060S） | 读 |
| 出厂空开关（宿主门关闭且文件里显式写着的键） | **31 枚**（清单口径复算恰好 31；加 `ui_font`、`meshlet.coplanar` 则 33——见 §1.2 差异 ③） | 组成：fsr×2 + ssr 系数×6 + pcss×3/evssm×4 + izb.grid + rpc_port + ui_font_size + llm_shot×3 + headless_size + scene.taa×9（scene_config_impl.cppm:106-114 九键）= 31 |
| 注释/文档漂移 | 清单 10 条**全部证实**（§1.2 逐条），另新发现 **3 条**（§1.2 差异 ⑥） | 见 §1.2 |

### 1.2 复算发现的差异（清单 → 实测）

① **initFsr 的 findAttachmentSlot 调用点是 5 个不是 4 个**（vkrenderer_offgraph.cppm:299-307：
taaOut / aoApplied / sceneHdr / velocity / depth）。「6 处现查」的总数按「initFsr 算 1 处」仍自洽，子计数差 1。

② **装饰包装层出声点是 7 处不是 6 处**（picking object_id 与 highlight 是两个独立 warn，
main_impl.cppm:483-487 与 :498-501）。清单「6 处」若把 picking 一枚键控两 pass 算一处则成立——口径歧义，本文按 7。

③ **「≥31 枚」复算恰好 31**（清单口径）。两个边界案例清单未计入：`ui_font`（imgui 关时无消费者，
与 ui_font_size 同形）和 `meshlet.coplanar`（overlay 关时无消费者，但 wireMeshletOverlayDepthCompare
每次启动仍打一行 INFO「生效档 = off」——出厂日志里**有一行**关于无消费者档的陈述）。计入则 33。

④ **meshlet.cull 的「编码泄漏」定性不准确**。消费者 `wireMeshletCull` 读的是**生效事实**
（`ctx.contains<MeshletSwitchOn>()`，main_meshlets.cppm:709；:701-708 注释明确论证「读生效事实
而不是配置意图」——switch 的三条回落路不写配置，读配置会让剔除在回落档下照跑）。这是**正确形态**，
不是泄漏。真正的泄漏在 `switch` 键本身：它的三条回落路（设备规格 / 非 cpu cull 档 / 逐资产对账失败，
:210-232）使「配置说 switch 开」≠「switch 生效」。本文把 cull 改归 ③（正确申报 + lint），泄漏记在 switch。

⑤ **meshlet.debug/tint 的漂移比清单说的更严重一层**。清单：gate_lint 消息说「或 enable=false ⟹
强制归 base」而 pickMeshletMeshVariant 只判 overlay（main_meshlets.cppm:136-144）——**证实**。
复算补充：lint **谓词** `meshletDebug && (!meshletEnable || !meshletOverlay)`（gate_lint_impl.cppm:237-238）
的 `!enable` 半边同样不是消费者的判据。`enable=false × overlay=true × debug=true` 组合下：lint 喊
「强制归 base」，代码实际烘 debugOffset 变体——但条目烘焙在 `!slots` 处早退（:440-441），所以
**孤立结论（键无消费者）对、机制描述（谁把它归 base）错**：归 base 的不是 pickMeshletMeshVariant，
是烘焙早退。lint 申报了一个消费者不存在的机制。

⑥ **新发现的漂移 3 条**（清单 10 条之外）：
 - `main_impl.cppm:412`：decorateAoApplyOrWarn 注释「门 = **FSR 门开**（`runtime.fsr_upscaler`）」
   ——旧键名（14.6 已删，现在是 aa=fsr）。
 - `runtime_config.cppm:480-482`：meshletSwitch 三态语义注释「`enable && !switch` → α 形态：旧路径
   照画 + **共面影子叠加**」——债-105 后叠加段有独立门 overlay（默认关），此描述过期（同文件
   :378-381 的 meshletEnable 注释已更正，switch 注释没跟到）。
 - `configs/runtime.lua` meshlet 块头注释：「★1-a1ii-α 起 enable 开 = 恒 L0 簇**影子叠加**
   （同 objectIndex 重画一遍……）」——同样过期于债-105（「叠加段现在有自己独立的门，默认关」）。

（清单原 10 条全部证实，从略；其中第 5 条 diagnostics.lua:49-52 的 dgc_static「静默回落 false」
注释与 meshlet 块 readStrictBoolKey 的严格纪律并存 = 同一份配置面两种布尔纪律，属实。）

### 1.3 一个清单没写、但推演要用的事实

**「出厂 0 findings」是谓词推断，不是运行结果**（清单自己注明；本文同样未运行程序——见 §6）。
出厂态下逐行过 60 行表：唯一 true 的门是 meshlet.enable（nullptr 谓词）与 shadow.mode=csm
（非 evssm/none），variant 交叉校验 variant=deferred × deferredLight=true 不触发 ⟹ 0 findings。

---

## 2. Q1 尺子：一枚配置键分到四类之一

### 2.1 判据（可复用、可落到代码）

对键 K，**先找它的消费者函数**（读它的 wireXxx / decorateXxx / fillXxx / 解析期推导点），然后按序问：

**问 1：消费者的早退/门控条件读的是什么？**（看函数体前几行 + 调用链上的条件）

| 读到 | 前置属于 | 归类 |
|---|---|---|
| 另一枚**配置键的值**，且组合存在**非法形态**（互斥/让位/无宿主子键） | 编码 | **①** 类型/枚举消灭（非法组合写不出来；先例 = shadow.mode 四值、aa 三值——「让位链不是被修好，是被消掉」shadow_config.cppm:109-110） |
| 另一枚配置键的值，且组合**全合法**、只是某些子组合**无消费者** | 能力定义 | **③** 留在配置层 + gate_lint 申报（前置 = 能力本身的定义，如 fsr.temporal 要 aa=fsr——「FSR 子档」是 FSR 的定义，不是泄漏） |
| `graph.attachments` / `graph.passes`（按名/kind 现查）或「已装饰」标志（taaDecorated 等） | **图形态事实** | **②** 该由**图**回答（谓词面；先例 = graphHasDeferredLight 3 问者、「已装饰」标志 = 图事实的合法投影） |
| **装配期产物**（slots/entries/载体 != nullptr、registry ctx 生效标记） | 资源分配 | **④** 从配置层消失（键名若是「总门」实为资源宿主 → 位置错了：宿主条件归装配层事实，配置层只申报「要不要这个能力」） |
| 设备能力（shader16Supported / staticGraphicsIndirect） | 设备事实 | **③** 的设备半边（GateEnv 先例，lint 入参） |

**问 2：键的申报（字段注释/设计记录）与消费者的真实门控是否一致？**（注释与消费者代码逐行对）
不一致 → 归类以**代码**为准（本仓纪律「照源码写，不照注释写」）；注释进漂移清单。
典型：`velocity_field` 申报「非 FSR 消费者——TAA 逐像素重投影线要它」（runtime_config.cppm:304-307、
configs/runtime.lua:28-30），消费者事实 = TAA 重投影是**解析的**（taa.frag.slang:783 自陈「本项目没有
velocity buffer 是因为不需要」），引擎内真消费者只有 FSR（+ RPC 按名回读）⟹ 「独立键」裁决建立在
**假前提**上 ⟹ **④**（独立性应撤并，见 §7 拍板 C）。

**问 3：键是不是实现未完的细节**（路没接 / 载体零消费者）？
是 → **④**（删或降级，「宁删不留假能力」D3 先例），不是挂号。

**判据冲突时的让位**：① > ② > ③ > ④。能枚举消灭的不许 lint 申报（申报一个写不出来的形态 =
假护栏，phase14_plan.md:1380-1381「枚举比守卫强」）；前置是图事实的不许在配置组合谓词里重述
（= 6 处现查的漂移源）；只有纯能力定义进申报表。

### 2.2 六块逐枚分类结果

**meshlet（10 键）**

| 键（出厂值） | 类 | 依据（消费者 file:line） | 处置 |
|---|---|---|---|
| enable (true) | **④** | 本体 = 资源分配：4 段簇 buffer + 描述符槽 + 条目烘焙宿主（wireMeshletBindings main_meshlets.cppm:269-319）；块内 9 键的宿主条件**不统一含它**（overlay/mdi_shadow 不含、debug/tint 经条目间接含）= 「总门」是假象 | 从「总门」位降级为资源门（改名或改注释 + 逐键宿主条件注释修正）；拍板点 A |
| overlay (false) | **①** | 与 switch 互斥，现靠运行期抑制 + 两层各喊一次（lint gate_lint_impl.cppm:265-270 要 overlay=true；装配 :246-250 switch 生效就喊——**出厂 overlay=false 开 switch 时只有装配层喊，lint 沉默**，配对表证实） | 与 switch 合一枚 `meshlet.mode` 三值（off/overlay/switch） |
| debug (false) | **①** | 两枚布尔表达 base/debug/tint 三选一（pickMeshletMeshVariant :136-144 只判 overlay） | 与 tint 合一枚三值键 |
| tint (false) | **①** | 同上 + 让位规则（lint :244-248「tint 优先、debug 让位」——让位是两布尔的产物，三值后消失） | 同上 |
| coplanar ("off") | **③** | 已是三值枚举 ✓；前置 = overlay 有消费者 = 纯配置组合（lint :249-255 在表） | 不动 |
| switch (false) | **①**+**③**+**④** | ① 互斥 → mode；③ 前置 cull_replay==cpu（配置组合）+ 设备规格（设备事实）（lint :256-270 在表）；④ 三条回落路（:210-232）使声明≠生效 = 装配事实——**读 ctx 生效标记（:234）是正确形态，别改成读配置** | mode 三值；回落路保留在装配层 |
| cull (false) | **③**（清单「编码泄漏」定性改） | 消费者读**生效事实** ctx MeshletSwitchOn（:709）= 正确形态（:701-708 论证）；lint 2 行在表（:271-281） | 不动 |
| shadow_lod (5) | **④**+**③** | 宿主 = enable 的 slots = 资源（:400-404、:487）；D≥0 档 = 能力定义 | 键保留；「宿主缺失」申报（lint :282-286）在 O3/O4 形态下由图/装配层接管后退役 |
| shadow_lod_fallback ("deepest") | **③** | 前置 = shadow_lod≥0（配置组合）；lint inertNote 在表（:287-291） | 不动 |
| mdi_shadow (false) | **③**+**④** | 键本身 = MDI draw 形态开关（能力定义，生效条件**不含本键** = 正确，:460-467）；④ 在**载体**：出厂也建 MeshletShadowArgsService（GPU buffer，零消费者）= 资源分配泄漏在载体构造条件，不在键 | 键保留；载体构造条件是否收进门 = 拍板点（§7，代价很小：sliceRows×24B×2 ≈ 数百 KB） |

**shadow（回显表 13 键）**

| 键（出厂值） | 类 | 依据 | 处置 |
|---|---|---|---|
| mode ("csm") | **①** 已完 + **②** | 四值枚举（N6b，shadow_routes_plan.md:186-190）；evssm 档要 deferred = 图事实，lint 行**已经读 graphHasDeferredLight**（gate_lint_impl.cppm:131-133）= **已正确的形态样板** | 不动（O2 只是把谓词源统一） |
| izb (false) | **②**+**③** | 要 deferred 图（lint :116-118 读图事实 ✓）；izb×mode="none" = 声明组合无内容（:119-122 error ✓）——注意 mode=none 时 shadow0 段**仍在图里**（decorateShadowPass 无条件，main_impl.cppm:507），只是段内零 draw ⟹ 这是**申报**不是图形态，留在 lint 是对的 | 不动 |
| izb.grid (512) | **④** | 宿主 = IZB 构造（网格随构造才存在）；已嵌套块 ✓；lint inertNote 在表（:162-164） | 不动 |
| izb.dump_frame (0) | **④** | 同上；lint warn 在表（:154-156） | 不动 |
| pcss.*×3 / evssm.*×4 | **①** 已完 | 按档分立 = 「两门同开」表达不出来 ✓；死值申报 = 7 行 inertNote 在表 ✓ —— **O1 推广的模板**（shadow.lua 已是正交化的完成形态） | 不动 |
| csm ({}) | **③** | 预留空块（nullptr 谓词 ✓） | 不动 |

**fsr（2 键）**

| 键（出厂值） | 类 | 依据 | 处置 |
|---|---|---|---|
| fsr.temporal (true) | **③**+**②** | 能力定义：FSR 子档；前置 aa=fsr = 纯配置组合，**现不在 lint 射程**（readSsrBlock:315-327 的「其余 runtime.lua 键不在表里，本批没有欠账」裁决覆盖 fsr.*）→ 出厂零引擎出声，只有工具层 2 处守（smoke.py:563-566、imgqa:357-363）；更深的「FSR 接得起来」= velocity 在图 = 图事实，现 ad-hoc 现查（computeFsrUsable:634-639），warn 在（:641-647） | 键保留；宿主申报归拍板点 D；「接得起来」归 O2 谓词 |
| fsr.jitter (true) | 同上 | wireFsrJitter main_scene.cppm:132 `env.fsrJitter = fsrUsable && fsrJitter` | 同上 |

**ssr（7 键）**

| 键（出厂值） | 类 | 依据 | 处置 |
|---|---|---|---|
| ssr.enabled (false) | **②** | 装饰四判据全是图形态：锚点（写 swapchain 且采色）/ G-buffer 契约表**全通道**（deferred-only 的真实形态）/ 借可 store 的 depth / 次序（taa、ao_apply 不在图）（decorateSsrPass render_graph_decorate.cppm:513-557）；包装层 warn（main_impl.cppm:462-466） | O3 形态 = 需求声明进图、compile 期拒 |
| ssr.*×6 | **③** | 前置 = enabled（配置组合）；**无条件写进 scene data** 是设计决定（尾区纪律：不按门分支、无消费者不改像素，fillSsrParams render_list_impl.cppm:497-501 + 注释 :495-496）——参数本身不需要前置校验；「enabled=off ⟹ 6 枚死值」的申报 = **刻意不挂号**（readSsrBlock:315-327 裁决，理由成立：不造第三张回显表） | 键保留；死值申报归拍板点 D |

**wet（6 键）**

| 键（出厂值） | 类 | 依据 | 处置 |
|---|---|---|---|
| wet.*×6（strength 出厂 1.0 = 开） | **②**/**③** 混合 | 消费者住 deferred_geometry frag ⟹ **只有 variant==deferred 时被选中** = 图形态事实；**现无人申报**（无 lint 行、无引擎 warn、不在 readSsrBlock 同款裁决的显式名单里——裁决文字只点了「本块」，wet 块是同款理由但没写）= 六块里**真正的申报缺口** | 加 1 行 lint（variant≠deferred × wet 偏离默认）或归 O2 谓词；见拍板点 D 的范围 |

**taa（9 键）**

| 键（出厂值） | 类 | 依据 | 处置 |
|---|---|---|---|
| taa.*×9（clip_mode=13 等） | **③** | 前置 = aa==taa（配置组合）+ 装饰成功（图事实，taaDecorated 标志 = 合法投影，main_scene.cppm:707）；scene.lua:288-291 自陈「本键不是交付默认」；引擎内零出声，只有 smoke.py:560-562 守 --clip | 键保留（跨文件所有权：taa 块在 scene.lua、mode 在 runtime.lua——形态弱点，见 §3 O1-1.3）；死值申报归拍板点 D |

---

## 3. Q2 方案对比（O0–O4）

### 3.0 每个方案的「能消掉什么」（具体数字，基线见 §1.1）

| 消掉的对象 | O0 不动 | O1 纯配置正交 | O2 +图谓词面 | O3 +需求进图 | O4 开关=图操作 |
|---|---|---|---|---|---|
| gate_lint 行 | 0 | **−3**（tint 让位行、switch×overlay 互斥行、debug+tint 两行并一行）；**+6~31**（31 枚死值申报，拍板点 D：块级 6 行需扩护栏口径 / 逐键 31 行现有口径 / 不补 0 行） | 0 | **−3**（izb×非deferred、evssm×非deferred、dump_gbuf×deferred 三行图形态行 = 编译期已拒 ⟹ 死代码删除；门数 53→50） | 同 O3 |
| 包装层出声点（7 处） | 0 | 0 | 0 | **−7**（warn/debug/INFO → 图 compile 期诊断；消息形态变 ⟹ smoke.py 正则同步） | 同 O3 |
| meshlet 装配层 warn（≥10 处） | 0 | **−1**（switch×overlay 互斥 warn main_meshlets.cppm:246-250，含 coplanar 第二句） | 0 | 0 | 0 |
| 工具层守卫（4 处） | 0 | 0（守的是 CLI 旗标不是配置键） | 0 | 0（但消息形态变 ⟹ 工具同步改） | 0 |
| 图事实现查点（6 处） | 0 | 0 | **−6**（全部改吃 render_graph 谓词面） | −6 | −6 |
| 两处判据对（1 对，条件集 4 vs 3 不同、无测试对账） | 0 | 0 | **−1**（统一成一个 `fsrInputsReady(graph)` 函数） | −1 | −1 |
| **可写出的非法形态** | 0 | **−2**（tint×debug 让位、overlay×switch 互斥 = 写不出来） | −2 | −2 | −2 |
| 出厂空开关（31 枚）的引擎零出声 | 维持 | 视拍板 D：+6 行（块级，debug 级，出厂不可见）/ +31 行 / 0 | 同 O1 | 同 O1 | 同 O1 |

### 3.1 O0：什么都不动（基线）

只继续往申报表加行（每加一枚键加一行，覆盖率护栏逼着回答前置）。

- **消掉**：无。
- **新增的税**（诚实记）：60 行表继续长；配对表 11 个根因继续「同根因多层各喊一次」
  （出厂 overlay=false 开 switch 时 lint 沉默、装配层喊——判据不同 = 两层各说各话）；
  31 枚出厂空开关继续引擎零出声（唯一守卫 = 工具层 2 处，第四层出声点未查清 = 再出现再守一遍）；
  两处判据对（4 vs 3 条件、无对账）留在热路径；漂移 13 条（10+3 新）继续累积。
- **与 D6 三条坚持理由**：不碰。
- **迁移代价**：0。
- **验收判据**：无（基线）。

### 3.2 O1：纯配置层正交化（图不动）

形态 = 把 **shadow.lua 已是完成态的形状**（mode 枚举 + 按档参数块 + lint inertNote 申报）
推广到 runtime.lua / scene.lua：

1. **O1-1.1** `meshlet.debug` + `meshlet.tint` → 一枚三值 `meshlet.variant ∈ base|debug|tint`
   （aa 三值先例的复刻）。让位规则（tint 优先）结构性消失。
2. **O1-1.2** `meshlet.overlay` + `meshlet.switch` → 一枚三值 `meshlet.mode ∈ off|overlay|switch`。
   互斥结构性消失；switch 的三条回落路（装配事实）原样保留。
3. **O1-1.3** 参数嵌进所属模式块。fsr 块现是 `aa` 的**平级**块（runtime.lua 内 `fsr = {...}` 与
   `aa = "off"` 并列）→ 激进形 = `aa = { mode = "fsr", temporal = true, jitter = true }` 表形态
   （mode=off 的表里没有 temporal 字段可写 = 「键在但模式关」形态消灭）；taa 块在 scene.lua、
   mode 在 runtime.lua = **跨文件所有权**，保守形 = 块不动、只加死值申报；ssr 块已含 enabled
   （模式在块内）= 只差死值申报。
4. **O1-1.4** `meshlet.enable` 拆「总门」名（拍板点 A）。

- **消掉**：lint −3 行；装配 warn −1；非法形态 −2（上表）。
- **新增的税**（诚实记）：**配置面破坏性** = 键名变更，下游引用面 = tools/smoke.py（--set 语法、
  正则）、imgqa profile、`qa/configs/**`、docs——本仓纪律「无扁平键双读兼容」= 硬切，无迁移期
  缓冲；aa 表形态若做 = 更大的破坏（解析器 + 工具 + 锚点配置全动）。**锚不动**：95edc8cc（出厂
  = variant=base + mode=off = 全默认）与 54e40665（门关 = enable=false + shadow_lod=-1）的图形状
  都不变 ⟹ 两锚逐位不变是构造性的。启动时序：不变（解析期 + 装配期）。Lua 词表 / kind 加档税：0。
- **与 D6**：不碰（纯配置层 / 申报层）。
- **分阶段路径与独立验收**：
  - 1.1 variant 三值：验收 = 出厂锚 95edc8cc 逐位 + 四臂判据（coplanar off/equal/ne）绿 +
    lint meshlet 段 3 行→2 行 + `tests/test_gate_lint.cppm` 覆盖率绿（键名变 ⟹ knownKeys 表同步）+ `just gates`。
  - 1.2 mode 三值：验收 = 95edc8cc 与 54e40665 两锚逐位 + switch 三条回落 warn 逐字保留 +
    lint 互斥行删 + smoke/imgqa 引用同步 + `just regress selftest`。
  - 1.3 aa 表形态（含 taa 死值申报）：验收 = **纯空间的两档**逐位（`aa=off` / `aa=fsr` 且
    `temporal=false`）+ `aa=taa` 档走 imgqa 统计判据（🔴 带时域历史的档**不做位比**，
    初稿在这里写的「三档锚逐位」含红线 B `360ce0d2` = 时域档，判据口径错）+ smoke `--set aa=…`
    工具同步 + 解析期「mode=off + 子字段在文件里出现」出声（inertNote，见 §5.3）。
  - 1.4 enable 改名：验收 = 两锚逐位 + tools/qa/docs 引用零残留（`grep -rn "meshlet.enable" tools/ qa/ docs/`
    只命中历史记录）+ 覆盖率护栏绿。

### 3.3 O2：O1 + 图作为图事实的唯一提供者

把 6 处「图里有没有 X」现查收进 render_graph 的谓词面（现成先例 = graphHasDeferredLight
（3 问者）与 swapchainColorConsumerIndex（4 装饰器共用，render_graph_decorate.cppm:88-101））：
新增 `hasAttachment(name)` / `hasPassName(name)` / `hasPassState(kind, state)`（或同等形态），
装饰包装层、computeFsrUsable、initFsr、izbPreFrame、picking、frame_capture 全改吃它。

- **消掉**：6 处现查 → 谓词面；**两处判据对合一**（fsrInputsReady(graph) = 4 条件全部变成
  谓词调用，initFsr 与 computeFsrUsable 共享同一函数 ⟹ 「漂移」结构性不可能）。
- **新增的税**（诚实记）：谓词面 API **常驻**——isSchedulable 是现成的反面教材（查询面建了、
  生产侧零调用者 = 机制常驻收益悬空，offgraph_contract_impl.cppm:183 只有测试在调）⟹ 验收必须
  钉「每个谓词切走时都有调用者」，不许「先建后找」。render_graph 模块职责扩大（仍零发射、
  纯查询——不碰 plan-only）。lint 侧：签名不变（已吃 `std::span<const RgPassPlan> passes`），
  Ctx.deferredLight 的计算源从调用方换成谓词函数 = 零行为变化。
- **与 D6 三条坚持理由**：逐条不碰——谓词是只读查询；「模块拥有录制」不变（查询不产生命令）；
  「半发」不变（图对 buffer 附件仍一条命令不发，nullopt 早退不动）；「layoutDrift」不变
  （模块仍返回真实离开布局，图不代发不代报）。
- **迁移代价**：配置面 0；锚 0（纯查询，图形状不动，逐位不变是构造性的）；启动时序 0；
  Lua 词表 0；kind 加档税 0。**这是五个方案里唯一迁移代价全零的。**
- **分阶段路径与独立验收**：
  - 2.1 谓词面 + 单测：验收 = 每谓词一正一反用例（无设备毫秒级，进 `just test`）+ `just gates` 绿。
  - 2.2 切走 6 处现查：验收 = 原 6 处 grep 命中 0（谓词函数是唯一读 `graph.attachments` 做
    「存在性」判定的地方，`grep -rn "find_if(renderGraph.attachments" rgl/` 只剩谓词本体）+
    两锚（95edc8cc/54e40665）逐位 + `just regress selftest`。
  - 2.3 两处判据对合一：验收 = 变异红线——把 fsrInputsReady 的一个条件删掉 ⟹ 启动期行为必须变
    （fsrUsable 假 + warn 行出现，smoke.py:899-909 的核对看见），或结构性共享同一函数 = 变异写不出来。

### 3.4 O3：O2 + 能力开关的前置申报进图

装饰请求携带「我要什么图形态」（RgDecorationRequirement：锚点角色 / 必需附件 / 「不得在图里」
名单 / 借用 depth 需求），图 compile 期校验，非法形态 **compile 期拒**（error，不是 warn）。
图级需求（「本图必须有 deferredLight」）与 pass 级需求（装饰）两档。

🔴 **定案（2026-10-16 用户裁：「失败不降级不是好事吗？少一堆动态检查呢」）**：**「失败不降级」是
本方案的收益本体，不是风险**。初稿把「warn 降级」当成要保护的既有行为，方向错——那套降级
**正是本债的病**（「配了却没生效且不指名」），而哲学 4 写的是「让错的东西编不过 > 让错的东西被看见」。
下面 3.4.0 把"少一堆动态检查"落到具体删除清单，3.4.2 钉住唯一不许进射程的那根轴。

#### 3.4.0 「不降级」删掉的不是日志，是**一整个决定**

| 被删对象 | 它今天为什么存在 |
|---|---|
| 包装层 7 处出声点 | 装饰失败要有人喊 |
| `computeFsrUsable` 的 `inputsReady` 那半 | 装配层要决定「接不接 FSR」 |
| `Renderer::initFsr` 的「两处判据漂移」throw | 两处判据要一致 |
| 🔴 **「两处判据必须一致」整个问题类别** | 不合法就起不来 ⟹ **没有第二处** |
| smoke.py 的「非错误」降级分支（`aa="fsr" 开门但接不起来`） | 降级是合法结局 |
| `CompiledGraphResult` 4 枚 `*Decorated` | 装配层拿它做 bool 决定 |

⚠ **S1a 的 `RgFsrInputs::complete()` 在这张表里占两行**：它把两处判据合成一个本体，而 O3 的终局是
**装配层那一半根本不该存在**。合一不是白做（它是"两处确实一致"的机器证明、本轮逐位不变的载体，
而且正是它让 `965f146` 那条 `ssr_applied` 漏档**只有一处要改**）。⟹ 由此推出的结论是
**「装饰器返回带内容的落图结果」整项撤销**：4 枚 bool 会随这张表一起消失，给它们加内容是给
一个要删的东西装修（论证四条 → §8.2.0 L3 撤销块）。

#### 3.4.1 「拒」不等于「崩进程」——动态路径早就有这套语义

图在运行期**会变**（B2-5：`declarePass` / `withdrawPass` / `redeclarePass` 在 drawFrame 安全点消费
意图，见 `Renderer::withdrawPassTransaction`），所以需求校验不是"只在启动期跑一次"。但这**不需要
新造失败形态**：那批 API 已经是「试编译 fail-fast、失败**不落账**直接返回 Error」。⟹ O3 的需求校验
长在**同一个 compile** 上，两侧各得其所：启动侧 = 起不来；运行侧 = 事务失败、图不变、出声。
两侧共享的是同一句判据：**不存在「带着非法图继续画」这个状态**。

#### 3.4.2 射程的第二条边界：前置分两轴，O3 只取图形态轴

`computeFsrUsable` 今天把四个条件 AND 成**一个 bool**，而这四个条件住在两根不同的轴上——这正是
§2 的 ① 类成因（两条正交量揉成一个量），所以这条不是"额外小心"，是**本债在同一个函数里的另一例**：

| 条件 | 属于哪根轴 | O3 处置 |
|---|---|---|
| `fsrEnabled` | 配置意图 | 不是前置，不进需求 |
| `velocityDecorated`（S3 之后 ≡ `velocityEnabled`，见 §3.4.0） | **图形态** | ✅ 进需求，compile 期拒 |
| `inputsReady`（`graphFsrInputs`） | **图形态** | ✅ 进需求，compile 期拒 |
| `device.shader16Supported()` | **设备能力** | 🔴 **不进** |

🔴 **设备能力轴不许进 O3**：`aa=fsr` 在没有 `shaderFloat16` 的机器上不是配置错，是「这台机器跑不了
这份配置」。做成 compile 期拒 = 同一份配置换台机器起不来，直接撞 AGENTS 的「工作机器不著书」
（三机轮换）。而且它**已有既定归宿**：`GateEnv` 里就带着一枚设备能力（`staticGraphicsIndirect` =
dgc_static 的门槛）⟹ 设备事实走 GateEnv 是既有裁决，不是新开的口子。

⟹ 需求类型要带的不是含糊的「硬 / 软」标记，而是**「这条前置属于哪根轴」这个字段本身**；
只有图形态轴参与 compile 期拒。

- **消掉**：包装层 7 处出声点 → compile 期诊断；lint 3 行图形态行（izb×非deferred / evssm×非deferred /
  dump_gbuf×deferred）死代码化删除（组合在 compile 期就过不了 ⟹ lint 永远收不到该输入）；
  现查 6 处（**已由 S1a 拿到**）+ 判据对 1 处（终局见 3.4.0）。
- **新增的税**（诚实记，按严重度不按成本排）：① 需求面是**类型**——每次新增装饰要写一份需求
  （加档税，与 kind 加档税同族，**持续**不是一次性）；② 需求×图形态矩阵 = 7 装饰器 × 3 变体 =
  **21 格用例**，且每格要配反向断言（本仓纪律：写不出反向断言的护栏不配进门）；③ **消息形态迁移**
  = smoke.py 的正则钉着引擎消息（`resolveFsrTemporal` 注释自认「文本格式被 smoke.py 的正则钉住」）
  ——warn→error 要工具层同步，漏一处 = 工具静默失配。
- ⚠ **一条诱惑要单独写出来**：O3 做顺之后，「既然需求都进图了，barrier 干脆也让图发」是下一步的
  自然动作，而那一步会让 `layoutDrift` 那类对账**变成恒真**——本项目唯一一类「加了东西反而让判据
  失效」的形态。**射程因此是两条，不是一条**：①不碰 buffer 附件注册面（127，§5.4 拍板 E）；
  ②不接设备能力轴（3.4.2）。两条都写进裁决句。
- **与 D6 三条坚持理由**：逐条不碰，且 O3 正是 AGENTS 哲学 3（「申报 ≠ 发射权：申报面只拿
  计划 + 校验，barrier 与真实布局仍由模块给」）的**既有形态强化**：(1) 模块拥有录制不可约——
  申报只改「图何时拒」，录制仍归模块；(2) 半发——图仍零 buffer barrier（nullopt 早退不动）；
  (3) layoutDrift——模块仍申报真实布局，图不代报。
- **迁移代价**：配置面 0（键不变，需求在 C++ 装饰请求里）；锚 0（合法路径逐位不变；非法组合的
  启动报错消息变）；启动时序：装饰的 skip 条件从「装饰函数返回 false」挪到「compile 校验拒」
  （同一时期、同样无设备、同样 fail-fast，时序语义不变）；Lua 词表 0（O3 不要求 Lua 表达条件
  pass；将来要表达 = 另一次词表加档）；kind 加档税 0。
- **分阶段路径与独立验收**（在 O2 之上）：
  - 3.1 需求类型（**带"属于哪根轴"字段**）+ 单测：验收 = 需求×图形态矩阵用例（deferred/forward/
    offscreen × 7 装饰器）+ 🔴 一条「设备能力不在需求里」的反向用例（变异：把 `shader16Supported`
    塞进需求 → 用例红；钉住 3.4.2，防下一轮"顺手把设备也申报进来"）。
  - 3.2 7 个装饰器的 skip 条件 → compile 校验：验收 = 非法形态 = compile error 点名需求项
    （变异：把 ssr 需求里删一个 G-buffer 通道 → 装饰被拒且消息点名该通道）+ 包装层 7 处出声点
    删除 + smoke.py 正则同步 + 两锚逐位 + 🔴 **运行侧那一半也要有判据**：构造一个会撞需求的
    `declarePass` → 试编译拒、**图不变**、`checkEmission` 不红（钉住 3.4.1 的"两侧同一语义"；
    只测启动侧 = 动态图那条路径的校验其实没人验过）。
  - 3.3 lint 3 行删除：验收 = 变异「构造非 deferred 图 + evssm 配置」→ compile 拒（lint 不可达）
    + 覆盖率护栏门集合同步（53→50）+ `just gates`。

### 3.5 O4：开关本身 = 动态渲染图操作（出厂设置纳入动态图）

feature gate → `declarePass`/`adoptPass`/`withdrawPass`（B2-5 已落地的 API，dynamic_render_graph.cppm:170-237）；
出厂设置 = 动态图的首次编译。

- **消掉**：O3 的全部 + `CompiledGraphResult` 的 4 个「已装饰」标志（taaDecorated/velocityDecorated/
  aoApplyDecorated/ssrDecorated，main.cppm:87-102）→ 图自己知道（谓词查询）；装饰顺序契约
  （「TAA 必须最后」「velocity 先于 TAA」「SSR 在 shadow 后、velocity/taa/ao_apply 前」，
  现只活在 main_impl.cppm:512-536 的注释与调用序里，代码接管的只有 ssr/ao_apply 两条 skip）
  → 声明序 + compile 校验（结构性）。
- **新增的税**（诚实记）：**启动时序契约变更** = 当前 compileGraphOrThrow 是启动期纯函数
  fail-fast（Renderer 构造之前）；O4 = 出厂设置走动态图，启动预编译要**统一两个编译入口**
  （静态 compile + 动态 recompile 并存 = 双源，本仓最忌的形态）；重编译成本已实测（单次中位数
  4.5µs，D5 落地读数 compute_调研.md:338-341）= 低频开关可接受，但「逐帧」是能力不是承诺
  （D5 定案第 5 条）。**诱惑管理**：D5 定案原文警告「图现在什么都知道了，顺手代发 barrier 吧」
  的复发概率**上升**——O4 把「开关生效」与「barrier 发射」放进同一个 API 家族，只隔一个方法名
  的距离。
- **与 D6 三条坚持理由**：发射权不搬则三条不变（pass 级 declare/withdraw 不产生任何命令——
  图 compile，模块录制）；但 O4 是**离红线最近**的方案，验收必须含：「动态面不新增任何发射
  入口」红线 + 判据 6 变异（模块谎报真实离开布局 → 帧末对账必须炸；测不出红 = 红线已破 =
  回退形态，compute_调研.md:400-401）。
- **迁移代价**：配置面 0（开关仍在 Lua，C++ 接线变）；锚 0（出厂图形状不变 = 动态图首编译，
  95edc8cc/54e40665 逐位）；启动时序：**变**（上面已述）；Lua 词表：0（出厂设置 = C++ declarePass；
  若要把出厂设置写进 Lua 图配置 = 词表加档，而 extent/extentScale/history/viewport/buffer 字节数
  今天都表达不了（render_graph_config_impl.cppm 只读 name/kind/format + pass 的 name/kind/
  state/draws + 引用 7 种 usage）——**Lua 半边扩词表是 O4 独有的潜在税**，O3 之前不触发）；
  kind 加档税 0。
- **分阶段路径与独立验收**（在 O3 之上）：
  - 4.1 出厂设置 = 动态图首编译（统一编译入口）：验收 = 启动 fail-fast 语义不变（非法形态启动
    即错，不是首帧）+ 两锚逐位 + `just cpu` 启动时序读数不漂（口径 → cpu_perf_plan.md）。
  - 4.2 开关 → declarePass/withdrawPass（aa/ssr/velocity/picking）：验收 = A→B→A′ 往返逐位
    （B2-5 既有判据）+ RPC /param 开关可用 + 判据 6 变异红。
  - 4.3 开关前置 = 重编译拒：验收 = 变异「切到非法组合」→ 重编译失败 + 脏位保留 + 本帧继续
    用旧图（B2-4③′ 失败形态 vkrenderer_impl.cppm:230-234 不变）+ 下一安全点重试语义不变。

### 3.6 五方案一句话总评

- O0：税照付，漂移照积，唯一「收益」是不动。
- O1：消灭 2 个可写非法形态（哲学上最对），但付配置面破坏性，且 31 枚空开关的处置被它
  推成「表 +6 或 +31 行」——**正交化做了一半（形态消灭）、另一半（申报）转嫁给拍板点 D**。
- O2：迁移代价全零，消灭唯一一处「无对账、无外部兜底」的热路径漂移风险（两处判据对），
  且是 O3/O4 的地基。
- O3：**定案要做**（2026-10-16 用户裁「失败不降级是收益，少一堆动态检查」）。形态最强：非法组合
  compile 期拒，而且它删的是**一个决定**不是几行日志（→ §3.4.0 六行删除清单，含「两处判据必须一致」
  整个问题类别）。税 = 需求类型（要带**轴字段**）+ 21 格需求×图形态矩阵用例 + 消息形态迁移。
  🔴 射程是**两条**：不碰 buffer 附件面（127）、不接设备能力轴（§3.4.2）。
- O4：出厂设置入动态图 = D5 定案的顺理成章延伸，但启动时序统一与诱惑管理是**新**税，
  且其增量收益在 O3 之后已经不大（🔴 2026-10-16 三改：原记「4 个标志消掉 + 顺序契约结构化」
  那半收益**由 O3 顺带拿到**——裁 (b) 之后 `xxxDecorated ≡ xxxEnabled`，标志自然消失；
  ~~已由 S1b 拿到~~ 的说法随 S1b 一起作废，见 §8.2.0 撤销块。O4 剩下的增量只有「开关走
  declarePass/withdrawPass」，而那半正是被 D6 三条理由否决的）。

### 3.7 O5：Lua 侧「子图供引用」（已否决，2026-10-16 用户提出后复核）

提案：谓词颗粒度太大，改成 Lua 侧可被引用的子图，让这次重构简单些。
**否决**。三条理由里前两条是设计，第三条是顺手纠正一条被当论据用的过期说法。

1. **装饰不是「追加一段 pass」，是「给别人的 pass 打补丁」**。看 `render_graph_decorate.cppm`
   实际在做什么：`swapchainColorConsumerIndex` 动态找锚点（「写 swapchain 且采场景色的那个 pass」）、
   `borrowDepthFromPriorWriter` 改**前一个 depth 写者**的 `storeOp`、`RgStoreOpPatch::originalStoreOp`
   记原值以便撤回、插完还要改写**消费者的 inputs**。Lua 词表现今只有
   `name/kind/format/state/draws/outputs/inputs/usage/load/store/clear`；要表达这四件事就得加
   **角色/锚点词表 + 跨 pass 补丁语义** = §3.5 点名的「Lua 词表加档税」，而那笔税原被记为 O4 独有，
   子图把它提前到 S1。**颗粒度问题是真的，但它的解法是 §8.2.0 的 L2/L3，不是配置语言扩力。**
2. **子图名是意图，6 处现查要的是编译图事实**——两个真相源。手写 Lua 图里可以有一个没人装饰出来的
   `taa_out`（`graphHasDeferredLight` 接口注释明说「自定义 Lua 图可以在 forward 变体下手写 light
   pass」），此时「我没引用 taa 子图」与「图里有可采的 taa_out」**同时为真**。这就是 债-129 的根因
   （申报点 ≠ 提供方）搬到配置层。同形问题的既有裁决 = `graphHasDeferredLight`（图事实）与
   `RgGraphVariant::deferred`（配置意图）**不抹平**，分歧交给 gate_lint 交叉校验。
3. **纠正：「配置沙箱禁引用」不成立为论据**（本文初稿把它当过论据，分量不足）。
   `open_libraries(base, table)` 之后只 nil 了 `dofile/loadfile/package/require`，**`base` 里的
   `load`/`pcall` 没禁** ⟹ chunk 内任意**计算**本来就能做，真正关掉的只有**文件与模块可达性**
   （配合没开 `io`/`os`）。所以"沙箱"作为**安全**边界是名不副实的——威胁模型也不成立
   （能写 `configs/*.lua` 的人已经能写这个二进制更多输入）。它真正承重的是两条**可判定性质**：
   ① 被加载文件自足 ⟹ smoke.py 的扁平目录 staging（`iterdir()` 只拷 `is_file()` + `CONFIG_FILES`
   齐全性）与单文件文本补丁那一整类「改了没生效要响亮失败」护栏才有前提；② 被加载文件是数据不是
   程序 ⟹ 配置面不会变成零护栏的第三真相源（Lua 侧零类型、零 lint、零 doctest）。
   **实测放宽的成本 = 0**：全仓 Lua 配置零处使用 `math.` / `string.` / `os.` / 字符串拼接。
   真实代价在别处：`qa/configs/env_band` 与 `nm_rig` 的 `runtime.lua` 去掉注释后**只差一行**
   （截图文件名），注释写着「正文沿用 env_band 同一套」——**护栏把组合赶进了散文，而散文级复制
   不可校验**。正确修法 = 组合归**生成器**（在 `tools/` 加一个 stdlib-only 的铺目录工具：
   base + 每靶 overlay，**本条尚未落地**），不归被加载文件。→ 另案登记，**不进 债-129 射程**
   （S1–S4 无一步需要 Lua 有计算力）。

---

## 4. Q3 直接回答：plan-only 的出厂设置，纳入动态渲染图是否有必要

**结论：部分必要。**

把问题拆成两件事（清单里的事实正好用在这里）：

**(a) 把「能力清单 / 前置判定」交给图——图当注册表 + 校验器，零发射权：必要。**
判据 = **凡是编译图的纯函数（零设备、零副作用、可由 RgPassPlan/RgAttachmentDesc 序列算出）
的事实，归图；需要知道「模块实际发了什么 / 离开时布局是什么」的判定，归模块，图不拿。**
证据（全部复算）：
- 图**已经**是 compute 参与者的校验器 + 类型化注册表，不是调度器：registerBufferAttachment /
  declareBufferProductions/Consumptions / reconcileAttachmentSyncEdges（缺边 = 启动期/声明期/
  重编译期点名抛错）——机制已付过建的成本；
- `isSchedulable`（offgraph_contract_impl.cppm:183）**生产侧零调用者** = 注册表的查询面建了
  没人用——缺的不是机制，是「让消费者消费它」这一步（O2 就是这一步；isSchedulable 同时是
  反面教材：查询面不许无消费者地常驻）；
- `UsageInfo::layout = nullopt` 结构性早退（render_graph_usage.cppm:87-90）证明「零发射权」
  是**类型层事实**——把前置判定交给图，不触碰这个类型事实，D6 红线原样成立；
- 6 处现查 = 注册表的事实没有以谓词暴露 ⟹ 每个消费者自己写一份 = 「申报点 ≠ 提供方」
  （与 127 同根因，§5）。

**(b) 把「开关生效」交给图——图当调度器：不必要，且碰红线。**
D6 三条坚持理由（compute_调研.md:359-368）逐条对照：
1. **模块拥有录制不可约**——IZB 的 13 条边夹在它自己的 beginRendering/dispatch/readback 之间
   （izb_shadow_impl.cppm 的 beginMaskPass 自己开 render pass），FSR 是 vendor 黑盒一次调用
   录完全部 job ⟹ 图代发 = 图拥有整段录制（把模块改写成图的回调），不是「顺手发一条 barrier」；
2. **半发 = 两套发射模型**——图唯一发得出来的是 meshletVisibleFlags（prologue 写→opaque 读）；
   IZB/EVSSM/FSR 发不出来 ⟹ 「给 buffer 发 barrier」落地必然一半图发一半模块发，比现状差；
3. **撤掉的是唯一有牙的对账**——layoutDrift 的输入是「模块真实离开时的布局」；图代发 ⟹ 模块
   返回的就是图自己算的数 ⟹ 判据恒真；这条线抓到的三个真 bug（resize 描述符链 / 相位表吃旧图 /
   门位说谎）全部依赖两侧来源独立。

**代价（必要的这半）**：谓词面 API 常驻（isSchedulable 教训：每个谓词必须有调用者才准常驻）；
需求类型是 O3 的新税；启动时序统一是 O4 的新税；判据 6 的维护（每次动态面扩张，谎报布局变异
必须红——红线护栏不免费）。**不付的代价**（(b) 不做）：发射权不动、layoutDrift 的牙不动、
buffer 附件层维持 127 现状（到期判据在 127 自己手里）。

---

## 5. Q4 与 127 的关系

### 5.1 是不是同一症状？是——同一根因，两个症状

根因 = **「一个事实被申报在不是它提供方的层」**。
- 127 的形态（还债总账.md 债-127 行）：buffer 附件的「依赖可见性」申报在图里（注册表 4 名 /
  进图 3 个 / 真消费者 1 个；29 处 emit 全在模块侧；图一条命令都不发）⟹ 机制常驻、收益悬空；
- 配置侧的形态：「velocity 在图里」这个事实被 6 处各自申报（§1.1），提供方（图）没有暴露谓词
  ⟹ 每层各写一份，漂移无人对账（两处判据对的条件集已经不同：4 vs 3）。
两者是同一句话的两种读法：**申报点 ≠ 提供方 ⟹ 要么多源漂移，要么机制悬空。**

### 5.2 一次裁决能不能同时收掉？能。裁决句（措辞）：

> **「一个事实只在它的提供方那里申报：图形态事实由 render_graph 谓词面唯一提供（配置层 /
> 装配层 / 工具层是消费者，不是申报点）；无消费者的机制（谓词无问者 / 图内附件无消费者）
> 删除或降级，不挂号。图提供的是「图长什么样」，不是「发什么命令」——发射权不在此句射程内。
> 本裁决的 O3 射程只取 image 侧（装饰需求），不碰 buffer 附件注册面（那是 127 的到期判据）；
> 且需求只申报**图形态轴**的前置——设备能力（`shaderFloat16` 一类）归 `GateEnv`，把它做成
> compile 期拒 = 同一份配置换台机器起不来（🔴 三机轮换，见 §3.4.2）。」**

收法分两条路径：配置侧 = O2 谓词面（现在就能做）；127 = 等触发（到期判据已在 127：下一次动
阴影或材质时，frag 采样 / 拷贝读回两类仍不接进图 ⟹ buffer 附件降级成独立注册表；接了 ⟹ 结案）。
**边界**：若有人把「图提供事实」读成「图顺手把 barrier 也发了」= B3 形态（已否决；D5 定案第 5
条明写诱惑复发概率上升）——裁决句第三句就是钉这条的。

### 5.3 试金石：`aa = "off"` 出厂时，`fsr.temporal` 这枚键该不该存在？

三种答法，各把方案推向一条路：

| 答法 | 内容 | 推向 | 后果 |
|---|---|---|---|
| **A「该存在，且允许它继续沉默」** | 维持现状：键在、宿主关、引擎零出声 | **O0** | 31 枚空开关永久零引擎出声；唯一守卫 = 工具层（今天 2 处，smoke.py + imgqa profile）；第四层出声点未查清 = 每出现一层再守一遍（配对表「fsr.temporal/jitter 要 aa=fsr」行的现状） |
| **B「该存在，但死值状态要申报」** | 键保留，「宿主关 ⟹ 块死值」按 shadow 先例申报（inertNote，debug 级） | **O1** | 配置形态不动；+6 行（块级口径，需扩覆盖率护栏口径）或 +31 行（逐键口径，现有口径，表肿半倍）——拍板点 D；出厂零**可见**噪音（inertNote = debug 级，默认 info 日志不打） |
| **C「不该存在（作为顶层独立键）」** | fsr 块归属 aa=fsr 模式（模式表形态：mode=off 的表里没有 temporal 字段可写）；「FSR 接得起来」前置（velocity 在图）由图谓词回答 | **O2/O3** | 配置面破坏（aa 字符串→表，工具 --set 语法同步）；谓词面是前置；「键在但模式关」的形态**结构性消灭**（正交化的正解） |

**本文裁决：C 的形态 + B 的兜底。**
- C 消形态：模式表形态下「aa=off 而 fsr.temporal 存在」写不出来（= 用户要的「非法组合写不出来」，
  shadow.lua 的 mode+按档块已是同款完成态，推广即可，不是新机制）；
- B 兜残留：表形态仍写得出来「mode=off 的表里塞了 temporal 字段」⟹ 解析期 dead-key inertNote
  接住（复用 lintUnknownRuntimeKeys 机制——该 lint 今天只管顶层键（runtime_config.cppm:18-22
  「lint 只看顶层」），扩展到模式块内 = 小改，0 新护栏、0 新表）；
- 「FSR 接得起来」（velocity 在图 + shader16 + 输入齐）= ② 图事实 + 设备事实，由 O2 谓词 +
  GateEnv 回答（computeFsrUsable 的 warn 保留——那是**可用性**申报，不是前置申报，两者不冲突）。
这个裁决**不依赖**拍板点 D（31 枚的申报口径）——试金石答的是「该不该存在」，D 答的是
「死值状态怎么出声」，两件事。

### 5.4 ≥31 枚「出厂即空开关」的处置（不新增噪音）

先纠正清单的一个**高估**（§5.5 详述）：31 枚里 29 枚的零出声**是既有裁决**，不是缺口——
readSsrBlock 头注释（runtime_config_impl.cppm:315-327）明确裁决「configs/runtime.lua 的**其余**
键（含 aa / render_scale / fsr.* 这些既有键）不在 gate_lint 表里，本批因此没有欠账」，理由成立
（硬塞 = 造第三张回显表 = 与 RuntimeConfig 字段并存的第二真相源，或把 runtime 键混进 diagnostics
的回显语义）。清单把它们标 🔴 并断言「deviated() 口径结构上碰不到」——机制描述对（deviated 只
在偏离默认时谈，出厂生效值=默认值 ⟹ 恒沉默；且 31 枚中 fsr/ssr/taa/llm_shot/headless/ui_font 各
**根本没有行**，谈不上「碰不到」，是「没挂号」——两种机制被一句话混了），**问题规模**错。

处置（三件，按序）：
1. **裁决记录补全**（0 机制）：readSsrBlock 的同款理由显式覆盖 wet 块（现只写了「本块」）与
   scene.lua 的 taa 块（跨文件，无任何裁决记录）——写进当前任务文档，可改写纪律。
2. **shadow 先例推广**（debug 级，0 可见噪音）：对「模式是块的属主」的两类块（aa 表形态 / ssr 块）
   加**块级** dead-key inertNote——触发 = **文件显式写了死参数**（不是生效值≠默认 ⟹ 出厂配置
   全量显式写值也**不**在 info 级出声；inertNote = debug 级，默认日志不可见 ⟹ 出厂零噪音成立）。
   行数 = 拍板点 D（块级 6 行需扩护栏口径 vs 逐键 31 行现有口径 vs 不补）。
3. **两枚「最危险」的归 ④ 不归 ③**（它们不是死值申报问题）：
   - `velocity_field`：真全屏 pass + 真 RT 每帧跑、零消费者（aa=off 时 velocity pass 照装饰照跑，
     tonemap 不读它），且「独立键」裁决建立在假前提上（drift ①，§1.2）⟹ **撤并进 aa 块**
     （拍板点 C）——这是 31 枚里唯一「空开关有 GPU 代价」的；
   - `mdi_shadow` 的载体：出厂建 MeshletShadowArgsService（GPU buffer，sliceRows×24B×2 ≈ 数百 KB，
     零消费者）——刻意裁决（§19.6：生效条件不含本键，main_meshlets.cppm:460-467）⟹ 裁决本身
     站得住（载体≠draw 门），但「出厂 GPU 资源零消费者」应在 §1.1 基线里挂号（本文已挂），
     是否收进门 = 拍板点（代价：wireMeshletShadowArgs 早退条件 +1 项，§19.6 的裁决文字要改）。

---

## 6. 我不知道的（不推测填）

1. ~~**未运行任何程序**~~ → **S1a 落地时已运行（2026-10-16，本机 AMD 8060S / RADV STRIX_HALO）**：
   `just smoke` = `PASS errors=0 warnings=0` ⟹ §1.3 那条「出厂 0 findings」由**谓词推断**升级为
   **实测**（出厂态连 warn 级都没有）；`regress floor --runs 3` 两锚各 3/3 命中（出厂
   `95edc8cc…` / 门关红线 A `54e40665…`）。本文其余数字仍读自源码。
2. `docs/compute_调研.md` §7 未逐节读（kind 加档「10 处静默点」的逐处、B2-4③′ 裁决全文——
   本文只复核了引用的数字：4.5µs / 29 emit / nullopt 早退）。
3. ~~`qa/configs/**` 与 `tools/qa_scenes.py` 是否有**第四层出声点**~~ —— **已查（2026-10-16，
   本文实测）**：没有。`qa/configs/` 四个冻结目录（analytic / env_band / nm_rig / ssr_rig）
   零引用 `meshlet.*` / `fsr.*` / `taa.*` / `velocity_field`；唯一命中的 `qa/log/imgqa_calls.jsonl`
   是追加式调用台账不是配置。tools 侧真实引用面：`velocity_field` 4 文件、`fsr.*` 3–4 文件、
   `meshlet.overlay` / `meshlet.enable` 各 1 文件（**比 §8.3 预估的小**）。
4. ~~`tools/checks/` 是否有静态规则守配置组合~~ —— **已查**：没有（`rules_text.py` 的 meshlet
   命中是 schema 常量三方对账，与配置键无关）⟹ S2 不必同步静态规则，**但也意味着配置面除顶层
   未知键 lint 外没有机器牙**——这条读数是 §8.2「S2 硬前置 = 嵌套死键 lint」的直接依据。
5. imgqa 其余子命令的组合守卫（只复核了 engine_profile.cpp:357-363 的 checkFsrOpts）。
6. `meshletCullItems`「只注册不进图」是刻意还是迁移半成品：事实已核（注册于
   meshlet_cull_service_impl.cppm:192；图内无声明；kAttachmentSyncEdges 恰 1 条只有
   meshletVisibleFlags），定性不能——它恰是 127 到期判据清单上的样本，等 127 触发时一并裁。
7. izb/evssm 懒构造首帧时序对帧末对账的全部影响（清单同未查）。
8. 两处判据对（computeFsrUsable × initFsr）漂移的**实际后果分布**：initFsr 缺输入 = 构造 throw
   （可见）；shader16 半边的漂移 = FsrUpscaler 构造期 vendor SPIR-V 能力检查失败（形态是启动
   报错还是垃圾画面，未实机验——注释说「垃圾画面且不指名」，按注释记，未复现）。

## 7. 需要项目主人拍板的（二选一 + 各自后果）

**拍板 A：`meshlet.enable` 的处置**
- (a) 改名成资源门（如 `meshlet.assets`）+ 块内 9 键的宿主条件注释逐键修正。
  后果：配置面破坏（tools/smoke.py、imgqa、`qa/configs/**`、docs 的引用一次同步，一次性成本）；
  「总门」假象消除，块内新键不再继承谎言。
- (b) 保留键名，只修注释（含 §1.2 差异 ⑥ 的两条过期注释）。
  后果：零破坏；「总门」谎言保留，meshlet 块再加新键时继续借语义（本文 Q1 分类里 enable 的
  ④ 定性留在文档里，不落在代码里）。

**拍板 B：图作图事实提供者的射程** ✅ **已裁 (b)（2026-10-16 用户：「把需求申报进图，绝对值得做」）**
- (a) 只出查询（O2）。后果：启动期 warn 保留（形态不变），漂移风险消除，消息形态 0 迁移；
  O3 的「compile 期拒」留待下一次。
- (b) 含需求申报（O3）。后果：warn → compile error（消息形态变 ⟹ smoke.py 正则等工具同步，
  漏一处 = 工具静默失配）；形态更强（非法组合启动即拒）；127 射程边界（§5.2 裁决句第三句）
  要写进验收。
- **裁 (b) 的理由不是 (b) 更便宜，是 (a) 保留的降级本身有病**：(a) 的「启动期 warn 保留」= 「配了
  却没生效且不指名」这个形态永久保留，而它正是本债的 ② 类成因。⟹ 施工上先做 (a) 的那半
  （S1a 查询面），但**设计目标定在 (b)**；🔴 原案里「再由 S1b 铺一层落图结果」这一跳**已取消**——
  裁 (b) 之后那一跳要造的东西没人问（四条否证 → §8.2.0）。

**拍板 C：`velocity_field` 的处置**
- (a) 撤并进 aa 块（删独立键；aa=fsr 隐含打开——applyAaKey 现成那一行 runtime_config_impl.cppm:263
  就是全部隐含逻辑，删键 = 「velocity 不能单独开」的形态）。后果：配置面破坏（键删除，
  tools/qa/docs 引用同步）；「空开关有 GPU 代价」形态（aa=off × velocity=true = 每帧白付全屏
  pass + RT）结构性消灭；假前提（drift ①）连裁决带注释一起删。
- (b) 保留独立键，重写「非 FSR 消费者」注释为「无独立消费者，aa=fsr 时隐含打开」。
  后果：假前提裁决保留（独立性没有事实支撑但保留）；velocity_field=true × aa=off 的每帧 GPU
  代价保留（只多一条 lint inertNote 或 0 申报）。

**拍板 D：31 枚死值申报的护栏口径**
- (a) 扩口径到「块级门」（每块 1 行，≈6 行；新门名种类 ⟹ tests/test_gate_lint.cppm 覆盖率
  断言同步改）。后果：表不长，口径多一种（块级 vs 键级，两种门的判据不同 = 维护面 +1）。
- (b) 逐键行（31 行，现有口径不动）。后果：护栏口径零变化；表从 60 行涨到 ≈90 行（「加行」
  路径的税，用户原话「没想到那么严重」的另一种严重）。
- (c) 不补（维持 readSsrBlock 裁决，0 行）。后果：零出声状态保留（§5.4 的 3 件处置里第 2 件
  也不做）；只有拍板 C 的 velocity 撤并消掉最危险那一枚。

**拍板 E：O3 是否触发 127 到期判据** ✅ **已认（2026-10-16 用户「O3 绝对值得做，改文档后按路线推进」）**
127 的到期判据 = 「下一次动阴影或材质时」检查 frag 采样/拷贝读回是否接进图。O3 的「前置申报
进图」若被理解为「动图附件面」= 触发 ⟹ 按 127 现状（两类仍不接）buffer 附件应**降级**成独立
注册表——与 O3「申报进图」方向**直接冲突**。裁决句已把 O3 射程钉死在 image 侧（装饰需求），
不碰 buffer 附件注册面 ⟹ **O3 与 127 并行排期**，S3 不必等 127 结案。
🔴 本轮改写补上**第二条边界**（原文只钉了一条）：设备能力轴不进需求 → §3.4.2。

## 8. 目标形态与施工顺序（主 agent 定稿，2026-10-16）

⚠ **本节取代初稿的「如果只能做一件事」一节**，原结论（只做 O2 谓词面）作废。
项目主人的批评是：「我总觉得就是最小阻力原则，更好的设计都被否决了。」——**这条成立**，
而且错在方法层：那个提问（"只能做一件"）**按成本排序，答案必然是地板**。
把被提问方式约束出来的最小答案当成设计结论推荐，是本篇初稿的缺陷。逐条复核见 §8.0。

### 8.0 三项被"成本理由"推迟、而成本理由站不住的

| 项 | 当初的推迟理由 | 为什么站不住 |
|---|---|---|
| **O1 枚举化**（`meshlet.mode` / `instrument` 三值 / `fsr` 归 `aa` 模式表） | 配置面破坏性 | 本仓为**同样的**破坏性付过两次钱、两次都赢：`aa` 三态化（`docs/phase14_plan.md:1337-1352`，动机原话"三布尔 8 种写法只有 3 种有意义"）、`shadow.mode` 换轨（`docs/shadow_routes_plan.md:186-190`，"让位链不是被修好，是被消掉"）。两次都是**删规则**不是加规则 ⟹ 这是已验证打法，不是新冒险 |
| **拍板 C：`velocity_field` 撤并** | 删键要同步工具/qa/docs | 它的独立性**没有事实支撑**（`shaders/taa.frag.slang:783` 自陈"本项目没有 velocity buffer 是因为不需要：重投影是解析的"，而 `configs/runtime.lua:28-30` 与 `runtime_config.cppm:304-305` 仍写"TAA 要它"）。留着 = 给一个假前提持续交维护税；且 `velocity_field=true × aa=off` 是**每帧白付一个全屏 pass + 一张 RT** 的空开关（有 GPU 代价，不是零代价的沉默） |
| **拍板 A：`meshlet.enable` 改名** | 改名要同步一片引用 | "总门"是假象这件事**正是**块内 9 键宿主条件不统一的成因（§2.2：overlay/mdi_shadow 不含它、debug/tint 只经条目间接含）。不改名，下一个新键继续继承这个谎言。同步是一次性 grep 成本，不是持续税 |

**仍然按原则否决的只有一项：O4**（开关 = 动态渲染图操作）。理由**不是成本**，是 D6 三条坚持理由
逐条命中、且 layoutDrift 会被撤成恒真（§3.5 / §4(b)）。**这条是设计结论，不是最小阻力**——
把它和上面三项混在"以后再说"里，正是初稿的错。

### 8.1 目标形态（一句话）

> **一枚能力 = 一枚模式（或一个从属该模式的参数块）；它需要什么图形态由图回答；
> 资源分配不出现在配置面。**

在这个形态下 gate_lint 申报表是**变短**的。申报表的存在意义只是"记录还没被类型消灭的合法组合"
⟹ **它的行数是要往下掉的指标，不是往上加的**（这条也是判"某次改动方向对不对"的试纸）。

### 8.2 施工顺序（四阶段 + 一把清理刀，每阶段独立可验收、可回退）

> **2026-10-16 二改**（用户「8.2 修改，开始 1」）：S1 拆两刀、`aa` 表形态从 S2 拆出成 S2.5、
> 嵌套死键护栏从 S4 提到 S2 当硬前置。三条改动的共同理由：**原四阶段把"风险等级不同"与
> "依赖顺序不同"混进了同一个编号**——验收失败时分不清是哪一半错。
>
> 🔴 **2026-10-16 三改（S1a 落地后开工前侦察实测）**：**S1b 取消**——「装饰器返回带内容的落图
> 结果」的四条否证见 §8.2.0，核心一句 = 那些内容**当期消费者为 0**，而 4 枚 bool 会随 O3 的
> 「不降级」自己消失。阶段因此回到 **S1a → S2 → S2.5 → S3 → S4**，另有一把不占编号的
> **S1c 死 API 清理刀**（三项，零风险）。S3 的前置从 S1b 改成 **S1a**，范围吸收「装饰次序契约
> 从 skip+warn 改 fail-fast」。

#### 8.2.0 图事实的三级粒度（S1 的设计核心：先定粒度，再定接口）

「图里有没有 X」不是一件事，是三件事，**粒度取消费者真正需要的形状**：

| 档 | 内容 | 提供方 | 今天的形态 |
|---|---|---|---|
| **L1 原始扫描** | `attachmentSlot(name)` / `attachmentSlotByKind(kind)` / `passIndex(name)` → 下标或 `nullopt` | 编译图 | 6 处各写一份 `for (s…) if (name == …)`；`Renderer::findAttachmentSlot` 是其中一份的私有化 |
| **L2 角色查询** | `fsrColorInput(graph) → {slot, baseSpace}`——**拥有** `ao_applied > taa_out > ssr_applied > scene_hdr` 优先级链与「色输入在哪个空间」这条不变量；`fsrInputs(graph)` 再带 velocity / depth | 编译图 | **两份策略**：`initFsr` 里是三分支 + 注释自陈「这条不变量的唯一来源」，`computeFsrUsable` 里把它重述成 `(has(taa_out) \|\| has(scene_hdr))` |
| ~~**L3 能力落图**~~ 🔴 **已撤销**（→ 下面的撤销块） | ~~装饰器返回**它到底插了什么**（锚点 / 绑定槽 / storeOp 补丁原值）~~ | ~~装饰器~~ | ~~压成 4 枚 bool，内容丢弃后由消费者重推~~ ⟹ 实测：装配层问的**就是**存在性，**bool 是正确答案的形状** |

🔴 **只出 `bool` 的谓词面是错的粒度**：6 处现查里 **5 处要的是下标**（`initFsr` 五次
`findAttachmentSlot`、picking 构造存 slot、frame_capture 按 kind 存 slot + `slotOfName`），
只有 `computeFsrUsable` 要真假。先建 `hasAttachment(name) -> bool`，收尾时会被迫「用谓词判存在、
再自己扫一遍拿 index」——现查点没消掉，还多一层。`hasAttachment` 只能是 `attachmentSlot` 的派生。

🔴 **L2 才是「两处判据对」的正解**，比「合一成一个 `fsrInputsReady`」强：合一只是让两处吃同一个
函数，L2 是让**策略只剩一份**——`fsrInputsReady` 变成 `fsrInputs(graph)` 的推论，漂移不是被
对账住，是写不出来。

🔴🔴 **L3 撤销（2026-10-16，S1b 开工前侦察实测；本块推翻同日二改时由我写下的方案）**。
原案：装饰器返回带内容的落图结果（锚点 / 新附件名 / 借的 depth / storeOp 原值），4 枚 bool 退役。
逐条查下来四条否证：

1. **那些内容的当期消费者 = 0**。4 枚 bool 的读者只有 `assembleBackEnd` 一处，它问的是「这个能力的
   RT 与条目领不领建」= **存在性**；装饰器新建的附件名，装配层吃的是 `kXxxName` 常量；锚点下标只有
   装饰器自己用得上。加字段 = 建了没人问 = 新护栏 `graph_fact_predicate_checks` 当天判红、我当场
   删掉两条谓词的**同一件事**，只是这次规模大十倍、而且写在计划里。
2. **bool 不是被压扁的残次品**。§2.1 原话「内容丢弃后由消费者扫描重推」——那个"重推"在 S1a 已经收进
   `graphFsrInputs` 一处；剩下的存在性判断，装饰器是唯一知情人，**它就该出一个存在性结果**。
   `velocityDecorated` 尤其：附件在图里 ≠ 有人在写它，弱判据推不出来。
3. **4 枚 bool 的真正归宿在 S3**：拍板 B 裁 (b) 之后「请求了但没落图」这个状态**不存在**
   ⟹ `xxxDecorated` 退化成 `xxxEnabled`，bool 随**降级决定**一起消失。为它单开一刀是绕路。
4. `borrowDepthFromPriorWriter`（`render_graph_decorate.cppm:154`）覆写 `storeOp` **不记原值**，
   而 taa / velocity / ssr 三条装饰是启动期一次性、**没有撤回档** ⟹ 原值今天无消费者，记它同样是
   先建后找。（highlight 不同：它有 `withdrawPass` 撤回，`originalStoreOp` 三处真读者。）

⟹ S1b 缩成 **S1b′**（下节）：只做**有当期消费者**的事。连带撤销两处同日写下的东西：
§3.4.0 里「S1b 的 L3 按 compile 校验的输入设计」那条形状纪律，和「落图结果里出现 `bool` 字段即
ERROR」那条拟议护栏（没有结果结构体可扫 = 护栏本身会是一条零问者机制）。

🔴 **4 枚 bool 的归宿 = S3，不是 O4，也不是 S1b**：消掉它们不需要「开关 = 动态图操作」，也不需要
落图结果——裁 (b) 之后 `xxxDecorated ≡ xxxEnabled`。⟹ O4 的增量只剩「开关走 `declarePass/
withdrawPass`」，**恰好是被 D6 三条理由否决的那一半**（§3.5 / §4(b)）。否决 O4 的理由因此更干净：
它原本顺带的收益由 O3 顺带拿到。

**两件顺手**：① `graphHasDeferredLight` 改成 `hasPassState(kind, state)` 的薄封装——否则图里同时
存在两份扫描，而它是唯一先例，不能留成孤本；② 补 `kDepthName` 具名常量——`"depth"` 今天
在 `computeFsrUsable` 与 `initFsr` 各写一份字面量，其余附件名全吃 `render_graph.cppm` 的
constexpr，**谓词面自己不能成为新的抄写点**（哲学 1：访问路径必须消费那个来源）。

#### S1a 图事实查询面（L1 + L2）

- 交付：L1 / L2 两组函数住 `render_graph`（接口 + impl 分离，同 `graphHasDeferredLight` 现状），
  纯只读、零设备、零发射；`computeFsrUsable` 与 `Renderer::initFsr` 改吃 L2。
- 验收：① 6 处现查 grep 命中 0（只剩查询面本体）② 每函数一正一反单测进 `just test`
  ③ 两锚逐位（纯查询，逐位不变是构造性的）④ 🔴 **每个新导出函数必须有生产侧调用者**，且这条
  落成**机器护栏**（`tools/checks/`，先例 = RPC 路由↔dispatch 双向对账、Lua 词表↔枚举本体对账），
  不是纪律句——没有护栏的「必须有调用者」就是 `isSchedulable` 第二。
- 附带了断（**实测后改判，别照原句执行**）：`OffGraphSchedule::isSchedulable` 生产侧零问者属实
  （问者全在 tests），但读实现后它**不是** §5.2 说的那类"平行申报点"：它是 债-128 锚点命中哨兵
  （`kUnschedulablePhase`）的唯一 accessor，且 `enter()` 已在事务点结构性拦住不可录申报。
  真正的重复在**另一侧**——生产路径（`izbPreFrame` 查 `graphHasDeferredLight` + 深度附件）在别处
  重算了同一件事（锚点命中），而接口注释写着「判据请问 `isSchedulable`」却无人问。
  ⟹ 本批**不删**（删了会把"锚点命中的唯一 accessor"和它的 债-128 归因消息一起弄没），
  改记 债-129 的一条待裁项：**S3 时一并裁**（原写 S1b，随 S1b 取消改挂 S3）——要么让录制侧改问它
  （消掉重算），要么连 债-128 的哨兵一起降级。→ 已挂 `docs/还债总账.md` 债-129 行。
- **验收读数（2026-10-16 本机 8060S，commit `0ebfcba`）**：
  ① 编译图现查 **6 → 0**（`render_graph*` 之外对 `attachments` 的手写 `find_if` 命中 0；
  `Renderer::findAttachmentSlot` 已删）；② `just test` 555/555，新增组 10 共 7 例；
  ③ 🔴 **13 条变异全部验红**且各红在预期用例上（M4/M6 第一版变异自身编译不过——`std::find_if`
  未声明 / `-Werror=unused-parameter`，按 skill「谓词类变异要保留参数使用」改写成**弱化**形态后
  重跑才有效）；④ 新护栏 `graph_fact_predicate_checks` 进门，读数 `谓词 6 条 / 不在存活链 0 条`，
  自身 7 条内存注入全达期望（其中一条专钉「传递存活方向写反 = 死谓词调用活谓词即永久假绿」)；
  ⑤ `just gates` 全绿；⑥ 逐位四档对拍 `--ref HEAD^` 全**逐位相同**（默认 / `aa=fsr`+`temporal=false`
  8×8 / `shadow:izb=true` / `diagnostics:picking=true`）——`dump_gbuf` 档**比不了**：它会把截图
  输出文件名改掉，regress 找不到自己的 shot（工具限制，非代码问题；该档引擎跑通出图）。
  ⚠ 期间撞见 债-88 的第四次漂移读数 + `regress.py` 归因的**循环**（`max(floor_a, floor_b)`
  让被怀疑的一方自己提供豁免额度）→ 两条都记在 债-88，**不随本刀夹带修**。

#### ~~S1b 装饰落图结果（L3）~~ 🔴 **取消**；剩余可做的并入 S1c 清理刀

取消理由见 §8.2.0 的 L3 撤销块（四条否证）。这里只记**取消之后还剩下什么**——剩下的两件事都有
当期消费者，且都不需要落图结果这个新形态：

**S1c 死 API 清理刀**（小、零风险、可单独提交，不占阶段编号）

- `decorateHighlightPass`：生产侧**零调用点**（`main_impl` 走 `planHighlightDecoration` +
  `applyHighlightPlan` 两步，只有 tests 在调这个薄壳）⟹ 删，tests 改吃那两个真入口。
  ⚠ 顺带记一条正面事实：`RgHighlightPlan`（带 `insertBefore` 锚点名 + `originalStoreOp`）
  **已经是** L3 该有的形态——它是样板，不是待改造对象。它成立恰恰因为 highlight 有 `withdrawPass`
  撤回档、那些字段有真读者。
- `decorateShadowPass` 的返回 `bool`：函数体**零 `return false`**（恒真），唯一调用点
  `main_impl.cppm:507` **把返回值丢掉**，紧接的 info 日志也是无条件的 ⟹ 改 `void`；
  连带删测试里「无条件返回 true（不校验 mapSize）」那条断言——它断的是一条不存在的判据。
- `main.cppm:196` 的**悬空声明** `compileGraphOrThrow(const char*, bool, bool, bool, bool)`：
  全仓无定义（唯一定义是 `(const char*, const RuntimeConfig&)`），Doxygen 还在逐条解释那四个
  不存在的形参 ⟹ 声明与注释一起删。

**顺序契约 → 归 S3，不归本刀**。原案「把顺序契约从注释挪进落图结果里的声明序」有个更好的归宿：
`RgDecorationRequirement` 的「**不得在图里**名单」正是这条契约的类型（§3.4 原文就列了它）。
而且裁 (b) 之后它的语义也跟着变准了：**装饰次序是 C++ 写死的，不是用户配置** ⟹ 错序 = 装配错 =
**该 fail-fast，不该 skip + warn**。今天 ssr 的跳过条件 1（taa/ao_apply 已在图里）把「装饰太晚」
当合法降级处理，正是这条要改的对象；而 taa / velocity 今天**一条结构判据都没有**（只有
`main_impl.cppm:526-531` 的注释），失败模式按注释原话是「落在 taa 与 tonemap 之间而吃不到滤波」
= 静默错画面。

**明确不做**：色输入链两处合一（装配层那条要按档**条件领建 RT 堆槽**，门关 = 堆槽零变化是逐位性质，
改「先问链头再领槽」会动领建序）——已由 16 组合表钉住同解（`965f146`），那是当前形态下的最强判据。

#### S2 配置层类型消灭（拍板 A + C 的键名 / 键合并半）

- 交付：`meshlet.instrument`（none/offset/tint，两布尔 → 三值）→ `meshlet.mode`
  （off/overlay/switch）→ `meshlet.enable` 降级改名为资源门 + 块内逐键宿主条件注释修正。
  🔴 **本阶段不动解析器形态、不动 `--set` 语法**（那是 S2.5）。
- 🔴 **硬前置（原 S4 的第 2 件，提到这里）**：未知键 lint 目前**只看顶层**
  （`collectPresentKeys` 自陈「子表只记表名」）⟹ 改名后残留的旧**嵌套**键静默消失。本仓纪律
  「无扁平键双读兼容」= 硬切，硬切 + 静默残留的组合是：本地改过的 `build/rgl/configs/runtime.lua`
  副本（`copy_if_different` 只源更新才覆盖）带着 `debug = true` 继续跑而无人出声。修法现成：
  `kRuntimeKeys` 本来就是点分名表，把未知键 lint 扩到「被改名的块内一层」。
  **这一个改动同时是 S2 的迁移护栏与 §5.3 的 B 兜底（dead-key inertNote）**——做一次两阶段受益，
  所以它不属于 S4。
- 验收：**申报表行数下降**（目标 ≥ −6 行）+ 装配层"让位/互斥"warn 删除 + 两锚逐位 +
  §8.3 的 grep 面全同步 + **残留旧键必须出声**（变异红线：配置里留一枚 `meshlet.debug`
  → 启动 warn 点名它）。

#### S2.5 `aa` 表形态 + `velocity_field` 撤并（从 S2 拆出）

- 为什么单独一阶段：S2 三项是键改名 / 键合并（解析器不动、工具语法不动），本项是**全计划里唯一
  动 loader 形态 + smoke.py `--set` 文本补丁语法**的一项。smoke.py 是文本级改 lua 且带「唯一命中」
  护栏（曾因第二个 `enable` 撞红过一次），`meshlet.mode` / `shadow.mode` / `aa.mode` 撞不撞取决于
  是否带 `[FILE:]` 前缀。
- 🔴 **开工前必须先出纸面键名表**（改后的 `configs/*.lua` 样例 + 逐条过 `--set` 语法）：命名是
  唯一不可回退的东西——代码可回退，配置面破坏性付两次钱就是付两次。
- 交付：`aa = { mode = …, temporal = …, jitter = … }`（§5.3 试金石的 C 形态：mode=off 的表里没有
  `temporal` 字段可写）；`velocity_field` 撤并进 aa 块（拍板 C，`applyAaKey` 现成那行就是全部
  隐含逻辑）。
- 验收：aa 三档锚逐位 + `--set aa=…` 全部用法同步（smoke.py / imgqa profile / regress）+
  解析期「mode=off 的表里塞子字段」出声（复用 S2 已建的嵌套死键 lint）。

#### S3 需求申报进图（O3，**只取 image 侧装饰需求**）✅ 已定案要做（拍板 B 裁 (b)）

装饰请求携带"我要什么图形态"，非法形态 compile 期拒；gate_lint 的图形态类前置降级为"图已保证"
（目标再 −3 行）。🔴 **前置 = S1a，不是 S1b**（S1b 已取消，§8.2.0）：需求校验吃的是**compile 期的
desc / 图形态**，本来就在装饰**之前**，不需要"落图结果"这个中间形态——原案把它当前置是我把
「需求 vs 结果」这对概念想反了。

🔴 **本刀的收益本体是「失败不降级」**（用户原话：「少一堆动态检查」）。验收因此**不是**"加了校验"，
而是 §3.4.0 那张删除清单**逐项命中 0**：包装层 7 处出声点、`computeFsrUsable` 的 `inputsReady` 半、
`Renderer::initFsr` 的「两处判据漂移」throw、smoke.py 的「非错误」降级分支，外加**「两处判据必须
一致」这个问题类别**（它没有可 grep 的实体，判据 = 前两处都已经是 0）。只加校验不删这些 = 半吊子。

🔴 **射程两条，缺一条就会反向触发别的债**：
① **不碰 buffer 附件注册面**——那是 债-127 的到期判据（§5.2 裁决句），碰了就会把 127 的"降级"
   结论反向触发；
② **不接设备能力轴**——需求类型带"属于哪根轴"字段，只有图形态轴参与 compile 期拒；`shader16`
   一类归 `GateEnv`（它已经带着一枚设备能力 `staticGraphicsIndirect` = 先例）。把设备能力做成
   compile 拒 = 同一份配置换台机器起不来（§3.4.2）。

🔴 **两侧都要有判据**：启动侧（非法形态 → 起不来 + error 点名需求项）与**运行侧**（构造一个会撞
需求的 `declarePass` → 试编译拒、**图不变**、`checkEmission` 不红）。只测启动侧 = 动态图那条路径
的校验其实没人验过，而图在运行期**确实会变**（B2-5）。

#### S4 死值申报口径（拍板 D）——**排最后，且默认不做**

S1–S3 做完后重数"出厂即空开关"，剩下的才决定块级 / 逐键 / 不报。理由：S2 / S2.5 会直接消灭其中
一部分，**先加行再删行是纯浪费**。原第 2 件（块级 dead-key inertNote）已提到 S2 当迁移护栏，
本阶段只裁「剩下的还要不要再报」。

### 8.3 每阶段都要付的三件小事（别漏）

1. 配置面改名的 grep 面（**实测读数 → §6.3**：`qa/configs/**` 四个冻结目录与 `tools/qa_scenes.py`
   对本批涉及的键**零引用** ⟹ 真实面只剩 `configs/*.lua` + `tools/smoke.py` +
   `tools/imgqa/engine_profile.cpp` + `tools/regress.py` + docs，比初稿估的小）。
   附录 A 的 4 条纯文档计数漂移已随 §8 二改收掉——**收法 = 删数字、指真相源，不是把数字改对**
   （改对 = 把同一笔债再借一次）；其余各条随所属阶段收。
2. 锚点：本文的「两锚」= **出厂**（`95edc8cc…`）+ **门关红线 A**（`54e40665…`，态 =
   `--set meshlet.enable=false --set meshlet.shadow_lod=-1`）。🔴 **哈希不在本文抄**，唯一来源 =
   `docs/还债总账.md` §锚点口径；引用处必须同时写明**是哪一态**（`docs/mdi_plan.md` §18.11）。
   本轮实测抓到初稿这条已经漂了：它列的第三枚 `fdece9d0…`（csm + izb 开）**2026-10-10 已退役**
   （退役理由 → `docs/shadow_routes_plan.md` §3.1），而 `360ce0d2…`（红线 B）是**带时域历史的档**
   ⟹ 按项目自己的口径根本不做位比。⟹ 抄哈希与抄门牌一样会漂，且这里漂成了"拿退役锚对拍"。
3. 覆盖率护栏：S2 改门名 ⟹ `tests/test_gate_lint.cppm` 的三方对账钉子数字同步；
   🔴 不许把键名清单抄进测试（抄了等于没抄来源，见该用例注释）。
4. 🔴 **新建机制不得以"无消费者常驻"形态落地**：本计划每阶段新建的机制（查询面 / 需求类型 /
   嵌套死键 lint）验收都必须含一条**机器护栏**证明它有人用或它会出声。反例两个样本已经付过学费：
   `isSchedulable`（查询面建了、生产侧零问者）、`mdi_shadow` 载体（出厂就建 GPU buffer、零消费者）。

---

## 附录 A：漂移清单（10 条证实 + 3 条新发现 + 收账时又抓到 1 条）

| # | 位置 | 漂移 | 状态 |
|---|---|---|---|
| 1 | configs/runtime.lua:28-30 + runtime_config.cppm:304-305 | velocity_field 称有非 FSR 消费者（TAA 重投影要它）；taa.frag.slang:783 自陈重投影是解析的、不需要 velocity buffer | 证实 |
| 2 | main_meshlets.cppm:387 | 「-1 = 出厂默认」；实为 5（runtime_config.cppm:529） | 证实 |
| 3 | gate_lint_impl.cppm:233-243 | debug/tint 消息「或 enable=false ⟹ 强制归 base」；pickMeshletMeshVariant 只判 overlay（main_meshlets.cppm:136-144）——**本文补充**：谓词的 `!enable` 半边同样不是消费者判据（§1.2 差异 ⑤） | 证实 + 加重 |
| 4 | gate_lint_impl.cppm:107 | picking inertNote「任何可装饰图都行」；highlight 在 offscreen 内建图必失败（firstColorConsumerIndex 要求 sampleRead 色输入，offscreen 图只有 blit 上屏，render_graph_builtin.cppm:108-145） | 证实 |
| 5 | configs/diagnostics.lua:49-52 | dgc_static 非布尔「静默回落 false」；meshlet 块走 readStrictBoolKey（类型错 = 启动失败）——同一配置面两种布尔纪律 | 证实 |
| 6 | render_graph_usage.cppm:128 | 段标题「三个 kind 谓词」，其下四个（同文件 :7、:20 写四个） | 证实 |
| 7 | docs/技术概念.md（gate_lint 行） | 「43 门」；实际 60 行 / 53 门 | **已收**（删数字 → 指 `kGateRules`） |
| 8 | docs/技术概念.md（offgraph_sync 行） | 「边表 24 条 / 门 6 道」；实际 28 条 / 7 道（B2-4② meshlet 加 4 边 1 门没跟到），且**客户名单也漏了 `MeshletCullService`** | **已收**（删数字 + 补客户，条数指 `RgSyncEdgeId`） |
| 9 | docs/gate_lint_plan.md :106/:108/:163/:204 | 「46 行 43 门」「45 个门」「覆盖率 38 门」「阴影 13 行 11 门」；实际 60 行 / 53 门 / 阴影 16 行 13 门 | **已收**（删数字；163 那处是批次交付表的历史读数，保留） |
| 10 | docs/shadow_routes_plan.md:186-190 | 「阴影 13 行 11 门」（旧读数） | **已收**（改标「N6b 当批形态」+ 指表本体） |
| 14（收 7 时新发现） | docs/技术概念.md（gate_lint 行） | 「diagnostics 与 shadow **两份**配置各带一份 defaults」「**两份**回显表键集合之并」——债-106 后是**三份**（+ runtime），签名与覆盖率护栏都已三方化 | **已收**（就地改准；附录 A 原 13 条因此不再全绿） |
| 11（新） | main_impl.cppm:412 | 「门 = FSR 门开（`runtime.fsr_upscaler`）」——旧键名（14.6 已删，现为 aa=fsr） | 新发现 |
| 12（新） | runtime_config.cppm:480-482 | meshletSwitch 三态注释「enable && !switch → α 形态：旧路径照画 + 共面影子叠加」——债-105 后叠加段有独立门 overlay（默认关），描述过期 | 新发现 |
| 13（新） | configs/runtime.lua meshlet 块头注释 | 「★1-a1ii-α 起 enable 开 = 恒 L0 簇影子叠加」——同样过期于债-105 | 新发现 |

## 附录 B：关键数字复算公式

```bash
# gate_lint 60 行 / 53 门
awk '/const GateRule kGateRules\[\]/,/^};/' rgl/gate_lint_impl.cppm | grep -c '{"'          # 60
awk '/const GateRule kGateRules\[\]/,/^};/' rgl/gate_lint_impl.cppm | grep -oP '\{"[^"]+"' | sort -u | wc -l  # 53
# meshlet 段 14 行
awk '/const GateRule kGateRules\[\]/,/^};/' rgl/gate_lint_impl.cppm | grep -c 'meshlet\.'   # 17（14 行门名 + 3 行消息内提及）

# 29 处 emit（债-127 复算口径）
grep -rn "\.emit(" rgl/*.cppm | wc -l                                                        # 29
# 边表 28 / 门 7
sed -n '151,189p' rgl/offgraph_sync.cppm   # RgSyncEdgeId 枚举：evssm 3 + izb 13 + cull 8 + meshlet 4
grep -n "kSyncGateCount = " rgl/offgraph_sync.cppm                                           # 7

# buffer 附件：注册 4 / 进图 3 / 真消费者 1
grep -rn "registerBufferAttachment(" rgl/ --include=*.cppm | grep -v "void register"          # 4 调用点
sed -n '488,495p' rgl/offgraph_sync_impl.cppm                                                # kAttachmentSyncEdges = 1 条
# 进图 3 个 = declareBufferProductions 客户：izb_head/izb_nodes（vkrenderer_offgraph.cppm:141,166）
#   + meshletVisibleFlags（vkrenderer.cppm:371-372 注释 + syncIzbGraphAttachments 同款调用）；meshletCullItems 无声明

# 图事实谓词唯一导出 + 3 问者
grep -rn "graphHasDeferredLight" rgl/ --include=*.cppm                                       # 声明 :650 + 3 生产调用
# isSchedulable 生产侧零调用者
grep -rn "isSchedulable" rgl/ tests/ --include=*.cppm                                        # 只有 tests 调

# 6 处现查（含 initFsr 5 调用点）
sed -n '632,649p' rgl/main_scene.cppm        # computeFsrUsable has() lambda
sed -n '299,307p' rgl/vkrenderer_offgraph.cppm   # findAttachmentSlot ×5
sed -n '100,101p' rgl/vkrenderer_offgraph.cppm   # izbPreFrame depth find_if
sed -n '22,24p' rgl/picking_impl.cppm      # object_id
sed -n '51,52p;90,91p' rgl/frame_capture_impl.cppm  # swapchain kind + slotOfName

# 31 枚出厂空开关（清单口径）
grep -n "taa\." rgl/scene_config_impl.cppm | head -10      # taa 9 键（:106-114）
cat configs/runtime.lua    # fsr×2 / ssr 系数×6 / llm_shot×3 / headless_size / ui_font_size
cat configs/shadow.lua     # pcss×3 / evssm×4 / izb.grid / rpc_port 在 diagnostics.lua

# 9 个 ensure
grep -c "std::expected<uint32_t, Error> ensure" rgl/render_graph_rtslot.cppm   # 7
grep -n "ensureDepthSampleSlot\|ensureFsrSampleSlot" rgl/vkresources.cppm      # +2 = 9

# 7 个 decorateXxxPass + 装饰函数本体零 logger
grep -c "^bool decorate.*Pass" rgl/render_graph_decorate.cppm                  # 7
grep -c "RLLogger\|logger" rgl/render_graph_decorate.cppm                      # 0

# 8 附件级校验 + 5 pass 级 precheck
sed -n '179p' rgl/render_graph_impl.cppm        # 注释列 8 条
grep -c "precheck" rgl/render_graph_impl.cppm   # 5 函数（ReadUsages/Draw/Fullscreen/Blit/Pass）
```

（完）

---

## 附录 C：同一根因多层各喊一次（配对表，11 个根因）

「申报点 ≠ 提供方」的可数证据。**读法**：同一行里各层的判据**是否同一条**决定它是不是重复申报——
"不同"往往意味着**出厂态下有一层结构性沉默**（最危险的是 switch×overlay 那一行）。

| 根因 | 各层位置 | 判据是否相同 |
|---|---|---|
| `meshlet.switch` × `cull_replay ≠ cpu` | lint `gate_lint_impl.cppm:259-264` / 装配 `main_meshlets.cppm:216-221` | 不同（装配排在设备规格闸门之后） |
| `meshlet.switch` × `overlay` 互斥 | lint `:265-270`（要 `overlay=true`）/ 装配 `main_meshlets.cppm:246-250`（switch 生效就喊） | **不同 ⟹ 出厂 `overlay=false` 时开 switch，只有装配层喊，lint 沉默** |
| `meshlet.coplanar` 无消费者 | lint `:249-255` / INFO orphan 尾巴 `main_meshlets.cppm:674-677` / switch 互斥 warn 第二句 `:244-250` | 三条各不同 |
| `meshlet.overlay` 无条目可采 | lint `:228-232` / 装配按 `hosts==0` 事实 `main_meshlets.cppm:105-110` | 不同层（配置组合 vs 事实） |
| `meshlet.cull` 无消费者 | lint `:271-281`（两条）/ 装配 `main_meshlets.cppm:697,711` / 建表侧 `main_meshlet_tables.cppm:532` | 三处 |
| `meshlet.shadow_lod` × `enable` | lint `:282-286` / 装配按"零条目" `main_meshlets.cppm:426` | 不同（lint 判 enable，装配判产物命中） |
| `meshlet.mdi_shadow` 宿主不在 | lint `:292-297` / 装配 `main_meshlets.cppm:568` / 载体未建 `:503` | 三处 |
| `dump_gbuf` 需 `llm_debug` + deferred | lint error `:113-115` / `frame_capture_impl.cppm:139` / `diagnostics_impl.cppm:272`（越界） | 三层 |
| `izb` × 非 deferred / × `mode=none` | lint error `:116-122` / 运行期**刻意不喊**的静默早退 `vkrenderer_offgraph.cppm:104-105` / izb 空段 warn `:542` | 分工明确（lint 喊配置、运行期喊场景）——**本表里唯一一处"各层判据本就不同且都该如此"** |
| `fsr.temporal` / `fsr.jitter` 要 `aa=fsr` | 引擎内**零出声** / `tools/smoke.py:563-566` / `tools/imgqa/engine_profile.cpp:357-363` | 工具两处同谓词；**引擎内没有守卫**（S2 的 C 形态要收的就是这一格） |
| `--clip`（`scene.taa.clip_mode`）要 `aa=taa` | 引擎内**零出声** / `tools/smoke.py:560-562` | 只有工具层守 |
| `velocity_field` 与 `aa=fsr` | 方向是 **fsr 要 velocity**（`main_scene.cppm:640`）；`aa=fsr` 解析期隐含打开 velocity（`runtime_config_impl.cppm:258-263`） | 反向（velocity 单独开）**无人守**，且其"非 FSR 消费者"理由已漂（附录 A 第 1 条） |

**这张表怎么用**：S2 每消灭一格，就在本文勾掉一行；如果某次改动让表**变长**（新根因又跨层了），
说明它在加申报点而不是在收提供方。
