// 一机位的**编排层**（批 13.7b）：六组采集的先后、两条量具守卫在哪一步中止、热图与分带表
// 的出表顺序，以及回传给命令入口的退出码。
//
//   从 `temporal_residual.cpp` 拆出（文件护栏 500 代码行）。缝 = **编排 vs 采集原语**：采集在
//   `temporal_residual_bistro.cpp`，分带出表在 `temporal_residual_bands.cpp`，本 TU 只把它们串起来。
//
//   ⚠ 打印顺序与臂迭代顺序 = 契约（G0 地板 → N1–N6 读数 → 热图 → 分带 → 拒收）；G1 锚点红 =
//   **立刻 rc=2 中止**，不跑其余机位。`writeSwingHeat` / `percentileOfMat` 只被本 TU 用
//   （热图 scale 必须短窗/长窗共用，各图各自拉伸 = 「看起来更闪」的把戏）⟹ 留在本 TU。

#include "temporal_residual_metric.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

namespace {
/// 热图：逐像素 swing → INFERNO 8bit。**scale 必须短窗/长窗共用**——各图各自拉伸 =
/// 「看起来更闪」的把戏，肉眼比对当场失效。返回实际用的 scale（回显进 stdout）。
double writeSwingHeat(const std::string& path, const cv::Mat& swing, double scale)
{
    cv::Mat d;
    swing.convertTo(d, CV_8UC1, 255.0 / std::max(scale, 1e-12));  // saturate cast = 截到 [0,255]
    cv::Mat heat;
    cv::applyColorMap(d, heat, cv::COLORMAP_INFERNO);
    if (!cv::imwrite(path, heat))
        throw std::runtime_error("热图写入失败: " + path);
    return scale;
}

/// 全帧 swing 图的 p 分位（共用 scale 从它来）。
double percentileOfMat(const cv::Mat& m, double p)
{
    std::vector<double> v;
    v.reserve(m.total());
    for (int y = 0; y < m.rows; ++y) {
        const double* px = m.ptr<double>(y);
        v.insert(v.end(), px, px + m.cols);
    }
    std::sort(v.begin(), v.end());
    return percentileOf(v, p);
}

/// G0 确定性地板回显（判据 = **恰为 0**；红不红由调用侧汇总进 rc，这里只如实打）。
void printBistroFloorGuard(const BistroRun& n0, bool floorOk)
{
    std::printf("  %s G0 确定性地板（N0 TAA关）：与首帧 %2.0fLSB / %lldpx / meanSwing %s"
                "（判据 = **恰为 0**）\n",
        floorOk ? "✓" : "✗", n0.byteMaxVsFirst, static_cast<long long>(n0.changedPx),
        d6(n0.stat.meanSwing).c_str());
}

/**
 * 本机位其余各组：N1 短窗（定 τ，也是分带的尺子）→ N2 长窗 → N3/N5 反向 → N4 梯度参考。
 *
 * ⚠ 组序 = 打印序 = 契约。G1 阳性锚点（只钉 doorframe——用户报障机位是既成事实；其余机位
 *   接近 0 如实打印）挂零 ⟹ 返回 2 **立刻停下**，不跑其余机位、不出任何长窗结论。
 */
int collectBistroGroups(const BistroCam& cam, int periods, double warmMult,
    double longFeedback, const std::string& dir, const std::string& pre, double& tau,
    const ClipArm& arm, std::vector<BistroRun>& runs)
{
    try {
        runs.push_back(runBistroGroup(dir + "_n1",
            "N1 短窗 fb=.12 js=1", pre + "n1", cam, 0.12, 1.0, periods, warmMult, tau, true, true,
            true, arm));
        // G1 阳性锚点（只钉 doorframe——用户报障机位是既成事实；其余机位接近 0 如实打印）。
        if (std::string(cam.name) == "doorframe" && runs[0].stat.fracOverTau < kAnchorFracFloor) {
            std::printf(
                "\n  ✗ 阳性锚点挂零：doorframe 短窗 frac(swing>τ) = %s%% < %s%%。\n"
                "    用户肉眼看得见闪烁是既成事实，量出来接近 0 = **量具坏了**（或症状在此口径下\n"
                "    根本不存在——那也是要紧的负结果）。立刻停下，不跑其余机位，不出任何长窗结论。\n",
                d2(runs[0].stat.fracOverTau * 100.0).c_str(), d2(kAnchorFracFloor * 100.0).c_str());
            return 2;
        }
        runs.push_back(runBistroGroup(projectRoot() + "/build/imgqa_resbis_" + cam.name + "_n2",
            "N2 长窗 fb=" + d4(longFeedback) + " js=1", pre + "n2", cam, longFeedback, 1.0,
            periods, warmMult, tau, true, true, true, arm));
        runs.push_back(runBistroGroup(projectRoot() + "/build/imgqa_resbis_" + cam.name + "_n3",
            "N3 反向 长窗 js=0", pre + "n3", cam, longFeedback, 0.0, periods, warmMult, tau,
            false));
        runs.push_back(runBistroGroup(projectRoot() + "/build/imgqa_resbis_" + cam.name + "_n5",
            "N5 反向对照 js=0短窗", pre + "n5", cam, 0.12, 0.0, periods, warmMult, tau, false));
        runs.push_back(runBistroGroup(projectRoot() + "/build/imgqa_resbis_" + cam.name + "_n4",
            "N4 梯度参考 fb=1", pre + "n4", cam, 1.0, 1.0, periods, warmMult, tau, false));
    } catch (const std::exception& e) {
        std::printf("  %s 采集失败: %s\n", cam.name, e.what());
        return 2;
    }
    return 0;
}

/// 出表后的三个比值（既进 stdout 也进台账；分母为 0 时取 0，与判决线同口径）。
struct BistroRatios {
    double swing = 0.0, gradLin = 0.0, gradDisp = 0.0;
};

/// τ 回显 + 逐组读数行（半间差对闭式容差的热身自证）+ 长窗/短窗比 + 债-47 梯度能量比。
BistroRatios printBistroReadouts(double tau, const std::vector<BistroRun>& runs)
{
    const BistroRun& N1 = runs[0];
    const BistroRun& N2 = runs[1];
    const BistroRun& N4 = runs[4];
    BistroRatios out;
    std::printf("  τ = %s（N1 定，各组共用）白电平 p99 = %s  量化项 = %s\n", d6(tau).c_str(),
        d6(N1.whiteLuma).c_str(), d6(N1.quant).c_str());
    for (const auto& r : runs) {
        // 半间差闭式容差 = IIR 尾巴 (1−α)^warm × 满幅 + 一格真实跨度（热身自证，见头注释）。
        const double tol =
            std::pow(1.0 - r.alpha, static_cast<double>(r.warm)) * std::max(1e-9, N1.whiteLuma)
                + N1.quant;
        std::printf("  %-18s 热身 %5d  meanSwing %-9s p99.9 %-9s max %-9s frac>τ %6.2f%%  "
                    "半间差max %s%s%s **超τ %lldpx**  与首帧 %2.0fLSB/%lldpx\n",
            r.name.c_str(), r.warm, d6(r.stat.meanSwing).c_str(), d6(r.stat.p999Swing).c_str(),
            d6(r.stat.maxSwing).c_str(), r.stat.fracOverTau * 100.0,
            d6(r.stat.halfGapMax).c_str(), r.stat.halfGapMax > tol ? "（>容差 " : "（≤容差 ",
            d6(tol).c_str(), static_cast<long long>(r.stat.halfGapOverPx), r.byteMaxVsFirst,
            static_cast<long long>(r.changedPx));
    }
    const double swingRatio =
        N1.stat.meanSwing > 0.0 ? N2.stat.meanSwing / N1.stat.meanSwing : 0.0;
    std::printf("  长窗/短窗：meanSwing ×%s（%s→%s）  p99.9 %s→%s  frac>τ %s%%→%s%%\n",
        d4(swingRatio).c_str(), d6(N1.stat.meanSwing).c_str(), d6(N2.stat.meanSwing).c_str(),
        d6(N1.stat.p999Swing).c_str(), d6(N2.stat.p999Swing).c_str(),
        d2(N1.stat.fracOverTau * 100.0).c_str(), d2(N2.stat.fracOverTau * 100.0).c_str());
    const double gradLin = N4.grad > 0.0 ? N1.grad / N4.grad : 0.0;
    const double gradDisp = N4.byteGrad > 0.0 ? N1.byteGrad / N4.byteGrad : 0.0;
    std::printf("  债-47 梯度能量比（N1 收敛 / N4 理想 16 相位平均）：**线性域 %s** ｜ 显示域 %s\n",
        d6(gradLin).c_str(), d6(gradDisp).c_str());
    std::printf("    （§1.5 旧账 0.671 = 显示空间读数；aa_target 同口径 显示 0.669 ↔ 线性 0.985。"
                "本行只出数，改判由主 agent 记账）\n");
    out.swing = swingRatio;
    out.gradLin = gradLin;
    out.gradDisp = gradDisp;
    return out;
}

/// G2 反向守卫的两个布尔（`ok` = N3 恰 0 进 rc；`floorZero` = N5 恰 0 只作归因）。
struct BistroReverseGuard {
    bool ok = false;
    bool floorZero = false;
};

/// G2 反向守卫的执行与两态归因（判据本体见下面原样搬来的注释块）。
    // G2 反向守卫：js=0 + 静止 ⟹ 首帧起历史 = 本帧（EMA 不动点）⟹ 残差**恰为 0**，无容差。
    // 非零时先归因再下结论：N5（js=0 且短窗 α=.12）是同场景的地板对照。doorframe 两轮实测
    // （PIL 复核）：N3 = **8 个固定像素 × 1 LSB、奇偶帧两态交替**（偶帧=首帧，奇帧翻），7/8 个
    // 落在强边缘（|∇luma| 47–115 vs 全图中值 4.5）且是暗部（luma 13–32）；N5（短窗）同现
    // （7px，maxSwing 反而更大）⟹ 地板与窗档无关。机制候选 = `jitter_scale=0` 时
    // jitterPx = (halton−0.5)·0 = **±0.0**，其符号随 Halton 相位逐帧交替（实测奇偶结构与之吻合），
    // 经 `proj[2][0] -= 2jx/W` 进入投影矩阵 ⟹ 恰压在边上的片元 tie-break 逐帧翻。N0（TAA 全关）
    // 与 N6（base_space 关）两个对照负责把「TAA 特性内 vs 之外」「base_space 路径 vs 公共路径」
    // 定死。⚠ 本注释早期版本写过「固定 ~1255 像素集」= 自写 PNG 解码器的伪影，已用 PIL 推翻。
BistroReverseGuard printBistroReverseGuard(const BistroRun& N3, const BistroRun& N5)
{
    const bool revOk = N3.stat.meanSwing == 0.0 && N3.stat.maxSwing == 0.0;
    const bool revFloor = N5.stat.meanSwing == 0.0 && N5.stat.maxSwing == 0.0;
    std::printf("  %s 反向守卫（N3 长窗 js=0）：meanSwing %s maxSwing %s 变化 %lldpx"
                "（期望**恰为 0**）\n",
        revOk ? "✓" : "✗", dExp(N3.stat.meanSwing).c_str(), dExp(N3.stat.maxSwing).c_str(),
        static_cast<long long>(N3.changedPx));
    std::printf("  %s 反向对照（N5 短窗 js=0，α=.12）：meanSwing %s maxSwing %s 变化 %lldpx"
                "（地板参照：非零 ⟹ js=0 残差地板是**场景/驱动**的，非长窗档引入）\n",
        revFloor ? "✓" : "⚠", dExp(N5.stat.meanSwing).c_str(), dExp(N5.stat.maxSwing).c_str(),
        static_cast<long long>(N5.changedPx));
    if (!revOk && !revFloor)
        std::printf("    ⚠ 两态同现（短窗地板 %s vs 长窗 %s）⟹ 归因 = 引擎 js=0 既有 tie-flip，"
                    "「恰为 0」红线在 bistro 上不可达（合成靶 aa_target 的 R3/B4 是 0，场景不同）；"
                    "N1/N2 读数**不作废**但守卫保持红，处置由主 agent 判\n",
            d6(N5.stat.maxSwing).c_str(), d6(N3.stat.maxSwing).c_str());
    if (!revOk && revFloor)
        std::printf("    ✗ N3 非零而 N5（短窗同 js=0）恰为 0 ⟹ **长窗档自己引入了第二时域源**，"
                    "本机位 N1/N2 长窗读数作废\n");
    return { revOk, revFloor };
}

/// N6 诊断（`--diag-bs-off`）：长窗 js=0 但 **base_space 关**。返回「N6 是否非零」。
/// ⚠ 只作归因信息，**不进 rc**（诊断组不是判据）。
// N6 诊断（`--diag-bs-off`）：长窗 js=0 但 **base_space 关**——摘掉 un-jitter 与 c_{t−1}
// 修正、其余（jitter 注入 proj、历史裁剪盒、EMA）不动。N3 非零而 N6 恰 0 ⟹ 翻转住在
// base_space 路径；N6 同非零 ⟹ 住在两者共用部分（jitter 注入 proj 的 ±0.0 / 历史裁剪盒）。
bool printBistroDiagBsOff(const BistroCam& cam, int periods, double warmMult,
    double longFeedback, const std::string& dir, const std::string& pre, double tau,
    bool diagBsOff)
{
    if (!diagBsOff)
        return false;
    double tau6 = tau;  // τ 已非零，不会被 N6 覆写
    const BistroRun N6 = runBistroGroup(dir + "_n6", "N6 诊断 js=0 bs关", pre + "n6", cam,
        longFeedback, 0.0, periods, warmMult, tau6, false, true, false);
    const bool sixOk = N6.stat.meanSwing == 0.0 && N6.stat.maxSwing == 0.0;
    std::printf("  %s N6 诊断（js=0 长窗 base_space关）：meanSwing %s maxSwing %s %lldpx"
                "（与 N3 对比归因：N3 非零而 N6 恰 0 = 翻转在 base_space 路径）\n",
        sixOk ? "✓" : "⚠", d6(N6.stat.meanSwing).c_str(), d6(N6.stat.maxSwing).c_str(),
        static_cast<long long>(N6.changedPx));
    return !sixOk;
}

/// 热图：短窗/长窗**共用** scale（各图各自拉伸 = 「看起来更闪」的把戏）。返回实际用的 scale。
double printBistroHeat(const std::string& shotDir, const std::string& camName,
    const BistroRun& N1, const BistroRun& N2, double tau)
{
    const double scale = std::max(percentileOfMat(N1.swingMap, 0.999), 1e-12);
    const std::string offPath = shotDir + "/" + camName + "_swing_boost-off.png";
    const std::string onPath = shotDir + "/" + camName + "_swing_boost-on.png";
    writeSwingHeat(offPath, N1.swingMap, scale);
    writeSwingHeat(onPath, N2.swingMap, scale);
    std::printf("  热图（共用 scale = 短窗 p99.9 = %s，白电平处 1 LSB τ = %s；文件名 boost-off/on "
                "= 历史遗留，语义 = 短窗/长窗）：\n", d6(scale).c_str(),
        d6(tau).c_str());
    std::printf("    %s\n    %s\n", offPath.c_str(), onPath.c_str());
    return scale;
}

/// 批 13.10 拒收交叉表的两次采集与两张表（N7 按短窗态分档 / N8 按长窗态分档）。
/// 返回 2 = 采集失败。
    // 批 13.10：历史拒收 × 残余 交叉表（`--reject`）。它要回答的是本命令此前**答不了**的问题：
    // 长窗把 meanSwing 压到 ×0.25，但纯 EMA 数学的下界是 ×0.082（|H| 比）——**压不满的那部分
    // 住在哪**。判决线跑前写死 = `temporal_residual.h`「历史拒收 × 残余」段。
int collectBistroReject(const BistroCam& cam, int periods, double warmMult,
    double longFeedback, const std::string& dir, const std::string& pre,
    const BistroRun& N1, const BistroRun& N2, double tau, RejectReport& rejOff,
    RejectReport& rejOn)
{
    try {
        // 两次调用喂**同一对 swing**（长窗 N2 / 短窗 N1），差别只在**分档用的拒收图**：
        // N7 按短窗态（α=.12）的盒行为分档、N8 按长窗态分档。主判据只在长窗那次出
        // （alphaOn>0），短窗那次没有「压」这个动作，比无可比。
        rejOff = reportRejectAttribution(
            runBistroRejectGroup(dir + "_rejoff", "N7 拒收码 短窗", pre + "n7", cam, 0.12,
                periods, warmMult),
            N2.swingMap, N1.swingMap, tau, std::string(cam.name) + " · 分档 = 短窗 N1（fb=.12）",
            0.0, 0.12);
        rejOn = reportRejectAttribution(
            runBistroRejectGroup(dir + "_rejon", "N8 拒收码 长窗", pre + "n8", cam,
                longFeedback, periods, warmMult),
            N2.swingMap, N1.swingMap, tau,
            std::string(cam.name) + " · 分档 = 长窗 N8（长窗档，用户现状）", longFeedback,
            0.12);
    } catch (const std::exception& e) {
        std::printf("\n  ✗ 拒收交叉表采集失败: %s\n", e.what());
        return 2;
    }
    return 0;
}

/// 台账行的类型（`logCall` 的入参面）。
using LogFields = std::vector<std::pair<std::string, std::string>>;

/// 台账通用字段：口径（含 🔴 消融档位）+ 五组读数 + 批 13.7c 分带。
/// ⚠ 字段名与**追加顺序**是历史契约，**别改、别重排**（三段拼起来 = 原来那一坨）。
LogFields logFieldsCamera(const BistroCam& cam, int periods, double warmMult,
    double longFeedback, const ClipArm& arm, double tau, bool floorOk, const BistroRun& n0,
    const BistroRun& N1, const BistroRun& N2, const BistroRun& N3, const BistroRun& N5,
    bool sixRan, bool sixNonZero, const BistroRatios& ratio, double scale,
    const BandReport& band)
{
    return {
        {"camera", cam.name}, {"periods", std::to_string(periods)},
        // 🔴 档位必须进机器可读日志：本工具整块替换 taa 段，`--clip` 不给 = 不写键 =
        // 引擎默认 0，而交付默认自批 13.13 起是 11。臂 0 与臂 11 的 ρ 差 3 倍，而**历史条目
        // 里根本没有这一列** ⟹ 「这个已发表数是哪个臂」事后无从审计（债-67）。
        {"clipMode", d4(arm.mode)}, {"clipSoft", d4(arm.soft)},
        {"warmMult", d4(warmMult)}, {"longFeedback", d4(longFeedback)}, {"tau", d6(tau)},
        {"N0_floor_ok", floorOk ? "1" : "0"},
        {"N0_px", std::to_string(n0.changedPx)},
        {"N1_swing", d6(N1.stat.meanSwing)}, {"N1_p999", d6(N1.stat.p999Swing)},
        {"N1_frac", d2(N1.stat.fracOverTau * 100.0)},
        {"N2_swing", d6(N2.stat.meanSwing)}, {"N2_p999", d6(N2.stat.p999Swing)},
        {"N2_frac", d2(N2.stat.fracOverTau * 100.0)},
        {"N3_swing", d6(N3.stat.meanSwing)}, {"N3_max", d6(N3.stat.maxSwing)},
        {"N3_px", std::to_string(N3.changedPx)}, {"N5_swing", d6(N5.stat.meanSwing)},
        {"N5_max", d6(N5.stat.maxSwing)}, {"N5_px", std::to_string(N5.changedPx)},
        {"N6_ran", sixRan ? "1" : "0"}, {"N6_nonzero", sixNonZero ? "1" : "0"},
        {"swingRatio", d4(ratio.swing)}, {"gradLin", d6(ratio.gradLin)},
        {"gradDisp", d6(ratio.gradDisp)}, {"heatScale", d6(scale)},
        // 批 13.7c 亮度×梯度分带（判决 + 判决线两侧的原始数，事后复核用）
        {"bandVerdict", band.verdict}, {"bandT_hi", d2(band.tHi)},
        {"bandGradThr", d6(band.gradThr)}, {"bandShareDrift", d6(band.shareDrift)},
        {"bandA_ratio", d4(band.rA)}, {"bandB_ratio", d4(band.rB)},
        {"bandB_massOff", d2(band.bMassOff * 100.0)},
        {"bandB_massOn", d2(band.bMassOn * 100.0)},
    };
}

/// 批 13.10 拒收交叉表字段（短窗侧）。`--reject` 没跑时留**空串**（列必须在、值可空）。
LogFields logFieldsRejectOff(bool reject, const RejectReport& rejOff)
{
    return {
        // 批 13.10 拒收交叉表（`--reject` 才跑；未跑 = 空串）
        {"rejOffBranch", reject ? std::to_string(rejOff.branch) : ""},
        {"rejOff_r0OverAll", reject ? d4(rejOff.rAll > 0 ? rejOff.r0 / rejOff.rAll : -1.0) : ""},
        {"rejOff_m3p", reject ? d4(rejOff.m3p) : ""},
        {"rejOff_collapse", reject ? d4(rejOff.collapsed) : ""},
        {"rejOff_clip", reject ? d4(rejOff.clipped) : ""},
    };
}

/// 批 13.10 拒收交叉表（长窗侧）+ 主判据（闭式锚定抑制保真度）字段。
/// 没跑时同样留空串；`fidRan` 就是「这次到底跑没跑」的机器可读位。
LogFields logFieldsFidelity(bool reject, const RejectReport& rejOn)
{
    return {
        {"rejOnBranch", reject ? std::to_string(rejOn.branch) : ""},
        {"rejOn_r0OverAll", reject ? d4(rejOn.rAll > 0 ? rejOn.r0 / rejOn.rAll : -1.0) : ""},
        {"rejOn_m3p", reject ? d4(rejOn.m3p) : ""},
        {"rejOn_collapse", reject ? d4(rejOn.collapsed) : ""},
        {"rejOn_clip", reject ? d4(rejOn.clipped) : ""},
        // 主判据（闭式锚定抑制保真度）
        {"fidRan", reject ? "1" : "0"},
        {"fidPass", reject ? (rejOn.fidelityPass ? "1" : "0") : ""},
        {"fidRho", reject ? d6(rejOn.rho) : ""},
        {"fidRhoStar", reject ? d6(rejOn.rhoStar) : ""},
        {"fidLine", reject ? d6(rejOn.line) : ""},
        {"fidBin0Rho", reject ? d6(rejOn.bin0Rho) : ""},
    };
}

/// 台账（`qa/log`）：三段字段按历史顺序拼成一行。
void logBistroCamera(const BistroCam& cam, int periods, double warmMult, double longFeedback,
    const ClipArm& arm, double tau, bool floorOk, const BistroRun& n0, const BistroRun& N1,
    const BistroRun& N2, const BistroRun& N3, const BistroRun& N5, bool sixRan,
    bool sixNonZero, const BistroRatios& ratio, double scale, const BandReport& band,
    bool reject, const RejectReport& rejOff, const RejectReport& rejOn)
{
    LogFields args = logFieldsCamera(
        cam, periods, warmMult, longFeedback, arm, tau, floorOk, n0, N1, N2, N3, N5, sixRan,
        sixNonZero, ratio, scale, band);
    const LogFields rej = logFieldsRejectOff(reject, rejOff);
    args.insert(args.end(), rej.begin(), rej.end());
    const LogFields fid = logFieldsFidelity(reject, rejOn);
    args.insert(args.end(), fid.begin(), fid.end());
    logCall("temporal-residual-bistro", args, {});
}

}  // namespace
/// 一机位的六组（N6 仅 `--diag-bs-off`；N7/N8 仅 `--reject`）+ 出表 + 守卫 + 热图。
/// 返回 0 = 守卫全过；1 = 某守卫红；2 = 采集失败/锚点中止。
///
/// 本函数只走骨架（地板 → 六组 → 出表 → 守卫 → 热图 → 分带 → 拒收 → 台账 → rc 汇总）；
/// ⚠ **打印顺序与臂迭代顺序 = 结构签名**，下面段函数的调用次序就是契约，别重排。
int runBistroCamera(const BistroCam& cam, int periods, double warmMult, double longFeedback,
    const std::string& shotDir, bool diagBsOff, bool reject, const ClipArm& arm)
{
    double tau = 0.0;
    std::vector<BistroRun> runs;
    const std::string pre = "rb" + std::string(1, cam.name[0]);
    const std::string dir = projectRoot() + "/build/imgqa_resbis_" + cam.name;
    // G0 确定性地板（TAA 整个关 = 连 jitter 注入都不发生）：静止机位连拍必须**逐位相同**。
    // 这是 `determinism` 地板在 bistro 交付配置上的实例化。它非零 ⟹ 非确定性在 TAA 之外
    // （更底层的引擎/驱动问题），本表所有读数存疑；它为零而 N3/N5 非零 ⟹ 非确定性住在
    // **TAA 特性内**（jitter 注入 proj 的 ±0.0 / 历史裁剪盒 / base_space 的 un-jitter·c_t）。
    // τ 用局部 throwaway（地板组的白电平不参与判据 τ，τ 一律由 N1 定）。
    BistroRun n0;
    {
        double tau0 = 0.0;
        try {
            n0 = runBistroGroup(dir + "_n0", "N0 TAA关(地板)", pre + "n0", cam, 0.12, 1.0, periods,
                warmMult, tau0, false, false);
        } catch (const std::exception& e) {
            std::printf("  %s N0 采集失败: %s\n", cam.name, e.what());
            return 2;
        }
    }
    const bool floorOk = n0.byteMaxVsFirst == 0.0 && n0.changedPx == 0 && n0.stat.meanSwing == 0.0;
    std::printf("\n── %s ──\n", cam.name);
    printBistroFloorGuard(n0, floorOk);
    // ⚠ 消融臂只加在 N1/N2（短窗/长窗）——N0/N3/N4/N5 是**反向守卫与梯度参考**，
    //   让它们也吃臂 = 守卫自己被被测改动污染，那就不是守卫了（债-55 型：键生效在错误的组上）。
    if (arm.mode > 0.5)
        std::printf("  ⚠ 消融臂 mode=%d soft=%s **只加在 N1/N2**；N0/N3/N4/N5 保持基线\n",
            static_cast<int>(arm.mode + 0.5), d4(arm.soft).c_str());

    if (const int rc = collectBistroGroups(
            cam, periods, warmMult, longFeedback, dir, pre, tau, arm, runs);
        rc != 0)
        return rc;
    const BistroRun& N1 = runs[0];
    const BistroRun& N2 = runs[1];
    const BistroRun& N3 = runs[2];
    const BistroRun& N5 = runs[3];  // 反向**对照**：js=0 且短窗（α=.12）

    const BistroRatios ratio = printBistroReadouts(tau, runs);
    const BistroReverseGuard rev = printBistroReverseGuard(N3, N5);
    const bool sixNonZero = printBistroDiagBsOff(
        cam, periods, warmMult, longFeedback, dir, pre, tau, diagBsOff);

    // 热图：短窗/长窗共用 scale（短窗 swing 的 p99.9，截顶显示；各图各自拉伸 = 把戏）。
    const double scale = printBistroHeat(shotDir, cam.name, N1, N2, tau);

    // 亮度 × 梯度 二维分带（判「残余住在高光边还是普通边缘」）。判决线**跑前写死**，
    // 全文 = temporal_residual.h 同名段；这里只执行。守卫红不拦它——分带表要回答的正是
    // 「N1/N2 这两个读数里的残余长什么样」，与反向守卫是否可达无关。
    const BandReport band = reportLumaGradBands(N1, N2, tau);

    RejectReport rejOff, rejOn;
    if (reject) {
        if (const int rc = collectBistroReject(
                cam, periods, warmMult, longFeedback, dir, pre, N1, N2, tau, rejOff, rejOn);
            rc != 0)
            return rc;
    }

    logBistroCamera(cam, periods, warmMult, longFeedback, arm, tau, floorOk, n0, N1, N2, N3,
        N5, diagBsOff, sixNonZero, ratio, scale, band, reject, rejOff, rejOn);
    // 守卫汇总：G0 地板与 G2 反向都计入 rc（G0 红 = 全表读数存疑，比 G2 更严重，绝不吞）。
    // 主判据红 ⟹ rc=1（`temporal-residual-window-fidelity`，线跑前写死）。它与两条守卫同级别：
    // 都是「这条命令的结论不许被读成通过」。
    // 主判据红 ⟹ rc=1（`temporal-residual-window-fidelity`，线跑前写死）。它与两条守卫同级别：
    // 都是「这条命令的结论不许被读成通过」。
    if (reject && rejOn.fidelityRan && !rejOn.fidelityPass) {
        std::printf("\n  ✗ 主判据 `temporal-residual-window-fidelity` **FAIL**：ρ = %s > 线 %s"
                    "（ρ* 闭式 %s，超 %s×）\n    ⟹ 长窗档的抑制没到闭式的 2/3，差额住在被"
                    "邻域盒夹过的像素上（见上表 ρ 列）\n",
            d4(rejOn.rho).c_str(), d4(rejOn.line).c_str(), d4(rejOn.rhoStar).c_str(),
            d4(rejOn.rho / rejOn.line).c_str());
        return 1;
    }
    return (rev.ok && floorOk) ? 0 : 1;
}

}  // namespace RGL::imgqa