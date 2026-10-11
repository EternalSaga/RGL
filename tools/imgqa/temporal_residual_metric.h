#pragma once
//
// temporal_residual_metric.h —— `temporal-residual` 家族的**跨 TU 共享层**（类型 + 阈值 + 折叠算子）
//
//   为什么拆：`temporal_residual.cpp` 到 1908 代码行（工具侧上限 500）被文件护栏逼出刀。那个文件里
//   其实住着**六台仪器**，缝按仪器切（每台一个 TU；判据口径仍以本头的阈值注释与
//   `temporal_residual.h` 为准，别在 TU 里另抄一份）：
//
//     · 纯度量算子（`bandTableOf` / `swingFoldCore` / `rejectCrossTab` …，selftest 直接打得到）
//       → `temporal_residual_metric.cpp`。
//     · 合成靶采集（aa_target 五组 R0–R4 + 批 13.7 长短窗对照 B1–B4）→ `temporal_residual_target.cpp`。
//     · 合成靶 7 判据路径（验参 → 出表 → 判据 → 台账）→ `temporal_residual_target_verdicts.cpp`。
//     · bistro 真实场景采集原语 + 批 13.10 历史拒收交叉表 → `temporal_residual_bistro.cpp`。
//     · 批 13.7c 静止态「亮度 × 梯度」二维分带出表 → `temporal_residual_bands.cpp`。
//     · 一机位六组的编排（守卫 / 热图 / 出表顺序 = 契约）→ `temporal_residual_bistro_camera.cpp`。
//     · 批 13.11 裁剪消融 A/B 臂与两个 sweep → `temporal_residual_bistro_abl.cpp`。
//     · 批 13.9 移动机位分带（两把尺子共用的出表与判决）→ `temporal_residual_moving_bands.cpp`。
//     · 命令入口与变体分岔留在 `temporal_residual.cpp`。
//
//   为什么这些东西必须离开原 TU：`Run` / `BistroRun` / `BistroReject` / `RejectReport` / `BandReport` 是
//   采集层与判据层之间**唯一**的数据通道（采集层填、判据层读）；`swingFoldCore` 是模板（泛型累加器
//   `At&&`），模板**只能**放头里——改成 explicit instantiation 要枚举全部 lambda 类型，等于把「折叠
//   口径只有一份」这条红线换成两份真相。阈值常量整块（含实测表与变异记录注释）在这里 = 唯一出处；
//   `inline constexpr` 而不是裸 `constexpr`：namespace 作用域的 `constexpr` 变量是**内部链接**，头里
//   定义 + N 个 TU 各 include 一次 = 每个不消费它的 TU 一条 `-Wunused-const-variable`
//   （先例 = `aa_coverage.h`）。
//
//   ⚠ **本头只准 `temporal_residual*.cpp` include**：`d6` / `d2` 在本头是 `RGL::imgqa` 作用域的 `inline`
//   函数，而 `temporal_swim.h` 在同作用域有**同名同签名的另一份**（体也逐字相同）。同一个 TU 同时
//   include 两者 = 编译期重定义报错（响的，不是静默换口径）。`selftest.h` 会传递包含
//   `temporal_swim.h`，故 `selftest_*.cpp` 也别接本头。格式化件同样**不进公共头** `temporal_residual.h`：
//   `temporal_homography*.cpp` / `temporal_motion.cpp` 各自在匿名 namespace 里有 `d2` / `d4`，无名空间
//   的可见性提升会把它们摆到调用点上造成歧义（同向裁决的先例 = `temporal_homography_metric.h` 头注释）。
//
#include "temporal_residual.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace RGL::imgqa {

/// 采集分辨率（720p）。⚠ 残差口径按它钉死（守卫在 `runBistroGroup` 里点名），换分辨率 = 换口径。
inline constexpr int kSize[2] = {1280, 720};

