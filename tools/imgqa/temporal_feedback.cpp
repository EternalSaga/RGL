// temporal-feedback —— **运动态 feedback 扫描**（批次 B11 从 `temporal_motion.cpp` 拆出：
// 那是同一文件里的**第二台仪器**，2748 代码行 > 500 逼的刀，缝按仪器切）。
// 设计全文（已确立的前提 / 待验假设 / 臂表 / 判决线）= 下面那段原样搬来的头注释。
#include "temporal_motion.h"

#include "imgqa.h"
#include "temporal_residual.h"
#include "temporal_yaw_remap.h"

#include <algorithm>
#include <cmath>
#include <numeric>  // std::accumulate（阶跃主指标 AUC = Σ ghost(n)）
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
using namespace tmotion;  // 本族跨 TU 的类型 / 常量 / 算子（唯一出处 = 本文件的第一条 include）

// ═══════════════════════════════════════════════════════════════════════════════
// temporal-feedback —— **运动态 feedback 扫描**（把「历史在非零分数偏移上被 Catmull-Rom
// 重采样的复利低通」这条假设变成数；只出数 + 一句判决，不改引擎默认值、不动 docs/）
// ═══════════════════════════════════════════════════════════════════════════════
//
// ## 已确立的前提（`temporal-motion` 量的，本命令不重复做）
//
// R = dev(M1)/dev(M0) = 1.138 > 1（相机运动下 TAA **主动变差**），静止可达下界
// dev(M4)/dev(M0) = 0.779；`taa.diag_reject` 显示 M1 收下 90.36% / 盒拒收 **0.00%**
// ⟹ **不是历史被丢，是收下的历史内容不对**。
//
// ## 待验假设（本命令**判它**，不是信它）
//
//   静止 + base_space 下历史采样偏移 ≈0 ⟹ CR 核退化为恒等（权重 (0,1,0,0)）⟹ 零低通；
//   相机一动，**每帧**历史都在非零分数偏移上被 CR 重采样 ⟹ 每帧一次低通、在 ≈1/α 帧的
//   累积窗内**复利** ⟹ 越攒越糊。（与 §1.5/批 13.1e 的「双线性复利」同族**不同因**：
//   那次病灶是核形状，这次是偏移非零。）
//
// 可检验推论只有一个方向：偏差随累积窗变长而恶化 ⟹ 运动态把 `feedback`（= **本帧权重**，
// 调大 = 窗变短）拉高应让偏差**回落**，甚至低于 M0。
//
// ## 为什么 fb=1 那一格不许跳过
//
// fb=1 ⟹ `lerp(clipped, cur, 1) = cur` ⟹ **零累积**，但 **jitter 仍在**（投影照抖、
// base_space 照 un-jitter）。故 R(fb=1) = 「**抖动本身**在运动下贡献了多少偏差」——M0 连
// jitter 都没有。这一格把「累积」与「抖动」两个贡献拆开，也正是判决 ②/③ 的分界。
// 另配 `F1.0js0`（fb=1 且 jitter_scale=0）：按构造 ≈ M0（无抖动 + 无累积 ⟹ 只剩 taa pass
// 本身），是这条拆分的**恒等对照**；它离 M0 越远说明「TAA pass 在图里」这件事本身越不免费
// （AO 施加点从 tonemap 挪到 taa pass = 批 13.6），读数如实打印并进 rc。
//
// ## 判决口径（写死，三条都要能接受——本命令不是用来证明「有 bug 可修」的）
//
//   d(fb) = 运动态边缘带线性域偏差，R(fb) = d(fb)/d(M0)，spread = max d / min d：
//   ① spread < 1.02 ⟹ **与 fb 无关** ⟹ 假设否证。
//   ② d 随 fb↑ 回落**且 min R < 1** ⟹ **假设成立**，修法 = 运动态上调 fb；
//      带内最优 = argmin d，报它相对 M0 的改善倍数。
//   ③ d 随 fb↑ 回落但 **R(fb=1) ≥ 1**（拉到完全不累积仍差于地板）⟹ 病灶**不在累积**，
//      在 **jitter 本身**（运动下抖动没被正确抵消）——这是**另一个根因**。
//
// ## 顺带：静止态窗棂残余是不是同一个根
//
// 静止态长窗（α=0.01）后 frac(swing>τ) 仍有 1.61%。若判成 ③，两者很可能是同一个根（窗棂 =
// 亚像素细结构，对抖动相位最敏感）。对照 = 静止态也扫 fb（0.02/0.12/0.5/1），看 swing 随 fb 的
// **方向与幅度**是否与运动态同比例：同比例 ⟹ 支持统一解释；反向或幅度差一个量级 ⟹ 否证。
// 0.5 也在静止档里：交付建议若是「运动态把 fb 抬到 0.5」，必须能回答「静止态会不会被一起抬」
// ——fb 是同一个字段，不 gate 住就一起被抬，而静止态的窗棂恰恰靠**长窗**（feedback=0.01）压。
// swing 口径 = `phaseSwingStat`（同 `temporal-residual --bistro` 的 N1/N2；τ 由静止组自己
// 定、各组共用，与运动表的 τ 是两个数，别混读）。
//
// ## 红线（与 temporal-motion 同源，逐条落点）
//  1. 分辨率钉死 1280×720，每组回显实际帧尺寸（`collectGroup` 自证，不等即抛）。
//  2. 连拍一律 `/shotburst` 逐帧连续。
//  3. 统计在 sRGB 解码后的**线性域**（复用 `prepareLinear`，本文件不另写解码）。
//  4. 交付配置（`deliveryConfigDir` = 仓库 configs/，env/IBL/AO/shadow 全开）；
//     配置走 `makeProfile` 生成的独立目录，**不碰 build 端 configs 副本**。
//  5. 单实例：`Engine` 构造内 `pgrep -x rgl` 有残留直接拒跑。
//  6. **参数生效自证**：每组回显**引擎自己报的** feedback（`GET /state` 读回，不是写进 lua
//     的值），并在其中一组做**同进程 `/param` A/B**（两张图之间只隔一个 float）证明这个
//     滑杆真的改画面。「写进 lua 没生效」是参数扫描最容易犯的错，而它的症状（曲线是平的）
//     恰好会被读成「该参数无效」这个结论。
//
// ## 热身：两条约束不可同时满足，取哪一条写死在这里
//
// ① 跨进程对齐要求「候选与参考的**绝对帧号**差 ≤ window」，而 yaw = 每帧角 × 绝对帧号
//    ⟹ 同一参考下各组必须同 warm，且 **warm 一变采到的视点就变**（0.2°/帧 × 304 帧 = 61°
//    ⟹ 早已不是门框）。② EMA 瞬态按 (1−α)^t 衰减，小 fb 想要稳态要 warm ≈ 6/α。
// 取 ①：**运动组一律 warm=16**（= 已发表 M0/M1 的那个视点、那台仪器），② 逐行打印成
// `(1−fb)^warm` 作为**已知偏保守**的偏差（fb=0.02 是 0.72 ⟹ 长窗的糊被低估不会被高估）。
// 静止组不受 ① 约束（机位不动 ⟹ 任何两帧同视点），照 ② 用 align16(6/α)。
// 两次翻车的实测记录（每组各自 warm ⟹ 假配对、偏差虚高 10×；整套 warm=304 ⟹ 曲线跑在门框
// 外 61° 的另一个视点上，且 R(fb=1) 与 R(fb=.12) 的大小关系**随视点翻号**）见采集处 ⚠⚠。
//
// ## 跨进程重复性的地板（两轮实测，读结论前先量这个）
//
// 静止行**逐位相同**（0.001419 / 0.004850 / 0.022862 两轮一字不差）；运动行差 0.1–0.5%。
// 根因不在渲染非确定性，在**采集**：burst 的绝对起始帧号跨进程差 1 帧（RPC 机位写入重定基），
// 而 yaw = 每帧角 × 绝对帧号 ⟹ 整条轨迹差 0.2° = 略不同的场景内容。argmin 配对本身是对的
// （候选与参考在同一轮内共享那个偏移），残余是「视点真的不一样」。**故本表只认显著大于
// 0.5% 的结论**；fb=.12 → fb=.5 的落差两轮实测 1.147× / 1.154×，比地板大两个数量级。

