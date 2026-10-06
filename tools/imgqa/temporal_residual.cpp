// 时域**绝对**残差判据（批 13.1 主判据）——`imgqa temporal` 的盲区补账。
//
// ## 为什么现有四条全绿还不够（本命令存在的全部理由）
//
// `temporal` 的抑制判据是**相对比值** `S1/S2 ≥ 3.0`（闭式 √16 = 4.0）。相对量有一个结构性
// 盲区：它不约束「收敛后逐帧残差的**绝对大小**」。实测同一份输出里 TAA 开仍有 **20.78% 像素
// 逐帧变化、最大摆幅 0.535**（线性 luma = 136 显示 LSB），四条判据一条都没红。用户报的症状
// （bistro 静止机位下 1px 宽高亮细结构持续闪烁）正好住在这个盲区里：抑制比好 = 每个相位上的
// 估计比不滤时干净，但**残余摆动仍然肉眼可见**。
//
// 本命令把「收敛后还在摆多少」变成绝对读数：静止机位连拍折成 `(2, 16, H, W)`（两半同列 =
// 同一绝对抖动相位），逐像素 `swing = max_over_16 − median_over_16`，报 meanSwing / p99.9 /
// `frac(swing > τ)`，τ = 一个显示 LSB 在靶白电平处的线性跨度（闭式，见 `lsbLinearAtLuma`）。
// 另加一条**收敛正确性**：线性域梯度能量比 `grad(TAA 收敛连拍的相位平均) / grad(fb=1 一周期的
// 相位平均)`，分母 = 「理想 16 相位平均」（fb=1 时输出逐位 = 本帧光栅，按构造成立）。
//
// ## 五组采集（全部 aa_target 细线靶 + 静止机位，只差被测项本身）
//
//   R0  TAA 关                → **确定性地板**：连拍必须逐字节相同（不是接近相同）
//   R1  TAA 开 fb=1  js=1     → **阳性锚点**：只抖不滤 = 闪烁本体，残差必须 ≫ 0；
//                               同时它的相位平均 = 收敛正确性判据的**参考图**
//   R2  TAA 开 fb=.12 js=1    → 被测量（现行工作点）
//   R3  TAA 开 fb=.12 js=0    → **反向用例**：抖动是激励源，掐了残差必须恰为 0
//                               （不掐零 = 量具坏了，R2 的读数全部作废）
//   R4  TAA 开 fb=.002 js=1   → **分辨力轴**：等效窗 1/α = 500 帧 ≫ 抖动周期 16，
//                               残差必须显著低于 R2。不单调 = 量具无效
//
// 三条锚点（缺一条就是假绿）：R1 阳性（≫0）、R0 地板（恰 0）、R3 反向（恰 0）。
// 度量定义与三条测量红线（线性域 / 热身 ≥6/α 且**不写盘** / 配置须带 environment 段）的完整
// 理由 = `temporal_residual.h` 头注释，勿只看这里。
//
// ## 为什么折叠口径不需要标定相位（与 `taa-swim` 的分工）
//
// `taa-swim` 量「相邻帧之间是不是整幅平移」，必须知道「采集第 i 帧 = 引擎抖动序号几」，那个
// 常数相位每次启动都不一样（RPC 登记落在第几帧由启动时序决定），它烧掉两版量具才做对标定。
// 本命令**按 16 折叠**：两半各含整数个周期 ⟹ 同一列 = 同一绝对相位，与常数相位偏移无关。
// 代价是量不到「摆的方向」，只量到「摆多大」——绝对残差本来就不需要方向。
#include "temporal_residual.h"

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

// ═══════════════════════════════════════════ 纯度量层（selftest 直接打得到）

double lsbLinearAtLuma(double lin)
{
    // srgbDecode 的导数（推导见头文件）：d(linear)/d(byte) = (2.4/1.055)·lin^(7/12)/255。
    // lin ≤ 0 给 0：黑电平处「1 LSB」没有定义（本靶的白电平才是可见性发生的地方）。
    if (lin <= 0.0)
        return 0.0;
    return (2.4 / 1.055) * std::pow(lin, 7.0 / 12.0) / 255.0;
}

double maxLsbStepUpTo(double maxLin)
{
    // 逐字节扫真解码器（256 次，闭式在这里不适用：`srgbDecode` 是分段函数，而本判据读的就是
    // 字节）。`srgbDecode` 单调 ⟹ 一旦下沿超过 maxLin 就可以停 —— 但**不能**在超过的那一刻
    // 才把这一步算进去：mask 里最亮的像素就住在那一格的下沿上，那一步正是它翻一格的跨度。
    double best = 0.0;
    for (int b = 0; b < 255; ++b) {
        const double lo = srgbDecode(static_cast<double>(b) / 255.0);
        const double hi = srgbDecode(static_cast<double>(b + 1) / 255.0);
        if (lo > maxLin)
            break;
        best = std::max(best, hi - lo);
    }
    return best;
}

cv::Mat phaseAverageLuma(const std::vector<Frame>& shots, int periods)
{
    if (periods < 2 || periods % 2 != 0)
        throw std::runtime_error("周期数必须是 ≥2 的偶数（两半折叠靠它，奇数半没有同相位对照）");
    const int frames = kResidualPeriod * periods;
    if (static_cast<int>(shots.size()) != frames)
        throw std::runtime_error("连拍张数 " + std::to_string(shots.size()) + " ≠ periods×16 = "
            + std::to_string(frames) + "（折叠口径按构造要求整数个周期，缺帧 = 列与相位脱钩）");
    cv::Mat acc;
    for (const auto& f : shots) {
        if (f.luma.empty())
            throw std::runtime_error("序列里有帧没 prepareLinear（线性域是这条判据的前提）");
        if (acc.empty())
            acc = cv::Mat::zeros(f.luma.size(), CV_64FC1);
        else if (acc.size() != f.luma.size())
            throw std::runtime_error("连拍里混进了别的 extent——两帧不是同一次采集");
        cv::add(acc, f.luma, acc);
    }
    acc /= static_cast<double>(frames);
    return acc;
}

cv::Mat sobelAbsGrad(const cv::Mat& luma)
{
    cv::Mat gx, gy, g;
    cv::Sobel(luma, gx, CV_64F, 1, 0, 1);
    cv::Sobel(luma, gy, CV_64F, 0, 1, 1);
    g = cv::abs(gx) + cv::abs(gy);  // MatExpr 必须先落地成 Mat 才是 InputArray
    return g;
}

double gradEnergy(const cv::Mat& luma, const cv::Mat& mask)
{
    // 梯度本体只许一份实现（`sobelAbsGrad`），本函数是它的均值；selftest 的斜坡闭式
    // （2·slope·(W−2)/W）同时钉住两者，重构不会悄悄改口径。
    const cv::Mat g = sobelAbsGrad(luma);
    if (mask.empty())
        return cv::mean(g)(0);
    return cv::mean(g, mask)(0);
}

double percentileOf(const std::vector<double>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    const size_t i = std::min(sorted.size() - 1,
        static_cast<size_t>(p * static_cast<double>(sorted.size())));
    return sorted[i];
}

std::vector<double> valuesInMask(const cv::Mat& x, const cv::Mat& mask)
{
    std::vector<double> v;
    if (mask.empty()) {
        v.reserve(x.total());
        for (int y = 0; y < x.rows; ++y) {
            const double* px = x.ptr<double>(y);
            v.insert(v.end(), px, px + x.cols);
        }
        return v;
    }
    v.reserve(static_cast<size_t>(std::max<int64_t>(1, cv::countNonZero(mask))));
    for (int y = 0; y < x.rows; ++y) {
        const double* px = x.ptr<double>(y);
        const uchar* pm = mask.ptr<uchar>(y);
        for (int c = 0; c < x.cols; ++c)
            if (pm[c])
                v.push_back(px[c]);
    }
    return v;
}

BandTable bandTableOf(const cv::Mat& swing, const cv::Mat& lumaByte, const cv::Mat& grad,
    double tLo, double tHi, double gradThr, double tau)
{
    BandTable t;
    // 尺子与观测量必须是同一次采集：尺寸不符 = 直接不出数（「表看起来正常但像素是错位的」
    // 是这套仪器里最贵的一类失效，与 `phaseAverageLuma` 拒收混 extent 同一课）。
    if (swing.empty() || swing.type() != CV_64FC1 || lumaByte.size() != swing.size()
        || grad.size() != swing.size() || lumaByte.type() != CV_64FC1 || grad.type() != CV_64FC1)
        return t;
    for (int y = 0; y < swing.rows; ++y) {
        const double* ps = swing.ptr<double>(y);
        const double* pl = lumaByte.ptr<double>(y);
        const double* pg = grad.ptr<double>(y);
        for (int x = 0; x < swing.cols; ++x) {
            // 边界写死：`< tLo` 暗 / `[tLo, tHi)` 中间调 / `≥ tHi` 近削顶；梯度 `≥ gradThr` 高。
            const int lb = pl[x] < tLo ? 0 : (pl[x] < tHi ? 1 : 2);
            const int gb = pg[x] >= gradThr ? 1 : 0;
            BandCell& c = t.cell[lb][gb];
            ++c.pixels;
            ++t.total;
            c.mass += ps[x];
            t.mass += ps[x];
            if (tau > 0.0 && ps[x] > tau) {  // 严格大于，与 swingFoldCore 的 fracOverTau 同口径
                ++c.over;
                ++t.over;
            }
        }
    }
    return t;
}

RejectFraction rejectFractionOf(const cv::Mat& alpha8)
{
    RejectFraction f;
    if (alpha8.empty() || alpha8.type() != CV_8UC1)
        return f;  // 空图 / 非单通道 = 不出数（调用侧的「五桶之和 = 1」自检会抓）
    const double n = static_cast<double>(alpha8.total());
    int64_t c[5] = { 0, 0, 0, 0, 0 };
    for (int y = 0; y < alpha8.rows; ++y) {
        const uint8_t* p = alpha8.ptr<uint8_t>(y);
        for (int x = 0; x < alpha8.cols; ++x)
            ++c[p[x] < 32 ? 0 : (p[x] < 96 ? 1 : (p[x] < 160 ? 2 : (p[x] < 224 ? 3 : 4)))];
    }
    f.accepted = c[0] / n;
    f.clipped = c[1] / n;
    f.collapsed = c[2] / n;
    f.reproject = c[3] / n;
    f.noHistory = c[4] / n;
    return f;
}

/// 拒收帧数 → 交叉表档位（0 / 1–2 / 3–8 / 9–16 / 17+）。
/// **唯一口径**：`rejectCrossTab` 与 `suppressionByRejectBin` 共用它，否则两张表的「档」
/// 不是同一个区域，而跨表的差值正是判据读的东西。
int rejectBinIndexOf(int32_t r)
{
    return r <= 0 ? 0 : (r <= 2 ? 1 : (r <= 8 ? 2 : (r <= 16 ? 3 : 4)));
}

double emaTransmittance(double alpha, int k, int period)
{
    if (period <= 0 || k < 0 || alpha <= 0.0)
        return 0.0;
    const double w = 2.0 * 3.14159265358979323846 * static_cast<double>(k)
        / static_cast<double>(period);
    // |1 − (1−α)e^{−jw}|，实部/虚部分开（k=0 时虚部为 0、实部为 α ⟹ |H| = 1 = DC 增益，
    // 这条是自检钉的边界之一）。
    const double re = 1.0 - (1.0 - alpha) * std::cos(w);
    const double im = (1.0 - alpha) * std::sin(w);
    return alpha / std::hypot(re, im);
}

double emaSuppressionRatio(double alphaOn, double alphaOff, int k, int period)
{
    const double off = emaTransmittance(alphaOff, k, period);
    return off > 0.0 ? emaTransmittance(alphaOn, k, period) / off : 0.0;
}

