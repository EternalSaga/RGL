# 图变体收敛（forward / deferred / offscreen）

> **状态：调研**（2026-10-17）。本文只出**事实与模型**，不含已拍板的施工承诺。
> 上游：`docs/配置正交性_plan.md`（债-129）；MDI 侧定案：`docs/mdi_plan.md` §19.20。

## 0. 一句话结论

**该砍的是 `RgGraphVariant` 这枚枚举，不是 forward 这个渲染器。**
光照路径（forward / deferred）与呈现方式（采样上屏 / 离屏 + blit）是**两条正交轴**，
今天被压成一枚三值枚举。拆开之后，"要不要留 forward"才第一次变成一个**纯成本问题**，
而不是一个背着"模式"包袱的问题。

## 1. 设计种子（用户 2026-10-17 原话）

> 「offscreen 是固有能力，讲道理和渲染模式没有任何关系。」

这句话本身就是判据：`offscreen` 今天住在 `RgGraphVariant{forward, deferred, offscreen}` 里，
等于把"怎么把画面弄上屏"当成"用哪条光照路径"的一种。

## 2. 两轴模型：摊开是 4 格，今天只有 3 格

| 光照路径 ＼ 呈现方式 | 采样上屏（pass 直接写 swapchain） | 离屏 + blit（渲到 RT 再 composite） |
|---|---|---|
| **forward**（直接光，画在 opaque 段） | ✅ 今天的 `forward` | ✅ 今天的 `offscreen` |
| **deferred**（G-buffer + light pass） | ✅ 今天的 `deferred` | ❌ **不存在** |

⟹ 🔴 **"砍 offscreen" 不是删代码，是补上缺的第 4 格。** 把 offscreen 从变体降级成呈现属性之后，
`deferred + 离屏` 必须能跑——否则只是把一格的实现换个名字。

## 3. 🔴 核心发现：这枚枚举是**没有消费者的真相源**

`rgl/render_graph.cppm:649-657` 的接口注释（2026-10-02 收口时写下的裁决原文）：

> 它与配置层的 `RgGraphVariant::deferred` **是两件事**……消费方要的是"有没有人在跑 deferred
> 光照 / 有没有 G-buffer 可消费"，不是"配置里 variant 写了什么"——**自定义 Lua 图可以在
> forward 变体下手写 light pass**。两个真相源的分歧交给 gate_lint 交叉校验，**不在这里抹平**。

实测支持这句话：`deferredLight`（图事实）的全部生产消费者只有 **3 个**
（`izbPreFrame` / `evssmPreFrame` 早退 + `gate_lint` 算 `Ctx`），**没有一个消费者用 variant 意图**。
`CompiledGraphResult::variant` 在生产侧的唯一消费者就是**那条交叉校验本身**
（`gate_lint_impl.cppm:330-337`：`variant==deferred && !deferredLight` → error，反向 → warn）。

⟹ 这正是本项目哲学第 1 条点名的形态：**两个真相源 + 一条对账判据**，而对账判据的存在本身
就在为那个**本该删掉的第二真相源**交维护税。删掉枚举，交叉校验整条消失（不是搬家）。

⚠ 另有一条**活雷**（同因）：配置文件找不到 → `warn "using default 3-pass graph"` + 返回 **forward**，
而**出厂默认是 deferred**（`configs/render_graph.lua:31`）⟹ 路径写错就静默换一个渲染器，
且那句 warn 的 pass 数还是错的（forward 是 5 pass）。代码缺省 = forward、配置缺省 = deferred，
两个缺省不一致。

## 4. 缺的第 4 格成本：低

`offscreenRenderGraphDesc`（`render_graph_builtin.cppm:107-140`）的结构是
三段渲 `scene_rt` + 一枚 `composite` **blit**（`transferRead → transferWrite`），
`scene_rt` 格式留 `inherit` 由执行器解析成 swapchain 格式以满足 LINEAR blit 前提。

⟹ `deferred + 离屏` 的改动是：**把 deferred 链尾 `tonemap` 的输出附件从 `swapchain` 改成 `scene_rt`，
再追加一枚 composite blit**。HDR→SDR 仍在 tonemap 里做完，`scene_rt` 本来就是 LDR，
blit 兼容前提自动满足。**没有渲染难题，只有图描述今天是一张整块而不是两个可组合维度。**

## 5. 锚点泛化：C 类 5 条硬失败的存在理由是**锚点过窄**

装饰器找落点用的判据（`render_graph_decorate.cppm:88-99`）：

