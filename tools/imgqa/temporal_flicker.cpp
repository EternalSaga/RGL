//
// temporal_flicker.cpp — 运动态时间闪烁（闭式解判据）
//
// 判据一句话：相机匀速扫过 1px 边缘，「每帧最大变化」实测 ÷ 闭式理想 = 闪烁增益。
//   G = ρ_meas / ρ_ideal                       （1 = 理想 AA）
//   ζ = (ρ_meas − ρ_ideal)/(ρ_noAA − ρ_ideal)  （0 = 理想 AA，1 = 完全无 AA，可越界）
//
// 闭式解在**观测域**（ACES → sRGB → 8bit → 解回线性），两端电平由 ROI 自身标定（反解 ACES
// 前的线性黑/白），**形状**（斜坡 vs 阶梯）是闭式的 —— 这是「独立第二实现」的本体。
//
// 写码前用独立 Python 实现做过标定（16 个亚像素相位 τ 的网格，N=32/64、v=0.125/0.25/0.5）：
//   · 理想斜坡 ρ 实测/闭式 = 0.996（v=0.125, N=32），τ 敏感度 ±1.4%
//   · 无 AA 阶梯 ρ 实测/闭式 = 1.000，τ 敏感度 **0**（16 个 τ 全同，因阶梯的每帧差非 0 即整跳）
//   · 高频带能量比（Hann + 半谱，f_hi = v）：理想 [0.15, 0.60]、无 AA [0.00, 0.86] → **重叠，弃用**
//     根因：混叠是零阶保持，把锯齿**变成方波**，方波高频不比锯齿大；换 τ = 换占空比。
//   · 2 次谐波比 A(2v)/A(v)（v 取 1/8 使周期恰为 8 帧）：理想 [0.45, 0.69]、无 AA [0, 1] → 也弃用
//   ⇒ FFT 只作诊断打印（看能量落在哪个 bin、看谐波结构），不当判据。
//
#include "temporal_flicker.h"
#include "temporal_residual.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kSize[2] = { 1280, 720 };
constexpr float kFovDeg = 45.0f;
constexpr double kTaaFeedback = 0.12;     ///< 交付默认（configs 未写 taa 段 ⟹ 引擎结构体默认）
constexpr int kWarm = 16;                 ///< 一个抖动周期（历史先动起来）

/// `taa.feedback` 滑杆 label（真相源 = `rgl/generated/param_semantics_generated.cppm`；
/// imgqa 不链 rgl，同值重抄，先例 = temporal_motion.cpp 的 kTaaFeedbackLabel）。
constexpr const char* kTaaFeedbackLabel = "TAA 记忆 taa.feedback";

/// aa_target 白板左边缘的参考机位：把**边缘本身**钉到主点（不是白板中心）。
/// 于是 u = 0、局部扫速退化为 f·dψ；ROI 行窗 ±100px 落在白板内部
/// （白板上边界投在 cy−124、下边界 cy+124；栅栏投在 cy+210 以下，进不来）。
constexpr std::array<double, 3> kEdgePosition{ -1.0, 0.335, 2.0 };
constexpr std::array<double, 3> kEdgeTarget{ -1.0, 0.335, -1.0 };

std::string d4(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}
std::string d5(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.5f", v);
    return b;
}

/// 一组采集的产物 + 统计
struct Group: FlickerSeries {
    cv::Rect roi;
    double edgeX = 0.0;
    double sweepLocal = 0.0;
    double rho = 0.0;                ///< 实测每帧变化（p99.5，线性）
    double darkObs = 0.0, whiteObs = 0.0;
    FlickerScale scale;
    FlickerAnchor anchor;
    std::vector<float> signal;       ///< ROI 均值时间序列（线性观测域）
    std::vector<float> spectrum;     ///< Hann 半谱幅度
};

}  // namespace

/**
 * 跑一组：profile → 起引擎 → 钉机位 → 热身（空转不写盘）→ /shotburst 逐帧连续采集。
 * 两条判据（flicker / homography）共用，见 temporal_flicker.h 的 FlickerSeries 注释。
 */
