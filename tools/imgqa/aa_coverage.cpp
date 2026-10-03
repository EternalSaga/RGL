// 逐杆覆盖率对拍闭式（批 12.6 T4）——实现与判据执行。原理见 aa_coverage.h 头注释。
#include "imgqa.h"
#include "aa_coverage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include <opencv2/core.hpp>

namespace RGL::imgqa {

double pxPerMeter(double d)
{
    return static_cast<double>(kViewH) / (2.0 * d * std::tan(kFovRad * 0.5));
}

std::vector<RodExpectation> ladderExpectations()
{
    // 杆画在 z=0（靶头注释：图元在 z=0 前层压过画板）⇒ 深度 = 机位 z。
    const double ppm = pxPerMeter(kCamZ);
    std::vector<RodExpectation> out;
    out.reserve(kLadderRods);
    for (unsigned i = 0; i < kLadderRods; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kLadderRods - 1u);
        const double w = kLadderWMin + t * (kLadderWMax - kLadderWMin);
        const double x = kLadderX0 + static_cast<double>(i) * kLadderDx;
        RodExpectation e;
        e.index = i;
        e.widthWorld = w;
        e.widthPx = w * ppm;
        e.colCenter = static_cast<int>(std::lround(kViewW * 0.5 + x * ppm));
        // 行区间：杆顶在屏幕上方（y 越大越靠上），两端各内缩 4px 避开端点覆盖不完整
        const double yTop = kLadderY + kLadderHeight * 0.5;
        const double yBot = kLadderY - kLadderHeight * 0.5;
        e.rowMin = static_cast<int>(std::lround(kViewH * 0.5 - yTop * ppm)) + 4;
        e.rowMax = static_cast<int>(std::lround(kViewH * 0.5 - yBot * ppm)) - 4;
        out.push_back(e);
    }
    return out;
}

