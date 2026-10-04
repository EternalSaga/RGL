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
// ## 三条硬判据（全部静止机位）
//
//   S0 TAA 关 静止             → 引擎确定性地板（≠0 则一切比值是噪声比噪声）
//   S1 TAA 开 feedback=1 静止   → 只抖不滤 = 闪烁本体（**阳性锚点**：必须非零）
//   S2 TAA 开 feedback=.12 静止 → 滤波后（S1/S2 = 时域累积抑制，对拍闭式）
//
// 采集一律走 `/shotburst`（连续 N 帧，引擎在帧首结算钩子里自己登记下一张）。用 `/shot`
// 逐张拍**测不出东西**：每张落在哪一帧取决于 HTTP 请求与帧首钩子的先后（= 墙钟），
// 相邻两张根本不是相邻两帧——实测同一个量随预热长度差 3 倍（docs/taa_plan.md §7 第 8 条）。
//
// 区域来自 objectID（本工具地基纪律：判据的区域来自几何，不来自图像）；统计一律在
// sRGB 解码后的线性 luma 域做。
//
// 为什么**没有**「微动机位」组：逐帧改机位唯一的配置驱动来源是 SceneSpin，而它是
// `angle += speed * dt`（按帧时长累积）⇒ 每帧角度本身随帧时长漂，测不出可复现的数。
// 要做微动激励得先加一条「按帧索引驱动」的相机运动（帧号而非 dt 的函数），那是新引擎
// 面，而这条判据已被登记为反模式（`moving-camera-l1-ratio`）——不值得。
// 机位若将来要动，正确形态 = 绕 up 轴**定角** yaw：像素位移与深度无关，且是刚体相机
// 运动，`prevViewProj` 重投影能精确对齐（平移 dolly 会让 9m 外栅栏的单帧位移只剩近处
// 的 22%，信号随深度衰减）。
#include "imgqa.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
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
    const std::string& variant, int shots, int warm, const std::string& tag, bool baseSpace)
{
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
    o.taaBaseSpace = baseSpace;  // 批 13.2 基空间输出（false = 现行为 = 输出在抖动空间）
    o.variant = variant;
    o.size = {kSize[0], kSize[1]};
    makeProfile(dir, o);

    Engine eng(dir);
    RunResult r;
    eng.setCamera(yawCam(0.0));  // S 组：机位一次钉死，全程不动（唯一变化 = jitter 相位）

    // 预热：一次突发走完 warm 帧（图不要，只为让 TAA 历史收敛）。用突发而不是 warm 次
    // /shot 的理由不是省往返——是「到底走了几帧」从此确定，不再看墙钟脸色。
    // 路径刻意丢弃：这里要的是「帧真的走了」，不是产物（异常仍会抛出，不会静默少走）。
    std::ignore = eng.shotBurst("screenshots", static_cast<uint32_t>(warm), tag + "_w");

    // 被测量：**连续 shots 帧**。必须走 /shotburst——/shot 相邻两张不是相邻两帧
    // （每张落在哪帧取决于 HTTP 请求与帧首钩子先后），逐帧差会退化成随机相位窗。
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(shots), tag + "_c");
    std::vector<Frame> seq;
    seq.reserve(files.size());
    for (const auto& f : files) {
        Frame fr;
        fr.camera = yawCam(0.0);
        fr.bgr = readPngBgr(f);
        prepareLinear(fr);
        seq.push_back(std::move(fr));
    }

    // object_id 单张即可（区域不需要连续性）；机位与序列同一，故 mask 与序列同帧形态。
    const cv::Mat ids = decodeObjectId(readPngBgr(eng.shot(tag + "_id", "object_id")));
    r.idPixels = cv::countNonZero(ids);
    cv::Mat interior;
    cv::compare(ids, 0, interior, cv::CMP_NE);  // ids 是 CV_32SC1 ⟹ 直接 8UC1
    const cv::Mat m = dilateMask(interior, 3);  // 3px：吸收 jitter ±0.5px 的相位漂移
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

