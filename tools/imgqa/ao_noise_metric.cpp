// ao_noise_metric.cpp —— `ao-noise` 的**口径层**（帧差 / 逐位相同 / 取值分布 / 存图可视化）。
//
// 拆出本 TU 的理由与缝的划法见 `ao_noise_metric.h` 头注释（一句话：口径与仪器不是同一条改动
// 缝，先例 = `shadow_kernel_metric.cpp` / `diff_struct_metric.cpp`）。
//
// 本文件每一条都是**搬**过来的，函数体一字未改（验收 = 原始二进制 vs 当前二进制的结构签名
// 对拍 + 字面量对账）。已知在场但本批**不许修**的一条照原样搬着：`printAoScanline` 在全图
// 拉普拉斯能量恒 0 时带着 `bestRow = -1` 去 `ao.ptr<uint8_t>(-1)` 越界读（→ 交付报告）。
#include "ao_noise_metric.h"

#include "imgqa.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace RGL::imgqa {

// ───────────────────────────── 格式化 ─────────────────────────────

std::string d3(double v)
{
    std::ostringstream o;
    o << std::fixed << std::setprecision(3) << v;
    return o.str();
}
std::string d5(double v)
{
    std::ostringstream o;
    o << std::fixed << std::setprecision(5) << v;
    return o.str();
}

// ───────────────────────────── 逐位相同判定 ─────────────────────────────

/// 两张 PNG 是否**逐位**相同（解码后比像素，不比文件字节——PNG 编码本身不是判据）。
bool samePng(const std::string& a, const std::string& b)
{
    const cv::Mat x = readPngBgr(a), y = readPngBgr(b);
    if (x.size() != y.size() || x.type() != y.type())
        return false;
    cv::Mat d;
    cv::absdiff(x, y, d);
    // 三通道 Mat 不能直接 countNonZero（OpenCV 断言 cn==1）；L1 范数对 8U 是精确整数和，
    // 为 0 ⟺ 每通道逐位相同，不需要先降通道（降通道会引入「加权抵消」这一层需要论证的东西）。
    return cv::norm(d, cv::NORM_L1) == 0.0;
}

// ───────────────────────────── 帧差算子 ─────────────────────────────

/// `mask` 空 = 全图。分区是必需的：全图逐帧差被 TAA jitter 打在**几何边缘**上的变化主导
/// （实测地板 27.6 万像素/帧），AO 的时间贡献淹在里面；契约区（无边缘的平坦面）才是有
/// 分辨力的地方。
FrameDiff diffFrames(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask)
{
    FrameDiff d;
    cv::Mat ad, g, g32;
    cv::absdiff(a, b, ad);
    cv::cvtColor(ad, g, cv::COLOR_BGR2GRAY);
    if (mask.empty()) {
        d.changed = cv::countNonZero(g);
        g.convertTo(g32, CV_64F);
        d.mean = cv::mean(g32)[0];
        double hi = 0.0;
        cv::minMaxLoc(g32, nullptr, &hi);
        d.mx = hi;
        return d;
    }
    cv::Mat gm;
    cv::bitwise_and(g, mask, gm);
    d.changed = cv::countNonZero(gm);
    cv::Mat v;
    cv::meanStdDev(gm, v, v);  // mask 外是 0，均值是「含零填充」的，故按像素数自己除
    d.mean = d.changed > 0 ? v.at<double>(0) * gm.total() / static_cast<double>(cv::countNonZero(mask))
                           : 0.0;
    double hi = 0.0;
    cv::minMaxLoc(gm, nullptr, &hi);
    d.mx = hi;
    return d;
}

// ───────────────────────────── 机位 ─────────────────────────────

/// 绕相机自身 up（世界 Y）纯 yaw：像素位移 = f_px·θ，**与深度无关**（刚体运动，
/// 重投影能精确对齐；平移 dolly 会让信号随深度衰减，见 temporal.cpp 头注释）。
/// 换算：pixel.x = W/2 + (W/2)·X/(aspect·tan(fov/2)·(−Z)) ⟹  yaw 角 θ 时
/// Δpixel = −f_px·θ，f_px = H/(2·tan(fov/2))。二阶项 O(θ²) 在 θ≈1/869 rad 下 < 1e-5 px。
Engine::Camera yawCam(const Engine::Camera& base, double shiftPx)
{
    const double fPx = (kAoSize[1] / 2.0)
        / std::tan(base.fovDegrees * 3.14159265358979323846 / 360.0);
    const double th = shiftPx / fPx;  // 弧度
    const double dx = base.target[0] - base.position[0];
    const double dy = base.target[1] - base.position[1];
    const double dz = base.target[2] - base.position[2];
    Engine::Camera c = base;
    c.target = {base.position[0] + std::cos(th) * dx + std::sin(th) * dz, base.position[1] + dy,
        base.position[2] - std::sin(th) * dx + std::cos(th) * dz};
    return c;
}

// ───────────────────────────── 可视化 ─────────────────────────────

