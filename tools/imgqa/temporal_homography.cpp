//
// temporal_homography.cpp — 精确单应 warp 的逐像素闪烁归因图
//
// 一句话：相机做**原地**绕 up 轴 yaw（`frame_yaw`，位置不动）⟹ 像面映射是纯旋转单应，
// 与深度无关、对整幅画面精确。把上一帧按它搬到本帧网格，剩下的差 = **搬不动的部分**
// = 采样格移动造成的锯齿差（TAA 关）/ 时域滤波做的事（TAA 开）。逐像素画出来 = 归因图。
//
// 三条纪律：
//   1. **零参考图、零对齐搜索**：单应由 tan(Δψ) + 焦距 + 主点闭式给出；符号（yaw 正方向
//      对应像面左移还是右移）用 t 与 −t **两个闭式假设对拍**取小者，不是亚像素搜索。
//      两个假设的残差都回显——符号搞错时两者差一个量级，当场可辨。
//   2. **warp 自己要插值** ⟹ 残差里混着插值误差。同批出**往返地板** |I − H⁻¹(H·I)| 的
//      p99.5：残差与地板同量级 = 量具在响，不是闪烁。
//   3. **解释率**才是主数：1 − 残差/原始帧差。残差小但原始帧差也小 = 什么都没解释。
//
#include "temporal_homography.h"
#include "material_attribution.h"
#include "temporal_flicker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
namespace {

constexpr int kSize[2] = { 1280, 720 };
constexpr float kFovDeg = 45.0f;
constexpr int kHistBins = 1 << 16;  ///< [0,1] 线性域直方图：bin 宽 1.5e-5 ≪ 1 显示 LSB

std::string d4(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}
std::string d2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f%%", v * 100.0);
    return b;
}

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

// ── 与**引擎 TAA 同权重**的重采样核（详见下面 writeHeat 之后的「为什么双线性不够」段）──────
// 名字先于 HomoStat 声明（守卫量里要带核的标签）。
enum class WarpKernel { Bilinear, CatmullRom, Lanczos3 };

/// 一组（TAA 开 / 关）的归因结果
struct HomoStat {
    std::string name;
    cv::Size size;
    double rawP995 = 0.0;     ///< 原始相邻帧差的 p99.5（不 warp，解释率的分母）
    double residP995 = 0.0;   ///< 精确单应对齐后的残差 p99.5（取符号假设中的小者）
    double residOther = 0.0;  ///< 另一个符号假设的残差（符号自证）
    double floorP995 = 0.0;   ///< 往返地板 |I − H⁻¹(H·I)| 的 p99.5（warp 插值自身误差）
    double frac1Lsb = 0.0;    ///< 残差 > 1 显示 LSB 的像素占比（对全帧）
    size_t pairs = 0;
    double byteMaxAdjacent = 0.0;
    cv::Mat heat;             ///< CV_32FC1，逐像素跨帧 max 残差
    // ── 守卫量（2026-10-06 新增；一律用**跨帧最大场**的均值，与分带表同一份证据）──────────
    WarpKernel kernel = WarpKernel::Bilinear;
    /// 显示域字节亮度（0..255）的**全帧全场**均值；−1 = 本次没算（未开分带）。
    /// 为什么守卫要它：A/B 口径能把画面**关黑**（实测 `env.intensity=0` 打在只吃天光的
    /// 机位上 ⟹ 交付面全黑 ⟹ 帧差/残差/地板全 0）。那种跑法既没有闪烁也没有归因对象，
    /// 却会因为「四个数都小」而让 1a/2/3 以另一种方式失效——必须单独点名，见守卫 ⓪。
    double lumaMean = -1.0;
    double rawMean = 0.0;     ///< |I_k − I_{k−1}| 跨帧最大场的均值
    double residMean = 0.0;   ///< 按 δ 分开 warp 的残差跨帧最大场均值（= 守卫的「对照」）
    double floorMean = 0.0;   ///< 同态往返地板跨帧最大场的均值（= 守卫的「地板」）
    cv::Mat floorHeat;        ///< 地板跨帧最大场（τ 的出处：取它的分位数）
    /// 对齐自证曲线 (α, 残差均值)。α = warp 强度倍率：0 = 不 warp（= 原始帧差），
    /// ±1 = 闭式 Δψ（含反号）。**残差必须在 |α|=1 附近见底**，否则"残差 = 运动没搬动的部分"
    /// 这句话没有依据。为什么不用「两帧搬到同一点」→ temporal_residual.h 的守卫 1 注释。
    std::vector<std::pair<double, double>> alignCurve;
};

/// 把 [0,1] 的残差图写成可比的热图（**固定标尺** 0→黑 / 1→白，两组之间才能对眼）
void writeHeat(const cv::Mat& heat, const std::string& path)
{
    cv::Mat byte;
    heat.convertTo(byte, CV_8UC1, 255.0);  // 固定标尺，不做 min/max 自缩放（自缩放会让两组看起来一样）
    cv::imwrite(path, byte);
}

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

[[nodiscard]] const char* kernelName(WarpKernel k)
{
    switch (k) {
    case WarpKernel::Bilinear: return "bilinear(旧口径)";
    case WarpKernel::CatmullRom: return "CatmullRom(=引擎历史侧)";
    case WarpKernel::Lanczos3: return "Lanczos3(=引擎默认)";
    }
    return "?";
}

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

/// dst→src 的逐像素源坐标（单应与帧无关，故一次算好给整段连拍复用）
struct SrcMap {
    cv::Mat x, y;  ///< CV_64FC1
};

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

/// 对齐自证的扫描点（**跑前写死**）：含 0（没对齐）、±1（闭式，含反号）、两侧各两档。
inline const double kAlignSweepAlphas[]
    = { 0.0, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, -0.5, -0.75, -1.0, -1.25, -1.5, -2.0 };


