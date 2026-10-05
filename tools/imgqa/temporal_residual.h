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

/// bistro 用户报障机位（**仓库资产**，改坐标 = 改口径）。
///
/// 提到本头 = 单一真相源：`temporal-residual --bistro` 与 `temporal-motion` 必须打在
/// **同一个机位**上，否则「静止机位的残差」与「运动机位的边缘偏差」说的是两幅画面。
/// 坐标 = 2026-10-05 凌晨量具（/tmp 里的 Python，机器重启随 tmpfs 没了）从会话记录原样捞回。
/// doorframe = 用户报障机位（门框缝隙 / 门板斜边 / 窗棂的 1px 高亮细线）；
/// wetground = 湿润地面成区颗粒；doorpanel = 旧门板白点。
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
};

}  // namespace RGL::imgqa
