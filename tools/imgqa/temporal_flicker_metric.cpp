//
// temporal_flicker_metric.cpp — `temporal-flicker` 的**度量口径层**（闭式解 + 反标定 + 谱诊断 + ROI 定位）
//
// 拆出本 TU 的理由见 `temporal_flicker_metric.h` 头注释（一句话：口径层不碰引擎、被 selftest
// 用闭式直接测，仪器层要引擎，两者不是同一条改动缝）。
//
// ⚠ 本层的**闭式解是判据的刻度来源**（理想 AA 下界 / 完全无 AA 上界），一个算子都不许
// 「顺手化简」：displayChain 必须走 toBytes/observedLuma 这条唯一显示链出处（闭式解与像素
// 同链才可比，写两份就是第二份真相）；flickerClosedForm 的网格 1/8192 ≪ 扫速是上确界取到
// 斜坡段的前提；hannHalfSpectrum 是**诊断打印**（高频带能量比已被闭式对照否决，见主 TU 头
// 注释），不参与判决但必须留着——「为什么 FFT 不能当判据」的实物证据就是它。
// 判据的实测标定数据（16 个亚像素相位 τ 的网格）在主 TU 的头注释里，别去别处找。
//
#include "temporal_flicker_metric.h"
#include "temporal_residual.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
namespace {

/// 列平均亮度剖面（线性域，跨 ROI 行求均值）
std::vector<float> columnProfile(const cv::Mat& luma, const cv::Rect& win)
{
    std::vector<float> prof(static_cast<size_t>(win.width), 0.0f);
    for (int c = 0; c < win.width; ++c) {
        double sum = 0.0;
        for (int r = 0; r < win.height; ++r)
            sum += static_cast<double>(luma.at<double>(win.y + r, win.x + c));
        prof[static_cast<size_t>(c)] = static_cast<float>(sum / static_cast<double>(win.height));
    }
    return prof;
}

}  // namespace

cv::Rect edgeRoi(const std::vector<cv::Mat>& luma, double expectedX, double& edgeX,
    double& edgeStep, double halfWidth, double rowFrac)
{
    const int w = luma[0].cols, h = luma[0].rows;
    const int cy = h / 2;
    const int halfRows = std::max(8, static_cast<int>(rowFrac * static_cast<double>(h)));
    const int y0 = std::max(0, cy - halfRows);
    cv::Rect win(0, y0, w, std::min(2 * halfRows, h - y0));
    constexpr int kSearch = 16;  ///< 期望位置 ±16px（扫程只有 4px，余量足够）
    const int x0 = std::max(1, static_cast<int>(std::lround(expectedX)) - kSearch);
    const int x1 = std::min(w - 2, static_cast<int>(std::lround(expectedX)) + kSearch);
    cv::Rect sub(x0, win.y, x1 - x0 + 1, win.height);
    const auto prof = columnProfile(luma[0], sub);
    const float local = steepestEdgeColumn(prof);
    edgeX = static_cast<double>(x0) + local;
    const size_t i = std::clamp(static_cast<size_t>(std::lround(local)), size_t{ 1 }, prof.size() - 2);
    edgeStep = std::abs(prof[i + 1] - prof[i - 1]);
    const int hw = std::max(2, static_cast<int>(std::lround(halfWidth)));
    cv::Rect roi(std::max(0, static_cast<int>(std::lround(edgeX)) - hw), win.y, 2 * hw, win.height);
    roi.x = std::min(roi.x, w - roi.width);
    roi.y = std::min(roi.y, h - roi.height);
    return roi;
}

