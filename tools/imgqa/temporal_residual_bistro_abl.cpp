// 批 13.11 **邻域盒裁剪消融** A/B：一臂一对（同机位同窗档，只差 `diag_clip_mode`），
// 逐臂打「支持 / 否证 / 分不出」的因果判决；外加 bistro 变体的两个 sweep 入口。
//
//   从 `temporal_residual.cpp` 拆出（文件护栏 500 代码行）。缝 = **消融臂集 vs 单臂采集**：
//   采集原语在 `temporal_residual_bistro.cpp`，一机位六组的编排在
//   `temporal_residual_bistro_camera.cpp`，本 TU 只负责臂集、配对与判决表。
//
//   ⚠ 臂集与两条判决线（`kClipCausalFracDrop` / `kClipRefuteRatio`，跑前写死在
//   `temporal_residual.h`）不许动；`clipArmName` 是臂的**打印名**，进结构签名 ⟹ 逐字搬。

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
#include <sstream>

namespace RGL::imgqa {

namespace {
/// 一臂的打印名（进判决表与台账；`mode` 0 = 基准臂）。
std::string clipArmName(const ClipArm& arm)
{
    switch (static_cast<int>(arm.mode + 0.5)) {
        case 0: return "0 现行";
        case 1: return "1 关盒";
        case 2: return "2 扩1.5×";
        case 3: return "3 box9";
        case 4: {
            std::ostringstream s;
            s << "4 软λ=" << arm.soft;
            return s.str();
        }
        case 5: {
            std::ostringstream s;
            s << "5 扩" << arm.soft << "×";
            return s.str();
        }
        case 6: {
            std::ostringstream s;
            s << "6 信任γ=" << (arm.soft > 0.0 ? std::to_string(arm.soft) : "1/16");
            return s.str();
        }
        case 7: {
            std::ostringstream s;
            s << "7 记忆d=" << (arm.soft > 0.0 ? std::to_string(arm.soft) : "0.5");
            return s.str();
        }
        case 8: {
            std::ostringstream s;
            // 0 = 用**推导值** 1−1/16（记忆覆盖一整个抖动周期），显示成出处而不是某个小数
            s << "8 软记忆d=" << (arm.soft > 0.0 ? std::to_string(arm.soft) : "1-1/16");
            return s.str();
        }
        case 9: {
            std::ostringstream s;
            s << "9 mem接受+ext斜率d=" << (arm.soft > 0.0 ? std::to_string(arm.soft) : "1-1/16");
            return s.str();
        }
        case 10: {
            std::ostringstream s;
            s << "10 臂8+α逐像素d=" << (arm.soft > 0.0 ? std::to_string(arm.soft) : "1-1/16");
            return s.str();
        }
        case 11: {
            std::ostringstream s;
            s << "11 记忆挂重投影位移d=" << (arm.soft > 0.0 ? std::to_string(arm.soft) : "1-1/16");
            return s.str();
        }
        default: return "未知臂" + std::to_string(arm.mode);
    }
}

/// 消融臂集（第四轮 = **修法验证**，2026-10-06）。
/// `--clip-abl --clip <mode>` = **只跑 {0, mode} 两臂**：跨机位 / 跨靶的通用性复核要的是
/// 「同一臂在别的场景上同向」，不是重扫参数；全臂集在 4 个机位上要 100 分钟，会没人跑。
std::vector<ClipArm> clipAblArmSet(double onlyMode, double onlySoft)
{
    // 臂集（第四轮 = **修法验证**，2026-10-06）：0 基准 · 1 关盒（闭式天花板）·
    // 7 标量时域 luma 膨胀 decay ∈ {0.5 推导值, 0.25, 0.75}。⚠ **decay 扫 3× 是"平台期"护栏**：
    // 修法若靠某个调出来的常数，只会在一个值上好看；三档同向 = 机制在起作用，不是常数拟合场景。
    // （前三轮已定的臂不重跑：扩 1.5/3/6/12× 与 box9 证伪、软裁剪 λ=0.1 有效但差 2.2×、
    //   信任门三档 γ 只动 3–5% 否决 —— 全见git:调试历史 2026-10-06。）
    // `--clip-abl --clip <mode>` = **只跑 {0, mode} 两臂**：跨机位 / 跨靶的通用性复核要的是
    // 「同一臂在别的场景上同向」，不是重扫参数；全臂集在 4 个机位上要 100 分钟，会没人跑。
    std::vector<ClipArm> arms = {
        { 0.0, 1.0 }, { 1.0, 1.0 }, { 7.0, 0.0 }, { 7.0, 0.25 }, { 7.0, 0.75 } };
    if (onlyMode > 0.0) {
        // ⚠ mode 6/7 的 `--clip-soft` 是**时间常数覆盖位**，而它的缺省值 1.0 有具体含义且危险：
        //   mode 6 → γ_up=1（一次收下就满信任）；mode 7 → decay=1（**记忆永不衰减** = 盒单调涨到
        //   满量程 = 等价关盒）。1.0 在这里不是用户意图而是"没给参数" ⟹ 映射成 0 = 用推导值。
        double soft = onlySoft;
        if ((onlyMode == 6.0 || onlyMode == 7.0) && soft == 1.0) {
            soft = 0.0;
            std::printf("  （--clip-soft 未给 ⟹ 用推导时间常数，不是缺省值 1.0）\n");
        }
        arms = {{ 0.0, 1.0 }, { onlyMode, soft }};
    }
    return arms;
}

/// 一机位的台账（逐臂三列；采集失败的臂留 `fail` / 空串）。
void logClipAblCamera(const BistroCam& cam, double longFeedback, int periods,
    const std::vector<ClipAblRow>& rows)
{
        std::vector<std::pair<std::string, std::string>> args = {
            { "camera", cam.name }, { "floor", d4(longFeedback) }, { "periods", std::to_string(periods) }
        };
        for (const auto& r : rows) {
            const std::string k = clipArmName(r.arm);
            args.emplace_back(k + "_rho", r.ok ? d6(r.rho) : "fail");
            args.emplace_back(k + "_fracOn", r.ok ? d4(r.fracOn * 100.0) : "");
            args.emplace_back(k + "_swingOn", r.ok ? d6(r.swingOn) : "");
        }
        logCall("temporal-residual-clip-abl", args, {});
}

/// 消融表的表头 + 三条判决线回显（线**跑前写死**在 `temporal_residual.h` 同名常量）。
void printClipAblHeader(const BistroCam& cam, double longFeedback)
{
    std::printf("\n── 历史裁剪消融 A/B（%s｜每臂两组：短窗 fb=.12 / 长窗 fb=%.3g；"
                "τ 由臂 0 短窗定，全臂共用）──\n",
        cam.name, longFeedback);
    std::printf("  判决线（**跑前写死**，见 temporal_residual.h 同名常量）：\n"
                "    ① 因果坐实 = ρ ≤ %s×ρ* 且 frac>τ 相对臂 0 降 ≥ %sx\n"
                "    ② 否证     = 所有消融臂 ρ ≥ %s×ρ(臂0) ⟹ 盒不是主因\n"
                "    ③ 通用性   = 逐机位 / 逐靶方向一致（只有报障机位降 = 场景特异 = 不算修好）\n",
        d2(kBoostFidelityLine).c_str(), d2(kClipCausalFracDrop).c_str(),
        d2(kClipRefuteRatio).c_str());
    // ⚠ **面积与尾巴可以反向动**（2026-10-06 在 aa_target 上撞见的口径告警，写在这里防误读）：
    // 关盒让 `frac>τ`（面积）**升**而 p99.9（尾巴）**降 3×**——长窗累积把「少数像素大幅跳」换成
    // 「多数像素小幅漂」。两者量的不是同一件事：尾巴 = 用户报的「数得清的闪」，面积 = 1 LSB
    // 边缘的可见面积。所以「修得掉可见症状」这条线**两栏都要看**，任一单独当判据都会误判。
    std::printf("  ⚠ frac>τ 是**面积**、p999 是**尾巴**，二者可反向（长窗把少数大幅跳换成多数"
                "小幅漂）；判「修没修掉」两栏一起读\n");
}

/**
 * 一臂的两次采集（短窗 + 长窗）并填好 `r`。返回 false = 采集失败（调用侧立刻收表）。
 *
 * ⚠ **两态必须吃同一臂**：ρ 是「同一裁剪行为下，长窗档压掉几倍」。若短窗固定在
 * 臂 0、长窗换臂，ρ 的分母就跟着臂变，跨臂不可比（表会看着漂亮但答不了问题）。
 * 「本臂 vs 今天」是另一个数 = swingOn[臂X] / swingOn[臂0]，表里另算。
 */
bool collectClipAblArm(const std::string& dir, const std::string& pre, const BistroCam& cam,
    int periods, double warmMult, double longFeedback, const ClipArm& arm, size_t i,
    double gradRef, double& tau, ClipAblRow& r)
{
    const std::string nm = clipArmName(arm);
    const std::string idx = "_" + std::to_string(i);
    try {
        const BistroRun off = runBistroGroup(dir + "_off" + idx, "臂" + nm + " 短窗",
            pre + "off" + idx, cam, 0.12, 1.0, periods, warmMult, tau, false, true, true, arm);
        const BistroRun on = runBistroGroup(dir + "_on" + idx, "臂" + nm + " 长窗",
            pre + "on" + idx, cam, longFeedback, 1.0, periods, warmMult, tau, false, true,
            true, arm);
        r.swingOff = off.stat.meanSwing;
        r.swingOn = on.stat.meanSwing;
        r.rho = r.swingOff > 0.0 ? r.swingOn / r.swingOff : 0.0;
        r.rhoStar = emaSuppressionRatio(longFeedback, 0.12);
        r.line = kBoostFidelityLine * r.rhoStar;
        r.fracOff = off.stat.fracOverTau;
        r.fracOn = on.stat.fracOverTau;
        r.p999Off = off.stat.p999Swing;
        r.p999On = on.stat.p999Swing;
        r.gradOff = gradRef > 0.0 ? off.grad / gradRef : 0.0;
        r.gradOn = gradRef > 0.0 ? on.grad / gradRef : 0.0;
        r.ok = true;
        return true;
    } catch (const std::exception& e) {
        std::printf("  %-11s ✗ 采集失败: %s\n", nm.c_str(), e.what());
        return false;
    }
}

/// 一臂的读数行 + 三条判定（ρ 机制 · 可见症状面积 + 尾巴 · 锐度**只罚变糊侧**）。
void printClipAblRow(const ClipAblRow& r, const std::vector<ClipAblRow>& rows, size_t i)
{
    const std::string nm = clipArmName(r.arm);
    const std::string vsToday = i == 0 || rows[0].swingOn <= 0.0 ? "—"
        : d4(r.swingOn / rows[0].swingOn);
    std::printf("  %-11s %9s %9s %7s %7s %7s %6s%% %6s%% %8s %8s %7s %7s  ", nm.c_str(),
        d6(r.swingOff).c_str(), d6(r.swingOn).c_str(), d4(r.rho).c_str(), d4(r.line).c_str(),
        vsToday.c_str(), d2(r.fracOff * 100.0).c_str(), d2(r.fracOn * 100.0).c_str(),
        d4(r.p999Off).c_str(), d4(r.p999On).c_str(), d4(r.gradOff).c_str(),
        d4(r.gradOn).c_str());
    if (i == 0)
        std::printf("（基准）\n");
    else {
        // 三条都得报：ρ（机制）· 可见症状（面积 + 尾巴）· 锐度（**只罚变糊侧**）。
        const bool rhoOk = r.rho <= r.line;
        const bool fracOk = rows[0].fracOn > 0.0
            && r.fracOn <= rows[0].fracOn / kClipCausalFracDrop;
        const bool blurCheat = r.gradOn < rows[0].gradOn - kBlurTolerance
            || r.gradOff < rows[0].gradOff - kBlurTolerance;
        std::printf("%s%s%s\n", rhoOk ? "ρ达闭式 " : "",
            fracOk ? "**症状降(面积)** " : (rhoOk ? "面积未达4× " : "未达标 "),
            blurCheat ? "**比今天更糊 = 靠糊换安静，不算修好**" : "");
    }
}

/// **在场性守卫**（债-55 的教训：参数写进去了但没生效，注释写了、代码没做、判据没红）：
/// scene.lua 的未知键引擎**不报错**，所以「键没生效」必须由判据自己抓——所有臂的读数与臂 0
/// **逐位相同** = 键根本没进引擎（拼错 / 载荷断链），整表作废。
bool clipAblAllIdentical(const std::vector<ClipAblRow>& rows)
{
    bool identical = rows.size() > 1;
    for (size_t i = 1; i < rows.size(); ++i)
        identical = identical && rows[i].ok
            && rows[i].swingOn == rows[0].swingOn && rows[i].swingOff == rows[0].swingOff;
    return identical;
}

/// 否证线 = **所有**消融臂 ρ ≥ 0.8 × ρ(臂0)（线跑前写死）。
bool clipAblAllRefuted(const std::vector<ClipAblRow>& rows)
{
    bool refute = true;
    for (size_t i = 1; i < rows.size(); ++i)
        refute = refute && rows[i].ok && rows[i].rho >= kClipRefuteRatio * rows[0].rho;
    return refute;
}

/// 汇总行（否证 / 坐实 两分支，跑前写死）。
void printClipAblSummary(const std::vector<ClipAblRow>& rows)
{
    // ⚠ 外层**不能**再包 `**`：被包的串自己带 `**否证**` / `**坐实**`，套上去会打出
    //   `**…**否证**：…**` 这种错位的 markdown（2026-10-06 修；判据输出是要被读的东西）。
    std::printf("  ⭐ 汇总：ρ(臂0) = %s；%s\n", d4(rows[0].rho).c_str(),
        clipAblAllRefuted(rows)
            ? "所有消融臂都没把 ρ 压下来 ⟹ **否证**：邻域盒不是主因，短路假设死，"
                 "必须回头查别处（不许换说法继续猜）"
            : "至少一臂显著压下了 ρ ⟹ 邻域盒短路**坐实**为因（逐臂看上行判定）");
}

/**
 * 逐臂跑「短窗 / 长窗」两组并**当场出表**（长跑中途可读，不是一次性在最后打印）。
 * τ 由臂 0 的短窗组定、全臂共用（与 `runBistroCamera` 同口径：跨臂比的是同一把尺子）。
 * 臂内 α：短窗 = feedback 0.12、长窗 = `--long-feedback`；热身按各自 α 算（红线 2，
 * 长窗档按长窗 α 热身）。
 */
std::vector<ClipAblRow> runBistroClipAblation(const std::string& dir, const std::string& pre,
    const BistroCam& cam, int periods, double warmMult, double longFeedback,
    const std::vector<ClipArm>& arms)
{
    std::vector<ClipAblRow> rows;
    double tau = 0.0;
    printClipAblHeader(cam, longFeedback);
    // **锐度分母**：同次运行的 fb=1（完全不累积）= 理想 16 相位平均的梯度能量。
    // 没有它，「尾巴降 20×」可以纯靠"更糊了"拿到（ρ 是比值糊不动它，但可见症状的改善会被糊虚报）。
    // ── ⚠ 护栏的**方向**（第一版写反，2026-10-06，被自己的数据抓出来）──
    // 读数 = 本臂相位平均图梯度能量 ÷ fb=1 相位平均图。于是：
    //   **< 1 = 比理想 16 相位平均更糊** ⟹ 这才是「靠糊换安静」的方向；
    //   **> 1 = 比它更锐**，**不是作弊**：base_space 下每份贡献先 un-jitter 再累积，EMA 收敛到
    //     的是**基空间点采样**，而 fb=1 参考是 16 个亚像素偏移的**盒平均**（≈1px 模糊）。
    // 实测（brickfence，长窗档）：现行 0.828 · 关盒 1.133 ⟹ **今天的裁剪正在往图里注入模糊**
    //   （裁剪把历史换成 centroid = 5 抽头空间均值，长窗下裁剪事件权重 1−α ≈ 99%）——
    //   同一个 bug 的第二条代价：静止机位下交付的是糊图。
    // ⟹ 反作弊只查**变糊**这一侧，且以「不比臂 0 更糊」为准（臂 0 自己就偏糊；拿 1.0 当红线
    //    会把合法修法误杀）。
    double gradRef = 0.0;
    try {
        const BistroRun ref = runBistroGroup(dir + "_ref", "参考 fb=1（梯度分母）", pre + "ref",
            cam, 1.0, 1.0, periods, warmMult, tau, false);
        gradRef = ref.grad;
    } catch (const std::exception& e) {
        std::printf("  ✗ 锐度分母组采集失败: %s ⟹ 本表无法排除「靠糊换安静」，作废\n", e.what());
        return rows;
    }
    std::printf("  锐度分母 = fb=1 相位平均图线性域梯度能量 %s（锐度比 **<1 = 更糊 = 作弊方向**、"
                ">1 = 比 16 相位平均更锐 = 基空间 EMA 的性质；只罚「比臂 0 低 >%s」）\n",
        d6(gradRef).c_str(), d2(kBlurTolerance).c_str());
    std::printf("  %-11s %9s %9s %7s %7s %7s %7s%% %7s%% %8s %8s %7s %7s  %s\n", "臂", "swingOff",
        "swingOn", "ρ", "线", "vs今天", "fracOff", "fracOn", "p999Off", "p999On", "锐Off", "锐On",
        "判定");
    for (size_t i = 0; i < arms.size(); ++i) {
        ClipAblRow r;
        r.arm = arms[i];
        if (!collectClipAblArm(dir, pre, cam, periods, warmMult, longFeedback, r.arm, i,
                gradRef, tau, r)) {
            rows.push_back(r);
            return rows;
        }
        rows.push_back(r);
        printClipAblRow(r, rows, i);
    }
    if (rows.size() > 1 && rows[0].ok) {
        if (clipAblAllIdentical(rows)) {
            std::printf("  ✗ **所有臂读数与臂 0 逐位相同** ⟹ `taa.clip_mode` 没生效"
                        "（键名 / 载荷 / 配置注入断链），本表作废（在场性守卫，债-55）\n");
            return rows;
        }
        printClipAblSummary(rows);
    }
    return rows;
}
}  // namespace

/**
 * 批 13.11 **消融 A/B 的机位循环**（`--clip-abl`）。默认臂集 = 现行 + 四条消融/修法臂：
 * 关盒（天花板参照）/ 扩 1.5×（债-60 重测臂）/ box9（空间加宽）/ 软裁剪 λ=0.3 与 0.1（修法臂）。
 * 每臂两组（短窗 + 长窗），判决线跑前写死在 `runBistroClipAblation`。
 */
int runBistroClipAblSweep(int periods, double warmMult, const std::string& camera,
    double longFeedback, double onlyMode, double onlySoft)
{
    const std::vector<ClipArm> arms = clipAblArmSet(onlyMode, onlySoft);
    std::printf("bistro 历史裁剪消融 A/B（批 13.11）：交付配置原样，逐臂只差 `taa.clip_mode`"
                "（短窗 fb=.12 / 长窗 fb=%s）\n  %d×%d  静止机位  周期数 %d\n",
        d4(longFeedback).c_str(), kSize[0], kSize[1], periods);
    int rc = 0;
    for (const auto& cam : kBistroCams) {
        if (camera != "all" && camera != cam.name)
            continue;
        const auto rows = runBistroClipAblation(
            projectRoot() + "/build/imgqa_clipabl_" + cam.name, "abl" + std::string(cam.name),
            cam, periods, warmMult, longFeedback, arms);
        logClipAblCamera(cam, longFeedback, periods, rows);
        if (!rows.empty() && !rows.back().ok)
            return 2;
        // 在场性守卫红 ⟹ rc=2（读数全同 = 键没生效 = 本表作废，见 runBistroClipAblation）
        if (clipAblAllIdentical(rows))
            return 2;
    }
    return rc;
}

int runBistroSweep(int periods, double warmMult, const std::string& camera, double longFeedback,
    bool diagBsOff, bool reject, const ClipArm& arm)
{
    std::printf("bistro 真实场景时域残差（债-48，**只出数不判**）：交付配置 configs/ 原样"
                "（deferred + env/skybox/IBL/AO/shadow 全开 + base_space=交付默认 true）\n"
                "  %d×%d  静止机位  折叠 (2,%d,H,W)  周期数 %d  短窗档 α=0.12  长窗档 α=%s\n"
                "  区域 = **全帧**（交付 picking=false 无 objectID，打开 = 改图形态；与 aa_target 的"
                "数不可直接比，算子/域/τ 定义相同）\n",
        kSize[0], kSize[1], kResidualPeriod, periods, d4(longFeedback).c_str());
    const std::string shotDir = projectRoot() + "/build/rgl/screenshots";
    int rc = 0;
    for (const auto& cam : kBistroCams) {
        if (camera != "all" && camera != cam.name)
            continue;
        const int one = runBistroCamera(cam, periods, warmMult, longFeedback, shotDir, diagBsOff,
            reject, arm);
        if (one == 2)
            return 2;  // 采集失败 / 锚点中止：后面的机位不跑（停下报告）
        rc = std::max(rc, one);
    }
    if (rc == 0)
        std::printf("\n全部机位：反向守卫绿、锚点在场。长窗压没压住 = 看「长窗/短窗」行与热图，"
                    "本命令不判（口径同合成靶长短窗变体先例）。\n");
    return rc;
}

}  // namespace RGL::imgqa