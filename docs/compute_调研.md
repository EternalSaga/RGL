# compute 现状调研：图外 compute、渲染图边界、队列归属

> **状态**：`现行`（2026-10-14 立，同日 **§6 D1 定案**）。本篇 = **图外 compute 的事实基线 + 扩渲染图
> 形态的定案载体**；批次切分在 §7，**没有排期**。
> 触发：用户把优先级从「规模上限」（已判定不处理，→ `docs/还债总账.md` §3 降级条）让到
> 「把 compute 纳入声明式渲染图」，先普查现状。
> 🔴 **动手前先读 §2.1 与 §6**：形态定案 = **扩资源词表（buffer 档 + storage usage）+ plan-only**。
> 两条历史否决里 **§5.2 那条（B 档）仍然有效**，它就是 plan-only 红线的来源——**照"让图代发 barrier"
> 实施会撞上它**，而 §2.1 是"为什么这次能拆墙"的三条在场事实。
> 所有队列/设备数字**绑本机**（AMD Radeon 8060S / RADV STRIX_HALO / gfx1151 / Mesa 26.2.3），跨机先复测。

---

## 0. 怎么复算本篇的清单（反抄名单口径）

本篇的枚举是**快照**。口径给全，别信副本，要用的时候现跑：

| 清单 | 真相源 / 复算命令 | 快照值 |
|---|---|---|
| 引擎 compute 全集 | `ls rgl/shaders/*.comp.slang` | 9 枚 |
| 提交落点全集 | `grep -rn vkQueueSubmit rgl/*.cppm` | 14 处，**全部** `getGraphicsQueue()` |
| 图能表达什么 | `RgAttachmentKind` / `RgUsage` / `RgPassKind` 三个枚举本体（`rgl/render_graph.cppm`） | 见 §2 |
| 队列族现场 | `vulkaninfo \| grep -E "queueFlags\|queueCount"` | 见 §3 |
| 图外同步的既有账 | `docs/还债总账.md` **债-75** | buffer 侧零对账 |
| 图属 transient 资源的生命周期机器在哪 | `RenderTargetCache` 本体（`ensure` 幂等 / `generation` 漂移 / `heapSlot` 跨 resize 复用三条语义看它，别信摘要） | 已建好，**只管 image**（§2.1） |

---

## 1. 图外 compute 全景 —— 以及「为什么在图外」的**真实**原因

| shader | 录制函数 | 帧内位置 | 在图外的**真实原因** |
|---|---|---|---|
| `mip_gen.comp` | `MipGen::recordChain` | **启动期**（`texture_upload_*`） | **A 类：不逐帧** |
| `env_prefilter.comp` | `EnvPrefilter::recordPrefilter` | **启动期**（`texture_upload_cubemap`） | **A 类：不逐帧** |
| `cull.comp` | `GpuCull::recordCullPass` | 帧首，**独立 CB** | **B 类：提交模型** |
| `stream_fanout.comp` | `GpuCull::recordFanoutDispatch` | 同上，**独立 CB** | **B 类：提交模型** |
| `meshlet_cull.comp` | `MeshletCullService::recordDispatch` | 主 CB，`recordFixedDataPath` 后、pass 循环前 | **C 类：图表达不了依赖面** |
| `izb_create.comp` | `IzbShadow::recordCreateDispatch` | 主 CB，sandwich 在 shadow0 前后 | **C 类** |
| `izb_prime.comp` | `IzbShadow::recordPrimeDispatch` | 启动期一次性 | A + C 混合 |
| `evssm_moments.comp` | `EvssmShadow::recordFrame` | 主 CB，shadow0 之后 | **C 类** |
| `evssm_downsample.comp` | 同上（同一次 `recordFrame` 内） | 同上 | **C 类** |
| —（vendor FSR） | `FsrUpscaler::recordFrame` | 主 CB，tonemap pass 前 | **D 类：vendor 黑盒**（一次调用录完全部 job，无 per-pass 录制 API） |

