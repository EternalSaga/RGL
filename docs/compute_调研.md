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
- `RgUsage` = `colorWrite` / `depthWrite` / `sampleRead` / `transferRead` / `transferWrite`
  —— **无 storage 读写**（⚠ 本行 2026-10-14 前写的是 `read`/`write`/`readDepth`/`writeDepth`/
  `blitRead`，那是改名前的旧名，**已漂**；名单口径按 §0 的规矩读枚举本体 `render_graph.cppm:72-78`）
- `RgAttachmentKind` = `swapchain` / `depth` / `offscreen` / `depthOffscreen` —— **无 buffer kind**

图的产出 = 每 attachment 的**最终 layout** + `beginRendering` 的 layout 条目 + `RgBarrierPlan`
（pass 间 barrier）。`usageInfo`（`render_graph_impl.cppm:49`）是 usage → {layout, stage, access}
的**唯一推导点**——加 compute 必改它（配套的两个角色判据 `isWriteUsage` / `isReadUsage` 同改）。

⚠ **kind 的校验落点分两处，别记成一处**（2026-10-14 实测，先前本行写「`precheckPass` 按 kind
查词表，未知 kind 直接抛」是把两处混了）：

| 校验 | 位置 | 形态 |
|---|---|---|
| **附件级**（格式 / extent / extentScale / history 四张 kind 白名单） | `registerAttachment` 调的四个 `validateAttachment*`（`render_graph_impl.cppm:145/173/188/208`） | 返回 `std::unexpected(RenderGraphInvalid)`，**不抛** |
| **pass 级** | `precheckPass`（`:336`） | 只按 `pass.kind` **三路分派**，不查任何 kind 词表 |
| **执行器构造期**（state 按 kind 分档、段名查词表） | `validateRenderGraphForExecutor`（`vkrenderer_impl.cppm:51`） | `throw std::runtime_error` |

⟹ **加 buffer 档要改的是第一行那四处 + 三个 kind 谓词**（`aspectOf` / `isRtBackedKind` /
`isDepthKind`，`:80-92`），而 `aspectOf` 对未知 kind **回落 `eColor`** = 一个静默的错方向，
新 kind 必须显式给出取值而不是靠回落。

🔴 **真正的不变式不是「compute 全在图外」，是「图只跟 image attachment 的布局链」。**

这条才是决定"什么进得去、什么进不去"的东西。而它是一条**当前实现的范围限制**，
不是关于 compute 的架构原理——**债-75 就是这条限制的账单**（§4）。

**图的边界之外、本来就不该进图的部分**（这些不是债，是设计）：descriptor/sampler heap 的
host-write barrier、scene data staging 拷贝、swapchain 信号量、帧槽 fence。
任何"扩图"方案都不该碰它们。⚠ `RgAttachmentKind` 一旦有 buffer 档，**最大的诱惑就是把 scene
data staging 塞进来**——这条拒绝要落进**附件级校验的失败分支**（`registerAttachment` 那条链，
🔴 不是 `precheckPass`，见 §2 的校验落点对照表），不能只写在这里。可执行形态 = 「buffer 档名
必须在 C++ 注册表里，未注册即拒」一条规则，五类诱惑一起杀掉。

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
⚠ **本节是问题陈述**：这条债已由批次 B1 三刀还掉（2026-10-14 → §7.4 / §7.7 / §7.8），
残留缺口拆成 **债-117**（2026-10-15 并案入债-119：compute↔compute 漏登记已由 B2-2 构造期对账守住，剩余 = 非 compute 参与者 + 无句柄执行序边）。下面那段「VVL 实测不成立」的查证**仍然有效**
——它正是"为什么要立机制而不是指望层"的理由，不随债结案而失效。

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
     （§2 那五类），且拒绝要落进**附件级校验的失败分支**（= `registerAttachment` 那条链，
     🔴 **不是 `precheckPass`**——那是 pass 级、不查 kind 词表，落点见 §2 的三行对照表）。
     可执行的形态是一条**双向规则**：buffer 档附件名必须在 C++ 侧注册表里（注册 = 提供算尺寸的
     函数），**未注册即拒**——这一条同时杀掉那五类诱惑（它们都不在表里），而不是靠列黑名单。

  改动面以**符号本体**为准，本文不维护落点清单：三个枚举 + `usageInfo` + `precheckPass` +
  barrier 物化函数（现返回 image 形状）+ 执行器的逐附件布局追踪表（现按"每槽一个 image layout"建模）。
- **D2 第一个吃螃蟹的模块选谁？→ 改成 EVSSM 矩图**（原选 meshlet_cull，那是从"申报面"出发的排序，
  在定案形态下不成立）。新排序与理由：
  ① **EVSSM 矩图**——它是 **image** ⟹ 走**已有**的 RT 缓存路径；搬过去**删代码**（模块不再持
  私有 `VmaImage`、不再自领 8 个 Runtime 段 per-mip storage 槽）。
  ⚠ **2026-10-14 实测修正本行原文的三处过度承诺**（原文：「**零新机器**」「模块不再持私有图、
  **不再自己管重建**」「依赖从全局 stage barrier 变成**可声明的** storage-write → sample-read 边」）：
  - **不是零新机器**：矩图要 **8 个 per-mip storage 视图 + 1 个全 mip sampled 视图**，而 RT 缓存
    现在只支持「一个 image + 一个 view + 一个 `SAMPLED_IMAGE` 堆槽」（`RtAttachment` 的
    `hasSampleDescriptor` / `sampleViewCreate` 是**单个**描述符链）⟹ 得先给它长出**多描述符附件**
    能力。这一刀的真实内容 = 扩 RT 缓存 + 迁入，不是纯搬。
  - **「不再自己管重建」对 EVSSM 收益为零**：它本来就不重建（尺寸 = `kShadowMapSize` 与窗口解耦、
    构造门位早退）。判据 7 因此恒真，口径已就地改准（见 §7 判据 7）。
  - **「变成可声明的边」要说清是哪种可声明**：EVSSM 那 9 条全局 stage barrier 早在 B1b 就换成了
    3 个边名（`offgraph_sync`），而 plan-only 红线 ⟹ **图不代发 barrier** ⟹ 迁入的收益是
    「依赖变成图**可校验**的对象」（校验有没有未申报的写后读），**不是**「图替它发」。
    把它读成后者就会去动 B3。
  ② **meshlet_cull** = **buffer 档首发**（零 image、纯 buffer、位置契约最明确），要新 kind，第二刀。
  ③ **IZB 放最后**——sandwich + 已有 A 档护栏，动它必须保住既有对账（§5.2 的断言输入不能断）。
- **D3 `computeQueue` / `transferQueue` 怎么处置？→ ✅ 定案并执行：删**（2026-10-14，本仓一贯
  "宁删不留假能力"）。判据与"将来怎么拿第二条队列"的三条形态 → §3.1（本条不再重述）。
  连带删掉的还有 `applyRoleFallbacks` 里"专用 transfer 族 / compute 回落"那两条分支——
  **那段逻辑正是"假装独立队列是通用能力"的地方**。
- **D4 队列归属要不要一并做？** **不要。** 见 §9 N1。⚠ **扩词表不改变这条结论**：词表解决"依赖能不能被
  声明"，队列要的是"有没有可重叠的活"，两者正交。而且换队列新增的正是 §4 那类"布局对、缺顺序"的边
  （N3：syncval 抓不到）⟹ **在申报对账长出牙之前换队列 = 往唯一无护栏的风险类上加杠杆**。

- **D5 图允不允许运行期变？→ ✅ 定案（2026-10-15，用户拍板）：允许。图从「启动期产物」降级为
  「脏驱动的派生件」，逐步向真正的逐帧动态图进发。**

  **触发** = 三次撞墙：B2-4①「不升成图附件」、B2-4②「出厂图不含 buffer 附件」（§7.17 诚实账）、
  IZB 的构造顺序更硬（extent 漂了换整个模块对象）。每次绕开单看都对（最小刀、不猜形状、不为机制
  改骨架），但累计效果 = **D1 第 3 条的 Lua 半边无消费者 + 债-119 的修法方向（「附件化后判据扩到
  compute → 图内」）永久不可达** = 看起来已完工的半吊子。**出路既不是把编译时机挪后（改骨架），
  也不是放弃图校验这条线，而是质疑那个从未被质疑的假设：「图编译一次就定死」。**

  **定案五条：**

  1. **图 = 派生件**：`CompiledRenderGraph = f(Lua 描述 + C++ buffer 注册表 + 场景构成)`，脏了在帧
     边界重编译。🔴 **自我修改 ≠ 原地打补丁**：不给 `CompiledRenderGraph` 开可变 API——增量 patch
     等于给每个校验（barrier 推导 / 游标 / store 完整性）写两遍判据，那是双源；重编译路径上**没有
     任何东西绕过编译器**，fail-fast、确定性（纯函数、无设备、毫秒级）、单一校验面全部免费保留。
     同形先例 = RT 缓存的幂等 `ensure`（规格失配才重建、`heapSlot` 跨重建稳定、判据 7 守着）——
     这次把 ensure 语义从单个附件升到整张图。
  2. **安全点纪律**：重编译只发生在 beginFrame 围栏等待之后、录制之前；吃编译图的派生物（B1a 相位表、
     执行器逐附件布局追踪表、`validateRenderGraphForExecutor`）在同一安全点重派生。**写成断言，
     不是注释。**
  3. **跨帧状态结转**：布局追踪表跨重编译**按名字结转**而非重置（它是布局的跨帧真相、A 档
     `layoutDrift` 的输入）；新附件给初始布局；被删附件必须证明无 in-flight 用户（frames-in-flight=2
     ⟹ 上一帧的 transition 可能还在 GPU 上跑）⟹ **延迟逐出**。新判据：「重建后布局表逐名等于重建
     前」+ 变异能红。
  4. **前置债**：原 §8.1（RT 缓存无 erase 路径）就此**转正为债-124**——原文「谁让图变成动态的，
     谁就先还这笔」，本条就是那个触发声明。
  5. **红线不变，且多一条新诱惑**：plan-only 不变（图仍只出计划 + 校验，模块仍发射）；**B3 维持
     否决，且本定案让它更容易复发**（「图现在什么都知道了，顺手代发 barrier 吧」——判据 6 继续守，
     重编译后其语义更强）；**「逐帧」是能力不是承诺**：判据 = 脏驱动、重建延迟 ≤1 帧，真逐帧结构性
     变化先拿 `just cpu` 量出重建成本再谈预付。

  **批次重切**：B2-4③ → **B2-4③′ IZB 动态性首发**（资源级动态：buffer 附件 + extent 漂移重建 +
  erase/结转，还债-124；A 档 `layoutDrift` 在重建后仍要有牙——IZB 是最硬的样本，形状由最硬迁入方定，
  与 D2 同理）；**B2-4④′ 升级存量**（meshlet 在试点后近乎免费；EVSSM 可选，其收益只有「依赖被图
  看见」）；**B2-5 pass 级动态**（运行期效果开关 = 本定案的独立收益；pass 出现/消失 → 相位表 /
  barrier 计划随重编译自动跟——这条才碰 B1a 的索引形态）。B3 仍不排期，准入门不变（判据 6 有牙）。

  **收益**：① 渲染器内部运行期切换效果（拍板动机）；② D1 第 3 条 Lua 半边活了；③ 债-119 修法方向
  恢复可达；④ 「注册 vs 编译」的构造顺序死结整体溶解——注册晚到 = 标脏，下个安全点自然吃到。

---

## 7. 批次切分与判据要求（按 §6 D1 定案形态重排）

| | 内容 | 独立收益 | 风险 |
|---|---|---|---|
| **B1a**（**已落地 2026-10-14 → §7.1**） | **图外工作的申报面 · 挂点那一半**：四个图外 compute 向执行器申报挂点，执行器 ① **校验位置契约**（帧内相位）② **覆盖对账**（门开⟺录过，两半边）。sandwich 钩子的挂点判据从 C++ 里的字符串比较搬进申报表 | meshlet_cull 三条约束 + 三个 sandwich 的「录在 pass 外」从注释变断言；图与模块的锚点错配提前到**启动期** | 低——**不改 pass kind、不改执行器分发**，实测逐位不变 |
| **B1b**（B1 的第二刀，**三刀全部落地 2026-10-14 → §7.4 / §7.7 / §7.8**） | 原写「申报面补**读写面** + 现场核对」——**形态已在 §7.2 换轨**成**申报即发射**（二合一：barrier 参数由边表物化、发射与记账同一次调用，手抄第二份命令流这件事写不出来）。已迁客户 = `EvssmShadow`（3 条执行序边）+ `IzbShadow`（13 条 buffer 边）+ `GpuCull`（8 条，6 buffer + 2 无句柄）⟹ 边表 24 条 / 门 6 道 | **债-75 结案**（残留缺口拆成债-117 = L2）；「挡错 mask」与「门关多一条命令」两类错变成当场红 | 低——每加一个客户 = 一张边表 + 一次 A/B 逐位对拍（实测三家各半天） |
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
7. **尺寸漂移后 heapSlot 不变**（B2，EVSSM 矩图迁入的验收）——重建后采样侧 baked 的槽号必须仍是
   同一个（RT 缓存的 `heapSlot` 跨重建复用是本次迁入的**唯一**结构性收益来源，它不成立就等于白搬）。
   🔴 **判据口径 2026-10-14 改过：原文写的是「改窗口尺寸（resize）触发重建后」，那个触发条件对
   EVSSM 恒不成立**——矩图尺寸 = `kShadowMapSize`（2048）**与窗口 extent 解耦**，且 `evssmShadow`
   全仓仅一处赋值、门位 `if (evssmShadow || …) return;` ⟹ **模块对象一旦构造永不重建**（对照 IZB：
   extent 漂了就 `make_unique` 换整个对象）。照原文测 = 一条**恒真**的判据，测不出有意义的红。
   改后的口径：**单测直接对同一个 name 调两次 `ensure` 传不同 extent**，断言 `generation` 变了而
   `heapSlot` 没变——不需要真 resize。（真程序侧另留一条：EVSSM 出厂档 resize 冒烟，断言
   `generation` **不漂**，防的是"迁入后莫名每帧重建"。）

