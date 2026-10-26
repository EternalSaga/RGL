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

/// 远工作点靶的文件名（tools/qa_scenes.py 的 SCENES 键；GLB 的**顶点缓冲**与出厂靶逐字节
/// 相同，差的只是每个 node 多一枚 `translation`，见 `EdgeTarget::nodeTranslation`）。
inline constexpr const char* kFarStem = "shadow_edge_rig_far";

/**
 * 🔴 远工作点的 **float32 网格事实**（债-159 的运行期判据；全部从 manifest 派生，不抄米数）。
 *
 * **这里曾经有一份抄本**：`inline constexpr double kEdgeFarOffsetM = 1e6;`，G1 拿它核对
 * manifest 的 `worldOffset`。生成器把远工作点挪出 float 网格之后，那份抄本会做的唯一一件事
 * 是**抛**（"靶给的工作点与量具设计的工作点不是同一枚"）——它不产出任何真值（真值一直只从
 * manifest 读），却要求两侧同步改数 = 项目哲学第 2 条点名的第二份真相。本债的验收正是
 * 「发现任何一份抄本就地改成从 worldOffset 派生」，所以删掉它，改钉**性质**：
 *   ① 平移是纯 +X（y/z 必须为 0，否则地面接收关系破，qa_scenes 的 assert ⑪ 同口径）；
 *   ② 进 `rootTransformOf` 的那枚数（= `worldOffset − nodeTranslation`，**不是** worldOffset）
 *      必须**不在 float32 网格上**——网格上的工作点没有亚格小数可丢，「绝对层多过一遍 float」
 *      这类回归在图像上根本不存在（实测：1e6 恰可精确表示 ⟹ 注入该变异后两台仪器的报告与
 *      不注入时**逐字节相同**）。
 * 而「圆走的量够不够大」这条**不在这里判**：界（1 枚本级纹素 + 2 枚像素刻度）是本命令的
 * 判据，只有 `plateBoundMm` 算得出，所以它住 `requireFarGridHasTeeth`（同文件，取 Cell）。
 */
struct FarGridFacts {
    std::array<double, 3> entity{};  ///< 写进 scene.lua 的 entity position = worldOffset − nodeTranslation
    double rounded = 0.0;            ///< entity[0] 的 float32 圆后值
    double inducedM = 0.0;           ///< rounded − entity[0]（米；带符号）
    double ulpM = 0.0;               ///< entity[0] 所在量级的 float32 ULP（米）
    [[nodiscard]] bool onFloatGrid() const { return rounded == entity[0]; }
};

/// float32 圆后值（IEEE754 binary32，round-to-nearest-even）。与生成器的 `_f32` 同语义——
/// 两侧各一份是**没办法**的（Python 与 C++ 之间没有共享常量层），但两份都只做一件事、
/// 且都被各自的断言钉住（生成器 assert ⑫ / 本命令 G1），不是可以各自漂移的公式链。
[[nodiscard]] double roundToFloat32(double v);

/// 从 manifest 求远工作点的网格事实（🔴 纯派生：入参只有 worldOffset 与 nodeTranslation）。
[[nodiscard]] FarGridFacts farGridFacts(const EdgeTarget& far);

/**
 * 仪器该往 `scene.lua` 写的 entity `position`（🔴 唯一一份减法，两台仪器共用）。
 *
 * `worldOffset` 是**整靶的世界平移**（闭式真值 / 机位 / sceneBounds 都吃它），而 GLB 的每个
 * node 已经自带 `nodeTranslation`，所以根平移要少挪这一截：`entity = worldOffset − nodeTranslation`。
 * 忘了减 = 整靶在世界里多挪一截，而真值不认 ⟹ 逐板误差凭空多 |nodeTranslation|（本靶 62.5 mm
 * = 最紧逐板界的 2.5 倍，J-R2 会红得很像「rebase 没买到精度」，其实是仪器自己算错了落位）。
 */
