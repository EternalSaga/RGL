// blend_sort_metric.cpp —— 透明合成次序的闭式算式（`blend-sort` 的口径层，实现
// `blend_sort_metric.h` 声明的那四件事：投影 / 排序键 / 合成 / 判别力）。
//
// 逐式对应的一次性参考实现 = 刀3 当时写在仓库外（/tmp）的那份 Python 判据脚本，本 TU 是它的
// C++ 落地；落地后那份脚本作废，不再是任何读数的出处（读数出处 = 本文件 + selftest 断言）。
// 它当时实测过：四个判据区**逐字节零误差**命中闭式，且把排序比较子
// `>` 改成 `<` 之后 overlap 区实测变 (142,115,142) = 正好等于本算子按错误次序给出的预测。
// 也就是说这套算式**两种次序都预测得中**，它不是一台差分噪声计。
//
// 三条「这次测量没有意义」的拒跑条件全部走 `std::runtime_error`，**不做成 PASS/FAIL**：
// 输出一条 PASS 是在撒谎（判据的成立前提本身没了），抛到 main 的 catch → rc=3。
// 消息一律点名"怎么修"。
#include "blend_sort_metric.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

namespace RGL::imgqa {
namespace {

using Vec3d = std::array<double, 3>;

/// 投影常数（由相机与画幅解出来的量，见 `makeView`）。
struct BlendView {
    double halfTan = 0.0;
    double aspect = 1.0;
    int width = 0;
    int height = 0;
};

/// 面片的世界矩形（局部 aabb 的 x/y 跨度 + node 平移）。
struct WorldRect {
    double x0 = 0.0, x1 = 0.0, y0 = 0.0, y1 = 0.0;
};

/// 面片的屏幕矩形（浮点像素边界；覆盖判定见 `covers`，左闭右开）。
struct PixelRect {
    double x0 = 0.0, x1 = 0.0, y0 = 0.0, y1 = 0.0;
};

std::string label(const BlendPiece& p)
{
    return p.name.empty() ? std::string("<未命名面片>") : p.name;
}

Vec3d operator-(const Vec3d& a, const Vec3d& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

std::string vecText(const Vec3d& v)
{
    return "(" + std::to_string(v[0]) + ", " + std::to_string(v[1]) + ", "
        + std::to_string(v[2]) + ")";
}

/// 投影常数。构造它 = 顺带守住「沿 −Z 精确正视」这条前提（理由 → 头文件适用范围段）。
BlendView makeView(const BlendCamera& cam, int width, int height)
{
    const Vec3d fwd = cam.target - cam.position;
    if (fwd[2] >= 0.0 || std::abs(fwd[0]) > kBlendEps || std::abs(fwd[1]) > kBlendEps)
        throw std::runtime_error(
            "本判据只覆盖「沿 −Z 精确正视」的机位，实得 forward = " + vecText(fwd)
                + "\n  采样点的像素坐标是按「世界 x/y → 屏幕 px 是纯线性映射」解析算的，带俯角或"
                  "偏航就得建 view 矩阵。要修的是那份测试输入：tools/qa_scenes.py 的 "
                  "BLEND_SORT_CAM 与 qa/configs/blend_sort/camera.lua 一起改回纯 −Z 正视"
                  "（两处必须同时改，本命令另有一道相机对账闸门）。");
    if (width <= 0 || height <= 0)
        throw std::runtime_error("画幅必须为正，实得 " + std::to_string(width) + "x"
            + std::to_string(height) + "（截图尺寸与 manifest 的 closedForm.aspect 对不上？"
              "先确认跑的是 qa/configs/blend_sort 那份 runtime.lua 的 headless_size / llm_shot.size）");
    const double halfTan = std::tan(std::numbers::pi * cam.fovDegrees / 180.0 / 2.0);
    return {halfTan, static_cast<double>(width) / static_cast<double>(height), width, height};
}

/// 局部 aabb → 世界矩形。顺带钉住「片在局部 z=0 的平面上」这条前提：本算子把整片解成
/// 同一个 depth，片一旦有 z 厚度，footprint 就不是这两条边了（而读数看起来完全正常）。
WorldRect worldRect(const BlendPiece& p)
{
    if (std::abs(p.aabbMin[2]) > kBlendEps || std::abs(p.aabbMax[2]) > kBlendEps)
        throw std::runtime_error("面片 " + label(p) + " 的局部 z 不是 0（实得 ["
            + std::to_string(p.aabbMin[2]) + ", " + std::to_string(p.aabbMax[2])
            + "]）——本闭式把整片解成同一 depth，有厚度的片 footprint 解不出来。"
              "测试输入请用平面片（tools/qa_scenes.py 的 _xy_quad）");
    return {p.aabbMin[0] + p.translation[0], p.aabbMax[0] + p.translation[0],
        p.aabbMin[1] + p.translation[1], p.aabbMax[1] + p.translation[1]};
}

/// 竖直 fov 的针孔相机（aspect 只压横轴），屏幕 y 向下 ⟹ 世界 y1 落在上边界。
PixelRect pixelRect(const BlendPiece& p, const BlendCamera& cam, const BlendView& v)
{
    const WorldRect r = worldRect(p);
    const double depth = cam.position[2] - p.translation[2];
    if (depth <= 0.0)
        throw std::runtime_error("面片 " + label(p) + " 在机位之后（depth = "
            + std::to_string(depth) + "）——它根本不出现在画面上，拿它算判据区 = 量错层。"
              "改 node.translation 把它挪到相机前方");
    const double halfW = v.halfTan * depth * v.aspect;
    const double halfH = v.halfTan * depth;
    const double cx = static_cast<double>(v.width) / 2.0;
    const double cy = static_cast<double>(v.height) / 2.0;
    return {cx + (r.x0 / halfW) * cx, cx + (r.x1 / halfW) * cx, cy - (r.y1 / halfH) * cy,
        cy - (r.y0 / halfH) * cy};
}

/// 像素中心落在矩形内？口径与光栅化一致：像素 (x,y) 代表中心 (x+0.5, y+0.5) 一侧的覆盖，
/// 而本判据刻意把采样点取在**离边界 ≥ 30px** 的区域中点，这条半开区间取哪种都不改读数。
bool covers(const PixelRect& r, double px, double py)
{
    return r.x0 <= px && px < r.x1 && r.y0 <= py && py < r.y1;
}

/// 该像素上的全部层（按 blend 标志分流），顺序 = pieces 的输入顺序（作者顺序）。
std::vector<const BlendPiece*> layersAt(const std::vector<BlendPiece>& pieces,
    const BlendCamera& cam, const BlendView& v, double px, double py, bool blendLayer)
{
    std::vector<const BlendPiece*> out;
    for (const auto& p : pieces)
        if (p.blend == blendLayer && covers(pixelRect(p, cam, v), px, py))
            out.push_back(&p);
    return out;
}

/// 半透层按**绘制次序**（distanceSq 降序 = 远先画）排好。**不是恰好两层即抛**：
/// 四个判据区（far-only / overlap / near-only / bg-only）的定义本身就建立在两层上，
/// 层数一变，"区域来自几何"这句话就没有内容了。
std::vector<const BlendPiece*> drawOrder(const std::vector<BlendPiece>& pieces,
    const BlendCamera& cam)
{
    std::vector<const BlendPiece*> out;
    for (const auto& p : pieces)
        if (p.blend)
            out.push_back(&p);
    if (out.size() != 2)
        throw std::runtime_error("本判据需要**恰好两层**半透面片，实得 " + std::to_string(out.size())
            + " 层——四个判据区（far-only / overlap / near-only / bg-only）的定义就是这两层的"
              "footprint 交差集，层数一变区域就不是那四个。改 tools/qa_scenes.py 的 scene_blend_sort"
              "（连同本算子与判据文案一起改，别只改场景）");
    std::stable_sort(out.begin(), out.end(),
        [&](const BlendPiece* a, const BlendPiece* b) {
            return blendDistanceSq(*a, cam) > blendDistanceSq(*b, cam);
        });
    return out;
}

/// 一层 over 到已有颜色上。`Premultiplied` 把 src 直接加（不乘 α）而 dst 仍乘 (1−α)
/// ——**只用来算判别力**，引擎不是这个方程（`rgl/shader.cppm` 的 colorBlend = 非预乘）。
void overLayer(Vec3d& dst, const BlendPiece& layer, BlendModel model)
{
    const double oneMinus = 1.0 - layer.alpha;
    for (int c = 0; c < 3; ++c) {
        const double src = model == BlendModel::NonPremultiplied
            ? layer.alpha * layer.color[c]
            : layer.color[c];
        dst[c] = src + oneMinus * dst[c];
    }
}

}  // namespace

double blendDistanceSq(const BlendPiece& p, const BlendCamera& cam)
{
    const Vec3d d = p.translation - cam.position;
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

BlendSpan blendPixelSpan(const BlendPiece& p, const BlendCamera& cam, int width, int height)
{
    const PixelRect r = pixelRect(p, cam, makeView(cam, width, height));
    return {r.x0, r.x1};
}

void requireSortableKeys(const std::vector<BlendPiece>& pieces, const BlendCamera& cam)
{
    std::vector<double> keys;
    for (const auto& p : pieces)
        if (p.blend)
            keys.push_back(blendDistanceSq(p, cam));
    std::vector<double> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    const auto dup = std::adjacent_find(sorted.begin(), sorted.end());
    if (dup != sorted.end())
        throw std::runtime_error("半透层的 distanceSq 出现相等（" + std::to_string(*dup)
            + "）⟹ 排序比较子不可判：引擎用 stable_sort，相等键视为**已序**，`>` 与 `<` 给出"
              "同一个序列 ⟹ 变异不会红，而画面可能因作者顺序恰好正确 = 一条假绿的判据。"
              "修法 = 给每个半透层不同的 node.translation（排序代理是 node 原点，不是几何中心；"
              "见 tools/qa_scenes.py 的 Piece.translation 注释）");
}

Vec3 blendLinearAt(const std::vector<BlendPiece>& pieces, const BlendCamera& cam, double px,
    double py, int width, int height, BlendOrder order, BlendModel model)
{
    const BlendView v = makeView(cam, width, height);
    const std::vector<const BlendPiece*> opaque = layersAt(pieces, cam, v, px, py, false);
    if (opaque.size() != 1)
        throw std::runtime_error("像素 (" + std::to_string(px) + "," + std::to_string(py)
            + ") 上的不透明层数 = " + std::to_string(opaque.size()) + "，本闭式要求**恰好 1 层**"
              "（背景）。0 层 = 采样点落在背景之外；多层 = 这份测试输入叠了不透明面，"
              "闭式要按绘制次序逐层算而不是取一层");
    // 不透明侧 albedo 要过 G-buffer 8bit sRGB 往返（deferred 的着色输入面）。本测试输入
    // 刻意只取纯 0/1 ⟹ 往返是**不动点**；不是不动点时闭式必须带量化模型，直接抛。
    Vec3d lin{roundTrip8Srgb(opaque[0]->color[0]), roundTrip8Srgb(opaque[0]->color[1]),
        roundTrip8Srgb(opaque[0]->color[2])};
    for (int c = 0; c < 3; ++c)
        if (std::abs(lin[c] - opaque[0]->color[c]) > kBlendEps)
            throw std::runtime_error("背景色 " + vecText(opaque[0]->color) + " 过 G-buffer 8bit "
                "sRGB 往返后不是不动点（实得 "
                + vecText({lin[0], lin[1], lin[2]})
                + "）——闭式需要显式模型化量化，而"
                  "「纯 0/1 所以不必」这条前提已经不成立了。改测试输入请改回纯 0/1，"
                  "或在这里补量化模型（补了就别再声称零误差）");

    std::vector<const BlendPiece*> layers = drawOrder(pieces, cam);
    if (order == BlendOrder::FrontToBack)
        std::reverse(layers.begin(), layers.end());
    for (const auto* p : layers)
        if (covers(pixelRect(*p, cam, v), px, py))
            overLayer(lin, *p, model);
    return {lin[0], lin[1], lin[2]};
}

std::map<std::string, int> blendRegionSamples(const std::vector<BlendPiece>& pieces,
    const BlendCamera& cam, int width, int height)
{
    const BlendView v = makeView(cam, width, height);
    const std::vector<const BlendPiece*> layers = drawOrder(pieces, cam);
    const PixelRect far = pixelRect(*layers[0], cam, v);
    const PixelRect near = pixelRect(*layers[1], cam, v);
    const double ovLo = std::max(far.x0, near.x0);
    const double ovHi = std::min(far.x1, near.x1);
    if (!(ovLo < ovHi))
        throw std::runtime_error("两层半透面片在屏幕上**不相交**（远层 x ∈[" + std::to_string(far.x0)
            + ", " + std::to_string(far.x1) + "]，近层 x ∈[" + std::to_string(near.x0) + ", "
            + std::to_string(near.x1)
            + "]）——overlap 区不存在，而它是本判据的正主（只有它同时覆盖两层）。"
              "改 node.translation 的 x 让两条 footprint 相交（相交宽度要够放一个采样点）");
    // 采样点 = 各 footprint 交/差集的中点（区域来自几何，不在图里找边界）。
    return {{"far_only", static_cast<int>((far.x0 + ovLo) / 2.0)},
        {"overlap", static_cast<int>((ovLo + ovHi) / 2.0)},
        {"near_only", static_cast<int>((ovHi + near.x1) / 2.0)},
        {"bg_only", static_cast<int>((near.x1 + static_cast<double>(width)) / 2.0)}};
}

int blendMaxModelDiff(const std::vector<BlendPiece>& pieces, const BlendCamera& cam, int width,
    int height, BlendOrder order, BlendModel model)
{
    const auto regions = blendRegionSamples(pieces, cam, width, height);
    const int cy = height / 2;
    int best = 0;
    for (const auto& [region, px] : regions) {
        (void)region;
        const auto alt = toBytes(
            blendLinearAt(pieces, cam, px, cy, width, height, order, model), kBlendExposure);
        const auto want = toBytes(blendLinearAt(pieces, cam, px, cy, width, height,
                                      BlendOrder::BackToFront, BlendModel::NonPremultiplied),
            kBlendExposure);
        for (int c = 0; c < 3; ++c)
            best = std::max(best, std::abs(alt[c] - want[c]));
    }
    return best;
}

}  // namespace RGL::imgqa