### 7.1 B1 第一刀已落地（2026-10-14）：挂点从注释变成断言

**做了什么**（新模块 `rgl/offgraph_contract.cppm` + `_impl.cppm`，住 rlscene，判据在
`tests/test_offgraph_contract.cppm`；接线在 `vk_renderer:offgraph` 与 `drawFrame`）：

- **申报表** = `Renderer::offGraphWorkDecls()`（唯一真相源，四项：`meshlet_cull` / `izb_mask` /
  `evssm_moments` / `fsr_upscale`）。它替代的正是本行原来写的那句"把 sandwich 钩子的挂点判据
  从 C++ 里的字符串比较搬进图声明"——`pass.kind == drawLight && pass.state == "light"` 这类
  比较现在只剩钩子体内一份冗余，权威在表里，且**锚点用 `pass_vocabulary` 的状态名常量**
  （改名会启动期 throw，而不是画面悄悄不对）。
- **相位模型**（帧内单调计数）：帧首段 = 1、第 i 个 pass 之前 = `2+2i`、其**体内** = `2+2i+1`、
  循环之后 = `2+2N`。两条不显然的设计，都是被变异逼出来的：
  ① **为什么是相位而不是布尔**——判据若写成"在不在 pass 循环里"，把 meshlet_cull 挪到循环
  **之后**时布尔会重新变 false ⟹ 判据 2 要求的变异全绿。
  ② **为什么 pass 还要再分两档**——三个 sandwich 的真实契约是「录在 dynamic rendering **之外**」
  （IZB 自己 `beginMaskPass`、FSR 发 barrier + compute），只按 pass 分档的话，把挂点从
  `beginRendering` 之前挪到之后就完全 invisible。并档变异实测：只有"建表"那条红，
  所以「体内录制必须红」这条用例是**独立**的判据，不是重复。
- **两条机器可查的判据**：`enter()` 查位置契约（相位对不上 = 出声并点名期望/实际）；
  `checkCoverage()` 查覆盖，**两个半边**——门开着却整帧没录（静默旁路）、门关了却录了
  （破「门关零命令」这条逐位不变红线）。门位在**帧末**求值（IZB / EVSSM 首帧懒构造），
  且必须关于**门与输入**，不许写成"刚才 enter 过没有"（那会让断言自证）。
- **plan-only 红线守住了**：本层零 barrier、零执行权，`drawFrame` 的命令流一条不多
  （判据 1 实测见下）。申报失败只 `logger->error` 出声，不改行为。
- 顺带：`drawFrame` 因加了 4 处相位推进越过 100 行红线，按本文件既有口径把 blit 块提为
  文件内函数 `recordBlitPass`（命令与参数逐条不变，判据 1 覆盖）。

**没做什么**（别把这一刀读成 B1 完成）：读写面与 **buffer 侧 barrier 对账 = B1-b**（后续三刀
已落地 → §7.4 / §7.7 / §7.8）；`RgOffGraphWorkDecl` 里刻意**没有** reads/writes 字段——那份名单必须
逐条照抄各模块真实发的 barrier，现在填一份"看起来对"的会被读成已接线。

**判据实测**：

| 判据 | 读数 |
|---|---|
| 1 门关逐位不变 | `just regress floor --runs 3 --expect 95edc8ccd7a4` → 3/3 同哈希、0 像素差 |
| 2 位置契约能红 | 单元层 9 条变异全红在预期用例（含 `!=`→`>` 弱化、两档并档）；**真程序**三条：删执行器的 `advanceTo` → `'evssm_moments' 位置契约违例——申报相位 12，实际相位 11`；挂点挪进 pass 体内 → `实际相位 13`；删 hook 调用 → `门开着却整帧没录 = 静默旁路` |
| 4 gates + 冒烟 | `just gates` 全绿；出厂 / IZB / EVSSM / FSR / meshlet-cull 五态冒烟 errors=0 |

> 🔴 **教训：变异必须在门开着的配置上跑，门关着测判据 = 假验收。**
> 第一版接线变异拿 `meshlet_cull` 开刀，三条全绿——因为出厂配置 `meshlet.cull = false`，
> 那道门**本来就是关的**（`wireMeshletCull` 直接早退，Renderer 成员恒 nullptr）。
> 每条变异跑之前要先在日志里**证明门是开的**（下表"在场证据"），否则"没红"根本不构成读数。
> 门开配方（`--set` 的字符串值要自己带引号；`just` 会吃掉引号，直连 `python3 tools/smoke.py`）：
> `--set shadow:izb.on=true` / `--set 'shadow:mode="evssm"'`（在场证据 = 日志 `EvssmShadow:`）/
> `--aa fsr` / `--set runtime:meshlet.switch=true --set runtime:meshlet.cull=true`
> （在场证据 = 日志「逐簇视锥剔除生效档 = gpu」）。
> **沉淀地址**：`TEST: tests/test_offgraph_contract.cppm`（契约逻辑本身与门无关，恒可测）+
> `DOC: 本节配方`（"门开着才测得到"这件事做不成断言，但它已经是**命令**而不是感想）。

### 7.2 B1b 形态定案（2026-10-14，用户拍板 A 档 + 二合一约束）

**问题**：A 档（逐条 barrier 申报）的"申报"若是**另写一份**，就得到第二份命令流——两份事实
必然漂，而漂了没人查的护栏比没有更坏。用户给的约束是「申报边/计算节点这个过程，和定义计算
节点/边二合一，**绝对不能手抄**」。

**定案 = 申报即发射（declare-by-emit）+ 边表独立于发射调用做双向对账**。本仓已有两个半成品，
B1b 是把它们合起来推广到 buffer/global 侧：

| 先例 | 它已经证明的事 |
|---|---|
| `Renderer::transitionAndTrack` | 「**为什么必须是一个函数**而不是调用方自己发 barrier 再自己写表：两个独立字面量一旦分家，表就可能与真实布局脱钩，而**读表的护栏随之退化成同义反复**」 |
| `usageInfo`（`render_graph_impl.cppm:49`） | usage → `{layout, stage, access}` **查表生成命令参数**，所以图内根本不存在"声明与命令流分家" |

**形态**：

1. **usage 词表**（rlgfx 新模块 `offgraph_sync`）：原子 usage → `{stage, access}`，
   边 = `(生产者 usage, 消费者 usage, 资源引用)`。barrier 的 masks 由边**物化**，
   模块侧一行都不写 mask。
2. **发射即申报**：模块调 `emitSync(cmd, edgeId)`，同一次调用 ① 发命令 ② 记"本帧这条边被覆盖了"。
   于是「申报了却没发」与「发了但挡错对象」**在写法上不可能**——这正是 `transitionAndTrack`
   那条红线的 buffer 侧版本。
3. 🔴 **但二合一本身不抓"忘发 barrier"**——那是债-75 真正的痛处（"缺 barrier VVL 抓不到"；
   B1b 三刀之后**这一半拆成债-117 单立**），
   而 declare-by-emit 只保证已发的一致，**没发的它看不见**。所以边表必须**独立于发射调用**写
   （谁产谁消，B2 的依赖图），帧末做**双向**对账：门都开着 ⟹ 这条边必须被覆盖；门关 ⟹ 不许出现。
   **形状与 B1a 的 `checkCoverage` 完全同构**（同一个"门开⟺发生过"的两半边），不是新发明。
   少了第 3 条，二合一就是一台把冗余搬进查表的打字机——省了手抄，没省漂移。

**发射权仍在模块**（plan-only 红线）：让执行器在节点边界自动插 barrier = 发射权交给图，
正是 §5.2 仍有效的 14.4b B 档否决。所以模块侧**保留一行** `emitSync(edgeId)`（一个名字，
不是六个 mask 字面量），这是红线的价格，不是没做到二合一。

**不用通用图容器（BGL 之类）的三条理由**：① 这里不需要任何图算法——节点 = B1a 的 4 个工作项、
边十几条、拓扑由编译图 + 帧内顺序**给定**，判据只有"集合覆盖"；② 通用容器会把判据形状摊平，
本仓的护栏靠的正是形状写死（`checkCoverage` 两个半边各一条）；③ 引擎侧新依赖要走 vcpkg。

**现场勘察（2026-10-14 实测，逐条到行号，主 agent 抽查复核）**：逐帧 buffer/global barrier
共 **33 条目 / 31 条命令**，去重后 **19 种形态**，其中 **8 种覆盖 22 条目（2/3）**；
全部形态都是「某类写 → 某类读」或其写前依赖镜像，**没有需要 attachment 布局语义的异形**
⟹ 二维查表（生产者 usage × 消费者 usage）可表达全部 19 种。分布：

| 模块 | 条目 | 门 |
|---|---|---|
| `Renderer::recordFixedDataPath` | 3（1G+2B） | 无条件逐帧 |
| `GpuCull::recordCullPass`/`recordFanoutDispatch` | 8（2G+6B） | GPU cull 档（**独立 cull CB**） |
| `MeshletCullService::recordDispatch` | 4（1G+3B，**合一成 2 条命令**） | 指针非空 |
| `IzbShadow` | 9（9B）+ 4 条诊断门控 | 指针非空 |
| `EvssmShadow::recordFrame` | 9（9G，**全部无句柄纯执行序**） | 指针非空 |
| `fsr_upscaler` | **0**（逐帧侧零 barrier；`toGeneral` 是建期一次 image） | — |

三处**必须承认的异形**（词表要留兜底档，不许为此放弃二合一）：`vkCmdFillBuffer` 没有专属
stage/access，只能拿 `AllCommands/MemoryWrite` 兜底（IZB 4 条）；EVSSM 的 depth-write → compute
是无句柄全局序（access 用 `eMemoryWrite` 通配）；meshlet 把 heap 与 scene data 两条语义**并进
一条 global**（词表要允许"合并档"或 entries 分层）。另有一处**条件发射**（meshlet 的 scene data
条目 `if (buffer != VK_NULL_HANDLE)`）——declare-by-emit 天然处理它（没发就不记账），
但也因此**只有第 3 条的独立边表能发现"该发而没发"**，再次说明三者缺一不可。

**顺带纠正本篇一处旧措辞**：§7 B1b 行原写"FSR 那条 VVL 抓不到的 UB 进对账"。实测 FSR 逐帧侧
**零 buffer/global barrier**，它的两条是 **image 但 `oldLayout == newLayout == eGeneral`**
（`vkrenderer_offgraph.cppm:381-393`）——即"布局不变、纯执行依赖"，`transitionAndTrack` 的
同布局早退会**跳过**它。所以它仍然属于"VVL 抓不到"那一类，但要收的是**执行依赖**而不是布局，
归 B1b 词表的"同布局执行档"，不是 image 布局档。


### 7.3 检查机制定案（2026-10-14）：装什么判据，以及"可选"选的是什么

二合一抓不到"忘发 barrier"（§7.2 第 3 条）。用户提：**在二合一自己造一个可选开关的拦截机制**。
裁决 = 做，但**开关不产生判据**，它只是"什么时候付钱 / 要不要阻断"的旋钮 ⟹ 真问题是往里装什么。

**三档判据**：

| 档 | 判据来源 | 抓得到 | 抓不到 | 归期 |
|---|---|---|---|---|
| **L1 应发次数账** | 边表每条带 `expectedPerFrame`（0/1/N） | 忘发、少发、多发、门关却发 | **表里根本没这条边** | **B1b 本刀** |
| **L2 访问对推导** | 着色器**实际读写方向** + 命令序：writer 与 reader 之间无覆盖 barrier ⟹ 报 | 根本没写这条边 | 跨帧/跨 CB 的序（要另建账） | **B2 第一件事** |
| L3 外部工具拦截 | — | — | — | **不做** |

**L2 的数据源现在不存在**（三处实测，别再找第二遍）：`rgl/generated/*_types_generated.cppm`
只有**类型布局**；`PipelineBindings`（`rgl/shader.cppm`）的 binding→槽是 C++ **手拼**、无方向字段；
`spvDeclaredBindings`（同文件）只采"带 Binding 装饰的 OpVariable 的号"。
可行路径 = 扩既有 SPV 骨架（`spvSplit` + decoration 采集）去采 storage class 与
`NonWritable`/`Coherent`，或扫该变量上的 `OpStore`/`OpAtomicStore`。**读写方向从码流里读出来
而不是人抄** ⟹ 它既是 L2 的判据源，也顺带是 B2 依赖图的第一块数据源（同一份事实，别做两遍）。

**L3 已实测无效，别重新发明**：syncval 对 shader 侧 buffer 访问 0 条（删 IZB 那条 barrier 的变异
开与不开都 0）；GPU-AV 故意越界读 `sceneData.Load(4000000u)`（buffer 仅 789760B）也 0 条，
而同一次运行截图 1376KB→1360KB 证明那次读真执行了。

**🔴 落地形态 = 检查常开 + 一律 error，没有开关**（这一条**推翻**了我自己先写的"warn / 闸门档
error 分档"方案，也偏离用户"可开可关"的字面要求；推翻理由必须留在这里）：

