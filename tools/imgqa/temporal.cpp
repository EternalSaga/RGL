// 时域差分判据（批 12.6 T4）——TAA 阳性锚点的度量与执行（docs/taa_plan.md §6）。
//
// ## 为什么激励是「静止机位 + jitter」而不是「移动机位」
//
// 本节是本轮实测换来的，写死在这里免得下一次重新踩：
//
// 时域滤波压的是「同一视点、不同采样相位」的估计差；它**压不掉**「视点真的动了」引起
// 的画面变化——那部分是正确影像，重投影对齐后照样要变。更糟的是 L1 逐帧差对模糊近似
// **守恒**：把一次满幅翻转摊成 3 个像素各差一点，均值几乎不变。所以「匀速微动机位下
// 逐帧差降一个量级」这条初稿判据（plan §6），对**完全正确**的 TAA 也几乎不成立——
// 实测只有 1.2×，而当时闭式算出来该是 3.95×，差点把引擎判死。
//
// 正确的激励 = 平均视点恒定、只变采样相位，也就是 **jitter 本身**；再用 `feedback = 1`
// 做消融（输出 = 本帧 ⇒ 只抖不滤），两组**输入统计完全相同**，比值就是时域累积本身的
// 增益，可以直接拿一阶 IIR 的闭式对拍。移动机位那两组降级为参考值（见 antiPatterns 的
// `moving-camera-l1-ratio`）。
//
// ## 三条硬判据 + 两条参考
//
//   S0 TAA 关 静止             → 引擎确定性地板（≠0 则一切比值是噪声比噪声）
//   S1 TAA 开 feedback=1 静止   → 只抖不滤 = 闪烁本体（**阳性锚点**：必须非零）
//   S2 TAA 开 feedback=.12 静止 → 滤波后（S1/S2 = 时域累积抑制，对拍闭式）
//   M0/M1 TAA 关/开 微动        → 端到端参考值，只报不判
//
// 区域来自 objectID（本工具地基纪律：判据的区域来自几何，不来自图像）；统计一律在
// sRGB 解码后的线性 luma 域做。
//
// 机位运动（M 组）= **绕 up 轴定角 yaw**（不是平移机位）：像素位移与深度无关，且是刚体
// 相机运动，`prevViewProj` 重投影能精确对齐。平移 dolly 会让 9m 外栅栏的单帧位移只剩
// 近处的 22%，信号随深度衰减；SceneSpin 速度不可配（~1°/帧，远超亚像素档）更不能用。
#include "imgqa.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