// ── 阈值（**先测后定**，2026-10-05 首轮实测；每条理由同步进 judge.cpp 注册表）─────────────
//
// 实测表（aa_target 1280×720，periods=2，warm-multiplier=6，variant=forward；单位 = 线性 luma；
// 斜体组 = 同一次运行内的参照。两轮独立运行差 ≤0.3%，故**最小余量取 3%**，见头注释「可复现性」）：
//
//   组（meanSwing / frac(swing>τ)）    门关 = 交付默认          门开 = --base-space
//     R1 fb=1  js=1（阳性锚点）         0.021982 /  3.88%        0.023909 / 13.04%
//     R2 fb=.12 js=1（现行工作点）      0.022600 / 18.09%        0.004459 / 12.09%
//     R4 fb=.002 js=1（分辨力轴）       0.015783 / 15.12%        0.000218 /  0.22%
//     R0 TAA 关、R3 js=0               两条**恰为 0**（无容差，不进表）
//   梯度能量比 R2：门关 0.887–0.893 / 门开 0.966–0.968；显示空间同口径 = 0.768–0.771（红线 1 的
//   代价，实测非引用）
//   「完全不平均的单帧」参照：门关 1.031–1.083 / 门开 1.004。⚠ 它取连拍**末帧**，末帧的抖动相位
//   随 burst 起点浮动 ⟹ 5 次运行跨 5%（上面那句「≤0.3%」只对被判的四个数成立，不成立在这个参照上）
//   —— 这正是它只打印不判的原因，也是 ⑦ 的上限只能取 1.02（门关退化下沿 1.031 之下）的原因。
//   τ = 0.006732（白电平 0.617207 = byte 206 处 1 个显示 LSB）；mask = 246045 / 245901 px
//
// 阈值的取舍纪律：**能用闭式就不用实测数**。锚点下限写成 τ 的倍数（τ 由场景白电平现算，换靶
// 自动跟着走），只有「工作点残差落在哪个带」这种没有闭式可推的量才直接写实测数 + 余量。
inline constexpr double kAnchorMeanSwingTau = 1.0; ///< R1 meanSwing 下限 = 1 个显示 LSB@白电平
inline constexpr double kAnchorFracFloor = 0.01;   ///< R1 frac(swing>τ) 下限（实测 3.88%/13.04%）
/// R2 工作点**信号在场带**（不是可见性线，见 ⑤ 的容差理由）。门关实测 0.022600：
/// 下限 0.010 = 实测的 44%（信号消失才算红），上限 0.030 = 实测 +33%（回归才算红）。
inline constexpr double kMagnitudeMeanLo = 0.010, kMagnitudeMeanHi = 0.030;
/// 门开态（`taa.base_space`）整幅游动被消掉，残差只剩闪烁 ⟹ 读数低 5.1×，带跟着下移。
/// 实测 0.004459：带 [0.002, 0.010] = 实测的 45% / 2.2×。
inline constexpr double kMagnitudeMeanLoBase = 0.002, kMagnitudeMeanHiBase = 0.010;
inline constexpr double kMagnitudeFracCap = 0.30; ///< R2 frac 上限（实测 18.09%/12.09%）
inline constexpr double kMonotoneCap = 0.90;      ///< R4/R2 meanSwing 比上限（实测 0.698/0.049）
inline constexpr double kGradRatioLo = 0.85;      ///< 收敛正确性下界（实测 0.8925/0.9683）
inline constexpr double kGradRatioHi = 1.02;      ///< 上界（门关退化态 1.047；门开退化态 1.004 → 见理由）

