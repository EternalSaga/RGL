// shadow_edge_metric.cpp — `shadow-edge-ruler` 口径层实现（闭式、无引擎；自检 = selftest_edge.cpp）。
//
// 口径与变异红线的全文在 `shadow_edge_metric.h` 头注；本文件只写实现处的局部理由。
#include "shadow_edge_metric.h"
#include "imgqa.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace RGL::imgqa {
namespace {

std::string mmStr(double v)
{
    std::ostringstream o;
    o.precision(3);
    o << std::fixed << v;
    return o.str();
}

/// 双线性取 cov（CV_64FC1）在像素坐标 (fx, fy) 处的值（像素中心 = 整数 + 0.5 的
/// `projectPoint3` 口径，与 `sampleLuma` 同一套取整）。贴边取不到 2×2 = false。
bool sampleCovBilinear(const cv::Mat& cov, double fx, double fy, double& out)
{
    const int ix = static_cast<int>(std::floor(fx - 0.5));
    const int iy = static_cast<int>(std::floor(fy - 0.5));
    if (ix < 0 || iy < 0 || ix + 1 >= cov.cols || iy + 1 >= cov.rows)
        return false;
    const double tx = fx - 0.5 - ix;
    const double ty = fy - 0.5 - iy;
    const auto* r0 = cov.ptr<double>(iy);
    const auto* r1 = cov.ptr<double>(iy + 1);
    out = (1.0 - ty) * ((1.0 - tx) * r0[ix] + tx * r0[ix + 1])
        + ty * ((1.0 - tx) * r1[ix] + tx * r1[ix + 1]);
    return true;
}

/// quantizeShadowDistance 的 double 版（rgl/scene_context.cppm 的逐式复刻：向上取整到
/// 2 的幂；mantissa == 0.5 ⟺ 恰为 2 的幂，取自身）。
double quantShadowDistance(double d)
{
    if (!(d > 0.0) || !std::isfinite(d))
        throw std::runtime_error("edgeCascadeModel: 距离不是正有限值（" + std::to_string(d)
            + "）——机位/SceneBounds 输入坏了，texel 无从谈起");
    int e = 0;
    const double m = std::frexp(d, &e);
    return std::ldexp(1.0, m == 0.5 ? e - 1 : e);
}

/// cascadeSplitDistance 的 double 版（practical split，mix=0.5，count=4）。
double splitDistance(double nearQ, double farQ, int c)
{
    if (c + 1 >= 4)
        return farQ;
    const double logSplit = nearQ * std::pow(farQ / nearQ, c / 4.0);
    const double uniSplit = nearQ + (farQ - nearQ) * ((c + 1) / 4.0);
    return 0.5 * logSplit + 0.5 * uniSplit;
}

/// frustumBoundingSphere 半径的 double 版（rgl/scene_context.cppm 4 参重载逐式：
/// cornerScale = tanHalf·√(aspect²+1)，zc 闭式，×1.001 余量同引擎）。
double frustumRadius(double tanHalf, double aspect, double n, double f)
{
    const double cs = tanHalf * std::sqrt(aspect * aspect + 1.0);
    const double rn = n * cs;
    const double rf = f * cs;
    const double zc = (f * f - n * n + rf * rf - rn * rn) / (2.0 * (f - n));
    return std::sqrt((f - zc) * (f - zc) + rf * rf) * 1.001;
}

}  // namespace

/// 样本区间 [lo, hi) 的 cov 中位数（偶数取中间两样本均值；区间空 = 调用方守卫，不到这）。
double medianRange(const std::vector<std::pair<double, double>>& prof, std::size_t lo,
    std::size_t hi)
{
    std::vector<double> vals;
    vals.reserve(hi - lo);
    for (std::size_t i = lo; i < hi; ++i)
        vals.push_back(prof[i].second);
    const std::size_t mid = vals.size() / 2;
    std::nth_element(vals.begin(), vals.begin() + static_cast<long>(mid), vals.end());
    const double hiMid = vals[mid];
    if (vals.size() % 2 == 1)
        return hiMid;
    const double loMid = *std::max_element(vals.begin(), vals.begin() + static_cast<long>(mid));
    return 0.5 * (loMid + hiMid);
}