namespace {

// ── 靶机位（与 rgl/aa_target.cppm 头注释的推荐机位同源）───────────────────────
constexpr double kFovDeg = 45.0;
constexpr int kSize[2] = {1280, 720};
constexpr std::array<double, 3> kCamPos{0.0, 0.0, 2.0};
constexpr std::array<double, 3> kCamTarget{0.0, 0.0, -1.0};

double deg2rad(double d) { return d * 3.14159265358979323846 / 180.0; }

/// 像素焦距（glm::perspective 的 fov 是**垂直**方向，故用高度换算）。
/// 1280×720 @45° ⇒ 869.1 px；画板平面（距机位 2m）⇒ 434.5 px/m。
double focalPx() { return kSize[1] / (2.0 * std::tan(0.5 * deg2rad(kFovDeg))); }

/// 影像整体水平平移 px 像素对应的机位（yaw = px/focal，target 沿世界 X 平移）。
/// 引擎 fps 相机朝向由 `cameraAimAt` 从 position→target 反推（yaw = atan2(-fwd.x, -fwd.z)）。
Engine::Camera yawCam(double px)
{
    const double dist = kCamPos[2] - kCamTarget[2];  // 前向距离 = 3
    return Engine::Camera{{kCamPos[0], kCamPos[1], kCamPos[2]},
        {kCamTarget[0] + dist * std::tan(std::atan(px / focalPx())), kCamTarget[1],
            kCamTarget[2]},
        kFovDeg};
}

/// 一次采集（独立引擎进程 = 独立配置；单实例纪律由 Engine 自己守）。
struct RunResult {
    TemporalStat stat;
    int64_t idPixels = 0;
};

RunResult runSequence(const std::string& dir, bool taa, double feedback, double jitterScale,
    const std::string& variant, double stepPx, int shots, int warm, bool moving,
    const std::string& tag)
{
    std::string lastShot;
    ProfileOpts o;
    o.entitiesLua = "{ type = \"aa_target\" }";  // 内存合成靶，无 glb 文件
    o.envIntensity = 0.0;                        // 背景 = 常量 clear 色，零时域噪声
    o.skybox = false;
    o.exposure = 1.0;
    o.aoStrength = 0.0;   // strength=0 逐位旁路，不给差分添第二来源
    o.shadowPass = false;
    o.picking = true;     // object_id = 区域的唯一出处
    o.taa = taa;
    o.taaFeedback = feedback;
    o.taaJitterScale = jitterScale;
    o.variant = variant;
    o.size = {kSize[0], kSize[1]};
    makeProfile(dir, o);

    Engine eng(dir);
    RunResult r;
    // 预热：每步一次 /shot（**setCamera 本身不推进帧**，靠拍图强制推进；预热不拍就
    // 不知道引擎到底走了几帧，TAA 的 ≈19 帧收敛就成了许愿）。
    for (int i = 0; i < warm; ++i) {
        eng.setCamera(yawCam(moving ? stepPx * i : 0.0));
        lastShot = eng.shot(tag + "_w");
    }
    std::vector<Frame> seq;
    seq.reserve(static_cast<size_t>(shots));
    for (int i = 0; i < shots; ++i) {
        const double px = moving ? stepPx * (warm + i) : 0.0;
        eng.setCamera(yawCam(px));
        Frame f;
        f.camera = yawCam(px);
        lastShot = eng.shot(tag + "_c" + std::to_string(i));
        f.bgr = readPngBgr(lastShot);
        prepareLinear(f);
        seq.push_back(std::move(f));
    }
    // object_id **最后拍**：夹在序列中间会多走一帧，破坏"相邻帧"这个被测量本身。
    // 拍完把机位退回首帧，mask 与序列首图对齐。
    eng.setCamera(yawCam(moving ? stepPx * warm : 0.0));
    const cv::Mat ids = decodeObjectId(readPngBgr(eng.shot(tag + "_id", "object_id")));
    r.idPixels = cv::countNonZero(ids);
    cv::Mat interior;
    cv::compare(ids, 0, interior, cv::CMP_NE);  // ids 是 CV_32SC1 ⟹ 直接 8UC1
    const cv::Mat m = dilateMask(interior, 3);  // 3px：吸收 jitter ±0.5px + 机位扫动
    // 逐帧差用**外扩**mask（边缘像素才是被测量所在），中间调用**未外扩**的靶内部
    // （外扩圈会把背景 clear 色算成中间调，见 imgqa.h 声明处 ⚠）。
    r.stat = temporalDiffStat(seq, m, interior);
    return r;
}

std::string d6(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}

std::string d2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

/// 任一通道不同的 8UC1 掩码（同 judge.cpp 的教训：别用 cv::reduce 合通道）。
cv::Mat anyDiff(const cv::Mat& a, const cv::Mat& b)
{
    cv::Mat d;
    cv::compare(a, b, d, cv::CMP_NE);
    std::vector<cv::Mat> ch;
    cv::split(d, ch);
    cv::Mat any = ch[0];
    for (size_t i = 1; i < ch.size(); ++i)
        cv::bitwise_or(any, ch[i], any);
    return any;
}

}  // namespace

TemporalStat temporalDiffStat(const std::vector<Frame>& shots, const cv::Mat& mask,
    const cv::Mat& softMask)
{
    TemporalStat s;
    if (shots.empty() || shots[0].luma.empty())
        return s;  // pairs = 0：统计量无定义，调用侧按失败处理（不给无定义的东西出数）
    cv::Mat m;
    if (mask.empty())
        m = cv::Mat(shots[0].luma.size(), CV_8UC1, cv::Scalar(255));
    else
        m = mask;
    const cv::Mat sm = softMask.empty() ? m : softMask;
    s.maskPixels = cv::countNonZero(m);
    if (s.maskPixels == 0 || cv::countNonZero(sm) == 0)
        return s;

    // 中间调占比（**空间**抗锯齿在场性，与逐帧差正交）：靶是黑白两值 unlit ⟹ 既非黑
    // 也非白的亮度只可能来自覆盖混合。带按**实测白电平**取（理由见头文件声明）。
    double soft = 0.0;
    for (const auto& f : shots) {
        const double mx = cv::norm(f.luma, cv::NORM_INF, sm);  // luma ≥ 0 ⟹ INF 范数 = 最大值
        cv::Mat band;
        cv::inRange(f.luma, cv::Scalar(0.15 * mx), cv::Scalar(0.85 * mx), band);
        cv::Mat inBand;
        cv::bitwise_and(band, sm, inBand);
        soft += static_cast<double>(cv::countNonZero(inBand))
            / static_cast<double>(cv::countNonZero(sm));
    }
    s.edgeSoftness = soft / static_cast<double>(shots.size());

    if (shots.size() < 2)
        return s;

    double meanSum = 0.0, maxPair = 0.0, maxFrac = 0.0;
    int pairs = 0;
    for (size_t i = 1; i < shots.size(); ++i) {
        if (shots[i].luma.size() != shots[i - 1].luma.size())
            throw std::runtime_error("相邻帧尺寸不同——序列里混进了别的 extent");
        cv::Mat d;
        cv::absdiff(shots[i - 1].luma, shots[i].luma, d);
        // 本 OpenCV 无 cv::sum(src, mask) 重载，用 mask 内均值（分母同为 maskPixels）
        meanSum += cv::mean(d, m)[0];
        maxPair = std::max(maxPair, cv::norm(d, cv::NORM_INF, m));
        cv::Mat changed;
        cv::bitwise_and(anyDiff(shots[i - 1].bgr, shots[i].bgr), m, changed);
        maxFrac = std::max(maxFrac, static_cast<double>(cv::countNonZero(changed))
                / static_cast<double>(s.maskPixels));
        ++pairs;
    }
    s.pairs = pairs;
    s.meanAbsDiff = pairs ? meanSum / pairs : 0.0;
    s.maxAbsDiff = maxPair;
    s.changedFraction = maxFrac;
    return s;
}

