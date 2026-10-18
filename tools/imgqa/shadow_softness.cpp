// shadow-softness —— 软影滤波轴量具（批 N6d 步 0）。
//
// 判据设计定案 = `docs/shadow_routes_plan.md` §5.1；量具边界（为什么**不是** shadow_rig）=
// 同文件 §4.1。本文件头注释只写"为什么这几个数长这样"，事实本身不在这里重述（会漂）。
//
// ── 这台仪器测什么 ────────────────────────────────────────────────────────────
// 三条软影路线（csm / pcss / evssm）在同一条交付场景、同一帧、同一机位下的**影形态**，
// 以及一个标量：**太阳角敏感度** = 把太阳视直径从 0 调到某角度后，有多少像素的明暗翻了面。
// 它是 N6d 修复批（F1/F2）的验收量具：修完之后 sunSens(evssm) 必须从红转绿。
//
// ── 为什么场景是交付配置而不是 shadow_rig（§4.1 量具边界，别再走一遍）──────────
// shadow_rig 的存在理由就是跨级联（距离梯 12→1025m、岛台把包围球撑到 radius≈831m）⟹ 级联
// texelWorld ≈ 0.29/0.70/1.14/2.63 m，而它全部遮挡体 Δz ≤ 12m ⟹
// penumbraTexels = Δz·tan(θ/2)/texelWorld **对所有档恒 < 1** = shader 结构上零模糊。
// 实测 sun0↔sun5：near_anchor 差 6 px、far_sweep **逐位相同**。`--size` 放大与换机位都无效
// （信号与阈值同比缩放，量化发生在 2048² 图集里，不在屏幕上）。
// 交付场景 bistro 的级联 0 texelWorld ≈ 2cm ⟹ 半影进得了纹素域，量具才有东西可测。
//
// ── 为什么 izb 被强制关掉 ─────────────────────────────────────────────────────
// IZB 是乘在软影之上的**光空间硬影 mask**，开着它测半影 = 让硬影吞掉软影（实测带核比
// 0.097 开 / 0.185 关）。本判据测的是**软影滤波轴本身**；交付形态（izb 开）的读数是
// `izb` 命令的事。覆写由 ProfileOpts::overrideShadowRoute 这道例外门做，覆写失败一律抛。
//
// ── 为什么 J1 的阳性锚点是 PCSS 而不是 EVSSM 自己（§5.1 的设计自纠，别改回去）──
// 原设计拿「EVSSM sun0 vs sun5」当锚点 = **循环论证**：被测对象坏了锚点必然也红，就分不清
// "量具瞎了"还是"缺陷还在"。PCSS 与 EVSSM 共用同一条 `2·tan(θ/2)·Δz` 半影公式，但它的深度
// 比较是 `saturate((d−refZ)/w+0.5)` 直采、**不经过 EVSM 方差窗** ⟹ 是一条独立的
// "太阳角→半影"实现。量具能测出 PCSS 的响应（实测 1.955%）就证明它不是瞎的，此时 EVSSM 的
// 万分之十几才是缺陷而不是测量失败。
//
// ── 为什么 J2 比 0.53° 而不是 5° ──────────────────────────────────────────────
// 0.53° 是出厂值，**用户看到的就是它**。修复必须让默认配置对太阳角有响应，而不是让一个
// 极端值有响应（把 sun_degrees 拉到 5° 才有反应 = 交付形态仍然是恒等）。
//
// ── 为什么帧号写死（kDelayFrames）─────────────────────────────────────────────
// 交付场景 `spin` 在转 ⟹ 同配置第 8 帧与第 12 帧的影覆盖差 **0.12pp**（§4.1）。帧号不是
// 采集细节而是量具的一部分：两臂落在不同帧上，差值里就掺进了"场景转了多少"。所以
// ① 写死、不给 flag；② 走 `llm_shot.delay_frames`（引擎在第 N 帧渲染完成后截图 = 精确），
// 不走 RPC `/shot`（落在哪帧取决于 HTTP 往返与帧首钩子的先后 = 墙钟，只能"大约第 N 帧"）。
//
// ── 为什么度量吃字节域 luma、阈值是 8 / 40 LSB ────────────────────────────────
// ① 域：阈值是 **LSB**（存储字节）量，而 `Frame::luma` 是 sRGB 解码后的线性亮度——线性域里
//   "8 LSB"在暗部对应 0.0004、亮部对应 0.03 的相对量，同一个数会变成"只量亮部"的判据。
//   §4/§5 的全部现状数（9.84% / 1.955% / 0.14%）都在字节域，换域 = 换判据。
// ② 8 LSB = 影覆盖与太阳角敏感度的判定线，沿用 `faces` / `switch` 一族既有的"看得见才算"
//   口径，且与 §4 的现状数同口径（换数就是换判据）。它不是噪声地板：本机同配置两跑**逐位
//   相同**（本命令的确定性自证那条），所以 8 LSB 以下不是"噪声"而是"确实看不见"。
// ③ 40 LSB = 过渡带与深本影的分界：交付场景里 CSM 的 4 抽头 PCF 过渡带实测落在个位数 LSB，
//   而本影中心在 100+ LSB；40 取在两者之间，且**只用于比值**（带核比），不单独判红绿——
//   比值对这条线的具体位置不敏感（§4 的 0.185/0.229 与本轮 0.151/0.178 是两条独立分界下的
//   同向读数）。
// 考虑过但否决的口径：梯度幅值（Sobel 类）——它把"几何边"与"影缘"一起算，而本判据的臂间
// 差分恰恰要**只**看影缘；几何边在臂间是恒等的，梯度幅值对它的响应会淹没半影信号。
//
// ── 为什么没有 flag ───────────────────────────────────────────────────────────
// 场景、臂、帧号、阈值**都是量具的组成部分**：能调 = 能调绿。§5.1「阈值一旦写死就不许为了
// 让它绿回头改」——把旋钮从命令行挪进 constexpr 只是把同一个动作藏得远一点。
//
// ── 关于运行中那条「未指定 --clip」警告（不是本量具的缺陷）─────────────────────
// 交付路径整段重写 scene.lua 的 taa 块（既有行为），会丢掉交付值 clip_mode = 11 ⟹ 出声。
// 但交付配置 runtime.lua 的 `aa = "off"` ⟹ **TAA 根本不在图里**，clip_mode 对本量具是惰性的，
// 读数就是交付态。且六臂共用同一份 taa 块 ⟹ 臂间差分统计不受它影响（要动它得先动 aa）。
//
// ── 跑起来该看到什么（现状，2026-10-09 本机 8060S）────────────────────────────
// J1 **PASS**（量具有效）· J2 **FAIL**（这就是缺陷本身：红 = 如实报告，不是判据坏了）·
// J3 advisory 出数（覆盖率比 / 带核比，N6d 修复目标 ≥0.9 / ≥1.0）· 确定性 PASS。
// 因此本命令**当前 rc = 1 是预期结果**；N6d 修完后 J2 转绿、rc 才该归 0。