**这个四分类是本篇最重要的一条结论**，因为「compute 该不该在图外」这个问题在历史上被回答错，
错就错在把 A 类和 B 类当成了 C 类的论据：

- **A 类（不逐帧）**：`mip_gen` / `env_prefilter` 的调用方是 `texture_upload_*`（资产上传路径）。
  它们**永远不可能**进帧图——帧图描述的是每帧的工作。**用它们当「逐帧 compute 该在图外」的先例是范畴错误。**
- **B 类（提交模型）**：`cull` 走 `cullCommandBuffers[frameSlot]`——独立命令缓冲 + 独立提交 +
  fence 读回。它在图外的原因是**它不在主命令缓冲上**，与「它是 compute」无关。**同样是范畴错误。**
- **C 类（图表达不了）**：这才是真原因。这些 dispatch 的依赖是 storage buffer / scene data /
  descriptor heap 槽，而图**没有描述 buffer 的词汇**（§2）。
  最硬的例子：`EvssmShadow` **连 `VkImage` 句柄都没有**（阴影图归 RT 缓存，本模块只有 heap 槽），
  它的依赖靠一条**全局 stage barrier**（`stageBarrier`，无句柄、纯执行依赖）表达——
  这种形态在「attachment + layout」模型里**根本写不出来**。
- **D 类（vendor）**：FSR 是黑盒，图连"它内部有几个 pass"都不知道，谈不上纳图。

---

## 2. 渲染图的真实边界

三个枚举本体（**图的全部词汇表**）：

- `RgPassKind` = `drawMesh` / `blit` / `drawLight` —— **无 compute**
- `RgUsage` = `read` / `write` / `readDepth` / `writeDepth` / `blitRead` —— **无 storage 读写**
- `RgAttachmentKind` = `swapchain` / `depth` / `offscreen` / `depthOffscreen` —— **无 buffer kind**

图的产出 = 每 attachment 的**最终 layout** + `beginRendering` 的 layout 条目 + `RgBarrierPlan`
（pass 间 barrier）。`usageInfo`（`rgl/render_graph_impl.cppm`）是 usage → {layout, stage, access}
的**唯一推导点**——加 compute 必改它，`precheckPass` 也要改（它按 kind 查词表，未知 kind 直接抛）。

🔴 **真正的不变式不是「compute 全在图外」，是「图只跟 image attachment 的布局链」。**

这条才是决定"什么进得去、什么进不去"的东西。而它是一条**当前实现的范围限制**，
不是关于 compute 的架构原理——**债-75 就是这条限制的账单**（§4）。

**图的边界之外、本来就不该进图的部分**（这些不是债，是设计）：descriptor/sampler heap 的
host-write barrier、scene data staging 拷贝、swapchain 信号量、帧槽 fence。
任何"扩图"方案都不该碰它们。⚠ `RgAttachmentKind` 一旦有 buffer 档，**最大的诱惑就是把 scene
data staging 塞进来**——这条拒绝要落进 `precheckPass` 的失败分支，不能只写在这里。

### 2.1 但边界上的**机器**已经建好了（2026-10-14 重裁时实测）

拆墙的前提不是"重新设计一套"，而是下面三条已在场的既有事实：

1. **transient 生命周期已经是声明驱动的**。`RenderTargetCache` 已是完整的 transient 管理器：
   按名幂等 `ensure`、extent/format 漂移 → `waitIdle` 后重建 + `generation+1`、消费者按
   generation 重置布局追踪、`heapSlot` 首建领槽且**跨 resize 复用**（管线 baked 的槽号永久有效）。
   而它的驱动源就是编译图的附件表（`Renderer::ensureOffscreenAttachments` 逐帧遍历
   `CompiledRenderGraph::attachments`）。**"在声明图里管 transient 生命周期"这一半不是待建项。**
