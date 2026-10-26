// shadow-swim-flat 判据层 —— 闸门 G1–G5 + 五条判决线 J-F1…J-F5。
//
// 另立 TU 的理由 = 先例（shadow-edge-ruler / -rebase / shadow-grain 的四切法）：缝按**仪器
// 内部职责**切，「采集与出表」与「判什么、界怎么推」不是同一条改动缝——改阈值/改判据形态只动
// 本文件，改采集只动 `shadow_swim_flat.cpp`。护栏（单文件 < 500 代码行）只是把这条既有的缝
// 逼了出来，不是切出这条缝的理由。
//
// 🔴 闸门与判决线的**顺序纪律**（照 shadow-edge-rebase 的交付纪律）：主表与判决线先打印，
// 闸门后抛。理由不是礼貌：本批的交付物就是那些数，而闸门红往往恰恰发生在「某一格读不出数」
// 的时候——先抛就拿不到另外三格，那件事本身不可归因。
//
// 五条判决线的分工（缺一条都会瞎）：
//   J-F1 无地板残差（Q1 的头条数：债-139 的 2.73 % 在零起伏靶上重测，剔掉重采样地板后剩多少）
//   J-F2 阴性对照（同机位两次**恰 0**：它守的是其余四条的分母——引擎不确定时任何百分比都是噪声）
//   J-F3 不坏（门开不得比门关差；与 J-F1 同一个阈值、同一个算子）
//   J-F4 变好 🔴（Q2 的头条数：远点门开的无地板残差必须回到出厂水平）
//   J-F5 区分力守卫（注入 d 必须报出 +d；🔴 它红时 J-F1 的「0」不作数——读到 0 有两种解释，
//     本条把「量具看不见滑移」这一种当场否掉）
//
// ── 🔴 J-F4 的形态为什么**不是**「③ 必须显著差于 ④」────────────────────────────────
// 那条写法在断言「缺陷存在」。今天 ③ 确实差 5.7 倍（实测 mean |Δcov| 4.213 ‰ vs 0.739 ‰），
// 但一旦有人把大坐标下「阴影整块消失」那条新缺陷修掉，③ 变好，J-F4 就会**因为修好了 bug 而
// 红**——那是把技术债写成了判据。所以 J-F4 的通过条件只留可长期成立的那半句：
//     「远点 + 门开 ⟹ 影场世界不变性达到出厂点同一口径的界」。
// 而 ③/④ 的**对比**（rebase 到底买到了多少）走两条不裁决的路：主表逐格打印 + 台账字段。
// 本批的 Q2 交付物是那个对比**读数**，不是一条永久断言。
#include "shadow_swim_flat.h"
#include "shadow_edge_rebase.h"  // requireFarIsTranslatedCopy（靶同源判据复用，不抄第二份）

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

/// 两个级联闭式模型是否**逐数相同**（同桶判据的本体）。
/// 比的是量化后的 nearQ/farQ、四档 splits 与四档纹素——不是比 raw：nearRaw/farRaw 随相机
/// 连续变，比它等于要求「相机挪了而距离没变」，那是假的。要钉的是**量化输出**没变。
bool sameCascadeBucket(const EdgeCascadeModel& a, const EdgeCascadeModel& b)
{
    if (a.nearQ != b.nearQ || a.farQ != b.farQ)
        return false;
    for (int i = 0; i < 4; ++i)
        if (a.splits[i] != b.splits[i] || a.texelM[i] != b.texelM[i])
            return false;
    return true;
}

std::string pct4(double v)
{
    char b[40];
    std::snprintf(b, sizeof b, "%.4f%%", v);
    return b;
}

std::string permille(double v)
{
    char b[40];
    std::snprintf(b, sizeof b, "%.3f‰", v);
    return b;
}

std::string metres(double v)
{
    char b[40];
    std::snprintf(b, sizeof b, "%+.3f", v);
    return b;
}

}  // namespace

