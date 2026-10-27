// selftest_swim_wall.cpp —— **垂直接收面**口径层（`shadow_wall_metric.cpp`）的自检。
//
// 全部闭式、不启引擎（判据：算子坏了读数也是假的，先钉算子）。这层是 `shadow-swim-wall`
// 那台仪器「门面由哪一级级联服务」的唯一读数出处 ⟹ 这里红，J-W1/J-W3 的数一个都不作数。
//
// ## 这层要钉住的是一件很窄的事：**「逐位相同 ⟺ 取该级」的读法**
// 四张钉级图当四把尺，自然臂与哪把尺逐位相同就读成哪一级。读法本身没有阈值、没有拟合，
// 所以自检的全部力气放在三件事上：
//   ① **无信息 ≠ 级 0**（四把尺全同的像素必须整个剔掉，不能被读成「匹配了第一把」）；
//   ② 边界估计的方向（差异 ⊆ 近侧 ⟹ 取低级视深的 **max**，不是 min、不是区间中点）；
//   ③ 空读数必须**抛**（「什么都没看见」与「看见了且接缝在 d」不能同形）。
//
// ## 拆成五个测试函数的缝（按**问的东西**切，不是按行数）
//   · `testWallVotes`        ①②③④ 投票四态：唯一 / 多义 / 无信息 / 全不匹配
//   · `testWallSeamDepth`    ⑤⑥⑦⑧ 合成已知接缝 → 读回真值（± 一格）+ 方向 + 单调 + 单级
//   · `testWallSeamGuards`   ⑨⑩⑪⑫ 尺数不符 / 类型 / 空掩码 / 无信息 一律抛
//   · `testWallPlaneGeneric` ⑬⑭ 🔴 **反向对照**：同一套世界不变场投在水平面与竖直面上，
//                              两层重建都读恰 0 —— 「垂直接收面」不是新物理，别把结论
//                              记在「竖直」头上（任务书钦定的水平面反证）
//   · `testWallRegistration` ⑮ 六条判决线挂号
//
// ## 变异红线（哪个变异让我红；逐条实跑记录见交付说明）
//   · 把「唯一匹配」放宽成「第一张匹配的」→ ② 红（四把尺全同的像素被算成级 0），① 照绿
//     ——所以 ② 是这条变异**唯一**的捕手，它必须存在且必须独立于 ①。
//   · 把接缝视深的 `max` 改成 `min`（方向反）→ ⑥ 红（读回的是低级区最浅那一格，不是接缝）。
//   · 把边界估计**钉成恒 0** → ⑤⑥⑦ 一起红。任务书钦定的那条。
//   · 把单调性守卫删掉 → ⑧ 红（把两级的像素互换深度后仍报「单调」）。
//   · 把 `kNoInfo` 与 `kNone` 合成一个枚举值（第一版的错）→ ③④ 红 + ⑨ 红。
//   · 守卫改成返回空读数 → ⑨⑩⑪⑫ 红（空 = 「没有接缝」= 假绿）。
//   · 判据名拼错或漏 append → ⑮ 红。
#include "selftest.h"

#include "imgqa.h"
#include "plane_reconstruct.h"
#include "shadow_kernel_metric.h"  // kNaN（全家族同一个 NaN 约定，不另立）
#include "shadow_wall_metric.h"

#include <opencv2/core.hpp>

#include <cmath>
#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

constexpr int kWW = 64;
constexpr int kWH = 48;

/// 一张三色图，整幅填同一个字节（钉级图的语义只需要「相等/不相等」，不需要真实着色）。
cv::Mat flatBgr(uint8_t v)
{
    return cv::Mat(kWH, kWW, CV_8UC3, cv::Scalar(v, v, v));
}

/// 视深场：depth(y, x) = x（每列一格的分辨率 = 1.0 m，故意取整好让断言写得准）。
cv::Mat depthByColumn()
{
    cv::Mat d(kWH, kWW, CV_64FC1);
    for (int y = 0; y < kWH; ++y)
        for (int x = 0; x < kWW; ++x)
            d.at<double>(y, x) = static_cast<double>(x);
    return d;
}