SuppressionTable suppressionByRejectBin(const cv::Mat& swingOn, const cv::Mat& swingOff,
    const cv::Mat& rejectFrames)
{
    SuppressionTable t;
    // 前提守卫（同 `rejectCrossTab`）：三者必须同尺寸同一次采集，否则**不出数**。
    if (swingOn.empty() || swingOff.empty() || rejectFrames.empty() || swingOn.type() != CV_64FC1
        || swingOff.type() != CV_64FC1 || rejectFrames.type() != CV_32SC1
        || swingOn.size() != swingOff.size() || rejectFrames.size() != swingOn.size())
        return t;
    double massOff[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    double massOn[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    int64_t px[6] = { 0, 0, 0, 0, 0, 0 };
    for (int y = 0; y < swingOn.rows; ++y) {
        const double* pon = swingOn.ptr<double>(y);
        const double* poff = swingOff.ptr<double>(y);
        const int32_t* pr = rejectFrames.ptr<int32_t>(y);
        for (int x = 0; x < swingOn.cols; ++x) {
            const int b = rejectBinIndexOf(pr[x]);
            massOff[b] += poff[x];
            massOn[b] += pon[x];
            ++px[b];
            massOff[5] += poff[x];
            massOn[5] += pon[x];
            ++px[5];
        }
    }
    for (int b = 0; b < 6; ++b) {
        BinSuppression& s = b < 5 ? t.bin[b] : t.total;
        s.pixels = px[b];
        s.swingOff = px[b] > 0 ? massOff[b] / static_cast<double>(px[b]) : 0.0;
        s.swingOn = px[b] > 0 ? massOn[b] / static_cast<double>(px[b]) : 0.0;
        s.rho = s.swingOff > 0.0 ? s.swingOn / s.swingOff : 0.0;
    }
    t.valid = px[5] > 0;
    return t;
}

RejectCrossTab rejectCrossTab(const cv::Mat& rejectFrames, const cv::Mat& swing, double tau)
{
    RejectCrossTab t;
    // 前提守卫（同 `bandTableOf`）：两侧必须同尺寸同一次采集，否则**不出数**。
    // 「表看起来完全正常但像素是错位的」是这套仪器里最贵的假绿。
    if (rejectFrames.empty() || swing.empty() || swing.type() != CV_64FC1
        || rejectFrames.type() != CV_32SC1 || rejectFrames.size() != swing.size())
        return t;
    for (int y = 0; y < swing.rows; ++y) {
        const double* ps = swing.ptr<double>(y);
        const int32_t* pr = rejectFrames.ptr<int32_t>(y);
        for (int x = 0; x < swing.cols; ++x) {
            const int32_t r = pr[x];
            const int b = rejectBinIndexOf(r);
            RejectBinCell& c = t.bin[b];
            ++c.pixels;
            ++t.total;
            c.mass += ps[x];
            t.mass += ps[x];
            if (tau > 0.0 && ps[x] > tau) {  // 严格大于，与 fracOverTau / bandTableOf 同口径
                ++c.over;
                ++t.over;
            }
        }
    }
    return t;
}

RejectBranch classifyReject(const RejectCrossTab& t, double bin0Share)
{
    if (!t.valid() || bin0Share < kRejectMinBin0Share)
        return RejectBranch::Indeterminate;  // 没有「老实累积」的对照 ⟹ 不许比
    const double rAll = static_cast<double>(t.over) / static_cast<double>(t.total);
    const double r0 = t.bin[0].pixels > 0
            ? static_cast<double>(t.bin[0].over) / static_cast<double>(t.bin[0].pixels)
            : 0.0;
    if (rAll <= 0.0)
        return RejectBranch::Indeterminate;  // 全帧不闪 = 本判据无从判起
    const double m3p = t.mass > 0.0
            ? (t.bin[2].mass + t.bin[3].mass + t.bin[4].mass) / t.mass
            : 0.0;
    if (r0 <= kRejectAnchorQuiet * rAll && m3p >= kRejectMassLine)
        return RejectBranch::Support;
    if (r0 >= kRejectRefuteSame * rAll)
        return RejectBranch::Refute;
    return RejectBranch::Indeterminate;
}

const char* rejectBranchName(RejectBranch b)
{
    switch (b) {
    case RejectBranch::Support:
        return "支持：残余住在「历史被盒夹过」的像素上（α 调多小都没用）";
    case RejectBranch::Refute:
        return "否证：0 拒收档与全帧同率 ⟹ 残余 = EMA 泄漏，与邻域盒无关";
    case RejectBranch::Indeterminate:
        return "分不出";
    }
    return "分不出";
}

int bandClipProxyByte(double lin)
{
    // 绿道：灰阶输入下三通道取整后同值（ACES 的通道串扰在灰输入下不改变亮度序），而绿是
    // Rec.709/601 的主权重。闭式出处 = `toBytes`（与 shader 逐式对应的独立第二实现）。
    const auto b = toBytes({lin, lin, lin}, 1.0);
    return b[1];
}

namespace {

/// 折叠 + swing 的**唯一实现**（线性 luma 判据与显示空间报告共用同一份代码，杜绝两套口径漂移）。
/// `at(i, y, x)` 取第 i 帧在 (y,x) 的值并折成 double。
struct SwingCore {
    double mean = 0.0, p999 = 0.0, mx = 0.0, fracOver = 0.0;
    double halfGap = 0.0, halfGapMax = 0.0, meanValue = 0.0;
    /// 「同相位两半差 > τ」的**像素数**。⚠ 只有 `halfGapMax` 是不够的：max 分不出
    /// 「1 个像素抖 0.9」与「1 万个像素抖 0.9」，而这两者的结论完全相反（一个是 PNG 平局翻转
    /// 噪声地板，一个是真机制）。2026-10-06 实测踩过：臂 7 的半间差max 冲到 0.91，没有计数就只能猜。
    int64_t halfGapOverPx = 0;
    int64_t pixels = 0;
};

template <class At>
SwingCore swingFoldCore(const cv::Size& sz, const cv::Mat& m, double tau, int periods, At&& at,
    cv::Mat* swingOut = nullptr)
{
    SwingCore s;
    const int perHalf = periods / 2;
    std::vector<double> v0(kResidualPeriod), v1(kResidualPeriod), v(kResidualPeriod);
    std::vector<double> swings;
    swings.reserve(static_cast<size_t>(std::max<int64_t>(1, cv::countNonZero(m))));
    double sumSwing = 0.0, sumVal = 0.0, halfSum = 0.0;
    int64_t overTau = 0;
    if (swingOut)
        *swingOut = cv::Mat::zeros(sz, CV_64F);  // mask 外保持 0（区域纪律在图上同样成立）
    for (int y = 0; y < sz.height; ++y) {
        const uchar* pm = m.ptr<uchar>(y);
        for (int x = 0; x < sz.width; ++x) {
            if (!pm[x])
                continue;
            double colSum = 0.0;
            double pxGapMax = 0.0;  // 本像素跨 16 相位的最大「两半同相位差」
            for (int p = 0; p < kResidualPeriod; ++p) {
                double a = 0.0, b = 0.0;
                for (int k = 0; k < perHalf; ++k) {
                    a += at(static_cast<size_t>(p + k * kResidualPeriod), y, x);
                    b += at(static_cast<size_t>(p + (perHalf + k) * kResidualPeriod), y, x);
                }
                v0[p] = a / perHalf;
                v1[p] = b / perHalf;
                v[p] = 0.5 * (v0[p] + v1[p]);
                colSum += v[p];
                const double g = std::abs(v0[p] - v1[p]);
                halfSum += g;
                s.halfGapMax = std::max(s.halfGapMax, g);
                pxGapMax = std::max(pxGapMax, g);
            }
            if (pxGapMax > tau)
                ++s.halfGapOverPx;
            // 16 个数的中位数 = 排序后第 8、9 位取平均。nth_element(nth=+8) 保证 *mid 是
            // 第 9 小、且 [begin,mid) 全 ≤ 它 ⟹ 第 8 小 = 前半段的**最大**（不是最小）。
            const auto mid = v.begin() + kResidualPeriod / 2;
            std::nth_element(v.begin(), mid, v.end());
            const double med = 0.5 * (*mid + *std::max_element(v.begin(), mid));
            const double swing = *std::max_element(v.begin(), v.end()) - med;
            swings.push_back(swing);
            sumSwing += swing;
            sumVal += colSum / kResidualPeriod;
            if (tau > 0.0 && swing > tau)
                ++overTau;
            s.mx = std::max(s.mx, swing);
            if (swingOut)
                swingOut->at<double>(y, x) = swing;
        }
    }
    s.pixels = static_cast<int64_t>(swings.size());
    if (swings.empty())
        return s;
    const double n = static_cast<double>(swings.size());
    s.mean = sumSwing / n;
    s.meanValue = sumVal / n;
    std::sort(swings.begin(), swings.end());
    s.p999 = percentileOf(swings, 0.999);
    s.fracOver = static_cast<double>(overTau) / n;
    s.halfGap = halfSum / (n * kResidualPeriod);
    return s;
}

}  // namespace

ResidualStat phaseSwingStat(const std::vector<Frame>& shots, const cv::Mat& mask, double tau,
    int periods, cv::Mat* swingMap)
{
    ResidualStat s;
    // phaseAverageLuma 顺带把三条前提钉死：张数 = 整数个周期、尺寸一致、已 prepareLinear。
    const cv::Mat avg = phaseAverageLuma(shots, periods);
    s.frames = kResidualPeriod * periods;
    s.periods = periods;
    s.tau = tau;
    cv::Mat m;
    if (mask.empty())
        m = cv::Mat(avg.size(), CV_8UC1, cv::Scalar(255));
    else
        m = mask;
    s.maskPixels = cv::countNonZero(m);
    if (s.maskPixels == 0)
        return s;  // 无定义：不给「空区域测得 0」出数的机会（区域纪律）

    const auto c = swingFoldCore(avg.size(), m, tau, periods,
        [&shots](size_t i, int y, int x) { return shots[i].luma.at<double>(y, x); },
        swingMap);
    s.meanSwing = c.mean;
    s.p999Swing = c.p999;
    s.maxSwing = c.mx;
    s.fracOverTau = c.fracOver;
    s.halfGap = c.halfGap;
    s.halfGapMax = c.halfGapMax;
    s.halfGapOverPx = c.halfGapOverPx;
    s.meanLuma = c.meanValue;
    return s;
}

// ═══════════════════════════════════════════ 采集 + 判据

namespace {

// ── 靶机位（与 temporal.cpp 的 kCamPos / `rgl/aa_target.cppm` 头注释推荐机位同源）─────────
constexpr double kFovDeg = 45.0;
constexpr int kSize[2] = {1280, 720};
const Engine::Camera kTargetCamera{
    {0.0, 0.0, 2.0}, {0.0, 0.0, -1.0}, kFovDeg};

// ── 阈值（**先测后定**，2026-10-05 首轮实测；每条理由同步进 judge.cpp 注册表）─────────────
//
// 实测表（aa_target 1280×720，periods=2，warm-multiplier=6，variant=forward；单位 = 线性 luma；
// 斜体组 = 同一次运行内的参照。两轮独立运行差 ≤0.3%，故**最小余量取 3%**，见头注释「可复现性」）：
//
//   组（meanSwing / frac(swing>τ)）    门关 = 交付默认          门开 = --base-space
//     R1 fb=1  js=1（阳性锚点）         0.021982 /  3.88%        0.023909 / 13.04%
//     R2 fb=.12 js=1（现行工作点）      0.022600 / 18.09%        0.004459 / 12.09%
//     R4 fb=.002 js=1（分辨力轴）       0.015783 / 15.12%        0.000218 /  0.22%
//     R0 TAA 关、R3 js=0               两条**恰为 0**（无容差，不进表）
//   梯度能量比 R2：门关 0.887–0.893 / 门开 0.966–0.968；显示空间同口径 = 0.768–0.771（红线 1 的
//   代价，实测非引用）
//   「完全不平均的单帧」参照：门关 1.031–1.083 / 门开 1.004。⚠ 它取连拍**末帧**，末帧的抖动相位
//   随 burst 起点浮动 ⟹ 5 次运行跨 5%（上面那句「≤0.3%」只对被判的四个数成立，不成立在这个参照上）
//   —— 这正是它只打印不判的原因，也是 ⑦ 的上限只能取 1.02（门关退化下沿 1.031 之下）的原因。
//   τ = 0.006732（白电平 0.617207 = byte 206 处 1 个显示 LSB）；mask = 246045 / 245901 px
//
// 阈值的取舍纪律：**能用闭式就不用实测数**。锚点下限写成 τ 的倍数（τ 由场景白电平现算，换靶
// 自动跟着走），只有「工作点残差落在哪个带」这种没有闭式可推的量才直接写实测数 + 余量。
constexpr double kAnchorMeanSwingTau = 1.0; ///< R1 meanSwing 下限 = 1 个显示 LSB@白电平
constexpr double kAnchorFracFloor = 0.01;   ///< R1 frac(swing>τ) 下限（实测 3.88%/13.04%）
/// R2 工作点**信号在场带**（不是可见性线，见 ⑤ 的容差理由）。门关实测 0.022600：
/// 下限 0.010 = 实测的 44%（信号消失才算红），上限 0.030 = 实测 +33%（回归才算红）。
constexpr double kMagnitudeMeanLo = 0.010, kMagnitudeMeanHi = 0.030;
/// 门开态（`taa.base_space`）整幅游动被消掉，残差只剩闪烁 ⟹ 读数低 5.1×，带跟着下移。
/// 实测 0.004459：带 [0.002, 0.010] = 实测的 45% / 2.2×。
constexpr double kMagnitudeMeanLoBase = 0.002, kMagnitudeMeanHiBase = 0.010;
constexpr double kMagnitudeFracCap = 0.30; ///< R2 frac 上限（实测 18.09%/12.09%）
constexpr double kMonotoneCap = 0.90;      ///< R4/R2 meanSwing 比上限（实测 0.698/0.049）
constexpr double kGradRatioLo = 0.85;      ///< 收敛正确性下界（实测 0.8925/0.9683）
constexpr double kGradRatioHi = 1.02;      ///< 上界（门关退化态 1.047；门开退化态 1.004 → 见理由）

// ── 变异记录（2026-10-05，每条判据至少一条变异能打红；「测试通过」≠「测试有效」）─────────
//
//   变异（临时改本文件，跑完还原）              红掉的判据                        仍然绿的判据
//   M1 折叠配对 `p+k·16` → `p+k`（连续帧）      selftest 3 条（swing 1→0.75）      —
//   M2 中位数 → 最小值                          selftest 2 条（0.75→1.0）          —
//   M3 `prepareLinear` 不做 sRGB 解码（红线 1）  ⑦ 收敛 0.769 < 0.85               ①②③④⑤
//   M4 `phaseSwingStat` 忽略 mask               selftest 2 条                      —
//   M5 半间差容差去掉量化项                     ② 折叠 0.0066 > 0.00017            其余全绿
//   M6 抖动周期 16 → 8                          ② 折叠（R1 半间差 0 → **0.617**）  ③④⑤⑦ **全绿**
//   M7 锚点组 R1 的 jitter_scale 1 → 0          ③ 锚点（meanSwing 恰为 0）+ ⑦     ①②④⑤
//   M8 反向组 R3 的 jitter_scale 0 → 1          ④ 反向（0.0226 ≠ 0）               其余全绿
//   M9 工作点 R2 的 fb 0.12 → 0.002             ⑥ 单调（比值 1.001）+ ⑦ 0.668      ①②③④⑤
//   M10 区域外扩 3px → 内缩 3px                 **无**（见下面那条诚实边界）        全部
//   M11 区域退回全图（丢掉 objectID）           ③ 锚点 0.0059 + ⑤ 低于下限          ①②④⑦
//
// 三条读法：
//  · **M6 只有 ② 抓得到**（③④⑤⑦ 全绿）——折叠前提这条判据不是装饰，它是本命令唯一的
//    「连拍还是不是抖动相位的确定性函数」的守卫。
//  · **M11 只有 ③⑤ 的下限抓得到**（相对口径 ②④⑦ 全绿）——这就是「下限必须有」的实证：
//    区域被背景稀释时，所有比值都还好看。⚠ 也正因如此，⑤ 的下限**不是**质量线（M9 那种
//    「真的变好了」的改动它不红，M11 这种「量具坏了」它才红）。
//  · ⚠ **诚实边界（M10）**：区域口径本身**没有**判据——外扩改内缩 3px 后读数 0.0226 → 0.0168
//    仍在带内，全绿。本命令只守「区域为空 / 画面全黑」（守卫会抛）与「区域与像素错位」
//    （① 确定性 + ③ 锚点），不守「外扩半径是不是 3」。改半径 = 改口径 = 本文件的实测表与
//    七个阈值一并作废重写，不是调参。

std::string d6(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}
std::string d4(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}
std::string d2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}
/// 科学计数（守卫行用：`== 0` 判据的"实际是多少"必须在 6 位小数下显示为 0 时仍可判读）
std::string dExp(double v)
{
    char b[24];
    std::snprintf(b, sizeof b, "%.3e", v);
    return b;
}

/// 任一通道不同的 8UC1 掩码（同 judge.cpp 的教训：别用 cv::reduce 合通道）。
cv::Mat anyChannelDiff(const cv::Mat& a, const cv::Mat& b)
{
    cv::Mat d;
    cv::compare(a, b, d, cv::CMP_NE);
    std::vector<cv::Mat> ch;
    cv::split(d, ch);
    cv::Mat any = ch[0];
    for (size_t i = 1; i < ch.size(); ++i)
        cv::bitwise_or(any, ch[i], any);
    return any;
}

struct Run {
    std::string name;
    bool taa = false;
    double feedback = 0.0, jitterScale = 0.0;
    int warm = 0;
    ResidualStat stat;
    cv::Mat avg;       ///< 相位平均图（线性 luma）——收敛正确性判据的输入
    double grad = 0.0; ///< 相位平均图的梯度能量
    double byteGrad = 0.0; ///< **显示空间**同口径的梯度能量（只打印：红线 1 的反面教材，实测非引用）
    double gradSingle = 0.0; ///< 收敛段**单帧**的梯度能量（只打印：= 「完全不平均」的参照）
    double whiteLuma = 0.0;  ///< mask 内 luma 的 p99 = 靶白电平（τ 从它来）
    double maxLuma = 0.0;    ///< mask 内 luma 的**最大**（半间差量化项从它来，见 maxLsbStepUpTo）
    double quant = 0.0;      ///< 不亮于 maxLuma 的像素翻一格的最大真实跨度 = 半间差的量化项
    /// **只报不判**的显示空间同口径读数（8bit 灰，同一份折叠代码）。留它的唯一理由：
    /// 红线 1 说统计必须在线性域做，而"显示域差多少"是排查者与历史读数（phase13 §1.5 的
    /// 梯度比 0.671 等）通用的单位。两个域同时打印，混淆当场可见，不用靠注释提醒。
    double byteMeanSwing = 0.0, byteFrac = 0.0, byteMax = 0.0, byteP999 = 0.0;
    double byteMaxVsFirst = 0.0; ///< 与第 1 帧的逐字节最大偏离（确定性地板）
    double changedVsFirst = 0.0; ///< 与第 1 帧任一通道不同的像素占比（跨帧取最大）
};

/**
 * 一组采集。配置一律走 `makeProfile`（红线 3：scene.lua 的 `environment` 段由它写，缺段
 * aa_target 整帧全黑而残差恰好「完美为 0」= 最阴的假绿）。
 *
 * 热身走 `Engine::advanceFrames`（读 RPC 帧计数空转，**不写盘**）：α=0.002 要 3000 帧，
 * 用 `/shotburst` 热身就是写 3000 张 PNG（实测直接超时）。
 */
Run runGroup(const std::string& dir, const std::string& name, const std::string& tag, bool taa,
    double feedback, double jitterScale, const std::string& variant, int periods,
    double warmMult, double& tau, bool baseSpace, bool staticBoost = false,
    double staticFloor = 0.01, const ClipArm& arm = {})
{
    Run r;
    r.name = name;
    r.taa = taa;
    r.feedback = feedback;
    r.jitterScale = jitterScale;

    ProfileOpts o;
    o.entitiesLua = "{ type = \"aa_target\" }";  // 内存合成靶，无 glb 文件
    o.envIntensity = 0.0;                        // 背景 = 常量 clear 色，零时域噪声
    o.skybox = false;
    o.exposure = 1.0;
    o.aoStrength = 0.0;   // strength=0 逐位旁路，不给残差添第二来源
    o.shadowPass = false;
    o.picking = true;     // object_id = 区域的唯一出处
    o.taa = taa;
    o.taaFeedback = feedback;
    o.taaJitterScale = jitterScale;
    // 批 13.2 的门 `taa.base_space`：开 = 输出搬回**基空间**（引擎按内容偏移重采样，整幅
    // 游动被消掉）。本命令用它做**归因**而非判据：门关读数 = 游动 + 闪烁，门开读数 ≈ 只剩
    // 闪烁。差多少 = 绝对残差里有多少是「输出住在抖动空间」这个已登记机制（`temporal-swim-pixels`
    // 说过累积压不掉它），而不是"TAA 没干活"。
    o.taaBaseSpace = baseSpace;
    // 批 13.7 门 `taa.static_boost`（本帧权重地板；⚠ 调小 = 更信历史）。静止机位是本命令
    // 的固有条件（机位一次钉死）⟹ 门开态热身跑满 ramp 后 feedback 恒 = staticFloor。
    o.taaStaticBoost = staticBoost;
    o.taaStaticFeedback = staticFloor;
    // 批 13.11 消融臂透传到合成靶：「修法不得是 bistro 专用」这条护栏要求**同一臂在
    // aa_target 上同向**，所以靶侧也必须能开臂（0 = 门关 = 不写键 = 逐位今天）。
    o.taaClipMode = arm.mode;
    o.taaClipSoft = arm.soft;
    o.variant = variant;
    o.size = {kSize[0], kSize[1]};
    makeProfile(dir, o);

    // 热身 = ⌈warmMult/α⌉ 向上对齐到抖动周期。α = feedback（EMA 记忆长度 1/α）；TAA 关时没有
    // 历史，一个周期纯走稳采集本身。门开态 α = **地板**（爬满后生效 feedback = static_floor，
    // 按 0.12 热身 = 热身不足 = 把画面冻在瞬态上读「更好」的假绿）。⚠ 热身不足**不会报错**，
    // 所以这里宁可多热。
    const double alpha =
        taa ? std::clamp(staticBoost ? staticFloor : feedback, 1e-6, 1.0) : 1.0;
    const int warmRaw = static_cast<int>(std::ceil(warmMult / alpha));
    r.warm = ((warmRaw + kResidualPeriod - 1) / kResidualPeriod) * kResidualPeriod;

    const int frames = kResidualPeriod * periods;
    Engine eng(dir);
    eng.setCamera(kTargetCamera);  // 机位一次钉死，全程不动（唯一变化 = jitter 相位）
    eng.advanceFrames(static_cast<uint32_t>(r.warm));
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(frames), tag + "_c");

    std::vector<Frame> shots;
    shots.reserve(files.size());
    std::vector<cv::Mat> gray;  ///< 显示空间报告用的 8UC1 灰（判据不读它，见 Run 注释）
    gray.reserve(files.size());
    cv::Mat first;
    for (size_t i = 0; i < files.size(); ++i) {
        Frame f;
        f.camera = kTargetCamera;
        f.bgr = readPngBgr(files[i]);
        if (i == 0)
            first = f.bgr.clone();
        else {
            cv::Mat d;
            cv::absdiff(first, f.bgr, d);
            r.byteMaxVsFirst = std::max(r.byteMaxVsFirst, cv::norm(d, cv::NORM_INF));
            r.changedVsFirst = std::max(r.changedVsFirst,
                static_cast<double>(cv::countNonZero(anyChannelDiff(first, f.bgr)))
                    / static_cast<double>(f.bgr.total()));
        }
        cv::Mat g;
        cv::cvtColor(f.bgr, g, cv::COLOR_BGR2GRAY);
        gray.push_back(std::move(g));
        prepareLinear(f);
        // 只留 luma：32 帧 × (bgr 2.7MB + lin 22MB) 是 790MB 的白烧内存，而本判据的每一个
        // 统计都只吃线性 luma（红线 1）；字节面在上面用完（确定性地板）就该放手。
        f.bgr.release();
        f.lin.release();
        shots.push_back(std::move(f));
    }

    const cv::Mat ids = decodeObjectId(readPngBgr(eng.shot(tag + "_id", "object_id")));
    cv::Mat interior;
    cv::compare(ids, 0, interior, cv::CMP_NE);  // ids 是 CV_32SC1 ⟹ 直接 8UC1
    const cv::Mat mask = dilateMask(interior, 3);  // 3px：吸收 jitter ±0.5px 的相位漂移
    r.avg = phaseAverageLuma(shots, periods);
    auto whiteVals = valuesInMask(r.avg, mask);
    std::sort(whiteVals.begin(), whiteVals.end());
    r.whiteLuma = percentileOf(whiteVals, 0.99);
    // 量化项走 **max**、τ 走 **p99**，两个电平故意分开（理由见 maxLsbStepUpTo 的头注释：容差要
    // 覆盖最亮那个像素翻一格，可见性阈值要的是靶白面的一格）。
    r.maxLuma = whiteVals.empty() ? 0.0 : whiteVals.back();
    r.quant = maxLsbStepUpTo(r.maxLuma);
    // τ 是**场景**属性（靶白电平处 1 个显示 LSB），不是每组各自的属性 ⟹ 只在第一组算一次，
    // 之后原样传下去。跨组换 τ = `frac` 的跨组比较失去意义 = 单调性轴作废。
    if (tau <= 0.0)
        tau = lsbLinearAtLuma(r.whiteLuma);
    r.stat = phaseSwingStat(shots, mask, tau, periods);
    // 显示空间同口径（**只报不判**，见 Run 注释）：同一份折叠代码，τ 换成「1 个显示 LSB」。
    const auto bc = swingFoldCore(r.avg.size(), mask, 1.0, periods,
        [&gray](size_t i, int y, int x) { return static_cast<double>(gray[i].at<uchar>(y, x)); });
    r.byteMeanSwing = bc.mean;
    r.byteP999 = bc.p999;
    r.byteFrac = bc.fracOver;
    r.byteMax = bc.mx;
    r.grad = gradEnergy(r.avg, mask);
    // **显示空间**同口径梯度（先 tonemap 再平均 = 红线 1 禁止的那个口径）。算出来只为打印：
    // 让「域选错会把 0.89 读成多少」是实测数，不是从文档里抄来的数。
    {
        cv::Mat acc = cv::Mat::zeros(r.avg.size(), CV_64FC1), tmp;
        for (const auto& g : gray) {
            g.convertTo(tmp, CV_64F);
            cv::add(acc, tmp, acc);
        }
        acc /= static_cast<double>(gray.size());
        r.byteGrad = gradEnergy(acc, mask);
    }
    r.gradSingle = gradEnergy(shots.back().luma, mask);
    // 在场性守卫（红线 3 的兜底）：全黑画面的残差**恰好为 0**，四条判据会全绿。
    if (r.stat.meanLuma <= 0.0 || r.stat.maskPixels == 0)
        throw std::runtime_error(name + "：画面是黑的（meanLuma=" + d6(r.stat.meanLuma)
            + "，mask " + std::to_string(r.stat.maskPixels) + " px）——配置丢了 environment 段？"
              "全零的残差不是「稳定」，是「没画」");
    return r;
}

void printRow(const Run& r)
{
    std::printf("  %-24s 热身 %5d  meanSwing %-9s p99.9 %-9s max %-8s frac>τ %5.2f%%  "
                "半间差 %8s/%-8s **超τ %lldpx**  与首帧 %2.0fLSB/%5.2f%%\n",
        r.name.c_str(), r.warm, d6(r.stat.meanSwing).c_str(), d6(r.stat.p999Swing).c_str(),
        d6(r.stat.maxSwing).c_str(), r.stat.fracOverTau * 100.0, d6(r.stat.halfGap).c_str(),
        d6(r.stat.halfGapMax).c_str(), static_cast<long long>(r.stat.halfGapOverPx),
        r.byteMaxVsFirst, r.changedVsFirst * 100.0);
    std::printf("  %-24s 显示域（只报不判）meanSwing %-7s p99.9 %-6s max %-5s frac>1LSB %5.2f%%\n",
        "", d4(r.byteMeanSwing).c_str(), d2(r.byteP999).c_str(), d2(r.byteMax).c_str(),
        r.byteFrac * 100.0);
}

}  // namespace

