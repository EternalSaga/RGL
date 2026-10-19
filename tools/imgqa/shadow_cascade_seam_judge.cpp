// shadow_cascade_seam_judge.cpp —— `shadow-cascade-seam` 的**三条判决线**（缝 = 仪器内部，
// 跨 TU 面在 shadow_cascade_seam.h；本文件只出 Verdict，不碰引擎、不读像素）。
//
// 判决方向的完整论证（含「为什么必须差分、单张屏幕空间统计量为什么被证伪」）住
// `shadow_cascade_seam.cpp` 头注；容差理由的**用户可见版**住 `judgments_table_seam.cpp`
// （`imgqa check` 打印的就是它，两边口径必须一致，改一边必须改另一边）。
//
// ## 🔴 J1 今天是红的，而**红是正确的交付内容**
// 本批零引擎改动（任务红线），所以这条红 = 债-104 那个已定死缺陷的图像侧读数第一次被入库判据
// 抓到。为了让这条红**不会被误读成「判据坏了」**，本文件的 observed 文本做了三件事：
//  ① 报出可归因增量并与**固有下界**（纹素推导 0.057）比倍数——「6.6 倍」这句话本身就是
//    「读数远超任何合理容差 = 缺陷在场」的陈述，不是「阈值定太严」的陈述；
//  ② 同时报出参照臂 D=-1 的读数（闭式该为 0.000）——它绿着而 J1 红，恰好说明**仪器在工作**
//    （差分把无关项消掉了），而不是仪器在乱叫；
//  ③ 红文案第一句就写「这条红是预期现状」，并点名修法（交界带内两级混合，债-139 定案）。
//
// ## 变异红线（哪个变异让我红；实跑记录见交付说明）
//   · `render_list_mdi.cppm:284` 的 `c >= 1u` → `c >= 0u`（级 0 也走簇路径 ⟹ 两臂几何重新一致）
//     → **J1 转绿**（方向自检：判据量的确实是 LOD 造成的那一项）。
//   · J1 比较方向翻转（`<=` → `>=`）→ 实机红转绿（读数 0.379 在两侧给出相反判决）。
//   · J1 上限挪到 < 0.379（例如 0.2）→ 仍红（红不是阈值伪影）；挪到 0 → 仍红。
//   · J2 的判据恒真化（`diffPx > 0` → `diffPx >= 0`）→ 本条**变绿**，而它是 J1 的守门人，
//     所以 selftest 侧另钉「两幅同图 → 0 翻转」的算子级断言，两层各守一段。
//   · J3 单调性判据反向（`>=` → `<=`）→ 实机红；把「高段饱和」删掉 → 本批实机仍绿
//     （单调已成立），所以它是**第二条腿**：专抓「D 越大越炸」这种单调但发散的形态。
#include "shadow_cascade_seam.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

/// 倍数文本（J1 的可归因陈述：读数 / 固有下界）。下界是常数且 > 0，不需要 0 除守卫。
std::string ratioOf(double value)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.1f", value / kSeamTexelFloor);
    return buf;
}

}  // namespace

