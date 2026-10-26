#ifndef RGL_IMGQA_SHADOW_SWIM_FLAT_H
#define RGL_IMGQA_SHADOW_SWIM_FLAT_H
// shadow-swim-flat 仪器内部头 —— 量具常数 + 四格读数类型 + 闸门/判决线声明的**唯一出处**。
//
// 全部论证（为什么是像素域搬运而不是 5 cm 世界格 / 为什么必须 double / 侵蚀带为什么就是
// 「无地板」三个字的载体）住 `shadow_transport_metric.h` 与 `shadow_swim_flat.cpp` 的头注释，
// 本文件不重述，只放两处都要消费的常数与类型。
//
// 分 TU（护栏：单文件 < 500 代码行；四切法照 shadow-edge-ruler / shadow-edge-rebase）：
//   · shadow_swim_flat.cpp        = 臂表 + 采集 + 引擎回显对账 + 逐格读数装配 + 出表编排
//   · shadow_swim_flat_judge.cpp  = 闸门 G1–G6 + 五条判决线
//   · shadow_transport_metric.*   = **度量口径层**（double 域地面搬运 / 影缘侵蚀带 / 翻转读数 /
//                                   平移搜索），`just imgqa selftest` 闭式直测的那一层
// 🔴 与既有仪器共用的东西一律**不抄第二份**：靶读取（`loadEdgeTarget(stem)`）、级联闭式
// （`edgeCascadeModel`）、cov 场（`seamCovField`）、二值阈值（`kSeamBin`）、量具尺寸与帧号
// （`kEdge*`）全部来自 `shadow_edge_ruler.h` / `shadow_seam_metric.h`。本文件只新增**本仪器
// 自己的**自变量（激励量、侵蚀半径、可信集容差、搜索网格）与阈值。
#include "shadow_edge_ruler.h"  // kEdgeWidth/Height/DelayFrames/FactoryD/kEdgeGlbStem + EdgeTarget
#include "shadow_kernel_metric.h"  // Projector / makeProjector
#include "shadow_transport_metric.h"

#include <opencv2/core.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace RGL::imgqa {

// ── 量具常数（改动 = 改判据；每条阈值的出处 = judgments_table_swim_flat.cpp 的容差理由）──
/// 远工作点靶（与 shadow-edge-rebase **同一枚**靶，`worldOffset = [1e6,0,0]`，manifest 自带
/// 自证 assert ⑪）。复用而不是另造：两枚靶的 GLB 逐字节相同，差别只在 manifest。
inline constexpr const char* kFlatFarStem = "shadow_edge_rig_far";

/**
 * 🔴 激励 = 相机 B 相对相机 A 沿**世界 +X** 平移这个量（纯平移，姿态逐字相同）。
 *
 * 为什么是 0.5 m 而不是「随便挪一点」——两条同时满足的窗口很窄，且都是闭式可证的：
 * ① **必须留在同一个量化桶里**。级联的 near/far 由 `dist(相机, SceneBounds.center)` 量化到
 *   2 的幂（本靶出厂点：nearRaw 1.4686 → 2，farRaw 10.9999 → 16）。相机沿 +X 挪 dx 时该
 *   距离的变化率 = 视线与 +X 的夹角余弦 ≈ 0.5017 ⟹ dx = 0.960 m 时 nearRaw 才涨到 2.0 而
 *   **翻到 4**。取 0.5 m ⟹ nearRaw ∈ [1.22, 1.72]，两枚机位都量化到 2、far 都量化到 16。
 *   这条不是「大概没事」：G1 把两枚机位的 `edgeCascadeModel` 逐数对账，翻桶就抛。
 * ② **必须足够大到能看见滑移**。影场若随相机在世界里滑 dx，翻转只发生在影缘两侧各 ~dx/纹素
 *   像素的带里；dx 太小会被侵蚀带（2 像素 ≈ 1.1 cm）一起吃掉。0.5 m 在 6 m 处 = 88 像素，
 *   远大于侵蚀带 ⟹ 「侵蚀后仍翻转」一定是真滑移。
 * 与 material-swim 的激励同值同向（0.5 m / +X），两台仪器因此可比。
 */
inline constexpr double kFlatStimulusM = 0.5;

/// 影缘侵蚀半径（像素）。🔴 这个数**就是**「无地板」三个字的载体，报数必须连它一起报。
/// 取 2 的实测推导（本靶出厂点门关、两机位差 0.5 m，2026-10-26 本机）：erode 0 → 0.2558 %，
/// erode 1 → 0.0000 %，erode 2 → **恰 0.0000 %**，erode 4 → 0.0000 %。取 2 而不是 1 = 给
/// 双线性插值的支撑域（2 像素）留一格余量；不再大 = 每多 1 像素就从分母里多删一圈真信号。
inline constexpr int kFlatErodePx = 2;

