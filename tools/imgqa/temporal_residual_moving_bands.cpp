// 批 13.9：**移动机位**的亮度 × 梯度分带 —— 四数出表 + 三分支判决（两把尺子共用）。
//
//   从 `temporal_residual.cpp` 拆出（文件护栏 500 代码行），但**仍与静止态共用同一批算子**：
//   `bandTableOf` / `bandClipProxyByte` / `sobelAbsGrad` 与两根轴的阈值常量只有**一份**，
//   出表与判决也必须只有一份。两把尺子（`temporal-homography` 的单应 warp 残差、
//   `temporal-motion` 的对超采样参考偏差差）喂进来的只是**被分带的图**不同，
//   分档、四数、判决线逐字相同 ⟹ 两张表能并读。
//
//   这六个函数在 `RGL::imgqa` 作用域（不是匿名空间）且已在 `temporal_residual.h` 声明——
//   它们被那两个命令的 TU 调用。

#include "temporal_residual_metric.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

// ═══════════════════════════════════════════════════════════════════════════════════
// 批 13.9：**移动机位**的亮度 × 梯度分带 —— 四数出表 + 三分支判决（两把尺子共用）
//
// 为什么住在这里而不是各自 .cpp：`bandTableOf` / `bandClipProxyByte` / `sobelAbsGrad` 与
// 两根轴的阈值常量只有**一份**，出表与判决也必须只有一份。两把尺子（`temporal-homography`
// 的单应 warp 残差、`temporal-motion` 的对超采样参考偏差差）喂进来的只是**被分带的图**不同，
// 分档、四数、判决线逐字相同 ⟹ 两张表能并读。
//
// ⚠ 与静止态（批 13.7c）的口径差别只有一条，但是**本次任务的全部动机**：判据主数从
// 「rate 比」换成**幅度份额**。`frac>τ` 是不加权像素计数，平坦中间调摆 1 LSB 与高光摆一大截
// 各算一票，而高光是对比度最高处、同样绝对摆动最看得见 ⟹「我一直看到高亮在闪」与
// 「高亮只占残余 0.5%」可同时为真。故每格四数**缺一不可**，只出计数份额 = 未完成。
// ═══════════════════════════════════════════════════════════════════════════════════

namespace {
std::string pct2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}
}  // namespace

BandShare bandShareOf(const BandTable& t, int lb, int gb)
{
    BandShare s;
    const BandCell& c = t.cell[lb][gb];
    s.pixels = c.pixels;
    s.over = c.over;
    s.mass = c.mass;
    if (t.total > 0)
        s.share = static_cast<double>(c.pixels) / static_cast<double>(t.total);
    if (c.pixels > 0)
        s.rate = static_cast<double>(c.over) / static_cast<double>(c.pixels);
    if (t.over > 0)
        s.cntShare = static_cast<double>(c.over) / static_cast<double>(t.over);
    if (t.mass > 0.0)
        s.ampShare = c.mass / t.mass;
    return s;
}

MovingBandBranch classifyMovingBand(double ampShareB, double cntShareB, bool excessInB)
{
    // ⭐ 三分支（**跑前写死**，线不许事后改）。两条线在「ampShareB ≥ 30%」与「两个份额都 < 5%」
    // 上互斥，不存在同时命中的输入；excessInB 只可能把分支从 ②/③ 抬到 ①。
    if (ampShareB >= kMovingSupportAmpShare || excessInB)
        return MovingBandBranch::Support;
    if (ampShareB < kMovingRefuteShare && cntShareB < kMovingRefuteShare && !excessInB)
        return MovingBandBranch::Refute;
    return MovingBandBranch::Indeterminate;
}

const char* movingBandBranchName(MovingBandBranch b)
{
    switch (b) {
    case MovingBandBranch::Support:
        return "分支①「支持」（移动锯齿 = 高光专属）";
    case MovingBandBranch::Refute:
        return "分支②「否证」（移动锯齿不是高光专属）";
    case MovingBandBranch::Indeterminate:
        return "分支③「分不出」";
    }
    return "分支③「分不出」";
}

