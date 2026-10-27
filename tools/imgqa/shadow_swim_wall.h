#ifndef RGL_IMGQA_SHADOW_SWIM_WALL_H
#define RGL_IMGQA_SHADOW_SWIM_WALL_H
// shadow-swim-wall 仪器内部头 —— 量具常数 + 读数类型 + 闸门/判决线声明的**唯一出处**。
//
// 全部论证（为什么垂直接收面要单独一台 / 为什么级联索引必须**逐位读**而不是差分 /
// 为什么观测量是视深不是屏幕行号）住 `shadow_swim_wall.cpp` 与 `shadow_wall_metric.h` 的头注释，
// 本文件不重述，只放两处都要消费的常数与类型。
//
// 分 TU（护栏：单文件 < 500 代码行；切法照 shadow-swim-flat 的四分）：
//   · shadow_swim_wall_target.cpp = **靶侧与机位侧**（manifest 读取 + 机位构造 + 闭式掩码/
//       视深场）。判据可机器核对：这一半**不起引擎进程、不写磁盘**，没有 GPU 也跑得完。
//   · shadow_swim_wall.cpp        = 臂表 + 采集 + 引擎回显对账 + 出表编排（每次装配一臂
//       = 至少一次 `makeProfile` + `runEngineOnce`）
//   · shadow_swim_wall_judge.cpp  = 闸门 + 判决线
//   · shadow_wall_metric.*        = **度量口径层**（级联索引投票 / 接缝视深 / 按视深合成），selftest 直测
//   · plane_reconstruct.*         = 平面接收面重建（与 shadow-swim-flat 共用，本仪器不另写）
// 🔴 与既有仪器共用的东西一律**不抄第二份**：cov 场（`seamCovField`）、二值阈值（`kSeamBin`）、
// 连续量 mean/p99（`field_diff_metric`）、像素域搬运与平移搜索（`shadow_transport_metric`）、
// 投影器（`makeProjector`）、引擎日志读取（`cluster_path_gate.h`）全部来自既有头。
// 🔴 方向红线（第一版的死法，别再踩）：钉级图**只准拿来出差分/做尺子**，不准把「自然臂 −
// 钉级图」的差当接缝定位判据 —— 钉级换的是整块表面的采样源，远离接缝处两级也给不同答案
// （本靶实测门面有影像素里 pin2↔pin3 分歧 ≈ 50 %），差场是一片糊。逐位**匹配**给索引。
#include "field_diff_metric.h"
#include "imgqa.h"
#include "plane_reconstruct.h"
#include "shadow_kernel_metric.h"
#include "shadow_transport_metric.h"
#include "shadow_wall_metric.h"  // CascadeVoteMaps / CascadeIndexReadout

