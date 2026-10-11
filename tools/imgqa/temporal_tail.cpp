// temporal-tail（spin 版）—— **鬼影尾长直测**（批次 B11 从 `temporal_motion.cpp` 拆出：
// 同一文件里的**第三台仪器**）。设计全文 = 下面原样搬来的头注释；`TailRow` 与 `measureTail`
// 的口径算子在 `temporal_motion_metric.cpp`（缝 = 仪器 vs 口径）。
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

// ══════════════════════════════════════════════════════════════════════════════
// 批 13.13 `temporal-tail`：**鬼影尾长直测**（债-63 的出口）
// ══════════════════════════════════════════════════════════════════════════════
//
// ## 为什么必须有这条（问题本体）
//
// 整个臂族（8/9/10/11）的交付裁决原本压在 `temporal-motion --motion spin` 的鬼影读数上，
// 而那条仪器**过不了自己的守卫**（alignment 余量 0.000065 ≪ 0.02、ablation 1.492 < 1.5），
// `git:调试历史:2521` 早就写着「那张表不可信」⟹ 债-63：臂 0/8/9/10 的鬼影数字**全部作废**。
// 本条是它的出口，设计目标只有一条：**整条链上不出现任何图像配准**。
//
// ## 为什么这里可以不对齐（而 temporal-motion 必须对齐）
//
// 运动源 = **场景 spin**、相机**静止** ⟹ 屏幕像素与世界格点的对应不随时间变（动的只有内容）。
// 于是「候选第 i 帧」与「参考第 i 帧」比的是**同一个屏幕像素**，不存在两个视点要配准。
// 跨进程的**绝对帧号偏移**由引擎自己的计数器消掉：spin 角 = `spin_frame_angle × 帧号` 是纯函数，
// 而每组 burst 的起始帧号 = `Engine::frameCount()` 回显 ⟹ 偏移是一个**整数常量**，
// 零阈值、零 argmin。（temporal-motion 那边相机在动，第 i 帧与第 j 帧是不同视点，只能图像
// 配准 + 余量护栏——而那两条护栏正是它常红的地方。换运动源 = 换掉判据里最脆的那一层。）
//
// ## 参考 = 同一次 spin、TAA 关、**jitter_scale = 0**
//
// TAA 关 ⟹ 没有历史 ⟹ 每帧就是「这个像素此刻该是什么」的真值（不需要基线图像）。
// ⚠ **参考必须关抖动**：候选开 `base_space` ⟹ 它交付的是**基位置**样本；参考若带抖动就是
//   抖动位置渲染 ⟹ 边缘上凭空多出 ≤0.5px 错位，而 1px 高亮细线正是本判据的激励源。
//   候选侧照旧开抖动（被测项的动力学必须真实），两边在**基位置**上对齐。
//
// ## 主指标 = 归一化鬼影份额 frac（无量纲）
//
// 「刚让开」事件 (p, j)，记 `d_j(p) = |ref_j(p) − ref_{j−1}(p)|`：
//   `d_j > τ` **且** `d_{j−1} ≤ τ`（**刚**让开，不是一直在动）
//   **且** `d_{j+1..j+K} ≤ τ`（让开后内容稳住；否则第 k 帧的残差混的是**新**鬼影）
//
//     frac = mean_j |out_j(p) − ref_j(p)|  ÷  mean |ref_j(p) − ref_{j−1}(p)|
//
// 分母 = 这次让开的内容变化量 `|N − S|` ⟹ frac **无量纲**、跨机位/跨场景可比，且有闭式锚点：
// 盒全关（臂 1）时 `out_j = α·N + (1−α)·S` ⟹ **frac = 1 − α**（α = 生效的本帧权重）。
// 盒起效就是把历史拽回盒边界 ⟹ frac < 1 − α。**越小越好，0 = 无鬼影。**
//
// ## 判决线（跑前写死，四条 AND；红一条就不许报结论）
//
//  1. `temporal-tail-pairing` **零锚点**：Z = TAA 开 + `feedback = 1` + `jitter_scale = 0`。
//     ⚠⚠ **Z 的 α 必须真的 = 1**（历史红线，批 13.14b 前踩过：首轮实测 frac(Z)=0.199 红，
//     根因是当年那个 static boost 机制把「相机静止时的**本帧权重**」压到地板，而本命令相机
//     **恒静止** ⟹ `feedback = 1` 被整个覆盖（α 实际 = 地板值，一百帧窗）。机制已删，
//     红线随之结案；留话防复发：**任何用 `feedback` 定义「无历史 / 短历史」的实验，都要先
//     确认没有第二个旋钮在覆写 α**（债-55 同型：读数在，读的是别的量）。
//     α=1 ⟹ `lerp(·, cur, 1) ≡ cur`；抖动 0 ⟹ `baseUv ≡ input.uv` ⟹ 采样是恒等映射
//     ⟹ Z 与参考只差一次 Reinhard 往返（<1e-3）。**线：frac(Z) ≤ 0.02**。
//     为什么这条最硬：配对错开一帧 ⟹ spin 已把内容搬走 ⟹ frac 立刻跳到 0.3+ 量级。
//     0.02 与 0.3 之间没有中间地带 ⟹ 它同时守「帧号配对正确」与「残差算子零点正确」。
//  2. `temporal-tail-events` **在场性 + 反向**：spin 组让开事件数 ≥ 2000（太少 ⟹ frac 是噪声）；
//     反向 = 把**静止**参考（spin=0）自己配自己喂进同一个算子，事件数**必须 ≤ 20**
//     ——检测器在静止内容上点火 = 它检测的是噪声不是让开，整张表作废。
//  3. `temporal-tail-box-load` **阳性锚点**：同窗档下**关盒（臂 1）frac 必须 > 臂 0**。
//     不成立 ⟹ 邻域盒在鬼影上**没承重** ⟹ 整个臂族「为压鬼影而牺牲闪烁」的前提被推翻，
//     那时正确处置是**关盒**而不是继续修盒。这是本判据最想要的那种红。
//  4. `temporal-tail-monotone` **量具有力气**：关盒态下把 α 从短窗 0.12 推到长窗 0.01
//     （唯一变量 = 窗长）frac 必须**上升**（闭式：1−α 从 0.88 → 0.99）。
//     不升 ⟹ frac 量的不是历史长度，四条判据全绿也不许报「臂间差异」。
//
// ⚠ **τ 是唯一自由阈值**（默认线性 luma 0.05 ≈ 7.5 个显示 LSB）⟹ 必须出 0.02/0.05/0.10
//    三档敏感性表；三档方向不一致 = 不许出结论。
// ⚠ 跨进程渲染确定性不是逐位（bistro 上 `G0 确定性地板` 已知常红，1 LSB / 个位数像素，债-61）。
//    它给 frac 加的是**加性**噪声，而分子分母都是跨十万像素的均值 ⟹ 影响在 1e-3 量级，
//    被判决线 1 的 0.02 吸收。
// ⚠ **单实例串行**：每组一个引擎进程，前一组没退干净不起下一组（并行双实例污染测量，已烧过）。

