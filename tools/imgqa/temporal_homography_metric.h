#pragma once
//
// temporal_homography_metric.h —— `temporal-homography` 判据的**跨 TU 共享层**（类型 + 常量 + 算子声明）
//
//   为什么拆：`temporal_homography.cpp` 到 824 代码行（工具侧上限 500）被文件护栏逼出三刀。缝按
//   **仪器层次**切，与 `shadow_kernel_metric.*` / `diff_struct_metric.*` / `ao_noise_metric.*` /
//   `temporal_flicker_metric.*` 是同一条缝（「口径 vs 仪器」），只是这台仪器有**三层**：
//
//     · `temporal_homography_metric.cpp` = **重投影算子层**——闭式单应（yawHomography）、与引擎 TAA
//       逐式同权重的重建核（Catmull-Rom / Lanczos3 / 双线性）、三条核自检、直方图与逐像素场的分位、
//       α 扫描、热图落盘。全部不碰引擎、不读配置、不打印。
//       ⚠ 那三条 check（恒等性 / 整数平移 / 与 cv::warpPerspective 比量级）是**仪器自己的标定锚点**：
//       「权重函数或抽头基址写错了当场红」。一个权重系数都不许"化简"或合并，边界与归一化处理也不动
//       ——核的权重式与抽头索引是**同一个约定的两半**（理由逐行写在该 TU 的「为什么双线性不够」段）。
//     · `temporal_homography_bands.cpp` = **运动带判据层**——分带尺子（MovingBandRuler + τ 覆盖）、
//       可测性守卫的执行与回显、六格表 + 材质归因 + 三分支判决。判决线**不在**这里：跑前写死在
//       `temporal_residual.h`，本层只执行。
//     · `temporal_homography.cpp` = **采集与命令**——runHomography（两态归因 + 守卫量）、
//       deliveryGlb（材质名的真相源）与 cmdTemporalHomography（CLI 生命周期、读数表、守卫闸门）。
//
//   类型与常量为什么住这里：`WarpKernel` / `HomoStat` / `MovingBandRuler` 是三层之间**唯一**的数据
//   通道（算子层填、判据层读、命令层搬运），`kFovDeg` 既是采集侧的像素焦距换算又是算子层
//   sweepResidual 的 focal——两处各抄一份 = 第二份真相（改一处忘另一处时读数与解释会分家）。
//   分带阈值与守卫线**不在**这里：唯一出处 = `temporal_residual.h`（跑前写死），本头只引用不重拍。
//   两条分位口径（`histPercentile` 直方图法 / `fieldPercentile` 排序法）也**不在**本头统一尺度：
//   本仓有过 0–100 与 0–1 两种分位尺度混用的前科，插值方式与"第几个序"本身就是判据。
//
//   ⚠ 读数格式化 `d4` / `d2` **故意不在这里**：`temporal_swim.h` 已在 `RGL::imgqa` 作用域放了
//   `inline d2`（`"%.2f"`），而本命令的 `d2` 是 `"%.2f%%"`（乘 100 带百分号）。同名同签名不同体
//   ⟹ 提到本头 = 跨 TU 的 ODR 冲突（链接器挑走一个，另一台仪器静默换口径）。既有惯例就是每个 TU
//   在匿名 namespace 里各留一份（`temporal_motion.cpp` / `temporal_residual.cpp` /
//   `material_attribution.cpp` 皆如此），本命令拆出的两个 TU 沿用，**别"顺手"合并**。
//
//   ⚠ 本头**不是** temporal-homography 的对外接口：对外声明（`yawHomography` /
//   `cmdTemporalHomography`）全在 `temporal_homography.h`（selftest 与 temporal_yaw_remap 吃
//   yawHomography）。这里声明的东西只给 temporal_homography*.cpp 三个 TU 用，别的 TU 别接。
//   跨 TU 的名字一律 `RGL::imgqa` 作用域（匿名 namespace 的成员跨不了 TU）；只被单个 TU 用的
//   小件（重建核权重函数、warpTaps / warpBilinear、命令层的采集结构）仍留在各自文件里面。
//
#include "material_attribution.h"
#include "temporal_flicker.h"
#include "temporal_residual.h"

#include <opencv2/core.hpp>

#include <string>
#include <utility>
#include <vector>

