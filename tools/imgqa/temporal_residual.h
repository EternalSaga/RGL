#pragma once
// 时域**绝对**残差的纯度量层（批 13.1 主判据的固化；无引擎、无 IO、除 OpenCV 外无依赖）——从
// temporal_residual.cpp 拆出来的唯一理由与 `ao_metrics.h` 同：**判据的算子要能被合成靶自检**。
// 留在匿名 namespace 里 selftest 就够不着它们，「判据工具自己要有自检」会退化成空话。
//
// ## 这条量具与 `temporal` 现有四条的分工（勿混）
//
// `imgqa temporal` 的抑制判据是**相对比值** `S1/S2 ≥ 3.0`（闭式 √16 = 4.0）。相对比值有一个
// 结构性盲区：**它不约束"收敛后逐帧残差的绝对大小"**——同一份输出里 TAA 开仍有 20.78% 像素
// 逐帧变化、最大摆幅 0.535（线性 luma，= 136 显示 LSB），而四条判据全绿。本层量的就是那个
// 绝对量：**静止机位收敛后，同一个绝对抖动相位上像素还会摆多少。**
//
// ## 度量定义（口径写死，改口径 = 改判据）
//
// 采集：静止机位连拍 `periods × 16` 帧，折成 `(2, 16, H, W)`——两半各含整数个抖动周期，
// 于是**两半的同一列落在同一绝对抖动相位**（Halton 周期 16，见 `rgl/taa_jitter.cppm`）。
// 逐像素 `swing = max_over_16 − median_over_16`（先按半折叠成 16 列）。
//
// 为什么是 `max − median` 而不是 `max − min`：`max − min` 被单个离群相位主导（一根杆恰好在
// 某一相位整列翻转就把整个像素记成满幅），`max − median` 量的是「最亮相位离典型相位多远」，
// 对单相位离群稳健、对"每相位都在摆"敏感。为什么不是逐帧相邻差：那是 `temporal` 已经量过的
// 轴，且被 L1 守恒骗（反模式 `screen-space-l1-frame-diff`）；**同相位折叠**把「帧序」整个摘掉，
// 剩下的只可能是「同一视点的不同采样相位给出的不同估计」= 闪烁本体。
//
// ## ⚠ 三条测量红线（违反即测量无效）
//
//  1. **所有统计必须在 sRGB 解码后的线性 luma 域做**（`temporal.cpp` 头注释同款纪律）。
//     实测混淆量级（aa_target，fb=0.12 工作点，本命令自己两个域同时算，见 .cpp 的 `byteGrad`）：
//     同一份数据**线性域**梯度比 0.8925、**显示空间**给 0.771——域选错会把「比理想平均糊 11%」
//     读成「糊 23%」。原因：TAA 在**线性 HDR 域**累积，而截图是 ACES + sRGB 之后 ⟹
//     `mean(tonemap(x)) ≠ tonemap(mean(x))`，高对比细线正是偏差最大处。⚠ 长窗（fb=0.002）线性域
//     只给 0.6677，与 `docs/phase13_plan.md` §1.5 在 bistro 上记的 0.671 撞数——**别当成同一件事**，
//     一个是合成靶长窗、一个是 bistro 工作点（该文档由主 agent 记账，本工具不改它，只留指针）。
//  2. **热身 ≥ 6/α 帧**（EMA 瞬态按 (1−α)^t 衰减；α=0.01 要 600 帧）。热身**只空转不写盘**
//     （`Engine::advanceFrames` 读 RPC 帧计数推进）——用 `/shotburst` 热身 = 写 warm 张 PNG，
//     α=0.002 要 3000 张直接超时；热身不够量到的是**瞬态**（历史还没动起来），不是稳态。
//  3. **配置必须带 `environment` 段**，否则 aa_target 整帧全黑（实测 mean=0.000）而四条判据
//     全绿（全零也满足「残差为 0」）。`makeProfile` 写 scene.lua 时本来就带（engine.cpp 的
//     `environment = { ... }` 行）；**另起配置生成路径务必别丢**。本命令另有在场性守卫
//     （`meanLuma > 0` 且 id 面非空）兜这一条。
//
// ## 可复现性（两轮独立运行实测 = 定阈值时余量的依据）
//
// 被判的四个数（meanSwing / p99.9 / frac / halfGapMax）两轮差 **≤0.3%**，但**不是逐位**。根因不在
// 渲染（R0 / R3 两条锚点逐位为 0，已排除引擎噪声），而在**区域**：objectID 是在连拍之外单独一拍
// 取的，那一拍落在哪个抖动相位是随机的 ⟹ 靶轮廓随相位移动 ≤0.5px ⟹ 外扩 3px 的 mask 边界差一条
// 1px 带（三轮 mask 面积实测 246382 / 246045 / 245751）。要彻底消掉得对全部 16 个相位取并集，而
// `/shotburst` 的 burst 分支只支持按 gbuf 通道连拍、不支持按附件名取 object_id（见
// `rgl/debug_rpc_exec_impl.cppm`），那要动引擎侧——本命令的纪律是**不动引擎**，故如实记录、
// 阈值最小余量取 3%。
// ⚠ 唯一浮动显著的是**只打印不判**的「完全不平均的单帧」参照：它取连拍末帧，末帧相位随 burst
// 起点浮动 ⟹ 5 次运行跨 5%（1.031–1.083）。它进不了判据，也正是收敛正确性的上界只能取 1.02
// （门关退化下沿 1.031 之下）的原因。
//
// 定义在 `temporal_residual.cpp`；本头只放声明与类型（默认参数只写接口）。
#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>

#include "imgqa.h"  // Frame（luma = sRGB 解码后的 Rec.709 线性亮度）