/// 一个世界采样点沿 v 的平行子线求 cov 均值（从 sampleCovProfile 拆出 = 认知复杂度护栏）。
/// 返回 false = 该 s 的所有子线都投不到/贴边取不到 2×2（**不夹边**、不拿部分子线凑数——
/// 凑数会让窗口边缘样本与内部样本混在同一列里比较，同 shadow_kernel_scale 的口径）。
bool covAtSample(const cv::Mat& cov, const Projector& proj, const EdgeScanLine& line,
    const std::array<double, 3>& dir, double s, double& out)
{
    double sum = 0.0;
    int hit = 0;
    for (const double vo : line.vOffsets) {
        const std::array<double, 3> p{line.from[0] + dir[0] * s + line.vDir[0] * vo,
            line.from[1] + dir[1] * s + line.vDir[1] * vo,
            line.from[2] + dir[2] * s + line.vDir[2] * vo};
        double fx = 0.0, fy = 0.0;
        double v = 0.0;
        if (!projectPoint3(proj, p, fx, fy))
            continue;  // 投到相机背后/屏外：该子线此 s 作废（不夹边）
        if (!sampleCovBilinear(cov, fx, fy, v))
            continue;
        sum += v;
        ++hit;
    }
    if (hit == 0)
        return false;
    out = sum / static_cast<double>(hit);
    return true;
}

std::vector<std::pair<double, double>> sampleCovProfile(const cv::Mat& cov,
    const Projector& proj, const EdgeScanLine& line)
{
    if (cov.empty() || cov.type() != CV_64FC1)
        throw std::runtime_error("sampleCovProfile: cov 不是 CV_64FC1（读错内存的过零点 = 假绿）");
    if (line.vOffsets.empty() || !(line.stepM > 0.0))
        throw std::runtime_error("sampleCovProfile: vOffsets 为空或 step ≤ 0（分母塌了）");
    const double len = std::sqrt(std::pow(line.to[0] - line.from[0], 2)
        + std::pow(line.to[1] - line.from[1], 2) + std::pow(line.to[2] - line.from[2], 2));
    if (!(len > 0.0))
        throw std::runtime_error("sampleCovProfile: 扫描线长度 ≤ 0（from == to，没有可扫的窗）");
    const std::array<double, 3> dir{(line.to[0] - line.from[0]) / len,
        (line.to[1] - line.from[1]) / len, (line.to[2] - line.from[2]) / len};
    std::vector<std::pair<double, double>> out;
    for (double s = 0.0; s <= len + 1e-12; s += line.stepM) {
        double v = 0.0;
        if (covAtSample(cov, proj, line, dir, s, v))
            out.emplace_back(s, v);
    }
    if (out.size() < kEdgeMinSamples)
        throw std::runtime_error("sampleCovProfile: 有效样本 " + std::to_string(out.size())
            + " < " + std::to_string(kEdgeMinSamples) + "（窗口几乎全投不到 = 机位/靶漂了）");
    return out;
}

namespace {

/// 过渡区 [lo..hi]（步骤 ①②）：最大梯度处向两侧走到梯度 ≤ kEdgeWalkFraction·gmax。
/// 常函数（gmax = 0）⟹ 抛，错误串按电平给「整窗受光 / 整窗本影 / 影缘不在窗口」归因。
struct TransitionSpan {
    std::size_t lo, hi;
};
TransitionSpan findTransition(const std::vector<std::pair<double, double>>& prof)
{
    const std::size_t n = prof.size();
    std::vector<double> g(n - 1);
    double gmax = 0.0;
    std::size_t k = 0;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        g[i] = std::abs(prof[i + 1].second - prof[i].second);
        if (g[i] > gmax) {
            gmax = g[i];
            k = i;
        }
    }
    if (!(gmax > 0.0)) {
        const double level = prof[0].second;
        const char* hint = level < 0.05
            ? "≈全 0 = 整窗受光：影没画出来（塌了/没进图）"
            : (level > 0.95 ? "≈全 1 = 整窗本影：影没退出去或窗口放错了" : "影缘不在窗口里");
        throw std::runtime_error(std::string("edgePosition: 剖面常函数（找不到梯度，窗口内 cov ≡ ")
            + mmStr(level) + "）——" + hint
            + "——不许静默返回 NaN/0，那是把「量不到」读成读数");
    }
    TransitionSpan sp { k, k + 1 };
    while (sp.lo > 0 && g[sp.lo - 1] > kEdgeWalkFraction * gmax)
        --sp.lo;
    while (sp.hi + 1 < n && g[sp.hi] > kEdgeWalkFraction * gmax)
        ++sp.hi;
    return sp;
}

