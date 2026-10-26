// shadow_edge_rebase_judge.cpp —— `shadow-edge-rebase` 的**判决与闸门面**（四格对拍）。
//
// 缝 = 「四格怎么采」（`shadow_edge_rebase.cpp`：臂表 / 采集 / 回显四查 / 读数装配）与
// 「读数怎么裁」（本文件：G1 靶同源 / G5 每板必量 / J-R1·R2·R3 / 像素差）。改界动本文件与
// `judgments_table_edge.cpp` 的 rebase 段（两处口径必须同改），改采集动另一侧。
//
// 三条判决线为什么长这样（尤其「变好」为什么不写成 ④/③ 的比值）全文住
// `shadow_edge_rebase.cpp` 头注与 `judgments_table_edge.cpp`，本文件不重述。
//
// ## 变异红线（哪个变异让我红；🔴 四条全部真跑，实跑记录在文件末）
//   · **M1** 生成器把远点靶 measure 的 trueEdge 的 `off` 删掉（= 「真值不随平移」）
//     → **构建期 assert ⑪ 红**（读回已写进 manifest 的数，不是重算工作帧）；把 assert ⑪
//     单独旁掉后 **G1 红**（在任何一次引擎进程之前拦下）⟹ 构建期一条网、运行期一条网。
//     ⚠ 首版 assert ⑪ 只比 fr 与 fr0（恒真自检），**M1 当场从它下面走过去**——补强成读回
//     manifest 才拦住（这条已写进 assert ⑪ 的注释，别改回去）。
//   · **M2a** `o.coordinateRebaseValue = false`（门开强制关掉）→ **G3 第 4 查红**
//     （「落盘了但引擎没按它跑」）。它比任何判决都早，是「配置写了没生效」的唯一出口。
//   · **M2b** 把 ① 的数据喂进 ④ 那一格（`judgeBenefit(cells[0], cells[2], cells[0])`）
//     → J-R2 **必须翻绿**，否则它不读 ④ 的数据。
//   · **M3** 删掉在场性判定（`c.shadowAbsent = false`，即「恒不认为阴影缺席」）→ J-R3 从红
//     变**绿**（恰 0 那格被放行）⟹ 这条判据承重。⚠ 反向的「把 `== 0` 改成 `< 某正小数`」
//     **不是**弱化而是强化（只会让更多格被判缺席），首版把这条红线写反了、实测才发现。
//   · `requireEveryPlateMeasured` 的 `plates == 0` 守卫删掉 → 对空集合的断言恒真 = 假绿。
//   · `plateBoundMm` 的纹素项写成常数 → J-R1/J-R2 的界与级联脱钩，换机位静默换刻度。
//
// 🔴 **M2b 改变了 J-R2 的界的形态**（不是调数）：首版钉绝对界 |err④| ≤ 1 纹素 + 2 mm/px，
//    实测 M2b **仍然红** ⟹ 那条红不来自 ④ 的数据，是判据自身坏了——绝对误差里有一枚与原点
//    无关的共模偏置（出厂点门关态自己就 1.24–2.46 枚纹素），绝对界在最好的实现下也满足不了。
//    界因此改成「与出厂基准 ① 之差」；改完 M2b 翻绿、诚实跑仍红。这是本批第二条「界在变异
//    下自曝」（同族前例 = 计划 §3 批 5「判据失明第四例」）。
#include "shadow_edge_rebase.h"
#include "imgqa.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {

double plateBoundMm(const Cell& c, std::size_t p)
{
    const auto& pl = c.target.plates[p];
    const std::array<double, 3> dir { pl.scan.to[0] - pl.scan.from[0], 0.0,
        pl.scan.to[2] - pl.scan.from[2] };
    return c.texelMm[p] + 2.0 * worldPerpMm(c.proj, pl.trueEdge, dir, 1.0);
}

double medianAbs(const std::vector<double>& errs)
{
    std::vector<double> a;
    for (const double e : errs)
        if (!std::isnan(e))
            a.push_back(std::abs(e));
    if (a.empty())
        return kNaN;
    std::sort(a.begin(), a.end());
    return a[a.size() / 2];
}