2. **图外 compute 已经在按图声明的身份与名字挂/取资源**。三个 sandwich 钩子的挂点判据就是图声明的
   pass 身份（`pass.kind` + `pass.state`），FSR 的两个图属输入也是按**编译图里的附件名**去 RT 缓存取
   （`vkrenderer_offgraph.cppm` 的 `fsrTonemapPassPre`，其注释还写了为什么不传句柄）。
   ⟹ **位置契约本来就是数据，只是它现在活在 C++ 的字符串比较里，不在图里。**
3. **词表缺失已经在制造形态扭曲**：velocity 是计算结果，因图无 compute 档而由全屏 `drawLight` 产
   （§5.3——注意它三条理由里"vertex 开销可忽略"那条**仍成立**，所以这条是"理由之一作废"，不是纯被迫）。

**模块侧的对照代价**（同一件事的更差做法，实测）：IZB 门开时若 extent 漂了，重建的是**整个模块对象**
（含 shader 与描述符）；RT 缓存做同一件事是"换一张图、槽号不变"。EVSSM 的矩图同理住模块里。
无 erase 路径的隐患另记 §8。

---

## 3. 队列归属：独立 compute 队列**从来没被拿到过**

> ⚠ 本节描述的是**处置前的现场**（那些成员现已删除，处置见 §3.1）；留着是因为**根因分析仍然有效**
> ——它解释的是"为什么独立队列这件事不能靠常驻句柄解决"。

本机队列族现场：

| family | flags | queueCount |
|---|---|---|
| 0 | GRAPHICS \| COMPUTE \| TRANSFER \| SPARSE | **1** |
| 1 | COMPUTE \| TRANSFER \| SPARSE（**无 graphics**） | **4** |
| 2 / 3 / 4 | video decode / video encode / sparse | 1 / 1 / 1 |

把 `QueueFamilyManager` 的选择逻辑跑一遍（规则原文见其类注释）：

| 角色 | 规则 | 落点 |
|---|---|---|
| Graphics | 首个带 eGraphics 的族 | family **0** |
| Compute | 首个带 eCompute 的族 | family **0**（family 0 就带 COMPUTE，先被扫到） |
| Transfer | 优先「仅 TRANSFER 不含 graphics/compute」的专用族 | family 1 有 COMPUTE ⟹ 排除；family 4 只有 SPARSE ⟹ 无专用族 ⟹ 回落 family **0** |

再由 `allocateQueues` 在 family 0（queueCount=**1**）上发号：Graphics 拿 index 0；
Compute 与 Transfer 走到 `else` 分支 `{familyIdx, maxAvailable - 1}` = **{0, 0}**。

⟹ **三个角色是同一条物理队列。** `DeviceContext` 的 `computeQueue` / `transferQueue` 两个成员
注释写「保留给未来使用，当前未用」，且**连 getter 都没有**（`grep getComputeQueue rgl/` 为空）
——不是"闲置"，是**从没被读过的死状态**。

**根因是选族规则，不是"没需求"**：`Compute = 首个带 eCompute 的 family`，而**任何有图形队列的
桌面/APU 上 family 0 必然带 COMPUTE** ⟹ 注释里「Compute 无匹配时回落 graphics family」这条路径
**恒真** ⟹ 真正独立的 compute family（本机 family 1：**4 条** COMPUTE|TRANSFER 队列）**永远选不到**。
NV / Intel 同理。

**全部 14 处 `vkQueueSubmit` 都打 `getGraphicsQueue()`**（cull CB 也是——独立 CB ≠ 独立队列）。

### 3.1 处置（D3 定案 2026-10-14，已执行）：删，不是修

**删掉 `QueueFamilyRole` 三角色 + `computeQueue` / `transferQueue` 两个成员 + 两个白建的命令池**，
`QueueFamilyManager` 退化成"取首个带 eGraphics 的族、申请其 index 0 一条队列"。三条定案理由：