// ── 批 13.9：【机位状态 = **相机移动**】把 warp 残差图按「亮度 × 梯度」二维分带 ──────────
//
// ## 要判的假设（用户 2026-10-06 提出，本尺子是它的两把量具之一）
//
// 「所有**移动**锯齿的来源都是 1px **高亮**直线；**普通边缘确实没锯齿**。」
//
// 昨天批 13.7c 已给**静止机位**跨相位摆动加了同一套分带，判出「静止残余非高光专属」。
// 但那条只覆盖静止态 —— 「**移动** × 亮度 × 梯度」这一格至今是空的，用户的主张既没证实
// 也没否证。本尺子量的正是它：**纯旋转单应 warp 上一帧到本帧网格，搬不动的那部分
// = 移动时的闪烁本身**（闭式、与深度无关、零参考图、零对齐搜索）。
//
// ## 与静止态（批 13.7c）的口径关系：**复用，不另定**
//
// 分档函数 = `bandTableOf`；亮度轴 = 显示域字节亮度多帧均值（`T_lo=128`、`T_hi=240` 由
// `bandClipProxyByte` 闭式反解显示链）；梯度轴 = `sobelAbsGrad` 于线性 luma 的 **≥p90**。
// 三个阈值常量在 `temporal_residual.h` 里只有一份，**这里不许重拍**。
//
// ⚠ **被迫的差别只有一条，且必须写清**：静止态的梯度算在「相位平均图」上（静止机位下那是
// 一张干净图），移动机位**没有**这张图 —— 连拍平均会把边缘糊掉（糊量 = 扫速 × 帧数）。
// 故梯度轴取**跨帧最大**：移动时「这个像素在边缘带上」的正确含义是「burst 期间有边缘扫过它」。
//
// ## ⚠ 本尺子的固有限制（**必须在输出里自曝**）：尺子来自帧自身
//
// 本判据按构造**零参考图**（这是它相对 `temporal-motion` 的增量：不需要超采样参考、不需要
// 对齐）。代价是分带的亮度档与梯度档只能由**被测量自己的画面**定 —— 这正是昨天因**选择偏差**
// 翻过一次方向的那种做法（用被测量选区域）。具体风险：若高光只出现在某一帧，均值口径会把它
// 判出 B 档（对假设保守）；若 warp 残差本身把某块区域的表观亮度改了，档位也跟着动。
// ⟹ 本尺子的结论**只在「与尺子 2（尺子来自超采样参考）同向」时才算强结论**；两把尺子反向时
// 以尺子 2 为准，本尺子读作「量具口径受限」。
//
// ## ⭐ 判决线（**2026-10-06 跑前写死**，防事后挑数；三分支全部命名）
//
// 记 A = 中间调×高梯度（普通几何边缘）、B = 近削顶×高梯度（假设的元凶）。
// 主表 = **TAA 关**那组（= 移动时的闪烁本身，无 TAA 干预 = 用户原话里的「锯齿」）。
//
// * **分支①「支持」** ⟺ B 的**幅度份额** ≥ 30%（`kMovingSupportAmpShare`）
//   **或** 正向超额集中在 B：`rB ≥ 1.5 × rA`，其中 `rX = rate_on(X)/rate_off(X)`
//   （该格「开 TAA 后还剩多少」的比；1.5 = 跑前沿用的 `kBandSupportRatio`，不新拍）。
// * **分支②「否证」** ⟺ B 的幅度份额**与**计数份额**都** < 5%（`kMovingRefuteShare`）
//   **且** 超额不向 B 集中。
// * **分支③「分不出」** ⟺ 其余。出表处必须再报性质（三选一）：**阈值没分开**（附敏感性表，
//   换档结论跟着翻）/ **两格本来同分布**（怎么换都不翻）/ **反向分离**（超额集中在 A，
//   与用户主张方向相反）。昨天漏了「反向分离」这一支的名字，导致真结果落进未命名兜底支、
//   被读成「没有信号」；今天三分支全部命名，落进哪支照实报哪支，**不许改判决线**。
//
// 幅度份额 = Σ残差 归一化，**本次新增的胜负手**：`frac>τ` 是不加权像素计数，平坦中间调摆
// 1 LSB 与高光摆一大截各算一票，而高光是对比度最高处、同样绝对摆动**最看得见**。
// ⟹「我一直看到高亮在闪」与「高亮只占残余 0.5%」可同时为真；只有幅度份额能分辨
// 「像素少但每像素摆动巨大」。**只出计数份额 = 未完成。**
//
// ## ⛔ 守卫红分支（**2026-10-06 跑前写死**，与三分支平级、优先级更高）
//
// 任一守卫（⓪a/⓪b/1a/1b/2/3/4）不过 ⟹ **唯一**合法输出是 `Indeterminate` + 缺哪条前提 + rc≠0，
// **不许**打印①②③任何一支、**不许** rc=0。理由：旧实现这两条守卫只 ⚠ 警告，实测「四率全
// 100.00% + 自证失败」照样打印「分支②否证」且 rc=0（`/tmp/opencode/homo_before_doorframe.log`）。
// 守卫 ⓪ 是同日补的第三条实测教训：`env.intensity=0` 打在只吃天光的门框机位会把交付面整个
// 关黑（实测显示域均值 0.0044/255），帧差/残差/地板一起变 0——**「没有闪烁」与「没有画面」
// 是两件事**，后者没有归因对象。当时 `rulerOk=false` 还会让整段守卫被静默跳过直接 rc=0。
//
// ## 🧪 A/B 归因判据（**2026-10-06 写，时序如实**：在 `env=0` 那批跑废之后、剂量响应那批跑之前）
//
// 问的是「移动闪烁是 **IBL 反射**（金属/玻璃/湿面）/ **AO** / **几何边** 哪个驱动的」。
//
// * **AO**：`ao.strength=0` 是 IEEE 精确旁路，与交付基线逐格对拍。判据 = 残差/地板与材质
//   份额是否移动（不看绝对值，绝对值随画面亮度一起动，没有可比性）。
// * **IBL**：`env.intensity` 的**开关不可用**（见守卫 ⓪），改用**剂量响应** `--env 1 / 0.5 / 0.25`。
//   判据 = **残差/地板随剂量的走向**：IBL 若是信号源，关掉它残差应比地板**掉得更快**（比值**下降**）；
//   若闪烁受交付量化支配（信号与噪声同源），比值应**近似平坦**。材质侧配套判据 = 前八名**集合**
//   是否随剂量换人（换人 = IBL 驱动；不换人只抖次序 = 非 IBL 驱动）。
// * **几何边**：既有六格的梯度轴（高梯度 = 线性 luma 梯度 ≥ p90）。判据 = 高梯度格的
//   **幅度份额 / 面积份额**集中度与 rate 比（低梯度格的 rate 是它的分母参照）。
//
// ⚠ 本尺子的**固有**限制照旧（亮度/梯度档由被测量自己定 = 选择偏差）：结论只在与尺子 2
//   （超采样参考）**同向**时才算强结论，反向时以尺子 2 为准。
struct MovingBandRuler {
    cv::Mat byteMean, byteMax, gradMax;
    double tau = 0.0, tauLegacy = 0.0, white = 0.0, gradThr = 0.0, tHi = 0.0;
    std::vector<double> gradSorted;  ///< 排序后的梯度（敏感性档 p75/p95 从它来）
};

/// 从**移动机位连拍**造分带尺子。
///
/// τ 的**旧**定义留在这里当 `tauLegacy` 回显（= 场景白电平处 1 个显示 LSB 的线性跨度，闭式
/// `lsbLinearAtLuma`）；真正用于分带的 `tau` 由 `applyFloorTau` 用**本次实测地板**覆盖。
/// 为什么必须覆盖：旧 τ ≈ 0.0073 而实测地板 0.34~0.44（差 46~60 倍）⟹ 「超 τ」被地板饱和。
/// 完整理由与 p99 的取舍 → `temporal_residual.h` 的 `kMovingTauFloorPct`。
MovingBandRuler movingBandRulerFrom(const FlickerSeries& g)
{
    MovingBandRuler r;
    if (g.byteMean.empty() || g.gradMax.empty() || g.luma.empty())
        return r;
    cv::Mat acc = cv::Mat::zeros(g.frameSize, CV_64FC1);
    for (const auto& m : g.luma)
        cv::add(acc, m, acc);
    acc /= static_cast<double>(g.luma.size());
    auto vals = valuesInMask(acc, {});
    std::sort(vals.begin(), vals.end());
    r.white = percentileOf(vals, 0.99);
    r.tauLegacy = lsbLinearAtLuma(r.white);
    r.tau = r.tauLegacy;  // ← 立刻被 applyFloorTau 覆盖；这里只是"地板缺失时的兜底"
    r.byteMean = g.byteMean;
    r.byteMax = g.byteMax;
    r.gradMax = g.gradMax;
    r.gradSorted = valuesInMask(g.gradMax, {});
    std::sort(r.gradSorted.begin(), r.gradSorted.end());
    r.gradThr = percentileOf(r.gradSorted, kBandGradPct);
    r.tHi = static_cast<double>(bandClipProxyByte(kBandClipProxyLinear));
    return r;
}

