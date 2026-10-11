//
// temporal_homography_metric.cpp — `temporal-homography` 的**重投影算子层**（口径，不碰引擎）
//
//   本 TU 里的每一条都是**仪器自己的标定锚点**，不是"实现细节"：
//   `identityCheck`（零偏移逐位取回原值）/ `translationCheck`（整数平移一列）/
//   `bilinearCrossCheck`（与 cv::warpPerspective 比量级）三条 check 红 = **本实现抄错了核**，
//   与引擎无关。所以：
//
//     ⚠ **一个权重系数都不许"化简"或合并**，包括边界处理与归一化分支。三次项取 |a|、Lanczos3 的
//       两条显式归零、抽头基址 `1 − NTap/2`，都是与 `taa.frag.slang` 逐式同约定的那一半；权重式
//       与抽头索引是同一个约定的两半，改一处必须改另一处（下面逐行标注出处）。
//     ⚠ `histPercentile`（直方图法）与 `fieldPercentile`（排序法）是**两条口径**，不是同一个函数
//       的两种写法：前者把 ≥1 的值全塞进最后一个 bin。插值方式与"第几个序"就是判据。
//
//   缝在哪 → `temporal_homography_metric.h` 头注释。跨 TU 的类型与常量唯一出处也在那里。
//
#include "temporal_homography_metric.h"

