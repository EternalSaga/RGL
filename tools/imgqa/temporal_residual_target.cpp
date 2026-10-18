// 合成靶（aa_target 细线靶）**采集层**：静止机位五组 R0–R4 的采集（`runGroup`）与
// 批 13.7 的长短窗对照变体（B1–B4，**只出数不判**）。
//
//   从 `temporal_residual.cpp` 拆出（文件护栏 500 代码行）。缝 = **采集 vs 判据**：这一层只负责
//   「按口径把连拍取回来、折成 `Run`」；七个阈值怎么判、打印什么文案、退出码是什么，全在
//   `temporal_residual_target_verdicts.cpp`。`Run` 是两层之间唯一的数据通道（定义在内部头）。
//
//   ⚠ 采集口径本身就是判据的一部分：热身走 `Engine::advanceToFrame`（读 RPC 帧计数空转到
//   **绝对锚点**、债-89）、机位一次钉死全程不动、配置一律走 `makeProfile`（红线 3：scene.lua
//   缺 `environment` 段 = aa_target 整帧全黑而残差恰好「完美为 0」= 最阴的假绿）。
//   `runWindowSweep` 的四组**不判**的理由见它上方注释（判据 ⑤ 是带，长窗读数撞短窗的带 =
//   把「变好了」判成「不合格」）。

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

// ── 靶机位（与 temporal.cpp 的 kCamPos / `rgl/aa_target.cppm` 头注释推荐机位同源）─────────
constexpr double kFovDeg = 45.0;
const Engine::Camera kTargetCamera{
    {0.0, 0.0, 2.0}, {0.0, 0.0, -1.0}, kFovDeg};

/**
 * 一组采集。配置一律走 `makeProfile`（红线 3：scene.lua 的 `environment` 段由它写，缺段
 * aa_target 整帧全黑而残差恰好「完美为 0」= 最阴的假绿）。
 *
 * 热身走 `Engine::advanceToFrame`（读 RPC 帧计数空转到绝对锚点，**不写盘**；债-89）：
 * α=0.002 要 3000 帧，用 `/shotburst` 热身就是写 3000 张 PNG（实测直接超时）。
 */
