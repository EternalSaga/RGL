#ifndef RGL_IMGQA_SHADOW_EDGE_REBASE_H
#define RGL_IMGQA_SHADOW_EDGE_REBASE_H
// shadow-edge-rebase 仪器内部头 —— 四格对拍的读数类型 + 闸门/判决线声明的唯一出处。
// 全部论证（判据为什么从「图集逐位」换成「闭式绝对尺」/ 远点门关态实测是「阴影整块消失」
// 而不是厘米级抖动 / 平移量为什么不烘进 GLB 顶点）住 `shadow_edge_rebase.cpp` 头注释，
// 本文件不重述。
//
// 分 TU（护栏：单文件 < 500 代码行；四切法照 shadow-edge-ruler 与 shadow-caster-survival）：
//   · shadow_edge_rebase.cpp        = 臂表 + 采集 + 引擎回显四查 + 读数装配 + 主表编排
//   · shadow_edge_rebase_judge.cpp  = 闸门 G1/G5 + 判决线 J-R1/J-R2/J-R3 + 像素差打印
//   · shadow_edge_metric.*          = 度量口径层（与 ruler 共用，`just tools-check` 覆盖到的
//                                     唯一一层）
//   · cluster_path_gate.*           = 簇路径生效闸门（复用）
// 🔴 与 ruler 共用的东西一律**不抄第二份**：靶读取（`loadEdgeTarget(stem)`）、闭式可量性
// 预判（`plateMeasurableInCascade`）、级联闭式对账（`requireCascadeModelAgreement`）、
// cov 场（`seamCovField`）、量具常数（`kEdge*`）全部来自 `shadow_edge_ruler.h`。
#include "shadow_edge_ruler.h"
#include "shadow_kernel_metric.h"

#include <opencv2/core.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace RGL::imgqa {

/// 一「格」= (工作点靶, rebase 开关态) 的一整套采集与读数。四格编号 = ①②③④，
/// 顺序在 `cmdShadowEdgeRebase` 里钉死（主表列序与三条判决的取数都按它）。
struct Cell {
    std::string title;    ///< "① 出厂·门关" 一类（打印与 Verdict 文本用）
    std::string stem;     ///< 靶文件名（不含扩展名）
    bool rebase = false;  ///< coordinate.rebase
    EdgeTarget target;
    EdgeCascadeModel model;
    Projector proj { };
    std::vector<cv::Mat> img;                   ///< 与仪器内部臂表同序
    std::vector<std::string> plateNames;        ///< 逐板名（两靶必须同名同序，G1 守）
    std::vector<double> depthM;                 ///< 逐板影边视深（闭式可量性预判的输入）
    std::vector<int> cascadePred;               ///< 逐板预测级联
    std::vector<double> texelMm;                ///< 逐板**本级**纹素 mm（判决线的刻度）
    std::vector<double> errNat;                 ///< 自然臂逐板误差 mm（NaN = 未量到）
    std::vector<std::vector<double>> errPinned; ///< [级 c][板]，同口径
    std::int64_t shadowPx = -1;    ///< none ↔ 自然臂 逐位不同像素数
    bool shadowAbsent = false;     ///< shadowPx == 0（🔴 G6 点名「阴影整块消失」）
    std::vector<std::string> unexpected;  ///< G5 的抛集合（格 × 臂 × 板）
};

/// 远工作点靶的文件名（tools/qa_scenes.py 的 SCENES 键；GLB 与出厂靶逐字节相同）。
inline constexpr const char* kFarStem = "shadow_edge_rig_far";
/// 远工作点平移量的**期望值**（世界 +X 1e6 m）。G1 用它核对 manifest——🔴 不是拿它算真值：
/// 真值只从 manifest 读（第二份真相 = 项目哲学第 2 条）。
inline constexpr double kEdgeFarOffsetM = 1e6;
/// G1 的比对容差（米）。两枚 manifest 都是 Python double 的十进制文本，往返误差 ~1e-10；
/// 取 1e-6 = 一微米，比任何真实几何错位小三个数量级、比文本往返大四个数量级。
inline constexpr double kEdgeShiftTolM = 1e-6;

/**
 * 一板在本格的**界**（mm）= 1 枚该板本级纹素 + 2 枚像素刻度。逐项推导住
 * `judgments_table_edge.cpp` 的 J-R1 容差理由（两态盒心各离真盒心 ≤ 半纹素 ⟹ 三角不等式
 * 给 1 纹素；像素刻度项与 ruler 闸门 4 同口径，不另推）。
 */
[[nodiscard]] double plateBoundMm(const Cell& c, std::size_t p);

/// |误差| 的中位数（NaN 安全：空集合返回 NaN——0 是合法读数，占不了「没数」那个位置）。
[[nodiscard]] double medianAbs(const std::vector<double>& errs);

/// 可量板数（非 NaN 的格数）。
[[nodiscard]] std::size_t measurable(const std::vector<double>& errs);

/// 两格自然臂的像素差（C 部分：**只报数**，阈值由主 agent 定）。
void printPixelDiff(const Cell& a, const Cell& b, const char* what);

/**
 * **G1｜靶同源闸门**（变异红线 ① 的落点）：远点 manifest 的 `worldOffset` 必须 = 常数，
 * 且相机 / sceneBounds / 逐板 farFace·trueEdge·scan 两端必须恰等于出厂靶的对应量 + 平移，
 * 级联闭式必须两靶**相同**（相对量随平移漂了 = 公式链藏着未声明的绝对坐标假设）。
 * 板清单必须同名同序（板序 = 距离序 = 级联序）。任何一条不满足 ⟹ 抛（rc 3，不产 Verdict）。
 */
void requireFarIsTranslatedCopy(const EdgeTarget& base, const EdgeTarget& far,
    const EdgeCascadeModel& baseModel, const EdgeCascadeModel& farModel);

/// **G5｜每块板必须真的量到过**（意外失踪一次报全；四格板数为 0 时同样抛——对空集合的
/// 断言恒真 = 假绿）。
void requireEveryPlateMeasured(const std::vector<Cell>& cells);

/// J-R3 `shadow-edge-rebase-shadow-present`：四格 csm↔none 逐位不同像素数全部 > 0。
[[nodiscard]] Verdict judgeShadowPresent(const std::vector<Cell>& cells);
/// J-R1 `shadow-edge-rebase-no-harm`：出厂点门开逐板 |误差| 不得比门关变大超过界。
[[nodiscard]] Verdict judgeNoHarm(const Cell& off, const Cell& on);
/// J-R2 `shadow-edge-rebase-benefit` 🔴：远点门开把影边精度买回出厂水平（可量 + 界内）。
[[nodiscard]] Verdict judgeBenefit(const Cell& base, const Cell& farOff, const Cell& farOn);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_EDGE_REBASE_H
