// selftest_blend_lit.cpp —— `blend-lit` 的**口径层**自检（债-134 先决）。
//
// 全部**不启引擎、不读文件**：合成几何 + 合成两臂小图直接打 `blend_lit_metric.cpp` 的算子，
// 期望值全部来自闭式或已实测的读数。这一层是本任务的重点——没有它，`blend-lit` 这条检查在
// `just build-tools` 里等于零覆盖（只有插上设备跑一次才可能被看见），而"编译得过、一条都没
// 断言"恰恰是本项目反复红过的半吊子形态。
//
// 六组断言各钉一件事（逐条可红，反向用例写在各组末尾）：
//   ① 投影锚点 —— 三支 pane 的屏幕框必须命中实机像素边界（整条检查能拿解析坐标划区域的
//      前提；投影与引擎不一致时两条像素判据会一起红，而红得像是渲染坏了）
//   ② 分桶正解 —— 只在 footprint 内不同的合成两臂 → inside 计数与 maxLsb **恰好**命中
//   ③ 分桶反例 —— 只在 footprint 外不同 → inside 必须恰为 0（这条是"confined 判据不是
//      恒绿的摆设"的直接证据，缺它则 ② 可以由一个恒返回 inside 的实现蒙过）
//   ④ pad 边界 —— box 边界外第 1 px 算 inside、第 2 px 算 outside（pad 的全部理由就是
//      覆盖抗锯齿边缘带，口径漂 1 px 就会把真判据判成红）
//   ⑤ 四条拒跑 —— 机位不是精确 −Z 正视 / paneXs 空 / footprint 重叠 / 两臂尺寸不同，都必须抛
//   ⑥ 挂号 —— 六条判据名必须已在注册表里（改名漏挂号 = verdictFor 运行期才炸，太晚）
//   ⑦ 同臂众数算子 —— 判据 6 的口径本体：pane 与背景板同色时差**恰为 0**（= 删掉透明段时
//      的形态，判据 6 会红的直接证据），不同色时差一个量级；空区域必须抛
//
// 七组分两个函数：①⑤⑥ 在 `testBlendLit`（吃合成**几何**），②③④⑦ 在
// `testBlendLitBuckets`（吃合成**图像**）。缝 = 被测算子的输入类型，理由写在后者的头注里。
#include "selftest.h"

#include "blend_lit_metric.h"
#include "imgqa.h"

#include <array>
#include <string>
#include <vector>

