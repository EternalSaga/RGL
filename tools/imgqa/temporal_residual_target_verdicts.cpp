// 合成靶主路径（现行 **7 条判据**）：验参 → 采集 → 出表 → 判据 → 台账。
//
//   从 `temporal_residual.cpp` 拆出（文件护栏 500 代码行）。`cmdTemporalResidual` 曾是 199 代码行 /
//   认知复杂度 44 的怪物（样式检查两条点名：120 行红线 + 30 认知红线），拆法 = 按它本来就在走
//   的五步；**行为逐字不变**：打印顺序、文案、退出码、台账字段全部原样搬运，只有函数边界是新的。
//
//   判据口径的权威载体 = `temporal_residual_metric.h` 的实测表 + 变异记录（每条判据至少一条变异
//   能打红）；度量定义与三条测量红线 = `temporal_residual.h` 头注释。采集在
//   `temporal_residual_target.cpp`，本 TU 只吃 `std::vector<Run>`。

#include "temporal_residual_metric.h"

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

// ── 合成靶主路径（现行 7 条判据）的函数边界 ────────────────────────────────────────
//
// `cmdTemporalResidual` 曾是 199 代码行 / 认知复杂度 44 的怪物（样式检查两条点名：120 行
// 红线 + 30 认知红线）。拆法 = 按它本来就在走的五步：验参 → 采集 → 出表 → 判据 → 台账。
// **行为逐字不变**：打印顺序、文案、退出码、台账字段全部原样搬运，只有函数边界是新的；
// 判据口径仍以 `temporal_residual_metric.h` 的实测表 + 变异记录为准。

/// 入参**当场拒**（返回非 0 = 直接作为命令退出码）。共同点 = 宁可拒也不静默忽略：
/// 一个被吃掉的键 = 「我加了 --xxx」这句话没有意义（同本仓「camelCase 静默回落」那条债）。
int validateResidualArgs(int periods, bool bistro, bool windowAbl, bool reject, bool clipAbl,
    double longFeedback)
{
    if (periods < 2 || periods % 2 != 0) {
        std::printf("--periods 必须是 ≥2 的偶数（两半折叠 = 同绝对相位对照）。收到 %d\n", periods);
        return 2;
    }
    // 🔴 **长窗档必须严格短于短窗端 0.12**（批 13.14b 把这一档从 bool `--static-boost` 换成了
    // 数值 `--long-feedback`，于是多出一条**必然绿**的自欺路径）：若 `longFeedback == 0.12`，
    // 两臂在引擎侧是**同一个配置** ⟹ ρ = swingOn/swingOff ≡ 1，而闭式
    // `ρ* = emaSuppressionRatio(0.12, 0.12) = 1`（selftest 里就钉着这条）⟹ 线 = 1.5×1 = 1.5
    // ⟹ **ρ = 1 ≤ 1.5 恒绿**。`> 0.12` 更糟：ρ* > 1 把线抬得更高，而「长窗」其实更短。
    // ⟹ 与其留一条会绿的空判据，不如当场拒（同本文件 `--reject` / `--clip-abl` 的处置方向：
    // **被吃掉的键与被污染的答案，成本一样高**）。
    if ((bistro || windowAbl) && !(longFeedback > 0.0 && longFeedback < 0.12)) {
        std::printf("--long-feedback 必须落在 (0, 0.12)：0.12 是短窗端（B2 / N1 写死），"
                    "两档相同 ⟹ ρ ≡ 1 且 ρ* ≡ 1 ⟹ 线 1.5 **必绿**，判据失去内容。"
                    "收到 %g。默认 0.01 = 仓库拍板的长窗档。\n",
            longFeedback);
        return 2;
    }
    // `--reject` 只在 bistro 变体里实现（拒收码要交付配置的 taa_out 附件 + 全帧区域口径）。
    // **当场拒**而不是静默忽略（同上一条的处置方向）。
    if (reject && !bistro) {
        std::printf("--reject 只在 --bistro 变体里实现（要交付配置的 taa_out 附件通道 + 全帧区域"
                    "口径）；合成靶路径未接，**拒绝静默忽略该键**。加 --bistro 或去掉 --reject。\n");
        return 2;
    }
    // 批 13.11 消融臂：`--clip-abl` 是 bistro 侧的逐臂 A/B；`--clip <mode>` 是透传（两条路径都吃）。
    if (clipAbl && !bistro) {
        std::printf("--clip-abl 只在 --bistro 变体里实现；合成靶侧请用 --long-feedback <档> "
                    "--clip <mode>（B2/B3/B4 三组吃同一臂，ρ 同口径可比）。\n");
        return 2;
    }
    return 0;
}