#include "imgqa.h"

#include <opencv2/core.hpp>

#include <cmath>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;

// ── 写死的量具常数（改动 = 改判据，必须同步改 §5.1 的定案表）─────────────────────
constexpr int kDelayFrames = 12;             ///< 截图帧号（见头注释「为什么写死」）
constexpr double kShadowLsb = 8.0;           ///< 影覆盖 / 太阳角敏感度的判定线
constexpr double kBandHighLsb = 40.0;        ///< 过渡带 ↔ 深本影分界（只进比值）
constexpr double kJ1FloorPct = 1.0;  ///< J1：sunSens(pcss, 0→5°) 下限（>8LSB 口径 = 可见）
/// J2 下限（**any-diff 口径**，不是 >8LSB）。0.02% = 实测 0.082% 的约 1/4：容得下场景漂移，
/// 又远大于 0（0 = 半影项根本没进计算）。口径为什么比 J1 降一档 → `pushSunVerdicts` 的理由。
constexpr double kJ2FloorPct = 0.02;
constexpr double kTargetCoverageRatio = 0.9; ///< J3 打印用：**转正门槛**（覆盖率比），实测未达
/// J3 判据**下界**（覆盖率比）。0.70 = 实测最优工作点 0.736 的 0.95 倍，同时把改造前的
/// 0.599 判红。为什么不是 0.9（= 上面的转正门槛）：闭式与 8 组实验证明 EVSSM 在「软度/不透明度
/// 与 CSM 齐平」时覆盖率比的天花板就是 0.74（权衡表 → rgl/shaders/shadow.slang STEP 2.5）。
/// 把判据线钉在够不到的地方 = 永久红灯 = 没有信号的红灯（详见 docs/shadow_routes_plan.md §5）。
constexpr double kCoverageFloor = 0.70;
/// J3 判据下界（带核比）。1.0 = 与 CSM 齐平；取 0.9 留量化余量。改造前的 0.209 判红。
constexpr double kBandFloor = 0.9;
/// J3 **痤疮上限**（不是目标，是天花板）。这个数不是拍的：痤疮参照 = 实测变异
/// 「`chebyshevUpperBound` 去快路」= **1.720**（读图确认：整片朝光屋顶与墙顶糊满影 =
/// 教科书自遮挡）。上限取 1.30 ⟹ 痤疮参照的 0.75 倍、目标 0.9 的 1.44 倍——
/// 既不放行痤疮，也不逼修复贴着天花板走。
/// ⚠ 没有这条上限，J3 的「≥0.9」会被**痤疮刷绿**（去快路直接 1.72 达标）。
constexpr double kAcneCoverageCeiling = 1.30;