#include <opencv2/core.hpp>

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace RGL::imgqa {

struct Projector;

// ── 量具常数（🔴 一个都不许做成命令行参数：能调 = 能调绿）───────────────────────────
inline constexpr int kWallWidth = 1600;
inline constexpr int kWallHeight = 900;
inline constexpr int kWallDelayFrames = 8;  ///< 与全家族同帧号（TAA 收敛后、无窗口交互）
inline constexpr double kWallMaxDistanceM = 500.0;  ///< 射线-平面求交的射程（靶尺度 ~1e1 m）
inline constexpr const char* kWallStem = "shadow_wall_rig";
inline constexpr const char* kWallGlbDir = "build/rgl/models";

/// 影缘侵蚀带半径（世界不变性对拍那一半，口径与 shadow-swim-flat 逐字相同 = 两台可比）。
inline constexpr int kWallErodePx = 2;
/// 光度可信判据容差（同 shadow-swim-flat 的 kPhotometricTol）。
inline constexpr double kWallPhotometricTol = 1.0 / 255.0;
/// 世界不变性的门（‰）：沿用 shadow-swim-flat 的 2.0 ‰ 口径（同一把尺才可比）。
inline constexpr double kWallMeanLimitPermille = 2.0;
/// 平移搜索网格（米）：粗 0.05 / 细 0.01，与 shadow-swim-flat 同（激励 0.25 m = 50 个粗格）。
inline constexpr double kWallSearchCoarseM = 0.05;
inline constexpr double kWallSearchFineM = 0.01;
inline constexpr double kWallSearchCoarseRadiusM = 0.4;
inline constexpr double kWallSearchFineRadiusM = 0.06;
/// 搜索候选的覆盖率地板（低于它 ⟹ 该候选把大半图当失配，不算「最优」）。
inline constexpr double kWallCoverageFloor = 0.5;
/// 接缝视深容差（米）：接缝**屏幕位置**预测的容差 = 门面上一格的视深宽（见判决线推导）。
inline constexpr double kWallSeamTolM = 0.02;
/// 「带随相机动」两条竞争模型（等视深 vs 世界锚定）的判据界（米，沿门面 v 方向）。
inline constexpr double kWallBandTolM = 0.05;

// ── 靶（models/shadow_wall_rig.manifest.json 的 measure 块 = 唯一真相源）─────────────
struct WallTarget {
    PlaneFrame plane { };             ///< 门面平面（O, e1, e2；n = e1×e2）
    std::array<double, 2> uRange { }; ///< 门面在 e1 上的跨度
    std::array<double, 2> vRange { }; ///< 门面在 e2 上的跨度（竖直）
    double fovDegrees = 0.0;
    double stimulusM = 0.0;           ///< 相机沿 n 的平移激励量
    /// 机位档名 → 沿 n 的距离 / 闭式级联模型读数（**模型**，本仪器只用它对账，不当判据分子）。
    std::map<std::string, std::array<double, 3>> camDist;
    std::map<std::string, std::array<double, 4>> splits;
    std::map<std::string, std::array<double, 4>> texelM;
    std::map<std::string, double> nearQ, farQ;
    /// 因果序列（按距离升序）与配对（同桶/跨桶由**实测**级联集合判定，不靠模型）。
    std::vector<std::string> causalOrder;
    struct Pair {
        std::string a, b, kind;
    };
    std::vector<Pair> pairs;
};

/// 一枚机位 = 六张图（none / 自然 csm / 钉级 0..3）+ 由它们逐位读出的级联索引。
struct WallCamera {
    std::string name;
    double distanceN = 0.0;
    Engine::Camera cam { };
    Projector proj { };
    std::array<double, 3> g { };  ///< (gU, gY, c0)：视深场 depth = c0 + gU·u + gY·v
    double entityShift = 0.0;     ///< 区分力守卫臂的注入量（靶沿 `shiftDir` 挪，相机跟着挪）
    std::array<double, 3> shiftDir { { 1.0, 0.0, 0.0 } };  ///< 注入方向 = 门面内的 e1（🔴 不是 n）

    cv::Mat none, csm;                      ///< 分母 / 自然臂
    cv::Mat arm;                            ///< 本格**实际用**的那张遮挡图（自然或钉级），cov 的分子
    std::array<cv::Mat, kWallCascadeCount> pin { };  ///< 四把钉级尺（force_cascade = 0..3）
    cv::Mat cov;                            ///< seamCovField(csm, none)：自然臂的遮挡度
    std::array<cv::Mat, kWallCascadeCount> covPin { };  ///< seamCovField(pin[k], none)
    cv::Mat mask;                           ///< 闭式门面掩码
    cv::Mat depthAt;                        ///< 逐像素闭式视深（CV_64FC1）
    PlaneSamples samples { };
    int64_t maskPx = 0;
    CascadeVoteMaps votes { };
    CascadeIndexReadout idx { };
    std::string idxError;                   ///< 读不出来的原因（算子按设计**抛**，表先出再判）
};

/// 一格 = 一套配置（自然/单级 × 门开/门关）下、两枚机位的世界不变性对拍。
struct WallCell {
    std::string title;
    bool rebase = false;
    int forceCascade = -1;  ///< −1 = 自然（多级联）；≥0 = 钉到该级（= 「没有接缝」那一格）
    double stimulusM = 0.0;
    WallCamera a, b;        ///< 两枚机位（B = A 沿 n 平移 stimulusM）
    cv::Mat keep;           ///< 光度可信掩码
    cv::Mat bandA, bandB;   ///< 影缘侵蚀带
    ShadowFlipReadout raw, floor;
    ShiftCandidate best;
    int64_t shadowPx = 0;   ///< none↔csm 逐位不同像素数（阴影在场性）
    double umbra = 0.0;     ///< 有影面积占比
};

[[nodiscard]] WallTarget loadWallTarget();
[[nodiscard]] WallCamera makeWallCamera(const WallTarget& t, const std::string& name);
/// 区分力守卫用的注入机位：与 `baseName` 同距离、同取景，但靶与相机都沿 +n 挪 `shift`。
[[nodiscard]] WallCamera makeInjectedCamera(const WallTarget& t, const std::string& baseName,
    double shift);
/**
 * 闭式门面掩码（8UC1，255 = 该像素在门面上）：射线与门面平面的交点落在矩形内。
 *
 * 前提「画内没有别的几何」由靶的构建期 assert ⑤ 守（`qa_scenes.py` 场景 11 的机关 ②）。
 * 它不是「阈值掩码」——没有可调参数，也没有读图。**定义在 `shadow_swim_wall_target.cpp`**
 * （零引擎那一半），本 TU 的采集装配消费它。
 */
[[nodiscard]] cv::Mat wallMask(const WallCamera& c, const WallTarget& t);
/// 逐像素闭式视深（CV_64FC1：depth = c0 + gU·u + gY·v）。掩码外 = NaN。同上，定义在靶侧 TU。
[[nodiscard]] cv::Mat wallDepthField(const WallCamera& c, const WallTarget& t);
/// 一臂 = 一次引擎启动。`noneArm` 切 shadow:mode，`forceCascade` 切 csm.force_cascade。
void captureWallArm(WallCamera& c, bool noneArm, int forceCascade, bool rebase,
    const std::string& profileDir, const std::string& what);
/// 采满六张图 + 算 cov / 掩码 / 视深 / 级联索引（索引读不出来时把原因记进 `idxError`）。
void fillWallCamera(WallCamera& c, const WallTarget& t, bool rebase,
    const std::string& profileDir);
/// 采一格的两枚机位（按格的 forceCascade 决定用自然臂还是钉级臂）+ 世界不变性对拍。
void fillWallCell(WallCell& cell, const WallTarget& t, const std::string& profileDir);

// ── 闸门（前提塌了必须抛，不许让判据在坏前提上出绿）────────────────────────────────
/// G1 两枚机位由**同一组级联**服务（实测，不吃模型）· G2 激励在场 · G3 阴影在场且场非退化 ·
/// G4 闭式掩码与可信集规模 · G5 四把钉级尺互不相同 · G6 引擎回显对账 · G7 索引可读性。
void requireWallCellsValid(const std::vector<WallCell>& cells, const std::vector<WallCamera>& cams);
void requireWallStimulusLive(const std::vector<WallCell>& cells);
void requireWallFieldAlive(const std::vector<WallCell>& cells);
void requireWallTrustSufficient(const std::vector<WallCell>& cells);
void requireWallPinsDistinct(const std::vector<WallCamera>& cams);
void requireWallIndexReadable(const std::vector<WallCamera>& cams);

// ── 判决线 ─────────────────────────────────────────────────────────────────────────
/// J-W1 `shadow-swim-wall-index-unique`：分歧像素必须**恰好**匹配一把尺（逐位地板）。
[[nodiscard]] Verdict judgeWallIndexUnique(const std::vector<WallCamera>& cams);
/// J-W2 `shadow-swim-wall-index-monotone`：级联索引对视深单调（引擎选级语义的直接推论）。
[[nodiscard]] Verdict judgeWallIndexMonotone(const std::vector<WallCamera>& cams);
/// J-W3 `shadow-swim-wall-seam-intervention` 🔴：把选级**钉死到实测服务级** ⟹ 画面逐位不变；
/// 钉到**别的级** ⟹ 画面大改（阳性对照）。本批头条：接缝机制对这块立面的贡献，用干预量出来。
[[nodiscard]] Verdict judgeWallSeamIntervention(const std::vector<WallCamera>& cams, int serving);
/// J-W4 `shadow-swim-wall-world-invariance`：四格（自然/单级 × 门开/门关）的残差与 S/T/C 归因。
[[nodiscard]] Verdict judgeWallWorldInvariance(const std::vector<WallCell>& cells);
/// J-W5 `shadow-swim-wall-determinism`：同机位重跑逐位相同 ⟹ 残差恰 0。
[[nodiscard]] Verdict judgeWallDeterminism(const cv::Mat& aNone, const cv::Mat& aCsm,
    const cv::Mat& bNone, const cv::Mat& bCsm, const ShadowFlipReadout& r);
/// J-W6 `shadow-swim-wall-offset-sensitivity`：注入已知世界滑移必须原样报出（区分力守卫）。
[[nodiscard]] Verdict judgeWallOffsetSensitivity(const ShiftCandidate& best, double injectedM,
    double meanPermilleAtZero);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_SWIM_WALL_H
