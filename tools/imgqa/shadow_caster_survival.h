#ifndef RGL_IMGQA_SHADOW_CASTER_SURVIVAL_H
#define RGL_IMGQA_SHADOW_CASTER_SURVIVAL_H
// shadow-caster-survival 仪器内部头 —— 量具常数 + 读数类型 + 判决线声明的唯一出处；
// 全部论证（缺陷类 / 为什么必须是绝对量 / 为什么 shadow-grain 在这里也是瞎的）
// 住 `shadow_caster_survival.cpp` 头注释，本文件不重述。
//
// 分 TU（护栏：单文件 < 500 代码行；四切法照 shadow-cascade-seam）：
//   · shadow_caster_survival.cpp        = 臂表 + 双机位采集 + 引擎回显对账 + 四道闸门 + 出表
//   · shadow_caster_survival_judge.cpp  = J1（INFO 不判）+ J2（单调性判决线）+ 数字格式化
//   · shadow_survival_metric.*          = 度量口径层（`imgqa selftest` 闭式自检，`just
//                                         tools-check` 覆盖得到的唯一一层）
//   · cluster_path_gate.*               = 簇路径生效闸门（与 shadow-cascade-seam 共用）
//   · **没有 target TU**：区域 = 全幅 + 固定近场带，与几何无关，理由住仪器层头注。
#include "imgqa.h"
#include "shadow_survival_metric.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RGL::imgqa {

// ── 量具常数（改动 = 改判据；每条阈值的出处 = judgments_table_survival.cpp 的容差理由）────
/// 采集尺寸（与 shadow-cascade-seam 同 = 主 agent 定口径时的取景尺寸；换尺寸 = 换一台仪器）。
inline constexpr int kSurvWidth = 1600;
inline constexpr int kSurvHeight = 900;
inline constexpr int kSurvDelayFrames = 8;  ///< 截图帧号（与 seam 同，llm_shot.delay_frames）

/// 机位 A `near` = 债-139 第三机位（与 shadow-cascade-seam 同机位——主 agent 的丢失块实测
/// 读数全部出在这一机位，E5 的对账锚要求同取景）。坐标原文抄自 `docs/还债总账.md` 债-139 行。
inline constexpr double kSurvNearPosition[3] = { 0.546840, 0.235768, 0.368027 };
inline constexpr double kSurvNearTarget[3] = { 0.314997, -0.172310, 0.085601 };
inline constexpr double kSurvNearFov = 45.0;
/// 机位 B `far` = **出厂默认机位**（`configs/camera.lua` 顶层 position/target/fovDegrees，
/// `just shot` 用的就是它）。实现 = **不写 camera.lua**（交付配置原样）——把出厂坐标抄进
/// 本文件 = 第二份真相，configs 一改这台仪器就和 `just shot` 分家。

/// 近场带 = 行 [500, 899]（含端点，全幅宽）。主 agent 实测的接触阴影实例（牛腿贴墙、花箱
/// 底投影）全部落在画面下半；行号区间是它定口径时记的读数面，换机位要重定。
inline constexpr int kSurvNearRow0 = 500;
inline constexpr int kSurvNearRows = 400;

/// 闸门 B 的下界（复用 `requireCoveragePresent`，界跟着本靶重推）：闭式死态 = 恰 0%；
/// near 机位实测 48.6% 量级（seam 头注记的 48.55%），far 机位实跑核对（首跑必须看到两条
/// 「有影面积」打印都过界）。取 1% = 离死态一个数量级以上（同 seam 的推导形态）。
inline constexpr double kSurvCoverageFloor = 0.01;

/// 参照臂 D = -1（四级全画原始几何 = 阴影的「应有样子」）；出厂档 D = 5（J1 的读数臂）。
inline constexpr int kSurvRefD = -1;
inline constexpr int kSurvFactoryD = 5;
/// D 扫描点（升序；含出厂 5 与阳性对照 0，参照 -1 不在这里——它是分母不是候选）。
/// 1/2/3 是 seam 没扫过的加密点：J2 的单调性要足够的点才有内容（两点只能连一条线）。
inline constexpr int kSurvSweepD[] = { 0, 1, 2, 3, 5, 7, 11 };

/// 一台机位的一份完整读数表（J1/J2 与打印都从这里取数，不在判据层重算任何像素）。
struct SurvCameraReadout {
    std::string camera;  ///< "near" | "far"（进打印与台账）
    /// 见证臂与参照臂 cov 场逐位不同的像素数（闸门 D 的输入；见证 = d11 + d5，选法与小 D
    /// 合法同档的实测理由住 .cpp 的 requireReferenceGates 注释）。
    std::vector<std::pair<std::string, std::int64_t>> refDiffByArm;
    /// 扫描行（D 升序，含参照行 lod = kSurvRefD；参照行的四项读数 = 自己与自己的恒等）。
    std::vector<int> lods;
    std::vector<double> areaSurvival;  ///< 与 lods 同序（参照行 = 1）
    std::vector<double> nearAreaSurvival;
    std::vector<UmbraBlobStats> fullStats;
    std::vector<UmbraBlobStats> nearStats;
    std::vector<double> flip;  ///< flipFraction(cov(D), cov(ref))，全幅（参照行 = 0）

    /// lod → 行号（找不到 = 抛，判据取数不许静默取到别的行）。
    [[nodiscard]] size_t rowOf(int lod) const;
};

// ── shadow_caster_survival_judge.cpp ─────────────────────────────────────────────────
/// J1 `shadow-caster-survival-factory`（**阈值未定期：只报数不判**，打印 INFO，不出
/// PASS/FAIL、不进 rc）。阈值 TODO(债-139)，理由住本函数定义处与 judgments_table_survival.cpp。
void printFactoryInfo(const SurvCameraReadout& r);
/// J2 `shadow-caster-survival-monotone`：本影面积存活率随 D **单调不增**（每台机位各自判，
/// 全过才绿）。D 点 < 3 ⟹ 抛。
void pushMonotoneVerdict(std::vector<Verdict>& vs, const std::vector<SurvCameraReadout>& cams);

/// 数字格式化（表与 Verdict 文本共用，一台仪器一份）。
[[nodiscard]] std::string survFixed3(double v);   ///< 存活率（"1.005"；NaN → "—"）
[[nodiscard]] std::string survPct3(double v);     ///< 翻转率（"0.994%"；NaN → "—"）
[[nodiscard]] std::string survBucketCell(double survival, std::int64_t alive,
    std::int64_t total);                          ///< 档格："0.875 (21/24)"；空档 → "— (0/0)"

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_CASTER_SURVIVAL_H