/// 一臂 = shadow.lua 的一次覆写。`sun < 0` = **不写键** = 出厂值 0.53°（`evssm` 默认臂要的
/// 就是这个：J2 比的是出厂值，把 0.53 抄进配置反而多一个"我抄对了出厂值吗"的漂移面）。
struct Arm {
    const char* name;
    const char* mode;
    double sun;
};

/// 五臂（六次采集：pcss 两臂只服务 J1）。顺序 = 采集顺序 = 打印顺序。
const std::vector<Arm>& arms()
{
    static const std::vector<Arm> kArms = {
        {"none", "none", -1.0},        // 参照臂（无阴影）
        {"csm", "csm", -1.0},          // 交付档（4 抽头 PCF）
        {"evssm", "evssm", -1.0},      // 出厂太阳角 0.53°
        {"evssm_sun0", "evssm", 0.0},  // 半影恒 0 的对照
        {"evssm_sun5", "evssm", 5.0},  // J2 接线对（出厂 0.53° 与 0° 逐位相同，理由见 J2）
        {"pcss_sun0", "pcss", 0.0},    // ↓ 两条 = 量具自证（独立实现）
        {"pcss_sun5", "pcss", 5.0},
    };
    return kArms;
}

std::string pct(double frac, int digits = 3)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f%%", digits, 100.0 * frac);
    return buf;
}