namespace RGL::imgqa {

// ───────────────────────── 常量（全命令唯一出处）─────────────────────────

/// 垂直 FOV（deg）。采集侧的像素焦距换算（`verticalFocalPx`）与算子层 sweepResidual 的 focal
/// 必须是同一个数，否则「名义扫速 → Δψ」与「按 Δψ warp」用的是两把尺子。
constexpr float kFovDeg = 45.0f;

/// 与**引擎 TAA 同权重**的重采样核（详见 `temporal_homography_metric.cpp` 的「为什么双线性不够」
/// 段）。名字先于 HomoStat 声明（守卫量里要带核的标签）。
enum class WarpKernel { Bilinear, CatmullRom, Lanczos3 };

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
    // ── 守卫量（2026-10-06 新增；一律用**跨帧最大场**的均值，与分带表同一份证据）──────────
    WarpKernel kernel = WarpKernel::Bilinear;
    /// 显示域字节亮度（0..255）的**全帧全场**均值；−1 = 本次没算（未开分带）。
    /// 为什么守卫要它：A/B 口径能把画面**关黑**（实测 `env.intensity=0` 打在只吃天光的
    /// 机位上 ⟹ 交付面全黑 ⟹ 帧差/残差/地板全 0）。那种跑法既没有闪烁也没有归因对象，
    /// 却会因为「四个数都小」而让 1a/2/3 以另一种方式失效——必须单独点名，见守卫 ⓪。
    double lumaMean = -1.0;
    double rawMean = 0.0;     ///< |I_k − I_{k−1}| 跨帧最大场的均值
    double residMean = 0.0;   ///< 按 δ 分开 warp 的残差跨帧最大场均值（= 守卫的「对照」）
    double floorMean = 0.0;   ///< 同态往返地板跨帧最大场的均值（= 守卫的「地板」）
    cv::Mat floorHeat;        ///< 地板跨帧最大场（τ 的出处：取它的分位数）
    /// 对齐自证曲线 (α, 残差均值)。α = warp 强度倍率：0 = 不 warp（= 原始帧差），
    /// ±1 = 闭式 Δψ（含反号）。**残差必须在 |α|=1 附近见底**，否则"残差 = 运动没搬动的部分"
    /// 这句话没有依据。为什么不用「两帧搬到同一点」→ temporal_residual.h 的守卫 1 注释。
    std::vector<std::pair<double, double>> alignCurve;
};

/// dst→src 的逐像素源坐标（单应与帧无关，故一次算好给整段连拍复用）
struct SrcMap {
    cv::Mat x, y;  ///< CV_64FC1
};

/// 「亮度 × 梯度」分带的**尺子**。口径与静止态批 13.7c 逐字相同；完整理由与判决线 →
/// `temporal_homography_bands.cpp` 头注释的「批 13.9」段与 `temporal_residual.h`。
struct MovingBandRuler {
    cv::Mat byteMean, byteMax, gradMax;
    double tau = 0.0, tauLegacy = 0.0, white = 0.0, gradThr = 0.0, tHi = 0.0;
    std::vector<double> gradSorted;  ///< 排序后的梯度（敏感性档 p75/p95 从它来）
};

// ─────────────── 重投影算子层（定义在 `temporal_homography_metric.cpp`）───────────────

/// 绝对差图的 p 分位（直方图法；p 取 0..1）。⚠ 与 `fieldPercentile` 是**两条口径**：直方图法把
/// ≥1 的值全塞进最后一个 bin，排序法不会——两者不可互换，也不许"顺手统一尺度"。
double histPercentile(const cv::Mat& absdiff, double p);

/// 把 [0,1] 的残差图写成可比的热图（**固定标尺** 0→黑 / 1→白，两组之间才能对眼）
void writeHeat(const cv::Mat& heat, const std::string& path);

[[nodiscard]] const char* kernelName(WarpKernel k);

/// dst→src 单应 → 逐像素源坐标表（整段连拍复用一次）
[[nodiscard]] SrcMap buildSrcMap(const cv::Matx33d& dstToSrc, cv::Size sz);

/// 按选定核把 src 重采样到 dst（dst 由本函数分配）
void warpKernel(const cv::Mat& src, const SrcMap& m, WarpKernel k, cv::Mat& dst);