void requireFlatCellsValid(const std::vector<FlatCell>& cells)
{
    if (cells.size() != 4)
        throw std::runtime_error("G1: 四格表只有 " + std::to_string(cells.size())
            + " 格 ⟹ makeFlatCells 被改过，主表列序与判决线取数全部错位");
    // 靶同源（远点 = 出厂靶 + worldOffset，逐板真值/机位/bounds 跟着平移）。🔴 复用 rebase 的
    // 那份实现：生成器若漏挪真值，出厂点一切照旧绿、远点凭空多出 1e6 mm，而其它闸门一条不红。
    requireFarIsTranslatedCopy(cells[0].target, cells[2].target, cells[0].modelA,
        cells[2].modelA);
    // 同桶（本仪器独有，rebase 没有：它的两枚机位是同一枚）。
    // 🔴 债-159：这里**不核对米数**（原来那份 `= kEdgeFarOffsetM` 是本仓的第二份抄本，已删）。
    // 出厂格要求 worldOffset 恰为零（它要是非零就不是出厂工作点），远点格要求非零 + 网格外
    // ——后一条由上面复用的 `requireFarIsTranslatedCopy` 求值，本仪器不另写一份判据。
    for (const auto& c : cells) {
        const auto& off = c.target.worldOffset;
        const bool far = c.stem == kFlatFarStem;
        const double want = far ? 1.0 : 0.0;  // 只问「零 / 非零」，不问「等于几」
        const bool ok = far ? (std::abs(off[0]) > kEdgeShiftTolM && std::abs(off[1]) <= kEdgeShiftTolM
                                  && std::abs(off[2]) <= kEdgeShiftTolM)
                            : (std::abs(off[0]) <= kEdgeShiftTolM
                                  && std::abs(off[1]) <= kEdgeShiftTolM
                                  && std::abs(off[2]) <= kEdgeShiftTolM);
        if (!ok)
            throw std::runtime_error("G1: " + c.title + " 的 manifest worldOffset 不该是"
                + std::string(want > 0.0 ? "零（远点格必须真的挪出去）"
                                         : "非零（出厂格是基准，挪了就不是基准）")
                + "（读到的 x = " + std::to_string(off[0]) + "）⟹ 工作点不是量具以为的那个");
        if (!sameCascadeBucket(c.modelA, c.modelB))
            throw std::runtime_error("G1: " + c.title
                + " 的两枚机位（沿世界 +X 差 0.5 m）**不在同一个量化桶**里（nearQ "
                + std::to_string(c.modelA.nearQ) + " vs " + std::to_string(c.modelB.nearQ)
                + "，farQ " + std::to_string(c.modelA.farQ) + " vs "
                + std::to_string(c.modelB.farQ)
                + "）⟹ 那时「阴影随相机变」里有一半是**级联本身**变了，与坐标原点无关，"
                  "不是本判据的射程。要么把激励量改小回到同桶、要么换靶，不许放宽这条闸门");
    }
}

void requireStimulusLive(const std::vector<FlatCell>& cells)
{
    for (const auto& c : cells) {
        const long long d = cv::countNonZero(anyChannelDiff(c.noneA, c.noneB));
        if (d == 0)
            throw std::runtime_error("G2: " + c.title
                + " 的两枚机位 none 臂**逐位相同** ⟹ 相机根本没动（camera.lua 覆写打偏 /"
                  " 激励量被写成 0）。两枚机位是同一枚 ⟹ 对拍恒真，全部读数作废");
    }
}

void requireFieldAlive(const std::vector<FlatCell>& cells)
{
    for (const auto& c : cells) {
        if (c.shadowPx == 0)
            throw std::runtime_error("G3: " + c.title
                + " 的 none↔csm 逐位相同 ⟹ 阴影对交付图**一个像素都没改过**（分母死）。"
                  "cov ≡ 0 时两臂之差也 ≡ 0 ⟹ 本仪器所有差分判据一起变绿，那条绿是假的");
        // 有影面积用**恰 0** 判死（同 shadow-edge-ruler 的 kEdgeCoverageFloor 口径：本靶确定性
        // 渲染、none/csm 只差阴影段 ⟹ 死态是恰 0，不是「小于某个小数」）。这里再要一个非零下限
        // 是因为本判据的分母是全幅可信集：影面积掉到千分之一以下时「翻转率 0」几乎恒真。
        if (!(c.umbraA > kEdgeCoverageFloor) || !(c.umbraB > kEdgeCoverageFloor))
            throw std::runtime_error("G3: " + c.title + " 的有影面积 A/B = "
                + edgePct(c.umbraA) + "/" + edgePct(c.umbraB) + "，低于死态下界 "
                + edgePct(kEdgeCoverageFloor)
                + " ⟹ 影几乎没画进画面，「翻转率 0」是恒真而不是判据通过");
    }
}

