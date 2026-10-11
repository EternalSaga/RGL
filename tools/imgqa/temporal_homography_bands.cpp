//
// temporal_homography_bands.cpp — 【机位状态 = 相机移动】的**运动带判据层**（尺子 / 守卫 / 判决）
//
//   本 TU 只做三件事：造分带尺子（含 τ 的实测覆盖）、执行可测性守卫、出六格表 + 材质表 + 三分支判决。
//   **不采集、不 warp、不碰引擎**——重投影算子在 `temporal_homography_metric.cpp`，采集与命令在
//   `temporal_homography.cpp`；缝的完整说明 → `temporal_homography_metric.h` 头注释。
//
//   ⚠ 本文件两条搬动时最容易坏的东西，逐字保留：
//     1. **打印顺序与判决线**。判据台账与调用记录引用这些文字与线；出表顺序 = 验收的结构签名。
//     2. **τ 与门限比较的严格性**（`>=` 与 `>`、`!(...)` 的反写）。它们是判据，不是笔误。
//   尺子的阈值常量（`kBandMidByte` / `kBandGradPct` / `kMovingTauFloorPct` / 守卫那几条线）唯一
//   出处 = `temporal_residual.h`，本文件只引用，**不重拍**。
//   ⚠ 读数格式化 `d4` / `d2` 是本 TU 自己的一份（与主 TU 各一份）：`temporal_swim.h` 已在
//   `RGL::imgqa` 作用域占了 `inline d2`（`"%.2f"`，与本命令的 `"%.2f%%"` 同签名不同体），
//   提到共享头 = 跨 TU ODR 冲突。既有惯例就是每 TU 一份，别"顺手"合并。
//
#include "temporal_homography_metric.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
namespace {

std::string d4(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}
std::string d2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f%%", v * 100.0);
    return b;
}

