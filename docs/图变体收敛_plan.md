# 图变体收敛（forward / deferred / offscreen）

> **状态：调研**（2026-10-17，同日二改：用户回答否掉了初稿的主结论，见 §3）。
> 本文只出**事实与模型**，不含已拍板的施工承诺。
> 上游：`docs/配置正交性_plan.md`（债-129）；MDI 侧定案：`docs/mdi_plan.md` §19.20。

## 0. 结论

**该砍的是 `RgGraphVariant` 这枚枚举，而 `offscreen` 这一档可以直接删——不是改造、不是补格。**
工具依赖的是**离屏能力**（离屏 RT + 回读），它与 variant 无关且每个图都具备；
`variant = "offscreen"` 这张 blit 呈现图**全仓零使用者**。

删完之后 `variant` 只剩光照路径一轴，而"配置面不该有这一轴"是另一刀（§6 级联末端）。

## 1. 设计种子（用户 2026-10-17 两句）

> 「offscreen 是固有能力，讲道理和渲染模式没有任何关系。」
> 「offscreen is needed by llm visual mode and imgqa test. **not needed by user**.
> so **the config is not needed** unless you prefer to use the config to launch offscreen mode.」

两句合起来是完整裁决：offscreen 是**工具侧需求**，所以它**不该出现在配置面**——
既不该是 `variant` 的一个值，也不该新设一个键。

## 2. 关键区分：`kind = offscreen`（承重）vs `variant = offscreen`（零使用）

| | 是什么 | 谁在用 | 判定 |
|---|---|---|---|
| `RgAttachmentKind::offscreen` | 「渲到一张不是 swapchain 的图」 | `render_graph_builtin.cppm` 里 **13 处**：`scene_hdr` / `ao` / `gbuffer_*` / `scene_rt` 全是离屏 RT；`RenderTargetCache` 整个就是离屏 RT 缓存 | 🔴 **固有能力， pervasive，动不得** |
| `RgGraphVariant::offscreen` | 一张「三段渲 `scene_rt` + composite **blit** 上屏」的具体图 | **零**：`configs/` 与 `qa/configs/` 零命中、`tools/imgqa/` 零命中、llm_debug 与 `frame_capture` 的截图/回读路**根本不引用 variant**（实测 grep 空） | ✅ **只有 CLI 的 `choices` 名单里还挂着它** |

⟹ 用户那句「固有能力」在代码里就是 `kind = offscreen`；而 `variant = offscreen` 是把它
错误地实例化成了一种"渲染模式"。

## 3. 初稿错在哪（记一条，防后来者重犯）

初稿按「光照路径 × 呈现方式 = 两轴 4 格」推出**缺 `deferred + 离屏` 那一格、砍 offscreen 要先补它**。
**这条被用户的回答直接否掉**：工具要的是离屏 RT 与回读（每格都有），不是"blit 呈现"这个模式。

教训：**两轴模型是"什么值得存在"的判据，不是"什么该被实现"的判据。** 空格子不一定要填——
先问那格有没有人站。这次是我拿模型的整齐度当了需求。

## 4. 🔴 `variant` 是一枚**没有消费者的真相源**

`render_graph.cppm:649-657` 的裁决原文（2026-10-02 写下）：

> 它与配置层的 `RgGraphVariant::deferred` **是两件事**……消费方要的是"有没有人在跑 deferred
> 光照 / 有没有 G-buffer 可消费"，不是"配置里 variant 写了什么"——**自定义 Lua 图可以在
> forward 变体下手写 light pass**。两个真相源的分歧交给 gate_lint 交叉校验，**不在这里抹平**。

实测支持：图事实 `deferredLight` 有 3 个生产消费者（`izbPreFrame` / `evssmPreFrame` 早退 +
`gate_lint` 算 `Ctx`）；而 `CompiledGraphResult::variant` 在生产侧的**唯一**消费者就是那条
交叉校验本身（`gate_lint_impl.cppm:330-337`）。

⟹ 本项目哲学第 1 条点名的形态：**两个真相源 + 一条对账判据**，而对账判据的存在本身就在为那个
本该删掉的第二真相源交维护税。删枚举 ⟹ 交叉校验整条消失，不是搬家。

⚠ 同因的**活雷**：配置文件找不到 → `warn "using default 3-pass graph"` + 返回 **forward**，
而**出厂默认是 deferred**（`configs/render_graph.lua:31`）⟹ 路径写错就静默换一个渲染器，
且 warn 里的 pass 数还是错的（forward 是 5 pass）。代码缺省 = forward、配置缺省 = deferred。

## 5. 删除级联（这才是"更好维护"的实际内容）

不是"少一个 if"，是一串**只服务于被删形态的判据与装饰**跟着消失：