原方案是"检查常开、开关只切严重度（日常 warn、`just smoke --check-sync` 档 error）"。实现时
发现那个开关**买不到任何东西**：① 违规只出声不中断（`reportOffGraphViolation` 只 `log->error`），
所以"阻断"从来不是真阻断，代价上限 = 冒烟 errors 计数红一次；② 而 L1 的判据是**确定性**的
（实发次数 vs 边表应发次数），不存在"这个配置下少发 3 条是合法的"那种现场——**能误报的只有
边表本身写错**，而边表写错会在迁移当口被自己的冒烟抓到（errors>0 就是本次验收的判据本身）；
③ 加一档严重度 = 多一个配置键 + 一条 gate_lint 规则行 + 一个 smoke 旗标，**为还没发生的误报
预留逃生舱**，正是本仓反复删的那类东西。于是"可选"整个概念在这里退化掉了：
**判据常开、出声常开、只有边表是人写的**。

> 真遇到"条件边"（IZB 的 dump 门控那 4 条：dump 开 = 发 4 次、关 = 0 次）怎么办？
> **在边表里写成两条边、各挂各的门**（dump 门与 IZB 门是两个门），而不是回来加严重度档。
> 这条是 B1b 第二刀铺 IZB 时的硬约束——**已按此落地**（`izb` / `izbDump` / `izbMaskSegment` 三道门
> 各挂各的边，§7.7）。写不出第二条边就说明门位口径没定对，那是要修的事。

**两条红线**（仍然有效，且不依赖有没有开关）：① **判据不许换路径**——对账走的就是交付路径上
那一次 `emit` 记的账，没有"检查模式专用"的第二条录制路径（`--sync` 档同型要求）；
② L1 的 `expectedPerFrame` 是人写的整数，**残留一份手抄**（抄次数而不是抄 6 个 mask）——
它必须被变异直接覆盖（EVSSM 的 7 改 6 必红），不许被读成零成本。

**沉淀地址**：`TEST: tests/test_offgraph_sync.cppm`（词表 masks 逐位 + 边表逐条 + 次数账双向 +
10 条变异）；`DOC: 本节`（L2 数据源三处实测 = 负知识，做不成断言）；`GUARD:` **无独立入口**——
判据就在交付路径上，`just smoke` 的 errors 计数就是它的闸门（这正是"不加开关"的推论：
加了开关就得再养一个"有人自动跑它"的入口，那是给护栏再套一层"没人按就失效"）。

### 7.4 B1b 第一刀已落地（2026-10-14）：EVSSM 的 9 条 barrier 换成 3 个边名

**做了什么**（新模块 `rgl/offgraph_sync.cppm` + `_impl.cppm`，住 **rlgfx**——判据核零设备，
但 `emit` 要 `CommandBuffer`，按"需不需要 VkDevice"的分流它属设备层；接线在 `EvssmShadow`
与 `vkrenderer_*`）：

- **usage 词表**（`RgSyncUsage` → `{stage, access}` 唯一来源）。🔴 **原子 = 语义角色，不是
  "观测到的掩码集合"**——后者会让"词表覆盖全部 19 形态"变成恒真的废话（18 个匿名掩码换个地方
  存而已）。宽档 `anyWriteGlobal`（AG / MemoryWrite）是 EVSSM 阴影图**本模块无句柄**逼出来的
  兜底，单独命名 + 注释写明"宽出来的部分是本模块白付的同步成本"。
- **边表**（`RgSyncEdgeDesc`：scope / src / dst / 归属门 / 应发次数）。EVSSM 三条边
  `evssm_depth_to_compute` / `evssm_moments_chain` / `evssm_moments_to_light`，
  模块侧从**六个 mask 字面量**变成**一个边名**；`stageBarrier` helper 随之删除。
- **`emit` = 物化 + 发命令 + 记账同一次调用**（§7.2 的"二合一"）。于是「申报了却没发」与
  「发了但挡错 mask」在写法上不可能。
- **L1 次数账**（`checkEmission`，两半边同 B1a 的 `checkCoverage` 同构，只是比**次数**不是比
  布尔）。应发次数由 `EvssmShadow` **构造期** `expectCount(chain, mipLevels − 1)` 供——
  录制期申报等于自己出题自己答，那条判据就自证了（这是本刀唯一不显然的时序设计）。
- **发射权仍在模块**：plan-only 红线守住了，`emit` 由 `recordFrame` 发起，本层零决策。

**没做什么**（本刀当时的边界，括号里是后续状态）：IZB 与 GpuCull 未迁（→ §7.7 / §7.8
**已迁完，债-75 结案**）；`buffer` 档的边一条没有——EVSSM 三条全是无句柄执行序，
`emit(cmd, edge, buffer)` 这个重载当时**只有单测与编译覆盖**（首次真跑 = §7.7）。
**仍然没做**的是 L2（读写方向从着色器码流读）——「该同步却压根没写一条边」至今没人抓，
已拆成 **债-117** 单立（判据的独立来源只能来自码流，抓手待 spike 验证 → 债-117 载体列）。

> 🔴 **教训（本刀实测踩到，代价 = 九个文件的编辑重做）：变异脚本在跑的时候，不要编辑它盯的
> 文件。** 脚本的还原是 `copy(备份 → 原路径)`，备份取的是**脚本启动那一刻**的内容 ⟹ 期间手改
> 的东西会被**静默覆盖**（不报错、不冲突，就像从没写过）。本刀两处注释改在 `offgraph_sync*.cppm`
> 上，脚本跑完就没了，靠重读文件才发现。两条护栏：① **一个能编译的完整改动做完立刻提交**，
> 别攒着（本刀攒了 9 个文件）；② 变异脚本开头写死它盯哪些文件，跑之前先看一眼。
> 顺带一条同族的坑：`pgrep -f <脚本名>` 会匹配到**发起检查的那条命令自身** ⟹ 恒假阳性
> （本仓 regress 那条纪律的同型问题，实测又踩一次），要看得看进程表里有没有真的 `python3 …`。

### 7.5 B1b 第二刀（IZB）的两条硬约束（2026-10-14 定，已按此落地 → §7.7）

1. **条件边写成两条边、各挂各的门**。IZB 的 dump 门控那 4 条：dump 开 = 发 4 次、关 = 0 次。
   不要回来给 L1 加"严重度档 / 豁免"（§7.3 已裁决不加），正确形态 = dump 门与 IZB 门是两个门，
   两条边各带自己的 `expectedPerFrame`。**写不出第二条边就说明门位口径没定对，那是要修的事。**
2. `buffer` 档首次实测就在这一刀：`izb_shadow_impl.cppm` 的 `bufferBarrier` 写死 `VK_WHOLE_SIZE`
   （接口为此不留 `size` 参数），迁移时顺手确认"整块"这个口径真的没窄化过任何一条。

### 7.6 判据实测（B1b 第一刀，2026-10-14，本机 AMD Radeon 8060S / RADV STRIX_HALO）

| 判据 | 读数 |
|---|---|
| 单元层 | `just test` **450/450**（本刀新增 6 用例）；`just gates` 全绿（build + test + `checks --strict`） |
| **变异 · 单元层 10 条** | 全红且**归因到期望用例**：词表改位 ×2（`computeReadWrite` 少一位 / `anyWriteGlobal` 被收窄）→ 词表 + 边表两条同时红；边 src/dst 对调；对账**两半边各删一次**；判据从"次数"弱化成"有没有"（只有「门开多发」那条能抓）；`beginFrame` 不清账；`expectCount` 不生效；门位表长度校验删掉；词表养一个闲原子 |
| **变异 · 真程序 3 条**（EVSSM 门开） | 全红且**点名边**：① 忘发（循环里删一条）→ `边 'evssm_moments_chain' 门开着却发了 0 次，应发 7 次`；② 应发次数抄错（`mipLevels−1` → `mipLevels`）→ `发了 7 次，应发 8 次`；③ 多发（depth→compute 发两遍）→ `'evssm_depth_to_compute' … 2 次，应发 1 次`。每条跑前先证明门开（在场证据 = 日志 `EvssmShadow: 2048x2048 RGBA16F x8 mip`），基线 errors=0 |
| 门开冒烟 | `--set 'shadow:mode="evssm"' --frames 12` → **errors=0 warnings=0**；`--sync`（VVL syncval）→ **hazard 0 条** |
| 门关逐位不变 | `regress floor --runs 3 --expect 95edc8ccd7a4` → 3/3 同哈希、0 像素差（出厂锚未漂） |
| **门开逐位不变 = 迁移没改语义** | `regress ab --set 'shadow:mode="evssm"' --runs 3`（工作树 vs `1207770`）→ **两侧同哈希 `865fd1aa5a82`、A×B 跨构建 0 像素差** ⟹ 判定逐位相同 ✅ |

> **`865fd1aa5a82…` = EVSSM 开档（出厂机位、`aa` 随配置、frames 8）的文件哈希锚**。
> 之前本仓只有出厂档 `95edc8ccd7a4` 一个锚，门控特性一直没有——因为 `regress ab` 过去**没有
> `--set`**，门开的配置根本进不了 A/B。本刀顺手补了 `--set` 透传（`tools/regress.py`，与
> `just smoke --set` 同一个 `patch_lua` 护栏）。⚠ 走 `just regress` 时引号会被 just 吃掉，
> 字符串值要直连 `python3 tools/regress.py`。
> 这条 A/B 还顺带自证了 `--set` 真生效：EVSSM 档哈希 `865fd1aa` ≠ 出厂档 `95edc8cc`。

**已知软处（写清楚，别读成"全都被机器守着"）**：`expectCount` 是 public，理论上模块可以在
录制期反复调它把判据喂成自证——**运行时守不住**（EVSSM 首帧懒构造，本来就发生在第一帧
`beginFrame` 之后，任何"只许启动期调"的硬闸门都会误伤合法用法）。现在的约束是接口注释里的
那条理由 + 单测「构造期申报」用例。**沉淀地址**：`TEST: tests/test_offgraph_sync.cppm`
（十条变异的出处）+ `DOC: 本节`（门开配方与两个哈希锚）。



### 7.7 B1b 第二刀已落地（2026-10-14）：IZB 的 13 条 buffer barrier 换成 13 个边名

**做了什么**（`IzbShadow` 迁入 `offgraph_sync`；边表从 3 条长到 16 条、门从 1 道长到 4 道）：

- **`buffer` 档首次真跑**：13 条全是 `BufferMemoryBarrier2`（EVSSM 那三条是无句柄执行序，
  上一刀 `emit(cmd, edge, buffer)` 这个重载只有编译与单测覆盖）。句柄由发射点给，词表不持资源。
  ⚠ 这里说的是**同步边的 scope** `RgSyncScope::buffer`，**不是** §6 D2 那条"meshlet_cull = buffer 档
  首发"——那个指**图资源词表**要扩出的 buffer 档，属 B2，至今没做。
- **`bufferBarrier` helper 删除**（13 个调用点各写六个 mask 字面量 → 13 个边名）。原位置留了
  一句"别再在这里加回本地 helper"——加回去等于把"谁都能写 mask 字面量"重新发明，而对账会
  立刻看不见它（它不在边表里）。
- **条件边按 §7.5 第 1 条的裁决落地**：`izb`（对象在场）/ `izbDump`（本帧是 dump 帧）/
  `izbMaskSegment`（shadow0 段非空）三道门，13 条边各挂各的，`expectedPerFrame` 全是 1。
  于是"段空的那一帧发了一条 mask 边"这种错 = **门关却发** = 当场红，而不是靠人记得。
- **两处门位口径**（这是本刀唯一有设计含量的地方，其余是机械迁移）：
  ① `izbDumpDue()` 抽成一处，`izbLightPassPre`（决定录不录）与帧末门位表**共用**——两处各写
  一遍表达式 = 门位算错时护栏假阳性，而假阳性的护栏三个月后只会被关掉。
  ② mask 段的门 = 帧末**重算** `segmentSpan(shadowDrawSource(0), list)` 是否为空，
  不是记"刚才调过 `beginMaskPass` 没有"——后者让断言自证（段真被跳过时"没调过"与"没发"一起
  消失，什么都测不到）。为此 `offGraphEndFrame` 多收一个 `const FrameDrawList&`。
  ③ 门之间不独立（dump ⊂ izb、mask 段 ⊂ izb），帧末必须带上 `izbOn &&`，否则运行中途关掉
  IZB 会让子门按配置算成开 ⟹ 假阳性。
- **顺带修掉一个静默改坏配置的工具 bug**（`tools/smoke.py::patch_lua`）：单行子表
  （`izb = { on = false, grid = 512, dump_frame = 0 }`）里 `re.M` 的 `^` 会匹配到**块作用域
  的 0 号位**（它在行中间）⟹ 改首键时 `.*$` 一路吃到行尾，**把后面的兄弟键连同 `}` 一起吃掉**
  （实测 `--set shadow:izb.on=true` 把 `grid`/`dump_frame` 静默删成默认值，护栏全绿、
  smoke 报 PASS，量的是默认档）；而改非首键则反过来"命中 0 次"直接红。修法 = 单行块走另一条
  式子（只替换值本身）。**沉淀地址**：`GUARD: python3 tools/regress.py selftest`（新增
  `smoke.selftest()`，7 项，断言打在**真配置副本**上 ⟹ 配置形态变了也在这里红而不是在某次
  判据里静默失效；变异 = 把 `inline` 判据写死成 `False` 退回旧行为 → 自检红、退出码 1）。

**没做什么**：GpuCull 那 8 条（跨独立 cull CB，需要单独设计提交边界的表达）= 第三刀
（→ §7.8 已落地，实际要设计的不是机制而是**注入形态**与**跨 CB 依赖不建模的定案**）；
L2 本刀没做（现状：数据源已由 B2-1 的 access 反射承载，「忘写一条边」的 compute↔compute 部分 B2-2 起构造期对账，剩余面积 → **债-119**）。

