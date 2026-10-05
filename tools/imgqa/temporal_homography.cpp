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
#include "temporal_flicker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
};

/// 把 [0,1] 的残差图写成可比的热图（**固定标尺** 0→黑 / 1→白，两组之间才能对眼）
void writeHeat(const cv::Mat& heat, const std::string& path)
{
    cv::Mat byte;
    heat.convertTo(byte, CV_8UC1, 255.0);  // 固定标尺，不做 min/max 自缩放（自缩放会让两组看起来一样）
    cv::imwrite(path, byte);
}

HomoStat runHomography(const FlickerSeries& g, double yawRadPerFrame, const std::string& heatPath)
{
    HomoStat s;
    s.name = g.name;
    s.size = g.frameSize;
    s.byteMaxAdjacent = g.byteMaxAdjacent;
    if (g.luma.size() < 3)
        throw std::runtime_error(g.name + "：帧数不足 3，无法做相邻帧归因");

    const double focal = verticalFocalPx(g.frameSize.height, kFovDeg);
    const double cx = static_cast<double>(g.frameSize.width) * 0.5;
    const double cy = static_cast<double>(g.frameSize.height) * 0.5;
    const cv::Size sz = g.frameSize;
    constexpr double kP = 0.999;  ///< 全帧分位：1px 边缘只占 0.16% 像素，p99.5 会把它整条抹平（实测踩过）

    // 两个符号假设各跑一遍（闭式对拍，不是搜索）；热图留到选定符号后再出
    double resid[2] = { 0.0, 0.0 };
    std::vector<cv::Mat> heat(2, cv::Mat::zeros(sz, CV_32FC1));
    for (int si = 0; si < 2; ++si) {
        const cv::Matx33d Hm = yawHomography((si == 0 ? 1.0 : -1.0) * yawRadPerFrame, focal, cx, cy);
        const cv::Mat H = cv::Mat(Hm);
        for (size_t k = 1; k < g.luma.size(); ++k) {
            cv::Mat warped, res, res32;
            cv::warpPerspective(g.luma[k - 1], warped, H, sz, cv::INTER_LINEAR, cv::BORDER_REPLICATE);
            cv::absdiff(g.luma[k], warped, res);
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
    const cv::Matx33d Hm = yawHomography(yawRadPerFrame, focal, cx, cy);
    const cv::Mat H = cv::Mat(Hm), Hi = cv::Mat(Hm.inv());
    double rawSum = 0.0, floorSum = 0.0;
    for (size_t k = 1; k < g.luma.size(); ++k) {
        cv::Mat raw, there, back, rt, raw32;
        cv::absdiff(g.luma[k], g.luma[k - 1], raw);
        raw.convertTo(raw32, CV_32FC1);
        rawSum += histPercentile(raw32, kP);
        cv::warpPerspective(g.luma[k], there, H, sz, cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        cv::warpPerspective(there, back, Hi, sz, cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        cv::absdiff(g.luma[k], back, rt);
        cv::Mat rt32;
        rt.convertTo(rt32, CV_32FC1);
        floorSum += histPercentile(rt32, kP);
    }
    s.rawP995 = rawSum / static_cast<double>(pairs);
    s.floorP995 = floorSum / static_cast<double>(pairs);

    s.heat = heat[static_cast<size_t>(best)];
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
    const std::string& outPrefix)
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

    std::vector<HomoStat> stats;
    for (bool on: { false, true }) {
        const std::string tag = on ? "homo_on" : "homo_off";
        const std::string dir = projectRoot() + "/build/imgqa_homo_" + tag;
        std::printf("  采集 %s …\n", tag.c_str());
        try {
            FlickerSeries g = captureFlickerSeries(dir, on ? "TAA 开" : "TAA 关", tag, edgeTarget, cam,
                on, yawRad, frames);
            stats.push_back(runHomography(g, yawRad,
                outPrefix.empty() ? std::string() : outPrefix + (on ? "_taa_on_heat.png" : "_taa_off_heat.png")));
        }
        catch (const std::exception& e) {
            std::printf("  [失败] %s\n", e.what());
            return 1;
        }
    }

    std::printf("\n  组       原始帧差p99.9  残差p99.9  反号残差  往返地板  残差−地板  解释率  残差>1LSB\n");
    std::printf("  %s\n", std::string(96, '-').c_str());
    for (const auto& s: stats) {
        const double explained = s.rawP995 > 1e-9 ? 1.0 - s.residP995 / s.rawP995 : 0.0;
        std::printf("  %-7s %-14s %-11s %-10s %-10s %-11s %-8s %s\n", s.name.c_str(),
            d4(s.rawP995).c_str(), d4(s.residP995).c_str(), d4(s.residOther).c_str(),
            d4(s.floorP995).c_str(), d4(s.residP995 - s.floorP995).c_str(), d2(explained).c_str(),
            d2(s.frac1Lsb).c_str());
        if (s.residP995 < s.floorP995 * 1.5)
            std::printf("    ⚠ 残差与 warp 往返地板同量级（%s vs %s）——量具在响，归因图不可当闪烁读\n",
                d4(s.residP995).c_str(), d4(s.floorP995).c_str());
        if (s.residOther < s.residP995 * 1.5)
            std::printf("    ⚠ 两个符号假设的残差同量级（%s vs %s）——扫速太小或画面无边缘，符号无从分辨\n",
                d4(s.residP995).c_str(), d4(s.residOther).c_str());
    }
    if (!outPrefix.empty())
        std::printf("\n  热图（固定标尺 0→黑 / 1→白，两组可直接对眼）: %s_taa_{on,off}_heat.png\n", outPrefix.c_str());
    return 0;
}

}  // namespace RGL::imgqa