// ── 批 13.7 `--static-boost <floor>` 变体：**只出数不判** ──────────────────────────
//
// 为什么不判（任务红线）：判据 ⑤（temporal-residual-magnitude）是**带**，其下限红 =
// 「信号消失向」，作者刻意设计成「修复落地后应当变红」——拿 boost 后的低残差去撞门关
// 口径的带，会把「变好了」判成「不合格」。默认判据保持门关口径（7 条不变），本变体出
// 三档地板的 meanSwing / p99.9 / frac>τ 与收敛正确性（梯度比），选档由人判：
// **梯度比 ∈ [0.85, 1.02]（与 ⑦ 同带）前提下取 meanSwing 最低档**。
//
// 四组（只差被测项，全部 aa_target 细线靶 + 静止机位）：
//   B1 fb=1  js=1 门关 → 梯度参考（理想 16 相位平均，**必须门关**：boost 会把 fb=1 也
//      压到地板，「只抖不滤」的参考语义就没了——这是它与 R1 的唯一区别）
//   B2 fb=.12 js=1 门关 → 门关对照（= R2 口径）
//   B3 fb=.12 js=1 门开 → 被测（热身按地板 α，见 runGroup）
//   B4 fb=.12 js=0 门开 → 反向哨兵：无激励源时 boost 必须**恰为 0**（boost 只改权重值，
//      不该引入任何第二时域源；非零 = boost 接错了地方）
namespace {

int runStaticBoostSweep(const std::string& root, int periods, double warmMult,
    const std::string& variant, bool baseSpace, double floorVal, const ClipArm& arm = {})
{
    double tau = 0.0;
    std::vector<Run> runs;
    try {
        runs.push_back(runGroup(root + "/imgqa_resb_b1", "B1 fb=1  js=1(梯度参考)", "resbb1",
            true, 1.0, 1.0, variant, periods, warmMult, tau, baseSpace));
        runs.push_back(runGroup(root + "/imgqa_resb_b2",
            "B2 fb=.12 js=1 门关(对照)", "resbb2", true, 0.12, 1.0, variant, periods, warmMult,
            tau, baseSpace, false, 0.01, arm));
        runs.push_back(runGroup(root + "/imgqa_resb_b3",
            "B3 fb=.12 js=1 门开 fb=" + d4(floorVal), "resbb3", true, 0.12, 1.0, variant, periods,
            warmMult, tau, baseSpace, true, floorVal, arm));
        runs.push_back(runGroup(root + "/imgqa_resb_b4", "B4 fb=.12 js=0 门开(反向)", "resbb4",
            true, 0.12, 0.0, variant, periods, warmMult, tau, baseSpace, true, floorVal, arm));
    } catch (const std::exception& e) {
        std::printf("  采集失败: %s\n", e.what());
        return 2;
    }
    const Run& B1 = runs[0];
    const Run& B2 = runs[1];
    const Run& B3 = runs[2];
    const Run& B4 = runs[3];

    std::printf("static-boost 变体（**只出数不判**）：aa_target %d×%d  静止机位  折叠 (2,%d,H,W)  "
                "周期数 %d  地板 static_feedback=%s  base_space=%s\n",
        kSize[0], kSize[1], kResidualPeriod, periods, d4(floorVal).c_str(),
        baseSpace ? "开（交付态）" : "关");
    std::printf("  ⚠ 判据 ⑤ 的带是门关口径（下限红 = 信号消失向，修复落地后**应当**变红），"
                "本变体不判它；选档判据 = 梯度比 ∈ [0.85, 1.02]（同 ⑦）前提下 meanSwing 最低。\n");
    std::printf("  区域 = objectID 靶内部外扩 3px（%lld px）；τ = %s（B1 定，各组共用）\n\n",
        static_cast<long long>(B2.stat.maskPixels), d6(tau).c_str());
    // ⚠ **口径守卫（2026-10-06 实测踩过）**：本变体的 `base_space` 默认**关**，而交付态是**开**。
    //   关掉时历史住在抖动空间 ⟹ 残差被「每帧亚像素重采样历史」的复利糊化（批 13.1a/13.5 已修机制）
    //   主导，它会盖过、甚至**反向扭曲**裁剪臂的效应：同一对臂在关/开两口径下，`frac>τ` 与梯度比的
    //   好坏方向**相反**（关：frac 17.9%→30.4% 变差、梯度比 0.722→0.596 变糊；开：2.01%→0.03% 变好、
    //   0.932→1.011 变锐）。跨口径比较无效，故只要「消融臂 ≠ 0 且 base_space 关」就出声。
    if (!baseSpace && arm.mode > 0.5)
        std::printf("  🔴 **口径警告**：base_space=**关**（非交付态）却带着消融臂 mode=%d —— 本表量的"
                    "主要是复利糊化，不是裁剪效应，方向可能是**反的**。判修法请用 `--base-space`。\n",
            static_cast<int>(arm.mode + 0.5));
    for (const auto& r : runs)
        printRow(r);

    const double gradRef = B1.grad;
    const auto ratio = [gradRef](double g) { return gradRef > 0.0 ? g / gradRef : 0.0; };
    std::printf("\n  收敛正确性（梯度能量比，线性域；参考 = B1 fb=1 相位平均 = %s）\n",
        d6(gradRef).c_str());
    std::printf("    B2 门关 比值 %s   B3 门开 比值 %s（选档前提：B3 ∈ [0.85, 1.02]）\n",
        d6(ratio(B2.grad)).c_str(), d6(ratio(B3.grad)).c_str());
    std::printf("    B4 反向（门开 + 零抖动）meanSwing %s  maxSwing %s（期望**恰为 0**，"
                "非零 = boost 引入了第二时域源，本组读数作废）\n",
        d6(B4.stat.meanSwing).c_str(), d6(B4.stat.maxSwing).c_str());

    logCall("temporal-residual-static-boost",
        {{"periods", std::to_string(periods)}, {"warmMult", d4(warmMult)},
            {"variant", variant.empty() ? "forward" : variant},
            {"floor", d4(floorVal)}, {"baseSpace", baseSpace ? "1" : "0"},
            {"tau", d6(tau)}, {"B2_swing", d6(B2.stat.meanSwing)},
            {"B3_swing", d6(B3.stat.meanSwing)}, {"B3_p999", d6(B3.stat.p999Swing)},
            {"B3_frac", d2(B3.stat.fracOverTau * 100.0)},
            {"B2_p999", d6(B2.stat.p999Swing)}, {"B2_frac", d2(B2.stat.fracOverTau * 100.0)},
            {"B3_gradRatio", d6(ratio(B3.grad))}, {"B2_gradRatio", d6(ratio(B2.grad))},
            {"B4_swing", d6(B4.stat.meanSwing)}},
        {});
    return 0;
}

}  // namespace

// ── 批 13.7b `--bistro` 变体：**真实场景** bistro（债-48 主体，**只出数不判** + 两条量具守卫）──
//
// 为什么必须有这条（一句话版）：合成靶 aa_target 是 LDR 黑白细杆，交付档 `frac>64LSB` 本来就是
// 0，症状幅度比 bistro 的 HDR 高亮细线低一个量级 ⟹「meanSwing 降 7.2×」推不出「门框缝隙没了」。
// 本变体在**交付配置**（deferred + env/skybox/IBL/AO/shadow 全开）+ **用户报障机位**上重测
// 门关 vs 门开（`taa.static_boost`）的绝对残差，并顺带出债-47：`phaseAverageLuma` 梯度能量比
// 的**线性域**读数（`docs/phase13_plan.md` §1.5 的 0.671 是显示空间旧账，本处只出数、改判由
// 主 agent 记账，本工具不动那份文档）。
//
// 与 aa_target profile 的三处口径差异（每处都是被迫的，读数字前必读）：
//   ① 配置 = `deliveryConfigDir` 模式（红线 4：合成副本不覆盖 env/skybox 路径，bistro 的 HDR
//      高亮细线正是本症状的激励源）。variant 留空 = 尊重交付的 deferred；`base_space` **钉真**
//      ——引擎默认 true（`scene_config.cppm::SceneConfig::taaBaseSpace`，批 13.5 起 = 交付态），
//      而 `ProfileOpts::taaBaseSpace` 默认 false 是批 13.2 的旧口径且 `rewriteTaaBlock` 会
//      **显式写键**，不钉 = 静默换场景。
//   ② 区域 = **全帧**：交付配置 `diagnostics.picking = false`（object_id pass 不在图里），
//      打开它 = 改图形态 = 不再是交付路径。aa_target 的「objectID 内部外扩 3px」是为排除
//      常量 clear 背景；bistro 画面几乎被几何填满，全帧就是「交付了什么」的最直白区域。
//      ⚠ 因此 bistro 的数与 aa_target 的数**不可直接比**（区域不同），可比的是算子、域、τ 定义。
//   ③ 机位 = `ProfileOpts::hasCamera` 写进 profile 的 camera.lua（**第 1 帧生效**；RPC 的
//      `setCamera` 只是幂等重发，理由全文见 `ProfileOpts::hasCamera` 注释）。
//
// 每机位五组（只差被测项；编号 N 与 aa_target 的 R/B 错开防混读；N5 为后补的归因对照）：
//   N1 fb=.12 js=1 门关   → 被测（用户现状）+ **定 τ**（五组共用）+ 阳性锚点的载体
//   N2 fb=.12 js=1 门开   → 被测（`static_boost`，地板 = `--boost-floor`，默认 0.01）
//   N3 fb=.12 js=0 门开   → **反向**：js=0 + 静止机位 ⟹ 首帧起历史 = 本帧（EMA 不动点，
//        与 α 无关）⟹ 输出逐帧逐位相同 ⟹ 残差必须**恰为 0**；非零 = boost 接错地方或引擎
//        有第二时域源，该机位 N1/N2 的读数一并作废
//   N5 fb=.12 js=0 门关   → 反向**对照**（α=.12 无 boost）：N3 非零时用它归因——doorframe
//        实测 bistro 在 js=0 有 8px×1LSB 的奇偶帧交替地板（与 boost 无关；机制候选 =
//        jitterPx=(halton−0.5)·0 的**零符号**随相位翻，详见 G2 注释）
//   N0 TAA 全关           → **确定性地板**（`determinism` 在 bistro 交付配置上的实例化）：
//        连拍必须逐位相同；非零 ⟹ 非确定性在 TAA 之外，全表读数存疑（G0，见 runBistroCamera）
//   N6（--diag-bs-off）   → js=0 门开但 base_space 关：把 N3 的翻转归因到 base_space 路径还是
//        jitter 注入/裁剪盒等公共路径
//   N4 fb=1  js=1 门关   → 梯度参考（理想 16 相位平均；**必须门关**——boost 会把 fb=1 也压到
//        地板，「只抖不滤」的参考语义就没了，同 B1 的理由）
//
// 两条守卫（不是质量判据，是「量具还活着吗」；质量结论本轮**只出数不判**，口径同
// `--static-boost` 变体先例——判据 ⑤ 的带是门关口径，拿门开读数撞它 = 把「变好」判成「不合格」）：
//   G1 阳性锚点：doorframe 门关 frac(swing>τ) ≥ 1%（与 aa_target 锚点同常量 `kAnchorFracFloor`）。
//      用户肉眼看得见闪烁是既成事实，量出来接近 0 = 量具坏了 ⟹ **立刻停下**（rc=2），不跑其余机位。
//   G2 反向：N3 的 meanSwing / maxSwing **恰为 0**（无容差，机制见上）。
//   半间差 halfGapMax 与闭式容差 (1−α)^warm×满幅+量化项 只打印作**热身自证**（bistro 全帧 p99
//   白电平高于合成靶，该容差在这里偏保守；超了打 ⚠ 不判红——它防的是「量到瞬态」，而 N3 恰 0
//   已经排除了「连拍不是相位的确定性函数」里最贵的那类失效）。
//
// 批 13.7c 追加**亮度 × 梯度二维分带**（每机位一张门关表 + 一张门开表 + 一张差表，每格三个数：
// share / rate>τ / 残余质量占比；另附阈值与梯度轴的敏感性）。它要回答的是现有分带口径答不了的
// 问题——所有现有测量只按梯度分带，而梯度分不清「几何边」与「高亮边」。要判的假设、两根轴的
// 口径与定阈值的规则（闭式反解显示链 + 实测直方图门槛）、⭐ 判决线（**跑前写死**）全文 =
// `temporal_residual.h`「亮度 × 梯度 二维分带」段；出表代码在本文件 `reportLumaGradBands`。
// 尺子取 N1（门关 = 用户现状）并**两态共用**，差表里 Δshare 恰为 0 就是这件事的自证。
namespace {

struct BistroRun {
    std::string name;
    int warm = 0;
    double alpha = 0.0;
    ResidualStat stat;
    cv::Mat avg;
    cv::Mat swingMap;  ///< 逐像素 swing（CV_64FC1）——热图与分带表的输入，与 stat 同一份折叠代码写出
    cv::Mat byteMean;  ///< **显示域**字节亮度多帧均值（CV_64FC1，0..255）= 亮度轴的尺子
    cv::Mat byteMax;   ///< 同上取跨帧**最大**（敏感性组：均值把「正在闪的削顶」判出档外的对照）
    cv::Mat gradLin;   ///< 线性 luma 相位平均的梯度图 = 梯度轴的尺子（主口径）
    cv::Mat gradByte;  ///< 显示域梯度图（敏感性组：对「高亮边」更保守的一把尺子）
    double grad = 0.0;      ///< 相位平均图的线性域梯度能量（债-47 的分子/分母）
    double byteGrad = 0.0;  ///< **显示空间**同口径（只打印：§1.5 的 0.671 是这类读数）
    double whiteLuma = 0.0, maxLuma = 0.0, quant = 0.0;
    double byteMaxVsFirst = 0.0, changedVsFirst = 0.0;
    int64_t changedPx = 0;  ///< 与首帧任一通道不同的像素数（跨帧取最大）——1LSB 级失效要**精确数**
    cv::Size frameSize;
};

/**
 * 一组 bistro 采集。红线逐条落点：分辨率自证（红线 1）、热身空转（红线 2）、
 * `prepareLinear` 复用（红线 3）、交付配置（红线 4）。
 *
 * `taa=false` = N0 确定性地板（TAA 整个关，连 jitter 注入都不发生）；`baseSpace=false` 只服务
 * N6 诊断组（把 base_space 的 un-jitter / c_{t−1} 修正摘掉，其余不动）——两者默认值 = 交付态。
 * `wantMaps` = 留下**逐像素图**（swing / 显示域亮度 / 两域梯度）：热图与「亮度×梯度」分带表
 * 要它们，其余组（N0/N3/N4/N5/N6）只出标量读数，不留图省内存。
 */
BistroRun runBistroGroup(const std::string& dir, const std::string& name, const std::string& tag,
    const BistroCam& cam, double feedback, double jitterScale, int periods, double warmMult,
    double& tau, bool staticBoost, double staticFloor, bool wantMaps, bool taa = true,
    bool baseSpace = true, const ClipArm& arm = {})
{
    BistroRun r;
    r.name = name;

    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 红线 4（口径差异①）
    o.hasCamera = true;                                // 机位第 1 帧生效（口径差异③）
    o.cameraPosition = cam.position;
    o.cameraTarget = cam.target;
    o.cameraFov = cam.fov;
    o.taa = taa;
    o.taaFeedback = feedback;
    o.taaJitterScale = jitterScale;
    o.taaBaseSpace = baseSpace;  // 交付态 = 引擎默认 true（口径差异①，勿改默认）
    o.taaStaticBoost = staticBoost;
    o.taaStaticFeedback = staticFloor;
    o.taaClipMode = arm.mode;    // 批 13.11 消融臂（0 = 门关 = 不写键 = 交付配置原样）
    o.taaClipSoft = arm.soft;
    makeProfile(dir, o);

    // 热身 = ⌈warmMult/α⌉ 对齐到抖动周期；门开态 α = **地板**（爬满 ramp 后 feedback 恒 =
    // static_floor，按 0.12 热身 = 把画面冻在瞬态上读「更好」的假绿）。红线 2。TAA 关无历史。
    r.alpha = taa ? std::clamp(staticBoost ? staticFloor : feedback, 1e-6, 1.0) : 1.0;
    const int warmRaw = static_cast<int>(std::ceil(warmMult / r.alpha));
    r.warm = ((warmRaw + kResidualPeriod - 1) / kResidualPeriod) * kResidualPeriod;

    const int frames = kResidualPeriod * periods;
    Engine eng(dir);
    eng.setCamera({cam.position, cam.target, cam.fov});
    eng.advanceFrames(static_cast<uint32_t>(r.warm));  // 空转不写盘（红线 2）
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(frames), tag + "_c");