/// 平台值与相对阈值（步骤 ③④）：过渡区两侧剩余全部样本取中位数，按值排 umbra/lit，
/// 过渡高 < kEdgeTransitionMinH ⟹ 抛（噪声定位的「边」不许出数）。返回阈值 t。
double plateauThreshold(const std::vector<std::pair<double, double>>& prof,
    const TransitionSpan& sp)
{
    const std::size_t n = prof.size();
    if (sp.lo < kEdgePlateauMinSamples || n - (sp.hi + 1) < kEdgePlateauMinSamples)
        throw std::runtime_error("edgePosition: 平台窗退化（上游 " + std::to_string(sp.lo)
            + " 样本 / 下游 " + std::to_string(n - (sp.hi + 1)) + " 样本，最少 "
            + std::to_string(kEdgePlateauMinSamples) + "）——过渡区几乎占满窗口，lit/umbra "
              "平台量不出来，阈值无从谈起（窗口该重摆或过渡比窗还宽）");
    const double medUp = medianRange(prof, 0, sp.lo);
    const double medDn = medianRange(prof, sp.hi + 1, n);
    const double umbra = std::max(medUp, medDn);
    const double lit = std::min(medUp, medDn);
    if (umbra - lit < kEdgeTransitionMinH)
        throw std::runtime_error("edgePosition: 过渡过浅（umbra " + mmStr(umbra) + " − lit "
            + mmStr(lit) + " = " + mmStr(umbra - lit) + " < 下界 "
            + mmStr(kEdgeTransitionMinH) + "，推导 = 10 个 8bit-LSB，见 kEdgeTransitionMinH）"
            "——噪声定位的「边」不许出数");
    return 0.5 * (lit + umbra);
}

/// 全窗口扫相对阈值 t 的过零点（步骤 ⑤）：0 个 / >1 个各抛各的串；恰 1 个返回括号对。
struct EdgeBracket {
    double s0, s1, v0, v1;
};
EdgeBracket soleCrossing(const std::vector<std::pair<double, double>>& prof, double t)
{
    std::vector<EdgeBracket> cross;
    for (std::size_t i = 1; i < prof.size(); ++i) {
        const double a = prof[i - 1].second;
        const double b = prof[i].second;
        if ((a >= t) != (b >= t))
            cross.push_back({prof[i - 1].first, prof[i].first, a, b});
    }
    if (cross.empty())
        throw std::runtime_error("edgePosition: 相对阈值 t = " + mmStr(t) + " 在窗口内无过零点"
            "（平台中位数说有两级、样本却没跨过去 = 剖面与平台估计矛盾，拒绝出数）");
    if (cross.size() > 1) {
        std::ostringstream o;
        o << "edgePosition: 窗口内有 " << cross.size() << " 个 50% 过零点（相对阈值 " << mmStr(t)
          << "，s ≈";
        for (const auto& c : cross)
            o << " " << mmStr(c.s0 * 1000.0) << "mm";
        o << "）——多半是窗口吞进了接触缘或邻影；返回第一个 = 静默选错边，抛";
        throw std::runtime_error(o.str());
    }
    return cross[0];
}

}  // namespace

double edgePosition(const cv::Mat& cov, const Projector& proj, const EdgeScanLine& line,
    bool subpixel)
{
    const auto prof = sampleCovProfile(cov, proj, line);
    // 五步（定义与每步的抛住 shadow_edge_metric.h 头注「过零点 = 相对阈值」）。
    const TransitionSpan sp = findTransition(prof);
    const double t = plateauThreshold(prof, sp);
    const EdgeBracket c = soleCrossing(prof, t);
    if (!subpixel)
        return 0.5 * (c.s0 + c.s1);
    if (c.v1 == c.v0)
        return 0.5 * (c.s0 + c.s1);  // 等值跨零（理论不可达）：中点兜底
    return c.s0 + (t - c.v0) * (c.s1 - c.s0) / (c.v1 - c.v0);
}

std::array<double, 3> edgeWorldAt(const EdgeScanLine& line, double s)
{
    const double len = std::sqrt(std::pow(line.to[0] - line.from[0], 2)
        + std::pow(line.to[1] - line.from[1], 2) + std::pow(line.to[2] - line.from[2], 2));
    if (!(len > 0.0))
        throw std::runtime_error("edgeWorldAt: 扫描线长度 ≤ 0");
    return {line.from[0] + (line.to[0] - line.from[0]) / len * s,
        line.from[1] + (line.to[1] - line.from[1]) / len * s,
        line.from[2] + (line.to[2] - line.from[2]) / len * s};
}

double worldPerpMm(const Projector& proj, const std::array<double, 3>& p,
    const std::array<double, 3>& dir, double px)
{
    const double n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(n > 1e-12))
        throw std::runtime_error("worldPerpMm: dir 是零向量（换算方向都没了）");
    const std::array<double, 3> u{dir[0] / n, dir[1] / n, dir[2] / n};
    std::array<double, 3> a = p, b = p;
    for (int k = 0; k < 3; ++k) {
        a[k] -= u[k] * kEdgeJacEpsM;
        b[k] += u[k] * kEdgeJacEpsM;
    }
    double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
    if (!projectPoint3(proj, a, ax, ay) || !projectPoint3(proj, b, bx, by))
        throw std::runtime_error("worldPerpMm: Jacobian 探针点投不到（相机背后/屏外）——"
              "换算系数在这一点没有定义，拒绝给数");
    const double dpx = std::sqrt(std::pow(bx - ax, 2) + std::pow(by - ay, 2));
    if (!(dpx > 1e-9))
        throw std::runtime_error("worldPerpMm: 投影退化（px 位移 = 0）");
    return px * (2.0 * kEdgeJacEpsM / dpx) * 1000.0;  // 米/px × px → mm
}