FlickerSeries captureFlickerSeries(const std::string& dir, const std::string& name, const std::string& tag,
    bool edgeTarget, const BistroCam* cam, bool taaOn, double yawRadPerFrame, int frames,
    double kernel, bool wantBands, bool wantObjectId, double envIntensity, double aoStrength,
    bool overrideEnv, bool overrideAo, const FlickerArm* arm)
{
    FlickerSeries g;
    g.name = name;
    ProfileOpts o;
    o.size = { kSize[0], kSize[1] };
    o.aa = taaOn ? Aa::Taa : Aa::Off;
    o.taaFeedback = kTaaFeedback;
    o.taaJitterScale = 1.0;
    o.taaInputKernel = kernel;  // 批 13.8 输入侧重建核（0 = 不写键 = 引擎默认 = 已发表口径）
    o.cameraFrameYaw = yawRadPerFrame;  // rad/帧（键名 = camera.lua `frame_yaw`）
    // 批 14.5 升频 / FSR 臂。⚠ `arm` 只改**渲染分辨率与升频器**，运动激励（cameraFrameYaw）、
    // ROI、帧数一律不动 —— 换了激励就是换口径，两条轴就没法比了。
    if (arm) {
        o.renderScale = arm->renderScale;
        o.aa = arm->aa;  // 批 14.6：aa 一个槽位同时表达「谁做抗锯齿」与「不做」
        o.fsrTemporal = arm->fsrTemporal;
        o.fsrJitter = arm->fsrJitter;
        // 工作点对齐鬼影轴（α 与 clip_mode），见 `FlickerArm` 的注释。
        o.taaFeedback = arm->feedback;
        o.taaClipMode = o.aa == Aa::Taa ? arm->clipMode : 0.0;
    }
    std::array<double, 3> pos = kEdgePosition;
    std::array<double, 3> tgt = kEdgeTarget;
    double fov = kFovDeg;
    if (edgeTarget) {
        // 合成 profile：aa_target 是内存合成模型，没有 glb，只能走 entitiesLua 那条路。
        // ⚠ 闭式判据**必须**跑在这种无光照变量的靶上；交付配置口径由 bistro 组承担。
        o.entitiesLua = "{ type = \"aa_target\" }";
    }
    else {
        o.deliveryConfigDir = projectRoot() + "/configs";  // 红线：判据不跑在合成绿上
        pos = cam->position;
        tgt = cam->target;
        fov = cam->fov;
    }
    // 材质归因要 object_id 附件 ⟹ diagnostics.picking 必须开。实测（2026-10-06）：开 picking
    // **不扰动交付画面**（同机位同帧 picking 开/关截图逐位相同，921600 px 最大差 0），
    // 所以它是一条**零侵入**的观测通道。合成靶没有 glb，材质名映射不出来，故只有 bistro 采。
    if (wantObjectId && !edgeTarget) {
        o.picking = true;
        o.overridePicking = true;
    }
    // A/B 侧：env.intensity=0 关 IBL、ao.strength=0 关 AO。两个门都是**IEEE 精确**旁路
    // （ao：tonemap 的 lerp(1,ao,0)≡1；env：model_pbr_ibl / light 的 diffuse+specular 都乘
    // env.intensity）——但 ⚠ **天穹背景不受 env.intensity 影响**（skybox.frag.slang 与
    // light.frag.slang 的 envSky 分支直接返回 env 采样、不乘 intensity），所以背景会**原样亮着**。
    if (overrideEnv) {
        o.envIntensity = envIntensity;
        o.overrideEnvIntensity = true;
    }
    if (overrideAo) {
        o.aoStrength = aoStrength;
        o.overrideAoStrength = true;
    }
    o.hasCamera = true;
    o.cameraPosition = pos;
    o.cameraTarget = tgt;
    o.cameraFov = fov;
    makeProfile(dir, o);

    Engine eng(dir);
    eng.setCamera({ pos, tgt, fov });
    g.liveFeedback = eng.paramValue("", kTaaFeedbackLabel);  // label 改名 ⟹ 抛，不静默
    eng.advanceFrames(static_cast<uint32_t>(kWarm));
    g.framePre = eng.frameCount();
    // 中点姿态要能算出来，靠的是 yaw = 基准 + 每帧角 × 帧号 这个**纯函数**关系（camera.lua
    // `frame_yaw`）。这里取 burst **之前**的 yaw 作锚点；「每帧角是不是真值」由尺子 1 的
    // 对齐自证（残差在 α=1 见底）兜底——Δψ 错了那条守卫当场红。
    const double yawPre = eng.cameraYawDegrees();
    const double yawStepDeg = yawRadPerFrame * 180.0 / 3.14159265358979323846;
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(frames), tag + "_c");
    g.framePost = eng.frameCount();
    cv::Mat idChk;  ///< 中点姿态补拍的彩色（落位自证用；比对**必须**等 g.luma 填完，见下面）
    if (wantObjectId && !edgeTarget && frames >= 2) {
        const double yawMid = yawPre + yawStepDeg * (1.0 + 0.5 * static_cast<double>(frames - 1));
        // 标定「一次 /shot 会把 yaw 推进多少」：/shot 落在第几帧由 HTTP 时序决定，**不能假设**。
        // 用同一种请求标定同一种请求，比数帧号可靠；标定与使用之间不夹任何别的写相机操作。
        const double y1 = eng.cameraYawDegrees();
        static_cast<void>(eng.shot(tag + "_calib"));
        const double drift = eng.cameraYawDegrees() - y1;
        eng.setCameraYaw(yawMid - drift);  // 预补偿：shot 会把 yaw 推回 yawMid
        static_cast<void>(eng.shot(tag + "_objectid", "object_id"));
        // 落位残差（度 → 像面 px：dx/dψ = f = (H/2)/tan(fov/2)，主点处）
        const double fPx = static_cast<double>(kSize[1]) * 0.5
            / std::tan(0.5 * static_cast<double>(kFovDeg) * 3.14159265358979323846 / 180.0);
        g.yawErrPx = (eng.cameraYawDegrees() - yawMid) * 3.14159265358979323846 / 180.0 * fPx;
        g.ids = decodeObjectId(readPngBgr(eng.shot(tag + "_objectid2", "object_id")));
        // **落位自证**：同一姿态再补拍一张彩色，与 burst 各帧比。差最小者就是中点那一帧，
        // 差值 ≈ 0 ⟺ 「RPC 落的姿态」与「frame_yaw 跑到中点的那一帧」是同一个姿态。
        // 不为 0 也**不拦**（材质是慢变量，几 px 不改变归因），但必须回显，不许读者以为精确。
        // ⚠ 这里只**拍**，比对要挪到函数末尾——`g.luma` 是后面逐帧解码时才填的，
        //   在这里比会拿空 vector 比出「最佳帧 = −1、差 = 1e30」（实测踩过）。
        eng.setCameraYaw(yawMid - drift);
        idChk = readPngBgr(eng.shot(tag + "_idchk"));
    }
    if (files.empty())
        throw std::runtime_error(name + "：/shotburst 一张都没拿到");

    cv::Mat prev;
    // 分带尺子的累加器（`wantBands=false` 时一个字节都不多算，`temporal-flicker` 逐位今天）
    cv::Mat byteSum, byteMax, gradMax;
    for (const auto& path: files) {
        cv::Mat bgr = cv::imread(path, cv::IMREAD_UNCHANGED);
        if (bgr.empty())
            throw std::runtime_error(name + "：读不到 " + path);
        if (bgr.channels() == 4) {
            std::vector<cv::Mat> ch;
            cv::split(bgr, ch);
            bgr = cv::Mat(ch[0].rows, ch[0].cols, CV_8UC3);
            std::vector<cv::Mat> bgrCh = { ch[0], ch[1], ch[2] };
            cv::merge(bgrCh, bgr);
        }
        if (!prev.empty()) {  // 字节域自证：运动激励没进场时这里就是 0，判据当场失去意义
            cv::Mat d;
            cv::absdiff(prev, bgr, d);
            g.byteMaxAdjacent = std::max(g.byteMaxAdjacent, static_cast<double>(cv::norm(d, cv::NORM_INF)));
        }
        prev = bgr.clone();
        Frame f;
        f.camera = { pos, tgt, fov };
        f.bgr = bgr;
        prepareLinear(f);  // 红线：复用，不另写解码
        g.luma.push_back(f.luma.clone());
        if (wantBands) {
            // 亮度轴 = **显示域字节**亮度的多帧均值（tonemap+sRGB 之后取 Rec.601 灰）。
            // 为什么不是线性域：这一轴要的是「交付面上这块**看起来**是不是高光区」，而截图里
            // 没有线性 HDR（ACES 已把 ≫1 压进 8bit）；「削顶」本来就是字节域的事件。口径与
            // `temporal_residual.cpp` 的 bistro 分带逐字相同（同一算子只许一份实现）。
            cv::Mat gray;
            cv::cvtColor(f.bgr, gray, cv::COLOR_BGR2GRAY);
            cv::Mat gd;
            gray.convertTo(gd, CV_64FC1);
            if (byteSum.empty()) {
                byteSum = gd.clone();
                byteMax = gd.clone();
                gradMax = sobelAbsGrad(f.luma);  // 梯度轴主口径 = 线性 luma（与 gradEnergy 同算子）
            }
            else {
                cv::add(byteSum, gd, byteSum);
                cv::max(byteMax, gd, byteMax);
                const cv::Mat g1 = sobelAbsGrad(f.luma);
                cv::max(gradMax, g1, gradMax);  // **跨帧最大**：移动机位没有「相位平均」那张干净图
            }
        }
    }
    if (wantBands && !byteSum.empty()) {
        byteSum /= static_cast<double>(g.luma.size());
        g.byteMean = byteSum;
        g.byteMax = byteMax;
        g.gradMax = gradMax;
    }
    g.frameSize = g.luma.front().size();
    if (!idChk.empty()) {
        if (idChk.channels() == 4) {
            std::vector<cv::Mat> ch;
            cv::split(idChk, ch);
            idChk = cv::Mat(ch[0].rows, ch[0].cols, CV_8UC3);
            std::vector<cv::Mat> b3 = { ch[0], ch[1], ch[2] };
            cv::merge(b3, idChk);
        }
        Frame fchk;
        fchk.camera = { pos, tgt, fov };
        fchk.bgr = idChk;
        prepareLinear(fchk);
        double best = 1e30;
        for (size_t i = 0; i < g.luma.size(); ++i) {
            cv::Mat d;
            cv::absdiff(g.luma[i], fchk.luma, d);
            const double v = cv::mean(d)[0];
            if (v < best) {
                best = v;
                g.idBestFrame = static_cast<int>(i);
            }
        }
        g.idBestDiff = best;
    }
    return g;
}

