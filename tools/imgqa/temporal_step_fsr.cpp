// temporal-tail --motion step 的 **FSR 臂集**（批次 B11 从 `temporal_motion.cpp` 拆出）：
// 臂表 / 一臂三跑 / AUC 排序。判据线与 TAA 臂共用 `temporal_step.cpp` 的那一套。
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

namespace tmotion {

/// 阶跃臂的热身 = **`⌈6/α⌉`（EMA 判据）与 CLI `--warm` 取大**，向上对齐到抖动周期。
/// 🔴 为什么是**取大**而不是「CLI 给了就覆写」：`--warm` 默认值 64 是本子命令三个
/// `--motion` 模式共用的，若「> 0 即覆写」，α=0.01 的臂就**永远**只热身 64 帧
/// （EMA 判据要 600）——首轮实测正是如此，且它伪装得极好：抖动关断下静止机位的收敛
/// 帧本来就是不动点，参考图完全正确，只有「历史攒了多久」这一件事被悄悄改了。
int stepWarmFor(double alpha, int warmFloor)
{
    const int w = std::max(warmFloor, static_cast<int>(std::ceil(6.0 / alpha)));
    return static_cast<int>(
        (static_cast<uint32_t>(w) + kJitterPeriod - 1) / kJitterPeriod * kJitterPeriod);
}

/// 臂 → 采集口径。`jitterScale = 0` 恒成立（理由见段头【为什么 jitter 恒 0】）。
///
/// ✅ **FSR 臂不破坏这条纪律**：`taa.jitter_scale` 与 `fsr` 块 `jitter` 是两个独立旋钮，而
/// FSR 时域臂的 `aa` 恒非 Taa（一个槽位装不下两个时域滤波器）⟹ 前者根本不被消费，FSR 的
/// 亚像素抖动走 `fsrJitter` 自足。于是仪器的三条 jitter 红利（B∞ 是固定点 / 零锚恰 0 /
/// 分母无相位噪声）一条都不必牺牲，FSR 也照样跑在它的设计点上。
Spec stepSpecFor(const StepArm& a, int warm)
{
    Spec sp;
    sp.aa = a.aa;
    sp.feedback = a.feedback;
    sp.jitterScale = 0.0;
    sp.baseSpace = true;  // 交付态
    sp.clipMode = a.clipMode;
    sp.reprojection = a.reprojection;
    sp.renderScale = a.renderScale;
    sp.fsrTemporal = a.fsrTemporal;
    sp.fsrJitter = a.fsrJitter;
    sp.warm = warm;
    return sp;
}

StepArmResult collectStepArm(const std::string& dir, const BistroCam& from, const BistroCam& to,
    const StepArm& a, int burst, int overrideWarm, double tau, int margin)
{
    // TAA 臂的热身判据 = `⌈6/α⌉`（EMA）。非 TAA 臂（FSR）**没有 α 可依据**：vendor 的历史
    // 长度是它内部的事，这里给一个保守地板，够不够**不靠猜** —— 没收敛则末 16 帧仍在漂，
    // 由 `spread` 当场抓（见 `convergedSpread` 的理由）。
    const int warm = a.aa == Aa::Taa ? stepWarmFor(a.feedback, overrideWarm)
                                     : std::max(overrideWarm, 120);
    const Spec sp = stepSpecFor(a, warm);
    const std::string tag = a.tag;
    const StepRun ra = captureStepRun(dir + "_a", tag + "/A", tag + "a", from, from, sp, burst);
    const StepRun rb = captureStepRun(dir + "_b", tag + "/B", tag + "b", to, to, sp, burst);
    const StepRun rs = captureStepRun(dir + "_s", tag + "/step", tag + "s", from, to, sp, burst);
    const cv::Mat aRef = convergedRef(ra), bRef = convergedRef(rb);

    StepArmResult out;
    out.main = stepCurveOf(a, aRef, bRef, rs, tau, margin);
    out.aRef = aRef;
    out.bRef = bRef;
    out.spread = std::max(convergedSpread(ra), convergedSpread(rb));
    // 地板 = B∞ 那一跑**自己内部**的不稳定，按分子同一个掩膜/分母折成 ghost 单位。
    // 取 B 跑而不是 A 跑：ghost(n) 的参考就是 B∞，污染也从它来。
    double den = 0.0;
    const cv::Mat mask = stepMask(aRef, bRef, tau, margin, &den, nullptr);
    out.floor = convergedFloor(rb, bRef, mask, den);
    // τ 敏感性：主判决只读主 τ，其余两档只打印（τ 是唯一自由阈值 ⟹ 结论不能依赖它）。
    for (double t : kStepTauSweep) {
        const StepCurve s = std::abs(t - tau) < 1e-12
            ? out.main
            : stepCurveOf(a, aRef, bRef, rs, t, margin);
        out.sens.push_back(s.vis.size() > 16 ? s.vis[16] : 0.0);
    }
    return out;
}

/**
 * `--fsr` 追加的升频臂组（批 14.5）。
 *
 * # 为什么 FSR 必须挤进这台仪器
 * 鬼影的定义是「让开的内容回不来」。唯一能去遮挡的激励 = 本模式的**平移阶跃**（债-63：
 * `frame_yaw` 是原地纯旋转 ⟹ 无视差 ⟹ 结构上不可能有鬼影）。换激励 = 换口径，所以
 * 「FSR 的鬼影比 TAA 轻不轻」这个问题**只能**在这把尺上答，不能拿 yaw 运动的 L1 代替。
 *
 * # 六臂分工
 *   B75 = 双线性 @0.75，**无历史** ⟹ 按构造 `ghost ≡ 0` = **下界锚**
 *   S75 = FSR 纯空间 @0.75（vendor 每帧 reset，同样无历史）= **第二个下界锚**，且它把
 *         「FSR 管线本身」与「FSR 的历史」分开：S75 干净而 F75 脏 ⟹ 脏在历史，不在管线
 *   T11 = TAA @原生 1.0 + **现行交付盒 clip_mode = 11** = 今天真的在跑的那条
 *   T13 = TAA @原生 1.0 + **臂 13**（债-69 修法，鬼影 AUC 1.073→0.011 = 好 79×，只差转交付）
 *         🔴 **这条才是 FSR 真正的对手**。拿 FSR 去赢 T11 等于赢一个已被判定要换掉的配置——
 *         债-69 明写「本条剩『转交付』未做」，所以 T11 是**临时的**交付态。少了 T13 的话，
 *         「FSR 鬼影比 TAA 轻」会是一句对着旧臂说、下个月就作废的结论。
 *   T75 = TAA 臂 13 @0.75 = 与 FSR **同内部分辨率**的机制对照（分辨率损失变共模 ⟹ 差值归给
 *         时域滤波器本身，而不是归给「谁渲染得粗」）
 *   F75 = FSR 时域 + 抖动 @0.75 = 候选（= 交付档）
 *
 * # 两条量具守卫（红 ⟹ 本组数字一律不许读）
 * ① **下界锚**：B75 与 S75 的 `max ghost(n) ≤ 0.02`。它们没有历史，非零只能说明分子里
 *    混进了与时间无关的东西（参考构造错、跨进程地板、render_scale 下的采样差）。
 * ② **基空间自证**：F75 的收敛态 `spread ≤ 0.02`。理由见 `convergedSpread` —— 末 16 帧
 *    这个窗口是 **TAA 的**抖动周期，不是 FSR 的；FSR 输出若不在基空间，B∞ 就是相位平均糊图。
 *
 * # 为什么这里**不**下「谁更好」的判决
 * 跷跷板有两条轴，本仪器只钉得住**鬼影**这一条（闪烁轴在 `temporal-flicker`）。单轴下结论
 * 正是本批已被否证过的失效模式（L1 会替模糊打分 ⟹ 两轴规则）。所以这里只出数 + 两条守卫，
 * **rc 只由守卫决定**；判决留给两轴合起来看。
 */
int runFsrStepArms(const std::string& dir, const BistroCam& from, const BistroCam& to, int burst,
    int warm, double tau, int margin, bool taaGuardsOk)
{
    if (!taaGuardsOk) {
        std::printf("\n  ⟹ TAA 锚臂守卫已红 ⟹ **跳过** FSR 臂（不在坏量具上读数）\n");
        return 1;
    }
    // 臂表用**指定初始化器**：批 14.6 之后 `aa` 夹在中间，位置初值容易把「哪个槽是抗锯齿」
    // 读错（读错 = 静默换臂），写出来就点不出来。
    const std::vector<StepArm> arms = {
        StepArm{ .tag = "B75", .clipMode = 0.0, .feedback = 0.01, .aa = Aa::Off, .renderScale = 0.75 },
        StepArm{ .tag = "S75", .clipMode = 0.0, .feedback = 0.01, .aa = Aa::Fsr, .renderScale = 0.75 },
        StepArm{ .tag = "T11", .clipMode = 11.0, .feedback = 0.01, .aa = Aa::Taa },
        StepArm{ .tag = "T13", .clipMode = 13.0, .feedback = 0.01, .aa = Aa::Taa },
        StepArm{ .tag = "T75", .clipMode = 13.0, .feedback = 0.01, .aa = Aa::Taa, .renderScale = 0.75 },
        StepArm{ .tag = "F75", .clipMode = 0.0, .feedback = 0.01, .aa = Aa::Fsr, .renderScale = 0.75,
            .fsrTemporal = true, .fsrJitter = true },
    };
    std::printf("\n── 升频 / FSR 臂（同一次阶跃、同一把尺；S75/F75 的 aa = fsr，"
                "结构上不是 TAA ⟹ 无「两个时域滤波器同开」可犯）──\n");

    std::vector<StepCurve> curves;
    std::vector<double> spreads, floors;
    for (const StepArm& a : arms) {
        StepArmResult r = collectStepArm(dir, from, to, a, burst, warm, tau, margin);
        spreads.push_back(r.spread);
        floors.push_back(r.floor);
        curves.push_back(std::move(r.main));
    }
    for (size_t i = 0; i < curves.size(); ++i) {
        printStepCurve(curves[i]);
        std::printf("       地板 %s ghost 单位（收敛态不稳定折进分子，闸门读它）"
                    " ｜ max 偏离 %s（仅诊断：全图 max 会把少数硬边缘翻转放大 ~700×）\n",
            d4(floors[i]).c_str(), d4(spreads[i]).c_str());
    }

    const auto maxGhost = [](const StepCurve& c) {
        double m = 0.0;
        for (double v : c.g)
            m = std::max(m, v);
        return m;
    };
    std::vector<Verdict> vs;
    // ① 下界锚：两条无历史臂必须 ≈ 0。
    const double floorMax = std::max(maxGhost(curves[0]), maxGhost(curves[1]));
    vs.push_back(verdictFor("temporal-tail-step-fsr-floor", floorMax <= 0.02,
        "max ghost(n) 无历史臂 (B75/S75) = " + d4(floorMax)
            + "（线 ≤ 0.02；无历史 ⟹ 阶跃后每帧就是 B∞ ⟹ 应恰为 0。非零 = 分子混进了与时间"
              "无关的东西，本组全部数字作废）"));
    std::cout << "  " << vs.back().line() << "\n";
    // ② 地板污染：F75 的 ghost 读数必须**显著高于它自己的不稳定地板**。
    //    线写成「本臂自己的量」而不是绝对数 ⟹ 同时封住两种失效：有鬼影但被地板冒充、
    //    以及报「我 ≈ 0」而地板比自己还大（后者按仪器最小可分辨量级 0.001 卡）。
    const double g0 = curves[5].g.empty() ? 0.0 : curves[5].g[0];
    const double floorLine = 0.2 * std::max(g0, 0.001);
    vs.push_back(verdictFor("temporal-tail-step-fsr-basespace", floors[5] <= floorLine,
        "F75 地板 = " + d4(floors[5]) + " ghost 单位，线 ≤ " + d4(floorLine)
            + "（= 20% × max(本臂 ghost(0) = " + d4(g0) + ", 仪器最小可分辨量级 0.001)）。"
              "超线 = 该臂 ghost 读数里主要是不稳定地板而不是鬼影"));
    std::cout << "  " << vs.back().line() << "\n";

    const bool ok = std::all_of(vs.begin(), vs.end(), [](const Verdict& v) { return v.ok; });
    std::printf("\n  ⟹ %s\n", ok
            ? "守卫全绿：鬼影轴读数可用。**但跷跷板有两条轴** —— 闪烁轴见 `temporal-flicker`，"
              "单轴不许下「谁更好」"
            : "守卫红 ⟹ **本组数字一律不许读**（先修量具，别改判决线）");
    if (!ok)
        return 1;
    std::printf("  鬼影轴排序（AUC 小 = 尾短）：");
    std::vector<size_t> ord(curves.size());
    for (size_t i = 0; i < ord.size(); ++i)
        ord[i] = i;
    std::stable_sort(ord.begin(), ord.end(),
        [&](size_t a, size_t b) { return curves[a].auc < curves[b].auc; });
    for (size_t i = 0; i < ord.size(); ++i)
        std::printf("%s%s AUC %s", i ? " < " : "", curves[ord[i]].tag.c_str(),
            d2(curves[ord[i]].auc).c_str());
    std::printf("\n");
    return 0;
}

}  // namespace tmotion

}  // namespace RGL::imgqa
