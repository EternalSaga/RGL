// selftest_edge.cpp —— `shadow-edge-ruler`（影边世界位置绝对尺）**口径层算子自检**，无引擎。
//
// 另立 TU（不并进 selftest_survival.cpp）的理由 = 该文件的缝声明为「shadow-caster-survival
// 口径层（存活率族算子）」——本仪器测的是**影边的世界位置**（过渡中点/级联闭式/换算），
// 算子面完全不同。切法先例 = 一台仪器一个自检 TU（selftest_seam / selftest_survival）。
//
// 🔴 2026-10-20 口径改版（主 agent 规格错误 E1/E2 的自检侧，论证住 shadow_edge_metric.h
// 与 shadow_edge_ruler.cpp 头注）：过零点从「50% 绝对」改成「自身过渡中点（相对阈值）」，
// 本文件的**立命断言因此是浅斜坡两条**——它们证明浅影（0→0.30、0.70→1.00）量得到且与
// 深影同样精确；旧「深斜坡」断言全部保留（深影是新口径的特例，lit=0/umbra=1 时相对阈值
// 退化为 0.5）。E2（D 扫描退役）把 J3 从「随 D 单调」换成 seamWidths 纯函数（级间差）。
//
// ## 这些断言钉的是「算子数的是我说的那个量」，期望值全部闭式（不抄实机读数、不抄被测码）
//   ① 🔴 立命断言（本批改后）：合成 cov 斜坡的**过渡中点**误差 < 1e-9，三条——
//     ①a 深斜坡 0→1（旧口径特例）；①b 浅斜坡 **0→0.30**（浅影量得到——首跑 112 组合
//     全灭的形态，现在必须出数且精确）；①c 近受光端浅斜坡 **0.70→1.00**（证明不是
//     「只在下半段管用」）。线性插值退化 = step 量级误差，三条立刻红。
//   ①d 平台取中位数：下游平台叠 0.02 起伏后定位不动（取 max 的弱化变异把阈值搬走 → 红）。
//   ①e 过渡高 0.30→0.31（h=0.01 < 下界 0.15 = 10 个 8bit-LSB）→ **必须抛**，不许出数。
//   ①f 四类抛的错误串**两两不同**（常函数 / 平台窗退化 / 过渡过浅 / 多过零）= 可归因。
//   ② 斜坡**变宽中点不动**（w = 4/8/16 px 三档读数一致 <1e-9）——证明量的是位置不是
//     陡峭度（shadow-caster-survival「拿被模糊量当位置量」教训的算子侧防线）。
//   ③ 全 0 / 全 1 / 多个过零点 各一条守卫；🔴 多个过零点**必须抛**——返回第一个 = 静默
//     把接触缘当远影缘（读数看着正常、物理上是另一个量）。
//   ④ subpixel=false 粗模式落在 ±step/2（与 true 分支确有差别，不是同码）。
//   ⑤ worldPerpMm 闭式对拍（针孔 mm/px = 2·tan(fov/2)·aspect·depth/width）+ 零方向/屏外守卫。
//   ⑥ 级联公式链闭式对账（期望值 = 手算抄录，推导注释在断言旁；quantize 向上取整、
//     practical split mix=0.5、texel 分母 = 块边长 1024 各钉一处）。
//   ⑦ 闸门 C 五条（正常不抛 / 缺失点名一次报全 / **零臂、零板两个空集合守卫** / 错误串可归因）。
//   ⑧ 闸门 D 四条（正常不抛 / 恰等于界算通过 / 超界抛且点名 / 空清单抛）。
//   ⑨ 三条判决线挂号（J3 已改名 shadow-edge-cascade-agreement——旧名复发 = 挂号检查红）。
//   ⑩ seamWidths 纯函数五条（全局翻号读数不变 = 先减后取绝对值 / NaN 传播不猜 0 /
//     空板清单抛 / 形状守卫抛 / 某交界全不可量 printSeamWidthInfo 抛）。
//
// ## 变异红线（哪个变异让我红；🔴 = 2026-10-20 实跑记录在交付说明，未标 = 声称未跑）
//   · 🔴 相对阈值改回绝对 0.5（G5-①）→ 浅斜坡 ①b/①c 红（浅影立刻不可量 = 首跑形态复发），
//     深斜坡断言照绿——这条分家正是新旧口径的分界。
//   · 🔴 平台中位数改成最大值（G5-② 弱化）→ 浅斜坡定位红（umbra 侧取 max 抬高阈值）。
//   · 🔴 过渡高下界挪到 0（G5-③）→ ①d「过渡过浅必须抛」红。
//   · 🔴 过零点插值退化成取整（G5-④，返回左样本）→ ① 三条红（实测差恰 1 step = 1 mm）。
//   · 多过零守卫删掉（返回第一个）→ ③ 红。**静默选错边的入口就在这条**。
//   · 常函数守卫删掉 → 全 0 / 全 1 两条红。走行阈值 0.5·gmax 挪到 0 → 平台窗退化红。
//   · subpixel 分支同码 → ④ 红。
//   · worldPerpMm 的 depth 写成常数 → ⑤ 红（换深度不换刻度 = 换机位静默换单位）。
//   · 🔴 quantize 向下取整（M3）→ 「量化向上取整方向」那条红（⚠ 首版**没有**这条时变异
//     实测存活 = 自检真缺口：幂点输入不执行取整分支；补非幂输入用例后 M3d 红于 2.75）。
//     split mix 写 0 或 1、texel 分母写 2048 → ⑥ 闭式各红。
//   · 闸门 C 空集合守卫删掉 → ⑦「零臂应抛」「零板应抛」红（对空集合的断言恒真 = 假绿）。
//   · 🔴 闸门 D 界弱化（M4，`err > bound` → `err > 10·bound`）→ ⑧「超界应抛」两条红。
//   · seamWidths 的 `|b−a|` 改成带符号 `b−a` → ⑩「全局翻号读数不变」红；NaN 当 0 →
//     ⑩「NaN 传播」红；空守卫删掉 → ⑩ 两条红。
//   · 另：采样口径 ±0.5px（`sampleCovBilinear` 的 fx−0.5 约定挪掉）→ ① 红恰 0.5px
//     ——2026-10-20 开发期误改实测（got 0.251838 want 0.230000），当场坐实两条防线有效。
#include "selftest.h"

