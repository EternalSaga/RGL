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
// 增益，可以直接拿一阶 IIR 的闭式对拍。移动机位的组**已从本命令删除**（当年这里写的是
// "降级为参考值"，现行 = S0/S1/S2 三组全静止，代码里一组移动机位都没有）。
// ⚠ 别把这条读成"移动机位不值得测"：`antiPatterns` 的 `moving-camera-l1-ratio` 的 scope =
// **Operator**（否的是 L1 总量这个算子），**激励没有被否** → 它活在 `temporal-motion` /
// `temporal-flicker` / `temporal-homography`，详见下面「微动机位」那段的历史。
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
// 「微动机位」组的历史（**2026-10-07 改写：原文正在劝退一个后来被建出来的东西**）：
//   当时为什么没有：逐帧改机位唯一的配置驱动来源是 SceneSpin，而它是 `angle += speed * dt`
//     （按帧时长累积）⇒ 每帧角度本身随帧时长漂，测不出可复现的数。
//   当时错在哪：原文接着写「要做微动激励得先加一条按帧索引驱动的相机运动，那是新引擎面，
//     而这条判据已被登记为反模式（`moving-camera-l1-ratio`）——不值得」。**那个反模式否的是
//     L1 总量这个算子（scope = Operator），从没否过这个激励**；作用域读宽一格，就把后来
//     真正需要的激励冻了两天（学费记在 `antiPatterns` 的 `screen-space-l1-frame-diff` 原文）。
//   后来怎么有的：那条"新引擎面"加了 = camera.lua `frame_yaw`（`ProfileOpts::cameraFrameYaw`，
//     yaw = 基准 + frame_yaw·帧号，纯函数跨进程可复现），形态正是本段下面写的"绕 up 轴定角 yaw"。
//   现行口径：**本命令仍不加移动机位组**——S 组的成立条件就是平均视点恒定（S1/S2 输入统计
//     完全相同，比值才是时域累积本身的增益），加了移动机位就不是同一条判据，不是因为"不值得"。
//     移动机位激励归 `temporal-motion`（默认运动源就是相机 yaw）/ `temporal-flicker` /
//     `temporal-homography`。
// 机位要动，正确形态 = 绕 up 轴**定角** yaw（上面那条"新引擎面"就是照这个形态加的）：
// 像素位移与深度无关，且是刚体相机
// 运动，`prevViewProj` 重投影能精确对齐（平移 dolly 会让 9m 外栅栏的单帧位移只剩近处
// 的 22%，信号随深度衰减）。
//
// ## 游动线拆到哪去了（护栏逼的一刀：本文件曾到 926 代码行 > 500）
// 本文件原本住着两台仪器。锚点线（上面全部：yawCam / runSequence / temporalDiffStat /
// cmdTemporal）留本 TU；`taa-swim`（真实场景逐帧差 = 整幅亚像素平移）按「一个仪器一个缝」
// 拆出：算子与采集在 `temporal_swim_metric.cpp`、cmdTaaSwim 与阶段函数在 `temporal_swim.cpp`、
// 跨 TU 类型/算子声明在 `temporal_swim.h`（缝的全文见该头注释）。d6 / d2 两个定点格式化
// 被两台仪器共用，唯一出处也挪进了那个头。
#include "imgqa.h"
#include "temporal_swim.h"
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
    o.aa = taa ? Aa::Taa : Aa::Off;
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

/// 锚点线的四条判决线（S0/S1/S2 读数 → Verdict 列表）。判据名/阈值/容差理由是契约，
/// 一字未动；提出函数只为把 cmdTemporal 的认知复杂度压回阈内（四条 `&&` 链全在这里）。
std::vector<Verdict> anchorVerdicts(const RunResult& S0, const RunResult& S1, const RunResult& S2,
    double abs1, double a2)
{
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
    return vs;
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
        cv::bitwise_and(anyChannelDiff(shots[i - 1].bgr, shots[i].bgr), m, changed);
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
    //   再正确的 TAA 也测不出抑制（实测只有 1.2×，而闭式该是 3.95×——不是引擎的错）。
    //   两条理由分属不同层，别并成一句：① 视点真动了 ⟹ 那部分变化是**正确影像**，任何算子
    //   都测不出"抑制"（这才是本命令选静止机位的理由）；② L1 总量对模糊守恒 ⟹ **算子**不行，
    //   登记为反模式的是这条（`moving-camera-l1-ratio`，scope = Operator；同一激励换算子后
    //   重立为 `temporal-residual-*`，见其 stillOpen）。
    //   静止机位把运动项彻底摘掉：平均视点恒定，唯一变化 = jitter 的采样相位。此时
    //   feedback=1 的消融组（S1）与默认组（S2）**输入统计完全相同**，比值就是时域
    //   累积本身的增益，可直接对拍闭式。
    //     S0  TAA 关 静止             → 引擎确定性地板（硬判据：恰为 0）
    //     S1  TAA 开 feedback=1 静止   → **只抖不滤** = 闪烁本体（硬判据：非零 = 阳性锚点）
    //     S2  TAA 开 feedback=.12 静止  → 滤波后（硬判据：S1/S2 ≥ 3.0，闭式 ≈11×）
    //   ⚠ 没有「微动机位」组，但**不是**因为移动机位不值得测：帧驱动相机运动后来已经有了
    //   （camera.lua `frame_yaw`），它住在 `temporal-motion`；本命令不加它是因为 S 组要求
    //   平均视点恒定。完整历史与口径 → 本文件头注释「微动机位」段 + `moving-camera-l1-ratio`
    //   的 scope/stillOpen。
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

    const std::vector<Verdict> vs = anchorVerdicts(S0, S1, S2, abs1, a2);

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
}  // namespace RGL::imgqa
