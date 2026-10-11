// temporal-tail --motion step —— **平移阶跃**鬼影衰减曲线（批次 B11 从
// `temporal_motion.cpp` 拆出：同一文件里的**第五台仪器**）。设计全文 = 下面原样搬来的头注释。
// FSR 臂集在 `temporal_step_fsr.cpp`（缝 = 同一台仪器的 TAA 臂 vs FSR 臂）。
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

// ═══════════════════════════════════════════════════════════════════════════
//  temporal-tail --motion step —— **平移阶跃**鬼影衰减曲线（债-63 的真鬼影量具）
// ═══════════════════════════════════════════════════════════════════════════
//
// 【为什么必须新造一台，而不是给 --motion camera 补个参数】
// 前两台仪器**结构上量不到鬼影**：
//  · `temporal-residual`（六机位 ρ 那一族）—— `runBistroGroup` 是 `setCamera` 一次 + 连拍，
//    机位在整组里**钉死**，唯一运动是 TAA 抖动 ⟹ 量的是静止机位逐帧闪烁。
//  · `--motion camera` —— `frame_yaw` 是**原地纯旋转** ⟹ 无视差 ⟹ 无去遮挡。
// 而鬼影的定义就是「让开的内容回不来」。所以 ρ 全绿 + tail 全绿 与「肉眼有鬼影」**不矛盾**，
// 那是两套量具的公共盲区（`phase13_plan.md` §1.17 结论 6 已把这句话写在账上）。
//
// 【协议】每臂三跑，全部 jitter_scale = 0：
//   A  = 在机位 A 收敛后连拍（历史 = A 的参考画面）
//   B∞ = 在机位 B 收敛后连拍（**同一臂配置**的收敛态，不是 TAA 关的图）
//   S  = 在 A 热身 → 中途 `POST /camera` 瞬切到 B → 连拍 ⟹ 第 n 帧的历史里掺着 A 的内容
//   `ghost(n) = mean_mask |S_n − B∞| ÷ mean_mask |A − B∞|`，掩膜 = |A − B∞| > τ 再内腐蚀。
//
// 【为什么 jitter 恒 0】鬼影是**低频大幅度**现象（一整个遮挡物的颜色留在原地），抖动是
// 亚像素高频。关掉它换来三件更值钱的东西：① 静止机位收敛帧**逐位相同** ⟹ B∞ 是个确定的
// 固定点，不需要相位配对；② 零锚（α=1）残差**恰为 0**；③ 分母不再掺相位噪声。
// 代价 = 不激励抗锯齿本身，而本仪器从不判抗锯齿（那是 `aa_target` 那族的活）。
//
// 【闭式锚点 = 仪器自证】门关（`clip_mode = 0`）时 `out = lerp(hist, cur, α)` 且 hist 恒为
// A 的收敛值 ⟹ `ghost(n) = (1−α)ⁿ` **精确** ⟹ n₅₀ = `ln0.5/ln0.99` = 69 帧（α=0.01）。
// 门关臂必须**精确**落在它上面，否则说明历史通道上还有别的东西在动 ⟹ 整表作废。
//
// 【一条会造假绿的坑】引擎有跳切判据 `cameraJumps`（`rgl/taa_jitter.cppm`）：单帧位移
// > 0.25·sceneRadius ⟹ `historyValid = 0` ⟹ 历史**直接作废** ⟹ 残影恒 0 = 满分假绿。
// 本仪器不读那个阈值（读的是 CPU 侧 `bounds.radius`，imgqa 拿不到），改成**当场量**：
// 门关臂的 `ghost(0)` 必须 ≥ 0.5（纯 EMA 预测 = 0.99）。掉到 0 = 这一刀被引擎当跳切切了，
// 判决线 `temporal-tail-step-stimulus` 红，整表不许读。


/**
 * 沿相机 right 轴纯横移（= 带视差的平移，且**朝向不变** ⟹ `cameraJumps` 的 45° 夹角判据
 * 天然通过，只剩位移判据要防）。target 跟着平移同样的向量，否则会变成「绕着目标转」= 旋转。
 */