void requireTrustSufficient(const std::vector<FlatCell>& cells)
{
    // 两条下限都要：绝对数（防止「占比正常但画面太小」）与占比（防止「绝对数够但只是全图一角」）。
    // 实测正常读数：可信 ~29 万点 / 全幅 144 万 = 20 %（与债-139 在 bistro 上的 21.8 % 同量级）。
    constexpr std::size_t kMinTrust = 20000;
    constexpr double kMinTrustFraction = 0.05;
    for (const auto& c : cells) {
        const double total = static_cast<double>(c.covA.cols) * static_cast<double>(c.covA.rows);
        const double frac = static_cast<double>(c.floor.trust) / (total > 0.0 ? total : 1.0);
        if (c.floor.trust < kMinTrust || frac < kMinTrustFraction)
            throw std::runtime_error("G4: " + c.title + " 的无地板可信集只有 "
                + std::to_string(c.floor.trust) + " 点（占全幅 " + std::to_string(frac)
                + "），低于 " + std::to_string(kMinTrust) + " 点 / 5 % ⟹ 分母是彩票，"
                  "任何百分比都没有意义（剔缘剔掉了 " + std::to_string(c.floor.edgeExcluded)
                + " 点；若这是侵蚀半径调大的后果，先回去看 J-F1 的容差理由）");
    }
}

void requireRebaseMovedImage(const std::vector<FlatCell>& cells)
{
    const auto diff = [](const FlatCell& a, const FlatCell& b) {
        return static_cast<long long>(cv::countNonZero(anyChannelDiff(a.csmA, b.csmA)));
    };
    const long long d12 = diff(cells[0], cells[1]);
    const long long d34 = diff(cells[2], cells[3]);
    std::printf("\n  G5 门开 vs 门关 逐位不同像素：出厂点 ②vs① = %lld，远点 ④vs③ = %lld\n", d12,
        d34);
    if (d12 == 0 || d34 == 0)
        throw std::runtime_error("G5: 门开与门关的自然臂**逐位相同**（出厂点 "
            + std::to_string(d12) + "，远点 " + std::to_string(d34)
            + "）⟹ 注入非零原点没有改变任何东西（批 4 判据 ② 的形态：接了但没接上）。"
              "四格只剩两格，「rebase 买到了什么」这个问题在这台仪器上问不出来");
}