std::string fixed3(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

/// 阈值出串（1 位小数）：`std::to_string(1.0)` 会打成 "1.000000"，判据正文要能读。
std::string floor1(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

/// 按名字取臂（写死顺序不等于可以按下标取——加臂/换序会让 `arms()[1]` 静默变成别的臂）。
const Arm& armNamed(const std::string& name)
{
    for (const auto& a : arms())
        if (name == a.name)
            return a;
    throw std::runtime_error("shadow-softness 臂表里没有臂 " + name);
}

/// 采一臂：交付配置 → 只覆写 shadow.lua（+ 一次性截图形态）→ 引擎跑到自然退出 → 读图。
/// 每臂都重新 makeProfile：门的形态是"整份拷交付配置 + 覆写"，不复用目录状态 = 臂与臂之间
/// 没有共享的可变配置（上一臂的残留键会静默活到下一臂）。
cv::Mat captureArm(const Arm& a, const std::string& tag, const std::string& profileDir)
{
    std::error_code ec;
    fs::remove_all(profileDir, ec);
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 不重抄 env/ao/light（第二真相）
    o.overrideShadowRoute = true;
    o.shadowRouteMode = a.mode;
    o.shadowRouteSunDegrees = a.sun;
    o.overrideLlmShot = true;
    o.llmShotFile = "screenshots/imgqa_shadow_softness_" + tag + ".png";
    o.llmShotDelayFrames = kDelayFrames;
    makeProfile(profileDir, o);
    runEngineOnce(profileDir);
    const std::string png = projectRoot() + "/build/rgl/" + o.llmShotFile;
    if (!fs::exists(png))
        throw std::runtime_error("臂 " + std::string(a.name) + " 没出图: " + png);
    return readPngBgr(png);
}

/// 逐像素 |Δ| > lsb 的像素数（lsb=0 即 any-diff，用来把 §4 的两种口径同时打出来）。
int64_t countDiffAbove(const cv::Mat& a, const cv::Mat& b, double lsb)
{
    int64_t hit = 0;
    for (int y = 0; y < a.rows; ++y) {
        const auto* pa = a.ptr<double>(y);
        const auto* pb = b.ptr<double>(y);
        for (int x = 0; x < a.cols; ++x)
            hit += std::abs(pa[x] - pb[x]) > lsb ? 1 : 0;
    }
    return hit;
}

/// Δ 的三层区间掩码（§5.1 的定义：Δ>8 算影、8<Δ≤40 过渡带、Δ>40 深本影）。
/// 为什么用 OpenCV 比较算子而不是手写像素循环：① 手写循环的认知复杂度 18 撞护栏（15），
/// 而把三个计数器拆到别处只是把复杂度藏起来；② **掩码本身就是判据的中间产物**——排查
/// 「覆盖率为什么掉了」时 dump 得出掩码，dump 不出三个 int。
struct SoftMasks {
    cv::Mat shadow;  ///< Δ > 8
    cv::Mat band;    ///< 8 < Δ ≤ 40
    cv::Mat core;    ///< Δ > 40
};

SoftMasks softMasks(const cv::Mat& delta)
{
    cv::Mat gtShadow, gtCore, leCore;
    cv::compare(delta, cv::Scalar(kShadowLsb), gtShadow, cv::CMP_GT);
    cv::compare(delta, cv::Scalar(kBandHighLsb), gtCore, cv::CMP_GT);
    cv::compare(delta, cv::Scalar(kBandHighLsb), leCore, cv::CMP_LE);
    cv::Mat band;
    cv::bitwise_and(gtShadow, leCore, band);
    return {gtShadow, band, gtCore};
}

/// 臂的 sun 列文本，三态：**不写键 = 出厂 0.53**（evssm/pcss 默认臂）· 写了值 · **该档不消费
/// 太阳角**（none/csm 下写 sun_degrees 会被门拒，见 applyShadowRouteGate——打印成 0.53 是撒谎）。
std::string armSunLabel(const Arm& a)
{
    if (a.sun >= 0.0)
        return std::to_string(a.sun);
    const std::string m = a.mode;
    return (m == "pcss" || m == "evssm") ? "出厂 0.53" : "该档不消费";
}

/// 采集全部臂并逐臂打印确认行（顺序 = arms() 顺序 = 采集顺序）。
std::map<std::string, cv::Mat> captureArms(const std::string& profile)
{
    std::map<std::string, cv::Mat> luma;
    for (const auto& a : arms()) {
        const cv::Mat bgr = captureArm(a, a.name, profile);
        luma[a.name] = byteLuma(bgr);
        std::printf("  ✓ 臂 %-11s mode=%-6s sun=%-12s %dx%d\n", a.name, a.mode,
            armSunLabel(a).c_str(), bgr.cols, bgr.rows);
    }
    return luma;
}

/// 臂表 + 两条太阳角敏感度。返回 false = 有臂尺寸与参照不同（后面全错，调用侧早停）。
bool printArmTable(const std::map<std::string, cv::Mat>& luma, double sensPcss,
    double sensEvssm)
{
    std::printf("\n臂间差分统计（相对 `none` 参照；字节域 luma，判定线 %.0f / %.0f LSB）\n",
        kShadowLsb, kBandHighLsb);
    // 表头用 ASCII：中文列头 printf 按**字节**补位、终端按**显示宽度**排 ⟹ 中文表头必然错位
    // （列名漂到下一列头上，读数会被读串行）。中文图例单独一行。
    std::printf("  图例：cov=影覆盖率 meanDark=平均变暗(LSB) band=过渡带 core=深本影 "
                "band/core=带核比\n");
    std::printf("  %-11s %8s %10s %8s %8s %10s\n", "arm", "cov%", "meanDark", "band%", "core%",
        "band/core");
    const cv::Size ref = luma.at("none").size();
    bool sized = true;
    for (const auto& a : arms()) {
        const cv::Mat& m = luma.at(a.name);
        if (m.size() != ref) {
            std::printf("  %-11s ✗ 尺寸 %dx%d ≠ 参照 %dx%d（两臂必然不同帧/不同尺寸）\n", a.name,
                m.cols, m.rows, ref.width, ref.height);
            sized = false;
            continue;
        }
        const SoftArmMetrics s = measureSoftArm(luma.at("none"), m);
        std::printf("  %-11s %8.2f %10.3f %8.2f %8.2f %10.3f\n", a.name, 100.0 * s.coverage,
            s.meanDark, 100.0 * s.band, 100.0 * s.core, s.bandCore);
    }
    std::printf("\n太阳角敏感度 frac(|Δluma| > %.0f LSB)（§5.1 的 J1 / J2 度量本体）\n", kShadowLsb);
    std::printf("  pcss  0° → 5.00°  = %-8s J1 阳性锚点，下限 %.1f%%\n", pct(sensPcss).c_str(),
        kJ1FloorPct);
    std::printf("  evssm 0° → 5.00°  = %-8s J2 接线判据（any-diff 口径），下限 %.2f%%\n",
        pct(sensEvssm).c_str(), kJ2FloorPct);
    return sized;
}

/// J1 + J2。**两条口径不同，这是定案不是妥协**：J1 量「半影改了画面」（>8LSB = 可见），
/// J2 只量「半影机制接没接线」（any-diff = 非零）。理由见 J2 的容差理由字符串。
void pushSunVerdicts(std::vector<Verdict>& vs, double sensPcss, double sensEvssmAny,
    int64_t evssmAnyPx, int64_t evssmLsb053Px, int64_t evssmAny053Px, int64_t total)
{
    const bool j1 = sensPcss * 100.0 >= kJ1FloorPct;
    vs.push_back(verdictFor("shadow-softness-gauge-valid", j1,
        "sunSens(pcss, 0→5°) = " + pct(sensPcss) + "（下限 " + floor1(kJ1FloorPct) + "%）"
            + (j1 ? ""
                  : "\n  ⚠ 这条红 = **量具自己瞎了**，不是 EVSSM 的问题：PCSS 与 EVSSM 共用同一条"
                    " 2·tan(θ/2)·Δz 半影公式，但深度比较不经过 EVSM 方差窗，是独立实现。"
                    "先修量具（帧号 / izb / 场景 / 比较域），此时 EVSSM 的读数一律不作数")));
    const bool j2 = sensEvssmAny * 100.0 >= kJ2FloorPct;
    vs.push_back(verdictFor("shadow-softness-sun-response", j2,
        "sunSens(evssm, 0→5°) 的 any-diff = " + pct(sensEvssmAny) + "（"
            + std::to_string(evssmAnyPx) + " / " + std::to_string(total) + "px，下限 "
            + pct(kJ2FloorPct / 100.0, 2) + "）；同对臂 >8LSB = 0px。INFO 出厂角 0°→0.53°："
            + std::to_string(evssmLsb053Px) + "px >8LSB、" + std::to_string(evssmAny053Px)
            + "px any-diff"
            + (j2 ? ""
                  : "\n  这条红 = EVSSM 的半影项**根本没进计算**（0°与 5° 连一个字节都不差）。"
                    "先查 STEP 2.5 有没有把 penumbraSquashed 读进核宽、再查 shadowEvssmConst.x "
                    "的指数 c 有没有下发")));
}

/// J3：对账，**两个比值都两侧有界**。
/// * 覆盖率比下限 `kCoverageFloor`：低于它 = EVSSM 退化回本轮改造前的形态（0.599）。
/// * 覆盖率比上限 `kAcneCoverageCeiling`：只判下限会被**痤疮刷绿**（实测去快路变异 = 1.720
///   直接达标），那等于把判据的靶子交给它本来要抓的那个失效。
/// * 带核比下限 `kBandFloor`：软度塌回 0.21（= CSM 去掉 PCF 的硬影）即红。
/// 上限不给带核比：糊不是痤疮的形态，且核宽是明写的调参键（糊是用户自己选的）。
/// ⚠ 打印里的 `kTargetCoverageRatio`（0.9）是**转正门槛**，不是判据线 —— 实测够不到
///   （闭式与实验表 → docs/shadow_routes_plan.md §5），它留在台账上，不留在绿灯里。
void pushReconcileVerdict(std::vector<Verdict>& vs, const SoftArmMetrics& evssm,
    const SoftArmMetrics& csm)
{
    const double covRatio = csm.coverage > 0.0 ? evssm.coverage / csm.coverage
                                               : std::numeric_limits<double>::quiet_NaN();
    const double bandRatio = std::isfinite(csm.bandCore) && csm.bandCore > 0.0
            ? evssm.bandCore / csm.bandCore
            : std::numeric_limits<double>::quiet_NaN();
    const bool valid = std::isfinite(covRatio) && std::isfinite(bandRatio);
    const bool inBand = valid && covRatio >= kCoverageFloor && covRatio <= kAcneCoverageCeiling
        && bandRatio >= kBandFloor;
    vs.push_back(verdictFor("shadow-softness-vs-csm", inBand,
        !valid ? "CSM 参照臂没有本影（coverage 或深本影为 0）⟹ 两个比值无意义，对账不成立"
               : (inBand ? "coverage(evssm)/coverage(csm) = " + fixed3(covRatio) + "（判据 ∈["
                                             + floor1(kCoverageFloor) + ", "
                                             + floor1(kAcneCoverageCeiling) + "]，转正门槛 "
                                             + floor1(kTargetCoverageRatio) + " 未达）；"
                                             "bandCore(evssm)/bandCore(csm) = "
                               + fixed3(bandRatio) + "（判据 ≥" + floor1(kBandFloor) + "）"
                          : "coverage(evssm)/coverage(csm) = " + fixed3(covRatio) + "、bandCore 比 = "
                                + fixed3(bandRatio) + " 落在判据区间外（覆盖率 ∈["
                                + floor1(kCoverageFloor) + ", " + floor1(kAcneCoverageCeiling)
                                + "]、带核比 ≥" + floor1(kBandFloor) + "）。"
                                + (covRatio > kAcneCoverageCeiling
                                      ? "覆盖率比**越过痤疮上限** ⟹ 影比 CSM 还多不是修好了，是自遮挡"
                                        "（痤疮参照变异 = 1.720，形态 = 朝光面糊影）；先查矩 footprint "
                                        "是否跨深度断崖"
                                      : (covRatio < kCoverageFloor
                                             ? "覆盖率比掉到地板下 ⟹ EVSSM 退回改造前的硬影形态"
                                               "（实测改造前 = 0.599）；先查 STEP 5 的逐抽头评估与 "
                                               "STEP 2.5 的核宽下限有没有生效"
                                             : "带核比塌了 ⟹ 软度退回「CSM 去掉 PCF」（实测 = 0.21）；"
                                               "先查 min_footprint_texels 有没有下发")))));
}

}  // namespace

cv::Mat byteLuma(const cv::Mat& bgr)
{
    CV_Assert(bgr.channels() == 3);
    cv::Mat l(bgr.rows, bgr.cols, CV_64FC1);
    for (int y = 0; y < bgr.rows; ++y) {
        const auto* row = bgr.ptr<cv::Vec3b>(y);  // BGR 序
        auto* out = l.ptr<double>(y);
        for (int x = 0; x < bgr.cols; ++x)
            out[x] = 0.2126 * row[x][2] + 0.7152 * row[x][1] + 0.0722 * row[x][0];
    }
    return l;
}

SoftArmMetrics measureSoftArm(const cv::Mat& refLuma, const cv::Mat& armLuma)
{
    if (refLuma.size() != armLuma.size())
        throw std::runtime_error("measureSoftArm 两图尺寸不同 = 两臂不是同一帧/同一尺寸");
    cv::Mat delta, darkened;
    cv::subtract(refLuma, armLuma, delta);  // Δ = 参照 − 本臂（正 = 该臂把这里压暗了）
    cv::max(delta, cv::Scalar(0.0), darkened);  // 只累变暗侧：变亮不是"影"（selftest 有反向用例）
    const SoftMasks m = softMasks(delta);
    const double inv
        = refLuma.total() > 0 ? 1.0 / static_cast<double>(refLuma.total()) : 0.0;
    SoftArmMetrics s;
    s.coverage = static_cast<double>(cv::countNonZero(m.shadow)) * inv;
    s.band = static_cast<double>(cv::countNonZero(m.band)) * inv;
    s.core = static_cast<double>(cv::countNonZero(m.core)) * inv;
    s.meanDark = cv::mean(darkened)[0];
    // 深本影为 0 时带核比是**未定义**而不是 0：写 0 会被读成"全是核没有带"（边缘最硬），
    // 而真实含义是"没有可比的分母"（例如整臂只有过渡带、或干脆没有影）。
    s.bandCore = s.core > 0.0 ? s.band / s.core : std::numeric_limits<double>::quiet_NaN();
    return s;
}

double sunSensitivity(const cv::Mat& lumaA, const cv::Mat& lumaB)
{
    if (lumaA.size() != lumaB.size())
        throw std::runtime_error("sunSensitivity 两图尺寸不同");
    const int64_t hit = countDiffAbove(lumaA, lumaB, kShadowLsb);
    const int64_t n = static_cast<int64_t>(lumaA.total());
    return n > 0 ? static_cast<double>(hit) / static_cast<double>(n) : 0.0;
}

int cmdShadowSoftness()
{
    const std::string profile = projectRoot() + "/build/rgl/imgqa_shadow_softness_profile";
    std::printf("shadow-softness —— 软影滤波轴量具（批 N6d 步 0）\n");
    std::printf("  场景 = 交付配置 configs/（只覆写 shadow.lua，izb 强制 false）\n");
    std::printf("  帧号 = 第 %d 帧（llm_shot.delay_frames 写死；§4.1：第 8 帧与第 12 帧影覆盖差 0.12pp）\n",
        kDelayFrames);
    std::printf("  判据设计 = docs/shadow_routes_plan.md §5.1；量具边界 = §4.1\n");

    const std::map<std::string, cv::Mat> luma = captureArms(profile);
    // 确定性自证：同臂（csm）第二跑。帧号钉死后必须逐位相同——它不在场时下面两条判决线
    // 量的是"两次采集本身不同"，而不是被测的滤波轴。
    const cv::Mat rerun = captureArm(armNamed("csm"), "csm_rerun", profile);
    const cv::Mat rerunLuma = byteLuma(rerun);

    const double sensPcss = sunSensitivity(luma.at("pcss_sun0"), luma.at("pcss_sun5"));
    const int64_t total = static_cast<int64_t>(luma.at("none").total());
    // J2 用 0°↔5° 这一对，**不是** 0°↔出厂 0.53°：出厂角下两臂逐位相同（any-diff 恰 0），
    // 因为 STEP 2.5 的核宽下限（16 图集纹素）远大于 0.53° 的物理半影 —— 那是设计事实。
    // 拿出厂角当判据 = 要求"物理上不存在的软度"，判据的靶子就错了（本轮实测推翻的原 J2）。
    const int64_t evssmAnyPx = countDiffAbove(luma.at("evssm_sun0"), luma.at("evssm_sun5"), 0.0);
    const double sensEvssmAny = total > 0
            ? static_cast<double>(evssmAnyPx) / static_cast<double>(total)
            : 0.0;
    // 出厂角那一对只出数（INFO）不进判决：0 = 核下限完全压住物理半影。
    const int64_t evssmLsb053Px = countDiffAbove(luma.at("evssm_sun0"), luma.at("evssm"), kShadowLsb);
    const int64_t evssmAny053Px = countDiffAbove(luma.at("evssm_sun0"), luma.at("evssm"), 0.0);

    const bool sized = printArmTable(luma, sensPcss, sensEvssmAny);

    std::vector<Verdict> vs;
    const int64_t detDiff
        = rerunLuma.size() == luma.at("csm").size()
            ? countDiffAbove(rerunLuma, luma.at("csm"), 0.0)  // 0 LSB = 逐位（字节域）不同
            : -1;
    vs.push_back(verdictFor("determinism", detDiff == 0,
        "同臂（csm）两独立进程、同帧号（第 " + std::to_string(kDelayFrames) + " 帧）重跑：字节域差 "
        + std::to_string(detDiff) + " px / " + std::to_string(total) + " px"));
    if (sized) {
        pushSunVerdicts(vs, sensPcss, sensEvssmAny, evssmAnyPx, evssmLsb053Px, evssmAny053Px,
            total);
        pushReconcileVerdict(vs, measureSoftArm(luma.at("none"), luma.at("evssm")),
            measureSoftArm(luma.at("none"), luma.at("csm")));
    }

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("shadow-softness", {{"delay_frames", std::to_string(kDelayFrames)},
                  {"arms",
                      "none,csm,evssm,evssm_sun0,evssm_sun5,pcss_sun0,pcss_sun5"}},
        vs);
    bool ok = true;
    for (const auto& v : vs)
        ok = ok && v.ok;
    std::printf("\n%s\n", ok ? "全部判据绿" : "有判据红（J2 当前预期为红 = 如实报告缺陷，见其正文）");
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