const BistroCam* resolveFlickerTarget(const std::string& target, bool& edgeTarget)
{
    edgeTarget = (target == "edge" || target.empty());
    if (edgeTarget)
        return nullptr;
    for (const auto& c: kBistroCams)
        if (target == c.name)
            return &c;
    throw std::runtime_error("--target 只认 edge / doorframe / wetground / doorpanel（给了 " + target + "）");
}

double sweepToYawRad(double sweepPxPerFrame, float focalPx)
{
    return std::atan(sweepPxPerFrame / static_cast<double>(focalPx));
}

namespace {

/// 列平均亮度剖面（线性域，跨 ROI 行求均值）
std::vector<float> columnProfile(const cv::Mat& luma, const cv::Rect& win)
{
    std::vector<float> prof(static_cast<size_t>(win.width), 0.0f);
    for (int c = 0; c < win.width; ++c) {
        double sum = 0.0;
        for (int r = 0; r < win.height; ++r)
            sum += static_cast<double>(luma.at<double>(win.y + r, win.x + c));
        prof[static_cast<size_t>(c)] = static_cast<float>(sum / static_cast<double>(win.height));
    }
    return prof;
}

/// 把 ROI 钉到边缘上：在**期望位置 ±16px** 的窗内取列剖面最陡处（全图 argmax 会被白板上的
/// 黑杆抢走——TAA 开时边缘被时域累积抹平，最陡处反而是阶梯杆，实测踩过），并返回该处的
/// 列剖面跳变量（阳性锚点：跳变量 ≈0 = 根本没找到边缘）。
cv::Rect edgeRoi(const std::vector<cv::Mat>& luma, double expectedX, double& edgeX,
    double& edgeStep, double halfWidth, double rowFrac)
{
    const int w = luma[0].cols, h = luma[0].rows;
    const int cy = h / 2;
    const int halfRows = std::max(8, static_cast<int>(rowFrac * static_cast<double>(h)));
    const int y0 = std::max(0, cy - halfRows);
    cv::Rect win(0, y0, w, std::min(2 * halfRows, h - y0));
    constexpr int kSearch = 16;  ///< 期望位置 ±16px（扫程只有 4px，余量足够）
    const int x0 = std::max(1, static_cast<int>(std::lround(expectedX)) - kSearch);
    const int x1 = std::min(w - 2, static_cast<int>(std::lround(expectedX)) + kSearch);
    cv::Rect sub(x0, win.y, x1 - x0 + 1, win.height);
    const auto prof = columnProfile(luma[0], sub);
    const float local = steepestEdgeColumn(prof);
    edgeX = static_cast<double>(x0) + local;
    const size_t i = std::clamp(static_cast<size_t>(std::lround(local)), size_t{ 1 }, prof.size() - 2);
    edgeStep = std::abs(prof[i + 1] - prof[i - 1]);
    const int hw = std::max(2, static_cast<int>(std::lround(halfWidth)));
    cv::Rect roi(std::max(0, static_cast<int>(std::lround(edgeX)) - hw), win.y, 2 * hw, win.height);
    roi.x = std::min(roi.x, w - roi.width);
    roi.y = std::min(roi.y, h - roi.height);
    return roi;
}

/// 画面里**最强梯度**处的方形窗（bistro 模式：闭式不适用，只出同口径的 ρ 做 on/off 对比）。
/// 为什么不能钉画面中心：中心可能是块平墙，ρ 恒 0 而 on/off 比是 0/0；窗口必须落在边缘带上。
/// 定位用 TAA 关那一组（无抖动），两组共用同一个窗。
cv::Rect strongestRoi(const std::vector<cv::Mat>& luma, double halfWidth, cv::Point2d& at)
{
    const int w = luma[0].cols, h = luma[0].rows;
    cv::Mat g;
    cv::Sobel(luma[0], g, CV_64F, 1, 0, 1);  // |dx|（垂直边缘是本项目的主战场）
    double best = 0.0;
    int bx = w / 2, by = h / 2;
    for (int y = 2; y + 2 < h; ++y)
        for (int x = 2; x + 2 < w; ++x) {
            // 3×3 局部和：单像素极值会被高光噪点抢走，边缘带要的是「一片」强梯度
            const double s = std::abs(g.at<double>(y, x)) + std::abs(g.at<double>(y, x + 1))
                + std::abs(g.at<double>(y + 1, x)) + std::abs(g.at<double>(y + 1, x + 1));
            if (s > best) {
                best = s;
                bx = x;
                by = y;
            }
        }
    at = cv::Point2d(bx, by);
    const int hw = std::max(4, static_cast<int>(std::lround(halfWidth)));
    cv::Rect roi(bx - hw / 2, by - hw / 2, hw, hw);
    roi.x = std::clamp(roi.x, 0, w - roi.width);
    roi.y = std::clamp(roi.y, 0, h - roi.height);
    return roi;
}

/// ROI 内的两端观测电平（p0.5 / p99.5）+ 时间序列 + 半谱
void measureGroup(Group& g)
{
    std::vector<double> vals;
    for (const auto& m: g.luma)
        for (int r = 0; r < g.roi.height; ++r)
            for (int c = 0; c < g.roi.width; ++c)
                vals.push_back(m.at<double>(g.roi.y + r, g.roi.x + c));
    std::sort(vals.begin(), vals.end());
    g.darkObs = percentileOf(vals, 0.005);
    g.whiteObs = percentileOf(vals, 0.995);
    g.scale = scaleFromObserved(static_cast<float>(g.darkObs), static_cast<float>(g.whiteObs));
    g.rho = frameDeltaPercentile(g.luma, g.roi, 0.995);
    for (const auto& m: g.luma) {
        double sum = 0.0;
        for (int r = 0; r < g.roi.height; ++r)
            for (int c = 0; c < g.roi.width; ++c)
                sum += m.at<double>(g.roi.y + r, g.roi.x + c);
        g.signal.push_back(static_cast<float>(sum / static_cast<double>(g.roi.width * g.roi.height)));
    }
    g.spectrum = hannHalfSpectrum(g.signal);
    g.anchor = flickerClosedForm(static_cast<float>(g.sweepLocal), g.scale);
}

}  // namespace

