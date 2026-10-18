//
// temporal_swim_metric.cpp —— `taa-swim` 的**口径层**（平移搜索 + 相位/λ 标定 + 绝对抖动传递 + 真实场景采集）
//
// 拆出本 TU 的理由见 `temporal_swim.h` 头注释（一句话：口径层是纯算子与采集、不打印判决线，
// 仪器层是 cmdTaaSwim 的阶段函数，两者不是同一条改动缝）。
//
// ⚠ 本层的 freeShift / shiftInto / lagTranslation 是**平移搜索**：边界处理（卷绕折回、双符号
// 取小、同分取谁）就是判据本身，逐字搬来的，不许"化简"。jitterDeltaPx 预测引擎抖动再抖回去，
// Halton 与周期直接取引擎真值（`import taa_jitter`）——手抄一份 = 第二份真相，漂移不报错，
// 只会让仪器静默量到另一个东西。
//
#include "temporal_swim.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

// 引擎真值：Halton 序列与抖动周期（理由同 `temporal_motion.cpp` 的对应处——预测必须同式）。
import taa_jitter;

namespace RGL::imgqa {
namespace {

/// Halton(2,3) 抖动（像素单位，值域 ±0.5）——直接取引擎真值：`kTaaJitterPeriod` 与
/// `haltonDouble` 都来自 `import taa_jitter`（此前这里是手抄字面量 + 一个内联 lambda）。
///
/// 为什么用 **double** 版而不用 shader 那条 float 路：本函数的输出要喂 `cv::warpAffine`
/// 去抖，中途收窄成 float 会改判据读数末位（判据数字不许静默变）。
///
/// ⚠ 本命令 T1 组钉「按构造必须命中预测」——它验的是**口径**（符号、尺度、周期），
///   不再是公式本身；公式现在同源，漂不了。
std::array<double, 2> jitterPredPx(uint32_t frameIndex)
{
    const uint32_t i = frameIndex % kTaaJitterPeriod + 1u;
    return {haltonDouble(i, 2u) - 0.5, haltonDouble(i, 3u) - 0.5};
}

/// **用户机位**（报障机位；`mode = fps`，position/target/fov 与任务书逐字一致）。
/// 判据跑在报障的那台机位上，否则「绿」只覆盖工具自己挑的机位。
const Engine::Camera kUserCamera{
    {1.1, 0.75, 1.05}, {0.196717, 0.034493, 0.024559}, 45.0};

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

/// Halton 预测集匹配的结果：最小 RMS 及命中它的 (φ, sx, sy)。
struct HaltonMatch {
    double rms = 1e30;
    int ph = 0, sx = 1, sy = 1;
};

/// 实测位移 vs 给定 (φ, sx, sy) 预测集的 RMS（px）。公式与累加序逐字来自原匹配循环。
double matchRms(const std::vector<AbsRow>& rows, double js, int ph, int sx, int sy)
{
    double acc = 0.0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto j = jitterDeltaPx(static_cast<uint32_t>(i) + static_cast<uint32_t>(ph), js);
        acc += std::pow(rows[i].dx - sx * j[0], 2) + std::pow(rows[i].dy - sy * j[1], 2);
    }
    return std::sqrt(acc / (2.0 * static_cast<double>(rows.size())));
}

/// 16 相位 × 逐轴双符号穷举，取 RMS 最小。迭代顺序（φ 升序、sx {1,−1}、sy {1,−1}）与严格
/// 小于的 tie-break 逐字来自原匹配循环——同分时报告哪个 (φ, sx, sy) 也是读数的一部分。
HaltonMatch matchHaltonPhase(const std::vector<AbsRow>& rows, double js)
{
    HaltonMatch m;
    // 用实测位移去匹配 Halton 预测集 {js·j(i+φ)}：16 相位 × 2 符号，取 RMS 最小
    // 符号**逐轴**搜（4 组合），不是整体一个符号：引擎 x 与 y 的符号本来就不同（y 多一次
    // Vulkan 翻转）。第一版只试整体 ±，于是永远匹配不上，实测 RMS 1.748px 被误读成
    // 「画面搬了但不按 Halton 搬」——**搜索空间少一维，结论就会反过来**。
    for (int ph = 0; ph < static_cast<int>(kTaaJitterPeriod); ++ph)
        for (int sx : {1, -1})
            for (int sy : {1, -1}) {
                const double rms = matchRms(rows, js, ph, sx, sy);
                if (rms < m.rms) {
                    m.rms = rms;
                    m.ph = ph;
                    m.sx = sx;
                    m.sy = sy;
                }
            }
    return m;
}

}  // namespace

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

/**
 * 机位**同时**给两处：profile 的 camera.lua（`ProfileOpts::hasCamera`，在 makeProfile 里
 * 落盘，管第 1 帧——RPC 落在第 2~3 帧，之前那几帧已经进了 TAA 历史，理由全文见
 * `ProfileOpts::hasCamera` 注释）与 `POST /camera`（管 RPC 口径本身也被覆盖到）。
 * 两者机位一致时第二条是幂等的。
 */

SwimRun runSwimSequence(const std::string& dir, bool taa, double feedback, double jitterScale,
    int shots, int warm, const std::string& tag, int lag,
    bool baseSpace)
{
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 交付配置原样（见 SwimRun 头注释）
    o.aa = taa ? Aa::Taa : Aa::Off;
    o.taaFeedback = feedback;
    o.taaJitterScale = jitterScale;
    o.taaBaseSpace = baseSpace;  // 批 13.2 基空间输出（false = 现行为）
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
    // 债-89：热身连拍**不锚**（PNG 即弃，历史只依赖渲染过的帧序列，起点落在第几帧无所谓），
    // 采集连拍**必须**从绝对帧号起——否则抖动相位是彩票，`temporal-swim-anchor` 的
    // lag-16 判据 PASS↔FAIL 随启动时序互换（债-89 亲验）。锚点 = 基座 + 热身：到锚帧
    // 引擎已渲染 anchor ≥ warm 帧，热身语义保持。
    const uint32_t anchor = kBurstAnchorBase + static_cast<uint32_t>(warm);
    eng.advanceToFrame(anchor - kAnchorLead);
    const auto files = eng.shotBurstAt("screenshots", static_cast<uint32_t>(shots), anchor,
        tag + "_c");

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

AbsMeasure absMeasure(const cv::Mat& ref, const std::vector<cv::Mat>& frames, double js)
{
    std::vector<AbsRow> rows;
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
    const HaltonMatch m = matchHaltonPhase(rows, js);
    st.rms = m.rms; st.ph = m.ph; st.sx = m.sx; st.sy = m.sy;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto j =
            jitterDeltaPx(static_cast<uint32_t>(i) + static_cast<uint32_t>(st.ph), js);
        const double ex = st.sx * j[0], ey = st.sy * j[1];
        st.pred += std::hypot(ex, ey);
        st.err += std::hypot(rows[i].dx - ex, rows[i].dy - ey);
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
    AbsMeasure out;
    out.rows = std::move(rows);
    out.st = st;
    return out;
}

}  // namespace RGL::imgqa