// ── 变异记录（2026-10-05，每条判据至少一条变异能打红；「测试通过」≠「测试有效」）─────────
//
//   变异（临时改本文件，跑完还原）              红掉的判据                        仍然绿的判据
//   M1 折叠配对 `p+k·16` → `p+k`（连续帧）      selftest 3 条（swing 1→0.75）      —
//   M2 中位数 → 最小值                          selftest 2 条（0.75→1.0）          —
//   M3 `prepareLinear` 不做 sRGB 解码（红线 1）  ⑦ 收敛 0.769 < 0.85               ①②③④⑤
//   M4 `phaseSwingStat` 忽略 mask               selftest 2 条                      —
//   M5 半间差容差去掉量化项                     ② 折叠 0.0066 > 0.00017            其余全绿
//   M6 抖动周期 16 → 8                          ② 折叠（R1 半间差 0 → **0.617**）  ③④⑤⑦ **全绿**
//   M7 锚点组 R1 的 jitter_scale 1 → 0          ③ 锚点（meanSwing 恰为 0）+ ⑦     ①②④⑤
//   M8 反向组 R3 的 jitter_scale 0 → 1          ④ 反向（0.0226 ≠ 0）               其余全绿
//   M9 工作点 R2 的 fb 0.12 → 0.002             ⑥ 单调（比值 1.001）+ ⑦ 0.668      ①②③④⑤
//   M10 区域外扩 3px → 内缩 3px                 **无**（见下面那条诚实边界）        全部
//   M11 区域退回全图（丢掉 objectID）           ③ 锚点 0.0059 + ⑤ 低于下限          ①②④⑦
//
// 三条读法：
//  · **M6 只有 ② 抓得到**（③④⑤⑦ 全绿）——折叠前提这条判据不是装饰，它是本命令唯一的
//    「连拍还是不是抖动相位的确定性函数」的守卫。
//  · **M11 只有 ③⑤ 的下限抓得到**（相对口径 ②④⑦ 全绿）——这就是「下限必须有」的实证：
//    区域被背景稀释时，所有比值都还好看。⚠ 也正因如此，⑤ 的下限**不是**质量线（M9 那种
//    「真的变好了」的改动它不红，M11 这种「量具坏了」它才红）。
//  · ⚠ **诚实边界（M10）**：区域口径本身**没有**判据——外扩改内缩 3px 后读数 0.0226 → 0.0168
//    仍在带内，全绿。本命令只守「区域为空 / 画面全黑」（守卫会抛）与「区域与像素错位」
//    （① 确定性 + ③ 锚点），不守「外扩半径是不是 3」。改半径 = 改口径 = 本文件的实测表与
//    七个阈值一并作废重写，不是调参。


inline std::string d6(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}
inline std::string d4(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}
inline std::string d2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}
/// 科学计数（守卫行用：`== 0` 判据的"实际是多少"必须在 6 位小数下显示为 0 时仍可判读）
inline std::string dExp(double v)
{
    char b[24];
    std::snprintf(b, sizeof b, "%.3e", v);
    return b;
}

struct Run {
    std::string name;
    bool taa = false;
    double feedback = 0.0, jitterScale = 0.0;
    int warm = 0;
    ResidualStat stat;
    cv::Mat avg;       ///< 相位平均图（线性 luma）——收敛正确性判据的输入
    double grad = 0.0; ///< 相位平均图的梯度能量
    double byteGrad = 0.0; ///< **显示空间**同口径的梯度能量（只打印：红线 1 的反面教材，实测非引用）
    double gradSingle = 0.0; ///< 收敛段**单帧**的梯度能量（只打印：= 「完全不平均」的参照）
    double whiteLuma = 0.0;  ///< mask 内 luma 的 p99 = 靶白电平（τ 从它来）
    double maxLuma = 0.0;    ///< mask 内 luma 的**最大**（半间差量化项从它来，见 maxLsbStepUpTo）
    double quant = 0.0;      ///< 不亮于 maxLuma 的像素翻一格的最大真实跨度 = 半间差的量化项
    /// **只报不判**的显示空间同口径读数（8bit 灰，同一份折叠代码）。留它的唯一理由：
    /// 红线 1 说统计必须在线性域做，而"显示域差多少"是排查者与历史读数（phase13 §1.5 的
    /// 梯度比 0.671 等）通用的单位。两个域同时打印，混淆当场可见，不用靠注释提醒。
    double byteMeanSwing = 0.0, byteFrac = 0.0, byteMax = 0.0, byteP999 = 0.0;
    double byteMaxVsFirst = 0.0; ///< 与第 1 帧的逐字节最大偏离（确定性地板）
    double changedVsFirst = 0.0; ///< 与第 1 帧任一通道不同的像素占比（跨帧取最大）
};

struct BistroRun {
    std::string name;
    int warm = 0;
    double alpha = 0.0;
    ResidualStat stat;
    cv::Mat avg;
    cv::Mat swingMap;  ///< 逐像素 swing（CV_64FC1）——热图与分带表的输入，与 stat 同一份折叠代码写出
    cv::Mat byteMean;  ///< **显示域**字节亮度多帧均值（CV_64FC1，0..255）= 亮度轴的尺子
    cv::Mat byteMax;   ///< 同上取跨帧**最大**（敏感性组：均值把「正在闪的削顶」判出档外的对照）
    cv::Mat gradLin;   ///< 线性 luma 相位平均的梯度图 = 梯度轴的尺子（主口径）
    cv::Mat gradByte;  ///< 显示域梯度图（敏感性组：对「高亮边」更保守的一把尺子）
    double grad = 0.0;      ///< 相位平均图的线性域梯度能量（债-47 的分子/分母）
    double byteGrad = 0.0;  ///< **显示空间**同口径（只打印：§1.5 的 0.671 是这类读数）
    double whiteLuma = 0.0, maxLuma = 0.0, quant = 0.0;
    double byteMaxVsFirst = 0.0, changedVsFirst = 0.0;
    int64_t changedPx = 0;  ///< 与首帧任一通道不同的像素数（跨帧取最大）——1LSB 级失效要**精确数**
    cv::Size frameSize;
};