double invAces(double luma)
{
    // 正向 = analytic.cpp 的闭式 ACES（probe 已拿真机 ±2 LSB 对拍验证）；这里只做反解。
    // 曲线在 x≥0 单调增 ⟹ 二分。上界 8.0 足够：靶是 unlit 黑白，线性值 ∈ [0,1]。
    double lo = 0.0, hi = 8.0;
    for (int i = 0; i < 64; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (acesTonemap(Vec3{mid, mid, mid}).x < luma)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

CoverageReport measureCoverage(const cv::Mat& luma)
{
    CoverageReport r;
    if (luma.empty() || luma.cols != kViewW || luma.rows != kViewH)
        return r;  // 尺寸不对 = 采集配置错，不出数（出了就会被当成"测到了"）

    // ── 电平自检（阴阳对照）──
    // 白板取阶梯**上方**的一条带：画板 y∈[0.05,0.62]、阶梯 y∈[0.13,0.55]，
    // y∈[0.56,0.61] 只剩画板本体（环/十字/放射都在 y≈0.34）。
    const double ppm = pxPerMeter(kCamZ);
    const int wRow0 = static_cast<int>(std::lround(kViewH * 0.5 - 0.61 * ppm));
    const int wRow1 = static_cast<int>(std::lround(kViewH * 0.5 - 0.56 * ppm));
    if (wRow1 <= wRow0 || wRow1 > luma.rows)
        return r;
    cv::Mat plateBand = luma(cv::Rect(300, wRow0, 600, wRow1 - wRow0));
    r.whiteLuma = cv::mean(plateBand)[0];

    const auto rods = ladderExpectations();
    // 窗口 ±6px：最宽杆 6.5px、杆间距 21.7px ⟹ 既容得下 AA 摊开，又碰不到邻杆。
    constexpr int kHalf = 6;
    double darkestLuma = 1e9;
    std::vector<cv::Mat> cov;  // 每杆的逐像素覆盖率图（先算覆盖率再对行取均值，顺序不能反）
    for (const auto& e : rods) {
        if (e.colCenter - kHalf < 0 || e.colCenter + kHalf >= luma.cols || e.rowMin < 0
            || e.rowMax > luma.rows || e.rowMax <= e.rowMin) {
            return r;  // 投出屏 = 机位不对
        }
        cv::Mat c(e.rowMax - e.rowMin, 2 * kHalf + 1, CV_64FC1);
        double centerSum = 0.0;
        for (int y = e.rowMin; y < e.rowMax; ++y) {
            for (int x = -kHalf; x <= kHalf; ++x)
                c.at<double>(y - e.rowMin, x + kHalf)
                    = 1.0 - invAces(luma.at<double>(y, e.colCenter + x));
            centerSum += luma.at<double>(y, e.colCenter);
        }
        // 杆心亮度（**亮度域**，与 whiteLuma 同域可直接比）。⚠ 别拿 c 求均值再反解一次：
        // c 已经是覆盖率，再套一次 1−aces⁻¹ 会得到 0.97 这种荒值（本文件真实踩过）。
        darkestLuma = std::min(darkestLuma, centerSum / static_cast<double>(c.rows));
        cov.push_back(std::move(c));
    }
    r.blackLuma = darkestLuma;
    // 阴阳自检，任一不满足 = 靶没画对 / 机位不对 / 曝光被改，此时「覆盖率误差」无意义：
    // ① 白板亮度须对拍**闭式** ACES(1)（不是"落在某区间"——区间是拍的，闭式是推的）；
    // ② 杆心须接近全黑（最宽杆 6.5px，中心列是全覆盖）。
    const double whiteExpect = acesTonemap(Vec3{1.0, 1.0, 1.0}).x;
    r.valid = std::abs(r.whiteLuma - whiteExpect) < 0.05 && r.blackLuma < 0.05;

    for (size_t i = 0; i < rods.size(); ++i) {
        RodMeasurement m;
        m.expect = rods[i];
        // 列覆盖率 = 该列各行覆盖率的均值；沿列求和 = 黑度积分（单位：列）。
        for (int x = 0; x < cov[i].cols; ++x) {
            double s = 0.0;
            for (int y = 0; y < cov[i].rows; ++y)
                s += cov[i].at<double>(y, x);
            m.integral += s / static_cast<double>(cov[i].rows);
        }
        m.error = m.integral - m.expect.widthPx;
        m.relError = m.expect.widthPx > 0.0 ? m.error / m.expect.widthPx : 0.0;
        r.rods.push_back(m);
    }
    return r;
}

int cmdAaCoverage(const std::string& variant, int warm)
{
    const std::string root = projectRoot() + "/build";
    std::printf("逐杆覆盖率对拍：%d×%d  px/m = %.2f  variant=%s  warm=%d\n\n", kViewW, kViewH,
        pxPerMeter(kCamZ), variant.empty() ? "forward" : variant.c_str(), warm);

    // 两态各采一帧：TAA 关 = 被测量（无 AA 时应当量化失真），TAA 开 = 判据对象。
    // 静止机位、单帧 ⟹ 与采集时序无关（这正是它优于逐帧差的地方，见 plan §7 第 10 条）。
    const auto grab = [&](const std::string& tag, bool taa, double jitterScale) {
        ProfileOpts o;
        o.entitiesLua = "{ type = \"aa_target\" }";
        o.envIntensity = 0.0;
        o.skybox = false;
        o.exposure = 1.0;
        o.aoStrength = 0.0;
        o.shadowPass = false;
        o.picking = false;
        o.taa = taa;
        o.taaJitterScale = jitterScale;
        o.variant = variant;
        o.size = {kViewW, kViewH};
        const std::string dir = root + "/imgqa_aacov_" + tag;
        makeProfile(dir, o);
        Engine eng(dir);
        // 机位必须与 aa_coverage.h 的 kCamZ/kFovRad 一致（投影映射全靠它），且与
        // `imgqa temporal` 的 yawCam(0) 同一台相机：pos (0,0,2) → target (0,0,-1)、fov 45°。
        eng.setCamera(Engine::Camera{{0.0, 0.0, kCamZ}, {0.0, 0.0, -1.0}, kFovRad * 180.0 / M_PI});
        std::ignore = eng.shotBurst("screenshots", warm, "aacov_" + tag + "_w");  // 让历史收敛
        const auto files = eng.shotBurst("screenshots", 1, "aacov_" + tag + "_c");
        Frame f;
        f.bgr = readPngBgr(files.front());
        prepareLinear(f);
        return f;
    };

    CoverageReport off, on, onJ0;
    try {
        off = measureCoverage(grab("off", false, 1.0).luma);
        on = measureCoverage(grab("on", true, 1.0).luma);
        // 消融：TAA 开但**不抖**（jitter_scale=0）⟹ 历史重采样零偏移。若黑晕随抖动一起消失，
        // 就把「两侧各一列 0.87 覆盖率」的成因锁到「历史重采样 × 邻域裁剪」的相互作用上；
        // 若仍在，则与重采样无关，得往裁剪本身找。
        onJ0 = measureCoverage(grab("onj0", true, 0.0).luma);
    } catch (const std::exception& e) {
        std::printf("  采集失败: %s\n", e.what());
        return 2;
    }

    if (!off.valid || !on.valid || !onJ0.valid) {
        std::printf("  电平自检未过（TAA关 白=%.4f 黑=%.4f / TAA开 白=%.4f 黑=%.4f / 不抖 白=%.4f 黑=%.4f）"
                    "——靶/机位/曝光不对，不出数\n",
            off.whiteLuma, off.blackLuma, on.whiteLuma, on.blackLuma, onJ0.whiteLuma,
            onJ0.blackLuma);
        return 2;
    }
    std::printf("  电平自检：白板 %.4f（闭式 ACES(1)）  杆心 < %.4f  ✓\n\n", on.whiteLuma,
        on.blackLuma);
    std::printf("  杆  世界宽    期望列   TAA关    误差     TAA开    误差      不抖(消融)\n");
    std::printf("  --  -------   ------   ------   -----    ------   -----    ---------\n");
    for (size_t i = 0; i < on.rods.size(); ++i) {
        const auto& a = off.rods[i];
        const auto& b = on.rods[i];
        const auto& d = onJ0.rods[i];
        std::printf("  %2u  %.5f  %6.3f   %6.3f  %+6.3f    %6.3f  %+6.3f   %6.3f\n",
            b.expect.index, b.expect.widthWorld, b.expect.widthPx, a.integral, a.error,
            b.integral, b.error, d.integral);
    }
    const auto maxAbsErr = [](const CoverageReport& c) {
        double m = 0.0;
        for (const auto& r : c.rods)
            m = std::max(m, std::abs(r.error));
        return m;
    };
    const double eOff = maxAbsErr(off), eOn = maxAbsErr(on), eJ0 = maxAbsErr(onJ0);
    const auto mean = [](const CoverageReport& c) {
        double s = 0.0;
        for (const auto& r : c.rods)
            s += r.integral;
        return s / static_cast<double>(c.rods.size());
    };
    std::printf("\n  最大逐杆绝对误差：TAA 关 %.3f 列   TAA 开 %.3f 列   不抖消融 %.3f 列\n", eOff,
        eOn, eJ0);
    // 黑晕诊断：正确的时间平均只该把「floor 掉的小数部分」补回来，即 on − off 的均值
    // 应 ≈ expected − off；实测若显著更大，多出来的就是凭空产生的黑。
    const double wantGain = [](const CoverageReport& c) {
        double s = 0.0;
        for (const auto& r : c.rods)
            s += r.expect.widthPx - r.integral;
        return s / static_cast<double>(c.rods.size());
    }(off);
    std::printf("  黑晕：TAA 开相对 TAA 关多黑 %.3f 列/杆（闭式只该补 %.3f）⟹ 凭空多黑 %.3f 列\n",
        mean(on) - mean(off), wantGain, mean(on) - mean(off) - wantGain);
    std::printf("  不抖消融（jitter_scale=0）相对 TAA 关多黑 %.3f 列/杆"
                "——≈0 即黑晕需要抖动，锁定「历史重采样 × 邻域裁剪」\n",
        mean(onJ0) - mean(off));

    const auto d3 = [](double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.3f", v);
        return std::string(b);
    };
    std::vector<Verdict> vs;
    vs.push_back(verdictFor("aa-coverage-fidelity",
        eOn <= 0.35 && eOff >= 3.0 * std::max(eOn, 1e-9),
        "最大逐杆误差：TAA 开 " + d3(eOn) + " 列（上限 0.35）  TAA 关 " + d3(eOff)
            + " 列（须 ≥3× 于开态，反向对照）  不抖消融 " + d3(eJ0) + " 列"));

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("aa-coverage",
        {{"variant", variant.empty() ? "forward" : variant}, {"warm", std::to_string(warm)},
            {"white_luma", d3(on.whiteLuma)}, {"err_off", d3(eOff)}, {"err_on", d3(eOn)},
            {"err_jitter0", d3(eJ0)},
            {"halo", d3(mean(on) - mean(off) - wantGain)}},
        vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