int cmdTemporalFeedback(const std::string& cameraName, double angleDeg, int burst,
    double edgePercentile, const std::vector<double>& fbs, double kernel)
{
    const BistroCam* cam = nullptr;
    for (const auto& c : kBistroCams)
        if (cameraName == c.name)
            cam = &c;
    if (!cam) {
        std::fprintf(stderr, "机位不存在: %s（可选 %s）\n", cameraName.c_str(),
            bistroCamNames().c_str());
        return 2;
    }
    if (burst < 32 || burst % static_cast<int>(kJitterPeriod) != 0)
        throw std::runtime_error(
            "--burst 必须是 ≥32 且为抖动周期 16 的整数倍（静止组的 swing 相位折叠要整数个周期）");
    if (fbs.size() < 3)
        throw std::runtime_error("--fb 至少 3 档，否则「随 fb 单调」这个命题根本没法判");
    if (edgePercentile <= 0.0 || edgePercentile >= 1.0)
        throw std::runtime_error("--edge-pct 必须在 (0,1)，它是分位不是百分比");

    const double angle = angleDeg * 3.14159265358979323846 / 180.0;
    const int periods = burst / static_cast<int>(kJitterPeriod);
    std::printf("=== temporal-feedback：运动态 feedback 扫描（机位 %s）===\n", cameraName.c_str());
    std::printf("  假设 = 「静止 CR 核退化为恒等 ⟹ 零低通；相机一动每帧在非零分数偏移上重采样"
                "⟹ 每帧一次低通、在 ≈1/α 窗内复利」\n");
    std::printf("  推论 = fb↑（窗变短）应让边缘偏差**回落**；fb 是**本帧权重**，调大 = 更信本帧\n");
    std::printf("  运动 = 相机帧驱动 yaw %.4f°/帧 × 帧号（纯函数，= temporal-motion 的 M 组）\n",
        angleDeg);
    std::printf("  参考 = 同机位同帧序列 %dx%d + TAA 关 → 线性域面积平均降到 %dx%d\n",
        kRefSize[0], kRefSize[1], kSize[0], kSize[1]);
    std::printf("  连拍 = %d 帧（/shotburst 逐帧连续，= %d 个抖动周期）；运动组 warm 一律 16"
                "（= 已发表 M0/M1 的视点），静止组 warm = align16(6/α)\n",
        burst, periods);
    std::printf("  配置 = 交付 configs/（deferred + env/IBL/AO/shadow 全开）；单实例；"
                "统计一律线性域\n");
    std::printf("  输入侧重建核 taa.input_kernel = %g（0 双线性 / 1 CR 4×4 / 2 Lanczos3；"
                "只作用于 TAA 开组，M0/REFm/REFs/S0 = TAA 关 = 不消费本键）\n\n", kernel);
    // ── 采集（每组一个进程；串行，单实例纪律）────────────────────────────────
    //

    FeedbackCollection fc;
    collectFeedbackGroups(fc, cameraName, angle, burst, kernel, fbs);
    Group& gRefm = *fc.refm;
    Group& gM0 = *fc.m0;
    Group& gZero = *fc.zero;
    Group& gRefs = *fc.refs;
    Group& gS0 = *fc.s0;
    auto& motion = fc.motion;
    auto& still = fc.still;
    const int warmMotion = 16;  // 只用于出表列；采集处同名，理由见 collectFeedbackGroups
    std::vector<Verdict> vs;
    // ── 守卫 1：运动激励在场（复用 `temporal-motion-excitation` 的口径与注册项）──────

    if (!guardMotionExcitation(vs, gM0, gRefm, cameraName))
        return 2;
    // ── 守卫 2：feedback **真的传进去了**（红线 6）───────────────────────────────
    // 三条同时成立才算过：① 每组**引擎自己回显**的值 == 请求值；② 回显值互不相同（①的比较
    // 写坏了会恒真，②堵这条自欺路径）；③ **同进程** `/param` A/B 的字节差 > 0（静止机位 +
    // 隔整一个抖动周期 ⟹ 同相位同视点，两张图之间只隔一个 float，见采集处）。

    if (fc.probeHost == nullptr)
        throw std::runtime_error("没有 /param 探针宿主（静止组里没有 fb=0.12？）");
    if (!guardFeedbackParamLive(vs, motion, *fc.probeHost, cameraName))
        return 2;

    // ── 区域与阈值（口径与 temporal-motion 一致：来自参考，不来自候选）──────────────
    // 运动套的掩码来自 REFm、静止套来自 REFs —— **掩码必须住在候选自己的视点上**。已发表的
    // M4 用运动参考的掩码量静止候选（两者差 ~5°）⟹ 掩码与边缘错开、读数被稀释，故静止表
    // 两列并列出数（本命令口径 + 借掩码口径），不判谁对。

    const FeedbackBand bM = feedbackBandOf(gRefm, edgePercentile);
    const FeedbackBand bS = feedbackBandOf(gRefs, edgePercentile);
    std::printf("\n  边缘带（梯度 top%.0f%%，各套自算）：运动 %s px · 静止 %s px\n"
                "  τ（白电平 p99 处 1 显示 LSB，闭式）：运动 %s · 静止 %s\n",
        (1.0 - edgePercentile) * 100.0, std::to_string(cv::countNonZero(bM.edge)).c_str(),
        std::to_string(cv::countNonZero(bS.edge)).c_str(), d4(bM.tau).c_str(),
        d4(bS.tau).c_str());

    // ── 对齐 + 读数 ───────────────────────────────────────────────────────────
    const int window = 8;
    std::vector<FeedbackRow> rows;
    addFeedbackRows(rows, gM0, gRefm, motion, gZero, bM, window);

    const double devM0 = rows.front().dev.devEdge;

    // ── 守卫 3：对齐护栏（复用 temporal-motion-alignment 的阈值）─────────────────

    guardFeedbackAlignment(vs, rows);
    printFeedbackMotionTable(rows, bM, devM0, warmMotion);

    // ── 守卫 4：零态锚点（fb=1 + js=0 按构造 ≈ M0）+ 回锚（≈ 已发表 M1）───────────

    guardFeedbackZeroState(vs, rows, devM0);
    guardFeedbackTieback(vs, rows, devM0);

    // ── 静止态对照（窗棂残余 = 相位折叠 swing，口径同 temporal-residual --bistro）──

    const double tauStill = feedbackStillTau(still, periods);
    std::vector<FeedbackStillRow> srows;
    collectFeedbackStillRows(srows, still, gS0, gRefs, bS, bM, tauStill, window, periods);
    printFeedbackStillTable(srows, tauStill);

    // ── 批 13.8 第 0 步读数：**静止态**的「抖动自身代价」= dev(S fb=1) / dev(S0) ──
    // 与运动态 R(fb=1) 同构（那边是 dev(F1.0)/dev(M0)）。两边同 ~4% ⟹ 那 4% 是「无收益
    // 重采样的代价」、与运动无关；静止 ~0% 而运动 4% ⟹ 才是「运动专属」。

    printFeedbackStillStepZero(srows);

    // ── 判决 ──────────────────────────────────────────────────────────────────
    // 只用**对齐可信**的曲线组；不可信的逐条点名（它们的大读数是对配对误差的测量）。

    const FeedbackCurve cu = buildFeedbackCurve(rows, devM0);
    printFeedbackVerdict(cu, devM0);
    printFeedbackCrossCheck(srows, cu.curve);

    std::printf("  ★ 口径边界：AO 的 1440p/720p 差异是各组共用的偏差地板，只出比值结论；"
                "运动套与静止套的掩码和 τ 互不通用；换 warm = 换视点，跨 warm 的数不可并读\n");

    logCall("temporal-feedback",
        {{"camera", cameraName}, {"angleDeg", std::to_string(angleDeg)},
            {"burst", std::to_string(burst)}, {"edgePct", std::to_string(edgePercentile)},
            {"warmMotion", std::to_string(warmMotion)}, {"devM0", d4(devM0)},
            {"bestFb", d2(cu.bestFb)}, {"Rbest", d4(cu.rBest)}, {"Rfb1", d4(cu.r100)},
            {"Rfb12", d4(cu.r12)}, {"spread", d4(cu.spread)}, {"inversions", std::to_string(cu.inversions)},
            {"dropped", cu.dropped}, {"tauM", d4(bM.tau)}, {"tauStill", d4(tauStill)}},
        vs);
    bool allOk = true;
    for (const auto& v : vs)
        allOk &= v.ok;
    return allOk ? 0 : 1;
}