// ── 批 13.9：【机位状态 = **相机移动**】把 warp 残差图按「亮度 × 梯度」二维分带 ──────────
//
// ## 要判的假设（用户 2026-10-06 提出，本尺子是它的两把量具之一）
//
// 「所有**移动**锯齿的来源都是 1px **高亮**直线；**普通边缘确实没锯齿**。」
//
// 昨天批 13.7c 已给**静止机位**跨相位摆动加了同一套分带，判出「静止残余非高光专属」。
// 但那条只覆盖静止态 —— 「**移动** × 亮度 × 梯度」这一格至今是空的，用户的主张既没证实
// 也没否证。本尺子量的正是它：**纯旋转单应 warp 上一帧到本帧网格，搬不动的那部分
// = 移动时的闪烁本身**（闭式、与深度无关、零参考图、零对齐搜索）。
//
// ## 与静止态（批 13.7c）的口径关系：**复用，不另定**
//
// 分档函数 = `bandTableOf`；亮度轴 = 显示域字节亮度多帧均值（`T_lo=128`、`T_hi=240` 由
// `bandClipProxyByte` 闭式反解显示链）；梯度轴 = `sobelAbsGrad` 于线性 luma 的 **≥p90**。
// 三个阈值常量在 `temporal_residual.h` 里只有一份，**这里不许重拍**。
//
// ⚠ **被迫的差别只有一条，且必须写清**：静止态的梯度算在「相位平均图」上（静止机位下那是
// 一张干净图），移动机位**没有**这张图 —— 连拍平均会把边缘糊掉（糊量 = 扫速 × 帧数）。
// 故梯度轴取**跨帧最大**：移动时「这个像素在边缘带上」的正确含义是「burst 期间有边缘扫过它」。
//
// ## ⚠ 本尺子的固有限制（**必须在输出里自曝**）：尺子来自帧自身
//
// 本判据按构造**零参考图**（这是它相对 `temporal-motion` 的增量：不需要超采样参考、不需要
// 对齐）。代价是分带的亮度档与梯度档只能由**被测量自己的画面**定 —— 这正是昨天因**选择偏差**
// 翻过一次方向的那种做法（用被测量选区域）。具体风险：若高光只出现在某一帧，均值口径会把它
// 判出 B 档（对假设保守）；若 warp 残差本身把某块区域的表观亮度改了，档位也跟着动。
// ⟹ 本尺子的结论**只在「与尺子 2（尺子来自超采样参考）同向」时才算强结论**；两把尺子反向时
// 以尺子 2 为准，本尺子读作「量具口径受限」。
//
// ## ⭐ 判决线（**2026-10-06 跑前写死**，防事后挑数；三分支全部命名）
//
// 记 A = 中间调×高梯度（普通几何边缘）、B = 近削顶×高梯度（假设的元凶）。
// 主表 = **TAA 关**那组（= 移动时的闪烁本身，无 TAA 干预 = 用户原话里的「锯齿」）。
//
// * **分支①「支持」** ⟺ B 的**幅度份额** ≥ 30%（`kMovingSupportAmpShare`）
//   **或** 正向超额集中在 B：`rB ≥ 1.5 × rA`，其中 `rX = rate_on(X)/rate_off(X)`
//   （该格「开 TAA 后还剩多少」的比；1.5 = 跑前沿用的 `kBandSupportRatio`，不新拍）。
// * **分支②「否证」** ⟺ B 的幅度份额**与**计数份额**都** < 5%（`kMovingRefuteShare`）
//   **且** 超额不向 B 集中。
// * **分支③「分不出」** ⟺ 其余。出表处必须再报性质（三选一）：**阈值没分开**（附敏感性表，
//   换档结论跟着翻）/ **两格本来同分布**（怎么换都不翻）/ **反向分离**（超额集中在 A，
//   与用户主张方向相反）。昨天漏了「反向分离」这一支的名字，导致真结果落进未命名兜底支、
//   被读成「没有信号」；今天三分支全部命名，落进哪支照实报哪支，**不许改判决线**。
//
// 幅度份额 = Σ残差 归一化，**本次新增的胜负手**：`frac>τ` 是不加权像素计数，平坦中间调摆
// 1 LSB 与高光摆一大截各算一票，而高光是对比度最高处、同样绝对摆动**最看得见**。
// ⟹「我一直看到高亮在闪」与「高亮只占残余 0.5%」可同时为真；只有幅度份额能分辨
// 「像素少但每像素摆动巨大」。**只出计数份额 = 未完成。**
//
// ## ⛔ 守卫红分支（**2026-10-06 跑前写死**，与三分支平级、优先级更高）
//
// 任一守卫（⓪a/⓪b/1a/1b/2/3/4）不过 ⟹ **唯一**合法输出是 `Indeterminate` + 缺哪条前提 + rc≠0，
// **不许**打印①②③任何一支、**不许** rc=0。理由：旧实现这两条守卫只 ⚠ 警告，实测「四率全
// 100.00% + 自证失败」照样打印「分支②否证」且 rc=0（`/tmp/opencode/homo_before_doorframe.log`）。
// 守卫 ⓪ 是同日补的第三条实测教训：`env.intensity=0` 打在只吃天光的门框机位会把交付面整个
// 关黑（实测显示域均值 0.0044/255），帧差/残差/地板一起变 0——**「没有闪烁」与「没有画面」
// 是两件事**，后者没有归因对象。当时 `rulerOk=false` 还会让整段守卫被静默跳过直接 rc=0。
//
// ## 🧪 A/B 归因判据（**2026-10-06 写，时序如实**：在 `env=0` 那批跑废之后、剂量响应那批跑之前）
//
// 问的是「移动闪烁是 **IBL 反射**（金属/玻璃/湿面）/ **AO** / **几何边** 哪个驱动的」。
//
// * **AO**：`ao.strength=0` 是 IEEE 精确旁路，与交付基线逐格对拍。判据 = 残差/地板与材质
//   份额是否移动（不看绝对值，绝对值随画面亮度一起动，没有可比性）。
// * **IBL**：`env.intensity` 的**开关不可用**（见守卫 ⓪），改用**剂量响应** `--env 1 / 0.5 / 0.25`。
//   判据 = **残差/地板随剂量的走向**：IBL 若是信号源，关掉它残差应比地板**掉得更快**（比值**下降**）；
//   若闪烁受交付量化支配（信号与噪声同源），比值应**近似平坦**。材质侧配套判据 = 前八名**集合**
//   是否随剂量换人（换人 = IBL 驱动；不换人只抖次序 = 非 IBL 驱动）。
// * **几何边**：既有六格的梯度轴（高梯度 = 线性 luma 梯度 ≥ p90）。判据 = 高梯度格的
//   **幅度份额 / 面积份额**集中度与 rate 比（低梯度格的 rate 是它的分母参照）。
//
// ⚠ 本尺子的**固有**限制照旧（亮度/梯度档由被测量自己定 = 选择偏差）：结论只在与尺子 2
//   （超采样参考）**同向**时才算强结论，反向时以尺子 2 为准。
/// 全帧 over-τ 率（守卫 ④ 的被测量：零假设下应 ≈ 1−τ 分位，而不是 ~100%）。
double overTauRate(const cv::Mat& residOff, double tau)
{
    cv::Mat over;
    cv::threshold(residOff, over, tau, 1e30, cv::THRESH_BINARY);
    return static_cast<double>(cv::countNonZero(over))
        / (static_cast<double>(residOff.rows) * static_cast<double>(residOff.cols));
}