namespace RGL::imgqa {

/// 抖动周期（帧）——与 `rgl/taa_jitter.cppm::kTaaJitterPeriod` 同值（imgqa 不链 rgl 库，
/// 同式重抄；先例 = `temporal.cpp::kTaaJitterPeriod`）。漂移由本命令的 R1 半折叠锚点抓：
/// 周期若不再是 16，「两半同列 = 同相位」不成立 ⟹ 半间差不再是 IIR 尾巴而是结构错位。
constexpr int kResidualPeriod = 16;

/// 一次连拍的绝对残差读数（全部在**线性 luma** 域，量纲 0..1）。
struct ResidualStat {
    int64_t maskPixels = 0;
    int frames = 0;       ///< 连拍张数（= periods × 16）
    int periods = 0;      ///< 抖动周期数（≥2 的偶数）
    double meanSwing = 0.0;   ///< mask 内逐像素 swing 的均值
    double p999Swing = 0.0;   ///< swing 的 99.9 分位（离群尾巴，均值会被大面积平坦区稀释）
    double maxSwing = 0.0;    ///< 逐像素最大（看有没有单点爆掉）
    double fracOverTau = 0.0; ///< swing > tau 的像素占比（「肉眼在 8bit 交付面上看得见」的面积）
    double tau = 0.0;         ///< 阈值：一个显示 LSB 在靶白电平处的线性跨度（闭式，见 lsbLinearAtLuma）
    double halfGap = 0.0;     ///< 同相位两半 |Δ| 的均值（收敛/确定性自检，非被测项）
    double halfGapMax = 0.0;  ///< 同上取最大
    /// 同相位两半差 > τ 的**像素数**（`halfGapMax` 是 max，分不出「1 个像素」与「1 万个像素」，
    /// 而这两种结论相反：前者是 8bit 平局翻转噪声地板，后者是真机制。两个一起读才作数）
    int64_t halfGapOverPx = 0;
    double meanLuma = 0.0;    ///< 在场性守卫：画面不是黑的（红线 3）
};

/**
 * 逐像素 `swing = max_over_16 − median_over_16`（两半同相位先平均，再在 16 列上取统计）。
 *
 * 半折叠的**两件事同时做到**：① 两半各含整数个周期 ⟹ 同一列 = 同一绝对相位，与「采集第 0 帧
 * 落在引擎抖动序列的哪个相位」**无关**（那个常数相位偏移在本口径下根本不需要标定——这是它
 * 优于 `taa-swim` 差分口径的地方，那边为标定一个常数相位烧掉两版量具）；② 两半之差 = 收敛
 * 自检（无记忆组须**恰为 0**，有记忆组的容差 = IIR 尾巴闭式 (1−α)^warm × 满幅 **+ 1 个显示
 * LSB**，两项各堵一种机制，见 .cpp 的 `iirTol`）。
 *
 * @param shots 必须**恰好** periods×16 帧、同尺寸、已 prepareLinear（不足就抛，不出数）
 * @param mask  求值区域（空 = 全图）。区域纪律：判据的区域来自几何（objectID 外扩 3px）
 * @param tau   `fracOverTau` 的分母阈值；**各组必须同一个 τ**，否则 frac 跨组不可比
 * @param swingMap 非空 = 顺带写出逐像素 swing 图（CV_64FC1，与帧同尺寸；mask 外为 0）。
 *                热图（`--bistro` 的肉眼证据）与统计**必须同源**——同一份折叠代码写两份输出，
 *                杜绝「图上不闪而数说闪」（或反之）的口径漂移。
 */
[[nodiscard]] ResidualStat phaseSwingStat(const std::vector<Frame>& shots, const cv::Mat& mask,
    double tau, int periods, cv::Mat* swingMap = nullptr);

/// 相位平均图（线性 luma，全图）= 连拍逐像素均值 = 16 列的均值（每列样本数相同）。
/// 收敛正确性判据的**两边共用这一个算子**：分子 = TAA 收敛连拍的相位平均，分母 = 同场景
/// `feedback=1`（输出逐位 = 本帧光栅）连拍一周期的相位平均。⚠ 两边同算子不是风格问题：
/// 分子分母不同口径 = `taa_plan` §7 第 10、12 条与 `temporal.cpp` 头注释烧过四次的同一课。
[[nodiscard]] cv::Mat phaseAverageLuma(const std::vector<Frame>& shots, int periods);

/// 线性 luma 的梯度能量 = mask 上 `|Sobel_x| + |Sobel_y|` 的均值（与 `taa-swim` 的
/// `gradEnergy` 同式；只在**同一 mask、同一算子**的两张图之间取比值才有意义）。
[[nodiscard]] double gradEnergy(const cv::Mat& luma, const cv::Mat& mask);

/// 梯度图本体 = `|Sobel_x| + |Sobel_y|`（3×3 核、REFLECT_101 边界）。`gradEnergy` 是它的均值，
/// 二维分带要的是**逐像素**那张图 ⟹ 两者必须共用这一份实现（「同一算子只许一份实现」，
/// 先例 = 本文件 `swingFoldCore`：统计与热图同源，杜绝「图上不闪而数说闪」）。
[[nodiscard]] cv::Mat sobelAbsGrad(const cv::Mat& luma);

/**
 * 「一个显示 LSB」在线性亮度 `lin` 处的跨度 = `d(srgbDecode)/d(byte)` 的闭式：
 *
 *   srgbDecode(x) = ((x+0.055)/1.055)^2.4 ⟹ d/db = (2.4/1.055)·lin^(7/12) / 255
 *
 * 为什么判据的 τ 要长这样：`frac(swing > τ)` 要回答的是「交付的 8bit 图里有多少像素真的在闪」。
 * 线性域里「多大」没有可见性含义（同一个线性差在白处不到 1 LSB、在暗处能有好几 LSB），而
 * 拍一个线性绝对数（"0.01"）就是讨价还价的阈值。取**靶白电平**处的 1 LSB 把 τ 钉成闭式：
 * 白电平由实测给出（本靶实测 byte 206 ⟹ 线性 0.617 ⟹ τ ≈ 0.0067），算子零自由参数。
 * ⚠ 对白电平以下的像素这是**偏松**的方向（暗处 1 LSB 更小），故本判据不会放过亮处闪烁，
 * 而可能放过暗处的——报告里如实写，别反过来读成「暗部也验过了」。
 * ⚠ 本闭式只在 sRGB **拐点以上**成立：`analytic.cpp::srgbDecode` 是分段函数（byte ≤ 10 走线性段
 * `c/12.92`），拐点以下本式给的是幂律外推值。不影响上面那句「白处 τ 是 mask 内上界」——线性段的
 * 真实跨度 3.0e-4 比白处 τ（6.7e-3）小 22 倍，方向仍然安全；selftest 钉的就是这个边界。
 */
[[nodiscard]] double lsbLinearAtLuma(double lin);

/**
 * 「1 个显示 LSB」在**不亮于 `maxLin` 的像素**上的最大真实跨度——按真解码器逐字节取离散步长：
 * `max { srgbDecode((b+1)/255) − srgbDecode(b/255) | srgbDecode(b/255) ≤ maxLin }`。
 *
 * 为什么半间差容差要用它、而不是用上面那个闭式 τ（两者在同一个电平处差 <1%）：
 *  1. 本判据读的是 **8bit PNG**，两半之间能出现的最小差别就是一个字节，量的是**离散步长**；
 *     闭式是它的导数近似，在拐点以下（`srgbDecode` 的线性段）根本不是同一个函数。
 *  2. 取 **max 而不是 p99**：容差要覆盖 mask 内**最亮**的那个像素翻一格。用 p99 白电平时，
 *     比 p99 更亮的那批像素（本靶实测边缘有到 byte 254 的）一格能翻 0.0089 > τ(206)=0.0067，
 *     会把正常读数判成结构失效。⚠ 反过来 **τ（可见性阈值）必须留在 p99 白电平**——那里要的
 *     是「靶的白面在 8bit 上的一格」，取 max 会被边缘高光稀释。两个数**故意不同源**，别合并。
 *  3. 单调递增 ⟹ `maxLin` 取 mask 内最大 luma 就是**上界**，与像素具体落在哪个字节无关。
 */
[[nodiscard]] double maxLsbStepUpTo(double maxLin);

// ── 区域统计小工具（本层与 `temporal_motion.cpp` **共用同一份实现**）───────────────
// 为什么从匿名 namespace 提出来：这两条是「分位」与「mask 内取值」的唯一口径。
// 留在匿名 namespace 里，新判据想要 p90 边缘带就只能再抄一份——两份 percentile
// 的分位取法（`p*n` 下标 vs 插值）一旦不同，两条判据的「边缘」就不是同一个区域，
// 而这种漂移不会让任何一条变红。先例 = 本文件头注释写的「同一算子只许一份实现」。

/// 升序样本的 p 分位（白电平 / p99.9 / 边缘带阈值用）。空集给 0。
[[nodiscard]] double percentileOf(const std::vector<double>& sorted, double p);

/// mask 内（空 = 全图）的单通道图取值，**未排序**（调用侧自己 sort 后再喂 percentileOf）。
[[nodiscard]] std::vector<double> valuesInMask(const cv::Mat& x, const cv::Mat& mask);

// ── 亮度 × 梯度 二维分带（批 13.7c：判「残余闪烁住在高光边还是普通边缘」）──────────────
//
// ## 要判的假设（用户 2026-10-06 提出）
//
// 「所有移动锯齿的来源，都是之前修过的 1px **高亮**直线；普通边缘确实没锯齿。」
//
// ## 为什么现有测量判不了（本仪器存在的全部理由）
//
// 本项目**所有**分带口径都是**按梯度**（边缘带 = 梯度 ≥p90，见 `temporal_motion.cpp`
// 「区域与阈值」）。而梯度**分不清几何边与高亮边**——两者梯度都大 ⟹ 没有任何一条现有测量
// 能把「高亮边专属」与「普通边为主」这两个假设分开。补的第二根轴是**亮度**：
//
//   |            | 低梯度   | 高梯度               |
//   |------------|----------|----------------------|
//   | 暗部       | 无关区   | 几何边（暗侧）       |
//   | 中间调     | 平坦     | **普通几何边缘**     |
//   | 近削顶     | 大白墙   | **高亮边（元凶？）** │
//
// 「近削顶」是「HDR ≫1」的**代理**：TAA pass 在 tonemap **之前**，通道 ≫1 是常态，而只有
// 高光会 ≫1（`judge.cpp` 记着本 pass 历史上两次最大事故都源于 `saturate()` 把 HDR 亮部顶平）。
// ⚠ **代理不纯，必须靠梯度那一轴拆**：纯白漫反射材质也会削顶，但它**低梯度**；亚像素高光是
// **削顶 + 高梯度**。所以**二维表才是判据，一维表不算完成**。
//
// ## 两根轴的口径（阈值先测后定，理由随输出回显）
//
// * **亮度轴 = 显示域字节亮度的多帧均值**（`cv::cvtColor(BGR2GRAY)`，即 tonemap+sRGB 之后的
//   字节上取 Rec.601 加权）。为什么不是线性域：本轴要的是「交付面上这块**看起来**是不是高光区」，
//   而截图里没有线性 HDR（ACES 已把 ≫1 压进 8bit）⟹ 字节是唯一可得的证据，且「削顶」本来就是
//   字节域的事件（255 堆积）。为什么取**多帧均值**：单帧判定会把「正在闪的高光」判成非高光，
//   均值 = 「这个**区域**是不是高亮区」，正是分带要的语义。⚠ 均值同时是把双刃剑（真在削顶的
//   像素若逐帧闪就掉出 255），故敏感性组里带一份**跨帧最大**口径，两者结论不一致时如实报「分不出」。
// * **梯度轴 = `sobelAbsGrad` 于线性 luma 相位平均**，高梯度 = **≥ p90**。分位而非绝对阈值
//   的理由与 `temporal_motion.cpp` 逐字同源（绝对阈值是拍出来的数，bistro 梯度跨机位跨曝光
//   量级；p90 让「边缘带」= 这个画面里梯度最高的那 10%，与亮度尺度无关）。
//   ⚠ 线性域梯度是 **HDR 加权**的（高亮边的线性梯度天然大几个量级）⟹ 本轴对「高亮边」有
//   **先天偏向**。故敏感性组里带一份**显示域**梯度 p90（ACES 肩部把高光梯度压回普通量级 =
//   对假设更保守的尺子）；两把尺子结论不一致 ⟹ 结论是尺子依赖的，必须报「分不出」。
//
// ## ⭐ 判决线（**2026-10-06 跑前写死**，防事后挑数）
//
// 记 A = 中间调×高梯度（普通几何边缘）、B = 近削顶×高梯度（假设的元凶），
// `rX = rate_on(X) / rate_off(X)`（该格的「被压倍数」，越小 = 长窗档压得越狠）。
//
// * **支持假设**（长窗档压不掉高亮边 ⟹ 残余向「近削顶×高梯度」集中）⟺ `rB ≥ 1.5 × rA`
//   **且** B 的 residual mass share 上升。
// * **否证假设**（两格等比例被压 ⟹ 残余不是高光专属）⟺ `rB / rA ∈ [0.8, 1.25]`。
// * 其余 = **分不出**（须再看敏感性表区分「阈值没分开」与「两格本来同分布」）。
// * 前置门槛：B 格在短窗态的 over-τ 像素 < 200 ⟹ 直接判「分不出（B 格样本不足）」，
//   不许拿噪声比噪声。
//
// 1.5 与 ±25% 的理由：本命令被判读数的运行间噪声 ≤0.3%（本文件头「可复现性」），但分带表是
// **两态相除**，噪声主要落在 swing 本身与两态各自的内容差上；1.5 是「明显压不动」的下限，
// ±25% 是「等比例」的容差，两条线之间的灰带**如实报分不出**，不硬掰成结论。
//
// ### 首轮实测（2026-10-06，AMD Radeon 8060S / RADV STRIX_HALO，三机位全跑）——跑后记录
//
// 三机位的 `rB/rA` 全部 **< 1**（doorframe 0.63 / wetground 0.42 / doorpanel 0.34），且 B 的
// residual mass share 三机位**全部下降**（0.24→0.09 / 1.14→0.53 / 0.94→0.26）⟹ 上面那条预测
// 「残余向近削顶×高梯度集中」**方向为假**：长窗档恰恰是把高亮边压得最狠的一格（B 比 A 多压
// 1.6–3×）。按跑前写死的三分支，它落的是第三支（既非等比例、也未达 1.5× 的**正向**分离）——
// 跑前只写了「等比例」这一种否证形态，没给「反向分离」起名，这是预登记的缺口，故出表处另打
// 一行**方向自述**（不改判决文字）。方向在 T_hi∈{240,248,252}、梯度轴∈{显示 p90, 线性 p75/p90/
// p95}、T_lo∈{96,128,160} 下全部不变；**唯一**翻方向的是「亮度轴=跨帧最大」（doorframe 与
// wetground 翻，doorpanel 不翻）。
// ⚠ 「跨帧最大」那条**不能当主口径**：用「32 帧里最亮的那一帧」定档 = 用被测量的极值选区域，
// 天然把「摆得大」的像素捞进 B 格（选择偏差），且实测三机位两翻一不翻 = 本身不稳。均值口径
// 的偏向是反方向的（真削顶像素若逐帧闪就掉出档外 = 对假设保守），主口径取均值是跑前定的。
//
// 另一条**读表红线**：高梯度 = ≥p90 ⟹ 「低梯度」档是**剩下 90% 像素**的兜底档，不是「平坦」。
// 所以「残余质量 68% 住在低梯度」这种读法没有含义（share 就有 90%）；能读的是**率**（wetground
// 中间调×低梯度 rate 58.9% ⟹ 该画面在 τ 之上大面积普遍摆，不是只有边在摆）与 p75/p95 敏感性。

/// 六格中的一格。三个数**缺一不可**：share 说「这格有多大」、rate 说「这格内闪的比例」、
/// massShare 说「全部残余质量里有多少住在这格」——只有 share 会把「1% 面积全闪」与「50% 面积
/// 微闪」读成同一件事，只有 rate 会忽略该格只有 3 个像素。
struct BandCell {
    int64_t pixels = 0;  ///< 该格像素数
    int64_t over = 0;    ///< 该格内 `swing > tau` 的像素数（rate 与 massShare 的分子）
    double mass = 0.0;   ///< 该格 Σ swing（残余质量的**第二口径**：率之外还看幅度）
};

/// 六行的 ASCII 标签与展开顺序（低→高 × 低→高）。提到本头 = 静止态表与两把移动机位尺子
/// **共用同一行序**（CJK 宽度会让 printf 的 %-Ns 错位，故表体用 ASCII + 表头图例）。
inline const char* const kBandRowTag[6] = {
    "dark x low", "dark x HIGH", "mid x low", "mid x HIGH", "clip x low", "clip x HIGH" };
inline constexpr int kBandRowLb[6] = { 0, 0, 1, 1, 2, 2 };
inline constexpr int kBandRowGb[6] = { 0, 1, 0, 1, 0, 1 };

/// 敏感性组的削顶下界（闭式对应 ACES 输入 ≥3.0 / 5.2 / 9.0 / 真削顶 20+）。
/// 提到本头 = 静止态与两把移动机位尺子**共用同一组敏感性档**（阈值不许各拍一套）。
inline constexpr double kBandClipSensitivity[] = { 240.0, 248.0, 252.0, 255.0 };

/// 亮度档（0=暗 1=中间调 2=近削顶）× 梯度档（0=低 1=高）的六格表。
struct BandTable {
    BandCell cell[3][2];
    int64_t total = 0;  ///< 参与分带的像素数（bistro = 全帧）
    int64_t over = 0;   ///< 全帧 `swing > tau` 的像素数（massShare 的分母）
    double mass = 0.0;  ///< 全帧 Σ swing（第二口径的分母）
    [[nodiscard]] bool valid() const { return total > 0; }
};

/**
 * 逐像素分带计数。**两态必须传同一把尺子**（同一 `lumaByte`/`grad`/三个阈值），否则
 * 「残余质量向某格集中」可能只是两态的带边界不同 ⟹ 差表失去意义。
 *
 * 分档边界（写死并被 selftest 钉住）：亮度 `< tLo` 暗、`[tLo, tHi)` 中间调、`≥ tHi` 近削顶；
 * 梯度 `≥ gradThr` 高；`swing > tau` 记 over（与 `swingFoldCore` 的 `fracOverTau` 同严格性）。
 */
[[nodiscard]] BandTable bandTableOf(const cv::Mat& swing, const cv::Mat& lumaByte,
    const cv::Mat& grad, double tLo, double tHi, double gradThr, double tau);

/**
 * 「近削顶」档的字节下界 = **闭式反解显示链**：`round(srgbEncode(ACES(lin))·255)`。
 *
 * 为什么阈值长这样而不是拍一个 250：本档要的是「ACES 输入 ≫1」的代理，而「字节 b 对应多少
 * 线性 HDR」是**显示链的闭式**（`toBytes` 是本项目独立第二实现，与 shader 逐式对应，selftest
 * 钉着）。lin=3 给 byte 240：3 是「交付面已进 ACES 肩部、亮到没细节」的下界，普通漫反射面在
 * 交付曝光下到不了（实测直方图随表回显）。敏感性另跑 248/252/255（lin ≥ 5.2/9.0/20）。
 */
[[nodiscard]] int bandClipProxyByte(double lin);

// ── 分带阈值（**单一出处**：静止态 `--bistro` 与两把**移动机位**尺子共用，不许在别处重拍）──
// 每条的定法与理由见上面「亮度 × 梯度 二维分带」段；`bandClipProxyByte` 的闭式锚点被 selftest 钉住。
inline constexpr double kBandClipProxyLinear = 3.0; ///< 近削顶 = ACES 输入 ≥3（「≫1」的下界）→ byte 240
inline constexpr double kBandMidByte = 128.0;       ///< 暗/中间调 = 8bit 码字中点（显示域线性 0.2159）
inline constexpr double kBandGradPct = 0.90;        ///< 高梯度 = 梯度 ≥p90（同 temporal_motion 默认）
/// 「明显压不动」的分离倍数下限（批 13.7c 跑前写死；两把移动尺子的「超额集中在 B」沿用，不新拍）
inline constexpr double kBandSupportRatio = 1.5;

// ── 移动态 τ 的出处（2026-10-06 改定义，**两条尺子共用**）────────────────────────────
//
// **旧定义**：τ = 场景白电平（连拍均值线性 luma 的 p99）处 1 个显示 LSB 的线性跨度
// （闭式 `lsbLinearAtLuma`）。它的毛病不是"闭式"，而是**闭式在错的地方**：1 个显示 LSB 是
// 「交付面上恰好能分辨的两级」，而本判据的残差是**线性域 HDR luma 的 warp 残差**——那里
// 1 LSB ≈ 0.0073，而实测同态往返地板（纯重采样就能造出的差）是 **0.34~0.44**，差 **46~60 倍**。
// ⟹ 「超 τ」被地板彻底饱和（实测四率全 100.00%），分带表变成一张全 100% 的表。
//
// **新定义**：τ = **本次实测同态往返地板**（跨帧最大场）的 `kMovingTauFloorPct` 分位。
// 为什么这个出处比闭式更对：地板是本测量的**零假设分布**（没有闪烁、只有重采样时残差长什么样），
// 而"什么算超出噪声"的定义**必须**来自零假设，不能来自显示链的一个通用常数。
// 为什么取 **p99** 而不是 p99.9/p99.99：零假设下每格的 over-τ 率恒等于 1−p（与格的面积**无关**），
// 格间比较因此不被饱和主导；但 p 越高，小面积格（B 档常 < 1% 面积 = 720p 的 9.2k px）的期望
// 计数越小（p99.9 时 B 档零假设期望只剩 9 px），份额数字会抖。p99 = 「1% 误报率」与
// 「小格仍有 ~92 px 期望计数」的折中。⚠ 这条是**统计功效**的取舍，不是精度取舍，写死在此。
inline constexpr double kMovingTauFloorPct = 0.99;

// ── 移动机位分带的三分支判决（批 13.9；**跑前写死**，全文见 temporal_motion.cpp 与
//    temporal_homography.cpp 各自头注释的「⭐ 判决线」段）───────────────────────────────
//
// 与静止态（批 13.7c）的**两处**口径升级，都是昨天那条真实缺口逼出来的：
//  1. 判据的主数从「rate 比」换成 **幅度份额**（Σ残差 归一化）。`frac>τ` 是不加权像素计数：
//     平坦中间调摆 1 LSB 与高光摆一大截各算一票，而高光是对比度最高处、同样绝对摆动**最看得见**。
//     ⟹「我一直看到高亮在闪」与「高亮只占残余 0.5%」**可同时为真**。只有幅度份额能分辨
//     「像素少但每像素摆动巨大」，故**只出计数份额 = 未完成**。
//  2. 三个分支**全部命名**。昨天只给「支持」与「等比例否证」两端起了名字，真结果落在
//     **反向分离**（B 被压得更狠）却只能报「分不出（未命名兜底支）」，读起来像「没有信号」。
//     今天落进哪一支就照实报哪一支，**不许改判决线**。
enum class MovingBandBranch {
    /// 分支①「支持」：B（近削顶×高梯度）的**幅度份额** ≥ 30%，**或**正向超额集中在 B
    /// （B 的 rate 比 ≥ 1.5 × A 的 rate 比）。⟹ 用户主张成立的方向：移动锯齿是高光专属。
    Support,
    /// 分支②「否证」：B 的幅度份额**与**计数份额**都** < 5%，且超额不向 B 集中。
    /// ⟹ 移动锯齿不是高光专属。
    Refute,
    /// 分支③「分不出」：其余。出表处必须再报**性质**（三选一）：阈值没分开（附敏感性表）/
    /// 两格本来同分布 / **反向分离**（超额集中在 A = 普通边缘，与用户主张方向相反）。
    Indeterminate,
};

/// 判决线常量（**跑前写死**，出处 = 上面那段与两个 .cpp 的头注释）。
inline constexpr double kMovingSupportAmpShare = 0.30; ///< 支持线：B 的幅度份额 ≥ 30%
inline constexpr double kMovingRefuteShare = 0.05;     ///< 否证线：B 的幅度份额与计数份额都 < 5%

/**
 * 一格的四数。**四个都要有**（缺任何一个都会读错）：
 * `share` 说这格多大 · `rate` 说这格内闪的比例 · `cntShare` 说全部 over-τ 像素里多少住在这格
 * （**计数份额**，昨天口径：不加权）· `ampShare` 说全部残差**幅度**里多少住在这格
 * （**幅度份额**，本次新增，用来堵可见性缺口）。
 */
struct BandShare {
    double share = 0.0;    ///< 该格像素 / 全帧像素（面积份额）
    double rate = 0.0;     ///< 该格 over-τ / 该格像素
    double cntShare = 0.0; ///< 该格 over-τ / 全帧 over-τ（计数份额）
    double ampShare = 0.0; ///< 该格 Σ残差 / 全帧 Σ残差（幅度份额）
    int64_t pixels = 0, over = 0;
    double mass = 0.0;
};
[[nodiscard]] BandShare bandShareOf(const BandTable& t, int lb, int gb);

/**
 * 出六格表（每格**四数**：share / rate>τ / 计数份额 / 幅度份额 + A/B 两格标注）。
 *
 * 为什么另开一份而不复用 `temporal_residual.cpp` 里那张静止态表：两列的**表头文字**不同
 * （静止态那两张写的是「残余质量 / Σswing」，移动机位这边必须把「计数份额 / 幅度份额」这条
 * 口径升级写在脸上，否则读表的人仍会只读第一列）。**计数只有一份实现**（`bandTableOf`），
 * 这里只是出表；两处的表体数字口径逐字相同。
 *
 * @param residualName 被分带的观测量是什么（进表头，例：「warp 残差（TAA 关）」）
 */
void printMovingBandTable(const std::string& title, const BandTable& t,
    const std::string& residualName);

/// 三分支判决的执行（**只执行**，不解释成新规则；线在头注释里跑前写死）。
[[nodiscard]] MovingBandBranch classifyMovingBand(
    double ampShareB, double cntShareB, bool excessInB);
[[nodiscard]] const char* movingBandBranchName(MovingBandBranch b);

/// 「近空集」线：B 档面积份额低于此值 ⟹ 份额数字建立在极小样本上（0.5% ≈ 720p 的 4.6k px）。
inline constexpr double kBandTinyAreaShare = 0.005;

/**
 * B 档（近削顶 × 高梯度）的**空洞性守卫** —— 判决之后必须紧跟的一句自曝。
 *
 * 为什么必须有（doorframe 实测逼出来的）：尺子 2 的亮度轴来自**超采样参考降采样后**的字节亮度，
 * 1px 高亮细线在降采样里被面积平均掉 ⟹ 近削顶档**一个像素都没有**。此时「B 份额 < 5%」是
 * **空洞成立**（前提无处落子），与「高光不闪」是两回事 —— 正是 phase13 记过的「验收被空洞满足」
 * 那种假绿。空集与近空集分别出话，判决文字本身不改（线跑前写死），只追加性质说明。
 *
 * @param axisName 本尺子的亮度轴是什么（进文字，让读者知道是哪把尺子看不见）
 */
void printMovingBandVacuity(
    const BandShare& bOff, const std::string& axisName, const std::string& extraNote = "");

// ── 移动态「可测性」守卫（2026-10-06 用户拍板：守卫红 ⟹ 只报 Indeterminate，不出结论）──
//
// ## 为什么必须是**硬闸门**而不是警告
//
// `temporal_homography.cpp` 的 runHomography 里原本只有两条**只警告不拦截**的守卫（同态往返
// 地板 + 反号残差）。实测后果（bistro 门框机位，sweep 0.5，2026-10-06 复现）：四条率全部
// = **100.00%**、93.19% 像素 >1 LSB、残差贴着地板，而程序 **rc=0** 还打印了
// 「分支②「否证」：移动锯齿不是高光专属」。一个连"残差是不是运动造成的"都没证明的测量，
// 输出了一份看起来像结论的判决 —— 这正是 phase13 记的「验收被空洞满足」的**判决版**。
//
// ## 四条前提（缺一条 = 没有结论，不是"结论偏弱"）
//
// 1. **对齐自证**（本次的新守卫，理由见下面「为什么不是"搬到同一点"」）：把 warp 强度按
//    α 缩放扫一遍，残差必须在 **α=1 附近取最小**、反号必须更差、且 α=0（不 warp）必须明显
//    比 α=1 差。这三条合起来证明"残差确实是**被这次运动对齐消化掉的**"。
// 2. **残差 > 地板**：地板 = 同态往返残差（warp 到某点再 warp 回同一点 = 真机重采样能造出的
//    最小差）。残差贴着地板 ⟹ 量的是重采样本身，不是闪烁。
// 3. **τ 高于地板**：τ 若低于地板，「超 τ」被地板饱和 ⟹ 占比恒 ~100%（上面实测正是如此）。
// 4. **over-τ 率不饱和**（τ 那条的**实测**版本，直接看占比，不依赖量之间的比较）。
//
// ## ⚠ 为什么守卫 1 **不是**「把两帧 warp 到同一个点对拍」（本守卫第一版的设计错误，实测否决）
//
// 第一版把两帧各搬 Δψ/2 到中间视点对拍，要求差值塌下来。**它按构造不可能塌**：
// 闪烁的定义就是「同一条边在**两个不同亚像素相位**上被各自独立光栅化出来的差」。
// 把两帧几何对齐后，两张源图里那条边的**亮度差原封不动地跟着进来** ⟹ 自证值 ≈ 对照值。
// 实测（Lanczos3 核，恒等性自检 0.000e+00 已确认核没抄错）：合成靶 edge 上只掉 **6.98%**、
// bistro 门框只掉 **9.47%** —— 两个场景都"失败"，说明失败的是**判据**，不是场景。
// 换句话说：这条守卫把「真有闪烁」的场景一律判成"不可测"，是**反向**的。
//
// 正确的问法是「**这份差值是不是被这次运动对齐消化掉的**」，而它有一个不依赖任何阈值的
// 自证形式：残差作为 warp 强度 α 的函数必须在 α=1 处见底。α=0 就是原始帧差（没对齐），
// α=−1 是反号；若运动是真的、闭式单应是对的，曲线就该在 α=1 见底。这条**能**绿（合成靶
// 与 bistro 实测 resid(0)/resid(1) = 1.7 / 1.6），也**能**红（把 Δψ 写错、把符号搞反、
// 扫速与画面都不符时当场红）。
struct MovingGuards
{
    // 守卫 1：对齐自证（warp 强度扫描）
    double residAt0 = 0.0;   ///< α=0 = 不 warp（= 原始帧差），对齐无效时的参照
    double residAt1 = 0.0;   ///< α=1 = 按闭式 Δψ warp（= 本次残差）
    double residAtNeg1 = 0.0;///< α=−1 = 反号假设
    double bestAlpha = 0.0;  ///< 扫描中残差最小的 α
    // 守卫 2：残差 vs 地板
    double floorMean = 0.0;  ///< 地板：同态往返残差均值
    double ratio = 0.0;      ///< residAt1/floorMean
    // 守卫 ⓪：采集是否**活着**（2026-10-06 实测补：`env.intensity=0` 打在只吃天光的机位上
    // 会把交付面整个关黑，帧差/残差/地板一起变 0；那不是「没有闪烁」，是「没有画面」）
    double lumaMean = -1.0;   ///< 显示域字节亮度均值（0..255）；<0 = 本次未采（不判）
    double byteMaxAdjacent = 0.0; ///< 相邻帧最大字节差 = 「相机真动了」的在场性自证
    // 守卫 3/4：阈值与饱和
    double tau = 0.0;        ///< 本次 τ
    double tauFloor = 0.0;   ///< τ 的下界（= 本次实测地板的**中位数**）
    double overRateAll = 0.0;///< 全帧 over-τ 率（零假设下应 ≈ 1−τ 分位，而不是 ~100%）
    bool ok = false;         ///< 四条前提是否全过
    std::string why;         ///< 不过时的人话原因（多条以「；」连，过则空）
};

/// 守卫 1a：α=0 的残差至少是 α=1 的多少倍才算「对齐真的消化掉了东西」。
/// 1.5 = 与 `kBandSupportRatio` 同值同理由（本项目里「明显」一律取 1.5×，不各拍一套）。
inline constexpr double kMovingGuardExplainRatio = 1.5;
/// 守卫 1b：argmin 允许偏离 α=1 多远。0.25 = 半个扫速步（Δψ 的 1/4），再远就是"对不上"。
inline constexpr double kMovingGuardAlphaTol = 0.25;
/// 守卫 2：残差 / 地板 的最小倍数。
inline constexpr double kMovingGuardFloorRatio = 1.5;
/// 守卫 4：全帧 over-τ 率的**粗**上限 = 「全局饱和」的红线。
/// 这条只干一件事：抓住旧口径那种「四率全 **100.00%**」的退化（τ 比地板低 46~60 倍时必然出现）。
/// 为什么取 50% 而不是更严的 25%（本条第一版取 25%，实测门框机位 25.41% **贴线红**）：贴线的
/// 阈值既抓不到新东西，又让正常读数随时可能翻红。50% 把「一半像素超」与「全都超」切开——
/// 健康实测 ~25%、退化实测 100%，两侧都有 2 倍以上余量。
/// ⚠ 它**不管**「单个格内饱和」——那是另一件事，由 `reportMovingBandsFromHomography` 里既有的
///    `rateSaturated`（四格 rate 同贴 1.0 ⟹ 率比退化失效、方向不判）负责，两层不重复。
inline constexpr double kMovingGuardMaxOverRate = 0.50;
/// 守卫 ⓪a：交付面**平均亮度**（显示域 0..255）的下界。取 1.0 = 「八位面上连 1/255 都没有」，
/// 只抓真死的采集，不误伤夜景（实测 `env.intensity=0` 打在门框机位：均值 = 0.00，全黑）。
inline constexpr double kMovingGuardMinLuma = 1.0;
/// 守卫 ⓪b：相邻帧最大字节差的下界。0 = 引擎一帧没换 / 相机根本没动（在场性自证，
/// 出处 `temporal_flicker.cpp` 的 `byteMaxAdjacent`，与 `temporal-flicker` 同一条证据）。
inline constexpr double kMovingGuardMinMotion = 1.0;

/**
 * @brief 守卫不过时的**唯一**合法输出：报 Indeterminate + 缺哪条前提，**不出任何分支结论**
 *
 * 调用方拿它当退出码（非 0）。判决线本身不改（跑前写死），这里只是**不让没资格的话说出口**。
 */
void printIndeterminate(const std::string& target, const MovingGuards& g);

// ── 历史拒收 × 残余 的因果交叉表（批 13.10；用户报障「长窗档开仍闪」的归因）──────────────
//
// ## 要判的假设
//
// 长窗档把本帧权重压到 0.01（等效窗 ≈100 帧；历史：这一档曾由 `static_boost` 给出，
// 批 13.14b 后 = 显式 `feedback`）之后，砖块亮缝 / 窗棂细线**仍在闪**。
// 纯 EMA 数学给的下界是**已知**的：16 帧周期分量在本帧权重 α 处的传递
// `|H| = α/|1−(1−α)e^{−j2π/16}|` 在 α=0.01 是 **0.0256**、α=0.12 是 **0.312** ⟹ 若每个像素都在
// 老实累积，meanSwing 该降 **12.2×**（brickfence 实测只降 **4.0×**、p99.9 只降 2.5×）。
// 差额指向 `taa.frag.slang` 里一条**与 α 无关**的路径：`clipToAabb` 把历史夹到本帧邻域盒
// （诊断码 0.25）甚至塌回 centroid（0.5）⟹ 被夹的「历史」本身就是本帧的函数 ⟹ shader 注释
// 原话「α 调多小都没用」。本表 = 那句话的**实测版本**（注释早就写了假设，从来没人量过）。
//
// ## 口径（三条，每条都是被迫的）
//
//  1. **拒收码来自引擎本体**（门 `taa.diag_reject` → `taa_out` 的 alpha，raw8 直出），不是从
//     图像反推。RGB 逐位不变（`taa.frag.slang` 的诊断 alpha 段），所以拒收组与交付组是**同一
//     幅画面**的两种读法，像素一一对齐。
//  2. **交叉表的一侧是逐像素帧计数**，不是「本帧是否拒收」：抖动周期 16、连拍 32 帧 ⟹ 一根
//     1px 细线只要有一半相位被夹，它就**必然**在 swing 里留痕。分档因此按「32 帧里有几帧历史
//     被动过」，`0` 档 = 「全程原样收下」= **纯 EMA 泄漏档**（它必须安静，否则本假设死）。
//     ⚠ 两组是**不同进程**，绝对起始相位可差常数帧 ⟹ 本表**不做逐帧配对**，只做「计数 → 分档」
//     （两半各含整数个周期，计数对相位平移不敏感）。
//  3. 求值域 = 全帧（与 `--bistro` 的残差同区域口径），τ = 同一次运行的 N1 τ（跨组共用）。
//
// ## ⭐ 判决线（**2026-10-06 跑前写死**，防事后挑数）
//
// 记 `r0` = 0 档（全程收下）的 rate>τ、`rAll` = 全帧 rate>τ、`m3p` = 「≥3 拒收帧」三档合计的
// **残余质量占比**（Σswing 口径，理由同 `MovingBandBranch` 的幅度份额升级）。
//
// * **支持「盒裁剪/拒收是残余主因」** ⟺ `r0 ≤ 0.20 × rAll` **且** `m3p ≥ 0.50`。
//   （0 档安静 = 老实累积的地方确实不闪；质量住在被夹的像素上 = 差额不是 EMA 泄漏。）
// * **否证（残余 = EMA 泄漏，与盒无关）** ⟺ `r0 ≥ 0.80 × rAll`。
//   ⟹ 修法只在「α 还能不能更低 / 历史侧还能不能更准」，**动邻域盒是白动**。
// * 其余 = **分不出**（如实报，不硬掰）。
// * 前置门槛：0 档像素 < 全帧 5% ⟹ 直接判「分不出（0 档样本不足，没有『老实累积』的对照）」。
//
// 0.20 / 0.80 与 0.50 的理由：0.20 = 「安静」的下界（本项目「明显」一律取 1.5×，这里要的是
// **反向**判据，取 5× 分离 = 0.20，比 1.5× 更保守，因为 0 档面积通常 >90%，稍有泄漏就会被面积
// 稀释）；0.80 = 「与全帧无异」的容差（±25% 的等价形式，同 `kBandEqualLo/Hi` 的量级）；0.50 =
// 「多数」的最低含义。三条线之间的灰带如实报分不出。
struct RejectFraction {
    double accepted = 0.0;   ///< 历史原样收下（alpha 0）
    double clipped = 0.0;    ///< 被邻域盒裁剪、部分保留（0.25）
    double collapsed = 0.0;  ///< 塌回邻域盒 centroid = 拒收（0.5）
    double reproject = 0.0;  ///< 重投影出画拒收（0.75）
    double noHistory = 0.0;  ///< 本帧无历史（1.0）
};

/**
 * 单帧拒收码图（CV_8UC1，raw8 直出的 alpha）→ 五桶占比（和恰为 1）。
 *
 * **唯一口径**：`temporal_motion.cpp::rejectStats` 与本文件的交叉表**共用这一份**分桶
 * （先例 = 分带常量提到本头；两份分桶代码的边界一旦不同，两条判据的「拒收」就不是同一件事，
 * 而这种漂移不会让任何一条变红）。分桶边界取码值中点（32/96/160/224），raw8 舍入 ±1 被吸收。
 */
[[nodiscard]] RejectFraction rejectFractionOf(const cv::Mat& alpha8);

/// 交叉表的一格（三个数缺一不可，理由同 `BandCell`）。
struct RejectBinCell {
    int64_t pixels = 0;
    int64_t over = 0;   ///< 该档内 `swing > tau` 的像素数
    double mass = 0.0;  ///< 该档 Σswing
};

/// 「N 帧里有几帧历史被动过」五档交叉表：0 / 1–2 / 3–8 / 9–16 / 17+。
struct RejectCrossTab {
    RejectBinCell bin[5];
    int64_t total = 0;  ///< 参与计数的像素数（尺寸不符 = 0，见 .cpp 前提守卫）
    int64_t over = 0;
    double mass = 0.0;
    [[nodiscard]] bool valid() const { return total > 0; }
};

/**
 * 拒收帧数 × 逐像素 swing 的交叉计数。
 *
 * @param rejectFrames 每像素「历史被动过（≠收下）」的帧数（CV_32SC1，与 swing 同尺寸）
 * @param swing        同一次 `--bistro` 采集的逐像素 swing（CV_64FC1，线性 luma）
 * 尺寸不符 / 空图 ⟹ `total = 0`（**不出数**，同 `bandTableOf` 的前提守卫）。
 */
[[nodiscard]] RejectCrossTab rejectCrossTab(const cv::Mat& rejectFrames, const cv::Mat& swing,
    double tau);

/// 判决线常量（**跑前写死**，出处 = 上面那段）。
inline constexpr double kRejectAnchorQuiet = 0.20;  ///< 支持线：0 档 rate ≤ 0.20 × 全帧 rate
inline constexpr double kRejectRefuteSame = 0.80;   ///< 否证线：0 档 rate ≥ 0.80 × 全帧 rate
inline constexpr double kRejectMassLine = 0.50;     ///< 支持线：≥3 拒收帧三档的残余质量 ≥ 50%
inline constexpr double kRejectMinBin0Share = 0.05; ///< 前置门槛：0 档面积下限（否则无对照）

/// 判决三分支（命名齐三条，防批 13.7c 那种「落进未命名兜底支」）。
enum class RejectBranch { Support, Refute, Indeterminate };
[[nodiscard]] RejectBranch classifyReject(const RejectCrossTab& t, double bin0Share);
[[nodiscard]] const char* rejectBranchName(RejectBranch b);

/// EMA（**本帧权重** α，与 `taa.feedback` 同语义）对 16 帧周期分量的传递幅度
/// `|H(α,k)| = α / |1 − (1−α)·e^{−j2πk/P}|`。
///
/// 这是「累积窗该压掉多少闪烁」的**闭式**，不是拟合出来的曲线：静止机位 + 固定抖动 ⟹ 输入
/// 对帧号是**严格 P 周期**的（Halton 周期 `kResidualPeriod`），EMA 是线性时不变滤波 ⟹ 输出
/// 同周期、每个谐波乘 `|H|`。
[[nodiscard]] double emaTransmittance(double alpha, int k = 1, int period = kResidualPeriod);

/**
 * 闭式**抑制比** `ρ* = |H(α_on, k)| / |H(α_off, k)|`（长窗档该把残差压到几分之一；
 * α_on = 长窗档、α_off = 短窗档）。
 *
 * ⭐ 本判据能成立靠的是一条实测出来的**形状无关性**：ρ* 跨谐波 k=1..8 只散布 **5.0%**
 * （0.0787–0.0827，α 0.12→0.01）。所以「一根 1px 细线的闪烁是方波还是尖峰」根本不影响这条线
 * ——任何老实累积的像素都必须被压到 ρ* 附近，与内容无关。
 * ⚠ 它是 **EMA 专属**的地板，不是普适下界：换成 16 抽头时域盒式平均能压到更低，而本判据是
 * **上界**（`ρ ≤ 线`），换更强的滤波只会更绿 ⟹ 不存在「把判据逼到调阈值」的死角。
 * ⚠ α 从**配置**读（长短窗两档 `feedback`，批 13.14b 前长窗档在 `static_feedback` 里），
 * 线随档位自动自适应，改档位不必重拍阈值。
 */
[[nodiscard]] double emaSuppressionRatio(double alphaOn, double alphaOff, int k = 1,
    int period = kResidualPeriod);

/// 判决线（**2026-10-06 跑前写死**）：`ρ ≤ 1.5 × ρ*`。1.5 = 本项目「明显」统一常量
/// （`kBandSupportRatio` 同值同理由）。语义 = 「至少得把闭式差距的 2/3 补上」，
/// 不是噪声余量（本命令运行间噪声 ≤0.3%，远小于此）。
/// ⚠ 名字里的 boost = 历史：判据曾名 `temporal-residual-boost-fidelity`，批 13.14b 随引擎删
/// boost 改名为 `temporal-residual-window-fidelity`，**线本身一字未动**，常量名保留以免台账断链。
inline constexpr double kBoostFidelityLine = 1.5;

/**
 * 批 13.11 **历史裁剪消融臂**（门 `taa.clip_mode` / `taa.clip_soft`）。默认 = 门关 = 逐位今天。
 *
 * 档位（权威语义 → `rgl/scene_data.cppm` kSceneDataTaaClipControlOffset）：
 * 0 现行 / 1 关盒（短路天花板参照）/ 2 绕 centroid 扩 1.5× / 3 box9 / 4 软裁剪（λ = soft）
 * / 5 绕 centroid 扩 K×（K = soft，参数化扩盒扫描臂）。
 */
struct ClipArm {
    double mode = 0.0;
    double soft = 1.0;
};

/// 消融臂的显示名（表头与台账都用它，别在多处手写字符串）。实现 = 本文件匿名空间内
/// （只有本 TU 用；声明放头文件会与匿名空间定义**同名歧义**，实测踩过）。

/// A/B 判决线（**2026-10-06 跑前写死**，跑完不许改口径）：
/// ① 因果坐实 = 某臂 `ρ ≤ kBoostFidelityLine × ρ*` **且** `frac>τ` 相对臂 0 降 ≥ 4×
///    （4× = 短窗 9.99% → 现状 2.38% 的再 1/5；「肉眼不再数得清」量级，不是可调 epsilon）。
/// ② 否证 = **所有**消融臂 `ρ ≥ 0.8 × ρ(臂0)` ⟹ 盒不是主因，短路假设死，必须回头查别处。
/// ③ 通用性 = 逐机位/逐靶**方向一致**（只有报障机位降 = 场景特异 = 不算修好）。
inline constexpr double kClipCausalFracDrop = 4.0;
inline constexpr double kClipRefuteRatio = 0.8;
/// 锐度反作弊容差（**只罚变糊这一侧**，方向论证 → `runBistroClipAblation` 内注释）：
/// 本臂梯度比低于臂 0 超过本值 = 「靠糊换安静」= 该臂不算修好。0.03 ≫ 读数噪声（≪0.005）。
inline constexpr double kBlurTolerance = 0.03;

struct BistroCam;  ///< 前置声明（定义在本文件下方机位表处；本命令的 A/B 以它为入参）

/**
 * 批 13.11 **因果 A/B**（`--clip-abl`）：同一机位、同一构建，逐臂跑「短窗 / 长窗」两组，
 * 出每臂的 ρ（对闭式线判）、`frac>τ`、p99.9，并执行上面三条线。
 */
struct ClipAblRow {
    ClipArm arm;
    double rho = 0.0, rhoStar = 0.0, line = 0.0;
    double swingOff = 0.0, swingOn = 0.0;
    double fracOff = 0.0, fracOn = 0.0;  ///< frac>τ（短窗 / 长窗）
    double p999Off = 0.0, p999On = 0.0;  ///< 尾巴（两态都要；13.14b 后**交付 α=0.01 = 长窗档**）
    /// 锐度对照（**堵「靠糊换安静」这个洞**）：本臂相位平均图的线性域梯度能量 ÷
    /// 同次运行的 fb=1 参考（= 理想 16 相位平均）。带 [0.85, 1.02] = 既有选档口径。
    /// ⚠ 没有这两列，「尾巴降 20×」完全可以靠"更糊了"拿到——ρ 是比值，糊不动它，
    ///   但可见症状的改善会被糊虚报。首轮（2026-10-06）就漏了这两列。
    double gradOff = 0.0, gradOn = 0.0;
    bool ok = false;
};

/// 一个档（或全帧）的两态平均 swing 与抑制比。
struct BinSuppression {
    double swingOff = 0.0;  ///< 短窗（α=0.12）平均 swing
    double swingOn = 0.0;   ///< 长窗（α=`--long-feedback`）平均 swing
    double rho = 0.0;       ///< swingOn/swingOff（0 档的闭式期望 = ρ*）
    int64_t pixels = 0;
};

/// 逐拒收档的**抑制比**表（+ 全帧）。这是「判据能不能绿」的**内部阳性锚点**：
/// 0 档（全程收下）必须 ρ ≈ ρ*，被夹档 ρ → 1。绿例与红例在**同一帧**里，判据不可能
/// 是「永远红的摆设」，也不必去别的场景借一个绿。
/// 两侧 swing 必须同尺寸（同一次 `--bistro` 的 N1/N2），否则 `valid = false`。
struct SuppressionTable {
    BinSuppression bin[5];
    BinSuppression total;
    bool valid = false;
};
[[nodiscard]] SuppressionTable suppressionByRejectBin(const cv::Mat& swingOn,
    const cv::Mat& swingOff, const cv::Mat& rejectFrames);

/// bistro 用户报障机位（**仓库资产**，改坐标 = 改口径）。
///
/// 提到本头 = 单一真相源：`temporal-residual --bistro` 与 `temporal-motion` 必须打在
/// **同一个机位**上，否则「静止机位的残差」与「运动机位的边缘偏差」说的是两幅画面。
/// 坐标 = 2026-10-05 凌晨量具（/tmp 里的 Python，机器重启随 tmpfs 没了）从会话记录原样捞回。
/// doorframe = 用户报障机位（门框缝隙 / 门板斜边 / 窗棂的 1px 高亮细线）；
/// wetground = 湿润地面成区颗粒；doorpanel = 旧门板白点；
/// brickfence = 2026-10-06 用户报障机位（**长窗档 α=0.01 仍闪**——当时由 `static_boost` 给出，
/// 批 13.14b 后 = 显式 `feedback`；砖块墙面 + 窗户围栏），
///   坐标 = 用户 `camera_saved` 日志原样（mode=fps，fov 45）。
struct BistroCam {
    const char* name;
    std::array<double, 3> position;
    std::array<double, 3> target;
    double fov;
};

inline const BistroCam kBistroCams[] = {
    {"doorframe", {-0.115745, 0.030675, 0.005653}, {-0.259691, 0.043088, 0.235251}, 45.0},
    {"wetground", {-0.157946, 0.054863, 0.014969}, {-0.030544, -0.117198, -0.202296}, 45.0},
    {"doorpanel", {-0.047810, 0.068143, 0.046978}, {-0.223579, 0.014183, 0.116858}, 45.0},
    {"brickfence", {-0.185123, 0.087174, -0.088511}, {-0.322725, 0.158539, -0.395969}, 45.0},
    // 2026-10-06 用户**第二次报障**机位（`[camera_saved] mode="fps"` 原文坐标，未改名）。
    // 登记理由 = 它是**新视角**，不能假设 brickfence 的结论自动覆盖。实测（`--clip N --clip-soft 0`）：
    // 臂 0 ρ=0.2199 / frac>τ 1.51% · 臂 8 **ρ=0.0790** / 0.41% / 折叠 0px · 臂 9 ρ=0.0800 / 折叠 90·323px。
    // ⟹ 与 brickfence **同一机制**（ρ 同向、同量级），修法在新视角上不需要重新调参。
    {"report1", {-0.240498, 0.112888, -0.106188}, {-0.166224, 0.121830, -0.500087}, 45.0},
    // 2026-10-06 用户**第三次报障/首次验收**机位（`[camera_saved]` 原文坐标）。与 report1 相邻但
    // 不是同一视角（位置差 0.014 / 0.010 / 0.012），**不能假设 report1 的结论自动覆盖**。
    // 用户肉眼（`clip_mode = 11` + 长窗档 α=0.01——当时由 `static_boost = true` 给出，手动晃机）：
    // **静止无闪烁 · 未见鬼影 ·
    // 左右轻微转动仅轻微闪烁（较此前大幅改善）**。本表登记 = 让这条肉眼结论有可复现的数。
    {"report2", {-0.245181, 0.102938, -0.093967}, {-0.140388, 0.130429, -0.481145}, 45.0},
};

/// 机位名清单（**从表现拼，绝不手写**——登记 brickfence 时两处手写清单当场过时，
/// 报错误导排查者以为机位没接上）。
[[nodiscard]] inline std::string bistroCamNames()
{
    std::string n;
    for (const auto& c : kBistroCams)
        n += (n.empty() ? std::string() : std::string(" / ")) + c.name;
    return n;
}

}  // namespace RGL::imgqa
