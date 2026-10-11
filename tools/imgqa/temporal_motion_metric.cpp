// temporal-motion 家族**口径层** —— 把 luma 压成读数的算子：内容位移闭式解 / 平移 / 峰值
// 池化 / 拒收分桶 / 逐帧 argmin 对齐 / 偏差与抖动相位拟合 / 尾长阶梯 / 静止收敛差。
// 拆出来是因为 `temporal_motion.cpp` 2748 代码行 > 500（批次 B11），缝按**仪器层次**切：
// 这一层不启引擎（`staticDiff` 除外，它只吃两张已读入的图），同 `shadow_kernel_metric.*`
// 与 `temporal_residual_metric.*` 那条「口径 vs 仪器」的缝。跨 TU 的类型与阈值常量唯一
// 出处 = `temporal_motion.h`。
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

/**
 * 帧号 → **内容位移** c_t（像素单位）。口径 = `taa_jitter.cppm::contentShiftPx` =
 * `(+jx, −jy)`，**勿凭直觉写符号**（该函数注释里写着：x 不翻、y 被负高视口翻一次；
 * 符号若反，M2 的去抖会把 0.7px 的错位**翻倍**成 1.4px，实测可分辨）。
 *
 * Halton 取 `haltonDouble`（double 版）而**不是**引擎 shader 用的 `haltonSequence`
 * （float 版）：本函数要喂 `cv::warpAffine`，中途收窄成 float 会改判据读数末位。
 * 两份精度的关系由 `tests/test_taa_jitter.cppm` 钉死。
 */
cv::Point2d contentShiftPx(uint32_t frameIndex, double scale)
{
    const uint32_t i = frameIndex % kJitterPeriod + 1u;
    const double jx = (haltonDouble(i, 2u) - 0.5) * scale;
    const double jy = (haltonDouble(i, 3u) - 0.5) * scale;
    return {jx, -jy};
}

/// 把一张线性 luma 平移 (dx,dy)（双线性）。dst(x,y) = src(x−dx, y−dy)，边界钳边。
[[nodiscard]] cv::Mat shiftLuma(const cv::Mat& luma, cv::Point2d d)
{
    if (std::abs(d.x) < 1e-9 && std::abs(d.y) < 1e-9)
        return luma;  // 零位移**原样返回**：M1 的 p=0 读数必须与「不去抖」逐位相同
    const cv::Mat m = (cv::Mat_<double>(2, 3) << 1, 0, d.x, 0, 1, d.y);
    cv::Mat out;
    cv::warpAffine(luma, out, m, luma.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    return out;
}

std::string d4(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}
std::string d2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}
std::string pct2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f%%", v * 100.0);
    return b;
}

/**
 * 2×2 **峰值**池化（把原生分辨率的字节亮度轴降到统计网格）。
 *
 * 为什么手写：`cv::resize` 的 INTER_MAX 在 OpenCV 4.12 里**不存在**（`resize()` 只认
 * NEAREST/AREA/LINEAR/CUBIC/LANCZOS4，其余一律 `CV_Error("Unknown interpolation method")`，
 * 实测在 REFm 组崩过一次 rc=3）。而 INTER_AREA 是面积**平均** ⟹ 1px 高亮细线被邻居稀释
 * （最坏 4 倍），恰好把「1px 高亮直线」这个假设的元凶从尺子上抹掉 —— 所以敏感性表需要一把
 * 「这个统计像素的邻域里**曾经**有多亮」的轴，它必须是 max 而不是 mean。
 *
 * 只支持整数倍降采样（本项目 = 1440p→720p 恰好 2×）；非整数倍退回 INTER_NEAREST 并如实记录。
 */
cv::Mat maxPoolTo(const cv::Mat& src, cv::Size dst)
{
    if (src.empty() || dst.width <= 0 || dst.height <= 0 || src.cols % dst.width != 0
        || src.rows % dst.height != 0) {
        cv::Mat fallback;
        cv::resize(src, fallback, dst, 0, 0, cv::INTER_NEAREST);
        return fallback;  // 非整数倍：本函数不适用（调用方只有 2× 一条路），退回最近邻
    }
    const int sx = src.cols / dst.width, sy = src.rows / dst.height;
    cv::Mat out(dst, CV_64FC1);
    for (int y = 0; y < dst.height; ++y) {
        double* drow = out.ptr<double>(y);
        for (int x = 0; x < dst.width; ++x) {
            double v = 0.0;
            for (int dy = 0; dy < sy; ++dy) {
                const double* r = src.ptr<double>(y * sy + dy) + x * sx;
                for (int dx = 0; dx < sx; ++dx)
                    v = std::max(v, r[dx]);
            }
            drow[x] = v;
        }
    }
    return out;
}