#include "temporal_homography.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
namespace {

constexpr int kHistBins = 1 << 16;  ///< [0,1] 线性域直方图：bin 宽 1.5e-5 ≪ 1 显示 LSB

// ── 与**引擎 TAA 同权重**的重采样核（2026-10-06 用户拍板：工具核必须与引擎核同族）─────────
//
// ## 为什么原来的 `cv::warpPerspective(INTER_LINEAR)` 不够
//
// 本尺子的残差 = |I_k − warp(I_{k−1})|，而 warp **自己要插值** ⟹ 残差里混着"warp 核与引擎
// 核不一致"带来的差。双线性与引擎的 Catmull-Rom / Lanczos3 差多少？差在**负瓣**：双线性权重
// 恒 ∈[0,1]（数学上表示不了负瓣，`taa.frag.slang:187` 那条注释就是为此写的），所以对同一条
// 1px 高对比边，双线性比引擎核**更糊**。于是：
//
//   * 残差被**高估**（糊度差本身算进残差），地板也被高估，但两者不同尺度 ⟹ 残差/地板不可信；
//   * 更要命的是「符号自证」：把两帧 warp 到同一点时，两帧各自的亚像素相位不同 ⟹ 两次双线性
//     糊出**两个不同**的图，差值天然不为零。核越糊，这个假差越大 ⟹ 自证**必然**失败。
//     实测（bistro 门框，sweep 0.5）：自证 0.6015 vs 对照 0.7652，只掉 21%（要求 ≥30%）。
//
// ⟹ 换成与引擎**逐式同权重**的核，自证的"零"才真的是零（核越接近恒等，两帧糊得越像）。
//
// ## 三条照抄细节（错一条整个自证就变味，出处逐行标注）
//
// 1. **三次项取 |a|**：`a3 = a2·|a|`（`taa.frag.slang:174`）。写成带符号 a³ 会让 t=0 的权重从
//    (0,1,0,0) 变成 (0,1,−3,8)（Σ=6）⟹ **恒等性破产**，自证的零点不再是零（slang 那条注释
//    记着它当年把 2D 直流增益放大 36×）。
// 2. **Lanczos3 两条显式归零一条都不能省**（`slang:232-238`）：`|x|<1e-5 → 1`（可去奇点）与
//    非零整数处解析零点 → 0。float32 的 sin(π)=8.7e-8 ≠ 0，不显式归零则"零偏移 = 精确插值"
//    只在 1e-7 意义上成立。
// 3. **抽头基址与纹素中心约定**：`p = uv/texel − 0.5` 把"纹素 i 的中心"放到整数 i 上
//    （`slang:197/251`），OpenCV 的 warp 坐标约定（整数 = 纹素中心）与之**天然一致**，故这里
//    直接 `b = floor(s)`、CR 抽头 `b−1+i`（`slang:210`）、Lanczos3 抽头 `b−2+i`（`slang:269`）；
//    取数用**整数 clamp**（= CLAMP_TO_EDGE，`slang:210/269`），与 cv::BORDER_REPLICATE 同义。
// 枚举本身声明在 HomoStat 之前（守卫量要带核标签）。

/// 一维 Catmull-Rom 4 抽头权重 —— 逐式对应 `taa.frag.slang:168 catmullRomWeights`
void catmullRomWeights1D(double t, double w[4])
{
    for (int i = 0; i < 4; ++i) {
        const double a = t + 1.0 - static_cast<double>(i);  // 到抽头 b−1+i 的有向距离 ∈ (−2,2]
        const double m = std::abs(a), a2 = a * a, a3 = a2 * m;  // ⚠ 三次项取 |a|（slang:174）
        w[i] = m > 1.0 ? -0.5 * a3 + 2.5 * a2 - 4.0 * m + 2.0 : 1.5 * a3 - 2.5 * a2 + 1.0;
    }
}

/// 一维 Lanczos3 权重 —— 逐式对应 `taa.frag.slang:229 lanczos3Weight`（两条归零分支都在）
double lanczos3Weight1D(double x)
{
    const double ax = std::abs(x);
    if (ax < 1e-5)
        return 1.0;
    if (ax >= 3.0)
        return 0.0;
    const double r = std::round(ax);
    if (r >= 1.0 && std::abs(ax - r) < 1e-5)
        return 0.0;  // 解析零点
    const double px = 3.14159265358979323846 * x;
    const double p3 = px / 3.0;
    return (std::sin(px) / px) * (std::sin(p3) / p3);
}

void lanczos3Weights1D(double t, double w[6])
{
    for (int i = 0; i < 6; ++i)
        w[i] = lanczos3Weight1D(t + 2.0 - static_cast<double>(i));  // 到抽头 b−2+i 的距离
}

/// N 抽头可分离核（4 = CR，6 = Lanczos3）。基址 = −NTap/2，逐字对应 slang 的 b−1+i / b−2+i。
template <int NTap>
void warpTaps(const cv::Mat& src, const SrcMap& m, cv::Mat& dst)
{
    // ⚠ 基址 = 1 − NTap/2：CR(4) → −1（`slang:210` 的 b−1+i）、Lanczos3(6) → −2（`slang:269`
    //   的 b−2+i）。写成 −NTap/2（本函数第一版就是这么错的）会把整组抽头平移一个像素：
    //   t=0 处权重仍是 (0,1,0,0)，但落在 b∓1 上 ⟹ 核变成「恒等 + 平移 1px」。
    //   **恒等性自检正是为此而设**——它当场报最大偏差 ≈ 一个边缘的幅度，不是悄悄通过。
    //   权重式与抽头索引是**同一个约定的两半**，改一处必须改另一处。
    constexpr int kBase = 1 - NTap / 2;
    const int W = src.cols, H = src.rows;
    for (int r = 0; r < H; ++r) {
        const double* mxr = m.x.ptr<double>(r);
        const double* myr = m.y.ptr<double>(r);
        double* dr = dst.ptr<double>(r);
        for (int c = 0; c < W; ++c) {
            const double sx = mxr[c], sy = myr[c];
            const int bx = static_cast<int>(std::floor(sx)), by = static_cast<int>(std::floor(sy));
            double wx[NTap], wy[NTap];
            if constexpr (NTap == 4) {
                catmullRomWeights1D(sx - static_cast<double>(bx), wx);
                catmullRomWeights1D(sy - static_cast<double>(by), wy);
            }
            else {
                lanczos3Weights1D(sx - static_cast<double>(bx), wx);
                lanczos3Weights1D(sy - static_cast<double>(by), wy);
            }
            double acc = 0.0;
            for (int j = 0; j < NTap; ++j) {
                const int qy = std::clamp(by + kBase + j, 0, H - 1);  // 整数 clamp = CLAMP_TO_EDGE
                const double* sp = src.ptr<double>(qy);
                const double wj = wy[j];
                for (int i = 0; i < NTap; ++i) {
                    const int qx = std::clamp(bx + kBase + i, 0, W - 1);
                    acc += wx[i] * wj * sp[qx];
                }
            }
            dr[c] = acc;
        }
    }
}

/// 双线性（= 旧口径，保留用于对拍与「换核前后差多少」的自曝）
void warpBilinear(const cv::Mat& src, const SrcMap& m, cv::Mat& dst)
{
    const int W = src.cols, H = src.rows;
    for (int r = 0; r < H; ++r) {
        const double* mxr = m.x.ptr<double>(r);
        const double* myr = m.y.ptr<double>(r);
        double* dr = dst.ptr<double>(r);
        for (int c = 0; c < W; ++c) {
            const double sx = mxr[c], sy = myr[c];
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            const double fx = sx - static_cast<double>(x0), fy = sy - static_cast<double>(y0);
            const int cx0 = std::clamp(x0, 0, W - 1), cx1 = std::clamp(x0 + 1, 0, W - 1);
            const int cy0 = std::clamp(y0, 0, H - 1), cy1 = std::clamp(y0 + 1, 0, H - 1);
            const double* r0 = src.ptr<double>(cy0);
            const double* r1 = src.ptr<double>(cy1);
            dr[c] = (1.0 - fy) * ((1.0 - fx) * r0[cx0] + fx * r0[cx1])
                + fy * ((1.0 - fx) * r1[cx0] + fx * r1[cx1]);
        }
    }
}

}  // namespace

