#pragma once
//
// temporal_flicker_metric.h —— `temporal-flicker` 判据的**跨 TU 共享层**（类型 + 闭式常量 + ROI 定位算子声明）
//
//   为什么拆：`temporal_flicker.cpp` 到 571 代码行（工具侧上限 500）被文件护栏逼出一刀。缝按
//   **仪器层次**切，与 `shadow_kernel_metric.*` / `diff_struct_metric.*` / `ao_noise_metric.*`
//   同一条缝 = 「口径 vs 仪器」：
//
//     · `temporal_flicker_metric.cpp` = **口径层**——闭式解（displayChain / flickerClosedForm /
//       scaleFromObserved）、谱诊断（hannHalfSpectrum）、帧差统计（frameDeltaPercentile）、
//       边缘定位（columnProfile / steepestEdgeColumn / edgeRoi / strongestRoi）与 measureGroup。
//       全部不碰引擎、不打印；前九个被 `just build-tools` 的 selftest（testTemporalFlicker）
//       用闭式直接测——改测量算子动这里，改阈值文案与采集流程不动这里。
//     · `temporal_flicker.cpp` = **仪器层**——采集（captureFlickerSeries，temporal-homography
//       共用）、命令（cmdTemporalFlicker）与判决线。文件头注释（含**被否决的 FFT 频带比对的
//       实测标定数据**）留在主 TU：`temporal_flicker.h` 按名字指它（「实测数据在
//       temporal_flicker.cpp 头注释」），别把那段搬来搬去。
//
//   类型与常量为什么住这里：`Group` 是两个 TU 之间**唯一**的数据通道（口径层的 measureGroup
//   填它、仪器层的 ROI 定位与读数表读它）；`kPi` 闭式谱（口径层）与 yaw↔px 换算（仪器层）
//   都要——两处各抄一份 = 第二份真相。判决阈值（边缘跳变 0.15 / 平区跨度 0.05 / 运动在场 2.0）
//   **不在**这里：它们只被仪器层的判决线消费一次，唯一出处就在判决线旁边。
//
//   ⚠ 本头**不是** temporal-flicker 的对外接口：对外声明（captureFlickerSeries / FlickerScale /
//   FlickerAnchor / cmdTemporalFlicker 等）全在 `temporal_flicker.h`（temporal_homography.cpp 与
//   selftest.cpp 消费）。这里声明的东西只给 temporal_flicker*.cpp 两个 TU 用，别的 TU 别接。
//
#include "temporal_flicker.h"

#include <opencv2/core.hpp>

#include <vector>

namespace RGL::imgqa {

/// 圆周率（单精度）。闭式谱与焦距换算（口径层）、偏航角换算（仪器层）共用，唯一出处。
inline constexpr float kPi = 3.14159265358979323846f;

/// 一组采集的产物 + 统计（口径层 measureGroup 填、仪器层判决线读；采集产物基类 FlickerSeries
/// 声明在 temporal_flicker.h —— 与 temporal-homography 共用同一个采集口）
struct Group: FlickerSeries {
    cv::Rect roi;
    double edgeX = 0.0;
    double sweepLocal = 0.0;
    double rho = 0.0;                ///< 实测每帧变化（p99.5，线性）
    double darkObs = 0.0, whiteObs = 0.0;
    FlickerScale scale;
    FlickerAnchor anchor;
    std::vector<float> signal;       ///< ROI 均值时间序列（线性观测域）
    std::vector<float> spectrum;     ///< Hann 半谱幅度
};

// ─────────────── 口径层算子（定义在 `temporal_flicker_metric.cpp`）───────────────
//
// 闭式解与谱/帧差/亚像素定位的公开声明在 `temporal_flicker.h`（selftest 直接吃它们），
// 本头只补**跨 TU 但非对外**的三条。

/// 把 ROI 钉到边缘上：在**期望位置 ±16px** 的窗内取列剖面最陡处（全图 argmax 会被白板上的
/// 黑杆抢走——TAA 开时边缘被时域累积抹平，最陡处反而是阶梯杆，实测踩过），并返回该处的
/// 列剖面跳变量（阳性锚点：跳变量 ≈0 = 根本没找到边缘）。
/// 口径（债-98）：`halfWidth` 在这里是**半宽**语义 —— ROI 宽 = `2 × halfWidth`。
cv::Rect edgeRoi(const std::vector<cv::Mat>& luma, double expectedX, double& edgeX,
    double& edgeStep, double halfWidth, double rowFrac);

/**
 * 画面里**最强梯度**处的方形窗（bistro 模式：闭式不适用，只出同口径的 ρ 做 on/off 对比）。
 * 为什么不能钉画面中心：中心可能是块平墙，ρ 恒 0 而 on/off 比是 0/0；窗口必须落在边缘带上。
 * 定位用 TAA 关那一组（无抖动），两组共用同一个窗。
 *
 * 🔴 口径（债-98）：`fullWidth` 在这里是**全宽**语义 —— ROI = `fullWidth × fullWidth`，
 * CLI 的 `--halfwidth` 传到这里当全宽用（默认 8 → ROI 8×8，而合成靶分支同参数是 16×200）。
 * **两个分支几何不统一是拍板不是疏忽**：这是已发表读数的口径（bistro 组 ρ 的窗口大小），
 * 把它改成 `2 × halfwidth` = 换仪器、读数全部要重标。修法 = 把语义写进名字与打印，
 * 让读者不必记住差异（表头行见 `temporal_flicker.cpp` 的「ROI 全宽」）。
 */
cv::Rect strongestRoi(const std::vector<cv::Mat>& luma, double fullWidth, cv::Point2d& at);

/// ROI 内的两端观测电平（p0.5 / p99.5）+ 时间序列 + 半谱
void measureGroup(Group& g);

}  // namespace RGL::imgqa
