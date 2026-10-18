#pragma once
//
// shadow_kernel_metric.h — `shadow-kernel-scale` 的**度量口径层**（投影复刻 + 扫描取样 + 三条算子）
//
//   为什么拆这一刀：`shadow_kernel_scale.cpp`（仪器：三臂采集 + 读数表 + 三条判决线）与
//   `shadow_kernel_metric.cpp`（口径：把「一张截图」变成「一条世界米制的 S 剖面」再变成宽度）
//   不是同一条改动缝——改阈值/改臂定义动前者，改测量算子动后者，而**只有后者被
//   `just imgqa selftest` 用闭式直接测**（不启引擎）。文件代码行护栏（500）把这条本来就存在的
//   缝逼了出来。
//
//   常数与类型为什么住这里：`kUmbraFactor` / `kCrossLo` / `kCrossHi` 既进算子（判「这条样本算
//   不算交叉」）又进仪器（打印与判决线文案），`ScanLine` 由仪器从 manifest 填、由口径消费。
//   两处各写一份 = 第二份真相，改一处忘另一处时**读数与解释会分家**（判据绿着说一件没发生的事）。
//
#include "imgqa.h"

#include <opencv2/core.hpp>

#include <array>
#include <limits>
#include <vector>

namespace RGL::imgqa {

/// 「取不到」的表示。⚠ 必须是 NaN 不能是 0：0 = 「影缘无限硬」，而缺陷态（影根本没画出来）
/// 想伪装成的恰恰是它。打印层把 NaN 打成 `—`（见仪器侧 `fixed2`）。
inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// 参照 luma 低于 1 LSB ⟹ S = (L_none − L_arm)/L_none 的分母是黑的，S 无定义。
/// 这条下限是判据 1 最要紧的反向：env 关时**背景就是黑的**（skybox 标志已随刀②.2 删除），没有下限会把整屏
/// 算成「有影」= 缺陷态刷绿。
inline constexpr double kLumaUndefined = 1.0;

/// 判据 1：S > 0.5 记「这里有影」（严格 >，恰 0.5 不计）。
inline constexpr double kUmbraFactor = 0.5;

/// 半影下沿 / 上沿交叉（宽度 = 两者之差的绝对值，单位 = 世界米）。
inline constexpr double kCrossLo = 0.10;
inline constexpr double kCrossHi = 0.90;

/**
 * 沿 v（平行于影缘）铺开的取样线偏移，单位 = 世界米。
 *
 * 为什么必须有它（实测，别删）：PCSS 的 blocker 搜索让 S 剖面带**像素尺度**的噪声，而
 * 「第一个穿阈」的交叉点会被**单个样本**的噪声尖峰决定。单线取样实测：同一批截图，只把
 * 扫描步长从 0.04 m 改成 0.05 m，pcss5 的 W(64) 就在 4.86 ↔ 3.55 m 之间跳（−27%），
 * 判据 3 的比值随之从 2.06 掉到 1.50（阈值 1.60）——**换采样网格等于换判据结论**。
 * 影缘沿 v 是直的、地面是平的 ⟹ S 只随 s 变，沿 v 平均是**无偏**的去噪（不像沿 s 平滑会
 * 把缘本身摊宽）。5 线 ±2 m 下同一批图在 0.04/0.05/0.06 三种步长上给出**逐位相同**的 8 个
 * W，且与已验证的 Python 参考实现（单线 0.04 m）八个值全部落在 ±25% 内。
 * ⚠ ±2 m 的上限来自靶：板边长 16 m ⟹ 影在 v 方向只有 ±8 m，再远就扫到影外/邻影。
 */
inline constexpr std::array<double, 5> kVLineOffsets{0.0, 1.0, -1.0, 2.0, -2.0};

/// 竖直 fov 的针孔相机（aspect 只压横轴）——与已验证的 Python 参考实现逐式同。
/// 画幅**跟着图走**（cols/rows 由调用方从截图取），不在口径层写死 1280×720：
/// 采集尺寸是仪器侧的臂定义，写死在这里 = 采集改了而投影还在按旧画幅解像素。
struct Projector {
    std::array<double, 3> pos{}, f{}, r{}, up{};
    double tanHalf = 0.0;
    double aspect = 1.0;
    int cols = 0;
    int rows = 0;
};

Projector makeProjector(const Engine::Camera& cam, int cols, int rows);

/// 地面 y=0 上的 (x,z) → 像素。投到相机背后或屏外 = false（该样本作废，不夹到边缘）。
bool projectTo(const Projector& p, double x, double z, double& px, double& py);

/// 双线性取字节域 luma（`byteLuma` 的输出面）。投出屏 / 贴边取不到 2×2 = false。
bool sampleLuma(const cv::Mat& luma, const Projector& p, double x, double z, double& out);

/// 一条扫描线（世界米参数化）：origin = 影心，[lo, hi] = 沿 u 的扫描窗，v = 影缘方向。
struct ScanLine {
    std::array<double, 2> origin{};
    std::array<double, 2> u{};
    std::array<double, 2> v{};
    double lo = 0.0;
    double hi = 0.0;
    double step = 0.0;
};

/// 扫描线上 s 米处的一点（沿 v 的 5 条线平均后的 S）。见 `kVLineOffsets`。
ShadowScanSample shadowScanPoint(const cv::Mat& refLuma, const cv::Mat& armLuma,
    const Projector& proj, const ScanLine& line, double s);

/// 沿 [lo, hi] 以 step 米步长扫出剖面（s 升序）。
std::vector<ShadowScanSample> scanShadowProfile(const cv::Mat& refLuma, const cv::Mat& armLuma,
    const Projector& proj, const ScanLine& line);

// 剖面 → 读数的三个算子（shadowCrossing / shadowPenumbraWidth / shadowUmbraFraction）
// 声明在 `imgqa.h`（selftest 直接吃它们，本头不重复声明）。

}  // namespace RGL::imgqa