struct BistroReject {
    std::string name;
    int frames = 0;
    RejectFraction frac{};  ///< 五桶占比（帧均）
    cv::Mat nAny;           ///< 每像素「历史被动过（≠收下）」的帧数（CV_32SC1）
    cv::Mat nCollapse;      ///< 每像素「塌回邻域盒 centroid = 拒收」的帧数
};

/// 出表后的关键读数（进台账，事后复核判决用）。
struct RejectReport {
    bool ok = false;
    double r0 = 0.0, rAll = 0.0, m3p = 0.0, bin0Share = 0.0;
    int branch = 2;  ///< 0 支持 / 1 否证 / 2 分不出
    double collapsed = 0.0, clipped = 0.0;
    // 抑制保真度（闭式锚定主判据；只在长窗档那次算）
    bool fidelityRan = false, fidelityPass = false;
    double rho = 0.0, rhoStar = 0.0, line = 0.0, bin0Rho = 0.0;
};

/// 分带报告进台账的关键读数（判决线只有一条，其余是复核用的原始数）。
struct BandReport {
    std::string verdict = "未出";
    double rA = 0.0;       ///< A 格（中间调×高梯度）的 rate 被压倍数 on/off
    double rB = 0.0;       ///< B 格（近削顶×高梯度）同上
    double bMassOff = 0.0, bMassOn = 0.0;  ///< B 格残余质量占比（短窗 / 长窗）
    double tHi = 0.0;      ///< 实际用的近削顶字节下界（闭式给出）
    double gradThr = 0.0;  ///< 实际用的梯度 p90 阈值
    double shareDrift = 0.0;  ///< 两态带边界漂移（**须恰为 0**）
};

/// 折叠 + swing 的**唯一实现**（线性 luma 判据与显示空间报告共用同一份代码，杜绝两套口径漂移）。
/// `at(i, y, x)` 取第 i 帧在 (y,x) 的值并折成 double。
struct SwingCore {
    double mean = 0.0, p999 = 0.0, mx = 0.0, fracOver = 0.0;
    double halfGap = 0.0, halfGapMax = 0.0, meanValue = 0.0;
    /// 「同相位两半差 > τ」的**像素数**。⚠ 只有 `halfGapMax` 是不够的：max 分不出
    /// 「1 个像素抖 0.9」与「1 万个像素抖 0.9」，而这两者的结论完全相反（一个是 PNG 平局翻转
    /// 噪声地板，一个是真机制）。2026-10-06 实测踩过：臂 7 的半间差max 冲到 0.91，没有计数就只能猜。
    int64_t halfGapOverPx = 0;
    int64_t pixels = 0;
};

/// 折叠统计的逐像素累加缓冲（`swingFoldCore` 的内层状态，收尾时折成 `SwingCore`）。
/// ⚠ `halfSum` 的累加顺序 = 浮点求和序契约（见 `foldPixelColumns`），别改成「每像素求部分和再累回」。
struct SwingFoldAccum {
    std::vector<double> swings;  ///< 掩膜内逐像素 swing（p99.9 要在全图排序后取，不能在像素内取）
    double sumSwing = 0.0, sumVal = 0.0, halfSum = 0.0;
    double halfGapMax = 0.0, mx = 0.0;
    int64_t overTau = 0, halfGapOverPx = 0;
};

/// 单像素 16 相位折叠的产物。
struct FoldedPixel {
    double colSum = 0.0;   ///< 16 个合成值之和（本像素均值 = colSum / kResidualPeriod）
    double pxGapMax = 0.0; ///< 本像素跨 16 相位的最大「两半同相位差」
    double swing = 0.0;    ///< max_over_16 − median_over_16
};