/// 绝对差图的 p 分位（直方图法；逐像素排序在 1280×720×31 对上要几十亿次比较）。
double histPercentile(const cv::Mat& absdiff, double p)
{
    std::vector<size_t> hist(kHistBins, 0);
    for (int y = 0; y < absdiff.rows; ++y) {
        const float* row = absdiff.ptr<float>(y);
        for (int x = 0; x < absdiff.cols; ++x) {
            int b = static_cast<int>(row[x] * static_cast<float>(kHistBins));
            hist[static_cast<size_t>(std::clamp(b, 0, kHistBins - 1))]++;
        }
    }
    const size_t n = static_cast<size_t>(absdiff.rows) * static_cast<size_t>(absdiff.cols);
    const size_t want = std::min(static_cast<size_t>(static_cast<double>(n) * p), n - 1);
    size_t acc = 0;
    for (int i = 0; i < kHistBins; ++i) {
        acc += hist[static_cast<size_t>(i)];
        if (acc > want)
            return static_cast<double>(i) / static_cast<double>(kHistBins);
    }
    return 1.0;
}

/// 把 [0,1] 的残差图写成可比的热图（**固定标尺** 0→黑 / 1→白，两组之间才能对眼）
void writeHeat(const cv::Mat& heat, const std::string& path)
{
    cv::Mat byte;
    heat.convertTo(byte, CV_8UC1, 255.0);  // 固定标尺，不做 min/max 自缩放（自缩放会让两组看起来一样）
    cv::imwrite(path, byte);
}

[[nodiscard]] const char* kernelName(WarpKernel k)
{
    switch (k) {
    case WarpKernel::Bilinear: return "bilinear(旧口径)";
    case WarpKernel::CatmullRom: return "CatmullRom(=引擎历史侧)";
    case WarpKernel::Lanczos3: return "Lanczos3(=引擎默认)";
    }
    return "?";
}