float displayChain(float coverage, const FlickerScale& scale)
{
    // 复用 imgqa 既有的「线性 HDR → 屏幕字节 → 观测亮度」唯一出处（analytic 探针判据同源），
    // 不在此手搓 ACES/sRGB —— 闭式解与像素必须走同一条链，写两份就是第二份真相。
    const float c = std::clamp(coverage, 0.0f, 1.0f);
    const double lin = static_cast<double>(scale.black) + static_cast<double>(c) * (scale.white - scale.black);
    const std::array<int, 3> bytes = toBytes(Vec3{ lin, lin, lin }, 1.0);
    return static_cast<float>(observedLuma(bytes));
}

FlickerAnchor flickerClosedForm(float sweepPx, const FlickerScale& scale)
{
    FlickerAnchor a;
    a.sweepPx = sweepPx;
    constexpr int kSteps = 8192;
    float best = 0.0f;
    for (int i = 0; i <= kSteps; ++i) {
        const float cov = static_cast<float>(i) / static_cast<float>(kSteps);
        best = std::max(best, std::abs(displayChain(cov + sweepPx, scale) - displayChain(cov, scale)));
    }
    a.idealRho = best;
    a.noAaRho = displayChain(1.0f, scale) - displayChain(0.0f, scale);
    return a;
}