cv::Mat fullMask()
{
    return cv::Mat(kWH, kWW, CV_8UC1, cv::Scalar(255));
}

/// 直接手搓一张「索引场」：x < split 记 lower 级、其余记 upper 级（vote 全 = 唯一）。
CascadeVoteMaps syntheticIndex(double split, int lower, int upper, const cv::Mat& depthAt)
{
    CascadeVoteMaps m;
    m.vote = cv::Mat(kWH, kWW, CV_8UC1, cv::Scalar(static_cast<uint8_t>(CascadeVote::kUnique)));
    m.index = cv::Mat(kWH, kWW, CV_8SC1, cv::Scalar(-1));
    for (int y = 0; y < kWH; ++y)
        for (int x = 0; x < kWW; ++x)
            m.index.at<int8_t>(y, x)
                = static_cast<int8_t>(depthAt.at<double>(y, x) > split ? upper : lower);
    return m;
}

}  // namespace

namespace {
const PlaneFrame& groundPlaneRef()
{
    return groundPlane();
}

/// 判据是否挂号。🔴 不新抄一份同名的：本文件与 selftest_{field_diff,material,swim_flat}.cpp
/// 各有一个文件内的同名 helper，那是**既有**重复（本批不动它们，避免把纯搬移混进验证批）；
/// 这里直接吃 `registeredJudgments()`，不再新增第四份定义面。
bool wallJudgmentRegistered(const std::string& name)
{
    for (const auto& j : registeredJudgments())
        if (j.name == name)
            return true;
    return false;
}
}  // namespace

namespace {

// ── `testWallVotes` 的四个用例：一个票态一个函数 ─────────────────────────────────────
// 缝 = `CascadeVote` 的四个枚举值。把它们写在一个函数里是「四态一起测」，实际测的是四件
// 互不相干的事（唯一读出 / 无信息剔除 / 多义剔分母 / 失配报警），而本文件头注的变异红线
// 明确写着 ② 是某个变异的**唯一**捕手——四态分家之后，「哪条用例为哪个变异存在」在函数名
// 上就读得出来，混在一起时只能靠注释。每个用例各自铺图、各自数像素，不共用循环：
// 共用一个「数某态像素」的 helper 会让四态同时坏在 helper 上，而四态恰恰要能各红各的。

/// ① 自然臂 == 第 2 把尺，而四把尺互不相同 ⟹ 每像素唯一读出级 2。
void voteCaseUnique(Checker& c)
{
    std::vector<cv::Mat> pins { flatBgr(10), flatBgr(20), flatBgr(40), flatBgr(50) };
    const auto m = cascadeVotes(pins[2], pins);
    size_t uniq = 0;
    size_t wrong = 0;
    for (int y = 0; y < kWH; ++y)
        for (int x = 0; x < kWW; ++x) {
            if (m.vote.at<uint8_t>(y, x) == static_cast<uint8_t>(CascadeVote::kUnique))
                ++uniq;
            if (m.index.at<int8_t>(y, x) != 2)
                ++wrong;
        }
    c.chk("votes·自然臂与第 2 把尺逐位相同且四尺互异 ⟹ 全图唯一读出级 2",
        uniq == static_cast<size_t>(kWW * kWH) && wrong == 0,
        "唯一 " + std::to_string(uniq) + "、错级 " + std::to_string(wrong));
}

/// ② 🔴 四把尺**全同**的像素必须是 kNoInfo，而不是「匹配了第一把」= 级 0。
///    这条是「把唯一匹配放宽成第一张匹配的」那个变异的**唯一**捕手（① 在那个变异下照绿）。
void voteCaseNoInfo(Checker& c)
{
    const cv::Mat a = flatBgr(10);
    std::vector<cv::Mat> pins { a, a, a, a };
    const auto m = cascadeVotes(a, pins);
    size_t info = 0;
    for (int y = 0; y < kWH; ++y)
        for (int x = 0; x < kWW; ++x)
            if (m.vote.at<uint8_t>(y, x) != static_cast<uint8_t>(CascadeVote::kNoInfo))
                ++info;
    c.chk("votes·四把尺全同 ⟹ 无信息（既不是票也不是失配），一个像素都不许计",
        info == 0, "被计票的像素 = " + std::to_string(info));
}

/// ③ 自然臂同时匹配两把尺（两级的图在这里给出同一个字节）⟹ 读不出索引，记 ambiguous。
void voteCaseAmbiguous(Checker& c)
{
    cv::Mat p0 = flatBgr(10);
    cv::Mat p1 = flatBgr(20);
    cv::Mat p2 = flatBgr(10);  // 与 p0 同 ⟹ 自然臂取 p0 时会同时匹配两把
    cv::Mat p3 = flatBgr(40);
    const auto m = cascadeVotes(p0, { p0, p1, p2, p3 });
    size_t amb = 0;
    for (int y = 0; y < kWH; ++y)
        for (int x = 0; x < kWW; ++x)
            if (m.vote.at<uint8_t>(y, x) == static_cast<uint8_t>(CascadeVote::kAmbiguous))
                ++amb;
    c.chk("votes·匹配 ≥2 把尺 ⟹ 记 ambiguous 并剔出分母（不硬判一级）",
        amb == static_cast<size_t>(kWW * kWH), "ambiguous = " + std::to_string(amb));
}

/// ④ 尺子有分歧而自然臂一把都不匹配 ⟹ kNone（结构性违规，仪器侧必须抛）。
void voteCaseNone(Checker& c)
{
    std::vector<cv::Mat> pins { flatBgr(10), flatBgr(20), flatBgr(40), flatBgr(50) };
    const auto m = cascadeVotes(flatBgr(99), pins);
    size_t none = 0;
    for (int y = 0; y < kWH; ++y)
        for (int x = 0; x < kWW; ++x)
            if (m.vote.at<uint8_t>(y, x) == static_cast<uint8_t>(CascadeVote::kNone))
                ++none;
    c.chk("votes·一把都不匹配 ⟹ kNone（不是 kNoInfo，两者下游待遇不同）",
        none == static_cast<size_t>(kWW * kWH), "kNone = " + std::to_string(none));
}

}  // namespace

