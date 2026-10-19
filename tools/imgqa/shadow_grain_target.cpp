// shadow_grain_target.cpp —— `shadow-grain` 的**靶侧**：manifest measure 块（按缝深分组）
// 的读取 + 「世界角点 → 判据窗口/列区间」的投影。缝 = 仪器内部（跨 TU 面在 shadow_grain.h）：
// 改测量接口形状动本文件，改判决线动 shadow_grain_judge.cpp，改臂/采集动 shadow_grain.cpp。
//
// 「区域来自几何」是 imgqa 的硬纪律：判据窗口与每条带的列区间全部由 manifest 的世界坐标
// 角点经针孔投影得出，C++ 不重抄任何几何常量（重抄 = 第二真相，改靶必漂）。
//
// ## 变异红线（哪个变异让我红；实跑记录见 2026-10-19 交付说明）
//   · 把某条缝的 d 挪出分离窗（改 qa_scenes.py 重生成）→ 该缝的在场闸门抛（rc=3），
//     不许静默进 J1——死缝进 J1 = 多一个恒真好消息（penumbra_ladder 教训）。
//   · 轴对齐守卫（projectRegionWindow 的 spreadPair）删掉 → 把机位歪一下（改 manifest
//     cameras）后闭式 2·count/(cols−1) 在斜平行四边形上不成立，而读数照常出——红不了
//     的判据不配叫判据，这条守卫的实机触发记录 = 首版 height 算成负数被 kMinWindow 抓住。
#include "shadow_grain.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

std::string readFileText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到文件: " + p.string());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::array<double, 3> asVec3(const json& v, const std::string& what)
{
    if (!v.is_array() || v.size() != 3)
        throw std::runtime_error("manifest 的 " + what + " 不是 3 元数值数组——测量接口"
              "形状变了，本命令拒绝猜");
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}

/// 4 角点投影 + 钉住「轴对齐矩形」前提：墙面 ⊥ 视轴（生成器机关②）⟹ 透视退化为相似
/// 变换，4 角点的 x 恰两两相同、y 恰两两相同（容差 kAxisAlignTol px）。前提破了（机位/
/// 靶漂了）= 抛——在斜 parallelogram 上按矩形分母算密度，闭式刻度不成立。
/// @return 排序后的 [x0, x1, y0, y1]
std::array<double, 4> projectAligned(const Projector& proj,
    const std::array<std::array<double, 3>, 4>& corners, const std::string& what)
{
    std::array<double, 4> xs{}, ys{};
    for (size_t i = 0; i < 4; ++i) {
        if (!projectPoint3(proj, corners[i], xs[i], ys[i]))
            throw std::runtime_error(what + " 角点投到相机背后/屏外——机位或靶漂了，"
                  "区域来自几何的前提已破，拒绝在夹出来的窗口上求值");
    }
    const auto spreadPair = [&](double* v, const char* axis) {
        std::array<double, 4> s{v[0], v[1], v[2], v[3]};
        std::sort(s.begin(), s.end());
        if (std::abs(s[1] - s[0]) > kAxisAlignTol || std::abs(s[3] - s[2]) > kAxisAlignTol)
            throw std::runtime_error(std::string(what) + " 投影不是轴对齐矩形（" + axis
                + " 坐标分裂超 " + std::to_string(kAxisAlignTol) + " px）——生成器机关②"
                  "（视轴 ∥ 墙法线）破了，闭式 2·count/(cols−1) 不再成立");
    };
    spreadPair(xs.data(), "x");
    spreadPair(ys.data(), "y");
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    return {xs[0], xs[3], ys[0], ys[3]};
}

/// measure.groups 的解析（从 loadGrainTarget 拆出守认知复杂度阈值；语义一字未动）。
std::vector<GrainGroup> parseGrainGroups(const json& groups)
{
    if (!groups.is_array() || groups.empty())
        throw std::runtime_error("manifest 的 measure.groups 不是非空数组——缝深组是靶子"
              "加密（多缝深守「8 之下提前抹除」）的测量接口，旧版单组 measure.bands 已随"
              "接口升级删除，本命令拒绝猜形状");
    std::vector<GrainGroup> out;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& gj = groups[gi];
        GrainGroup g;
        g.depthM = gj.at("depthM").get<double>();
        const auto& bands = gj.at("bands");
        if (!bands.is_array() || bands.empty())
            throw std::runtime_error("measure.groups[" + std::to_string(gi)
                + "] 的 bands 不是非空数组——槽带是在场闸门与闭式刻度的载体，"
                  "没有它这个组无事可判");
        for (const auto& b : bands) {
            if (!b.is_array() || b.size() != 2)
                throw std::runtime_error("measure.groups[].bands 的某条带不是 2 角点——"
                      "列区间无从投影，拒绝猜");
            g.bands.push_back({asVec3(b[0], "measure.groups[].bands[][0]"),
                asVec3(b[1], "measure.groups[].bands[][1]")});
        }
        out.push_back(std::move(g));
    }
    return out;
}

}  // namespace