    std::vector<Frame> shots;
    shots.reserve(files.size());
    std::vector<cv::Mat> gray;  ///< 显示空间报告用（判据不读它，同 Run 的注释）
    gray.reserve(files.size());
    cv::Mat first;
    for (size_t i = 0; i < files.size(); ++i) {
        Frame f;
        f.camera = {cam.position, cam.target, cam.fov};
        f.bgr = readPngBgr(files[i]);
        if (i == 0)
            first = f.bgr.clone();
        else {
            cv::Mat d;
            cv::absdiff(first, f.bgr, d);
            r.byteMaxVsFirst = std::max(r.byteMaxVsFirst, cv::norm(d, cv::NORM_INF));
            const int64_t ch = cv::countNonZero(anyChannelDiff(first, f.bgr));
            r.changedPx = std::max(r.changedPx, ch);
            r.changedVsFirst =
                std::max(r.changedVsFirst, static_cast<double>(ch) / static_cast<double>(
                                                       f.bgr.total()));
        }
        cv::Mat g;
        cv::cvtColor(f.bgr, g, cv::COLOR_BGR2GRAY);
        gray.push_back(std::move(g));
        prepareLinear(f);  // 红线 3：复用，不另写解码
        f.bgr.release();
        f.lin.release();
        shots.push_back(std::move(f));
    }
    // 红线 1 自证：残差是逐像素计数/均值，换分辨率 = 换口径（实测湿面 720p vs 1440p ≈ 4× 像素数）。
    r.frameSize = shots.front().luma.size();
    std::printf("  [%s] 帧 %dx%d（红线 1 自证）\n", name.c_str(), r.frameSize.width,
        r.frameSize.height);
    if (r.frameSize != cv::Size(kSize[0], kSize[1]))
        throw std::runtime_error(name + "：帧尺寸 " + std::to_string(r.frameSize.width) + "x"
            + std::to_string(r.frameSize.height) + " ≠ " + std::to_string(kSize[0]) + "x"
            + std::to_string(kSize[1]) + "——残差口径按 720p 钉，换分辨率即换口径");

    r.avg = phaseAverageLuma(shots, periods);
    auto vals = valuesInMask(r.avg, {});  // 区域 = 全帧（口径差异②）
    std::sort(vals.begin(), vals.end());
    r.whiteLuma = percentileOf(vals, 0.99);
    r.maxLuma = vals.empty() ? 0.0 : vals.back();
    r.quant = maxLsbStepUpTo(r.maxLuma);
    // τ 是**场景×机位**属性（白电平处 1 个显示 LSB）不是组属性 ⟹ N1 定一次，四组共用
    // （跨组换 τ = frac 的跨组比较失去意义，同 aa_target 的纪律）。
    if (tau <= 0.0)
        tau = lsbLinearAtLuma(r.whiteLuma);
    r.stat = phaseSwingStat(shots, {}, tau, periods, wantMaps ? &r.swingMap : nullptr);
    r.grad = gradEnergy(r.avg, {});
    {
        // **显示域**字节亮度多帧均值（先 tonemap 再平均 = 红线 1 禁止的那个**统计**口径）。
        // 它在这里不是统计而是**分带尺子**：亮度轴要的就是「交付面上这块看起来多亮」，
        // 而截图里没有线性 HDR。同一个 acc 顺带给出 byteGrad（§1.5 的 0.671 就是这类读数，
        // 两个域同时出数，域选错的代价当场可见）。
        cv::Mat acc = cv::Mat::zeros(r.avg.size(), CV_64FC1), tmp;
        for (const auto& g : gray) {
            g.convertTo(tmp, CV_64F);
            cv::add(acc, tmp, acc);
        }
        acc /= static_cast<double>(gray.size());
        r.byteGrad = gradEnergy(acc, {});
        if (wantMaps) {
            r.byteMean = acc;
            r.gradLin = sobelAbsGrad(r.avg);   // 梯度轴主口径（与 r.grad 同一算子）
            r.gradByte = sobelAbsGrad(acc);    // 敏感性口径（显示域，对高亮边更保守）
            // 跨帧**最大**：均值把「逐帧闪出 255 的真削顶像素」判出档外（偏保守方向），
            // 最大口径是它的对照。两者结论不一致 = 亮度代理本身不稳，须报「分不出」。
            cv::Mat mx;
            gray.front().convertTo(mx, CV_8UC1);
            for (const auto& g : gray)
                cv::max(mx, g, mx);
            mx.convertTo(r.byteMax, CV_64F);
        }
    }
    // 在场性守卫（红线 4 的兜底）：全黑画面的残差**恰好为 0**，什么判据都会绿。
    if (r.stat.meanLuma <= 0.0)
        throw std::runtime_error(name + "：画面是黑的（meanLuma=" + d6(r.stat.meanLuma)
            + "）——交付配置坏了？全零的残差不是「稳定」，是「没画」");
    return r;
}

// ── 批 13.10：历史拒收 × 残余 的因果交叉表（判「boost 压不住」是不是邻域盒）──────────────
//
// 假设、口径三条、⭐ 判决线（跑前写死）全文 = `temporal_residual.h`「历史拒收 × 残余」段。
// 本处只放采集与出表；分桶与交叉计数是**共用算子**（`rejectFractionOf` / `rejectCrossTab`）。
struct BistroReject {
    std::string name;
    int frames = 0;
    RejectFraction frac{};  ///< 五桶占比（帧均）
    cv::Mat nAny;           ///< 每像素「历史被动过（≠收下）」的帧数（CV_32SC1）
    cv::Mat nCollapse;      ///< 每像素「塌回邻域盒 centroid = 拒收」的帧数
};

/**
 * 一组「历史拒收」采集：与 N1/N2 **只差 `diag_reject` 一个键**（RGB 逐位不变，见
 * `taa.frag.slang` 诊断 alpha 段），连拍取 **taa_out 附件**的 alpha 码。
 *
 * 热身与 α 的口径与 `runBistroGroup` 逐字相同（门开态 α = 地板）——拒收模式**依赖历史状态**，
 * 热身不足读到的是瞬态的盒行为，不是稳态的。
 */
BistroReject runBistroRejectGroup(const std::string& dir, const std::string& name,
    const std::string& tag, const BistroCam& cam, double feedback, int periods, double warmMult,
    double staticFloor, bool staticBoost)
{
    BistroReject r;
    r.name = name;

    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";
    o.hasCamera = true;
    o.cameraPosition = cam.position;
    o.cameraTarget = cam.target;
    o.cameraFov = cam.fov;
    o.taa = true;
    o.taaFeedback = feedback;
    o.taaJitterScale = 1.0;
    o.taaBaseSpace = true;
    o.taaStaticBoost = staticBoost;
    o.taaStaticFeedback = staticFloor;
    o.taaDiagReject = true;  // ← 与被测组唯一的差别（RGB 逐位不变）
    makeProfile(dir, o);

    const double alpha = std::clamp(staticBoost ? staticFloor : feedback, 1e-6, 1.0);
    const int warmRaw = static_cast<int>(std::ceil(warmMult / alpha));
    const int warm = ((warmRaw + kResidualPeriod - 1) / kResidualPeriod) * kResidualPeriod;

    Engine eng(dir);
    eng.setCamera({cam.position, cam.target, cam.fov});
    eng.advanceFrames(static_cast<uint32_t>(warm));
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(kResidualPeriod * periods),
        tag + "_c", "taa_out");

    for (size_t i = 0; i < files.size(); ++i) {
        cv::Mat img = cv::imread(files[i], cv::IMREAD_UNCHANGED);
        if (img.empty() || img.channels() != 4)
            throw std::runtime_error(name + "：taa_out 附件读不到或不是 BGRA: " + files[i]);
        cv::Mat ch[4];
        cv::split(img, ch);
        const RejectFraction f = rejectFractionOf(ch[3]);
        r.frac.accepted += f.accepted;
        r.frac.clipped += f.clipped;
        r.frac.collapsed += f.collapsed;
        r.frac.reproject += f.reproject;
        r.frac.noHistory += f.noHistory;
        cv::Mat m, mi;
        if (i == 0) {
            r.nAny = cv::Mat::zeros(ch[3].size(), CV_32SC1);
            r.nCollapse = cv::Mat::zeros(ch[3].size(), CV_32SC1);
        }
        // ≥32 = 不是「原样收下」（0 码）。掩膜是 0/255 的 CV_8U，**必须换算成 0/1 再累加**
        // （直接 `+=` 会把每帧记成 255 帧，且类型不符当场抛）。
        cv::compare(ch[3], 32, m, cv::CMP_GE);
        m.convertTo(mi, CV_32SC1, 1.0 / 255.0);
        r.nAny += mi;
        // [96,160) = 塌回邻域盒 centroid（0.5 码）= 「历史贡献变成本帧邻域均值」的那一档。
        cv::Mat lo, hi, both;
        cv::compare(ch[3], 96, lo, cv::CMP_GE);
        cv::compare(ch[3], 160, hi, cv::CMP_LT);
        cv::bitwise_and(lo, hi, both);
        both.convertTo(mi, CV_32SC1, 1.0 / 255.0);
        r.nCollapse += mi;
        ++r.frames;
        // 在场性守卫（同交付组）：全黑画面的拒收码照样能读，但那张图不是「交付了什么」。
        cv::Mat gray;
        cv::cvtColor(img, gray, cv::COLOR_BGRA2GRAY);
        if (cv::mean(gray)[0] <= 1.0)
            throw std::runtime_error(name + "：taa_out 画面是黑的——交付配置坏了？");
    }
    if (r.frames == 0)
        throw std::runtime_error(name + "：连拍 0 帧（/shotburst 没出图）");
    // 自检 1（判据工具自己要有自检）：五桶是**划分** ⟹ 之和必须 = 帧数。
    const double sum = r.frac.accepted + r.frac.clipped + r.frac.collapsed + r.frac.reproject
        + r.frac.noHistory;
    if (std::abs(sum - static_cast<double>(r.frames)) > 1e-6)
        throw std::runtime_error(name + "：五桶之和 = " + d6(sum) + " ≠ 帧数 "
            + std::to_string(r.frames) + "——分桶代码坏了，读数无效");
    r.frac.accepted /= r.frames;
    r.frac.clipped /= r.frames;
    r.frac.collapsed /= r.frames;
    r.frac.reproject /= r.frames;
    r.frac.noHistory /= r.frames;
    // 自检 2（阳性锚点）：门没生效 = alpha 恒 1.0 = 全桶「无历史」。历史在热身 16 帧后必然
    // 有效 ⟹ noHistory > 0.5 只可能是 `diag_reject` 键没生效（键名/载荷/alpha 任一环断了）。
    if (r.frac.noHistory > 0.5)
        throw std::runtime_error(name + "：掩膜全桶「无历史」(noHistory = " + d6(r.frac.noHistory)
            + "）——diag_reject 没生效，拒收读数无效");
    return r;
}

/// 出表后的关键读数（进台账，事后复核判决用）。
struct RejectReport {
    bool ok = false;
    double r0 = 0.0, rAll = 0.0, m3p = 0.0, bin0Share = 0.0;
    int branch = 2;  ///< 0 支持 / 1 否证 / 2 分不出
    double collapsed = 0.0, clipped = 0.0;
    // 抑制保真度（闭式锚定主判据；只在门开那次算）
    bool fidelityRan = false, fidelityPass = false;
    double rho = 0.0, rhoStar = 0.0, line = 0.0, bin0Rho = 0.0;
};

/**
 * 出拒收交叉表 + 逐档抑制比 + 执行两条判决线（两条都**跑前写死**在头注释；这里只执行）。
 *
 * @param swingOn  门开态的逐像素 swing（`--reject` 用它做交叉表）
 * @param swingOff 门关态的逐像素 swing（抑制比的分母；同一次采集）
 * @param alphaOn  门开态的**本帧权重**（= `static_feedback`）。**>0 才出抑制比与主判据**
 *                 （门关那次没有「压」这个动作，比无可比）
 * @param alphaOff 门关态的本帧权重（= `feedback`）
 */
RejectReport reportRejectAttribution(const BistroReject& R, const cv::Mat& swingOn,
    const cv::Mat& swingOff, double tau, const std::string& camName, double alphaOn,
    double alphaOff)
{
    std::printf("\n── 历史拒收 × 残余 交叉表（%s｜连拍 %d 帧取 taa_out alpha；τ = %s）──\n",
        camName.c_str(), R.frames, d6(tau).c_str());
    RejectReport rep;
    rep.collapsed = R.frac.collapsed;
    rep.clipped = R.frac.clipped;
    std::printf("  五桶（帧均占比）：收下 %s%% · 盒裁剪 %s%% · **塌回 centroid %s%%** · "
                "重投影拒收 %s%% · 无历史 %s%%\n",
        d2(R.frac.accepted * 100.0).c_str(), d2(R.frac.clipped * 100.0).c_str(),
        d2(R.frac.collapsed * 100.0).c_str(), d2(R.frac.reproject * 100.0).c_str(),
        d2(R.frac.noHistory * 100.0).c_str());
    const RejectCrossTab t = rejectCrossTab(R.nAny, swingOn, tau);
    if (!t.valid()) {
        std::printf("  ✗ 交叉表未出（swing 与拒收图尺寸/类型不符 = 不是同一次采集）\n");
        return rep;
    }
    rep.ok = true;
    static const char* const kBinTag[5] = {
        "0 全程收下", "1-2 偶发", "3-8", "9-16", "17+" };
    const SuppressionTable sup = suppressionByRejectBin(swingOn, swingOff, R.nAny);
    const double n = static_cast<double>(t.total);
    std::printf("  ┌ 每像素「%d 帧里有几帧历史被动过」× swing（ρ = 门开/门关平均 swing）\n",
        R.frames);
    std::printf("    %-13s %9s %9s %10s %10s %10s\n", "档", "share", "rate>τ", "残余质量",
        "Σswing", "ρ 抑制比");
    for (int i = 0; i < 5; ++i) {
        const RejectBinCell& c = t.bin[i];
        std::printf("    %-13s %8s%% %8s%% %9s%% %8s%% %10s%s\n", kBinTag[i],
            d2(100.0 * c.pixels / n).c_str(),
            d2(c.pixels > 0 ? 100.0 * c.over / c.pixels : 0.0).c_str(),
            d2(t.over > 0 ? 100.0 * c.over / t.over : 0.0).c_str(),
            d2(t.mass > 0 ? 100.0 * c.mass / t.mass : 0.0).c_str(),
            sup.valid ? d4(sup.bin[i].rho).c_str() : "n/a",
            i == 0 ? "   ← 纯 EMA 档（ρ 的闭式期望 = ρ*）" : "");
    }
    const double rAll = t.total > 0 ? static_cast<double>(t.over) / n : 0.0;
    const double r0 = t.bin[0].pixels > 0
            ? static_cast<double>(t.bin[0].over) / static_cast<double>(t.bin[0].pixels)
            : 0.0;
    const double m3p = t.mass > 0.0
            ? (t.bin[2].mass + t.bin[3].mass + t.bin[4].mass) / t.mass
            : 0.0;
    const double bin0Share = t.bin[0].pixels / n;
    const RejectBranch br = classifyReject(t, bin0Share);
    std::printf("  ⭐ 归因判决（跑前写死：支持 = r0 ≤ %s×rAll 且 m3p ≥ %s；否证 = r0 ≥ %s×rAll）\n",
        d2(kRejectAnchorQuiet).c_str(), d2(kRejectMassLine).c_str(), d2(kRejectRefuteSame).c_str());
    std::printf("    r0 = %s%%  rAll = %s%%（r0/rAll = %s）  m3p = %s%%  0 档面积 = %s%%%s\n",
        d2(r0 * 100.0).c_str(), d2(rAll * 100.0).c_str(),
        rAll > 0.0 ? d4(r0 / rAll).c_str() : "n/a", d2(m3p * 100.0).c_str(),
        d2(bin0Share * 100.0).c_str(),
        bin0Share < kRejectMinBin0Share ? "  ⚠ 低于门槛 5% ⟹ 无「老实累积」对照" : "");
    std::printf("    ⟹ **%s**\n", rejectBranchName(br));
    rep.r0 = r0;
    rep.rAll = rAll;
    rep.m3p = m3p;
    rep.bin0Share = bin0Share;
    rep.branch = br == RejectBranch::Support ? 0 : (br == RejectBranch::Refute ? 1 : 2);

    // ── 主判据：抑制保真度（闭式锚定，`temporal-residual-boost-fidelity`）──
    // 线 = 1.5 × ρ*，ρ* = |H(α_on)|/|H(α_off)| 由**配置的 α** 运行时算（改档位自动自适应）。
    if (alphaOn > 0.0 && sup.valid) {
        rep.fidelityRan = true;
        rep.bin0Rho = sup.bin[0].rho;
        rep.rho = sup.total.rho;
        rep.rhoStar = emaSuppressionRatio(alphaOn, alphaOff);
        rep.line = kBoostFidelityLine * rep.rhoStar;
        rep.fidelityPass = rep.rho <= rep.line;
        std::printf("  ⭐ 抑制保真度（主判据，跑前写死：ρ ≤ %s × ρ*；ρ* = |H(α=%.4g)|/|H(α=%.4g)|）\n",
            d2(kBoostFidelityLine).c_str(), alphaOn, alphaOff);
        std::printf("    ρ = %s（%s→%s）  ρ* 闭式 = %s  线 = %s  ⟹ **%s**（超线 %s×）\n",
            d4(rep.rho).c_str(), d6(sup.total.swingOff).c_str(), d6(sup.total.swingOn).c_str(),
            d4(rep.rhoStar).c_str(), d4(rep.line).c_str(),
            rep.fidelityPass ? "PASS" : "FAIL", d4(rep.rho / rep.line).c_str());
        std::printf("    内部阳性锚点：0 档 ρ = %s（闭式期望 %s，偏 %s×）⟹ 绿例就在同一帧里，"
                    "本判据不是永远红的摆设\n",
            d4(rep.bin0Rho).c_str(), d4(rep.rhoStar).c_str(),
            rep.rhoStar > 0.0 ? d4(rep.bin0Rho / rep.rhoStar).c_str() : "n/a");
    }
    return rep;
}

