// 判据契约表 · **结构差分（湿面/SSR 反射结构对比度）段**（`diffstruct-*`，2 条）。
//
// 从 `judgments_table.cpp` 另立 TU 的理由与 `judgments_table_shadow.cpp` 同一条：主表撞
// 「单文件 < 500 代码行」护栏，而缝按**仪器**切——本文件两条全部属于 `imgqa diffstruct`
// 这一台算子（Phase 15 批 15.2 子步 D1 把一次性脚本 `/tmp/opencode/ssr_subtract_check.py`
// 升成常驻算子时登记）。族仍是 **Foundation**：它不涉时域比值口径（两张静态图，无 jitter、
// 无机位运动），成立条件 = 「减法这一项在不在 + 漏没漏出影响域」。
// ⚠ 拼接顺序只在 `judgments_table.cpp` 的 registeredJudgments()，本文件不抄条数。
//
// 两条的分工（缺一条就退化成假绿）：`-anchor` 管「减法生效没有」，`-domain` 管「生效的地方
// 是不是只在该生效的地方」。前者单独绿可以是「减法漏到干面」——那正是后者存在的理由。
#include "judgments_table.h"

namespace RGL::imgqa {

const std::vector<JudgmentInfo>& ssrDiffStructJudgments()
{
    static const std::vector<JudgmentInfo> kPart = {
        {"diffstruct-dark-anchor", JudgmentFamily::Foundation,
            "线 = **> 0**（只在 `--expect-dark` 在场时求值；不给该旗标 ⟹ 变暗计数只作报告值）。\n"
            "· **缺陷态 = 恰 0**，不是「变小」：把 `ssr.frag.slang` 的 "
            "`weight·(F·reflected − iblSpec)` 改回只加 `+ weight·F·reflected` 后，5835 px 差分"
            "**全部变亮**、变暗计数从 4221 掉到 **0**（§3.3 子步 C 变异自验行）。缺陷态落在"
            "**开区间端点之外**，所以 `> 0` 已经是分得开两态的判据，不需要再编一个下限。\n"
            "· **为什么不设下限**（现状 4221，看着很可以取个 1000）：本条判的是「减法这一项在"
            "不在场」，不是「减得够不够」。设了下限，参数扫描把湿面强度调小就会把一条**仍然"
            "生效**的减法读成失效——那是把画质结论混进机制结论。「够不够」由 p90/p10 报告值与"
            "参数扫描表负责（子步 D2），不在本条。\n"
            "· **为什么 `> 0` 不会被噪声刷绿**：SSR-on 图确有 ~400–600 px 的进程级 run-to-run "
            "漂移（§3.3 子步 C 登记，根因 = DDA 命中边界翻转），但漂移是**同档连拍**的量，而本条"
            "数的是「变暗」这一**方向**——只加形态下差分集里连一个负号都没有，漂移再大也造不出"
            "方向性。反向自证：同图自比（A=B）差分集合为空 ⟹ 计数恒 0。\n"
            "· 与 `-domain` 互锁：本条绿而 `-domain` 红 = 减法在但漏出湿区，**不许**拿本条的绿"
            "说「湿面生效了」；两条同绿才是「减法在且只在湿区」。",
            {"变异红线：`rgl/shaders/ssr.frag.slang` 的置信度混合改回「只加」→ 变暗计数 4221 → "
             "恰 0（docs/phase15_plan.md §3.3 子步 C 变异自验行）",
             "docs/phase15_plan.md §2 批次表 15.2 行 = 判据本体（2026-10-10 用户拍板：「湿区内 "
             "Δ<0（变暗）像素占比 > 0」是「命中处减 IBL specular」生效的阳性锚点）",
             "docs/phase15_plan.md §1.1 表「差分中变暗的像素 0」= 15.1 时代（只加形态）的缺陷态"
             "读数，与本条的 0 是同一件事的两个来源",
             "tools/imgqa/selftest.cpp：注入 N 个变暗像素 → 计数 == N（反向：全非负 → 恰 0）"}},
        {"diffstruct-dark-domain", JudgmentFamily::Foundation,
            "线 = **恰 0**（只在 `--gbuf` 在场时求值），量 = 「变暗 ∧ roughness ≥ gate」的像素数。"
            "影响域判据只认这一种形态：**给容差 = 允许漏**（同 `switch-impact-domain` 的「域外"
            "恰为 0」，那条的教训是容差一开就没人再看得出漏了多少）。\n"
            "· **现状 = 0**，且不是贴着线过的：变暗像素的 roughness 分布 min 0.051 / p50 0.337 / "
            "**max 0.349**，与 gate = 0.40 之间隔着 0.051 ≈ **13 个 UNORM8 量化台阶**的空档"
            "（§3.3 子步 C 影响域交叉行）。资产 roughness 或 gate 任一小幅漂移都碰不到这条线，"
            "而「减法漏到干面」是整片漏、不是漏 13 个台阶。\n"
            "· **gate = 0.40 的出处**：出厂资产 roughness 双峰（水洼 0.05 / 湿石 0.35 一簇，"
            "≥ 0.74 的干面一簇，§3.2 表），0.40 落在两簇之间的空档里 ⟹ 「过门控」= 「身份已经是"
            "湿面」，与湿面身份的门控**同源**（不是另找一个阈值）。旁证：湿面 B 落地时交付图差分"
            "13620 px 与「过门控光滑面 13620 px」是**恰同一集合**（§3.3 表）。\n"
            "· 分母口径（本条不判分母，但读数离开分母会被误读，故写死）：过门控像素 = 全图 "
            "`0 < rough < gate`，**g=0 的天空/背景排除在外**——写成 `rough < gate` 会把整片天空"
            "算进分母，1.48% 的湿区被稀释成 0.02% 量级，「漏了一点」就真的看不见了。\n"
            "· 尺寸不符（gbuf 与 A/B 不是同一次采集）在算子层**直接抛**，不进入本条求值。",
            {"docs/phase15_plan.md §3.3 子步 C 影响域交叉行：roughness ≥ gate.y=0.40 上变暗 = 0，"
             "变暗像素 roughness max = 0.349（全部落在过门控光滑面内 = 水洼 0.05 / 湿石 0.35 两簇）",
             "docs/phase15_plan.md §3.2/§3.3 影响域行：roughness ≥ 0.40 的像素上 gbuf 与交付图"
             "差分均 = 0，且交付图差分 13620 px 与过门控 13620 px 恰同一集合（门控同源的旁证）",
             "口径反向用例（tools/imgqa/selftest.cpp）：合成 gbuf 把一个 rough ≥ gate 的像素做成"
             "变暗 → 本条计数 = 1 且 gatePixels 仍按**全图** `0<rough<gate` 计数（挪进差分集就漏）"}},
    };
    return kPart;
}

}  // namespace RGL::imgqa