#include "imgqa.h"
#include "shadow_edge_metric.h"
#include "shadow_edge_ruler.h"
#include "shadow_kernel_metric.h"

#include <opencv2/core.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

/// 自检测试相机：pos (0,1,10) → target (0,1,0)（水平朝 −z），画幅 200×100，fov 40。
/// 地面 y=0 上 z=4 的一条 x 向扫描线 = 平行于像平面（深度恒 6）⟹ 世界 x 与像素 px
/// **精确线性**——合成斜坡的过零点因此有 <1e-9 的闭式期望（透视二阶项在这个取景里为零）。
inline Engine::Camera edgeTestCamera()
{
    Engine::Camera c;
    c.position = { 0.0, 1.0, 10.0 };
    c.target = { 0.0, 1.0, 0.0 };
    c.fovDegrees = 40.0;
    return c;
}

/// 一行从 lo 升到 hi 的斜坡 + 全行同值的 cov 场（中点在像素坐标 pxHalf 处，宽 widthPx）。
/// 🔴 口径：`projectPoint3` 是**边缘约定**（像素 i 覆盖 [i, i+1)，中心在 i+0.5），
/// 而这里造的是**索引数组**——所以斜坡按「像素中心 = 索引 x」写成 (x+0.5)，中点交叉
/// 才恰好落在采样器认定的 pxHalf 上。少这个 +0.5 = 全场影边静默平移半像素
/// （2026-10-20 首跑实测：① 差恰 0.5px 红，算子本身没错，错的是这条合成靶）。
cv::Mat rampField(int cols, int rows, double pxHalf, double widthPx, double lo = 0.0,
    double hi = 1.0)
{
    cv::Mat m(rows, cols, CV_64FC1);
    const double a = pxHalf - widthPx / 2.0;
    for (int y = 0; y < rows; ++y) {
        auto* row = m.ptr<double>(y);
        for (int x = 0; x < cols; ++x) {
            const double t = std::min(1.0, std::max(0.0, ((x + 0.5) - a) / widthPx));
            row[x] = lo + (hi - lo) * t;
        }
    }
    return m;
}