FlickerScale scaleFromObserved(float darkObserved, float whiteObserved)
{
    // 链对「ACES 前线性值」单调递增 ⟹ 两端各自二分即可；40 步到 1e-12 相对精度
    auto invert = [](float observed) {
        float lo = 0.0f, hi = 16.0f;
        if (observed <= 0.0f)
            return 0.0f;
        for (int i = 0; i < 40; ++i) {
            const float mid = 0.5f * (lo + hi);
            const FlickerScale s{ mid, mid };  // 覆盖恒 1 时 chain = decode(aces(mid))
            if (displayChain(1.0f, s) < observed)
                lo = mid;
            else
                hi = mid;
        }
        return 0.5f * (lo + hi);
    };
    FlickerScale s;
    s.black = invert(darkObserved);
    s.white = invert(whiteObserved);
    if (s.white <= s.black)
        s.white = s.black + 1e-4f;  // 全平 ROI（没找到边缘）：给个非零分母，比值会爆，由调用侧判红
    return s;
}

std::vector<float> hannHalfSpectrum(const std::vector<float>& signal)
{
    const int n = static_cast<int>(signal.size());
    std::vector<float> out;
    if (n < 4)
        return out;
    double mean = 0.0;
    for (float v: signal)
        mean += static_cast<double>(v);
    mean /= static_cast<double>(n);
    std::vector<float> w(static_cast<size_t>(n), 0.0f);
    for (int k = 0; k < n; ++k) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(k) / static_cast<double>(n - 1));
        w[static_cast<size_t>(k)] = static_cast<float>((static_cast<double>(signal[static_cast<size_t>(k)]) - mean) * win);
    }
    const int half = n / 2;
    out.reserve(static_cast<size_t>(half + 1));
    for (int m = 0; m <= half; ++m) {
        double re = 0.0, im = 0.0;
        for (int k = 0; k < n; ++k) {
            const double a = 2.0 * kPi * static_cast<double>(m * k) / static_cast<double>(n);
            re += static_cast<double>(w[static_cast<size_t>(k)]) * std::cos(a);
            im -= static_cast<double>(w[static_cast<size_t>(k)]) * std::sin(a);
        }
        out.push_back(static_cast<float>(std::sqrt(re * re + im * im)));
    }
    return out;
}

double frameDeltaPercentile(const std::vector<cv::Mat>& frames, const cv::Rect& roi, double percentile)
{
    if (frames.size() < 2)
        return 0.0;
    std::vector<double> deltas;
    deltas.reserve((frames.size() - 1) * static_cast<size_t>(roi.width) * static_cast<size_t>(roi.height));
    for (size_t t = 1; t < frames.size(); ++t)
        for (int r = 0; r < roi.height; ++r)
            for (int c = 0; c < roi.width; ++c)
                deltas.push_back(std::abs(frames[t].at<double>(roi.y + r, roi.x + c) - frames[t - 1].at<double>(roi.y + r, roi.x + c)));
    std::sort(deltas.begin(), deltas.end());
    return percentileOf(deltas, percentile);
}