Verdict judgeFlatFloor(const FlatCell& c)
{
    // 🔴 通过条件里**不含**「最优平移恰为零位移」。首版含，实机跑出来才发现那条不可判：最优平移
    // 是拿 Σ|Δcov| 搜出来的连续量，而残差本身是浮点闭合级 + 亚像素量级的东西，任何非零平移都能
    // 把 Σ|Δcov| 再压低一点点（压低的是重采样噪声，不是滑移）。把它写进判据 = 造一条永远红的线。
    // 「有没有滑移」这件事由 J-F5 负责（它给一个**已知**的滑移让搜索去找），本条只答「残差多大」。
    // 最优平移仍然打印，且是 observed 的第一手读数——只是不裁决。
    const bool ok = c.floor.flipPct() <= kFlatFlipLimitPct
        && c.floor.meanAbsCovPermille() <= kFlatMeanCovLimitPermille;
    return { "shadow-swim-flat-floor", ok,
        "① 出厂·门关：erode0（债-139 原口径）= " + pct4(c.raw.flipPct()) + " → erode"
            + std::to_string(kFlatErodePx) + "（**无地板**）= " + pct4(c.floor.flipPct())
            + "；mean |Δcov| = " + permille(c.floor.meanAbsCovPermille()) + "，p99 = "
            + permille(1000.0 * c.floor.p99AbsCov) + "；最优世界滑移 = (" + metres(c.best.sx)
            + ", " + metres(c.best.sz) + ") m；可信 " + std::to_string(c.floor.trust)
            + " 点 / 剔缘 " + std::to_string(c.floor.edgeExcluded) + " 点",
        "两个量都要过：无地板翻转率 ≤ 0.01 % 且 mean |Δcov| ≤ 2.0 ‰。\n"
        "· 🔴 通过条件里**不含**「最优平移恰为零位移」（首版含，实机跑出来才发现那条不可判）：最优"
        "平移是拿 Σ|Δcov| 搜出来的连续量，而 erode2 的残差本身是亚像素量级的东西，任何非零平移都"
        "能把 Σ|Δcov| 再压低一点（压低的是重采样噪声，不是滑移）——写进判据就是造一条永远红的线。"
        "实测四格里 ①②④ 的最优平移都是恰 (0,0)、③ 报 (0.04, −0.02)，作为**读数**有信息量，作为"
        "**阈值**没有。「到底有没有滑移」这件事由 J-F5 负责：它给一个**已知**的滑移让搜索去找。\n"
        "· 0.01 % 不是拍的：erode 2 之后**实测恰 0.0000 %**（erode 0 = 0.2593 %），而阴性对照"
        "（同机位两次）在 erode 0 也是 0 ⟹ 那 0.2593 % 全部住在影缘 ≤ 2 像素的带里 = 重采样地板，"
        "不是阴影真的变了。留 0.01 %（≈ 28 点）而不是「恰 0」的唯一理由：侵蚀半径是**整数像素**，"
        "而 1 像素的半影加亚像素搬运误差可以漏到第 3 像素——真漏出去就是影缘挪了 > 1.1 cm，"
        "那是要出声的世界滑移，不该被半径盖掉。\n"
        "· 2.0 ‰ 的来历（先测后定，2026-10-26 本机）：定界那代 ①②④ 实测 0.866 / 0.915 / 0.710 ‰，"
        "取最差者的 2.2 倍（债-159 换远工作点后当代实测 ① 0.806 / ② 0.855 / ④ 0.861 ‹界内›，"
        "界不随工作点改——它的定义就是「门开在远点达到门开在出厂点的水平」）。它是「没跨 0.5 阈值的连续差」——翻转率看不见的部分，也是本批唯一"
        "能区分 ③ 与 ④ 的统计量（二值翻转率反而给 ③ 报**更低**的数：0.029 % vs 0.267 %，"
        "因为门关在远点的影场更粗、刀锋更少）。\n"
        "· 两个统计量都要报的实测理由就在这条判据自己身上：erode0 与 erode2 差 60 倍（侵蚀带的"
        "规模），而四格之间的排序只有连续量给得出。",
        { "债-139（docs/还债总账.md）：2.73 % 残差是在 bistro 鹅卵石地面上量的，结案时明确写着"
          "「不是直接『阴影随相机位置变』」——本条把地面换成零起伏平面，按构造删掉地面几何误差，"
          "再用影缘侵蚀带删掉重采样地板，剩下的才是无地板残差",
          "shadow_transport_metric.h 头注「本层唯一的新概念：影缘侵蚀带」+ 实测 erode 扫描 "
          "0 → 0.2558 %、2 → 恰 0.0000 %、4 → 0.0000 %",
          "selftest_swim_flat.cpp 的「刀锋场零侵蚀必翻 / 侵蚀 2 像素后恰 0」两条：侵蚀带算子坏成"
          "恒零（闭运算−开运算）时本条会绿着骗过去，那两条就是它的防线",
          "本仪器 J-F5（区分力守卫）：它红时本条的 0 不作数" } };
}

