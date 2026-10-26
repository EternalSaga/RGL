#ifndef RGL_IMGQA_SHADOW_EDGE_RULER_H
#define RGL_IMGQA_SHADOW_EDGE_RULER_H
// shadow-edge-ruler 仪器内部头 —— 量具常数 + 读数类型 + 判决线声明的唯一出处；
// 全部论证（两台参照渲染仪器的两次翻车 / 为什么真值必须是算术 / 机位为什么在下游）
// 住 `shadow_edge_ruler.cpp` 头注释，本文件不重述。
//
// 分 TU（护栏：单文件 < 500 代码行；四切法照 shadow-caster-survival）：
//   · shadow_edge_ruler.cpp        = 臂表 + 采集 + 引擎回显对账 + 五道闸门 + 主表/缝宽编排
//   · shadow_edge_ruler_judge.cpp  = J1/J2/J3（全部 INFO 只报数不判）+ seamWidths 纯函数
//   · shadow_edge_target.cpp       = manifest measure 块读取 + 扫描线构造 + 级联闭式对账
//   · shadow_edge_metric.*         = 度量口径层（`imgqa selftest` 闭式自检，`just
//                                    tools-check` 覆盖得到的唯一一层）
//   · cluster_path_gate.*          = 簇路径生效闸门（与两台既有仪器共用，复用别重写）
#include "imgqa.h"
#include "shadow_edge_metric.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RGL::imgqa {

// ── 量具常数（改动 = 改判据；每条阈值的出处 = judgments_table_edge.cpp 的容差理由）────
/// 采集尺寸（= 靶 manifest 的 captureSize；不一致 = 抛，生成器的取景 assert 按它算）。
inline constexpr int kEdgeWidth = 1600;
inline constexpr int kEdgeHeight = 900;
inline constexpr int kEdgeDelayFrames = 8;  ///< 截图帧号（与 seam/survival 同，llm_shot 钉帧）

/// 靶文件（tools/qa_scenes.py 生成；簇产物 = 同目录 <glb>.meshlets.bin，缺 = 抛）。
inline constexpr const char* kEdgeGlbStem = "shadow_edge_rig";

/// 自然渲染臂的唯一 D 档 = 出厂档（用户拍板 5）。🔴 原六档 D 扫描（-1/0/3/5/7/11）已删
/// ——本靶上 D 逐位空操作（凸平板剪影不变，`shadow_edge_ruler.cpp` 头注 E2），继续扫 =
/// 演戏。LOD 画质/成本 = 下一台「细附属结构」靶的自变量，别在这里找 LOD 信号。
inline constexpr int kEdgeFactoryD = 5;
/// 钉级臂：force_cascade ∈ {0,1,2,3} × shadow_lod ∈ {5, -1}。D=-1 列 = 闸门 4 的闭式
/// 参照态（LOD 关 = 全量几何进阴影图），不是 LOD 对照。
inline constexpr int kEdgeForceLevels[] = { 0, 1, 2, 3 };
inline constexpr int kEdgeForceD[] = { 5, -1 };
/// kEdgeForceD 里参照态（D=-1）的下标。
inline constexpr int kEdgeRefDIdx = 1;

/// 覆盖度分母下界（闸门 B，复用 `requireCoveragePresent`）。闭式死态 = **恰 0%**（cov ≡ 0
/// 只在「阴影根本没画」时成立；本仪器确定性渲染、none/csm 只差阴影段 ⟹ 噪声地板恰为 0）。
/// 首跑实测（2026-10-20 扫描）：本靶影比设计预估浅得多——地面受光里太阳只占 ~30%
/// （env 1.0 的桥景天光补满），cov 场峰值 0.303（字节域）/ **0.510（线性域，主 agent 复算
/// 修正，p99 0.367、均值 0.004）**；umbra（cov>0.5，线性域）面积逐臂 0.01%（f3）…0.17%
/// （自然臂）。原预估「1–2%」错在拿 bistro 墙面影深（太阳占墙面受光 >50%）套地面。取
/// **0.002%** = 最弱臂的 1/5、死态的 ∞ 倍：它只答「阴影画没画进画面」；影浅由**相对阈值**
/// 口径解决（`shadow_edge_metric.h` E1），不再由本闸门管。
inline constexpr double kEdgeCoverageFloor = 0.00002;