int cmdTemporal(int pairs, int warm, const std::string& variant, bool baseSpace)
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
    std::printf("细线靶时域锚点：%d×%d  静止机位  预热 %d 帧  帧对 %d  variant=%s  "
        "base_space=%s\n\n",
        kSize[0], kSize[1], warm, pairs, variant.empty() ? "forward" : variant.c_str(),
        baseSpace ? "开（输出在基空间）" : "关（输出在抖动空间）");

    // ── 采集矩阵（S = 静止机位；三组全是静止机位）────────────────────────────
    // 判据只建立在静止机位上。理由（本轮实测换来的，别再走回头路）：
    //   时域滤波能压的是「同一视点、不同采样相位」的估计差；它**压不掉**「视点真的动了」
    //   引起的画面变化（那部分是正确影像，重投影后照样要变）。而 L1 逐帧差对模糊近似
    //   **守恒**——把一次满幅翻转摊成 3 个像素各差一点，均值几乎不变。故匀速扫动下
    //   再正确的 TAA 也测不出抑制（实测只有 1.2×，而闭式该是 3.95×——不是引擎的错，
    //   是激励选错了；已登记反模式 `moving-camera-l1-ratio`）。
    //   静止机位把运动项彻底摘掉：平均视点恒定，唯一变化 = jitter 的采样相位。此时
    //   feedback=1 的消融组（S1）与默认组（S2）**输入统计完全相同**，比值就是时域
    //   累积本身的增益，可直接对拍闭式。
    //     S0  TAA 关 静止             → 引擎确定性地板（硬判据：恰为 0）
    //     S1  TAA 开 feedback=1 静止   → **只抖不滤** = 闪烁本体（硬判据：非零 = 阳性锚点）
    //     S2  TAA 开 feedback=.12 静止  → 滤波后（硬判据：S1/S2 ≥ 3.0，闭式 ≈11×）
    //   ⚠ 没有「微动机位」组：逐帧改机位唯一的配置驱动来源是 SceneSpin，而它是
    //   `angle += speed * dt`（按帧时长累积）⇒ 每帧角度本身就不确定，测不出可复现的数。
    RunResult S0, S1, S2;
    try {
        S0 = runSequence(root + "/imgqa_taa_s0", false, 0.12, 1.0, variant, shots, warm,
            "taas0", false);  // TAA 关 ⟹ base_space 无消费者，恒按门关采
        // ⚠ 消融组是 **feedback=1**（输出 = 本帧），不是 0。写成 0 会得到「输出 = 裁剪后的
        // 历史」= 完全丢弃本帧的退化反馈环，它反而**更**依赖重投影，测出来的不是闪烁本体
        // （本轮真实踩过：fb=0 时 S1 会随 prevViewProj 口径变化，而"无滤波"本不该依赖历史）。
        S1 = runSequence(root + "/imgqa_taa_s1", true, 1.0, 1.0, variant, shots, warm,
            "taas1", baseSpace);
        S2 = runSequence(root + "/imgqa_taa_s2", true, 0.12, 1.0, variant, shots, warm,
            "taas2", baseSpace);
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

    const auto ratioOf = [](double a, double b) {
        return b > 0.0 ? a / b : std::numeric_limits<double>::infinity();
    };
    const double rSup = ratioOf(S1.stat.meanAbsDiff, S2.stat.meanAbsDiff);
    // S1/S2 **只报不判**（已登记反模式 `screen-space-l1-frame-diff`）：屏幕空间 L1 逐帧差
    // 量不出抑制比——边缘摊开后「单像素幅度降」被「变化像素数涨」抵消，乘积近似守恒。
    // 下面把这两个分量打出来，是为了让**指纹**可见（比值 ≈ 1 时能一眼看出是守恒而不是没干活）。
    const auto amp = [](const TemporalStat& s) {
        return s.changedFraction > 0.0 ? s.meanAbsDiff / s.changedFraction : 0.0;
    };
    const double abs1 = amp(S1.stat), a2 = amp(S2.stat);
    std::printf("\n  参考  抑制比 S1/S2 = %.2f×（**不判**，理由见反模式 screen-space-l1-frame-diff）\n",
        rSup);
    std::printf("        L1 守恒指纹：单像素幅度 %.3f → %.3f（降 %.2f×）被变化像素 %.2f%% → "
        "%.2f%%（涨 %.2f×）抵消\n",
        abs1, a2, a2 > 0.0 ? abs1 / a2 : 0.0, S1.stat.changedFraction * 100.0,
        S2.stat.changedFraction * 100.0,
        S1.stat.changedFraction > 0.0 ? S2.stat.changedFraction / S1.stat.changedFraction : 0.0);
    std::printf("  判据  空间抗锯齿：中间调 S0 %.2f%% → S2 %.2f%%\n",
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
    // 硬判据换成「空间抗锯齿在场」：TAA 关时靶**恰好二值**（实测全图只有 3 个亮度级
    // = 无 MSAA 的光栅化是二值覆盖），TAA 开则必须出现中间调。两条互为阴阳：
    // S0 == 0 是反向对照（没有它，「中间调变多」可能只是靶本来就糊），
    // S2 > 0 是阳性锚点（没有它，「S0 为 0」可能只是靶根本没画）。
    vs.push_back(verdictFor("temporal-aa-presence",
        S0.stat.pairs > 0 && S0.stat.edgeSoftness == 0.0 && S2.stat.edgeSoftness >= 0.01,
        "中间调占比：TAA 关 = " + d6(S0.stat.edgeSoftness * 100.0) + "%（须恰为 0）  TAA 开 = "
            + d2(S2.stat.edgeSoftness * 100.0) + "%（下限 1.0%）"));
    // 抑制比——**换到正确的观测空间后重新立起来**（原 L1 总量版仍是反模式，别复活）。
    // 闭式：16 相位抖动 = 对同一视点的 16 个独立采样相位做平均 ⟹ 逐样本估计噪声按 √N = 4.0
    // 下降。这条预测的适用域是**逐样本幅度**，不是 L1 总量（总量对模糊守恒 ⟹ 只给 1.38×，
    // 这正是它当初被推翻的原因）。实测 3.99×，与 √16 差 0.3%。
    const double sup = a2 > 0.0 ? abs1 / a2 : 0.0;
    vs.push_back(verdictFor("temporal-suppression-sqrtN",
        S1.stat.pairs > 0 && S2.stat.pairs > 0 && sup >= 3.0,
        "变化像素上的平均幅度：只抖不滤 " + d6(abs1) + " → 滤波 " + d6(a2) + " = 降 " + d2(sup)
            + "×（闭式 √16 = 4.00×，下限 3.0×）"));

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("temporal", {{"pairs", std::to_string(pairs)}, {"warm", std::to_string(warm)},
               {"variant", variant.empty() ? "forward" : variant},
               {"S0_off_stay", d6(S0.stat.meanAbsDiff)},
               {"S1_fb1_stay", d6(S1.stat.meanAbsDiff)},
               {"S2_fb12_stay", d6(S2.stat.meanAbsDiff)},
               {"S1_over_S2", d2(rSup)},
               {"soft_S0", d2(S0.stat.edgeSoftness * 100.0)},
               {"soft_S2", d2(S2.stat.edgeSoftness * 100.0)}},
        vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    return ok ? 0 : 1;
}


// ═══════════════════════════════════════════════════════════════ taa-swim（批 13.1b）
//
// **要回答的问题**（phase13 §1.6 末留的未定案）：真实场景上 TAA 开 = 静止机位逐帧差
// 2.36~4.0/255（关 = 恰为 0）。§1.6 推了一个具体机制：抖动空间累积 ⟹
// `out_t(uv) = F(uv − jitter_t)`，也就是**整幅图随抖动相位每帧平移 ≤1px**。
// 上一版打算拿「软件 EMA 参考」判它，已撤回（参考在未抖基空间、引擎在抖动空间，不是同一个
// 量；§1.6 第 2 条）。
//
// ## 两版废掉的量具（写下来免得第三次重新发明）
//
// ① **自由拟合相邻两帧的全局位移**：L1 对自然图像的极小值很浅，拟合位移逐帧乱跳（对 Halton
//    预测 RMS 0.33px），且没有标定——「残差降到 57%」这句话里没有一把尺。
// ② **拿未抖基图按 Halton 搬过去对**（零自由参数：16 相位 × 2 符号，整段连拍共用一个相位₀）：
//    阳性锚点组 T1（`feedback=1`，输出 = 本帧）**按构造**就该命中，实测搬基图反而**更差**
//    （110.9%，逐帧改善仅 39%）。**量具在阳性锚点面前当场阵亡。** 根因不在实现而在口径：
//    抖动帧是**按亚像素偏移重新光栅化**出来的，边缘二值覆盖翻转的幅度压过了「把基图插值搬
//    移」能表达的量——插值给出的是中间灰，真图给出的是翻转后的极值。
//    ⟹ 教训：**参考必须与被测项在同一坐标系**。这条本项目已栽过三次（`taa_plan` §7 第 10、
//      12 条 + phase13 §1.6 第 2 条），这是第四次。**阳性锚点当场把量具打死，正是它的价值。**
//
// ## 现量具：同一段连拍内部按 lag 对比（不引任何外部参考）
//
// 稳态下 out_i(uv) = F(uv − j_i)，于是
//     frame_{i+k}(uv) = frame_i(uv − (j_{i+k} − j_i))
// 两帧之间是**精确平移**，而累积糊度 F 在两帧里同时出现 ⟹ **糊度不进残差**，覆盖翻转也被
// 一起搬走。预测位移 (j_{i+k} − j_i) 是 Halton 闭式，**零自由参数**。
//
// 三条自身有效性保障：
//   · **免费标定**：lag = 16（一个完整抖动周期）时预测位移恰为 0 ⟹ 实测差必须恰为 0。
//     首轮实测 T1/T2 的 lag-16 差**都恰为 0** ⟹ 连拍确实是相位 16 的确定性函数。
//   · **反向对照**：位移换成「相位整体错半个周期」的（量级相同、方向系统性错开），残差必须
//     明显升回去；搬对搬错一样低 = 算子没有区分力（box9 消融那次的教训：先确认被动的会动）。
//   · **符号不猜**：±两个都试并回显命中哪个（引擎 y 口径若与本工具相反会在这里暴露）。
//
// 五组采集（全部真实 bistro：env/skybox/IBL/AO/shadow 全开；静止机位）：
//   T0  TAA 关                       → 确定性地板（逐帧差须恰为 0）+ 提供「离基图多远」的 R
//   T1  TAA 开 fb=1（只抖不滤）      → 输出 = 本帧（无累积、无糊）= 游动量的**下界参照**
//   T2  TAA 开 fb=.12（现行）        → 被测量
//   T3  TAA 开 fb=.12 **jitter_scale=0** → **恒等式组**：零抖动 + 静止机位 ⟹ prevUv ≡ uv
//     ⟹ 输出必须与 T0 相同（判据 = 均值 ≤0.05 且逐像素 max ≤2 LSB；max 才是关键，理由见
//     judge.cpp `temporal-identity`）。这条把「抖动固有代价」整个摘掉，只剩实现本身
//     （phase13 §1.8 就是它抓出来的：CR 核写成带符号 a³ ⟹ 直流增益 36× ⟹ 历史被拒）
//   T4  TAA 开 **fb=1** jitter_scale=8 → **本帧档 + ±4px 大抖动**：着色器尾部是
//     `lerp(clipped, cur, params.x)`，fb 是**本帧权重**（Filament 序）⟹ fb=1 让输出 = 本帧光栅，
//     画面按构造 = 基图平移 8·jitter_t。用途 = ① 沿预测位移做 λ 一维扫描**标定引擎抖动**
//     （argmin λ 须 = +1）；② 给游动算子定力气（±0.5px 档首轮实测它没力气）。
//     ⚠ fb 的口径**别凭记忆写**：fb=0 是「输出 = 纯历史」的退化环，画面逐帧仍在变，
//     本命令的 lag-16 免费标定（须恰为 0）会立刻把它抓出来（实测 0.068 ≠ 0）。
//
// 差异一律在**观测域字节**上算（0..255，与 §1.5 表里的 2.36/255 同单位可直接对历史）。
// 理由：这条量的是「肉眼看不看得出抖」，感知域 = sRGB；线性 luma 会把暗部抖动压成噪声
// （第一版就是在线性 luma 上算的，逐帧差只有 0.0015，与实机"地震"完全不成比例）。
// 梯度能量仍用线性 luma（与 §1.5 的 0.02498 同口径）。
//
// ⚠ 「离未抖基图多远」也打印，但它**不是**平移判据（口径缺陷见上面被废的第 ② 版），只当
//   画质量读：它回答「输出偏离正确画面多少」，不回答「偏离是不是平移」。
// ⚠ T2 的游动判据阈值**等首轮数出来再立**（AGENTS：先测后定 + 写理由）。

namespace {

/// 抖动周期（帧）——与 `rgl/taa_jitter.cppm::kTaaJitterPeriod` 同值（重抄理由见 jitterPredPx）。
constexpr uint32_t kTaaJitterPeriod = 16u;

/// Halton(2,3) 抖动（像素单位，值域 ±0.5）——与 `rgl/taa_jitter.cppm::taaJitterPixels`
/// **同式重抄**：imgqa 不链 rgl 库（模块 GMF 常量不导出），先例 = `aa_coverage.h` 的杆表。
/// ⚠ 漂移风险两处把守：`tests/test_taa_jitter.cppm` 钉引擎侧闭式锚点，本命令 T1 组钉
///   「按构造必须命中预测」（引擎改了公式而探针没改 ⟹ T1 立刻红）。
std::array<double, 2> jitterPredPx(uint32_t frameIndex)
{
    const auto halton = [](uint32_t idx, uint32_t base) {
        double r = 0.0, f = 1.0 / static_cast<double>(base);
        for (uint32_t i = idx; i > 0u; i /= base) {
            r += f * static_cast<double>(i % base);
            f /= static_cast<double>(base);
        }
        return r;
    };
    const uint32_t i = frameIndex % kTaaJitterPeriod + 1u;
    return {halton(i, 2u) - 0.5, halton(i, 3u) - 0.5};
}

/// 抖动引起的位移预测（像素，含尺度）——**口径 = `freeShift` 返回的 d = 图像内容位移的相反数**。
///
/// ⚠ 2026-10-04 更正口径（此前本注释自称「图像位移」，方向写反了；数值读数全部不受影响，
/// 因为匹配器拿本函数与 `freeShift` 的 d 在**同一口径**内比较，且模长与符号无关）。逐轴推导
/// （与 `rgl/render_list_impl.cppm::taaResolveOffset` 同式，那边的裁决更强 = 基空间输出实测）：
///   · `applyProjectionJitter` 做 `proj[2][0] -= 2jx/W`（glm 列主序 ⟹ 这是 **clip.x 的 v.z
///     系数**，不是 w 行！），`clip.w = −v.z` 带来一次翻号 ⟹ `Δndc = (+2jx/W, +2jy/H)`；
///   · x：列 = (ndc.x+1)/2·W 不翻 ⟹ **列位移 = +jx**；
///   · y：本引擎用**负高视口**（ndc.y=+1 落首行，行 = (1−ndc.y)/2·H 再翻一次）⟹ **行位移 = −jy**；
///   · ⟹ **图像内容位移 c_t = scale·(+jx, −jy)**（`render_list_impl.cppm` 写进 resolve 偏移
///     的就是它，门开实测游动 0.643→0.060px 归零 = 最终裁决；符号若反游动会**翻倍**到 ~1.33px）。
///   · 而 `freeShift` 的 d 语义是 `frame(p) = ref(p + d)`（warpAffine 方向），= **−c_t**
///     ⟹ 本函数返回 `scale·(−jx, +jy)`，与 d 同口径直接匹配。
/// 标定本身是**量出来的**（2026-10-03，绝对口径 + 合成锚点）：`jitter_scale=8` 时逐帧相位相关
/// 实测位移模长均值 2.917px vs 预测 2.956px（差 1.3%），峰值 0.988。**在此之前，本命令所有
/// 「按预测位移搬」的差分读数都在搬错方向**（y 轴系统性反号），差分残差从 100% 只降到 68%，
/// 看起来像「画面没被搬起来」——口径错配不会报错，只会让读数变得似是而非。
std::array<double, 2> jitterDeltaPx(uint32_t frameIndex, double scale)
{
    const auto j = jitterPredPx(frameIndex);
    return { -j[0] * scale, j[1] * scale };
}

/// 自由拟合 `frame ≈ shift(ref, d)` 的二维位移，用 **`cv::phaseCorrelate`**（相位相关）。
///
/// 为什么不用手写网格搜索（第一版就是手写的，且**尺度写错**：把 1/4 降采样的步长当成全分辨率
/// 像素去 warp，于是搜索域只有 ±2.25px 却报成 ±9px，输出「拟合位移 7.9px、拟合后残差反而比
/// 不拟合更大」这种自相矛盾的假数——**辅助函数自己要有独立判据**，这里 = `resid ≤ zero` 是
/// 按构造必须成立的性质，它一破就说明拟合器坏了，而当时没人看它）。相位相关一次 FFT 出解，
/// 没有可写错的搜索循环，且自带亚像素峰值。
/// ⚠ 相位相关对**纯平移**最灵；本用例（抖动帧 ↔ 零抖动参考）按构造就是纯平移，正好对口。
/// 返回的 `resid/zero` 就是它自己的有效性读数：`resid ≪ zero` 才说明平移模型站得住。
struct FreeShift {
    double dx = 0.0, dy = 0.0;  ///< 拟合位移（px）：frame(p) ≈ ref(p + d)，= 内容位移的相反数
    double resid = 0.0;         ///< 拟合位移处的残差（0..255）
    double zero = 0.0;          ///< 不搬的残差（= 两图原始差）
    double peak = 0.0;          ///< 相位相关峰值（0..1，平移越纯越接近 1）
};

FreeShift freeShift(const cv::Mat& ref, const cv::Mat& frame)
{
    cv::Mat a, b, win;
    cv::cvtColor(ref, a, cv::COLOR_BGR2GRAY);
    cv::cvtColor(frame, b, cv::COLOR_BGR2GRAY);
    a.convertTo(a, CV_32F);
    b.convertTo(b, CV_32F);
    cv::subtract(a, cv::mean(a), a);   // 去直流：否则峰值被平均亮度主导
    cv::subtract(b, cv::mean(b), b);
    cv::createHanningWindow(win, ref.size(), CV_32F);
    double resp = 0.0;
    const cv::Point2d sh = cv::phaseCorrelate(a, b, win, &resp);
    // 相位相关返回 [0,W) 卷绕位移，折回带符号主值
    const double sx = sh.x > ref.cols * 0.5 ? sh.x - ref.cols : sh.x;
    const double sy = sh.y > ref.rows * 0.5 ? sh.y - ref.rows : sh.y;

    // 符号口径**不猜**：`residAt` 按构造定义 d 的语义 = `frame(p) ≈ ref(p + d)`（warpAffine
    // 的 dst(p) = src(p − t)，这里 t = −d）⟹ **d = 内容位移的相反数**（旧注释写的
    // `frame(p) = ref(p − d)` 与代码相反，2026-10-04 更正，理由见 jitterDeltaPx 头注释）。
    // phaseCorrelate 的返回约定可能与它相反，而约定错一次就毁掉整组读数，所以**两个符号都算
    // 残差取小**——把「谁的约定对」从注释问题变成数据问题（锚点 `temporal-fitter-anchor` 再兜一层）。
    const auto residAt = [&ref, &frame](double dx, double dy) {
        cv::Mat w, d;
        cv::warpAffine(ref, w, cv::Matx23d(1, 0, -dx, 0, 1, -dy), ref.size(), cv::INTER_CUBIC,
            cv::BORDER_REPLICATE);
        cv::absdiff(w, frame, d);
        return cv::mean(d)(0);
    };
    FreeShift fs;
    fs.peak = resp;
    const double rp = residAt(-sx, -sy), rn = residAt(sx, sy);
    const bool pos = rp <= rn;
    fs.dx = pos ? -sx : sx;
    fs.dy = pos ? -sy : sy;
    fs.resid = pos ? rp : rn;
    cv::Mat d0;
    cv::absdiff(ref, frame, d0);
    fs.zero = cv::mean(d0)(0);
    return fs;
}


/// 把 BGR 字节图搬 (dx,dy) 像素（正 = 内容向右下移）。三次插值：亚像素搬图里它最接近
/// 「重新光栅化」，双线性会额外糊掉高频、把残差地板抬高（T1/T2 同地板，比值仍可比）。
void shiftInto(const cv::Mat& src, cv::Mat& dst, double dx, double dy)
{
    // dst(p) = src(p + t)，要 dst(p) = src(p − d) ⟹ t = −d
    const cv::Matx23d M(1.0, 0.0, -dx, 0.0, 1.0, -dy);
    cv::warpAffine(src, dst, M, src.size(), cv::INTER_CUBIC, cv::BORDER_REPLICATE);
}

/// 一次真实场景连拍。与合成靶的 runSequence 分开：这里**要**真实 bistro（§1.5 的教训 =
/// 判据全跑在 env 关闭的合成靶上，真实场景外推失败）。
///
/// ⚠ **配置与机位（批 13.1e 改）**：基底 = **交付配置 `configs/*.lua` 原样** + **用户机位**。
/// 之前跑的是合成 profile（forward + 无 izb + 默认机位），于是 `temporal-identity` 在
/// 那里绿（残差 0.016/255）而在交付配置 + 用户机位上红（mad 1.688、max 37、10.37% >2LSB）
/// ——**判据量在别的场景上得出的绿不是绿**。交付配置是 deferred + scale 0.01 + izb_shadow
/// + env/skybox/AO/shadow 全开，与合成 profile 不是同一个场景，别再拿两边的数互相解释。
struct SwimRun {
    std::vector<cv::Mat> bytes;  ///< 各帧观测字节（中央裁块，8UC3）
    double consecDiff = 0.0;     ///< 相邻帧 |Δ| 均值（0..255；§1.5 表同单位）
    double lagDiff = -1.0;       ///< 同相位（相隔一个抖动周期整数倍）|Δ| 均值；<0 = 序列太短
    double gradEnergy = 0.0;     ///< 线性 luma 梯度能量（§1.5 的 0.02498 同口径）
};

/// **用户机位**（报障机位；`mode = fps`，position/target/fov 与任务书逐字一致）。
/// 判据跑在报障的那台机位上，否则「绿」只覆盖工具自己挑的机位。
const Engine::Camera kUserCamera{
    {1.1, 0.75, 1.05}, {0.196717, 0.034493, 0.024559}, 45.0};

/**
 * 「残差 ≥80% 落在边缘上」这条**参审的最小像素数**（2026-10-04 定）。
 *
 * 边缘归属是比例判据，样本少的时候比例就是噪声：修复后 dev≥2 只剩 30 像素（0.0033%），
 * 此时要求「≥80% 在边缘」等于在噪声里追一个不存在的模式（实测 56.7%，而这些像素的梯度
 * 中位数已是全图的 2.7 倍）。缺陷的指纹是「**大量**残差不在边缘」（修复前：10.37% 像素
 * dev≥2、集中在白色屋顶 = 平坦高亮区，正是该条该抓的），所以给它一个样本量下限：
 * 够多才参审，不够多由「dev≥2 占比 ≤0.01%」那条绝对量兜住。
 */
constexpr int kEdgeAttributionFloor = 1000;

/**
 * 连拍分析窗口 = **整帧**（2026-10-04 随「交付配置 + 用户机位」一起改）。
 *
 * 旧窗口是 640×640@(120,120) 的中央裁块，配的是 800×800 合成档。换到交付配置 1280×720 后
 * 它只占 **40%** 画面，而恒等式残差是**边缘局域**的（修复后 dev≥2 的像素 96.6% 在边缘），
 * 中央恰好是建筑簇最密的地方 ⟹ 同一个稳态读数在裁块里是 0.132、在全帧是 0.0226，**差 5.6 倍**。
 * 恒等式的容差（均值 ≤0.05）当初是按全帧定的，判据是逐像素等式、没有任何理由只看 15% 画面，
 * 所以改成整帧：读数不再随「窗口落在画面哪一块」漂移。
 */
cv::Rect swimRoi(const cv::Size& sz) {
    return cv::Rect{0, 0, sz.width, sz.height};
}

/**
 * 机位**同时**给两处：profile 的 camera.lua（`ProfileOpts::hasCamera`，在 makeProfile 里
 * 落盘，管第 1 帧——RPC 落在第 2~3 帧，之前那几帧已经进了 TAA 历史，理由全文见
 * `ProfileOpts::hasCamera` 注释）与 `POST /camera`（管 RPC 口径本身也被覆盖到）。
 * 两者机位一致时第二条是幂等的。
 */

SwimRun runSwimSequence(const std::string& dir, bool taa, double feedback, double jitterScale,
    const std::string& variant, int shots, int warm, const std::string& tag, int lag,
    bool baseSpace = false)
{
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 交付配置原样（见 SwimRun 头注释）
    o.taa = taa;
    o.taaFeedback = feedback;
    o.taaJitterScale = jitterScale;
    o.taaBaseSpace = baseSpace;  // 批 13.2 基空间输出（false = 现行为）
    o.variant = variant;  // 空 = 尊重交付配置自己写的路线（deferred）
    // 机位从**第 1 帧**就对上（只发 RPC 会让默认机位的那几帧渗进历史，0.88/帧洗不掉；
    // 实测恒等式残差 0.100 vs 0.0226）——见 ProfileOpts::hasCamera
    o.hasCamera = true;
    o.cameraPosition = kUserCamera.position;
    o.cameraTarget = kUserCamera.target;
    o.cameraFov = kUserCamera.fovDegrees;
    makeProfile(dir, o);

    Engine eng(dir);
    eng.setCamera(kUserCamera);
    std::ignore = eng.shotBurst("screenshots", static_cast<uint32_t>(warm), tag + "_w");
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(shots), tag + "_c");

    SwimRun r;
    r.bytes.reserve(files.size());
    double grad = 0.0;
    cv::Rect roi;
    for (const auto& f : files) {
        const cv::Mat bgr = readPngBgr(f);
        if (r.bytes.empty())
            roi = swimRoi(bgr.size());
        r.bytes.push_back(bgr(roi).clone());
        Frame fr;
        fr.bgr = bgr;
        prepareLinear(fr);
        cv::Mat gx, gy, g;
        cv::Sobel(fr.luma, gx, CV_64F, 1, 0, 1);
        cv::Sobel(fr.luma, gy, CV_64F, 0, 1, 1);
        g = cv::abs(gx) + cv::abs(gy);  // MatExpr 必须先落地成 Mat 才是 InputArray
        grad += cv::mean(g)(0);
    }
    if (r.bytes.size() < 2)
        throw std::runtime_error("连拍不足 2 帧，逐帧差无定义");
    r.gradEnergy = grad / static_cast<double>(r.bytes.size());
    for (size_t i = 1; i < r.bytes.size(); ++i) {
        cv::Mat d;
        cv::absdiff(r.bytes[i - 1], r.bytes[i], d);
        r.consecDiff += cv::mean(d)(0);
    }
    r.consecDiff /= static_cast<double>(r.bytes.size() - 1);
    if (static_cast<int>(r.bytes.size()) > lag) {
        double s = 0.0;
        int n = 0;
        for (size_t i = static_cast<size_t>(lag); i < r.bytes.size(); ++i) {
            cv::Mat d;
            cv::absdiff(r.bytes[i - lag], r.bytes[i], d);
            s += cv::mean(d)(0);
            ++n;
        }
        r.lagDiff = n ? s / n : -1.0;
    }
    return r;
}

/// 逐 lag 的**平移假设**检验（本量具的核心）。
///
/// 稳态推导：收敛后 out_i(uv) = F(uv − j_i)（F = 基空间累积图，j = 本帧抖动）
/// ⟹ frame_{i+k}(uv) = frame_i(uv − (j_{i+k} − j_i))，即**同一段连拍里的两帧之间是精确平移**。
/// 关键在于 F 在两帧里**同时出现** ⟹ 累积糊度不参与残差，二值覆盖翻转也被一起搬走。
///
/// ⚠ 为什么不能拿「未抖基图」当参考去搬（第一版就是这么瞎的）：基图是**重新光栅化**出来的，
/// 不是基空间图插值出来的——亚像素偏移下边缘的二值覆盖翻转与三次插值给出的中间值根本不是一
/// 回事，这一项压过平移项，实测 T1 组（按构造该命中）搬基图反而**更差** 110.9%。lag 对比把
/// 这一项与糊度一起消掉，是这里唯一量得出「有没有整幅平移」的口径。
struct LagStat {
    int lag = 0;
    double pred = 0.0;    ///< 预测位移模长均值 |j_{i+k} − j_i|（px，Halton 闭式）
    double meas = 0.0;    ///< 实测 |frame_i − frame_{i+k}|（0..255）
    double resid = 0.0;   ///< 把 frame_i 按**预测位移**搬过去后与 frame_{i+k} 的差
    double wrong = 0.0;   ///< 同上但搬**错相位**的位移（相位整体错 8 = 反号）= 反向对照
    int sign = 1;         ///< 命中的符号（−1 = 引擎 y 口径与本工具相反）
};

/// 两帧平均绝对差（观测字节，0..255）。
double meanAbs(const cv::Mat& a, const cv::Mat& b)
{
    cv::Mat d;
    cv::absdiff(a, b, d);
    return cv::mean(d)(0);
}

LagStat lagTranslation(const std::vector<cv::Mat>& frames, int lag, double jitterScale,
    uint32_t phase)
{
    LagStat s;
    s.lag = lag;
    if (static_cast<int>(frames.size()) <= lag)
        return s;
    double predSum = 0.0, measSum = 0.0, rPos = 0.0, rNeg = 0.0, wrongSum = 0.0;
    int n = 0;
    const auto scaled = [jitterScale, phase](uint32_t i) {
        const auto j = jitterDeltaPx(i + phase, jitterScale);
        return j;
    };
    for (size_t i = 0; i + static_cast<size_t>(lag) < frames.size(); ++i) {
        const auto j0 = scaled(static_cast<uint32_t>(i));
        const auto j1 = scaled(static_cast<uint32_t>(i) + static_cast<uint32_t>(lag));
        const double dx = j1[0] - j0[0], dy = j1[1] - j0[1];
        predSum += std::hypot(dx, dy);
        measSum += meanAbs(frames[i], frames[i + static_cast<size_t>(lag)]);
        cv::Mat a, b;
        shiftInto(frames[i], a, dx, dy);
        shiftInto(frames[i], b, -dx, -dy);
        rPos += meanAbs(a, frames[i + static_cast<size_t>(lag)]);
        rNeg += meanAbs(b, frames[i + static_cast<size_t>(lag)]);
        // 反向对照：位移取「相位整体错半个周期」那一个（j_{i+k+8} − j_i）。Halton 的 8 相位
        // 偏移把每个分量映到 (x → ±x 的另一支)，量级相同而方向系统性错开——若搬对与搬错
        // 的残差一样，说明这个算子根本没有区分力（box9 消融那次的教训：先确认量会动）。
        const auto j1w = scaled(static_cast<uint32_t>(i) + static_cast<uint32_t>(lag) + 8u);
        cv::Mat c;
        shiftInto(frames[i], c, j1w[0] - j0[0], j1w[1] - j0[1]);
        wrongSum += meanAbs(c, frames[i + static_cast<size_t>(lag)]);
        ++n;
    }
    if (n == 0)
        return s;
    s.pred = predSum / n;
    s.meas = measSum / n;
    s.wrong = wrongSum / n;
    if (rPos <= rNeg) {
        s.resid = rPos / n;
        s.sign = 1;
    }
    else {
        s.resid = rNeg / n;
        s.sign = -1;
    }
    return s;
}

/// 与基图的**逐像素最大**偏离（所有帧取最大，观测字节 0..255）。
/// 恒等式判据必须有它：均值 0.016 既可能是「大量像素各差 1 LSB」（= 浮点结合序，无害），
/// 也可能是「个别像素差 60 LSB」（= 实现缺陷）。只有 max 能区分这两者。
double maxDistTo(const cv::Mat& ref, const std::vector<cv::Mat>& frames)
{
    double mx = 0.0;
    for (const auto& f : frames) {
        cv::Mat d;
        cv::absdiff(f, ref, d);
        mx = std::max(mx, cv::norm(d, cv::NORM_INF));
    }
    return mx;
}

/// 取连拍的**收敛段**（末尾 max(4, N/4) 帧）。
///
/// 恒等式是**不动点**命题（零抖动 + 静止 ⟹ 收敛后输出必须逐字节 = 本帧），只在历史瞬态过去
/// 之后才可测。连拍前段带着历史瞬态——实测（交付配置 + 报障机位，预热 32 + 连拍 32）逐帧
/// 对基图 max = 5,5,4,4,…,3,…,7,4,3,3,3,3，dev≥2 占比 0.236% → **0.0033%**，衰减率 ≈0.88/帧
/// = (1−feedback)。**这是收敛速度，不是实现缺陷**；把它算进恒等式判据，等于让容差去吸收一个
/// 与「TAA 算得对不对」无关的量（而上一轮就是靠这种容差把 max 从 2 放到 8 的）。
std::vector<cv::Mat> convergedTail(const std::vector<cv::Mat>& frames)
{
    const size_t n = std::max<size_t>(4, frames.size() / 4);
    return {frames.end() - static_cast<long>(n), frames.end()};
}

/// **相位标定**：采集第 0 帧落在引擎抖动序列的哪个相位，是一个未知常数（引擎自己的
/// `frameIndex` 从 0 起算，但预热长度 + RPC 登记时序让它与采集下标不同源；引擎还额外
/// `+1` 跳过全零点）。⚠ `lag = 16 同相位差恰为 0` 那条免费标定**只能验周期，验不出常数
/// 相位偏移**——本轮就是栽在这里：按 λ=1 搬不动，第一反应是「画面没被搬起来」，
/// 实际可能只是**每对帧的预测位移整体错位了一个常数相位**。
/// 于是对 16 个候选相位各算一次「按预测位移搬完之后」的残差（±符号都取小），真相位必须
/// 显著塌下去。这是**零自由参数**的（16 个离散候选，无拟合）。
struct PhaseScan {
    std::vector<double> resid;  ///< resid[φ] = 平均残差（0..255）
    int best = 0;               ///< 残差最小的相位
    double bestVal = 0.0;
    double zero = 0.0;          ///< 「不搬」的基线（= 原始逐帧差）
};

PhaseScan phaseScan(const std::vector<cv::Mat>& frames, double jitterScale)
{
    PhaseScan ps;
    if (frames.size() < 2)
        return ps;
    const size_t n = frames.size() - 1;
    ps.resid.assign(kTaaJitterPeriod, 0.0);
    for (uint32_t ph = 0; ph < kTaaJitterPeriod; ++ph) {
        double acc = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const auto idx = static_cast<uint32_t>(i) + ph;
            const auto d0 = jitterDeltaPx(idx, jitterScale);
            const auto d1 = jitterDeltaPx(idx + 1u, jitterScale);
            const double dx = d1[0] - d0[0], dy = d1[1] - d0[1];
            double bestHere = -1.0;
            for (double sgn : {1.0, -1.0}) {
                cv::Mat w;
                shiftInto(frames[i], w, sgn * dx, sgn * dy);
                const double r = meanAbs(w, frames[i + 1]);
                bestHere = bestHere < 0.0 ? r : std::min(bestHere, r);
            }
            acc += bestHere;
        }
        ps.resid[ph] = acc / static_cast<double>(n);
    }
    ps.best = static_cast<int>(
        std::min_element(ps.resid.begin(), ps.resid.end()) - ps.resid.begin());
    ps.bestVal = ps.resid[static_cast<size_t>(ps.best)];
    double z = 0.0;
    for (size_t i = 0; i < n; ++i)
        z += meanAbs(frames[i], frames[i + 1]);
    ps.zero = z / static_cast<double>(n);
    return ps;
}

/// 在**给定相位**上沿预测位移做带符号 λ 一维扫描（相位标定通过后才知道幅度对不对）。
struct ScanCurve {
    std::vector<std::pair<double, double>> pts;  ///< (λ, 残差 0..255)，λ 含正负
    double best = 0.0;
    double bestLam = 0.0;
    double zero = 0.0;
};

ScanCurve lambdaScan(const std::vector<cv::Mat>& frames, double jitterScale, uint32_t phase)
{
    ScanCurve c;
    if (frames.size() < 2)
        return c;
    static const double kLams[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0};
    std::vector<double> sum(std::size(kLams) * 2 + 1, 0.0);
    const size_t n = frames.size() - 1;
    for (size_t i = 0; i < n; ++i) {
        const auto idx = static_cast<uint32_t>(i) + phase;
        const auto d0 = jitterDeltaPx(idx, jitterScale);
        const auto d1 = jitterDeltaPx(idx + 1u, jitterScale);
        const double dx = d1[0] - d0[0], dy = d1[1] - d0[1];
        sum[0] += meanAbs(frames[i], frames[i + 1]);
        size_t k = 1;
        for (double sgn : {1.0, -1.0}) {
            for (double lam : kLams) {
                // 方向口径与 lagTranslation 一致：pred(p) = frame_i(p − λΔ) 逼近 frame_{i+1}
                cv::Mat w;
                shiftInto(frames[i], w, sgn * lam * dx, sgn * lam * dy);
                sum[k++] += meanAbs(w, frames[i + 1]);
            }
        }
    }
    c.zero = sum[0] / static_cast<double>(n);
    c.pts.emplace_back(0.0, c.zero);
    size_t k = 1;
    for (double sgn : {1.0, -1.0})
        for (double lam : kLams) {
            const double r = sum[k++] / static_cast<double>(n);
            c.pts.emplace_back(sgn * lam, r);
            if (c.pts.size() == 2 || r < c.best) {
                c.best = r;
                c.bestLam = sgn * lam;
            }
        }
    return c;
}

}  // namespace