/// 从扫描曲线取三个锚点：α=0、|α|=1 里的小者（= 上报的那个残差）、以及全曲线最小点。
/// 单独成函数只为把两条扫描循环从守卫判定里摘出去——守卫的复杂度预算要留给那七条线。
void applyAlignAnchors(const HomoStat& off, MovingGuards& g)
{
    // 从扫描曲线取三个量：α=0、|α|=1 里的小者（= 上报的那个残差）、以及全曲线最小点。
    double bestAbs = 1e30;
    for (const auto& [a, v]: off.alignCurve) {
        if (a == 0.0)
            g.residAt0 = v;
        if (std::abs(std::abs(a) - 1.0) < 1e-9 && v < bestAbs)
            bestAbs = v;
    }
    g.residAt1 = bestAbs < 1e29 ? bestAbs : off.residMean;
    g.residAtNeg1 = off.residOther;
    double bestV = 1e30;
    for (const auto& [a, v]: off.alignCurve) {
        if (v < bestV) {
            bestV = v;
            g.bestAlpha = a;
        }
    }
}

/**
 * @brief 七条可测性线的逐条判定（**只判定**；线在 `temporal_residual.h` 跑前写死）
 *
 * 不过的前提按原顺序拼进返回值（多条以「；」连，全过则空串）。⚠ 比较的严格性（`>=` 与 `>`、以及
 * `!(...)` 的反写）逐字来自原实现——它们是判据本身，不是笔误，别"化简"成 `if (x < 线)`。
 */
std::string movingGuardWhy(const MovingGuards& g)
{
    std::string why;
    const auto push = [&](const std::string& s) {
        if (!why.empty())
            why += "；";
        why += s;
    };
    // 守卫 ⓪：**先问采集是不是活的**。A/B 口径能把画面整个关黑（实测 env.intensity=0 打在
    // 只吃天光的门框机位），此时帧差/残差/地板一起变 0——「没有闪烁」与「没有画面」是两件事，
    // 后者没有任何归因对象，必须点名而不是让它走进后面的比值。
    if (g.lumaMean >= 0.0 && g.lumaMean < kMovingGuardMinLuma)
        push("⓪a 交付面**全黑**（显示域均值 " + d4(g.lumaMean) + "/255，下限 "
            + d4(kMovingGuardMinLuma) + "）⟹ 本次 A/B 口径把画面关掉了，没有可归因的像素");
    if (g.byteMaxAdjacent < kMovingGuardMinMotion)
        push("⓪b 相邻帧最大字节差只有 " + d4(g.byteMaxAdjacent) + "（下限 "
            + d4(kMovingGuardMinMotion) + "）⟹ 运动激励没进场，残差无从谈起");
    const double explain = g.residAt1 > 0.0 ? g.residAt0 / g.residAt1 : 0.0;
    if (!(explain >= kMovingGuardExplainRatio))
        push("①a 对齐没消化掉多少帧差：α=0/α=1 = " + d4(explain) + "（要求 ≥ "
            + d4(kMovingGuardExplainRatio) + "）");
    if (!(std::abs(std::abs(g.bestAlpha) - 1.0) <= kMovingGuardAlphaTol))
        push("①b 残差最小点不在 |α|=1 上（实测 α* = " + d4(g.bestAlpha) + "，容差 "
            + d4(kMovingGuardAlphaTol) + "）⟹ 闭式 Δψ 与画面真实运动对不上");
    if (!(g.ratio >= kMovingGuardFloorRatio))
        push("② 残差/地板只有 " + d4(g.ratio) + "（要求 ≥ " + d4(kMovingGuardFloorRatio)
            + "）⟹ 量的是重采样本身");
    if (!(g.tau > 0.0))
        push("③ τ **没定出来**（= 0，本次实测地板的 p99 就是 0）⟹ 尺子不存在，分带无从谈起");
    else if (!(g.tau >= g.tauFloor))
        push("③ τ " + d4(g.tau) + " 落在地板里（地板中位数 " + d4(g.tauFloor) + "）");
    if (!(g.overRateAll <= kMovingGuardMaxOverRate))
        push("④ 全帧 over-τ 率 " + d2(g.overRateAll) + " 已饱和（上限 "
            + d2(kMovingGuardMaxOverRate) + "）");
    return why;
}

/// 率比退化闸门的饱和线：四格 rate 全部越过它 ⟹ `frac>τ` 已饱和，率比不再判方向。
constexpr double kBandRateSaturated = 0.90;

