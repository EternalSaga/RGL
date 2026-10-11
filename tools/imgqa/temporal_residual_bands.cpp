// 批 13.7c 静止态**「亮度 × 梯度」二维分带**：两张六格表 + 差表 + 判决 + 敏感性。
//
//   从 `temporal_residual.cpp` 拆出（文件护栏 500 代码行）。它要回答的是现有分带口径答不了的
//   问题——所有现有测量只按梯度分带，而梯度分不清「几何边」与「高亮边」。要判的假设、两根轴的
//   口径与定阈值的规则（闭式反解显示链 + 实测直方图门槛）、⭐ 判决线（**跑前写死**）全文 =
//   `temporal_residual.h`「亮度 × 梯度 二维分带」段。
//
//   ⚠ 分档与判决线只准有一份：移动机位那条线（批 13.9）复用同一批算子（`bandTableOf` /
//   `sobelAbsGrad` / `bandClipProxyByte`），差别只在主数从 rate 比换成幅度份额，见
//   `temporal_residual_moving_bands.cpp`。四个门槛常量只被本 TU 的判决线消费 ⟹ 唯一出处在这里。

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

namespace {
// ── 批 13.7c「亮度 × 梯度」二维分带 ────────────────────────────────────────────────
//
// 判的假设、为什么现有分带判不了、⭐ 判决线（**跑前写死**）全文 = `temporal_residual.h`
// 「亮度 × 梯度 二维分带」段。这里只放本命令独有的门槛与出表代码；改判决线 = 改判据。
// 两根轴的阈值常量（`kBandClipProxyLinear` / `kBandMidByte` / `kBandGradPct` /
// `kBandClipSensitivity` / `kBandSupportRatio`）与行序（`kBandRow*`）已提到本头 =
// **单一出处**：批 13.9 的两把**移动机位**尺子复用同一套，谁都不许在自家 .cpp 里重拍。
constexpr double kBandMinClipMass = 0.0002;
/// T_lo 稳定性门槛：|x−128| ≤ 8 带内质量须 < 2%（否则切在直方图尖峰上，档位不稳）
constexpr double kBandMidSpikeCap = 0.02;
/// 判决线的样本量门槛：B 格短窗态 over-τ 像素低于它 ⟹ 不许拿噪声比噪声
constexpr int64_t kBandMinOverForVerdict = 200;
/// ⭐ 静止态判决线（批 13.7c 跑前写死，出处见头注释）：支持 = rB ≥ 1.5×rA；否证 = rB/rA ∈ [0.8,1.25]
/// （1.5 = 头文件里的 `kBandSupportRatio`，两把移动尺子的「超额集中在 B」沿用同一个数）
constexpr double kBandEqualLo = 0.8, kBandEqualHi = 1.25;


/// 出一张六格表（share / rate / 残余质量占比 / Σswing 占比）。
void printBandTable(const char* title, const BandTable& t)
{
    std::printf("  ┌ %s\n", title);
    std::printf("    %-13s %9s %9s %10s %9s\n", "格", "share", "rate>τ", "残余质量", "Σswing");
    for (int i = 0; i < 6; ++i) {
        const BandCell& c = t.cell[kBandRowLb[i]][kBandRowGb[i]];
        const double share = t.total > 0 ? static_cast<double>(c.pixels) / t.total : 0.0;
        const double rate = c.pixels > 0 ? static_cast<double>(c.over) / c.pixels : 0.0;
        const double mass = t.over > 0 ? static_cast<double>(c.over) / t.over : 0.0;
        const double m2 = t.mass > 0 ? c.mass / t.mass : 0.0;
        std::printf("    %-13s %8s%% %8s%% %9s%% %8s%%%s\n", kBandRowTag[i],
            d2(share * 100.0).c_str(), d2(rate * 100.0).c_str(), d2(mass * 100.0).c_str(),
            d2(m2 * 100.0).c_str(), i == 3 ? "   ← A 普通边缘" : (i == 5 ? "   ← B 高亮边" : ""));
    }
    std::printf("    %-13s %8s%% %8s%% %9s%% %8s%%\n", "TOTAL", "100.00",
        d2(t.total > 0 ? 100.0 * t.over / t.total : 0.0).c_str(), "100.00", "100.00");
    std::printf("    （图例：dark 暗 / mid 中间调 / clip 近削顶 × low 低梯度 / HIGH 高梯度；"
                "残余质量 = 该格 over-τ 像素占全帧的比例）\n");
}

/// 一格的 rate（over / pixels）。⚠ 空格给 0（不是「无定义」），判决线靠 `rate <= 0` 分支
/// 自己判「比值无定义」——把 0 改成 optional 会改掉判决形态。
double bandRate(const BandCell& c)
{
    return c.pixels > 0 ? static_cast<double>(c.over) / c.pixels : 0.0;
}

/// 差表行尾的标注：A/B 两格是判决线的判据格，其余六格里只有它们需要被认出来。
const char* bandDiffRowTag(int i)
{
    return i == 3 ? "  ← A" : (i == 5 ? "  ← B（压不动 = 假设成立）" : "");
}

/// 差表（长窗 − 短窗）。Δshare **必须恒 0** = 两态共用一把尺子的自证；非 0 = 尺子漏了。
double printBandDiff(const BandTable& off, const BandTable& on)
{
    std::printf("  ┌ 差（长窗 N2 − 短窗 N1）\n");
    std::printf("    %-13s %9s %9s %10s %12s\n", "格", "Δshare", "Δrate", "Δ残余质量",
        "rate比(on/off)");
    double maxShareDrift = 0.0;
    for (int i = 0; i < 6; ++i) {
        const BandCell& a = off.cell[kBandRowLb[i]][kBandRowGb[i]];
        const BandCell& b = on.cell[kBandRowLb[i]][kBandRowGb[i]];
        const double dShare = off.total > 0 && on.total > 0
                ? 100.0 * (static_cast<double>(b.pixels) / on.total
                      - static_cast<double>(a.pixels) / off.total)
                : 0.0;
        const double ra = bandRate(a);
        const double rb = bandRate(b);
        const double dMass = off.over > 0 && on.over > 0
                ? 100.0 * (static_cast<double>(b.over) / on.over
                      - static_cast<double>(a.over) / off.over)
                : 0.0;
        maxShareDrift = std::max(maxShareDrift, std::abs(dShare));
        std::printf("    %-13s %+8s %+8s %+10s %10s%s\n", kBandRowTag[i], d2(dShare).c_str(),
            d2(100.0 * (rb - ra)).c_str(), d2(dMass).c_str(),
            ra > 0.0 ? d4(rb / ra).c_str() : "inf",
            bandDiffRowTag(i));
    }
    return maxShareDrift;
}
}  // namespace