void testWallVotes(Checker& c)
{
    voteCaseUnique(c);
    voteCaseNoInfo(c);
    voteCaseAmbiguous(c);
    voteCaseNone(c);
}

void testWallSeamDepth(Checker& c)
{
    const cv::Mat depth = depthByColumn();
    const cv::Mat mask = fullMask();

    // ⑤ 接缝在 split = 20（整数格）：低级 = 2 的最大视深必须恰是 20（下界，不是中点）
    {
        const auto r = cascadeIndexReadout(syntheticIndex(20.0, 2, 3, depth), mask, depth);
        c.chk("接缝视深 = 低级最大视深（split=20 ⟹ 读回 20，🔴 不是区间中点 32）",
            r.found && r.straddles && r.lower == 2 && r.upper == 3 && r.seamDepth == 20.0,
            "found=" + std::to_string(r.found) + " seam=" + std::to_string(r.seamDepth)
                + " lower=c" + std::to_string(r.lower) + " upper=c" + std::to_string(r.upper));
        c.chk("上界 = 高级最小视深（21）⟹ 格宽 1.0 m 当场可核对，容差不必靠注释自证",
            r.sameMinAbove == 21.0 && r.monotone && r.monotoneSlack == 1.0,
            "上界=" + std::to_string(r.sameMinAbove) + " 松弛=" + std::to_string(r.monotoneSlack));
    }
    // ⑥ 接缝落在两格之间（split = 20.5）：读回仍是低级侧的最大格 20（**只准从下方逼近**）
    {
        const auto r = cascadeIndexReadout(syntheticIndex(20.5, 2, 3, depth), mask, depth);
        c.chk("接缝在两格之间（split=20.5）⟹ 读回 20 = 下界，不许四舍五入到 21",
            r.seamDepth == 20.0 && r.sameMinAbove == 21.0,
            "seam=" + std::to_string(r.seamDepth) + " 上界=" + std::to_string(r.sameMinAbove));
    }
    // ⑦ 单级（门面被一级覆盖，= 本批实机结论）：straddles = false、found = true、不抛
    {
        auto m = syntheticIndex(20.0, 2, 3, depth);
        cv::Mat(depth.rows, depth.cols, CV_8SC1, cv::Scalar(2)).copyTo(m.index);
        const auto r = cascadeIndexReadout(m, mask, depth);
        c.chk("门面只有一级 ⟹ straddles=false 且 found=true（这是**结论**，不是失明）",
            r.found && !r.straddles && r.lower == -1 && r.hist[2] == m.index.total(),
            "straddles=" + std::to_string(r.straddles) + " hist2=" + std::to_string(r.hist[2]));
    }
    // ⑧ 🔴 单调性守卫：把两级的像素**互换**深度 ⟹ 必须读出不单调（守卫删掉就漏过）
    {
        auto m = syntheticIndex(20.0, 2, 3, depth);
        cv::Mat flipped(kWH, kWW, CV_8SC1, cv::Scalar(-1));
        for (int y = 0; y < kWH; ++y)
            for (int x = 0; x < kWW; ++x)
                flipped.at<int8_t>(y, x)
                    = depth.at<double>(y, x) > 20.0 ? int8_t { 2 } : int8_t { 3 };  // 反了
        m.index = flipped;
        const auto r = cascadeIndexReadout(m, mask, depth);
        c.chk("索引与深度**反序**（高级在近侧）⟹ monotone=false（守卫在场，不是恒真）",
            r.straddles && !r.monotone && r.monotoneSlack < 0.0,
            "monotone=" + std::to_string(r.monotone) + " 松弛=" + std::to_string(r.monotoneSlack));
    }
}

