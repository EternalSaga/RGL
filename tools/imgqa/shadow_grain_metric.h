#pragma once
//
// shadow_grain_metric.h — `shadow-grain`（影场颗粒度）的**度量口径层**（覆盖度场 + 二值化跳变密度）
//
//   缝同 `shadow_kernel_metric.h`（「口径 vs 仪器」那条既有缝）：`shadow_grain.cpp`（仪器：三臂
//   采集 + 区域投影 + 槽带在场闸门 + 两条判决线）与本层（口径：两张截图 → 逐像素覆盖度场 →
//   一个密度）不是同一条改动缝——改臂 / 阈值 / 区域动前者，改测量算子动后者，而**只有后者被
//   `just imgqa selftest` 用闭式输入直接测**（不启引擎、毫秒级，故 `just tools-check` 覆盖得到）。
//
//   ## 口径为什么长这样（缺陷本体全文见仪器层头注释，这里只写算子层面的理由）
//   门板自阴影痤疮**钉死在世界表面** ⟹ 世界不变性判据（`imgqa swim`）对它结构性失明——
//   swim 测的性质是对的（世界点影值不变 ⟹ 影子不随相机游），只是不够：它看不见"影场在世界表面
//   上的高频程度"。本口径问的就是那个：给定一块由几何闭式算出的区域，覆盖度的每次跳变都必须
//   对应一条闭式影缘/影带（靶上 = 2·count 条缘 ⟹ 带全在场时 density = 2·count/(cols−1)）；
//   对不上账的反复翻转 = 噪声，不是影子。
//
//   ## 变异红线（哪个变异让我红——全部由 `selftest_grain.cpp` 钉死）
//   · 去掉线性域转换（直接拿 sRGB 字节算 cov）→ 「线性域锚点」红：cov(白,128灰) 闭式
//     0.784061，字节域给 0.497989——差 0.29，不是噪声量级。
//   · 只数单向跳变（0→1 或 1→0，「本地正确的另一半」）→ 棋盘 / 竖条纹两条红（1.0000→0.5000）。
//   · 分母写成 rows×cols → 斜坡红（1/33 → 32/33÷34）。
//   · 删 eps（分母 off+0）→ 「参照全黑」红（0 → NaN，NaN>0.5 为 false 会被读成"没影"，
//     但 NaN 传染进 density 后比较全 false = 整窗读 0 = 缺陷态刷绿）。
//   · 二值化阈值挪离 0.5（如 0.9）→ 「cov=0.75 计影」与「cov=0.5 不计」两条各红一边。
//   · clip 下界删掉（on 比 off 亮的像素给负值）→ 「变亮不计」红（负值 > 0.5 为 false 看似
//     无害，但三通道均值会把「一通道爆负」的像素拉过线，与 kernelscale 的变亮反向同理）。
//   · 逐带跳变计数 grainBandFlips 只数单向（`l != r` → `!l && r`）→ 「带在场存活率恰 1」
//     红（给 0.5——带右缘的 1→0 被吞，与 grainDensity 的单向变异同型、各钉各的函数）。
//   · 存活率分母 (2·rows) 写成 (rows) → 「带在场 = 1.0」红（给 2.0）。
//   · grainBandFlips 的 cols⊂win 守卫删掉 → 「区间越窗」自检红（夹边窗口的「0 跳变」
//     是假绿的一种，与 grainDensity 的越界守卫同罪）。
//
#include "imgqa.h"

#include <opencv2/core.hpp>

namespace RGL::imgqa {

/// 覆盖度二值化阈值（**严格 >**，恰 0.5 不计影——与 kernelscale 的 kUmbraFactor 同口径）。
inline constexpr double kGrainBin = 0.5;

/// 参照下限 eps：off（mode=none）为黑的像素上 (off−on)/(off+eps) 不爆（0/0 → 0）。
/// 黑参照上没有"影"可谈（没光可遮），这条把「分母为 0」与「影覆盖度为 0」压成同一个 0，
/// 而两者在密度算子里的行为一致（不计跳变），不需要再区分。
inline constexpr double kGrainEps = 1e-4;

/**
 * 逐像素阴影覆盖度 cov = mean_RGB clip((off − on)/(off + kGrainEps), 0, 1)，**线性域**。
 * on/off = 同机位同帧、只差 configs/shadow.lua 的 mode = "csm" / "none"（开关对拍，
 * albedo / 曝光 / ACES 全部约掉，剩下的只有阴影项）。
 * 必须先 sRGB 解码再算比值（项目红线「统计一律线性域」；imgqa.h 硬约束第 2 条）。
 * 两图尺寸不同立刻抛（不是同一帧的读数没有意义，同 measureSoftArm 的先例）。
 */
[[nodiscard]] cv::Mat grainCoverage(const cv::Mat& onBgr, const cv::Mat& offBgr);

/**
 * 二值化覆盖度场在窗口 win 内的**水平跳变密度**：
 *     density = |{(x,y) : bin(y,x) ≠ bin(y,x+1)}| / (rows × (cols−1))
 * 闭式刻度：一条影缘横穿窗口 = 每行至多 1 次跳变 ⟹ 上界 1/(cols−1)（缘竖穿全部行时取等）；
 * 周期 2px 的椒盐噪声 ≈ 1.0。区域必须来自几何（仪器层从 manifest 投影），不许手填屏幕矩形。
 * win 宽 < 2 / 越界 / cov 非 CV_64F ⟹ 抛（分母为 0 或读错内存的"0 密度"是假绿，拒绝）。
 */
[[nodiscard]] double grainDensity(const cv::Mat& cov, const cv::Rect& win);

/**
 * bin(cov) 在列区间 cols × 窗口 win 全部行内的**水平跳变计数**（逐行计，不除列数）。
 * 闭式刻度：一条闭式带完整落在 cols 内 = 每行 2（带两缘各跨 0.5 线一次）；被抹 = 0。
 * 与 grainDensity 的分工：那边量「整窗里有多少条影缘」（区域级），这边量「**这一条带**
 * 还在不在」（逐带级）——多缝深靶的存活率判据必须逐带出数，一条缝被抹不能靠其余缝摊平。
 * cols 宽 < 3 ⟹ 抛（宽 2 的唯一相邻对同时是带的左缘与右缘，闭式 2 不成立）；
 * cols 不在 win 的 x 跨度内 / 越界 / 非 CV_64F ⟹ 抛（同 grainDensity 的纪律：
 * 空分母或夹边的「0 跳变」是假绿）。
 */
[[nodiscard]] int64_t grainBandFlips(const cv::Mat& cov, const cv::Range& cols,
    const cv::Rect& win);

/**
 * 带存活率 = grainBandFlips / (2·rows)——**分母是闭式刻度**（带在场每行恰 2 次跳变），
 * 不抄任何实测数字：带在场 = 1、被抹 = 0；带缘噪声双跳变可 > 1（判决线只钉下限，
 * 仪器层 kSurvivalFloor 取两闭式态的中点）。
 */
[[nodiscard]] double grainBandSurvival(const cv::Mat& cov, const cv::Range& cols,
    const cv::Rect& win);

}  // namespace RGL::imgqa