SrcMap buildSrcMap(const cv::Matx33d& dstToSrc, cv::Size sz)
{
    SrcMap m;
    m.x = cv::Mat(sz, CV_64FC1);
    m.y = cv::Mat(sz, CV_64FC1);
    for (int r = 0; r < sz.height; ++r) {
        double* xr = m.x.ptr<double>(r);
        double* yr = m.y.ptr<double>(r);
        for (int c = 0; c < sz.width; ++c) {
            const double vx = static_cast<double>(c), vy = static_cast<double>(r);
            const double den = dstToSrc(2, 0) * vx + dstToSrc(2, 1) * vy + dstToSrc(2, 2);
            xr[c] = (dstToSrc(0, 0) * vx + dstToSrc(0, 1) * vy + dstToSrc(0, 2)) / den;
            yr[c] = (dstToSrc(1, 0) * vx + dstToSrc(1, 1) * vy + dstToSrc(1, 2)) / den;
        }
    }
    return m;
}

void warpKernel(const cv::Mat& src, const SrcMap& m, WarpKernel k, cv::Mat& dst)
{
    dst.create(src.size(), CV_64FC1);
    switch (k) {
    case WarpKernel::Bilinear: warpBilinear(src, m, dst); break;
    case WarpKernel::CatmullRom: warpTaps<4>(src, m, dst); break;
    case WarpKernel::Lanczos3: warpTaps<6>(src, m, dst); break;
    }
}

/// 恒等性自检：核在**零偏移**处必须逐位取回原值（`temporal-identity` 判据的算术基础）。
/// 返回最大绝对偏差；非零 = **本实现抄错了核**（不是"引擎有误差"）。
double identityCheck(const cv::Mat& src, WarpKernel k)
{
    const cv::Matx33d eye = cv::Matx33d::eye();
    const SrcMap m = buildSrcMap(eye, src.size());
    cv::Mat w;
    warpKernel(src, m, k, w);
    cv::Mat d;
    cv::absdiff(src, w, d);
    return cv::norm(d, cv::NORM_INF);
}

/// **整数平移**自检：把内容搬恰好 1 个像素，任何"整数偏移处精确"的核都必须逐位取回平移值。
/// 这条比「与 cv::warpPerspective 对拍」强：它同时钉住**坐标约定**（整数 = 纹素中心）与
/// **抽头基址**（`kBase` 错一格 ⟹ 平移量错一格 ⟹ 当场爆）。
double translationCheck(const cv::Mat& src, WarpKernel k)
{
    // dst→src = x−1（映射把源坐标往**左**取 ⟹ 内容往**右**移一列，与下面 want 同向）。
    // ⚠ 本函数第一版填的是 +1，得到「内容左移」而 want 仍是右移 ⟹ 自检报出 O(1)（实测 0.97
    //   = 一条边的幅度）。也就是说：**这条自检抓到了它自己第一版的错**，这正是它存在的理由。
    const cv::Matx33d shift = cv::Matx33d(1, 0, -1.0, 0, 1, 0, 0, 0, 1);
    const SrcMap m = buildSrcMap(shift, src.size());
    cv::Mat w;
    warpKernel(src, m, k, w);
    cv::Mat want(src.size(), CV_64FC1);  // 期望 = src 右移一列（首列 replicate = 自己）
    for (int r = 0; r < src.rows; ++r) {
        const double* sp = src.ptr<double>(r);
        double* wp = want.ptr<double>(r);
        for (int c = 0; c < src.cols; ++c)
            wp[c] = sp[c > 0 ? c - 1 : 0];
    }
    cv::Mat d;
    cv::absdiff(w, want, d);
    return cv::norm(d, cv::NORM_INF);
}