/// 用**本次实测同态往返地板**（跨帧最大场）覆盖 τ。地板缺失 = 不动（保持旧口径并如实报）。
void applyFloorTau(MovingBandRuler& r, const cv::Mat& floorHeat)
{
    if (r.tauLegacy <= 0.0 || floorHeat.empty())
        return;
    r.tau = fieldPercentile(floorHeat, kMovingTauFloorPct);
}

/**
 * @brief 三条可测性守卫的执行（**只执行**；线在 `temporal_residual.h` 跑前写死）
 *
 * @param off   TAA **关**那组的归因结果（= 移动锯齿本体，与主表同一组）
 * @param residOff 与分带表**同一份**残差场（跨帧最大），over-τ 率从它数
 */
MovingGuards evaluateMovingGuards(
    const HomoStat& off, const MovingBandRuler& r, const cv::Mat& residOff)
{
    MovingGuards g;
    g.floorMean = off.floorMean;
    g.tau = r.tau;
    g.tauFloor = off.floorHeat.empty() ? 0.0 : fieldPercentile(off.floorHeat, 0.50);
    cv::Mat over;
    cv::threshold(residOff, over, r.tau, 1e30, cv::THRESH_BINARY);
    g.overRateAll = static_cast<double>(cv::countNonZero(over))
        / (static_cast<double>(residOff.rows) * static_cast<double>(residOff.cols));

    // 从扫描曲线取三个量：α=0、|α|=1 里的小者（= 上报的那个残差）、以及全曲线最小点。
    double bestAbs = 1e30;
    for (const auto& [a, v]: off.alignCurve) {
        if (a == 0.0)
            g.residAt0 = v;
        if (std::abs(std::abs(a) - 1.0) < 1e-9 && v < bestAbs)
            bestAbs = v;
    }
    g.residAt1 = bestAbs < 1e29 ? bestAbs : off.residMean;
    g.residAtNeg1 = off.residOther;
    double bestV = 1e30;
    for (const auto& [a, v]: off.alignCurve) {
        if (v < bestV) {
            bestV = v;
            g.bestAlpha = a;
        }
    }
    g.ratio = g.floorMean > 0.0 ? g.residAt1 / g.floorMean : 0.0;

    std::string why;
    const auto push = [&](const std::string& s) {
        if (!why.empty())
            why += "；";
        why += s;
    };
    // 守卫 ⓪：**先问采集是不是活的**。A/B 口径能把画面整个关黑（实测 env.intensity=0 打在
    // 只吃天光的门框机位），此时帧差/残差/地板一起变 0——「没有闪烁」与「没有画面」是两件事，
    // 后者没有任何归因对象，必须点名而不是让它走进后面的比值。
    g.lumaMean = off.lumaMean;
    g.byteMaxAdjacent = off.byteMaxAdjacent;
    if (g.lumaMean >= 0.0 && g.lumaMean < kMovingGuardMinLuma)
        push("⓪a 交付面**全黑**（显示域均值 " + d4(g.lumaMean) + "/255，下限 "
            + d4(kMovingGuardMinLuma) + "）⟹ 本次 A/B 口径把画面关掉了，没有可归因的像素");
    if (g.byteMaxAdjacent < kMovingGuardMinMotion)
        push("⓪b 相邻帧最大字节差只有 " + d4(g.byteMaxAdjacent) + "（下限 "
            + d4(kMovingGuardMinMotion) + "）⟹ 运动激励没进场，残差无从谈起");
    const double explain = g.residAt1 > 0.0 ? g.residAt0 / g.residAt1 : 0.0;
    if (!(explain >= kMovingGuardExplainRatio))
        push("①a 对齐没消化掉多少帧差：α=0/α=1 = " + d4(explain) + "（要求 ≥ "
            + d4(kMovingGuardExplainRatio) + "）");
    if (!(std::abs(std::abs(g.bestAlpha) - 1.0) <= kMovingGuardAlphaTol))
        push("①b 残差最小点不在 |α|=1 上（实测 α* = " + d4(g.bestAlpha) + "，容差 "
            + d4(kMovingGuardAlphaTol) + "）⟹ 闭式 Δψ 与画面真实运动对不上");
    if (!(g.ratio >= kMovingGuardFloorRatio))
        push("② 残差/地板只有 " + d4(g.ratio) + "（要求 ≥ " + d4(kMovingGuardFloorRatio)
            + "）⟹ 量的是重采样本身");
    if (!(g.tau > 0.0))
        push("③ τ **没定出来**（= 0，本次实测地板的 p99 就是 0）⟹ 尺子不存在，分带无从谈起");
    else if (!(g.tau >= g.tauFloor))
        push("③ τ " + d4(g.tau) + " 落在地板里（地板中位数 " + d4(g.tauFloor) + "）");
    if (!(g.overRateAll <= kMovingGuardMaxOverRate))
        push("④ 全帧 over-τ 率 " + d2(g.overRateAll) + " 已饱和（上限 "
            + d2(kMovingGuardMaxOverRate) + "）");
    g.ok = why.empty();
    g.why = why;
    return g;
}