namespace tmotion {
/// 热身帧数 = align16(max(16, ⌈6/α⌉))。α = feedback（EMA 记忆长度 ≈ 1/α）。
int warmForFeedback(double fb)
{
    const int raw = static_cast<int>(std::ceil(6.0 / std::max(fb, 1e-6)));
    const int per = static_cast<int>(kJitterPeriod);
    return ((std::max(16, raw) + per - 1) / per) * per;
}

/// fb 的文件名后缀（两位百分比，零填充）。
std::string fbTagOf(double fb)
{
    char b[16];
    std::snprintf(b, sizeof b, "%04d", static_cast<int>(std::lround(fb * 100.0)));
    return std::string(b);
}

/// 由参考首帧算边缘带 / 平坦带 / τ（口径与 temporal-motion 一致：来自参考，不来自候选）。
FeedbackBand feedbackBandOf(const Group& ref, double edgePercentile)
{
    FeedbackBand b;
    cv::Mat gx, gy, grad;
    cv::Sobel(ref.luma.front(), gx, CV_64F, 1, 0, 1);
    cv::Sobel(ref.luma.front(), gy, CV_64F, 0, 1, 1);
    grad = cv::abs(gx) + cv::abs(gy);  // 与 gradEnergy 同式
    auto gv = valuesInMask(grad, {});
    std::sort(gv.begin(), gv.end());
    cv::compare(grad, percentileOf(gv, edgePercentile), b.edge, cv::CMP_GE);
    cv::compare(grad, percentileOf(gv, 0.50), b.flat, cv::CMP_LT);
    auto rv = valuesInMask(ref.luma.front(), b.edge);
    std::sort(rv.begin(), rv.end());
    b.white = percentileOf(rv, 0.99);
    b.tau = lsbLinearAtLuma(b.white);  // 闭式：白电平处 1 个显示 LSB
    return b;
}

/// 采集全部组（运动 + 静止 + 两个超采样参考 + 零态锚点 + S0 地板）。**串行、单实例纪律**。
void collectFeedbackGroups(FeedbackCollection& c, const std::string& cameraName, double angle,
    int burst, double kernel, const std::vector<double>& fbs)
{
    const BistroCam* cam = nullptr;
    for (const auto& cc : kBistroCams)
        if (cameraName == cc.name)
            cam = &cc;
    assert(cam != nullptr && "调用方已校验机位存在");
    const std::string base = projectRoot() + "/build/imgqa_fbsweep_" + cameraName;
    const std::string pre = "fbs_";
    // ⚠⚠ **两条互相冲突的约束，取哪一条必须写下来**（本命令在这里翻过两次车，护栏都抓住了）：
    //  ① **跨进程对齐要求「候选与参考的绝对帧号差 ≤ window」**。yaw = 每帧角 × 绝对帧号，
    //     所以同一参考下的各组**必须同 warm**；而且 warm 一变，采到的**视点**就跟着变
    //     （0.2°/帧 × 304 帧 = 60.8° ⟹ 早已不是门框，实测截图 = 街景）。
    //  ② **EMA 瞬态按 (1−α)^t 衰减**，小 fb 想要稳态需要 warm ≈ 6/α（fb=0.02 ⟹ 304 帧）。
    // 两者不可同时满足。试过 ②优先（每组按 6/α 配 warm）：fb≤0.25 的四组与 warm=16 的参考
    // 差 48–288 帧 ⟹ argmin 配出**假对**，边缘偏差虚高 10×，被 `temporal-motion-alignment`
    // 判红（余量 0.0003、前后半窗差 1）。又试过整套 warm=304：对齐全绿、每组都在稳态，但
    // 整条曲线跑在**门框外 61° 的另一个视点**上，与已发表的 M0/M1 不可并读（且 R(fb=1) 与
    // R(fb=.12) 的大小关系**随视点翻号**——跨视点比较无效的直接证据）。
    // ⟹ 取 ①：**运动组一律 warm=16**（= 已发表 M0/M1 的那个视点、那台仪器），把 ② 作为
    // **已知偏保守**的偏差逐行打印 `(1−α)^warm`：fb=0.02 是 0.72 ⟹ 读数里 72% 权重还在第
    // 0 帧上，长窗的糊**被低估、不会被高估**，判决方向不受它影响。
    // 静止组不受 ① 约束（机位不动 ⟹ 任何两帧同视点，配对平凡正确），照 ② 用 6/α。
    const int warmMotion = 16;
    std::printf("── 采集 ──\n  运动组 warm 一律 %d（= 已发表 M0/M1 的视点；同一参考下必须同 warm，"
                "且 warm 会变视点，理由见 ⚠⚠ 注释）\n"
                "  静止组 warm = align16(6/α)（机位不动 ⟹ 配对平凡正确，只需稳态）\n",
        warmMotion);

    auto run = [&](const std::string& name, const std::string& sub, const Spec& sp) -> Group& {
        c.slots.push_back(std::make_unique<Group>(
            collectGroup(base + "_" + sub, name, pre + sub, *cam, sp, burst)));
        return *c.slots.back();
    };

    Spec m0;
    m0.aa = Aa::Off;
    m0.motionAngle = angle;
    m0.warm = warmMotion;
    Spec refm = m0;
    refm.refScale = 1;
    c.refm = &run("REFm 超采样参考(运动)", "refm", refm);
    c.m0 = &run("M0 TAA 关 + 运动（地板）", "m0", m0);
    std::vector<std::pair<double, Group*>> motion;
    for (const double fb : fbs) {
        Spec s;
        s.aa = Aa::Taa;
        s.feedback = fb;
        s.kernel = kernel;
        s.motionAngle = angle;
        s.warm = warmMotion;
        c.motion.push_back({fb, &run("F" + d2(fb) + " 运动 fb=" + d2(fb), "f" + fbTagOf(fb), s)});
    }
    Spec zs;
    zs.aa = Aa::Taa;
    zs.feedback = 1.0;
    zs.jitterScale = 0.0;  // 零抖动 + 零累积 ⟹ 按构造 ≈ M0
    zs.kernel = kernel;
    zs.motionAngle = angle;
    zs.warm = warmMotion;
    c.zero = &run("F1.0js0 零态锚点（应 ≈ M0）", "zero", zs);

    Spec refs = m0;
    refs.aa = Aa::Off;
    refs.motionAngle = 0.0;
    refs.refScale = 1;
    refs.warm = 64;
    c.refs = &run("REFs 超采样参考(静止)", "refs", refs);
    std::vector<std::pair<double, Group*>> still;
    // probeHost = 同进程 /param A/B 探针的宿主（必须是静止 + js>0 组）
    // 静止档里带上 0.5 = 运动态的带内最优：交付建议若是「运动态把 fb 抬到 0.5」，必须能
    // 回答「那静止态会不会被搞坏」——静止态的窗棂残余靠**长窗**压（feedback=0.01），
    // 而 fb 是同一个字段，不 gate 住就会一起被抬。
    for (const double fb : {0.02, 0.12, 0.5, 1.0}) {
        Spec s;
        s.aa = Aa::Taa;
        s.feedback = fb;
        s.kernel = kernel;
        s.motionAngle = 0.0;
        s.warm = warmForFeedback(fb);
        if (std::abs(fb - 0.12) < 1e-9)
            s.probeFeedback = 1.0;  // 翻到「零累积」：同相位两帧之差只可能来自 fb
        Group& g = run("S" + d2(fb) + " 静止 fb=" + d2(fb), "s" + fbTagOf(fb), s);
        if (s.probeFeedback > 0.0)
            c.probeHost = &g;
        c.still.push_back({fb, &g});
    }
    // ── S0 = **静止 + TAA 关**（批 13.8 第 0 步的地板，缺它「运动专属」这句话没东西可对）──
    // 为什么要这一格：运动态已发表 R(fb=1) = 1.0395 被读成「抖动自身在**运动下**贡献 +3.95%
    // = 位置错」。但 fb=1 = 完全不累积 ⟹ 抖动买不到任何东西、而 un-jitter 那次重采样照付
    // ⟹ fb=1 比 TAA 关差是**应该的**。要判断那 4% 是不是「运动专属」，唯一的对照就是
    // **静止态**同一格：静止 + fb=1 vs 静止 + TAA 关。两边同参考（REFs）、同边缘带（bS）、
    // 同 τ、同 warm 口径（64 = 静止组口径）⟹ 比值里没有跨视点项。
    Spec s0;
    s0.aa = Aa::Off;
    s0.motionAngle = 0.0;
    s0.warm = 64;
    c.s0 = &run("S0 静止 TAA 关（静止地板）", "s0", s0);
}

/// 守卫 1：运动激励在场（复用 `temporal-motion-excitation` 的口径与注册项）。
bool guardMotionExcitation(std::vector<Verdict>& vs, const Group& gM0, const Group& gRefm,
    const std::string& cameraName)
{
    std::string obs;
    for (const auto* g : {&gM0, &gRefm})
        obs += g->name.substr(0, 2) + " 相邻帧最大字节差 "
            + std::to_string(static_cast<int>(g->byteMaxAdjacent)) + "  ";
    vs.push_back(verdictFor("temporal-motion-excitation",
        gM0.byteMaxAdjacent > 0.0 && gRefm.byteMaxAdjacent > 0.0,
        obs + "（M0/REFm = TAA 关组，必须 > 0；= 0 ⟺ frame_yaw 没生效）"));
    std::cout << "\n── 守卫 ──\n  " << vs.back().line() << "\n";
    if (!vs.back().ok) {
        std::printf("  ⟹ 立刻停下：运动激励不在场，整张扫描表会跑在静止场景上。\n");
        logCall("temporal-feedback", {{"camera", cameraName}}, vs);
        return false;
    }
    return true;
}

/// 守卫 2：feedback **真的传进去了**（红线 6：回显相符 + 互异 + 同进程 /param A/B 字节差 > 0）。
bool guardFeedbackParamLive(std::vector<Verdict>& vs,
    const std::vector<std::pair<double, Group*>>& motion, const Group& probeGroup,
    const std::string& cameraName)
{
    std::string bad, obs;
    std::set<double> distinct;
    for (const auto& [fb, g] : motion) {
        distinct.insert(g->liveFeedback);
        if (std::abs(g->liveFeedback - fb) > 1e-5)
            bad += "F" + d2(fb) + " 回显 " + d4(g->liveFeedback) + " ";
    }
    const bool readbackOk = bad.empty();
    const bool distinctOk = distinct.size() == motion.size();
    obs = "引擎回显 vs 请求：" + (readbackOk ? "全部相符" : ("**不符** " + bad))
        + " ｜ 互异值 " + std::to_string(distinct.size()) + "/"
        + std::to_string(motion.size())
        + " ｜ 同进程 /param A/B（" + probeGroup.name + "，隔整 1 周期 = 同相位）字节差 "
        + std::to_string(static_cast<int>(probeGroup.probeBytes)) + "、变化 "
        + d2(probeGroup.probeChangedPx * 100.0) + "%";
    vs.push_back(verdictFor("temporal-feedback-param-live",
        readbackOk && distinctOk && probeGroup.probeBytes > 0.0, obs));
    std::cout << "  " << vs.back().line() << "\n";
    if (!vs.back().ok) {
        std::printf("  ⟹ 立刻停下：feedback 没被证明生效，曲线无论长什么样都不能解释。\n");
        logCall("temporal-feedback", {{"camera", cameraName}}, vs);
        return false;
    }
    return true;
}

/// 对齐 + 读数：地板 M0、每个 fb 档、零态锚点各一行。
void addFeedbackRows(std::vector<FeedbackRow>& rows, const Group& gM0, const Group& gRefm,
    const std::vector<std::pair<double, Group*>>& motion, const Group& gZero,
    const FeedbackBand& bM, int window)
{
    auto addRow = [&](const std::string& tag, double fb, const Group& g, const Group& ref,
                          const FeedbackBand& b, bool inCurve) {
        FeedbackRow r;
        r.tag = tag;
        r.fb = fb;
        r.curve = inCurve;
        const Alignment a = alignSequence(g, ref, window, true);
        r.margin = a.minMargin;
        r.gap = a.halfGap;
        r.trust = a.minMargin >= 0.02 && a.halfGap == 0;
        r.dev = measureDeviation(g.luma, ref, a, b.edge, b.flat, b.tau);
        rows.push_back(std::move(r));
    };
    addRow("M0", 0.0, gM0, gRefm, bM, false);
    for (const auto& [fb, g] : motion)
        addRow("F" + d2(fb), fb, *g, gRefm, bM, true);
    addRow("F1.0js0", 1.0, gZero, gRefm, bM, false);
}

/// 守卫 3：对齐护栏（复用 temporal-motion-alignment 的阈值）。
void guardFeedbackAlignment(std::vector<Verdict>& vs, const std::vector<FeedbackRow>& rows)
{
    double worst = std::numeric_limits<double>::max();
    int gap = 0;
    int untrusted = 0;
    for (const auto& r : rows) {
        worst = std::min(worst, r.margin);
        gap = std::max(gap, r.gap);
        untrusted += r.trust ? 0 : 1;
    }
    vs.push_back(verdictFor("temporal-motion-alignment", worst >= 0.02 && gap == 0,
        "最小 argmin 余量 " + d4(worst) + "（下限 0.02）· 最大前后半窗差 " + std::to_string(gap)
            + "（要求 0）· 组数 " + std::to_string(rows.size()) + "，其中对齐失效 "
            + std::to_string(untrusted) + " 组（失效组的读数不进判决）"));
    std::cout << "  " << vs.back().line() << "\n";
}

void printFeedbackMotionTable(const std::vector<FeedbackRow>& rows, const FeedbackBand& bM,
    double devM0, int warmMotion)
{
    std::printf("\n── 运动态 A/B 表（线性域；边缘带 = 梯度 top10%%；warm 一律 %d）──\n",
        warmMotion);
    std::printf("  %-10s %6s %10s %8s %9s %10s %8s %8s %8s  %s\n", "组", "fb", "边缘偏差",
        "(÷LSB)", "可见边缘%", "平坦偏差", "梯度比", "dev/M0", "瞬态", "argmin 余量");
    for (const auto& r : rows)
        std::printf("  %-10s %6s %10s %8s %8.2f%% %10s %8.3f %8s %8s  %.4f%s\n", r.tag.c_str(),
            r.fb > 0.0 ? d2(r.fb).c_str() : "off", d4(r.dev.devEdge).c_str(),
            d2(r.dev.devEdge / bM.tau).c_str(), r.dev.fracVisible * 100.0,
            d4(r.dev.devFlat).c_str(), r.dev.gradRatio,
            d4(r.dev.devEdge / std::max(devM0, 1e-12)).c_str(),
            r.fb > 0.0 ? d4(std::pow(1.0 - r.fb, static_cast<double>(warmMotion))).c_str()
                       : "n/a",
            r.margin, r.trust ? "" : "  ⟵ **对齐失效，读数不可解释**");
    std::printf("  「瞬态」= (1−fb)^warm = 读数里还压着第 0 帧的权重比例（fb=0.02 的 0.72 ⟹ "
                "长窗的糊**被低估**，方向保守）\n");
}

/// 守卫 4：零态锚点（fb=1 + js=0 按构造 ≈ M0）+ 回锚（≈ 已发表 M1）。
void guardFeedbackZeroState(std::vector<Verdict>& vs, const std::vector<FeedbackRow>& rows,
    double devM0)
{
    const FeedbackRow& zr = rows.back();
    const double ratio = zr.dev.devEdge / std::max(devM0, 1e-12);
    vs.push_back(verdictFor("temporal-feedback-zero-state", ratio <= 1.05,
        "dev(F1.0js0)/dev(M0) = " + d4(ratio)
            + "（上限 1.05）· 无抖动 + 无累积 ⟹ 只剩 taa pass 本身（AO 施加点挪位也记在"
              "这里）；显著 >1 = 跨进程可比性坏了，本表比值不可信"));
    std::cout << "  " << vs.back().line() << "\n";
}

/// 回锚：fb=0.12 那一档就是已发表的 M1（同 warm、同视点、同配置），比值必须 ≈ 1.138。
void guardFeedbackTieback(std::vector<Verdict>& vs, const std::vector<FeedbackRow>& rows,
    double devM0)
{
    double r12 = -1.0;
    const auto it = std::find_if(rows.begin(), rows.end(),
        [](const FeedbackRow& r) { return r.curve && std::abs(r.fb - 0.12) < 1e-9; });
    if (it != rows.end() && it->trust) {
        r12 = it->dev.devEdge / std::max(devM0, 1e-12);
        vs.push_back(verdictFor("temporal-feedback-tieback", std::abs(r12 - 1.138) <= 0.05,
            "dev(fb=.12)/dev(M0) = " + d4(r12)
                + "（已发表 M1/M0 = 1.138，容差 ±0.05）· 本扫描与那张 A/B 表是不是同一台"
                  "仪器的**唯一**证据；漂了则两边的比值不能并读"));
        std::cout << "  " << vs.back().line() << "\n";
    }
    else {
        vs.push_back(verdictFor("temporal-feedback-tieback", false,
            "扫描档里没有可信的 fb=0.12 ⟹ 无法证明与已发表 M1 是同一台仪器"));
        std::cout << "  " << vs.back().line() << "\n";
    }
}

}  // namespace RGL::imgqa::tmotion

}  // namespace RGL::imgqa