EdgeScanLine edgeTestLine(const std::array<double, 3>& from, const std::array<double, 3>& to)
{
    EdgeScanLine l;
    l.from = from;
    l.to = to;
    l.vDir = { 0.0, 0.0, 1.0 };
    l.vOffsets = { 0.0 };
    l.stepM = 0.001;
    return l;
}

const double kEdgeTestX0 = 0.03;  // 合成影边的世界 x（真值）

/// 捕获式测量：抛 → NaN（chk 干净地红，而不是把整个 selftest 打断）。
double edgePosOrNan(const cv::Mat& field, const Projector& proj, const EdgeScanLine& line)
{
    try {
        return edgePosition(field, proj, line, true);
    } catch (const std::exception&) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

/// 在指定像素处叠一个三角小起伏：幅度 amp、半宽 halfPx（斜率刻意小于斜坡梯度，最大梯度
/// 不落其上）。用途 = 钉住「平台取**中位数**」：中位数对起伏稳健（定位不动），取 max/min
/// 的变异把阈值搬走 → 定位红。
void addPlateauBump(cv::Mat& field, double centerPx, double halfPx, double amp)
{
    for (int y = 0; y < field.rows; ++y) {
        auto* row = field.ptr<double>(y);
        for (int x = 0; x < field.cols; ++x) {
            const double d = std::abs((x + 0.5) - centerPx);
            if (d < halfPx)
                row[x] = std::min(1.0, row[x] + amp * (1.0 - d / halfPx));
        }
    }
}

/// 捕获 edgePosition 的抛错串（没抛返回空串 = 调用方的 chk 红）。
std::string edgeThrowMessage(const cv::Mat& field, const Projector& proj,
    const EdgeScanLine& line)
{
    try {
        (void)edgePosition(field, proj, line, true);
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

/**
 * ⑥ 级联公式链闭式（从 testShadowEdgeRuler 拆出 = 认知复杂度护栏，断言一字未改）。
 * 期望手算抄录：near=4 far=8 → splits/texels。中心 (0,1,4) 与相机 (0,1,10) 恰距 6
 * （±2 → 4/8，都是 2 的幂 ⟹ 量化取自身）；摆 (0,0,4) 距离是 √37 ⟹ 量化到 8/16，
 * 期望全数翻倍——首跑实测踩过。
 */
inline void edgeCascadeClosedFormChecks(Checker& c)
{
    const EdgeCascadeModel m = edgeCascadeModel(edgeTestCamera(), 200, 100, { 0.0, 1.0, 4.0 }, 2.0);
    // splits = [4.5, 4·2^{1/4}/2 + 3, 4·2^{1/2}/2 + 3.5, 8]（practical split mix=0.5）
    const double wantSplits[4] = { 4.5, 5.378414230, 6.328427125, 8.0 };
    // texel = 2·r_c/1024（块边长 = 图集 2048/2；写成 2048 的变异在这里红一半以上）
    const double wantTexelMm[4] = { 7.306118, 10.193262, 12.068048, 14.847869 };
    bool splitsOk = true, texelOk = true;
    for (int i = 0; i < 4; ++i) {
        splitsOk = splitsOk && std::abs(m.splits[i] - wantSplits[i]) < 1e-6;
        texelOk = texelOk && std::abs(m.texelM[i] * 1000.0 - wantTexelMm[i]) < 1e-4;
    }
    c.chk("edge 级联：splits 闭式（quantize 向上取整 + practical split mix=0.5，期望手算）",
        splitsOk, std::to_string(m.splits[1]) + " / " + std::to_string(m.splits[2]));
    c.chk("edge 级联：texel = 2r/1024 闭式（分母 = 块边长不是图集整幅，期望手算）", texelOk,
        std::to_string(m.texelM[0] * 1000) + " / " + std::to_string(m.texelM[3] * 1000));
    c.chk("edge 级联：edgeCascadeOf 三段比较口径（恰等于 split 不进下一级，严格 >）",
        edgeCascadeOf(m, 4.0) == 0 && edgeCascadeOf(m, 4.5) == 0
            && edgeCascadeOf(m, 4.5 + 1e-6) == 1 && edgeCascadeOf(m, 6.0) == 2
            && edgeCascadeOf(m, 7.0) == 3);
    // 🔴 量化**方向**必须单独钉一条：上面那组 near/far 恰为 2 的幂（量化取自身），
    // 「向上取整」的分支在它们身上不执行——「向下取整」变异实测在上面全绿（2026-10-20
    // 变异记录 M3b 存活 = 自检真缺口，已补）。这里给非幂输入：dist=6、R=2.1 ⟹
    // nearRaw=3.9→q=4、farRaw=8.1→q=16（向上）；向下取整会给 2/8，splits[0] 从
    // 0.5·(4+7)=5.5 塌到 0.5·(2+3.5)=2.75，一条闭式立刻分家。
    const EdgeCascadeModel mUp = edgeCascadeModel(edgeTestCamera(), 200, 100, { 0.0, 1.0, 4.0 }, 2.1);
    c.chk("edge 级联：量化向上取整方向（3.9→4 / 8.1→16，splits[0]=5.5 闭式；向下取整的变异在这红）",
        std::abs(mUp.splits[0] - 5.5) < 1e-9 && std::abs(mUp.splits[3] - 16.0) < 1e-9,
        "splits[0]=" + std::to_string(mUp.splits[0]));
    c.chk("edge 级联守卫：半径 ≤ 0 / 量化退化（near ≥ far）→ 抛",
        throwsStd([&] {
            (void)edgeCascadeModel(edgeTestCamera(), 200, 100, { 0.0, 0.0, 4.0 }, 0.0);
        }) && throwsStd([&] {
            (void)edgeCascadeModel(edgeTestCamera(), 200, 100, { 0.0, 1.0, 10.0 }, 0.04);
        }));
}

}  // namespace

/// `shadow-edge-ruler` 口径层：过渡中点算子（浅/深）+ 换算 + 级联闭式的闭式刻度，不启引擎。
void testShadowEdgeRuler(Checker& c)
{
    const Projector proj = makeProjector(edgeTestCamera(), 200, 100);
    const std::array<double, 3> edge{ kEdgeTestX0, 0.0, 4.0 };
    double pxEdge = 0.0, pyEdge = 0.0;
    const bool projected = projectPoint3(proj, edge, pxEdge, pyEdge);
    c.chk("edge 测试取景：真值点可投影", projected, std::to_string(pxEdge) + "," + std::to_string(pyEdge));

    // 窗口 ±0.6 m（16 px 斜坡 = 0.7 m 宽，两侧平台各剩 ~0.28 m = 280 样本；旧 ±0.2 窗
    // 在相对阈值口径下平台放不下——E1 改版连带，断言语义不变）。
    const auto line = edgeTestLine({ -0.6, 0.0, 4.0 }, { 0.6, 0.0, 4.0 });
    const double sTrue = kEdgeTestX0 - (-0.6);

    // ── ① 立命：过渡中点误差 < 1e-9，深/浅/近受光端三条 ──────────────────────────────
    const cv::Mat ramp = rampField(200, 100, pxEdge, 8.0);
    const double s = edgePosition(ramp, proj, line, true);
    c.chk("edge 立命·深斜坡：0→1 过渡中点误差 < 1e-9（旧口径特例；插值退化 = step 量级红）",
        std::abs(s - sTrue) < 1e-9, "got " + std::to_string(s) + " want " + std::to_string(sTrue));
    // 🔴 浅斜坡两条 = 本批改口径的立命断言（主 agent 规格错误 E1 的正面证据）：
    // 浅影（0→0.30 = 本靶地面实测平台高）与近受光端浅影（0.70→1.00）都必须同样精确。
    const double sShallow = edgePosOrNan(rampField(200, 100, pxEdge, 8.0, 0.0, 0.30), proj, line);
    c.chk("edge 立命·浅斜坡：0→0.30 过渡中点误差 < 1e-9（浅影量得到——绝对 0.5 口径下这条"
          "必抛「无过零」，E1 改版的存在理由）",
        std::abs(sShallow - sTrue) < 1e-9, "got " + std::to_string(sShallow));
    const double sHigh = edgePosOrNan(rampField(200, 100, pxEdge, 8.0, 0.70, 1.00), proj, line);
    c.chk("edge 立命·近受光端浅斜坡：0.70→1.00 同样精确 <1e-9（不是「只在下半段管用」）",
        std::abs(sHigh - sTrue) < 1e-9, "got " + std::to_string(sHigh));

    // ── ①d 平台取中位数（不是 max/min）：下游平台叠 ±0.02 小起伏，定位必须不动 ──────
    {
        cv::Mat bumpy = rampField(200, 100, pxEdge, 8.0, 0.0, 0.30);
        addPlateauBump(bumpy, pxEdge + 12.0, 2.0, 0.02);  // 下游平台内的三角起伏
        const double sBumpy = edgePosOrNan(bumpy, proj, line);
        c.chk("edge 平台中位数：下游平台叠 0.02 起伏后浅斜坡定位不动 <1e-9（取 max 的弱化"
              "变异把 umbra 抬到 0.32 → 阈值搬走 → 这条红）",
            std::abs(sBumpy - sTrue) < 1e-9, "got " + std::to_string(sBumpy));
    }

    // ── ①e 过渡过浅必须抛（0.30→0.31：h=0.01 < 下界 0.15 = 10 个 8bit-LSB）────────────
    const std::string msgThin = edgeThrowMessage(
        rampField(200, 100, pxEdge, 8.0, 0.30, 0.31), proj, line);
    c.chk("edge 守卫：过渡高 0.01 < 下界 0.15 → 抛（噪声定位的「边」不许出数）",
        msgThin.find("过渡过浅") != std::string::npos, msgThin.substr(0, 80));

    // ── ①f 四类抛的错误串两两不同（可归因；「各抛各的」是规格要求，不是修辞）──────────
    const std::string msgFlat = edgeThrowMessage(cv::Mat::zeros(100, 200, CV_64FC1), proj, line);
    const std::string msgPlat
        = edgeThrowMessage(rampField(200, 100, pxEdge, 40.0), proj, line);  // 过渡宽 > 窗
    const std::string msgMultiProbe = edgeThrowMessage(ramp, proj, line);   // 正常 = 空串
    const bool distinct = !msgFlat.empty() && !msgPlat.empty() && !msgThin.empty()
        && msgFlat != msgPlat && msgFlat != msgThin && msgPlat != msgThin
        && msgPlat.find("平台窗退化") != std::string::npos
        && msgFlat.find("找不到梯度") != std::string::npos && msgMultiProbe.empty();
    c.chk("edge 守卫：常函数/平台窗退化/过渡过浅 三类错误串互不相同且各含本类点名（正常"
          "输入不误抛）",
        distinct, msgFlat.substr(0, 40) + " | " + msgPlat.substr(0, 40) + " | "
            + msgThin.substr(0, 40));

    // ── ② 斜坡变宽中点不动（量的是位置不是陡峭度）──────────────────────────────────
    const double s4 = edgePosition(rampField(200, 100, pxEdge, 4.0), proj, line, true);
    const double s16 = edgePosition(rampField(200, 100, pxEdge, 16.0), proj, line, true);
    c.chk("edge 变宽不动：w=4/8/16 px 三档读数一致 <1e-9（阈值挪进插值 = 读数随宽度漂）",
        std::abs(s4 - s) < 1e-9 && std::abs(s16 - s) < 1e-9,
        std::to_string(s4) + " / " + std::to_string(s) + " / " + std::to_string(s16));

    // ── ③ 全 0 / 全 1 / 多过零 三守卫（多过零必须抛，不许返回第一个）──────────────────
    const cv::Mat zero = cv::Mat::zeros(100, 200, CV_64FC1);
    cv::Mat one(100, 200, CV_64FC1, cv::Scalar(1.0));
    c.chk("edge 守卫：全 0（影没画出来）→ 抛且归因「整窗受光」（返回 NaN/0 会被读成读数）",
        edgeThrowMessage(zero, proj, line).find("整窗受光") != std::string::npos);
    c.chk("edge 守卫：全 1（整窗本影）→ 抛且归因「整窗本影」",
        edgeThrowMessage(one, proj, line).find("整窗本影") != std::string::npos);
    // 双斜坡：上升沿在 x=0.03（0→1）、下降沿在 x=0.35（1→0）→ 相对阈值两个交叉。
    // 下降沿摆 0.35（不是 0.12）：下游平台区必须被 1 占多数，中位数才读得出 umbra=1
    // ——两沿重叠会让平台与过渡搅在一起，那是另一个（也该抛的）形态，别混用。
    double px2 = 0.0, py2 = 0.0;
    (void)projectPoint3(proj, { 0.35, 0.0, 4.0 }, px2, py2);
    cv::Mat two(100, 200, CV_64FC1);
    const double wRamp = 8.0;
    for (int y = 0; y < 100; ++y) {
        auto* row = two.ptr<double>(y);
        for (int x = 0; x < 200; ++x) {
            // 与 rampField 同口径：像素中心 = 索引 + 0.5（边缘约定），见 rampField 头注。
            const double up = std::min(1.0, std::max(0.0, ((x + 0.5) - (pxEdge - wRamp / 2)) / wRamp));
            const double down
                = std::min(1.0, std::max(0.0, ((px2 + wRamp / 2) - (x + 0.5)) / wRamp));
            row[x] = std::min(up, down);
        }
    }
    const std::string msgMulti = edgeThrowMessage(two, proj, line);
    c.chk("edge 守卫：多个 50% 过零点 → 抛（返回第一个 = 静默把接触缘当远影缘）",
        msgMulti.find("个 50% 过零点") != std::string::npos, msgMulti.substr(0, 80));
    c.chk("edge 守卫：多过零的错误串**点名每个过零位置**（可归因）",
        msgMulti.find("过零点") != std::string::npos && msgMulti.find("mm") != std::string::npos,
        msgMulti.substr(0, 80));

    // ── ④ 粗模式（subpixel=false）落在 ±step/2，且与插值分支确有差别 ──────────────────
    const double coarse = edgePosition(ramp, proj, line, false);
    c.chk("edge 粗模式：subpixel=false 落在 ±step/2 内（分支同码 = 对照失效）",
        std::abs(coarse - sTrue) <= 0.001 / 2.0 + 1e-12 && coarse != s,
        "coarse=" + std::to_string(coarse));

    // ── ⑤ worldPerpMm 闭式：mm/px = 2·tan(fov/2)·aspect·depth/width ────────────────────
    // 期望 = 2·tan(20°)·2·6/200·1000 = 43.676428… mm/px（手算抄录；depth=6 是相机平面到
    // z=4 扫描线的视深）。depth 写成常数（不随 p 变）的变异在这里红。
    const double mmPerPx = worldPerpMm(proj, { 0.0, 0.0, 4.0 }, { 1.0, 0.0, 0.0 }, 1.0);
    const double want = 2.0 * std::tan(20.0 * 3.14159265358979323846 / 180.0) * 2.0 * 6.0 / 200.0
        * 1000.0;
    c.chk("edge 换算：worldPerpMm 闭式对拍（2·tan(fov/2)·aspect·depth/width，相对 <1e-6）",
        std::abs(mmPerPx - want) / want < 1e-6, "got " + std::to_string(mmPerPx) + " want " + std::to_string(want));
    // z=6（不是 8：相机高 1 水平看，地面 z=8 已在屏外——操作子拒得对，测试点得在屏内）
    const double mmPerPx2 = worldPerpMm(proj, { 0.0, 0.0, 6.0 }, { 1.0, 0.0, 0.0 }, 1.0);
    c.chk("edge 换算：depth 随点变（z=6 的刻度 ≠ z=4——写死深度的变异在这里红）",
        mmPerPx2 < mmPerPx * 0.75, std::to_string(mmPerPx2) + " vs " + std::to_string(mmPerPx));
    c.chk("edge 换算守卫：零方向 / 相机背后的点 → 抛",
        throwsStd([&] { (void)worldPerpMm(proj, { 0.0, 0.0, 4.0 }, { 0.0, 0.0, 0.0 }, 1.0); })
            && throwsStd([&] {
                   (void)worldPerpMm(proj, { 0.0, 0.0, 11.0 }, { 1.0, 0.0, 0.0 }, 1.0);
               }));

    // ── ⑥ 级联公式链闭式（拆出 = 认知复杂度护栏；期望值推导全在 helper 头注）────────
    edgeCascadeClosedFormChecks(c);

    // ── 剖面守卫（分母塌了不许出数）─────────────────────────────────────────────────
    c.chk("edge 剖面守卫：cov 非 CV_64FC1 / vOffsets 空 / step ≤ 0 / from==to → 各抛",
        throwsStd([&] {
            (void)sampleCovProfile(cv::Mat::zeros(100, 200, CV_8UC1), proj, line);
        }) && throwsStd([&] {
            auto l = line;
            l.vOffsets.clear();
            (void)sampleCovProfile(ramp, proj, l);
        }) && throwsStd([&] {
            auto l = line;
            l.stepM = 0.0;
            (void)sampleCovProfile(ramp, proj, l);
        }) && throwsStd([&] { (void)sampleCovProfile(ramp, proj, edgeTestLine({ -0.2, 0.0, 4.0 }, { -0.2, 0.0, 4.0 })); }));
    c.chk("edge 剖面守卫：整条线投到相机背后（有效样本塌光）→ 抛，不返回空剖面",
        throwsStd([&] {
            (void)sampleCovProfile(ramp, proj, edgeTestLine({ -0.2, 0.0, 10.5 }, { 0.2, 0.0, 10.5 }));
        }));

    // ── edgeWorldAt 往返 ──────────────────────────────────────────────────────────────
    const auto w = edgeWorldAt(line, sTrue);
    c.chk("edge 往返：edgeWorldAt(sTrue) 落回真值影边（<1e-12）",
        std::abs(w[0] - kEdgeTestX0) < 1e-12 && std::abs(w[2] - 4.0) < 1e-12);
}

/// 闸门、缝宽纯函数与挂号面（缝 = 「输入 → 读数」的闭式数值 vs 「输入不合法 ⟹ 抛」）。
void testShadowEdgeRulerGuards(Checker& c)
{
    // ── 闸门 C｜每块板必须真的量到过（意外失踪，带抛因）────────────────────────────
    c.chk("edge 闸门C：无缺失 + 臂/板计数正常 → 不抛（闸门恒抛 = 仪器永久红）",
        !throwsStd([&] { requireEveryPlateMeasured({}, 9, 8); }));
    std::string msgC;
    bool threwC = false;
    try {
        requireEveryPlateMeasured({ { "d5", "plate_t003", "过渡过浅" },
                                    { "f2d-1", "plate_t300", "平台窗退化" } },
            9, 8);
    } catch (const std::exception& e) {
        threwC = true;
        msgC = e.what();
    }
    c.chk("edge 闸门C：有缺失 → 抛并**一次报全**（逐条抛会把第二条藏到下一轮）", threwC,
        threwC ? "" : "「某板在某臂找不到过零点」被静默跳过 = 对空集合的断言恒真");
    c.chk("edge 闸门C：错误串点名两个缺失组合 + 各带算子抛因（可直接归因）",
        msgC.find("d5") != std::string::npos && msgC.find("plate_t003") != std::string::npos
            && msgC.find("f2d-1") != std::string::npos && msgC.find("plate_t300") != std::string::npos
            && msgC.find("过渡过浅") != std::string::npos
            && msgC.find("平台窗退化") != std::string::npos,
        msgC.substr(0, 120));
    c.chk("edge 闸门C：🔴 零臂 / 零板 → 抛（missing 天然为空 ⟹ 断言恒真 = 假绿，本仓 skill 明写）",
        throwsStd([&] { requireEveryPlateMeasured({}, 0, 8); })
            && throwsStd([&] { requireEveryPlateMeasured({}, 9, 0); }));

    // ── 闸门 D｜参照态自洽 ──────────────────────────────────────────────────────────
    c.chk("edge 闸门D：全部在界内 → 不抛",
        !throwsStd([&] {
            requireReferenceEdgeAtTruth({ { "f0d-1 × a", 1.0, 10.0 }, { "f0d5 × b", 9.99, 10.0 } });
        }));
    c.chk("edge 闸门D：恰等于界算通过（`>=` 弱化合规变异在这里红——含端点是口径）",
        !throwsStd([&] { requireReferenceEdgeAtTruth({ { "f0d-1 × a", 10.0, 10.0 } }); }));
    std::string msgD;
    bool threwD = false;
    try {
        requireReferenceEdgeAtTruth({ { "f0d-1 × a", 10.0, 10.0 }, { "f0d5 × b", 10.01, 10.0 } });
    } catch (const std::exception& e) {
        threwD = true;
        msgD = e.what();
    }
    c.chk("edge 闸门D：超界 → 抛且点名（比较反向的变异在这里红）", threwD
            && msgD.find("f0d5 × b") != std::string::npos
            && msgD.find("f0d-1 × a") == std::string::npos,  // 在界内的不许被误指
        msgD.substr(0, 120));
    c.chk("edge 闸门D：错误串说清「靶或换算错了，不是阴影错了」（归因方向）",
        msgD.find("靶或换算错了") != std::string::npos, msgD.substr(0, 120));
    c.chk("edge 闸门D：空清单 → 抛（恒真断言）",
        throwsStd([&] { requireReferenceEdgeAtTruth({}); }));

    // ── ⑩ seamWidths 纯函数（J2/J3 的共同取数面；E2 改版后「能判的闭式」就在这）──────
    const double nan = kNaN;
    const auto mkPinned = [](std::vector<std::vector<double>> d5ByCascade) {
        // d5ByCascade = [c][板] = 出厂 D=5 列（缝宽取数列）；D=-1 列填 0 占位（本测试不碰）。
        EdgeReadout r;
        const size_t plates = d5ByCascade[0].size();
        for (size_t p = 0; p < plates; ++p) {
            r.plateNames.push_back("p" + std::to_string(p));
            r.texelMm.push_back(10.0);
        }
        for (auto& d5 : d5ByCascade)
            r.errPinned.push_back({ d5, std::vector<double>(plates, 0.0) });
        for (int cc = 0; cc < 4; ++cc)
            r.texelMmByCascade[cc] = 10.0;
        return r;
    };
    // 两板四臂 D=5 误差：p0 = (5, 15, NaN, NaN)，p1 = (0, 0, 0, 0)。
    const auto r1 = mkPinned({ { 5.0, 0.0 }, { 15.0, 0.0 }, { nan, 0.0 }, { nan, 0.0 } });
    const auto seams1 = seamWidths(r1);
    c.chk("edge 缝宽：|15−5|=10 逐板给；任一侧 NaN → NaN（「没量到」不许读成「缝为 0」）",
        std::abs(seams1[0][0] - 10.0) < 1e-12 && std::abs(seams1[0][1]) < 1e-12
            && std::isnan(seams1[1][0]) && std::isnan(seams1[2][0]));
    // 全局翻号（各级误差同时取反）= 同一把尺倒着放，缝宽必须逐格不变。
    const auto rFlip = mkPinned({ { -5.0, 0.0 }, { -15.0, 0.0 }, { nan, 0.0 }, { nan, 0.0 } });
    const auto seamsFlip = seamWidths(rFlip);
    c.chk("edge 缝宽：全局翻号读数不变（先减后取绝对值；带符号做差的变异在这红）",
        std::abs(seamsFlip[0][0] - seams1[0][0]) < 1e-12);
    c.chk("edge 缝宽：空板清单 → 抛（对空集合的「每交界缝宽都达标」恒真 = 假绿）",
        throwsStd([&] { (void)seamWidths(EdgeReadout {}); }));
    c.chk("edge 缝宽：形状不对（级数 ≠ 4）→ 抛（内部错误不静默）",
        throwsStd([&] {
            EdgeReadout bad;
            bad.plateNames = { "p0" };
            bad.errPinned.resize(3);
            (void)seamWidths(bad);
        }));
    {
        // 某交界两侧全无可量板 = 缝宽缺角 ⟹ printSeamWidthInfo 抛（打印面用
        // captureStdout 吞掉，自检不往终端吐表）。
        const auto rAllNan = mkPinned({ { 1.0 }, { 2.0 }, { nan }, { nan } });
        std::string thrown;
        try {
            (void)captureStdout([&] { printSeamWidthInfo(rAllNan); });
        } catch (const std::exception& e) {
            thrown = e.what();
        }
        c.chk("edge 缝宽：某交界无任何可量板 → 抛（「缝宽不许缺角」，静默少一条交界 = 输出"
              "看起来完整 = 假绿）",
            thrown.find("没有任何一块板") != std::string::npos, thrown.substr(0, 80));
    }

    // ── 三条判决线必须挂号（改名漏挂 = verdictFor 运行期才炸，太晚）──────────────────
    // 🔴 J3 已改名 shadow-edge-cascade-agreement（E2：「随 D 单调」在本靶前提不存在）；
    //    旧名 shadow-edge-monotone 若复发，这条与注册表都会说话。
    for (const char* name : { "shadow-edge-absolute-error", "shadow-edge-seam-width",
             "shadow-edge-cascade-agreement" }) {
        bool found = false;
        for (const auto& j : registeredJudgments())
            found = found || std::string(j.name) == name;
        c.chk("edge 判据已挂号进注册表", found, name);
    }
}

}  // namespace RGL::imgqa