std::string seamFixed3(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

std::string seamFixed4(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

std::string seamPct3(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f%%", 100.0 * v);
    return buf;
}

/// J1（**主判据**）：LOD 归因的缝台阶增量 ≤ kSeamLodGrowthCeil。
void pushLodGrowthVerdict(std::vector<Verdict>& vs, const SeamReadout& r)
{
    const double growth = r.stepLod.step - r.stepRef.step;
    const bool ok = growth <= kSeamLodGrowthCeil;
    vs.push_back(verdictFor("shadow-seam-lod-growth", ok,
        "LOD 归因的缝台阶增量 = maxRowStep(Δ|D=5) − maxRowStep(Δ|D=-1) = "
            + seamFixed3(r.stepLod.step) + " − " + seamFixed3(r.stepRef.step) + " = "
            + seamFixed3(growth) + "，上限 " + seamFixed3(kSeamLodGrowthCeil) + "\n"
            + "  · D=5 臂：台阶 " + seamFixed3(r.stepLod.step) + " @ 行 "
            + std::to_string(r.stepLod.row) + "，c0↔c1 翻转率 " + seamPct3(r.flipLod) + "\n"
            + "  · D=-1 臂（几何一致参照，闭式该为 0.000）：台阶 "
            + seamFixed3(r.stepRef.step) + "，翻转率 " + seamPct3(r.flipRef)
            + "\n  · 🔴 **LOD 归因 " + seamFixed3(growth) + " = 固有下界 "
            + seamFixed3(kSeamTexelFloor) + " 的 " + ratioOf(growth) + " 倍**（下界 = 相邻级"
              "纹素粗细差折成的一个纹素影边摆放误差，推导见容差理由）"
            + (ok ? ""
                  : "\n  · 这条红 = **LOD 正在造一条肉眼可见的级联交界缝**（不是判据坏了）："
                    "参照臂台阶 " + seamFixed3(r.stepRef.step) + " 说明差分仪器工作正常"
                    "（几何一致时相邻级本就逐位一致），而 LOD 臂在同一位置长出 "
                + seamFixed3(growth) + " 的台阶 = 全部增量都归因到「c1..c3 画粗簇 LOD、c0 画"
                  "全量」这一件事（`render_list_mdi.cppm:284` 的 `c >= 1u`）。\n"
                    "    本批**零引擎改动**，所以这条红是债-104 那个已定死缺陷的图像侧读数"
                    "**首次入库**——红是预期现状、是交付内容。修法 = 交界带内两级混合"
                    "（债-139 定案），**不是**把上限放宽：放宽等于把这条缝重新藏回屏幕空间。")));
}

/// J2（**阳性锚点，防恒绿**）：D=5 的 force=0 与 force=1 两臂不得逐位相同。
void pushArmsAliveVerdict(std::vector<Verdict>& vs, const SeamReadout& r)
{
    const bool ok = r.diffPxLod > 0;
    vs.push_back(verdictFor("shadow-cascade-seam-arms-alive", ok,
        "D=5 的 force=0 臂与 force=1 臂逐位不同的像素数 = "
            + std::to_string(static_cast<long long>(r.diffPxLod))
            + "（布尔判据：> 0 即绿，**没有阈值可谈**——闭式两态 = 相同 / 不同）"
            + "；本条红时 **J1 的绿一律不作数**"
            + (ok ? ""
                  : "\n  这条红 = 差分仪器没接线：两臂塌成同一张图 ⟹ Δ 场恒 0 ⟹ J1 会"
                    "**恒绿**（恒绿比红危险得多，这正是本锚点存在的理由）。逐条查："
                    "① 两臂的 `csm.force_cascade=` 回显行是否各自存在（拼错键 = 静默拿"
                    "出厂值 -1，两臂同图）；② 级联是否只剩一级（`forceShadowCascadeSplits` "
                    "对 force >= 生效级数**原样返回** = 等同关，两臂同图）；③ shadow.lua 的 "
                    "csm 块是否被后面的门整值盖掉（第 4 例外门会重写 shadow.lua）。\n"
                    "    ⚠ 本条**只在 D=5 上判**：参照臂 D=-1 下两 force 臂本就该可能逐位"
                    "相同（几何一致 ⟹ 相邻级对同一表面的遮挡一致），那是正常不是坏。")));
}

namespace {

/// J3 的形状分析（从 pushSweepVerdict 拆出，守认知复杂度阈值；语义与打印一字未动）。
struct SeamSweepShape {
    bool mono = true;        ///< 随 D 单调不减
    bool saturates = true;   ///< 高段饱和（末段增量 ≤ 首段增量）
    double first = 0.0;      ///< 首段增量
    double last = 0.0;       ///< 末段增量
    std::string force0Bad;   ///< force=0 反向对照的坏点（空 = 全部恰 0）
};

SeamSweepShape analyzeSweep(const std::vector<SeamSweepPoint>& s)
{
    SeamSweepShape h;
    // 反向对照：force=0 时全屏采 c0，而 c0 **恒旧路径**（`c >= 1u`）⟹ D 这个自变量根本不被
    // 消费 ⟹ 翻转率闭式 = 0。它非 0 = 诊断键或 LOD 接线漏到了级 0（比 J1 更基本的坏）。
    for (const auto& p : s)
        if (p.flipForce0 != 0.0)
            h.force0Bad += (h.force0Bad.empty() ? std::string() : std::string(", "))
                + "D=" + std::to_string(p.lod) + " " + seamPct3(p.flipForce0);
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i].flipForce1 < s[i - 1].flipForce1)
            h.mono = false;
    // 高段饱和：最后一段增量 ≤ 第一段增量（曲线在顶部**不加速**）。抓的是「单调但发散」——
    // 只钉单调的话，「D 越大越炸」（例如 D 超过自身最深级时回落成画 L0）照样绿。
    h.first = s[1].flipForce1 - s[0].flipForce1;
    h.last = s.back().flipForce1 - s[s.size() - 2].flipForce1;
    h.saturates = h.last <= h.first;
    return h;
}

}  // namespace