/**
 * 一个像素：两半各 `perHalf` 帧求均 → 合成 16 个相位值 → 取 swing。
 *
 * ⚠ `halfSum` 与 `gapMax` **按引用累加**：`halfSum += g` 的逐相位顺序是浮点求和序的一部分，
 *   在本函数内先求部分和再累回宿主会换加法结合序 ⟹ 读数末位漂（逐位截图回归当场红）。
 */
template <class At>
FoldedPixel foldPixelColumns(int periods, At&& at, int y, int x, double& halfSum, double& gapMax)
{
    const int perHalf = periods / 2;
    std::array<double, kResidualPeriod> v0{}, v1{}, v{};
    FoldedPixel f;
    for (int p = 0; p < kResidualPeriod; ++p) {
        double a = 0.0, b = 0.0;
        for (int k = 0; k < perHalf; ++k) {
            a += at(static_cast<size_t>(p + k * kResidualPeriod), y, x);
            b += at(static_cast<size_t>(p + (perHalf + k) * kResidualPeriod), y, x);
        }
        v0[p] = a / perHalf;
        v1[p] = b / perHalf;
        v[p] = 0.5 * (v0[p] + v1[p]);
        f.colSum += v[p];
        const double g = std::abs(v0[p] - v1[p]);
        halfSum += g;
        gapMax = std::max(gapMax, g);
        f.pxGapMax = std::max(f.pxGapMax, g);
    }
    // 16 个数的中位数 = 排序后第 8、9 位取平均。nth_element(nth=+8) 保证 *mid 是
    // 第 9 小、且 [begin,mid) 全 ≤ 它 ⟹ 第 8 小 = 前半段的**最大**（不是最小）。
    const auto mid = v.begin() + kResidualPeriod / 2;
    std::nth_element(v.begin(), mid, v.end());
    const double med = 0.5 * (*mid + *std::max_element(v.begin(), mid));
    f.swing = *std::max_element(v.begin(), v.end()) - med;
    return f;
}

/// 一行掩膜：逐像素折叠并累进 `acc`（`swingOut` 非空则同步写热图那一格）。
template <class At>
void foldSwingRow(
    SwingFoldAccum& acc, const cv::Mat& m, double tau, int periods, int y, At&& at, cv::Mat* swingOut)
{
    const uchar* pm = m.ptr<uchar>(y);
    for (int x = 0; x < m.cols; ++x) {
        if (!pm[x])
            continue;
        const auto f = foldPixelColumns(periods, at, y, x, acc.halfSum, acc.halfGapMax);
        if (f.pxGapMax > tau)
            ++acc.halfGapOverPx;
        acc.swings.push_back(f.swing);
        acc.sumSwing += f.swing;
        acc.sumVal += f.colSum / kResidualPeriod;
        if (tau > 0.0 && f.swing > tau)
            ++acc.overTau;
        acc.mx = std::max(acc.mx, f.swing);
        if (swingOut)
            swingOut->at<double>(y, x) = f.swing;
    }
}

/// 折叠统计的收尾（p99.9 必须在全图 `std::sort` 之后取，不能在像素内取）。
inline SwingCore finishSwingCore(SwingFoldAccum& acc)
{
    SwingCore s;
    s.halfGapMax = acc.halfGapMax;
    s.halfGapOverPx = acc.halfGapOverPx;
    s.mx = acc.mx;
    s.pixels = static_cast<int64_t>(acc.swings.size());
    if (acc.swings.empty())
        return s;
    const double n = static_cast<double>(acc.swings.size());
    s.mean = acc.sumSwing / n;
    s.meanValue = acc.sumVal / n;
    std::sort(acc.swings.begin(), acc.swings.end());
    s.p999 = percentileOf(acc.swings, 0.999);
    s.fracOver = static_cast<double>(acc.overTau) / n;
    s.halfGap = acc.halfSum / (n * kResidualPeriod);
    return s;
}

