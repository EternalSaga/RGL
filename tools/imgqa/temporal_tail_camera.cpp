// temporal-tail --motion camera 的**闸门与判决编排**（批次 B11 从 `temporal_motion.cpp`
// 拆出）：δ=0 反向组、gate 断点、逐机位判决与 `cmdTemporalTailCamera`。
// 缝 = **采集在 `temporal_smear.cpp`、判决编排在**本文件（同 `temporal_residual_target*.cpp`
// 那条「采集层 vs 判据层」的缝）。
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

StaticBaseline collectStaticBaseline(const std::string& base, const BistroCam& cam, int burst,
    int warm, double mainRad, double tau, int margin,
    const std::vector<const SmearRow*>& mainRows)
{
    StaticBaseline sb;
    sb.arms = {
        { "REF", 0.0, 0.12, false, 0.0 },
        { "Z", 0.0, 1.0, true, 0.0 },    ///< **零点锚**：jitter 0 + α=1
        { "A0", 0.0, 0.01, true, 1.0 },  ///< 现行盒 + 长窗（旧版唯一的地板臂）
        { "B12", 1.0, 0.12, true, 1.0 }, ///< α 轴低端的 δ=0 对照（本轮新增）
        { "B01", 1.0, 0.01, true, 1.0 }, ///< α 轴高端的 δ=0 对照（本轮新增）
    };
    // 🔴 采集用 **0**（相机静止），映射用 **mainRad**（借主角度的 φ）。见 collectSmearAngle 头注释。
    sb.stat = collectSmearAngle(base, cam, burst, warm, 0.0, mainRad, tau, margin, sb.arms, "s");
    std::printf("\n── 运动归因（主角度 frac − 同臂 δ=0 frac；δ=0 借同一份 φ）──\n");
    std::printf("  %-5s %12s %12s %12s %24s\n", "臂", "frac(主)", "frac(δ=0)", "归因",
        "meanResid 主 / δ=0");
    for (const SmearRow* r : mainRows) {
        const SmearRow& s = sb.row(r->tag);
        std::printf("  %-5s %12.6f %12.6f %12.6f %12.6f / %.6f\n", r->tag.c_str(), r->frac, s.frac,
            sb.attrFrac(*r), r->meanResid, s.meanResid);
    }
    // **分子零点对照**：δ=0 组的 meanResid 是「相机不动时 out 离参考多远」，它是主角度组
    // 读数的**加性底**。两者同量级 ⟹ 相机运动根本没被量进去，分子量的是别的东西。
    std::printf("\n── 分子零点/地板对照（meanResid，与 Δ 无关）──\n");
    std::printf("  零点锚 Z(δ=0) %.6f · 地板 A0(δ=0) %.6f\n", sb.row("Z").meanResid,
        sb.row("A0").meanResid);
    return sb;
}

