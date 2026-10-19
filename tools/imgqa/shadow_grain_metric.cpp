// shadow_grain_metric.cpp —— `shadow-grain` 的**度量口径层**实现（覆盖度场 + 跳变密度）。
//
// 缝与全部口径理由见 `shadow_grain_metric.h` 头注释。本文件只放实现；四条算子（覆盖度 /
// 区域密度 / 逐带跳变计数 / 逐带存活率）的期望值全部来自闭式（`selftest_grain.cpp`：
// 棋盘/竖条纹 = 恰 1.0、34 宽单调斜坡 = 恰 1/33、常数场 = 恰 0、带在场存活率 = 恰 1.0、
// cov 的线性域锚点 = 闭式 0.784061…），实机截图只证明「与上次一样」，
// 闭式才证明「算子算的是我说的那个量」。
//
// 参考实现 = 判据设计者定口径时用的 Python 脚本（sRGB 解码逐式同 `srgbDecode`，
// 密度 = `np.abs(np.diff(bin, axis=1)).sum() / rows / (cols−1)`）；本实现与它逐式同，
// 别「顺手优化」成别的形态（比如把 diff 换成 Sobel——那会把「跳变计数」变成「梯度幅值」，
// 闭式 1/33 立刻不成立）。
#include "shadow_grain_metric.h"
#include "imgqa.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <stdexcept>

namespace RGL::imgqa {

cv::Mat grainCoverage(const cv::Mat& onBgr, const cv::Mat& offBgr)
{
    if (onBgr.size() != offBgr.size() || onBgr.type() != CV_8UC3 || offBgr.type() != CV_8UC3)
        throw std::runtime_error("grainCoverage: 两图必须同尺寸 CV_8UC3（on/off 不是同一帧的"
              "两次采集，覆盖度差没有意义）");
    cv::Mat cov(onBgr.rows, onBgr.cols, CV_64FC1);
    for (int y = 0; y < onBgr.rows; ++y) {
        const auto* on = onBgr.ptr<cv::Vec3b>(y);
        const auto* off = offBgr.ptr<cv::Vec3b>(y);
        auto* out = cov.ptr<double>(y);
        for (int x = 0; x < onBgr.cols; ++x) {
            double sum = 0.0;
            for (int c = 0; c < 3; ++c) {
                // 🔴 先 sRGB 解码再算比值（硬约束第 2 条「统计一律线性域」）。字节域里
                // 同一个绝对差在暗部/亮部是完全不同的物理量，cov 的 0.5 二值线会随亮度漂移。
                const double o = srgbDecode(off[x][c] / 255.0);
                const double n = srgbDecode(on[x][c] / 255.0);
                sum += std::clamp((o - n) / (o + kGrainEps), 0.0, 1.0);
            }
            out[x] = sum / 3.0;
        }
    }
    return cov;
}

double grainDensity(const cv::Mat& cov, const cv::Rect& win)
{
    if (cov.type() != CV_64FC1)
        throw std::runtime_error("grainDensity: cov 必须是 grainCoverage 产出的 CV_64FC1");
    if (win.width < 2 || win.height < 1)
        throw std::runtime_error("grainDensity: 窗口宽必须 ≥ 2（分母 rows×(cols−1) 不许为 0，"
              "空分母的「0 密度」= 假绿）");
    if (win.x < 0 || win.y < 0 || win.x + win.width > cov.cols || win.y + win.height > cov.rows)
        throw std::runtime_error("grainDensity: 窗口越界（区域来自几何，越界 = 投影或靶漂了，"
              "不许夹到图像边缘再算）");
    int64_t flips = 0;
    for (int y = win.y; y < win.y + win.height; ++y) {
        const auto* row = cov.ptr<double>(y);
        for (int x = win.x; x + 1 < win.x + win.width; ++x) {
            // 严格 >（恰 0.5 不计，与 kUmbraFactor 同口径）；**双向**跳变都数——只数
            // 0→1 或只数 1→0 是「本地正确的另一半」，棋盘上它给 0.5 而不是 1.0。
            const bool l = row[x] > kGrainBin;
            const bool r = row[x + 1] > kGrainBin;
            flips += l != r ? 1 : 0;
        }
    }
    return static_cast<double>(flips) / static_cast<double>(win.height)
        / static_cast<double>(win.width - 1);
}

int64_t grainBandFlips(const cv::Mat& cov, const cv::Range& cols, const cv::Rect& win)
{
    if (cov.type() != CV_64FC1)
        throw std::runtime_error("grainBandFlips: cov 必须是 grainCoverage 产出的 CV_64FC1");
    if (cols.end - cols.start < 3)
        throw std::runtime_error("grainBandFlips: 列区间宽 < 3——宽 2 的唯一相邻对同时是"
              "带左缘与右缘，闭式「每行 2」不成立，拒绝出读数");
    if (cols.start < win.x || cols.end > win.x + win.width)
        throw std::runtime_error("grainBandFlips: 列区间不在窗口 x 跨度内——夹边窗口的"
              "「0 跳变」是假绿（同 grainDensity 的越界守卫）");
    if (win.x < 0 || win.y < 0 || win.x + win.width > cov.cols || win.y + win.height > cov.rows)
        throw std::runtime_error("grainBandFlips: 窗口越界（区域来自几何，越界 = 投影或靶"
              "漂了，不许夹到图像边缘再算）");
    int64_t flips = 0;
    for (int y = win.y; y < win.y + win.height; ++y) {
        const auto* row = cov.ptr<double>(y);
        // 对 (x, x+1) 全落在 [cols.start, cols.end) 内的相邻列逐对比较；**双向**都数——
        // 带在场 = 左缘 0→1 + 右缘 1→0 = 每行恰 2（单向变异给 1，存活率 0.5，selftest 红）。
        for (int x = cols.start; x + 1 < cols.end; ++x) {
            const bool l = row[x] > kGrainBin;
            const bool r = row[x + 1] > kGrainBin;
            flips += l != r ? 1 : 0;
        }
    }
    return flips;
}

double grainBandSurvival(const cv::Mat& cov, const cv::Range& cols, const cv::Rect& win)
{
    // 分母 = 闭式刻度「带在场每行 2 次跳变」× 行数——存活率的刻度是算出来的，不是测出来的。
    return static_cast<double>(grainBandFlips(cov, cols, win))
        / (2.0 * static_cast<double>(win.height));
}

}  // namespace RGL::imgqa