BistroCam strafedCam(const BistroCam& c, double dist)
{
    const std::array<double, 3> p = c.position, t = c.target;
    std::array<double, 3> f = { t[0] - p[0], t[1] - p[1], t[2] - p[2] };
    const double n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (n <= 0.0)
        throw std::runtime_error(std::string("机位 ") + c.name + "：position == target，无从定朝向");
    for (double& v : f)
        v /= n;
    // right = forward × up（up = 世界 +Y）⟹ (f.z, 0, −f.x)。退化只在俯视正下方出现，
    // 本表六个机位都不是。
    const std::array<double, 3> r = { f[2], 0.0, -f[0] };
    const double rn = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rn < 1e-9)
        throw std::runtime_error(std::string("机位 ") + c.name + "：朝向与世界 up 平行，横移轴退化");
    BistroCam b = c;
    for (int i = 0; i < 3; ++i) {
        const double d = dist * r[i] / rn;
        b.position[i] = c.position[i] + d;
        b.target[i] = c.target[i] + d;
    }
    return b;
}

/**
 * 跑一程：在 `from` 收敛 → （`to` 不同则）中途瞬切 → 连拍。
 *
 * ⚠ 与 `collectGroup` **不共用**的唯一理由 = 中途那次 `setCamera`。其余口径逐字对齐：
 * 交付配置目录（红线 3）、`prepareLinear`（红线 1，不另写解码）、尺寸自证（红线 2）。
 */
StepRun captureStepRun(const std::string& dir, const std::string& name, const std::string& tag,
    const BistroCam& from, const BistroCam& to, const Spec& sp, int burst)
{
    StepRun r;
    r.name = name;
    r.warm = sp.warm;

    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";
    o.hasCamera = true;
    o.cameraPosition = from.position;
    o.cameraTarget = from.target;
    o.cameraFov = from.fov;
    o.aa = sp.aa;
    o.taaFeedback = sp.feedback;
    o.taaJitterScale = sp.jitterScale;  // 本仪器恒 0（理由见段头【为什么 jitter 恒 0】）
    o.taaBaseSpace = sp.baseSpace;
    o.taaReprojection = sp.reprojection;  // 闭式锚臂 = false（纯屏幕空间 EMA）
    o.taaClipMode = sp.aa == Aa::Taa ? sp.clipMode : 0.0;
    o.taaClipSoft = sp.clipSoft;
    // 升频 / FSR 臂（默认全关 = 不写键）。批 14.6：TAA 与 FSR 时域**写不出同时开**（aa 单槽位），
    // 旧的「守卫拒绝组合」随之删除；velocity 由引擎在 aa=fsr 时隐含打开，这里不再代开。
    o.renderScale = sp.renderScale;
    o.fsrTemporal = sp.fsrTemporal;
    o.fsrJitter = sp.fsrJitter;
    makeProfile(dir, o);

    Engine eng(dir);
    eng.setCamera({ from.position, from.target, from.fov });
    eng.advanceFrames(static_cast<uint32_t>(sp.warm));
    const bool stepped = from.position != to.position || from.target != to.target;
    if (stepped)
        eng.setCamera({ to.position, to.target, to.fov });
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(burst), tag + "_s");

    for (const auto& path : files) {
        cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
        if (img.empty())
            throw std::runtime_error(name + "：读不到 " + path);
        cv::Mat bgr;
        if (img.channels() == 4) {  // 与 collectGroup 同口径：只取 BGR，alpha 另作他用
            cv::Mat ch[4];
            cv::split(img, ch);
            const std::vector<cv::Mat> bgrCh = { ch[0], ch[1], ch[2] };
            cv::merge(bgrCh, bgr);
        }
        else
            bgr = img;
        if (bgr.size() != cv::Size(kSize[0], kSize[1]))
            throw std::runtime_error(name + "：渲染 " + std::to_string(bgr.cols) + "x"
                + std::to_string(bgr.rows) + " ≠ 期望 1280x720（红线 2）");
        Frame f;
        f.camera = { to.position, to.target, to.fov };
        f.bgr = bgr;
        prepareLinear(f);  // 红线 1：复用，不另写解码
        r.frames.push_back(f.luma.clone());
    }
    if (r.frames.empty())
        throw std::runtime_error(name + "：/shotburst 一张都没拿到");
    std::printf("  [%s] %s 帧 %zu（热身 %d，α=%s，clip=%s）\n", name.c_str(),
        stepped ? "阶跃" : "收敛", r.frames.size(), sp.warm, d2(sp.feedback).c_str(),
        d2(sp.clipMode).c_str());
    return r;
}