float steepestEdgeColumn(const std::vector<float>& profile)
{
    const int n = static_cast<int>(profile.size());
    if (n < 5)
        return static_cast<float>(n) * 0.5f;
    std::vector<float> g(static_cast<size_t>(n), 0.0f);
    for (int i = 1; i + 1 < n; ++i)
        g[static_cast<size_t>(i)] = std::abs(profile[static_cast<size_t>(i + 1)] - profile[static_cast<size_t>(i - 1)]);
    size_t best = 1;
    for (size_t i = 1; i + 1 < static_cast<size_t>(n); ++i)
        if (g[i] > g[best])
            best = i;
    const float denom = g[best - 1] - 2.0f * g[best] + g[best + 1];
    const float shift = std::abs(denom) > 1e-9f ? std::clamp(0.5f * (g[best - 1] - g[best + 1]) / denom, -1.0f, 1.0f) : 0.0f;
    return static_cast<float>(best) + shift;
}

float localSweepPxPerFrame(float yawRadPerFrame, float focalPx, float xPx, float cxPx)
{
    const float u = (xPx - cxPx) / focalPx;
    return std::abs(yawRadPerFrame) * focalPx * (1.0f + u * u);
}

float verticalFocalPx(int heightPx, float fovDeg)
{
    return static_cast<float>(static_cast<double>(heightPx) * 0.5 / std::tan(0.5 * static_cast<double>(fovDeg) * kPi / 180.0));
}