std::size_t GrainTarget::bandCount() const
{
    size_t n = 0;
    for (const auto& g : groups)
        n += g.bands.size();
    return n;
}

/**
 * 读靶。**缺文件 = 抛**（不 SKIP，同 kernelscale：静默跳过 = 假绿）。
 * captureSize 与采集尺寸不符 = 抛——生成器的取景断言（region 在画内 + 16px 裕量）
 * 是按它算的，偷改尺寸 = 在没被断言过的画幅上用闭式。
 * measure.groups 是缝深加密后的测量接口：每组 = 缝深 + 本组各带（死缝由闸门在
 * csm0 臂上逐条判，不在这里判——这里只管形状）。
 */
GrainTarget loadGrainTarget()
{
    const fs::path dir = fs::path(projectRoot()) / "build/rgl/models";
    const fs::path glb = dir / (std::string(kGlbStem) + ".glb");
    const fs::path manifestPath = dir / (std::string(kGlbStem) + ".manifest.json");
    if (!fs::exists(glb))
        throw std::runtime_error("靶不存在: " + glb.string()
            + "\n  生成：python3 tools/qa_scenes.py --only shadow_grain --out "
              "build/rgl/models（缺靶必须 FAIL，不许 SKIP）");
    if (!fs::exists(manifestPath))
        throw std::runtime_error("靶的 manifest 不存在: " + manifestPath.string()
            + "\n  只有 .glb 没有 manifest ⟹ 测量接口（measure 块）没有来源，拒绝猜几何");
    const json m = json::parse(readFileText(manifestPath));
    if (!m.contains("measure") || !m.at("measure").is_object())
        throw std::runtime_error("manifest 里没有 measure 块——它是场景生成器留给本命令的"
              "测量接口；没有它就得在 C++ 里重抄几何常量（第二真相），拒绝");
    const json& ms = m.at("measure");
    GrainTarget t;
    const auto& rc = ms.at("regionCorners");
    if (!rc.is_array() || rc.size() != 4)
        throw std::runtime_error("manifest 的 measure.regionCorners 不是 4 角点——"
              "region 是矩形是闭式刻度的前提，形状不对拒绝求值");
    for (size_t i = 0; i < 4; ++i)
        t.corners[i] = asVec3(rc[i], "measure.regionCorners[]");
    t.groups = parseGrainGroups(ms.at("groups"));
    if (t.bandCount() < 2)
        throw std::runtime_error("靶上闭式带总数 < 2——「取最严一条缝」的判据在单带上"
              "退化成「这条缝在不在」，多缝深加密名存实亡，拒绝求值");
    t.litProbe = asVec3(ms.at("litProbe"), "measure.litProbe");
    const auto cs = ms.at("captureSize");
    if (cs.size() != 2 || cs[0].get<int>() != kWidth || cs[1].get<int>() != kHeight)
        throw std::runtime_error("manifest 的 measure.captureSize 与采集尺寸 "
            + std::to_string(kWidth) + "x" + std::to_string(kHeight)
            + " 不符——生成器的取景断言是按它算的，换尺寸 = 换靶，重推闭式");
    const json& cam = m.at("cameras").at("grain");
    t.camera.position = asVec3(cam.at("position"), "cameras.grain.position");
    t.camera.target = asVec3(cam.at("target"), "cameras.grain.target");
    t.camera.fovDegrees = cam.value("fovDegrees", 60.0);
    return t;
}

/// region 投影窗（cov/density 的定义域）+ 全靶闭式刻度 2·总带数/(cols−1)。
/// 内缩 1 px：角点落在像素边界上时 ceil/floor 的取整方向不该进判据（窗口抖 1 px 改不动
/// 闭式刻度的分母量级，但「窗口恰好吃掉第一条带缘」这种边界态不值得赌）。
GrainWindow projectRegionWindow(const Projector& proj, const GrainTarget& t)
{
    const auto b = projectAligned(proj, t.corners, "region");
    GrainWindow g;
    g.win = cv::Rect(static_cast<int>(std::ceil(b[0])) + 1,
        static_cast<int>(std::ceil(b[2])) + 1,
        static_cast<int>(std::floor(b[1])) - 1 - (static_cast<int>(std::ceil(b[0])) + 1),
        static_cast<int>(std::floor(b[3])) - 1 - (static_cast<int>(std::ceil(b[2])) + 1));
    if (g.win.width < kMinWindow || g.win.height < kMinWindow)
        throw std::runtime_error("region 投影窗口小于 " + std::to_string(kMinWindow)
            + " px——闭式刻度 2·count/(cols−1) 在小分母上失稳，拒绝求值");
    g.closed = 2.0 * static_cast<double>(t.bandCount())
        / static_cast<double>(g.win.width - 1);
    return g;
}