int cmdTaaSwim(int pairs, int warm, const std::string& variant)
{
    const auto up = [](int v, int p) { return ((v + p - 1) / p) * p; };
    const int nPairs = up(pairs, static_cast<int>(kTaaJitterPeriod));
    const int nWarm = up(warm, static_cast<int>(kTaaJitterPeriod));
    if (nPairs != pairs || nWarm != warm)
        std::printf("⚠ 窗口对齐抖动周期 %u：pairs %d→%d、warm %d→%d（周期内帧间跳变幅度不均，"
            "见 taa_plan §6 末）\n\n", kTaaJitterPeriod, pairs, nPairs, warm, nWarm);
    const int shots = nPairs + 1;
    const std::string root = projectRoot() + "/build";
    std::printf("真实场景 TAA 游动量测：1280×720  bistro（env/skybox/IBL/AO/shadow 全开）  "
        "静止机位  预热 %d 帧  帧对 %d\n"
        "假设：收敛后 out_i(uv) = F(uv − jitter_i) ⟹ 相隔 k 帧的两帧是精确平移 (j_{i+k} − j_i)\n\n",
        nWarm, nPairs);

    SwimRun T0, T1, T2, T3, T4, T5, T6;
    try {
        T0 = runSwimSequence(root + "/imgqa_swim_t0", false, 0.12, 1.0, variant, shots, nWarm,
            "swimt0", static_cast<int>(kTaaJitterPeriod));
        T1 = runSwimSequence(root + "/imgqa_swim_t1", true, 1.0, 1.0, variant, shots, nWarm,
            "swimt1", static_cast<int>(kTaaJitterPeriod));
        T2 = runSwimSequence(root + "/imgqa_swim_t2", true, 0.12, 1.0, variant, shots, nWarm,
            "swimt2", static_cast<int>(kTaaJitterPeriod));
        // T3 **恒等式组**：jitter_scale=0 + 静止机位 ⟹ 每帧画面相同、prevUv 必须恰为 uv
        // ⟹ 历史 = 同一张图 ⟹ 收敛输出必须与 TAA 关**逐位相同**。本项目最便宜且不可辩驳
        // 的判据形态（`taa_plan` §7 第 12 条可复用判断①：先把它自己的恒等式做成判据）。
        // 它不依赖参考实现、不依赖 Halton 口径、不依赖观测域选择。
        T3 = runSwimSequence(root + "/imgqa_swim_t3", true, 0.12, 0.0, variant, shots, nWarm,
            "swimt3", static_cast<int>(kTaaJitterPeriod));
        // T4 **大抖动组**：jitter_scale=8（±4px）+ **feedback=1**。
        // 为什么 ±0.5px 档不行：那里「整幅平移」与「边缘二值覆盖翻转」同量级 ⟹ 平移算子没有
        // 区分力（首轮实测：搬对相位反而更差 120%，反向对照也不更高）。把抖动推到 8 倍让平移
        // 项压过覆盖翻转项，算子才有力气回答「是不是平移」（把参数推到极端值确认管道真的通）。
        // ⚠ 本组**必须** feedback=1：着色器尾部是 `lerp(clipped, cur, params.x)`，**feedback 是
        // 「本帧权重」**（Filament `mix(history, filtered, feedback)` 同序，见 taa.frag.slang 那段
        // 批 12.6 T4 注释）。fb=1 ⟹ 输出 = 本帧光栅（无累积、无糊）⟹ 画面按构造 = 基图平移
        // 8·jitter_t。fb=0 反而是「输出 = 纯历史」的退化反馈环。
        // **本轮实测踩的就是这条**：主 agent 凭记忆把 fb 写成 0（以为是 `mix(cur, hist, f)`），
        // 于是 T4 的 lag-16 同相位差从 0.000 变成 **0.068 ≠ 0**——纯历史当然逐帧不同。
        // ⟹ 教训：`temporal-swim-anchor` 这条免费标定不只防采集，它**也防工具自己的口径漂移**；
        //   任何「按构造必须为 0」的锚点都有这个副产品。
        T4 = runSwimSequence(root + "/imgqa_swim_t4", true, 1.0, 8.0, variant, shots, nWarm,
            "swimt4", static_cast<int>(kTaaJitterPeriod));
        // T5/T6 **基空间组**（批 13.2，门 `taa.base_space`）：与 T2/T1 **只差一个门**，
        // 于是「门关读数」与「门开读数」在同一次运行、同一机位、同一交付配置下直接可比
        // ——这正是 `temporal-base-space` 的阳性锚点结构（锚点与被测项共用一切噪声源）。
        //   T5 = T2 + 门开（现行工作点 fb=.12 js=1）：判据主体，游动应显著下降。
        //   T6 = T1 + 门开（fb=1，无累积）：把「重采样本身引入的帧间差」与「游动」分开。
        //        fb=1 时输出 = 本帧光栅重采样到基网格，相邻帧之间**没有**整幅平移项，
        //        只剩「按亚像素偏移重新光栅化」的覆盖翻转地板（≈ T4 交付场景档 37%）。
        T5 = runSwimSequence(root + "/imgqa_swim_t5", true, 0.12, 1.0, variant, shots, nWarm,
            "swimt5", static_cast<int>(kTaaJitterPeriod), true);
        T6 = runSwimSequence(root + "/imgqa_swim_t6", true, 1.0, 1.0, variant, shots, nWarm,
            "swimt6", static_cast<int>(kTaaJitterPeriod), true);
    } catch (const std::exception& e) {
        std::printf("  采集失败: %s\n", e.what());
        return 2;
    }

    const auto row = [](const char* name, const SwimRun& r) {
        std::printf("  %-27s 逐帧|Δ| %6.3f/255   同相位差 %6.3f/255   梯度能量 %s\n", name,
            r.consecDiff, r.lagDiff, d6(r.gradEnergy).c_str());
    };
    row("T0 TAA 关（地板）", T0);
    row("T1 TAA开 fb=1 js=1（只抖不滤）", T1);
    row("T2 TAA开 fb=.12 js=1（现行）", T2);
    row("T3 TAA开 fb=.12 js=0（恒等式）", T3);
    row("T4 TAA开 fb=1 js=8（本帧+大抖动）", T4);
    row("T5 = T2 + base_space 开（基空间）", T5);
    row("T6 = T1 + base_space 开（基空间不累积）", T6);

    // 离未抖基图多远（T0 逐帧差恰为 0 ⟹ 取第一帧当基图 R）。这条**不是**平移判据
    // （理由见 LagStat 头注释），它是「输出偏离正确画面多少」的画质量。
    const cv::Mat ref = T0.bytes.front();
    const auto distTo = [&ref](const std::vector<cv::Mat>& fr) {
        double s = 0.0;
        for (const auto& f : fr)
            s += meanAbs(f, ref);
        return s / static_cast<double>(fr.size());
    };
    std::printf("\n  离未抖基图 R 的平均偏离（画质量，不是平移判据）：T1 %.3f  T2 %.3f  "
        "T3 %.3f  T4 %.3f /255\n",
        distTo(T1.bytes), distTo(T2.bytes), distTo(T3.bytes), distTo(T4.bytes));
    std::printf("  ⚠ T3 一列**必须恰为 0**（恒等式：零抖动 + 静止 ⟹ TAA 开 ≡ 关）。\n");

    // ── 相位标定（**必须先于一切「按预测位移搬」的表**）────────────────────────
    // 采集下标与引擎抖动序号之间差一个未知常数相位（引擎 frameIndex 自己起算 + 序号再
    // +1 + 每次启动的 RPC 登记落在第几帧）。本轮实测：同一组数据，相位错位时「搬对相位」残差
    // 86.7%，标定后 14.4% —— 同一个算子，结论从「帧间差不是平移」翻成「帧间差就是平移」。
    // ⟹ 顺序即正确性：任何按预测位移做的表都必须排在标定之后。
    // T4 的 fb=1 让输出**精确**等于本帧光栅，于是相邻两帧之间按构造就是 Halton 闭式位移的
    // 整幅平移。但「采集第 i 帧」与「引擎抖动序号 i」之间差一个未知常数相位（引擎 frameIndex
    // 自己起算 + 序号再 +1），λ 扫描必须先标定掉它，否则每对帧的预测方向都是错位的——
    // 本轮实测：不标定相位时 ±4px 档搬对搬错都是 ~100%，看起来像「画面没被搬起来」。
    const PhaseScan ps = phaseScan(T4.bytes, 8.0);
    std::printf("\n  相位标定（T4 = 本帧 + ±4px 抖动；16 个候选常数相位，15 帧对均值）\n");
    for (size_t ph = 0; ph < ps.resid.size(); ++ph)
        std::printf("    φ=%2zu  残差 %7.3f/255  (%5.1f%% of 不搬)%s\n", ph, ps.resid[ph],
            100.0 * ps.resid[ph] / (ps.zero > 0 ? ps.zero : 1.0),
            static_cast<int>(ph) == ps.best ? "   ← argmin" : "");
    std::printf("  → 真相位 φ = %d，残差压到 %.1f%%（不搬基线 %.3f/255）\n", ps.best,
        100.0 * ps.bestVal / (ps.zero > 0 ? ps.zero : 1.0), ps.zero);

    const ScanCurve scan = lambdaScan(T4.bytes, 8.0, static_cast<uint32_t>(ps.best));
    std::printf("\n  λ 扫描（在标定出的相位 φ=%d 上；沿预测位移方向带符号，λ=+1 为真值）\n",
        ps.best);
    for (const auto& [lam, r] : scan.pts)
        std::printf("    λ=%+5.2f  残差 %7.3f/255  (%5.1f%% of 不搬)%s\n", lam, r,
            100.0 * r / (scan.zero > 0 ? scan.zero : 1.0),
            lam == scan.bestLam ? "   ← argmin" : "");
    std::printf("  → argmin λ = %+.2f  最深处压到 %.1f%%\n", scan.bestLam,
        100.0 * scan.best / (scan.zero > 0 ? scan.zero : 1.0));

    // ── 平移检验：逐 lag ───────────────────────────────────────────────────
    std::printf("\n  %-4s %-8s %-10s %-10s %-10s %-10s\n", "lag", "预测位移", "实测|Δ|",
        "搬对相位", "搬错相位", "搬对/实测");
    const auto lagTable = [](const char* name, const std::vector<cv::Mat>& frames,
                             double jitterScale, uint32_t phase) {
        std::printf("  %s\n", name);
        double sumWrong = 0.0, sumMeas = 0.0, sumResid = 0.0;
        int counted = 0;
        for (int k = 1; k <= static_cast<int>(kTaaJitterPeriod); ++k) {
            const LagStat s = lagTranslation(frames, k, jitterScale, phase);
            if (s.meas == 0.0 && s.pred == 0.0 && k > 1)
                continue;
            std::printf("  %-4d %-8.3f %-10.3f %-10.3f %-10.3f %-10.1f%%  符号%+d\n", s.lag,
                s.pred, s.meas, s.resid, s.wrong, 100.0 * s.resid / (s.meas > 0 ? s.meas : 1.0),
                s.sign);
            if (k < static_cast<int>(kTaaJitterPeriod)) {
                sumWrong += s.wrong;
                sumMeas += s.meas;
                sumResid += s.resid;
                ++counted;
            }
        }
        std::printf("  → 汇总（lag 1..15）：搬对相位残差 %.3f  搬错相位 %.3f  实测 %.3f"
            "  ⟹ 搬对压到 %.1f%%、搬错压到 %.1f%%\n",
            sumResid / counted, sumWrong / counted, sumMeas / counted,
            100.0 * sumResid / (sumMeas > 0 ? sumMeas : 1.0),
            100.0 * sumWrong / (sumMeas > 0 ? sumMeas : 1.0));
        return std::array<double, 2>{sumResid / (sumMeas > 0 ? sumMeas : 1.0),
            sumWrong / (sumMeas > 0 ? sumMeas : 1.0)};
    };
    const auto a2 = lagTable("T2 TAA开 fb=.12 js=1（现行，±0.5px）", T2.bytes, 1.0,
        static_cast<uint32_t>(ps.best));
    const auto a4 = lagTable("T4 TAA开 fb=1  js=8（本帧，±4px）", T4.bytes, 8.0,
        static_cast<uint32_t>(ps.best));

    // ── **绝对**抖动传递测量（本命令最硬的一条）─────────────────────────────
    // 上面全是「帧对帧差分」口径，弱点：预测位移只要整体错位一个常数相位，差分就搬不动，
    // 而相位标定本身又是靠同一个弱算子做的（实测 16 个候选里 φ=0 与 φ=10 只差 1%，浅谷）。
    // 换成**绝对**口径：T0 = 零抖动参考 R（TAA 关，逐帧差恰为 0），T4 = fb=1 的本帧光栅，
    // 于是「T4 第 i 帧 = R 平移 d_i」**按构造成立**，d_i 由自由拟合直接量出（信号 ±4px，
    // 覆盖翻转项压不过它）。再拿量出的 d_i 去匹配 Halton 预测集 {−js·j(φ+i)}：
    //   · |d_i| ≈ 0 ⟹ 抖动根本没把画面搬起来（注入断了 / 被别的东西吃掉）；
    //   · |d_i| ≈ js·|j| 且某个 (φ, 符号) 拟合得好 ⟹ 抖动传递 = 文档公式，且顺带量出相位；
    //   · |d_i| ≈ js·|j| 但怎么都拟合不上 ⟹ 画面搬了，但不是按 Halton 序列搬（另有机制）。
    // 拟合器自检（**阳性锚点**）：把 R 按已知位移做成一张「假抖动帧」，看 freeShift 能不能把它
    // 量回来。位移取值 = 非整、非零、双轴、跨符号——尺度/符号/卷绕任何一处写错都会在这里暴露。
    // 本项目纪律：辅助函数要有**独立**判据（第一版手写网格拟合就是缺这条，尺度写错
    // 却一路把假数打印出来）。
    // ⚠ **锚点位移 = (3.25, −2.125)，不是最初的 (3.50, −2.25)**（2026-10-04 换探针不换阈值）：
    // OpenCV phaseCorrelate 的三点抛物线亚像素拟合在**位移恰为半整数**处有系统性奇点，本场景
    // 独立探针对 14 组已知位移实测：x ∈ {1.5, 2.5, 3.5, 7.5, −3.5}（全半整数）误差一律
    // **−0.320±0.002px**（与内容几乎无关的常数偏移），而 x ∈ {3.0, 3.25, 3.75} 误差 ≤0.002px、
    // y = −2.25（四分之一整数）误差 0.003px。旧锚点 x = 3.5 正踩奇点 ⟹ 它量到的是**估计器
    // 奇点，不是拟合器分辨率**（旧场景当年量回 3.73 = 同一奇点的内容扰动项，0.23px 贴着 0.25
    // 阈值过线，本就没有余量）。实测量程从不落在奇点上：T4 位移 = 8·j ∈ ℤ、T1/T2 = j ∈
    // {±⅛, ±¼, ±⅜}，逐帧误差实测 0.030~0.063px ⟹ 新锚点位移取与实测量程同档的四分之一整数。
    // 阈值 **0.25px / 10% 一字未动**；区分力：尺度错 2× → 误差 ≥3.25px、符号反 → ≥6.5px、
    // 整数错位 → ≥1px，全部远超 0.25px。半整数奇点保留为**只打印**诊断（anchorHalf）。
    constexpr double kAnchorDx = 3.25, kAnchorDy = -2.125;
    cv::Mat probe;
    cv::warpAffine(ref, probe, cv::Matx23d(1, 0, -kAnchorDx, 0, 1, -kAnchorDy), ref.size(),
        cv::INTER_CUBIC, cv::BORDER_REPLICATE);
    const FreeShift anchor = freeShift(ref, probe);
    // **反向用例**（堵「拟合器卡常数/直流泄漏」——只有阳性锚点时，一个恒返回常数的坏拟合器
    // 也是绿的）：零位移必须量回 |d| ≤ 0.25px。
    const FreeShift anchor0 = freeShift(ref, ref.clone());
    // 半整数奇点诊断（只打印不判，见上）：它若从 −0.32 漂走 = OpenCV 亚像素估计器换了实现。
    cv::Mat probeHalf;
    cv::warpAffine(ref, probeHalf, cv::Matx23d(1, 0, -3.5, 0, 1, 2.25), ref.size(),
        cv::INTER_CUBIC, cv::BORDER_REPLICATE);
    const FreeShift anchorHalf = freeShift(ref, probeHalf);

    struct AbsStat {
        double meas = 0.0, pred = 0.0, err = 0.0, rms = 1e30, resid = 0.0, zero = 0.0, peak = 0.0;
        double swing = 0.0, swingMax = 0.0;  ///< 逐帧位移变化 |d_{i+1} − d_i| 的均值 / 最大值（px）
        int ph = 0, sx = 1, sy = 1;
    };
    const auto absMeasure = [&ref](const std::vector<cv::Mat>& frames, double js, bool verbose) {
        struct Row { double dx, dy, resid, zero, peak; };
        std::vector<Row> rows;
        rows.reserve(frames.size());
        AbsStat st;
        for (const auto& f : frames) {
            const FreeShift fs = freeShift(ref, f);
            rows.push_back({fs.dx, fs.dy, fs.resid, fs.zero, fs.peak});
            st.meas += std::hypot(fs.dx, fs.dy);
            st.zero += fs.zero;
            st.resid += fs.resid;
            st.peak += fs.peak;
        }
        const auto n = static_cast<double>(rows.size());
        // 用实测位移去匹配 Halton 预测集 {js·j(i+φ)}：16 相位 × 2 符号，取 RMS 最小
        // 符号**逐轴**搜（4 组合），不是整体一个符号：引擎 x 与 y 的符号本来就不同（y 多一次
        // Vulkan 翻转）。第一版只试整体 ±，于是永远匹配不上，实测 RMS 1.748px 被误读成
        // 「画面搬了但不按 Halton 搬」——**搜索空间少一维，结论就会反过来**。
        for (int ph = 0; ph < static_cast<int>(kTaaJitterPeriod); ++ph)
            for (int sx : {1, -1})
                for (int sy : {1, -1}) {
                    double acc = 0.0;
                    for (size_t i = 0; i < rows.size(); ++i) {
                        const auto j =
                            jitterDeltaPx(static_cast<uint32_t>(i) + static_cast<uint32_t>(ph), js);
                        acc += std::pow(rows[i].dx - sx * j[0], 2)
                             + std::pow(rows[i].dy - sy * j[1], 2);
                    }
                    const double rms = std::sqrt(acc / (2.0 * n));
                    if (rms < st.rms) {
                        st.rms = rms;
                        st.ph = ph;
                        st.sx = sx;
                        st.sy = sy;
                    }
                }
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto j =
                jitterDeltaPx(static_cast<uint32_t>(i) + static_cast<uint32_t>(st.ph), js);
            const double ex = st.sx * j[0], ey = st.sy * j[1];
            st.pred += std::hypot(ex, ey);
            st.err += std::hypot(rows[i].dx - ex, rows[i].dy - ey);
            if (verbose && i < 8)
                std::printf("    %-4zu (%+6.2f,%+6.2f)  (%+6.2f,%+6.2f)  |d|=%-6.2f 峰 %-5.3f"
                            " 残差塌到 %5.1f%%\n",
                    i, rows[i].dx, rows[i].dy, ex, ey, std::hypot(rows[i].dx, rows[i].dy),
                    rows[i].peak, 100.0 * rows[i].resid / (rows[i].zero > 0 ? rows[i].zero : 1.0));
        }
        // **游动的物理单位**：相邻两帧输出之间的整幅位移差。这才是用户嘴里「画面游动 1~2px」
        // 的直接对应量（像素），不需要任何观测域/阈值换算。
        for (size_t i = 1; i < rows.size(); ++i) {
            const double d = std::hypot(rows[i].dx - rows[i - 1].dx, rows[i].dy - rows[i - 1].dy);
            st.swing += d;
            st.swingMax = std::max(st.swingMax, d);
        }
        st.swing /= std::max(1.0, n - 1.0);
        st.meas /= n; st.pred /= n; st.err /= n; st.resid /= n; st.zero /= n; st.peak /= n;
        return st;
    };
    std::printf("\n  绝对抖动传递（相位相关：抖动帧 ↔ 零抖动参考 R）\n");
    std::printf("\n  拟合器自检：已知位移 (%+.2f,%+.3f) → 量回 (%+.2f,%+.2f)，残差塌到 %.1f%%"
                "（须 |误差|≤0.25px 且 <10%%）｜零位移反向用例 (%+.3f,%+.3f)｜半整数奇点诊断"
                " (3.50,−2.25) → (%+.2f,%+.2f)（只打印不判，理由见锚点注释）\n",
        kAnchorDx, kAnchorDy, anchor.dx, anchor.dy,
        100.0 * anchor.resid / (anchor.zero > 0 ? anchor.zero : 1.0), anchor0.dx, anchor0.dy,
        anchorHalf.dx, anchorHalf.dy);
    std::printf("  %-4s %-19s %-19s %-9s %-7s %s\n", "帧", "实测 d=(dx,dy)px", "预测 js·j(i+φ)",
        "|d|px", "峰值", "残差塌到");
    const AbsStat abs8 = absMeasure(T4.bytes, 8.0, true);
    std::printf("    … （共 %zu 帧，只列前 8）\n", T4.bytes.size());
    std::printf("  T4 js=8  ：实测|d| %.3fpx  预测 %.3fpx  逐帧误差 %.3fpx  RMS %.3fpx"
                "（φ=%d 符号 x%+d y%+d）  峰值 %.3f\n",
        abs8.meas, abs8.pred, abs8.err, abs8.rms, abs8.ph, abs8.sx, abs8.sy, abs8.peak);
    std::printf("  T4 js=8  ：位移把「抖动帧 ↔ R」的差从 %.3f/255 压到 %.3f/255（%.1f%%）\n",
        abs8.zero, abs8.resid, 100.0 * abs8.resid / (abs8.zero > 0 ? abs8.zero : 1.0));
    const AbsStat abs1 = absMeasure(T1.bytes, 1.0, false);
    const AbsStat abs2 = absMeasure(T2.bytes, 1.0, false);
    // 基空间组（批 13.2）：与 abs2 **只差一个门**，`swing` 直接可比。
    // ⚠ `meas`/`err`/`rms` 这几列对基空间组**无意义**（它们拿 Halton 预测集去匹配，
    //   而基空间组按设计就不该有位移）；只有 `swing` 与 `resid/zero` 是要读的量。
    const AbsStat abs5 = absMeasure(T5.bytes, 1.0, false);
    const AbsStat abs6 = absMeasure(T6.bytes, 1.0, false);
    const auto line = [](const char* nm, const AbsStat& st) {
        std::printf("  %-9s：实测|d| %.3fpx  预测 %.3fpx  逐帧误差 %.3fpx  RMS %.3fpx"
                    "（φ=%d 符号 x%+d y%+d）  峰值 %.3f  残差塌到 %5.1f%%\n",
            nm, st.meas, st.pred, st.err, st.rms, st.ph, st.sx, st.sy, st.peak,
            100.0 * st.resid / (st.zero > 0 ? st.zero : 1.0));
    };
    line("T1 fb=1", abs1);
    line("T2 fb=.12", abs2);
    std::printf("  ⟹ **游动量（像素，直接对应肉眼）**：T2 相邻帧整幅位移 均值 %.3fpx  最大 %.3fpx"
                "   ｜  T1（不累积）均值 %.3fpx  最大 %.3fpx\n",
        abs2.swing, abs2.swingMax, abs1.swing, abs1.swingMax);
    std::printf("  ⟹ **基空间（base_space 开）**：T5 相邻帧整幅位移 均值 %.3fpx  最大 %.3fpx"
                "   ｜  T6（不累积）均值 %.3fpx  最大 %.3fpx\n",
        abs5.swing, abs5.swingMax, abs6.swing, abs6.swingMax);
    std::printf("     离零抖动参考 R 的平均偏离：T2 %.3f  T5 %.3f /255   "
                "平移模型残差塌到：T2 %.1f%%  T5 %.1f%%\n",
        distTo(T2.bytes), distTo(T5.bytes),
        100.0 * abs2.resid / (abs2.zero > 0 ? abs2.zero : 1.0),
        100.0 * abs5.resid / (abs5.zero > 0 ? abs5.zero : 1.0));

    std::printf("\n  读法：残差塌到近零 ⟹ 两帧之间确是整幅亚像素平移（**游动**）。\n"
        "        残差 ≈ 实测（搬不动）⟹ 帧间差不是平移，是逐像素覆盖翻转/历史被拒。\n"
        "        「搬错相位」是反向对照：它若也压到同样低，本算子没有区分力，两条都不作数。\n"
        "        lag 16 是免费标定：预测位移恰为 0，实测差必须恰为 0（同相位 = 同画面）。\n"
        "        ±0.5px 档算子可能没力气（首轮实测如此）⟹ 看 T4 的 ±4px 档 + 下面的 λ 扫描。\n");

    std::vector<Verdict> vs;
    vs.push_back(verdictFor("determinism", T0.consecDiff == 0.0,
        "TAA 关 + 静止机位连拍：逐帧差均值 " + d6(T0.consecDiff)
            + "/255（非零则一切比值是噪声比噪声）"));
    // 同相位（lag = 一个周期）差的容差**不是拍的，是 IIR 尾巴的闭式**：fb=0.12 时 n 帧前的
    // 样本权重 = f(1−f)^(n−1)，预热 W 帧后残留的非周期成分 ≲ (1−f)^W × 满幅逐帧变化。
    // (1−0.12)^32 = 0.0167，满幅取 2.0/255（宽松）⟹ 容差 0.033/255。实测 0.0033，留 10× 余量。
    // 而 fb=1 的 T1 **没有记忆** ⟹ 必须**恰为 0**，一个 bit 都不许差。
    const double iirTol = std::pow(1.0 - 0.12, static_cast<double>(nWarm)) * 2.0;
    vs.push_back(verdictFor("temporal-swim-anchor",
        T1.lagDiff == 0.0 && T2.lagDiff <= iirTol,
        "免费标定（lag = 一个完整抖动周期 ⟹ 预测位移恰为 0）：T1 同相位差 " + d6(T1.lagDiff)
            + "、T2 同相位差 " + d6(T2.lagDiff)
            + "/255。T1（fb=1，**无记忆**）须**恰为 0**；T2（fb=.12，有记忆）容差 = IIR 尾巴"
              "闭式 (1−f)^warm × 2 = "
            + d6(iirTol)
            + "（预热 32 帧残留 0.0167 的满幅成分）。非零超界 = 连拍不是周期 16 的确定性函数"
              "（窗口没对齐 / 历史没收敛 / 帧不连续），此时任何按 Halton 预测做的判据都不作数"));
    // 判据只取**收敛段**（理由见 convergedTail 注释：前段是历史瞬态 = 收敛速度，不是缺陷）
    const std::vector<cv::Mat> t3 = convergedTail(T3.bytes);
    const double d3 = distTo(t3);
    const double mx3 = maxDistTo(ref, t3);
    // 残差**集中在哪**？sub-ULP 的重投影差经 CR 核放大只可能发生在高对比边缘（核在 t≈0 附近
    // 对 t 的导数最大）。这句话不能只写在注释里——算出来：边缘 = 梯度 ≥8/255 的像素。
    double edgeFracAll = 0.0, edgeFracDev = 0.0;
    int nBad = 0;
    {
        cv::Mat g, gx, gy, dev;
        cv::cvtColor(ref, g, cv::COLOR_BGR2GRAY);
        cv::Sobel(g, gx, CV_32F, 1, 0, 1);
        cv::Sobel(g, gy, CV_32F, 0, 1, 1);
        cv::magnitude(gx, gy, gx);
        cv::Mat edge = gx >= 8.0;
        cv::Mat dmax;
        cv::absdiff(t3.back(), ref, dmax);
        cv::cvtColor(dmax, dmax, cv::COLOR_BGR2GRAY);
        const cv::Mat bad = dmax >= 2;
        edgeFracAll = static_cast<double>(cv::countNonZero(edge)) / (edge.total() + 1e-9);
        nBad = cv::countNonZero(bad);
        edgeFracDev = nBad > 0
            ? static_cast<double>(cv::countNonZero(bad & edge)) / (static_cast<double>(nBad) + 1e-9)
            : 1.0;
    }
    const double devFrac = static_cast<double>(nBad) / (ref.total() + 1e-9);
    std::printf("  恒等式残差归属：边缘像素占全图 %.1f%%，dev≥2 共 %d 像素（%.4f%%），"
                "其中 %.1f%% 落在边缘上\n",
        edgeFracAll * 100.0, nBad, devFrac * 100.0, edgeFracDev * 100.0);
    std::printf("\n  T3 恒等式残差：均值 %.6f/255  **逐像素最大 %.0f/255**\n", d3, mx3);
    vs.push_back(verdictFor("temporal-identity",
        d3 <= 0.05 && mx3 <= 4.0 && devFrac <= 0.0001
            && (nBad < kEdgeAttributionFloor || edgeFracDev >= 0.8) && T3.consecDiff < 0.01,
        "恒等式（零抖动 + 静止机位 ⟹ 历史与本帧同图、prevUv 必须落在 uv 上 ⟹ TAA 开 ≡ TAA 关）："
        "离基图均值 "
            + d6(d3) + "/255、逐像素最大 " + std::to_string(static_cast<int>(mx3))
            + "/255、dev≥2 共 " + std::to_string(nBad) + " 像素（" + d2(devFrac * 100.0)
            + "%）、逐帧差 " + d6(T3.consecDiff)
            + "/255。判据 = 均值 ≤0.05 且 **max ≤4** 且 **dev≥2 占比 ≤0.01%** 且（当 dev≥2 "
              "像素数 ≥"
            + std::to_string(kEdgeAttributionFloor)
            + " 时）残差 ≥80% 落在边缘上 且逐帧差 <0.01。\n"
              "  · 判据窗口 = 连拍**收敛段**（末尾 max(4,N/4) 帧）：恒等式是不动点命题，"
              "前段的历史瞬态（0.88/帧衰减）量的是收敛速度不是缺陷，见 convergedTail 注释。\n"
              "  · max 才是关键：均值小可能是「大量像素各差 1 LSB」（浮点结合序，无害），也可能是"
              "「个别像素差几十 LSB」（实现缺陷）。上限从 8 收到 4 = 实测修复后 max=3（2026-10-04，"
              "交付配置 + 用户机位；修复前 max=37、dev≥2 占 10.37%）。\n"
              "  · 新增「dev≥2 占比 ≤0.01%」：max 单独用会漏「大量像素各差 3 LSB」这种不触 max 的"
              "系统性偏移。\n"
              "  · 边缘归属只在 dev≥2 像素足够多时才有统计力：残差降到 30 像素量级后，"
              "「≥80% 在边缘」变成了在噪声里追一个不存在的模式（实测 30 像素里 56.7% 在边缘，"
              "而这些像素的梯度中位数 8.8 已是全图中位数 3.3 的 2.7 倍）。像素少 ⟹ 该条**不参审**，"
              "像素多 ⟹ 必须回到 ≥80%（缺陷的指纹就是「残差不在边缘」）。"
              "与抖动幅度无关 ⟹ 把固有代价整个摘掉"));
    // 相位唯一性：次小残差必须显著高于最小残差，否则「argmin φ」只是噪声里挑出来的，
    // 拿它去解释 λ 扫描 = 用一把没刻度的尺。
    double secondBest = 1e30;
    for (size_t ph = 0; ph < ps.resid.size(); ++ph)
        if (static_cast<int>(ph) != ps.best)
            secondBest = std::min(secondBest, ps.resid[ph]);
    vs.push_back(verdictFor("temporal-fitter-anchor",
        std::abs(anchor.dx - kAnchorDx) <= 0.25 && std::abs(anchor.dy - kAnchorDy) <= 0.25
            && anchor.resid < 0.1 * anchor.zero
            && std::hypot(anchor0.dx, anchor0.dy) <= 0.25,
        "拟合器阳性锚点 + 反向用例：把 R 按**已知**位移 (3.25,−2.125)（与实测量程同档：T4 位移"
        " = 8j ∈ ℤ、T1/T2 = j ∈ {±⅛,±¼,±⅜}）做成假抖动帧，相位相关必须量回来：实测 ("
            + d6(anchor.dx) + "," + d6(anchor.dy) + ")（各轴误差 ≤0.25px），且位移把残差压到 "
            + d2(100.0 * anchor.resid / (anchor.zero > 0 ? anchor.zero : 1.0))
            + "%（须 <10%）；**反向用例**零位移须量回 |d| ≤0.25px（堵『拟合器卡常数』——只有"
              "阳性锚点时恒返回常数的坏拟合器也是绿的，实测 ("
            + d6(anchor0.dx) + "," + d6(anchor0.dy)
            + "））。下面所有『实测位移』都建立在这个拟合器上，它自己没有独立判据的话，整组读数"
              "无刻度。⚠ 本条不是形式主义：第一版手写网格拟合缺这条，尺度写错（把 1/4 降采样步长"
              "当全分辨率像素）却一路打印自相矛盾的假数。阈值 0.25px/10% **未放宽**（2026-10-04 "
              "换探针位移而非换阈值）：旧锚点 (3.50,−2.25) 的 x 恰为半整数，踩在 phaseCorrelate "
              "三点抛物线亚像素拟合的奇点上——本场景 14 组已知位移实测：半整数处误差恒 "
              "−0.320±0.002px（x∈{1.5,2.5,3.5,7.5,−3.5}），四分之一整数/整数处 ≤0.002px ⟹ 旧"
              "读数红的是**估计器奇点不是拟合器分辨率**（奇点诊断仍打印：anchorHalf）。区分力："
              "尺度错 2× → ≥3.25px、符号反 → ≥6.5px、整数错位 → ≥1px，全部 ≫0.25px"));
    vs.push_back(verdictFor("temporal-jitter-transfer",
        std::abs(abs8.meas - abs8.pred) <= 0.25 * abs8.pred && abs8.err <= 0.25 * abs8.pred
            && abs8.resid <= 0.5 * abs8.zero && abs8.sx == 1 && abs8.sy == 1,
        "抖动传递（**绝对口径**，js=8 大信号档）。四条各自堵一种失败：① 实测位移模长 "
            + d6(abs8.meas) + "px vs 预测 " + d6(abs8.pred)
            + "px（相对误差 ≤25% —— 堵『抖动没注入』与 px/NDC 单位错配，后者会差 2× 以上）；"
              "② 逐帧误差 "
            + d6(abs8.err) + "px ≤25% 预测均值（堵『搬了但不是按 Halton 序列搬』）；③ 位移把"
              "『抖动帧 ↔ 零抖动 R』的差压到 "
            + d2(100.0 * abs8.resid / (abs8.zero > 0 ? abs8.zero : 1.0))
            + "% ≤50%（残差地板 = 重新光栅化的覆盖翻转，**随场景内容变**：合成 profile 档实测 "
              "18.9%、交付配置 + 用户机位 + 整帧档实测 36.7%（2026-10-04），旧 30% 阈值是前一档"
              "标定的，换场景后红的是地板不是模型。区分力证据（同一次运行）：搬错相位残差 92.1%、"
              "不搬 = 100%，失败模式与地板之间隔着 55pp，50% 阈值两侧都有 ≥13pp 余量；且地板的"
              "在场性由 `temporal-swim-discriminative`（搬错须比搬对高 15%）反向守着；"
              "④ 匹配出的**额外**符号必须是 (+1,+1)——因为 `jitterDeltaPx` 已经把引擎口径的"
              "**相反数**（freeShift 口径，= −c_t，逐轴推导与最终裁决见其头注释）写进预测里了，"
              "匹配器再需要翻号 ⟹ 引擎改了投影/翻转/抖动符号而预测没跟上（**坐标约定守卫**）。"
              "⚠ 本条第一版写的是『必须 (−1,+1)』，那是把『预测未含符号』时的旧口径当成恒定真理，"
              "预测改口径后它就自己变红——**守卫也会因自身假设过时而假红**，改预测必须同批改守卫）。"
              "实测 φ=" + std::to_string(abs8.ph)
            + "（φ 是**每次启动的常数**：跨组实测 3/11/15 不等，预热长度对齐周期也压不住——"
              "RPC 登记落在第几帧由启动时序决定。所以 φ 只打印、不判，判了就是假红）"));
    // 预测的相邻帧位移差（一个周期内的均值）——游动的**闭式期望**，零自由参数。
    double predLag1 = 0.0;
    for (uint32_t i = 0; i < kTaaJitterPeriod; ++i) {
        const auto d0 = jitterDeltaPx(i, 1.0), d1 = jitterDeltaPx(i + 1u, 1.0);
        predLag1 += std::hypot(d1[0] - d0[0], d1[1] - d0[1]);
    }
    predLag1 /= static_cast<double>(kTaaJitterPeriod);
    vs.push_back(verdictFor("temporal-swim-pixels",
        abs2.swing >= 0.75 * predLag1 && abs2.swing <= 1.35 * predLag1,
        "**游动量（单位=像素，直接对应肉眼**，phase13 §1.9 的验收量）：现行工作点（fb=.12 "
        "js=1）相邻两帧之间的整幅位移 均值 "
            + d6(abs2.swing) + "px（最大 " + d6(abs2.swingMax) + "px），闭式期望 = 相邻抖动差 = "
            + d6(predLag1)
            + "px（须落在 [0.75, 1.35]×）。**这条钉的是机制不是好坏**：若输出落在基空间，游动应"
              "≈0；实测 = 抖动差 ⟹ **输出在抖动空间**，且时域累积压不掉它（同表 T1 不累积 = "
            + d6(abs1.swing) + "px，几乎同值）。要不要消掉是产品决策，不在本判据里判"));
    // ── 批 13.2 基空间输出（门 `taa.base_space`）的验收判据 ────────────────────
    // 三条各堵一种假绿：
    //  ① 门开态游动 ≤ 闭式抖动差的 **kBaseSpaceSwimCap** 倍 —— 这是「输出搬回基网格」的
    //     直接读数（单位 = 像素，与肉眼同源）。
    //  ② 门关态（T2）游动仍 ≥ 0.75× 闭式 —— **阳性锚点**：没有它，① 可能只是量具坏了、
    //     或整幅平移模型失效（`freeShift` 拟合塌掉时 swing 也恒为 0）。同一次运行、同一
    //     机位、同一交付配置，只差一个门 ⟹ 锚点与被测项共用全部噪声源。
    //  ③ 门开态 ≤ 门关态的一半 —— 相对量兜底：即使 ① 的绝对阈值因场景变化偏松，
    //     「门没接上 ⟹ T5 == T2」也过不了这条。
    constexpr double kBaseSpaceSwimCap = 0.35;
    vs.push_back(verdictFor("temporal-base-space",
        abs5.swing <= kBaseSpaceSwimCap * predLag1 && abs2.swing >= 0.75 * predLag1
            && abs5.swing <= 0.5 * abs2.swing,
        "**基空间输出（`taa.base_space`）主判据**：门开态（T5 = T2 + 门）相邻帧整幅位移 均值 "
            + d6(abs5.swing) + "px（最大 " + d6(abs5.swingMax) + "px），须 ≤"
            + d2(100.0 * kBaseSpaceSwimCap) + "% × 闭式抖动差 " + d6(predLag1)
            + "px，且 ≤ 门关态 T2 的 " + d6(abs2.swing)
            + "px 的一半。阳性锚点 = 同一次运行的门关态 T2 仍 ≥75% 闭式（"
            + d6(abs2.swing)
            + "px）——没有它，① 可能只是量具坏了。T6（门开 + fb=1，无累积）游动 "
            + d6(abs6.swing)
            + "px 只打印不判：它把「重采样本身的帧间差」与「游动」分开，是解读 ① 的旁证。\n"
              "  · 阈值 0.35 的来历（先测后定，2026-10-04 本机）：见 docs/phase13_plan.md 批 "
              "13.2 的实测表。留的不是「效果越好越严」的余量，而是**量具地板**——`freeShift` "
              "是相位相关拟合，静止对上静止时它自己的抖动/亚像素偏置就是零点几像素量级。\n"
              "  · 为什么判 swing 而不是判「离 R 的距离」：后者是画质量（模糊也算偏离），"
              "对平移不敏感；swing 的单位就是像素，直接对应肉眼。"));
    vs.push_back(verdictFor("temporal-jitter-phase",
        ps.bestVal <= 0.5 * ps.zero && secondBest >= 1.25 * ps.bestVal,
        "相位标定（**量具自检**，T4 = 本帧 + ±4px）：16 个候选常数相位里，argmin φ="
            + std::to_string(ps.best) + " 的残差 " + d6(ps.bestVal) + "/255 须 ≤50% of 不搬基线 "
            + d6(ps.zero) + "，且**次小相位** " + d6(secondBest) + "/255 须 ≥1.25× 最小（相位必须"
            "唯一，否则 argmin 只是噪声里挑的）。『不搬』= 原始逐帧差。塌不下去 ⟹ 帧间差不是按 "
            "Halton 位移的整幅平移（要么抖动没注入，要么本工具重抄的公式/序号约定与引擎不符）。"
              "这条不做完，λ 扫描与所有 lag 表读数都是**错位相**，不能作数"));
    vs.push_back(verdictFor("temporal-jitter-calibration",
        std::abs(std::abs(scan.bestLam) - 1.0) <= 0.25 && scan.best < 0.5 * scan.zero,
        "抖动幅度标定（**量具自检**，在相位 φ=" + std::to_string(ps.best) + " 上做带符号 λ 扫描）："
        "argmin λ = " + d2(scan.bestLam) + "（|λ| 须落在 [0.75,1.25]）、最深处残差 "
            + d2(100.0 * scan.best / (scan.zero > 0 ? scan.zero : 1.0))
            + "% of 不搬（须 ≤50%）。argmin 落在 0 ⟹ 抖动没把画面搬起来（注入断了）；|λ| 明显偏离"
              " 1 ⟹ 幅度口径与 `taa_jitter.cppm` 不符；只有负号支命中 ⟹ 符号口径与预测相反（预测 = "
              "`jitterDeltaPx` = freeShift 口径 = −内容位移，逐轴推导见其头注释）。上面所有「按预测位移搬」的"
              "读数都靠这两条撑着，它们红则全部作废"));
    vs.push_back(verdictFor("temporal-swim-discriminative",
        a4[0] < 0.5 && a4[1] > 1.15 * a4[0],
        "算子有效性 + 游动读数（**只在 ±4px 大抖动档判**；±0.5px 档首轮实测无区分力，见本命令"
        "头注释）。T4 按构造 = 基图平移 8·jitter_t ⟹ 搬对相位残差 "
            + d2(100.0 * a4[0]) + "%（须 ≤50%），搬错相位 " + d2(100.0 * a4[1])
            + "%（须比搬对高 15% 以上，否则算子没区分力，读数不作数）"));

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("taa-swim", {{"pairs", std::to_string(nPairs)}, {"warm", std::to_string(nWarm)},
               {"variant", variant.empty() ? "forward" : variant},
               {"T0_consec", d6(T0.consecDiff)}, {"T1_consec", d6(T1.consecDiff)},
               {"T2_consec", d6(T2.consecDiff)}, {"T3_consec", d6(T3.consecDiff)},
               {"T4_consec", d6(T4.consecDiff)},
               {"T1_lag16", d6(T1.lagDiff)}, {"T2_lag16", d6(T2.lagDiff)},
               {"T1_distR", d6(distTo(T1.bytes))}, {"T2_distR", d6(distTo(T2.bytes))},
               {"T3_distR", d6(d3)}, {"T4_distR", d6(distTo(T4.bytes))},
               {"T2_residFrac", d2(a2[0] * 100.0)}, {"T2_wrongFrac", d2(a2[1] * 100.0)},
               {"T4_residFrac", d2(a4[0] * 100.0)}, {"T4_wrongFrac", d2(a4[1] * 100.0)},
               {"T2_grad", d6(T2.gradEnergy)}, {"T0_grad", d6(T0.gradEnergy)},
               {"T5_swing", d6(abs5.swing)}, {"T5_swingMax", d6(abs5.swingMax)},
               {"T6_swing", d6(abs6.swing)},
               {"T5_distR", d6(distTo(T5.bytes))}, {"T5_grad", d6(T5.gradEnergy)},
               {"T3_max", std::to_string(static_cast<int>(mx3))},
               {"T4_phase", std::to_string(ps.best)},
               {"T4_phase_resid", d6(ps.bestVal)}, {"T4_zero", d6(ps.zero)},
               {"T4_scan_argminLam", d2(scan.bestLam)},
               {"T4_scan_best", d6(scan.best)}, {"T4_scan_zero", d6(scan.zero)}},
        vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
