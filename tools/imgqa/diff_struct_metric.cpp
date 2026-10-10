// 结构差分统计的算子本体（口径与对账声明 → diff_struct_metric.h 头注释）。
// 本文件**不吃截图不启引擎**：输入 = 内存里的线性亮度数组 + 可选 roughness 数组，
// selftest 用合成数组把每条性质钉成闭式（实机数字只能证明「与上次一样」）。
#include "diff_struct_metric.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace RGL::imgqa {
namespace {

/// nearest-rank 分位数：`sorted[floor(q·(n−1))]`（与 `attribute()` 的 pct 同式）。
/// 调用侧保证 sorted 非空、q ∈ [0,1]。
double nearestRank(const std::vector<double>& sorted, double q)
{
    const size_t idx = static_cast<size_t>(q * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

/// 第一遍：**差分集合**（|Δ| > minDelta）与三个计数。集合的构造口径（严格 >、方向按符号）
/// 全部集中在这里，域交叉那一遍按同一判据取「变暗」子集，两处不许各写一套比较。
void collectDiff(const cv::Mat& lumaA, const cv::Mat& lumaB, double minDelta,
    DiffStructStat& st, std::vector<double>& absDeltas)
{
    for (int y = 0; y < lumaA.rows; ++y) {
        const double* ra = lumaA.ptr<double>(y);
        const double* rb = lumaB.ptr<double>(y);
        for (int x = 0; x < lumaA.cols; ++x) {
            const double d = ra[x] - rb[x];
            if (std::abs(d) <= minDelta)
                continue;
            ++st.diff;
            absDeltas.push_back(std::abs(d));
            if (d < 0.0) {
                ++st.darker;
                st.minDarkDelta = std::min(st.minDarkDelta, d);
            } else {
                ++st.brighter;
            }
        }
    }
}

/// 第二遍（仅 rough 在场时跑）：**影响域交叉**与两个分布。
/// 为什么融成一遍不行 / 拆两遍为什么不要钱：本算子量的是 1280×720 的一次性判据，多一遍
/// 线性扫描是微秒级；换来的是「差分集合」与「域交叉」两条口径各自能独立读、也各自在认知
/// 复杂度护栏内（合在一起 33 > 30 直接打红）。
void collectDomain(const cv::Mat& lumaA, const cv::Mat& lumaB, const cv::Mat& rough, double gate,
    double minDelta, DiffStructStat& st, std::vector<double>& darkRough)
{
    for (int y = 0; y < lumaA.rows; ++y) {
        const double* ra = lumaA.ptr<double>(y);
        const double* rb = lumaB.ptr<double>(y);
        const double* rr = rough.ptr<double>(y);
        for (int x = 0; x < lumaA.cols; ++x) {
            // 过门控像素 = 0<rough<gate 的**全图**计数（参考实现在差分判定之外数它，
            // 挪进差分集会把「没变但光滑」的像素漏出分母；g=0 天空排除，口径见头注释）。
            if (rr[x] > 0.0 && rr[x] < gate)
                ++st.gatePixels;
            const double d = ra[x] - rb[x];
            if (d >= 0.0 || std::abs(d) <= minDelta)
                continue;
            darkRough.push_back(rr[x]);
            if (rr[x] >= gate)
                ++st.darkOutsideGate;  // 域外变暗 = 减法漏到干面
        }
    }
}

}  // namespace

double diffStructRatio(double p90, double p10)
{
    // p10 = 0 → +inf：比值无定义 ≠ 比值是 0（写 0 = 把「量具取不出数」读成满分）。
    return p10 == 0.0 ? std::numeric_limits<double>::infinity() : p90 / p10;
}

DiffStructStat diffStructStat(const cv::Mat& lumaA, const cv::Mat& lumaB,
    const cv::Mat& rough, double gate, double minDelta)
{
    // 前提守卫：错位/错型的两幅图不是同一次采集，出数就是假绿（同 bandTableOf 的纪律）。
    if (lumaA.empty() || lumaA.type() != CV_64FC1 || lumaB.type() != CV_64FC1
        || lumaA.size() != lumaB.size())
        throw std::invalid_argument(
            "diffStructStat：两侧 luma 必须同为 CV_64FC1 且同尺寸（不是同一次采集不出数）");
    if (!rough.empty() && (rough.type() != CV_64FC1 || rough.size() != lumaA.size()))
        throw std::invalid_argument(
            "diffStructStat：roughness 必须为 CV_64FC1 且与亮度图同尺寸（g/255 归一后传入）");

    DiffStructStat st;
    st.width = lumaA.cols;
    st.height = lumaA.rows;
    st.total = static_cast<int64_t>(lumaA.total());
    st.hasRough = !rough.empty();

    std::vector<double> absDeltas;  // 差分集合上的 |Δ|（分位数输入）
    collectDiff(lumaA, lumaB, minDelta, st, absDeltas);
    if (!absDeltas.empty()) {
        std::sort(absDeltas.begin(), absDeltas.end());
        st.p10 = nearestRank(absDeltas, 0.10);
        st.p50 = nearestRank(absDeltas, 0.50);
        st.p90 = nearestRank(absDeltas, 0.90);
        st.ratio = diffStructRatio(st.p90, st.p10);
        st.ratioFinite = std::isfinite(st.ratio);
    } else {
        // 空差分集合（两张图逐位相同）：分位数与比值**无定义**。留 0.0 会被下游读成
        // 「p90/p10 = 0 = 结构完全平」= 把「量具取不出数」读成满分（同 shadowPenumbraWidth
        // 的 NaN 纪律，也同上面 diffStructRatio 那条），故给 NaN 并置 ratioFinite=false。
        st.ratio = std::numeric_limits<double>::quiet_NaN();
        st.ratioFinite = false;
    }

    std::vector<double> darkRough;  // 变暗像素的 roughness（分布输入）
    if (st.hasRough)
        collectDomain(lumaA, lumaB, rough, gate, minDelta, st, darkRough);
    if (!darkRough.empty()) {
        std::sort(darkRough.begin(), darkRough.end());
        st.roughMin = darkRough.front();
        st.roughP50 = nearestRank(darkRough, 0.50);
        st.roughMax = darkRough.back();
        st.roughDistValid = true;
    }
    return st;
}

}  // namespace RGL::imgqa
