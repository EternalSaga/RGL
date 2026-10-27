// plane_reconstruct.cpp —— 「像素 ↔ 已知平面上的世界点」重建算子（为什么在这一层 → 同名 .h）。
//
// 本文件**只有算子**：不启引擎、不读图、不认识任何仪器。被 `just imgqa selftest` 用闭式输入
// 直测（往返恒等 / 平面方向无关性 / 平行与距离守卫），也是 `just tools-check` 覆盖得到的那一层。
//
// 🔴 本层一条铁律（从 `shadow_transport_metric` 原样继承）：**没有任何循环的迭代次数取决于
// 数据**。本层连一个循环都不新增（`samplePlane` 的双层像素循环尺寸来自画幅，是常量）。
#include "plane_reconstruct.h"

#include <cmath>
#include <stdexcept>

namespace RGL::imgqa {
namespace {

double dot3(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<double, 3> cross3(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0] };
}

double norm3(const std::array<double, 3>& a) { return std::sqrt(dot3(a, a)); }

/// 像素中心 → ndc（与 projectPoint3 的 ndc→像素严格互逆，selftest 的往返锚点钉这条）。
/// 与搬移前的 `groundHitDouble` 逐字同式。
void pixelRay(const Projector& p, int x, int y, std::array<double, 3>& d)
{
    const double nx = (x + 0.5) / static_cast<double>(p.cols) * 2.0 - 1.0;
    const double ny = 1.0 - (y + 0.5) / static_cast<double>(p.rows) * 2.0;
    d = { p.f[0] + p.r[0] * (nx * p.tanHalf * p.aspect) + p.up[0] * (ny * p.tanHalf),
          p.f[1] + p.r[1] * (nx * p.tanHalf * p.aspect) + p.up[1] * (ny * p.tanHalf),
          p.f[2] + p.r[2] * (nx * p.tanHalf * p.aspect) + p.up[2] * (ny * p.tanHalf) };
    const double n = norm3(d);
    if (!(n > 0.0))
        throw std::runtime_error("planeHitDouble: 视线方向是零向量（Projector 没填对？）");
    for (double& c : d)
        c /= n;  // 归一化 ⟹ 后面的 t 就是欧氏距离，`maxDistanceM` 才是「米」
}

}  // namespace

PlaneFrame makePlaneFrame(const std::array<double, 3>& origin,
    const std::array<double, 3>& e1, const std::array<double, 3>& e2)
{
    const double n1 = norm3(e1);
    const double n2 = norm3(e2);
    if (!(n1 > 0.0) || !(n2 > 0.0))
        throw std::runtime_error("makePlaneFrame: 局部轴是零向量 —— u/v 无从谈起");
    const std::array<double, 3> a1 { e1[0] / n1, e1[1] / n1, e1[2] / n1 };
    const std::array<double, 3> a2 { e2[0] / n2, e2[1] / n2, e2[2] / n2 };
    const double cosang = dot3(a1, a2);
    if (std::abs(cosang) > 1e-9)
        throw std::runtime_error("makePlaneFrame: 两根局部轴不正交（cos = "
            + std::to_string(cosang) + "）—— u/v 不再是互相独立的米数，平移搜索的刻度是错的");
    return PlaneFrame { origin, a1, a2 };
}

std::array<double, 3> planeNormal(const PlaneFrame& p)
{
    const std::array<double, 3> n = cross3(p.e1, p.e2);
    const double l = norm3(n);
    if (!(l > 0.0))
        throw std::runtime_error("planeNormal: e1 × e2 退化（轴共线？）");
    return { n[0] / l, n[1] / l, n[2] / l };
}

std::array<double, 3> planePoint(const PlaneFrame& p, double u, double v)
{
    return { p.origin[0] + u * p.e1[0] + v * p.e2[0],
        p.origin[1] + u * p.e1[1] + v * p.e2[1],
        p.origin[2] + u * p.e1[2] + v * p.e2[2] };
}

const PlaneFrame& groundPlane()
{
    // 🔴 这三行就是「无损」三个字的落点：e1 = x̂、e2 = ẑ 让 planePoint 退化成
    // (u, 0, v)，而 dot(n, d) 退化成 d.y —— 与搬移前的 y=0 特化版逐位相同。
    static const PlaneFrame kGround
        = { { { 0.0, 0.0, 0.0 } }, { { 1.0, 0.0, 0.0 } }, { { 0.0, 0.0, 1.0 } } };
    return kGround;
}

bool planeHitDouble(const Projector& p, int x, int y, double maxDistanceM,
    const PlaneFrame& plane, double& u, double& v)
{
    std::array<double, 3> d { };
    pixelRay(p, x, y, d);
    const std::array<double, 3> nrm = planeNormal(plane);
    const double denom = dot3(nrm, d);
    if (std::abs(denom) < 1e-9)
        return false;  // 视线与平面平行：交点在无穷远
    const std::array<double, 3> toOrigin { plane.origin[0] - p.pos[0],
        plane.origin[1] - p.pos[1], plane.origin[2] - p.pos[2] };
    const double t = dot3(nrm, toOrigin) / denom;
    if (!(t > 0.0) || t > maxDistanceM)
        return false;  // 相机背后 / 远到不可信（掠射区一像素跨数米）
    const std::array<double, 3> hit { p.pos[0] + d[0] * t, p.pos[1] + d[1] * t,
        p.pos[2] + d[2] * t };
    const std::array<double, 3> rel { hit[0] - plane.origin[0], hit[1] - plane.origin[1],
        hit[2] - plane.origin[2] };
    u = dot3(rel, plane.e1);
    v = dot3(rel, plane.e2);
    return std::isfinite(u) && std::isfinite(v);
}

PlaneSamples samplePlane(const Projector& from, int cols, int rows, double maxDistanceM,
    const PlaneFrame& plane)
{
    PlaneSamples s;
    s.plane = plane;
    s.srcIndex.reserve(static_cast<size_t>(cols) * static_cast<size_t>(rows) / 4);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            double u = 0.0;
            double v = 0.0;
            if (!planeHitDouble(from, x, y, maxDistanceM, plane, u, v))
                continue;
            s.srcIndex.push_back(y * cols + x);
            s.u.push_back(u);
            s.v.push_back(v);
        }
    return s;
}

void projectPlaneSamples(const Projector& into, const PlaneSamples& s, double du, double dv,
    std::vector<double>& dstX, std::vector<double>& dstY)
{
    dstX.assign(s.srcIndex.size(), kNaN);
    dstY.assign(s.srcIndex.size(), kNaN);
    for (size_t k = 0; k < s.srcIndex.size(); ++k) {
        double px = 0.0;
        double py = 0.0;
        // 🔴 −0.5：projectPoint3 回的是像素**边缘**坐标，本层全程用像素**中心**（见头注）。
        if (!projectPoint3(into, planeSampleWorld(s, k, du, dv), px, py))
            continue;
        dstX[k] = px - 0.5;
        dstY[k] = py - 0.5;
    }
}

std::array<double, 3> planeSampleWorld(const PlaneSamples& s, size_t k, double du, double dv)
{
    // 先加局部平移再还原（而不是先还原再加）：与搬移前 `{wx + sx, 0, wz + sz}` 的算式同序，
    // 水平面特例下逐位相同（u + du 先算，再乘 e1）。
    return planePoint(s.plane, s.u[k] + du, s.v[k] + dv);
}

}  // namespace RGL::imgqa
