// shadow-swim-wall 的闸门与判决线（缝 = 「采集/编排 vs 判定」，同 shadow_swim_flat_judge.cpp）。
//
// 🔴 交付纪律：表先出、闸门后抛（本批的交付物就是那些数，闸门红时更要拿到它们）。
// 🔴 判据设计的一条继承自 `shadow_swim_flat.h`：**通过条件里不许写「当前有缺陷」**，也不许写
//    「当前没有缺陷」。本文件的 J-W3 判的是**干预的效应模式**（钉到服务级必须逐位不变、钉到
//    别级必须大改）——两条都是构造性质，与「带随相机动」这个现象存不存在无关；现象将来被
//    修好或变坏，J-W3 的判据一个字都不用改，改的是它读到的数。
#include "shadow_swim_wall.h"
#include "shadow_wall_metric.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace RGL::imgqa {
namespace {

std::string f3(double v)
{
    char b[40];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

std::string pct(double v)
{
    char b[40];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}

Verdict mk(std::string name, std::string observed, std::string why, std::vector<std::string> g)
{
    Verdict v;
    v.name = std::move(name);
    v.observed = std::move(observed);
    v.whyTol = std::move(why);
    v.guards = std::move(g);
    return v;
}

/// 一格里「主导级」的集合（票数 ≥ 分歧像素 2 %），G1 用它比较两枚机位。
std::vector<int> dominantCascades(const CascadeIndexReadout& r)
{
    std::vector<int> out;
    if (r.divergent == 0)
        return out;
    for (size_t k = 0; k < kWallCascadeCount; ++k)
        if (r.hist[k] >= 0.02 * static_cast<double>(r.divergent))
            out.push_back(static_cast<int>(k));
    return out;
}

}  // namespace

// ── 闸门 ────────────────────────────────────────────────────────────────────────────
namespace {
/// 按「沿 n 的距离」找回因果序列里同机位的级联索引读数。四格的两枚机位与 d1…d4 是**同一批
/// 机位**（距离逐字相同，都来自靶 manifest），而四格本身不采四把钉级尺（那要多 8×4 次进程），
/// 所以级联集合从因果序列借。距离对不上 ⟹ 抛：宁可说「无从比较」，不许悄悄放行。
const CascadeIndexReadout& indexOfSameRig(const std::vector<WallCamera>& cams, double distanceN,
    const std::string& cellTitle, const std::string& who)
{
    for (const auto& c : cams)
        if (c.idx.found && c.distanceN == distanceN)
            return c.idx;
    throw std::runtime_error(cellTitle + " 的" + who + "机位（沿 n " + std::to_string(distanceN)
        + " m）在因果序列里没有同距离档 ⟹ 级联集合无从实测，G1 变成空判，抛");
}
}  // namespace

void requireWallCellsValid(const std::vector<WallCell>& cells,
    const std::vector<WallCamera>& cams)
{
    // G1 的**实测**版本：两枚机位的门面由同一组级联服务。第一版这里吃的是闭式模型的
    // splits 逐数比较，而本批实机证明该模型在部分机位段与引擎不符（见 shadow_wall_metric.h）
    // ⟹ 换成不吃模型的判据：级联集合不同 = 阴影图的纹素尺寸换了 = 「带随相机动」里有一半
    // 是级联本身变了，与 rebase / 纹素吸附都无关，不是本判据的射程，宁可抛。
    if (cells.empty())
        throw std::runtime_error("shadow-swim-wall: 四格为空（恒真断言，拒绝）");
    for (const auto& c : cells) {
        const auto& ia = indexOfSameRig(cams, c.a.distanceN, c.title, "A ");
        const auto& ib = indexOfSameRig(cams, c.b.distanceN, c.title, "B ");
        if (ia.divergent == 0 || ib.divergent == 0)
            throw std::runtime_error(c.title + " 的门面分歧像素为 0（A 档 "
                + std::to_string(ia.divergent) + "、B 档 "
                + std::to_string(ib.divergent)
                + "）——级联集合无从比较，G1 变成空判，抛");
        const auto da = dominantCascades(ia);
        const auto db = dominantCascades(ib);
        if (da != db) {
            std::string sa;
            std::string sb;
            for (int k : da)
                sa += "c" + std::to_string(k);
            for (int k : db)
                sb += "c" + std::to_string(k);
            throw std::runtime_error(c.title + " 的两枚机位由**不同级联集合**服务（A=" + sa
                + " B=" + sb
                + "）——那时残差里混着「换了张阴影图」，与 rebase / 纹素吸附都无关，不是本判据射程");
        }
    }
}

void requireWallStimulusLive(const std::vector<WallCell>& cells)
{
    for (const auto& c : cells)
        if (cv::countNonZero(anyChannelDiff(c.a.none, c.b.none)) == 0)
            throw std::runtime_error(c.title + " 的两枚机位 none 臂**逐位相同** ⟹ 激励没生效"
                "（相机根本没动），四格其实只剩一格，全部读数作废");
}

void requireWallFieldAlive(const std::vector<WallCell>& cells)
{
    for (const auto& c : cells) {
        if (c.shadowPx <= 0)
            throw std::runtime_error(c.title + " 的 none↔本格臂 逐位不同像素 = "
                + std::to_string(c.shadowPx) + " ⟹ 阴影整块不在图上，恒 0 场上「残差 0」是恒真");
        if (c.umbra <= 1e-5)
            throw std::runtime_error(c.title + " 的有影面积 = " + pct(c.umbra)
                + " ⟹ 场退化（没有影缘，接缝与滑移都无从体现），读数作废");
    }
}

void requireWallTrustSufficient(const std::vector<WallCell>& cells)
{
    const double total = static_cast<double>(kWallWidth) * static_cast<double>(kWallHeight);
    for (const auto& c : cells) {
        if (c.floor.trust < 20000)
            throw std::runtime_error(c.title + " 的可信点数 = " + std::to_string(c.floor.trust)
                + " < 20000 ⟹ 分母是彩票，任何百分比都没有意义");
        if (static_cast<double>(c.floor.trust) < 0.05 * total)
            throw std::runtime_error(c.title + " 的可信点占比 < 5 % ⟹ 同上，抛");
        if (c.a.maskPx < 20000 || c.b.maskPx < 20000)
            throw std::runtime_error(c.title + " 的闭式门面掩码只有 " + std::to_string(c.a.maskPx)
                + "/" + std::to_string(c.b.maskPx) + " 点 ⟹ 门面根本没进画，抛");
    }
}

void requireWallPinsDistinct(const std::vector<WallCamera>& cams)
{
    // G5：四把尺必须互不相同。全同 = `csm.force_cascade` 根本没换采样源（覆写落盘了但引擎
    // 没吃，或级联几何退化成一个盒），那「自然臂 == 哪把尺」这条逐位判据恒真。
    for (const auto& c : cams) {
        int distinct = 0;
        for (size_t k = 1; k < kWallCascadeCount; ++k)
            if (cv::countNonZero(anyChannelDiff(c.pin[0], c.pin[k])) > 0)
                ++distinct;
        if (distinct == 0)
            throw std::runtime_error("因果档 " + c.name
                + " 的四张钉级图**两两全同** ⟹ csm.force_cascade 没有改变任何片元的取级，"
                  "四把尺塌成一把，级联索引读出整块是假的");
    }
}

void requireWallIndexReadable(const std::vector<WallCamera>& cams)
{
    for (const auto& c : cams) {
        if (!c.idx.found)
            throw std::runtime_error("因果档 " + c.name + " 的级联索引没读出来：" + c.idxError
                + " ⟹ 本仪器的主读数缺失，拒绝继续（表已出，见上）");
        // 🔴 只有 unmatched 是结构性违规；ambiguous 是**预期常态**：远离影缘处相邻几级给出
        // 同一个字节答案（本靶实测：有分歧的门面像素里 91 % 读不出唯一级），那些像素被剔出
        // 分母。把它当失败等于要求「每级在每像素都给出不同字节」，那是不存在的性质。
        if (c.idx.unmatched > 0)
            throw std::runtime_error("因果档 " + c.name + " 的门面分歧像素里有 "
                + std::to_string(c.idx.unmatched)
                + " 点一把尺都不匹配 ⟹ 「逐位相同 ⟺ 取该级」这条构造性质不成立"
                  "（要么钉级还动了别的东西，要么掩码进来了非门面几何）");
        if (c.idx.divergent == 0)
            throw std::runtime_error("因果档 " + c.name + " 的门面有分歧像素 "
                + std::to_string(c.idx.informed) + " 点里**没有一点**能唯一定级 ⟹ 级联索引读不"
                  "出来（四把尺在能读出的地方全给同一答案 = 阴影没进门面，或级联退化成一个盒）");
    }
}

// ── 判决线 ──────────────────────────────────────────────────────────────────────────
Verdict judgeWallIndexUnique(const std::vector<WallCamera>& cams)
{
    size_t amb = 0;
    size_t un = 0;
    size_t div = 0;
    size_t inf = 0;
    for (const auto& c : cams) {
        amb += c.idx.ambiguous;
        un += c.idx.unmatched;
        div += c.idx.divergent;
        inf += c.idx.informed;
    }
    Verdict v = mk("shadow-swim-wall-index-unique",
        "四档合计：四把尺有分歧的门面像素 " + std::to_string(inf) + "，其中能**唯一**定级 "
            + std::to_string(div) + "、匹配 ≥2 把尺（读不出、剔出分母）" + std::to_string(amb)
            + "、一把都不匹配 " + std::to_string(un),
        "界 = **恰 0**（两个都是）。这是「逐位相同 ⟺ 该片元取该级」这条构造性质的直接检验："
        "钉级 k 只覆写写进 scene data 的 splits（rgl/render_list_impl.cppm 红字：级联球/切片/"
        "阴影图内容一个字都不动）⟹ 取第 k 级的片元在钉级臂里读同一张图的同一个纹素，必逐位相同；"
        "不取第 k 级的片元换了采样源，四把尺里恰好只有它那一把能给回原答案。匹配两把 = 两级的图"
        "在该像素给出同一个字节（本靶的深黑区确实可能，但那说明**该像素读不出索引**，必须剔出"
        "分母而不是硬判一级）；一把都不匹配 = 自然臂用了四把尺之外的东西，整台仪器的读法作废。",
        { "本层不依赖任何闭式级联模型：读数只吃「相等/不相等」",
            "变异：把「唯一匹配」放宽成「第一张匹配的」→ 四把尺全同的像素被算成级 0 → 本条红" });
    v.ok = un == 0 && div > 0;
    return v;
}

Verdict judgeWallIndexMonotone(const std::vector<WallCamera>& cams)
{
    std::string all;
    bool ok = true;
    size_t straddle = 0;
    for (const auto& c : cams) {
        if (c.idx.straddles)
            ++straddle;
        char buf[200];
        std::snprintf(buf, sizeof buf, "%s[%s 松弛 %s m] ", c.name.c_str(),
            c.idx.monotone ? "单调" : "非单调", f3(c.idx.monotoneSlack).c_str());
        all += buf;
        ok = ok && c.idx.monotone;
    }
    Verdict v = mk("shadow-swim-wall-index-monotone",
        "级联索引对视深单调（只在**跨两级**的档上有可判边界，本批 " + std::to_string(straddle)
            + "/4 档跨两级）：" + all,
        "界 = 低级的最大视深 ≤ 高级的最小视深（松弛量 ≥ 0），**恰 0 容差**。理由：选级用的是"
        "视深 `dot(worldPos − eye, forward)` 的三段无条件比较（rgl/shaders/shadow.slang ①），"
        "于是「索引对深度单调」是该语义的**直接推论**，不是统计假设。读出不单调只有两种解释："
        "选级不按视深（引擎语义变了），或闭式门面掩码进来了别的几何（靶的构建期 assert ⑤ 破了）"
        "——两种都必须红，不能让接缝位置带着这个洞继续报数。门面只有一级时本条没有可判边界，"
        "记「单调」并在 observed 里点名几档跨两级，避免把它读成「验过了」。",
        { "同 J-W1：构造性质，不吃模型、不吃阈值",
            "变异：删掉单调性守卫 → 把两级的像素互换深度后本条仍绿 → 反向用例红" });
    v.ok = ok;
    return v;
}

Verdict judgeWallSeamIntervention(const std::vector<WallCamera>& cams, int serving)
{
    // 干预的两半：① 钉到**实测服务级** ⟹ 画面必须逐位不变（关掉选级机制，什么都不变）；
    //             ② 钉到**别的级** ⟹ 画面必须大改（阳性对照：量具看得见「换采样源」这件事）。
    std::string all;
    bool sameOk = true;
    bool diffOk = true;
    for (const auto& c : cams) {
        const size_t s = static_cast<size_t>(serving);
        const int64_t same = cv::countNonZero(anyChannelDiff(c.csm, c.pin[s]));
        size_t worstOther = 0;
        size_t otherDiv = 0;
        for (size_t k = 0; k < kWallCascadeCount; ++k) {
            if (k == s)
                continue;
            const int64_t d = cv::countNonZero(anyChannelDiff(c.csm, c.pin[k]));
            otherDiv += static_cast<size_t>(d);
            worstOther = std::max(worstOther, static_cast<size_t>(d));
        }
        // ② 的门槛用「门面分歧像素」做分母：换级必须在门面上留下可见的痕，而不是只在天穹/画外。
        const double otherShare = c.idx.divergent == 0
            ? 0.0
            : static_cast<double>(worstOther) / static_cast<double>(c.maskPx);
        sameOk = sameOk && same == 0;
        diffOk = diffOk && otherShare >= 0.01 && otherDiv > 0;
        char buf[240];
        std::snprintf(buf, sizeof buf, "%s 钉c%d 逐位不同=%lld｜最坏别级差=%zu（占画幅 %.2f%%） ",
            c.name.c_str(), serving, static_cast<long long>(same), worstOther, 100.0 * otherShare);
        all += buf;
    }
    Verdict v = mk("shadow-swim-wall-seam-intervention",
        "干预 = 把选级机制关掉（`csm.force_cascade` 钉到实测服务级 c" + std::to_string(serving)
            + "）：" + all + "；同桶对/跨桶对的接缝视深位移见 J-W2 与出表",
        "① 界 = **恰 0**：服务级是按同一批图逐位读出来的（票数众数），把选级钉到它 = 每个片元"
        "仍然取它本来取的那一级 ⟹ 采样输入逐位相同 ⟹ 画面逐位相同。任何非零都说明「服务级」"
        "这个读数不自洽（四档不一致时上面 measuredServingCascade 已经先抛）。"
        "② 界 = 最坏别级至少改动 **1 % 画幅**：这是阳性对照（工作点必须有牙齿）。本靶实测"
        "相邻两级在门面有影像素上的分歧 ≈ 50 %（远大于 1 %），⟹ 如果接缝横切这块立面，本仪器"
        "一定看得见；看不见就是不在，而不是量具瞎。"
        "🔴 通过条件里**不含**「必须看得见接缝」——那是在断言用户的猜想；本条判的是干预的**效应"
        "模式**（关掉机制不变、换源必然大改），接缝在不在门面上是 J-W1 直方图与出表里的**读数**。",
        { "任务书钦定的因果（干预）实验：拧「选级机制」这个开关，看现象跟不跟",
            "变异：把 force_cascade 的覆写写死成 −1（钉级臂退化成自然臂）→ ②红 + G5 红",
            "变异：把服务级写死成 2（不实测）→ 引擎选级语义一变就 ① 红" });
    v.ok = sameOk && diffOk;
    return v;
}

Verdict judgeWallWorldInvariance(const std::vector<WallCell>& cells)
{
    if (cells.size() != 4)
        throw std::runtime_error("shadow-swim-wall: 四格缺一格（" + std::to_string(cells.size())
            + "），S/T/C 归因的分母不成立");
    const double A = cells[0].floor.meanAbsCovPermille();
    const double B = cells[1].floor.meanAbsCovPermille();
    const double C = cells[2].floor.meanAbsCovPermille();
    const double D = cells[3].floor.meanAbsCovPermille();
    const double S = A - B;
    const double Cc = C - A;
    std::string obs;
    for (const auto& c : cells)
        obs += c.title + " mean=" + f3(c.floor.meanAbsCovPermille()) + "‰ p99="
            + f3(1000.0 * c.floor.p99AbsCov) + "‰ erode0=" + pct(c.raw.flipPct()) + "% erode"
            + std::to_string(kWallErodePx) + "=" + pct(c.floor.flipPct()) + "% 最优=("
            + f3(c.best.sx) + "," + f3(c.best.sz) + ")m   ";
    Verdict v = mk("shadow-swim-wall-world-invariance",
        "四组对照（A 选级·门开 / B 单级·门开 / C 选级·门关 / D 单级·门关）：" + obs
            + "⟹ S（级联接缝机制，= 干预量 A−B）= " + f3(S) + "‰；T（单级联内纹素量化，= B）= "
            + f3(B) + "‰（门关态 D = " + f3(D) + "‰）；C（坐标精度，= C−A）= " + f3(Cc) + "‰",
        "判的是「每格残差不超过门」这一条**绝对量**判据，不判「S 必须占大头」——那是在断言用户"
        "的猜想成立，本仪器要做的恰恰是去证伪它。阈值：每格 mean |Δcov| ≤ "
        + f3(kWallMeanLimitPermille) + " ‰（沿用 shadow-swim-flat 的 2.0 ‰ 口径：它的定义是"
        "「门开在远点要达到门开在出厂点的水平」，同一把尺才可比）；并额外要求四格的 erode"
        + std::to_string(kWallErodePx) + " 翻转率都 ≤ 0.01 %。"
        "🔴 比例是**读数**不是判据：S/T/C 的数值在 observed 里给全，让主 agent 自己重算。"
        "B/D 两格的「单级」= 钉到**实测服务级**（J-W3 的 ①），不是钉到随便一级：钉错级会把"
        "整块面搬到另一张图上，测出来的就不是「没有接缝」而是「没有阴影」。",
        { "债-139「~30 % 级联边界 / ~70 % 未定」那句就是没做这个拆分留下的",
            "口径与 shadow-swim-flat 同（erode 2 / mean / p99），两台仪器的数可比" });
    bool ok = true;
    for (const auto& c : cells)
        ok = ok && c.floor.meanAbsCovPermille() <= kWallMeanLimitPermille
            && c.floor.flipPct() <= 0.01;
    v.ok = ok;
    return v;
}

Verdict judgeWallDeterminism(const cv::Mat& aNone, const cv::Mat& aCsm, const cv::Mat& bNone,
    const cv::Mat& bCsm, const ShadowFlipReadout& r)
{
    const int64_t bit = cv::countNonZero(anyChannelDiff(aNone, bNone))
        + cv::countNonZero(anyChannelDiff(aCsm, bCsm));
    Verdict v = mk("shadow-swim-wall-determinism",
        "同机位重跑：逐位不同像素 = " + std::to_string(bit) + "（none + csm 两臂合计），"
            "erode0 翻转 = " + std::to_string(r.flips) + "/" + std::to_string(r.trust),
        "界 = **恰 0**。引擎在同一配置同一帧号下是逐位确定的（第 7 刀实测），所以任何非零"
        "都意味着「下面所有读数不是噪声」这个前提塌了 —— 那时四格的差全部无法归因。"
        "用 erode 0（最严口径）而不是 erode 2：同一枚机位的搬运是恒等映射，重采样误差按构造"
        "为 0，没有资格要侵蚀带。🔴 本条还是 J-W3 的 ① 的**对照**：J-W3 说「钉到服务级逐位不变」，"
        "本条说「同配置重跑逐位不变」——两者共用同一个 0 地板，任何一边不成立另一边就不能信。",
        { "债-139「地板先钉死：同机位跑两次逐位相同」同一条纪律",
            "变异：把相机 B 的平移写成 0 → G2 红（两臂逐位相同）" });
    v.ok = bit == 0 && r.flips == 0;
    return v;
}

Verdict judgeWallOffsetSensitivity(const ShiftCandidate& best, double injectedM,
    double meanPermilleAtZero)
{
    const double dx = std::abs(best.sx - injectedM);
    Verdict v = mk("shadow-swim-wall-offset-sensitivity",
        "注入 " + f3(injectedM) + " m 世界滑移（挪靶 + 跟相机）：最优平移 = (" + f3(best.sx)
            + ", " + f3(best.sz) + ") m，该处失配 " + std::to_string(best.flips) + "/"
            + std::to_string(best.trust) + "；零位移处 mean |Δcov| = " + f3(meanPermilleAtZero)
            + " ‰",
        "判据 = 最优平移的 u 分量落在 (d, 0) ± 一个粗网格步长（" + f3(kWallSearchCoarseM)
            + " m）内、v 分量 |·| ≤ 同一步长，且零位移处**看得见**差（mean |Δcov| > "
            + f3(kWallMeanLimitPermille) + " ‰）。🔴 「看得见差」这一支必须用**连续量**而不是"
            "翻转率：shadow-swim-flat 实测注入臂的 erode0 翻转率是 0.0000 %（影缘像素太少，"
            "翻进翻出几乎相消），只有 mean |Δcov| 看得见 —— 拿翻转率当这一支会把整条守卫废掉。"
            "本条红时 J-W4 的「0」不作数：量具看不见滑移，读出的 0 有两种解释。",
        { "任务书钦定的阳性对照（工作点必须有牙齿：诱导量 ≥ 2 × 判据界）",
            "变异：把平移搜索钉成恒 0 → 本条红（报 (0,0)）" });
    v.ok = dx <= kWallSearchCoarseM && std::abs(best.sz) <= kWallSearchCoarseM
        && meanPermilleAtZero > kWallMeanLimitPermille && best.found;
    return v;
}

}  // namespace RGL::imgqa
