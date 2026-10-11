// 时域**绝对**残差判据（批 13.1 主判据）——`imgqa temporal` 的盲区补账。
//
// ## 为什么现有四条全绿还不够（本命令存在的全部理由）
//
// `temporal` 的抑制判据是**相对比值** `S1/S2 ≥ 3.0`（闭式 √16 = 4.0）。相对量有一个结构性
// 盲区：它不约束「收敛后逐帧残差的**绝对大小**」。实测同一份输出里 TAA 开仍有 **20.78% 像素
// 逐帧变化、最大摆幅 0.535**（线性 luma = 136 显示 LSB），四条判据一条都没红。用户报的症状
// （bistro 静止机位下 1px 宽高亮细结构持续闪烁）正好住在这个盲区里：抑制比好 = 每个相位上的
// 估计比不滤时干净，但**残余摆动仍然肉眼可见**。
//
// 本命令把「收敛后还在摆多少」变成绝对读数：静止机位连拍折成 `(2, 16, H, W)`（两半同列 =
// 同一绝对抖动相位），逐像素 `swing = max_over_16 − median_over_16`，报 meanSwing / p99.9 /
// `frac(swing > τ)`，τ = 一个显示 LSB 在靶白电平处的线性跨度（闭式，见 `lsbLinearAtLuma`）。
// 另加一条**收敛正确性**：线性域梯度能量比 `grad(TAA 收敛连拍的相位平均) / grad(fb=1 一周期的
// 相位平均)`，分母 = 「理想 16 相位平均」（fb=1 时输出逐位 = 本帧光栅，按构造成立）。
//
// ## 五组采集（全部 aa_target 细线靶 + 静止机位，只差被测项本身）
//
//   R0  TAA 关                → **确定性地板**：连拍必须逐字节相同（不是接近相同）
//   R1  TAA 开 fb=1  js=1     → **阳性锚点**：只抖不滤 = 闪烁本体，残差必须 ≫ 0；
//                               同时它的相位平均 = 收敛正确性判据的**参考图**
//   R2  TAA 开 fb=.12 js=1    → 被测量（现行工作点）
//   R3  TAA 开 fb=.12 js=0    → **反向用例**：抖动是激励源，掐了残差必须恰为 0
//                               （不掐零 = 量具坏了，R2 的读数全部作废）
//   R4  TAA 开 fb=.002 js=1   → **分辨力轴**：等效窗 1/α = 500 帧 ≫ 抖动周期 16，
//                               残差必须显著低于 R2。不单调 = 量具无效
//
// 三条锚点（缺一条就是假绿）：R1 阳性（≫0）、R0 地板（恰 0）、R3 反向（恰 0）。
// 度量定义与三条测量红线（线性域 / 热身 ≥6/α 且**不写盘** / 配置须带 environment 段）的完整
// 理由 = `temporal_residual.h` 头注释，勿只看这里。
//
// ## 为什么折叠口径不需要标定相位（与 `taa-swim` 的分工）
//
// `taa-swim` 量「相邻帧之间是不是整幅平移」，必须知道「采集第 i 帧 = 引擎抖动序号几」，那个
// 常数相位每次启动都不一样（RPC 登记落在第几帧由启动时序决定），它烧掉两版量具才做对标定。
// 本命令**按 16 折叠**：两半各含整数个周期 ⟹ 同一列 = 同一绝对相位，与常数相位偏移无关。
// 代价是量不到「摆的方向」，只量到「摆多大」——绝对残差本来就不需要方向。
#include "temporal_residual_metric.h"
//
// ## 本 TU 只剩命令入口
//
//   原文件 1908 代码行（工具侧上限 500）被护栏拆成八个 TU，缝表与拆的理由写在
//   `temporal_residual_metric.h` 头注释。上面两段（「为什么现有四条全绿还不够」与「为什么折叠口径
//   不需要标定相位」）是本命令的存在理由，留在入口，别去别的 TU 找。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

int cmdTemporalResidual(int periods, double warmMult, const std::string& variant,
    double slowFeedback, bool skipSlow, bool baseSpace, bool windowAbl, bool bistro,
    const std::string& camera, double longFeedback, bool diagBsOff, bool reject, bool clipAbl,
    double clipMode, double clipSoft)
{
    if (const int rc
        = validateResidualArgs(periods, bistro, windowAbl, reject, clipAbl, longFeedback);
        rc != 0)
        return rc;
    // 批 13.11 消融臂：`--clip <mode>` 是透传（两条路径都吃）。
    const ClipArm arm{clipMode, clipSoft};
    // 批 13.7b 变体分岔（默认不进，现行 7 条判据路径逐字不变）
    if (bistro) {
        if (clipAbl)
            return runBistroClipAblSweep(
                periods, warmMult, camera, longFeedback, clipMode, clipSoft);
        return runBistroSweep(periods, warmMult, camera, longFeedback, diagBsOff, reject, arm);
    }
    // 批 13.7 变体分岔（`--long-feedback` 显式给出才进，现行 7 条判据路径逐字不变）
    if (windowAbl)
        return runWindowSweep(projectRoot() + "/build", periods, warmMult, variant, baseSpace,
            longFeedback, arm);
    return runTargetResidual(periods, warmMult, variant, slowFeedback, skipSlow, baseSpace);
}

}  // namespace RGL::imgqa