/// 光度自吻合容差（线性域相对差，债-139 的既有口径，同 material-swim 的 2%）。
inline constexpr double kFlatPhotometricTol = 0.02;

/// 地面交点距离上限（米）：超出即掠射区，一像素跨数米、世界对应关系不可信，不进判据。
/// 12 = 覆盖本靶地面最远角（相机到地面远角闭式 10.7 m）再留 1.3 m 余量。
inline constexpr double kFlatMaxDistanceM = 12.0;

/// 平移搜索的两轮网格（米）：粗 ±0.50 步 0.10（= 激励量本身，守卫要报得出 d）、细 ±0.10 步
/// 0.02（≈ 3.5 枚最近级纹素）。🔴 轮次与每轮候选数**写死**（121 + 121 = 242 个，含中心），
/// 仪器不给收敛循环留口子——本批的一次交付就折在「搜索自己决定跑多久」的无界工作上。
inline constexpr double kFlatSearchCoarseM = 0.10;
inline constexpr double kFlatSearchCoarseRadiusM = 0.50;
inline constexpr double kFlatSearchFineM = 0.02;
inline constexpr double kFlatSearchFineRadiusM = 0.10;
/// 候选覆盖率下限（相对零位移的可信点数）。0.8 = 与 material-swim 同式：只有一小半重叠的
/// 角落平移当最优解 = 分母是彩票。
inline constexpr double kFlatCoverageFloor = 0.8;

// ── 判据阈值（先测后定，2026-10-26 本机；逐条推导住 judgments_table_swim_flat.cpp）────────
/// 无地板翻转率上界（%）。实测 ①②④ 全为 **恰 0.0000 %**，③（预期最差）0.0009 %。
inline constexpr double kFlatFlipLimitPct = 0.01;
/// 无地板 mean |Δcov| 上界（千分点）。实测 ① 0.787 / ② 0.838 / ④ 0.739 / ③ 4.213 ‰。
inline constexpr double kFlatMeanCovLimitPermille = 2.0;
/// J4 的「显著」倍率：③ 的 mean |Δcov| 必须 ≥ 此倍数的 ④ 才算 rebase 买到了东西。
/// 实测 5.7 倍（4.213 / 0.739），取 2.0 = 实测余量的一半以上。
inline constexpr double kFlatBenefitRatio = 2.0;

/// 一「格」= (工作点靶, coordinate.rebase 开关态) 的一整套采集与读数。四格编号 ①②③④，
/// 顺序在 `makeFlatCells` 里钉死（主表列序与判决线取数都按它，同 shadow-edge-rebase）。
struct FlatCell {
    std::string title;     ///< "① 出厂·门关" 一类（打印与 Verdict 文本用）
    std::string stem;      ///< 靶文件名（不含扩展名）
    bool rebase = false;   ///< coordinate.rebase

    EdgeTarget target;     ///< manifest（机位 / bounds / worldOffset 的唯一真相源）
    EdgeCascadeModel modelA { };  ///< 相机 A 的级联闭式
    EdgeCascadeModel modelB { };  ///< 相机 B 的级联闭式（G1 与 modelA 逐数对账）
    Projector projA { };
    Projector projB { };

    cv::Mat noneA, csmA;   ///< 相机 A 的两臂（none = cov 分母）
    cv::Mat noneB, csmB;   ///< 相机 B 的两臂

    cv::Mat covA, covB;    ///< seamCovField(csm, none)（线性域，与 grain/seam/ruler 同式）
    cv::Mat keep;          ///< 光度自吻合可信集（8UC1，A 网格）
    cv::Mat bandA, bandB;  ///< 影缘侵蚀带（8UC1，各自网格）
    GroundSamples samples; ///< A 网格的像素→世界地面采样点（double 域，搜索时复用）

    ShadowFlipReadout raw { };   ///< erode 0 = 债-139 原口径（含重采样地板，只做可比性判定）
    ShadowFlipReadout floor { }; ///< erode kFlatErodePx = **无地板读数**（本仪器的头条数）
    ShiftCandidate best { };     ///< 最优世界滑移（非零 = 影场随相机在世界里滑了）

    std::int64_t shadowPx = -1;  ///< 本臂 none ↔ csm 逐位不同像素数（恰 0 = 阴影整块消失）
    double umbraA = 0.0;         ///< A 臂有影面积（cov > kSeamBin 占比，G5 的取数）
    double umbraB = 0.0;
};

/// 四格定义 + 靶读取 + 闭式装配（工厂函数而不是聚合初始化，理由同 shadow-edge-rebase）。
[[nodiscard]] std::vector<FlatCell> makeFlatCells();