std::size_t measurable(const std::vector<double>& errs)
{
    return static_cast<std::size_t>(
        std::count_if(errs.begin(), errs.end(), [](double v) { return !std::isnan(v); }));
}

void printPixelDiff(const Cell& a, const Cell& b, const char* what)
{
    const long long n = cv::countNonZero(anyChannelDiff(a.img[1], b.img[1]));
    cv::Mat ad;
    cv::absdiff(a.img[1], b.img[1], ad);
    std::vector<cv::Mat> ch;
    cv::split(ad, ch);
    cv::Mat m;
    cv::max(ch[0], ch[1], m);
    cv::max(m, ch[2], m);
    double mx = 0.0;
    cv::minMaxLoc(m, nullptr, &mx);
    const double pct
        = 100.0 * static_cast<double>(n) / static_cast<double>(ad.rows * ad.cols);
    std::printf("  %-30s 逐位不同 %8lld 像素 = %6.3f %%   最大 %3.0f LSB\n", what, n, pct, mx);
}

double roundToFloat32(double v)
{
    return static_cast<double>(static_cast<float>(v));
}

FarGridFacts farGridFacts(const EdgeTarget& far)
{
    FarGridFacts g;
    g.entity = entityPositionOf(far);  // 🔴 减法只有 `entityPositionOf` 这一份，别在这里再写一遍
    g.rounded = roundToFloat32(g.entity[0]);
    g.inducedM = g.rounded - g.entity[0];
    const double a = std::abs(g.entity[0]);
    int e = 0;
    std::frexp(a, &e);  // a = mantissa · 2^e，mantissa ∈ [0.5, 1) ⟹ e = floor(log2 a) + 1
    // 🔴 指数换算：frexp 的 e 比 floor(log2 a) **大一**（首版按 floor 写 e−23，把 ULP 报成 0.5
    // 而真值是 0.25 —— 打印错、且 node translation 的「亚格」上界松了一倍）。
    g.ulpM = std::ldexp(1.0, e - 24);
    return g;
}

/// 世界 X 上的误差投到**测量轴 u**（光的水平行进方向）上的投影系数。绝对尺量的是沿 u 的影边
/// 落点，不是世界 X，所以「诱导了多少误差」与「量得出多少」之间差这一个系数。
/// 值 = (2/3) / |(2/3, −3/5)|；与生成器的 `EDGE_U_PROJ_X` 同源（都从 scene.lua 的光向派生），
/// 而本命令的 u 向量本身从 manifest 的 `measure.u` 读——🔴 所以这里**不写死 0.7433**，
/// 由调用侧把 manifest 的 u[0] 传进来（抄一份 = 光向一改就静默失效的第二份真相）。
double tightestPlateBoundMm(const Cell& base)
{
    double tightest = -1.0;
    for (std::size_t p = 0; p < base.target.plates.size(); ++p) {
        const double b = plateBoundMm(base, p);
        if (tightest < 0.0 || b < tightest)
            tightest = b;
    }
    return tightest < 0.0 ? kNaN : tightest;
}