/// 热图：逐像素 swing → INFERNO 8bit。**scale 必须门关/门开共用**——各图各自拉伸 =
/// 「看起来更闪」的把戏，肉眼比对当场失效。返回实际用的 scale（回显进 stdout）。
double writeSwingHeat(const std::string& path, const cv::Mat& swing, double scale)
{
    cv::Mat d;
    swing.convertTo(d, CV_8UC1, 255.0 / std::max(scale, 1e-12));  // saturate cast = 截到 [0,255]
    cv::Mat heat;
    cv::applyColorMap(d, heat, cv::COLORMAP_INFERNO);
    if (!cv::imwrite(path, heat))
        throw std::runtime_error("热图写入失败: " + path);
    return scale;
}

/// 全帧 swing 图的 p 分位（共用 scale 从它来）。
double percentileOfMat(const cv::Mat& m, double p)
{
    std::vector<double> v;
    v.reserve(m.total());
    for (int y = 0; y < m.rows; ++y) {
        const double* px = m.ptr<double>(y);
        v.insert(v.end(), px, px + m.cols);
    }
    std::sort(v.begin(), v.end());
    return percentileOf(v, p);
}

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
/// 判决线的样本量门槛：B 格门关态 over-τ 像素低于它 ⟹ 不许拿噪声比噪声
constexpr int64_t kBandMinOverForVerdict = 200;
/// ⭐ 静止态判决线（批 13.7c 跑前写死，出处见头注释）：支持 = rB ≥ 1.5×rA；否证 = rB/rA ∈ [0.8,1.25]
/// （1.5 = 头文件里的 `kBandSupportRatio`，两把移动尺子的「超额集中在 B」沿用同一个数）
constexpr double kBandEqualLo = 0.8, kBandEqualHi = 1.25;

/// 分带报告进台账的关键读数（判决线只有一条，其余是复核用的原始数）。
struct BandReport {
    std::string verdict = "未出";
    double rA = 0.0;       ///< A 格（中间调×高梯度）的 rate 被压倍数 on/off
    double rB = 0.0;       ///< B 格（近削顶×高梯度）同上
    double bMassOff = 0.0, bMassOn = 0.0;  ///< B 格残余质量占比（门关 / 门开）
    double tHi = 0.0;      ///< 实际用的近削顶字节下界（闭式给出）
    double gradThr = 0.0;  ///< 实际用的梯度 p90 阈值
    double shareDrift = 0.0;  ///< 两态带边界漂移（**须恰为 0**）
};

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

/// 差表（门开 − 门关）。Δshare **必须恒 0** = 两态共用一把尺子的自证；非 0 = 尺子漏了。
double printBandDiff(const BandTable& off, const BandTable& on)
{
    std::printf("  ┌ 差（门开 N2 − 门关 N1）\n");
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
        const double ra = a.pixels > 0 ? static_cast<double>(a.over) / a.pixels : 0.0;
        const double rb = b.pixels > 0 ? static_cast<double>(b.over) / b.pixels : 0.0;
        const double dMass = off.over > 0 && on.over > 0
                ? 100.0 * (static_cast<double>(b.over) / on.over
                      - static_cast<double>(a.over) / off.over)
                : 0.0;
        maxShareDrift = std::max(maxShareDrift, std::abs(dShare));
        std::printf("    %-13s %+8s %+8s %+10s %10s%s\n", kBandRowTag[i], d2(dShare).c_str(),
            d2(100.0 * (rb - ra)).c_str(), d2(dMass).c_str(),
            ra > 0.0 ? d4(rb / ra).c_str() : "inf",
            i == 3 ? "  ← A" : (i == 5 ? "  ← B（压不动 = 假设成立）" : ""));
    }
    return maxShareDrift;
}

/// 出两态分带表 + 差表 + 判决 + 敏感性。返回进台账的关键读数。
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
    const double n = static_cast<double>(luma.total());

    // ── 先测：显示域字节亮度分布（阈值由它定，不许拍数）─────────────────────────
    auto lv = valuesInMask(luma, {});
    std::sort(lv.begin(), lv.end());
    const auto massAtLeast = [&](double b) {
        return static_cast<double>(
                   lv.end() - std::lower_bound(lv.begin(), lv.end(), b)) / n;
    };
    const auto massNear = [&](double c, double w) {
        const auto lo = std::lower_bound(lv.begin(), lv.end(), c - w);
        const auto hi = std::upper_bound(lv.begin(), lv.end(), c + w);
        return static_cast<double>(hi - lo) / n;
    };
    const double tHi = static_cast<double>(bandClipProxyByte(kBandClipProxyLinear));
    const double tLo = kBandMidByte;
    const double clipMass = massAtLeast(tHi);
    const double pile255 = massAtLeast(254.999);
    const double midSpike = massNear(tLo, 8.0);
    std::printf("  亮度轴 = 显示域字节亮度的**多帧均值**（tonemap+sRGB 之后取 Rec.601 加权）\n");
    std::printf("    实测分布 p50 %s · p90 %s · p99 %s · p99.9 %s · p99.99 %s（码字 0..255）\n",
        d2(percentileOf(lv, 0.50)).c_str(), d2(percentileOf(lv, 0.90)).c_str(),
        d2(percentileOf(lv, 0.99)).c_str(), d2(percentileOf(lv, 0.999)).c_str(),
        d2(percentileOf(lv, 0.9999)).c_str());
    std::printf("    255 精确堆积 %s%% · [%.0f,255) %s%% · [245,255) %s%% · [250,255) %s%%\n",
        d4(pile255 * 100.0).c_str(), tHi, d4(massAtLeast(tHi) * 100.0).c_str(),
        d4(massAtLeast(245.0) * 100.0).c_str(), d4(massAtLeast(250.0) * 100.0).c_str());
    std::printf("    阈值 T_lo=%.0f（8bit 码字中点 ⟺ 显示域线性 %s ≈ 18%% 灰）· T_hi=%.0f"
                "（**闭式**：ACES 输入 ≥%s ⟺ byte %.0f，见 bandClipProxyByte）\n",
        tLo, d4(srgbDecode(tLo / 255.0)).c_str(), tHi, d2(kBandClipProxyLinear).c_str(), tHi);
    std::printf("    门槛：近削顶档质量 %s%%（下限 %s%%）%s · T_lo±8 带内质量 %s%%（上限 %s%%）%s\n",
        d4(clipMass * 100.0).c_str(), d4(kBandMinClipMass * 100.0).c_str(),
        clipMass >= kBandMinClipMass ? "✓" : "✗ 该档无定义", d4(midSpike * 100.0).c_str(),
        d4(kBandMidSpikeCap * 100.0).c_str(), midSpike < kBandMidSpikeCap ? "✓ 切在谷底" : "⚠ 切在尖峰");

    // ── 梯度轴（分位口径复用 temporal_motion：绝对阈值是拍出来的数）──────────────
    auto gv = valuesInMask(grad, {});
    std::sort(gv.begin(), gv.end());
    const double gradThr = percentileOf(gv, kBandGradPct);
    std::printf("  梯度轴 = |Sobel_x|+|Sobel_y| 于**线性 luma 相位平均**（与 gradEnergy 同算子）\n");
    std::printf("    高梯度 = ≥ p%.0f = %s（占 %s%%；分位而非绝对阈值的理由见头注释）\n",
        kBandGradPct * 100.0, d4(gradThr).c_str(),
        d2(100.0 - kBandGradPct * 100.0).c_str());
    std::printf("  尺子 = N1（门关 = 用户现状）的 byte-mean 与线性梯度，**两态共用一把尺子**\n");

    const BandTable off = bandTableOf(N1.swingMap, luma, grad, tLo, tHi, gradThr, tau);
    const BandTable on = bandTableOf(N2.swingMap, luma, grad, tLo, tHi, gradThr, tau);
    if (!off.valid() || !on.valid()) {
        std::printf("  ✗ 分带表无像素（total=0），不出数\n");
        return rep;
    }
    printBandTable("门关 N1（用户现状）", off);
    printBandTable("门开 N2（taa.static_boost）", on);
    const double drift = printBandDiff(off, on);
    std::printf("    Δshare 最大 %s%%（**须恰为 0**：两态共用一把尺子的自证，非 0 = 差表作废）\n",
        d4(drift).c_str());

    // ── ⭐ 判决（线跑前写死，见头注释；此处只执行，不解释成新规则）───────────────
    const BandCell& aOff = off.cell[1][1];
    const BandCell& aOn = on.cell[1][1];
    const BandCell& bOff = off.cell[2][1];
    const BandCell& bOn = on.cell[2][1];
    const auto rate = [](const BandCell& c) {
        return c.pixels > 0 ? static_cast<double>(c.over) / c.pixels : 0.0;
    };
    const double rA = rate(aOff) > 0.0 ? rate(aOn) / rate(aOff) : 0.0;
    const double rB = rate(bOff) > 0.0 ? rate(bOn) / rate(bOff) : 0.0;
    const double bMassOff = off.over > 0 ? static_cast<double>(bOff.over) / off.over : 0.0;
    const double bMassOn = on.over > 0 ? static_cast<double>(bOn.over) / on.over : 0.0;
    rep.rA = rA;
    rep.rB = rB;
    rep.bMassOff = bMassOff;
    rep.bMassOn = bMassOn;
    rep.tHi = tHi;
    rep.gradThr = gradThr;
    std::printf("\n  ⭐ 判决（线跑前写死：支持 = rB ≥ %s×rA 且 B 残余质量上升；否证 = rB/rA ∈ [%s,%s]）\n",
        d2(kBandSupportRatio).c_str(), d2(kBandEqualLo).c_str(), d2(kBandEqualHi).c_str());
    std::printf("    A 中间调×高梯度 rate %s%%→%s%%（×%s）  B 近削顶×高梯度 rate %s%%→%s%%（×%s）\n",
        d2(rate(aOff) * 100.0).c_str(), d2(rate(aOn) * 100.0).c_str(), d4(rA).c_str(),
        d2(rate(bOff) * 100.0).c_str(), d2(rate(bOn) * 100.0).c_str(), d4(rB).c_str());
    std::printf("    B 的残余质量占比 %s%% → %s%%（%+.2f pp）  B 门关 over-τ 像素 %lld\n",
        d2(bMassOff * 100.0).c_str(), d2(bMassOn * 100.0).c_str(), (bMassOn - bMassOff) * 100.0,
        static_cast<long long>(bOff.over));
    if (bOff.over < kBandMinOverForVerdict || clipMass < kBandMinClipMass)
        rep.verdict = "分不出（B 格样本不足 / 近削顶档无定义）";
    else if (rate(bOff) <= 0.0 || rate(aOff) <= 0.0)
        rep.verdict = "分不出（A 或 B 门关态 rate=0，比值无定义）";
    else if (rB >= kBandSupportRatio * rA && bMassOn > bMassOff)
        rep.verdict = "支持假设：残余向「近削顶×高梯度」集中，boost 压不动高亮边";
    else if (rB >= kBandEqualLo * rA && rB <= kBandEqualHi * rA)
        rep.verdict = "否证假设：两格等比例被压，残余不是高光专属";
    else
        rep.verdict = "分不出（既非等比例、也未达 1.5× 分离——看下面敏感性表定性质）";
    std::printf("    ⟹ **%s**\n", rep.verdict.c_str());
    // 方向自述（**不是**判决的一部分）。跑前只写死了「支持」与「等比例否证」两端，落第三支的
    // 其实有两种：**无分离**（真·分不出）与**反向分离**（B 比 A 压得更狠）。后者对「高亮边
    // 压不动」这条预测是否证方向，不打印出来会被「分不出」三个字读成「没有信号」。判决文字
    // 仍按跑前的三分支，不改写。
    std::printf("    分离方向：rB %s rA ⟹ %s（预测要求的是 rB > rA：B 比 A 压不动）\n",
        rB < rA ? "<" : (rB > rA ? ">" : "="),
        rB < rA ? "**B（高亮边）被压得更狠**，与预测方向相反"
                : (rB > rA ? "B 被压得没那么狠（方向与预测一致，幅度见上）" : "两格同倍数"));
    rep.shareDrift = drift;

    // ── 敏感性（同一次采集换阈值，零额外引擎成本）───────────────────────────────
    // 判「分不出」时的分工：换阈值后结论跟着翻 = **阈值没分开**；怎么换都不翻 = 两格同分布。
    std::printf("\n  ── 敏感性（同一次采集换阈值；主档 = 上面那张）──\n");
    std::printf("    %-26s %14s %14s %14s\n", "口径", "A rate ×(on/off)",
        "B rate ×(on/off)", "B 残余质量 off→on");
    const auto sensLine = [&](const std::string& tag, const cv::Mat& lm, const cv::Mat& gm,
                                  double tl, double th, double gthr) {
        const BandTable t1 = bandTableOf(N1.swingMap, lm, gm, tl, th, gthr, tau);
        const BandTable t2 = bandTableOf(N2.swingMap, lm, gm, tl, th, gthr, tau);
        const double a1 = rate(t1.cell[1][1]), a2 = rate(t2.cell[1][1]);
        const double b1 = rate(t1.cell[2][1]), b2 = rate(t2.cell[2][1]);
        const double m1 = t1.over > 0 ? static_cast<double>(t1.cell[2][1].over) / t1.over : 0.0;
        const double m2 = t2.over > 0 ? static_cast<double>(t2.cell[2][1].over) / t2.over : 0.0;
        std::printf("    %-26s %14s %14s %8s%% →%-7s%% (%lldpx)\n", tag.c_str(),
            a1 > 0.0 ? d4(a2 / a1).c_str() : "inf", b1 > 0.0 ? d4(b2 / b1).c_str() : "inf",
            d2(m1 * 100.0).c_str(), d2(m2 * 100.0).c_str(),
            static_cast<long long>(t1.cell[2][1].over));
    };
    for (double th : kBandClipSensitivity)
        sensLine("T_hi=" + d2(th) + (std::abs(th - tHi) < 0.5 ? "（主档）" : ""), luma, grad, tLo,
            th, gradThr);
    {
        auto gbv = valuesInMask(N1.gradByte, {});
        std::sort(gbv.begin(), gbv.end());
        const double gthrByte = percentileOf(gbv, kBandGradPct);
        sensLine("梯度轴=显示域 p90", luma, N1.gradByte, tLo, tHi, gthrByte);
        auto gv2 = valuesInMask(grad, {});
        std::sort(gv2.begin(), gv2.end());
        sensLine("梯度轴=线性 p75", luma, grad, tLo, tHi, percentileOf(gv2, 0.75));
        sensLine("梯度轴=线性 p95", luma, grad, tLo, tHi, percentileOf(gv2, 0.95));
        sensLine("亮度轴=跨帧最大", N1.byteMax, grad, tLo, tHi, gradThr);
        sensLine("T_lo=96", luma, grad, 96.0, tHi, gradThr);
        sensLine("T_lo=160", luma, grad, 160.0, tHi, gradThr);
    }
    return rep;
}

/// 一机位的六组（N6 仅 `--diag-bs-off`；N7/N8 仅 `--reject`）+ 出表 + 守卫 + 热图。
/// 返回 0 = 守卫全过；1 = 某守卫红；2 = 采集失败/锚点中止。
std::string clipArmName(const ClipArm& arm)
{
    switch (static_cast<int>(arm.mode + 0.5)) {
        case 0: return "0 现行";
        case 1: return "1 关盒";
        case 2: return "2 扩1.5×";
        case 3: return "3 box9";
        case 4: {
            std::ostringstream s;
            s << "4 软λ=" << arm.soft;
            return s.str();
        }
        case 5: {
            std::ostringstream s;
            s << "5 扩" << arm.soft << "×";
            return s.str();
        }
        case 6: {
            std::ostringstream s;
            s << "6 信任γ=" << (arm.soft > 0.0 ? std::to_string(arm.soft) : "1/16");
            return s.str();
        }
        case 7: {
            std::ostringstream s;
            s << "7 记忆d=" << (arm.soft > 0.0 ? std::to_string(arm.soft) : "0.5");
            return s.str();
        }
        case 8: {
            std::ostringstream s;
            // 0 = 用**推导值** 1−1/16（记忆覆盖一整个抖动周期），显示成出处而不是某个小数
            s << "8 软记忆d=" << (arm.soft > 0.0 ? std::to_string(arm.soft) : "1-1/16");
            return s.str();
        }
        case 9: {
            std::ostringstream s;
            s << "9 mem接受+ext斜率d=" << (arm.soft > 0.0 ? std::to_string(arm.soft) : "1-1/16");
            return s.str();
        }
        default: return "未知臂" + std::to_string(arm.mode);
    }
}