| 步 | 删什么 | 连带消失 | 证据 |
|---|---|---|---|
| ① | `variant = "offscreen"` 那张图 | `composite` blit pass、`scene_rt` 附件、`--variant offscreen` CLI 档 | 零使用者（§2） |
| ② | `variant = "forward"` | **6 个 forward-only frag + 12 条 CMake 目标**；`styleMap` 分支（含与 deferred **不一致的 throw**）；D1 对等这条决定的 **18 处 / 12 文件**注释副本 | `CMakeLists.txt:30-67`、`main_scene.cppm:293-300` |
| ② | 同上 | **Lua 数据驱动写图整条路**：`readRoleList` 18 + `readAttachments` 40 + `readPass` 35 + `readPasses` 15 = **108 行**失去唯一调用者（`resolveVariantConfig` 对 preset 提前 return，只有 forward 落到解析） | `render_graph_config_impl.cppm:305-319` / `:367-368` |
| ③ | ①+② 之后 | 🔴 **装饰器锚点的 nullopt 分支不可达** ⟹ S3-C 那 **4 条 `!consumerIdx` 硬失败 + 1 条 D 类 G-buffer 契约**（forward 才有）连同其用例一起删。原因：forward 与 deferred 的 `tonemap` 都「写 swapchain 且 sampleRead 场景色」，锚点必命中；而唯一能造出"无采样上屏消费者"的图正是已删的 offscreen 与自定义 Lua 图 | `render_graph_decorate.cppm:88-99` |
| ④ | 只剩 deferred 之后 | `graphHasDeferredLight` 变成**恒真** ⟹ 3 条 `deferredLight` lint error + 2 处运行期早退 + `izb_mask`/`evssm_moments` 的 `mayBeAbsent` 全删。⚠ 恒真的判据**比没有判据更坏**（它让"绿"看起来像结论） | `gate_lint_impl.cppm:113-133` |
| ⑤ | 级联末端 | 🔴 **`configs/render_graph.lua` 整份文件消失**——实测它的顶层键只有 `variant` / `attachments` / `passes`，即"选一张 preset 或自己写一张图"；两者都没了，这文件就没有内容可表达 | 实测顶层键 |

**保留**：B 类次序守卫（「TAA 必须最后」那类）——它与变体无关，是 S3 的真·剩余。
**保留**：动态渲染图（`declarePass` / `withdrawPass`）——运行期图派生不受影响，死的只是**文件**写图。

## 6. forward 的成本表（拆轴后它只剩成本，没有模式包袱）

| 项 | 读数 |
|---|---|
| 🔴 **闭式解探针绑 forward** | `qa/configs/analytic/render_graph.lua:31 = "forward"` + `scene.lua:21` 的 forward 规格。它是五层判据里**唯一不吃基线图像**的那层 |
| forward 的逐位锚点 | **0 条** ⟹ 今天只被"必须能起跑"覆盖，像素正确性全靠上面那层钉 |
| 二相性漏进错误处理 | 同一种"规格缺失"：deferred **静默回落** `deferred_geometry`，forward **throw**（`main_scene.cppm:293-300`） |
| 三套 qa 冻结靶 | `analytic` / `nm_rig` / `env_band` 用 `variant = "forward"`；实测它们的附件/pass 名集合与出厂 forward 图**逐字相同**（只是"选 forward"，不是自定义图） |

🔴 **唯一硬前置：闭式解探针重立到 deferred。** 它要额外建模 G-buffer 编码（量化），是真活。
**探针没重立之前不许删 forward**——那是拆掉自己唯一的无基线判据去换维护面。

## 7. 排序

1. **债-129 剩余项先做完**（嵌套键护栏 → 拍板 A）。它们只碰配置面与 lint 表，不依赖变体存废；
   且 D 刀已把 A 要动的行塌成块条目（`142f7c2`），现在改只碰块声明。
2. **然后单独立项做闭式解探针重立**（§6 硬前置，要单独估一次成本）。
3. **最后走 §5 的级联**，按 ①→⑤ 逐刀，每刀独立可验收（逐位锚 + `just gates`）。
   ⚠ ① 可以**立刻单独做**（零前置、零使用者），且它一动，S3-C 的 4 条 C 类硬失败就只剩
   "自定义 Lua 图"一个触发面——这本身就是"① 值不值得先做"的读数。

## 8. 待决

1. §5 的 ② 之后，**还要不要保留"用文件写图"这个能力**？保留 = 那 108 行解析路与 46 行配置块留着，
   且 C 类锚点仍可达（自定义图能造出无采样上屏消费者）；不保留 = 级联继续走到 ⑤。
   本项目是学习项目，"图是数据"这件事有教学价值——**这条我不替你拍**。
2. 闭式解探针重立的代价（§6）——要不要做、什么时候做。