/// 六格表 A/B 两格（A = 中间调×高梯度、B = 近削顶×高梯度）的率比与三分支判决输入。
/// 一次算好给「率比表 / 判决 / 分离方向」三处打印共用——同一个数打三遍，必须同源。
struct MovingBandVerdict {
    BandShare aOff, aOn, bOff, bOn;
    double rA = 0.0, rB = 0.0;
    double relDiff = 0.0;      ///< rB/rA 的相对差（判据线要的是 ≥50% 的分离，远小于它 = 方向没有内容）
    bool excessInB = false;    ///< 正向超额集中在 B（rB ≥ 1.5 × rA）
    bool rateSaturated = false;///< 四格 rate 同贴 1.0 ⟹ 率比退化失效、方向不判
    MovingBandBranch branch = MovingBandBranch::Indeterminate;
};

MovingBandVerdict movingBandVerdict(const BandTable& off, const BandTable& on)
{
    MovingBandVerdict v;
    v.aOff = bandShareOf(off, 1, 1), v.aOn = bandShareOf(on, 1, 1);
    v.bOff = bandShareOf(off, 2, 1), v.bOn = bandShareOf(on, 2, 1);
    v.rA = v.aOff.rate > 0.0 ? v.aOn.rate / v.aOff.rate : 0.0;
    v.rB = v.bOff.rate > 0.0 ? v.bOn.rate / v.bOff.rate : 0.0;
    v.excessInB = v.rA > 0.0 && v.rB >= kBandSupportRatio * v.rA;
    v.rateSaturated = v.aOff.rate > kBandRateSaturated && v.bOff.rate > kBandRateSaturated
        && v.aOn.rate > kBandRateSaturated && v.bOn.rate > kBandRateSaturated;
    v.relDiff = std::abs(v.rB - v.rA) / std::max({ v.rA, v.rB, 1e-12 });
    v.branch = classifyMovingBand(v.bOff.ampShare, v.bOff.cntShare, v.excessInB);
    return v;
}

/// 表头与口径回显（含「本尺子的固有限制」自曝）。返回 false = 尺子缺失，调用方直接出「未出」。
bool printMovingBandHeader(const MovingBandRuler& r)
{
    std::printf("\n── 【机位状态 = **相机移动**】亮度 × 梯度 二维分带（尺子 1：精确单应 warp 残差"
                "）──\n");
    if (r.tau <= 0.0) {
        std::printf("  ✗ 尺子缺失（采集侧 wantBands 没开 = 仪器没装上），分带表未出\n");
        return false;
    }
    std::printf("  被测量 = 上一帧按闭式单应 warp 到本帧网格后**搬不动的部分**（= 移动时的闪烁本身）\n");
    std::printf("  亮度轴 = 显示域字节亮度的多帧均值；T_lo=%.0f（码字中点）· T_hi=%.0f（**闭式**："
                "ACES 输入 ≥%s）\n", kBandMidByte, r.tHi, d4(kBandClipProxyLinear).c_str());
    std::printf("  梯度轴 = |Sobel_x|+|Sobel_y| 于**线性 luma**，取**跨帧最大**（移动机位没有"
                "「相位平均」那张干净图，理由见文件内注释）；高梯度 = ≥ p%.0f = %s\n",
        kBandGradPct * 100.0, d4(r.gradThr).c_str());
    std::printf("  τ = %s ← **本次实测同态往返地板的 p%.0f 分位**（零假设分布的上尾；旧口径「白电平 %s"
                " 处 1 显示 LSB = %s」比地板低一个量级以上，用它 = 饱和）\n",
        d4(r.tau).c_str(), kMovingTauFloorPct * 100.0, d4(r.white).c_str(), d4(r.tauLegacy).c_str());
    std::printf("  ⚠⚠ **本尺子的固有限制**：零参考图 ⟹ 亮度档与梯度档只能由**被测量自己的画面**定"
                "（选择偏差）。静止态批 13.7c 已因这种口径翻过一次方向。故本尺子的结论**只在"
                "与尺子 2（尺子来自超采样参考）同向时才算强结论**；反向时以尺子 2 为准。\n");
    return true;
}

