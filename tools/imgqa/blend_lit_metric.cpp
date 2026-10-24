// blend_lit_metric.cpp —— `blend-lit` 的口径层实现（纯算式，不启引擎、不读文件）。
//
// 每条前提检查都是**抛**而不是返回 false：本层的调用方是仪器，"这次测量没有意义"与
// "测量结果为假"必须走不同的出口。把拒跑混进 PASS/FAIL 里，现场就会去查一个根本不存在的
// 渲染 bug（同 `blend_sort_metric.cpp` 的三条拒跑口径）。
#include "blend_lit_metric.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace RGL::imgqa {
namespace {

/// 机位朝向必须是**精确**的 (0,0,−1)（允许 position 非零，但 target 必须正对 −Z）。
/// 用精确比较而不是 eps：这条前提的两种状态之间没有中间地带——forward 差 1e-9 世界单位
/// 在屏幕上是 1e-6 像素，而"是不是纯线性映射"是二值的。真歪了就是歪了，歪 1e-9 是巧合。
void requireForwardMinusZ(const BlendLitGeometry& g)
{
    const double fx = g.cameraTarget[0] - g.cameraPosition[0];
    const double fy = g.cameraTarget[1] - g.cameraPosition[1];
    const double fz = g.cameraTarget[2] - g.cameraPosition[2];
    if (fx != 0.0 || fy != 0.0 || !(fz < 0.0))
        throw std::runtime_error(
            "blend-lit: 机位 forward 不是精确的 (0,0,−1)——本算子的世界→屏幕映射是纯线性"
            "（不建 view 矩阵），前提就是无俯角无偏航。歪了的话 footprint 会框到别的几何上，"
            "而「双臂像素不同」照旧成立 = 一条会给出假绿的判据。拒跑。\n  position=("
            + std::to_string(g.cameraPosition[0]) + ", " + std::to_string(g.cameraPosition[1])
            + ", " + std::to_string(g.cameraPosition[2]) + ") target=("
            + std::to_string(g.cameraTarget[0]) + ", " + std::to_string(g.cameraTarget[1]) + ", "
            + std::to_string(g.cameraTarget[2]) + ")");
}

/// 某深度处的视锥半高（世界单位）。调用方保证 depth > 0。
double halfHeightAt(const BlendLitGeometry& g, double depth)
{
    return std::tan(g.fovDegrees * 0.5 * std::numbers::pi_v<double> / 180.0) * depth;
}

}  // namespace

std::vector<BlendLitBox> blendLitBoxes(const BlendLitGeometry& g)
{
    requireForwardMinusZ(g);
    if (g.width <= 0 || g.height <= 0)
        throw std::runtime_error("blend-lit: 画幅非正（" + std::to_string(g.width) + "x"
            + std::to_string(g.height) + "）——投影算子的分母就是它，拒跑");
    if (g.paneXs.empty())
        throw std::runtime_error("blend-lit: manifest 的 paneXs 是空表——没有 pane 就没有"
            "footprint，两条像素判据都无从求值。拒跑（这不是「反向用例」，反向用例是 OPAQUE 臂）");
    const double depth = g.cameraPosition[2] - g.paneZ;
    if (!(depth > 0.0))
        throw std::runtime_error("blend-lit: pane 平面 z=" + std::to_string(g.paneZ)
            + " 不在机位之前（depth=" + std::to_string(depth) + "），拒跑");
    const double pxPerUnit = (g.height / 2.0) / halfHeightAt(g, depth);
    const double half = g.paneSize / 2.0;

    std::vector<BlendLitBox> out;
    out.reserve(g.paneXs.size());
    for (const double cx : g.paneXs) {
        // 向外取整：x0 = floor、x1 = ceil。宁可多圈一像素，也不让覆盖边界漏进
        // "应当逐位相同"那一侧（漏过去 = 把一条真判据判成红，然后有人来加容差）。
        BlendLitBox b;
        b.x0 = static_cast<int>(std::floor((cx - half - g.cameraPosition[0]) * pxPerUnit
            + g.width / 2.0));
        b.x1 = static_cast<int>(std::ceil((cx + half - g.cameraPosition[0]) * pxPerUnit
            + g.width / 2.0));
        // y 轴向下：世界 +y = 屏幕向上 ⟹ 上边界取 worldY = +half。pane 的 node y 恒 0。
        b.y0 = static_cast<int>(std::floor(g.height / 2.0 - half * pxPerUnit));
        b.y1 = static_cast<int>(std::ceil(g.height / 2.0 + half * pxPerUnit));
        out.push_back(b);
    }
    // 互不重叠是本靶的设计前提（"这块像素是谁画的"必须一一对应）。真重叠了不抛不行：
    // 两桶的划分本身没坏，但读数归因会含糊，而含糊的读数会被当成证据用。
    auto sorted = out;
    std::ranges::sort(sorted, [](const BlendLitBox& a, const BlendLitBox& b) { return a.x0 < b.x0; });
    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i].x0 <= sorted[i - 1].x1)
            throw std::runtime_error("blend-lit: 第 " + std::to_string(i - 1) + " 与第 "
                + std::to_string(i) + " 支 pane 的屏幕 footprint 重叠——本靶的区域归因要求"
                  "「一块像素只属于一支 pane」（多层叠加是 blend-sort 的射程）。改落位或改画幅，"
                  "别改这条检查");
    return out;
}