Run runGroup(const std::string& dir, const std::string& name, const std::string& tag, bool taa,
    double feedback, double jitterScale, int periods,
    double warmMult, double& tau, bool baseSpace, const ClipArm& arm)
{
    Run r;
    r.name = name;
    r.taa = taa;
    r.feedback = feedback;
    r.jitterScale = jitterScale;

    ProfileOpts o;
    o.entitiesLua = "{ type = \"aa_target\" }";  // 内存合成靶，无 glb 文件
    o.envIntensity = 0.0;                        // 背景 = 常量 clear 色，零时域噪声
    o.skybox = false;
    o.exposure = 1.0;
    o.aoStrength = 0.0;   // strength=0 逐位旁路，不给残差添第二来源
    o.shadowPass = false;
    o.picking = true;     // object_id = 区域的唯一出处
    o.aa = taa ? Aa::Taa : Aa::Off;
    o.taaFeedback = feedback;
    o.taaJitterScale = jitterScale;
    // 批 13.2 的门 `taa.base_space`：开 = 输出搬回**基空间**（引擎按内容偏移重采样，整幅
    // 游动被消掉）。本命令用它做**归因**而非判据：门关读数 = 游动 + 闪烁，门开读数 ≈ 只剩
    // 闪烁。差多少 = 绝对残差里有多少是「输出住在抖动空间」这个已登记机制（`temporal-swim-pixels`
    // 说过累积压不掉它），而不是"TAA 没干活"。
    o.taaBaseSpace = baseSpace;
    // 批 13.11 消融臂透传到合成靶：「修法不得是 bistro 专用」这条护栏要求**同一臂在
    // aa_target 上同向**，所以靶侧也必须能开臂（0 = 门关 = 不写键 = 逐位今天）。
    o.taaClipMode = arm.mode;
    o.taaClipSoft = arm.soft;
    o.size = {kSize[0], kSize[1]};
    makeProfile(dir, o);

    // 热身 = ⌈warmMult/α⌉ 向上对齐到抖动周期。α = feedback（EMA 记忆长度 1/α；长短窗两档
    // 都是**显式写进 feedback 的 α**，批 13.14b 删 static boost 后没有第二个权重源）；
    // TAA 关时没有历史，一个周期纯走稳采集本身。⚠ 热身不足**不会报错**，所以这里宁可多热。
    const double alpha = taa ? std::clamp(feedback, 1e-6, 1.0) : 1.0;
    const int warmRaw = static_cast<int>(std::ceil(warmMult / alpha));
    r.warm = ((warmRaw + kResidualPeriod - 1) / kResidualPeriod) * kResidualPeriod;

    const int frames = kResidualPeriod * periods;
    Engine eng(dir);
    eng.setCamera(kTargetCamera);  // 机位一次钉死，全程不动（唯一变化 = jitter 相位）
    // 债-89：热身 + 采集一步走 = 「空转到**绝对锚点**，burst 从锚点起」。到锚帧引擎已渲染
    // anchor = 基座 + warm 帧 ≥ warm，热身口径保持；burst 起始帧跨进程确定 ⟹ 抖动相位
    // 不再是彩票（改前 pristine 连跑结构签名都会变）。
    const uint32_t anchor = kBurstAnchorBase + static_cast<uint32_t>(r.warm);
    eng.advanceToFrame(anchor - kAnchorLead);
    const auto files = eng.shotBurstAt("screenshots", static_cast<uint32_t>(frames), anchor,
        tag + "_c");

    std::vector<Frame> shots;
    shots.reserve(files.size());
    std::vector<cv::Mat> gray;  ///< 显示空间报告用的 8UC1 灰（判据不读它，见 Run 注释）
    gray.reserve(files.size());
    cv::Mat first;
    for (size_t i = 0; i < files.size(); ++i) {
        Frame f;
        f.camera = kTargetCamera;
        f.bgr = readPngBgr(files[i]);
        if (i == 0)
            first = f.bgr.clone();
        else {
            cv::Mat d;
            cv::absdiff(first, f.bgr, d);
            r.byteMaxVsFirst = std::max(r.byteMaxVsFirst, cv::norm(d, cv::NORM_INF));
            r.changedVsFirst = std::max(r.changedVsFirst,
                static_cast<double>(cv::countNonZero(anyChannelDiff(first, f.bgr)))
                    / static_cast<double>(f.bgr.total()));
        }
        cv::Mat g;
        cv::cvtColor(f.bgr, g, cv::COLOR_BGR2GRAY);
        gray.push_back(std::move(g));
        prepareLinear(f);
        // 只留 luma：32 帧 × (bgr 2.7MB + lin 22MB) 是 790MB 的白烧内存，而本判据的每一个
        // 统计都只吃线性 luma（红线 1）；字节面在上面用完（确定性地板）就该放手。
        f.bgr.release();
        f.lin.release();
        shots.push_back(std::move(f));
    }

    const cv::Mat ids = decodeObjectId(readPngBgr(eng.shot(tag + "_id", "object_id")));
    cv::Mat interior;
    cv::compare(ids, 0, interior, cv::CMP_NE);  // ids 是 CV_32SC1 ⟹ 直接 8UC1
    const cv::Mat mask = dilateMask(interior, 3);  // 3px：吸收 jitter ±0.5px 的相位漂移
    r.avg = phaseAverageLuma(shots, periods);
    auto whiteVals = valuesInMask(r.avg, mask);
    std::sort(whiteVals.begin(), whiteVals.end());
    r.whiteLuma = percentileOf(whiteVals, 0.99);
    // 量化项走 **max**、τ 走 **p99**，两个电平故意分开（理由见 maxLsbStepUpTo 的头注释：容差要
    // 覆盖最亮那个像素翻一格，可见性阈值要的是靶白面的一格）。
    r.maxLuma = whiteVals.empty() ? 0.0 : whiteVals.back();
    r.quant = maxLsbStepUpTo(r.maxLuma);
    // τ 是**场景**属性（靶白电平处 1 个显示 LSB），不是每组各自的属性 ⟹ 只在第一组算一次，
    // 之后原样传下去。跨组换 τ = `frac` 的跨组比较失去意义 = 单调性轴作废。
    if (tau <= 0.0)
        tau = lsbLinearAtLuma(r.whiteLuma);
    r.stat = phaseSwingStat(shots, mask, tau, periods);
    // 显示空间同口径（**只报不判**，见 Run 注释）：同一份折叠代码，τ 换成「1 个显示 LSB」。
    const auto bc = swingFoldCore(r.avg.size(), mask, 1.0, periods,
        [&gray](size_t i, int y, int x) { return static_cast<double>(gray[i].at<uchar>(y, x)); });
    r.byteMeanSwing = bc.mean;
    r.byteP999 = bc.p999;
    r.byteFrac = bc.fracOver;
    r.byteMax = bc.mx;
    r.grad = gradEnergy(r.avg, mask);
    // **显示空间**同口径梯度（先 tonemap 再平均 = 红线 1 禁止的那个口径）。算出来只为打印：
    // 让「域选错会把 0.89 读成多少」是实测数，不是从文档里抄来的数。
    {
        cv::Mat acc = cv::Mat::zeros(r.avg.size(), CV_64FC1), tmp;
        for (const auto& g : gray) {
            g.convertTo(tmp, CV_64F);
            cv::add(acc, tmp, acc);
        }
        acc /= static_cast<double>(gray.size());
        r.byteGrad = gradEnergy(acc, mask);
    }
    r.gradSingle = gradEnergy(shots.back().luma, mask);
    // 在场性守卫（红线 3 的兜底）：全黑画面的残差**恰好为 0**，四条判据会全绿。
    if (r.stat.meanLuma <= 0.0 || r.stat.maskPixels == 0)
        throw std::runtime_error(name + "：画面是黑的（meanLuma=" + d6(r.stat.meanLuma)
            + "，mask " + std::to_string(r.stat.maskPixels) + " px）——配置丢了 environment 段？"
              "全零的残差不是「稳定」，是「没画」");
    return r;
}