/// 率比表（含**率比退化失效**的自曝——wetground 实测逼出来的，不是预防性装饰）
void printMovingBandRates(const MovingBandVerdict& v)
{
    const BandShare aOff = v.aOff, aOn = v.aOn, bOff = v.bOff, bOn = v.bOn;
    const double rA = v.rA, rB = v.rB, relDiff = v.relDiff;
    const bool rateSaturated = v.rateSaturated;
    const double kSat = kBandRateSaturated;
    std::printf("\n  率比 rX = rate_on(X)/rate_off(X)（该格「开 TAA 后还剩多少」）\n");
    std::printf("    A 中间调×高梯度 rate %s → %s（×%s）  B 近削顶×高梯度 rate %s → %s（×%s）\n",
        d2(aOff.rate).c_str(), d2(aOn.rate).c_str(), d4(rA).c_str(), d2(bOff.rate).c_str(),
        d2(bOn.rate).c_str(), d4(rB).c_str());
    std::printf("    B 幅度份额 %s → %s（门关→门开）  B 计数份额 %s → %s\n",
        d2(bOff.ampShare).c_str(), d2(bOn.ampShare).c_str(), d2(bOff.cntShare).c_str(),
        d2(bOn.cntShare).c_str());
    if (rateSaturated)
        std::printf("    ⚠ **率比在本场景退化失效**：四格 rate 全部 > %.0f%%（`frac>τ` 饱和）⟹ rX 的"
                    "分子分母同贴 1.0，rB/rA 的相对差只有 %s ——**方向不判**，只看幅度份额\n",
            kSat * 100.0, d4(relDiff).c_str());
}

/// ⭐ 三分支判决 + B 档空洞性自曝 + 分离方向的性质（三分支全部命名，落进哪支照实报哪支）
void printMovingBandBranch(const MovingBandVerdict& v)
{
    const BandShare bOff = v.bOff;
    const double rA = v.rA, rB = v.rB, relDiff = v.relDiff;
    const bool excessInB = v.excessInB, rateSaturated = v.rateSaturated;
    const MovingBandBranch branch = v.branch;
    std::printf("\n  ⭐ 判决（线**跑前写死**：支持 = B 幅度份额 ≥ %s 或 rB ≥ %s×rA；"
                "否证 = B 两份额都 < %s 且超额不向 B 集中）\n",
        d2(kMovingSupportAmpShare).c_str(), d4(kBandSupportRatio).c_str(),
        d2(kMovingRefuteShare).c_str());
    std::printf("    支撑格数字：B 幅度份额 %s（支持线 %s / 否证线 %s）· B 计数份额 %s · "
                "rB/rA = %s · 超额集中在B = %s\n",
        d2(bOff.ampShare).c_str(), d2(kMovingSupportAmpShare).c_str(),
        d2(kMovingRefuteShare).c_str(), d2(bOff.cntShare).c_str(),
        rA > 0.0 ? d4(rB / rA).c_str() : "inf", excessInB ? "是" : "否");
    std::printf("    ⟹ **%s**\n", movingBandBranchName(branch));
    printMovingBandVacuity(bOff, "连拍帧**自身**的显示域字节亮度均值（零参考图）",
        "本尺子的亮度轴来自被测量自己（选择偏差自曝过），若高光只出现在个别帧，"
        "均值口径会把它判出近削顶档 —— 对假设是**保守**方向");
    // 分支③的**性质**也必须命名（昨天缺的就是这个）：方向 + 阈值敏感性两条一起读。
    if (rateSaturated)
        std::printf("    分离方向：**不判**（rate 饱和；rB/rA 相对差 %s，而判据线要的是 ≥50%% 的分离"
                    "⟹ 方向这一维在本场景没有内容）——用户主张要求 rB > rA，但本场景 rate 轴没有"
                    "分辨力，方向问题由**幅度份额**与**尺子 2**回答\n",
            d4(relDiff).c_str());
    else
        std::printf("    分离方向：rB %s rA ⟹ %s（用户主张要求的是 rB > rA：B 比 A 更压不动）\n",
            rB < rA ? "<" : (rB > rA ? ">" : "="),
            rB < rA ? "**反向分离**（B 比 A 压得更狠，与主张方向相反）"
                    : (rB > rA ? "B 压得没那么狠（方向与主张一致，幅度见上）" : "两格同倍数"));
}