1. **删才是零行为变化，修才是引入差异的那一方**。`allocateQueues` 的 else 分支把 Compute/Transfer
   钳回 index 0 且**不再增长计数** ⟹ `vkCreateDevice` 现在要的就是 family 0 × 1 条队列，
   删掉两个角色后 create info **一字节不变**；反过来"Compute 优先不含 graphics 的族"会真的多要
   一个 family（本机 family 1），跨设备形态还不一致（Intel 单族 = 不变，NV / AMD = 变）。
2. **"独立 compute 队列"不是设备保证，是运行期查询结果**（很多设备的族是全功能的）。把它建模成
   常驻成员 ⟹ 在单族设备上 `computeQueue == graphicsQueue`，那个句柄永远半真半假、注释永远得挂
   着"可能与图形同一条"。**修选族规则修不掉这个误导，只有删能修掉。**
3. 修规则 + 加 getter 的**收益是 0**：B1/B2 不碰队列，D4 已定"不做"。而删的收益立刻兑现——
   误导消失、少两个成员、少两条同 family 上的 `VkCommandPool`。

**护栏（已落地 + 变异实测）**：`tests/test_device_primitives.cppm` 的 gpu 用例钉三件事——只申请一条
create info、只申请一条队列、family = 首个带 eGraphics 的族（测试里**独立重算**当第二实现）。
变异实测（2026-10-14）：把期望的 `queueCount` 改成 2 ⟹ **红**（断言非空转）。

⚠ **这条护栏的作用域要连着读，别当成全变异覆盖**：
- "取第一个还是最后一个 graphics 族"那一支**在本机测不出红**——本机只有一个 graphics 族
 （family 1 是 compute-only），"第一个"与"最后一个"是同一个值 ⟹ 那一支是**跨设备护栏**。
- "重新加回一个角色"在本机同样**不可区分**：旧版三角色在本机坍缩成一条 create info / 一条队列
 （这正是"删是零行为变化"的同一件事）⟹ 输出面区分不了新旧实现。
⟹ 本用例的真实价值 = 钉住"**不再多要队列**"这个决定 + 跨设备的选族正确性；它**不是**用来证明
"删除改了什么"——删除本来就不该改行为。真正防住"又长出常驻 compute 句柄"的是**编译器**
（成员已不存在，写 `getComputeQueue()` 直接编不过），测试只兜"多要队列 / 选错族"这一半。

**将来真要第二条队列时的形态（三条，缺一不做，同步写进 `QueueFamilyManager` 类注释）**：
① 能力查询返回 `optional` + **查不到就打日志出声**，禁再建常驻成员；② 提交面分叉的两个必要条件
= 查询真的拿到独立族 **且** 图派生的"本帧无消费者"集合非空（§7 批次 B2 的产物）；③ 跨界资源的
queue-family 所有权转移要一并补（全仓现为 0 处）。**删掉的那两条"专用 transfer 族 / compute 回落"
分支正是"假装独立队列是通用能力"的地方——留着它，下次长回来的还是常驻句柄。**

---

## 4. 正确性现状：三处只靠录制顺序 + buffer 侧零对账

| dispatch | barrier 谁发 | 位置契约有无机器校验 |
|---|---|---|
| `MeshletCullService` | 服务自己（buffer 侧 `MemoryBarrier2`） | **无**——三条前置约束只活在 `recordMeshletCullDispatch` 的注释里 |
| `IzbShadow` | 模块自己（layout sandwich） | **有**——A 档 `layoutDrift` 对账（`recordFrame` 返回真实布局） |
| `EvssmShadow` | 模块自己（全局 stage barrier） | **无** |
| `FsrUpscaler` | vendor 内部 + 调用方手补 | **无** |

**债-75**：buffer 侧约 15 条逐帧手写 barrier，**零对账护栏**（图外布局对账只管 image）。

**「缺 barrier VVL 会报」实测不成立**：descriptor heap 上的 buffer 访问，syncval **开不开都 0 条**。

最具体的一例（`vkrenderer_offgraph.cppm`，FSR 输出 → tonemap 采样读，代码注释原文要点）：