/// 一台机位（= 一枚机位，本仪器只有一枚）的一份完整读数表。
struct EdgeReadout {
    /// 自然渲染（出厂 D）：每板误差 mm（实测过渡中点 − 真值，正 = 影边向下游/相机侧挪）。
    /// NaN = 未量到（表先出；闸门 3 之后必无 NaN——除非闭式预判它预期出块）。
    std::vector<double> errNat;
    /// 钉级：[级 c][D 档 kEdgeForceD 序][板]，同口径误差 mm。NaN = 该格未量到
    /// （预期出块 = 板在该级切片区外，或平台窗退化——闸门 3 按闭式预判分家）。
    std::vector<std::vector<std::vector<double>>> errPinned;
    /// 每板：预测级联 + 该级纹素 mm + 视深 m（打印与判据取数用）。
    std::vector<int> cascadePred;
    std::vector<double> texelMm;
    std::vector<double> depthM;
    std::vector<std::string> plateNames;
    /// 级联闭式模型的全部四枚纹素（mm）——钉级臂的刻度按**钉的那级**取，不按板所在级。
    std::array<double, 4> texelMmByCascade{};
};

// ── shadow_edge_target.cpp（manifest = 测量接口的唯一真相源，C++ 不重抄几何）────────────
/// measure.plates 的一条：真值影边 + 扫描线（世界米参数化）。
struct EdgePlate {
    std::string name;
    double thicknessM = 0.0;
    std::array<double, 3> farFace{};    ///< 板远侧面世界点（y=0，v=0）
    std::array<double, 3> trueEdge{};   ///< 真值影边世界点（y=0，v=0）
    EdgeScanLine scan;                  ///< from = 窗口上游端，to = 下游端
    double sTrue = 0.0;                 ///< 真值影边在扫描线上的弧长（米）
};

/// 靶的全部测量事实（含生成器的级联预测——对账用，不是求值输入）。
struct EdgeTarget {
    Engine::Camera camera;
    std::array<int, 2> capture{};
    std::array<double, 3> boundsCenter{};
    double boundsRadius = 0.0;
    /// 🔴 整靶的世界平移（manifest 的 `measure.worldOffset`；**缺键 = 零** = 出厂靶）。
    /// 它是**闭式真值 / 机位 / sceneBounds 共用的那枚世界平移**，也是「远工作点」与
    /// 「出厂工作点」的唯一区别。为什么由 manifest 给而不是在 C++ 里另算一份：manifest =
    /// 场景生成器留给测量方的唯一读数接口；真值与平移量分两处真相 = 项目哲学第 2 条点名的形态。
    std::array<double, 3> worldOffset{};
    /// 🔴 每个 piece 烘在 GLB node 里的平移（manifest 的 `measure.nodeTranslation`；缺键 = 零）。
    /// 债-159：仪器写进 `scene.lua` 的 entity `position` 是 **`worldOffset − nodeTranslation`**
    /// （node 已经替 piece 自己挪掉了这一截，两根平移相加才是 worldOffset）。为什么靶要带它：
    /// node 矩阵是单位阵时 `absolute = root · I = root` 在任何数值域都成立 ⟹「逐图元复合改在
    /// double 域乘」那条链图像上不可见。它同时是**亚格**的（小于工作点处 float32 ULP 的一半），
    /// 所以一旦有人把复合改回 float 域乘，这一截就整个消失。
    std::array<double, 3> nodeTranslation{};
    /// 沿**光的水平行进方向**的单位向量（manifest 的 `measure.u`，只有二维）。债-159：把「世界
    /// X 上诱导的位移」换算到「绝对尺真正在量的那条轴」就靠它——写死系数 = 光向一改就静默失效。
    std::array<double, 2> uDir{};
    std::array<double, 4> splitsPredicted{};
    std::array<double, 4> texelMmPredicted{};
    std::vector<EdgePlate> plates;
};