/// 敏感性（同一次采集换阈值，零额外引擎成本）。判「分不出」时的分工：换档结论跟着翻
/// = **阈值没分开**；怎么换都不翻 = **两格本来同分布**。
void printMovingBandSensitivity(const MovingBandReport& rep)
{
    const MovingBandRuler& r = rep.ruler;
    const cv::Mat& residOff = rep.residOff;
    const cv::Mat& residOn = rep.residOn;
    std::printf("\n  ── 敏感性（同一次采集换阈值；主档 = 上面那张）──\n");
    std::printf("    %-24s %13s %13s %14s %14s\n", "口径", "A 率比", "B 率比",
        "B 幅度份额(关)", "B 计数份额(关)");
    const auto sensLine = [&](const std::string& tag, const cv::Mat& lm, const cv::Mat& gm,
                                  double tl, double th, double gthr) {
        const BandTable t1 = bandTableOf(residOff, lm, gm, tl, th, gthr, r.tau);
        const BandTable t2 = bandTableOf(residOn, lm, gm, tl, th, gthr, r.tau);
        const BandShare a1 = bandShareOf(t1, 1, 1), a2 = bandShareOf(t2, 1, 1);
        const BandShare b1 = bandShareOf(t1, 2, 1), b2 = bandShareOf(t2, 2, 1);
        const double ra = a1.rate > 0.0 ? a2.rate / a1.rate : 0.0;
        const double rb = b1.rate > 0.0 ? b2.rate / b1.rate : 0.0;
        std::printf("    %-24s %13s %13s %14s %14s\n", tag.c_str(), d4(ra).c_str(),
            d4(rb).c_str(), d2(b1.ampShare).c_str(), d2(b1.cntShare).c_str());
    };
    for (double th : kBandClipSensitivity)
        sensLine("T_hi=" + d4(th).substr(0, 3) + (std::abs(th - r.tHi) < 0.5 ? "（主档）" : ""),
            r.byteMean, r.gradMax, kBandMidByte, th, r.gradThr);
    sensLine("梯度轴=线性 p75", r.byteMean, r.gradMax, kBandMidByte, r.tHi,
        percentileOf(r.gradSorted, 0.75));
    sensLine("梯度轴=线性 p95", r.byteMean, r.gradMax, kBandMidByte, r.tHi,
        percentileOf(r.gradSorted, 0.95));
    sensLine("亮度轴=跨帧最大", r.byteMax, r.gradMax, kBandMidByte, r.tHi, r.gradThr);
    sensLine("T_lo=96", r.byteMean, r.gradMax, 96.0, r.tHi, r.gradThr);
    sensLine("T_lo=160", r.byteMean, r.gradMax, 160.0, r.tHi, r.gradThr);
    std::printf("    读法：换档后「B 幅度份额」跟着翻过 %s / %s 两条线 = **阈值没分开**；"
                "怎么换都不翻 = **两格本来同分布**\n",
        d2(kMovingRefuteShare).c_str(), d2(kMovingSupportAmpShare).c_str());
}

/// 材质归因（批 13.9d，**本次的主问题**）：objectID → glb primitive → 材质名
void printMovingBandMaterials(const MovingBandReport& rep)
{
    const MovingBandRuler& r = rep.ruler;
    const cv::Mat& residOff = rep.residOff;
    const cv::Mat& residOn = rep.residOn;
    const cv::Mat& ids = rep.ids;
    const std::vector<GlbPrim>& prims = rep.prims;
    const double floorMeanOff = rep.floorMeanOff, floorMeanOn = rep.floorMeanOn;
    const double idYawErrPx = rep.idYawErrPx;
    const int idBestFrame = rep.idBestFrame;
    const double idBestDiff = rep.idBestDiff;
    const double sweepTotalPx = rep.sweepTotalPx;
    if (!ids.empty() && !prims.empty()) {
        std::printf("\n  ── 材质归因（objectID → glb → 材质名；**按幅度份额降序**）──\n");
        std::printf("    objectID 落位 = burst 中点姿态，实测姿态残差 %s px；同姿态补拍的彩色与 "
                    "burst 第 %d 帧最接近（平均|Δ| = %s，≈0 = 姿态逐位复现）\n",
            d4(idYawErrPx).c_str(), idBestFrame, d4(idBestDiff).c_str());
        std::printf("    只拍一张 ID 的理由：材质是**慢变量**，整段 burst 总位移只有 %s px"
                    "（扫速 × 帧数），与分带尺子本身（均值 / 跨帧最大）同一量级\n",
            d4(sweepTotalPx).c_str());
        // ⚠ 不用 `cv::norm(NORM_INF)`：它对 CV_32SC1 没有实现（本构建里直接段错误，实测踩过），
        //   而 objectID 就是 32 位整图。自己扫一遍，921600 次比较不值一提。
        int64_t maxId = 0;
        for (int rr = 0; rr < ids.rows; ++rr) {
            const int32_t* ip = ids.ptr<int32_t>(rr);
            for (int cc = 0; cc < ids.cols; ++cc)
                maxId = std::max(maxId, static_cast<int64_t>(ip[cc]));
        }
        std::printf("    objectID 上界自证：本帧最大 id %lld，glb 摊平出 %zu 个 primitive ⟹ %s。"
                    "可见 id 少于总数是**正常**的（画面外的 primitive 不进图）；真正的失效是 id"
                    "**越界** ⟹ 表里会出现 unnamed#\n",
            static_cast<long long>(maxId), prims.size(),
            maxId < static_cast<int64_t>(prims.size()) ? "成立" : "**不成立，名字全部不可信**");
        printMaterialTable("TAA 关（= 移动锯齿本体，**主表**）",
            materialAttribution(ids, residOff, prims, r.tau, floorMeanOff), 14, r.tau,
            floorMeanOff, "单应 warp 残差（TAA 关）");
        printMaterialTable("TAA 开（= 开 TAA 之后还剩的闪烁）",
            materialAttribution(ids, residOn, prims, r.tau, floorMeanOn), 8, r.tau, floorMeanOn,
            "单应 warp 残差（TAA 开）");
    }
}
}  // namespace

