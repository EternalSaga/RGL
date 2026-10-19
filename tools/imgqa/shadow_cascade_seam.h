#ifndef RGL_IMGQA_SHADOW_CASCADE_SEAM_H
#define RGL_IMGQA_SHADOW_CASCADE_SEAM_H
// shadow-cascade-seam 仪器内部头 —— 跨 TU 面（量具常数 + 读数类型 + 判决线声明）的唯一出处；
// 全部论证（缺陷类 / 为什么必须差分 / 为什么走交付配置 / 簇路径闸门为什么比 J1 更重要）
// 住 `shadow_cascade_seam.cpp` 头注释，本文件不重述。
//
// 分 TU（护栏：单文件 < 500 代码行；四切法照 shadow_grain）：
//   · shadow_cascade_seam.cpp        = 臂表 + 采集 + 引擎回显对账 + 🔴 簇路径生效闸门 + 入口编排
//   · shadow_cascade_seam_judge.cpp  = 三条判决线（J1 LOD 归因 / J2 阳性锚点 / J3 D 扫描形状）
//   · shadow_seam_metric.*           = 度量口径层（`imgqa selftest` 闭式自检，`just tools-check`
//                                      覆盖得到的唯一一层）
//   · **没有 target TU**：本仪器的区域不是 manifest 投影而是与几何无关的固定带，理由住
//     仪器层头注「区域为什么是固定带而不是几何投影」。
#include "imgqa.h"
#include "shadow_seam_metric.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RGL::imgqa {

// ── 量具常数（改动 = 改判据；每条阈值的出处 = judgments_table_seam.cpp 的容差理由）────────
/// 采集尺寸（必须 == 主 agent 定口径时的取景尺寸；换尺寸 = 换一台仪器，读数不可比）。
inline constexpr int kSeamWidth = 1600;
inline constexpr int kSeamHeight = 900;
inline constexpr int kSeamDelayFrames = 8; ///< 截图帧号（llm_shot.delay_frames 写死）

/// 判据机位 = 债-139 的「第三机位」（用户报障时"正好是变化的瞬间"那一枚）。坐标原文抄自
/// `docs/还债总账.md` 债-139 行；机位朝向由 camera.lua 的 position→target 反推
/// （`rgl/camera_config_impl.cppm` 的「初始位置/朝向段读取」，fps 模式同样消费 target）。
inline constexpr double kSeamCamPosition[3] = { 0.546840, 0.235768, 0.368027 };
inline constexpr double kSeamCamTarget[3] = { 0.314997, -0.172310, 0.085601 };
inline constexpr double kSeamCamFov = 45.0;

/// 台阶带 = **中央 1/2 幅**（列 x ∈ [400, 1200)）× 行 [30, 870]（含两端）。
/// 为什么这个带不是「看着答案挑的窗口」→ 仪器层头注「区域为什么是固定带」。
inline constexpr int kSeamBandX = 400;
inline constexpr int kSeamBandCols = 800;
inline constexpr int kSeamBandRow0 = 30;
inline constexpr int kSeamBandRows = 841;

/// J1 上限：`maxRowStep(Δ|D=5) − maxRowStep(Δ|D=-1) ≤ 0.1`。固有下界 = 「一个纹素的影边
/// 摆放误差」≈ 0.057（推导住 judgments_table_seam.cpp），0.1 = 下界留一倍裕量。
/// 🔴 **今天这条是红的，且红是正确的交付内容**（实测增量 0.379 = 下界的 6.6 倍）。
inline constexpr double kSeamLodGrowthCeil = 0.1;

/// J1 的**固有下界**：LOD 完好时这条台阶也不可能为 0——相邻两级的阴影图纹素粗细不同，同一条
/// 影边在两张图里的摆放最多差一个纹素。本靶 seam 处 texel(c0)=0.506 mm、texel(c1)=1.034 mm
/// ⟹ 允许错位 0.528 mm；该处 cov 梯度 ≈ 0.108 /mm（0.65 跨度 ÷ 6 mm）⟹ 一个纹素折成 cov
/// 台阶 ≈ **0.057**。判决线 0.1 = 它的一倍裕量。
/// ⚠ 这个数是**推导**不是实测读数（实测读数只用来核对真实读数不落在线的中段）：换机位/尺寸
/// 要重推，改出厂 D 也要重推（相邻级纹素比随 D 变）。完整推导住 judgments_table_seam.cpp。
inline constexpr double kSeamTexelFloor = 0.057;

