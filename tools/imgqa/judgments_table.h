#ifndef RGL_IMGQA_JUDGMENTS_TABLE_H
#define RGL_IMGQA_JUDGMENTS_TABLE_H
// 判据契约表（按族分组）—— registeredJudgments() 的组成部分。
//
// 族 = **成立条件的分组**（枚举与理由 → imgqa.h 的 JudgmentFamily）。为什么按族拆：43 条
// 判据曾是一条扁平清单，而两族时域判据的比值口径不同（静止族要求平均视点恒定，运动族的
// 视点真的在动），扁平清单把这件事藏进了 564 行散文里。
//
// 为什么是「两个 .cpp + 本头」而不是一个文件：表体合计 558 代码行 ⟹ 单文件撞 tools/ 侧
// 「单文件 < 500 代码行」；而最大的两族（TemporalStatic 198 / TemporalMotion 269）单函数
// 撞「单函数 < 120 代码行」。于是族内再按语义段切函数（每段 < 120）。
// **拼接顺序只有一处** = judgments_table.cpp 的 registeredJudgments()，= 拆分前的 43 条现序。

#include "imgqa.h"

namespace RGL::imgqa {

/// 地基族（8 条）：可见集 / 逐面闭式解 / 开关影响域 / 恒等式 / 确定性 / 探针 / 影场不变量。
[[nodiscard]] const std::vector<JudgmentInfo>& foundationJudgments();

/// 静止机位族 · 锚点与恒等式（6 条）：闪烁锚 / 抗锯齿存在性 / 抑制比 sqrt(N) / 游动锚 /
/// 零抖动恒等式 / 拟合器锚。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalStaticAnchorJudgments();

/// 静止机位族 · 抖动传递与基空间（7 条）：抖动传递 / 游动像素 / 基空间 / 相位标定（两条）/
/// 判别力 / 抗锯齿覆盖率保真。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalStaticJitterSpaceJudgments();

/// 运动族 · 残余（6 条）：相位折叠 / 锚点 / 抖动关断 / 幅度 / 单调 / 收敛。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalResidualJudgments();

/// 运动族 · 鬼影尾长与长短窗（4 条）：窗抑制保真 / 运动激励 / 对齐 / 消融。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalTailBoostJudgments();

/// 运动族 · 尾长仪器护栏（4 条）：尾零锚 / 尾事件 / 盒载荷 / 尾单调。
/// 四条全绿只说明「尺子有
/// 力气」，**不**构成画质结论（每条正文末尾各写着这句）。
/// 📄 正文在 `judgments_table_tail.cpp`（`temporal-tail-*` 三段同处一文件）。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalTailGuardJudgments();

/// 运动族 · **机位运动**稳态份额（批 13.14a，4 条）：φ 符号标定 / α 非劣性 / 静止塌缩 /
/// gate 断点。与上面两族的差别 = 成立条件不同（spin 下臂 11 的 `gate` 恒 1.0，进不了斜坡区）。
/// 📄 正文在 `judgments_table_tail.cpp`。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalTailCameraJudgments();

/// 运动族 · **平移阶跃**残影衰减（批 13.15，5 条）：零锚 / 历史存活 / 闭式锚 / 裁剪兑住没有 /
/// 臂 13 修法验收。与上面那条的差别 = **激励带视差**（`frame_yaw` 是原地纯旋转 ⟹ 无去遮挡 ⟹
/// 量不到鬼影），本族是本项目第一条真正量「让开的内容回不来」的判据。
/// 📄 正文在 `judgments_table_tail.cpp`。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalTailStepJudgments();

/// 运动族 · feedback 与闭式回扣（5 条）：参数活性 / 零态 / 回扣 / 闪烁闭式 / 单应解释。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalFeedbackClosureJudgments();

/// 附件族（3 条）：IZB 消费域 / AO 噪点两条。
[[nodiscard]] const std::vector<JudgmentInfo>& attachmentJudgments();

/// 地基族 · **CSM PCF 核宽单位护栏**（`shadow-kernel-scale`，3 条）：CSM 必须有影 / CSM 的
/// 半影宽度对间隙必须平 / 靶必须分得出 PCSS 的接触硬化（阳性锚点）。族仍是 Foundation
/// （不涉时域比值口径），缝按**仪器**切。📄 正文在 `judgments_table_shadow.cpp`。
[[nodiscard]] const std::vector<JudgmentInfo>& shadowKernelScaleJudgments();

/// 地基族 · **湿面/SSR 结构差分**（`diffstruct`，2 条）：变暗阳性锚点（减法在不在）/ 域外变暗
/// 恰 0（减法有没有漏出湿区）。族仍是 Foundation（两张静态图，不涉时域比值口径），缝按**仪器**切。
/// 📄 正文在 `judgments_table_ssr.cpp`。
[[nodiscard]] const std::vector<JudgmentInfo>& ssrDiffStructJudgments();

/// 地基族 · **差异形态判据**（`diffblock`，1 条）：U(r) 恰 0 = 「差异处处够薄，没有整块」
/// （docs/mdi_plan.md §11.2，簇化轴 ★1-a1ii 的 A3/A4 形态闸门）。族仍是 Foundation，缝按
/// **仪器**切。📄 正文在 `judgments_table_block.cpp`。
[[nodiscard]] const std::vector<JudgmentInfo>& mdiDiffBlockJudgments();

}  // namespace RGL::imgqa

#endif  // RGL_IMGQA_JUDGMENTS_TABLE_H