/**
 * 逐臂跑「门关 / 门开」两组并**当场出表**（长跑中途可读，不是一次性在最后打印）。
 * τ 由臂 0 的门关组定、全臂共用（与 `runBistroCamera` 同口径：跨臂比的是同一把尺子）。
 * 臂内 α：门关 = feedback 0.12、门开 = 地板；热身按各自 α 算（红线 2，门开态按地板热身）。
 */
std::vector<ClipAblRow> runBistroClipAblation(const std::string& dir, const std::string& pre,
    const BistroCam& cam, int periods, double warmMult, double boostFloor,
    const std::vector<ClipArm>& arms)
{
    std::vector<ClipAblRow> rows;
    double tau = 0.0;
    std::printf("\n── 历史裁剪消融 A/B（%s｜每臂两组：门关 fb=.12 / 门开 boost=%.3g；"
                "τ 由臂 0 门关定，全臂共用）──\n",
        cam.name, boostFloor);
    std::printf("  判决线（**跑前写死**，见 temporal_residual.h 同名常量）：\n"
                "    ① 因果坐实 = ρ ≤ %s×ρ* 且 frac>τ 相对臂 0 降 ≥ %sx\n"
                "    ② 否证     = 所有消融臂 ρ ≥ %s×ρ(臂0) ⟹ 盒不是主因\n"
                "    ③ 通用性   = 逐机位 / 逐靶方向一致（只有报障机位降 = 场景特异 = 不算修好）\n",
        d2(kBoostFidelityLine).c_str(), d2(kClipCausalFracDrop).c_str(),
        d2(kClipRefuteRatio).c_str());
    // ⚠ **面积与尾巴可以反向动**（2026-10-06 在 aa_target 上撞见的口径告警，写在这里防误读）：
    // 关盒让 `frac>τ`（面积）**升**而 p99.9（尾巴）**降 3×**——长窗累积把「少数像素大幅跳」换成
    // 「多数像素小幅漂」。两者量的不是同一件事：尾巴 = 用户报的「数得清的闪」，面积 = 1 LSB
    // 边缘的可见面积。所以「修得掉可见症状」这条线**两栏都要看**，任一单独当判据都会误判。
    std::printf("  ⚠ frac>τ 是**面积**、p999 是**尾巴**，二者可反向（长窗把少数大幅跳换成多数"
                "小幅漂）；判「修没修掉」两栏一起读\n");
    // **锐度分母**：同次运行的 fb=1（完全不累积）= 理想 16 相位平均的梯度能量。
    // 没有它，「尾巴降 20×」可以纯靠"更糊了"拿到（ρ 是比值糊不动它，但可见症状的改善会被糊虚报）。
    // ── ⚠ 护栏的**方向**（第一版写反，2026-10-06，被自己的数据抓出来）──
    // 读数 = 本臂相位平均图梯度能量 ÷ fb=1 相位平均图。于是：
    //   **< 1 = 比理想 16 相位平均更糊** ⟹ 这才是「靠糊换安静」的方向；
    //   **> 1 = 比它更锐**，**不是作弊**：base_space 下每份贡献先 un-jitter 再累积，EMA 收敛到
    //     的是**基空间点采样**，而 fb=1 参考是 16 个亚像素偏移的**盒平均**（≈1px 模糊）。
    // 实测（brickfence，门开）：现行 0.828 · 关盒 1.133 ⟹ **今天的裁剪正在往图里注入模糊**
    //   （裁剪把历史换成 centroid = 5 抽头空间均值，boost 下裁剪事件权重 1−α ≈ 99%）——
    //   同一个 bug 的第二条代价：静止机位下交付的是糊图。
    // ⟹ 反作弊只查**变糊**这一侧，且以「不比臂 0 更糊」为准（臂 0 自己就偏糊；拿 1.0 当红线
    //    会把合法修法误杀）。
    double gradRef = 0.0;
    try {
        const BistroRun ref = runBistroGroup(dir + "_ref", "参考 fb=1（梯度分母）", pre + "ref",
            cam, 1.0, 1.0, periods, warmMult, tau, false, boostFloor, false);
        gradRef = ref.grad;
    } catch (const std::exception& e) {
        std::printf("  ✗ 锐度分母组采集失败: %s ⟹ 本表无法排除「靠糊换安静」，作废\n", e.what());
        return rows;
    }
    std::printf("  锐度分母 = fb=1 相位平均图线性域梯度能量 %s（锐度比 **<1 = 更糊 = 作弊方向**、"
                ">1 = 比 16 相位平均更锐 = 基空间 EMA 的性质；只罚「比臂 0 低 >%s」）\n",
        d6(gradRef).c_str(), d2(kBlurTolerance).c_str());
    std::printf("  %-11s %9s %9s %7s %7s %7s %7s%% %7s%% %8s %8s %7s %7s  %s\n", "臂", "swingOff",
        "swingOn", "ρ", "线", "vs今天", "fracOff", "fracOn", "p999Off", "p999On", "锐Off", "锐On",
        "判定");
    for (size_t i = 0; i < arms.size(); ++i) {
        ClipAblRow r;
        r.arm = arms[i];
        const std::string nm = clipArmName(r.arm);
        const std::string idx = "_" + std::to_string(i);
        try {
            // ⚠ **两态必须吃同一臂**：ρ 是「同一裁剪行为下，boost 压掉几倍」。若门关固定在
            // 臂 0、门开换臂，ρ 的分母就跟着臂变，跨臂不可比（表会看着漂亮但答不了问题）。
            // 「本臂 vs 今天」是另一个数 = swingOn[臂X] / swingOn[臂0]，表里另算。
            const BistroRun off = runBistroGroup(dir + "_off" + idx, "臂" + nm + " 门关",
                pre + "off" + idx, cam, 0.12, 1.0, periods, warmMult, tau, false, boostFloor,
                false, true, true, r.arm);
            const BistroRun on = runBistroGroup(dir + "_on" + idx, "臂" + nm + " 门开",
                pre + "on" + idx, cam, 0.12, 1.0, periods, warmMult, tau, true, boostFloor,
                false, true, true, r.arm);
            r.swingOff = off.stat.meanSwing;
            r.swingOn = on.stat.meanSwing;
            r.rho = r.swingOff > 0.0 ? r.swingOn / r.swingOff : 0.0;
            r.rhoStar = emaSuppressionRatio(boostFloor, 0.12);
            r.line = kBoostFidelityLine * r.rhoStar;
            r.fracOff = off.stat.fracOverTau;
            r.fracOn = on.stat.fracOverTau;
            r.p999Off = off.stat.p999Swing;
            r.p999On = on.stat.p999Swing;
            r.gradOff = gradRef > 0.0 ? off.grad / gradRef : 0.0;
            r.gradOn = gradRef > 0.0 ? on.grad / gradRef : 0.0;
            r.ok = true;
        } catch (const std::exception& e) {
            std::printf("  %-11s ✗ 采集失败: %s\n", nm.c_str(), e.what());
            rows.push_back(r);
            return rows;
        }
        rows.push_back(r);
        const std::string vsToday = i == 0 || rows[0].swingOn <= 0.0 ? "—"
            : d4(r.swingOn / rows[0].swingOn);
        std::printf("  %-11s %9s %9s %7s %7s %7s %6s%% %6s%% %8s %8s %7s %7s  ", nm.c_str(),
            d6(r.swingOff).c_str(), d6(r.swingOn).c_str(), d4(r.rho).c_str(), d4(r.line).c_str(),
            vsToday.c_str(), d2(r.fracOff * 100.0).c_str(), d2(r.fracOn * 100.0).c_str(),
            d4(r.p999Off).c_str(), d4(r.p999On).c_str(), d4(r.gradOff).c_str(),
            d4(r.gradOn).c_str());
        if (i == 0)
            std::printf("（基准）\n");
        else {
            // 三条都得报：ρ（机制）· 可见症状（面积 + 尾巴）· 锐度（**只罚变糊侧**）。
            const bool rhoOk = r.rho <= r.line;
            const bool fracOk = rows[0].fracOn > 0.0
                && r.fracOn <= rows[0].fracOn / kClipCausalFracDrop;
            const bool blurCheat = r.gradOn < rows[0].gradOn - kBlurTolerance
                || r.gradOff < rows[0].gradOff - kBlurTolerance;
            std::printf("%s%s%s\n", rhoOk ? "ρ达闭式 " : "",
                fracOk ? "**症状降(面积)** " : (rhoOk ? "面积未达4× " : "未达标 "),
                blurCheat ? "**比今天更糊 = 靠糊换安静，不算修好**" : "");
        }
    }
    if (rows.size() > 1 && rows[0].ok) {
        // **在场性守卫**（债-55 的教训：参数写进去了但没生效，注释写了、代码没做、判据没红）：
        // scene.lua 的未知键引擎**不报错**，所以"键没生效"必须由判据自己抓——所有臂的
        // 读数与臂 0 **逐位相同** = 键根本没进引擎（拼错 / 载荷断链），整表作废。
        bool identical = true;
        for (size_t i = 1; i < rows.size(); ++i)
            identical = identical && rows[i].ok
                && rows[i].swingOn == rows[0].swingOn && rows[i].swingOff == rows[0].swingOff;
        if (identical) {
            std::printf("  ✗ **所有臂读数与臂 0 逐位相同** ⟹ `taa.clip_mode` 没生效"
                        "（键名 / 载荷 / 配置注入断链），本表作废（在场性守卫，债-55）\n");
            return rows;
        }
        bool refute = true;
        for (size_t i = 1; i < rows.size(); ++i)
            refute = refute && rows[i].ok && rows[i].rho >= kClipRefuteRatio * rows[0].rho;
        std::printf("  ⭐ 汇总：ρ(臂0) = %s；**%s**\n", d4(rows[0].rho).c_str(),
            refute ? "所有消融臂都没把 ρ 压下来 ⟹ **否证**：邻域盒不是主因，短路假设死，"
                     "必须回头查别处（不许换说法继续猜）"
                   : "至少一臂显著压下了 ρ ⟹ 邻域盒短路**坐实**为因（逐臂看上行判定）");
    }
    return rows;
}