void requireFarGridHasTeeth(const Cell& base, const EdgeTarget& far)
{
    const FarGridFacts g = farGridFacts(far);
    if (g.onFloatGrid())
        throw std::runtime_error(
            "G1b: 远工作点的 entity position " + std::to_string(g.entity[0])
            + " 恰可 float32 精确表示 ⟹ 「绝对层在输入处多过一遍 float」这类回归诱导的位移"
              "恰为 0，本命令按构造量不出来（债-159 的成因）。工作点必须落在两个可表示值之间");
    double tightest = -1.0;
    std::string tightestPlate;
    for (std::size_t p = 0; p < base.target.plates.size(); ++p) {
        const double b = plateBoundMm(base, p);
        if (tightest < 0.0 || b < tightest) {
            tightest = b;
            tightestPlate = base.target.plates[p].name;
        }
    }
    if (tightest < 0.0)
        throw std::runtime_error("G1b: 出厂基准格里一块板都没有 ⟹ 界无从谈起（对空集合的"
            "「误差在界内」恒真 = 假绿）");
    // u[0] = 沿光水平行进方向的单位向量的 X 分量（manifest 的唯一读数接口）。
    // 🔴 单位口径：`plateBoundMm` 回**毫米**，而 `FarGridFacts::inducedM` 是**米**——两边统一到
    // 毫米再比。（首版没统一，G1b 拿 0.087「米」去比 2×18.57「毫米」当场自己抛了出来——
    // 闸门误抛也是闸门坏，不是通过。）
    const double uProj = far.uDir[0];
    const double projected = std::abs(g.inducedM) * 1000.0 * uProj;
    if (!(2.0 * tightest <= projected))
        throw std::runtime_error("G1b: 远工作点网格外诱导的误差 = "
            + std::to_string(g.inducedM * 1000.0) + " mm，投到测量轴 u 只有 "
            + std::to_string(projected) + " mm，不到最紧逐板界 " + std::to_string(tightest)
            + " mm（板 " + tightestPlate
            + "，= 1 枚本级纹素 + 2 枚像素刻度）的 2 倍 ⟹ 这台绝对尺量不出「大坐标精度」这一类"
              "缺陷，J-R2 的绿不作数（本机实测最紧界 18.57 mm，而 1e6 量级最多诱导 31.25 mm × "
              "u 投影 0.7433 = 23.2 mm，连 2 倍都不到 ⟹ 就是这种白改）。抬量级或换更细的判据，"
              "不许放宽本闸门");
}

void requireFarWorkingPointProperties(const EdgeTarget& far)
{
    // 🔴 债-159 的运行期判据本体。三条**性质**，全部从 manifest 派生（不抄米数）：
    //   ① 纯 +X（y/z 为 0）——动了 y 就破地面接收关系，闭式前提③当场作废；
    //   ② 进 rootTransformOf 的那枚数（= `worldOffset − nodeTranslation`，**不是** worldOffset）
    //      不在 float32 网格上——网格上的工作点没有亚格小数可丢，「绝对层在输入处多过一遍 float」
    //      这类回归在图像上根本不存在；
    //   ③ node translation 非零且**亚格**（单位阵 ⟹ 逐图元复合链 = 乘 1，那条链没被量过）。
    // 为什么从 `requireFarIsTranslatedCopy` 里拆出来（不是为了让护栏不叫，认知复杂度实测 18 > 15）：
    // 两条判据的**改动缝**不同——本条随「靶的取值怎么选」变（EDGE_FAR_OFFSET / EDGE_FAR_NODE_T），
    // 那条随「闭式真值的形状」变（哪些量该跟着平移）。混在一个函数里，改靶的取值就要去读一段
    // 几何对账代码才知道会不会碰坏它。
    for (int i = 1; i < 3; ++i)
        if (std::abs(far.worldOffset[i]) > 1e-9)
            throw std::runtime_error("G1: 远点靶的 measure.worldOffset[" + std::to_string(i)
                + "] = " + std::to_string(far.worldOffset[i]) + " ≠ 0 ⟹ 平移不是纯 +X。动了 y 会破"
                  "「接收面水平 y=0」这条闭式前提（真值影边带 Δz·cot 项漂移），仪器读的是假数");
    const FarGridFacts g = farGridFacts(far);
    if (g.onFloatGrid())
        throw std::runtime_error("G1: 远工作点的 entity position " + std::to_string(g.entity[0])
            + " 恰可 float32 精确表示 ⟹ 「绝对层在输入处多过一遍 float」这类回归在图像上根本不存在"
              "（实测：1e6 上注入该变异后 shadow-edge-rebase 与 shadow-swim-flat 的报告与不注入"
              "时**逐字节相同**）。这就是债-159。改 tools/qa_scenes.py 的 EDGE_FAR_OFFSET");
    if (std::abs(far.nodeTranslation[0]) <= 0.0 || std::abs(far.nodeTranslation[0]) >= g.ulpM / 2.0)
        throw std::runtime_error("G1: 远点靶的 measure.nodeTranslation = "
            + std::to_string(far.nodeTranslation[0]) + " m，不在 (0, ULP/2 = "
            + std::to_string(g.ulpM / 2.0) + ") 内 ⟹ 它在 float 域逐图元复合下不会消失，"
              "「复合改在 double 域乘」那条链又没有可丢的精度了（债-159 的第二半）");
    for (int i = 1; i < 3; ++i)
        if (std::abs(far.nodeTranslation[i]) > 0.0)
            throw std::runtime_error("G1: nodeTranslation[" + std::to_string(i)
                + "] ≠ 0 ⟹ node 平移动了 y/z：动 y 破地面接收关系，动 z 让「远点 = 出厂靶 + "
                  "worldOffset」这条自证多出一项待对账的偏移");
}