/// 一格的读数装配：cov 场 → 可信集 → 侵蚀带 → 两种口径的翻转读数 → 平移搜索。
/// 🔴 必须在 `captureCell` 之后、`fillFlatControls` 之前调（对照臂复用本格的 samples/keep/covB，
/// 抄第二份 = 两份会漂的「同一枚机位的搬运映射」）。
void fillFlatCellReadout(FlatCell& c);

/// 阴性对照（同机位重跑）与注入臂（挪靶 + 跟相机）的采集与读数。
struct FlatControl {
    cv::Mat noneA2, csmA2;             ///< ① 的 A 机位重跑（J2：必须逐位相同 ⟹ 翻转恰 0）
    ShadowFlipReadout determinism { }; ///< 上者的对拍读数（erode 0，最严口径）
    std::int64_t bitDiff = -1;         ///< 两次 A 臂逐位不同像素数（🔴 必须恰 0）

    cv::Mat noneS, csmS;               ///< 注入臂：靶沿 +X 挪 d、相机跟着挪 d
    cv::Mat keep;                      ///< 🔴 注入臂**自己的**可信集（B vs S 恒等搬运的光度自吻合）
    size_t trustKept = 0;              ///< 上面那张掩码的 255 计数（分母规模的读数）
    ShadowFlipReadout injected { };      ///< covB ↔ covS 的 erode 0 读数（含重采样地板）
    ShadowFlipReadout injectedFloor { }; ///< 同上的 erode kFlatErodePx 读数（无地板口径）
    ShiftCandidate injectedBest { };   ///< 🔴 必须报出 +d（报 0 = 量具看不见滑移）
};

/// 采集两条对照臂并出读数（顺序：先出数、闸门后抛，同 shadow-edge-rebase 的交付纪律）。
/// 🔴 必须在 `fillFlatCellReadout(base)` 之后调：对照臂复用本格的 samples / keep / covB，
/// 另算一份 = 两份会各自漂的「同一枚机位的搬运映射」（项目哲学第 1 条）。
void fillFlatControls(FlatCell& base, FlatControl& ctl, const std::string& profileDir);

// ── shadow_swim_flat_judge.cpp ──────────────────────────────────────────────────────
/**
 * **G1｜同桶闸门 + 靶同源**：① 远点 manifest 的 worldOffset 必须 = 1e6 且逐板真值 = 出厂靶 +
 * 平移（复用 `requireFarIsTranslatedCopy` 的口径，本仪器不抄第二份）；② 🔴 同一格的两枚机位
 * 必须落在**同一个量化桶**里（`edgeCascadeModel` 逐数相同）。翻桶时「阴影随相机变」有一半是
 * 级联本身变了，与 rebase 无关——那不是本判据的射程，宁可抛。
 */
void requireFlatCellsValid(const std::vector<FlatCell>& cells);

/// **G2｜激励在场**：每格 A/B 的 none 臂不得逐位相同（相机根本没动 = 四格只剩一格）。
void requireStimulusLive(const std::vector<FlatCell>& cells);

/// **G3｜阴影在场 + 场非退化**：每臂 none↔csm 逐位不同像素数 > 0 且有影面积 > 死态。
/// 🔴 恒 0 场上「翻转率 0」是恒真——这条红时全部读数作废，不许读成通过。
void requireFieldAlive(const std::vector<FlatCell>& cells);

/// **G4｜可信集规模**：可信点数与占比双下限（分母是彩票时任何百分比都没有意义）。
void requireTrustSufficient(const std::vector<FlatCell>& cells);

/// **G5｜门开确实动了图**：② vs ①、④ vs ③ 的自然臂不得逐位相同（原点没进链路 = 四格只剩两格）。
void requireRebaseMovedImage(const std::vector<FlatCell>& cells);

/// J-F1 `shadow-swim-flat-floor`：① 出厂门关的**无地板**残差（Q1 的头条数）。
[[nodiscard]] Verdict judgeFlatFloor(const FlatCell& c);
/// J-F2 `shadow-swim-flat-determinism`：同机位重跑逐位相同 ⟹ 翻转**恰 0**。
[[nodiscard]] Verdict judgeFlatDeterminism(const FlatControl& ctl);
/// J-F3 `shadow-swim-flat-no-harm`：② 门开不得比 ① 门关差（同阈值、同算子）。
[[nodiscard]] Verdict judgeFlatNoHarm(const FlatCell& off, const FlatCell& on);
/// J-F4 `shadow-swim-flat-benefit` 🔴：④ 远点门开的无地板残差必须回到 ①② 水平（Q2 的头条数）。
[[nodiscard]] Verdict judgeFlatBenefit(const FlatCell& base, const FlatCell& farOff,
    const FlatCell& farOn);
/// J-F5 `shadow-swim-flat-offset-sensitivity`：注入 d 必须报出 +d（区分力守卫）。
[[nodiscard]] Verdict judgeFlatOffsetSensitivity(const FlatControl& ctl);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_SWIM_FLAT_H
