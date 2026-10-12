#pragma once
// `imgqa diffblock` 的**口径层**（★0.5-b1；判据本体 → docs/mdi_plan.md §11.2，2026-10-12 拍板）。
// 与 `diff_struct_metric.h` 同一条缝：**判据的算子要能被合成靶自检**——本层不读图、不启引擎，
// 输入 = 内存里的线性亮度图，selftest 用合成形状把每条性质钉成闭式（实机数字只能证明
// 「与上次一样」，这里钉的是「算的就是它声称的那个式子」）。
//
// 判据（用户口径「UV 缝边缘有差异我能接受，整块不行」的形式化）：
//
//   M = { p | |luma(A) − luma(B)| ≥ τ }          luma = 线性域 Rec.709（prepareLinear 单点）
//   U = area( erode( M, 圆盘半径 r ) )            默认 r = 2
//   判据：U == 0    ⟺ 差异处处厚度 ≤ 2r 像素（「有内部」= 整块 = FAIL）
//
// 实现取 `U = #{ distanceTransform(M, DIST_L2, 3) > r }` 而非字面 erode：一次计算同时给出
// 「厚度分布」与 U，两者**天然同口径**（§11.2 要求同时输出 ①厚度分位 ②U(r) 曲线 ③连通域）。
// 代价 = 必须自检它与 `cv::erode(M, MORPH_ELLIPSE, 2r+1)` 的面积等价（`erodedAreaReference`，
// selftest 钉死）——否则「半径当直径」这类实现 bug 会静默改变判据语义，容差理由见
// kDiffBlockErodeTol 处。
//
// 边界口径（两条都不显然）：
//  · **τ 的单位 = 1 个 sRGB 量化步长**（kSrgbLumaStep），τ=0 特判为**严格 > 0**
//    （≥ 0 会把整幅图变成 M，判据恒红——「任一位不同」的正确形态是「有差」不是「差 ≥ 0」）。
//  · **图像边界**：distanceTransform 把图外当背景（贴边差异算得**更薄**），cv::erode 默认
//    把图外当前景（BORDER_CONSTANT + morphologyDefaultBorderValue，算得更厚）。判据取
//    distanceTransform 的读数；等价性自检的形状一律**不贴边**，两种约定在贴边处本就不同，
//    那不是 bug 是定义域。
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