/// 从**移动机位连拍**造分带尺子。
///
/// τ 的**旧**定义留在这里当 `tauLegacy` 回显（= 场景白电平处 1 个显示 LSB 的线性跨度，闭式
/// `lsbLinearAtLuma`）；真正用于分带的 `tau` 由 `applyFloorTau` 用**本次实测地板**覆盖。
/// 为什么必须覆盖：旧 τ ≈ 0.0073 而实测地板 0.34~0.44（差 46~60 倍）⟹ 「超 τ」被地板饱和。
/// 完整理由与 p99 的取舍 → `temporal_residual.h` 的 `kMovingTauFloorPct`。
MovingBandRuler movingBandRulerFrom(const FlickerSeries& g)
{
    MovingBandRuler r;
    if (g.byteMean.empty() || g.gradMax.empty() || g.luma.empty())
        return r;
    cv::Mat acc = cv::Mat::zeros(g.frameSize, CV_64FC1);
    for (const auto& m : g.luma)
        cv::add(acc, m, acc);
    acc /= static_cast<double>(g.luma.size());
    auto vals = valuesInMask(acc, {});
    std::sort(vals.begin(), vals.end());
    r.white = percentileOf(vals, 0.99);
    r.tauLegacy = lsbLinearAtLuma(r.white);
    r.tau = r.tauLegacy;  // ← 立刻被 applyFloorTau 覆盖；这里只是"地板缺失时的兜底"
    r.byteMean = g.byteMean;
    r.byteMax = g.byteMax;
    r.gradMax = g.gradMax;
    r.gradSorted = valuesInMask(g.gradMax, {});
    std::sort(r.gradSorted.begin(), r.gradSorted.end());
    r.gradThr = percentileOf(r.gradSorted, kBandGradPct);
    r.tHi = static_cast<double>(bandClipProxyByte(kBandClipProxyLinear));
    return r;
}

/// 用**本次实测同态往返地板**（跨帧最大场）覆盖 τ。地板缺失 = 不动（保持旧口径并如实报）。
void applyFloorTau(MovingBandRuler& r, const cv::Mat& floorHeat)
{
    if (r.tauLegacy <= 0.0 || floorHeat.empty())
        return;
    r.tau = fieldPercentile(floorHeat, kMovingTauFloorPct);
}

/**
 * @brief 三条可测性守卫的执行（**只执行**；线在 `temporal_residual.h` 跑前写死）
 *
 * @param off   TAA **关**那组的归因结果（= 移动锯齿本体，与主表同一组）
 * @param residOff 与分带表**同一份**残差场（跨帧最大），over-τ 率从它数
 */
MovingGuards evaluateMovingGuards(
    const HomoStat& off, const MovingBandRuler& r, const cv::Mat& residOff)
{
    MovingGuards g;
    g.floorMean = off.floorMean;
    g.tau = r.tau;
    g.tauFloor = off.floorHeat.empty() ? 0.0 : fieldPercentile(off.floorHeat, 0.50);
    g.overRateAll = overTauRate(residOff, r.tau);
    applyAlignAnchors(off, g);
    g.ratio = g.floorMean > 0.0 ? g.residAt1 / g.floorMean : 0.0;
    g.lumaMean = off.lumaMean;
    g.byteMaxAdjacent = off.byteMaxAdjacent;
    g.why = movingGuardWhy(g);
    g.ok = g.why.empty();
    return g;
}