Verdict judgeFlatDeterminism(const FlatControl& ctl)
{
    const bool ok = ctl.bitDiff == 0 && ctl.determinism.flips == 0
        && ctl.determinism.trust > 0;
    return { "shadow-swim-flat-determinism", ok,
        "同机位重跑：逐位不同像素 = " + std::to_string(ctl.bitDiff)
            + "（none + csm 两臂合计），erode0 翻转 = " + std::to_string(ctl.determinism.flips)
            + "/" + std::to_string(ctl.determinism.trust),
        "两个量都必须**恰为 0**，不是「小于某个小数」。交付态 `aa = \"off\"` 是纯空间档，本靶的"
        "全部自变量 = 几何、材质、光位、级联（相机位的函数）、坐标原点，**没有帧号输入** ⟹ 同机位"
        "两次逐位相同是按构造的恒等式。\n"
        "· 为什么还要额外要 trust > 0：空分母上「0 翻转」是恒真（本仓 skill 明写的那条）。\n"
        "· 它守的是其余四条的**分母**：非零 = 引擎本身不确定，此时「无地板残差 0.0000 %」是噪声"
        "比噪声，J-F1/J-F3/J-F4 的数一个都不作数。\n"
        "· 这条同时防工具自己的口径漂移：哪天有人给阴影加了帧号自变量（时域去噪、TAA 消费影），"
        "本条当场红——而那条改动恰恰会让所有单帧对比判据静默失效。",
        { "skill rgl-verify「单帧截图看不见时域不稳定」+ 同相位免费标定先例（temporal-swim-anchor："
          "fb=1 无记忆 ⟹ 同相位差须恰为 0，一个 bit 都不许差）",
          "material-swim-determinism 同形态先例（债-152）：同机位两跑逐位不同像素 = 0",
          "本批探针实测：四格各自的阴性对照全部 0 翻转 + 0 逐位差异" } };
}

Verdict judgeFlatNoHarm(const FlatCell& off, const FlatCell& on)
{
    const bool ok = on.floor.flipPct() <= kFlatFlipLimitPct
        && on.floor.meanAbsCovPermille() <= kFlatMeanCovLimitPermille;
    return { "shadow-swim-flat-no-harm", ok,
        "② 出厂·门开：erode" + std::to_string(kFlatErodePx) + " = " + pct4(on.floor.flipPct())
            + "（① 门关 " + pct4(off.floor.flipPct()) + "），mean |Δcov| = "
            + permille(on.floor.meanAbsCovPermille()) + "（① "
            + permille(off.floor.meanAbsCovPermille()) + "），最优滑移 = (" + metres(on.best.sx)
            + ", " + metres(on.best.sz) + ") m",
        "与 J-F1 **同一个**阈值、同一个算子，且额外要求「不比 ① 差」。两态的差别只有「渲染原点"
        "跟不跟相机走」，几何、采样、分辨率、机位逐字相同 ⟹ 任何「门开态可以更松」的写法都是在"
        "给 J-F1 松绑。\n"
        "· 实测 ② 0.915 ‰ vs ① 0.866 ‰（差 5.7 %，同量级），erode2 翻转两者都是恰 0。\n"
        "· 为什么出厂点也要测门开：rebase 在 |O| 小时按构造近似恒等（原点 = 相机位 snap 到纹素"
        "粒度，出厂点附近 snap 步长远小于纹素），所以这条**应该**绿；它红说明 snap 本身在出厂点"
        "就引入了可见滑移，那是 rebase 自己的缺陷，与远点无关。",
        { "docs/相对坐标地基_plan.md §3 批 5 第 1 刀：门关 = 原点恒零、toRelative 恒等（出厂三枚"
          "截图锚逐位不变就是这条的图像级对应）",
          "shadow-edge-rebase 的 J-R1「不坏」同形态先例",
          "skill rgl-test-discipline「阳性锚点」：判据必须能在已知好坏的两种输入上分别给出绿与红" } };
}