> ③ FSR 输出 → tonemap 采样读：**必须补执行依赖**（缺了是 UB，不是"可能慢一点"）。
> 为什么图属 barrier 覆盖不到：FSR 的输出图不是图附件（零拷贝烘采样槽），而本 pass 的全部图
> barrier 都物化在 `fsrTonemapPassPre` 之前。
> 🔴 **VVL 抓不到**：布局本身是对的（输出常驻 GENERAL，GENERAL 可采样），缺的只是
> compute-write → fragment-read 的**顺序**——「布局对、缺顺序」正是 syncval 的盲区。

⟹ 三处正确性**只靠录制顺序恰好正确**，且**没有机器能抓到它变错**。这是本次调研找到的最实的风险。

---

## 5. 历史裁决：三条，其中否决 1 已于 2026-10-14 重裁推翻

| 裁决 | 载体 | 现状 |
|---|---|---|
| **否决：render graph 增 compute pass kind** | `docs/还债总账.md` §3 | ✅ **2026-10-14 重裁：推翻**（原理由不成立，§5.1；触发条件已满足）。**形态 = 扩资源词表 + plan-only，定案见 §6 D1** |
| **否决：14.4b B 档（图外资源用 external-use 声明交图代发 barrier）** | `docs/phase14_plan.md` §1.21 六 | **理由成立**，其理由③是本任务的**硬约束**（§5.2）⟹ 它就是 §6 D1 的 plan-only 红线 |
| **定案：velocity 走全屏 `drawLight`，不新增 compute kind** | `docs/phase14_plan.md` §1.10 修正一 | 成立，但它的理由①已被 ★1-a2-i 部分推翻（§5.3） |

### 5.1 否决 1 的理由为什么不成立

原理由：「违背既有架构不变式（"compute 全在图外"，cull/mip_gen/env_prefilter 三处先例）」。
拆完这三处"先例"（§1 的 A/B 分类）：两处是**范畴错误**，剩下的四处逐帧主 CB compute
（IZB / EVSSM / meshlet_cull / FSR）**全是"图没有 compute kind"的结果而非理由** ⟹ 自我论证。

**触发条件也已满足**：否决时写「等出现第三个需要图内 compute 的新功能再做」，当时数到 2（IZB / EVSSM）；
此后新增 meshlet_cull（★1-a2-i）与 FSR upscaler 两枚。

### 5.2 仍然有效的硬约束（任何新方案绕不过）

B 档否决理由③：**barrier 改由图发 ⟹ 模块无从返回真实布局 ⟹ A 档 `layoutDrift` 对账失去断言输入。**

一般化成一句：**声明不能替代「发 barrier 的人返回真相」。**
这是本项目唯一一次为「护栏 vs 便利」做过正式裁决，判据是硬的（B 档的"少写代码"是收益，
但它把运行期断言换成了编译期声明）。任何"扩图"方案必须保住这个性质——
即形态必须是 **申报 + 对账（双向）**，不能是 **声明 + 代做**。

### 5.3 velocity 定案的理由①已失效（但定案本身不必翻）

当年三条理由：① 图无 compute kind ② 不碰闲置 computeQueue ③ 全屏 draw 的 vertex 开销可忽略。
① 已由 §6 D1 定案消掉（词表要扩了）；② 见 §3——那条队列**不存在**，"不碰它"不是成本而是**没得碰**；
③ 仍成立 ⟹ **D1 定案不要求 velocity 改形态**。它真正的待办不在本篇：闭式解（两条 spin 矩阵 + 深度）
在出现 per-entity 动画时先失效的是**正确性**不是开销，那笔账住 `docs/还债总账.md` **债-35**。

---

## 6. 决策点（D1 已定案 2026-10-14；余下未决从 D3 接）