/// J3（债-110 原规格 判据 3②）：各 D 相对 D=-1 的翻转率随 D 单调不减且高段饱和。
void pushSweepVerdict(std::vector<Verdict>& vs, const SeamReadout& r)
{
    // D 点不足 3 个 ⟹ 「单调」与「饱和」两条都无从谈起（两点只能连一条线）。**抛**不判：
    // 出一句「绿」会把「没扫够档」读成「扫过了没问题」（同 shadow-grain 的死缝闸门）。
    if (r.sweep.size() < 3)
        throw std::runtime_error("shadow-seam-sweep-monotone：D 扫描只有 "
            + std::to_string(r.sweep.size()) + " 个点（< 3），单调性与饱和都不可判，"
              "拒绝出这条判据——要么补扫描档（shadow_cascade_seam.h 的 kSeamSweepD），"
              "要么就地删掉本判据并在交付说明里写明，不许静默给绿");

    const SeamSweepShape h = analyzeSweep(r.sweep);
    std::string table;
    for (size_t i = 0; i < r.sweep.size(); ++i)
        table += (i == 0 ? std::string("  ") : std::string(" → "))
            + std::to_string(r.sweep[i].lod) + ":" + seamPct3(r.sweep[i].flipForce1);

    std::string bad;
    if (!h.mono)
        bad += "\n  · 单调性破：某档 D 的翻转率**低于**前一档 ⟹ 该档比更粗的档还接近全量几何 "
               "= 选级区间错位 / 该档漏画被后一档补回来。";
    if (!h.saturates)
        bad += "\n  · 饱和性破：末段增量 " + seamPct3(h.last) + " > 首段增量 "
               + seamPct3(h.first)
               + " ⟹ 曲线在高段**加速发散**，而成本表在高段是收敛的（D=7 已只占 L0 三角 3.3%）"
                 "⟹ 高 D 档画的东西与「轮廓误差」无关，先查回落策略（shadow_lod_fallback）。";
    if (!h.force0Bad.empty())
        bad += "\n  · force=0 反向对照破（D 本该不被消费）：" + h.force0Bad
               + " ⟹ 级 0 被 LOD 影响 = `render_list_mdi.cppm` 的 `c >= 1u` 不再成立，"
                 "差分仪器的「唯一差别 = 采哪张图」前提塌了。";
    const bool ok = h.mono && h.saturates && h.force0Bad.empty();
    vs.push_back(verdictFor("shadow-seam-sweep-monotone", ok,
        "D 扫描（相对 D=-1 参照的 c0↔c1 翻转率，force=1，全幅）：\n  " + table
            + "\n  两条形状判据：单调不减 = " + std::string(h.mono ? "✓" : "✗")
            + "；高段饱和（末段增量 " + seamPct3(h.last) + " ≤ 首段增量 " + seamPct3(h.first)
            + "）= " + std::string(h.saturates ? "✓" : "✗")
            + "\n  反向对照 force=0（c0 恒旧路径 ⟹ D 不被消费 ⟹ 闭式 0）："
            + (h.force0Bad.empty() ? std::string("全部 D 恰 0 ✓") : h.force0Bad)
            + (ok ? std::string()
                  : "\n  这条红 = D 扫描的形状不对，先查选级/回落而不是先查阴影质量：" + bad)));
}

}  // namespace RGL::imgqa
