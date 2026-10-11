#pragma once
// 结构差分统计（Phase 15 批 15.2 子步 D1）的**纯度量层**（无引擎、无 IO、除 OpenCV 外无依赖）
// ——拆出来的理由与 `ao_metrics.h` 同一条：**判据的算子要能被合成靶自检**。
//
// 判据本体（拍板记录 → docs/phase15_plan.md §2 批次表 15.2 行 + §3.3 子步 C/D1）：
// 湿面/SSR 的「反射结构对比度」= 纯 Δ 图（A−B，**线性亮度域**，sRGB 解码后 Rec.709）上的
//   ① |Δ| 的 p10/p50/p90 与 **p90/p10 比值**（结构对比度：只在 |Δ|>ε 的差分集合上算）
//   ② **Δ<0（变暗）像素数**——「命中处减 IBL specular」生效的阳性锚点（15.1 时代恒 0）
//   ③ 影响域交叉：变暗 ∧ roughness ≥ gate（dump_gbuf=3 的 metalrough .g = UNORM8）必须 = 0
//   ④ **命中率 = |Δ|>ε ∧ 过门控 / 过门控**（15.3 子步 A 还 §3.2 未还账⑤：此前手算）——
//     报告值不是判据；需 --gbuf 在场，缺席报「需 --gbuf」、分母 0 报「无定义」，两者都不是 0
//
// 与一次性参考实现 /tmp/opencode/ssr_subtract_check.py 的口径对账（该脚本本算子落地后作废）：
//   · 差分集合：脚本数 `d != 0`，本算子数 `|Δ| > minDelta`（默认 1e-6）。两者对 PNG 字节图
//     **逐位等量**：最小可量化亮度台阶 = 0.0722·srgbDecode(1/255) ≈ 2.2e-5 ≫ 1e-6，
//     不存在落在 (0, 1e-6] 的真实差分（selftest 钉这条台阶闭式）。
//   · 过门控像素：脚本数 `0 < g < 102`（g=0 = 天空/背景排除在外），本算子同口径
//     `0 < rough < gate`——**不是** `rough < gate`（那会把整片天空算进分母）。
//   · 分位数：nearest-rank `sorted[floor(q·(n−1))]`（与 `attribute()` 的 pct 同式）。
//     参考实现的 rough p50 用 `vs[n/2]`，偶数 n 时与本口径差一个下标——在双峰资产上
//     （0.05/0.35 两簇）两下标同值，实测读数逐位一致（§3.3 子步 D1 对拍表）。
//
// ⚠ p10 = 0 的 inf 标注是**防御分支**：差分集合按 |Δ|>ε 构造，p10 数学上取不到 0；
//   它防的是 ε 被显式给 0 之类的调用形态（比值无定义 ≠ 比值是 0，写 0 = 把失效读成满分，
//   同 `shadowPenumbraWidth` 的 NaN 纪律）。selftest 用 `diffStructRatio` 钉住这条分支。
#include <cstdint>
#include <limits>

#include <opencv2/core.hpp>

namespace RGL::imgqa {

/// A−B 的结构差分统计。luma 均为**线性域** Rec.709（`prepareLinear` 的 luma；比较域单点，
/// 文件头硬约束第 2 条）。分位数只在差分集合（|Δ| > minDelta）上算。
struct DiffStructStat {
    int64_t width = 0, height = 0, total = 0;
    int64_t diff = 0;    ///< |Δ| > minDelta 的像素数
    int64_t darker = 0;  ///< 差分集合内 Δ < 0（变暗）
    int64_t brighter = 0;  ///< 差分集合内 Δ > 0（变亮）
    double minDarkDelta = 0.0;  ///< 变暗像素 Δ 的最小值（最负者）；无变暗 = 0
    double p10 = 0.0, p50 = 0.0, p90 = 0.0;  ///< 差分集合上 |Δ| 的 nearest-rank 分位
    double ratio = 0.0;  ///< p90/p10；p10 = 0 → +inf、差分集合为空 → NaN（两者 ratioFinite = false）
    bool ratioFinite = true;
    // ── 以下仅 rough 在场（--gbuf）时有效 ──
    bool hasRough = false;
    int64_t darkOutsideGate = 0;  ///< 变暗 ∧ rough ≥ gate —— **判据：必须 = 0**
    int64_t gatePixels = 0;       ///< 过门控像素 0 < rough < gate（全图，非差分集）
    int64_t gateDiff = 0;         ///< |Δ| > minDelta ∧ 过门控 —— 命中率分子（§3.2 未还账⑤）
    /// 命中率 = gateDiff / gatePixels（「过门控分母命中率」的硬口径，15.3 子步 A ⑤）。
    /// 分母为 0 时 = **NaN 不是 0**（同 ratio 的纪律：无定义 ≠ 零命中，写 0 = 把
    /// 「量具取不出数」读成「一个都没命中」）。rough 缺席时同为 NaN。
    double hitRate = std::numeric_limits<double>::quiet_NaN();
    double roughMin = 0.0, roughP50 = 0.0, roughMax = 0.0;  ///< 变暗像素的 roughness 分布
    bool roughDistValid = false;  ///< 变暗集非空才有分布
};

/**
 * 核心统计：两幅线性亮度图 + 可选 roughness 图（g/255 归一，CV_64FC1；空 = 不做域交叉）。
 *
 * 前提守卫（错位输入 = 最贵的假绿，同 `bandTableOf` 的纪律）：两侧 luma 必须同尺寸
 * CV_64FC1，rough 给定时同尺寸 CV_64FC1——不满足**抛**，不出数。
 */
[[nodiscard]] DiffStructStat diffStructStat(const cv::Mat& lumaA, const cv::Mat& lumaB,
    const cv::Mat& rough, double gate, double minDelta);

/// p90/p10，p10 = 0 → +inf（不崩、不把「无定义」写成 0）。见文件头防御分支说明。
[[nodiscard]] double diffStructRatio(double p90, double p10);

}  // namespace RGL::imgqa