namespace {
/**
 * 合成靶五组采集（R0..R4，口径见本文件头注释的实测表）。τ 由第一组（R0）定一次后原样
 * 传下去——跨组换 τ = `frac` 的跨组比较失去意义 = 单调性轴作废。
 * 采集失败（在场性守卫抛）向上传，由调用方转成退出码 2。
 */
std::vector<Run> collectTargetRuns(const std::string& root,
    int periods, double warmMult, double slowFeedback, bool skipSlow, bool baseSpace, double& tau)
{
    std::vector<Run> runs;
    runs.push_back(runGroup(root + "/imgqa_res_r0", "R0 TAA关(地板)", "resr0", false, 0.12,
        1.0, periods, warmMult, tau, false));
    runs.push_back(runGroup(root + "/imgqa_res_r1", "R1 fb=1  js=1(锚点)", "resr1", true, 1.0,
        1.0, periods, warmMult, tau, baseSpace));
    runs.push_back(runGroup(root + "/imgqa_res_r2", "R2 fb=.12 js=1(现行)", "resr2", true,
        0.12, 1.0, periods, warmMult, tau, baseSpace));
    runs.push_back(runGroup(root + "/imgqa_res_r3", "R3 fb=.12 js=0(反向)", "resr3", true,
        0.12, 0.0, periods, warmMult, tau, baseSpace));
    if (!skipSlow)
        runs.push_back(runGroup(root + "/imgqa_res_r4",
            "R4 fb=" + d4(slowFeedback) + " js=1", "resr4", true, slowFeedback, 1.0,
            periods, warmMult, tau, baseSpace));
    return runs;
}

/// 表头 = 口径声明（域 / 区域 / τ / 量化项四个来源逐一声明）+ 逐组读数行。
void printTargetHeader(const std::vector<Run>& runs, int periods, bool baseSpace, double tau)
{
    const Run& R2 = runs[2];
    std::printf("时域绝对残差：aa_target %d×%d  静止机位  折叠 (2,%d,H,W)  周期数 %d  帧数 %d  "
                "base_space=%s\n",
        kSize[0], kSize[1], kResidualPeriod, periods, kResidualPeriod * periods,
        baseSpace ? "开（输出在基空间）" : "关（输出在抖动空间，交付默认）");
    std::printf("  统计域 = **sRGB 解码后的线性 luma**（红线 1）；swing = max−median over %d 列\n",
        kResidualPeriod);
    std::printf("  区域 = objectID 靶内部外扩 3px（%lld px）；τ = 白电平(p99) %s 处 1 个显示 LSB 的"
                "线性跨度 = %s（闭式见 lsbLinearAtLuma）\n",
        static_cast<long long>(R2.stat.maskPixels), d6(R2.whiteLuma).c_str(), d6(tau).c_str());
    std::printf("  半间差量化项 = mask 内**最亮** %s 处翻一格的最大真实跨度 = %s（离散步长，"
                "与 τ 故意不同源，见 maxLsbStepUpTo）\n\n",
        d6(R2.maxLuma).c_str(), d6(R2.quant).c_str());
    for (const auto& r : runs)
        printRow(r);
}

/// 收敛正确性的**比值族**（分子分母**同一个算子**，见 phaseAverageLuma 注释）。参考 = R1
/// fb=1 的相位平均 = 理想 16 相位平均（按构造）。只有 `r2` 进判据 ⑦，其余只打印。
struct GradRatios {
    double r2 = 0.0;        ///< R2 收敛连拍的相位平均（⑦ 的被判数）
    double r4 = 0.0;        ///< R4 慢累积（`--skip-slow` 时不采，本字段无意义）
    double single = 0.0;    ///< 「完全不平均的单帧」参照（⚠ 取末帧，随 burst 相位浮动 5%）
    double noTaa = 0.0;     ///< R0（TAA 关，未抖）参照
    double byteRatio = 0.0; ///< 同一份数据在**显示空间**算 = 红线 1 的反面教材（实测非引用）
};

GradRatios computeGradRatios(const std::vector<Run>& runs, bool hasSlow)
{
    const Run& R0 = runs[0];
    const Run& R1 = runs[1];
    const Run& R2 = runs[2];
    const double gradRef = R1.grad;
    const auto ratio = [gradRef](double g) { return gradRef > 0.0 ? g / gradRef : 0.0; };
    GradRatios g;
    g.r2 = ratio(R2.grad);
    g.r4 = hasSlow ? ratio(runs[4].grad) : 0.0;
    g.single = ratio(R1.gradSingle);
    g.noTaa = ratio(R0.grad);
    g.byteRatio = R1.byteGrad > 0.0 ? R2.byteGrad / R1.byteGrad : 0.0;
    return g;
}

/// 收敛正确性出表（⑦ 的被判数 + 两条只打印的参照 + 「域选错的代价」）。
void printTargetConvergence(const std::vector<Run>& runs, bool hasSlow, const GradRatios& g)
{
    std::printf("\n  收敛正确性（梯度能量比，线性域；参考 = R1 fb=1 一周期的相位平均 = %s）\n",
        d6(runs[1].grad).c_str());
    std::printf("    R2 fb=.12  梯度 %s  比值 %s", d6(runs[2].grad).c_str(), d6(g.r2).c_str());
    if (hasSlow)
        std::printf("   R4 慢累积  梯度 %s  比值 %s", d6(runs[4].grad).c_str(), d6(g.r4).c_str());
    std::printf("\n    参照（只打印不判）：完全不平均的单帧 = %s、R0（TAA 关，未抖）= %s\n",
        d6(g.single).c_str(), d6(g.noTaa).c_str());
    std::printf("    ⚠ 同一份数据在**显示空间**算这条给 %s（`mean(tonemap(x)) ≠ tonemap(mean(x))`，"
                "TAA 累积在线性 HDR 域、截图在 ACES+sRGB 之后）——域选错的代价，实测非引用\n",
        d6(g.byteRatio).c_str());
}

/// ⑤ 的工作点带：门态选带（门关 = 游动 + 闪烁，门开 ≈ 只剩闪烁 ⟹ 带整体下移）+ 方向措辞。
struct MagnitudeBand {
    double lo = 0.0, hi = 0.0;
    const char* dir = "";
};

MagnitudeBand magnitudeBand(double meanSwing, bool baseSpace)
{
    MagnitudeBand b;
    b.lo = baseSpace ? kMagnitudeMeanLoBase : kMagnitudeMeanLo;
    b.hi = baseSpace ? kMagnitudeMeanHiBase : kMagnitudeMeanHi;
    b.dir = meanSwing < b.lo ? "低于下限（信号消失向）"
                             : (meanSwing > b.hi ? "高于上限（回归向）" : "带内");
    return b;
}

/**
 * 七条判据（①..⑦，逐条的容差理由随字符串一起返回 = 打印时逐条附在判决后面）。
 *
 * 半间差的容差 = **两项之和**，两项各有独立机制，缺任何一项都会把正常读数判成失效：
 *   ① IIR 尾巴 (1−α)^warm × 满幅：热身结束时历史离收敛还差这么多，它是「同相位两半」
 *      唯一**物理**的差别（满幅取靶白电平 = mask 内 p99）。
 *   ② 量化项 = mask 内**最亮**电平处翻一格的最大真实跨度（`maxLsbStepUpTo`：逐字节扫真
 *      解码器取离散步长，**不用**闭式导数——闭式在 sRGB 拐点以下根本不是同一个函数，而这里
 *      读的就是 8bit 字节；且必须取 max 不是 p99，理由见其头注释）。①的残差经 ACES+sRGB
 *      量化后，落在分箱边界附近的那批像素在两半各翻一格。
 * 实测 ② 比 ① 大 36 倍 ⟹ 这条容差实际由量化决定，不是由 IIR 决定。
 */
std::vector<Verdict> buildTargetVerdicts(const std::vector<Run>& runs, bool hasSlow, double tau,
    bool baseSpace, double slowFeedback, const GradRatios& g)
{
    const Run& R0 = runs[0];
    const Run& R1 = runs[1];
    const Run& R2 = runs[2];
    const Run& R3 = runs[3];
    std::vector<Verdict> vs;

    // ① 确定性地板（复用 `determinism`：同配置 + 静止 ⟹ 逐位相同，无容差）。
    vs.push_back(verdictFor("determinism",
        R0.byteMaxVsFirst == 0.0 && R0.changedVsFirst == 0.0 && R0.stat.meanSwing == 0.0,
        "R0（TAA 关 + 静止机位）连拍 " + std::to_string(R0.stat.frames)
            + " 帧：与首帧逐字节最大偏离 " + std::to_string(static_cast<int>(R0.byteMaxVsFirst))
            + " LSB、变化像素 " + d2(R0.changedVsFirst * 100.0) + "%、meanSwing "
            + d6(R0.stat.meanSwing) + "。判据 = **恰为 0**（不是接近 0）；非零则一切残差读数"
              "是噪声比噪声"));

    // ② 折叠自检：R1（fb=1，**无记忆**）同相位两半必须逐位相同 ⟹ 半间差恰为 0。
    //    有记忆组容差 = IIR 尾巴闭式 (1−α)^warm × 满幅（= 靶白电平）。
    const double full = std::max(1e-9, R2.whiteLuma);
    const auto iirTol = [full, quant = R2.quant](double alpha, int warm) {
        return std::pow(1.0 - alpha, static_cast<double>(warm)) * full + quant;
    };
    const double tolR2 = iirTol(0.12, R2.warm);
    const double tolR4 = hasSlow ? iirTol(runs[4].feedback, runs[4].warm) : 0.0;
    vs.push_back(verdictFor("temporal-residual-phase-fold",
        R1.stat.halfGapMax == 0.0 && R2.stat.halfGapMax <= tolR2
            && (!hasSlow || runs[4].stat.halfGapMax <= tolR4),
        "同相位两半之差（折叠口径的前提）：R1（fb=1，无记忆）半间最大 "
            + d6(R1.stat.halfGapMax) + " 须**恰为 0**；R2（fb=.12）" + d6(R2.stat.halfGapMax)
            + " ≤ IIR 尾巴 (1−α)^warm × 满幅 + 一格真实跨度 " + d6(R2.quant) + " = " + d6(tolR2)
            + "；R4 " + d6(hasSlow ? runs[4].stat.halfGapMax : 0.0) + " ≤ " + d6(tolR4)
            + "。非零超界 = 连拍不是抖动相位的确定性函数（热身不足 / 窗口没对齐周期 / "
              "抓的不是连续帧），此时所有 swing 读数量的都是瞬态"));

    // ③ 阳性锚点：只抖不滤的残差必须 ≫ 0。下限写成 **τ 的倍数**（τ = 场景白电平处 1 个显示
    //    LSB 的线性跨度，运行时现算）——「量具有力气」的定义是「摆动能在 8bit 显示上走出至少
    //    一格」，与靶的绝对亮度无关，所以它不该是一个写死的线性数。
    const double anchorFloor = kAnchorMeanSwingTau * tau;
    vs.push_back(verdictFor("temporal-residual-anchor",
        R1.stat.meanSwing >= anchorFloor && R1.stat.fracOverTau >= kAnchorFracFloor,
        "R1（fb=1 只抖不滤 = 闪烁本体）meanSwing " + d6(R1.stat.meanSwing) + "（下限 = "
            + d2(kAnchorMeanSwingTau) + "×τ = " + d6(anchorFloor) + "，即 mask 平均摆动至少 1 个"
            "显示 LSB）、frac(swing>τ) " + d2(R1.stat.fracOverTau * 100.0) + "%（下限 "
            + d2(kAnchorFracFloor * 100.0) + "%）、maxSwing " + d6(R1.stat.maxSwing)
            + "。没有这条，R2/R3 的「残差为 0」可能只是量具坏了或靶没画"));

    // ④ 反向用例：掐掉激励源（jitter_scale=0）残差必须恰为 0。
    vs.push_back(verdictFor("temporal-residual-jitter-off",
        R3.stat.meanSwing == 0.0 && R3.stat.maxSwing == 0.0,
        "R3（TAA 开、fb=.12、**jitter_scale=0**）meanSwing " + d6(R3.stat.meanSwing)
            + "、maxSwing " + d6(R3.stat.maxSwing) + "、与首帧逐字节最大 "
            + std::to_string(static_cast<int>(R3.byteMaxVsFirst))
            + " LSB。判据 = **恰为 0**：抖动是唯一的激励源，静止机位 + 零抖动 ⟹ 每帧同一张图 "
              "⟹ 任何非零摆动都与「抖动」无关，是量具或引擎自己的噪声"));

    // ⑤ 工作点信号在场带（本命令的主体读数）。⚠ **带**而不是上限：下限红 = 信号消失（靶 /
    //    mask / 采集链路坏了，或漂移被修好了），上限红 = 回归变差。两种红的处置完全不同，
    //    所以打印里把方向写出来。门开态整幅游动被消掉 ⟹ 带整体下移，见常量表。
    const MagnitudeBand band = magnitudeBand(R2.stat.meanSwing, baseSpace);
    vs.push_back(verdictFor("temporal-residual-magnitude",
        R2.stat.meanSwing >= band.lo && R2.stat.meanSwing <= band.hi
            && R2.stat.fracOverTau <= kMagnitudeFracCap,
        "R2（fb=.12 现行工作点，收敛后）meanSwing " + d6(R2.stat.meanSwing) + " 须落在 ["
            + d6(band.lo) + ", " + d6(band.hi) + "]（" + band.dir + "；门关实测 0.022600 / 门开 "
            "0.004459）、frac(swing>τ) " + d2(R2.stat.fracOverTau * 100.0) + "%（上限 "
            + d2(kMagnitudeFracCap * 100.0) + "%）、p99.9 " + d6(R2.stat.p999Swing) + "、max "
            + d6(R2.stat.maxSwing)
            + "。**这是 `temporal` 四条全绿时仍然在场的那个绝对量**——抑制比好不等于看不见。"
              "⚠ 本条**不是可见性阈值**（可见性要按区域判，见注册表理由）"));

    // ⑥ 分辨力轴：把累积窗拉长（α→0.002，等效窗 500 帧 ≫ 周期 16）残差必须显著下降。
    if (hasSlow) {
        const Run& R4 = runs[4];
        vs.push_back(verdictFor("temporal-residual-monotone",
            R4.stat.meanSwing <= kMonotoneCap * R2.stat.meanSwing
                && R4.stat.fracOverTau < R2.stat.fracOverTau,
            "R4（fb=" + d4(slowFeedback) + "，等效窗 1/α = "
                + std::to_string(static_cast<int>(1.0 / slowFeedback))
                + " 帧 ≫ 抖动周期 16）meanSwing " + d6(R4.stat.meanSwing) + " 须 ≤ "
                + d2(kMonotoneCap * 100.0) + "% × R2 的 " + d6(R2.stat.meanSwing)
                + "，且 frac(swing>τ) " + d2(R4.stat.fracOverTau * 100.0) + "% 须 < R2 的 "
                + d2(R2.stat.fracOverTau * 100.0)
                + "%。**这条不判「TAA 好不好」，判「本量具在这条轴上有没有力气」**——不单调 "
                  "= swing 与累积窗无关 = 量具无效，⑤ 的读数一并作废"));
    } else {
        std::printf("\n  ⚠ 已跳过 R4（--skip-slow）：分辨力轴未验，⑤ 没有「量具有力气」的旁证\n");
    }

    // ⑦ 收敛正确性：线性域梯度比须贴着理想 16 相位平均。
    vs.push_back(verdictFor("temporal-residual-convergence",
        g.r2 >= kGradRatioLo && g.r2 <= kGradRatioHi,
        "R2 收敛连拍的相位平均 vs 理想 16 相位平均（= R1 fb=1 一周期的相位平均）的**线性域**"
        "梯度能量比 = "
            + d6(g.r2) + "（须落在 [" + d2(kGradRatioLo) + ", " + d2(kGradRatioHi)
            + "]）。低于下限 = 比理想平均还糊（过度累积 / 核太肥）；高于上限 = 没累积到"
              "（历史被拒 ⟹ 输出退化成本帧，同一次运行的参照：完全不平均的单帧比值 "
            + d6(g.single) + "）。⚠ 比值只能在**线性域**算：显示空间同一份数据给 "
            + d6(g.byteRatio) + "。⚠ 本条的**区分力随门变**：门关态收敛 0.887–0.893、退化参照 "
              "1.031–1.083（取连拍末帧，随 burst 起始相位浮动 5% ⟹ 只打印不判），上限 1.02 卡在"
              "两者中间 ⟹ 有区分力；门开态收敛 0.966–0.968、退化 1.004，上限 1.02 **高于**退化"
              "读数 ⟹ 上界在门开态是惰性的（只有下界有用），判「历史有没有生效」请用 ⑥ 的分辨力轴"));
    return vs;
}

/// 台账（`qa/log`）：判据可信度的一手数据。字段名是历史契约，**别改**。
void logTargetRun(const std::vector<Run>& runs, bool hasSlow, int periods, double warmMult,
    double tau, const GradRatios& g, const std::vector<Verdict>& vs)
{
    const Run& R0 = runs[0];
    const Run& R1 = runs[1];
    const Run& R2 = runs[2];
    const Run& R3 = runs[3];
    logCall("temporal-residual", {{"periods", std::to_string(periods)},
               {"warmMult", d4(warmMult)},
               {"tau", d6(tau)}, {"white", d6(R2.whiteLuma)},
               {"R0_swing", d6(R0.stat.meanSwing)}, {"R1_swing", d6(R1.stat.meanSwing)},
               {"R2_swing", d6(R2.stat.meanSwing)}, {"R3_swing", d6(R3.stat.meanSwing)},
               {"R4_swing", d6(hasSlow ? runs[4].stat.meanSwing : 0.0)},
               {"R2_p999", d6(R2.stat.p999Swing)}, {"R2_max", d6(R2.stat.maxSwing)},
               {"R2_frac", d2(R2.stat.fracOverTau * 100.0)},
               {"R1_frac", d2(R1.stat.fracOverTau * 100.0)},
               {"R1_halfGapMax", d6(R1.stat.halfGapMax)},
               {"R2_halfGapMax", d6(R2.stat.halfGapMax)},
               {"R2_gradRatio", d6(g.r2)}, {"R2_byteGradRatio", d6(g.byteRatio)},
               {"R4_gradRatio", d6(g.r4)},
               {"R1_singleGradRatio", d6(g.single)},
               {"R0_byteMax", std::to_string(static_cast<int>(R0.byteMaxVsFirst))},
               {"R3_byteMax", std::to_string(static_cast<int>(R3.byteMaxVsFirst))}},
        vs);
}
}  // namespace