> **一条不属于本刀的观察**（A/B 两侧同现，不是本刀引入）：`shadow:izb.dump_frame=1` 档下
> `drainReadback` 会打 `izb: cell N 链长触顶（坏链/环！）`，**HEAD 上同样有**，且条数在同构建
> 连跑之间会漂（实测 4 ↔ 5），而**截图逐位相同**。三条读数放一起 = 交付画面不受影响，
> 触顶的是 CPU 侧沿 `nodes` 链表走的那段诊断，链表**节点分配顺序本身是原子的、跨运行不确定**。
> 要不要处理属 IZB 自己的账（它默认关；本段就是这条观察的唯一承载，未单独挂号），不记在债-75。

**判据实测**（本机 AMD Radeon 8060S / RADV STRIX_HALO）：

| 判据 | 读数 |
|---|---|
| 单元层 | `just test` **452/452**（`test_offgraph_sync` 从 6 用例扩到 8：词表逐位钉 14 个原子、边表逐条钉 16 条边的四条掩码、IZB 十三条**逐条**漏发各验一遍、子门关着却发）；`just gates` 全绿 |
| 变异 · 单元层 4 条（本刀新增面） | 全红且**归因到期望用例**：① 两条跨帧宽档并成一条（`priorAccessWideGfx` 补上 compute）→ 词表 + 边表**两条同时**红；② `izb_dump_head` 的 gate 从 `izbDump` 改成 `izb` → 边表 + 「子门关着却发」两条红（后者就是 §7.5 第 1 条的机器判据）；③ mask 清零边 dst 少一位（`graphicsReadWrite`→`graphicsRead`）→ 边表红；④ 词表养一个闲原子 → 「每个原子都被某条边用到」红 |
| 变异 · 真程序 4 条 | 全红且**点名边**：R1 忘发（`beginMaskPass` 里 nodes 那条删掉）→ `边 'izb_nodes_to_mask_pass' 门开着却发了 0 次，应发 1 次`；R2 忘发（`endMaskPass` 那条）→ 点名 `izb_mask_to_light`；R3 **门关却发**（dump 拷贝改成无视 `dumpHead` 无条件录）→ `边 'izb_dump_head' 门关着却发了 1 次 = 破逐位不变红线（门关必须零命令）`；R4 **EVSSM 回归**（门位表从 1 扩到 4 之后旧客户仍要能红）→ 点名 `evssm_moments_chain`。每条跑前先在日志里证明门开（IZB = `izb: 开门 grid=512`；EVSSM = `EvssmShadow:`），基线 errors=0 |
| 门开冒烟 | `--set shadow:izb.on=true --frames 12` → **errors=0** warnings=1（那条 warn = gate_lint 提示 IZB 默认关，出厂就有）；`--sync`（VVL syncval）→ **hazard 0 条**；dump 档（再 `--set shadow:izb.dump_frame=1`）同样 **hazard 0 条** |
| 门关逐位不变 | `regress floor --runs 3 --expect 95edc8ccd7a4` → 3/3 命中出厂锚、0 像素差 |
| **门开逐位不变 = 迁移没改语义** | `regress ab --set shadow:izb.on=true --runs 3` → A×B 跨构建 0 对不同哈希、**逐位相同**；IZB 开档新锚 `2316b566b113…`（同构建 2/2 稳定）；dump 档另跑 `ab --runs 2` 亦逐位相同（同一哈希 = dump 回读不动交付画面） |
| 工具自检 | `regress selftest` 新增 `smoke.selftest()`（7 项）；变异 = 把 `inline` 判据写死成 `False` 退回旧行为 → 自检红、退出码 1 |

> 🔴 **一条变异方法学**（本刀实踩）：**变异体自己必须过 `-Werror`**。R3 第一版把 `if (dumpHead)`
> 直接删掉 ⟹ `unused parameter` 编译失败，脚本记 BUILD-FAIL 而不是读数——"编译不过"既不是
> 判据绿也不是判据红，只是没测。改成 `recordDumpCopy(cmd); (void)dumpHead;` 才拿到真读数。
> 与 §7.6 那条「门开着才测」是同一类坑的两侧：**变异要么真的没执行、要么真的没编译**，两种
> "全绿"都不构成验收。

### 7.8 B1b 第三刀已落地（2026-10-14）：GpuCull 的 8 处 barrier 换成 8 个边名

**做了什么**（`GpuCull` 迁入 `offgraph_sync`；边表 16 → 24 条、门 4 → 6 道、词表 14 → 17 个原子）：

- 8 处 = **6 条 buffer 档** + **2 条无句柄执行序**（heap 那条与 host 读回那条原本是
  `vk::MemoryBarrier2`，本来就没有句柄可交）。
- **新增 3 个原子，其中两个是新宽档、成因与已有四个都不同**：`heapReadWide`（消费者是
  `VK_EXT_descriptor_heap` 的**扩展对象**，不是本层能命名句柄的 buffer ⟹ 表达不成 buffer 档）、
  `priorAccessSceneData`（跨帧**且跨命令缓冲**：scene data 单份共享 + 每帧两个写者 = 主 CB 与
  cull CB；与 `priorAccessWideCs` 差在 access 的**方向**——scene data 不被 shader 写、也不被
  拷出读，但会被另一条 CB 拷写）。宽档表因此 4 → 6 个。
- 门 = `cull`（`CullDriver::gpuPassRuns()`，判据是**档**而不是"刚才录过没有"）+ 子门
  `cullFanout`（静态 DGC 流建了没，⊂ `cull`）。

**定案：跨命令缓冲的依赖 = 同队列提交顺序，边表不建模（显式非目标）**

cull CB 与主 CB 都提交到 `getGraphicsQueue()`，且 cull CB 先提交 ⟹ 两者之间**没有 barrier**，
靠同队列 batch order 排序。这件事**不进边表**，三条理由：

1. 它不是一条 barrier——`emit` 发不出"提交顺序"这种东西，硬加 `submission` scope 会让
   「一条边 = 一条 `pipelineBarrier2`」这条不变式破掉，而那条不变式正是对账能数次数的前提。
2. L1 是**次数**账，它对"提交顺序写错了"本来就无能为力——把它记成"已覆盖"是假覆盖。
3. **触发条件明确**：一旦 cull CB 换到真正独立的队列族（§3.1 删掉的那条假 `computeQueue`
   被重新拿起来），提交顺序不再蕴含排序，本条**立刻失效**，届时必须回来设计提交依赖的表达。

**构造顺序决定注入形态**（本刀唯一不显然的一步，也是"第三刀要单独设计"的实际内容）：

`GpuCull` 在 `main_scene.cppm` 构造，**早于 Renderer**（Renderer 只持其裸指针），而
`OffGraphSync` 是 Renderer 的成员 ⟹ 构造期那个引用还不存在，**不能**照 IZB 那样塞 ctor。
最终形态 = `CullDriver` 持 `OffGraphSync&` 成员（它是 Renderer 成员、声明在 `offGraphSync`
**之后** ⟹ 传入的是已构造成员），`recordCullPass` / `recordFanoutDispatch` 各收一个
`OffGraphSync&` **形参**，`GpuCull` 类本身不持。

> 三家客户三种注入形态不是风格不一致，是**生命周期决定的**：EVSSM / IZB 由 Renderer 首帧懒
> 构造 ⟹ 持引用成员；GpuCull 早于 Renderer 构造 ⟹ 只能逐次传参。**判据相同**（发射点只写
> 边名、帧末按边对账），形态允许不同。写死成一种形态的话，要么给 GpuCull 加两阶段 setter
> （= 允许"没接线"这个静默状态存在），要么把 `OffGraphSync` 提前到某个更早的对象里
> （= 为了机制的形状去改构造骨架）。

**`size` 等价性**（接口决策的现场核对）：scene data 那两条原本写 `size = kSceneDataBytes`
而非 `VK_WHOLE_SIZE`，而 `emit` 的 buffer 重载刻意不留 size 参数。等价性出处 =
`rgl/scene_data.cppm` 里 buffer 创建处 `info.size = kSceneDataBytes`（buffer 恰好等于该长度
⟹ 两者逐字节等价）。**若哪天 buffer 变大，`VK_WHOLE_SIZE` 只会更宽 = 过度同步，不是漏同步**
——错的方向是安全的，这一点写进了发射点注释。

**顺手把一条静默分支改成出声**：`recordCullPass` 里 `if (sceneBuffer.buffer != VK_NULL_HANDLE)`
原本在 else 侧**什么都不做**——而走到那里已经是 bug（cull dispatch 要从 scene data 读对象矩阵
表，buffer 不存在 ⟹ 那一读本来就在读垃圾）。现在 else 侧 `log->error` 点名根因。
🔴 **不要**为了消掉随之而来的帧末读数而给那两条边另挂一道门：那会把 bug 重新藏进护栏里
（条件边的正确形态见 §7.5 第 1 条——门位口径会变成"scene buffer 就绪没"，等于承认这个状态合法）。

**边表换了个住处**：24 行 × 6 字段塞在访问函数里会因**数据**撞上函数长度红线。移到文件作用域
`constexpr std::array<RgSyncEdgeDesc, static_cast<size_t>(RgSyncEdgeId::count)>`——表长与枚举
`count` 编译期绑定（多写行编译失败），**少写行不编译失败**（剩余元素值初始化成空 name）⟹
真护栏仍是单测「边表逐条钉死」按每个 id 断言 name 与四条掩码，漏行红在"name 是空的"上。

**判据实测**（本机 AMD Radeon 8060S / RADV STRIX_HALO）：

| 判据 | 读数 |
|---|---|
| 单元层 | `just test` **454/454**（`test_offgraph_sync` 8 → 10 用例：cull 八条**逐条**漏发各点名一条 + 子门 `cullFanout` 关着却发 / 开着却没发）；`just gates` 全绿 |
| 变异 · 单元层 4 条 | 全红且归因到期望用例：两条跨帧宽档并成一条（`priorAccessSceneData` 的 access 抄成 `priorAccessWideCs` 那对）→ 词表 + 边表同时红；扇出边挂到父门上 → 边表 + 子门用例同时红；heap 宽档少一位（去掉 `SamplerHeapReadEXT`）→ 词表 + 边表同时红；**边表漏一行**（发射点还在、表里删掉）→ 7 条红（漏的行 name 值初始化成空 ⟹ 逐条断言全炸，正是"加边不写用例"的形态） |
| 变异 · 真程序 4 条 | 全红且**点名边**（基线 CULL / EVSSM 两档各 errors=0 门开）：R1 忘发 → `边 'cull_seq_upload_to_create' 门开着却发了 0 次`；R2 **多发**（heap 那条发两遍）→ 点名 `cull_heap_host_to_reads`（这条证明 L1 真的是**次数**账）；R3 **子门门位算错**（帧末把 `cullFanout` 写成父门）→ 点名 `cull_seq_fanout_chain`（本机默认档没建流 ⟹ 门算成开而边一条没发）；R4 **跨客户回归**（门位表 4→6 之后 EVSSM 旧客户仍要能红）→ 点名 `evssm_moments_chain` |
| 门开冒烟 | `--set diagnostics:cull_replay="gpu_sync" --frames 12` → **errors=0 warnings=0**；`--sync`（VVL syncval）→ **hazard 0 条**（cull CB 上的 buffer barrier 第一次真过 syncval）。门开证据 = 日志 `gpu cull: 1591 candidates` + `cull_replay=gpu_sync`（`gpuPassRuns() = active() && 档≠cpu`，两条各有一行对应） |
| 门关逐位不变 | `regress floor --runs 3 --expect 95edc8ccd7a4` → 3/3 命中出厂锚、0 像素差 |
| **门开逐位不变 = 迁移没改语义** | `regress ab --set diagnostics:cull_replay="gpu_sync" --runs 3` → A×B 跨构建 0 对不同哈希、**逐位相同**。哈希 = `95edc8cc` **与出厂锚同一个**，与 `configs/diagnostics.lua` 那条"三档截图逐位不变"的既有断言自洽（换的是**谁驱动 draw**，不是像素） |

> ⚠ **门开配方的一条存量限制**：开 `cullFanout` 子门要 `dgc_static`，而它**不在
> `configs/diagnostics.lua` 里**、只有代码默认值 `false` ⟹ `--set diagnostics:dgc_static=true`
> 会被 `patch_lua` 的"键命中 0 次"护栏红掉。于是本机默认档下子门**只能真跑"关"这一侧**，
> "开着发够"那一侧靠单测 + 门位变异（R3 的反向）覆盖。要真跑得先把该键补进配置——**没顺手补**
> （补一个配置键 = 动出厂配置面，不属本刀）。




### 7.9 L2 可行性 spike（2026-10-14 实测）：**不用碰 SPV 码流**，反射直接给方向

**问题**（= 债-117）：L1 只数「已登记的边」。要抓「该同步却压根没写一条边」，需要一个**独立于
边表**的来源回答「这个 dispatch 会写哪些 buffer」。原设想 = 从 SPV 码流读 `NonWritable`
decoration（§7.3 写下时按"只能这样"处理），且当时 grep Slang C++ 反射头找不到 `isReadOnly`
类 API ⟹ 当时把 L2 标成**高不确定**。

**实测结论：读写方向拿得到，两条路都通。** `slangc -reflection-json` 里
`parameters[].type.access` 取值 `readWrite` = 声明可写、**字段整个缺失 = 只读**；
🔴 **C++ 反射侧同样有**（先前本节写过「C++ 那条路本来就走不通」= **错的**，当时只 grep 了
`isReadOnly` 这个名字）：`slang::ParameterReflection::getResourceAccess()`（`slang.h:2957`，
转发 `getType()->getResourceAccess()`）返回 `SlangResourceAccess`，取值比 JSON **更细**：
`NONE / READ / READ_WRITE / RASTER_ORDERED / APPEND / CONSUME / WRITE / FEEDBACK`
（`slang.h:2214-2224`）⟹ 落地走**已有 C++ 反射工具**那条路，不必为它单开一条 slangc 调用点。
JSON 侧八种形态一次跑齐（本机 slangc 2026.16）：