- **D1 推翻否决 1 吗？→ ✅ 定案：推翻。形态 = 扩「资源词表」，且第一版 plan-only。**
  用户 2026-10-14 拍板。原三形态的处置：**(a) 保留**为这套形态的对账半边、**(b) 是它的产物**、
  **(c) 的"执行器分发"明确不做**。定案形态四条：

  1. `RgAttachmentKind` 加 **buffer 档**、`RgUsage` 加 **storage 读/写**——**打的是 §2 那条真不变式**
     （图只跟 image attachment 的布局链），不是打"没有 compute pass kind"那个症状。依据 = §2.1。
  2. 🔴 **plan-only 红线**：图出**计划 + 校验**，节点体**仍由模块录、仍由模块返回真实布局 / 真实发出的
     barrier**，执行器现场核对。**发射权第一版不许交给图**——一旦图代发 barrier，模块就无从返回真相，
     A 档 `layoutDrift` 那类对账对这类节点**变成恒真**，正是 §5.2 那条**仍然有效**的否决（B 档）的形态。
     落地形态因此很具体：pass 循环里那几个硬编码 sandwich 钩子改成**按图声明的节点表驱动**，
     **钩子内部一行不改**。等 §7 判据能红，再单独裁"要不要把发射权交给图"。
  3. **尺寸与句柄由 C++ 按名注册，Lua 只声明"存在 + 谁产谁消"**。理由：buffer 的字节数是
     f(renderExtent)（IZB 的 nodes / mask）或 f(资产量)（meshlet 标志表），而算这个字节数的函数今天
     就在模块里——让 Lua 声明尺寸 = 给同一事实开第二份载体。**dispatch 实际写的上界必须与 ensure 出的字节数对账。**
  4. **拒绝面**：scene data staging / heap host-write / pushData / 信号量 / fence 一律不许声明成附件
     （§2 那五类），且拒绝要落进 `precheckPass` 的失败分支。

  改动面以**符号本体**为准，本文不维护落点清单：三个枚举 + `usageInfo` + `precheckPass` +
  barrier 物化函数（现返回 image 形状）+ 执行器的逐附件布局追踪表（现按"每槽一个 image layout"建模）。
- **D2 第一个吃螃蟹的模块选谁？→ 改成 EVSSM 矩图**（原选 meshlet_cull，那是从"申报面"出发的排序，
  在定案形态下不成立）。新排序与理由：
  ① **EVSSM 矩图**——它是 **image** ⟹ 走**已有**的 RT 缓存路径、**零新机器**；搬过去就**删代码**
  （模块不再持私有图、不再自己管重建）；且顺手解掉"本模块只有 heap 槽、没有句柄"那条——依赖从全局
  stage barrier 变成可声明的 storage-write → sample-read 边。
  ② **meshlet_cull** = **buffer 档首发**（零 image、纯 buffer、位置契约最明确），要新 kind，第二刀。
  ③ **IZB 放最后**——sandwich + 已有 A 档护栏，动它必须保住既有对账（§5.2 的断言输入不能断）。
- **D3 `computeQueue` / `transferQueue` 怎么处置？→ ✅ 定案并执行：删**（2026-10-14，本仓一贯
  "宁删不留假能力"）。判据与"将来怎么拿第二条队列"的三条形态 → §3.1（本条不再重述）。
  连带删掉的还有 `applyRoleFallbacks` 里"专用 transfer 族 / compute 回落"那两条分支——
  **那段逻辑正是"假装独立队列是通用能力"的地方**。
- **D4 队列归属要不要一并做？** **不要。** 见 §9 N1。⚠ **扩词表不改变这条结论**：词表解决"依赖能不能被
  声明"，队列要的是"有没有可重叠的活"，两者正交。而且换队列新增的正是 §4 那类"布局对、缺顺序"的边
  （N3：syncval 抓不到）⟹ **在申报对账长出牙之前换队列 = 往唯一无护栏的风险类上加杠杆**。

---

## 7. 批次切分与判据要求（按 §6 D1 定案形态重排）