/// 亮度轴的实测结果（阈值由实测分布定，不许拍数；打印完还要拿 clipMass 去判「该档有没有定义」）。
struct BandLumaAxis {
    double tLo = 0.0, tHi = 0.0;
    double clipMass = 0.0;  ///< ≥ T_hi 的像素占比（近削顶档质量）
    double pile255 = 0.0;   ///< 精确 255 的堆积占比
    double midSpike = 0.0;  ///< T_lo±8 带内质量（切在谷底还是切在尖峰）
};

/// 先测：显示域字节亮度分布（阈值由它定，不许拍数）+ 门槛自证出表。
BandLumaAxis measureLumaAxis(const cv::Mat& luma)
{
    BandLumaAxis ax;
    auto lv = valuesInMask(luma, {});
    std::sort(lv.begin(), lv.end());
    const double n = static_cast<double>(luma.total());
    const auto massAtLeast = [&](double b) {
        return static_cast<double>(
                   lv.end() - std::lower_bound(lv.begin(), lv.end(), b)) / n;
    };
    const auto massNear = [&](double c, double w) {
        const auto lo = std::lower_bound(lv.begin(), lv.end(), c - w);
        const auto hi = std::upper_bound(lv.begin(), lv.end(), c + w);
        return static_cast<double>(hi - lo) / n;
    };
    ax.tHi = static_cast<double>(bandClipProxyByte(kBandClipProxyLinear));
    ax.tLo = kBandMidByte;
    ax.clipMass = massAtLeast(ax.tHi);
    ax.pile255 = massAtLeast(254.999);
    ax.midSpike = massNear(ax.tLo, 8.0);
    std::printf("  亮度轴 = 显示域字节亮度的**多帧均值**（tonemap+sRGB 之后取 Rec.601 加权）\n");
    std::printf("    实测分布 p50 %s · p90 %s · p99 %s · p99.9 %s · p99.99 %s（码字 0..255）\n",
        d2(percentileOf(lv, 0.50)).c_str(), d2(percentileOf(lv, 0.90)).c_str(),
        d2(percentileOf(lv, 0.99)).c_str(), d2(percentileOf(lv, 0.999)).c_str(),
        d2(percentileOf(lv, 0.9999)).c_str());
    std::printf("    255 精确堆积 %s%% · [%.0f,255) %s%% · [245,255) %s%% · [250,255) %s%%\n",
        d4(ax.pile255 * 100.0).c_str(), ax.tHi, d4(massAtLeast(ax.tHi) * 100.0).c_str(),
        d4(massAtLeast(245.0) * 100.0).c_str(), d4(massAtLeast(250.0) * 100.0).c_str());
    std::printf("    阈值 T_lo=%.0f（8bit 码字中点 ⟺ 显示域线性 %s ≈ 18%% 灰）· T_hi=%.0f"
                "（**闭式**：ACES 输入 ≥%s ⟺ byte %.0f，见 bandClipProxyByte）\n",
        ax.tLo, d4(srgbDecode(ax.tLo / 255.0)).c_str(), ax.tHi, d2(kBandClipProxyLinear).c_str(), ax.tHi);
    std::printf("    门槛：近削顶档质量 %s%%（下限 %s%%）%s · T_lo±8 带内质量 %s%%（上限 %s%%）%s\n",
        d4(ax.clipMass * 100.0).c_str(), d4(kBandMinClipMass * 100.0).c_str(),
        ax.clipMass >= kBandMinClipMass ? "✓" : "✗ 该档无定义", d4(ax.midSpike * 100.0).c_str(),
        d4(kBandMidSpikeCap * 100.0).c_str(),
        ax.midSpike < kBandMidSpikeCap ? "✓ 切在谷底" : "⚠ 切在尖峰");
    return ax;
}