/// 收敛参考 = 末 `kConvergedAvg` 帧均值（抖动关断 ⟹ 这些帧本应逐位相同）。
cv::Mat convergedRef(const StepRun& r)
{
    const int k = std::min<int>(kConvergedAvg, static_cast<int>(r.frames.size()));
    cv::Mat acc = cv::Mat::zeros(r.frames.front().size(), CV_64FC1);
    for (size_t i = r.frames.size() - static_cast<size_t>(k); i < r.frames.size(); ++i)
        cv::add(acc, r.frames[i], acc);
    acc /= static_cast<double>(k);
    return acc;
}

/**
 * 阶跃掩膜 + 分母。**抽出来的理由**：`stepCurveOf`（分子）与 `convergedFloor`（地板污染）
 * 必须用**同一个算子**折，否则「地板占 ghost 的百分之几」这句话没有意义。
 * 掩膜 = `|A − B∞| > τ` 再内腐蚀 `margin`；分母 = 掩膜上的 `mean|A − B∞|`。
 */
cv::Mat stepMask(const cv::Mat& aRef, const cv::Mat& bRef, double tau, int margin, double* den,
    double* share)
{
    cv::Mat jump;
    cv::absdiff(aRef, bRef, jump);
    cv::Mat m;
    cv::compare(jump, tau, m, cv::CMP_GT);
    if (margin > 0) {
        const cv::Mat k = cv::getStructuringElement(cv::MORPH_RECT,
            { 2 * margin + 1, 2 * margin + 1 });
        cv::erode(m, m, k);
    }
    const double cnt = static_cast<double>(cv::countNonZero(m));
    if (share)
        *share = cnt / static_cast<double>(m.total());
    if (den) {
        cv::Mat w;
        jump.copyTo(w, m);
        *den = cnt > 0.0 ? cv::sum(w)[0] / cnt : 0.0;
    }
    return m;
}

/**
 * 收敛态「不是固定点」给鬼影分子带来的**地板**，单位 = ghost 本身（已除以分母）。
 *
 * 🔴 **为什么不是全图 max**（首轮实测的教训，全文见判据注册表同名条目）：max-norm 在 F75 上报
 * 0.83、看着像「FSR 输出根本不稳定」，但全图**均值**偏离只有 0.00075，那 0.83 来自 0.166% 的
 * 硬边缘像素（线性域梯度 0.95/px，亚像素相位一变就在黑白之间翻）。按鬼影掩膜与同一个分母折
 * 一遍，真实污染是 0.0010 ghost 单位 —— max-norm 把它夸大了 **700 倍**。
 * 本函数的算子与分子**逐字相同**（掩膜均值 ÷ 分母），所以它的读数可以直接和 ghost(n) 比。
 */
double convergedFloor(const StepRun& r, const cv::Mat& ref, const cv::Mat& mask, double den)
{
    const double cnt = static_cast<double>(cv::countNonZero(mask));
    if (cnt <= 0.0 || den <= 0.0)
        return 0.0;
    const int k = std::min<int>(kConvergedAvg, static_cast<int>(r.frames.size()));
    double acc = 0.0;
    for (size_t i = r.frames.size() - static_cast<size_t>(k); i < r.frames.size(); ++i) {
        cv::Mat d, dm;
        cv::absdiff(r.frames[i], ref, d);
        d.copyTo(dm, mask);
        acc += cv::sum(dm)[0] / cnt;
    }
    return acc / static_cast<double>(k) / den;
}