void printMovingBandVacuity(
    const BandShare& bOff, const std::string& axisName, const std::string& extraNote)
{
    if (bOff.pixels == 0) {
        std::printf("    ⚠⚠ **B 档空集**（近削顶 × 高梯度 = 0 像素）⟹ 上面那句「B 份额 < %s%%」是"
                    "**空洞成立**：本尺子的亮度轴（%s）上**没有任何**近削顶像素，假设的元凶**无处落子**。"
                    "这条**不能**读成「高光不闪」，只能读成「**本尺子看不见高光**」。\n",
            pct2(kMovingRefuteShare * 100.0).c_str(), axisName.c_str());
        if (!extraNote.empty())
            std::printf("       为什么空：%s\n", extraNote.c_str());
        return;
    }
    if (bOff.share < kBandTinyAreaShare) {
        std::printf("    ⚠ B 档**近空集**（面积份额 %s%% = %lld px）⟹ 份额数字建立在极小样本上："
                    "**方向可信、具体数值不稳**。且「近削顶只占这么点屏幕」本身就是结论的一部分"
                    "——它不是「高光不闪」，是「高光在这里几乎没有面积」。\n",
            pct2(bOff.share * 100.0).c_str(), static_cast<long long>(bOff.pixels));
        if (!extraNote.empty())
            std::printf("       补充：%s\n", extraNote.c_str());
        return;
    }
    std::printf("    B 档面积 %s%%（%lld px）= 非空集，「B 份额低」是**有像素支撑**的否证，"
                "不是空洞成立\n",
        pct2(bOff.share * 100.0).c_str(), static_cast<long long>(bOff.pixels));
}

void printIndeterminate(const std::string& target, const MovingGuards& g)
{
    const double explain = g.residAt1 > 0.0 ? g.residAt0 / g.residAt1 : 0.0;
    std::printf("\n══════════════════════════════════════════════════════════════════════════\n");
    std::printf("  ⛔ INDETERMINATE —— 「%s」移动态本次**不出结论**（守卫未过，读数无解释力）\n",
        target.c_str());
    std::printf("  ────────────────────────────────────────────────────────────────────────────\n");
    std::printf("  缺的前提：%s\n", g.why.empty() ? "（未填，属工具缺陷）" : g.why.c_str());
    std::printf("\n  守卫实测：\n");
    std::printf("    0)  采集活着吗：显示域均值 %.4f/255（要求 ≥ %.2f）｜相邻帧最大字节差 %.4f（要求 ≥ %.2f）%s\n",
        g.lumaMean, kMovingGuardMinLuma, g.byteMaxAdjacent, kMovingGuardMinMotion,
        (g.lumaMean < 0.0 || g.lumaMean >= kMovingGuardMinLuma) && g.byteMaxAdjacent >= kMovingGuardMinMotion
            ? "✅"
            : "❌");
    std::printf("        含义：全黑 / 没运动的画面**不是**「没有闪烁」，是**没有画面**——没有可归因的像素。\n");
    std::printf("              A/B 口径（如 `env.intensity=0`）打在只吃天光的机位上就会走到这里。\n");
    std::printf("    1a) 对齐消化率 α=0/α=1 = %.4f/%.4f = %.4f  （要求 ≥ %.2f）%s\n", g.residAt0,
        g.residAt1, explain, kMovingGuardExplainRatio,
        explain >= kMovingGuardExplainRatio ? "✅" : "❌");
    std::printf("        含义：不 warp 的帧差必须**明显大于**按闭式 Δψ warp 后的残差，\n");
    std::printf("              否则「残差 = 运动没搬动的部分」这句话没有依据。\n");
    std::printf("    1b) 扫描最小点 α* = %.4f  （要求 ||α*|−1| ≤ %.2f）%s\n", g.bestAlpha,
        kMovingGuardAlphaTol, std::abs(std::abs(g.bestAlpha) - 1.0) <= kMovingGuardAlphaTol ? "✅" : "❌");
    std::printf("        含义：残差作为 warp 强度 α 的函数必须在 α=1 见底。不在 ⟹ 闭式单应与\n");
    std::printf("              画面真实运动对不上（Δψ 写错 / 符号反 / 扫速与画面不符），\n");
    std::printf("              此时残差里混着**没对齐**的量，任何分带都是在给噪声分格子。\n");
    std::printf("    2)  残差/地板 %.4f / %.4f = %.2f  （要求 ≥ %.2f）%s\n", g.residAt1, g.floorMean,
        g.ratio, kMovingGuardFloorRatio, g.ratio >= kMovingGuardFloorRatio ? "✅" : "❌");
    std::printf("        含义：地板 = 同态往返残差（真机重采样能造出的最小差）。贴着地板 ⟹ 量的是重采样。\n");
    std::printf("    3)  τ %.4f（要求 > 0）vs  τ 下界 %.4f（= 本次实测地板的**中位数**）%s\n", g.tau,
        g.tauFloor, g.tau > 0.0 && g.tau >= g.tauFloor ? "✅" : "❌");
    std::printf("    4)  全帧 over-τ 率 %.2f%%  vs  上限 %.2f%%%s\n", g.overRateAll * 100.0,
        kMovingGuardMaxOverRate * 100.0, g.overRateAll <= kMovingGuardMaxOverRate ? "✅" : "❌");
    std::printf("        含义：这是**实测的**饱和检查。旧口径（τ = 白电平处 1 显示 LSB）在这里报\n");
    std::printf("              100.00%% —— 那正是「四率全 100%% 却还能出判决」那个假绿的直接来源。\n");
    std::printf("\n  ⟹ 本次**不打印**三分支判决。退出码非 0（守卫红 = 失败，不是"
                "「结论偏弱」）。\n");
    std::printf("══════════════════════════════════════════════════════════════════════════\n");
}