[[nodiscard]] inline std::array<double, 3> entityPositionOf(const EdgeTarget& t) {
    return { t.worldOffset[0] - t.nodeTranslation[0], t.worldOffset[1] - t.nodeTranslation[1],
        t.worldOffset[2] - t.nodeTranslation[2] };
}
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
 * **G1｜靶同源闸门**（变异红线 ① 的落点）：远点 manifest 的 `worldOffset` 必须非零、纯 +X、
 * 且 `worldOffset − nodeTranslation` 不在 float32 网格上（债-159，🔴 不核对米数——米数抄本
 * 就是本债要抓的第二份真相），且相机 / sceneBounds / 逐板 farFace·trueEdge·scan 两端必须
 * 恰等于出厂靶的对应量 + 平移，
 * 级联闭式必须两靶**相同**（相对量随平移漂了 = 公式链藏着未声明的绝对坐标假设）。
 * 板清单必须同名同序（板序 = 距离序 = 级联序）。任何一条不满足 ⟹ 抛（rc 3，不产 Verdict）。
 */
/**
 * **G1a｜远工作点的数值性质**（债-159 本体，从 `requireFarIsTranslatedCopy` 拆出——两条的改动缝
 * 不同：本条随「靶的取值怎么选」变，那条随「闭式真值的形状」变）。钉三条性质：纯 +X、
 * 进 `rootTransformOf` 的那枚 entity position **不在 float32 网格上**、node translation 非零且
 * **亚格**（< 该量级 ULP 的一半）。任何一条不满足 ⟹ 「大坐标精度」这一类缺陷对图像判据结构性
 * 不可见，四格的绿不作数（实测：旧工作点 1e6 恰可精确表示，注入该回归后两台仪器的报告与
 * 不注入时逐字节相同）。
 */
void requireFarWorkingPointProperties(const EdgeTarget& far);

void requireFarIsTranslatedCopy(const EdgeTarget& base, const EdgeTarget& far,
    const EdgeCascadeModel& baseModel, const EdgeCascadeModel& farModel);

/**
 * **G1b｜远工作点的「牙齿」闸门**（债-159 的承重判据，只有本命令算得出界，故不在 G1 里）。
 *
 * 判据：网格外工作点**诱导出的位置误差**（`|float32(entity) − entity|` 投到测量轴 u 上）必须
 * ≥ 2 × 本格最紧的逐板界（`plateBoundMm`）。红 = 这台绝对尺按构造量不出「大坐标精度」这一类
 * 缺陷，此时 J-R2 的绿不作数。
 *
 * 🔴 为什么必须有它，而不是有 G1 的「不在网格上」就够：**「网格外」是必要条件，不是充分条件**。
 * 本机实测（2026-10-26）：1e6 量级 float32 的 ULP = 0.0625 m ⟹ 最大可诱导误差 ULP/2 = 31.25 mm，
 * 而误差住在世界 X 上、绝对尺量的是沿 u 的影边（投影系数 0.7433）⟹ 最多 23.2 mm，**够不到**
 * 最紧的逐板界 **18.57 mm**（plate_t300，本命令自己打印）。1.25 倍不是「能量出来」：未注入变异
 * 时基准自身的 |err④ − err①| 实测已达 **12.86 mm = 界的 69 %**（共模偏置 + texel 吸附相位差），
 * 信号与它可加可消 ⟹ **不可判**。任务书建议的「1e6 + 0.3」更差：诱导 12.5 mm、投到 u 9.3 mm =
 * 界的 0.50 倍，**必然读绿**。把这条写成闸门，是为了让「改了工作点但白改」当场出声，而不是等
 * 下一批再立一次债-159。实跑：把 manifest 的 nodeTranslation 改成 −0.08（诱导只剩 10 mm）⟹
 * 本闸门抛 rc 3；改回 0.0625（诱导 117.5 mm = 界的 4.7 倍）⟹ 过。
 */
void requireFarGridHasTeeth(const Cell& base, const EdgeTarget& far);

/// 本格最紧的逐板界（米）= min_p `plateBoundMm(base, p)`。G1b 的判据本体与主命令的打印共用
/// 这**一份**算法（在打印里另算一遍 = 会与闸门各自漂的第二份真相）。空板集返回 NaN。
[[nodiscard]] double tightestPlateBoundMm(const Cell& base);

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