/// 收敛态**内部**的最大逐像素偏离（末 `kConvergedAvg` 帧各自 vs 其均值取最大）。
///
/// 为什么 FSR 臂必须有这条：整台仪器把「末 16 帧的均值」当 B∞ 固定点用，而这个假设是从
/// TAA + `jitter_scale = 0` 那里**借**来的。FSR 臂的抖动是 vendor 自己的相位序列（周期
/// = `8·(上屏宽/渲染宽)²`，0.75 档实测 **14**），而 16 恰好是 TAA 的抖动周期 —— 也就是说
/// 「取 16 帧 = 取满一个相位周期」这件事对 FSR 臂不自动成立。
/// ⚠ 本函数**只作诊断**（它抓的是「输出有没有亚像素不稳定」），闸门用 `convergedFloor`。
double convergedSpread(const StepRun& r)
{
    const cv::Mat m = convergedRef(r);
    const int k = std::min<int>(kConvergedAvg, static_cast<int>(r.frames.size()));
    double mx = 0.0;
    for (size_t i = r.frames.size() - static_cast<size_t>(k); i < r.frames.size(); ++i) {
        cv::Mat d;
        cv::absdiff(r.frames[i], m, d);
        mx = std::max(mx, static_cast<double>(cv::norm(d, cv::NORM_INF)));
    }
    return mx;
}

/// 残影掉到 level 的第一帧（线性插值）。
/// 🔴 **起点必须高于 level** 才谈得上「掉下来」：起点就低于 level 时曲线压根没有下降沿，
/// 插值会给出**负帧数**（实测 −1848 帧，把「历史被跳切切没了」伪装成一个荒谬的尾长）。
/// 起点不高于 level = 与「全程没掉到 level 以下」同样返回 −1，由判决线各自解释。
double firstBelow(const std::vector<double>& g, double level)
{
    if (g.empty() || g.front() <= level)
        return -1.0;
    for (size_t n = 1; n < g.size(); ++n) {
        if (g[n] > level)
            continue;
        const double d = g[n - 1] - g[n];
        if (d <= 0.0)
            return static_cast<double>(n);
        return static_cast<double>(n - 1) + (g[n - 1] - level) / d;
    }
    return -1.0;
}

/**
 * 算一条衰减曲线。
 *
 * 掩膜 = `|A − B∞| > τ` **再向内腐蚀** `margin`。腐蚀排掉的是「新内容从画外入画」——
 * 那不是鬼影（历史里根本没有那块的内容），把它算进分母会把所有臂一起稀释成同一个数。
 * 可见性阈值用**逐像素** `lsbLinearAtLuma(B∞)`：同一个线性差在白处不到 1 LSB、在暗处好几
 * LSB，拍一个线性绝对数就是讨价还价的阈值（闭式出处 → `temporal_residual.h`）。
 */
StepCurve stepCurveOf(const StepArm& arm, const cv::Mat& aRef, const cv::Mat& bRef,
    const StepRun& run, double tau, int margin)
{
    StepCurve c;
    c.tag = arm.tag;

    // 掩膜与分母走 `stepMask`（与 `convergedFloor` 共用同一算子，见彼处理由）。
    const cv::Mat m = stepMask(aRef, bRef, tau, margin, &c.den, &c.maskShare);
    const double cnt = static_cast<double>(cv::countNonZero(m));
    if (cnt <= 0.0)
        throw std::runtime_error(std::string("臂 ") + arm.tag
            + "：掩膜为空（平移没造出足够去遮挡，或 τ 太大）");

    cv::Mat lsb(bRef.size(), CV_64FC1);
    for (int y = 0; y < bRef.rows; ++y) {
        const double* b = bRef.ptr<double>(y);
        double* d = lsb.ptr<double>(y);
        for (int x = 0; x < bRef.cols; ++x)
            d[x] = lsbLinearAtLuma(b[x]);
    }

    cv::Mat d, dm, over, both;
    for (const auto& fr : run.frames) {
        cv::absdiff(fr, bRef, d);
        d.copyTo(dm, m);
        c.g.push_back(cv::sum(dm)[0] / cnt / (c.den > 0.0 ? c.den : 1.0));
        cv::compare(d, lsb, over, cv::CMP_GT);
        cv::bitwise_and(over, m, both);
        c.vis.push_back(static_cast<double>(cv::countNonZero(both)) / cnt);
    }
    c.auc = std::accumulate(c.g.begin(), c.g.end(), 0.0);
    c.visAuc = std::accumulate(c.vis.begin(), c.vis.end(), 0.0);
    c.n50 = firstBelow(c.g, 0.5);
    c.n5 = firstBelow(c.g, 0.05);
    return c;
}