GateBreakpoint collectGateBreakpoint(const std::string& base, const BistroCam& cam, int burst,
    int warm, double tau, int margin, double gateRad, double lowArm0, double lowArm11,
    double mainCenterShiftPx, double mainResid0, double mainResid11)
{
    GateBreakpoint out;
    const std::vector<SmearArm> gateArms = {
        { "REF", 0.0, 0.12, false, 0.0 },
        { "A0", 0.0, 0.01, true, 1.0 },
        { "A11", 11.0, 0.01, true, 1.0 },
    };
    const SmearAngle high
        = collectSmearAngle(base, cam, burst, warm, gateRad, gateRad, tau, margin, gateArms, "h");
    // ⚠ 对表用**中心位移**：臂 11 的 `motion` 是该像素的重投影位移，中心最具代表性；
    //   全图最大值在画面边缘（透视收缩放大），拿它对 2px 断点会低估所处的一侧。
    std::printf("\n── gate 断点对照（推导断点 = motion 2px，斜坡宽 1px）──\n");
    std::printf("  低角度档：中心 %gpx → gate %g（%s）\n", mainCenterShiftPx,
        gateAt(mainCenterShiftPx), gateSide(mainCenterShiftPx));
    std::printf("  高角度档：中心 %gpx → gate %g（%s）\n", high.centerShiftPx,
        gateAt(high.centerShiftPx), gateSide(high.centerShiftPx));
    // 🔴 **两条口径并排打**，因为它们的结论相反（见 `GateBreakpoint` 注释）：Δ 加权「几帧」列
    //    对门**失明**（纯旋转无去遮挡），无条件残差列才看得见门。只打一条必然误读。
    std::printf("  %-6s %10s %10s %10s %12s %12s %10s\n", "档", "臂0(帧)", "臂11(帧)", "差 11−0",
        "resid 臂0", "resid 臂11", "比 11/0");
    const SmearRow& h0 = high.byTau[kMainTau][0];
    const SmearRow& h11 = high.byTau[kMainTau][1];
    const double hu = mainTauValue(high.oneFrameFrac, 0.0);
    const auto hf = [&](const SmearRow& r) { return hu > 0.0 ? r.frac / hu : 0.0; };
    std::printf("  %-6s %10.2f %10.2f %10.4f %12.6f %12.6f %10.3f\n", "门关侧", hf(h0), hf(h11),
        hf(h11) - hf(h0), h0.meanResid, h11.meanResid, safeRatio(h11.meanResid, h0.meanResid));
    std::printf("  %-6s %10.2f %10.2f %10.4f %12.6f %12.6f %10.3f\n", "门开侧", lowArm0, lowArm11,
        lowArm11 - lowArm0, mainResid0, mainResid11, safeRatio(mainResid11, mainResid0));
    out.mainDiff = lowArm11 - lowArm0;
    out.highDiff = hf(h11) - hf(h0);
    out.mainMotion = mainCenterShiftPx;
    out.highMotion = high.centerShiftPx;
    out.mainResid0 = mainResid0;
    out.mainResid11 = mainResid11;
    out.highResid0 = h0.meanResid;
    out.highResid11 = h11.meanResid;
    out.ran = true;
    return out;
}

GateSides gateSidesOf(const GateBreakpoint& g)
{
    const bool mainOpens = gateAt(g.mainMotion) >= gateAt(g.highMotion);
    GateSides s;
    s.openDiff = mainOpens ? g.mainDiff : g.highDiff;
    s.closedDiff = mainOpens ? g.highDiff : g.mainDiff;
    s.openResidRatio = safeRatio(mainOpens ? g.mainResid11 : g.highResid11,
        mainOpens ? g.mainResid0 : g.highResid0);
    s.closedResidRatio = safeRatio(mainOpens ? g.highResid11 : g.mainResid11,
        mainOpens ? g.highResid0 : g.mainResid0);
    s.openMotion = mainOpens ? g.mainMotion : g.highMotion;
    s.closedMotion = mainOpens ? g.highMotion : g.mainMotion;
    // ⚠ 比的是 **motion**，不是 `gateAt(motion)`——后者返回 gate 值 ∈ [0,1]，拿它跟 2.0 比会
    //   恒假（首轮就把它读成「两档没夹住」，而同一张表上面明明印着 gate 1.000/0.000）。
    s.brackets = s.openMotion <= 1.0 && s.closedMotion >= 2.0;
    return s;
}