void testWallSeamGuards(Checker& c)
{
    const cv::Mat depth = depthByColumn();
    const cv::Mat mask = fullMask();
    const auto ok = syntheticIndex(20.0, 2, 3, depth);

    // ⑨ 尺数不是 4 ⟹ 抛（少一把就有一个级读不出来，多一把说明臂表与引擎级联数漂了）
    c.chk("cascadeVotes·尺数 3 必须抛（有一个级联索引永远读不出来）",
        throwsStd([&] {
            (void)cascadeVotes(flatBgr(0), { flatBgr(1), flatBgr(2), flatBgr(3) });
        }));
    c.chk("cascadeVotes·尺数 5 必须抛（臂表与引擎级联数漂了）",
        throwsStd([&] {
            (void)cascadeVotes(flatBgr(0),
                { flatBgr(1), flatBgr(2), flatBgr(3), flatBgr(4), flatBgr(5) });
        }));
    // ⑩ 空掩码 ⟹ 抛（分母是彩票）
    c.chk("cascadeIndexReadout·空掩码必须抛（不当「没有接缝」放行）",
        throwsStd([&] {
            (void)cascadeIndexReadout(ok, cv::Mat(), depth);
        }));
    // ⑪ 四把尺全同（informed = 0）⟹ 抛。「什么都没看见」与「门面只有一级」不能同形
    c.chk("cascadeIndexReadout·门面无信息（informed=0）必须抛，不许读成「接缝不在门面上」",
        throwsStd([&] {
            CascadeVoteMaps none;
            none.vote = cv::Mat(kWH, kWW, CV_8UC1,
                cv::Scalar(static_cast<uint8_t>(CascadeVote::kNoInfo)));
            none.index = cv::Mat(kWH, kWW, CV_8SC1, cv::Scalar(-1));
            (void)cascadeIndexReadout(none, mask, depth);
        }));
    // ⑫ 类型/尺寸守卫
    cv::Mat wrongType;
    depth.convertTo(wrongType, CV_32FC1);
    c.chk("cascadeIndexReadout·depthAt 非 CV_64FC1 必须抛",
        throwsStd([&] { (void)cascadeIndexReadout(ok, mask, wrongType); }));
    c.chk("cascadeIndexReadout·掩码与视深场尺寸不同必须抛（不是同一次采集）",
        throwsStd([&] {
            (void)cascadeIndexReadout(ok, cv::Mat(kWH + 2, kWW, CV_8UC1, cv::Scalar(255)), depth);
        }));
}