Verdict judgeFlatBenefit(const FlatCell& base, const FlatCell& farOff, const FlatCell& farOn)
{
    const double ratio = farOn.floor.meanAbsCovPermille() > 0.0
        ? farOff.floor.meanAbsCovPermille() / farOn.floor.meanAbsCovPermille()
        : kNaN;
    char ratioBuf[32];
    std::snprintf(ratioBuf, sizeof ratioBuf, "%.1f", ratio);
    const bool ok = farOn.floor.flipPct() <= kFlatFlipLimitPct
        && farOn.floor.meanAbsCovPermille() <= kFlatMeanCovLimitPermille;
    return { "shadow-swim-flat-benefit", ok,
        "④ 远点·门开：erode" + std::to_string(kFlatErodePx) + " 翻转 = "
            + pct4(farOn.floor.flipPct()) + "，mean |Δcov| = "
            + permille(farOn.floor.meanAbsCovPermille()) + "，p99 = "
            + permille(1000.0 * farOn.floor.p99AbsCov) + "，最优滑移 = (" + metres(farOn.best.sx)
            + ", " + metres(farOn.best.sz) + ")\n      对比 ③ 远点·门关 = mean "
            + permille(farOff.floor.meanAbsCovPermille()) + "（p99 "
            + permille(1000.0 * farOff.floor.p99AbsCov) + "），③/④ = " + ratioBuf
            + "×；对比 ① 出厂·门关 = " + permille(base.floor.meanAbsCovPermille()),
        "通过条件只有可长期成立的那半句：**远点 + 门开**的无地板残差 ≤ 与 ①② 同一个界（0.01 % / "
        "2.0 ‰）。实测 ④ 0.861 ‰（远点 4e6 网格外，债-159 当代）/ 0.649 ‰（上一代 1e6 网格上），"
        "都与 ① 同量级。\n"
        "· 🔴 通过条件里**故意不含**「③ 必须显著差于 ④」。那条写法在断言「缺陷存在」：今天 ③ 差 "
        "5.7 倍（4.213 vs 0.739 ‰，p99 差 11.6 倍），一旦大坐标下「阴影整块消失」那条新缺陷被修掉，"
        "③ 变好，本条就会**因为修好了 bug 而红**——那是把技术债写成判据。③/④ 的对比（rebase 到底"
        "买到多少）走不裁决的两条路：observed 里打印 + 台账字段 c3/c4_mean_dcov_permille。\n"
        "· 界取 2.0 ‰ 的推导见 J-F1；这里不另推，因为「门开在远点该达到什么水平」的答案就是"
        "「门开在出厂点的水平」——同一个量具、同一个口径，没有第二个界。\n"
        "· 为什么这条必须存在而不能只看 J-F3：只验「不坏」会放行一个「线接完了但一分精度都没"
        "买到」的实现（出厂点本来就不坏）。J-F3 + J-F4 同时绿才构成「rebase 在需要它的地方"
        "起作用了」。",
        { "shadow-edge-rebase 的 J-R2「变好」同形态先例（🔴 那条的头注原话：只验「不坏」会放行"
          "一个「线接完了但一分精度都没买到」的实现）",
          "四格实测**按远工作点分代**（跨代不可比）：erode4 口径 ① 0.787 / ② 0.838 / ③ 4.213 / "
          "④ 0.739 ‰；远点 = 1e6（网格上）erode2 口径 ① 0.806 / ② 0.855 / ③ 64.039 / ④ 0.649 ‰"
          "（③/④ = 98.7）；远点 = 4e6 且网格外（债-159 当代）① 0.806 / ② 0.855 / ③ 181.743 / "
          "④ 0.861 ‰（③/④ = 211.2，p99 ‰ 15.486 / 15.486 / 462.165 / 16.206）",
          "shadow-edge-rebase.cpp 头注：远点门关态实测是「阴影整块消失」而非厘米级抖动，那是"
          "另一类缺陷（J-R3 单独裁决），别记进 rebase 的收益——本条同理" } };
}