namespace tmotion {

    // ── 组定义（每组一个进程，串行）─────────────────────────────────────────
struct TailArm {
    const char* tag;
    double clip;
    double feedback; ///< 生效 α 就是它（批 13.14b 删掉第二个旋钮之后）
    bool taa;
    double jitter;
    const char* role;
};

/// 组定义（每组一个进程，串行）。原为 `cmdTemporalTail` 的局部表，拆函数后提到此处唯一出处。
constexpr TailArm kTailArms[] = {
        {"Z", 0.0, 1.0, true, 0.0, "零锚点（α=1 + 无抖动 ⟹ 应 ≡ 参考）"},
        {"B12", 1.0, 0.12, true, 1.0, "关盒 + 短窗（单调性低端，α=0.12）"},
        {"A1", 1.0, 0.01, true, 1.0, "关盒 + 长窗（阳性锚点的坏端）"},
        {"A0", 0.0, 0.01, true, 1.0, "现行盒 + 长窗"},
        {"A8", 8.0, 0.01, true, 1.0, "臂 8 + 长窗"},
        {"A10", 10.0, 0.01, true, 1.0, "臂 10 + 长窗"},
        {"A11", 11.0, 0.01, true, 1.0, "臂 11（记忆挂重投影位移）+ 长窗"},
    };

/// 一个臂 = 一个引擎进程（原 `cmdTemporalTail` 里的 `collect` lambda，逐字搬成具名函数）。
Group collectTailGroup(const std::string& base, const BistroCam& cam, int burst,
    const std::string& sub, bool taa, double fb, double jitter, double clip, double spinAngle,
    int w)
{
    Spec sp;
        sp.aa = taa ? Aa::Taa : Aa::Off;
        sp.feedback = fb;
        sp.jitterScale = jitter;
        sp.baseSpace = true;
        sp.clipMode = clip;
        sp.cameraMotion = false;  // 场景 spin，相机不动
        sp.motionAngle = spinAngle;
        sp.warm = w;
    return collectGroup(base + "_" + sub, sub, "t" + sub, cam, sp, burst);
}

/// 一次 `temporal-tail` 采集的全部读数（原 `cmdTemporalTail` 的局部量，拆函数后收进一处）。
struct TailRun {
    double tau = 0.0;                ///< 主档 τ（判决与主表标题都要）
    std::vector<double> taus;          ///< 三档 τ（唯一自由阈值 ⟹ 必须出敏感性表）
    std::vector<std::vector<TailRow>> table;  ///< [τ 档][臂]
    TailRow zero, boxOff, curBox, monoLo, monoHi;  ///< 主档下判决要用的五行
    TailRow rev;                       ///< 反向用例（静止参考自配自）
};

/// 主档留判决用的行（原样搬出的五个 `if (tag == …)`，逐字不动）。
void captureTailMainRow(const std::string& tag, const TailRow& r, TailRun& run)
{
    if (tag == "Z") run.zero = r;
    if (tag == "A1") run.boxOff = r;
    if (tag == "A0") run.curBox = r;
    if (tag == "B12") run.monoLo = r;
    if (tag == "A1") run.monoHi = r;
}

/// 采集（串行，单实例纪律）：REF → 共用窗口 → 三档掩膜 → 七个臂 → 反向用例。
/// 原样搬出；⚠ 采集**顺序**与每组的回显顺序就是原实现，臂与 REF 的进程串行也是。
TailRun collectTailRun(const std::string& base, const BistroCam& cam, double angle, int burst,
    int warm, int maxLag, double tau, int margin)
{
    TailRun run;
    std::printf("── 采集（串行，单实例纪律）──\n");
    Group ref = collectTailGroup(base, cam, burst, "REF", false, 0.12, 0.0, 0.0, angle, warm);
    std::printf("  REF  帧号 [%u, %u)  尺寸 %dx%d\n", ref.framePre, ref.framePost, ref.frameSize.width,
        ref.frameSize.height);

    // **共用内部窗口**：与偏移无关的常数 j 区间 ⟹ 各臂评在逐位相同的事件集上（理由见 measureTail）
    const int jlo = maxLag + margin;
    const int jhi = static_cast<int>(ref.luma.size()) - 1 - maxLag - margin;
    if (jhi <= jlo)
        throw std::runtime_error("内部窗口为空：burst 太小或 margin 太宽");
    std::printf("  共用事件窗口 j ∈ [%d, %d]（%d 帧，各臂逐位相同）\n", jlo, jhi, jhi - jlo + 1);

    run.tau = tau;
    // τ 三档敏感性（唯一自由阈值 ⟹ 必须出表；掩膜只依赖参考，三档共用一次采集）
    run.taus = {0.02, tau, 0.10};
    const auto& taus = run.taus;   // 别名 = 让下面这段保持原样
    auto& table = run.table;
    std::vector<std::vector<cv::Mat>> masks;
    for (double t : taus)
        masks.push_back(revealMasks(ref, t, maxLag));

    for (const TailArm& a : kTailArms) {
        Group g = collectTailGroup(base, cam, burst, a.tag, a.taa, a.feedback, a.jitter, a.clip, angle, warm);
        std::printf("  %-4s 帧号 [%u, %u)  clip=%g fb=%g jitter=%g\n", a.tag, g.framePre,
            g.framePost, a.clip, a.feedback, a.jitter);
        for (size_t ti = 0; ti < std::size(taus); ++ti) {
            TailRow r = measureTail(a.tag, a.clip, ref, g, masks[ti], maxLag, jlo, jhi);
            if (taus[ti] == tau)  // 主档留判决用的行
                captureTailMainRow(a.tag, r, run);
            table[ti].push_back(r);
        }
    }  // g 在此析构 ⟹ 同时最多驻留「参考 + 一个臂」（720p double × burst 很吃内存）

    // ── 反向用例：静止参考自配自 ⟹ 检测器不许点火 ──────────────────────────
    Group refs = collectTailGroup(base, cam, burst, "REFs", false, 0.12, 0.0, 0.0, 0.0, warm);
    auto revMasks = revealMasks(refs, tau, maxLag);
    run.rev = measureTail("REV", 0.0, refs, refs, revMasks, maxLag, jlo, jhi);
    return run;
}

/// 主表 + 衰减曲线 + τ 敏感性（原样搬出，打印顺序一字未动）。
void printTailTables(const TailRun& run)
{
    const double tau = run.tau;
    const auto& table = run.table;
    const auto& taus = run.taus;
    const auto& arms = kTailArms;

    // ── 主表 ────────────────────────────────────────────────────────────────
    std::printf("\n── 主表（τ = %g 线性 luma ≈ %.1f 显示 LSB）──\n", tau, tau / 0.006732);
    std::printf("  %-5s %-8s %10s %10s %8s %6s\n", "组", "clip", "事件数", "mean|N−S|", "frac",
        "帧偏移");
    for (const auto& r : table[1]) {
        std::printf("  %-5s %-8g %10.0f %10.5f %8.4f %6ld\n", r.tag.c_str(), r.clip, r.events,
            r.meanChange, r.frac, r.offset);
    }
    std::printf("\n── 衰减曲线 A[k]（归一化残差，让开后第 k 帧；τ = %g）──\n", tau);
    for (const auto& r : table[1]) {
        std::printf("  %-5s", r.tag.c_str());
        for (size_t k = 0; k < r.decay.size() && k < 25; ++k)
            std::printf(" %.3f", r.decay[k]);
        std::printf("\n");
    }
    std::printf("\n── τ 敏感性（三档方向必须一致）──\n");
    std::printf("  %-5s", "组");
    for (double t : taus)
        std::printf(" %10s", ("τ=" + std::to_string(t)).c_str());
    std::printf("\n");
    for (size_t ai = 0; ai < std::size(arms); ++ai) {
        std::printf("  %-5s", arms[ai].tag);
        for (size_t ti = 0; ti < std::size(taus); ++ti)
            std::printf(" %10.4f", table[ti][ai].frac);
        std::printf("\n");
    }
}

/// 判决（四条 AND）+ jsonl 落账 + rc（原样搬出）。
int emitTailVerdicts(const std::string& cameraName, double angle, int burst, double tau,
    int maxLag, const TailRun& run)
{
    const TailRow &zero = run.zero, &boxOff = run.boxOff, &curBox = run.curBox;
    const TailRow &monoLo = run.monoLo, &monoHi = run.monoHi, &rev = run.rev;

    // ── 判决（四条 AND）─────────────────────────────────────────────────────
    std::vector<Verdict> vs;
    vs.push_back(verdictFor("temporal-tail-pairing", zero.frac <= 0.02 && zero.events > 0.0,
        "frac(Z) = " + d4(zero.frac) + "（线 ≤ 0.02；配对错一帧会跳到 0.3+）"));
    std::cout << "  " << vs.back().line() << "\n";
    vs.push_back(verdictFor("temporal-tail-events",
        curBox.events >= 2000.0 && rev.events <= 20.0,
        "spin 事件 " + std::to_string(static_cast<long>(curBox.events)) + "（线 ≥ 2000）· 静止反向 "
            + std::to_string(static_cast<long>(rev.events)) + "（线 ≤ 20）"));
    std::cout << "  " << vs.back().line() << "\n";
    vs.push_back(verdictFor("temporal-tail-box-load", boxOff.frac > curBox.frac,
        "关盒 frac " + d4(boxOff.frac) + " vs 臂 0 " + d4(curBox.frac)
            + "（不成立 = 盒在鬼影上没承重 ⟹ 该关盒，不是继续修盒）"));
    std::cout << "  " << vs.back().line() << "\n";
    vs.push_back(verdictFor("temporal-tail-monotone", monoHi.frac > monoLo.frac,
        "关盒 α=0.12 frac " + d4(monoLo.frac) + " → 长窗 α=0.01 frac " + d4(monoHi.frac)
            + "（闭式 0.88 → 0.99；不升 = frac 量的不是历史长度）"));
    std::cout << "  " << vs.back().line() << "\n";

    logCall("temporal-tail",
        {{"camera", cameraName}, {"angle", d4(angle)}, {"burst", std::to_string(burst)},
            {"tau", d4(tau)}, {"max-lag", std::to_string(maxLag)}},
        vs);
    const bool allOk = std::all_of(vs.begin(), vs.end(),
                                   [](const Verdict& v) { return v.ok; });
    std::printf("\n  ⟹ %s\n",
        allOk ? "四条全绿：frac 可作为鬼影判据，读上面主表比较各臂"
              : "有判决红 ⟹ **不许报臂间结论**（先修仪器，别改判决线）");
    return allOk ? 0 : 1;
}

}  // namespace tmotion

