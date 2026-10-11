// temporal-motion 的**批 13.9 分带报告层** —— 【机位状态 = 相机移动】亮度 × 梯度二维分带
// （尺子 2：对超采样参考的逐像素偏差）。要判的假设、三张表各是什么、判决线全文 =
// `temporal_motion.cpp` 头注释「批 13.9」段；这里只放执行代码。
// 拆出来是因为 `temporal_motion.cpp` 2748 代码行 > 500（批次 B11），缝按**仪器**切。
#include "temporal_motion.h"

#include "imgqa.h"
#include "temporal_residual.h"
#include "temporal_yaw_remap.h"

#include <algorithm>
#include <cmath>
#include <numeric>  // std::accumulate（阶跃主指标 AUC = Σ ghost(n)）
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
using namespace tmotion;  // 本族跨 TU 的类型 / 常量 / 算子（唯一出处 = 本文件的第一条 include）

namespace tmotion {

// ── 批 13.9：【机位状态 = **相机移动**】亮度 × 梯度 二维分带（尺子 2）───────────────────
//
// 要判的假设、三张表各是什么、尺子为什么必须来自参考图、⭐ 判决线（**跑前写死**）全文 =
// 本文件头注释「批 13.9」段。这里只放执行代码；改判决线 = 改判据，两处一起改。

/**
 * 逐像素偏差图 = 跨帧平均的 `|候选 − 参考（按帧号配对到同一视点）|`（线性域，CV_64FC1）。
 *
 * 与 `measureDeviation` 的关系：那条把这张图**压成一个数**（边缘带上的均值），本函数保留
 * 整张图供分带。两者是同一口径（同一配对、同一线性域、同一绝对值），只是一个求值一个成图
 * ——「表上的边缘偏差」与「分带表里边缘带的贡献」因此必然自洽。
 */
cv::Mat movingDevMap(const std::vector<cv::Mat>& cand, const Group& ref, const Alignment& al)
{
    cv::Mat acc = cv::Mat::zeros(cand.front().size(), CV_64FC1);
    int frames = 0;
    for (size_t i = 0; i < cand.size() && i < ref.luma.size(); ++i) {
        cv::Mat d;
        cv::absdiff(cand[i], ref.luma[static_cast<size_t>(al.pairIndex[i])], d);
        cv::add(acc, d, acc);
        ++frames;
    }
    acc /= static_cast<double>(std::max(1, frames));
    return acc;
}

/// 率比退化闸门线（`frac>τ` 饱和时率比不判方向）。原为函数内局部量，拆函数后提到此处唯一出处。
constexpr double kSat = 0.90;

/// 三分支判决的**全部输入**（一次算好，判决与打印共用）。
/// 批次 B11 拆 `reportMovingBandsAgainstReference` 时引入的搬运容器：算式一条没改。
struct MovingBandShares {
    BandShare aOff, aOn, bOff, bOn, bExc, aExc;
    double rA = 0.0, rB = 0.0;
    bool excessInB = false;
    bool rateSaturated = false;
    double relDiff = 0.0;
};

/// 「尺子来自参考图」的回显（原样搬出）。返回**排序后**的梯度值，敏感性表要按它取分位。
[[nodiscard]] std::vector<double> movingBandRulerEcho(const cv::Mat& grad, double gradThr,
    double tau, double tHi)
{
    std::printf("  分带尺子**来自参考图 REFm**（超采样那张）：参考定义了「这个像素**本来**是什么」，"
                "用被测量自己定档 = 用被测量选区域（昨天已因此翻过一次方向）\n");
    std::printf("    亮度轴 = REFm 显示域字节亮度的多帧均值（1440p 求均值后 INTER_AREA 降到 720p）；"
                "T_lo=%.0f · T_hi=%.0f（**闭式**：ACES 输入 ≥%s）\n",
        kBandMidByte, tHi, d4(kBandClipProxyLinear).c_str());
    // 梯度轴**回显实际分位**（不是写死 p90）：调用方可以 --edge-pct 改档，写死会让表说谎。
    std::vector<double> gv = valuesInMask(grad, {});
    std::sort(gv.begin(), gv.end());
    const double gradPct = gv.empty() ? 0.0
        : static_cast<double>(
              std::lower_bound(gv.begin(), gv.end(), gradThr) - gv.begin())
            / static_cast<double>(gv.size());
    std::printf("    梯度轴 = sobelAbsGrad(REFm 线性 luma) ≥ %s（= 参考梯度的 p%.2f，**就是本命令"
                "已有的那条边缘带**，一个阈值都不新拍）\n",
        d4(gradThr).c_str(), gradPct);
    std::printf("    τ = %s（= 上面判据用的同一个 τ，白电平 p99 处 1 个显示 LSB，闭式）\n",
        d4(tau).c_str());
    return gv;
}

/// 六格份额 + 率比 + 两条退化闸门（原样搬出，算式一字未动）。
[[nodiscard]] MovingBandShares movingBandSharesOf(const BandTable& tOff, const BandTable& tOn,
    const BandTable& tExc)
{
    MovingBandShares s;
    s.aOff = bandShareOf(tOff, 1, 1);
    s.aOn = bandShareOf(tOn, 1, 1);
    s.bOff = bandShareOf(tOff, 2, 1);
    s.bOn = bandShareOf(tOn, 2, 1);
    s.bExc = bandShareOf(tExc, 2, 1);
    s.aExc = bandShareOf(tExc, 1, 1);
    s.rA = s.aOff.rate > 0.0 ? s.aOn.rate / s.aOff.rate : 0.0;
    s.rB = s.bOff.rate > 0.0 ? s.bOn.rate / s.bOff.rate : 0.0;
    s.excessInB = s.rA > 0.0 && s.rB >= kBandSupportRatio * s.rA;
    // ⚠ **率比退化闸门**（与尺子 1 同一条，实测逼出）：移动机位下 `frac>τ` 会饱和，四格 rate
    // 双双贴 100% 时 rX 的分子分母同贴 1.0，rB/rA 只剩浮点噪声 ⟹ 饱和时**方向不判**。
    s.rateSaturated = s.aOff.rate > kSat && s.bOff.rate > kSat && s.aOn.rate > kSat
        && s.bOn.rate > kSat;
    // rB/rA 的相对差（判据线要求的是 ≥50% 的分离，故这个数远小于 0.5 时「方向」根本没有内容）。
    s.relDiff = std::abs(s.rB - s.rA) / std::max({ s.rA, s.rB, 1e-12 });
    return s;
}

/// 正/负超额**总量**回显（原样搬出）——「R=1.14 在二维上展开成了什么」的守恒检查。
void printMovingBandMasses(const cv::Mat& excess, const cv::Mat& devOff, const cv::Mat& devOn)
{
    // 正/负超额的**总量**（读「R=1.14 这个整体比值在二维上展开成了什么」的守恒检查）
    const double posMass = cv::sum(excess)(0);
    cv::Mat deficit;
    cv::subtract(devOff, devOn, deficit);
    cv::max(deficit, 0.0, deficit);
    const double negMass = cv::sum(deficit)(0);

    std::printf("\n  正向超额合计 Σ=%.4f ｜ 反向（开 TAA 更贴真值）合计 Σ=%.4f ｜ 净 = %+.4f"
                "（>0 ⟺ 全帧平均上开 TAA 更偏离参考，与 R>1 同向）\n",
        posMass, negMass, posMass - negMass);
}

/// 三分支判决回显（原样搬出）。返回判决文字供台账。
[[nodiscard]] const char* printMovingBandVerdict(const MovingBandShares& s)
{
    std::printf("  率比 rX = rate_on(X)/rate_off(X)（该格 over-τ 比例开 TAA 后翻几倍）\n");
    std::printf("    A 中间调×高梯度 rate %s → %s（×%s）  B 近削顶×高梯度 rate %s → %s（×%s）\n",
        pct2(s.aOff.rate).c_str(), pct2(s.aOn.rate).c_str(), d4(s.rA).c_str(),
        pct2(s.bOff.rate).c_str(), pct2(s.bOn.rate).c_str(), d4(s.rB).c_str());
    std::printf("    B 幅度份额（主表 dev 关）%s ｜ B 在**正向超额**里的幅度份额 %s（面积只占 %s）"
                "｜ A 在正向超额里的幅度份额 %s（面积占 %s）\n",
        pct2(s.bOff.ampShare).c_str(), pct2(s.bExc.ampShare).c_str(),
        pct2(s.bExc.share).c_str(), pct2(s.aExc.ampShare).c_str(),
        pct2(s.aExc.share).c_str());
    if (s.rateSaturated)
        std::printf("    ⚠ **率比在本场景退化失效**：四格 rate 全部 > %.0f%%（`frac>τ` 饱和）⟹ rX 的"
                    "分子分母同贴 1.0，rB/rA 的相对差只有 %s ——**方向不判**，只看幅度份额与超额表\n",
            kSat * 100.0, d4(s.relDiff).c_str());

    const MovingBandBranch branch
        = classifyMovingBand(s.bOff.ampShare, s.bOff.cntShare, s.excessInB);
    std::printf("\n  ⭐ 判决（线**跑前写死**：支持 = B 幅度份额 ≥ %s 或 rB ≥ %s×rA；"
                "否证 = B 两份额都 < %s 且超额不向 B 集中）\n",
        pct2(kMovingSupportAmpShare).c_str(), d4(kBandSupportRatio).c_str(),
        pct2(kMovingRefuteShare).c_str());
    std::printf("    支撑格数字：B 幅度份额 %s（支持线 %s / 否证线 %s）· B 计数份额 %s · "
                "rB/rA = %s · 超额集中在B = %s\n",
        pct2(s.bOff.ampShare).c_str(), pct2(kMovingSupportAmpShare).c_str(),
        pct2(kMovingRefuteShare).c_str(), pct2(s.bOff.cntShare).c_str(),
        s.rA > 0.0 ? d4(s.rB / s.rA).c_str() : "inf", s.excessInB ? "是" : "否");
    std::printf("    ⟹ **%s**\n", movingBandBranchName(branch));
    printMovingBandVacuity(s.bOff, "REFm 超采样参考 INTER_AREA 降到 720p 后的字节亮度均值",
        "REFm 是 2560×1440 超采样图，亮度轴经 **INTER_AREA（面积平均）** 降到 720p 网格 ⟹ "
        "1px 高亮细线被邻居稀释（最坏 4 倍），**假设的元凶可能正好被尺子自己抹掉**。"
        "下面敏感性表的「亮度轴=原生格峰值」行就是量这件事的：若近削顶档在空集/非空之间翻，"
        "则「看不见」是降采样口径问题，不是「高光不闪」");
    // 分支③的**性质**也必须命名（昨天缺的就是这个）：方向 + 敏感性两条一起读。
    if (s.rateSaturated)
        std::printf("    分离方向：**不判**（rate 饱和；rB/rA 相对差 %s，而判据线要的是 ≥50%% 的分离"
                    "⟹ 方向这一维在本场景没有内容）——用户主张要求 rB > rA，但本场景 rate 轴没有"
                    "分辨力，方向问题由**幅度份额**与**正向超额表**回答\n",
            d4(s.relDiff).c_str());
    else
        std::printf("    分离方向：rB %s rA ⟹ %s（用户主张要求的是 rB > rA：B 比 A 更压不动）\n",
            s.rB < s.rA ? "<" : (s.rB > s.rA ? ">" : "="),
            s.rB < s.rA ? "**反向分离**（B 比 A 压得更狠，与主张方向相反）"
                    : (s.rB > s.rA ? "B 压得没那么狠（方向与主张一致，幅度见上）" : "两格同倍数"));
    return movingBandBranchName(branch);
}

/// 敏感性表（原样搬出）：同一次采集换阈值，零额外引擎成本。判「分不出」时的分工：换档结论
/// 跟着翻 = **阈值没分开**；怎么换都不翻 = **两格本来同分布**。
void printMovingBandSensitivity(const Group& ref, const cv::Mat& grad,
    const std::vector<double>& gv, double tHi, double gradThr, double tau,
    const cv::Mat& devOff, const cv::Mat& devOn, const cv::Mat& excess)
{
    // ── 敏感性（同一次采集换阈值，零额外引擎成本）。判「分不出」时的分工：换档结论跟着翻
    //    = **阈值没分开**；怎么换都不翻 = **两格本来同分布**。
    std::printf("\n  ── 敏感性（同一次采集换阈值；主档 = 上面那张）──\n");
    std::printf("    %-24s %12s %12s %16s %16s\n", "口径", "A 率比", "B 率比",
        "B 幅度份额(dev关)", "B 幅度份额(超额)");
    const auto sensLine = [&](const std::string& tag, const cv::Mat& lm, const cv::Mat& gm,
                                  double tl, double th, double gthr) {
        const BandTable o = bandTableOf(devOff, lm, gm, tl, th, gthr, tau);
        const BandTable n = bandTableOf(devOn, lm, gm, tl, th, gthr, tau);
        const BandTable e = bandTableOf(excess, lm, gm, tl, th, gthr, tau);
        const BandShare a1 = bandShareOf(o, 1, 1), a2 = bandShareOf(n, 1, 1);
        const BandShare b1 = bandShareOf(o, 2, 1), b2 = bandShareOf(n, 2, 1);
        const double ra = a1.rate > 0.0 ? a2.rate / a1.rate : 0.0;
        const double rb = b1.rate > 0.0 ? b2.rate / b1.rate : 0.0;
        std::printf("    %-24s %12s %12s %15s %15s\n", tag.c_str(), d4(ra).c_str(),
            d4(rb).c_str(), pct2(bandShareOf(o, 2, 1).ampShare).c_str(),
            pct2(bandShareOf(e, 2, 1).ampShare).c_str());
    };
    for (double th : kBandClipSensitivity)
        sensLine("T_hi=" + std::to_string(static_cast<int>(th))
                + (std::abs(th - tHi) < 0.5 ? "（主档）" : ""),
            ref.byteMean, grad, kBandMidByte, th, gradThr);
    sensLine("梯度轴=线性 p75", ref.byteMean, grad, kBandMidByte, tHi,
        percentileOf(gv, 0.75));
    sensLine("梯度轴=线性 p95", ref.byteMean, grad, kBandMidByte, tHi, percentileOf(gv, 0.95));
    sensLine("亮度轴=跨帧最大", ref.byteMax, grad, kBandMidByte, tHi, gradThr);
    sensLine("亮度轴=原生格峰值", ref.bytePeak, grad, kBandMidByte, tHi, gradThr);
    sensLine("T_lo=96", ref.byteMean, grad, 96.0, tHi, gradThr);
    sensLine("T_lo=160", ref.byteMean, grad, 160.0, tHi, gradThr);
    std::printf("    读法：换档后「B 幅度份额」跟着翻过 %s / %s 两条线 = **阈值没分开**；"
                "怎么换都不翻 = **两格本来同分布**\n",
        pct2(kMovingRefuteShare).c_str(), pct2(kMovingSupportAmpShare).c_str());
}

/// 出三张六格表（dev 关 / dev 开 / 正向超额）+ 三分支判决 + 敏感性。返回判决文字供台账。
///
/// 批次 B11 拆小（原 134 代码行 / 认知 27）：五段各自搬进 `movingBandRulerEcho` /
/// `movingBandSharesOf` / `printMovingBandMasses` / `printMovingBandVerdict` /
/// `printMovingBandSensitivity`。⚠ 本函数里的**调用顺序 = 原实现从上到下的打印顺序**。
std::string reportMovingBandsAgainstReference(const Group& ref, const cv::Mat& grad,
    double gradThr, double tau, const cv::Mat& devOff, const cv::Mat& devOn,
    const std::string& cameraName)
{
    std::printf("\n── 【机位状态 = **相机移动**】亮度 × 梯度 二维分带（尺子 2：对超采样参考的"
                "逐像素偏差；机位 %s）──\n",
        cameraName.c_str());
    if (ref.byteMean.empty() || grad.empty() || devOff.empty()) {
        std::printf("  ✗ 尺子缺失（REFm 的字节亮度没攒到 = 采集侧 wantBytes 没开），分带表未出\n");
        return "未出（尺子缺失）";
    }
    const double tHi = static_cast<double>(bandClipProxyByte(kBandClipProxyLinear));

    const std::vector<double> gv = movingBandRulerEcho(grad, gradThr, tau, tHi);
    const BandTable tOff = bandTableOf(devOff, ref.byteMean, grad, kBandMidByte, tHi, gradThr, tau);
    const BandTable tOn = bandTableOf(devOn, ref.byteMean, grad, kBandMidByte, tHi, gradThr, tau);
    cv::Mat excess;
    cv::subtract(devOn, devOff, excess);
    cv::max(excess, 0.0, excess);  // **正向**超额 = 开 TAA 让这里更偏离真值；负的部分不进分带
    const BandTable tExc = bandTableOf(excess, ref.byteMean, grad, kBandMidByte, tHi, gradThr, tau);
    if (!tOff.valid() || !tOn.valid() || !tExc.valid()) {
        std::printf("  ✗ 分带表无像素（total=0），不出数\n");
        return "未出（无像素）";
    }
    printMovingBandTable("dev(TAA 关) —— 无 AA 单帧光栅的锯齿本体（**主表**）", tOff,
        "对超采样参考的逐像素偏差（TAA 关）");
    printMovingBandTable("dev(TAA 开) —— 开 TAA 之后还剩的偏差", tOn,
        "对超采样参考的逐像素偏差（TAA 开）");
    printMovingBandTable("max(dev(开) − dev(关), 0) —— **正向超额**：开 TAA 让哪里更偏离真值", tExc,
        "偏差差图的正部");

    const MovingBandShares s = movingBandSharesOf(tOff, tOn, tExc);
    printMovingBandMasses(excess, devOff, devOn);
    const char* const branch = printMovingBandVerdict(s);
    printMovingBandSensitivity(ref, grad, gv, tHi, gradThr, tau, devOff, devOn, excess);
    return branch;
}

}  // namespace tmotion

}  // namespace RGL::imgqa
