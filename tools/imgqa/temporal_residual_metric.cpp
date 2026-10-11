// `temporal-residual` 家族的**纯度量算子层**（批 13.1 主判据的口径本体）。
//
//   从 `temporal_residual.cpp` 拆出（文件护栏 500 代码行）。这一层的判据价值全在**算子本身**：
//   折叠配对、中位数、掩膜口径、分带边界、拒收分桶、EMA 闭式——每一条的边界比较符与
//   tie-break 都是判据的一部分，不许「化简」。selftest 直接打这一层（不需要引擎、不需要窗口），
//   所以这一层**不碰** Engine / RPC / 截图，只吃 cv::Mat 与 Frame。
//
//   度量定义与三条测量红线（线性域 / 热身不写盘 / 配置须带 environment 段）的完整理由 =
//   `temporal_residual.h` 头注释；折叠口径为什么不需要标定相位 = 命令入口 TU 的头注释。
//   阈值与定点格式化 = `temporal_residual_metric.h`（唯一出处）。

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

namespace {

/// 分带输入**坏了**的判据（尺子与观测量必须来自同一次采集、同类型）。
/// ⚠ 六个条件与比较符逐字搬自原实现：「表看起来正常但像素是错位的」是这套仪器里最贵的一类
/// 失效，与 `phaseAverageLuma` 拒收混 extent 同一课。
bool bandInputsBroken(const cv::Mat& swing, const cv::Mat& lumaByte, const cv::Mat& grad)
{
    return swing.empty() || swing.type() != CV_64FC1 || lumaByte.size() != swing.size()
        || grad.size() != swing.size() || lumaByte.type() != CV_64FC1 || grad.type() != CV_64FC1;
}

/// 一根像素落在六格表里的行/列下标。
struct BandIndex {
    int lb = 0;  ///< 亮度档：0 暗 / 1 中间调 / 2 近削顶
    int gb = 0;  ///< 梯度档：0 低 / 1 高
};

/// 分档。⚠ 边界写死：`< tLo` 暗 / `[tLo, tHi)` 中间调 / `≥ tHi` 近削顶；梯度 `≥ gradThr` 高。
/// 比较符（`<` 与 `>=` 的不对称）与档位归属 = 判据本身，不许「化简」。
BandIndex bandIndexOf(double luma, double grad, double tLo, double tHi, double gradThr)
{
    return { luma < tLo ? 0 : (luma < tHi ? 1 : 2), grad >= gradThr ? 1 : 0 };
}

/// alpha 码 → 五桶下标（0 收下 / 1 盒裁剪 / 2 塌回 centroid / 3 重投影拒收 / 4 无历史）。
/// ⚠ 四个边界值与比较符（`< 32 / < 96 / < 160 / < 224`）= 判据的分桶口径，逐字搬。
int rejectBucketOf(uint8_t code)
{
    return code < 32 ? 0 : (code < 96 ? 1 : (code < 160 ? 2 : (code < 224 ? 3 : 4)));
}

}  // namespace

BandTable bandTableOf(const cv::Mat& swing, const cv::Mat& lumaByte, const cv::Mat& grad,
    double tLo, double tHi, double gradThr, double tau)
{
    BandTable t;
    // 尺子与观测量必须是同一次采集：尺寸不符 = 直接不出数（守卫本体见 `bandInputsBroken`）。
    if (bandInputsBroken(swing, lumaByte, grad))
        return t;
    for (int y = 0; y < swing.rows; ++y) {
        const double* ps = swing.ptr<double>(y);
        const double* pl = lumaByte.ptr<double>(y);
        const double* pg = grad.ptr<double>(y);
        for (int x = 0; x < swing.cols; ++x) {
            const auto [lb, gb] = bandIndexOf(pl[x], pg[x], tLo, tHi, gradThr);
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
            ++c[rejectBucketOf(p[x])];
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

}  // namespace RGL::imgqa
