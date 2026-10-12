// 判据契约表 · **差异形态（diffblock）段**（`diffblock-thickness`，1 条，★0.5-b1）。
//
// 从 `judgments_table.cpp` 另立 TU 的理由与 `judgments_table_ssr.cpp` 同一条：主表撞
// 「单文件 < 500 代码行」护栏，缝按**仪器**切——本条属于 `imgqa diffblock` 这一台算子
// （簇化轴 ★1-a1ii 验收判据 A3/A4 的形态闸门，docs/mdi_plan.md §11.2，2026-10-12 拍板）。
// 族仍是 **Foundation**：两张静态图，不涉时域比值口径；成立条件 = 「差异集处处够薄」。
// ⚠ 拼接顺序只在 `judgments_table.cpp` 的 registeredJudgments()，本文件不抄条数。
#include "judgments_table.h"

namespace RGL::imgqa {

const std::vector<JudgmentInfo>& mdiDiffBlockJudgments()
{
    static const std::vector<JudgmentInfo> kPart = {
        {"diffblock-thickness", JudgmentFamily::Foundation,
            "线 = **U(r) 恰 0**（默认 r=2 ⟺「差异处处厚度 ≤ 4 像素」= 用户口径「UV 缝边缘有"
            "差异我能接受，整块不行」的形式化，docs/mdi_plan.md §11.2）。影响形态判据只认这一"
            "种形态：**给容差 = 允许整块漏**（同 `switch-impact-domain` 的「域外恰为 0」）。\n"
            "· U = #{ distanceTransform(M, DIST_L2, 3) > r }，M = {|Δluma| ≥ τ·2.2e-5}（线性亮度）。"
            "实现走距离变换而非字面 erode 是为了**一次计算同时给厚度分布与 U（同口径）**；它与 "
            "`erode(MORPH_ELLIPSE, 2r+1)` 的面积等价性由 selftest 钉（相对容差 10%：椭圆结构元"
            "是半径 r+0.5 的格点盘 + maskSize=3 是 (1,√2) 近似，曲边形状实测最大定义差 8.8%；"
            "「半径当直径」类粗错差 ≥21%，容差吞不掉——两面都有实测数）。\n"
            "· τ 默认 **1 个 sRGB 量化步长**（最小可量化亮度台阶 0.0722·srgbDecode(1/255) "
            "≈ 2.2e-5，与 `diffstruct` 的 ε 口径同常数）；τ=0（任一位不同）的计数**同时报**——"
            "两数之差 = 字节不同但不足一个亮度台阶（交付颗粒噪声），它不进判据。\n"
            "· **本条只判「差异里有没有整块」，不判差异在哪、也不判差异大小**。整块错的主判据"
            "是零阈值的 gbuffer 逐字节对拍（§11.1 A2）；本条是最终图像/法线这类**有损通道**上"
            "的形态兜底。⚠ 设计边界：自然图像整体平移 1px 只产生 1–2px 薄带 → 本条 PASS，"
            "那种「整块错」由 A2 抓，别指望本条。",
            {"docs/mdi_plan.md §11.2（2026-10-12 主 agent 拍板：腐蚀测「有没有内部」，"
             "U==0 ⟺ 差异处处厚度 ≤ 2r）+ §11.4 A3/A4（本条的使用位置）",
             "tools/imgqa/selftest_block.cpp 五条阳性锚点：1px 线 PASS / 20×20 块 FAIL 且 bbox "
             "报对 / 3px 线 r=1 FAIL r=2 PASS（钉 r=半径）/ 对角阶梯线 PASS（8-连通）/ 整幅平移 "
             "1px FAIL（全图级差异必须红）",
             "tools/imgqa/selftest_block.cpp erode↔distanceTransform 等价性自检 + 「容差吞不掉"
             "直径错」反向用例（防实现静默换判据语义）"}},
    };
    return kPart;
}

}  // namespace RGL::imgqa