/// 守卫回显（**过与不过都打**——过了也要让人看见是"哪三个数"放行的）
void printMovingGuardEcho(const MovingBandRuler& r, const MovingGuards& g, WarpKernel k)
{
    const double explain = g.residAt1 > 0.0 ? g.residAt0 / g.residAt1 : 0.0;
    std::printf("\n  ── 可测性守卫（**不过则整段不出结论**，线跑前写死 → temporal_residual.h）──\n");
    std::printf("    warp 核 = **%s**（与引擎 TAA 逐式同权重，见本文件「为什么双线性不够」段）\n",
        kernelName(k));
    std::printf("    τ = %s  ← **本次实测地板的 p%.0f 分位**（零假设分布的上尾）\n", d4(r.tau).c_str(),
        kMovingTauFloorPct * 100.0);
    std::printf("      旧口径 τ = 白电平(%s) 处 1 显示 LSB = %s —— 比它低 **%s 倍**，"
                "用它 = 「超 τ」被地板饱和（这就是四率全 100%% 的原因）\n",
        d4(r.white).c_str(), d4(r.tauLegacy).c_str(),
        r.tauLegacy > 0.0 ? d4(r.tau / r.tauLegacy).c_str() : "inf");
    std::printf("    0)  采集活着吗：显示域均值 %s/255（≥ %s）｜相邻帧最大字节差 %s（≥ %s）%s\n",
        d4(g.lumaMean).c_str(), d4(kMovingGuardMinLuma).c_str(), d4(g.byteMaxAdjacent).c_str(),
        d4(kMovingGuardMinMotion).c_str(),
        (g.lumaMean < 0.0 || g.lumaMean >= kMovingGuardMinLuma)
                && g.byteMaxAdjacent >= kMovingGuardMinMotion
            ? "✅"
            : "❌");
    std::printf("        含义：全黑 / 没运动 ≠「没有闪烁」，是**没有画面**（没有可归因的像素）。\n");
    std::printf("    1a) 对齐消化率 α=0/α=1 = %s/%s = %s  （要求 ≥ %s）%s\n", d4(g.residAt0).c_str(),
        d4(g.residAt1).c_str(), d4(explain).c_str(), d4(kMovingGuardExplainRatio).c_str(),
        explain >= kMovingGuardExplainRatio ? "✅" : "❌");
    std::printf("    1b) 扫描最小点 α* = %s  （要求 |α*|−1 ≤ %s）%s\n", d4(g.bestAlpha).c_str(),
        d4(kMovingGuardAlphaTol).c_str(),
        std::abs(std::abs(g.bestAlpha) - 1.0) <= kMovingGuardAlphaTol ? "✅" : "❌");
    std::printf("        含义：残差必须在「按闭式 Δψ warp」处见底，才配叫「运动没搬动的部分」。\n");
    std::printf("        反号 α=−1 的残差 %s（本次取的小者 %s）—— 符号若反，1b 当场红\n",
        d4(g.residAtNeg1).c_str(), d4(g.residAt1).c_str());
    std::printf("    2)  残差/地板 %s/%s = %s  （要求 ≥ %s）%s\n", d4(g.residAt1).c_str(),
        d4(g.floorMean).c_str(), d4(g.ratio).c_str(), d4(kMovingGuardFloorRatio).c_str(),
        g.ratio >= kMovingGuardFloorRatio ? "✅" : "❌");
    std::printf("    3)  τ %s > 0 且 ≥ 地板中位数 %s  %s\n", d4(g.tau).c_str(), d4(g.tauFloor).c_str(),
        g.tau > 0.0 && g.tau >= g.tauFloor ? "✅" : "❌");
    std::printf("    4)  全帧 over-τ 率 %s  （上限 %s）%s\n", d2(g.overRateAll).c_str(),
        d2(kMovingGuardMaxOverRate).c_str(),
        g.overRateAll <= kMovingGuardMaxOverRate ? "✅" : "❌");
    std::printf("    ⟹ 守卫 **%s**\n", g.ok ? "全过 ✅（可以出结论）" : "未过 ⛔（只报 Indeterminate）");
}

/// 出「亮度 × 梯度」六格表（四数）+ 三分支判决。`residOff`/`residOn` = 两态的逐像素残差
/// （跨帧最大，CV_64FC1）。返回判决文字供台账。
std::string reportMovingBandsFromHomography(const MovingBandReport& rep)
{
    const MovingBandRuler& r = rep.ruler;
    if (!printMovingBandHeader(r))
        return "未出（尺子缺失）";
    const BandTable off = bandTableOf(rep.residOff, r.byteMean, r.gradMax, kBandMidByte, r.tHi,
        r.gradThr, r.tau);
    const BandTable on = bandTableOf(rep.residOn, r.byteMean, r.gradMax, kBandMidByte, r.tHi,
        r.gradThr, r.tau);
    if (!off.valid() || !on.valid()) {
        std::printf("  ✗ 分带表无像素（total=0），不出数\n");
        return "未出（无像素）";
    }
    printMovingBandTable("TAA 关（= 移动锯齿本体，**主表**）", off, "单应 warp 残差（TAA 关）");
    printMovingBandTable("TAA 开（= 开 TAA 之后还剩的闪烁）", on, "单应 warp 残差（TAA 开）");
    const MovingBandVerdict v = movingBandVerdict(off, on);
    printMovingBandRates(v);
    printMovingBandBranch(v);
    printMovingBandSensitivity(rep);
    printMovingBandMaterials(rep);
    (void)rep.targetName;
    return movingBandBranchName(v.branch);
}

}  // namespace RGL::imgqa
