#ifndef RGL_IMGQA_PLANE_RECONSTRUCT_H
#define RGL_IMGQA_PLANE_RECONSTRUCT_H
//
// plane_reconstruct.h — **「像素 ↔ 一个已知平面上的世界点」重建**的唯一口径层
//
//   ## 这条缝为什么是职责边界，而不是「哪个文件有空位」
//   两台既有仪器各自需要同一件事：把一枚机位下的每个像素换成「它看到的是平面上哪个世界点」，
//   再换到另一枚机位的网格里去。
//     · `shadow_transport_metric`（第 7 刀）吃的是**水平**接收面 y = 0（`shadow_edge_rig` 的
//       平地），它把这件事写成了 `groundHitDouble` / `sampleGround` / `projectSamples` 三个
//       只认 y=0 的函数（存 (wx, wz) 两个数、平移是 (sx, sz)）。
//     · `shadow-swim-wall`（本批）吃的是**竖直**接收面（门面），要的是同一个算子的任意平面版。
//   把 y=0 那份复制一份改个名字 = 两份会各自漂的「同一件事」，而漂了之后两台仪器报的两个
//   「世界不变性残差」看着可比、其实不可比（AGENTS 哲学第 1 条点名的形态）。⟹ 本层是它们
//   共同的**下面那一层**：平面 = 一个点 + 两根局部轴（+ 由它们定出的法线），世界点 =
//   origin + u·e1 + v·e2。水平面只是 (origin=0, e1=x̂, e2=ẑ) 的一个特例。
//
//   ## 无损性（本层是**搬移**出来的，不是重写出来的，这条必须可证）
//   `groundPlane()` 给出 origin = (0,0,0)、e1 = (1,0,0)、e2 = (0,0,1)、normal = (0,1,0)。
//   于是本层的每一步都与 `shadow_transport_metric.cpp` 搬移前**逐位相同**：
//     · 交点参数 t = dot(n, origin − pos)/dot(n, d) = (0 − pos.y)/d.y = −pos.y/d.y（分子是
//       0·x + 1·(−pos.y) + 0·z，加法单位元不引入舍入）；
//     · 世界点 = pos + t·d（同式）；u = dot(hit − origin, e1) = hit.x·1 + hit.y·0 + hit.z·0
//       = hit.x（乘 1 与加 0 都精确）；v 同理 = hit.z；
//     · 平行判据 |dot(n, d)| < 1e-9 = |d.y| < 1e-9（同式）；
//     · 投回像素时 world = origin + (u+du)·e1 + (v+dv)·e2 = (u+du, 0, v+dv)（同上，精确）。
//   ⟹ 判据回归 = `just imgqa shadow-swim-flat` 五条在搬移前后**逐数相同**（本批实跑对账）。
//
//   ## 像素中心约定（首版在这里错过一次，口径随搬移一起搬过来）
//   `projectPoint3` 回的是像素**边缘**坐标，本层全程用像素**中心**，故 `projectPlaneSamples`
//   统一减 0.5；往返恒等锚点在 `selftest_swim_flat.cpp`（同机位「像素→世界→像素」必须落回
//   原像素 ±1e-6）。🔴 这条约定属于本层，不属于任何一台仪器——换接收面方向不改变它。
//
//   ## 精度域
//   全程 double。理由不在本层重复（→ `shadow_transport_metric.h` 头注 ①：float32 在
//   |x| = 1e6 处 ULP = 0.0625 m，比要分辨的纹素还粗）。⚠ `ground_field_metric` 那份**刻意**
//   保持 float32（它的判据是计数阈值，换精度 = 改判据），它不归本层管，也别来「统一」它。
//
#include "shadow_kernel_metric.h"  // Projector / projectPoint3

#include <array>
#include <vector>

namespace RGL::imgqa {

/// 一个接收平面：一个参考点 + 两根局部轴（张成平面）。法线 = e1 × e2（现算，不存副本）。
/// 🔴 轴必须是**单位正交**的（`makePlaneFrame` 当场核对，不靠调用侧自觉）：u/v 一旦不是
/// 真实米数，「世界平移搜索」的刻度就是错的，而读数看起来完全正常。
struct PlaneFrame {
    std::array<double, 3> origin{};
    std::array<double, 3> e1{};
    std::array<double, 3> e2{};
};

/// 由 manifest 给的平面构造并**校验**（零向量 / 不正交 / 轴不是单位长 ⟹ 抛）。
[[nodiscard]] PlaneFrame makePlaneFrame(const std::array<double, 3>& origin,
    const std::array<double, 3>& e1, const std::array<double, 3>& e2);

/// 平面的单位法线 = normalize(e1 × e2)。
[[nodiscard]] std::array<double, 3> planeNormal(const PlaneFrame& p);

/// 局部坐标 → 世界点。
[[nodiscard]] std::array<double, 3> planePoint(const PlaneFrame& p, double u, double v);

/// 世界 y = 0 水平面（e1 = x̂、e2 = ẑ、normal = +ŷ）。`shadow-swim-flat` 的口径就是它。
[[nodiscard]] const PlaneFrame& groundPlane();

/// 一枚机位下「像素 → 平面局部坐标 (u, v)」的采样点集（double 域）。预计算一次、平移搜索时
/// 复用：搜索只改「世界点 → 目标机位像素」那一步，重投影不重算。
struct PlaneSamples {
    PlaneFrame plane{};          ///< 这些点落在哪个平面上（搬运时按它还原世界点）
    std::vector<int> srcIndex;   ///< 源网格内的像素下标（y*cols + x）
    std::vector<double> u;       ///< 局部第一轴坐标（世界米）
    std::vector<double> v;       ///< 局部第二轴坐标（世界米）
};

/// 像素 (x,y) 的视线与平面的交点局部坐标，**double 域**。
/// 量不到 = false：视线与平面平行（|n·d| < 1e-9）、交点在相机背后（t ≤ 0）、
/// 交点距离超 `maxDistanceM`（掠射区一像素跨数米，噪声主导，不进判据）。
[[nodiscard]] bool planeHitDouble(const Projector& p, int x, int y, double maxDistanceM,
    const PlaneFrame& plane, double& u, double& v);

/// 遍历 `from` 的全部像素，留下能打到平面且在距离上限内的采样点（顺序 = 行优先，确定）。
[[nodiscard]] PlaneSamples samplePlane(const Projector& from, int cols, int rows,
    double maxDistanceM, const PlaneFrame& plane);

/// 把 `s` 的世界点整体沿平面局部轴挪 (du, dv) 后投进 `into` 的网格，写回**像素中心**坐标。
/// `dstX/dstY` 与 `s.srcIndex` 等长；投不出（相机背后 / 屏外）= NaN，调用侧跳过。
void projectPlaneSamples(const Projector& into, const PlaneSamples& s, double du, double dv,
    std::vector<double>& dstX, std::vector<double>& dstY);

/// 采样点的**世界**坐标（= planePoint(plane, u, v) + 局部平移）。抬出来给「按世界坐标分格 /
/// 画归因图」这类消费者用，别让调用侧自己再乘一遍轴（第二份还原式 = 会漂）。
[[nodiscard]] std::array<double, 3> planeSampleWorld(const PlaneSamples& s, size_t k,
    double du, double dv);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_PLANE_RECONSTRUCT_H
