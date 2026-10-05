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
    bool edgeTarget, const BistroCam* cam, bool taaOn, double yawRadPerFrame, int frames)
{
    FlickerSeries g;
    g.name = name;
    ProfileOpts o;
    o.size = { kSize[0], kSize[1] };
    o.taa = taaOn;
    o.taaFeedback = kTaaFeedback;
    o.taaJitterScale = 1.0;
    o.cameraFrameYaw = yawRadPerFrame;  // rad/帧（键名 = camera.lua `frame_yaw`）
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
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(frames), tag + "_c");
    g.framePost = eng.frameCount();
    if (files.empty())
        throw std::runtime_error(name + "：/shotburst 一张都没拿到");

    cv::Mat prev;
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
        prepareLinear(f);  // 复用，不另写解码
        g.luma.push_back(f.luma.clone());
    }
    g.frameSize = g.luma.front().size();
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
    double halfWidth, double rowFrac, bool verbose)
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

    std::vector<Group> groups;
    for (bool on: { false, true }) {
        const std::string tag = on ? "flicker_on" : "flicker_off";
        const std::string dir = projectRoot() + "/build/imgqa_flicker_" + tag;
        std::printf("  采集 %s …\n", tag.c_str());
        try {
            Group g;
            static_cast<FlickerSeries&>(g) = captureFlickerSeries(dir, on ? "TAA 开" : "TAA 关", tag,
                edgeTarget, cam, on, yawRad, frames);
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
    std::printf("\n  ⚠ 高频带能量比已实测否决（理想 [0.15,0.60] 与无 AA [0.00,0.86] 重叠，换 τ = 换占空比）；\n"
                "    FFT 只作诊断，判据是「每帧变化 ÷ 闭式理想」。见 temporal_flicker.h 头注释。\n");
    return 0;
}

}  // namespace RGL::imgqa