int runBistroCamera(const BistroCam& cam, int periods, double warmMult, double boostFloor,
    const std::string& shotDir, bool diagBsOff, bool reject, const ClipArm& arm = {})
{
    double tau = 0.0;
    std::vector<BistroRun> runs;
    const std::string pre = "rb" + std::string(1, cam.name[0]);
    const std::string dir = projectRoot() + "/build/imgqa_resbis_" + cam.name;
    // G0 确定性地板（TAA 整个关 = 连 jitter 注入都不发生）：静止机位连拍必须**逐位相同**。
    // 这是 `determinism` 地板在 bistro 交付配置上的实例化。它非零 ⟹ 非确定性在 TAA 之外
    // （更底层的引擎/驱动问题），本表所有读数存疑；它为零而 N3/N5 非零 ⟹ 非确定性住在
    // **TAA 特性内**（jitter 注入 proj 的 ±0.0 / 历史裁剪盒 / base_space 的 un-jitter·c_t）。
    // τ 用局部 throwaway（地板组的白电平不参与判据 τ，τ 一律由 N1 定）。
    BistroRun n0;
    {
        double tau0 = 0.0;
        try {
            n0 = runBistroGroup(dir + "_n0", "N0 TAA关(地板)", pre + "n0", cam, 0.12, 1.0, periods,
                warmMult, tau0, false, boostFloor, false, false);
        } catch (const std::exception& e) {
            std::printf("  %s N0 采集失败: %s\n", cam.name, e.what());
            return 2;
        }
    }
    const bool floorOk = n0.byteMaxVsFirst == 0.0 && n0.changedPx == 0 && n0.stat.meanSwing == 0.0;
    std::printf("\n── %s ──\n", cam.name);
    std::printf("  %s G0 确定性地板（N0 TAA关）：与首帧 %2.0fLSB / %lldpx / meanSwing %s"
                "（判据 = **恰为 0**）\n",
        floorOk ? "✓" : "✗", n0.byteMaxVsFirst, static_cast<long long>(n0.changedPx),
        d6(n0.stat.meanSwing).c_str());
    // ⚠ 消融臂只加在 N1/N2（门关/门开）——N0/N3/N4/N5 是**反向守卫与梯度参考**，
    //   让它们也吃臂 = 守卫自己被被测改动污染，那就不是守卫了（债-55 型：键生效在错误的组上）。
    if (arm.mode > 0.5)
        std::printf("  ⚠ 消融臂 mode=%d soft=%s **只加在 N1/N2**；N0/N3/N4/N5 保持基线\n",
            static_cast<int>(arm.mode + 0.5), d4(arm.soft).c_str());
    try {
        runs.push_back(runBistroGroup(dir + "_n1",
            "N1 门关 fb=.12 js=1", pre + "n1", cam, 0.12, 1.0, periods, warmMult, tau, false,
            boostFloor, true, true, true, arm));
        // G1 阳性锚点（只钉 doorframe——用户报障机位是既成事实；其余机位接近 0 如实打印）。
        if (std::string(cam.name) == "doorframe" && runs[0].stat.fracOverTau < kAnchorFracFloor) {
            std::printf(
                "\n  ✗ 阳性锚点挂零：doorframe 门关 frac(swing>τ) = %s%% < %s%%。\n"
                "    用户肉眼看得见闪烁是既成事实，量出来接近 0 = **量具坏了**（或症状在此口径下\n"
                "    根本不存在——那也是要紧的负结果）。立刻停下，不跑其余机位，不出任何门开结论。\n",
                d2(runs[0].stat.fracOverTau * 100.0).c_str(), d2(kAnchorFracFloor * 100.0).c_str());
            return 2;
        }
        runs.push_back(runBistroGroup(projectRoot() + "/build/imgqa_resbis_" + cam.name + "_n2",
            "N2 门开 fb=.12 js=1", pre + "n2", cam, 0.12, 1.0, periods, warmMult, tau, true,
            boostFloor, true, true, true, arm));
        runs.push_back(runBistroGroup(projectRoot() + "/build/imgqa_resbis_" + cam.name + "_n3",
            "N3 反向 fb=.12 js=0", pre + "n3", cam, 0.12, 0.0, periods, warmMult, tau, true,
            boostFloor, false));
        runs.push_back(runBistroGroup(projectRoot() + "/build/imgqa_resbis_" + cam.name + "_n5",
            "N5 反向对照 js=0门关", pre + "n5", cam, 0.12, 0.0, periods, warmMult, tau, false,
            boostFloor, false));
        runs.push_back(runBistroGroup(projectRoot() + "/build/imgqa_resbis_" + cam.name + "_n4",
            "N4 梯度参考 fb=1", pre + "n4", cam, 1.0, 1.0, periods, warmMult, tau, false,
            boostFloor, false));
    } catch (const std::exception& e) {
        std::printf("  %s 采集失败: %s\n", cam.name, e.what());
        return 2;
    }
    const BistroRun& N1 = runs[0];
    const BistroRun& N2 = runs[1];
    const BistroRun& N3 = runs[2];
    const BistroRun& N5 = runs[3];  // 反向**对照**：js=0 且 boost 关（α=.12）
    const BistroRun& N4 = runs[4];

    std::printf("  τ = %s（N1 定，各组共用）白电平 p99 = %s  量化项 = %s\n", d6(tau).c_str(),
        d6(N1.whiteLuma).c_str(), d6(N1.quant).c_str());
    for (const auto& r : runs) {
        // 半间差闭式容差 = IIR 尾巴 (1−α)^warm × 满幅 + 一格真实跨度（热身自证，见头注释）。
        const double tol =
            std::pow(1.0 - r.alpha, static_cast<double>(r.warm)) * std::max(1e-9, N1.whiteLuma)
                + N1.quant;
        std::printf("  %-18s 热身 %5d  meanSwing %-9s p99.9 %-9s max %-9s frac>τ %6.2f%%  "
                    "半间差max %s%s%s **超τ %lldpx**  与首帧 %2.0fLSB/%lldpx\n",
            r.name.c_str(), r.warm, d6(r.stat.meanSwing).c_str(), d6(r.stat.p999Swing).c_str(),
            d6(r.stat.maxSwing).c_str(), r.stat.fracOverTau * 100.0,
            d6(r.stat.halfGapMax).c_str(), r.stat.halfGapMax > tol ? "（>容差 " : "（≤容差 ",
            d6(tol).c_str(), static_cast<long long>(r.stat.halfGapOverPx), r.byteMaxVsFirst,
            static_cast<long long>(r.changedPx));
    }
    const double swingRatio =
        N1.stat.meanSwing > 0.0 ? N2.stat.meanSwing / N1.stat.meanSwing : 0.0;
    std::printf("  门开/门关：meanSwing ×%s（%s→%s）  p99.9 %s→%s  frac>τ %s%%→%s%%\n",
        d4(swingRatio).c_str(), d6(N1.stat.meanSwing).c_str(), d6(N2.stat.meanSwing).c_str(),
        d6(N1.stat.p999Swing).c_str(), d6(N2.stat.p999Swing).c_str(),
        d2(N1.stat.fracOverTau * 100.0).c_str(), d2(N2.stat.fracOverTau * 100.0).c_str());
    const double gradLin = N4.grad > 0.0 ? N1.grad / N4.grad : 0.0;
    const double gradDisp = N4.byteGrad > 0.0 ? N1.byteGrad / N4.byteGrad : 0.0;
    std::printf("  债-47 梯度能量比（N1 收敛 / N4 理想 16 相位平均）：**线性域 %s** ｜ 显示域 %s\n",
        d6(gradLin).c_str(), d6(gradDisp).c_str());
    std::printf("    （§1.5 旧账 0.671 = 显示空间读数；aa_target 同口径 显示 0.669 ↔ 线性 0.985。"
                "本行只出数，改判由主 agent 记账）\n");

    // G2 反向守卫：js=0 + 静止 ⟹ 首帧起历史 = 本帧（EMA 不动点）⟹ 残差**恰为 0**，无容差。
    // 非零时先归因再下结论：N5（js=0 且 boost **关**）是同场景的地板对照。doorframe 两轮实测
    // （PIL 复核）：N3 = **8 个固定像素 × 1 LSB、奇偶帧两态交替**（偶帧=首帧，奇帧翻），7/8 个
    // 落在强边缘（|∇luma| 47–115 vs 全图中值 4.5）且是暗部（luma 13–32）；N5（无 boost）同现
    // （7px，maxSwing 反而更大）⟹ 地板与 boost 无关。机制候选 = `jitter_scale=0` 时
    // jitterPx = (halton−0.5)·0 = **±0.0**，其符号随 Halton 相位逐帧交替（实测奇偶结构与之吻合），
    // 经 `proj[2][0] -= 2jx/W` 进入投影矩阵 ⟹ 恰压在边上的片元 tie-break 逐帧翻。N0（TAA 全关）
    // 与 N6（base_space 关）两个对照负责把「TAA 特性内 vs 之外」「base_space 路径 vs 公共路径」
    // 定死。⚠ 本注释早期版本写过「固定 ~1255 像素集」= 自写 PNG 解码器的伪影，已用 PIL 推翻。
    const bool revOk = N3.stat.meanSwing == 0.0 && N3.stat.maxSwing == 0.0;
    const bool revFloor = N5.stat.meanSwing == 0.0 && N5.stat.maxSwing == 0.0;
    std::printf("  %s 反向守卫（N3 门开 js=0）：meanSwing %s maxSwing %s 变化 %lldpx"
                "（期望**恰为 0**）\n",
        revOk ? "✓" : "✗", dExp(N3.stat.meanSwing).c_str(), dExp(N3.stat.maxSwing).c_str(),
        static_cast<long long>(N3.changedPx));
    std::printf("  %s 反向对照（N5 门关 js=0，α=.12）：meanSwing %s maxSwing %s 变化 %lldpx"
                "（地板参照：非零 ⟹ js=0 残差地板是**场景/驱动**的，非 boost 引入）\n",
        revFloor ? "✓" : "⚠", dExp(N5.stat.meanSwing).c_str(), dExp(N5.stat.maxSwing).c_str(),
        static_cast<long long>(N5.changedPx));
    if (!revOk && !revFloor)
        std::printf("    ⚠ 两态同现（门关地板 %s vs 门开 %s）⟹ 归因 = 引擎 js=0 既有 tie-flip，"
                    "「恰为 0」红线在 bistro 上不可达（合成靶 aa_target 的 R3/B4 是 0，场景不同）；"
                    "N1/N2 读数**不作废**但守卫保持红，处置由主 agent 判\n",
            d6(N5.stat.maxSwing).c_str(), d6(N3.stat.maxSwing).c_str());
    if (!revOk && revFloor)
        std::printf("    ✗ N3 非零而 N5（无 boost 同 js=0）恰为 0 ⟹ **boost 自己引入了第二时域源**，"
                    "本机位 N1/N2 门开读数作废\n");

    // N6 诊断（`--diag-bs-off`）：js=0 门开但 **base_space 关**——摘掉 un-jitter 与 c_{t−1}
    // 修正、其余（jitter 注入 proj、历史裁剪盒、EMA）不动。N3 非零而 N6 恰 0 ⟹ 翻转住在
    // base_space 路径；N6 同非零 ⟹ 住在两者共用部分（jitter 注入 proj 的 ±0.0 / 历史裁剪盒）。
    bool sixRan = false, sixOk = false, sixNonZero = false;
    if (diagBsOff) {
        double tau6 = tau;  // τ 已非零，不会被 N6 覆写
        const BistroRun N6 = runBistroGroup(dir + "_n6", "N6 诊断 js=0 bs关", pre + "n6", cam,
            0.12, 0.0, periods, warmMult, tau6, true, boostFloor, false, true, false);
        sixRan = true;
        sixOk = N6.stat.meanSwing == 0.0 && N6.stat.maxSwing == 0.0;
        sixNonZero = !sixOk;
        std::printf("  %s N6 诊断（js=0 门开 base_space关）：meanSwing %s maxSwing %s %lldpx"
                    "（与 N3 对比归因：N3 非零而 N6 恰 0 = 翻转在 base_space 路径）\n",
            sixOk ? "✓" : "⚠", d6(N6.stat.meanSwing).c_str(), d6(N6.stat.maxSwing).c_str(),
            static_cast<long long>(N6.changedPx));
    }

    // 热图：门关/门开共用 scale（门关 swing 的 p99.9，截顶显示；各图各自拉伸 = 把戏）。
    const double scale = std::max(percentileOfMat(N1.swingMap, 0.999), 1e-12);
    const std::string offPath = shotDir + "/" + cam.name + "_swing_boost-off.png";
    const std::string onPath = shotDir + "/" + cam.name + "_swing_boost-on.png";
    writeSwingHeat(offPath, N1.swingMap, scale);
    writeSwingHeat(onPath, N2.swingMap, scale);
    std::printf("  热图（共用 scale = 门关 p99.9 = %s，白电平处 1 LSB τ = %s）：\n", d6(scale).c_str(),
        d6(tau).c_str());
    std::printf("    %s\n    %s\n", offPath.c_str(), onPath.c_str());

    // 亮度 × 梯度 二维分带（判「残余住在高光边还是普通边缘」）。判决线**跑前写死**，
    // 全文 = temporal_residual.h 同名段；这里只执行。守卫红不拦它——分带表要回答的正是
    // 「N1/N2 这两个读数里的残余长什么样」，与反向守卫是否可达无关。
    const BandReport band = reportLumaGradBands(N1, N2, tau);

    // 批 13.10：历史拒收 × 残余 交叉表（`--reject`）。它要回答的是本命令此前**答不了**的问题：
    // 门开把 meanSwing 压到 ×0.25，但纯 EMA 数学的下界是 ×0.082（|H| 比）——**压不满的那部分
    // 住在哪**。判决线跑前写死 = `temporal_residual.h`「历史拒收 × 残余」段。
    RejectReport rejOff, rejOn;
    if (reject) {
        try {
            // 两次调用喂**同一对 swing**（门开 N2 / 门关 N1），差别只在**分档用的拒收图**：
            // N7 按门关态（α=.12）的盒行为分档、N8 按门开态分档。主判据只在门开那次出
            // （alphaOn>0），门关那次没有「压」这个动作，比无可比。
            rejOff = reportRejectAttribution(
                runBistroRejectGroup(dir + "_rejoff", "N7 拒收码 门关", pre + "n7", cam, 0.12,
                    periods, warmMult, boostFloor, false),
                N2.swingMap, N1.swingMap, tau, std::string(cam.name) + " · 分档 = 门关 N1（fb=.12）",
                0.0, 0.12);
            rejOn = reportRejectAttribution(
                runBistroRejectGroup(dir + "_rejon", "N8 拒收码 门开", pre + "n8", cam, 0.12,
                    periods, warmMult, boostFloor, true),
                N2.swingMap, N1.swingMap, tau,
                std::string(cam.name) + " · 分档 = 门开 N8（static boost，用户现状）", boostFloor,
                0.12);
        } catch (const std::exception& e) {
            std::printf("\n  ✗ 拒收交叉表采集失败: %s\n", e.what());
            return 2;
        }
    }

    logCall("temporal-residual-bistro",
        {{"camera", cam.name}, {"periods", std::to_string(periods)},
            {"warmMult", d4(warmMult)}, {"floor", d4(boostFloor)}, {"tau", d6(tau)},
            {"N0_floor_ok", floorOk ? "1" : "0"},
            {"N0_px", std::to_string(n0.changedPx)},
            {"N1_swing", d6(N1.stat.meanSwing)}, {"N1_p999", d6(N1.stat.p999Swing)},
            {"N1_frac", d2(N1.stat.fracOverTau * 100.0)},
            {"N2_swing", d6(N2.stat.meanSwing)}, {"N2_p999", d6(N2.stat.p999Swing)},
            {"N2_frac", d2(N2.stat.fracOverTau * 100.0)},
            {"N3_swing", d6(N3.stat.meanSwing)}, {"N3_max", d6(N3.stat.maxSwing)},
            {"N3_px", std::to_string(N3.changedPx)}, {"N5_swing", d6(N5.stat.meanSwing)},
            {"N5_max", d6(N5.stat.maxSwing)}, {"N5_px", std::to_string(N5.changedPx)},
            {"N6_ran", sixRan ? "1" : "0"}, {"N6_nonzero", sixNonZero ? "1" : "0"},
            {"swingRatio", d4(swingRatio)}, {"gradLin", d6(gradLin)},
            {"gradDisp", d6(gradDisp)}, {"heatScale", d6(scale)},
            // 批 13.7c 亮度×梯度分带（判决 + 判决线两侧的原始数，事后复核用）
            {"bandVerdict", band.verdict}, {"bandT_hi", d2(band.tHi)},
            {"bandGradThr", d6(band.gradThr)}, {"bandShareDrift", d6(band.shareDrift)},
            {"bandA_ratio", d4(band.rA)}, {"bandB_ratio", d4(band.rB)},
            {"bandB_massOff", d2(band.bMassOff * 100.0)},
            {"bandB_massOn", d2(band.bMassOn * 100.0)},
            // 批 13.10 拒收交叉表（`--reject` 才跑；未跑 = 空串）
            {"rejOffBranch", reject ? std::to_string(rejOff.branch) : ""},
            {"rejOff_r0OverAll", reject ? d4(rejOff.rAll > 0 ? rejOff.r0 / rejOff.rAll : -1.0) : ""},
            {"rejOff_m3p", reject ? d4(rejOff.m3p) : ""},
            {"rejOff_collapse", reject ? d4(rejOff.collapsed) : ""},
            {"rejOff_clip", reject ? d4(rejOff.clipped) : ""},
            {"rejOnBranch", reject ? std::to_string(rejOn.branch) : ""},
            {"rejOn_r0OverAll", reject ? d4(rejOn.rAll > 0 ? rejOn.r0 / rejOn.rAll : -1.0) : ""},
            {"rejOn_m3p", reject ? d4(rejOn.m3p) : ""},
            {"rejOn_collapse", reject ? d4(rejOn.collapsed) : ""},
            {"rejOn_clip", reject ? d4(rejOn.clipped) : ""},
            // 主判据（闭式锚定抑制保真度）
            {"fidRan", reject ? "1" : "0"},
            {"fidPass", reject ? (rejOn.fidelityPass ? "1" : "0") : ""},
            {"fidRho", reject ? d6(rejOn.rho) : ""},
            {"fidRhoStar", reject ? d6(rejOn.rhoStar) : ""},
            {"fidLine", reject ? d6(rejOn.line) : ""},
            {"fidBin0Rho", reject ? d6(rejOn.bin0Rho) : ""}},
        {});
    // 守卫汇总：G0 地板与 G2 反向都计入 rc（G0 红 = 全表读数存疑，比 G2 更严重，绝不吞）。
    // sixOk 只作归因信息，不进 rc（诊断组不是判据）。
    (void)sixOk;
    // 主判据红 ⟹ rc=1（`temporal-residual-boost-fidelity`，线跑前写死）。它与两条守卫同级别：
    // 都是「这条命令的结论不许被读成通过」。
    if (reject && rejOn.fidelityRan && !rejOn.fidelityPass) {
        std::printf("\n  ✗ 主判据 `temporal-residual-boost-fidelity` **FAIL**：ρ = %s > 线 %s"
                    "（ρ* 闭式 %s，超 %s×）\n    ⟹ static boost 的抑制没到闭式的 2/3，差额住在被"
                    "邻域盒夹过的像素上（见上表 ρ 列）\n",
            d4(rejOn.rho).c_str(), d4(rejOn.line).c_str(), d4(rejOn.rhoStar).c_str(),
            d4(rejOn.rho / rejOn.line).c_str());
        return 1;
    }
    return (revOk && floorOk) ? 0 : 1;
}

/**
 * 批 13.11 **消融 A/B 的机位循环**（`--clip-abl`）。默认臂集 = 现行 + 四条消融/修法臂：
 * 关盒（天花板参照）/ 扩 1.5×（债-60 重测臂）/ box9（空间加宽）/ 软裁剪 λ=0.3 与 0.1（修法臂）。
 * 每臂两组（门关 + 门开），判决线跑前写死在 `runBistroClipAblation`。
 */
int runBistroClipAblSweep(int periods, double warmMult, const std::string& camera,
    double boostFloor, double onlyMode = 0.0, double onlySoft = 1.0)
{
    // 臂集（第四轮 = **修法验证**，2026-10-06）：0 基准 · 1 关盒（闭式天花板）·
    // 7 标量时域 luma 膨胀 decay ∈ {0.5 推导值, 0.25, 0.75}。⚠ **decay 扫 3× 是"平台期"护栏**：
    // 修法若靠某个调出来的常数，只会在一个值上好看；三档同向 = 机制在起作用，不是常数拟合场景。
    // （前三轮已定的臂不重跑：扩 1.5/3/6/12× 与 box9 证伪、软裁剪 λ=0.1 有效但差 2.2×、
    //   信任门三档 γ 只动 3–5% 否决 —— 全见调试历史 2026-10-06。）
    // `--clip-abl --clip <mode>` = **只跑 {0, mode} 两臂**：跨机位 / 跨靶的通用性复核要的是
    // 「同一臂在别的场景上同向」，不是重扫参数；全臂集在 4 个机位上要 100 分钟，会没人跑。
    std::vector<ClipArm> arms = {
        { 0.0, 1.0 }, { 1.0, 1.0 }, { 7.0, 0.0 }, { 7.0, 0.25 }, { 7.0, 0.75 } };
    if (onlyMode > 0.0) {
        // ⚠ mode 6/7 的 `--clip-soft` 是**时间常数覆盖位**，而它的缺省值 1.0 有具体含义且危险：
        //   mode 6 → γ_up=1（一次收下就满信任）；mode 7 → decay=1（**记忆永不衰减** = 盒单调涨到
        //   满量程 = 等价关盒）。1.0 在这里不是用户意图而是"没给参数" ⟹ 映射成 0 = 用推导值。
        double soft = onlySoft;
        if ((onlyMode == 6.0 || onlyMode == 7.0) && soft == 1.0) {
            soft = 0.0;
            std::printf("  （--clip-soft 未给 ⟹ 用推导时间常数，不是缺省值 1.0）\n");
        }
        arms = {{ 0.0, 1.0 }, { onlyMode, soft }};
    }
    std::printf("bistro 历史裁剪消融 A/B（批 13.11）：交付配置原样，逐臂只差 `taa.clip_mode`"
                "（门关 fb=.12 / 门开 static_feedback=%s）\n  %d×%d  静止机位  周期数 %d\n",
        d4(boostFloor).c_str(), kSize[0], kSize[1], periods);
    int rc = 0;
    for (const auto& cam : kBistroCams) {
        if (camera != "all" && camera != cam.name)
            continue;
        const auto rows = runBistroClipAblation(
            projectRoot() + "/build/imgqa_clipabl_" + cam.name, "abl" + std::string(cam.name),
            cam, periods, warmMult, boostFloor, arms);
        std::vector<std::pair<std::string, std::string>> args = {
            { "camera", cam.name }, { "floor", d4(boostFloor) }, { "periods", std::to_string(periods) }
        };
        for (const auto& r : rows) {
            const std::string k = clipArmName(r.arm);
            args.emplace_back(k + "_rho", r.ok ? d6(r.rho) : "fail");
            args.emplace_back(k + "_fracOn", r.ok ? d4(r.fracOn * 100.0) : "");
            args.emplace_back(k + "_swingOn", r.ok ? d6(r.swingOn) : "");
        }
        logCall("temporal-residual-clip-abl", args, {});
        if (!rows.empty() && !rows.back().ok)
            return 2;
        // 在场性守卫红 ⟹ rc=2（读数全同 = 键没生效 = 本表作废，见 runBistroClipAblation）
        bool identical = rows.size() > 1;
        for (size_t i = 1; i < rows.size(); ++i)
            identical = identical && rows[i].ok && rows[i].swingOn == rows[0].swingOn
                     && rows[i].swingOff == rows[0].swingOff;
        if (identical)
            return 2;
    }
    return rc;
}

int runBistroSweep(int periods, double warmMult, const std::string& camera, double boostFloor,
    bool diagBsOff, bool reject, const ClipArm& arm = {})
{
    std::printf("bistro 真实场景时域残差（债-48，**只出数不判**）：交付配置 configs/ 原样"
                "（deferred + env/skybox/IBL/AO/shadow 全开 + base_space=交付默认 true）\n"
                "  %d×%d  静止机位  折叠 (2,%d,H,W)  周期数 %d  门开地板 static_feedback=%s\n"
                "  区域 = **全帧**（交付 picking=false 无 objectID，打开 = 改图形态；与 aa_target 的"
                "数不可直接比，算子/域/τ 定义相同）\n",
        kSize[0], kSize[1], kResidualPeriod, periods, d4(boostFloor).c_str());
    const std::string shotDir = projectRoot() + "/build/rgl/screenshots";
    int rc = 0;
    for (const auto& cam : kBistroCams) {
        if (camera != "all" && camera != cam.name)
            continue;
        const int one = runBistroCamera(cam, periods, warmMult, boostFloor, shotDir, diagBsOff,
            reject, arm);
        if (one == 2)
            return 2;  // 采集失败 / 锚点中止：后面的机位不跑（停下报告）
        rc = std::max(rc, one);
    }
    if (rc == 0)
        std::printf("\n全部机位：反向守卫绿、锚点在场。门开压没压住 = 看「门开/门关」行与热图，"
                    "本命令不判（口径同 --static-boost 先例）。\n");
    return rc;
}

}  // namespace