void printStepCurve(const StepCurve& c)
{
    std::printf("  %-4s 掩膜 %s%%  分母 %s  **AUC %s 帧**  可见AUC %s 帧", c.tag.c_str(),
        d2(c.maskShare * 100.0).c_str(), d4(c.den).c_str(), d2(c.auc).c_str(),
        d2(c.visAuc).c_str());
    if (c.n50 >= 0.0)
        std::printf("  n₅₀ %s  n₅ %s", d2(c.n50).c_str(), d2(c.n5).c_str());
    if (c.staticDiff >= 0.0)
        std::printf("  静止侧偏离臂11 %s", d4(c.staticDiff).c_str());
    std::printf("\n");
    std::printf("       ghost(n):");
    for (int n : kStepProbes) {
        if (n >= static_cast<int>(c.g.size()))
            break;
        std::printf(" %d=%s", n, d2(c.g[static_cast<size_t>(n)]).c_str());
    }
    std::printf("\n       可见%    :");
    for (int n : kStepProbes) {
        if (n >= static_cast<int>(c.vis.size()))
            break;
        std::printf(" %d=%s", n, d2(c.vis[static_cast<size_t>(n)] * 100.0).c_str());
    }
    std::printf("\n");
}

/**
 * 四条判决（跑前写死，红一条即不许报臂间结论）。
 *
 * ①②③ 是**量具守卫**，不判画质；④ 才是结论。
 *
 * 🔴 **②③ 评的是「关盒」臂（`clip_mode = 1`），不是 `clip_mode = 0`**。首轮实测踩过：
 * `clip_mode = 0` **不是没有裁剪**，它带默认那条 5 抽头十字盒（关盒是臂 1，见
 * `cmdTemporalTailCamera` 的 `B12/B01` 注释「关盒 + 短窗」）。拿臂 0 当「纯 EMA」去要
 * `ghost(0) = 1−α`，实测拿到 0.015 —— 那不是历史被跳切切掉，**是盒在第一帧就把历史夹进了
 * 当前邻域盒**（AAO 本该有的行为）。闭式锚因此必须挂在真正无裁剪的臂上。
 * 这次误读本身成了本判据存在的理由的一个例证：**没有闭式锚，"0.015" 会被读成"鬼影没了"。**
 */