/// ⭐ 判决的三分支（线**跑前写死**在 `temporal_residual.h`「亮度 × 梯度 二维分带」段；
/// 这里只执行，不解释成新规则）。五个返回串 = 结构签名，逐字不许改。
std::string bandVerdictText(int64_t bOver, double clipMass, double rateAOff, double rateBOff,
    double rA, double rB, double bMassOff, double bMassOn)
{
    if (bOver < kBandMinOverForVerdict || clipMass < kBandMinClipMass)
        return "分不出（B 格样本不足 / 近削顶档无定义）";
    if (rateBOff <= 0.0 || rateAOff <= 0.0)
        return "分不出（A 或 B 短窗态 rate=0，比值无定义）";
    if (rB >= kBandSupportRatio * rA && bMassOn > bMassOff)
        return "支持假设：残余向「近削顶×高梯度」集中，长窗档压不动高亮边";
    if (rB >= kBandEqualLo * rA && rB <= kBandEqualHi * rA)
        return "否证假设：两格等比例被压，残余不是高光专属";
    return "分不出（既非等比例、也未达 1.5× 分离——看下面敏感性表定性质）";
}

/// 分离方向自述（**不是**判决的一部分）。跑前只写死了「支持」与「等比例否证」两端，落第三支的
/// 其实有两种：**无分离**（真·分不出）与**反向分离**（B 比 A 压得更狠）。后者对「高亮边
/// 压不动」这条预测是否证方向，不打印出来会被「分不出」三个字读成「没有信号」。判决文字
/// 仍按跑前的三分支，不改写。
void printBandSeparation(double rA, double rB)
{
    std::printf("    分离方向：rB %s rA ⟹ %s（预测要求的是 rB > rA：B 比 A 压不动）\n",
        rB < rA ? "<" : (rB > rA ? ">" : "="),
        rB < rA ? "**B（高亮边）被压得更狠**，与预测方向相反"
                : (rB > rA ? "B 被压得没那么狠（方向与预测一致，幅度见上）" : "两格同倍数"));
}

