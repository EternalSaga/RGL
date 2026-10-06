#pragma once
//
// temporal_homography.h — 精确单应 warp 的逐像素闪烁归因图（判据编号 temporal-homography）
//
//   相机做**原地绕 up 轴 yaw**（camera.lua `frame_yaw`，位置不动只转方向）⟹ 像面映射是
//   **纯旋转单应**，与深度无关、对整幅画面**精确**——不需要 velocity buffer、不需要对齐搜索、
//   不需要参考图。于是「上一帧按精确单应搬到本帧网格」与「本帧」之差 = **搬不动的那部分**：
//   采样格移动造成的锯齿差 + TAA 时域滤波做的事。逐像素画出来就是闪烁归因图。
//
//   闭式单应（推导见 .cpp）：t = tan(Δψ)，f = 垂直 FOV 像素焦距，(cx,cy) 主点
//     x' = [(f − cx·t)·x + t(cx² + f²)] / [(−t)·x + (f + t·cx)]
//     y' = [−cy·t·x + f·y + cy·cx·t] / [(−t)·x + (f + t·cx)]
//   垂直线仍映到垂直线（yaw 绕竖轴），y' 的分母与 x 耦合 = 透视收缩，不是仿射。
//
//   ⚠ warp 本身要插值（双线性），插值误差混在残差里。故同批出**往返地板**
//   |I − H⁻¹H·I| 的 p99.5：残差与地板同量级 = 测的是量具，不是闪烁。
//
#include "imgqa.h"

#include <opencv2/core.hpp>

#include <string>

namespace RGL::imgqa {

/// 原地 yaw 的闭式单应（帧 k → 帧 k+1，Δψ = yawRadPerFrame）。返回 3×3（作用在齐次像素 [x,y,1]）。
[[nodiscard]] cv::Matx33d yawHomography(double yawRadPerFrame, double focalPx, double cxPx, double cyPx);

/// 相机移动下的逐像素闪烁归因：精确单应 warp 上一帧，残差出热图 + 统计（TAA 开/关各一组）。
/// @param outPrefix 非空 = 写 `<prefix>_taa_{on,off}_heat.png`（固定标尺，两组可对眼）
/// @param bands     true = 残差图另出「亮度 × 梯度」二维分带（**机位状态 = 相机移动**）。
///                  既有四条读数与 rc 一律不动；判决线跑前写死，见 .cpp 内同名段。
/**
 * @param bands    true = 出「亮度 × 梯度」六格表 **+ 材质归因表**，并启用可测性守卫闸门
 *                 （守卫红 ⟹ 只报 Indeterminate、**rc≠0**，不出判决）
 * @param envIntensity / aoStrength  A/B 侧（负数 = 不覆写 = 交付配置原样）。
 *        `env.intensity=0` 关几何体 IBL、`ao.strength=0` 关 AO，两者都是 IEEE 精确旁路。
 *        ⚠ `env.intensity=0` **不影响天穹背景**（skybox 与 light 的 envSky 分支不乘它），
 *          所以背景会原样亮着 —— 输出里必须回显这条，否则读者会把"背景还在"读成"IBL 没关掉"。
 */
int cmdTemporalHomography(const std::string& target, double sweepPxPerFrame, int frames,
    const std::string& outPrefix, bool bands = false, double envIntensity = -1.0,
    double aoStrength = -1.0);

}  // namespace RGL::imgqa