EdgeCascadeModel edgeCascadeModel(const Engine::Camera& cam, int cols, int rows,
    const std::array<double, 3>& boundsCenter, double boundsRadius)
{
    if (!(boundsRadius > 0.0))
        throw std::runtime_error("edgeCascadeModel: SceneBounds 半径 ≤ 0（空场景？texel 无从谈起）");
    EdgeCascadeModel m;
    const double dist = std::sqrt(std::pow(cam.position[0] - boundsCenter[0], 2)
        + std::pow(cam.position[1] - boundsCenter[1], 2)
        + std::pow(cam.position[2] - boundsCenter[2], 2));
    m.nearRaw = std::max(0.05, dist - boundsRadius);
    m.farRaw = dist + boundsRadius;
    m.nearQ = quantShadowDistance(m.nearRaw);
    m.farQ = quantShadowDistance(m.farRaw);
    if (!(m.nearQ < m.farQ))
        throw std::runtime_error("edgeCascadeModel: 量化后 near(" + std::to_string(m.nearQ)
            + ") ≥ far(" + std::to_string(m.farQ) + ")——级联模型退化（机位/靶漂了）");
    for (int c = 0; c < 4; ++c)
        m.splits[c] = splitDistance(m.nearQ, m.farQ, c);
    const double tanHalf = std::tan(cam.fovDegrees * 3.14159265358979323846 / 360.0);
    const double aspect = static_cast<double>(cols) / static_cast<double>(rows);
    for (int c = 0; c < 4; ++c) {
        const double nearC = c == 0 ? 0.02 : m.splits[c - 1];  // kShadowNearFloor 同值
        m.texelM[c] = 2.0 * frustumRadius(tanHalf, aspect, nearC, m.splits[c]) / 1024.0;
    }
    return m;
}

int edgeCascadeOf(const EdgeCascadeModel& m, double viewDepth)
{
    int c = 0;
    for (int i = 0; i < 3; ++i)
        c += viewDepth > m.splits[i] ? 1 : 0;
    return c;
}

double edgeViewDepth(const Projector& proj, const std::array<double, 3>& p)
{
    const std::array<double, 3> d{p[0] - proj.pos[0], p[1] - proj.pos[1], p[2] - proj.pos[2]};
    return d[0] * proj.f[0] + d[1] * proj.f[1] + d[2] * proj.f[2];
}

void requireEveryPlateMeasured(
    const std::vector<std::tuple<std::string, std::string, std::string>>& missing,
    std::size_t armCount, std::size_t plateCount)
{
    if (armCount == 0 || plateCount == 0)
        throw std::runtime_error("闸门 C: 臂数 = " + std::to_string(armCount) + "、板数 = "
            + std::to_string(plateCount) + "——对空集合的「每块板都量到了」恒真是假绿，抛");
    if (missing.empty())
        return;
    std::ostringstream o;
    o << "闸门 C: " << missing.size() << " 个 (臂, 板) 组合找不到过渡中点（一次报全，带算子"
        "抛因；🔴 这些是**意外失踪**——闭式预判它们该在切片区内、该量到）：";
    for (const auto& [arm, plate, why] : missing)
        o << "\n  · " << arm << " × " << plate << "：" << why;
    o << "\n  「某板所有臂都量不到」= 该档影边塌到消失/被挡；「个别臂量不到」= 该臂把影边"
         "挪出了窗口或过渡被级联边界削成两段。两种都不许静默跳过——跳过 = 判据在缺数据的"
         "板上求值（假绿）";
    throw std::runtime_error(o.str());
}

void requireReferenceEdgeAtTruth(
    const std::vector<std::tuple<std::string, double, double>>& entries)
{
    if (entries.empty())
        throw std::runtime_error("闸门 D: 参照态清单为空——恒真断言 = 假绿，抛");
    std::vector<std::string> bad;
    for (const auto& [name, err, bound] : entries)
        if (err > bound)
            bad.push_back(name + ": |误差| " + mmStr(err) + " mm > 界 " + mmStr(bound)
                + " mm（闭式推导）");
    if (!bad.empty()) {
        std::ostringstream o;
        o << "闸门 D（参照态自洽）: " << bad.size() << " 条参照态误差显著非零——"
          << "**靶或换算错了**，不是阴影错了（LOD 关 / 级 0 恒画全量几何，闭式预期 ≈ 0）：";
        for (const auto& b : bad)
            o << "\n  · " << b;
        throw std::runtime_error(o.str());
    }
}

}  // namespace RGL::imgqa