void requireBackdropCoverage(const BlendLitGeometry& g)
{
    requireForwardMinusZ(g);
    const double depth = g.cameraPosition[2] - g.backdropZ;
    if (!(depth > 0.0))
        throw std::runtime_error("blend-lit: 背景板 z=" + std::to_string(g.backdropZ)
            + " 不在机位之前（depth=" + std::to_string(depth) + "），拒跑");
    const double halfH = halfHeightAt(g, depth);
    const double halfW = halfH * (static_cast<double>(g.width) / g.height);
    if (!(g.backdropHalf > halfW) || !(g.backdropHalf > halfH))
        throw std::runtime_error(
            "blend-lit: 背景板铺不满整屏（半尺寸 " + std::to_string(g.backdropHalf)
            + " vs 该深度视锥半宽 " + std::to_string(halfW) + " / 半高 " + std::to_string(halfH)
            + "）——「footprint 之外只有背景板」这条归因不成立，`blend-lit-confined` 会在测"
              "别的东西。改 qa_scenes.py 的 BLEND_LIT_BACK，别改这条检查");
}

bool blendLitInsideAny(
    const std::vector<BlendLitBox>& boxes, int x, int y, const int pad)
{
    return std::ranges::any_of(boxes,
        [&](const BlendLitBox& b) { return x >= b.x0 - pad && x <= b.x1 + pad && y >= b.y0 - pad
                                              && y <= b.y1 + pad; });
}

std::array<int, 3> blendLitDominant(const cv::Mat& bgr,
    const std::vector<BlendLitBox>& boxes, const bool inside, const int pad)
{
    if (bgr.channels() < 3)
        throw std::runtime_error("blend-lit: 图像不足 3 通道，众数颜色无从可取，拒跑");
    // key = R<<16 | G<<8 | B。用整型键而不是 cv::Vec3b 做 map 键：后者没有 std::hash，
    // 而为一枚键去引第三方 hash 头不值。
    std::map<int, long long> hist;
    for (int y = 0; y < bgr.rows; ++y)
        for (int x = 0; x < bgr.cols; ++x) {
            if (blendLitInsideAny(boxes, x, y, pad) != inside)
                continue;
            const cv::Vec3b p = bgr.at<cv::Vec3b>(y, x);
            ++hist[(p[2] << 16) | (p[1] << 8) | p[0]];
        }
    if (hist.empty())
        throw std::runtime_error(std::string("blend-lit: ")
            + (inside ? "footprint 内" : "footprint 外")
            + "区域是空的——众数颜色无从可取。空区域返回一个默认色 = 把「区域算错了」"
              "伪装成一条正常读数，所以这里必须抛");
    const auto best = std::max_element(hist.begin(), hist.end(),
        [](const std::pair<int, long long>& a, const std::pair<int, long long>& b) {
            return a.second < b.second;
        });
    return {(best->first >> 16) & 0xFF, (best->first >> 8) & 0xFF, best->first & 0xFF};
}

int blendLitColorDelta(const std::array<int, 3>& a, const std::array<int, 3>& b)
{
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

BlendLitArmDiff blendLitCompareArms(
    const cv::Mat& blendArm, const cv::Mat& opaqueArm, const std::vector<BlendLitBox>& boxes,
    const int pad)
{
    if (blendArm.size() != opaqueArm.size() || blendArm.type() != opaqueArm.type())
        throw std::runtime_error("blend-lit: 两臂图像不是同尺寸同格式（"
            + std::to_string(blendArm.cols) + "x" + std::to_string(blendArm.rows) + "/"
            + std::to_string(blendArm.channels()) + " 通道 vs " + std::to_string(opaqueArm.cols)
            + "x" + std::to_string(opaqueArm.rows) + "/" + std::to_string(opaqueArm.channels())
            + " 通道）——「逐位相同」这句话在不同格式之间没有含义。拒跑");
    if (blendArm.channels() < 3)
        throw std::runtime_error("blend-lit: 图像不足 3 通道（读到 "
            + std::to_string(blendArm.channels()) + " 通道），拒跑");

    BlendLitArmDiff d;
    for (int y = 0; y < blendArm.rows; ++y) {
        const bool insideRow = std::ranges::any_of(boxes,
            [&](const BlendLitBox& b) { return y >= b.y0 - pad && y <= b.y1 + pad; });
        for (int x = 0; x < blendArm.cols; ++x) {
            const cv::Vec3b a = blendArm.at<cv::Vec3b>(y, x);
            const cv::Vec3b b = opaqueArm.at<cv::Vec3b>(y, x);
            if (a == b)
                continue;
            const int lsb = std::max({std::abs(static_cast<int>(a[0]) - b[0]),
                std::abs(static_cast<int>(a[1]) - b[1]), std::abs(static_cast<int>(a[2]) - b[2])});
            // 逐像素的桶判定走 blendLitInsideAny（同一份口径，行级预筛只是它的快速路径；
            // 两者若哪天不一致，selftest 的边界用例先红）。
            if (insideRow && blendLitInsideAny(boxes, x, y, pad)) {
                ++d.changedInside;
                d.maxLsbInside = std::max(d.maxLsbInside, lsb);
            } else {
                ++d.changedOutside;
                d.maxLsbOutside = std::max(d.maxLsbOutside, lsb);
            }
        }
    }
    return d;
}

}  // namespace RGL::imgqa