namespace {

/**
 * 一枚平面上的「像素 → 平面局部坐标 → 同机位投回像素」往返实测。
 *
 * 返回 {参与样本数, 最大偏差（像素）}。它只负责在给定平面上跑一遍重建链并**如实报数**，
 * 自己不判断任何事——「恒等必须成立」这个口径留在调用侧：换采样密度与换判据界是两种
 * 不同的改动原因，混在一起时分不清哪次改动该让哪条断言红。
 * 步长 3 只是采样密度（判据是偏差 < 1e-6，与步长无关）；射线打不中平面的像素整个跳过，
 * 它们不构成反例（背面/画外的像素本来就不在重建的定义域里）。
 */
std::pair<size_t, double> roundTripIdentity(const Projector& proj, const PlaneFrame& plane)
{
    double worst = 0.0;
    size_t samples = 0;
    for (int y = 0; y < kWH; y += 3)
        for (int x = 0; x < kWW; x += 3) {
            double u = 0.0;
            double v = 0.0;
            if (!planeHitDouble(proj, x, y, 500.0, plane, u, v))
                continue;
            PlaneSamples s;
            s.plane = plane;
            s.srcIndex = { static_cast<int>(y) * kWW + x };
            s.u = { u };
            s.v = { v };
            std::vector<double> px;
            std::vector<double> py;
            projectPlaneSamples(proj, s, 0.0, 0.0, px, py);
            if (px.size() != 1 || !std::isfinite(px[0]))
                continue;
            worst = std::max({ worst, std::abs(px[0] - x), std::abs(py[0] - y) });
            ++samples;
        }
    return { samples, worst };
}

}  // namespace

void testWallPlaneGeneric(Checker& c)
{
    // ⑬⑭ 🔴 反向对照（任务书钦定）：「垂直接收面」不是新物理。同一条像素↔平面重建路径
    // 在**水平面**与**竖直面**上做同一件事——像素 → 平面局部坐标 → 投回像素——必须同样
    // 落回原像素（±1e-6）。读不出恒等 = 重建层在竖直面上偷偷做了重采样，那时本批所有
    // 「竖直面上量到的残差」都要打上「可能是量具造的」的问号。
    // ⚠ 不能拿「同一个像素在两枚机位下的世界不变场」当对照：同一像素在两机位下是**两个世界
    // 点**，本来就该不同（本批第一版就是这样写的，报出 2496/2496 全差 —— 那是测试自己错了）。
    Engine::Camera cam;
    cam.position = { 6.0, 2.0, 6.0 };
    cam.target = { 0.0, 0.5, 0.0 };
    cam.fovDegrees = 40.0;
    const auto proj = makeProjector(cam, kWW, kWH);
    const PlaneFrame vertical = makePlaneFrame({ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 },
        { 0.0, 0.0, 1.0 });
    const PlaneFrame& horizontal = groundPlaneRef();
    const std::pair<const char*, const PlaneFrame*> cases[2]
        = { { "竖直面", &vertical }, { "水平面", &horizontal } };
    for (const auto& pair : cases) {
        const auto [samples, worst] = roundTripIdentity(proj, *pair.second);
        const std::string label = std::string(pair.first)
            + "·往返恒等：像素→平面→同机位像素 落回原像素（±1e-6，与水平面同一条路径）";
        c.chk(label.c_str(), samples >= 12 && worst < 1e-6,
            std::string(pair.first) + "：样本 " + std::to_string(samples) + "，最大偏差 "
                + std::to_string(worst));
    }
}

void testWallRegistration(Checker& c)
{
    // ⑮ 六条判决线挂号在场（容差理由与 guard 的**内容**由 testRegistry 通用强制）
    for (const char* name : { "shadow-swim-wall-index-unique",
             "shadow-swim-wall-index-monotone", "shadow-swim-wall-seam-intervention",
             "shadow-swim-wall-world-invariance", "shadow-swim-wall-determinism",
             "shadow-swim-wall-offset-sensitivity" })
        c.chk("挂号：shadow-swim-wall 六条判决线在注册表里", wallJudgmentRegistered(name), name);
}

}  // namespace RGL::imgqa
