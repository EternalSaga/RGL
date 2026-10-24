// 判据契约表 · **透明段像素贡献段**（`blend-lit-*`，6 条）。
//
// 从 `judgments_table.cpp` 另立 TU 的理由同 `judgments_table_blend.cpp`：文件代码行护栏把
// 「六条带完整容差理由的条目」装不进任何现成函数，而缝按**仪器**切——本文件六条全部属于
// `imgqa blend-lit` 这一台仪器，成立条件共享「blend_lit 双臂同源输入 + 引擎回显条数 +
// footprint 分桶对账」。族仍是 **Foundation**：与机位运动无关、无时域比值口径。
// ⚠ 拼接顺序只在 `judgments_table.cpp` 的 registeredJudgments()，本文件不抄条数。
//
// ## 为什么是六条而不是一条「图变了」
// 债-134 记的是「透明段整段删掉也不会让任何检查红」。要把它变成会红，五条独立的门缺一不可：
//   条数非 0（画了）· 反向臂条数恰 0（那条数不是恒真的）· footprint 内两臂变了（真写进像素）·
//   footprint 外两臂逐位不变（写的就是它该写的那块）· 同臂内 footprint 众数 ≠ 背景板众数
//   （写的东西看得见，**不是**「两臂恰好都不画所以不一样」）。第六条 `[error]=0` 管过程干净。
// 少了「反向臂恰 0」：条数读数可以是「任何场景都非 0」的常数；
// 少了「之外逐位不变」：像素差可以来自任何旁路——**阴影那条旁路就是在这条的压力下被发现的**
// （collectShadowCasters 排 blend ⟹ 开着阴影时 OPAQUE 臂会投影，透明段一条不画两臂也不同）；
// 少了「同臂众数不同色」：**删掉透明段时「两臂不同」照旧成立**（BLEND 臂露出背景板、OPAQUE
// 臂是不透明 pane）——这一条不是想到才写的，是变异跑出来的，逐条正文里给了读数。
//
// ## 本段没有闭式像素值预测，这是刻意的
// 台账（docs/还债总账.md 债-134）裁过：透明光照的闭式要复刻 BRDF + IBL + CSM，成本 > 收益。
// 所以本段六条的容差全是**整数与计数**的容差（条数、像素数、LSB 下限），没有「预测字节 vs
// 实测字节」那一档。⟹ 它证「透明段对交付图有贡献」，**不证「透明画对了」**（后者归 blend-sort
// 的闭式与将来的强判据）。这句必须写在每条正文里，否则六条全绿会被读成「透明是对的」。
#include "judgments_table.h"