int emitStepVerdicts(const std::string& cameraName, double stepDist, int burst, double tau,
    int margin, const StepCurve& zero, const StepCurve& ema, const StepCurve& boxed,
    const StepCurve& delivery, const StepCurve& fix, bool* guardsOkOut = nullptr)
{
    constexpr double kAlpha = 0.01;
    std::vector<Verdict> vs;
    // ① 零锚：α=1 ⟹ `lerp(·, cur, 1) ≡ cur` ⟹ 阶跃后每一帧都**就是** B∞ ⟹ 残差恒 0。
    //    非零 ⟹ 分子算子或参考构造坏了（B∞ 取错机位、把两跑之间的系统差算进残差等）。
    double zmax = 0.0;
    for (double v : zero.g)
        zmax = std::max(zmax, v);
    vs.push_back(verdictFor("temporal-tail-step-zero", zmax <= 0.02,
        "max ghost(n) (α=1) = " + d4(zmax) + "（线 ≤ 0.02；α=1 无历史 ⟹ 应恰为 0）"));
    std::cout << "  " << vs.back().line() << "\n";

    // ② 激励在场 = **历史真的活过了这一刀**。引擎的 `cameraJumps` 一旦判跳切就把历史作废，
    //    残影当场归零 = 满分假绿，除本条没有任何暴露途径。
    //    E 臂（关盒 + **无重投影**）下预测是**尖锐**的：hist 按屏幕坐标原样搬 ⟹
    //    out_0 = 0.99·A + 0.01·B ⟹ ghost(0) = 0.99。线取 0.9（留量给 PNG 量化与掩膜边缘）。
    const double g0 = ema.g.empty() ? 0.0 : ema.g.front();
    vs.push_back(verdictFor("temporal-tail-step-stimulus", g0 >= 0.9,
        "闭式锚臂 ghost(0) = " + d4(g0) + "（线 ≥ 0.900；无裁剪 + 无重投影 ⟹ 预测 1−α = "
            + d2(1.0 - kAlpha)
            + "。掉到 0 = 这一刀被引擎当**跳切**切了，历史作废，整表作废）· 掩膜 "
            + d2(ema.maskShare * 100.0) + "% · 平移 " + d4(stepDist) + " 世界单位"));
    std::cout << "  " << vs.back().line() << "\n";

    // ③ 闭式锚 = 仪器自证：E 臂 ⟹ out_n = (1−α)ⁿ·A + (1−(1−α)ⁿ)·B∞ ⟹ ghost(n) = (1−α)ⁿ
    //    **精确** ⟹ AUC = Σ = (1−(1−α)^N)/α。用 AUC 而不是 n₅₀：AUC 吃整条曲线，
    //    任何「历史中途被重置 / α 没生效」都会把它拉离闭式，而 n₅₀ 只看一个穿越点。
    const double aucClosed = (1.0 - std::pow(1.0 - kAlpha, static_cast<double>(burst))) / kAlpha;
    vs.push_back(verdictFor("temporal-tail-step-ema-anchor",
        ema.auc >= 0.9 * aucClosed && ema.auc <= 1.1 * aucClosed,
        "闭式锚臂 AUC = " + d2(ema.auc) + " 帧·全对比度（闭式 (1−(1−α)^N)/α = " + d2(aucClosed)
            + "，N=" + std::to_string(burst) + "，线 ±10%）· ghost(0) = " + d4(g0)
            + " · 不命中 = 历史通道上还有别的东西在动"));
    std::cout << "  " << vs.back().line() << "\n";

    // ④ 主判据（**结论，不是守卫**）：交付臂（臂 11）的残影总量必须显著少于现行默认盒（臂 0）。
    //    线 0.7 = 「至少兑掉三成」，与本项目「明显」常量 1.5 互为倒数口径。
    //    > 1 ⟹ 交付臂**更鬼**。首轮实测就是这个方向，而且机制对得上：瞬切后机位静止 ⟹
    //    `motion` 恰为 0 ⟹ `gate = 1` ⟹ `mem = max(ext, memPrev·decay)` 是**被撑大的记忆**
    //    ⟹ 接受盒比臂 0 的瞬时 5 抽头盒更宽 ⟹ 夹得更松。
    const double ratio = boxed.auc > 0.0 ? delivery.auc / boxed.auc : 0.0;
    vs.push_back(verdictFor("temporal-tail-step-clip-holds",
        boxed.auc > 0.0 && ratio <= 0.7,
        "AUC 交付(臂11)/现行盒(臂0) = " + d2(delivery.auc) + " / " + d2(boxed.auc) + " = "
            + d4(ratio) + "（线 ≤ 0.7 = 记忆门在去遮挡处多兑掉三成；> 1 = 交付臂**更鬼**）\n"
            "       归因：闭式锚（无裁剪+无重投影）= " + d2(ema.auc) + " 帧 ⟹ 重投影+盒族共兑掉 "
            + d2(100.0 * (1.0 - boxed.auc / (ema.auc > 0.0 ? ema.auc : 1.0)))
            + "%（R 行 = 只开重投影的单独贡献）\n       可见鬼影总量：交付 " + d2(delivery.visAuc)
            + " 帧·满掩膜 vs 现行盒 " + d2(boxed.visAuc) + " vs 闭式锚 " + d2(ema.visAuc)));
    std::cout << "  " << vs.back().line() << "\n";

    // ⑤ **修法验收（批 13.15b，臂 13 = 债-69 的出口）**：两条 AND，一条都不能少。
    //    (a) 鬼影侧要真的赢：`AUC(臂13) ≤ 0.7 × AUC(臂0)`（= ④ 给臂 11 定的同一条线，
    //        不是给臂 13 放水）。预期 ≈0：去遮挡**就发生在 `motion` 大的那一帧** ⟹ 那一帧
    //        `gate = 0` ⟹ α = 1 ⟹ 历史当场冲掉 ⟹ 后面没有能拖出来的东西。
    //    (b) 闪烁侧**不许动**：`staticDiff(臂13, 臂11) ≤ 0.02`。理论值是 **0 精确**
    //        （`motion < 1px` ⟹ `2 − motion > 1` ⟹ saturate 精确返回 1.0 ⟹ `1 − gate = +0`
    //        ⟹ `α_eff ≡ α_long`）。线取 0.02 ≈ 跨进程确定性地板（1 LSB ≈ 4e-3，债-61）的 5 倍，
    //        留量给 PNG 量化；它要抓的是「α 被换了个数」那种 **1e-1** 量级的接线错。
    //    🔴 (b) 是本判据的全部价值所在：只测 (a) 的话，「把 α 恒置 1」也满分，而那是关掉 TAA。
    const bool fixAuc = boxed.auc > 0.0 && fix.auc <= 0.7 * boxed.auc;
    const bool fixStatic = fix.staticDiff >= 0.0 && fix.staticDiff <= 0.02;
    vs.push_back(verdictFor("temporal-tail-step-arm13-fixes", fixAuc && fixStatic,
        "臂 13（α 也吃 gate）鬼影 AUC = " + d2(fix.auc) + "（线 ≤ 0.7 × 现行盒 "
            + d2(0.7 * boxed.auc) + "）· 静止侧与臂 11 最大偏离 = " + d4(fix.staticDiff)
            + "（线 ≤ 0.02，理论 0 精确；> 0.02 = 闪烁轴被动了，本臂的『免费』前提不成立）\n"
            "       可见鬼影总量：臂13 " + d2(fix.visAuc) + " vs 臂11 " + d2(delivery.visAuc)
            + " vs 现行盒 " + d2(boxed.visAuc) + " 帧·满掩膜"));
    std::cout << "  " << vs.back().line() << "\n";

    logCall("temporal-tail",
        { { "camera", cameraName }, { "motion", "step" }, { "step", d4(stepDist) },
            { "burst", std::to_string(burst) }, { "tau", d4(tau) },
            { "margin", std::to_string(margin) }, { "mask-share", d4(ema.maskShare) },
            { "auc-ema", d2(ema.auc) }, { "auc-boxed", d2(boxed.auc) },
            { "auc-delivery", d2(delivery.auc) }, { "visauc-delivery", d2(delivery.visAuc) },
            { "auc-fix", d2(fix.auc) }, { "staticdiff-fix", d4(fix.staticDiff) } },
        vs);
    // 🔴 守卫与结论**分开判**（本轮踩过：④ 红时打印「先修仪器」是错的——④ 是结论本身）。
    //    ①②③ 红 = 仪器坏了 ⟹ 臂间数字一个都不许报。
    //    ④⑤ 红 = 仪器好、结论难看 ⟹ 那正是要报的东西，改判决线才是事故。
    const bool guardsOk = vs[0].ok && vs[1].ok && vs[2].ok;
    // 量具状态单独交出去：调用方要用它决定「要不要接着在 FSR 臂上读数」。
    // ⚠ 不能拿本函数的返回值当量具状态 —— 返回值把**臂间结论**（④⑤）也折进去了，
    //   而「结论是红」与「量具是坏」是两件事，混在一起会把一次真发现误判成仪器故障。
    if (guardsOkOut)
        *guardsOkOut = guardsOk;
    std::printf("\n  ⟹ %s\n", guardsOk
            ? "守卫全绿：仪器可信，④⑤ 的比值即臂间结论本身（红 = 结论，不是仪器坏）"
            : "守卫红 ⟹ **不许报臂间结论**（先修仪器，别改判决线）");
    const bool allOk = std::all_of(vs.begin(), vs.end(), [](const Verdict& v) { return v.ok; });
    return allOk ? 0 : 1;
}

}  // namespace tmotion