| binding | 声明 | `baseShape` | `type.access` |
|---|---|---|---|
| 0 | `readonly ByteAddressBuffer` | byteAddressBuffer | （无字段） |
| 1 | `ByteAddressBuffer` | byteAddressBuffer | （无字段） |
| 2 | `RWByteAddressBuffer` | byteAddressBuffer | **`readWrite`** |
| 3 | `StructuredBuffer<T>` | structuredBuffer | （无字段） |
| 4 | `RWStructuredBuffer<T>` | structuredBuffer | **`readWrite`** |
| 5 | `ConstantBuffer<T>` | — | （无字段） |
| 6 | `Texture2D<float4>` | texture2D | （无字段） |
| 7 | `RWTexture2D<float4>` | texture2D | **`readWrite`** |

复算命令（判据本体，别只信这张表）：

```bash
build/_deps/slang-prebuilt-src/bin/slangc <shader>.slang -target spirv \
  -capability spvDescriptorHeapEXT -profile sm_6_6+spirv_1_6 -fvk-use-scalar-layout \
  -entry main -stage compute -reflection-json out.json -o out.spv
# 然后读 out.json 的 parameters[].type.access（本项目 flags 唯一声明点 = cmake/RglSlang.cmake）
```

**三条口径限制**（不写清就会被读宽）：

1. 🔴 **声明级，不是使用级**：`RWStructuredBuffer` 即使本 entry point 只读不写，也照样报
   `readWrite` ⟹ 判据是**保守**的（宁可多要求一条边）。方向是安全的（过度同步 ≠ 漏同步，
   与 §7.8 那条 `size` 等价性同一条理）；代价是"消掉误报"要回去把 shader 改成 `readonly`
   ——**这反而好**：shader 侧的 `readonly` 从注释变成机器可校验的声明。
2. `writable ConstantBuffer` **不是 Slang 语法**（实测 `E20001 unexpected token`）⟹ 「CB 被
   shader 写」这条路在本项目的语言里不存在，判据不必为它设计。
3. 🔴 **主要工作量不在判据，在前置**：这些 compute shader **根本没进反射管线**
   （`rgl/shaders/cull.comp.slang` 头注释自述"首版不接反射管线"）。L2 动手 = 先把 compute
   纳入反射（或单开一条只出 binding 方向的轻量 pass），再谈与边表对账。

**结论**：L2 从「高不确定（可能拿不到信息）」降成「**判据已定、工作量在接反射管线**」。
B2 排期时按后者估。

### 7.10 B2 切刀（2026-10-14 定，用户拍板「先完成 B2」）

**顺序 = L2 数据面 → 依赖图对账 → 词表 + 拒绝面 → 资源迁入。** 与 §7 批次表原读法的差别只有一处，
但是承重的一处：**L2 排第一，不是排最后**。

**为什么 L2 必须第一**（两条，第二条是硬的）：

1. §7.3 的表本来就写「L2 …… **B2 第一件事**」。
2. 🔴 **依赖图没有 L2 就做不出来**。B1a 把 `RgOffGraphWorkDecl` 的读写面**刻意留空**，留空理由写在
   字段注释里：「那份名单必须逐条照抄各模块真实发的 barrier，现在填一份『看起来对』的会被读成
   已接线」——**能替代手抄的只有反射**。所以「申报面升成依赖图」排在 L2 之前 = 逼着手抄一份名单，
   正是 B1a 拒绝的那个形态。

**为什么词表那一刀往后放**：buffer 档 + 按名注册现在做 = **给一个还没有消费者的机制猜形状**
（RT 缓存的 buffer 版要不要 heapSlot、usage 要哪些位、尺寸漂移按字节还是元素——只有首发消费者
meshlet_cull 能定）。D2 ② 说的「buffer 档首发」就是这个意思。

**L2 产物形态（本刀拍板）**：**已提交的生成物**，不是运行期检查。理由：它守的是「shader 加了个
`RW` buffer 而边表没加对应边」，那是**已提交产物之间的静态关系**，不需要设备。代价写清：判据是
**声明级**的（`RW*` 即使本 entry point 只读也算写）⟹ 会有假阳性；方向安全（宁可多要一条边，
同 §7.8 那条 `size` 等价性同理）。

> 🔴 **本段原写「生成物 + 一条 `tools/checks/` 规则」，并拿 `byte_offset_baseline.json` 当先例——
> 两处都错，规则已撤回（判据 → §7.12）**。先例引错了是承重的错：那份基线是**棘轮**（只准降不准升，
> 新增即红），我建的却是**登记表**（补一行带 reason 就绿）——名字都叫"基线"，牙完全不同。
> 生成物一侧照旧落地，见 §7.11 末。

**证伪点（刀 1 的第一步，跑不出来就换路）**：`getResourceAccess()` 能不能区分
`ByteAddressBuffer` 与 `RWByteAddressBuffer`。头文件与 JSON 都指向能，**但没实测**。
拿 `cull.comp.slang` 测：binding 2 / 4 必须报 `READ_WRITE`、binding 0（`readonly`）必须报 `READ`。
跑不出来 → 退回 `slangc -reflection-json` 那条路（**形态换、判据不换**）。
✅ **2026-10-14 已跑，通过**（三条期望全中），读数与两条当场有用的推论 → **§7.11**。

| 刀 | 内容 | 判据 |
|---|---|---|
| **B2-1** | 反射工具读 `getResourceAccess()`，为 9 枚 compute 出「entry point × binding × 方向」的已提交产物。✅ **已落**（`3eae652`）；配套的 `tools/checks/` 基线规则**已撤回**（§7.12） | **证伪点**（上）；阳性锚点 = cull 的 2/4 报写、0 报读；反向 = 把某个 `RW` 改回只读，生成物必须变 |
| **B2-2** | 申报面升成依赖图（资源 → 生产者 → 消费者），与边表对账「有没有未申报的写后读」。🔴 **人工声明与消费它的检查必须同一刀落**，不许先建声明（§7.12） | 判据 3 的加强版：故意让一个 compute 写一个没边的资源，必须点名 |
| **B2-3** | `RgAttachmentKind::buffer` + `RgUsage::storage*` + **按名注册尺寸** + **拒绝面落进 `registerAttachment`** | 每条拒绝分支一条用例；buffer 档出现在 barrier 计划里 = 编译期/断言红（plan-only 的结构性护栏） |
| **B2-4** | 资源迁入：EVSSM 矩图（先扩 RT 缓存多描述符）→ meshlet_cull（buffer 档首发，**由它定 buffer 档的形状**）→ IZB（最后，保 A 档 `layoutDrift`） | 判据 1 / 5 / 6 / 7（口径已改准） |

⚠ **B2-3 有一条顺带要修的静默坑**：`aspectOf(kind)` 对未知 kind **回落 `eColor`**（
`render_graph_impl.cppm:80-84`）——加第五个 kind 不报错、直接被当色附件。buffer 档必须显式给值，
且那条回落要改成编译不过（去掉 `default` 分支让 `-Wswitch` 接管）。

### 7.11 B2-1 证伪点已跑（2026-10-14 实测）：C++ 反射侧口径

**结论：通过，且比 JSON 细。** 走 `SlangContext::compile` 同一条 API 路径
（`createGlobalSession → createSession → createCompileRequest → processCommandLineArguments
→ compile → ShaderReflection::get`，即 `tools/slang_reflect.cppm:75-112` 那套），
读 `getParameterByIndex(i)->getTypeLayout()->getType()->getResourceAccess()`。
八种形态一次跑齐（本机 slangc 2026.16，与 §7.9 的 JSON 表同一支 shader）：

| 声明 | `getResourceAccess()` |
|---|---|
| `readonly ByteAddressBuffer` / `ByteAddressBuffer` / `StructuredBuffer<T>` / `Texture2D<T>` | `READ` |
| `RWByteAddressBuffer` / `RWStructuredBuffer<T>` / `RWTexture2D<T>` | `READ_WRITE` |
| `ConstantBuffer<T>`（以及 push constant） | **`NONE`** |

🔴 **`NONE` 的语义必须写进判据**：它 = 「不是资源访问型参数」（CB / push），**实测从不与可写重合**
⟹ 判据里它要**跳过**，既不算读也不算写。这条不写清就是一个静默洞：把 `NONE` 当成"分类失败的
可写资源"会误报，当成"只读"会在哪天反射分类变化时漏报。

**九枚 compute 的可写 binding 全貌**（同一支 spike 拉出来的，**12 处**；⚠ 本行原写「13 处」是我数错了——同一条枚举自己加起来就是 12，实现时子 agent 用 JSON 侧独立复算顶回来才抓到。**枚举没错、只有那个总数错**）：`cull` 2/4 ·
`env_prefilter` 1 · `evssm_downsample` **0**/1 · `evssm_moments` 1 · `izb_create` 0/1 ·
`izb_prime` 3 · `meshlet_cull` 4 · `mip_gen` 1 · `stream_fanout` 2。两条当场有用的读数：

1. **声明级过近似的自家实证**：`evssm_downsample` 的 binding 0 `mipSrc` 声明成 `RWTexture2D`
   ⟹ 反射报可写，但它体内**只读不写**（四次 `mipSrc[...]` 读、写的是 `mipDst`）。用 RW 是因为
   要从 GENERAL 布局的图里 load。⟹ §7.9 那条"判据是声明级、会有假阳性"的代价**不是假设，
   第一个就撞上了**，基线里要登记它并写明原因（"GENERAL load 路径要求 RW"）。
2. **判据形态被一个真实案例定下来了**：`izb_prime` 写 binding 3 `primed`，而 `primedBuffer`
   全仓**没有任何消费者**（只有建 / 领槽 / 写描述符 / 绑 prime 四处引用），shader 头注释写明
   是「阶段 2」预留输出。它**不需要边**（无读者 ⟹ 无写后读 hazard）。
   ⟹ 判据**必须同时看读侧**：「某 dispatch 写 R **且** 之后有 dispatch 读 R」才要求边；
   只看写侧会把 `primed` 报成假阳性。（本条排查的初始假设"IZB 少一条边"**是错的**，已证否。）

> 复算：spike 是一次性程序（API 路径照 `SlangContext` 抄），**不入库**；B2-1 落地后同一份事实
> 由 `just reflect` 的生成物给出，届时以生成物为准。JSON 侧的复算命令在 §7.9。

**B2-1 落地读数**（2026-10-14，`3eae652`）：产物 = `rgl/generated/compute_access.json`（第 9 条
反射管线，56 个 global 参数全解析、12 条可写、连跑两次字节相同）。**顺手补了反射护栏一个机制性洞**：
`REFLECTION_PAIRS` 只列直接输入、**不含 import 闭包**，而方向信息恰恰从 import 来（`objectMatrixTable`
来自 `import scene_tables`）⟹ `ReflectionManifest.cmake` 加 `glob:` 输入展开，write / check **共用
同一个展开函数**（两处逻辑分叉 = 一致性 bug 温床），glob 命中 0 个文件 = FATAL。实测改
`scene_tables.slang`（被 import、不是任何管线的直接输入）能触发守卫。**现有 6 条管线的同型洞未修**，
已在 `RglReflection.cmake` 挂号——那是另一刀的账。


### 7.12 B2-1 的规则半边已撤回（2026-10-14，用户质疑后复核）

**撤掉的**：checks 侧那条规则 `rules_compute_access` 与它的基线 `compute_write_baseline`（12 条带
reason 的人工条目）+ config 三项 + `风格检查规则.md` §10（三份文件已删，名字留这儿供 git 检索）。
**留下的**：反射产物本身 + 上面那条 import 闭包修复（独立的机制修复，与规则无关）。

**判据——本节唯一要长期留着的东西**：

> 🔴 **双相对账成立的前提 = 两个来源各自携带对方推不出的信息。**

拿本仓真正在跑的三条对账验这条，它们都满足；我建的那条不满足：

| 对账 | 来源 A | 来源 B | **B 里 A 推不出的东西** |
|---|---|---|---|
| 面 6 `descriptorSlots` ↔ shader `[[vk::binding]]` | `*_schema.lua` | 反射 | `heapSlotOffset` / `cppName`（运行期槽 = `reservedSlots() + offset`，是 C++ 侧分配策略，**shader 里根本没有**） |
| R5 裸字节偏移 | 基线计数 | 源码扫描 | 「存量已评审」+ **只准降不准升**的口径本身 |
| ❌ 已撤的那条 | 反射产物 | 人工基线 | **只有 `reason` 那串散文**；三元组 `(shader, binding, param)` 产物里本来就有 |

第三行不是双源，是**单源 + 一份评论文件**。它两条判据里，"基线登记了而产物已不可写"（陈旧基线）
有一点独立信息，"产物可写而基线没登记"——也就是它存在的理由——**完全没有**。

更要紧的是**它的通过条件是"写一句话"**：新增一条 `RW` → 补一行带 reason 的条目 → 绿。我引先例时
把 `byte_offset_baseline.json` 当成同类，**那是错的**：它是**棘轮**（新增即红、只准降），糊不过去；
我建的是**登记表**（补一行就绿）。名字都叫基线，牙完全不同。

**为什么当时会做错**（写下来是为了下次认出同一个形态）：**产物先有了，就顺手给它配一个"对账"**。
顺序反了。正确顺序是先问"要拦哪一类错、那个错需要哪两个独立事实"。B2-1 的产物答"这里会写"，而真正
要拦的错是"写完有人读而中间没边"——它还需要**资源身份**与**读者**两个事实（§7.11 读数 2 已经把
它们点出来了）。只补一个 reason 字段 = 在离目标还差两格的地方立终点线。