int cmdTemporalFlicker(const std::string& target, double sweepPxPerFrame, int frames,
    double halfWidth, double rowFrac, bool verbose, double kernel, bool fsr)
{
    if (sweepPxPerFrame <= 0.0 || sweepPxPerFrame >= 1.0) {
        std::printf("用法: imgqa temporal-flicker [--sweep 0.125] [--frames 32] [--target edge|doorframe|...]\n"
                    "      --sweep 须在 (0,1) px/帧：≥1 退化成整数平移，混叠根本不被激励\n");
        return 2;
    }
    if (frames < 16) {
        std::printf("--frames 至少 16（要盖住 ≥2 个闪烁周期；扫速 0.125 时一周期 = 8 帧）\n");
        return 2;
    }
    bool edgeTarget = false;
    const BistroCam* cam = nullptr;
    try {
        cam = resolveFlickerTarget(target, edgeTarget);
    }
    catch (const std::exception& e) {
        std::printf("%s\n", e.what());
        return 2;
    }

    const float focal = verticalFocalPx(kSize[1], kFovDeg);
    const double yawRad = sweepToYawRad(sweepPxPerFrame, focal);  // 主点处 dx/dψ = f
    const double yawDeg = yawRad * 180.0 / kPi;

    std::printf("=== 运动态时间闪烁（闭式解判据 temporal-flicker）===\n");
    std::printf("  目标: %s\n", edgeTarget ? "aa_target 白板左边缘（闭式**适用**，主判据）"
                                           : "bistro 交付配置 ROI（闭式只作**参考刻度**）");
    std::printf("  名义扫速 %g px/帧 → 每帧偏航 %s°（f=%.1fpx @ fov %g°）；帧数 %d；ROI 半宽 %gpx\n",
        sweepPxPerFrame, d5(yawDeg).c_str(), focal, kFovDeg, frames, halfWidth);
    // 批 13.8：输入侧重建核。**只作用于 TAA 开那一组**（TAA 关 = 图里没有 taa pass，键无消费者）
    // ⟹ TAA 关那组是「闭式无 AA 锚点」，逐次重测它 = 跨运行的可比性自证（核不该动它一根毫毛）。
    static const char* kKernelNames[3] = { "0 双线性", "1 Catmull-Rom 4×4", "2 Lanczos3 6×6" };
    const int kIdx = kernel < 0.5 ? 0 : (kernel < 1.5 ? 1 : 2);
    const std::string kTag = kernel < 0.5 ? "" : ("_k" + std::to_string(kIdx));
    std::printf("  输入侧重建核 taa.input_kernel = %s（批 13.8 A/B；只作用于 TAA 开组）\n",
        kKernelNames[kIdx]);

    // 臂表。前两条 = 既有 TAA 关/开，tag 与目录后缀**逐字不变** ⟹ 已发表读数可复现。
    // 🔴 `双线性@0.75` **不是可选对照**：`render_scale < 1` 本身就糊，而糊天然降闪烁。
    //    少了它，「FSR 闪烁比 TAA 低」会被一句「你只是把它糊没了」推翻 —— 这正是两轴规则
    //    的由来（总账已否决「L1 对模糊守恒」）。它在这里的角色 = 闪烁轴的**糊地板**。
    std::vector<FlickerArm> arms = {
        { "flicker_off", "TAA 关", Aa::Off, 1.0, false, false },
        { "flicker_on", "TAA 开", Aa::Taa, 1.0, false, false },
    };
    if (fsr) {
        arms.push_back({ "bilin75", "双线性@0.75", Aa::Off, 0.75, false, false });
        arms.push_back({ "taaa75", "TAA开@0.75", Aa::Taa, 0.75, false, false });
        arms.push_back({ "fsr75", "FSR时域@0.75", Aa::Fsr, 0.75, true, true });
        // 🔴 这两条才是鬼影轴的**同工作点**对手：α = 0.01（交付）+ clip 11（今天）/ 13（修法）。
        //    上面 `TAA 开` 用的是本仪器历史的 α = 0.12，累积窗短 12 倍 ⟹ 拿它当 TAA 的代表
        //    会系统性**低估** TAA 的闪烁抑制、**高估** FSR 的优势。
        arms.push_back({ "taaa01c11", "TAAα.01臂11", Aa::Taa, 1.0, false, false, 0.01, 11.0 });
        arms.push_back({ "taaa01c13", "TAAα.01臂13", Aa::Taa, 1.0, false, false, 0.01, 13.0 });
    }
    std::vector<Group> groups;
    for (const FlickerArm& a : arms) {
        const std::string tag = a.tag;
        const std::string dir = projectRoot() + "/build/imgqa_flicker_" + tag + kTag;
        std::printf("  采集 %s …\n", tag.c_str());
        try {
            Group g;
            static_cast<FlickerSeries&>(g) = captureFlickerSeries(dir, a.name, tag, edgeTarget, cam,
                a.aa == Aa::Taa, yawRad, frames, kernel, false, false, 1.0, 0.5, false, false, &a);
            groups.push_back(std::move(g));
        }
        catch (const std::exception& e) {
            std::printf("  [采集失败] %s\n", e.what());
            return 1;
        }
    }

    // 边缘定位跑在 **TAA 关**那一组上（无抖动 ⟹ 列剖面最干净），两组**共用同一个 ROI**
    // ——ROI 不同则 on/off 比的是两幅画面，比值没有意义。
    double edgeStep = 0.0;
    if (edgeTarget) {
        if (groups[0].frameSize != groups[1].frameSize) {
            std::printf("  ⚠ 两组尺寸不一致（%dx%d vs %dx%d），无法共用 ROI\n",
                groups[0].frameSize.width, groups[0].frameSize.height,
                groups[1].frameSize.width, groups[1].frameSize.height);
            return 1;
        }
        double edgeX = 0.0;
        const cv::Rect roi = edgeRoi(groups[0].luma, static_cast<double>(groups[0].frameSize.width) * 0.5,
            edgeX, edgeStep, halfWidth, rowFrac);
        if (edgeStep < 0.15) {
            std::printf("  ⚠ 定位到的边缘跳变量只有 %s（<0.15 线性）——ROI 里没有高对比边缘，\n"
                        "    闭式解的两端无从谈起，本组数**不可用**\n",
                d4(edgeStep).c_str());
            return 1;
        }
        const float cx = static_cast<float>(groups[0].frameSize.width) * 0.5f;
        const float sweep = localSweepPxPerFrame(static_cast<float>(yawRad), focal, static_cast<float>(edgeX), cx);
        for (auto& g: groups) {
            g.roi = roi;
            g.edgeX = edgeX;
            g.sweepLocal = sweep;
        }
    }
    else {
        cv::Point2d at(0, 0);
        const cv::Rect roi = strongestRoi(groups[0].luma, halfWidth, at);
        for (auto& g: groups) {
            g.roi = roi;
            g.edgeX = at.x;
            g.sweepLocal = sweepPxPerFrame;
        }
    }
    for (auto& g: groups) {
        measureGroup(g);
        std::printf("  [%s] 渲染 %dx%d（%zu 帧）｜帧计数 %u→%u（Δ=%u，热身 %d）｜引擎回显 feedback=%s"
                    "｜相邻帧最大字节差 %.0f｜边缘x=%s 局部扫速=%s px/帧｜ROI %dx%d@%d,%d\n",
            g.name.c_str(), g.frameSize.width, g.frameSize.height, g.luma.size(), g.framePre, g.framePost,
            g.framePost - g.framePre, kWarm, d4(g.liveFeedback).c_str(), g.byteMaxAdjacent,
            d4(g.edgeX).c_str(), d4(g.sweepLocal).c_str(), g.roi.width, g.roi.height, g.roi.x, g.roi.y);
    }

    // bistro 组的平区守卫：窗口落在无对比区域时 ρ 恒 0，on/off 比是 0/0。
    if (!edgeTarget && groups[0].whiteObs - groups[0].darkObs < 0.05) {
        std::printf("  ⚠ ROI 内黑白跨度只有 %s（<0.05 线性）——窗口落在平区，闪烁无从发生，\n"
                    "    本组数**不可用**\n",
            d4(groups[0].whiteObs - groups[0].darkObs).c_str());
        return 1;
    }

    std::printf("\n  组       ρ实测      ρ闭式理想  ρ闭式无AA    G=实测/理想   ζ(0=理想,1=无AA)   黑/白观测\n");
    std::printf("  %s\n", std::string(96, '-').c_str());
    for (const auto& g: groups) {
        const double gain = g.anchor.idealRho > 0.0f ? g.rho / g.anchor.idealRho : 0.0;
        const double denom = static_cast<double>(g.anchor.noAaRho) - g.anchor.idealRho;
        const double zeta = denom > 1e-9 ? (g.rho - g.anchor.idealRho) / denom : 0.0;
        std::printf("  %-7s %-10s %-11s %-12s %-12s %-18s %s / %s\n", g.name.c_str(),
            d5(g.rho).c_str(), d5(g.anchor.idealRho).c_str(), d5(g.anchor.noAaRho).c_str(),
            d4(gain).c_str(), d4(zeta).c_str(), d4(g.darkObs).c_str(), d4(g.whiteObs).c_str());
        if (verbose) {
            std::printf("    闭式标定：ACES 前黑=%s 白=%s（观测 %s / %s）\n",
                d4(g.scale.black).c_str(), d4(g.scale.white).c_str(),
                d4(g.darkObs).c_str(), d4(g.whiteObs).c_str());
            std::printf("    ROI 均值序列(×1e4): ");
            for (size_t i = 0; i < g.signal.size(); ++i)
                std::printf(" %d", static_cast<int>(std::lround(10000.0f * g.signal[i])));
            std::printf("\n    Hann 半谱 %%AC（bin m ↔ m/N 周/帧；扫速落在 bin %d）: ",
                static_cast<int>(std::lround(g.sweepLocal * static_cast<float>(frames))));
            double tot = 0.0;
            for (size_t m = 1; m < g.spectrum.size(); ++m)
                tot += static_cast<double>(g.spectrum[m]) * g.spectrum[m];
            for (size_t m = 1; m < g.spectrum.size() && m <= 12; ++m) {
                const double p = static_cast<double>(g.spectrum[m]) * g.spectrum[m];
                std::printf(" %zu:%.1f", m, tot > 0 ? 100.0 * p / tot : 0.0);
            }
            std::printf("\n");
        }
    }

    const Group& off = groups[0];
    const Group& on = groups[1];
    // 运动激励在场性守卫（frame_yaw 键名拼错 / 门没生效 = lua 侧静默）：TAA 关且相机不动时
    // 连拍逐位相同，ρ 恒 0，任何「比理想好多少倍」的结论都是空的。首跑就是靠它抓到
    // 合成路径根本不写 camera.lua（frame_yaw 静默没生效）。
    if (off.byteMaxAdjacent < 2.0) {
        std::printf("  ⚠ TAA 关组相邻帧最大字节差仅 %.0f —— 相机**根本没动**（frame_yaw 没生效？），\n"
                    "    运动态判据失去对象，本组数**不可用**\n",
            off.byteMaxAdjacent);
        return 1;
    }
    const double ratio = off.rho > 1e-12 ? on.rho / off.rho : 0.0;
    const double idealGain = off.anchor.idealRho > 0.0f ? off.anchor.noAaRho / off.anchor.idealRho : 0.0;
    std::printf("\n  闪烁比 flicker(on)/flicker(off) = %s  （<1 = TAA 开更不闪）\n", d4(ratio).c_str());
    std::printf("  闭式刻度：理想 AA 的 G=1.0000；完全无 AA 的 G=%s（局部扫速 %s px/帧）\n",
        d4(idealGain).c_str(), d4(off.sweepLocal).c_str());

    if (groups.size() > 2) {
        std::printf("\n  ── 升频臂（闪烁轴；分母 = 同一个 TAA 关组，同一 ROI、同一激励）──\n");
        for (size_t i = 2; i < groups.size(); ++i) {
            const Group& g = groups[i];
            std::printf("    %-16s ρ %s  flicker/off %s  G=实测/理想 %s\n", g.name.c_str(),
                d5(g.rho).c_str(), d4(off.rho > 1e-12 ? g.rho / off.rho : 0.0).c_str(),
                d4(g.anchor.idealRho > 0.0f ? g.rho / g.anchor.idealRho : 0.0).c_str());
        }
        std::printf("    🔴 读法：FSR 要算「买了东西」，必须**同时**不劣于「双线性@0.75」（糊地板）\n"
                    "       且优于「TAA 开」。只赢后者而输前者 = 买到的只是分辨率损失的模糊，\n"
                    "       不是时域滤波 —— 两轴规则在闪烁轴上的这一侧。\n");
    }
    std::printf("\n  ⚠ 高频带能量比已实测否决（理想 [0.15,0.60] 与无 AA [0.00,0.86] 重叠，换 τ = 换占空比）；\n"
                "    FFT 只作诊断，判据是「每帧变化 ÷ 闭式理想」。见 temporal_flicker.h 头注释。\n");
    return 0;
}

}  // namespace RGL::imgqa