void requireFarIsTranslatedCopy(const EdgeTarget& base, const EdgeTarget& far,
    const EdgeCascadeModel& baseModel, const EdgeCascadeModel& farModel)
{
    const std::array<double, 3> near0 { 0.0, 0.0, 0.0 };
    if (far.worldOffset == near0)
        throw std::runtime_error("G1: 远点靶 " + std::string(kFarStem)
            + " 的 manifest 没有 measure.worldOffset（或它是零）⟹ 它不是远工作点，四格只剩"
              "两格。重新生成：python3 tools/qa_scenes.py --only "
            + std::string(kFarStem) + " --out build/rgl/models");
    requireFarWorkingPointProperties(far);  // 🔴 债-159：工作点的数值性质（拆出去的理由见该函数）
    const auto shifted = [&](const char* what, const std::array<double, 3>& got,
                              const std::array<double, 3>& exp) {
        for (int i = 0; i < 3; ++i)
            if (std::abs(got[i] - (exp[i] + far.worldOffset[i])) > kEdgeShiftTolM)
                throw std::runtime_error(std::string("G1: 远点靶的 ") + what + " 第 "
                    + std::to_string(i) + " 分量 = " + std::to_string(got[i])
                    + "，不是出厂靶 " + std::to_string(exp[i]) + " + worldOffset ⟹ **闭式真值"
                      "没跟着平移**（或平移不是纯几何平移）。仪器只信 manifest，这条不验就是"
                      "把 1e6 mm 的假误差当读数");
    };
    shifted("measure.camera.position", far.camera.position, base.camera.position);
    shifted("measure.camera.target", far.camera.target, base.camera.target);
    shifted("measure.sceneBounds.center", far.boundsCenter, base.boundsCenter);
    if (far.plates.size() != base.plates.size())
        throw std::runtime_error("G1: 两枚靶的板数不同（" + std::to_string(far.plates.size())
            + " vs " + std::to_string(base.plates.size()) + "）——不是同一台靶，逐板对拍无意义");
    for (std::size_t p = 0; p < far.plates.size(); ++p) {
        if (far.plates[p].name != base.plates[p].name)
            throw std::runtime_error("G1: 第 " + std::to_string(p) + " 块板两靶不同名（"
                + far.plates[p].name + " vs " + base.plates[p].name + "）——板序 = 距离序 = "
                  "级联序，错位一次整条曲线作废");
        shifted((far.plates[p].name + ".farFace").c_str(), far.plates[p].farFace,
            base.plates[p].farFace);
        shifted((far.plates[p].name + ".trueEdge").c_str(), far.plates[p].trueEdge,
            base.plates[p].trueEdge);
        shifted((far.plates[p].name + ".scan.from").c_str(), far.plates[p].scan.from,
            base.plates[p].scan.from);
        shifted((far.plates[p].name + ".scan.to").c_str(), far.plates[p].scan.to,
            base.plates[p].scan.to);
    }
    for (int c = 0; c < 4; ++c)
        if (std::abs(farModel.splits[c] - baseModel.splits[c])
                    > 1e-9 * std::abs(baseModel.splits[c])
            || std::abs(farModel.texelM[c] - baseModel.texelM[c])
                    > 1e-9 * std::abs(baseModel.texelM[c]))
            throw std::runtime_error("G1: 级联 splits/texel 随世界平移漂了（第 " + std::to_string(c)
                + " 级）——它们是相机到 SceneBounds 的**相对**量，本应与平移无关；漂了 = 公式链"
                  "里藏着未声明的绝对坐标假设，这台靶撑不住远工作点，直说换靶别硬凑");
}