/// 折叠 + swing 的**唯一实现**（线性 luma 判据与显示空间报告共用同一份代码，杜绝两套口径漂移）。
/// `at(i, y, x)` 取第 i 帧在 (y,x) 的值并折成 double。
template <class At>
SwingCore swingFoldCore(const cv::Size& sz, const cv::Mat& m, double tau, int periods, At&& at,
    cv::Mat* swingOut = nullptr)
{
    SwingFoldAccum acc;
    acc.swings.reserve(static_cast<size_t>(std::max<int64_t>(1, cv::countNonZero(m))));
    if (swingOut)
        *swingOut = cv::Mat::zeros(sz, CV_64F);  // mask 外保持 0（区域纪律在图上同样成立）
    for (int y = 0; y < sz.height; ++y)
        foldSwingRow(acc, m, tau, periods, y, at, swingOut);
    return finishSwingCore(acc);
}

// ─────────────────────── 跨 TU 函数（定义在各自仪器层 TU；默认实参只写这里）───────────────────────

/// 合成靶一组采集（配置一律走 `makeProfile`，热身不写盘）。`tau` 由首组现算后回传，后续组共用。
Run runGroup(const std::string& dir, const std::string& name, const std::string& tag, bool taa,
    double feedback, double jitterScale, const std::string& variant, int periods, double warmMult,
    double& tau, bool baseSpace, const ClipArm& arm = {});

/// 一行 `Run` 读数（列宽 = 判据台账引用的列，别改）。
void printRow(const Run& r);

/// 批 13.7 合成靶**长短窗对照**变体（四组 B1–B4，**只出数不判**）。
int runWindowSweep(const std::string& root, int periods, double warmMult,
    const std::string& variant, bool baseSpace, double longFeedback, const ClipArm& arm = {});

/// bistro 一机位的一组采集（交付配置 + 用户机位；`wantMaps` 才留逐帧图）。
BistroRun runBistroGroup(const std::string& dir, const std::string& name, const std::string& tag,
    const BistroCam& cam, double feedback, double jitterScale, int periods, double warmMult,
    double& tau, bool wantMaps, bool taa = true, bool baseSpace = true, const ClipArm& arm = {});

/// 一组「历史拒收」采集（与 N1/N2 只差 `diag_reject` 一个键，连拍取 taa_out 的 alpha 码）。
BistroReject runBistroRejectGroup(const std::string& dir, const std::string& name,
    const std::string& tag, const BistroCam& cam, double feedback, int periods, double warmMult);

/// 出拒收交叉表 + 逐档抑制比 + 执行两条判决线（两条都**跑前写死**在头注释；这里只执行）。
RejectReport reportRejectAttribution(const BistroReject& R, const cv::Mat& swingOn,
    const cv::Mat& swingOff, double tau, const std::string& camName, double alphaOn,
    double alphaOff);

/// 出两态分带表 + 差表 + 判决 + 敏感性。返回进台账的关键读数。
BandReport reportLumaGradBands(const BistroRun& N1, const BistroRun& N2, double tau);

/// 一机位的六组（N6 仅 `--diag-bs-off`；N7/N8 仅 `--reject`）+ 出表 + 守卫 + 热图。
/// 返回 0 = 守卫全过；1 = 某守卫红；2 = 采集失败/锚点中止。
int runBistroCamera(const BistroCam& cam, int periods, double warmMult, double longFeedback,
    const std::string& shotDir, bool diagBsOff, bool reject, const ClipArm& arm = {});

/// 批 13.11 裁剪消融臂扫（每臂一对 A/B，逐臂打因果/否证判决）。
int runBistroClipAblSweep(int periods, double warmMult, const std::string& camera,
    double longFeedback, double onlyMode = 0.0, double onlySoft = 1.0);

/// bistro 变体总入口（机位表 → 逐机位 `runBistroCamera`）。
int runBistroSweep(int periods, double warmMult, const std::string& camera, double longFeedback,
    bool diagBsOff, bool reject, const ClipArm& arm = {});

/// 入参**当场拒**（返回非 0 = 直接作为命令退出码）。共同点 = 宁可拒也不静默忽略：
/// 一个被吃掉的键 = 「我加了 --xxx」这句话没有意义（同本仓「camelCase 静默回落」那条债）。
int validateResidualArgs(int periods, bool bistro, bool windowAbl, bool reject, bool clipAbl,
    double longFeedback);

/// 合成靶主路径 = 现行 7 条判据（`--bistro` / `--window-abl` 两个变体在命令入口就分岔走了）。
int runTargetResidual(int periods, double warmMult, const std::string& variant,
    double slowFeedback, bool skipSlow, bool baseSpace);

}  // namespace RGL::imgqa
