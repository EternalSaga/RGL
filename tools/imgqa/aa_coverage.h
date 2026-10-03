#pragma once
// 逐杆覆盖率对拍闭式（批 12.6 T4）——抗锯齿**质量**的判据，替代被推翻的屏幕空间逐帧差
// （为什么后者不能用：docs/taa_plan.md §7 第 10 条，反模式 `screen-space-l1-frame-diff`）。
//
// ## 原理（一句话）
//
// 一根投影宽度 w 的黑杆画在白板上，正确覆盖混合后，沿杆方向的**黑度积分**恰为 w 列：
//     I = Σ_cols [ 1 − 覆盖率反解(该列平均亮度) ]  ≈  w_px
// 无抗锯齿时二值覆盖把每根杆量化成「整列黑」或「整列不黑」，I 只能取整数附近 ⟹ 亚像素杆
// （w<1）整根消失或整根出现，误差达 100%。这是**单帧**判据：不需要基线图、不涉及时域差分、
// 与采集时序无关。
//
// ## 为什么必须反 ACES（而不是直接拿亮度当覆盖率）
//
// 靶是 unlit：白板线性值 1、黑杆线性值 0，覆盖 c 的像素线性值 = 1−c；但成片经过
// ACES + sRGB 存储，工具 `observedLuma` 只解回 sRGB（ACES 曲线还在）。所以
//     c = 1 − aces⁻¹(luma)
// 正向复用 `analytic.cpp` 的闭式 ACES（`imgqa probe` 已拿真机 ±2 LSB 对拍验证过它），
// 反向用单调性 + 二分——不新引入任何一条曲线，避免"第二条实现"漂移。
//
// ## 几何常量是**第二份声明**（⚠ 有绊线）
//
// imgqa 不链 rgl 库、不能 `import aa_target`，而杆的**位置**必须与实际靶一致（只有覆盖度
// 可以独立闭式推），故这里重抄一份。绊线 = `tests/test_aa_target.cppm` 里那条同名常量
// 断言：改 `rgl/aa_target.cppm` 不改这里 → 单测红并点名本文件。

#include <array>
#include <vector>

#include <opencv2/core.hpp>

namespace RGL::imgqa {

/// 靶几何（与 `rgl/aa_target.cppm` namespace aaTarget 逐字同值；绊线见文件头 ⚠）。
inline constexpr unsigned kLadderRods = 12;
inline constexpr double kLadderX0 = -0.95;       ///< 首杆中心 x
inline constexpr double kLadderDx = 0.05;        ///< 杆间距
inline constexpr double kLadderY = 0.34;         ///< 杆中心 y
inline constexpr double kLadderHeight = 0.42;    ///< 杆高
inline constexpr double kLadderWMin = 0.0015;    ///< 首杆宽（0.65px = 亚像素）
inline constexpr double kLadderWMax = 0.015;     ///< 末杆宽（6.5px = 超像素）

/// 采集机位（与 `imgqa temporal` 的 profile 同一份；靶头注释亦同）。
inline constexpr double kCamZ = 2.0;             ///< 相机 (0,0,2) 看 −Z
inline constexpr double kFovRad = 0.78539816339744831;  ///< 45°
inline constexpr int kViewW = 1280;
inline constexpr int kViewH = 720;

/// 深度 d 处的 px/m（独立闭式：视锥高度 = 2·d·tan(fov/2)）。
[[nodiscard]] double pxPerMeter(double d);

/// 一根杆的期望：世界宽 / 投影宽（px）/ 屏幕列心与行区间。
struct RodExpectation {
    unsigned index = 0;
    double widthWorld = 0.0;
    double widthPx = 0.0;   ///< 闭式期望 = 黑度积分应等于它（单位：列）
    int colCenter = 0;
    int rowMin = 0;
    int rowMax = 0;         ///< 开区间
};

/// 阶梯杆的期望表（按 index 升序，宽度单调增）。
[[nodiscard]] std::vector<RodExpectation> ladderExpectations();

/// 由成片亮度反 ACES 得线性值（单调 + 二分）。luma 超出曲线值域时夹到端点。
[[nodiscard]] double invAces(double luma);

/// 单根杆的实测结果。
struct RodMeasurement {
    RodExpectation expect;
    double integral = 0.0;    ///< 实测黑度积分（列）
    double error = 0.0;       ///< integral − widthPx
    double relError = 0.0;    ///< error / widthPx（widthPx<1 时无意义，看 error）
};

/// 一批成片（同机位、已 prepareLinear 的 luma）上的逐杆测量 + 白板/黑件电平自检。
struct CoverageReport {
    std::vector<RodMeasurement> rods;
    double whiteLuma = 0.0;   ///< 白板实测亮度（覆盖率反解的基准，须接近 ACES(1)）
    double blackLuma = 0.0;   ///< 杆心最暗亮度（须接近 0）
    bool valid = false;       ///< 电平自检未过 = 靶没画对/机位不对，比值无意义
};

/// 逐杆测量。luma 为 CV_64FC1（`Frame::luma`）。
[[nodiscard]] CoverageReport measureCoverage(const cv::Mat& luma);

}  // namespace RGL::imgqa