**知识不跟着机制一起扔**。那 12 条 reason 是第一次有人逐条写下"每个 compute 写的读侧是谁、靠什么
同步"，正是 B2-2 要消费的那张表。落此处（方向列真相源 = `just reflect` 产物；边名 = `offgraph_sync_impl.cppm` 的 `kEdgeTable`）：

| compute · binding | 实测方向 | 读侧 | 靠什么同步 |
|---|---|---|---|
| `cull` · 2 `sequences` | 写 | fanout dispatch、读回拷贝、DGC execute | `cull_seq_fanout_chain`、`cull_seq_write_to_readback`；DGC 走同 CB 顺序 |
| `cull` · 4 `countBuf` | 写 | 读回拷贝、DGC push 的 `sequenceCountAddress` | `cull_count_write_to_readback`；DGC 走同队列顺序 |
| `env_prefilter` · 1 `dst` | 写 | 运行期 env 采样 | 启动期一次性，图外宿主 barrier |
| `evssm_downsample` · 0 `mipSrc` | **声明可写、体内只读** | —— | 不需要边（§7.11 读数 1） |
| `evssm_downsample` · 1 `mipDst` | 写 | 同链下一级的 `srcTex` | `evssm_moments_chain` |
| `evssm_moments` · 1 `moments` | 写 | EVSM 阴影采样 | `evssm_moments_to_light` |
| `izb_create` · 0 `head` | 写 | `izb_prime`、mask pass | `izb_head_create_to_prime`、`izb_head_to_mask_pass` |
| `izb_create` · 1 `nodes` | 写 | `izb_prime` 走链、mask mesh 读 payload | `izb_nodes_create_to_prime`、`izb_nodes_to_mask_pass` |
| `izb_prime` · 3 `primed` | 写 | **全仓无读者** | 不需要边（§7.11 读数 2） |
| `meshlet_cull` · 4 `clusterVisible` | 写 | mesh shader 按 `cullBase` + 簇内序读 | 同 render pass 内 compute → mesh 顺序 |
| `mip_gen` · 1 `dstTex` | 写 | 同链下一级 `srcTex`、运行期材质采样 | 层级 barrier 由宿主记录（图外） |
| `stream_fanout` · 2 `streams` | 写 | DGC execute | 同队列顺序，**无边** |

⚠ **本表不是护栏，没有机器在读。** 它是人工核对过的现状表，作用是给 B2-2 当人工半边——B2-2 要验的
正是"最后一列能不能从边表 + 相位顺序**推**出来"。推得出 = 那张表可以删；推不出来的那些行 = 真缺口。

**撤回后的诚实账**（让出了哪一格，别糊）：

| 仍会响 | **不再响**（本次让出） |
|---|---|
| 改任何 `.slang`（含被 import 的）没 `just reflect` → 构建期 SHA1 守卫 FATAL 点名文件 | 新增一条 `RW` 而没加同步边 → 只在 `just reflect` 的 diff 里多一行 `readWrite`，**没人看 diff 就过去了** |

让出这一格是**有意的**：一条拦不住这个错的规则，比没有更糟——它会把"已对账"的错觉一并交付。
B2-2 落"资源身份 + 读者"时，**同一刀**把护栏建回来（§7.10 的 B2-2 行已加这条硬约束）。


### 7.13 compute 侧契约形态定案（2026-10-14，用户问「schema 那套哲学套在 compute 上成立吗」）

**问题原样**：schema + shader 反射当初立起来的唯一目的是——① C++ 写错 shader 内变量名要**编译期**
知道；② shader 要有命名规范，不能天天改名导致引擎重编。这套哲学套在 compute 上还成立吗？

**答：成立。但要先分清「要钉的契约」在 compute 上换成了什么，而且 schema 文件本身不是哲学。**

四条实测（本机跑的，不是推断）：

| 实测 | 读数 | 推论 |
|---|---|---|
| `rgl_reflect_slang` 的编译路径能不能吃 `.comp.slang` | 能，**不需要** `-entry`/`-stage`：`[PASS] pushConst 'push' -> struct 'EvssmMomentsPush' matches API (size=12)` | 机器现成 |
| 八枚 `.comp.slang` 塞进**同一次**编译请求 | `conflicting declaration` / `function 'main' already has a body` / `ambiguous reference to 'push'` | 必须每枚一个 `SlangContext`（第 10 条管线就这么写） |
| shader 改名 `mapSize`、schema 仍声明旧名 | `[FAIL] pushConst 'push': schema field 'mapSize' not in API struct` → 拒生成 rc=1 | **目标 ① 达成** |
| shader 里两个 uint **换序** | schema 对账 **PASS**（只比 size + 字段存在性，不比顺序） | 顺序 hazard 不靠"检出"，靠**生成物跟着反射走**消掉；真正消不掉的是**语义**换序，那个任何机制都看不见 |

**哲学里承重的不是 schema，是「单一来源声明 + 另一方消费 + 机器拒生成」。** 图形侧 schema 之所以还
多带一份，是因为它携带 `heapSlotOffset`（运行期槽 = `reservedSlots() + offset`，C++ 侧分配策略，
shader 里没有）——按 §7.12 判据那才是真第二来源。

#### 🔴 定案：compute push **不做 schema**

它只能携带 stride / offset / 字段名，**全部是从 shader 手抄的派生信息** ⟹ 按 §7.12 判据不是真第二
来源。而 `meshlet_cull_schema.lua` 头部自己就写着：「把它当『第二份布局』来维护是多余的，改 shader
后同步本表只为让提取清单继续指得对」——**布局那半是冗余，只有提取清单承重**；而 compute 的提取
清单可以隐式（"给定这些 shader，每个的 push constant"，判别子 = `ParameterCategory::PushConstantBuffer`，
值 11；注意 `VariableLayoutReflection` **没有** `getParameterCategory()`，必须走 `getTypeLayout()`）。

> ⚠ **这条是给后来人设的路障：别"顺手补上"一份 compute_push_schema.lua。** 补了就是造第二个
> 真相源，而且比图形侧那份更糟——图形侧那份至少带 `heapSlotOffset`。

#### 要钉的契约，在 compute 上换成两样

| 面 | 靠不靠偏移量 | 状态 |
|---|---|---|
| **push 布局（资源里的字节）** | 靠，但偏移量必须**由反射生成**，不手算 | ✅ B2-1′ 已落 |
| **binding 身份（哪个资源）** | 🔴 **完全不靠**：键 = shader 里的 binding 名，索引由构建期生成的接口表给 | ⏳ B2-1″ |

界线一句话：**「哪个资源」用名字，「资源里的字节」用生成的 struct。** 前者多变、后者不变——正好
对上"schema 确定不变的部分"那句原话。

#### B2-1′ 落地读数（2026-10-14）

第 10 条反射管线 `compute-push` → `rgl/generated/compute_push_types_generated.cppm`（8 个 struct，
**零 schema**；`meshlet_cull` 刻意排除——它的 `MeshletCullPush` 已由 schema 生成，别造第二个）。
换掉 evssm + izb 四个手写镜像 struct，删掉 **20 条同义反复 `static_assert`**（4 个 struct 各
4/4/7/5 条；主 agent 亲数——**子 agent 报的 16 是错的**，今天它第二次数错数）。那些 assert 量的是
手抄的 C++ struct，不是 shader ⟹ shader 换序它们照样全过。

- **三锚主 agent 自己重跑全命中**（逐位不变）：出厂 `95edc8ccd7a4`、EVSSM 开档 `865fd1aa5a82`、
  IZB 开档 `2316b566b113`。
- **变异**：换序 → `just reflect` → 测试红（**正是旧 assert 抓不到的那种**，且生成物自带 assert 全过）；
  改字段名 → **产品代码编译失败**（`no member named 'pixelW' in 'RGL::IzbCreatePush'`）= 布局第一次有出处。
- **两处被我的产物撞出来的口径洞**（都是真回归，不是"顺手改"）：
  ① `check_spirv_stride.py` 的探针机制对**入口单元**结构性无效（`import cull.comp` 被 slang 当
  路径 `cull/comp.slang`）⟹ 改走 NOTE + 汇总读数（`入口单元不可探针 8`），依据是一条推论：导不进去
  ⟹ 不可能成为别处的 `StructuredBuffer<T>` 元素 ⟹ R4 不变量对它空真。原有 18 个覆盖面一位没少。
  ② 🔴 **R3 放行面被生成物 glob 从 18 静默扩到 26**——`StructuredBuffer<CullPush>` 从"拦得住"变成
  放行，而 R4 对它又探针不可达 ⟹ **两条判据对同一个误用一起变哑**。已收口成
  `放行面 = 生成物 − 入口单元类型`（从 shader 侧现算，不硬编码产物名），变异验过现在会咬。

#### B2-1″ 定案形态（用户拍板：**暂时不值得为 shader 做全面运行时反射**）

`rgl/shader.cppm` 的 `makeMappedCompute` **已经是那个容器**（9 个调用点覆盖 7/9 枚），错的只是
键的类型：

```cpp
std::vector<std::pair<uint32_t, RGL::HeapSlot>>{ {0, HeapSlot{headSlot}}, {3, HeapSlot{primedSlot}} }
//                     ^^^^^^^^ 索引字面量 ⟹ shader 改号，C++ 静默绑错：无编译错、无运行期错
```

键换成 binding 名，索引由**构建期生成的接口表**给（不是运行期反射——SPV 侧不持有名字→binding 映射，
已 grep 确认；而全面运行期反射已被否）。输入恰好是第 9 条管线的 `compute_access.json`——
**它交付时没有消费者，这一刀让它有了**。

过 §7.12 判据：容器申报带「哪个资源绑哪个名字」（C++ 独有），接口表带「这名字存不存在、第几号、
可不可写」（shader 独有）⟹ **真双源**。

三条代价，落地前先认：① 名字打错是**启动期**才炸不是编译期 ⟹ 补一条纯文本 checks 扫 `bind("x", …)`
字面量与接口表对账（两侧各持一半信息，过 §7.12）；② 容器只覆盖走 `makeMappedCompute` 的 7/9，
`env_prefilter` / `mip_gen` 是启动期一次性自己写描述符，**明确不收**（别为覆盖率去改两枚启动期
shader）；③ 键查表只准在构造期，**一旦有人挪进 `recordFrame` 就是白送的每帧开销**——这条要落成
断言，不能靠自觉。

#### B2-2 = 这个容器的第二个消费者，不是独立一刀

容器申报（资源身份）× 反射（方向）→ 该有哪些边，与 `offgraph_sync` 边表对账。
§7.12 那张 12 行表是它的**人工半边**：要验的是"最后一列（靠什么同步）能不能推出来"——推得出就删表。
⚠ 本段原列的第三项输入「相位（顺序）」**已被 §7.15 实测否掉**（相位粒度是工作项级，排不了同一工作项内
那 4 类；而 RAW/WAR/WAW 都要 barrier ⟹ 判据与顺序无关）。落地读数 → §7.15。

#### 切刀表（覆盖 §7.10 的表）

| 刀 | 状态 |
|---|---|
| B2-1 compute 方向表进反射（`compute_access.json`） | ✅ `3eae652`；配套基线规则已撤（§7.12） |
| **B2-1′** compute push 布局进反射、换掉手写镜像（evssm + izb 四枚） | ✅ 本刀 |
| **B2-1′-b** 余下四枚：`CullFrameInput`（🔴 BIT-IDENTICAL 的 CPU 读回重放路，最险）、`EnvPrefilterPush`（在接口 unit 导出）、`FanoutPush`、mip_gen 镜像 | ✅ 已落，**并连带定案了 §7.14 那条** |
| **B2-1″** `makeMappedCompute` 键 → binding 名 + 构建期接口表 | ✅ `39c46e1`，读数 → §7.15 |
| **B2-2** 容器申报 × 反射方向 → 与边表对账（**无相位项**，见上） | ✅ 本批，读数 → §7.15 |
| B2-3 / B2-4（buffer 档 + 拒绝面 + 资源迁入） | ⏳ 形态由首发消费者定，见 §7.10 |


### 7.14 B2-1′-b 落地 + 一条新机制知识：**生成物本身不带来编译期保护**（2026-10-14 实测）

**落地**：八枚 compute push 契约现在**全部**是反射生成物，手写镜像清零。本刀四枚——`CullFrameInput`
改类型别名、`EnvPrefilterPush` / `MipGenPush` 从接口 unit 删除且不再导出、`FanoutPush` 删除。

`CullFrameInput` 的形态 = `export using CullFrameInput = CullPush;`。**别名不是改名**：`CullFrameInput`
说的是"每帧 GPU culling 输入、由 render system 产出"（谁生产它），比只描述字节的 `CullPush` 信息量
大；而布局必须单源于 shader。两个名字各管一件事，删掉任何一个都丢信息。

#### 🔴 本刀真正的产出：它推翻了我上一刀写进提交信息的结论

`a06c5b7` 写「改字段名 → 产品代码编译失败 = 布局第一次有出处」——**有隐含前提：调用点按名访问**。
实测反例（`9e0634c` 的提交信息里已就地纠正）：

```cpp
const EnvPrefilterPushData pushData{EnvPrefilterPush{level, mipLevels, levelFs}, {0, 0}, 0u};
//                                        ^^^^^^ 位置聚合初始化 ⟹ 与字段名完全无关
```

| 变异 | 换成生成物（位置初始化） | 换成生成物 + **指定初始化** |
|---|---|---|
| shader 改字段名 | 🔴 `just build` **退出码 0** | ✅ 编译失败并点名调用点 |
| 同类型字段换序 | 🔴 `just build` **不红** | ✅ 编译失败（`-Wreorder-init-list`） |
| 改字段类型 / 尺寸 | ✅ 红 | ✅ 红 |