cv::Rect strongestRoi(const std::vector<cv::Mat>& luma, double halfWidth, cv::Point2d& at)
{
    const int w = luma[0].cols, h = luma[0].rows;
    cv::Mat g;
    cv::Sobel(luma[0], g, CV_64F, 1, 0, 1);  // |dx|（垂直边缘是本项目的主战场）
    double best = 0.0;
    int bx = w / 2, by = h / 2;
    for (int y = 2; y + 2 < h; ++y)
        for (int x = 2; x + 2 < w; ++x) {
            // 3×3 局部和：单像素极值会被高光噪点抢走，边缘带要的是「一片」强梯度
            const double s = std::abs(g.at<double>(y, x)) + std::abs(g.at<double>(y, x + 1))
                + std::abs(g.at<double>(y + 1, x)) + std::abs(g.at<double>(y + 1, x + 1));
            if (s > best) {
                best = s;
                bx = x;
                by = y;
            }
        }
    at = cv::Point2d(bx, by);
    const int hw = std::max(4, static_cast<int>(std::lround(halfWidth)));
    cv::Rect roi(bx - hw / 2, by - hw / 2, hw, hw);
    roi.x = std::clamp(roi.x, 0, w - roi.width);
    roi.y = std::clamp(roi.y, 0, h - roi.height);
    return roi;
}

void measureGroup(Group& g)
{
    std::vector<double> vals;
    for (const auto& m: g.luma)
        for (int r = 0; r < g.roi.height; ++r)
            for (int c = 0; c < g.roi.width; ++c)
                vals.push_back(m.at<double>(g.roi.y + r, g.roi.x + c));
    std::sort(vals.begin(), vals.end());
    g.darkObs = percentileOf(vals, 0.005);
    g.whiteObs = percentileOf(vals, 0.995);
    g.scale = scaleFromObserved(static_cast<float>(g.darkObs), static_cast<float>(g.whiteObs));
    g.rho = frameDeltaPercentile(g.luma, g.roi, 0.995);
    for (const auto& m: g.luma) {
        double sum = 0.0;
        for (int r = 0; r < g.roi.height; ++r)
            for (int c = 0; c < g.roi.width; ++c)
                sum += m.at<double>(g.roi.y + r, g.roi.x + c);
        g.signal.push_back(static_cast<float>(sum / static_cast<double>(g.roi.width * g.roi.height)));
    }
    g.spectrum = hannHalfSpectrum(g.signal);
    g.anchor = flickerClosedForm(static_cast<float>(g.sweepLocal), g.scale);
}

float displayChain(float coverage, const FlickerScale& scale)
{
    // 复用 imgqa 既有的「线性 HDR → 屏幕字节 → 观测亮度」唯一出处（analytic 探针判据同源），
    // 不在此手搓 ACES/sRGB —— 闭式解与像素必须走同一条链，写两份就是第二份真相。
    const float c = std::clamp(coverage, 0.0f, 1.0f);
    const double lin = static_cast<double>(scale.black) + static_cast<double>(c) * (scale.white - scale.black);
    const std::array<int, 3> bytes = toBytes(Vec3{ lin, lin, lin }, 1.0);
    return static_cast<float>(observedLuma(bytes));
}

FlickerAnchor flickerClosedForm(float sweepPx, const FlickerScale& scale)
{
    FlickerAnchor a;
    a.sweepPx = sweepPx;
    constexpr int kSteps = 8192;
    float best = 0.0f;
    for (int i = 0; i <= kSteps; ++i) {
        const float cov = static_cast<float>(i) / static_cast<float>(kSteps);
        best = std::max(best, std::abs(displayChain(cov + sweepPx, scale) - displayChain(cov, scale)));
    }
    a.idealRho = best;
    a.noAaRho = displayChain(1.0f, scale) - displayChain(0.0f, scale);
    return a;
}

FlickerScale scaleFromObserved(float darkObserved, float whiteObserved)
{
    // 链对「ACES 前线性值」单调递增 ⟹ 两端各自二分即可；40 步到 1e-12 相对精度
    auto invert = [](float observed) {
        float lo = 0.0f, hi = 16.0f;
        if (observed <= 0.0f)
            return 0.0f;
        for (int i = 0; i < 40; ++i) {
            const float mid = 0.5f * (lo + hi);
            const FlickerScale s{ mid, mid };  // 覆盖恒 1 时 chain = decode(aces(mid))
            if (displayChain(1.0f, s) < observed)
                lo = mid;
            else
                hi = mid;
        }
        return 0.5f * (lo + hi);
    };
    FlickerScale s;
    s.black = invert(darkObserved);
    s.white = invert(whiteObserved);
    if (s.white <= s.black)
        s.white = s.black + 1e-4f;  // 全平 ROI（没找到边缘）：给个非零分母，比值会爆，由调用侧判红
    return s;
}