/// 单通道 → 放大可视化（×gain 后饱和到 255）。0 值区域保持 0（黑），故 mask 外是黑的。
void saveAmp(const cv::Mat& x, const cv::Mat& mask, double gain, const std::string& path)
{
    cv::Mat f;
    x.convertTo(f, CV_32F);
    cv::Mat vis = cv::Mat::zeros(x.size(), CV_32F);
    for (int y = 0; y < x.rows; ++y)
        for (int x0 = 0; x0 < x.cols; ++x0)
            if (mask.at<uint8_t>(y, x0))
                vis.at<float>(y, x0) = std::abs(f.at<float>(y, x0)) * gain;
    cv::Mat o;
    vis.convertTo(o, CV_8U);  // convertTo 自带饱和
    cv::imwrite(path, o);
}

/// 把 mask 画成半透明红叠加在灰度底图上（人眼复核区域是否落在该落的地方）。
void saveMaskOverlay(const cv::Mat& base, const cv::Mat& mask, const std::string& path)
{
    cv::Mat g;
    cv::cvtColor(base, g, cv::COLOR_BGR2GRAY);
    cv::Mat bgr;
    cv::cvtColor(g, bgr, cv::COLOR_GRAY2BGR);
    for (int y = 0; y < mask.rows; ++y)
        for (int x = 0; x < mask.cols; ++x)
            if (mask.at<uint8_t>(y, x))
                bgr.at<cv::Vec3b>(y, x) = cv::Vec3b(0, 0, 255);
    cv::imwrite(path, bgr);
}

/// 把 AO 场写成文本化可视化（值域压到 0..255 的伪彩灰度）——直接看台阶/颗粒用。
void saveStretched(const cv::Mat& ao, double lo, double hi, const std::string& path)
{
    cv::Mat f;
    ao.convertTo(f, CV_32F);
    f = (f - lo) * (255.0 / std::max(1e-6, hi - lo));
    cv::Mat o;
    f.convertTo(o, CV_8U);
    cv::imwrite(path, o);
}

// ───────────────────────── 共享类型的小算子 ─────────────────────────

/// piece 名 → objectID；认不出 = 0（= 背景，调用方自己决定跳过还是照算）。
int32_t oidOf(const Collected& col, const std::string& n)
{
    const auto i = col.oidByName.find(n);
    return i == col.oidByName.end() ? 0 : i->second;
}

// ────────────────── 取值分布与扫描线（组 2 的读数面）──────────────────

/// AO 附件的字节直方图（组 2 的第一问：12 样本会不会走成离散台阶）。
std::vector<int> aoHistogram(const cv::Mat& ao)
{
    std::vector<int> hist(256, 0);
    for (int y = 0; y < ao.rows; ++y) {
        const uint8_t* r = ao.ptr<uint8_t>(y);
        for (int x = 0; x < ao.cols; ++x)
            ++hist[r[x]];
    }
    return hist;
}

/// 非 255 的取值（值:像素，按像素降序前 12）——台阶形态直接看这里。
void printNonContractValues(const std::vector<int>& hist)
{
    std::printf("  非 255 的取值（值:像素，按像素降序前 12）：");
    std::vector<int> order(256);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return hist[a] > hist[b]; });
    int shown = 0;
    for (int v : order) {
        if (v == 255 || hist[v] == 0)
            continue;
        std::printf("%d:%d ", v, hist[v]);
        if (++shown >= 12)
            break;
    }
    std::printf("\n");
}

/// 每 8 一档的分桶（自 224 起）——台阶若存在，桶间会有明显的空洞。
void printAoBuckets(const std::vector<int>& hist)
{
    std::printf("  分桶（每 8 一档，自 224 起）：");
    for (int lo = 224; lo < 255; lo += 8) {
        int sum = 0;
        for (int v = lo; v < lo + 8 && v < 255; ++v)
            sum += hist[v];
        std::printf("[%d,%d):%d  ", lo, lo + 8, sum);
    }
    std::printf("[255]:%d\n", hist[255]);
}

/// 全图拉普拉斯能量最大的一行 + 该行最小值的邻域剖面（台阶的现场照片）。
void printAoScanline(const cv::Mat& ao)
{
    int bestRow = -1;
    double bestLap = 0.0;
    for (int y = 0; y < ao.rows; y += 4) {
        const uint8_t* up = ao.ptr<uint8_t>(std::max(0, y - 1));
        const uint8_t* cur = ao.ptr<uint8_t>(y);
        const uint8_t* dn = ao.ptr<uint8_t>(std::min(ao.rows - 1, y + 1));
        double sum = 0.0;
        for (int x = 1; x + 1 < ao.cols; ++x) {
            const double l = 2.0 * cur[x] - up[x] - dn[x];
            sum += l * l;
        }
        if (sum > bestLap) {
            bestLap = sum;
            bestRow = y;
        }
    }
    const uint8_t* r = ao.ptr<uint8_t>(bestRow);
    int lo = 255, loX = -1;
    for (int x = 0; x < ao.cols; ++x)
        if (r[x] < lo) {
            lo = r[x];
            loX = x;
        }
    std::string prof;
    for (int x = std::max(0, loX - 10); x <= std::min(ao.cols - 1, loX + 10); ++x)
        prof += std::to_string(r[x]) + " ";
    std::printf("  扫描线 y=%d（全图拉普拉斯能量最大的一行）：最小 %d @ x=%d\n    邻域 %s\n",
        bestRow, lo, loX, prof.c_str());
}

}  // namespace RGL::imgqa