⟹ **「C++ 写错 shader 内变量名要编译期知道」这条目标的承重件，不是"布局来自反射"，而是"调用点
必须点名生成物字段"。** 不点名就不存在"写错名"这件事——布局再单源，也只是把一份手抄换成另一份手抄。
C++20 还要求指定初始化**按声明序**书写 ⟹ 换序一起红。**一个约定堵两个洞。**

**已立约定**：对生成物类型的聚合初始化必须写 `.字段 =` 形式。外层 24B push data 包装
（`MipGenPushData` / `EnvPrefilterPushData`）**不要求**——它们不是生成物类型。

**顺带把"哪些 assert 该删"的判据钉死了**（这条容易做错成两个方向）：**断言量的是 shader 里存在的
东西 = 同义反复，删；量的是 shader 里不存在的 C++ 侧布局（`heapIndex@20` / 留白 / 24B 总长）=
真不变量，一条不删。**

#### ⚠ 两条残留，别读成"已闭合"

1. 换序那条的红是 clang `-Wreorder-init-list` **被 `-Werror` 抬成 error** 的结果；关掉 `-Werror`
   它退化成警告。
2. 🔴 **没有任何护栏守"必须指定初始化"这条约定**——新代码写回位置初始化不会有人响。做成文本
   checks 规则要处理注释误报，脆；当前裁决 = **先记债不硬凑**（脆护栏会把人训练成忽略警告）。
   要推翻这条裁决很容易：一条 `StructuredBuffer<...>` 式的正则 + 注释剥离，先有阳性锚点再谈入册。

#### 别名删掉默认成员初始化器的连锁（"删一份手抄"的副作用，容易漏）

手写版有 `planes[6]{}` / `sequenceCount = 0`，生成物一个都没有；而 `glm::vec4` 在本项目 GLM 宏配置下
（只有 `GLM_FORCE_RADIANS` / `GLM_FORCE_DEPTH_ZERO_TO_ONE`）默认构造**不清零** ⟹ `list.cullInput.sequenceCount`
是从未写过的**不确定值**，而原先那句 `auto cullInput = list.cullInput;` 整份拷贝会读它（按标准是 UB，
实测无害——"实测无害的 UB"正是最难查的那类）。

修法**不是**给生成物加初始化器（生成物是产物，加 = C++ 侧偷偷改契约），而是改成按名逐字段取：

```cpp
CullFrameInput cullInput{.planes = list.cullInput.planes, .sequenceCount = gpuCull->candidateCount()};
```

与上面那条约定同向，一次改动同时消掉 UB 和一次多余的结构体拷贝。

#### 覆盖诚实账：`FanoutPush` 出厂锚**零覆盖**

出厂 `dgcStatic=false` ⟹ `nEntryStreams=0` ⟹ `recordFanoutDispatch` 根本不进；而 `dgc_static`
**不在 `configs/diagnostics.lua` 里** ⟹ `--set` 被 `patch_lua` 的「键须恰好命中 1 次」护栏拒
⟹ **判据"看日志有没有 `+ streams` 后缀"在默认配置下永远看不到**。本刀用 `--config-base`（仓库外
配置副本，塞 `dgc_static = true`）补验：`+ streams 475×190920B @slot 8262` 出现 + `cullFanout`
子门对账绿 ⟹ 那行真在跑（该臂截图哈希 ≠ 出厂锚，预期：`dgc_static=true` 是另一条渲染路线）。

这是**存量限制**，不是本刀引入。`FanoutPush` 在出厂锚下唯一的防线是
`tests/test_compute_push_layout.cppm` 的编译期 offsetof。

**判据（主 agent 亲验）**：`just gates` 全绿（457 用例）；出厂锚 + `cull_replay=gpu_sync` 档锚各
3/3 命中 `95edc8ccd7a4`；四条变异（改名 ×2、换序 ×2）**全部 rc=1 且点名调用点**——其中三条在
上一刀是不红的。

顺带：子 agent 撞见 AGENTS 一条过期断言（「构建内置两个检查步骤均只警告不阻断」），实测
`reflection_check` **FATAL 阻断**，已按代码就地改准——变异后只还原 `.slang` 与生成物、不还原
`reflection_manifest.txt`，会剩一条建不过去的构建。






### 7.15 B2-1″ / B2-2 落地：**hazard 判据不需要顺序模型**，而人工半边必须被机器消费（2026-10-14 实测）

#### B2-1″ 落地读数（`39c46e1`）

`makeMappedCompute` 的映射键 = binding 变量名，号由构建期接口表给（与 `compute_access.json` 同一次
编译遍历产出 ⟹ 两份产物结构上不可能互不一致；`compute_access.json` 至此有了消费者）。

🎯 **收益的实测形态**：把 `izb_prime` 的 `primed` 从 `[[vk::binding(3)]]` 改成 `(7)` → 接口表跟着变
→ **IZB 开档锚 `2316b566b113` 0 像素差异命中，C++ 一个字没改**。改号不再静默错位（旧形态：构建全绿、
VVL 沉默、运行期往错堆槽读数据）。错误路径：调用点名打错一个字母 → 启动抛并列出该 shader 全部合法名。

#### 🔴 B2-2 的定案纠正了 §7.10 / §7.13 给它写的输入清单

两节原话都是「容器申报（资源身份）× 反射（方向）× **相位（顺序）** → 该有哪些边」。**相位那一项是
多余的，而且用不上**：

1. **用不上**：现有 `OffGraphSchedule` 的相位粒度是**工作项**级（四个工作项），而派生出的 5 类 hazard
   里 **4 类发生在同一个工作项内部**（IZB 的 create→prime 在 `izb_mask` 内、EVSSM 矩链在 `evssm_moments`
   内、cull→fanout 在 cull 段内）——相位模型结构上排不了它们的序。
2. **不需要**：同一资源上一写一读（或两写），**无论谁先跑都要 barrier**（RAW / WAR / WAW 三种 hazard
   都要）。⟹ 判据取「同槽 ∧ 至少一方写」，**与顺序无关，保守安全**。

所以输入是**两样**：反射方向 × 资源身份。资源身份 = **descriptor heap 槽号**（同槽 = 同资源），而它
恰好已经在调用点上（B2-1″ 之后调用点本来就写着 `{"head", HeapSlot{headSlot}}`）。

#### 派生集合恰好 5 类，且全部已有边 ⟹ 判据出厂即绿、有牙、**零豁免**

| 共享资源（heap 槽） | 写侧 | 读侧 | 覆盖它的边 |
|---|---|---|---|
| `mipSlots[0]` | `evssm_moments` · `moments` | `evssm_downsample` · `mipSrc` | `evssm_moments_chain` |
| `mipSlots[L]` | `evssm_downsample` · `mipDst` | 下一级 · `mipSrc` | `evssm_moments_chain` |
| `headSlot` | `izb_create` · `head` | `izb_prime` · `head` | `izb_head_create_to_prime` |
| `nodesSlot` | `izb_create` · `nodes` | `izb_prime` · `nodes` | `izb_nodes_create_to_prime` |
| `sequenceSlot` | `cull` · `sequences` | `stream_fanout` · `sequences` | `cull_seq_fanout_chain` |

**正确放过、不成为 hazard 的**（假阳性守卫，逐条有单测）：`primed` / `countBuf` / `streams` /
`clusterVisible`（写侧有、**compute** 读者无）、`cullTable` / `objectMatrixTable`（同槽两读）、
`depthTex` / `shadowDepth`（compute 只读，写者是图形阶段/拷贝，反射看不见）。

🔴 **表里零豁免条目是设计约束，不是"今天刚好不需要"**：不预留 `reason` 字段、不预留"声明不需要边"
的口子。理由 = §7.12 那条判据——一条"补一行带 reason 就绿"的登记表没有牙，本仓刚因此撤回过一条规则。
真出现不需要边的新 hazard 时，那是另一次设计讨论，不是现在猜形状的地方。

#### 🔴 表本身不是护栏，**被测试消费**才是

`kComputeHazardEdges` 是人工声明（边不携带资源身份，这一格只能人给）。它单独存在时**与注释等价**：
shader 改名、把写侧改成只读，表照样全绿。真正互锁的是 `tests/test_compute_hazard.cppm` 那条双源对账：
遍历表里每条声明，断言 ① 两个 `(shader, param)` 都存在于反射生成物 ② 写侧 `writable`、读侧 `readable`。

变异实测（主 agent 亲验 ①，其余为子 agent 读数）：

| 变异 | 读数 |
|---|---|
| 表里 head 那条的 `readerShader` 改成不存在的 shader → 跑 IZB 开档 | 🔴 **构造期抛**，两侧全点名 + 槽号：`未申报的 compute hazard——'izb_create.comp.slang.head'（heap 槽 8252）与 'izb_prime.comp.slang.head'（heap 槽 8252）…`（**主 agent 自己复跑**） |
| 给 `izb_create` 加 `primed` 只读绑定（= §7.12 说的"阶段 2 预留输出"真被接上） | 抛，点名 `izb_create.primed ↔ izb_prime.primed`（同槽 8254）——**本刀要拦的那类错** |
| `izb_prime.head` 改成 `RWByteAddressBuffer`（方向变宽） | **不抛**、锚仍命中 ⟹ 匹配确实与方向无关（对称） |
| `izb_create.head` 改成只读 | 双源对账用例红 ⟹ 人工半边与反射第一次互锁 |

四锚逐位不变（`95edc8cc` / `865fd1aa` / `2316b566` / cull 开档 `dc846396`，态 = `switch=true` 且 overlay 关——债-105 让 overlay 独立成默认关的门后该臂不再是出厂锚，→ `docs/mdi_plan.md` §19）= **零命令红线**的证据：
本刀没发一条 Vulkan 命令，纯记账 + 抛。

#### 覆盖诚实账：护栏**只建回来了一半**

§7.12 撤回规则时让出的那一格（「新增一条 `RW` 而没加同步边」），本刀收回 **compute ↔ compute** 那部分。
**非 compute 参与者仍然看不见**：frag 采样（`moments` → light.frag、`dstTex` → 材质）、mesh shader 读
`clusterVisible`、拷贝读回、host 读——反射只覆盖 compute 自己的参数。⟹ §7.12 那张 12 行表现在
**5 行被机器消费、7 行仍是人工**。这条边界落成 **债-119**，别读成"buffer 侧同步已对账"。

两条"锚命中但其实没跑"的假绿守卫，读数如实记：

- **出厂默认档 hazard 日志 = 0 行**（csm 档、izb/evssm/dgc_static 全关 ⟹ 7 个调用点里没有跨实例同槽
  访问对）。这是**关于场景的事实**，不是判据失效；判据活着的证据 = 开档档里那 5 行 + 变异①②会抛。
- **第 5 类（`cull` ↔ `stream_fanout`）在四个锚里跑不出来**：fanout 的 shader object 只在 `dgc_static`
  为真时创建，而 `diagnostics.lua` 没有这个键（`--set` 命中 0 次被白名单拒）。子 agent 用仓库外配置副本
  补验，日志实机出现。**与 §7.14 末 `FanoutPush` 出厂锚零覆盖是同一条债**（同一个键、同一个原因）。

#### 机制坑一条：存进日志的名字必须自己持有

`RgComputeBindingRecord` 第一版按草图用 `std::string_view shader`，而 shader 键来自
`computeShaderKey()`——它**返回局部 `std::string`** ⟹ view 悬空。症状不是崩溃而是**抛出的消息里是乱码**
（最难查的那类：护栏在响，但读不出在说什么）。修法不是加 `std::move`，是**分层**：瞬时入参留
`string_view`（`add()` 的形参），**存进日志的记录体用 `std::string` 自己持有**。

#### 切刀表（覆盖 §7.13 的表）

| 刀 | 状态 |
|---|---|
| **B2-1″** `makeMappedCompute` 键 → binding 名 + 构建期接口表 | ✅ `39c46e1` |
| **B2-2** 反射方向 × 资源身份 → hazard 派生，与边表对账 | ✅ 本刀；**输入清单被改准（去掉"相位"）** |
| B2-3 / B2-4（buffer 档 + 拒绝面 + 资源迁入） | ⏳ 形态由首发消费者定，见 §7.10 |

### 7.16 B2-3 重切：先铺「加 kind 必须被迫回答每一处」这块地基（2026-10-14 实测）

#### 为什么不能照 §7.10 的表直接开 B2-3

§7.10 自己就写了反对它排在这的理由：**「buffer 档 + 按名注册现在做 = 给一个还没有消费者的机制猜
形状（RT 缓存的 buffer 版要不要 heapSlot、usage 要哪些位、尺寸漂移按字节还是元素——只有首发消费者
meshlet_cull 能定）」**，而 meshlet_cull 排在 B2-4。所以「B2-3 机制刀 → B2-4 迁入」这个顺序**是
自相矛盾的**：机制的形状要由迁入方定。

**重切结论：B2-3 里唯一与消费者无关的部分先做，其余并进首发消费者那一刀。** 与消费者无关的部分
= §7.10 点名那条「顺带要修的静默坑」，而且它比那一条大得多。

#### 本刀的产出：「加第五个 kind 会在哪里静默走错」的**全量答案 = 10 处**

判据形态 = **穷尽 `switch`、不加 `default:`**，让 `-Wswitch` + `-Werror` 在加档当天编译失败点名。
`default:` 一写就等于把护栏拆掉（`-Wswitch` 只对「switch 缺 case」出声）。