namespace RGL::imgqa {

namespace {

/// 与 `qa_scenes.py` 的 scene_blend_lit 同参数的合成几何（**不读 manifest**：本层要能在没有
/// build/rgl/models/ 的机器上跑，而读来的数只能证明"与生成器一样"，证不了算子自己对）。
BlendLitGeometry target()
{
    BlendLitGeometry g;
    g.cameraPosition = {0.0, 0.0, 6.0};
    g.cameraTarget = {0.0, 0.0, 0.0};
    g.fovDegrees = 45.0;
    g.paneZ = -1.0;
    g.paneSize = 2.0;
    g.paneXs = {-3.0, 0.0, 3.0};
    g.backdropHalf = 16.0;
    g.backdropZ = -2.0;
    g.width = 1280;
    g.height = 720;
    return g;
}

/// 合成一臂图像：全白底，`rects` 里的每个框填成 `fill`。
cv::Mat makeArm(int w, int h, const std::vector<std::array<int, 4>>& rects,
    const cv::Vec3b& fill)
{
    cv::Mat m(h, w, CV_8UC3, cv::Scalar(255, 255, 255));
    for (const auto& r : rects)
        for (int y = r[1]; y <= r[3]; ++y)
            for (int x = r[0]; x <= r[2]; ++x)
                m.at<cv::Vec3b>(y, x) = fill;
    return m;
}

int boxArea(const BlendLitBox& b)
{
    return (b.x1 - b.x0 + 1) * (b.y1 - b.y0 + 1);
}

}  // namespace

void testBlendLit(Checker& c)
{
    const BlendLitGeometry g = target();

    // ── ① 投影锚点 ────────────────────────────────────────────────────────────────
    // 期望值 = 闭式：depth=7、halfH=tan(22.5°)·7、pxPerUnit=360/halfH=124.1595546363306，
    // x0=floor((cx−1)·px+640)、x1=ceil((cx+1)·px+640)、y0=floor(360−px)、y1=ceil(360+px)。
    // 同时也是**实机截图上 pane 的真实边界**（本机 2026-10-24 双臂差分实测：changed 像素的
    // x 跨度 143…1136、y 跨度 236…483，与下面三个框的并集一致）。
    const std::vector<BlendLitBox> boxes = blendLitBoxes(g);
    c.chk("blend-lit 投影：footprint 数 == 3", boxes.size() == 3, std::to_string(boxes.size()));
    const std::array<std::array<int, 4>, 3> want{{
        {143, 235, 392, 485}, {515, 235, 765, 485}, {888, 235, 1137, 485},
    }};
    for (size_t i = 0; i < boxes.size() && i < want.size(); ++i) {
        const std::array<int, 4> got{boxes[i].x0, boxes[i].y0, boxes[i].x1, boxes[i].y1};
        c.chk(("blend-lit 投影锚点 pane[" + std::to_string(i) + "]").c_str(), got == want[i],
            std::to_string(got[0]) + "," + std::to_string(got[1]) + "," + std::to_string(got[2])
                + "," + std::to_string(got[3]) + " != " + std::to_string(want[i][0]) + ","
                + std::to_string(want[i][1]) + "," + std::to_string(want[i][2]) + ","
                + std::to_string(want[i][3]));
    }
    // 阳性锚点的反面：把 paneZ 挪 1 个世界单位，边界必须跟着变（否则投影算子根本没吃 z，
    // 而"深度参与投影"这件事在 ① 里会被一个恒定的错误蒙过去）。
    {
        BlendLitGeometry moved = g;
        moved.paneZ = 0.0;
        const auto mb = blendLitBoxes(moved);
        c.chk("blend-lit 投影：paneZ 变了边界必须跟着变", !mb.empty() && !boxes.empty()
                && mb[0].x0 != boxes[0].x0,
            mb.empty() ? "空" : std::to_string(mb[0].x0));
    }

    // ── ⑤ 四条拒跑（前提不成立必须抛，不许静默给数）─────────────────────────────
    // ⚠ 每个 lambda 里都把返回值 `(void)` 掉：那三个算子是 `[[nodiscard]]`，而"抛没抛"才是
    //    本组唯一关心的事——不写 (void) 会撞 -Wunused-result -Werror（编译不过），
    //    写错成 `auto x =` 又会招来 unused-variable。
    {
        BlendLitGeometry tilted = g;
        tilted.cameraTarget = {1.0, 0.0, 0.0};  // forward 不再是精确 (0,0,−1)
        c.chk("blend-lit 拒跑：机位不是精确 −Z 正视必须抛",
            throwsStd([&] { (void)blendLitBoxes(tilted); }));
        BlendLitGeometry none = g;
        none.paneXs.clear();
        c.chk("blend-lit 拒跑：paneXs 为空必须抛", throwsStd([&] { (void)blendLitBoxes(none); }));
        BlendLitGeometry overlap = g;
        overlap.paneXs = {-1.0, 1.0};  // 2.0 宽、间距 2.0 ⟹ 边界相接 = 重叠
        c.chk("blend-lit 拒跑：footprint 重叠必须抛", throwsStd([&] { (void)blendLitBoxes(overlap); }));
        BlendLitGeometry tiny = g;
        tiny.backdropHalf = 5.0;  // 该深度视锥半宽 5.89 > 5 ⟹ 铺不满
        c.chk("blend-lit 拒跑：背景板铺不满必须抛", throwsStd([&] { requireBackdropCoverage(tiny); }));
        c.chk("blend-lit 拒跑：真实几何的铺满对账必须通过",
            !throwsStd([&] { requireBackdropCoverage(g); }));
        const std::vector<BlendLitBox> one{BlendLitBox{0, 0, 1, 1}};
        const cv::Mat big = makeArm(8, 8, {}, cv::Vec3b(0, 0, 0));
        const cv::Mat small = makeArm(4, 4, {}, cv::Vec3b(0, 0, 0));
        c.chk("blend-lit 拒跑：两臂尺寸不同必须抛",
            throwsStd([&] { (void)blendLitCompareArms(big, small, one); }));
    }

    // ── ⑥ 挂号 ────────────────────────────────────────────────────────────────────
    {
        const auto& all = registeredJudgments();
        for (const char* name : {"blend-lit-draws", "blend-lit-reverse", "blend-lit-contributes",
                 "blend-lit-confined", "blend-lit-visible", "blend-lit-clean"}) {
            const bool found = std::any_of(all.begin(), all.end(),
                [&](const JudgmentInfo& j) { return std::string(j.name) == name; });
            c.chk((std::string("blend-lit 判据已挂号: ") + name).c_str(), found);
        }
    }
}


// ── 像素分桶算子的自检（②③④⑦ 四组）─────────────────────────────────────────────
//
// 为什么与 `testBlendLit` 分成两个函数（缝 = **几何/投影** vs **像素分类**，不是行数切）：
// 上面那组吃的是合成**几何**（blendLitBoxes / requireBackdropCoverage / 四条拒跑），本组吃的是
// 合成**图像**（blendLitCompareArms / blendLitInsideAny / blendLitDominant）。改区域定义动上面，
// 改分桶与统计口径动这里；两组合起来 128 代码行撞「单函数 < 120」的护栏，而这条缝本来就在那儿。
void testBlendLitBuckets(Checker& c)
{
    const BlendLitGeometry g = target();
    const std::vector<BlendLitBox> boxes = blendLitBoxes(g);

    // ── ② 分桶正解 ────────────────────────────────────────────────────────────────
    // 两臂：BLEND 臂在三个 footprint 内填红，OPAQUE 臂全白。
    {
        std::vector<std::array<int, 4>> rects;
        for (const auto& b : boxes)
            rects.push_back({b.x0, b.y0, b.x1, b.y1});
        const cv::Mat a = makeArm(g.width, g.height, rects, cv::Vec3b(0, 0, 255));
        const cv::Mat o = makeArm(g.width, g.height, {}, cv::Vec3b(0, 0, 0));
        const BlendLitArmDiff d = blendLitCompareArms(a, o, boxes);
        // 期望 = 三个框各自面积之和。三个框的**宽度不相等**（143…392 是 250 列、515…765 是
        // 251 列 —— 向外取整在三个不同的亚像素相位上各进一次位），所以这里必须逐框累加，
        // 写成 `3 * boxArea(boxes[0])` 会差 251（本条第一版就是这么错的，被自己红了出来）。
        long long area = 0;
        for (const auto& b : boxes)
            area += boxArea(b);
        c.chk("blend-lit 分桶：footprint 内 changed == 三个框面积之和", d.changedInside == area,
            std::to_string(d.changedInside) + " != " + std::to_string(area));
        c.chk("blend-lit 分桶：footprint 外 changed == 0", d.changedOutside == 0,
            std::to_string(d.changedOutside));
        c.chk("blend-lit 分桶：maxLsbInside == 255", d.maxLsbInside == 255,
            std::to_string(d.maxLsbInside));
        c.chk("blend-lit 分桶：maxLsbOutside == 0", d.maxLsbOutside == 0,
            std::to_string(d.maxLsbOutside));
    }

    // ── ③ 分桶反例（阳性锚点的反面：confined 判据不是恒绿的摆设）─────────────────
    // 差异**只**放在三个框之外（左侧一条竖条 + 上方一条横条，都离框 ≥ 2 px）。
    {
        const std::vector<std::array<int, 4>> rects{
            {0, 0, 40, g.height - 1}, {0, 0, g.width - 1, 40},
        };
        const cv::Mat a = makeArm(g.width, g.height, rects, cv::Vec3b(0, 255, 0));
        const cv::Mat o = makeArm(g.width, g.height, {}, cv::Vec3b(0, 0, 0));
        const BlendLitArmDiff d = blendLitCompareArms(a, o, boxes);
        c.chk("blend-lit 分桶反例：差异在框外时 inside changed == 0", d.changedInside == 0,
            std::to_string(d.changedInside));
        c.chk("blend-lit 分桶反例：差异在框外时 outside changed > 0", d.changedOutside > 0,
            std::to_string(d.changedOutside));
    }

    // ── ④ pad 边界口径 ────────────────────────────────────────────────────────────
    {
        const BlendLitBox b{100, 100, 110, 110};
        const std::vector<BlendLitBox> one{b};
        c.chk("blend-lit pad：框内算 inside", blendLitInsideAny(one, 105, 105, 1));
        c.chk("blend-lit pad：边界外第 1 px 算 inside", blendLitInsideAny(one, 111, 100, 1));
        c.chk("blend-lit pad：边界外第 2 px 算 outside", !blendLitInsideAny(one, 112, 100, 1));
        c.chk("blend-lit pad：y 方向同样只扩 1 px",
            blendLitInsideAny(one, 105, 99, 1) && blendLitInsideAny(one, 105, 111, 1)
                && !blendLitInsideAny(one, 105, 98, 1) && !blendLitInsideAny(one, 105, 112, 1));
        // pad=0 时边界外第 1 px 必须翻成 outside —— 证明 pad 真的在参与判定，
        // 而不是一个写了却没读的常量（本项目「加了东西反而没人读」的常客）。
        c.chk("blend-lit pad：pad=0 时同一像素翻成 outside", !blendLitInsideAny(one, 111, 100, 0));
    }
    // ── ⑦ 同臂众数算子（判据 6 的口径本体：不启引擎钉它自己会红）─────────────────
    // 合成图：底 = 纯白（`makeArm` 的底色，代表背景板），框内填 Vec3b(0,0,255) = **纯红**
    // （cv::Vec3b 是 BGR，而 `blendLitDominant` 返回 **RGB**，两个口径别混）。
    //  · footprint 外的众数必须**恰为**白，之内必须**恰为**红 ⟹ 钉住返回值的通道次序；
    //  · 两者差 255（一个量级以上）⟹ 判据 6 在这种形态下绿；
    //  · 再合成一张「框内也是白」（= pane 什么都没写，正是删掉透明段时的形态）：
    //    两者差必须**恰为 0** ⟹ 判据 6 红。缺了这半条，判据 6 可以由一个恒返回大差的实现蒙过。
    {
        const std::array<int, 3> kWhite{255, 255, 255};
        const std::array<int, 3> kRed{255, 0, 0};
        std::vector<std::array<int, 4>> paintedRects{
            {boxes[0].x0, boxes[0].y0, boxes[0].x1, boxes[0].y1},
        };
        const cv::Mat painted = makeArm(g.width, g.height, paintedRects, cv::Vec3b(0, 0, 255));
        const auto bgPainted = blendLitDominant(painted, boxes, false);
        const std::vector<BlendLitBox> oneBox{boxes[0]};
        const auto panePainted = blendLitDominant(painted, oneBox, true);
        c.chk("blend-lit 众数：背景板区众数 == 白（RGB 次序）",
            bgPainted == kWhite,
            std::to_string(bgPainted[0]) + "," + std::to_string(bgPainted[1]) + ","
                + std::to_string(bgPainted[2]));
        c.chk("blend-lit 众数：pane 区众数 == 红（RGB 次序，不是 BGR）",
            panePainted == kRed,
            std::to_string(panePainted[0]) + "," + std::to_string(panePainted[1]) + ","
                + std::to_string(panePainted[2]));
        c.chk("blend-lit 众数：pane 与背景板不同色时差 >= 8 LSB",
            blendLitColorDelta(panePainted, bgPainted) >= kBlendLitPowerFloorLsb,
            std::to_string(blendLitColorDelta(panePainted, bgPainted)));
        // 反向：pane 与背景板**同色**（透明段整段为空的形态）⟹ 差必须恰为 0。
        const cv::Mat blank = makeArm(g.width, g.height, {}, cv::Vec3b(0, 0, 0));
        const auto paneBlank = blendLitDominant(blank, oneBox, true);
        const auto bgBlank = blendLitDominant(blank, boxes, false);
        c.chk("blend-lit 众数：pane 与背景板同色时差 == 0（判据 6 会红的直接证据）",
            blendLitColorDelta(paneBlank, bgBlank) == 0,
            std::to_string(blendLitColorDelta(paneBlank, bgBlank)));
        // 空区域必须抛（返回默认色 = 把「区域算错了」伪装成一条正常读数）。
        c.chk("blend-lit 众数：空区域必须抛",
            throwsStd([&] { (void)blendLitDominant(blank, boxes, true, -1000000); }));
    }

}

}  // namespace RGL::imgqa