| | 内容 | 独立收益 | 风险 |
|---|---|---|---|
| **B1**（= 原 Increment 1，起手不变） | **图外工作的申报面**：compute/图外用户向执行器申报 `(挂点, 读的资源, 写的资源, 期望真实布局)`；执行器 ① **校验位置契约** ② **对账 buffer 侧同步**（barrier 仍由模块发，申报"我为谁发了什么"，现场核对）。附带把 sandwich 钩子的挂点判据从 C++ 里的字符串比较搬进图声明 | 债-75 结案；meshlet_cull 三条约束从注释变断言；FSR 那条"VVL 抓不到的 UB"进对账 | 低——**不改 pass kind、不改执行器分发**，可逐模块推 |
| **B2**（= 原 Increment 2 + D1 第 1 条） | **扩资源词表**（buffer 档 + storage 读写）+ 申报面升成**依赖图**（资源 → 生产者 → 消费者）+ 尺寸/句柄按名注册 | 校验"有没有未申报的写后读"（这才是替代"录制顺序恰好正确"的东西）；EVSSM 矩图与 IZB 那几块 buffer 有家；**重建粒度从"整个模块对象"降到"一个资源"**（§2.1） | 中 |
| **B3**（**明确不做**） | 把 barrier **发射权**交给图 / `RgPassKind::compute` 执行器分发 | 统一"谁执行" | 高，且这是 §5.2 否决的形态——**只有在判据 6 有牙之后才允许重新讨论** |

**判据要求**（按本仓纪律，写不出反向断言的护栏不配进门）：

1. **门关逐位不变**——`just regress selftest` 三锚逐字复现（出厂 `95edc8ccd7a4…`；
   meshlet 门关 `54e406655f16…`。**引用锚必须写明哪一态**）。
2. **位置契约断言必须能红**——把 `recordMeshletCullDispatch` 的调用点挪到 pass 循环**之后**，
   断言必须炸（变异红线，→ skill `rgl-test-discipline`）。
3. **buffer 对账必须能红**——故意抽掉一条 barrier，对账必须点名。
4. `just gates` 全绿 + syncval 0 条。
5. **尺寸对账必须能红**（B2）——把某个按名注册的附件字节数改到小于 dispatch 实际写的上界，
   断言必须点名；还原回绿。防的是"图校验一个虚构"（声明与真实写面分家）。
6. **申报通道不许被图取代**（plan-only 的护栏，也是 B3 的准入门）——让某模块**谎报**它离开时的真实
   布局，对账必须炸。**若这条测不出红，说明发射权已经在图手里 ⟹ 红线已破**，此时不是补测试，是回退形态。
7. **resize 后 heapSlot 不变**（B2，EVSSM 矩图迁入的验收）——改窗口尺寸触发重建后，采样侧 baked 的
   槽号必须仍是同一个（RT 缓存的 `heapSlot` 跨 resize 复用是本次迁入的**唯一**结构性收益来源，
   它不成立就等于白搬）。

---

## 8. 待挂号（本批**未落**总账，等用户选处置）

1. **（2026-10-14 重裁时实测）`RenderTargetCache` 没有 erase 路径**：条目一旦建出就常驻，
   `heapSlot` 从 Runtime 段领出后**永不归还**。今天**无害**——图在启动期编译完就不再变（`Renderer`
   持有编译产物、之后不变），所以"附件集合"是常量。**触发条件** = 图能在运行期变（配置热重载、
   运行期开关装饰图、或任何"按帧重建编译图"的想法）⟹ 那时它是**内存 + 堆槽双泄漏**，且堆槽泄漏
   不可回收（Runtime 段槽位是硬上限）。B1/B2 都不触发它，但"把生命周期收进声明图"这句话的代价
   正在这条——**谁让图变成动态的，谁就先还这笔**。

> 原第 1 条（`computeQueue` / `transferQueue` 的误导注释）已于 2026-10-14 结案：句柄与命令池**删掉**，
> 定案与判据在 §3.1，代码侧的形态三条住 `QueueFamilyManager` 类注释。