/// 读靶：缺 .glb / .manifest.json / .meshlets.bin = **抛**（静默跳过 = 假绿；缺簇产物 =
/// 不进簇路径 = 本仪器恒绿的形态，闸门 ① 的运行期兜底之前先给可归因的构建期指路）。
/// captureSize 与量具常数不符 = 抛（生成器的取景 assert 按它算）。
/// `stem` = 靶文件名（不含扩展名）。默认 = 出厂靶；`shadow-edge-rebase` 用它读远工作点靶，
/// 两枚靶**都从各自的 manifest 拿真值**，C++ 侧不另算一份（第二份真相 = 项目哲学第 2 条）。
[[nodiscard]] EdgeTarget loadEdgeTarget(const std::string& stem = kEdgeGlbStem);

/// 对账：仪器闭式（`edgeCascadeModel`）vs 生成器预测（manifest.cascadeModel）——同一组
/// 输入的**两个独立实现**，相对偏差 > 1% ⟹ 抛（有一边的公式链复刻漂了；在哪个数上求值
/// 从此不可信，先停下）。
void requireCascadeModelAgreement(const EdgeTarget& t, const EdgeCascadeModel& m);

/**
 * 闭式预判：板（视深 depth）在钉级/自有级 c 里**量得到影边**吗？判据本体与两条结构条件
 * （片区外 = 出块旁路 / 近平面 1 纹素精度带内 = 伪影平台）住本函数定义处注释。
 * 🔴 抬成跨 TU 面的理由 = 别抄第二份：`shadow-edge-rebase`（四格对拍）要把「量不到」分成
 * 「预期不可量」与「意外失踪」两格，用的就是这条判据；在两台仪器里各写一遍 = 两个会各自
 * 漂的「什么算可量」。
 */
[[nodiscard]] bool plateMeasurableInCascade(const EdgeCascadeModel& m, double depth, int c);


// ── shadow_edge_ruler_judge.cpp ─────────────────────────────────────────────────────
/// 缝宽纯函数（J2/J3 的共同取数面，selftest 闭式直测）：
/// `seams[交界 c][板] = |errPinned[c][0][p] − errPinned[c+1][0][p]|`（出厂 D=5 列；
/// 任一侧 NaN ⟹ 该格 NaN = 不可量，**不猜**）。板清单为空 ⟹ 抛（对空集合的断言恒真）。
/// 🔴 绝对值在做差之后取——带符号做差再取绝对值与逐格取绝对值再差**不同**，
/// 变异红线钉在 selftest（反号同缝宽应同读数）。
[[nodiscard]] std::vector<std::vector<double>> seamWidths(const EdgeReadout& r);
/// J1 `shadow-edge-absolute-error`：每板每级 |误差| 与**本级纹素尺寸**并排（INFO，不进
/// rc）。🔴 阈值 K 本批不定（TODO 见函数定义处）——这张并排表就是定 K 的推导材料。
void printAbsoluteErrorInfo(const EdgeReadout& r);
/// J2 `shadow-edge-seam-width`：🔴 **本靶头条交付数** = 缝宽毫米（逐板 + 中位 + 最大，
/// 逐交界）。= 债-139「交界带混合该开多宽」的设计输入。N 未定 ⟹ 只报数不判（INFO）。
void printSeamWidthInfo(const EdgeReadout& r);
/// J3 `shadow-edge-cascade-agreement`：seam / max(两纹素) 的无量纲比值（INFO，不进 rc）。
/// 🔴 N 未定不判；N 的推导两头住 judgments_table_edge.cpp（固有下界 + 混合方案容忍度）。
/// 原「误差随 D 单调」（shadow-edge-monotone）退役：本靶上 D 逐位空操作，前提不存在。
void printCascadeAgreementInfo(const EdgeReadout& r);

/// 数字格式化（表与 Verdict 文本共用，一台仪器一份）。
[[nodiscard]] std::string edgeMm(double v);    ///< "−12.345"；NaN → "—"
[[nodiscard]] std::string edgePct(double v);   ///< "1.23%"；NaN → "—"

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_EDGE_RULER_H