/// 守卫回显（**过与不过都打**——过了也要让人看见是"哪三个数"放行的）
void printMovingGuardEcho(const MovingBandRuler& r, const MovingGuards& g, WarpKernel k)
{
    const double explain = g.residAt1 > 0.0 ? g.residAt0 / g.residAt1 : 0.0;
    std::printf("\n  ── 可测性守卫（**不过则整段不出结论**，线跑前写死 → temporal_residual.h）──\n");
    std::printf("    warp 核 = **%s**（与引擎 TAA 逐式同权重，见本文件「为什么双线性不够」段）\n",
        kernelName(k));
    std::printf("    τ = %s  ← **本次实测地板的 p%.0f 分位**（零假设分布的上尾）\n", d4(r.tau).c_str(),
        kMovingTauFloorPct * 100.0);
    std::printf("      旧口径 τ = 白电平(%s) 处 1 显示 LSB = %s —— 比它低 **%s 倍**，"
                "用它 = 「超 τ」被地板饱和（这就是四率全 100%% 的原因）\n",
        d4(r.white).c_str(), d4(r.tauLegacy).c_str(),
        r.tauLegacy > 0.0 ? d4(r.tau / r.tauLegacy).c_str() : "inf");
    std::printf("    0)  采集活着吗：显示域均值 %s/255（≥ %s）｜相邻帧最大字节差 %s（≥ %s）%s\n",
        d4(g.lumaMean).c_str(), d4(kMovingGuardMinLuma).c_str(), d4(g.byteMaxAdjacent).c_str(),
        d4(kMovingGuardMinMotion).c_str(),
        (g.lumaMean < 0.0 || g.lumaMean >= kMovingGuardMinLuma)
                && g.byteMaxAdjacent >= kMovingGuardMinMotion
            ? "✅"
            : "❌");
    std::printf("        含义：全黑 / 没运动 ≠「没有闪烁」，是**没有画面**（没有可归因的像素）。\n");
    std::printf("    1a) 对齐消化率 α=0/α=1 = %s/%s = %s  （要求 ≥ %s）%s\n", d4(g.residAt0).c_str(),
        d4(g.residAt1).c_str(), d4(explain).c_str(), d4(kMovingGuardExplainRatio).c_str(),
        explain >= kMovingGuardExplainRatio ? "✅" : "❌");
    std::printf("    1b) 扫描最小点 α* = %s  （要求 |α*|−1 ≤ %s）%s\n", d4(g.bestAlpha).c_str(),
        d4(kMovingGuardAlphaTol).c_str(),
        std::abs(std::abs(g.bestAlpha) - 1.0) <= kMovingGuardAlphaTol ? "✅" : "❌");
    std::printf("        含义：残差必须在「按闭式 Δψ warp」处见底，才配叫「运动没搬动的部分」。\n");
    std::printf("        反号 α=−1 的残差 %s（本次取的小者 %s）—— 符号若反，1b 当场红\n",
        d4(g.residAtNeg1).c_str(), d4(g.residAt1).c_str());
    std::printf("    2)  残差/地板 %s/%s = %s  （要求 ≥ %s）%s\n", d4(g.residAt1).c_str(),
        d4(g.floorMean).c_str(), d4(g.ratio).c_str(), d4(kMovingGuardFloorRatio).c_str(),
        g.ratio >= kMovingGuardFloorRatio ? "✅" : "❌");
    std::printf("    3)  τ %s > 0 且 ≥ 地板中位数 %s  %s\n", d4(g.tau).c_str(), d4(g.tauFloor).c_str(),
        g.tau > 0.0 && g.tau >= g.tauFloor ? "✅" : "❌");
    std::printf("    4)  全帧 over-τ 率 %s  （上限 %s）%s\n", d2(g.overRateAll).c_str(),
        d2(kMovingGuardMaxOverRate).c_str(),
        g.overRateAll <= kMovingGuardMaxOverRate ? "✅" : "❌");
    std::printf("    ⟹ 守卫 **%s**\n", g.ok ? "全过 ✅（可以出结论）" : "未过 ⛔（只报 Indeterminate）");
}
/// 出「亮度 × 梯度」六格表（四数）+ 三分支判决。`residOff`/`residOn` = 两态的逐像素残差
/// （跨帧最大，CV_64FC1）。返回判决文字供台账。
std::string reportMovingBandsFromHomography(const MovingBandRuler& r, const cv::Mat& residOff,
    const cv::Mat& residOn, const std::string& targetName, const cv::Mat& ids,
    const std::vector<GlbPrim>& prims, double floorMeanOff, double floorMeanOn,
    double idYawErrPx, int idBestFrame, double idBestDiff, double sweepTotalPx)
{
    std::printf("\n── 【机位状态 = **相机移动**】亮度 × 梯度 二维分带（尺子 1：精确单应 warp 残差"
                "）──\n");
    if (r.tau <= 0.0) {
        std::printf("  ✗ 尺子缺失（采集侧 wantBands 没开 = 仪器没装上），分带表未出\n");
        return "未出（尺子缺失）";
    }
    std::printf("  被测量 = 上一帧按闭式单应 warp 到本帧网格后**搬不动的部分**（= 移动时的闪烁本身）\n");
    std::printf("  亮度轴 = 显示域字节亮度的多帧均值；T_lo=%.0f（码字中点）· T_hi=%.0f（**闭式**："
                "ACES 输入 ≥%s）\n", kBandMidByte, r.tHi, d4(kBandClipProxyLinear).c_str());
    std::printf("  梯度轴 = |Sobel_x|+|Sobel_y| 于**线性 luma**，取**跨帧最大**（移动机位没有"
                "「相位平均」那张干净图，理由见文件内注释）；高梯度 = ≥ p%.0f = %s\n",
        kBandGradPct * 100.0, d4(r.gradThr).c_str());
    std::printf("  τ = %s ← **本次实测同态往返地板的 p%.0f 分位**（零假设分布的上尾；旧口径「白电平 %s"
                " 处 1 显示 LSB = %s」比地板低一个量级以上，用它 = 饱和）\n",
        d4(r.tau).c_str(), kMovingTauFloorPct * 100.0, d4(r.white).c_str(), d4(r.tauLegacy).c_str());
    std::printf("  ⚠⚠ **本尺子的固有限制**：零参考图 ⟹ 亮度档与梯度档只能由**被测量自己的画面**定"
                "（选择偏差）。静止态批 13.7c 已因这种口径翻过一次方向。故本尺子的结论**只在"
                "与尺子 2（尺子来自超采样参考）同向时才算强结论**；反向时以尺子 2 为准。\n");

    const BandTable off = bandTableOf(residOff, r.byteMean, r.gradMax, kBandMidByte, r.tHi,
        r.gradThr, r.tau);
    const BandTable on = bandTableOf(residOn, r.byteMean, r.gradMax, kBandMidByte, r.tHi,
        r.gradThr, r.tau);
    if (!off.valid() || !on.valid()) {
        std::printf("  ✗ 分带表无像素（total=0），不出数\n");
        return "未出（无像素）";
    }
    printMovingBandTable("TAA 关（= 移动锯齿本体，**主表**）", off, "单应 warp 残差（TAA 关）");
    printMovingBandTable("TAA 开（= 开 TAA 之后还剩的闪烁）", on, "单应 warp 残差（TAA 开）");

    const BandShare aOff = bandShareOf(off, 1, 1), aOn = bandShareOf(on, 1, 1);
    const BandShare bOff = bandShareOf(off, 2, 1), bOn = bandShareOf(on, 2, 1);
    const double rA = aOff.rate > 0.0 ? aOn.rate / aOff.rate : 0.0;
    const double rB = bOff.rate > 0.0 ? bOn.rate / bOff.rate : 0.0;
    const bool excessInB = rA > 0.0 && rB >= kBandSupportRatio * rA;
    // ⚠ **率比退化闸门**（wetground 实测逼出来的，不是预防性装饰）：移动机位下 `frac>τ` 饱和
    //（A/B 两格 rate 双双贴 100%），rX 的分子分母同贴 1.0 ⟹ rB/rA 只剩浮点噪声（实测 1e-6 量级
    // 的差就被读成「反向分离」= 假信号）。饱和时**方向不判**，判据只认幅度份额。
    const double kSat = 0.90;
    const bool rateSaturated = aOff.rate > kSat && bOff.rate > kSat && aOn.rate > kSat && bOn.rate > kSat;
    // rB/rA 的相对差（判据线要求的是 ≥50% 的分离，故这个数远小于 0.5 时「方向」根本没有内容）。
    const double relDiff = std::abs(rB - rA) / std::max({ rA, rB, 1e-12 });
    std::printf("\n  率比 rX = rate_on(X)/rate_off(X)（该格「开 TAA 后还剩多少」）\n");
    std::printf("    A 中间调×高梯度 rate %s → %s（×%s）  B 近削顶×高梯度 rate %s → %s（×%s）\n",
        d2(aOff.rate).c_str(), d2(aOn.rate).c_str(), d4(rA).c_str(), d2(bOff.rate).c_str(),
        d2(bOn.rate).c_str(), d4(rB).c_str());
    std::printf("    B 幅度份额 %s → %s（门关→门开）  B 计数份额 %s → %s\n",
        d2(bOff.ampShare).c_str(), d2(bOn.ampShare).c_str(), d2(bOff.cntShare).c_str(),
        d2(bOn.cntShare).c_str());
    if (rateSaturated)
        std::printf("    ⚠ **率比在本场景退化失效**：四格 rate 全部 > %.0f%%（`frac>τ` 饱和）⟹ rX 的"
                    "分子分母同贴 1.0，rB/rA 的相对差只有 %s ——**方向不判**，只看幅度份额\n",
            kSat * 100.0, d4(relDiff).c_str());

    const MovingBandBranch branch
        = classifyMovingBand(bOff.ampShare, bOff.cntShare, excessInB);
    std::printf("\n  ⭐ 判决（线**跑前写死**：支持 = B 幅度份额 ≥ %s 或 rB ≥ %s×rA；"
                "否证 = B 两份额都 < %s 且超额不向 B 集中）\n",
        d2(kMovingSupportAmpShare).c_str(), d4(kBandSupportRatio).c_str(),
        d2(kMovingRefuteShare).c_str());
    std::printf("    支撑格数字：B 幅度份额 %s（支持线 %s / 否证线 %s）· B 计数份额 %s · "
                "rB/rA = %s · 超额集中在B = %s\n",
        d2(bOff.ampShare).c_str(), d2(kMovingSupportAmpShare).c_str(),
        d2(kMovingRefuteShare).c_str(), d2(bOff.cntShare).c_str(),
        rA > 0.0 ? d4(rB / rA).c_str() : "inf", excessInB ? "是" : "否");
    std::printf("    ⟹ **%s**\n", movingBandBranchName(branch));
    printMovingBandVacuity(bOff, "连拍帧**自身**的显示域字节亮度均值（零参考图）",
        "本尺子的亮度轴来自被测量自己（选择偏差自曝过），若高光只出现在个别帧，"
        "均值口径会把它判出近削顶档 —— 对假设是**保守**方向");
    // 分支③的**性质**也必须命名（昨天缺的就是这个）：方向 + 阈值敏感性两条一起读。
    if (rateSaturated)
        std::printf("    分离方向：**不判**（rate 饱和；rB/rA 相对差 %s，而判据线要的是 ≥50%% 的分离"
                    "⟹ 方向这一维在本场景没有内容）——用户主张要求 rB > rA，但本场景 rate 轴没有"
                    "分辨力，方向问题由**幅度份额**与**尺子 2**回答\n",
            d4(relDiff).c_str());
    else
        std::printf("    分离方向：rB %s rA ⟹ %s（用户主张要求的是 rB > rA：B 比 A 更压不动）\n",
            rB < rA ? "<" : (rB > rA ? ">" : "="),
            rB < rA ? "**反向分离**（B 比 A 压得更狠，与主张方向相反）"
                    : (rB > rA ? "B 压得没那么狠（方向与主张一致，幅度见上）" : "两格同倍数"));

    // ── 敏感性（同一次采集换阈值，零额外引擎成本）。判「分不出」时的分工：换档结论跟着翻
    //    = **阈值没分开**；怎么换都不翻 = **两格本来同分布**。
    std::printf("\n  ── 敏感性（同一次采集换阈值；主档 = 上面那张）──\n");
    std::printf("    %-24s %13s %13s %14s %14s\n", "口径", "A 率比", "B 率比",
        "B 幅度份额(关)", "B 计数份额(关)");
    const auto sensLine = [&](const std::string& tag, const cv::Mat& lm, const cv::Mat& gm,
                                  double tl, double th, double gthr) {
        const BandTable t1 = bandTableOf(residOff, lm, gm, tl, th, gthr, r.tau);
        const BandTable t2 = bandTableOf(residOn, lm, gm, tl, th, gthr, r.tau);
        const BandShare a1 = bandShareOf(t1, 1, 1), a2 = bandShareOf(t2, 1, 1);
        const BandShare b1 = bandShareOf(t1, 2, 1), b2 = bandShareOf(t2, 2, 1);
        const double ra = a1.rate > 0.0 ? a2.rate / a1.rate : 0.0;
        const double rb = b1.rate > 0.0 ? b2.rate / b1.rate : 0.0;
        std::printf("    %-24s %13s %13s %14s %14s\n", tag.c_str(), d4(ra).c_str(),
            d4(rb).c_str(), d2(b1.ampShare).c_str(), d2(b1.cntShare).c_str());
    };
    for (double th : kBandClipSensitivity)
        sensLine("T_hi=" + d4(th).substr(0, 3) + (std::abs(th - r.tHi) < 0.5 ? "（主档）" : ""),
            r.byteMean, r.gradMax, kBandMidByte, th, r.gradThr);
    sensLine("梯度轴=线性 p75", r.byteMean, r.gradMax, kBandMidByte, r.tHi,
        percentileOf(r.gradSorted, 0.75));
    sensLine("梯度轴=线性 p95", r.byteMean, r.gradMax, kBandMidByte, r.tHi,
        percentileOf(r.gradSorted, 0.95));
    sensLine("亮度轴=跨帧最大", r.byteMax, r.gradMax, kBandMidByte, r.tHi, r.gradThr);
    sensLine("T_lo=96", r.byteMean, r.gradMax, 96.0, r.tHi, r.gradThr);
    sensLine("T_lo=160", r.byteMean, r.gradMax, 160.0, r.tHi, r.gradThr);
    std::printf("    读法：换档后「B 幅度份额」跟着翻过 %s / %s 两条线 = **阈值没分开**；"
                "怎么换都不翻 = **两格本来同分布**\n",
        d2(kMovingRefuteShare).c_str(), d2(kMovingSupportAmpShare).c_str());
    // ── 材质归因（批 13.9d，**本次的主问题**）──────────────────────────────────────
    // 六格表在门框机位上是**按构造失效**的：B 档（近削顶×高梯度）只有 115 px = 0.01% 面积。
    // 所以真正的归因轴不是亮度代理，而是 objectID → glb primitive → **材质名**。
    if (!ids.empty() && !prims.empty()) {
        std::printf("\n  ── 材质归因（objectID → glb → 材质名；**按幅度份额降序**）──\n");
        std::printf("    objectID 落位 = burst 中点姿态，实测姿态残差 %s px；同姿态补拍的彩色与 "
                    "burst 第 %d 帧最接近（平均|Δ| = %s，≈0 = 姿态逐位复现）\n",
            d4(idYawErrPx).c_str(), idBestFrame, d4(idBestDiff).c_str());
        std::printf("    只拍一张 ID 的理由：材质是**慢变量**，整段 burst 总位移只有 %s px"
                    "（扫速 × 帧数），与分带尺子本身（均值 / 跨帧最大）同一量级\n",
            d4(sweepTotalPx).c_str());
        // ⚠ 不用 `cv::norm(NORM_INF)`：它对 CV_32SC1 没有实现（本构建里直接段错误，实测踩过），
        //   而 objectID 就是 32 位整图。自己扫一遍，921600 次比较不值一提。
        int64_t maxId = 0;
        for (int rr = 0; rr < ids.rows; ++rr) {
            const int32_t* ip = ids.ptr<int32_t>(rr);
            for (int cc = 0; cc < ids.cols; ++cc)
                maxId = std::max(maxId, static_cast<int64_t>(ip[cc]));
        }
        std::printf("    objectID 上界自证：本帧最大 id %lld，glb 摊平出 %zu 个 primitive ⟹ %s。"
                    "可见 id 少于总数是**正常**的（画面外的 primitive 不进图）；真正的失效是 id"
                    "**越界** ⟹ 表里会出现 unnamed#\n",
            static_cast<long long>(maxId), prims.size(),
            maxId < static_cast<int64_t>(prims.size()) ? "成立" : "**不成立，名字全部不可信**");
        printMaterialTable("TAA 关（= 移动锯齿本体，**主表**）",
            materialAttribution(ids, residOff, prims, r.tau, floorMeanOff), 14, r.tau,
            floorMeanOff, "单应 warp 残差（TAA 关）");
        printMaterialTable("TAA 开（= 开 TAA 之后还剩的闪烁）",
            materialAttribution(ids, residOn, prims, r.tau, floorMeanOn), 8, r.tau, floorMeanOn,
            "单应 warp 残差（TAA 开）");
    }
    (void)targetName;
    return movingBandBranchName(branch);
}