void printRow(const Run& r)
{
    std::printf("  %-24s 热身 %5d  meanSwing %-9s p99.9 %-9s max %-8s frac>τ %5.2f%%  "
                "半间差 %8s/%-8s **超τ %lldpx**  与首帧 %2.0fLSB/%5.2f%%\n",
        r.name.c_str(), r.warm, d6(r.stat.meanSwing).c_str(), d6(r.stat.p999Swing).c_str(),
        d6(r.stat.maxSwing).c_str(), r.stat.fracOverTau * 100.0, d6(r.stat.halfGap).c_str(),
        d6(r.stat.halfGapMax).c_str(), static_cast<long long>(r.stat.halfGapOverPx),
        r.byteMaxVsFirst, r.changedVsFirst * 100.0);
    std::printf("  %-24s 显示域（只报不判）meanSwing %-7s p99.9 %-6s max %-5s frac>1LSB %5.2f%%\n",
        "", d4(r.byteMeanSwing).c_str(), d2(r.byteP999).c_str(), d2(r.byteMax).c_str(),
        r.byteFrac * 100.0);
}

// ── 批 13.7 合成靶**长短窗对照**变体（`--long-feedback` 显式给出即进）：**只出数不判** ────────
//
// 为什么不判（任务红线）：判据 ⑤（temporal-residual-magnitude）是**带**，其下限红 =
// 「信号消失向」，作者刻意设计成「修复落地后应当变红」——拿长窗档的低残差去撞短窗口径
// 的带，会把「变好了」判成「不合格」。默认判据保持短窗口径（7 条不变），本变体出
// 两档的 meanSwing / p99.9 / frac>τ 与收敛正确性（梯度比），选档由人判：
// **梯度比 ∈ [0.85, 1.02]（与 ⑦ 同带）前提下取 meanSwing 最低档**。
//
// 历史：本变体曾由 `--static-boost <地板>` 驱动（批 13.7，长窗藏在 `static_boost` 的
// `static_feedback` 地板里）；批 13.14b 删 boost 后长窗档 = 显式 `feedback`，α 语义不变。
//
// 四组（只差被测项，全部 aa_target 细线靶 + 静止机位；短窗 = fb 0.12，长窗 = `--long-feedback`）：
//   B1 fb=1     js=1 → 梯度参考（理想 16 相位平均；长窗档**不碰它**——把 fb=1 也压到长窗，
//      「只抖不滤」的参考语义就没了，同旧 boost 时代 B1 必须门关的理由）
//   B2 短窗 .12 js=1 → 短窗对照（= R2 口径）
//   B3 长窗 α  js=1 → 被测（热身按长窗 α，见 runGroup）
//   B4 长窗 α  js=0 → 反向哨兵：无激励源时长窗必须**恰为 0**（长窗只改权重值，
//      不该引入任何第二时域源；非零 = 长窗档接错了地方）

