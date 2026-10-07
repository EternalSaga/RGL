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

/// 运动族 · 鬼影尾长与 static boost（8 条）：boost 保真 / 运动激励 / 对齐 / 消融 / 尾配对 /
/// 尾事件 / 盒载荷 / 尾单调。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalTailBoostJudgments();

/// 运动族 · **机位运动**稳态份额（批 13.14a，3 条）：闭式刻度 / 静止塌缩 / gate 断点。
/// 与上面那族的差别 = 成立条件不同（spin 下臂 11 的 `gate` 恒 1.0，进不了斜坡区）。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalTailCameraJudgments();

/// 运动族 · feedback 与闭式回扣（5 条）：参数活性 / 零态 / 回扣 / 闪烁闭式 / 单应解释。
[[nodiscard]] const std::vector<JudgmentInfo>& temporalFeedbackClosureJudgments();

/// 附件族（3 条）：IZB 消费域 / AO 噪点两条。
[[nodiscard]] const std::vector<JudgmentInfo>& attachmentJudgments();

}  // namespace RGL::imgqa

#endif  // RGL_IMGQA_JUDGMENTS_TABLE_H
