#ifndef RGL_IMGQA_SHADOW_GRAIN_H
#define RGL_IMGQA_SHADOW_GRAIN_H
// shadow-grain 仪器内部头 —— 跨 TU 面（类型 + 声明 + 量具常数）的唯一出处；
// 判决方向的论证全文在 `shadow_grain.cpp` 头注释，本文件不重述。
//
// 分 TU（护栏：单文件 < 500 代码行；先例 = ao_noise / temporal_residual，缝在仪器**内部**）：
//   · shadow_grain.cpp        = 臂表 + 采集 + 引擎回显对账 + 槽带在场闸门 + 入口编排
//   · shadow_grain_target.cpp = 靶侧：manifest measure 块（按缝深分组）+ 投影窗/列区间
//                               （区域来自几何，不在 C++ 重抄几何常量）
//   · shadow_grain_judge.cpp  = 三条判决线（J1 存活率 / J2 接线阳性锚点 / J3 出厂档对账）
//                               + 逐带读数表
//   · shadow_grain_metric.*   = 度量口径层（`imgqa selftest` 闭式自检，`just tools-check`
//                               覆盖得到的唯一一层）
#include "imgqa.h"
#include "shadow_grain_metric.h"  // kGrainBin（闸门与探针的共用二值线）
#include "shadow_kernel_metric.h" // Projector / makeProjector / projectPoint3：投影口径唯一出处

#include <opencv2/core.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RGL::imgqa {

// ── 量具常数（改动 = 改判据；阈值的出处 = shadow_grain.cpp 头注的 slope 扫描表）──────────
/// J1 存活率下限：带在场/被抹两个**闭式态**是 1 与 0，判决线取其中点 = 0.5。
/// 不抄任何实测数字——扫描表只证明真实读数不落在线的中段（100% vs ≤6.3%），不定义线。
inline constexpr double kSurvivalFloor = 0.5;
/// J2「两臂显著不同」下限：接线活着时 csm0 臂 ≈ 1×闭式刻度、大值臂 ≈ 0，差 ≈ 1×闭式；
/// 旋钮断线 = 两臂同值差 0。0.5 = 两个闭式态的中点（与 kSurvivalFloor 同一定线法）。
inline constexpr double kWiredGapFloor = 0.5;
/// J2「大值臂趋 0」上限，单位 = **单带残留增量**（闭式 = 区域刻度/带数 = 2/(cols−1)）：
/// 0.5 × 增量 = 「一条带的残留都不许有」，不是「大致干净」的模糊线。
inline constexpr double kWiredHighCeilFactor = 0.5;
/// 存活窗在闭式带两缘外的富余列（覆盖带缘的 ±1 px 光栅化漂移；必须 < 到邻带的距离，
/// 由生成器 assert ⑫ 钉）
inline constexpr int kBandEdgeMarginCols = 2;
inline constexpr int kMinWindow = 32; ///< 判据窗口最小边长（px，分母太小 = 闭式刻度失稳）
inline constexpr double kAxisAlignTol = 0.05; ///< 墙面投影的轴对齐容差（px，透视下应精确）
inline constexpr int kProbeHalf = 2; ///< litProbe 邻域半边长（px，必须 < 墙块半宽，生成器 assert ⑩）
inline constexpr const char* kGlbStem = "shadow_grain";
inline constexpr int kWidth = 1280;    ///< 采集宽（必须 == manifest measure.captureSize[0]）
inline constexpr int kHeight = 720;    ///< 采集高
inline constexpr int kDelayFrames = 8; ///< 截图帧号（llm_shot.delay_frames）

/// 臂序（= grainArms() 表序 = 采集数组下标；多处按索引读图的唯一真相源）。
enum GrainArmIdx : std::size_t {
    kArmNone = 0,  ///< mode=none：覆盖度分母参照
    kArmDefault,   ///< mode=csm 且**不写 slope_bias 键**：引擎出厂默认档（J1 的读数来源）
    kArmDelivery,  ///< 仓库 configs/shadow.lua 全文：J3 对账伙伴（「交付出厂值确实是 0」）
    kArmCsm0,      ///< slope_bias = 0：在场闸门 + J2 低臂
    kArmCsm32,     ///< slope_bias = 32：J2 高臂（必须趋 0）
    kArmCount,
};

/// 一臂 = shadow.lua 的整份内容 + 引擎回显里**必须**出现的证据串（同 kernelscale 的 Arm）。
struct GrainArm {
    std::string name;
    std::string wantMode;   ///< 路线行必须含 `shadow: mode=<wantMode>`
    std::string wantExtra;  ///< 还必须含的串（空 = 无差集行要求）
    std::string shadowLua;  ///< 本臂 shadow.lua 的整份内容
};