/// **闸门 B 的下界**：每条 csm 臂的有影面积（cov > 0.5 的像素占比，全幅）必须 ≥ 1%。
/// 界是闭式可辩护的，不是"实测 48% 所以钉 1%"：死态（`cov ≡ 0`）的有影面积是**恰 0%**，
/// 本靶实测正常读数在 **48.6% 量级**（主 agent 定口径时记的「有影面积 48.55%」）⟹ 取 1%
/// = 离闭式死态留一个数量级以上、离正常读数留 ~48 倍。⚠ 换机位/换场景要重推（有影面积随取景变）。
/// 完整论证与它防的失效形态住 `shadow_seam_metric.h` 的 `requireCoveragePresent` 声明处。
inline constexpr double kSeamCoverageFloor = 0.01;

/// J1 的两个 D 档：LOD 臂（出厂 D=5）与几何一致参照臂（D=-1 = 关）。
inline constexpr int kSeamLodArm = 5;
inline constexpr int kSeamRefLodArm = -1;
/// J3 的 D 扫描点（`shadow_lod` 语义：-1=关 / 0=阳性对照 L0 臂 / N≥1=uniform 选级 D=N）。
/// D=5 不在这里——它已由 J1 的臂采过，J3 复用（每臂一次引擎进程，能省就省）。
inline constexpr int kSeamSweepD[] = { 0, 3, 7, 11 };

/// 强制选级诊断键的两档 = 级对 c0↔c1。**为什么只取这一对**：主 agent 实测 LOD 的损害精确
/// 落在 c0↔c1（1.290%，是 c1↔c2 / c2↔c3 的 5 倍），机制预测也是它——
/// `rgl/render_list_mdi.cppm:284` 的 `if (c >= 1u && …)` ⟹ **只有级 0 画全量几何**。
inline constexpr int kSeamForceLow = 0;
inline constexpr int kSeamForceHigh = 1;

/// J3 的一个扫描点（D 升序；force=1 是判据列，force=0 是诊断列）。
struct SeamSweepPoint {
    int lod = 0;           ///< D
    double flipForce1 = 0; ///< flipFraction(cov(D,1), cov(-1,1))，全幅
    double flipForce0 = 0; ///< 同上 force=0：c0 恒旧路径 ⟹ **结构性 0**（诊断列，不参与判据）
};

/// 一台仪器一份读数（J1/J2/J3 各自从这里取数，不在判据层重算任何像素）。
struct SeamReadout {
    SeamStep stepLod;          ///< J1：Δ(D=5) 的最硬横向台阶
    SeamStep stepRef;          ///< J1：Δ(D=-1) 的最硬横向台阶（几何一致参照，闭式该为 0）
    double flipLod = 0.0;      ///< J1 辅助读数：c0↔c1 翻转率 @ D=5
    double flipRef = 0.0;      ///< J1 辅助读数：c0↔c1 翻转率 @ D=-1
    std::int64_t diffPxLod = 0;///< J2：D=5 的 force=0 与 force=1 两臂逐位不同的像素数
    std::vector<SeamSweepPoint> sweep; ///< J3：按 D 升序
};

// ── shadow_cascade_seam_judge.cpp ─────────────────────────────────────────────────────
/// J1 `shadow-seam-lod-growth`（**主判据**）：LOD 归因的缝台阶增量 ≤ kSeamLodGrowthCeil。
void pushLodGrowthVerdict(std::vector<Verdict>& vs, const SeamReadout& r);
/// J2 `shadow-cascade-seam-arms-alive`（**阳性锚点，防恒绿**）：D=5 的两 force 臂不得逐位
/// 相同。它红时 J1 的绿一律不作数。
void pushArmsAliveVerdict(std::vector<Verdict>& vs, const SeamReadout& r);
/// J3 `shadow-seam-sweep-monotone`：各 D 相对 D=-1 的翻转率随 D 单调不减且高段饱和。
void pushSweepVerdict(std::vector<Verdict>& vs, const SeamReadout& r);

[[nodiscard]] std::string seamFixed3(double v);
[[nodiscard]] std::string seamFixed4(double v);
[[nodiscard]] std::string seamPct3(double v);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_CASCADE_SEAM_H