void requireEveryPlateMeasured(const std::vector<Cell>& cells)
{
    std::vector<std::string> miss;
    std::size_t plates = 0;
    for (const auto& c : cells) {
        plates += c.target.plates.size();
        for (const auto& m : c.unexpected)
            miss.push_back(m);
    }
    if (cells.empty() || plates == 0)
        throw std::runtime_error("G5: 四格里一块板都没有（板数 = " + std::to_string(plates)
            + "）——对空集合的「每块板都量到了」恒真 = 假绿，拒绝出数");
    if (!miss.empty()) {
        std::string all;
        for (const auto& m : miss)
            all += "\n    " + m;
        throw std::runtime_error("G5: " + std::to_string(miss.size())
            + " 个 (格 × 臂 × 板) 闭式预判**可量**却找不到过渡中点（意外失踪 = 靶或影坏了）："
            + all);
    }
    std::printf("  G5 过：四格 × 六臂 × %zu 板，无意外失踪\n", plates / cells.size());
}

Verdict judgeShadowPresent(const std::vector<Cell>& cells)
{
    std::ostringstream obs;
    bool all = true;
    std::string bad;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i)
            obs << " · ";
        obs << cells[i].title << "=" << cells[i].shadowPx;
        if (cells[i].shadowAbsent) {
            all = false;
            bad += "\n    " + cells[i].title + "：csm 臂与 mode=none 臂**逐位相同** ⟹ 阴影对"
                   "交付图的贡献恰为 0 像素（不是「小」，是没有）。";
        }
    }
    std::string why =
        "判据 = 每格的 csm 臂与 mode=none 臂**不得逐位相同**（零容差算子，不是「差异大于某个"
        "阈值」）。\n"
        "🔴 为什么必须独立成一条：本批远点门关态读出的不是「影边摆偏」而是「影边根本不存在」。\n"
        "    没有这条，那种格子会被下游读成「误差量不到 = 预期不可量」而静默放行——「没有\n"
        "    阴影」与「阴影对但量不出」在误差表上是同一个 NaN，必须在这里分家。\n"
        "    它同时是 J-R2 的反向守卫：若门开态也掉到 0，J-R2 的「可量板数 ≥ 门关」会因两边\n"
        "    都是 0 而成立，只有本条能揭穿那种双输。\n"
        "· 界 = 恰 0（不是「小于某个小数」）：两臂是确定性渲染、只差阴影段 ⟹ 噪声地板恰为 0。";
    if (!all)
        why += bad
                 + "\n    ⟹ 这是**另一类失效**（大坐标把某个盒或剔除算飞），不许混进 rebase 的"
                   "\n       精度收益里；已排除 proxy / 阴影簇 LOD / 簇路径三条（命令头有读数）。";
    return { "shadow-edge-rebase-shadow-present", all, obs.str(), why,
        { "本批实测（2026-10-25）：|offset| ≥ 280 m 时 csm 臂与 none 臂逐位相同（128/192/256 m"
            " 正常），+X/+Y/+Z 三轴同阈值；ULP(280)=0.03mm 解释不了 ⟹ 新缺陷，另立案",
            "skill rgl-test-discipline「0 差异的两种解释」+ skill rgl-verify「永不触发的分支与"
            "坏掉的分支同形」",
            "变异红线：`shadowPx == 0` 改成 `< 某小数` → 恰 0 那格被放行，本条从红变绿" } };
}