Verdict judgeFlatOffsetSensitivity(const FlatControl& ctl)
{
    const double dx = std::abs(ctl.injectedBest.sx - kFlatStimulusM);
    const double dz = std::abs(ctl.injectedBest.sz);
    // 三条：① 搜索报出的平移 = 注入量（±一个粗网格步长）；② z 轴不许有假号（注入是纯 +X）；
    // 🔴 ③ 零位移处**确实看得见差**——用连续量而不是翻转率。本靶二值本影只占 0.14 %，注入 0.5 m
    // 的翻转率是 0.0000 %（影缘像素太少，翻进翻出几乎相消），只有 Σ|Δcov| 看得见（实测 5.487 ‰
    // vs 地板 0.866 ‰）。拿翻转率当「看得见」的判据会把这条守卫整个废掉。
    const bool ok = ctl.injectedBest.found && dx <= kFlatSearchCoarseM && dz <= kFlatSearchCoarseM
        && ctl.injected.meanAbsCovPermille() > kFlatMeanCovLimitPermille;
    return { "shadow-swim-flat-offset-sensitivity", ok,
        "注入 d = 0.5 m（挪靶 + 跟相机）：可信 " + std::to_string(ctl.trustKept) + " 点，erode0 翻转 = "
            + pct4(ctl.injected.flipPct()) + "，erode" + std::to_string(kFlatErodePx) + " 翻转 = "
            + pct4(ctl.injectedFloor.flipPct()) + "，最优世界滑移 = (" + metres(ctl.injectedBest.sx)
            + ", " + metres(ctl.injectedBest.sz) + ") m，该处失配 "
            + std::to_string(ctl.injectedBest.flips) + "/" + std::to_string(ctl.injectedBest.trust),
        "判据 = 最优平移落在 (d, 0) ± 一个粗网格步长（0.10 m）内，且零位移处**看得见**差"
        "（连续量 mean |Δcov| > 2.0 ‰）。\n"
        "· 注入形态：把**靶**沿 +X 挪 d、相机跟着挪 d ⟹ 取景逐字相同（同一枚相机看同一块地），"
        "而遮挡体在世界里真的挪了 d ⟹ 影场滑 d。为什么不拿「相机挪 d」当注入量：那是 J-F1 的"
        "自变量，守卫与被测项共用同一个失效（缺陷在场时两者一起红、修掉后一起绿 = 守卫没有独立"
        "信息）。挪靶给的是**独立**的一条：它改影场的锚，不改相机。\n"
        "· 容差 ± 0.10 m = 本量具平移搜索的分辨率（粗网格步长），再小就不可分辨；细网格只把"
        "中心附近再切 5 份，不改变「量具能分辨多少」这件事。\n"
        "· 🔴 「看得见差」这一支用**连续量**而不是翻转率：本靶二值本影只占 0.14 %（cov > 0.5 只有"
        "2000 像素——这块平地上太阳只占受光的三成），注入 0.5 m 的 erode0 翻转率实测 0.0000 %（"
        "影缘像素太少，翻进翻出几乎相消），只有 mean |Δcov| 看得见（5.487 ‰ vs 地板 0.866 ‰）。"
        "拿翻转率当这一支的判据会把整条守卫废掉。\n"
        "· 实机读数：最优世界滑移 = (+0.500, +0.000) m，**恰等于注入量、符号与轴向都对**（z 轴"
        "零假号）。这条同时是「搜索没被钉成恒 0」与「搬运的符号约定没写反」两件事的证据。\n"
        "· 🔴 这条是任务书钦定的变异「把平移搜索钉成恒 0」的判据本体：搜索恒返回 (0,0) 时本条"
        "红，而 J-F1/J-F3/J-F4 全部**照绿**——这正是它必须存在的理由：读到 0 有两种解释（场真的"
        "不变 / 量具看不见场），本条把第二种当场否掉。",
        { "skill rgl-test-discipline「区分力守卫」+ 本仓库先例 `temporal-swim-discriminative`"
          "（搬对相位与搬错相位必须显著不同，否则算子没力气，读数不作数）",
          "material-swim-offset-sensitivity（债-152）同形态先例：修前**也是红的**（报 0 而非 d），"
          "且这是设计——它证明 J1 修前的红不是量具坏",
          "selftest_swim_flat.cpp 的「注入 (0.05,0.05) m 世界滑移 → 原样同号报回且该处失配恰 0」"
          "：本条的闭式版（不启引擎）" } };
}

}  // namespace RGL::imgqa
