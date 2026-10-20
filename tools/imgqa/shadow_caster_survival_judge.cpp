// shadow_caster_survival_judge.cpp —— `shadow-caster-survival` 的**判决线**（仪器内部，
// 跨 TU 面在 shadow_caster_survival.h；本文件只出打印与 Verdict，不碰引擎、不读像素）。
//
// 判决方向的完整论证（含「为什么必须是绝对量、seam 差分判据为什么对这类缺陷结构失明」）住
// `shadow_caster_survival.cpp` 头注；容差理由的**用户可见版**住 `judgments_table_survival.cpp`
// （`imgqa check` 打印的就是它，两边口径必须一致，改一边必须改另一边）。
//
// ## 🔴 J1 本批**只报数不判**（INFO，不出 PASS/FAIL、不进 rc）——这是设计，不是偷懒
// 这台仪器今天才建。拿一个没推导过的数当门 = 把「实测没超所以钉个数」写进判据网，正是本仓
// 最恨的形态（AGENTS 哲学第 1 条的镜像：看起来已完工的半吊子）。阈值必须从扫描表 + 债-139
// Y 实验的三档归因推导出来（丢失块 237–557px / 覆盖度 0.62–0.78 是**缺陷态**读数，正常态
// 下限要钉在哪，得先有缺陷态与正常态两边的分布）。**本批的交付物是扫描表 + 四条闸门，
// 不是一条绿的判据。**
//
// ## 变异红线（哪个变异让我红；实跑记录见交付说明）
//   · J2 单调性判据 `cur > prev` 反向成 `cur < prev` → 实机判决翻转（增序列判绿、降序列判红）。
//   · J2 的「D 点 < 3 抛」删掉 → selftest「两点应抛」红（两点连一条线，单调无从谈起；
//     出一句绿会把「没扫够档」读成「扫过了没问题」，同 shadow-grain 死缝闸门）。
//   · J2 判据恒真化（`cur > prev` → `false`）→ selftest「升序应红」红。
//   · J1 的 INFO 改成 verdictFor 出 PASS → 挂号自检抓不到（名字都在表里），但台账会多出一条
//     **没有阈值背书的 PASS**——这正是「阈值未定期不许出判据」要防的事，实机自证 E4 时核对
//     打印里 J1 只有 INFO 行、Verdict 区只有 J2 一条。
#include "shadow_caster_survival.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {

std::string survFixed3(double v)
{
    if (std::isnan(v))
        return "—";  // 空档 = 无数据，打印成破折号而不是 1.000（把没数读成满分是本案大忌）
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

std::string survPct3(double v)
{
    if (std::isnan(v))
        return "—";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f%%", 100.0 * v);
    return buf;
}

std::string survBucketCell(double survival, std::int64_t alive, std::int64_t total)
{
    char buf[64];
    if (std::isnan(survival))
        std::snprintf(buf, sizeof buf, "— (%lld/%lld)", static_cast<long long>(alive),
            static_cast<long long>(total));
    else
        std::snprintf(buf, sizeof buf, "%.3f (%lld/%lld)", survival,
            static_cast<long long>(alive), static_cast<long long>(total));
    return buf;
}

/// J1（**阈值未定期：只报数不判**）：出厂档 D=5 的小块（<50px）存活率与近场接触阴影存活率。
void printFactoryInfo(const SurvCameraReadout& r)
{
    const size_t row = r.rowOf(kSurvFactoryD);
    const UmbraBlobStats& full = r.fullStats[row];
    const UmbraBlobStats& near = r.nearStats[row];
    std::printf("\n  [INFO] shadow-caster-survival-factory @ %s（出厂档 D=%d，**只报数不判**）：\n",
        r.camera.c_str(), kSurvFactoryD);
    std::printf("    · 全幅小块（<50px）存活率   = %s\n", survBucketCell(
        bucketSurvival(full, 0), full.bucketAlive[0], full.bucketTotal[0]).c_str());
    std::printf("    · 近场接触阴影存活率（<50px，行 [%d,%d]） = %s\n", kSurvNearRow0,
        kSurvNearRow0 + kSurvNearRows - 1,
        survBucketCell(bucketSurvival(near, 0), near.bucketAlive[0], near.bucketTotal[0]).c_str());
    std::printf("    🔴 TODO(债-139)：两条下限**阈值未定**——本仪器今天才建，阈值必须从这张扫描表\n"
                "       与 Y 实验（四级全 LOD 的缺陷态读数：丢失块 237–557px / 覆盖度 0.62–0.78）\n"
                "       两边分布推导，不是拿今天的读数倒钉。未定期：本行只出 INFO，不出 PASS/FAIL，\n"
                "       不进 rc；定阈值后把本函数改成 pushFactoryVerdict 并同步挂号表容差理由。\n");
}

namespace {

/// 一台机位的单调性检查（从 pushMonotoneVerdict 拆出，守认知复杂度阈值）。
/// 返回文本 = 该机的曲线 + 破点（空 = 单调不增成立）。
std::string monotoneCurve(const SurvCameraReadout& r, bool& okOut)
{
    // 参照行（D=-1）不是候选，不进曲线（它的面积存活率是与自己比的恒等 1.000，
    // 把它当曲线起点 = 把「候选都略 > 1（轮廓胖）」判成非单调——那是把恒等式当读数）。
    std::vector<std::pair<int, double>> pts;
    for (size_t i = 0; i < r.lods.size(); ++i)
        if (r.lods[i] != kSurvRefD)
            pts.emplace_back(r.lods[i], r.areaSurvival[i]);
    // D 点不足 3 个 ⟹ 「单调」无从谈起（两点只能连一条线）。**抛**不判：出一句「绿」会把
    // 「没扫够档」读成「扫过了没问题」（同 shadow-seam J3 的口径）。
    if (pts.size() < 3)
        throw std::runtime_error("shadow-caster-survival-monotone：机位 " + r.camera
            + " 的 D 扫描只有 " + std::to_string(pts.size()) + " 个点（< 3），单调性不可判，"
              "拒绝出这条判据——要么补扫描档（shadow_caster_survival.h 的 kSurvSweepD），"
              "要么就地删掉本判据并在交付说明里写明，不许静默给绿");
    std::string curve;
    std::string breaks;
    for (size_t i = 0; i < pts.size(); ++i) {
        curve += (i == 0 ? std::string("  ") : std::string(" → "));
        curve += std::to_string(pts[i].first) + ":" + survFixed3(pts[i].second);
        if (i > 0 && pts[i].second > pts[i - 1].second)
            breaks += (breaks.empty() ? std::string("；破点：") : std::string("、"))
                + "D=" + std::to_string(pts[i].first) + " " + survFixed3(pts[i].second) + " > 前一档 "
                + survFixed3(pts[i - 1].second);
    }
    okOut = breaks.empty();
    return curve + (breaks.empty() ? "（单调不增 ✓）" : "🔴 单调性破" + breaks);
}

}  // namespace

/// J2：本影面积存活率随 D **单调不增**（每台机位各自判，全过才绿）。
void pushMonotoneVerdict(std::vector<Verdict>& vs, const std::vector<SurvCameraReadout>& cams)
{
    bool ok = !cams.empty();
    std::string observed = cams.empty()
        ? std::string("一台机位的读数都没拿到（空表——空清单平凡通过 = 闸门看起来在其实没查，"
                      "本判据直接抛，不出一句绿）")
        : std::string("本影面积存活率（参照 = D=-1 全量几何）随 D 的曲线，逐机位：\n");
    for (const auto& r : cams) {
        bool oneOk = false;
        observed += "  " + r.camera + ":\n" + monotoneCurve(r, oneOk) + "\n";
        ok = ok && oneOk;
    }
    vs.push_back(verdictFor("shadow-caster-survival-monotone", ok,
        observed
            + "  判据 = 随 D 单调不增（D 越大越粗 ⟹ 遮挡只会丢不会多）。抓的形态：**区间错位 /\n"
              "  选级表漂了**——某档比更粗的档还粗（存活率跳升）= 该档选级区间错位或整档漏画。\n"
              "  ⚠ 读数**允许 > 1**（LOD 轮廓略胖是实测事实：主 agent 实测 D=5 面积 = 参照的\n"
              "  100.5%），本条钉的是**形状**不是「≤ 1」；等档并列（逐位相同的两臂）不算破。"
            + (ok ? std::string()
                  : std::string("\n  这条红 = 扫描曲线有上升段，先查选级表/回落策略（shadow_lod_fallback），"
                                "而不是先查阴影质量。"))));
}

}  // namespace RGL::imgqa