Verdict judgeNoHarm(const Cell& off, const Cell& on)
{
    std::size_t both = 0, worse = 0;
    double worstRatio = 0.0, worstDelta = 0.0;
    std::string worstName;
    for (std::size_t p = 0; p < off.errNat.size(); ++p) {
        if (std::isnan(off.errNat[p]) || std::isnan(on.errNat[p]))
            continue;
        ++both;
        const double delta = std::abs(on.errNat[p]) - std::abs(off.errNat[p]);
        const double bound = plateBoundMm(off, p);
        if (delta > 0.0)
            ++worse;
        if (std::abs(delta) / bound > worstRatio) {
            worstRatio = std::abs(delta) / bound;
            worstDelta = delta;
            worstName = off.plateNames[p];
        }
    }
    std::ostringstream obs;
    obs << "出厂点两态都可量 " << both << "/" << off.errNat.size() << " 板；逐板 |err②|−|err①| "
        << "最大 " << edgeMm(worstDelta) << " mm（板 " << worstName << "，= " << std::fixed
        << std::setprecision(1) << worstRatio * 100.0 << "% 界）；门开变差的板 " << worse
        << " 块（界内也算变差，逐数进 observed）";
    const std::string why
        = "界 = 1 枚该板本级纹素 + 2 枚像素刻度（逐项推导住本条注册项与 plateBoundMm）。\n"
          "判据 = 门开态逐板 |误差| 不得比门关**变大**超过这个界（超界 = 接原点引入了回归）。\n"
          "🔴 这条只管「不坏」：出厂点门关态精度本来就够，rebase 在那里本来就该什么都不改。\n"
          "    它单独用会放行「门关 = 门开」的实现，必须与 shadow-edge-rebase-benefit 同读；\n"
          "    本命令的 G3 第 4 查（coordinate.rebase 回显）+ G4（门开必须动图）是它不沦为\n"
          "    恒真断言的前提。";
    return { "shadow-edge-rebase-no-harm", both > 0 && worstRatio <= 1.0, obs.str(), why,
        { "docs/相对坐标地基_plan.md §3 批 5 判据表第一行（出厂点 = 不坏）",
            "第 1 刀落地记录：出厂点门开 vs 门关 = 13796 像素 / 1.497 % / 最大 165 LSB（本条"
            "的像素侧对照，本命令另给同口径三行只报数）",
            "本命令 G4 = 本条的反向守卫：门没真开时 ②① 逐位相同，本条会「全绿」而毫无意义" } };
}