---

## 9. 负知识（**带失效条件**——别再重查，但也别被读宽）

**N1：独立 compute 队列在本机无可兑现收益。**
- **作用域**：本机 = APU（STRIX_HALO）+ 交付路径 GPU-bound；且当前**每一处** compute 的输出
  **本帧就要被消费**（cull 读回、IZB 建 mask、meshlet_cull 写标志给 mesh shader、FSR 输出给 tonemap）。
- **为什么**：family 1 那 4 条队列背后是**同一批 CU、同一份显存** ⟹ 独立队列不造吞吐，只在有气泡时填，
  而 GPU-bound 意味着气泡少。有依赖的活换队列也得等，只是把"同队列顺序"换成"跨队列信号 + 所有权转移"。
- **仍然要做**：队列归属与"compute 纳入渲染图"**正交**——后者的收益是**时序与同步的声明化**，不是吞吐。
- **什么实验推翻它**：① 在 dGPU + CPU-bound 的机器上复测；② 出现**错帧/异步** compute 消费者
  （`cull_replay = gpu_lagged` 那条臂是雏形——它不等本帧）。

**N2：「compute 进图能修 MDI 那条 GPU 净亏」——不成立。**
- **作用域**：`docs/mdi_plan.md` 的净亏在**命令发射/阴影段**，与 compute 的同步声明化无关。
- **什么推翻它**：净亏主因若被证明是 barrier 开销（而非 task/MDI 发射成本）。

**N3：「buffer 缺 barrier VVL 会报」——实测不成立。**
- **作用域**：descriptor heap 上的 buffer 访问，syncval 开不开都 0 条；「布局对、缺顺序」是 syncval 盲区。
- **什么推翻它**：换驱动（本机 = Mesa 26.2.3 / RADV）或 VVL 版本升级后复测。

**N4：「本机枚举得到独立 compute 族（family 1，4 条）」不等于「有独立执行资源」。**
- **作用域**：本条否的是**把枚举事实当收益证据**这一种推理，不是否"独立队列有时有用"。
- **为什么**：族存在是**枚举事实**（驱动愿意暴露几个入口），独立执行资源是**驱动/硬件事实**
  （那些入口背后是不是不同的 CU / 不同的队列引擎）。收益只取决于后者。本机 APU 上 family 1
  与 family 0 背后是同一批 CU、同一份显存（N1）；换到真独立引擎的机器上结论可能反过来。
- **仍然要做**：§3.1 的形态①（能力查询 + 出声回落）照做——**查询本身是免费的，被禁止的只是
  把查询结果预先固化成常驻成员**。
- **什么推翻它**：在"compute 族确实落在独立引擎"的设备上（部分 dGPU 的异步计算队列 / 部分
  移动 SoC）复测，且同时满足 N1 的失效条件②（存在错帧消费者）。两个条件**同时**成立才有收益。

---

## 10. 相关指针（别处已承载，本篇不重述）

- **债-75**（buffer 侧同步零对账）与两条否决原文 → `docs/还债总账.md` §2 / §3
- **A 档 vs B 档完整判据** → `docs/phase14_plan.md` §1.21 六；IZB A 档护栏原型 → `rgl/izb_shadow_impl.cppm`
- **图外编舞分区**（`recordMeshletCullDispatch` / `izbLightPassPre` / `evssmLightPassPre` /
  `fsrTonemapPassPre` / `transitionAndTrack` / `layoutDrift` 调用点）→ `rgl/vkrenderer_offgraph.cppm`
- **pass 循环（图内盲消费）** → `rgl/vkrenderer_passrec.cppm` 的 `Renderer::drawFrame`
- **图编译判据形态（新 kind 的用例落点）** → `tests/test_render_graph.cppm`
- **验证流程与并行红线** → skill `rgl-verify`；**写测试硬纪律** → skill `rgl-test-discipline`
