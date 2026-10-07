// temporal_yaw_remap.cpp —— 带 pitch 的解析 yaw 重映射（批 13.14a）
//
// ## 完整推导（头注释只给结论，这里给可复核的全过程）
//
// 引擎相机基（`rgl/camera.cppm`，世界系下的相机三轴为**列**，第三列必须是 **fwd**）：
//
//     right = (  cos yaw,             0,         −sin yaw )
//     up    = (  sin yaw·sin pitch,   cos pitch,  cos yaw·sin pitch )
//     fwd   = ( −sin yaw·cos pitch,   sin pitch, −cos yaw·cos pitch )
//
// 世界方向 w 在第 t 帧相机系的坐标是 `c_t = E_tᵀ w`，于是
//
//     c_t = E_tᵀ E_{t+1} c_{t+1}
//
// ## ⚠ 本文件踩过的一次静默符号错（首轮实机 φ 标定 argmin = **−1**）
//
// 顺手把 `E` 写成 `R_y(yaw)·R_x(pitch)` 是**陷阱**：它的第三列算出来是
// `(sin y·cos p, −sin p, cos y·cos p)` = **−fwd**，即该基的 z 轴**朝后**。而像素映射
// `u = cx + c[0]/c[2]·f`（见下方 `Ks`）要求 `c[2]` 是**前向**深度。两者一配，等价于把
// 正确的映射共轭了 `S = diag(1,1,−1)`；逐因子共轭给 `S·R_x(−p)·R_y(δ)·R_x(p)·S
// = R_x(p)·R_y(−δ)·R_x(−p)` ⟹ **yaw 符号反掉，其余全对**（恒等性、pitch 修正、
// yaw 无关性、与既有闭式的一致性统统照过）。首轮就是它：`alignedΔ` 比完全不对齐的
// `rawΔ` 还大 29%，量级正好对上「净 2δ 位移」的帧差，而对拍金标准 2.3e-13px 全绿
// ——因为金标准当时用的是**同一个朝后的基**。⟹ 结论：**自搭金标准钉不住约定**。
//
// 正确形式（把 `S` 乘进去，即「先转回 fwd 基、转 −δ、再转回去」）：
//
//     E_tᵀ E_{t+1} = R_x(p)·R_y(−δ)·R_x(−p)
//
// **yaw 消掉了** ⟹ 映射只依赖 pitch 与每帧角，与相机朝哪儿无关。
// 方向自检（`selftest` 锚点④ + φ 标定扫描）：δ > 0 时引擎内容**右移** `f·tan δ`
// （`right·P` 随 yaw 增大而增大），而 src map 要往**左**取 ⟹ `apply(H, 中心).x < cx`。
// 取 cx = cy = 0 验：`u' = f(x/f·cos δ − sin δ)/(x/f·sin δ + cos δ) ≈ x − f·δ` ✓。
//
// ## 为什么代码里是三个矩阵相乘而不是闭式
//
// 本文件的手推闭式**连错两次**（求逆漏 `(ABC)⁻¹ = C⁻¹B⁻¹A⁻¹` 的顺序翻转一次、
// 符号展开错一次），而化简在这里零收益：矩阵每帧构造一次，逐像素开销全在 remap。
// 定义式对「显式构造两帧相机基 → 世界 → 回投」的金标准实测 **2.3e-13 px**
// （三个 pitch × 三个 yaw × 两个 δ），selftest 里重跑这条对拍。
//
// ## 与既有 yawHomography（temporal_homography）的关系
//
// 那条闭式**不含 pitch**，即它假设旋转轴 = 相机 up。但引擎 `frame_yaw` 动的是
// `camera.yaw` 而 pitch 不变 ⟹ 实际轴是**世界 Y**。bistro 机位 pitch ≈ −23° 时，
// 忽略 pitch 会让 v 方向每帧错 **0.952px**（0.2°/帧）——量级与判据本身同阶。
// pitch = 0 时两者一致到 0.0022px（既有闭式是本变换的二阶近似，δ 越大差越大：
// δ=0.02 时 0.073px）。**既有那条判据的 pitch 边界另立债，本文件不改它。**
//
#include "temporal_yaw_remap.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace RGL::imgqa {