/// 恒等性自检：核在**零偏移**处必须逐位取回原值（`temporal-identity` 判据的算术基础）
double identityCheck(const cv::Mat& src, WarpKernel k);

/// **整数平移**自检：把内容搬恰好 1 个像素，任何"整数偏移处精确"的核都必须逐位取回平移值
double translationCheck(const cv::Mat& src, WarpKernel k);

/// 与 `cv::warpPerspective(INTER_LINEAR, BORDER_REPLICATE)` 对拍（只 bilinear 路可比）
double bilinearCrossCheck(const cv::Mat& src, const cv::Matx33d& dstToSrc);

/// 跨帧最大场（acc = max(acc, x)）——分带与 τ 用同一片「逐像素最坏」证据
void maxInto(cv::Mat& acc, const cv::Mat& x);

/// 均值（守卫用它，不用直方图分位）
double meanOf(const cv::Mat& x);

/// 逐像素场的分位（排序法；p 取 0..1）。⚠ 与 `histPercentile` 两条口径不可互换，见上
double fieldPercentile(const cv::Mat& f, double p);

/// 按 **α × Δψ** 的强度 warp 上一帧到本帧网格，返回跨帧最大残差场的均值
double sweepResidual(
    const FlickerSeries& g, double yawRadPerFrame, double alpha, WarpKernel kernel);

// ─────────────── 运动带判据层（定义在 `temporal_homography_bands.cpp`）──────────────

/// 从**移动机位连拍**造分带尺子（τ 的旧口径留在这里当 tauLegacy 回显）
MovingBandRuler movingBandRulerFrom(const FlickerSeries& g);

/// 用**本次实测同态往返地板**（跨帧最大场）覆盖 τ。地板缺失 = 不动（保持旧口径并如实报）
void applyFloorTau(MovingBandRuler& r, const cv::Mat& floorHeat);

/// 三条可测性守卫的执行（**只执行**；线在 `temporal_residual.h` 跑前写死）
MovingGuards evaluateMovingGuards(
    const HomoStat& off, const MovingBandRuler& r, const cv::Mat& residOff);

/// 守卫回显（**过与不过都打**——过了也要让人看见是"哪三个数"放行的）
void printMovingGuardEcho(const MovingBandRuler& r, const MovingGuards& g, WarpKernel k);

/**
 * `reportMovingBandsFromHomography` 的入参包（原 12 个参数收进一处，默认值与调用点语义不变）。
 *
 * 除 `targetName` 外全是引用 ⟹ 搬运零成本：`MovingBandRuler::gradSorted` 是整幅画面的排序梯度
 * （720p = 921600 个 double ≈ 7 MB），按值收进结构体 = 每次调用多拷一份而它只是被读。
 */
struct MovingBandReport {
    const MovingBandRuler& ruler;       ///< 尺子（τ / 三阈值 / 敏感性档），TAA 关那组定、两态共用
    const cv::Mat& residOff;            ///< TAA **关**的逐像素残差（跨帧最大，CV_64FC1）
    const cv::Mat& residOn;             ///< TAA **开**的逐像素残差（同上）
    std::string targetName;             ///< 目标名（进台账；出表处当前不打印它）
    const cv::Mat& ids;                 ///< 中点姿态 objectID（空 = 未采 = 跳过材质表）
    const std::vector<GlbPrim>& prims;  ///< glb 摊平的 primitive 表（材质名真相源）
    double floorMeanOff = 0.0;          ///< 两态各自的全帧往返地板均值（材质表 residOverFloor 的分母）
    double floorMeanOn = 0.0;
    double idYawErrPx = 0.0;            ///< objectID 落位姿态残差（回显）
    int idBestFrame = -1;               ///< 同姿态补拍最接近 burst 第几帧（回显）
    double idBestDiff = 0.0;            ///< 与该帧的平均 |Δ|（回显 ≈0 = 姿态逐位复现）
    double sweepTotalPx = 0.0;          ///< 整段 burst 的总位移（回显「为什么只拍一张 ID」）
};

/// 出「亮度 × 梯度」六格表（四数）+ 三分支判决。返回判决文字供台账。
std::string reportMovingBandsFromHomography(const MovingBandReport& rep);

}  // namespace RGL::imgqa