RejectStats rejectStats(const Group& g)
{
    RejectStats s;
    for (const auto& a : g.alpha) {
        // 分桶口径**唯一出处** = `rejectFractionOf`（temporal_residual.h）。曾经这里是手写的
        // 四次 `cv::compare` + 作差，写错过一次（拿累加器作差 → 盒裁剪 −1388%）；现在那类错误
        // 结构上消失，且与 `temporal-residual --reject` 的交叉表**共用同一把尺子**——两份分桶
        // 的边界一旦不同，两条判据的「拒收」就不是同一件事，而这种漂移不会让任何一条变红。
        const RejectFraction f = rejectFractionOf(a);
        s.accepted += f.accepted;
        s.clipped += f.clipped;
        s.collapsed += f.collapsed;
        s.reproject += f.reproject;
        s.noHistory += f.noHistory;
        ++s.frames;
    }
    // 自检（判据工具自己要有自检）：五桶是**划分** ⟹ 占比之和必须为 1。
    // 作差写错 / 边界重叠 / 掩膜不是单通道，都会先在这里出声，而不是把 −1388% 打到表里。
    const double nfr = std::max(1, s.frames);
    const double sum = s.accepted + s.clipped + s.collapsed + s.reproject + s.noHistory;
    if (std::abs(sum - nfr) > 1e-9)
        throw std::runtime_error("拒收掩膜分桶不是划分：五桶之和 = " + std::to_string(sum)
            + "（应 = 帧数 " + std::to_string(s.frames) + "）——分桶代码坏了，读数无效");
    const double n = nfr;
    s.accepted /= n;
    s.clipped /= n;
    s.collapsed /= n;
    s.reproject /= n;
    s.noHistory /= n;
    return s;
}

/// 掩膜自检（阳性锚点）：diag_reject 键没生效 = alpha 恒 1.0 = 全桶「无历史」。
/// 历史在热身 16 帧后必然有效 ⟹ noHistory 占比 > 0.5 只可能是门没开，当场停下。
void assertMaskAlive(const std::string& name, const RejectStats& s)
{
    if (s.noHistory > 0.5)
        throw std::runtime_error(name + "：掩膜全桶「无历史」（noHistory = "
            + std::to_string(s.noHistory) + "）——diag_reject 没生效（键名/载荷/alpha 通道"
              "任一环断了都会这样），拒收比例读数无效");
}