int cmdTemporalTailStep(const std::string& cameraName, double stepDist, int burst, int warm,
    double tau, int margin, bool fsr)
{
    const BistroCam* base = nullptr;
    for (const BistroCam& c : kBistroCams)
        if (cameraName == c.name)
            base = &c;
    if (!base) {
        std::fprintf(stderr, "机位不存在: %s（可选 %s）\n", cameraName.c_str(),
            bistroCamNames().c_str());
        return 2;
    }
    const double dist = stepDist > 0.0 ? stepDist : 0.02;
    const BistroCam camB = strafedCam(*base, dist);
    const std::string dir = projectRoot() + "/build/imgqa_tailstep_" + cameraName;
    std::printf("平移阶跃：沿 right 轴移 %s 世界单位（target 同步平移 = 纯横移不转朝向）\n",
        d4(dist).c_str());

    // 六臂。Z = 零锚（α=1 ⟹ 无历史）。
    // E  = **闭式锚**：关盒 + **重投影关** ⟹ `hist(p) ≡ out_{n−1}(p)` 精确 ⟹
    //      `ghost(n) = (1−α)ⁿ` 精确 ⟹ AUC 有闭式。它是**尺子的刻度**，不参与臂间比较。
    //      ⚠ 为什么必须关重投影：0.01 世界单位的平移在像素上是 ~12px，重投影开着时
    //      `A(p+12px) ≈ B(p)` 对非去遮挡像素成立 ⟹ 那部分变化被重投影"解释掉" ⟹
    //      `ghost(0)` 实测只有 0.271（= **去遮挡份额**，本身是个好数，但不是 EMA 闭式）。
    // R  = 关盒 + 重投影开 = 「重投影单独兑掉了多少」的归因参照。
    // C0 = 现行默认盒（5 抽头十字盒，**不是**没有裁剪）；C11 = 交付（记忆门挂重投影位移）。
    // C13 = 批 13.15b 候选（债-69 修法：α 也吃 gate）。它与 C11 的两个**静止**收敛态
    //      应当只差确定性地板 ⟹ 顺手把「闪烁轴逐位不变」也量了（见 staticDiff）。
    const std::vector<StepArm> arms = {
        { "Z", 11.0, 1.0, true },
        { "E", 1.0, 0.01, false },
        { "R", 1.0, 0.01, true },
        { "C0", 0.0, 0.01, true },
        { "C11", 11.0, 0.01, true },
        { "C13", 13.0, 0.01, true },
    };
    std::vector<StepCurve> main;
    std::vector<std::vector<double>> sens;
    std::vector<cv::Mat> aRefs, bRefs;
    for (const StepArm& a : arms) {
        StepArmResult r = collectStepArm(dir, *base, camB, a, burst, warm, tau, margin);
        main.push_back(std::move(r.main));
        sens.push_back(std::move(r.sens));
        aRefs.push_back(std::move(r.aRef));
        bRefs.push_back(std::move(r.bRef));
    }
    // 臂 13 vs 臂 11 在静止侧的偏离（= 「α 吃 gate」这条线在 motion < 1px 时是否**真的**没动）。
    // ⚠ 跨进程 ⟹ 不是 bit 级判据（债-61 地板 = 1 LSB ≈ 4e-3 线性）；要抓的是 1e-1 量级的接线错。
    const size_t n11 = 4, n13 = 5;
    main[n13].staticDiff = std::max(staticDiff(aRefs[n11], aRefs[n13]),
        staticDiff(bRefs[n11], bRefs[n13]));

    std::printf("\n── 平移阶跃残影衰减（τ=%s，腐蚀 %d px，连拍 %d 帧）──\n",
        d2(tau * 100.0).c_str(), margin, burst);
    for (const auto& c : main)
        printStepCurve(c);
    std::printf("       τ 敏感性（可见像素占比 @n=16）：");
    for (size_t i = 0; i < main.size(); ++i) {
        std::printf("\n         臂 %-4s", main[i].tag.c_str());
        for (size_t j = 0; j < sens[i].size(); ++j)
            std::printf(" τ=%s→%s%%", d2(kStepTauSweep[j] * 100.0).c_str(),
                d2(sens[i][j] * 100.0).c_str());
    }
    std::printf("\n");

    bool taaGuardsOk = false;
    const int taaRc = emitStepVerdicts(cameraName, dist, burst, tau, margin, main[0], main[1],
        main[3], main[4], main[5], &taaGuardsOk);
    if (!fsr)
        return taaRc;
    // 六条 TAA 臂**照跑**，不是浪费：Z（零锚）与 E（闭式锚 `ghost(n) = (1−α)ⁿ`）是量具的
    // 刻度，FSR 臂要用同一把尺 ⟹ 刻度必须在**同一次运行**里自证。它们红了就不往下读。
    return runFsrStepArms(dir, *base, camB, burst, warm, tau, margin, taaGuardsOk) == 0
        ? taaRc
        : 1;
}


}  // namespace RGL::imgqa