Verdict judgeBenefit(const Cell& base, const Cell& farOff, const Cell& farOn)
{
    const std::size_t nOff = measurable(farOff.errNat);
    const std::size_t nOn = measurable(farOn.errNat);
    std::size_t both = 0, over = 0;
    double worstRatio = 0.0, worstDelta = 0.0;
    std::string worstName;
    // 🔴 判据是「④ 与**出厂基准 ①** 的差」而不是「④ 的绝对误差 ≤ 界」。理由（M2b 变异实测
    //    逼出来的，不是先想好的）：本靶的绝对误差里有一枚**共模偏置**（出厂点门关态自己就
    //    1.24–2.46 枚纹素，见 shadow-edge-ruler 的 J1 表），它来自常数深度偏置 + PCF 不对称，
    //    与原点无关 ⟹ 要求「|误差| ≤ 1 纹素」在**最好的实现下也做不到**，那条界会把
    //    「rebase 没买到精度」与「界写错了」读成同一个红。对 ① 做差把共模项消掉，剩下的
    //    才是「远点门开比出厂点差多少」。
    // ⚠ 这不是把判据退回差分（差分失明的坑在 J-R1/J-R3 与 ruler 的闸门 4 上都由绝对判据兜着）：
    //    基准 ① 的绝对正确性由 shadow-edge-ruler 的闸门 4（参照态自洽 = 全量几何进阴影图时
    //    误差落在本级纹素预算内）与 G1（两靶闭式逐数相同）担保，本条只回答「远点有没有比它坏」。
    for (std::size_t p = 0; p < farOn.errNat.size(); ++p) {
        if (std::isnan(farOn.errNat[p]) || std::isnan(base.errNat[p]))
            continue;
        ++both;
        const double delta = std::abs(farOn.errNat[p] - base.errNat[p]);
        const double bound = plateBoundMm(base, p);
        if (delta / bound > worstRatio) {
            worstRatio = delta / bound;
            worstDelta = delta;
            worstName = farOn.plateNames[p];
        }
        if (delta > bound)
            ++over;
    }
    std::ostringstream obs;
    obs << "远点门开可量 " << nOn << " 板 / 门关可量 " << nOff << " 板"
        << (farOff.shadowAbsent ? "（门关态阴影整块消失，无毫米数可言）" : "")
        << "；与出厂基准 ① 同板可比 " << both << " 板，逐板 |err④−err①| 最大 "
        << edgeMm(worstDelta) << " mm（板 " << worstName << "，= " << std::fixed
        << std::setprecision(1) << worstRatio * 100.0 << "% 界），超界 " << over << " 板";
    std::string why
        = "判据 = 远工作点（|camera| ≈ 大坐标·float32 网格外，量级从 manifest 读）门开态的\n"
          "    影边必须**不比出厂基准 ① 坏**：可量板数\n"
          "    ≥ 门关态，且逐板 |err④ − err①| ≤ 1 枚该板本级纹素 + 2 枚像素刻度（同 J-R1 的界）。\n"
          "🔴 这条才是 rebase 的存在理由。只验 J-R1 会放行「门关 = 门开」的实现。\n"
          "· 为什么对 ① 做差而不是钉绝对值：绝对误差里有一枚与原点无关的**共模偏置**（出厂点\n"
          "  门关态自己就 1.24–2.46 枚纹素，见 shadow-edge-ruler 的 J1 表；来源 = 常数深度偏置\n"
          "  + PCF 采样不对称），钉「|误差| ≤ 1 纹素」在最好的实现下也做不到 ⟹ 那条界会把\n"
          "  「rebase 没买到精度」与「界写错了」读成同一个红。**这条界是 M2b 变异实测逼出来的**\n"
          "  （把 ① 的数据喂进 ④ 那一格，绝对界仍然红 ⟹ 红不是 ④ 的数据造成的 = 判据自身坏了）。\n"
          "· 基准 ① 的绝对正确性由 shadow-edge-ruler 的闸门 4（参照态自洽）+ 本命令 G1（两靶\n"
          "  闭式逐数相同）担保，所以这里做差不等于退回「只能证不变」的差分判据。\n"
          "⚠ 门关在远点若**阴影整块消失**（本批实测如此，见 J-R3），它没有毫米读数 ⟹ 比值\n"
          "  无定义，所以不写成「④ 是 ③ 的 1/N」。把「另一类失效」混进「rebase 的收益」正是\n"
          "  本批判据变更点名要避免的形态。";
    if (over > 0)
        why += "\n    超界板数: " + std::to_string(over);
    return { "shadow-edge-rebase-benefit",
        nOn >= nOff && nOn > 0 && both > 0 && over == 0, obs.str(), why,
        { "docs/相对坐标地基_plan.md §3 批 5 判据表第二行（|camera|≈1e6 = 变好，本批唯一最重要"
            "的数）+「一条改变批 5 判据方向的依赖」",
            "本命令 G1（靶同源）= 本条的假绿守卫：真值漏挪时 ④ 与 ① 的差直接多出 1e6 mm 而红",
            "M2b 变异（④ 槽喂 ① 的数据）→ 本条必须翻绿；绝对界版本做不到这一点（实测仍红），"
            "这是界从绝对改成对基准之差的直接依据",
            "债-85 行内量级表（0.5px 可见爬行触发点 ≈ 7.6e5）= 远工作点取 1e6 的理由，不在本"
            "文件重新推导" } };
}

}  // namespace RGL::imgqa