int cmdTemporalResidual(int periods, double warmMult, const std::string& variant,
    double slowFeedback, bool skipSlow, bool baseSpace, double staticBoostFloor, bool bistro,
    const std::string& camera, double boostFloor, bool diagBsOff, bool reject, bool clipAbl,
    double clipMode, double clipSoft)
{
    if (periods < 2 || periods % 2 != 0) {
        std::printf("--periods 必须是 ≥2 的偶数（两半折叠 = 同绝对相位对照）。收到 %d\n", periods);
        return 2;
    }
    // `--reject` 只在 bistro 变体里实现（拒收码要交付配置的 taa_out 附件 + 全帧区域口径）。
    // **当场拒**而不是静默忽略：一个被吃掉的键 = 「我加了 --reject」这句话没有意义
    // （同本仓「camelCase 静默回落」那条债的处置方向）。
    if (reject && !bistro) {
        std::printf("--reject 只在 --bistro 变体里实现（要交付配置的 taa_out 附件通道 + 全帧区域"
                    "口径）；合成靶路径未接，**拒绝静默忽略该键**。加 --bistro 或去掉 --reject。\n");
        return 2;
    }
    // 批 13.11 消融臂：`--clip-abl` 是 bistro 侧的逐臂 A/B；`--clip <mode>` 是透传（两条路径都吃）。
    const ClipArm arm{clipMode, clipSoft};
    if (clipAbl && !bistro) {
        std::printf("--clip-abl 只在 --bistro 变体里实现；合成靶侧请用 --static-boost <地板> "
                    "--clip <mode>（B2/B3/B4 三组吃同一臂，ρ 同口径可比）。\n");
        return 2;
    }
    // 批 13.7b 变体分岔（默认不进，现行 7 条判据路径逐字不变）
    if (bistro) {
        if (clipAbl)
            return runBistroClipAblSweep(periods, warmMult, camera, boostFloor, clipMode, clipSoft);
        return runBistroSweep(periods, warmMult, camera, boostFloor, diagBsOff, reject, arm);
    }
    // 批 13.7 变体分岔（默认 0 = 不进，现行 7 条判据路径逐字不变）
    if (staticBoostFloor > 0.0)
        return runStaticBoostSweep(projectRoot() + "/build", periods, warmMult, variant, baseSpace,
            staticBoostFloor, arm);
    if (warmMult < 6.0)
        std::printf("⚠ --warm-multiplier %g < 6：EMA 瞬态 (1−α)^t 在 t=6/α 处还剩 0.25%%，"
                    "再小量的就是瞬态不是稳态（红线 2）\n\n",
            warmMult);

    const std::string root = projectRoot() + "/build";
    double tau = 0.0;  // 第一组（R0）定一次，之后各组共用
    std::vector<Run> runs;
    try {
        runs.push_back(runGroup(root + "/imgqa_res_r0", "R0 TAA关(地板)", "resr0", false, 0.12,
            1.0, variant, periods, warmMult, tau, false));
        runs.push_back(runGroup(root + "/imgqa_res_r1", "R1 fb=1  js=1(锚点)", "resr1", true, 1.0,
            1.0, variant, periods, warmMult, tau, baseSpace));
        runs.push_back(runGroup(root + "/imgqa_res_r2", "R2 fb=.12 js=1(现行)", "resr2", true,
            0.12, 1.0, variant, periods, warmMult, tau, baseSpace));
        runs.push_back(runGroup(root + "/imgqa_res_r3", "R3 fb=.12 js=0(反向)", "resr3", true,
            0.12, 0.0, variant, periods, warmMult, tau, baseSpace));
        if (!skipSlow)
            runs.push_back(runGroup(root + "/imgqa_res_r4",
                "R4 fb=" + d4(slowFeedback) + " js=1", "resr4", true, slowFeedback, 1.0, variant,
                periods, warmMult, tau, baseSpace));
    } catch (const std::exception& e) {
        std::printf("  采集失败: %s\n", e.what());
        return 2;
    }

    const Run& R0 = runs[0];
    const Run& R1 = runs[1];
    const Run& R2 = runs[2];
    const Run& R3 = runs[3];
    const bool hasSlow = runs.size() > 4;

    std::printf("时域绝对残差：aa_target %d×%d  静止机位  折叠 (2,%d,H,W)  周期数 %d  帧数 %d  "
                "base_space=%s\n",
        kSize[0], kSize[1], kResidualPeriod, periods, kResidualPeriod * periods,
        baseSpace ? "开（输出在基空间）" : "关（输出在抖动空间，交付默认）");
    std::printf("  统计域 = **sRGB 解码后的线性 luma**（红线 1）；swing = max−median over %d 列\n",
        kResidualPeriod);
    std::printf("  区域 = objectID 靶内部外扩 3px（%lld px）；τ = 白电平(p99) %s 处 1 个显示 LSB 的"
                "线性跨度 = %s（闭式见 lsbLinearAtLuma）\n",
        static_cast<long long>(R2.stat.maskPixels), d6(R2.whiteLuma).c_str(), d6(tau).c_str());
    std::printf("  半间差量化项 = mask 内**最亮** %s 处翻一格的最大真实跨度 = %s（离散步长，"
                "与 τ 故意不同源，见 maxLsbStepUpTo）\n\n",
        d6(R2.maxLuma).c_str(), d6(R2.quant).c_str());
    for (const auto& r : runs)
        printRow(r);

    // ── 收敛正确性：线性域梯度能量比（分子分母**同一个算子**，见 phaseAverageLuma 注释）──
    const double gradRef = R1.grad;  // fb=1 的相位平均 = 理想 16 相位平均（按构造）
    const auto ratio = [gradRef](double g) { return gradRef > 0.0 ? g / gradRef : 0.0; };
    const double gR2 = ratio(R2.grad);
    std::printf("\n  收敛正确性（梯度能量比，线性域；参考 = R1 fb=1 一周期的相位平均 = %s）\n",
        d6(gradRef).c_str());
    std::printf("    R2 fb=.12  梯度 %s  比值 %s", d6(R2.grad).c_str(), d6(gR2).c_str());
    if (hasSlow)
        std::printf("   R4 慢累积  梯度 %s  比值 %s", d6(runs[4].grad).c_str(),
            d6(ratio(runs[4].grad)).c_str());
    std::printf("\n    参照（只打印不判）：完全不平均的单帧 = %s、R0（TAA 关，未抖）= %s\n",
        d6(ratio(R1.gradSingle)).c_str(), d6(ratio(R0.grad)).c_str());
    const double byteRatio = R1.byteGrad > 0.0 ? R2.byteGrad / R1.byteGrad : 0.0;
    std::printf("    ⚠ 同一份数据在**显示空间**算这条给 %s（`mean(tonemap(x)) ≠ tonemap(mean(x))`，"
                "TAA 累积在线性 HDR 域、截图在 ACES+sRGB 之后）——域选错的代价，实测非引用\n",
        d6(byteRatio).c_str());

    // ── 判据 ────────────────────────────────────────────────────────────────
    std::vector<Verdict> vs;

    // ① 确定性地板（复用 `determinism`：同配置 + 静止 ⟹ 逐位相同，无容差）。
    vs.push_back(verdictFor("determinism",
        R0.byteMaxVsFirst == 0.0 && R0.changedVsFirst == 0.0 && R0.stat.meanSwing == 0.0,
        "R0（TAA 关 + 静止机位）连拍 " + std::to_string(R0.stat.frames)
            + " 帧：与首帧逐字节最大偏离 " + std::to_string(static_cast<int>(R0.byteMaxVsFirst))
            + " LSB、变化像素 " + d2(R0.changedVsFirst * 100.0) + "%、meanSwing "
            + d6(R0.stat.meanSwing) + "。判据 = **恰为 0**（不是接近 0）；非零则一切残差读数"
              "是噪声比噪声"));

    // ② 折叠自检：R1（fb=1，**无记忆**）同相位两半必须逐位相同 ⟹ 半间差恰为 0。
    //    有记忆组容差 = IIR 尾巴闭式 (1−α)^warm × 满幅（= 靶白电平）。
    // 半间差的容差 = **两项之和**，两项各有独立机制，缺任何一项都会把正常读数判成失效：
    //   ① IIR 尾巴 (1−α)^warm × 满幅：热身结束时历史离收敛还差这么多，它是「同相位两半」
    //      唯一**物理**的差别（满幅取靶白电平 = mask 内 p99）。
    //   ② 量化项 = mask 内**最亮**电平处翻一格的最大真实跨度（`maxLsbStepUpTo`：逐字节扫真
    //      解码器取离散步长，**不用**闭式导数——闭式在 sRGB 拐点以下根本不是同一个函数，而这里
    //      读的就是 8bit 字节；且必须取 max 不是 p99，理由见其头注释）。①的残差经 ACES+sRGB
    //      量化后，落在分箱边界附近的那批像素在两半各翻一格。
    // 实测 ② 比 ① 大 36 倍 ⟹ 这条容差实际由量化决定，不是由 IIR 决定。
    const double full = std::max(1e-9, R2.whiteLuma);
    const auto iirTol = [full, quant = R2.quant](double alpha, int warm) {
        return std::pow(1.0 - alpha, static_cast<double>(warm)) * full + quant;
    };
    const double tolR2 = iirTol(0.12, R2.warm);
    const double tolR4 = hasSlow ? iirTol(runs[4].feedback, runs[4].warm) : 0.0;
    vs.push_back(verdictFor("temporal-residual-phase-fold",
        R1.stat.halfGapMax == 0.0 && R2.stat.halfGapMax <= tolR2
            && (!hasSlow || runs[4].stat.halfGapMax <= tolR4),
        "同相位两半之差（折叠口径的前提）：R1（fb=1，无记忆）半间最大 "
            + d6(R1.stat.halfGapMax) + " 须**恰为 0**；R2（fb=.12）" + d6(R2.stat.halfGapMax)
            + " ≤ IIR 尾巴 (1−α)^warm × 满幅 + 一格真实跨度 " + d6(R2.quant) + " = " + d6(tolR2)
            + "；R4 " + d6(hasSlow ? runs[4].stat.halfGapMax : 0.0) + " ≤ " + d6(tolR4)
            + "。非零超界 = 连拍不是抖动相位的确定性函数（热身不足 / 窗口没对齐周期 / "
              "抓的不是连续帧），此时所有 swing 读数量的都是瞬态"));

    // ③ 阳性锚点：只抖不滤的残差必须 ≫ 0。下限写成 **τ 的倍数**（τ = 场景白电平处 1 个显示
    //    LSB 的线性跨度，运行时现算）——「量具有力气」的定义是「摆动能在 8bit 显示上走出至少
    //    一格」，与靶的绝对亮度无关，所以它不该是一个写死的线性数。
    const double anchorFloor = kAnchorMeanSwingTau * tau;
    vs.push_back(verdictFor("temporal-residual-anchor",
        R1.stat.meanSwing >= anchorFloor && R1.stat.fracOverTau >= kAnchorFracFloor,
        "R1（fb=1 只抖不滤 = 闪烁本体）meanSwing " + d6(R1.stat.meanSwing) + "（下限 = "
            + d2(kAnchorMeanSwingTau) + "×τ = " + d6(anchorFloor) + "，即 mask 平均摆动至少 1 个"
            "显示 LSB）、frac(swing>τ) " + d2(R1.stat.fracOverTau * 100.0) + "%（下限 "
            + d2(kAnchorFracFloor * 100.0) + "%）、maxSwing " + d6(R1.stat.maxSwing)
            + "。没有这条，R2/R3 的「残差为 0」可能只是量具坏了或靶没画"));

    // ④ 反向用例：掐掉激励源（jitter_scale=0）残差必须恰为 0。
    vs.push_back(verdictFor("temporal-residual-jitter-off",
        R3.stat.meanSwing == 0.0 && R3.stat.maxSwing == 0.0,
        "R3（TAA 开、fb=.12、**jitter_scale=0**）meanSwing " + d6(R3.stat.meanSwing)
            + "、maxSwing " + d6(R3.stat.maxSwing) + "、与首帧逐字节最大 "
            + std::to_string(static_cast<int>(R3.byteMaxVsFirst))
            + " LSB。判据 = **恰为 0**：抖动是唯一的激励源，静止机位 + 零抖动 ⟹ 每帧同一张图 "
              "⟹ 任何非零摆动都与「抖动」无关，是量具或引擎自己的噪声"));

    // ⑤ 工作点信号在场带（本命令的主体读数）。⚠ **带**而不是上限：下限红 = 信号消失（靶 /
    //    mask / 采集链路坏了，或漂移被修好了），上限红 = 回归变差。两种红的处置完全不同，
    //    所以打印里把方向写出来。门开态整幅游动被消掉 ⟹ 带整体下移，见常量表。
    const double magLo = baseSpace ? kMagnitudeMeanLoBase : kMagnitudeMeanLo;
    const double magHi = baseSpace ? kMagnitudeMeanHiBase : kMagnitudeMeanHi;
    const char* magDir = R2.stat.meanSwing < magLo ? "低于下限（信号消失向）"
                                                   : (R2.stat.meanSwing > magHi ? "高于上限（回归向）" : "带内");
    vs.push_back(verdictFor("temporal-residual-magnitude",
        R2.stat.meanSwing >= magLo && R2.stat.meanSwing <= magHi
            && R2.stat.fracOverTau <= kMagnitudeFracCap,
        "R2（fb=.12 现行工作点，收敛后）meanSwing " + d6(R2.stat.meanSwing) + " 须落在 ["
            + d6(magLo) + ", " + d6(magHi) + "]（" + magDir + "；门关实测 0.022600 / 门开 "
            "0.004459）、frac(swing>τ) " + d2(R2.stat.fracOverTau * 100.0) + "%（上限 "
            + d2(kMagnitudeFracCap * 100.0) + "%）、p99.9 " + d6(R2.stat.p999Swing) + "、max "
            + d6(R2.stat.maxSwing)
            + "。**这是 `temporal` 四条全绿时仍然在场的那个绝对量**——抑制比好不等于看不见。"
              "⚠ 本条**不是可见性阈值**（可见性要按区域判，见注册表理由）"));

    // ⑥ 分辨力轴：把累积窗拉长（α→0.002，等效窗 500 帧 ≫ 周期 16）残差必须显著下降。
    if (hasSlow) {
        const Run& R4 = runs[4];
        vs.push_back(verdictFor("temporal-residual-monotone",
            R4.stat.meanSwing <= kMonotoneCap * R2.stat.meanSwing
                && R4.stat.fracOverTau < R2.stat.fracOverTau,
            "R4（fb=" + d4(slowFeedback) + "，等效窗 1/α = "
                + std::to_string(static_cast<int>(1.0 / slowFeedback))
                + " 帧 ≫ 抖动周期 16）meanSwing " + d6(R4.stat.meanSwing) + " 须 ≤ "
                + d2(kMonotoneCap * 100.0) + "% × R2 的 " + d6(R2.stat.meanSwing)
                + "，且 frac(swing>τ) " + d2(R4.stat.fracOverTau * 100.0) + "% 须 < R2 的 "
                + d2(R2.stat.fracOverTau * 100.0)
                + "%。**这条不判「TAA 好不好」，判「本量具在这条轴上有没有力气」**——不单调 "
                  "= swing 与累积窗无关 = 量具无效，⑤ 的读数一并作废"));
    } else {
        std::printf("\n  ⚠ 已跳过 R4（--skip-slow）：分辨力轴未验，⑤ 没有「量具有力气」的旁证\n");
    }

    // ⑦ 收敛正确性：线性域梯度比须贴着理想 16 相位平均。
    vs.push_back(verdictFor("temporal-residual-convergence",
        gR2 >= kGradRatioLo && gR2 <= kGradRatioHi,
        "R2 收敛连拍的相位平均 vs 理想 16 相位平均（= R1 fb=1 一周期的相位平均）的**线性域**"
        "梯度能量比 = "
            + d6(gR2) + "（须落在 [" + d2(kGradRatioLo) + ", " + d2(kGradRatioHi)
            + "]）。低于下限 = 比理想平均还糊（过度累积 / 核太肥）；高于上限 = 没累积到"
              "（历史被拒 ⟹ 输出退化成本帧，同一次运行的参照：完全不平均的单帧比值 "
            + d6(ratio(R1.gradSingle)) + "）。⚠ 比值只能在**线性域**算：显示空间同一份数据给 "
            + d6(byteRatio) + "。⚠ 本条的**区分力随门变**：门关态收敛 0.887–0.893、退化参照 "
              "1.031–1.083（取连拍末帧，随 burst 起始相位浮动 5% ⟹ 只打印不判），上限 1.02 卡在"
              "两者中间 ⟹ 有区分力；门开态收敛 0.966–0.968、退化 1.004，上限 1.02 **高于**退化"
              "读数 ⟹ 上界在门开态是惰性的（只有下界有用），判「历史有没有生效」请用 ⑥ 的分辨力轴"));

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("temporal-residual", {{"periods", std::to_string(periods)},
               {"warmMult", d4(warmMult)}, {"variant", variant.empty() ? "forward" : variant},
               {"tau", d6(tau)}, {"white", d6(R2.whiteLuma)},
               {"R0_swing", d6(R0.stat.meanSwing)}, {"R1_swing", d6(R1.stat.meanSwing)},
               {"R2_swing", d6(R2.stat.meanSwing)}, {"R3_swing", d6(R3.stat.meanSwing)},
               {"R4_swing", d6(hasSlow ? runs[4].stat.meanSwing : 0.0)},
               {"R2_p999", d6(R2.stat.p999Swing)}, {"R2_max", d6(R2.stat.maxSwing)},
               {"R2_frac", d2(R2.stat.fracOverTau * 100.0)},
               {"R1_frac", d2(R1.stat.fracOverTau * 100.0)},
               {"R1_halfGapMax", d6(R1.stat.halfGapMax)},
               {"R2_halfGapMax", d6(R2.stat.halfGapMax)},
               {"R2_gradRatio", d6(gR2)}, {"R2_byteGradRatio", d6(byteRatio)},
               {"R4_gradRatio", d6(hasSlow ? ratio(runs[4].grad) : 0.0)},
               {"R1_singleGradRatio", d6(ratio(R1.gradSingle))},
               {"R0_byteMax", std::to_string(static_cast<int>(R0.byteMaxVsFirst))},
               {"R3_byteMax", std::to_string(static_cast<int>(R3.byteMaxVsFirst))}},
        vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    return ok ? 0 : 1;
}

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