/// 与 `cv::warpPerspective(INTER_LINEAR, BORDER_REPLICATE)` 对拍（只 bilinear 路可比）。
/// ⚠ **不能要求逐位为 0**：OpenCV 的 INTER_LINEAR 走 **5 位定点**权重（`INTER_BITS=5`，
///   权重只有 1/32 的分辨率），本实现是 float。差值的量级 = 1/64 × 局部梯度，实测 720p 的
///   高对比边上 ≈ 2e-2（线性 luma）。所以这条报的是「**量级**对不对」，不是「等不等」：
///   约定真错（差一整个纹素）会报出 **O(1)**，与定点量化差三个量级，当场可分。
double bilinearCrossCheck(const cv::Mat& src, const cv::Matx33d& dstToSrc)
{
    const SrcMap m = buildSrcMap(dstToSrc, src.size());
    cv::Mat mine;
    warpKernel(src, m, WarpKernel::Bilinear, mine);  // ⚠ 必须走 warpKernel：它才分配 dst
    cv::Mat theirs;
    cv::warpPerspective(src, theirs, cv::Mat(dstToSrc.inv()), src.size(), cv::INTER_LINEAR,
        cv::BORDER_REPLICATE);
    cv::Mat d;
    cv::absdiff(mine, theirs, d);
    return cv::norm(d, cv::NORM_INF);
}

/// 跨帧最大场（acc = max(acc, x)）——分带与 τ 用同一片「逐像素最坏」证据。
void maxInto(cv::Mat& acc, const cv::Mat& x)
{
    if (acc.empty())
        x.copyTo(acc);
    else
        cv::max(acc, x, acc);
}

/// 均值（守卫用它，不用直方图分位：`histPercentile` 把 ≥1 的值全塞进最后一个 bin，
/// 而线性 HDR luma 的残差**可以**超过 1 —— 分位数会被那个 bin 的边界顶住，均值没这个问题）。
double meanOf(const cv::Mat& x)
{
    return cv::mean(x)[0];
}

/// 逐像素场的分位（排序法；只在**每段一次**的 τ / 守卫上用，逐帧仍走直方图）
double fieldPercentile(const cv::Mat& f, double p)
{
    auto v = valuesInMask(f, {});
    std::sort(v.begin(), v.end());
    return percentileOf(v, p);
}

/**
 * @brief 按 **α × Δψ** 的强度 warp 上一帧到本帧网格，返回跨帧最大残差场的均值
 *
 * α=0 时单应是恒等 ⟹ 这个函数天然给出「不 warp 的原始帧差」，与 α=1 用**同一套代码同一种
 * 口径**（另开一条 raw 分支 = 两条实现可能不同源，曲线就不是同一条尺子量的）。
 */
double sweepResidual(
    const FlickerSeries& g, double yawRadPerFrame, double alpha, WarpKernel kernel)
{
    const double focal = verticalFocalPx(g.frameSize.height, kFovDeg);
    const cv::Size sz = g.frameSize;
    const cv::Matx33d H
        = yawHomography(alpha * yawRadPerFrame, focal, sz.width * 0.5, sz.height * 0.5);
    const SrcMap m = buildSrcMap(H.inv(), sz);
    cv::Mat acc;
    for (size_t k = 1; k < g.luma.size(); ++k) {
        cv::Mat w, r;
        warpKernel(g.luma[k - 1], m, kernel, w);
        cv::absdiff(g.luma[k], w, r);
        maxInto(acc, r);
    }
    return acc.empty() ? 0.0 : meanOf(acc);
}

cv::Matx33d yawHomography(double yawRadPerFrame, double focalPx, double cxPx, double cyPx)
{
    const double t = std::tan(yawRadPerFrame), f = focalPx, cx = cxPx, cy = cyPx;
    // x' = [(f−cx·t)x + t(cx²+f²)] / [−t·x + (f+t·cx)]，y' = [−cy·t·x + f·y + cy·cx·t] / 同分母
    return cv::Matx33d(
        f - cx * t, 0.0, t * (cx * cx + f * f),
        -cy * t, f, cy * cx * t,
        -t, 0.0, f + t * cx);
}

}  // namespace RGL::imgqa
