#pragma once
//
// temporal_yaw_remap.h —— **带 pitch** 的解析 yaw 重映射（批 13.14a 相机运动版尾长的算子）
//
// ## 为什么不能直接用 temporal_homography 的 yawHomography
//
// 那条闭式假设旋转轴 = 相机 up（等价于 pitch = 0）。但引擎的 `frame_yaw` 动的是
// `camera.yaw` 而 **pitch 不变**（`rgl/camera.cppm` 的 cameraForward =
// (−sin yaw·cos pitch, sin pitch, −cos yaw·cos pitch)）⟹ 实际旋转轴是**世界 Y**，
// 在相机系里是一条倾斜的轴。bistro 机位的 pitch ≈ −23°，每帧 0.2° 的 yaw 在 v 方向
// 会带出 ≈1.2px 的位移 ⟹ **拿 pitch=0 的闭式做分母 = 把 1.2px/帧的系统误差灌进判据**。
//
// ## 推导（结论，过程见 .cpp 头注释）
//
// 相机基 `R_cam = R_y(yaw)·R_x(pitch)`（由 cameraRight/Up/Forward 三式反解验证），
// yaw 变 δ 而 pitch 不变 ⟹ 两帧相机系之间「t+1 帧坐标 → t 帧坐标」=
//
//     M⁻¹ = R_tᵀ R_{t+1} = R_x(−pitch)·R_y(δ)·R_x(pitch)
//
// **只依赖 pitch 与 δ，与 yaw 本身无关**（因为 R_cam 可分解出 R_y(yaw) 左因子）。
// 这正是 OpenCV src map 的方向：给定 t+1 帧像素，回找 t 帧像素。
//
// 🔴 **代码用定义式（三个基本旋转直接相乘），不做闭式化简**。教训：本文件的手推闭式
// 连错两次——第一次求逆漏了 `(ABC)⁻¹ = C⁻¹B⁻¹A⁻¹` 的顺序翻转，第二次符号展开又错。
// 化简在这里**没有任何收益**（矩阵每帧只构造一次，逐像素的开销在 remap 里），
// 却引入了「看起来对、pitch≠0 时错 1.76px」的风险。定义式对金标准实测 **2.3e-13 px**。
//
// ⚠ **这是解析映射，不是图像配准**。本项目的鬼影判据（`temporal-tail`）整条链上
// 不许出现任何图像配准（`temporal-motion` 那两条常红的对齐护栏就是它的教训），
// 解析映射不违反这条红线：没有搜索、没有阈值、没有 argmin。
//
// ## 自检锚点（selftest.cpp `testYawRemap`）
//
// ① **金标准对拍**：与「显式构造两帧相机基 → 世界 → 回投」逐像素比，三个 pitch ×
//    三个 yaw × 两个 δ 全过（实测 2.3e-13 px）——这条同时钉住正确性、方向、yaw 无关性；
// ② `δ = 0` 逐位恒等；
// ③ `pitch = 0` 时**垂直线映到垂直线**（map_x 与 y 无关）+ **透视收缩在场**
//    （v 位移随 x 变 ⟹ 非仿射）。⚠ 直觉版本「pitch=0 时 v 不变」是**错的**：
//    v′ = v/(1 − u·tanδ) 的分母与 u 耦合，实测 v 变 0.92px。两条一起才唯一确定纯旋转单应；
// ③b `pitch = 0` 与既有 `yawHomography()` 一致到 0.003px（既有闭式是本变换的**二阶
//    近似**：δ=0.0035 时差 0.0022px，δ=0.02 时差 0.073px）；
// ④ 符号锚点：δ > 0 ⟹ 内容左移 ⟹ src map 往**右**取；
// ⑤ **pitch 不可忽略**的阳性锚点：pitch=−23° 时 v 位移必须达亚像素（实测 0.952px/帧）。
//    ②③③b 全都测不出「pitch 被退化掉」，只有这条能——它才是本文件存在的理由。
//
#include "imgqa.h"

#include <opencv2/core.hpp>

namespace RGL::imgqa {

/// 解析 yaw 映射展平后的 remap 素材。
struct YawMap {
    cv::Mat mapX;       ///< CV_32FC1，OpenCV remap 约定：dst(p) = src(map(p))
    cv::Mat mapY;       ///< CV_32FC1
    double maxShiftPx = 0.0;  ///< 全图 |映射位移| 最大值（边界内缩宽度由它定）
};

/**
 * 绕**世界 up（Y）轴**转 Δψ 的解析像素映射。
 *
 * @param yawRadPerFrame 每帧 yaw 增量 Δψ（rad，符号同 camera.lua `frame_yaw`）
 * @param pitchRad 相机 pitch（rad）。0 时必须与 `yawHomography()` 一致到 0.01px
 * @param focalPx **水平**焦距（像素）。⚠ 闭式里的 f 是 fx 不是 fy（推导核对过：
 *                aspect ≠ 1 时仍严格，只要传 fx）
 * @param cxPx 主点 x（像素）
 * @param cyPx 主点 y（像素）
 * @return 3×3，作用在齐次像素 [x,y,1]，方向 = t+1 帧 → t 帧
 */
[[nodiscard]] cv::Matx33d yawRemapHomography(double yawRadPerFrame, double pitchRad,
    double focalPx, double cxPx, double cyPx);

/// 把 3×3 射影映射展成 OpenCV remap 的两张 map，并统计最大位移。
[[nodiscard]] YawMap buildYawMap(const cv::Matx33d& H, cv::Size size);

/// 把 src 按 map 搬到目标网格（双线性；边界外填 0 并由 mask 排除）。
[[nodiscard]] cv::Mat remapWith(const cv::Mat& src, const YawMap& map);

}  // namespace RGL::imgqa
