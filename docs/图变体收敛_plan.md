# 图变体收敛（forward / deferred / offscreen）

> **状态：现行·施工中**（2026-10-17 调研；2026-10-18 三枚裁决全部关闭，见 §6.1，开始施工）。
> 上游：`docs/配置正交性_plan.md`（债-129，**已结案**）；MDI 侧定案：`docs/mdi_plan.md` §19.20。

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
⚠ 2026-10-18 裁决后本表 ③④ **不执行**（文件写图保留 ⟹ 触发面仍在），且 ② 另欠一条 skybox 死路 → §6.2。

## 6. forward 依赖盘点（2026-10-18 全量实测，替换初稿成本表）

初稿"很多 imgqa 只存在于 forward"**基本不成立**。25 条 imgqa 子命令逐个对账：

| 类别 | 条数 | 事实 |
|---|---|---|
| 🔴 真·只能 forward | **1**：`probe`（闭式解探针） | 依赖不是"最终像素必须 forward"，而是"着色输入不过 G-buffer 量化"（下详） |
| 硬编码 deferred（izb / swim / shadow-kernel-scale 等） | 4 | 方向与本题相反，删 forward 零影响 |
| 走交付 configs（已是 deferred） | 5 | 零影响 |
| "默认 forward" | 12 | **全部只因合成 profile 的模板 = `qa/configs/analytic`**（`engine_profile.cpp:29`）+ 模板 `render_graph.lua:31` 一行。无任何命令硬编码 forward；模板改一行全体迁移。判据对象是上屏像素**统计量**（覆盖率/逐帧差），换路线换的是激励不是判据成立性 |
| 不跑引擎（selftest / diffstruct 等） | 7 | 零影响 |

**探针的 forward 依赖精确到式**（三条实测，比初稿估的轻）：

1. 光照式两路**逐字相同**：`light.frag.slang:101-122` `shadeLight` ≡ `model_pbr_ibl.frag.slang:16-36`
   `shadeDirect`（连 epsilon `1e-7·a2` 都一致）⟹ 探针闭式模型 `analytic.cpp:shadeDirect` 一行不改。
2. tonemap 尾段（ACES + sRGB）两路**共用同一个 `tonemapPass()`** ⟹ 探针那半条链也不改。
3. 唯一硬墙 = G-buffer 编码往返：`gbuffer_albedo` 8bit sRGB、`gbuffer_metalrough` 8bit unorm
   （`gbuffer_contract.cppm:48-54`）。⚠ 初稿写"法线不经 **8bit** 量化"**数错**——normal 是
   `rg16_snorm`，且探针场景法线轴对齐，oct 往返精确无损。要补的只是两个 8bit 通道的量化往返（~15 行）。

**七道探针门在 deferred 下的对称性**（`qa/configs/analytic/scene.lua:11-16` + `runtime.lua:9-14`）：
env.intensity / ambient / ao / exposure / shadow / wet 六道门 deferred 下同样成立；
`skybox=false` 失效但对探针像素不承重（墙挡满画面，配置注释自称"双保险"）。

**三套 forward 冻结配置目录的核验**：`analytic` 头注**没写任何必须 forward 的理由**（`:31` 是裸选择）；
`nm_rig` 的 forward 锁**已被它自己的 `camera.lua:6-11` 判为不需要**（俯角 25° 地平线出画，注释明写
"deferred 路线可用"）；`env_band` 的锁仍成立（3° 掠射上半屏是天穹，`skybox=false` 只在 forward 生效
——该标志真实读取点 `render_list_impl.cppm:756`，该目录配置注释里的行号已漂）。nm_rig / env_band
**没有任何自动化消费者**（全仓 grep 只有 analytic 与 ssr_rig 被引用）。

### 6.1 三枚裁决（2026-10-18 用户拍板）

1. **探针重立 = 分段双锚**：最终像素锚保留（闭式补 G-buffer 量化往返，tonemap 尾段的唯一无基线判据）
   \+ **新增 `gbuffer_*` 附件锚**（期望字节 = 纯编码闭式，零光照建模）。附件回读走现成通道：
   RPC `/shot {gbuf:N}`（`debug_rpc_exec_impl.cppm:311-313`，imgqa 客户端补 `gbuf` 参数即可）。
   红灯分段归因：gbuffer 锚红 = 编码契约坏；最终锚红而 gbuffer 锚绿 = 光照或 tonemap 坏。
   **G-buffer 锚是 forward 下不可能存在的判据** ⟹ 删 forward 不是失去无基线判据，是把它变深、变多。