namespace RGL::imgqa {

/// τ 的单位换算：1 个 sRGB 量化步长 → 线性亮度。**最小可量化亮度台阶**
/// = 0.0722（Rec.709 权重最小的 B 通道）× srgbDecode(1/255) = 0.0722 × (1/255)/12.92
/// （1/255 落在 sRGB 线性段）≈ 2.19e-5。与 `diff_struct_metric.h` 里「ε=1e-6 与字节不同
/// 逐位等量」论证用的是**同一个常数**——比较域单点不另开（selftest 钉这条台阶闭式）。
constexpr double kSrgbLumaStep = 0.0722 * (1.0 / 255.0 / 12.92);

/// 默认腐蚀半径（像素）。r=2 ⟺ 「差异必须处处不超过 4 像素厚」= 用户那句话的形式化。
constexpr int kDiffBlockDefaultRadius = 2;
/// 默认门槛 = 1 个 sRGB 量化步长（§11.2「τ 默认 = 1 个 sRGB 量化步长」）。
constexpr double kDiffBlockDefaultTau = 1.0;
/// 连通域清单默认打印条数（按面积降序，「人只看这几个框」）。
constexpr int kDiffBlockDefaultTop = 8;
/// U(r) 曲线的 r 档位（§11.2 输出②：「若 r=4 才归零，说明判据被稀释」——档位写死在此，
/// 别在打印侧现写一份）。用户 --r 不在此列时 U(--r) 另算一条，曲线本身不随 --r 变形。
constexpr std::array<int, 4> kDiffBlockRadiusCurve = {0, 1, 2, 4};
/// distanceTransform 的距离掩码尺寸（DIST_L2 + 3 = (1,√2) 近似欧氏，见 kDiffBlockErodeTol 处）。
constexpr int kDiffBlockDistMaskSize = 3;
/// erode 参考面积与 distanceTransform 面积的**相对**容差（selftest 用）。为什么允许容差：
///  ① `getStructuringElement(MORPH_ELLIPSE, 2r+1)` 的格点盘半径是 r+0.5（r=2 的 5×5 含
///    (±2,±1) 即 √5≈2.24 > 2 的偏移），不是精确的「距离 ≤ r」；
///  ② DIST_L2 + maskSize=3 是 (1,√2) 近似（实测直邻 d=0.96、二邻 1.91，系统性低估 ~4%）。
/// 两者在直边形状上逐位同面积，曲边（圆盘）上实测最大相对偏差 8.8%（disk r=12 @ r=4）。
/// 为什么 0.10 仍有牙：「半径当直径」（U=#{d>2r}）在 20×20 块 @ r=1 上差 21%、3px 线 @ r=1
/// 上差 100%；「≥ 当 >」在 3px 线 @ r=2 上差 100%——粗错全在容差外，被吸收的只有定义差。
constexpr double kDiffBlockErodeTol = 0.10;

/// 连通域读数（§11.2 输出③：area / bbox / 该域最大厚度，按面积降序）。
struct DiffBlockComponent {
    int64_t area = 0;
    int x = 0, y = 0, width = 0, height = 0;  ///< bbox（像素，OpenCV Rect 语义）
    double maxThickness = 0.0;                ///< 该域内 d 的最大值（distanceTransform 同口径）
};

/// 形态判据全部读数。M 为空（两图在 τ 下无差）时厚度分位**无定义**（thicknessValid=false，
/// 读 0 = 把「量具取不出数」读成「差异无限薄」，同 `diffStructStat` 的 NaN 纪律）。
struct DiffBlockStat {
    int width = 0, height = 0;
    int64_t total = 0;
    /// 字节域「任一位不同」的像素数（τ=0 报告值）；byteDiff 缺席 = **-1（未测）**，不是 0。
    int64_t anyDiffPixels = -1;
    int64_t mPixels = 0;  ///< |Δluma| ≥ τ·kSrgbLumaStep（τ=0 时严格 > 0）
    double thicknessP50 = 0.0, thicknessP90 = 0.0, thicknessP99 = 0.0, thicknessMax = 0.0;
    bool thicknessValid = false;  ///< M 非空才有分布
    /// U(r) 曲线：(r, U)，r 档位 = kDiffBlockRadiusCurve（升序）。U(0) 恒 = mPixels（自检锚）。
    std::vector<std::pair<int, int64_t>> uCurve;
    int radius = 0;
    int64_t uAtRadius = 0;  ///< 判据读数 U(--r)（--r 在曲线档位上时与曲线同值）
    int64_t componentCount = 0;  ///< M 的 8-连通域总数（截断前）
    std::vector<DiffBlockComponent> components;  ///< 面积降序前 top 个
};

/**
 * 核心统计：两幅线性亮度图 + 可选字节域差分掩码（`anyChannelDiff` 的产物，CV_8UC1；
 * 空 = τ=0 计数报「未测」-1）。
 *
 * 前提守卫（错位输入 = 最贵的假绿，同 `diffStructStat`）：两侧 luma 必须同尺寸 CV_64FC1；
 * byteDiff 给定时同尺寸 CV_8UC1；τ ≥ 0、r ≥ 0、top ≥ 0——不满足**抛**，不出数。
 */
[[nodiscard]] DiffBlockStat diffBlockStat(const cv::Mat& lumaA, const cv::Mat& lumaB,
    const cv::Mat& byteDiff, double tau, int radius, int top);

/**
 * 参考实现：`area(erode(mask, getStructuringElement(MORPH_ELLIPSE, 2r+1)))`——判据的
 * **字面定义**。只给 selftest 的等价性自检用（diff_block.cpp 的判据走 distanceTransform，
 * 理由见文件头）；形状贴图边时两种边界约定本就不同，自检形状勿贴边。
 */
[[nodiscard]] int64_t erodedAreaReference(const cv::Mat& mask, int r);

}  // namespace RGL::imgqa