/// 出判决（⭐ 三分支 + 两条只自述的读数），并把关键读数写进台账载体 `rep`。
void printBandVerdict(BandReport& rep, const BandTable& off, const BandTable& on,
    const BandLumaAxis& ax, double gradThr, double drift)
{
    const BandCell& aOff = off.cell[1][1];
    const BandCell& aOn = on.cell[1][1];
    const BandCell& bOff = off.cell[2][1];
    const BandCell& bOn = on.cell[2][1];
    const double rA = bandRate(aOff) > 0.0 ? bandRate(aOn) / bandRate(aOff) : 0.0;
    const double rB = bandRate(bOff) > 0.0 ? bandRate(bOn) / bandRate(bOff) : 0.0;
    const double bMassOff = off.over > 0 ? static_cast<double>(bOff.over) / off.over : 0.0;
    const double bMassOn = on.over > 0 ? static_cast<double>(bOn.over) / on.over : 0.0;
    rep.rA = rA;
    rep.rB = rB;
    rep.bMassOff = bMassOff;
    rep.bMassOn = bMassOn;
    rep.tHi = ax.tHi;
    rep.gradThr = gradThr;
    std::printf("\n  ⭐ 判决（线跑前写死：支持 = rB ≥ %s×rA 且 B 残余质量上升；否证 = rB/rA ∈ [%s,%s]）\n",
        d2(kBandSupportRatio).c_str(), d2(kBandEqualLo).c_str(), d2(kBandEqualHi).c_str());
    std::printf("    A 中间调×高梯度 rate %s%%→%s%%（×%s）  B 近削顶×高梯度 rate %s%%→%s%%（×%s）\n",
        d2(bandRate(aOff) * 100.0).c_str(), d2(bandRate(aOn) * 100.0).c_str(), d4(rA).c_str(),
        d2(bandRate(bOff) * 100.0).c_str(), d2(bandRate(bOn) * 100.0).c_str(), d4(rB).c_str());
    std::printf("    B 的残余质量占比 %s%% → %s%%（%+.2f pp）  B 短窗 over-τ 像素 %lld\n",
        d2(bMassOff * 100.0).c_str(), d2(bMassOn * 100.0).c_str(), (bMassOn - bMassOff) * 100.0,
        static_cast<long long>(bOff.over));
    rep.verdict = bandVerdictText(bOff.over, ax.clipMass, bandRate(aOff), bandRate(bOff), rA, rB,
        bMassOff, bMassOn);
    std::printf("    ⟹ **%s**\n", rep.verdict.c_str());
    printBandSeparation(rA, rB);
    rep.shareDrift = drift;
}

/// 敏感性（同一次采集换阈值，零额外引擎成本）。
/// 判「分不出」时的分工：换阈值后结论跟着翻 = **阈值没分开**；怎么换都不翻 = 两格同分布。
void printBandSensitivity(const BistroRun& N1, const BistroRun& N2, const cv::Mat& luma,
    const cv::Mat& grad, const BandLumaAxis& ax, double gradThr, double tau)
{
    std::printf("\n  ── 敏感性（同一次采集换阈值；主档 = 上面那张）──\n");
    std::printf("    %-26s %14s %14s %14s\n", "口径", "A rate ×(on/off)",
        "B rate ×(on/off)", "B 残余质量 off→on");
    const auto sensLine = [&](const std::string& tag, const cv::Mat& lm, const cv::Mat& gm,
                                  double tl, double th, double gthr) {
        const BandTable t1 = bandTableOf(N1.swingMap, lm, gm, tl, th, gthr, tau);
        const BandTable t2 = bandTableOf(N2.swingMap, lm, gm, tl, th, gthr, tau);
        const double a1 = bandRate(t1.cell[1][1]), a2 = bandRate(t2.cell[1][1]);
        const double b1 = bandRate(t1.cell[2][1]), b2 = bandRate(t2.cell[2][1]);
        const double m1 = t1.over > 0 ? static_cast<double>(t1.cell[2][1].over) / t1.over : 0.0;
        const double m2 = t2.over > 0 ? static_cast<double>(t2.cell[2][1].over) / t2.over : 0.0;
        std::printf("    %-26s %14s %14s %8s%% →%-7s%% (%lldpx)\n", tag.c_str(),
            a1 > 0.0 ? d4(a2 / a1).c_str() : "inf", b1 > 0.0 ? d4(b2 / b1).c_str() : "inf",
            d2(m1 * 100.0).c_str(), d2(m2 * 100.0).c_str(),
            static_cast<long long>(t1.cell[2][1].over));
    };
    for (double th : kBandClipSensitivity)
        sensLine("T_hi=" + d2(th) + (std::abs(th - ax.tHi) < 0.5 ? "（主档）" : ""), luma, grad,
            ax.tLo, th, gradThr);
    {
        auto gbv = valuesInMask(N1.gradByte, {});
        std::sort(gbv.begin(), gbv.end());
        const double gthrByte = percentileOf(gbv, kBandGradPct);
        sensLine("梯度轴=显示域 p90", luma, N1.gradByte, ax.tLo, ax.tHi, gthrByte);
        auto gv2 = valuesInMask(grad, {});
        std::sort(gv2.begin(), gv2.end());
        sensLine("梯度轴=线性 p75", luma, grad, ax.tLo, ax.tHi, percentileOf(gv2, 0.75));
        sensLine("梯度轴=线性 p95", luma, grad, ax.tLo, ax.tHi, percentileOf(gv2, 0.95));
        sensLine("亮度轴=跨帧最大", N1.byteMax, grad, ax.tLo, ax.tHi, gradThr);
        sensLine("T_lo=96", luma, grad, 96.0, ax.tHi, gradThr);
        sensLine("T_lo=160", luma, grad, 160.0, ax.tHi, gradThr);
    }
}

