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

double gradEnergy(const cv::Mat& luma, const cv::Mat& mask)
{
    cv::Mat gx, gy, g;
    cv::Sobel(luma, gx, CV_64F, 1, 0, 1);
    cv::Sobel(luma, gy, CV_64F, 0, 1, 1);
    g = cv::abs(gx) + cv::abs(gy);  // MatExpr 必须先落地成 Mat 才是 InputArray
    if (mask.empty())
        return cv::mean(g)(0);
    return cv::mean(g, mask)(0);
}

namespace {

/// mask 内单通道图的 p 分位（白电平 / p99.9 用）。输入须已升序。
double percentileOf(const std::vector<double>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    const size_t i = std::min(sorted.size() - 1,
        static_cast<size_t>(p * static_cast<double>(sorted.size())));
    return sorted[i];
}

/// 折叠 + swing 的**唯一实现**（线性 luma 判据与显示空间报告共用同一份代码，杜绝两套口径漂移）。
/// `at(i, y, x)` 取第 i 帧在 (y,x) 的值并折成 double。
struct SwingCore {
    double mean = 0.0, p999 = 0.0, mx = 0.0, fracOver = 0.0;
    double halfGap = 0.0, halfGapMax = 0.0, meanValue = 0.0;
    int64_t pixels = 0;
};

template <class At>
SwingCore swingFoldCore(const cv::Size& sz, const cv::Mat& m, double tau, int periods, At&& at)
{
    SwingCore s;
    const int perHalf = periods / 2;
    std::vector<double> v0(kResidualPeriod), v1(kResidualPeriod), v(kResidualPeriod);
    std::vector<double> swings;
    swings.reserve(static_cast<size_t>(std::max<int64_t>(1, cv::countNonZero(m))));
    double sumSwing = 0.0, sumVal = 0.0, halfSum = 0.0;
    int64_t overTau = 0;
    for (int y = 0; y < sz.height; ++y) {
        const uchar* pm = m.ptr<uchar>(y);
        for (int x = 0; x < sz.width; ++x) {
            if (!pm[x])
                continue;
            double colSum = 0.0;
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
            }
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

}  // namespace

ResidualStat phaseSwingStat(const std::vector<Frame>& shots, const cv::Mat& mask, double tau,
    int periods)
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
        [&shots](size_t i, int y, int x) { return shots[i].luma.at<double>(y, x); });
    s.meanSwing = c.mean;
    s.p999Swing = c.p999;
    s.maxSwing = c.mx;
    s.fracOverTau = c.fracOver;
    s.halfGap = c.halfGap;
    s.halfGapMax = c.halfGapMax;
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
    double warmMult, double& tau, bool baseSpace)
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
    o.variant = variant;
    o.size = {kSize[0], kSize[1]};
    makeProfile(dir, o);

    // 热身 = ⌈warmMult/α⌉ 向上对齐到抖动周期。α = feedback（EMA 记忆长度 1/α）；TAA 关时没有
    // 历史，一个周期纯走稳采集本身。⚠ 热身不足**不会报错**：它把画面冻在初始相位上，残差
    // 反而更小 ⟹ 读数是「更好」的假绿，所以这里宁可多热。
    const double alpha = taa ? std::clamp(feedback, 1e-6, 1.0) : 1.0;
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
                "半间差 %8s/%-8s  与首帧 %2.0fLSB/%5.2f%%\n",
        r.name.c_str(), r.warm, d6(r.stat.meanSwing).c_str(), d6(r.stat.p999Swing).c_str(),
        d6(r.stat.maxSwing).c_str(), r.stat.fracOverTau * 100.0, d6(r.stat.halfGap).c_str(),
        d6(r.stat.halfGapMax).c_str(), r.byteMaxVsFirst, r.changedVsFirst * 100.0);
    std::printf("  %-24s 显示域（只报不判）meanSwing %-7s p99.9 %-6s max %-5s frac>1LSB %5.2f%%\n",
        "", d4(r.byteMeanSwing).c_str(), d2(r.byteP999).c_str(), d2(r.byteMax).c_str(),
        r.byteFrac * 100.0);
}

}  // namespace

int cmdTemporalResidual(int periods, double warmMult, const std::string& variant,
    double slowFeedback, bool skipSlow, bool baseSpace)
{
    if (periods < 2 || periods % 2 != 0) {
        std::printf("--periods 必须是 ≥2 的偶数（两半折叠 = 同绝对相位对照）。收到 %d\n", periods);
        return 2;
    }
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

}  // namespace RGL::imgqa