/// 合成靶主路径 = 现行 7 条判据（`--bistro` / `--window-abl` 两个变体在命令入口就分岔走了）。
int runTargetResidual(
    int periods, double warmMult, double slowFeedback, bool skipSlow,
    bool baseSpace)
{
    if (warmMult < 6.0)
        std::printf("⚠ --warm-multiplier %g < 6：EMA 瞬态 (1−α)^t 在 t=6/α 处还剩 0.25%%，"
                    "再小量的就是瞬态不是稳态（红线 2）\n\n",
            warmMult);

    const std::string root = projectRoot() + "/build";
    double tau = 0.0;  // 第一组（R0）定一次，之后各组共用
    std::vector<Run> runs;
    try {
        runs = collectTargetRuns(
            root, periods, warmMult, slowFeedback, skipSlow, baseSpace, tau);
    } catch (const std::exception& e) {
        std::printf("  采集失败: %s\n", e.what());
        return 2;
    }

    const bool hasSlow = runs.size() > 4;
    printTargetHeader(runs, periods, baseSpace, tau);
    const GradRatios g = computeGradRatios(runs, hasSlow);
    printTargetConvergence(runs, hasSlow, g);

    const std::vector<Verdict> vs
        = buildTargetVerdicts(runs, hasSlow, tau, baseSpace, slowFeedback, g);
    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logTargetRun(runs, hasSlow, periods, warmMult, tau, g, vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