int cmdTemporal(double stepPx, int pairs, int warm, const std::string& variant)
{
    // jitter 是 Halton **周期 16 的周期信号**，且周期内的帧间跳变幅度极不均匀
    // （|h(3)-h(4)| = 0.875px，而 |h(8)-h(9)| = 0.5px）。若只采任意 8 个连续帧对，测的
    // 其实是「随机抽到的那个相位窗」——实测同一个量 warm=24 与 warm=64 差 3 倍
    // （S1 0.0317 vs 0.0112），比值自然也不可复现（2.09× vs 0.62×）。
    // ⟹ 采集窗口与预热一律**向上取整到整数个抖动周期**，让每帧对覆盖完整周期。
    constexpr int kJitterPeriod = 16;
    const auto up = [](int v, int p) { return ((v + p - 1) / p) * p; };
    const int alignedPairs = up(pairs, kJitterPeriod);
    const int alignedWarm = up(warm, kJitterPeriod);
    if (alignedPairs != pairs || alignedWarm != warm)
        std::printf("⚠ 窗口对齐抖动周期 %d：pairs %d→%d、warm %d→%d（理由见本函数注释）\n\n",
            kJitterPeriod, pairs, alignedPairs, warm, alignedWarm);
    pairs = alignedPairs;
    warm = alignedWarm;

    const int shots = pairs + 1;
    const std::string root = projectRoot() + "/build";
    std::printf("细线靶时域锚点：%d×%d  yaw %.3f px/帧  预热 %d 帧  帧对 %d  variant=%s\n\n",
        kSize[0], kSize[1], stepPx, warm, pairs, variant.empty() ? "forward" : variant.c_str());

    // ── 采集矩阵（S = 静止机位，M = 微动机位）────────────────────────────────
    // 判据只建立在 **S 组**上。理由（本轮实测换来的，别再走回头路）：
    //   时域滤波能压的是「同一视点、不同采样相位」的估计差；它**压不掉**「视点真的动了」
    //   引起的画面变化（那部分是正确影像，重投影后照样要变）。而 L1 逐帧差对模糊近似
    //   **守恒**——把一次满幅翻转摊成 3 个像素各差一点，均值几乎不变。故匀速扫动下
    //   再正确的 TAA 也测不出抑制（实测 A/F 只有 1.2×，而闭式该是 3.95×——不是引擎的
    //   错，是激励选错了）。
    //   S 组把运动项彻底摘掉：平均视点恒定，唯一变化 = jitter 的采样相位。此时
    //   feedback=1 的消融组（S1）与默认组（S2）**输入统计完全相同**，比值就是时域
    //   累积本身的增益，可直接对拍闭式。
    //     S0  TAA 关 静止            → 引擎确定性地板（硬判据：恰为 0）
    //     S1  TAA 开 feedback=1 静止  → **只抖不滤** = 闪烁本体（硬判据：非零 = 阳性锚点）
    //     S2  TAA 开 feedback=.12 静止 → 滤波后（硬判据：S1/S2 ≥ 3.0，闭式 3.95）
    //     M0  TAA 关 微动            ┐ 端到端参考值，**只报不判**（理由见 antiPatterns
    //     M1  TAA 开 默认   微动      ┘ 的 moving-camera-l1-ratio 条目）
    RunResult S0, S1, S2, M0, M1;
    try {
        S0 = runSequence(root + "/imgqa_taa_s0", false, 0.12, 1.0, variant, stepPx, shots,
            warm, false, "taas0");
        // ⚠ 消融组是 **feedback=1**（输出 = 本帧），不是 0。写成 0 会得到「输出 = 裁剪后的
        // 历史」= 完全丢弃本帧的退化反馈环，它反而**更**依赖重投影，测出来的不是闪烁本体
        // （本轮真实踩过：fb=0 时 S1 会随 prevViewProj 口径变化，而"无滤波"本不该依赖历史）。
        S1 = runSequence(root + "/imgqa_taa_s1", true, 1.0, 1.0, variant, stepPx, shots, warm,
            false, "taas1");
        S2 = runSequence(root + "/imgqa_taa_s2", true, 0.12, 1.0, variant, stepPx, shots,
            warm, false, "taas2");
        M0 = runSequence(root + "/imgqa_taa_m0", false, 0.12, 1.0, variant, stepPx, shots,
            warm, true, "taam0");
        M1 = runSequence(root + "/imgqa_taa_m1", true, 0.12, 1.0, variant, stepPx, shots,
            warm, true, "taam1");
    } catch (const std::exception& e) {
        std::printf("  采集失败: %s\n", e.what());
        return 2;
    }

    const auto row = [](const char* name, const TemporalStat& s) {
        std::printf(
            "  %-30s 帧对 %2d  mask %8lld  均值|Δluma| %-10s 最大 %-8s 变化 %6.2f%%  中间调 %6.2f%%\n",
            name, s.pairs, static_cast<long long>(s.maskPixels), d6(s.meanAbsDiff).c_str(),
            d6(s.maxAbsDiff).c_str(), s.changedFraction * 100.0, s.edgeSoftness * 100.0);
    };
    row("S0 TAA关        静止", S0.stat);
    row("S1 TAA开 fb=1   静止（只抖不滤）", S1.stat);
    row("S2 TAA开 fb=.12 静止（滤波）", S2.stat);
    row("M0 TAA关        微动", M0.stat);
    row("M1 TAA开 默认    微动", M1.stat);

    const auto ratioOf = [](double a, double b) {
        return b > 0.0 ? a / b : std::numeric_limits<double>::infinity();
    };
    const double rSup = ratioOf(S1.stat.meanAbsDiff, S2.stat.meanAbsDiff);
    const double rE2E = ratioOf(M0.stat.meanAbsDiff, M1.stat.meanAbsDiff);
    std::printf("\n  判据  时域累积抑制 S1/S2 = %.2f×（闭式 ≈11×，下限 3.0×）\n", rSup);
    std::printf("  参考  端到端微动 M0/M1 = %.2f×（L1 对模糊守恒 → 只报不判）\n", rE2E);
    std::printf("  参考  空间抗锯齿：中间调 S0 %.2f%% → S2 %.2f%%\n",
        S0.stat.edgeSoftness * 100.0, S2.stat.edgeSoftness * 100.0);

    std::vector<Verdict> vs;
    vs.push_back(verdictFor("determinism", S0.stat.meanAbsDiff == 0.0 && S0.stat.pairs > 0,
        "TAA 关 + 机位静止：逐帧差均值 " + d6(S0.stat.meanAbsDiff) + "（"
            + std::to_string(S0.stat.pairs) + " 帧对）——非零则一切比值是噪声比噪声"));
    vs.push_back(verdictFor("temporal-flicker-anchor",
        S1.stat.pairs > 0 && S1.stat.meanAbsDiff > 0.0 && S1.stat.changedFraction >= 0.005,
        "只抖不滤（feedback=1）：均值|Δluma| = " + d6(S1.stat.meanAbsDiff) + "，变化像素占比 "
            + d2(S1.stat.changedFraction * 100.0) + "%（下限 0.5%）；靶 mask "
            + std::to_string(S1.stat.maskPixels) + "px，id 面 "
            + std::to_string(S1.idPixels) + "px"));
    vs.push_back(verdictFor("temporal-taa-suppression",
        S1.stat.meanAbsDiff > 0.0 && S2.stat.meanAbsDiff > 0.0 && rSup >= 3.0,
        "S1/S2 = " + d2(rSup) + "×（下限 3.0×）；S1 = " + d6(S1.stat.meanAbsDiff)
            + "  S2 = " + d6(S2.stat.meanAbsDiff)));

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("temporal", {{"step_px", std::to_string(stepPx)},
               {"pairs", std::to_string(pairs)}, {"warm", std::to_string(warm)},
               {"variant", variant.empty() ? "forward" : variant},
               {"S0_off_stay", d6(S0.stat.meanAbsDiff)},
               {"S1_fb0_stay", d6(S1.stat.meanAbsDiff)},
               {"S2_fb12_stay", d6(S2.stat.meanAbsDiff)},
               {"M0_off_move", d6(M0.stat.meanAbsDiff)},
               {"M1_on_move", d6(M1.stat.meanAbsDiff)},
               {"S1_over_S2", d2(rSup)}, {"M0_over_M1", d2(rE2E)},
               {"soft_S0", d2(S0.stat.edgeSoftness * 100.0)},
               {"soft_S2", d2(S2.stat.edgeSoftness * 100.0)}},
        vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
