// shadow_kernel_metric.cpp —— `shadow-kernel-scale` 的**度量口径层**。
//
// 拆出本 TU 的理由见 `shadow_kernel_metric.h` 头注释（一句话：口径层被 `just imgqa selftest`
// 用闭式直接测，仪器层要引擎，两者不是同一条改动缝）。
//
// 三个算子的**期望值全部来自闭式**（见 selftest.cpp::testShadowKernelScale）：下降斜坡
// S: 1→0 跨 [0,10] ⟹ 90% 交叉在 1.0、10% 在 9.0 ⟹ W = 0.8×10 = 8.0。实机截图只能证明
// 「与上次一样」，闭式才证明「算子算的是我说的那个量」。取样层（`shadowScanPoint`）同样有
// 闭式：一张「五条取样线里只有一条被涂黑」的合成图，平均后 S **恰为 1/5**（selftest 里那条
// 就是 `kVLineOffsets` 的存在性证明——删掉平均它立刻红）。
//
// 三条共同的口径决定（改动 = 改判据，三条的变异红线都在 selftest 里）：
//   ① 取不到交叉返回 **NaN**，绝不返回 0——0 会被读成「影缘无限硬」，而那正是缺陷态
//      （影根本没画出来 ⟹ 剖面恒 0 ⟹ 无交叉）想伪装成的样子。
//   ② 窗口 [lo,hi] 是判据的一部分：窗外的交叉不许用（否则放宽扫描窗就能把上游缘当下游缘，
//      读数换的靶子都没人知道）。
//   ③ 线性插值而非取整：世界步长 0.05 m 时取整会把 W(8) = 0.15 读成 0.10 或 0.20，
//      而判据 2 的界是 0.55/1.80 —— 量化噪声不该进到比值里。
#include "imgqa.h"
#include "shadow_kernel_metric.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace RGL::imgqa {
namespace {

double dot3(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
std::array<double, 3> cross3(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
std::array<double, 3> normalize3(const std::array<double, 3>& a)
{
    const double n = std::sqrt(dot3(a, a));
    return {a[0] / n, a[1] / n, a[2] / n};
}

}  // namespace

Projector makeProjector(const Engine::Camera& cam, int cols, int rows)
{
    Projector p;
    p.pos = cam.position;
    const std::array<double, 3> fwd
        = {cam.target[0] - cam.position[0], cam.target[1] - cam.position[1],
              cam.target[2] - cam.position[2]};
    p.f = normalize3(fwd);
    p.r = normalize3(cross3(p.f, {0.0, 1.0, 0.0}));
    p.up = cross3(p.r, p.f);
    p.tanHalf = std::tan(cam.fovDegrees * 3.14159265358979323846 / 360.0);  // 竖直半角
    p.cols = cols;
    p.rows = rows;
    p.aspect = rows > 0 ? static_cast<double>(cols) / rows : 1.0;
    return p;
}

bool projectTo(const Projector& p, double x, double z, double& px, double& py)
{
    const std::array<double, 3> d{x - p.pos[0], -p.pos[1], z - p.pos[2]};  // 地面 y = 0
    const double zc = dot3(d, p.f);
    if (zc <= 1e-3)
        return false;  // 相机背后
    const double xd = dot3(d, p.r) / (p.tanHalf * p.aspect) / zc;  // 竖直 fov ⟹ aspect 压横轴
    const double yd = dot3(d, p.up) / p.tanHalf / zc;
    if (xd < -1.0 || xd > 1.0 || yd < -1.0 || yd > 1.0)
        return false;  // 屏外：该样本作废，**不夹到边缘**（夹 = 把屏外读成边缘像素）
    px = (xd * 0.5 + 0.5) * p.cols;
    py = (0.5 - yd * 0.5) * p.rows;
    return true;
}

bool sampleLuma(const cv::Mat& luma, const Projector& p, double x, double z, double& out)
{
    double fx = 0.0, fy = 0.0;
    if (!projectTo(p, x, z, fx, fy))
        return false;
    const int ix = static_cast<int>(std::floor(fx - 0.5));
    const int iy = static_cast<int>(std::floor(fy - 0.5));
    if (ix < 0 || iy < 0 || ix + 1 >= luma.cols || iy + 1 >= luma.rows)
        return false;
    const double tx = fx - 0.5 - ix;
    const double ty = fy - 0.5 - iy;
    const auto* r0 = luma.ptr<double>(iy);
    const auto* r1 = luma.ptr<double>(iy + 1);
    out = (1.0 - ty) * ((1.0 - tx) * r0[ix] + tx * r0[ix + 1])
        + ty * ((1.0 - tx) * r1[ix] + tx * r1[ix + 1]);
    return true;
}

ShadowScanSample shadowScanPoint(const cv::Mat& refLuma, const cv::Mat& armLuma,
    const Projector& proj, const ScanLine& line, double s)
{
    // 沿 v 的 5 条线**先各自取样、再对两图分别求和**，最后才组成 S：等价于按 L_none 加权的
    // S 平均（地面平坦 ⟹ L_none 沿 v 不变 ⟹ 就是算术平均）。任何一条线取不到（出屏 / 参照
    // 黑过 kLumaUndefined）整点作废，**不做「剩下几条凑数」**——凑数会让边缘样本与内部样本
    // 混在同一列里比较。
    ShadowScanSample pt;
    pt.s = s;
    double accRef = 0.0, accArm = 0.0;
    pt.valid = true;
    for (const double dv : kVLineOffsets) {
        const double x = line.origin[0] + s * line.u[0] + dv * line.v[0];
        const double z = line.origin[1] + s * line.u[1] + dv * line.v[1];
        double lr = 0.0, la = 0.0;
        if (!sampleLuma(refLuma, proj, x, z, lr) || !sampleLuma(armLuma, proj, x, z, la)
            || lr < kLumaUndefined) {
            pt.valid = false;
            break;
        }
        accRef += lr;
        accArm += la;
    }
    pt.value = pt.valid ? (accRef - accArm) / accRef : 0.0;
    return pt;
}

std::vector<ShadowScanSample> scanShadowProfile(const cv::Mat& refLuma, const cv::Mat& armLuma,
    const Projector& proj, const ScanLine& line)
{
    const int n = std::max(1, static_cast<int>(std::llround((line.hi - line.lo) / line.step)));
    std::vector<ShadowScanSample> out;
    out.reserve(static_cast<size_t>(n) + 1);
    for (int i = 0; i <= n; ++i)
        out.push_back(shadowScanPoint(refLuma, armLuma, proj, line, line.lo + line.step * i));
    return out;
}

double shadowCrossing(const std::vector<ShadowScanSample>& pr, double thr, double lo, double hi)
{
    // 扫描本身就是 s 升序，这里只筛「有效 ∧ 落在窗口内」，取**第一个**穿阈的样本对。
    const ShadowScanSample* prev = nullptr;
    for (const auto& cur : pr) {
        if (!cur.valid || cur.s < lo || cur.s > hi)
            continue;
        if (prev != nullptr && (prev->value - thr) * (cur.value - thr) <= 0.0
            && std::abs(cur.value - prev->value) > 1e-9)
            return prev->s + (thr - prev->value) * (cur.s - prev->s) / (cur.value - prev->value);
        prev = &cur;
    }
    return kNaN;
}

double shadowPenumbraWidth(const std::vector<ShadowScanSample>& pr, double lo, double hi)
{
    const double sLo = shadowCrossing(pr, kCrossLo, lo, hi);
    const double sHi = shadowCrossing(pr, kCrossHi, lo, hi);
    if (!std::isfinite(sLo) || !std::isfinite(sHi))
        return kNaN;
    return std::abs(sHi - sLo);
}

double shadowUmbraFraction(const cv::Mat& refLuma, const cv::Mat& armLuma)
{
    if (refLuma.size() != armLuma.size())
        throw std::runtime_error("shadowUmbraFraction 两图尺寸不同 = 两臂不是同一帧/同一尺寸");
    int64_t hit = 0;
    for (int y = 0; y < refLuma.rows; ++y) {
        const auto* pn = refLuma.ptr<double>(y);
        const auto* pa = armLuma.ptr<double>(y);
        for (int x = 0; x < refLuma.cols; ++x)
            hit += pn[x] >= kLumaUndefined && (pn[x] - pa[x]) / pn[x] > kUmbraFactor ? 1 : 0;
    }
    const double total = static_cast<double>(refLuma.total());
    return total > 0.0 ? static_cast<double>(hit) / total : 0.0;
}

}  // namespace RGL::imgqa