/// 出两态分带表 + 差表 + 判决 + 敏感性。返回进台账的关键读数。
///
/// 本函数只走骨架（两根轴 → 两张表 → 差表 → 判决 → 敏感性）；每一段的口径与红线在段函数里。
BandReport reportLumaGradBands(const BistroRun& N1, const BistroRun& N2, double tau)
{
    BandReport rep;
    std::printf("\n── 亮度 × 梯度 二维分带（判「残余住在高光边还是普通边缘」）──\n");
    if (N1.byteMean.empty() || N1.gradLin.empty() || N1.swingMap.empty() || N2.swingMap.empty()) {
        std::printf("  ✗ 缺逐像素图，分带表未出（采集侧 wantMaps 没开 = 仪器没装上）\n");
        return rep;
    }
    const cv::Mat& luma = N1.byteMean;
    const cv::Mat& grad = N1.gradLin;

    // ── 两根轴的尺子都由实测定（阈值不许拍数）；亮度轴见 measureLumaAxis ──────────
    const BandLumaAxis ax = measureLumaAxis(luma);
    auto gv = valuesInMask(grad, {});
    std::sort(gv.begin(), gv.end());
    const double gradThr = percentileOf(gv, kBandGradPct);
    std::printf("  梯度轴 = |Sobel_x|+|Sobel_y| 于**线性 luma 相位平均**（与 gradEnergy 同算子）\n");
    std::printf("    高梯度 = ≥ p%.0f = %s（占 %s%%；分位而非绝对阈值的理由见头注释）\n",
        kBandGradPct * 100.0, d4(gradThr).c_str(),
        d2(100.0 - kBandGradPct * 100.0).c_str());
    std::printf("  尺子 = N1（短窗 = 用户现状的对照端）的 byte-mean 与线性梯度，**两态共用一把尺子**\n");

    const BandTable off = bandTableOf(N1.swingMap, luma, grad, ax.tLo, ax.tHi, gradThr, tau);
    const BandTable on = bandTableOf(N2.swingMap, luma, grad, ax.tLo, ax.tHi, gradThr, tau);
    if (!off.valid() || !on.valid()) {
        std::printf("  ✗ 分带表无像素（total=0），不出数\n");
        return rep;
    }
    printBandTable("短窗 N1（用户现状的对照端）", off);
    printBandTable("长窗 N2（长窗档 α）", on);
    const double drift = printBandDiff(off, on);
    std::printf("    Δshare 最大 %s%%（**须恰为 0**：两态共用一把尺子的自证，非 0 = 差表作废）\n",
        d4(drift).c_str());

    printBandVerdict(rep, off, on, ax, gradThr, drift);
    printBandSensitivity(N1, N2, luma, grad, ax, gradThr, tau);
    return rep;
}

}  // namespace RGL::imgqa