int cmdTemporalTail(const std::string& cameraName, double angleDeg, int burst, int warm,
    int maxLag, double tau, int margin)
{
    const BistroCam* cam = nullptr;
    for (const auto& c : kBistroCams)
        if (cameraName == c.name)
            cam = &c;
    if (!cam) {
        std::fprintf(stderr, "机位不存在: %s（可选 %s）\n", cameraName.c_str(),
            bistroCamNames().c_str());
        return 2;
    }
    if (burst < 2 * (maxLag + margin) + 2)
        throw std::runtime_error(
            "--burst 必须 ≥ 2×(--max-lag + --margin) + 2（内部窗口对所有臂共用，两端都要留帧）");
    if (maxLag < 4)
        throw std::runtime_error("--max-lag 至少 4（衰减曲线少于 4 点看不出指数）");

    const double angle = angleDeg * 3.14159265358979323846 / 180.0;
    const std::string base = projectRoot() + "/build/imgqa_tail_" + cameraName;

    std::printf("=== temporal-tail：鬼影尾长直测（机位 %s，债-63 的出口）===\n", cameraName.c_str());
    std::printf("  运动源 = 场景 spin %.3g°/帧（**相机静止** ⟹ 不需要图像配准）  burst %d  热身 %d  K %d\n"
        "  配对 = 引擎帧计数器之差（整数常量，零阈值）  参考 = TAA 关 + jitter_scale=0（基位置真值）\n"
        "  主指标 frac = mean|out−ref| ÷ mean|N−S|（无量纲；闭式锚点：关盒时 = 1 − α）\n",
        angleDeg, burst, warm, maxLag);

    const TailRun run = collectTailRun(base, *cam, angle, burst, warm, maxLag, tau, margin);
    printTailTables(run);
    return emitTailVerdicts(cameraName, angle, burst, tau, maxLag, run);
}


}  // namespace RGL::imgqa