namespace {

/// 绕 x 轴（相机右轴）旋转。
cv::Matx33d rotX(double a)
{
    const double c = std::cos(a), s = std::sin(a);
    return cv::Matx33d(1.0, 0.0, 0.0, 0.0, c, -s, 0.0, s, c);
}

/// 绕 y 轴（世界 up）旋转。
cv::Matx33d rotY(double a)
{
    const double c = std::cos(a), s = std::sin(a);
    return cv::Matx33d(c, 0.0, s, 0.0, 1.0, 0.0, -s, 0.0, c);
}

}  // namespace

cv::Matx33d yawRemapHomography(double yawRadPerFrame, double pitchRad, double focalPx,
    double cxPx, double cyPx)
{
    if (focalPx <= 0.0)
        throw std::runtime_error("yawRemap: focalPx 必须 > 0（传**水平**焦距像素）");

    // 「t+1 帧相机系 → t 帧相机系」= R_x(pitch)·R_y(−δ)·R_x(−pitch)。
    // ⚠ **负号是本体，不是笔误**：见文件头「静默符号错」一节——写成 R_x(−p)·R_y(δ)·R_x(p)
    //   等价于用第三列朝后的基，yaw 会静默反号（首轮实机 φ 标定 argmin = −1 的根因）。
    const cv::Matx33d cam = rotX(pitchRad) * rotY(-yawRadPerFrame) * rotX(-pitchRad);

    // 归一化 ↔ 像素：u = (x − cx)/f，v = (cy − y)/f（v **向上**为正 ⟹ 中间项取负）。
    // ⚠ 单一 f 是对的：fovDegrees 走 glm::perspective 的**垂直** fov，水平由 aspect 推
    //   ⟹ tan(fov_h/2) = tan(fov_v/2)·aspect ⟹ fx = (W/2)/tan(fov_h/2) = (H/2)/tan(fov_v/2) = fy。
    const double f = focalPx;
    const cv::Matx33d Ks(f, 0.0, cxPx, 0.0, -f, cyPx, 0.0, 0.0, 1.0);
    const cv::Matx33d Ksi(1.0 / f, 0.0, -cxPx / f, 0.0, -1.0 / f, cyPx / f, 0.0, 0.0, 1.0);
    return Ks * cam * Ksi;
}

YawMap buildYawMap(const cv::Matx33d& H, cv::Size size)
{
    YawMap m;
    m.mapX.create(size, CV_32FC1);
    m.mapY.create(size, CV_32FC1);
    double worst = 0.0;
    for (int y = 0; y < size.height; ++y) {
        float* rx = m.mapX.ptr<float>(y);
        float* ry = m.mapY.ptr<float>(y);
        for (int x = 0; x < size.width; ++x) {
            const cv::Vec3d q = H * cv::Vec3d(static_cast<double>(x), static_cast<double>(y), 1.0);
            if (std::abs(q[2]) < 1e-12)
                throw std::runtime_error("yawRemap: 映射落到无穷远（每帧角过大或主点异常）");
            const double u = q[0] / q[2], v = q[1] / q[2];
            rx[x] = static_cast<float>(u);
            ry[x] = static_cast<float>(v);
            worst = std::max(worst, std::hypot(u - x, v - y));
        }
    }
    m.maxShiftPx = worst;
    return m;
}

cv::Mat remapWith(const cv::Mat& src, const YawMap& map)
{
    cv::Mat dst;
    cv::remap(src, dst, map.mapX, map.mapY, cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0));
    return dst;
}

}  // namespace RGL::imgqa