void printMovingBandTable(const std::string& title, const BandTable& t,
    const std::string& residualName)
{
    std::printf("  ┌ %s\n", title.c_str());
    std::printf("    被分带的量 = %s（**机位状态 = 相机移动**）\n", residualName.c_str());
    std::printf("    %-13s %9s %9s %11s %11s %10s\n", "格", "share", "rate>τ", "计数份额",
        "幅度份额", "over像素");
    for (int i = 0; i < 6; ++i) {
        const BandShare s = bandShareOf(t, kBandRowLb[i], kBandRowGb[i]);
        std::printf("    %-13s %8s%% %8s%% %10s%% %10s%% %10lld%s\n", kBandRowTag[i],
            pct2(s.share * 100.0).c_str(), pct2(s.rate * 100.0).c_str(),
            pct2(s.cntShare * 100.0).c_str(), pct2(s.ampShare * 100.0).c_str(),
            static_cast<long long>(s.over), i == 3 ? "   ← A 普通边缘" : (i == 5 ? "   ← B 高亮边" : ""));
    }
    std::printf("    %-13s %8s%% %8s%% %10s%% %10s%% %10lld\n", "TOTAL", "100.00",
        pct2(t.total > 0 ? 100.0 * static_cast<double>(t.over) / static_cast<double>(t.total)
                         : 0.0)
            .c_str(),
        "100.00", "100.00", static_cast<long long>(t.over));
    std::printf("    （图例：dark 暗 / mid 中间调 / clip 近削顶 × low 低梯度 / HIGH 高梯度。"
                "**计数份额** = 该格 over-τ 像素占全帧比例（不加权，昨天口径）；**幅度份额** = "
                "该格 Σ残差 占全帧比例（本次新增，堵可见性缺口：高光像素少但每像素摆动巨大时"
                "只有它看得出来））\n");
}

}  // namespace RGL::imgqa
