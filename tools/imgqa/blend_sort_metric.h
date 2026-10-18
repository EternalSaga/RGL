#pragma once
//
// blend_sort_metric.h —— `blend-sort` 的**度量口径层**（透明合成次序的闭式算式）
//
//   为什么拆这一刀（缝 = 口径 vs 仪器，同 `shadow_kernel_metric.h` / `diff_struct_metric.h`）：
//   `blend_sort.cpp` 是仪器（复制冻结配置目录 → 跑引擎 → 取像素 → 落台账），本文件是口径
//   （把 manifest 记的事实变成「这个像素该是哪三个字节」）。改判据区定义 / 混合模型动这里，
//   改采集形态动那里。而**只有这里被 `just imgqa selftest` 直接测**：不启引擎、不读文件、
//   不碰 OpenCV，四个判据区的闭式数在无 GPU 的机器上照样能逐条断言。没有这一层，这条检查
//   在 `just build-tools` 里等于零覆盖——只有插上设备跑一次才可能被看见。
//
//   显示链**不在本文件**：曝光 → Narkowicz ACES → sRGB 编码 → 8bit 取整的唯一一份 C++
//   转录住在 `analytic.cpp`（`toBytes` / `roundTrip8Srgb`，声明在 `imgqa.h`）。这里只调它。
//   再抄一份 = 给显示链开第二个真相源，而四条判据里的 `blend-sort-backdrop` 专门就是钉它的。
//
//   适用范围（写死在算式里，越界一律抛、不静默给数）：
//    · 相机 forward **精确**沿 −Z（无俯角无偏航）⟹ 世界 x/y → 屏幕 px 是纯线性映射，
//      不必建 view 矩阵，闭式里少一个误差源。机位不是这形态时采样点会量到别的几何上，
//      而闭式预测照旧"自洽" = 一条会给出假绿的错。
//    · 每个面片是**局部 z=0 的平面片**，落位只靠 node 平移（透明排序的代理键就是它）。
//    · 恰好两层半透 + 至少一层不透明背景：四个判据区（far-only / overlap / near-only /
//      bg-only）的定义本身就依赖这个数量，层数一变区域就不是那四个了。
//
#include "imgqa.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace RGL::imgqa {

/// 判据阈值（LSB，四条同值）。本机四区实测**零误差**，但项目三台机器轮换，不把 0 写死；
/// 判别力最小 42 LSB（见 `blendMaxModelDiff`），1 与 42 之间没有争辩空间。
inline constexpr int kBlendTolLsb = 1;

/// 判别力下限（LSB）。备选错误模型与正确模型的最大字节差不足此值 ⟹ 这条检查分不开对错，
/// 没有资格当验收：**拒跑**（抛），不是判红——判红会让人去查一个根本不存在的渲染 bug。
inline constexpr int kBlendPowerFloorLsb = 8;

/// 曝光倍率。与 `qa/configs/blend_sort/scene.lua` 的 `exposure` 是**同一根承重键**：
/// 只改配置不改这里，四条判据会一起红（ACES 非线性，任何倍率都动字节），不会静默通过。
inline constexpr double kBlendExposure = 1.0;

/// 「应当精确相等」的容差（机位朝向、manifest 自洽性、G-buffer 不动点）。
inline constexpr double kBlendEps = 1e-12;

/// 合成次序。引擎契约 = `BackToFront`（`rgl/render_list_impl.cppm` 的
/// `stable_sort(a.distanceSq > b.distanceSq)` = 远的先画）；`FrontToBack` 只用来算判别力。
enum class BlendOrder { BackToFront, FrontToBack };

/// 混合方程。引擎契约 = `NonPremultiplied`（`rgl/shader.cppm` 的 colorBlend 分支：
/// SRC_ALPHA / ONE_MINUS_SRC_ALPHA）；`Premultiplied` 只用来算判别力。
enum class BlendModel { NonPremultiplied, Premultiplied };