```cpp
const bool writesSwapchain = any_of(p.outputs, attachment == "swapchain");
if (writesSwapchain && sampledColorName(desc, p)) return i;   // 要求「采样一个色附件」
```

其注释自陈：「（transferRead 不是 sampleRead）⟹ 返回 nullopt，两个装饰器都自动不参与该图形态」。

⟹ 它真正要找的**不是"采样上屏的消费者"，而是"呈现色的终端生产者"**——后者在 4 格里都存在：
采样上屏时是那个写 swapchain 的 pass；blit 上屏时是写 blit 源 RT 的那个 pass。
锚点按后者重写后，**债-129 S3-C 那 5 条「无落点」硬失败的大部分触发面自己消失**
（它们拒的从来不是"离屏"，是"呈现方式与装饰器的接线假设不匹配"）。

## 6. forward 是**独立**问题，且拆轴后只剩成本

拆轴之后"要不要留 forward"不再涉及模式，只剩这张成本表（2026-10-17 实测）：

| 项 | 读数 | 证据 |
|---|---|---|
| 闭式解探针绑 forward | 🔴 **唯一不吃基线图像**的判据层，`variant = "forward"` | `qa/configs/analytic/render_graph.lua:31` + `scene.lua:21` 的 forward 规格 |
| Lua 数据驱动写图 | forward 是**唯一入口**（deferred/offscreen 命中 preset 即 return、文件 passes 忽略） | `render_graph_config_impl.cppm:305-319` / `:367-368` |
| 该路的解析代码 | 四个函数 **108 行**（`readRoleList` 18 / `readAttachments` 40 / `readPass` 35 / `readPasses` 15）失去唯一调用者 | 实测行号 |
| 配置里只有 forward 才读的块 | **46 行**（`attachments` 32–40 + `passes` 41–77） | `configs/render_graph.lua` |
| forward-only 着色器 | **6 个 frag** + 12 条 CMake 目标 | `rgl/shaders/model*.frag.slang`、`rgl/CMakeLists.txt:30-67` |
| D1 对等这条决定的副本 | **18 处 / 12 个文件**（代码 + 配置 + shader 注释），其中 2 份已过期且方向误导 | 实测 grep |
| forward 的逐位锚点 | **0 条** ⟹ 它今天只被"必须能起跑"覆盖，像素正确性全靠闭式解探针一家钉 | `docs/还债总账.md` 锚点小节 |
| 二相性漏进错误处理 | 同一种"规格缺失"：deferred **静默回落** `deferred_geometry`，forward **throw** | `main_scene.cppm:293-300` |

🔴 **红线：闭式解探针没重立到 deferred 之前，不许删 forward。** 那是拆掉自己唯一的无基线判据去换维护面。

## 7. 与在飞的债-129 的关系

- **不冲突**：D 刀塌缩的块条目全是 owner 行，**一条 variant 行都没有**（已落地，`142f7c2`）；
  S3 剩余的「装饰次序契约」也不依赖变体存废。
- **会被本文改写的**：① 拍板 A 的 `meshlet.enable` 改名（要动的 lint 行刚被 D 塌成块条目，
  现在改只碰块声明——**D 先做已兑现这个收益**）；② S3-C 那 5 条硬失败的**消息文案**（锚点泛化后
  "offscreen 变体用 blit 上屏"那句要改成呈现方式口径）；③ 交叉校验整条删除。
- **顺序建议**：本文的**拆轴**（§4 + §5）应排在债-129 剩余项（嵌套键护栏 → 拍板 A）**之后**，
  因为它改的是图描述形态，而剩余项只碰配置面与 lint 表；反过来先拆轴会让剩余项踩在流沙上。

## 8. 待决（我不替项目主人拍的）

1. **图描述住哪里**：拆轴后 `deferred` / `offscreen` 还是 C++ 硬编码 preset，还是全部下沉成
   Lua 数据（那 §6 的"108 行解析路"就从成本变成**唯一路径**，删 forward 的账要重算）？
2. **第 4 格要不要真做**：`deferred + 离屏` 有消费者吗？今天 `offscreen` 的全部用途是 DIAG 截图，
   而**源配置无一使用 offscreen**（`configs/`、`qa/configs/` 零命中）⟹ 若第 4 格无人要，
   那"拆轴"的收益就只剩"删掉一枚枚举 + 一条交叉校验"，值不值要看 §3 那条活雷你多在意。
3. **闭式解探针重立的代价**：deferred 上要做闭式解，得额外建模 G-buffer 编码（量化）。
   这是删 forward 的**唯一硬前置**，值得单独估一次。