double medianOf(std::vector<int> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 == 1 ? static_cast<double>(v[n / 2]) : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// 全帧线性 L1（对齐代价函数；与掩码无关，避免「掩码依赖对齐、对齐依赖掩码」的循环）。
[[nodiscard]] double frameL1(const cv::Mat& a, const cv::Mat& b)
{
    cv::Mat d;
    cv::absdiff(a, b, d);
    return cv::mean(d)(0);
}

Alignment alignSequence(const Group& cand, const Group& ref, int window, bool moving)
{
    Alignment a;
    const int n = static_cast<int>(cand.luma.size());
    const int m = static_cast<int>(ref.luma.size());
    a.pairIndex.assign(static_cast<size_t>(n), 0);
    if (!moving) {  // 静止组：所有参考帧逐位相同 ⟹ argmin 恒平坦，配对无信息，钉 0 并跳过护栏
        for (int i = 0; i < n; ++i)
            a.pairIndex[static_cast<size_t>(i)] = std::min(i, m - 1);
        a.minMargin = std::numeric_limits<double>::quiet_NaN();
        return a;
    }
    std::vector<int> deltaFirstHalf, deltaSecondHalf;
    a.minMargin = std::numeric_limits<double>::max();
    for (int i = 0; i < n; ++i) {
        double best = std::numeric_limits<double>::max();
        double second = std::numeric_limits<double>::max();
        int bestJ = std::min(i, m - 1);
        for (int j = std::max(0, i - window); j <= std::min(m - 1, i + window); ++j) {
            const double c = frameL1(cand.luma[static_cast<size_t>(i)],
                ref.luma[static_cast<size_t>(j)]);
            if (c < best) {
                second = best;
                best = c;
                bestJ = j;
            }
            else if (c < second) {
                second = c;
            }
        }
        a.pairIndex[static_cast<size_t>(i)] = bestJ;
        const double margin = (second - best) / std::max(best, 1e-12);
        a.minMargin = std::min(a.minMargin, margin);
        a.offsets.push_back(bestJ - i);
        (i < n / 2 ? deltaFirstHalf : deltaSecondHalf).push_back(bestJ - i);
    }
    a.offset = static_cast<int>(std::lround(medianOf(a.offsets)));
    a.halfGap = static_cast<int>(std::lround(
        std::abs(medianOf(deltaFirstHalf) - medianOf(deltaSecondHalf))));
    return a;
}

Deviation measureDeviation(const std::vector<cv::Mat>& cand, const Group& ref,
    const Alignment& al, const cv::Mat& edgeMask, const cv::Mat& flatMask, double tau)
{
    Deviation d;
    d.edgePixels = cv::countNonZero(edgeMask);
    double sumEdge = 0.0, sumFlat = 0.0, sumFrac = 0.0, sumGrad = 0.0;
    int frames = 0;
    for (size_t i = 0; i < cand.size() && i < ref.luma.size(); ++i) {
        const cv::Mat& r = ref.luma[static_cast<size_t>(al.pairIndex[i])];
        cv::Mat diff;
        cv::absdiff(cand[i], r, diff);
        sumEdge += cv::mean(diff, edgeMask)(0);
        sumFlat += cv::mean(diff, flatMask)(0);
        cv::Mat over;
        cv::compare(diff, tau, over, cv::CMP_GT);
        cv::bitwise_and(over, edgeMask, over);
        sumFrac += static_cast<double>(cv::countNonZero(over))
            / static_cast<double>(std::max<int64_t>(1, d.edgePixels));
        const double gc = gradEnergy(cand[i], edgeMask);
        const double gr = gradEnergy(r, edgeMask);
        sumGrad += gc / std::max(gr, 1e-12);
        ++frames;
    }
    const double n = std::max(1, frames);
    d.devEdge = sumEdge / n;
    d.devFlat = sumFlat / n;
    d.fracVisible = sumFrac / n;
    d.gradRatio = sumGrad / n;
    return d;
}

PhaseFit fitJitterPhase(const Group& g, const Group& ref, const Alignment& al,
    const cv::Mat& edgeMask, double jitterScale)
{
    PhaseFit fit;
    const int n = static_cast<int>(g.luma.size());
    auto devOf = [&](const std::vector<cv::Mat>& seq) {
        double s = 0.0;
        int k = 0;
        for (int i = 0; i < n; ++i) {
            cv::Mat d;
            cv::absdiff(seq[static_cast<size_t>(i)],
                ref.luma[static_cast<size_t>(al.pairIndex[i])], d);
            s += cv::mean(d, edgeMask)(0);
            ++k;
        }
        return s / std::max(1, k);
    };
    fit.rawDev = devOf(g.luma);
    fit.bestDev = fit.rawDev;
    for (uint32_t p = 0; p < kJitterPeriod; ++p) {
        std::vector<cv::Mat> seq;
        seq.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const cv::Point2d c = contentShiftPx(static_cast<uint32_t>((p + i) % kJitterPeriod),
                jitterScale);
            // 候选 = F(x − c) ⟹ 复原 F̂(x) = cand(x + c) ⟹ 平移量取 −c（shiftLuma 的符号约定
            // 是 dst(x)=src(x−d)，故 d = −c）。
            seq.push_back(shiftLuma(g.luma[static_cast<size_t>(i)], {-c.x, -c.y}));
        }
        const double dev = devOf(seq);
        if (dev < fit.bestDev) {
            fit.bestDev = dev;
            fit.bestPhase = static_cast<int>(p);
            fit.warped = std::move(seq);
        }
    }
    return fit;
}

/// `reveal[j]` = CV_8UC1 掩膜：第 j 帧「刚让开且之后 maxLag 帧内容稳住」的像素。空 = 无事件。
std::vector<cv::Mat> revealMasks(const Group& ref, double tau, int maxLag)
{
    const int n = static_cast<int>(ref.luma.size());
    std::vector<cv::Mat> moved(n);
    for (int j = 1; j < n; ++j) {
        cv::Mat d;
        cv::absdiff(ref.luma[static_cast<size_t>(j)], ref.luma[static_cast<size_t>(j - 1)], d);
        cv::compare(d, tau, moved[static_cast<size_t>(j)], cv::CMP_GT);
    }
    std::vector<cv::Mat> reveal(n);
    for (int j = 2; j + maxLag < n; ++j) {
        cv::Mat m = moved[static_cast<size_t>(j)] & ~moved[static_cast<size_t>(j - 1)];
        for (int k = 1; k <= maxLag; ++k)
            m = m & ~moved[static_cast<size_t>(j + k)];
        if (cv::countNonZero(m) > 0)
            reveal[static_cast<size_t>(j)] = m;
    }
    return reveal;
}