namespace RGL::imgqa {

// 拆成两个函数的理由 = 单函数代码行上限（120）：六条带完整容差理由的条目装不进一个函数。
// 切缝按「读引擎回显的条数对」/「读像素与过程的其余四条」分，两半的成立条件各自闭合；
// 🔴 拼接顺序仍然只在 `judgments_table.cpp` 的 registeredJudgments() 一处，本文件不抄条数。
const std::vector<JudgmentInfo>& blendLitDrawCountJudgments()
{
    static const std::vector<JudgmentInfo> kPart = {
        {"blend-lit-draws", JudgmentFamily::Foundation,
            "阈值 **0**（条数是整数，「差不多对」不存在；写成 ±1 等于允许漏一条 draw）。\n"
            "· 期望 = manifest 的 `blendLit.expectTransparentDraws`（当前 3 = 三支 BLEND pane），"
            "由 `tools/qa_scenes.py` 数 `alpha < 1.0` 的 piece 得到——**不在 C++ 里重算**，"
            "否则「场景里有几支透明面」就有了两个答案。\n"
            "· 读数只吃引擎回显（`GET /state` → `render.transparentDraws`）。为什么不许数像素"
            "反推：条数是引擎侧的事实，从像素反推 = 用被测量证明被测量——像素变了只能说明"
            "「有别的东西变了」。\n"
            "· 只认 **frame ≥ 2** 的读数：RPC 请求在帧边界被 drain，而 poll 挂在 system 链"
            "**之前** ⟹ 第 1 次 drain 读到的是装配期 emplace 的默认空表，恒 0，"
            "**不是**「场景没有透明图元」。\n"
            "· 一次都读不到 = **FAIL 而不是 SKIP**（读出口坏了与透明段没画是两件事，"
            "但都不许报绿）。",
            {"rgl/debug_rpc_exec_impl.cppm 的 transparentSpan（口径本体：吃 "
             "segmentSpan(\"transparent\", …) = 执行器画那一段时用的同一个 span，"
             "所以 RPC 报的条数与透明 pass 真录的 draw 数结构上不可能各说各话）",
             "rgl/rpc_schema.lua 的 stateRender.transparentDraws（响应字段的单一来源，"
             "生成物 rpc_schema.json/.md 由 `just reflect` 带出）",
             "本机实测（2026-10-24，AMD Radeon 8060S Graphics）：BLEND 臂 337 次读数覆盖帧 "
             "7…1996，值集恒 {3}，与 manifest 声明一致",
             "主 agent 复核变异（2026-10-24，形态与「硬编码成 4」那条不同）：把读数口从 "
             "segmentSpan(\"transparent\") 指到 segmentSpan(\"opaque\")——一个「看着像对」的错量 "
             "⟹ 本条红（1 ≠ 3），blend-lit-reverse 同时红（4 ≠ 0）。附带收获：回显的 1 / 4 互证"
             "了段划分正确（BLEND 臂 opaque 1 + transparent 3 = OPAQUE 臂 opaque 4）。还原后复绿"}},
        {"blend-lit-reverse", JudgmentFamily::Foundation,
            "阈值 **恰为 0**（不是「比 BLEND 臂小」，也不是「小于某个上限」）。\n"
            "· 反向用例 = 同一份场景的 OPAQUE 臂（`blend_lit_opaque.glb`，三支 pane 只差 "
            "alphaMode）。它必须让条数**恰好**归零。\n"
            "· 为什么必须有这条：没有它，判据 1 的「非 0」可以是「任何场景都非 0」的常数——"
            "比如分桶判据被写成恒真、或 transparentStart 被写成 0。那种坏法下判据 1 永远绿，"
            "而透明段里画的是什么没人知道。这是本项目反复踩的「加了东西反而让判据失效」形态"
            "的反面：**判据必须带一条自己会因同一原因变 0 的对照**。\n"
            "· 两臂的同源性由仪器在跑之前**机器对账**（两臂 manifest 的相机/落位/尺寸/背景板"
            "逐字段全等，不一致即拒跑），不是靠「它们是一个函数生成的」这句注释。",
            {"tools/qa_scenes.py 的 scene_blend_lit(blend=)（双臂由同一个函数构造，"
             "只差 alpha；同源是构造性的）",
             "本机实测（2026-10-24）：OPAQUE 臂 335 次读数覆盖帧 2…1999，值集恒 {0}",
             "blend_lit.cpp 的 requireSameSourceArms（两臂 manifest 逐字段对账 + "
             "「expectDraws 必须不同且反向臂必须为 0」两条硬检查）"}},
    };
    return kPart;
}

const std::vector<JudgmentInfo>& blendLitPixelJudgments()
{
    static const std::vector<JudgmentInfo> kPart = {
        {"blend-lit-contributes", JudgmentFamily::Foundation,
            "阈值 = **changed > 0 且最大字节差 ≥ 8 LSB**（判别力下限）。本机实测 185008 像素、"
            "165 LSB，余量两个数量级；8 不是量出来的容差，是「低于它这份输入就分不开画与没画」"
            "的下界，不足即**拒跑**（抛），不是判红。\n"
            "· 区域 = 三支 pane 的解析 footprint（投影算子在 `blend_lit_metric.cpp`，"
            "向外取整 + 外扩 1 px 覆盖抗锯齿边缘带），**不在图里找边界**。\n"
            "· 本条只回答「透明段往像素上写了没有」，**不回答写得对不对**——它没有任何闭式"
            "预测（见本文件头注「这是刻意的」）。画对了归 blend-sort。\n"
            "· 为什么阈值不写成「changed > 0」就够：0 与 1 之间隔着噪声。若两臂只差一个 "
            "1 LSB（例如 α 通路接了但混合权重恒 0），「变了」是真的，而它不构成"
            "「透明段有贡献」的证据。",
            {"本机实测（2026-10-24）：footprint(+1px) 内 changed = 185008 / 921600 = 20.07%，"
             "max = 165 LSB（α=0.25 的红 pane：BLEND (119,0,184) vs OPAQUE (226,0,21)）",
             "🔴 变异实测（本条会红的直接证据）：让反向臂也加载 BLEND 的 .glb（两臂渲染同一个"
             "场景）→ 本条红，footprint 内 changed = **0** / 0 LSB（同时 blend-lit-reverse 也红）"
             "——本条不是恒真的",
             "blend_lit_metric.cpp 的 blendLitBoxes（投影 + 向外取整 + footprint 互不重叠的"
             "硬检查；前提不成立一律抛）",
             "tools/qa_scenes.py 的 blend_lit_projection（与 C++ 侧同一条投影的 Python 实现，"
             "selftest 钉两者逐像素一致）"}},
        {"blend-lit-confined", JudgmentFamily::Foundation,
            "阈值 = **恰为 0**（footprint 之外 changed 像素数）。这不是「小」，是零——"
            "两臂在透明段画不到的地方必须逐位相同。\n"
            "· 它是上一条的**归属**那一半：只有「变了」没有「在哪变了」，判据可以在任何旁路上"
            "成立。本条把「双臂之差」钉死在透明段自己的 footprint 里。\n"
            "· 🔴 **这条的存在直接抓出了一条真旁路**：`collectShadowCasters` 排除 blend 图元，"
            "于是开着阴影时 OPAQUE 臂的 pane 会投影、BLEND 臂的不投——透明段一条都没画，"
            "「双臂像素不同」照样成立。处置 = 冻结配置把 shadow.mode 钉成 "
              "`\"none\"`（结构性摘掉旁路，不是加容差豁免它）。\n"
            "· 前提 = 背景板铺满整屏（仪器跑之前对账，铺不满即拒跑）：否则「之外」里还有"
            "天空盒/清屏色，本条测的就不是「透明段没漏」。\n"
            "· 与 `just regress` 的逐位红线同源但**不是**同一件事：那边比的是同一条路改前改后，"
            "本条比的是同一次运行里的两个臂。",
            {"本机实测（2026-10-24）：footprint(+1px) 之外 changed = **0**（背景板逐位相同），"
             "而之内 changed = 185008 —— 两桶之和恰等于总 changed，没有第三类",
             "🔴 变异实测（本条会红的直接证据）：把 qa/configs/blend_lit/shadow.lua 的 mode 在"
             "**副本**里改成 pcss → 本条红，footprint 之外 changed = **49371** 像素 / 119 LSB，"
             "而其余五条判据全部照绿（条数仍 3/0、之内仍 187212 像素不同）——"
             "「透明段一条没多画而两臂已经不一样」是实测事实，不是假想",
             "rgl/main_meshlet_tables.cppm:283（collectShadowCasters 的第二条判据 = 非 blend，"
             "本条正文引用的那条旁路的事实依据）"}},
        {"blend-lit-visible", JudgmentFamily::Foundation,
            "阈值 = **≥ 8 LSB**（与判据 3 同一把判别力下限，同一个理由：低于它这份输入就"
            "分不开「画了」与「没画」）。\n"
            "· 口径 = **同臂自比**：BLEND 臂某支 pane 的 footprint 内**众数颜色** vs 同一张图"
            "footprint 外的众数（= 背景板）。不跨臂、不预测任何像素值。\n"
            "· 为什么是众数而不是均值：本条问的是「这块像素**是不是**背景板那个颜色」，那是"
            "二值问题；均值会被边界像素与光照梯度拖走，众数在「整块都是背景板」时**恰好**差 0。\n"
            "· 🔴 **变异实测（本条存在的全部理由）**：把 `rgl/render_list_impl.cppm` 里"
            "「透明段 append 进 calls」那个循环删掉（= 透明段整段为空，正是债-134 记的那个"
            "状态），判据 1 红，但判据 3 **照旧绿**——透明段没了之后 BLEND 臂在 footprint 里"
            "露出背景板，而 OPAQUE 臂在那儿是不透明的 pane，两臂**还是不同**（实测 changed 仍是 "
            "185008 像素，只是 max 从 165 变 226）。⟹ `contributes` 单独担不起「透明段画了」"
            "这句话。本条把这一洞补在**像素侧**，意义 = 万一引擎侧那条读数口坏了，"
            "像素侧仍然不是恒真。\n"
            "· 本条与判据 3 的分工：3 比「两臂之间」，6 比「同一臂之内」。两条都绿才是"
            "「透明段在自己的 footprint 上写了、写的东西与背景板不同」。",
            {"变异实测（2026-10-24，本机）：删掉透明段 append 循环 → blend-lit-draws 红"
             "（0 ≠ 3）、blend-lit-contributes **仍绿**（185008 像素 / 226 LSB）——本条就是"
             "为这个缺口立的；还原后复绿",
             "主 agent 复核变异（2026-10-24，形态与上面那条不同）：把 BLEND 臂三支 pane 的 α 从 "
             "0.25/0.5 压到 0.002（透明段照旧 3 条 draw、两臂照旧差 185008 像素）⟹ 本条红"
             "（Δ = 0 / 2 / 4 LSB）而判据 1 与 3 全绿 —— 本条是唯一咬住「有 draw、两臂不同、"
             "但透明面对交付图没有贡献」的那条。顺带一条事实：`tools/qa_scenes.py` 的 Piece "
             "守卫直接拒 α = 0（`alpha 0.0 不在 (0,1]`）⟹ 这个错形只能以「极小 α」存在，"
             "而它恰好落在 8 LSB 判别力下限之下",
             "blend_lit_metric.cpp 的 blendLitDominant（众数算子；空区域抛而不是返回默认色"
             "——「区域算错了」不许伪装成一条正常读数）",
             "selftest_blend_lit.cpp 的第 ⑦ 组（合成图上「pane 与背景板同色」必须让本条的"
             "读数恰为 0，「不同色」必须 ≥ 一个量级——不启引擎钉这条判据自己会红）"}},
        {"blend-lit-clean", JudgmentFamily::Foundation,
            "阈值 = **0 行** `[error]`（两次启动之和）。口径与 `tools/smoke.py` 的 "
            "count_severities 逐字相同：子串匹配 `\"[error]\"`；severity 映射见 "
            "`rgl/utils.cppm` 的 vulkanDebugCallback——引擎自身 error 与 VVL 的 VUID 都落这一级。\n"
            "· 为什么透明段特别需要它：透明是这条管线上第一次让「同一张 color attachment 被"
            "同一条 pass 以 blend 状态反复写」进入交付路径，而 barrier / layout / blend 状态"
            "三样里任何一样写错，VVL 都会报而画面可能照旧好看。\n"
            "· 读不到日志 = **抛**而不是报 0：「计数 = 0」不能建立在「没数」上。\n"
            "· 本条不覆盖 `[warning]`（交付配置本来就有若干条），也不覆盖 VVL 之外的正确性。",
            {"tools/smoke.py 的 count_severities（同一口径的 Python 侧实现，跨语言无法共享"
             "实现，故两处注释互指——改一处必须改另一处）",
             "rgl/utils.cppm 的 vulkanDebugCallback（severity 映射的真相源：什么落 [error]）",
             "本机实测（2026-10-24）：两臂各一次启动，[error] = 0 + 0（`just smoke` 对同一份"
             "冻结配置独立读数亦为 errors=0 warnings=0）"}},
    };
    return kPart;
}

}  // namespace RGL::imgqa