std::vector<float> hannHalfSpectrum(const std::vector<float>& signal)
{
    const int n = static_cast<int>(signal.size());
    std::vector<float> out;
    if (n < 4)
        return out;
    double mean = 0.0;
    for (float v: signal)
        mean += static_cast<double>(v);
    mean /= static_cast<double>(n);
    std::vector<float> w(static_cast<size_t>(n), 0.0f);
    for (int k = 0; k < n; ++k) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(k) / static_cast<double>(n - 1));
        w[static_cast<size_t>(k)] = static_cast<float>((static_cast<double>(signal[static_cast<size_t>(k)]) - mean) * win);
    }
    const int half = n / 2;
    out.reserve(static_cast<size_t>(half + 1));
    for (int m = 0; m <= half; ++m) {
        double re = 0.0, im = 0.0;
        for (int k = 0; k < n; ++k) {
            const double a = 2.0 * kPi * static_cast<double>(m * k) / static_cast<double>(n);
            re += static_cast<double>(w[static_cast<size_t>(k)]) * std::cos(a);
            im -= static_cast<double>(w[static_cast<size_t>(k)]) * std::sin(a);
        }
        out.push_back(static_cast<float>(std::sqrt(re * re + im * im)));
    }
    return out;
}

double frameDeltaPercentile(const std::vector<cv::Mat>& frames, const cv::Rect& roi, double percentile)
{
    if (frames.size() < 2)
        return 0.0;
    std::vector<double> deltas;
    deltas.reserve((frames.size() - 1) * static_cast<size_t>(roi.width) * static_cast<size_t>(roi.height));
    for (size_t t = 1; t < frames.size(); ++t)
        for (int r = 0; r < roi.height; ++r)
            for (int c = 0; c < roi.width; ++c)
                deltas.push_back(std::abs(frames[t].at<double>(roi.y + r, roi.x + c) - frames[t - 1].at<double>(roi.y + r, roi.x + c)));
    std::sort(deltas.begin(), deltas.end());
    return percentileOf(deltas, percentile);
}

float steepestEdgeColumn(const std::vector<float>& profile)
{
    const int n = static_cast<int>(profile.size());
    if (n < 5)
        return static_cast<float>(n) * 0.5f;
    std::vector<float> g(static_cast<size_t>(n), 0.0f);
    for (int i = 1; i + 1 < n; ++i)
        g[static_cast<size_t>(i)] = std::abs(profile[static_cast<size_t>(i + 1)] - profile[static_cast<size_t>(i - 1)]);
    size_t best = 1;
    for (size_t i = 1; i + 1 < static_cast<size_t>(n); ++i)
        if (g[i] > g[best])
            best = i;
    const float denom = g[best - 1] - 2.0f * g[best] + g[best + 1];
    const float shift = std::abs(denom) > 1e-9f ? std::clamp(0.5f * (g[best - 1] - g[best + 1]) / denom, -1.0f, 1.0f) : 0.0f;
    return static_cast<float>(best) + shift;
}

float localSweepPxPerFrame(float yawRadPerFrame, float focalPx, float xPx, float cxPx)
{
    const float u = (xPx - cxPx) / focalPx;
    return std::abs(yawRadPerFrame) * focalPx * (1.0f + u * u);
}

float verticalFocalPx(int heightPx, float fovDeg)
{
    return static_cast<float>(static_cast<double>(heightPx) * 0.5 / std::tan(0.5 * static_cast<double>(fovDeg) * kPi / 180.0));
}

}  // namespace RGL::imgqa