/**
 * 在 `reveal` 上求归一化鬼影份额与衰减曲线。配对 = **帧计数器之差**（整数常量，零阈值）。
 *
 * ⚠ **`jlo/jhi` 必须是与偏移无关的常数区间**（首轮实测踩的坑）：各臂进程的 burst 起始帧号
 * 不同 ⟹ 若让每臂各用自己的 `[2, nR-1-maxLag] ∩ 偏移可行域`，边界裁掉的帧数就不同
 * ⟹ **各臂比的是不同的事件子集**（实测事件数最多差 34%）。此时 frac 的臂间比值不可解释。
 * 内部窗口 = `[maxLag + margin, nR-1-maxLag-margin]`，对所有臂逐位相同；偏移大到覆盖不到
 * 就是**测量错误**，抛错出声，绝不再静默裁帧（裁帧 = 把缺陷变成口径）。
 */
/// 掩膜上的 `Σ|候选 − 参考|`（`measureTail` 的**分子**内层，逐字搬出以降认知复杂度）。
/// 口径一个字节没改：仍是「逐行取指针 + 只累加 `mr[x] != 0` 的像素」，`W` 仍是 `mask.cols`。
[[nodiscard]] double tailMaskAbsSum(const cv::Mat& mask, const cv::Mat& oc, const cv::Mat& gr)
{
    double acc = 0.0;
    const int W = mask.cols;
    for (int y = 0; y < mask.rows; ++y) {
        const uchar* mr = mask.ptr<uchar>(y);
        const double* o = oc.ptr<double>(y);
        const double* g = gr.ptr<double>(y);
        for (int x = 0; x < W; ++x)
            if (mr[x])
                acc += std::abs(o[x] - g[x]);
    }
    return acc;
}

/// 掩膜上的 `Σ|ref_j − ref_{j−1}|` 与事件计数（`measureTail` 的**分母**内层，逐字搬出）。
/// 出参累加而非返回：原实现就是在 `j` 循环里一路累到 `den` / `r.events`。
void tailMaskChangeSum(const cv::Mat& mask, const cv::Mat& prev, const cv::Mat& cur,
    double& den, double& events)
{
    const int W = mask.cols;
    for (int y = 0; y < mask.rows; ++y) {
        const uchar* mr = mask.ptr<uchar>(y);
        const double* s = prev.ptr<double>(y);
        const double* n1 = cur.ptr<double>(y);
        for (int x = 0; x < W; ++x)
            if (mr[x]) {
                den += std::abs(n1[x] - s[x]);
                events += 1.0;
            }
    }
}

TailRow measureTail(const std::string& tag, double clip, const Group& ref, const Group& cand,
    const std::vector<cv::Mat>& reveal, int maxLag, int jlo, int jhi)
{
    TailRow r;
    r.tag = tag;
    r.clip = clip;
    const long off = static_cast<long>(cand.framePre) - static_cast<long>(ref.framePre);
    r.offset = off;
    const int nC = static_cast<int>(cand.luma.size());
    std::vector<double> num(static_cast<size_t>(maxLag) + 1, 0.0);
    double den = 0.0;
    for (int j = jlo; j <= jhi; ++j) {
        const cv::Mat& mask = reveal[static_cast<size_t>(j)];
        if (mask.empty())
            continue;
        const long i = static_cast<long>(j) - off;  // 候选里同一绝对帧
        if (i < 0 || i + maxLag >= nC)
            throw std::runtime_error(
                tag + "：帧号偏移 " + std::to_string(off)
                    + " 超出内部窗口余量（burst 要加大或 --margin 要加宽；**不要改成静默裁帧**）");
        tailMaskChangeSum(mask, ref.luma[static_cast<size_t>(j - 1)],
            ref.luma[static_cast<size_t>(j)], den, r.events);
        for (int k = 0; k <= maxLag; ++k)
            num[static_cast<size_t>(k)] += tailMaskAbsSum(mask,
                cand.luma[static_cast<size_t>(i + k)], ref.luma[static_cast<size_t>(j + k)]);
    }
    if (r.events > 0.0 && den > 0.0) {
        r.meanChange = den / r.events;
        r.decay.resize(static_cast<size_t>(maxLag) + 1);
        for (int k = 0; k <= maxLag; ++k)
            r.decay[static_cast<size_t>(k)] = num[static_cast<size_t>(k)] / den;
        r.frac = r.decay[0];
    }
    return r;
}

/// 两幅静止收敛态的**最大**逐像素偏离（线性 luma）。
/// 为什么取 max 不取均值：这条要抓的是「某个像素上 α 被换了个数」= 局部 1e-1 量级错，
/// 均值会被大面积的 0 稀释掉。跨进程地板（债-61）= 1 LSB ≈ 4e-3。
double staticDiff(const cv::Mat& a, const cv::Mat& b)
{
    if (a.empty() || b.empty() || a.size() != b.size())
        return -1.0;
    cv::Mat d;
    cv::absdiff(a, b, d);
    double mx = 0.0;
    cv::minMaxLoc(d, nullptr, &mx);
    return mx;
}

}  // namespace tmotion

}  // namespace RGL::imgqa