int emitCameraVerdicts(const std::string& cameraName, double angleDeg, double gateAngleDeg,
    int burst, double tau, const CameraReadings& r)
{
    std::vector<Verdict> vs;
    // φ 符号标定放在**第一条**：它是其余三条的前提——φ 反号 ⟹ Δ 分母里全是系统错位，
    // 下面三条的读数会一起变得"看起来可解释"（首轮正是如此：pairing/collapse 全绿，
    // 而 gate 断点被稀释掉、α 下界撞线）。红的时候先修 φ，别碰判决线。
    vs.push_back(verdictFor("temporal-tail-phi-sign", r.phiScale > 0.5 && r.phiScale < 1.5,
        "φ 标定扫描 argmin = " + d2(r.phiScale)
            + "（线 = +1；−1 = 符号反了，0 = 映射没生效）· alignedΔ " + d4(r.main.alignedDelta)
            + " vs rawΔ " + d4(r.main.rawDelta) + "，比值 " + d4(r.main.rawDelta > 0.0 ? r.main.alignedDelta / r.main.rawDelta : 0.0)
            + "（>1 ⟹ φ 比恒等映射还差，是反号的定量指纹）"));
    std::cout << "  " << vs.back().line() << "\n";
    vs.push_back(verdictFor("temporal-tail-pairing", r.zero.frac <= 0.02 && r.zero.events > 0.0,
        "frac(Z) = " + d4(r.zero.frac) + "（线 ≤ 0.02；相机运动下配对错一帧立刻跳）"));
    std::cout << "  " << vs.back().line() << "\n";
    // 判决线 2（**重设计，取代 `temporal-tail-smear-alpha-monotone`**）：13.14b 要的是
    // 「α 恒写 0.01 不比 0.12 更鬼」的**非劣性**，不是「窗越长必须越鬼」。旧的
    // 「≥ 1−α = 0.88 帧」下界在**两个角度都被证伪**（0.7565 @2.97px / 0.7588 @1.34px），
    // 机制见上面「运动归因」：去遮挡处的历史本来就是**上一帧**写进去的，与窗长无关
    // ⟹ Δ 加权鬼影对 α 几乎不响应是**物理**，不是仪器坏。而无条件残差那 +49% 是
    // **jitter 相位平均模糊**（随窗长增长），不减掉同臂 δ=0 就会把它当成鬼影。
    const double aShort = r.sb.attrFrac(r.boxShort), aLong = r.sb.attrFrac(r.boxLong)
        , aZero = std::abs(r.sb.attrFrac(r.zero));
    vs.push_back(verdictFor("temporal-tail-alpha-noninferior",
        aShort > 0.0 && aLong <= 1.5 * aShort && aShort > 4.0 * aZero && r.boxShort.events > 1000.0,
        "归因鬼影 短窗(α=.12) = " + d4(aShort) + " · 长窗(α=.01，13.14b 拟交付) = " + d4(aLong)
            + " · 比值 " + d4(aShort > 0.0 ? aLong / aShort : 0.0)
            + "（线 ≤ 1.5 = 本项目「明显」常量；2-tap 无累积参考 1.125）· 零锚归因 " + d4(aZero)
            + "（线：短窗 > 4× 它）· 样本 " + std::to_string(static_cast<long>(r.boxShort.events))
            + " · 单位「一帧过期」= " + d4(r.unit)));
    std::cout << "  " << vs.back().line() << "\n";
    vs.push_back(verdictFor("temporal-tail-static-collapse",
        r.staticZero.frac <= 0.02 && r.staticZero.events > 1000.0,
        "零点锚 Z(δ=0) frac = " + d4(r.staticZero.frac)
            + "（线 ≤ 0.02，同 spin 版 pairing 口径；不塌 = φ 或配对在造信号）· 样本 "
            + std::to_string(static_cast<long>(r.staticZero.events))
            + " · 地板 A0(δ=0) = " + d4(r.staticFloor.frac) + "（相位平均，非零是对的）"));
    std::cout << "  " << vs.back().line() << "\n";
    if (r.gate.ran) {
        const GateSides s = gateSidesOf(r.gate);
        // 🔴 判据口径 = **无条件残差比**，不是 Δ 加权差。理由（§1.17 结论 6）：`frame_yaw` 是原地
        //    纯旋转 ⟹ 无视差 ⟹ 无去遮挡 ⟹ Δ 加权把质量全压在普通运动边缘上，而臂 11 多出来的那
        //    部分是**平坦区的时间平均模糊**，Δ 加权下测不出（实测 −0.0026 帧，而残差比 1.444）。
        //    本判据契约写的是「判门确实在起作用」⟹ 该用**看得见门**的口径。两臂只差 `clip_mode`
        //    （jitter/α/模糊地板全同）⟹ 比值无需 δ=0 归因即干净。
        // 线：② 门开侧 ≥ 1.15（方向由门的表达式定死：开记忆 ⟹ 历史更多 ⟹ 离单帧参考更远；
        //        1.15 比跨进程确定性地板高四个量级）；③ 门关侧更靠近 1（gate=0 ⟹ `mem` 退回
        //        本帧瞬时 `ext` ⟹ 硬裁剪 ⟹ 臂 11 拿臂 0 的行为）。
        vs.push_back(verdictFor("temporal-tail-gate-breakpoint",
            s.brackets && s.openResidRatio >= 1.15
                && std::abs(s.closedResidRatio - 1.0) < std::abs(s.openResidRatio - 1.0),
            "门开侧 " + d4(s.openMotion) + "px（gate " + d2(gateAt(s.openMotion))
                + "）resid 臂11/臂0 = " + d4(s.openResidRatio) + "（线 ≥ 1.15 = 门在场）· 门关侧 "
                + d4(s.closedMotion) + "px（gate " + d2(gateAt(s.closedMotion)) + "）= "
                + d4(s.closedResidRatio) + "（线 |·−1| < 门开侧 = 退回臂 0）· Δ 加权口径"
                + "（**对门失明**，仅打印）" + d4(s.openDiff) + " / " + d4(s.closedDiff) + " 帧"
                + (s.brackets ? "" : " ⟹ **两档没夹住斜坡 [1,2]px**，本判据无从谈起")));
        std::cout << "  " << vs.back().line() << "\n";
    }

    logCall("temporal-tail",
        {{"camera", cameraName}, {"motion", "camera"}, {"angle", d4(angleDeg)},
            {"gate-angle", d4(gateAngleDeg)}, {"burst", std::to_string(burst)},
            {"tau", d4(tau)}, {"center-shift-px", d4(r.main.centerShiftPx)},
            {"max-shift-px", d4(r.main.maxShiftPx)}},
        vs);
    const bool allOk = std::all_of(vs.begin(), vs.end(), [](const Verdict& v) { return v.ok; });
    std::printf("\n  ⟹ %s\n", allOk
            ? "全绿：frac 可作为机位运动鬼影判据，读主表与 gate 断点对照"
            : "有判决红 ⟹ **不许报臂间结论**（先修仪器，别改判决线）");
    return allOk ? 0 : 1;
}

}  // namespace tmotion

