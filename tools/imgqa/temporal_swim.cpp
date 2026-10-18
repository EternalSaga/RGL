//
// temporal_swim.cpp —— `taa-swim` 的**仪器层**（cmdTaaSwim 与它的阶段函数）
//
// 拆出本 TU 的理由与缝的全文见 `temporal_swim.h` 头注释（一句话：口径层算子在
// `temporal_swim_metric.cpp`，本文件是阶段函数 + 打印 + 判决线；打印顺序与判决线 = 契约）。
// 下面这段原头注释（cmdTaaSwim 的仪器说明，含两版废掉的量具的实测数据）随命令搬来，
// 别去别的文件找。
//
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
// 五组采集（全部真实 bistro：env/IBL/AO/shadow 全开；静止机位）：
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

#include "temporal_swim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

// 窗口对齐 / lag 表 / 游动闭式期望都要引擎抖动周期（与口径层同源 = `import taa_jitter`）。
import taa_jitter;

namespace RGL::imgqa {
namespace {

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

/// 七组采集产物（T0..T6）——collectSwimGroups 填，读数表与判决线读。
struct SwimGroups {
    SwimRun t0, t1, t2, t3, t4, t5, t6;
};

/// 绝对抖动传递段的读数（reportAbsoluteTransfer 填，判决线与 logCall 读）。
struct AbsReadings {
    FreeShift anchor, anchor0, anchorHalf;
    AbsStat abs1, abs2, abs5, abs6, abs8;
};

/// T3 恒等式段的读数（computeIdentityReadings 填，identity 判决线与 logCall 读）。
struct IdentityReadings {
    double d3 = 0.0, mx3 = 0.0, devFrac = 0.0, edgeFracDev = 0.0;
    int nBad = 0;
};

/// 离未抖基图 R 的平均偏离（画质量，不是平移判据——理由见 LagStat 头注释；原 distTo lambda）。
double distToRef(const cv::Mat& ref, const std::vector<cv::Mat>& fr)
{
    double s = 0.0;
    for (const auto& f : fr)
        s += meanAbs(f, ref);
    return s / static_cast<double>(fr.size());
}

/// 七组采集（T0..T6，每组一个独立引擎进程；try/catch 与「采集失败」出口逐字保留）。
/// 各组为什么存在——注释跟着各自的采集调用走（下面原样）。
int collectSwimGroups(SwimGroups& g, const std::string& root,
    int shots, int nWarm)
{
    try {
        g.t0 = runSwimSequence(root + "/imgqa_swim_t0", false, 0.12, 1.0, shots, nWarm,
            "swimt0", static_cast<int>(kTaaJitterPeriod));
        g.t1 = runSwimSequence(root + "/imgqa_swim_t1", true, 1.0, 1.0, shots, nWarm,
            "swimt1", static_cast<int>(kTaaJitterPeriod));
        g.t2 = runSwimSequence(root + "/imgqa_swim_t2", true, 0.12, 1.0, shots, nWarm,
            "swimt2", static_cast<int>(kTaaJitterPeriod));
        // T3 **恒等式组**：jitter_scale=0 + 静止机位 ⟹ 每帧画面相同、prevUv 必须恰为 uv
        // ⟹ 历史 = 同一张图 ⟹ 收敛输出必须与 TAA 关**逐位相同**。本项目最便宜且不可辩驳
        // 的判据形态（`taa_plan` §7 第 12 条可复用判断①：先把它自己的恒等式做成判据）。
        // 它不依赖参考实现、不依赖 Halton 口径、不依赖观测域选择。
        g.t3 = runSwimSequence(root + "/imgqa_swim_t3", true, 0.12, 0.0, shots, nWarm,
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
        g.t4 = runSwimSequence(root + "/imgqa_swim_t4", true, 1.0, 8.0, shots, nWarm,
            "swimt4", static_cast<int>(kTaaJitterPeriod));
        // T5/T6 **基空间组**（批 13.2，门 `taa.base_space`）：与 T2/T1 **只差一个门**，
        // 于是「门关读数」与「门开读数」在同一次运行、同一机位、同一交付配置下直接可比
        // ——这正是 `temporal-base-space` 的阳性锚点结构（锚点与被测项共用一切噪声源）。
        //   T5 = T2 + 门开（现行工作点 fb=.12 js=1）：判据主体，游动应显著下降。
        //   T6 = T1 + 门开（fb=1，无累积）：把「重采样本身引入的帧间差」与「游动」分开。
        //        fb=1 时输出 = 本帧光栅重采样到基网格，相邻帧之间**没有**整幅平移项，
        //        只剩「按亚像素偏移重新光栅化」的覆盖翻转地板（≈ T4 交付场景档 37%）。
        g.t5 = runSwimSequence(root + "/imgqa_swim_t5", true, 0.12, 1.0, shots, nWarm,
            "swimt5", static_cast<int>(kTaaJitterPeriod), true);
        g.t6 = runSwimSequence(root + "/imgqa_swim_t6", true, 1.0, 1.0, shots, nWarm,
            "swimt6", static_cast<int>(kTaaJitterPeriod), true);
    } catch (const std::exception& e) {
        std::printf("  采集失败: %s\n", e.what());
        return 2;
    }
    return 0;
}

/// 七组读数表 + 「离未抖基图 R 的平均偏离」两行（打印逐字）。
void printSwimRows(const SwimGroups& g, const cv::Mat& ref)
{
    const auto row = [](const char* name, const SwimRun& r) {
        std::printf("  %-27s 逐帧|Δ| %6.3f/255   同相位差 %6.3f/255   梯度能量 %s\n", name,
            r.consecDiff, r.lagDiff, d6(r.gradEnergy).c_str());
    };
    row("T0 TAA 关（地板）", g.t0);
    row("T1 TAA开 fb=1 js=1（只抖不滤）", g.t1);
    row("T2 TAA开 fb=.12 js=1（现行）", g.t2);
    row("T3 TAA开 fb=.12 js=0（恒等式）", g.t3);
    row("T4 TAA开 fb=1 js=8（本帧+大抖动）", g.t4);
    row("T5 = T2 + base_space 开（基空间）", g.t5);
    row("T6 = T1 + base_space 开（基空间不累积）", g.t6);
    std::printf("\n  离未抖基图 R 的平均偏离（画质量，不是平移判据）：T1 %.3f  T2 %.3f  "
        "T3 %.3f  T4 %.3f /255\n",
        distToRef(ref, g.t1.bytes), distToRef(ref, g.t2.bytes),
        distToRef(ref, g.t3.bytes), distToRef(ref, g.t4.bytes));
    std::printf("  ⚠ T3 一列**必须恰为 0**（恒等式：零抖动 + 静止 ⟹ TAA 开 ≡ 关）。\n");
}

/// 相位标定 + λ 扫描（**顺序即正确性**：任何按预测位移做的表都必须排在标定之后——
/// 理由全文见下面搬来的原注释）。ps / scan 是本命令后面所有 lag 表与判决线的输入。
void reportPhaseAndLambda(const SwimGroups& g, PhaseScan& ps, ScanCurve& scan)
{
    // ── 相位标定（**必须先于一切「按预测位移搬」的表**）────────────────────────
    // 采集下标与引擎抖动序号之间差一个未知常数相位（引擎 frameIndex 自己起算 + 序号再
    // +1 + 每次启动的 RPC 登记落在第几帧）。本轮实测：同一组数据，相位错位时「搬对相位」残差
    // 86.7%，标定后 14.4% —— 同一个算子，结论从「帧间差不是平移」翻成「帧间差就是平移」。
    // ⟹ 顺序即正确性：任何按预测位移做的表都必须排在标定之后。
    // T4 的 fb=1 让输出**精确**等于本帧光栅，于是相邻两帧之间按构造就是 Halton 闭式位移的
    // 整幅平移。但「采集第 i 帧」与「引擎抖动序号 i」之间差一个未知常数相位（引擎 frameIndex
    // 自己起算 + 序号再 +1），λ 扫描必须先标定掉它，否则每对帧的预测方向都是错位的——
    // 本轮实测：不标定相位时 ±4px 档搬对搬错都是 ~100%，看起来像「画面没被搬起来」。
    ps = phaseScan(g.t4.bytes, 8.0);
    std::printf("\n  相位标定（T4 = 本帧 + ±4px 抖动；16 个候选常数相位，15 帧对均值）\n");
    for (size_t ph = 0; ph < ps.resid.size(); ++ph)
        std::printf("    φ=%2zu  残差 %7.3f/255  (%5.1f%% of 不搬)%s\n", ph, ps.resid[ph],
            100.0 * ps.resid[ph] / (ps.zero > 0 ? ps.zero : 1.0),
            static_cast<int>(ph) == ps.best ? "   ← argmin" : "");
    std::printf("  → 真相位 φ = %d，残差压到 %.1f%%（不搬基线 %.3f/255）\n", ps.best,
        100.0 * ps.bestVal / (ps.zero > 0 ? ps.zero : 1.0), ps.zero);

    scan = lambdaScan(g.t4.bytes, 8.0, static_cast<uint32_t>(ps.best));
    std::printf("\n  λ 扫描（在标定出的相位 φ=%d 上；沿预测位移方向带符号，λ=+1 为真值）\n",
        ps.best);
    for (const auto& [lam, r] : scan.pts)
        std::printf("    λ=%+5.2f  残差 %7.3f/255  (%5.1f%% of 不搬)%s\n", lam, r,
            100.0 * r / (scan.zero > 0 ? scan.zero : 1.0),
            lam == scan.bestLam ? "   ← argmin" : "");
    std::printf("  → argmin λ = %+.2f  最深处压到 %.1f%%\n", scan.bestLam,
        100.0 * scan.best / (scan.zero > 0 ? scan.zero : 1.0));
}

/// 绝对抖动传递段：拟合器三探针 + 五组 absMeasure + 全部读数打印（打印逐字）。
void reportAbsoluteTransfer(const SwimGroups& g, const cv::Mat& ref, AbsReadings& ar)
{
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
    cv::Mat probe;
    cv::warpAffine(ref, probe, cv::Matx23d(1, 0, -kAnchorDx, 0, 1, -kAnchorDy), ref.size(),
        cv::INTER_CUBIC, cv::BORDER_REPLICATE);
    ar.anchor = freeShift(ref, probe);
    // **反向用例**（堵「拟合器卡常数/直流泄漏」——只有阳性锚点时，一个恒返回常数的坏拟合器
    // 也是绿的）：零位移必须量回 |d| ≤ 0.25px。
    ar.anchor0 = freeShift(ref, ref.clone());
    // 半整数奇点诊断（只打印不判，见上）：它若从 −0.32 漂走 = OpenCV 亚像素估计器换了实现。
    cv::Mat probeHalf;
    cv::warpAffine(ref, probeHalf, cv::Matx23d(1, 0, -3.5, 0, 1, 2.25), ref.size(),
        cv::INTER_CUBIC, cv::BORDER_REPLICATE);
    ar.anchorHalf = freeShift(ref, probeHalf);
    std::printf("\n  绝对抖动传递（相位相关：抖动帧 ↔ 零抖动参考 R）\n");
    std::printf("\n  拟合器自检：已知位移 (%+.2f,%+.3f) → 量回 (%+.2f,%+.2f)，残差塌到 %.1f%%"
                "（须 |误差|≤0.25px 且 <10%%）｜零位移反向用例 (%+.3f,%+.3f)｜半整数奇点诊断"
                " (3.50,−2.25) → (%+.2f,%+.2f)（只打印不判，理由见锚点注释）\n",
        kAnchorDx, kAnchorDy, ar.anchor.dx, ar.anchor.dy,
        100.0 * ar.anchor.resid / (ar.anchor.zero > 0 ? ar.anchor.zero : 1.0),
        ar.anchor0.dx, ar.anchor0.dy,
        ar.anchorHalf.dx, ar.anchorHalf.dy);
    std::printf("  %-4s %-19s %-19s %-9s %-7s %s\n", "帧", "实测 d=(dx,dy)px", "预测 js·j(i+φ)",
        "|d|px", "峰值", "残差塌到");
    const AbsMeasure am8 = absMeasure(ref, g.t4.bytes, 8.0);
    ar.abs8 = am8.st;
    // 逐帧读数表（原 absMeasure 的 verbose 支路，打印逐字）；预测列按匹配出的 (φ,sx,sy)
    // 现算——与 absMeasure 内部同一纯函数同口径，不另存一份状态。
    for (size_t i = 0; i < am8.rows.size() && i < 8; ++i) {
        const AbsRow& r = am8.rows[i];
        const auto j =
            jitterDeltaPx(static_cast<uint32_t>(i) + static_cast<uint32_t>(ar.abs8.ph), 8.0);
        const double ex = ar.abs8.sx * j[0], ey = ar.abs8.sy * j[1];
        std::printf("    %-4zu (%+6.2f,%+6.2f)  (%+6.2f,%+6.2f)  |d|=%-6.2f 峰 %-5.3f"
                    " 残差塌到 %5.1f%%\n",
            i, r.dx, r.dy, ex, ey, std::hypot(r.dx, r.dy),
            r.peak, 100.0 * r.resid / (r.zero > 0 ? r.zero : 1.0));
    }
    std::printf("    … （共 %zu 帧，只列前 8）\n", g.t4.bytes.size());
    std::printf("  T4 js=8  ：实测|d| %.3fpx  预测 %.3fpx  逐帧误差 %.3fpx  RMS %.3fpx"
                "（φ=%d 符号 x%+d y%+d）  峰值 %.3f\n",
        ar.abs8.meas, ar.abs8.pred, ar.abs8.err, ar.abs8.rms, ar.abs8.ph, ar.abs8.sx, ar.abs8.sy,
        ar.abs8.peak);
    std::printf("  T4 js=8  ：位移把「抖动帧 ↔ R」的差从 %.3f/255 压到 %.3f/255（%.1f%%）\n",
        ar.abs8.zero, ar.abs8.resid,
        100.0 * ar.abs8.resid / (ar.abs8.zero > 0 ? ar.abs8.zero : 1.0));
    ar.abs1 = absMeasure(ref, g.t1.bytes, 1.0).st;
    ar.abs2 = absMeasure(ref, g.t2.bytes, 1.0).st;
    // 基空间组（批 13.2）：与 abs2 **只差一个门**，`swing` 直接可比。
    // ⚠ `meas`/`err`/`rms` 这几列对基空间组**无意义**（它们拿 Halton 预测集去匹配，
    //   而基空间组按设计就不该有位移）；只有 `swing` 与 `resid/zero` 是要读的量。
    ar.abs5 = absMeasure(ref, g.t5.bytes, 1.0).st;
    ar.abs6 = absMeasure(ref, g.t6.bytes, 1.0).st;
    const auto line = [](const char* nm, const AbsStat& st) {
        std::printf("  %-9s：实测|d| %.3fpx  预测 %.3fpx  逐帧误差 %.3fpx  RMS %.3fpx"
                    "（φ=%d 符号 x%+d y%+d）  峰值 %.3f  残差塌到 %5.1f%%\n",
            nm, st.meas, st.pred, st.err, st.rms, st.ph, st.sx, st.sy, st.peak,
            100.0 * st.resid / (st.zero > 0 ? st.zero : 1.0));
    };
    line("T1 fb=1", ar.abs1);
    line("T2 fb=.12", ar.abs2);
    std::printf("  ⟹ **游动量（像素，直接对应肉眼）**：T2 相邻帧整幅位移 均值 %.3fpx  最大 %.3fpx"
                "   ｜  T1（不累积）均值 %.3fpx  最大 %.3fpx\n",
        ar.abs2.swing, ar.abs2.swingMax, ar.abs1.swing, ar.abs1.swingMax);
    std::printf("  ⟹ **基空间（base_space 开）**：T5 相邻帧整幅位移 均值 %.3fpx  最大 %.3fpx"
                "   ｜  T6（不累积）均值 %.3fpx  最大 %.3fpx\n",
        ar.abs5.swing, ar.abs5.swingMax, ar.abs6.swing, ar.abs6.swingMax);
    std::printf("     离零抖动参考 R 的平均偏离：T2 %.3f  T5 %.3f /255   "
                "平移模型残差塌到：T2 %.1f%%  T5 %.1f%%\n",
        distToRef(ref, g.t2.bytes), distToRef(ref, g.t5.bytes),
        100.0 * ar.abs2.resid / (ar.abs2.zero > 0 ? ar.abs2.zero : 1.0),
        100.0 * ar.abs5.resid / (ar.abs5.zero > 0 ? ar.abs5.zero : 1.0));
}

/// T3 恒等式读数段：收敛段离基图均值/最大 + 残差归属（边缘占比）+ 两行打印（逐字）。
IdentityReadings computeIdentityReadings(const SwimGroups& g, const cv::Mat& ref)
{
    // 判据只取**收敛段**（理由见 convergedTail 注释：前段是历史瞬态 = 收敛速度，不是缺陷）
    const std::vector<cv::Mat> t3 = convergedTail(g.t3.bytes);
    const double d3 = distToRef(ref, t3);
    const double mx3 = maxDistTo(ref, t3);
    // 残差**集中在哪**？sub-ULP 的重投影差经 CR 核放大只可能发生在高对比边缘（核在 t≈0 附近
    // 对 t 的导数最大）。这句话不能只写在注释里——算出来：边缘 = 梯度 ≥8/255 的像素。
    double edgeFracAll = 0.0, edgeFracDev = 0.0;
    int nBad = 0;
    {
        cv::Mat gray, gx, gy, dev;  // gray = 原局部 g（本函数参数 g 是采集组，避免遮蔽）
        cv::cvtColor(ref, gray, cv::COLOR_BGR2GRAY);
        cv::Sobel(gray, gx, CV_32F, 1, 0, 1);
        cv::Sobel(gray, gy, CV_32F, 0, 1, 1);
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
    IdentityReadings id;
    id.d3 = d3; id.mx3 = mx3; id.devFrac = devFrac; id.nBad = nBad; id.edgeFracDev = edgeFracDev;
    return id;
}

/// 判决线 1/3：确定性地板 + 免费标定锚 + 恒等式（判据名/阈值/容差理由逐字未动）。
void pushGuardVerdicts(std::vector<Verdict>& vs, const SwimGroups& g, const IdentityReadings& id,
    int nWarm)
{
    vs.push_back(verdictFor("determinism", g.t0.consecDiff == 0.0,
        "TAA 关 + 静止机位连拍：逐帧差均值 " + d6(g.t0.consecDiff)
            + "/255（非零则一切比值是噪声比噪声）"));
    // 同相位（lag = 一个周期）差的容差**不是拍的，是 IIR 尾巴的闭式**：fb=0.12 时 n 帧前的
    // 样本权重 = f(1−f)^(n−1)，预热 W 帧后残留的非周期成分 ≲ (1−f)^W × 满幅逐帧变化。
    // (1−0.12)^32 = 0.0167，满幅取 2.0/255（宽松）⟹ 容差 0.033/255。实测 0.0033，留 10× 余量。
    // 而 fb=1 的 T1 **没有记忆** ⟹ 必须**恰为 0**，一个 bit 都不许差。
    const double iirTol = std::pow(1.0 - 0.12, static_cast<double>(nWarm)) * 2.0;
    vs.push_back(verdictFor("temporal-swim-anchor",
        g.t1.lagDiff == 0.0 && g.t2.lagDiff <= iirTol,
        "免费标定（lag = 一个完整抖动周期 ⟹ 预测位移恰为 0）：T1 同相位差 " + d6(g.t1.lagDiff)
            + "、T2 同相位差 " + d6(g.t2.lagDiff)
            + "/255。T1（fb=1，**无记忆**）须**恰为 0**；T2（fb=.12，有记忆）容差 = IIR 尾巴"
              "闭式 (1−f)^warm × 2 = "
            + d6(iirTol)
            + "（预热 32 帧残留 0.0167 的满幅成分）。非零超界 = 连拍不是周期 16 的确定性函数"
              "（窗口没对齐 / 历史没收敛 / 帧不连续），此时任何按 Halton 预测做的判据都不作数"));
    vs.push_back(verdictFor("temporal-identity",
        id.d3 <= 0.05 && id.mx3 <= 4.0 && id.devFrac <= 0.0001
            && (id.nBad < kEdgeAttributionFloor || id.edgeFracDev >= 0.8) && g.t3.consecDiff < 0.01,
        "恒等式（零抖动 + 静止机位 ⟹ 历史与本帧同图、prevUv 必须落在 uv 上 ⟹ TAA 开 ≡ TAA 关）："
        "离基图均值 "
            + d6(id.d3) + "/255、逐像素最大 " + std::to_string(static_cast<int>(id.mx3))
            + "/255、dev≥2 共 " + std::to_string(id.nBad) + " 像素（" + d2(id.devFrac * 100.0)
            + "%）、逐帧差 " + d6(g.t3.consecDiff)
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
}

/// 相位唯一性：次小残差必须显著高于最小残差，否则「argmin φ」只是噪声里挑出来的，
/// 拿它去解释 λ 扫描 = 用一把没刻度的尺。
double secondBestPhase(const PhaseScan& ps)
{
    double secondBest = 1e30;
    for (size_t ph = 0; ph < ps.resid.size(); ++ph)
        if (static_cast<int>(ph) != ps.best)
            secondBest = std::min(secondBest, ps.resid[ph]);
    return secondBest;
}

/// 预测的相邻帧位移差（一个周期内的均值）——游动的**闭式期望**，零自由参数。
double predLag1ClosedForm()
{
    double predLag1 = 0.0;
    for (uint32_t i = 0; i < kTaaJitterPeriod; ++i) {
        const auto d0 = jitterDeltaPx(i, 1.0), d1 = jitterDeltaPx(i + 1u, 1.0);
        predLag1 += std::hypot(d1[0] - d0[0], d1[1] - d0[1]);
    }
    predLag1 /= static_cast<double>(kTaaJitterPeriod);
    return predLag1;
}

/// 判决线 2/3 + 3/3：拟合器锚点 + 抖动传递 + 游动量 + 基空间门 + 相位唯一性 + λ 标定 +
/// 算子区分力（判据名/阈值/容差理由逐字未动；push 顺序 = 原判决线现序）。
void pushCalibrationVerdicts(std::vector<Verdict>& vs, const AbsReadings& ar, const PhaseScan& ps,
    const ScanCurve& scan, const std::array<double, 2>& a4)
{
    const double secondBest = secondBestPhase(ps);
    vs.push_back(verdictFor("temporal-fitter-anchor",
        std::abs(ar.anchor.dx - kAnchorDx) <= 0.25 && std::abs(ar.anchor.dy - kAnchorDy) <= 0.25
            && ar.anchor.resid < 0.1 * ar.anchor.zero
            && std::hypot(ar.anchor0.dx, ar.anchor0.dy) <= 0.25,
        "拟合器阳性锚点 + 反向用例：把 R 按**已知**位移 (3.25,−2.125)（与实测量程同档：T4 位移"
        " = 8j ∈ ℤ、T1/T2 = j ∈ {±⅛,±¼,±⅜}）做成假抖动帧，相位相关必须量回来：实测 ("
            + d6(ar.anchor.dx) + "," + d6(ar.anchor.dy) + ")（各轴误差 ≤0.25px），且位移把残差压到 "
            + d2(100.0 * ar.anchor.resid / (ar.anchor.zero > 0 ? ar.anchor.zero : 1.0))
            + "%（须 <10%）；**反向用例**零位移须量回 |d| ≤0.25px（堵『拟合器卡常数』——只有"
              "阳性锚点时恒返回常数的坏拟合器也是绿的，实测 ("
            + d6(ar.anchor0.dx) + "," + d6(ar.anchor0.dy)
            + "））。下面所有『实测位移』都建立在这个拟合器上，它自己没有独立判据的话，整组读数"
              "无刻度。⚠ 本条不是形式主义：第一版手写网格拟合缺这条，尺度写错（把 1/4 降采样步长"
              "当全分辨率像素）却一路打印自相矛盾的假数。阈值 0.25px/10% **未放宽**（2026-10-04 "
              "换探针位移而非换阈值）：旧锚点 (3.50,−2.25) 的 x 恰为半整数，踩在 phaseCorrelate "
              "三点抛物线亚像素拟合的奇点上——本场景 14 组已知位移实测：半整数处误差恒 "
              "−0.320±0.002px（x∈{1.5,2.5,3.5,7.5,−3.5}），四分之一整数/整数处 ≤0.002px ⟹ 旧"
              "读数红的是**估计器奇点不是拟合器分辨率**（奇点诊断仍打印：anchorHalf）。区分力："
              "尺度错 2× → ≥3.25px、符号反 → ≥6.5px、整数错位 → ≥1px，全部 ≫0.25px"));
    vs.push_back(verdictFor("temporal-jitter-transfer",
        std::abs(ar.abs8.meas - ar.abs8.pred) <= 0.25 * ar.abs8.pred
            && ar.abs8.err <= 0.25 * ar.abs8.pred
            && ar.abs8.resid <= 0.5 * ar.abs8.zero && ar.abs8.sx == 1 && ar.abs8.sy == 1,
        "抖动传递（**绝对口径**，js=8 大信号档）。四条各自堵一种失败：① 实测位移模长 "
            + d6(ar.abs8.meas) + "px vs 预测 " + d6(ar.abs8.pred)
            + "px（相对误差 ≤25% —— 堵『抖动没注入』与 px/NDC 单位错配，后者会差 2× 以上）；"
              "② 逐帧误差 "
            + d6(ar.abs8.err) + "px ≤25% 预测均值（堵『搬了但不是按 Halton 序列搬』）；③ 位移把"
              "『抖动帧 ↔ 零抖动 R』的差压到 "
            + d2(100.0 * ar.abs8.resid / (ar.abs8.zero > 0 ? ar.abs8.zero : 1.0))
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
              "实测 φ=" + std::to_string(ar.abs8.ph)
            + "（φ 是**每次启动的常数**：跨组实测 3/11/15 不等，预热长度对齐周期也压不住——"
              "RPC 登记落在第几帧由启动时序决定。所以 φ 只打印、不判，判了就是假红）"));
    const double predLag1 = predLag1ClosedForm();
    vs.push_back(verdictFor("temporal-swim-pixels",
        ar.abs2.swing >= 0.75 * predLag1 && ar.abs2.swing <= 1.35 * predLag1,
        "**游动量（单位=像素，直接对应肉眼**，phase13 §1.9 的验收量）：现行工作点（fb=.12 "
        "js=1）相邻两帧之间的整幅位移 均值 "
            + d6(ar.abs2.swing) + "px（最大 " + d6(ar.abs2.swingMax) + "px），闭式期望 = 相邻抖动差 = "
            + d6(predLag1)
            + "px（须落在 [0.75, 1.35]×）。**这条钉的是机制不是好坏**：若输出落在基空间，游动应"
              "≈0；实测 = 抖动差 ⟹ **输出在抖动空间**，且时域累积压不掉它（同表 T1 不累积 = "
            + d6(ar.abs1.swing) + "px，几乎同值）。要不要消掉是产品决策，不在本判据里判"));
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
        ar.abs5.swing <= kBaseSpaceSwimCap * predLag1 && ar.abs2.swing >= 0.75 * predLag1
            && ar.abs5.swing <= 0.5 * ar.abs2.swing,
        "**基空间输出（`taa.base_space`）主判据**：门开态（T5 = T2 + 门）相邻帧整幅位移 均值 "
            + d6(ar.abs5.swing) + "px（最大 " + d6(ar.abs5.swingMax) + "px），须 ≤"
            + d2(100.0 * kBaseSpaceSwimCap) + "% × 闭式抖动差 " + d6(predLag1)
            + "px，且 ≤ 门关态 T2 的 " + d6(ar.abs2.swing)
            + "px 的一半。阳性锚点 = 同一次运行的门关态 T2 仍 ≥75% 闭式（"
            + d6(ar.abs2.swing)
            + "px）——没有它，① 可能只是量具坏了。T6（门开 + fb=1，无累积）游动 "
            + d6(ar.abs6.swing)
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
}

}  // namespace

/// 一组连拍的逐 lag 平移检验读数表（打印逐字）；返回 {搬对相位残差/实测, 搬错相位/实测}
/// ——判决线 temporal-swim-discriminative 的那两个数。
/// 声明在 `temporal_swim.h`（不在匿名 namespace：selftest 要直接喂合成帧序列核「输出行数
/// 不随噪声变」——债-94 的形态正是 stdout 行数是彩票，任何逐行/逐字节下游判据全失效）。
std::array<double, 2> lagTable(const char* name, const std::vector<cv::Mat>& frames,
    double jitterScale, uint32_t phase)
{
    std::printf("  %s\n", name);
    double sumWrong = 0.0, sumMeas = 0.0, sumResid = 0.0;
    int counted = 0;
    for (int k = 1; k <= static_cast<int>(kTaaJitterPeriod); ++k) {
        const LagStat s = lagTranslation(frames, k, jitterScale, phase);
        // 跳过条件**按语义**写（债-94）：k = 抖动周期那行是免费标定（预测位移恰为 0，不是
        // 逐 lag 读数），恒不进表。旧写法 `s.meas == 0.0 && s.pred == 0.0 && k > 1` 拿实测
        // 浮点精确相等当条件 ⟹ meas 随噪声离开 0 的那一行就多印一行——stdout 行数是彩票
        // （台账实测同一份代码两轮 15 行 vs 16 行），任何逐行/逐字节的下游判据全失效。
        // 汇总不受影响（`k < kTaaJitterPeriod` 本就把这行排除在累加外）。
        if (k == static_cast<int>(kTaaJitterPeriod))
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
}

int cmdTaaSwim(int pairs, int warm)
{
    const auto up = [](int v, int p) { return ((v + p - 1) / p) * p; };
    const int nPairs = up(pairs, static_cast<int>(kTaaJitterPeriod));
    const int nWarm = up(warm, static_cast<int>(kTaaJitterPeriod));
    if (nPairs != pairs || nWarm != warm)
        std::printf("⚠ 窗口对齐抖动周期 %u：pairs %d→%d、warm %d→%d（周期内帧间跳变幅度不均，"
            "见 taa_plan §6 末）\n\n", kTaaJitterPeriod, pairs, nPairs, warm, nWarm);
    const int shots = nPairs + 1;
    const std::string root = projectRoot() + "/build";
    std::printf("真实场景 TAA 游动量测：1280×720  bistro（env/IBL/AO/shadow 全开）  "
        "静止机位  预热 %d 帧  帧对 %d\n"
        "假设：收敛后 out_i(uv) = F(uv − jitter_i) ⟹ 相隔 k 帧的两帧是精确平移 (j_{i+k} − j_i)\n\n",
        nWarm, nPairs);
    SwimGroups g;
    if (const int rc = collectSwimGroups(g, root, shots, nWarm))
        return rc;

    // 离未抖基图多远（T0 逐帧差恰为 0 ⟹ 取第一帧当基图 R）。这条**不是**平移判据
    // （理由见 LagStat 头注释），它是「输出偏离正确画面多少」的画质量。
    const cv::Mat ref = g.t0.bytes.front();
    printSwimRows(g, ref);

    PhaseScan ps;
    ScanCurve scan;
    reportPhaseAndLambda(g, ps, scan);

    std::printf("\n  %-4s %-8s %-10s %-10s %-10s %-10s\n", "lag", "预测位移", "实测|Δ|",
        "搬对相位", "搬错相位", "搬对/实测");
    const auto a2 = lagTable("T2 TAA开 fb=.12 js=1（现行，±0.5px）", g.t2.bytes, 1.0,
        static_cast<uint32_t>(ps.best));
    const auto a4 = lagTable("T4 TAA开 fb=1  js=8（本帧，±4px）", g.t4.bytes, 8.0,
        static_cast<uint32_t>(ps.best));

    AbsReadings ar;
    reportAbsoluteTransfer(g, ref, ar);

    std::printf("\n  读法：残差塌到近零 ⟹ 两帧之间确是整幅亚像素平移（**游动**）。\n"
        "        残差 ≈ 实测（搬不动）⟹ 帧间差不是平移，是逐像素覆盖翻转/历史被拒。\n"
        "        「搬错相位」是反向对照：它若也压到同样低，本算子没有区分力，两条都不作数。\n"
        "        lag 16 是免费标定：预测位移恰为 0，实测差必须恰为 0（同相位 = 同画面）。\n"
        "        ±0.5px 档算子可能没力气（首轮实测如此）⟹ 看 T4 的 ±4px 档 + 下面的 λ 扫描。\n");
    const IdentityReadings id = computeIdentityReadings(g, ref);
    std::vector<Verdict> vs;
    pushGuardVerdicts(vs, g, id, nWarm);
    pushCalibrationVerdicts(vs, ar, ps, scan, a4);

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("taa-swim", {{"pairs", std::to_string(nPairs)}, {"warm", std::to_string(nWarm)},
               {"T0_consec", d6(g.t0.consecDiff)}, {"T1_consec", d6(g.t1.consecDiff)},
               {"T2_consec", d6(g.t2.consecDiff)}, {"T3_consec", d6(g.t3.consecDiff)},
               {"T4_consec", d6(g.t4.consecDiff)},
               {"T1_lag16", d6(g.t1.lagDiff)}, {"T2_lag16", d6(g.t2.lagDiff)},
               {"T1_distR", d6(distToRef(ref, g.t1.bytes))},
               {"T2_distR", d6(distToRef(ref, g.t2.bytes))},
               {"T3_distR", d6(id.d3)}, {"T4_distR", d6(distToRef(ref, g.t4.bytes))},
               {"T2_residFrac", d2(a2[0] * 100.0)}, {"T2_wrongFrac", d2(a2[1] * 100.0)},
               {"T4_residFrac", d2(a4[0] * 100.0)}, {"T4_wrongFrac", d2(a4[1] * 100.0)},
               {"T2_grad", d6(g.t2.gradEnergy)}, {"T0_grad", d6(g.t0.gradEnergy)},
               {"T5_swing", d6(ar.abs5.swing)}, {"T5_swingMax", d6(ar.abs5.swingMax)},
               {"T6_swing", d6(ar.abs6.swing)},
               {"T5_distR", d6(distToRef(ref, g.t5.bytes))}, {"T5_grad", d6(g.t5.gradEnergy)},
               {"T3_max", std::to_string(static_cast<int>(id.mx3))},
               {"T4_phase", std::to_string(ps.best)},
               {"T4_phase_resid", d6(ps.bestVal)}, {"T4_zero", d6(ps.zero)},
               {"T4_scan_argminLam", d2(scan.bestLam)},
               {"T4_scan_best", d6(scan.best)}, {"T4_scan_zero", d6(scan.zero)}},
        vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