/// 一个面片 = manifest `pieces[]` 里的一条。`translation` 是**排序代理**（引擎侧
/// = entity 世界矩阵的平移列 `tf.world[3]`），不是几何中心——两者在「几何烘世界坐标 +
/// node identity」的写法下会退化成所有图元 distanceSq 相等。
struct BlendPiece {
    std::string name;
    std::array<double, 3> color{};       ///< 线性 baseColor（glTF baseColorFactor）
    double alpha = 1.0;                  ///< 非预乘 α（`blend = false` 时不参与）
    bool blend = false;                  ///< alphaMode == "BLEND"
    std::array<double, 3> translation{};  ///< node 平移 = 落位 = 排序键的来源
    std::array<double, 3> aabbMin{}, aabbMax{};  ///< 局部包围盒（z 必须退化为 0）
};

/// 机位（manifest 的 `closedForm.camera` 与 `qa/configs/blend_sort/camera.lua` 必须一致，
/// 对账在仪器层做——采样点像素坐标是按这组相机解析算的）。
struct BlendCamera {
    std::array<double, 3> position{};
    std::array<double, 3> target{};
    double fovDegrees = 0.0;
};

/// 一个面片在屏幕上占的 x 区间（**浮点像素边界**，不是取整后的像素列号）。
/// 取整口径留给调用侧：判据区取的是区间中点，投影锚点比的是 `ceil`。
struct BlendSpan {
    double lo = 0.0;
    double hi = 0.0;
};

/// 面片到相机的排序距离平方（= 引擎的排序代理，node 原点而非几何中心）。
[[nodiscard]] double blendDistanceSq(const BlendPiece& p, const BlendCamera& cam);

/// 面片在屏幕上的 x 区间。前提不成立即抛：机位不是「沿 −Z 精确正视」、片不在局部 z=0 平面、
/// 面片在机位之后（depth ≤ 0）。
[[nodiscard]] BlendSpan blendPixelSpan(const BlendPiece& p, const BlendCamera& cam, int width,
    int height);

/// 排序键退化闸门：半透层的 distanceSq 若出现相等 ⟹ **抛**。
/// 理由（这条是整台仪器最容易被静默废掉的地方）：引擎用 `stable_sort`，相等键视为已序，
/// 比较子 `>` 与 `<` 给出**同一个序列** ⟹ 变异不会红，而画面可能因作者顺序恰好正确 = 假绿。
void requireSortableKeys(const std::vector<BlendPiece>& pieces, const BlendCamera& cam);

/// 该像素的线性 HDR：先取不透明背景（过 G-buffer 8bit sRGB 往返），再按 `order` 逐层 over。
/// 抛的三种前提：覆盖的不透明层数 ≠ 1、背景色过往返不是不动点（闭式需要量化模型）、
/// 机位/片平面不在本算子覆盖范围。
[[nodiscard]] Vec3 blendLinearAt(const std::vector<BlendPiece>& pieces, const BlendCamera& cam,
    double px, double py, int width, int height, BlendOrder order, BlendModel model);

/// 四个判据区各自的采样 x（键 = `bg_only` / `far_only` / `near_only` / `overlap`）。
/// 区域**来自几何**（footprint 的交/差集中点），不在图里找边界。半透层不是恰好两层、
/// 或两层在屏幕上不相交 ⟹ 抛。
[[nodiscard]] std::map<std::string, int> blendRegionSamples(const std::vector<BlendPiece>& pieces,
    const BlendCamera& cam, int width, int height);

/// 判别力 = 备选模型（`order` + `model`）与正确模型在四个采样点上的**最大字节差**。
/// 正确模型恒 = `BackToFront` + `NonPremultiplied`（引擎契约），实测读数：
/// `FrontToBack` = 42、`Premultiplied` = 95。
[[nodiscard]] int blendMaxModelDiff(const std::vector<BlendPiece>& pieces, const BlendCamera& cam,
    int width, int height, BlendOrder order, BlendModel model);

}  // namespace RGL::imgqa