int cmdTemporalTailCamera(const std::string& cameraName, double angleDeg, double gateAngleDeg,
    int burst, int warm, double tau, int margin)
{
    const BistroCam* cam = nullptr;
    for (const BistroCam& c : kBistroCams)
        if (cameraName == c.name)
            cam = &c;
    if (!cam) {
        std::fprintf(stderr, "机位不存在: %s（可选 %s）\n", cameraName.c_str(),
            bistroCamNames().c_str());
        return 2;
    }
    const double pi = 3.14159265358979323846;
    const std::string base = projectRoot() + "/build/imgqa_tailcam_" + cameraName;

    // arms[0] = 参考（TAA 关 + 无抖动 = 基位置真值）。Z = 零锚点（α=1 + 无抖动）。
    // 🔴 **本表就是 13.14b 的交付对照**：α 轴用 `feedback` **直接给**（B12 vs B01）。长窗曾经
    // 由 static boost 的地板提供 —— 首轮实测「B12」与「B12 + boost」的 frac 与 meanResid
    // **逐位相同**（0.0649 / 0.028841），机制上也本该如此：机位在动 ⟹ 静止探测器不点火
    // ⟹ boost 恒不生效。这既是「它在运动下是死代码」的证据，也是它该删的理由之一。
    const std::vector<SmearArm> arms = {
        { "REF", 0.0, 0.12, false, 0.0 },
        { "Z", 0.0, 1.0, true, 0.0 },
        { "B12", 1.0, 0.12, true, 1.0 },  ///< α 轴低端（关盒 + 短窗）
        { "B01", 1.0, 0.01, true, 1.0 },  ///< α 轴高端（关盒 + 长窗 = 13.14b 拟交付 α）
        { "A0", 0.0, 0.01, true, 1.0 },   ///< 现行盒 + 长窗
        { "A11", 11.0, 0.01, true, 1.0 }, ///< 臂 11（记忆挂重投影位移）+ 长窗
    };
    const auto indexOf = [&](const char* tag) {
        for (size_t i = 1; i < arms.size(); ++i)
            if (std::string(arms[i].tag) == tag)
                return static_cast<long>(i) - 1;
        throw std::runtime_error(std::string("臂不存在: ") + tag);
    };

    const double mainRad = angleDeg * pi / 180.0;
    const SmearAngle main = collectSmearAngle(base, *cam, burst, warm, mainRad, mainRad, tau,
        margin, arms, "m");
    const auto& row = main.byTau[kMainTau];
    const SmearRow& zero = row[static_cast<size_t>(indexOf("Z"))];
    const SmearRow& boxShort = row[static_cast<size_t>(indexOf("B12"))];
    const SmearRow& boxLong = row[static_cast<size_t>(indexOf("B01"))];
    const SmearRow& arm0 = row[static_cast<size_t>(indexOf("A0"))];
    const SmearRow& arm11 = row[static_cast<size_t>(indexOf("A11"))];
    /// 主指标单位：「恰好过期一帧」的 frac（见 `SmearAngle::oneFrameFrac`）
    const double unit = mainTauValue(main.oneFrameFrac, 0.0);
    const auto frames = [&](const SmearRow& r) { return unit > 0.0 ? r.frac / unit : 0.0; };

    std::printf("\n── 相机运动 %.4g°/帧 ──\n", angleDeg);
    printSmearTables(arms, main, tau);
    printSmearDiagnostics(arms, main, tau);
    const double phiScale = printSmearPhiCalibration(main, mainRad);

    // ── 判决线 3/5：**反向** = 相机静止（δ=0）但**沿用主角度的映射与权重门** ──────────
    // 为什么不能直接用 δ=0 的映射：δ=0 ⟹ φ=恒等 ⟹ 静止画面的 Δ ≡ 0 ⟹ frac = 0/0 被兜底成 0，
    // **什么也没证明**。借主角度的 φ 后，Δ 变成「画面平移若干像素后的边缘」= 鬼影本该出现的
    // 位置，于是「分子必须塌」才是真判据（同时证明 φ 本身不造信号）。
    // 🔴 静态组与主角度组**同臂**的理由见 `collectStaticBaseline` 头注释。
    const StaticBaseline sb = collectStaticBaseline(base, *cam, burst, warm, mainRad, tau, margin,
        { &zero, &arm0, &boxShort, &boxLong });
    const SmearRow& staticZero = sb.row("Z");
    const SmearRow& staticFloor = sb.row("A0");

    // ── 判决线 4：**gate 断点**（本仪器存在的理由）──────────────────────────────
    const GateBreakpoint gate = gateAngleDeg > 0.0
        ? collectGateBreakpoint(base, *cam, burst, warm, tau, margin, gateAngleDeg * pi / 180.0,
            frames(arm0), frames(arm11), main.centerShiftPx, arm0.meanResid, arm11.meanResid)
        : GateBreakpoint {};

    return emitCameraVerdicts(cameraName, angleDeg, gateAngleDeg, burst, tau,
        { main, sb, zero, boxShort, boxLong, staticZero, staticFloor, unit, phiScale, gate });
}


}  // namespace RGL::imgqa