2. **文件写图保留，换触发**：`variant` 键**整个消失**（§4 裁决：没有消费者的真相源）；
   新触发 = 文件里有 `attachments`/`passes` 键 → 解析自定义图，否则 → 内建 deferred。
   ⚠ 连带：出厂 `configs/render_graph.lua` 与 qa 配置目录的 attachments/passes 块（forward 图的逐字副本）
   **必须随刀删除**，否则新触发下它们会变成"自定义 forward 图"静默生效。
   ⚠ 对级联的修正：**③④ 不执行**——自定义图仍可达 ⟹ C 类锚点 nullopt 分支保留、
   `deferredLight` 图事实**不恒真**（自定义图可以没有 light pass）；消失的只有 variant 枚举本体
   与 `gate_lint_impl.cppm:464-474` 的交叉校验。
3. **env_band 归档冻结**（实验已结案、零消费者，随 forward 删，git 沉淀现场）；
   **nm_rig 切 deferred 保留**（按其自身 camera.lua 的判定）。

### 6.2 级联新增一条（§5 表漏账）→ 2026-10-18 施工改判

**`skybox` 标志**：它只门控 skybox DrawCall（`render_list_impl.cppm:755-760`），执行该 DrawCall 的
pass 只存在于 forward 与（已删的）offscreen 图。初判"随 forward 一起删"——**施工实测改判**：
deferred 的天穹对应物是 `env.intensity` 总闸（light.frag sky 分支 2026-10-18 起门控
`mipRange>0 && intensity>0`；intensity 不乘天穹亮度、但 =0 连天穹一起关——imgqa temporal
抑制比被"关不掉的天穹"污染实测抓出，与 unlit 破口同批修复）。
② 删的是 **forward draw 侧的那枚标志**；"env 开不开"这一轴由 intensity 独立表达，deferred 已具备。

## 7. 施工序（2026-10-18 定稿）

1. ~~债-129 剩余项~~ **已结案**（N9，2026-10-17）。
2. **刀①**：删 offscreen（零前置、零使用者）。blit **机制**保留——Lua 词表能声明 blit pass
   （`render_graph_config_impl.cppm:250`），自定义图仍是它的生产者。
3. **探针重立**（§6.1-1，刀② 的硬前置）：`Engine::shot` 补 `gbuf` 参数 → probe 切 RPC 模式
   单启动三拍（后缓冲 + 通道 0/3）→ 闭式补量化往返 → 模板切 deferred。
   验收 = `just analytic` 双锚绿 + 变异对照（临时改 `deferred_geometry` 写入，确认 gbuffer 锚红）。
4. **刀②**：删 forward + variant 枚举 + 换触发（§6.1-2）+ skybox 死路（§6.2）+ qa 配置目录处置（§6.1-3）。
   逐刀独立验收：逐位锚 + `just gates`。

## 8. 待决

（两枚原待决已于 2026-10-18 拍板关闭：文件写图 → 保留换触发；探针代价 → 做，形态 = 分段双锚。读数见 §6.1。）

## 9. 施工读数（2026-10-18）

1. **刀① 已落地**（offscreen 变体删除，`--variant offscreen` 被 CLI 拒；blit 机制保留）。
2. **探针重立已落地**（§6.1-1 双锚）：`Engine::shot` 补 `gbuf` 参数（服务端本就有
   `/shot {gbuf:N}`）；probe 切 RPC 单启动三拍（副本只改 rpc/llm_debug 两枚回读键，渲染门不碰）。
   首跑三锚全绿：锚③ (88,43,244) 与 forward 时代历史实测**逐字节相同**；
   锚② Δ1 = rough 0.9×255=229.5 平局带（引擎 float32 链落 229，容差理由已登记判据表）。
   **变异对照两次**（albedo 写侧 ×0.5 → 锚①红 Δ67/锚②绿；metalrough g↔b → 锚②红 Δ230/锚①绿）
   = 分段归因成立，判据有牙（读数 → `qa/log/imgqa_calls.jsonl`）。
3. **模板切 deferred 抓出两个引擎静默破口**（均已修，见 git log）：unlit 材质在 deferred
   被当 PBR 打光（= 交付缺陷：KHR_materials_unlit glb 在出厂配置下就错）；deferred 天穹
   关不掉（§6.2 改判）。**这条印证了 §6 盘点的方向**——"删 forward"的价值不只是少维护面，
   是把 forward 掩盖着的 deferred 缺口逼出来。
4. 合成 profile 判据迁移验收：aa-coverage / temporal / temporal-residual / faces / switch /
   attribution 在 deferred 下全绿（temporal 抑制比 3.52× vs forward 同靶 3.51×）。
5. **刀② 未动工**（下一步）。刀② 连带清单在 §5/§6.1/§6.2 之外新增一条：
   `environment.skybox` 配置键随 forward draw 侧一起删（deferred 的对应表达 = intensity）。