/**
 * 从交付配置里读**当前用的那个 glb**，拼成 build 下的实际路径。
 *
 * 为什么不硬编码 `bistro_exterior.glb`：材质名的唯一真相源就是「引擎这次实际加载的那个 glb」。
 * 换模型时硬编码会**静默**给出旧模型的名字（数字照样出、名字全错），比崩掉更难查。
 */
std::string deliveryGlb(const std::string& configDir)
{
    std::ifstream in(configDir + "/scene.lua");
    if (!in)
        throw std::runtime_error("读不到交付配置: " + configDir + "/scene.lua");
    const std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // 逐行找**未注释**的 `model = "...glb"`。为什么不用 `std::regex` 的 `(?m)`：本工具链的
    // `std::regex` 走 ECMAScript 语法，`(?...)` 只认零宽断言，`(?m)` 直接抛
    // `Invalid '(?...)' zero-width assertion`（实测踩过）。逐行 + 行首锚定既没有这个坑，
    // 也天然跳过 `-- model = ...` 这种注释行（它们的首个非空字符是 `-`）。
    static const std::regex rx(R"rx(^\s*model\s*=\s*"([^"]+\.glb)")rx");
    std::istringstream lines(txt);
    for (std::string line; std::getline(lines, line);) {
        std::smatch m;
        if (std::regex_search(line, m, rx))
            return projectRoot() + "/build/rgl/" + m[1].str();
    }
    throw std::runtime_error("交付配置 scene.lua 里没有未注释的 model 键，材质归因无法建立映射");
}

HomoStat runHomography(const FlickerSeries& g, double yawRadPerFrame, const std::string& heatPath,
    WarpKernel kernel, bool echoSelfCheck, bool doAlignSweep)
{
    HomoStat s;
    s.name = g.name;
    s.size = g.frameSize;
    s.byteMaxAdjacent = g.byteMaxAdjacent;
    s.lumaMean = g.byteMean.empty() ? -1.0 : cv::mean(g.byteMean)[0];
    s.kernel = kernel;
    if (g.luma.size() < 3)
        throw std::runtime_error(g.name + "：帧数不足 3，无法做相邻帧归因");

    const double focal = verticalFocalPx(g.frameSize.height, kFovDeg);
    const double cx = static_cast<double>(g.frameSize.width) * 0.5;
    const double cy = static_cast<double>(g.frameSize.height) * 0.5;
    const cv::Size sz = g.frameSize;
    constexpr double kP = 0.999;  ///< 全帧分位：1px 边缘只占 0.16% 像素，p99.5 会把它整条抹平（实测踩过）

    // ⚠ 自检先跑（判据工具自己要有自检）：核抄错 / 坐标约定与 OpenCV 不一致，后面全部数字作废。
    if (echoSelfCheck) {
        const cv::Matx33d probe = yawHomography(yawRadPerFrame, focal, cx, cy);
        const double idL = identityCheck(g.luma[1], WarpKernel::Lanczos3);
        const double idC = identityCheck(g.luma[1], WarpKernel::CatmullRom);
        const double idB = identityCheck(g.luma[1], WarpKernel::Bilinear);
        const double trL = translationCheck(g.luma[1], WarpKernel::Lanczos3);
        const double trC = translationCheck(g.luma[1], WarpKernel::CatmullRom);
        const double bl = bilinearCrossCheck(g.luma[1], probe.inv());
        const bool exact = idL == 0.0 && idC == 0.0 && idB == 0.0 && trL == 0.0 && trC == 0.0;
        std::printf("\n  ── warp 核自检（**工具自己的**，不是引擎的）──\n");
        std::printf("    恒等性（零偏移逐位取回原值）L3 %.3e ｜ CR %.3e ｜ bilinear %.3e\n", idL, idC,
            idB);
        std::printf("    整数平移一列（钉住坐标约定 **+ 抽头基址 kBase**）L3 %.3e ｜ CR %.3e\n", trL,
            trC);
        std::printf("      %s\n",
            exact ? "✅ 两条都 = 0：核与引擎逐式同权重、抽头基址与坐标约定同源（自证的「零」真的是零）"
                  : "❌ 核抄错了（恒等性或整数平移不精确）——本次全部数字作废");
        std::printf("    与 cv::warpPerspective(INTER_LINEAR,BORDER_REPLICATE) 比量级：%.3e"
                    "（OpenCV 走 5 位定点权重，**本就该差到 1e-2 量级**；约定真错会是 O(1)，"
                    "差三个量级）%s\n",
            bl, bl < 0.1 ? "✅ 量级正常" : "❌ 量级不对，坐标/边界约定有问题");
    }

    // 单应与帧无关 ⟹ 每个矩阵只建一次映射表
    const cv::Matx33d Hf = yawHomography(yawRadPerFrame, focal, cx, cy);
    const cv::Matx33d Hr = yawHomography(-yawRadPerFrame, focal, cx, cy);
    const SrcMap mapF = buildSrcMap(Hf.inv(), sz);  ///< 把上一帧**推进** Δψ 到本帧网格
    const SrcMap mapR = buildSrcMap(Hr.inv(), sz);  ///< 反号假设
    const SrcMap mapBack = buildSrcMap(Hf, sz);  ///< 往返地板第二段：dst→src = H（与 mapF 互逆）

    // 两个符号假设各跑一遍（闭式对拍，不是搜索）；热图留到选定符号后再出
    double resid[2] = { 0.0, 0.0 };
    std::vector<cv::Mat> heat(2, cv::Mat::zeros(sz, CV_32FC1));
    std::vector<cv::Mat> residMax(2);  ///< 跨帧最大残差场（CV_64F，守卫与分带共用）
    for (int si = 0; si < 2; ++si) {
        const SrcMap& mp = si == 0 ? mapF : mapR;
        for (size_t k = 1; k < g.luma.size(); ++k) {
            cv::Mat warped, res;
            warpKernel(g.luma[k - 1], mp, kernel, warped);
            cv::absdiff(g.luma[k], warped, res);
            maxInto(residMax[static_cast<size_t>(si)], res);
            cv::Mat res32;
            res.convertTo(res32, CV_32FC1);
            resid[si] += histPercentile(res32, kP);
            cv::max(heat[static_cast<size_t>(si)], res32, heat[static_cast<size_t>(si)]);
        }
    }
    const size_t pairs = g.luma.size() - 1;
    s.pairs = pairs;
    s.residP995 = std::min(resid[0], resid[1]) / static_cast<double>(pairs);
    s.residOther = std::max(resid[0], resid[1]) / static_cast<double>(pairs);
    const int best = resid[0] <= resid[1] ? 0 : 1;

    // 原始帧差（解释率的分母）+ 往返地板（warp 插值自身误差，与符号无关）
    // + **符号自证**：两帧各自搬 Δψ/2 到**同一个中间视点**后对拍。
    //   差值若由运动造成 ⟹ 搬到同一点后应当**塌到地板**；塌不下来 = 差值与运动无关。
    double rawSum = 0.0, floorSum = 0.0;
    cv::Mat rawMax, floorHeat;
    for (size_t k = 1; k < g.luma.size(); ++k) {
        cv::Mat raw, there, back, rt;
        cv::absdiff(g.luma[k], g.luma[k - 1], raw);
        maxInto(rawMax, raw);
        cv::Mat raw32;
        raw.convertTo(raw32, CV_32FC1);
        rawSum += histPercentile(raw32, kP);
        warpKernel(g.luma[k], mapF, kernel, there);
        warpKernel(there, mapBack, kernel, back);
        cv::absdiff(g.luma[k], back, rt);
        maxInto(floorHeat, rt);
        cv::Mat rt32;
        rt.convertTo(rt32, CV_32FC1);
        floorSum += histPercentile(rt32, kP);
    }
    s.rawP995 = rawSum / static_cast<double>(pairs);
    s.floorP995 = floorSum / static_cast<double>(pairs);
    s.rawMean = meanOf(rawMax);
    s.residMean = meanOf(residMax[static_cast<size_t>(best)]);
    s.floorMean = meanOf(floorHeat);

    // 对齐自证扫描（**只在主表 = TAA 关那组跑**：守卫定义在那一组，另一组跑一遍只是白花 GPU）
    if (doAlignSweep) {
        for (double a: kAlignSweepAlphas)
            s.alignCurve.emplace_back(a, sweepResidual(g, yawRadPerFrame, a, kernel));
    }
    s.heat = heat[static_cast<size_t>(best)];
    s.floorHeat = floorHeat;
    cv::Mat over;
    cv::threshold(s.heat, over, 1.0 / 255.0, 1.0, cv::THRESH_BINARY);
    s.frac1Lsb = static_cast<double>(cv::countNonZero(over))
        / (static_cast<double>(sz.width) * static_cast<double>(sz.height));
    if (!heatPath.empty())
        writeHeat(s.heat, heatPath);
    return s;
}

}  // namespace

cv::Matx33d yawHomography(double yawRadPerFrame, double focalPx, double cxPx, double cyPx)
{
    const double t = std::tan(yawRadPerFrame), f = focalPx, cx = cxPx, cy = cyPx;
    // x' = [(f−cx·t)x + t(cx²+f²)] / [−t·x + (f+t·cx)]，y' = [−cy·t·x + f·y + cy·cx·t] / 同分母
    return cv::Matx33d(
        f - cx * t, 0.0, t * (cx * cx + f * f),
        -cy * t, f, cy * cx * t,
        -t, 0.0, f + t * cx);
}

int cmdTemporalHomography(const std::string& target, double sweepPxPerFrame, int frames,
    const std::string& outPrefix, bool bands, double envIntensity, double aoStrength)
{
    bool edgeTarget = false;
    const BistroCam* cam = nullptr;
    try {
        cam = resolveFlickerTarget(target, edgeTarget);
    }
    catch (const std::exception& e) {
        std::printf("%s\n", e.what());
        return 2;
    }
    if (sweepPxPerFrame <= 0.0 || sweepPxPerFrame >= 1.0) {
        std::printf("--sweep 须在 (0,1) px/帧\n");
        return 2;
    }
    const float focal = verticalFocalPx(kSize[1], kFovDeg);
    const double yawRad = sweepToYawRad(sweepPxPerFrame, focal);

    std::printf("=== 精确单应 warp 的逐像素闪烁归因（temporal-homography）===\n");
    std::printf("  目标 %s｜扫速 %g px/帧 → Δψ = %.8f rad/帧｜帧数 %d｜主点 = 画面中心（cameraAimAt 钉的）\n",
        target.c_str(), sweepPxPerFrame, yawRad, frames);
    if (bands)
        std::printf("  --bands = 开：**机位状态 = 相机移动**（frame_yaw 每帧 Δψ），残差图另出"
                    "「亮度 × 梯度」二维分带（判决线跑前写死，见本文件注释）\n");

    // 与引擎 TAA **默认核**同族（Lanczos3）。为什么不是 bilinear：见本文件「为什么原来的
    // cv::warpPerspective(INTER_LINEAR) 不够」段——双线性没有负瓣，糊度差本身算进残差，
    // 且让「符号自证」因核太糊而**必然**失败。
    constexpr WarpKernel kKernel = WarpKernel::Lanczos3;
    // A/B 侧回显（**跑之前就出**，让读日志的人第一眼就知道这不是交付口径）
    const bool abEnv = envIntensity >= 0.0, abAo = aoStrength >= 0.0;
    if (abEnv || abAo) {
        std::printf("  ⚠ **A/B 口径**（不是交付配置）：");
        if (abEnv)
            std::printf("env.intensity = %g ", envIntensity);
        if (abAo)
            std::printf("%sao.strength = %g", abEnv ? "· " : "", aoStrength);
        std::printf("\n    两个门都是 IEEE 精确旁路（ao：tonemap 的 lerp(1,ao,0)≡1；env：几何体的 "
                    "IBL diffuse+specular 都乘 env.intensity）\n");
        if (abEnv)
            std::printf("    ⚠ 但 **天穹背景不受 env.intensity 影响**（skybox.frag.slang 与 "
                        "light.frag.slang 的 envSky 分支直接返回 env 采样、不乘它）\n"
                        "      ⟹ 背景会**原样亮着**。「背景还在」≠「IBL 没关掉」，"
                        "看几何体别看天。\n");
        if (abEnv && envIntensity <= 0.0)
            std::printf("    ⚠ 更要紧：env 是本场景**唯一的环境光**（scene.lua 原注释：0.2 时阴影面只剩"
                        "受光的 0.2%%，\n"
                        "      即「楼房背光面过暗」的根因）⟹ 只吃天光的机位（门框 / 门板）在 0 下"
                        "**整个关黑**。\n"
                        "      那种跑法守卫 ⓪ 会当场红并 rc≠0 —— 这是**预期**（没有画面可归因），"
                        "不是量具坏了。\n"
                        "      要问「IBL 是不是驱动」请改用**剂量响应**（`--env 0.5` / `0.25`），"
                        "别用开关。\n");
    }
    // 材质归因的真相源：交付配置里当前加载的那个 glb（不硬编码模型名）
    std::vector<GlbPrim> prims;
    if (bands && !edgeTarget) {
        try {
            prims = parseGlbPrimitives(deliveryGlb(projectRoot() + "/configs"));
            std::printf("  材质映射：%zu 个 primitive（glb 摊平：mesh-major / primitive-minor）\n",
                prims.size());
        }
        catch (const std::exception& e) {
            std::printf("  ⚠ 材质归因**不可用**（%s）⟹ 只出六格表，材质表跳过\n", e.what());
        }
    }
    std::vector<HomoStat> stats;
    std::vector<cv::Mat> idsOf;   ///< 与 stats 同序；TAA 关那组带 objectID（材质归因用）
    std::vector<double> idErrOf;  ///< (yawErrPx, idBestDiff) 成对
    std::vector<int> idFrameOf;
    std::vector<double> idDiffOf;
    MovingBandRuler ruler;
    bool rulerOk = false;
    for (bool on: { false, true }) {
        const std::string tag = on ? "homo_on" : "homo_off";
        const std::string dir = projectRoot() + "/build/imgqa_homo_" + tag;
        std::printf("  采集 %s …\n", tag.c_str());
        try {
            FlickerSeries g = captureFlickerSeries(dir, on ? "TAA 开" : "TAA 关", tag, edgeTarget, cam,
                on, yawRad, frames, 0.0, bands, bands && !edgeTarget && !on, envIntensity,
                aoStrength, abEnv, abAo);
            if (bands) {
                idsOf.push_back(g.ids);
                idErrOf.push_back(g.yawErrPx);
                idFrameOf.push_back(static_cast<int>(g.idBestFrame));
                idDiffOf.push_back(g.idBestDiff);
            }
            HomoStat st = runHomography(g, yawRad,
                outPrefix.empty() ? std::string() : outPrefix + (on ? "_taa_on_heat.png" : "_taa_off_heat.png"),
                kKernel, !on, bands && !on);
            // 尺子取 **TAA 关** 那组并两态共用（与静止态「尺子 = 门关 = 用户现状」同纪律：
            // 两态各定一套带边界，差表就失去意义）。
            if (bands && !on) {
                ruler = movingBandRulerFrom(g);
                applyFloorTau(ruler, st.floorHeat);  // τ ← 本次实测地板（旧口径留 tauLegacy）
                rulerOk = ruler.tau > 0.0;
            }
            stats.push_back(std::move(st));
        }
        catch (const std::exception& e) {
            std::printf("  [失败] %s\n", e.what());
            return 1;
        }
    }

    std::printf("\n  组       原始帧差p99.9  残差p99.9  反号残差  往返地板  残差−地板  解释率  残差>1LSB\n");
    std::printf("  （warp 核 = %s；表里的 p99.9 走直方图，守卫用**跨帧最大场的均值**，两者不是同一个数）\n",
        kernelName(kKernel));
    std::printf("  %s\n", std::string(96, '-').c_str());
    for (const auto& s: stats) {
        const double explained = s.rawP995 > 1e-9 ? 1.0 - s.residP995 / s.rawP995 : 0.0;
        std::printf("  %-7s %-14s %-11s %-10s %-10s %-11s %-8s %s\n", s.name.c_str(),
            d4(s.rawP995).c_str(), d4(s.residP995).c_str(), d4(s.residOther).c_str(),
            d4(s.floorP995).c_str(), d4(s.residP995 - s.floorP995).c_str(), d2(explained).c_str(),
            d2(s.frac1Lsb).c_str());
        std::printf("    均值口径（守卫用的就是这一行）：原始帧差 %s ｜ 残差 %s ｜ 地板 %s\n",
            d4(s.rawMean).c_str(), d4(s.residMean).c_str(), d4(s.floorMean).c_str());
        if (!s.alignCurve.empty()) {
            std::printf("    对齐自证扫描（残差 vs warp 强度 α；α=0 不 warp，±1 闭式含反号）：\n      ");
            for (const auto& [a, v]: s.alignCurve)
                std::printf("α=%+5.2f:%-8s  ", a, d4(v).c_str());
            std::printf("\n");
        }
        if (s.residP995 < s.floorP995 * 1.5)
            std::printf("    ⚠ 残差与 warp 往返地板同量级（%s vs %s）——量具在响，归因图不可当闪烁读\n",
                d4(s.residP995).c_str(), d4(s.floorP995).c_str());
        if (s.residOther < s.residP995 * 1.5)
            std::printf("    ⚠ 两个符号假设的 **p99.9** 残差同量级（%s vs %s）——p99.9 是钝尺子"
                        "（只看最坏那 0.1%%），符号能不能分以**下面那条 α 扫描**为准\n",
                d4(s.residP995).c_str(), d4(s.residOther).c_str());
    }
    if (!outPrefix.empty())
        std::printf("\n  热图（固定标尺 0→黑 / 1→白，两组可直接对眼）: %s_taa_{on,off}_heat.png\n", outPrefix.c_str());

    // ── 批 13.9：【机位状态 = 相机移动】亮度 × 梯度二维分带（**只出数 + 一句判决**，
    //    既有四条读数与 rc 一律不动 —— 本段不产 Verdict，判决是打印的一句结论）─────────
    std::string bandVerdict = "未开";
    if (bands && stats.size() == 2) {
        // ⚠ 这里**不许**再拿 `rulerOk` 当进入条件（2026-10-06 实测修）：尺子空（τ=0，画面全黑
        // 时必然）以前会整段跳过 ⟹ 既不报守卫也不报 Indeterminate，直接 rc=0 —— 正是本工具
        // 要消灭的「空洞满足」。现在无论尺子成不成都要走守卫闸门，让红变成**出声**的红。
        cv::Mat residOff, residOn;
        stats[0].heat.convertTo(residOff, CV_64FC1);
        stats[1].heat.convertTo(residOn, CV_64FC1);
        // ⭐ **守卫闸门在前**（2026-10-06 用户拍板）：任一守卫不过 ⟹ 只报 Indeterminate、
        //    **不出三分支判决**、**rc≠0**。原来这两条守卫只是 ⚠ 警告，实测后果是「四率全 100%%
        //    + 自证失败」照样打印「分支②「否证」」并且 rc=0 —— 判决版的空洞满足。
        MovingGuards g = evaluateMovingGuards(stats[0], ruler, residOff);
        if (!rulerOk && g.ok)
        {
            // 兜底（防回归）：尺子空却全绿 = 守卫 ③ 失效，宁可错杀不放过
            g.ok = false;
            g.why = "③ τ 没定出来（尺子空），守卫却全绿 ⟹ 工具缺陷，请报";
        }
        printMovingGuardEcho(ruler, g, kKernel);
        if (!g.ok) {
            printIndeterminate(target, g);
            logCall("temporal-homography",
                {{"target", target}, {"sweep", std::to_string(sweepPxPerFrame)},
                    {"frames", std::to_string(frames)}, {"bands", "1"},
                    {"kernel", kernelName(kKernel)},
                    {"bandVerdict", "Indeterminate（守卫未过，未出判决）"}, {"guardWhy", g.why}},
                {});
            return 1;
        }
        bandVerdict = reportMovingBandsFromHomography(ruler, residOff, residOn, target,
            idsOf.empty() ? cv::Mat() : idsOf[0], prims, stats[0].floorMean, stats[1].floorMean,
            idErrOf.empty() ? 0.0 : idErrOf[0], idFrameOf.empty() ? -1 : idFrameOf[0],
            idDiffOf.empty() ? 0.0 : idDiffOf[0], sweepPxPerFrame * static_cast<double>(frames));
        logCall("temporal-homography",
            {{"target", target}, {"sweep", std::to_string(sweepPxPerFrame)},
                {"frames", std::to_string(frames)}, {"bands", "1"},
                {"kernel", kernelName(kKernel)}, {"bandVerdict", bandVerdict}},
            {});
    }
    return 0;
}

}  // namespace RGL::imgqa