/// 一条闭式槽带 = 背板深度上的两个世界角点（v 下缘 / v 上缘），仪器投影成列区间。
using GrainBand = std::array<std::array<double, 3>, 2>;

/// 一个缝深组 = 该深度的缝（本靶每档一条）+ 其闭式带（manifest measure.groups 的镜像）。
struct GrainGroup {
    double depthM = 0.0;
    std::vector<GrainBand> bands;
};

struct GrainTarget {
    std::array<std::array<double, 3>, 4> corners{};  ///< region 世界角点（4 个）
    std::vector<GrainGroup> groups;
    std::array<double, 3> litProbe{};  ///< 分隔墙块中心（恒受光参照）
    Engine::Camera camera;
    [[nodiscard]] std::size_t bandCount() const;
};

/// 判据窗口（整数像素）+ 闭式刻度（region 窗 = 2·总带数/(cols−1)；组窗 = 2·组内带数/(cols−1)）。
struct GrainWindow {
    cv::Rect win;
    double closed = 0.0;
};

/// 一条带的列区间：存活窗（闭式带 ± kBandEdgeMarginCols）+ 在场探针（带中央 50%）。
struct GrainBandGeom {
    cv::Range survival;  ///< 该区间内数跳变；带在场闭式 = 每行 2
    cv::Range probe;     ///< 在场探针取列区间（中央 50%，**不内缩**——细带只有 4–12 px）
};

/// 逐带读数（J1 的读数表行；密度 = 存活率 × 单带闭式增量）。
struct GrainBandReadout {
    std::size_t group = 0;        ///< groups 下标
    std::size_t indexInGroup = 0; ///< 组内带序
    double depthM = 0.0;
    double bandStep = 0.0;        ///< 单带闭式增量 = 2/(存活窗 cols−1)
    double densityDefault = 0.0;  ///< 出厂默认臂的带窗密度
    double survivalDefault = 0.0; ///< 出厂默认臂的存活率
    double survivalCsm0 = 0.0;    ///< slope=0 臂的存活率（阳性对照）
};

// ── shadow_grain_target.cpp ─────────────────────────────────────────────────────
[[nodiscard]] GrainTarget loadGrainTarget();
[[nodiscard]] GrainWindow projectRegionWindow(const Projector& proj, const GrainTarget& t);
[[nodiscard]] GrainBandGeom projectBandGeom(const Projector& proj, const GrainBand& band,
    const cv::Rect& regionWin, std::size_t gi, std::size_t bi);
/// 全部带的几何（展平序 = 组序 × 组内带序，与 GrainBandReadout 的 flat 序一致）。
[[nodiscard]] std::vector<GrainBandGeom> projectAllBandGeoms(const GrainTarget& t,
    const Projector& proj, const GrainWindow& region);
/// 列区间 × 判据窗口全行 的 cov 均值。
[[nodiscard]] double columnsMean(const cv::Mat& cov, const cv::Range& cols, const cv::Rect& win);
/// litProbe 邻域（(2·kProbeHalf+1)² 像素）的 cov 均值。
[[nodiscard]] double probeMean(const cv::Mat& cov, const Projector& proj,
    const std::array<double, 3>& w, const cv::Rect& win, const std::string& what);

// ── shadow_grain.cpp（grainArms/captureArm/回显对账/在场闸门都是本 TU 匿名 namespace
//    的私件，不跨 TU，故不在此声明；入口 cmdShadowGrain 的声明在 imgqa.h）──────────

// ── shadow_grain_judge.cpp ─────────────────────────────────────────────────────
/// J1 `shadow-grain-band-survival`：出厂档真实槽底影带存活率下限（逐带，取最严一条）。
void pushSurvivalVerdict(std::vector<Verdict>& vs,
    const std::vector<GrainBandReadout>& readouts);
/// J2 `shadow-grain-wiring-anchor`：旋钮→画面通路活着（0 臂与 32 臂显著不同且高臂趋 0）。
void pushWiringVerdict(std::vector<Verdict>& vs, double density0, double density32,
    double closed, double bandStep);
/// J3 `shadow-grain-factory-identity`：出厂档对账（默认臂 ≡ 显式 8 ≡ 交付配置，逐位）。
void pushIdentityVerdict(std::vector<Verdict>& vs, std::int64_t defaultDiffPx,
    std::int64_t deliveryDiffPx);

[[nodiscard]] std::string grainFixed4(double v);
[[nodiscard]] std::string grainFixed3(double v);

}  // namespace RGL::imgqa
#endif