/// 一条槽带的两个列区间（生成器机关③：带是竖直面 ⟹ 列区间与行无关，行维 = region 全行）。
///  · 存活窗 = 闭式带 [round(x0)−1, round(x1)+1]（两缘的跳变对 (x0−1,x0) 与 (x1,x1+1)
///    都严格落在区间内）再各外扩 kBandEdgeMarginCols：带缘有 ±1 px 光栅化漂移，
///    富余列把它吃掉；富余列必须 < 到邻带的距离（生成器 assert ⑪ + 本文件调用侧的
///    「存活窗互不重叠」守卫），否则邻带的缘会被算进本带的跳变 = 存活率虚高。
///  · 在场探针 = 闭式带中央 50%（**不内缩**——细带只有 4–12 px，旧版「中央 50% 再内缩
///    1 px」在 0.14 m 缝深上直接放不下 2 px 探针，把能测的缝挡在门外）。
/// 真实带是闭式带的子集：normal offset（1.5 texel 沿法线）与 PCF（±0.5 texel）只会把带
/// 收窄、绝不加宽（偏置机制单调朝受光侧）——收窄量 ∝ 级联 texel（仪器不可知），故探针
/// 取中央：带活着 ⟹ 中央必在带内。区间必须落在 region 窗内（带不在 region = 靶/manifest
/// 漂了，抛）。
GrainBandGeom projectBandGeom(const Projector& proj, const GrainBand& band,
    const cv::Rect& regionWin, size_t gi, size_t bi)
{
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    if (!projectPoint3(proj, band[0], x0, y0) || !projectPoint3(proj, band[1], x1, y1))
        throw std::runtime_error("槽带 " + std::to_string(gi) + "/" + std::to_string(bi)
            + " 端点投到相机背后/屏外——靶/机位漂了");
    const double loE = std::min(x0, x1), hiE = std::max(x0, x1);
    const int lo = static_cast<int>(std::lround(loE));
    const int hi = static_cast<int>(std::lround(hiE));
    GrainBandGeom geom;
    geom.survival = cv::Range(lo - 1 - kBandEdgeMarginCols, hi + 2 + kBandEdgeMarginCols);
    if (geom.survival.start < regionWin.x || geom.survival.end > regionWin.x + regionWin.width)
        throw std::runtime_error("槽带 " + std::to_string(gi) + "/" + std::to_string(bi)
            + " 的存活窗不在 region 窗内——region 与槽的相对位置漂了（生成器 assert 的"
              "前提在实机上不成立）");
    const double mid = 0.5 * (loE + hiE);
    const double quarter = 0.25 * (hiE - loE);
    geom.probe = cv::Range(static_cast<int>(std::lround(mid - quarter)),
        static_cast<int>(std::lround(mid + quarter)) + 1);
    if (geom.probe.end - geom.probe.start < 2)
        throw std::runtime_error("槽带 " + std::to_string(gi) + "/" + std::to_string(bi)
            + " 中央 50% 可用列 < 2 px——闭式带太窄（生成器 assert ⑧ 应已拦住带宽 < 4 px"
              "的缝深；走到这里 = 投影与生成器对不上），探针失去意义");
    return geom;
}

/// 全部带的几何（展平序 = 组序 × 组内带序）。
std::vector<GrainBandGeom> projectAllBandGeoms(const GrainTarget& t, const Projector& proj,
    const GrainWindow& region)
{
    std::vector<GrainBandGeom> out;
    for (size_t gi = 0; gi < t.groups.size(); ++gi)
        for (size_t bi = 0; bi < t.groups[gi].bands.size(); ++bi)
            out.push_back(projectBandGeom(proj, t.groups[gi].bands[bi], region.win, gi, bi));
    return out;
}

/// 列区间 × 判据窗口全行 的 cov 均值。
double columnsMean(const cv::Mat& cov, const cv::Range& cols, const cv::Rect& win)
{
    double sum = 0.0;
    for (int y = win.y; y < win.y + win.height; ++y)
        for (int x = cols.start; x < cols.end; ++x)
            sum += cov.at<double>(y, x);
    return sum / (static_cast<double>(cols.end - cols.start) * win.height);
}

/// litProbe 邻域（(2·kProbeHalf+1)² 像素）的 cov 均值。探针必须在判据窗口内。
double probeMean(const cv::Mat& cov, const Projector& proj, const std::array<double, 3>& w,
    const cv::Rect& win, const std::string& what)
{
    double px = 0.0, py = 0.0;
    if (!projectPoint3(proj, w, px, py))
        throw std::runtime_error("探针 " + what + " 投出屏外——靶/机位漂了");
    const int cx = static_cast<int>(std::lround(px)), cy = static_cast<int>(std::lround(py));
    if (cx - kProbeHalf < win.x || cy - kProbeHalf < win.y
        || cx + kProbeHalf >= win.x + win.width || cy + kProbeHalf >= win.y + win.height)
        throw std::runtime_error("探针 " + what + " 的邻域不在判据窗口内——region 与"
              "遮挡体的相对位置漂了（生成器 assert 的前提在实机上不成立）");
    double sum = 0.0;
    for (int y = cy - kProbeHalf; y <= cy + kProbeHalf; ++y)
        for (int x = cx - kProbeHalf; x <= cx + kProbeHalf; ++x)
            sum += cov.at<double>(y, x);
    return sum / static_cast<double>((2 * kProbeHalf + 1) * (2 * kProbeHalf + 1));
}

}  // namespace RGL::imgqa