变异实测（主 agent 亲跑第一处）：给 `RgAttachmentKind` 加第五个值 `buffer`、**什么都不接线** →
`render_graph.cppm:89: error: enumeration value 'buffer' not handled in switch [-Werror,-Wswitch]`，
构建失败。全量点名 10 处 / 5 文件，与人工审计逐条一致，`-Wswitch` 面内**零残留静默点**。

其中六处**原本是静默的**，本刀改掉。「静默后果」那一列才是本刀的真实价值——它们是六个当时还不存在
的 bug：

| 位置 | 原形态 | 第五档进来后的静默后果 |
|---|---|---|
| `aspectOf` | 三元式 | 拿错 aspect → barrier / attachment 全链走错方向（§7.10 点名的那条） |
| `isRtBackedKind` | `\|\|` 式 | extent 校验被跳过 |
| `isDepthKind` | `\|\|` 式 | clearValue 只写 `.color`、`depthWrite` 误拒 |
| `frameStartDiscards` | `kind != offscreen` | **就在 barrier 物化链上**，首帧 discard 形态错 |
| `isColorAttachment`（decorate） | `kind != depth` | 「首个色消费者」换人 ⟹ 高亮 / TAA / velocity **三个装饰器插入点一起挪位**（图形态错，不是文案错） |
| `ensureOffscreenAttachments` | 两档白名单 + 其余 `continue` | 布局槽永不随资源重建归零 ⟹ 下帧首条 barrier 拿**陈旧 `oldLayout`** |

另外四处（`kindName` + 三个执行器 `imageForSlot` / `viewForSlot` / `extentForSlot`）：三个执行器
switch **原本就穷尽**，是本刀的正面样板（M1 也点名它们，证明穷尽形态确实有效）；`kindName` 是新增的
——四个 `validateAttachment*` 的错误消息原先用 `kind == swapchain ? "swapchain" : "depth"` 兜底，
**未知档会被叫成 `"depth"`**，而附件注册在构造期、现场只剩一行日志 ⟹ 归因错。改用 `kindName` 后
🔴 **对现有四种 kind 消息逐字节不变**（连 `render graph:` 与 `render_graph:` 两种前缀写法都原样保留
——那是既有的不一致，本刀不顺手统一，统一会打掉逐字节断言的意义）。

**判为「无关」的四类**（即"这里不需要穷尽"的裁决，写下来免得下次有人以为是漏网）：① `find` / `count`
只挑某个具体档（`== swapchain` 数引用数、`== depthOffscreen` 找阴影图）——第五档本就不该被计入，
穷尽化反而是在给它塞不该去的桶；② **reject-rest 分支**（`validateAttachmentFormat` 的两表分立、
extentScale、history 的「不是 offscreen 就拒」）——第五档落进兜底是**拒得响亮**，本刀只把归因文案修准；
③ **经已穷尽的谓词间接把关**（如 `pass.depth` 的档集合已被穷尽的 `isDepthKind` 卡住）；④ Lua 词表
`parseEnum` 未登记串**响亮报错**。判据一句话：**只有"对 kind 全集做分类"的点才必须穷尽**，挑档式的
过滤不是分类，强行穷尽化 = 给第五档编一个没人要的语义。

#### 一条测试可测性事实（写下来省下次再撞）

**impl partition 从模块外不可 import**（`import render_graph:impl;` 语法即非法），而四个谓词住在 impl
的匿名 namespace ⟹ 判据全部经 `compileRenderGraph` **公开面**覆盖（`isRtBackedKind` 用 extent 校验
触发、`isDepthKind` 用 clearValue 分量 + `depthWrite` 反向、`frameStartDiscards` 用编译产物字段）。
**没有为了测试把 impl 提升成接口**——那是拿分层换测试便利，方向反了。

#### 验收读数

出厂锚 `95edc8ccd7a4` 3/3 命中（本刀**零行为变化**）；`just gates` 全绿（485 用例 / +11）；变异 M3
（把某条期望消息改一个字符）只有新的逐字节用例红、既有 substring 用例全绿 ⟹ 逐字节断言是**净增强度**。

#### 切刀表（覆盖 §7.15 的表）

| 刀 | 状态 |
|---|---|
| **B2-3 前置** 加 kind 的穷尽性护栏（本刀） | ✅ 本刀 |
| **B2-3 本体** buffer 档 + storage usage + 按名注册 + 拒绝面 | ✅ **与 B2-4② 同刀交付**（buffer 档形状由迁入方定，落地 → §7.17） |
| **B2-4①** EVSSM 矩图迁入 RT 缓存托管 | ✅ `f7d455d`（RT 缓存长出多描述符附件能力 + 迁入；「不升成图附件」与「装配期发号 / 首帧建图两段式」两条设计定案见该 commit 说明） |
| **B2-4②** meshlet_cull 迁入（buffer 档首发） | ✅ 本刀（2026-10-15）→ §7.17 |
| **B2-4③′** IZB 动态性首发（资源级动态 + erase/结转，还债-124；**按 D5 重切**，原「迁入」刀） | ✅ 本刀（2026-10-15）→ §7.18 |
| **B2-4④′** 升级存量：meshlet / EVSSM 升成图附件（试点后近乎免费；EVSSM 收益仅「依赖被图看见」） | ⏳ 未动（D5） |
| **B2-5** pass 级动态（运行期效果开关；相位表 / barrier 计划随重编译自动跟；这条才碰 B1a 索引形态） | ⏳ 未动（D5） |

### 7.17 B2-4② 落地实况（2026-10-15）：buffer 档 + 按名注册 + meshlet 迁入（B2-3 本体同刀）

**机制侧**：`RgAttachmentKind::buffer` + `RgUsage::storageRead/storageWrite`；`UsageInfo::layout` 改
`std::optional`（storage = `nullopt` ⟹ barrier 计划与 beginRendering 条目首行早退）——**plan-only 红线
做成类型**，`eUndefined` 作 newLayout 是合法 Vulkan 值、VVL 不报，兜底写法会把红线降级成注释。
按名注册表 = impl 分区 `render_graph:buffers`（lambda **按值捕获字节数、绝不捕获 `this`**：注册表是
进程级单例，服务只保证比 Renderer 久）。拒绝面落 `registerAttachment`（D1 第 4 条双向规则：buffer
附件名未注册即拒，五类诱惑不进黑名单）。Lua 词表补 `buffer` 与 `depthOffscreen`（债-122），且
**format 前置校验改按 kind 分派**——只补词表则 `depthOffscreen` 配深度格式仍报未知格式 = 「补了词表
还是不能用」的半吊子形态，债-122 的另一半就此收掉。kind 加档的 10 处静默点全部必答；`kindCases()`
的 aspect 列从布尔改 `vk::ImageAspectFlags`（buffer 的正确答案 = 空 mask，布尔表达不出来）。

**迁入侧**：`visibleBuffer` / `itemsBuffer` 按名注册；`recordDispatch` 的 4 条手抄 barrier → 4 个边名，
门 6→7、命令流 5→7（**一条边 = 一条命令**，合并档只合并掩码语义）。三个裁决：
① 异形①取**合并档**（3 个具名宽原子 `hostAndCopyWrite` / `computeReadWithHeap` /
`priorAccessGfxCsReadWrite`）而非 entries 分层；② 🔴 **meshlet 只一道门、刻意无子门**——scene buffer
缺位本身是 bug，挂「就绪没」子门 = 护栏在状态坏掉那刻主动闭嘴（§7.8 对 `recordCullPass` 的同型裁决），
正确形态 = 四条边全挂父门 + else 侧无条件 `error` 点名根因（原代码 else 静默）；③ `itemsBuffer`
登记但**故意不对账**——注册字节与写上界同出一个表达式 ⟹ 恒真护栏；`visibleBuffer` 两侧来源独立
（容量公式 vs itemCount）才是真对账。

**🔴 诚实账（引用本刀前必读）**：
- **出厂编译图不含 meshlet buffer 附件**（`compileGraphOrThrow` 先于 meshlet 装配；为机制形状改构造
  骨架 = §7.8 同型否决）。⟹ **D1 第 3 条的 Lua 半边（「存在 + 谁产谁消」）本刀无消费者**——生产上
  注册表与拒绝面只被 meshlet 启动期 `checkBufferWriteBound` 吃到。**「依赖变成图可校验对象」对
  meshlet 未兑现**；兑现的是：尺寸单一来源 + 对账能红（判据 5）+ barrier 进边表记账（B1b 机制）+
  机制就位。与 EVSSM B2-4①「不升成图附件」同形——buffer 档首发的收益就到「有家」为止。
  **B2-4③（IZB）动手前先裁这个岔路**：IZB 尺寸 = f(renderExtent) 且 extent 漂了换整个模块对象，
  构造顺序问题比 meshlet 更硬，可能绕不开「首帧建图两段式」（EVSSM 先例）。**⟹ 2026-10-15 已裁
  = D5**：图 = 脏驱动派生件，重编译不受构造顺序约束；本段与 B2-4① 的两处「不升成图附件」绕开被
  推翻（当时处置正确，不再约束后续刀）。
- syncval 对 meshlet 的边 **0 条**（与 §4 既有实测同形）⟹ 判据 6 的红只能来自对账与结构护栏，
  不能指望 VVL。

**验收读数**：五锚逐位（出厂 `95edc8cc` 3/3、meshlet 门关 `54e40665`、cull 开档 `dc846396`、EVSSM
`865fd1aa`、IZB `2316b566`）；判据 3/5/6 逐条红且归因到点名边/两侧字节数/结构位；变异 10 条（单元 7 +
真程序 3）。**量具坑一条**：还原变异用 `cp -p` 保留旧 mtime ⟹ ninja 不重编，「还原后基线」跑的仍是
变异体——还原必须不带 `-p` 或显式 `touch`。

### 7.18 B2-4③′ 落地实况（2026-10-15）：动态图第一刀——安全点重编译 + 按名结转 + 延迟逐出（债-124 结案）

**机制三件套**（D5 第 1/2/3 条的落地形态）：
- `dynamic_render_graph`(.cppm+`_impl`)：派生件容器——生产者声明**幂等 + 试编译 fail-fast**（编译不过的
  desc 当场拒，不让它落进真相源后每帧刷失败日志）；重编译失败**脏位不清**、下帧安全点重试；重编译次数
  打点进日志（`DynamicGraph: recompiled (count=N)`）= 零命令红线的观测面。
- `carryOverPerSlot`：逐 slot 跨帧表（布局追踪等）**按名字结转**。「结转 ≠ 重置」是判据级的：布局表被
  重置成 `eUndefined` 后下帧首条 barrier 的 src 就错了，且错法延迟、归因成本极高 ⟹ 变异「结转改重置」
  必须红（`tests/test_dynamic_render_graph.cppm`）。
- `pending_evictions`（rlbase 叶子）：frames-in-flight 逐出台账——登记帧 D 的 due = D + framesInFlight，
  到期帧安全点（围栏后）才销毁；**同名未到期再次 schedule = Error**（「有 in-flight 用户逐出必须炸」）。
  纯算术就该住 rlbase 被单测锁死，而不是散在 Renderer 里。

**安全点** = beginFrame 围栏后、录制前（`vkrenderer_passrec` 调 `recompileGraphIfNeeded`，void 返回 +
失败内部出声 = drawFrame 零分支）。IZB 迁入：`izb_head` / `izb_nodes` 注册为 buffer 附件，两侧来源
独立 = 真对账（注册侧 f(extent) 公式 vs dispatch 写上界，闭式期望）；extent 漂移 → 重建 → 重注册 →
标脏 → 下个安全点吃到新尺寸。sandwich 钩子与 13 条边一行没动（pass 级动态 = B2-5）。

**零命令红线（出厂态新判据）**：出厂配置永不变脏 ⟹ 重编译 = 0。实测：出厂 smoke 打点 **0 条**（主 agent
亲验）、IZB 开档 = 1；`regress ab` 出厂 A×B 逐位相同（主 agent 亲验）。

**resize 冒烟抓出的真 bug**（🔴 五锚永远抓不到，教训进 §1.2 同族）：retire-on-drift 让漂移后的 `ensure`
走首建分支领**新描述符槽**，而消费方描述符集仍指退役节点的指针链 ⟹ 退役窗口内持续采旧尺寸 G-buffer
（症状 = resize 后整幅横向压缩 720/800），due 后悬空。修复 = 同名退役条目**继承 `descriptors` vector**
（move 转移缓冲 = 链地址不变、槽号随名字留下、旧图 due 才毁）+ 继承路径跳过 `waitIdle`（逐出成常态后
这条整设备停顿不能每帧付）。**教训：固定尺寸锚抓不到漂移流 bug——resize 冒烟是唯一那张网，验收清单
必须带它。**

**验收读数**：五锚逐位（含 IZB 开档 `2316b566`）；resize 冒烟与全新同尺寸 run 逐位相同（`3b04944c`）；
`just gates` 全绿（511 用例 + gpu 46）；变异逐条红：结转改重置（7 断言）/ nodes 公式 ×8（闭式）/
债-124 正反两半 / M8 删继承块 / 判据 6 拆 `UsageInfo::layout` optional / 真程序 write-bound ×2 启动
throw 点名两侧 / A 档 sandwich 不还布局在**含重编译的 IZB 开档**逐帧咬。

**过程记录**：本刀第一轮实现被外部 `git checkout/clean` 整体抹除（11:51 事件），按 HEAD 重建 + 快照
`/tmp/opencode/b24_snap/`；工作树完整性以 `regress ab` 逐位 + 全量测试为证。

---

## 8. 待挂号（本批**未落**总账，等用户选处置）

1. ~~`RenderTargetCache` 没有 erase 路径~~ **已挂号 = 债-124**（2026-10-15 D5 裁决转正；原观察、
   触发条件与「谁让图变动态谁先还这笔」的原文 → 总账债-124 行 + §6 D5 第 4 条，本节不再承载副本）。

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