int runWindowSweep(const std::string& root, int periods, double warmMult,
    bool baseSpace, double longFeedback, const ClipArm& arm)
{
    double tau = 0.0;
    std::vector<Run> runs;
    try {
        runs.push_back(runGroup(root + "/imgqa_resb_b1", "B1 fb=1  js=1(梯度参考)", "resbb1",
            true, 1.0, 1.0, periods, warmMult, tau, baseSpace));
        runs.push_back(runGroup(root + "/imgqa_resb_b2",
            "B2 短窗 fb=.12 js=1(对照)", "resbb2", true, 0.12, 1.0, periods, warmMult,
            tau, baseSpace, arm));
        runs.push_back(runGroup(root + "/imgqa_resb_b3",
            "B3 长窗 fb=" + d4(longFeedback) + " js=1", "resbb3", true, longFeedback, 1.0,
            periods, warmMult, tau, baseSpace, arm));
        runs.push_back(runGroup(root + "/imgqa_resb_b4",
            "B4 长窗 fb=" + d4(longFeedback) + " js=0(反向)", "resbb4",
            true, longFeedback, 0.0, periods, warmMult, tau, baseSpace, arm));
    } catch (const std::exception& e) {
        std::printf("  采集失败: %s\n", e.what());
        return 2;
    }
    const Run& B1 = runs[0];
    const Run& B2 = runs[1];
    const Run& B3 = runs[2];
    const Run& B4 = runs[3];

    std::printf("长短窗对照变体（**只出数不判**）：aa_target %d×%d  静止机位  折叠 (2,%d,H,W)  "
                "周期数 %d  长窗档 α=%s  短窗档 α=0.12  base_space=%s\n",
        kSize[0], kSize[1], kResidualPeriod, periods, d4(longFeedback).c_str(),
        baseSpace ? "开（交付态）" : "关");
    std::printf("  ⚠ 判据 ⑤ 的带是短窗口径（下限红 = 信号消失向，修复落地后**应当**变红），"
                "本变体不判它；选档判据 = 梯度比 ∈ [0.85, 1.02]（同 ⑦）前提下 meanSwing 最低。\n");
    std::printf("  区域 = objectID 靶内部外扩 3px（%lld px）；τ = %s（B1 定，各组共用）\n\n",
        static_cast<long long>(B2.stat.maskPixels), d6(tau).c_str());
    // ⚠ **口径守卫（2026-10-06 实测踩过）**：本变体的 `base_space` 默认**关**，而交付态是**开**。
    //   关掉时历史住在抖动空间 ⟹ 残差被「每帧亚像素重采样历史」的复利糊化（批 13.1a/13.5 已修机制）
    //   主导，它会盖过、甚至**反向扭曲**裁剪臂的效应：同一对臂在关/开两口径下，`frac>τ` 与梯度比的
    //   好坏方向**相反**（关：frac 17.9%→30.4% 变差、梯度比 0.722→0.596 变糊；开：2.01%→0.03% 变好、
    //   0.932→1.011 变锐）。跨口径比较无效，故只要「消融臂 ≠ 0 且 base_space 关」就出声。
    if (!baseSpace && arm.mode > 0.5)
        std::printf("  🔴 **口径警告**：base_space=**关**（非交付态）却带着消融臂 mode=%d —— 本表量的"
                    "主要是复利糊化，不是裁剪效应，方向可能是**反的**。判修法请用 `--base-space`。\n",
            static_cast<int>(arm.mode + 0.5));
    for (const auto& r : runs)
        printRow(r);

    const double gradRef = B1.grad;
    const auto ratio = [gradRef](double g) { return gradRef > 0.0 ? g / gradRef : 0.0; };
    std::printf("\n  收敛正确性（梯度能量比，线性域；参考 = B1 fb=1 相位平均 = %s）\n",
        d6(gradRef).c_str());
    std::printf("    B2 短窗 比值 %s   B3 长窗 比值 %s（选档前提：B3 ∈ [0.85, 1.02]）\n",
        d6(ratio(B2.grad)).c_str(), d6(ratio(B3.grad)).c_str());
    std::printf("    B4 反向（长窗档 + 零抖动）meanSwing %s  maxSwing %s（期望**恰为 0**，"
                "非零 = 长窗档引入了第二时域源，本组读数作废）\n",
        d6(B4.stat.meanSwing).c_str(), d6(B4.stat.maxSwing).c_str());

    // 🔴 标签曾名 `temporal-residual-static-boost`（批 13.7 起）。批 13.14b 删 boost 后长窗档
    // 改由显式 `--long-feedback` 给出，标签同步换成中性名——**机器可读的身份里不许留已删机制**
    // （`qa/log/imgqa_calls.jsonl` 是只增历史：旧条目保留旧标签，那正是它们当时的真相；
    //  分界 = 本 commit，跨分界按标签查数要知道这里换过名）。键 `floor` 同步 → `longFeedback`。
    logCall("temporal-residual-window",
        {{"periods", std::to_string(periods)}, {"warmMult", d4(warmMult)},
            {"longFeedback", d4(longFeedback)}, {"baseSpace", baseSpace ? "1" : "0"},
            {"tau", d6(tau)}, {"B2_swing", d6(B2.stat.meanSwing)},
            {"B3_swing", d6(B3.stat.meanSwing)}, {"B3_p999", d6(B3.stat.p999Swing)},
            {"B3_frac", d2(B3.stat.fracOverTau * 100.0)},
            {"B2_p999", d6(B2.stat.p999Swing)}, {"B2_frac", d2(B2.stat.fracOverTau * 100.0)},
            {"B3_gradRatio", d6(ratio(B3.grad))}, {"B2_gradRatio", d6(ratio(B2.grad))},
            {"B4_swing", d6(B4.stat.meanSwing)}},
        {});
    return 0;
}

}  // namespace RGL::imgqa
