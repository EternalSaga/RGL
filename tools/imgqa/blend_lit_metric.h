#pragma once
//
// blend_lit_metric.h —— `blend-lit` 的**度量口径层**（透明段像素贡献的区域归因算子）
//
//   为什么拆这一刀（缝 = 口径 vs 仪器，同 `blend_sort_metric.h` / `shadow_kernel_metric.h`）：
//   `blend_lit.cpp` 是仪器（复制冻结配置目录 → 跑引擎 → 读 /state → 取双臂像素 → 落台账），
//   本文件是口径（把 manifest 记的几何事实变成「哪些像素算透明段的 footprint」+「两臂在这块
//   区域内外各差了多少」）。改区域定义动这里，改采集形态动那里。
//
//   而**只有这里被 `just imgqa selftest` 直接测**：合成两张小图喂进 `blendLitCompareArms`，
//   不启引擎、不读文件、无 GPU 也能逐条断言。没有这一层，本判据在 `just build-tools` 里
//   等于零覆盖——只有插上设备跑一次才可能被看见，而"跑一次"恰恰是最容易不跑的那个动作。
//
//   ⚠ 本层**没有**任何闭式像素值预测，这是刻意的（债-134 的裁决：透明光照的闭式要复刻
//   BRDF + IBL + CSM，成本 > 收益）。本层只回答两个几何问题：这块像素在不在透明段 footprint
//   里、两臂在这里差了多少字节。显示链因此一行都不涉及，不需要 `analytic.cpp` 的 `toBytes`。
//
//   适用范围（前提不成立一律抛、不静默给数）：
//    · 相机 forward **精确**沿 −Z（无俯角无偏航）⟹ 世界 x/y → 屏幕 px 是纯线性映射，不必建
//      view 矩阵。机位不是这形态时区域会框到别的几何上，而"两臂不同"照旧成立 = 假绿。
//    · 每支 pane 是**局部 z=0 的平面片**、共面（同一个 z）、互不重叠。三条里任何一条破了，
//      "这块像素属于哪支 pane"失去一一对应，区域归因作废（多层叠加是 blend-sort 的射程）。
//    · 背景板必须在自身深度上**铺满整屏**——"footprint 之外双臂逐位相同"这条判据的全部
//      意义建立在"之外只有背景板"之上。铺不满 ⟹ 之外还有天空盒/清屏色，那条判据在测别的。
//
#include "imgqa.h"

#include <array>
#include <string>
#include <vector>

namespace RGL::imgqa {

/// footprint 向外扩张的像素数。pane 边界有几何抗锯齿覆盖（实测覆盖带恰好 1 px 宽），
/// 不扩的话边界像素会落进"应当逐位相同"那一侧，把一条真判据判成红。
/// 实测依据：pad=1 时 footprint 外 changed = 0（见 blend_lit.cpp 头注的实测表）。
inline constexpr int kBlendLitPadPx = 1;

/// 「双臂必须不同」的最小判别力（LSB）。footprint 内的最大字节差不足此值 ⟹ 本靶分不开
/// 「透明段画了」与「透明段没画」，**拒跑**（抛），不是判红——判红会让人去查一个不存在的问题。
/// 实测 165 LSB（α=0.25 的红 pane：BLEND (119,0,184) vs OPAQUE (226,0,21)），余量两个数量级。
inline constexpr int kBlendLitPowerFloorLsb = 8;

/// 透明段条数的容差 = **0**。条数是整数，"差不多对"不存在；写成 1 等于允许漏一条 draw。
inline constexpr int kBlendLitTolDraws = 0;

/// 读数要求的**最小帧号**。RPC 请求只在帧边界被 drain，而 poll 挂在 system 链**之前**
/// ⟹ 第 1 次 drain（frame==1）读到的是装配期 emplace 的默认空表，恒 0，
/// **不是**「场景没有透明图元」。口径出处 = `rgl/debug_rpc_exec_impl.cppm` 的 transparentSpan 注。
inline constexpr uint32_t kBlendLitMinFrame = 2;

/// 场景几何事实（全部来自 manifest 的 `blendLit` 块，**不在 C++ 里重抄** = 第二真相）。
struct BlendLitGeometry {
    std::array<double, 3> cameraPosition{};
    std::array<double, 3> cameraTarget{};
    double fovDegrees = 0.0;
    double paneZ = 0.0;
    double paneSize = 0.0;
    std::vector<double> paneXs;  ///< 每支 pane 的 node 原点 x（y 恒 0、共面）
    double backdropHalf = 0.0;   ///< 背景板半尺寸
    double backdropZ = 0.0;
    int width = 0;
    int height = 0;
};

/// 一支 pane 的屏幕 footprint（**闭区间**像素列/行号）。
struct BlendLitBox {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

/// 解析投影：pane → 像素框。x0 取 floor、x1 取 ceil（**向外取整**，宁可多圈一像素也不让
/// 边界像素漏进"应当逐位相同"那一侧）。前提不成立即抛：机位不是沿 −Z 精确正视、pane 在
/// 机位之后、画幅非正、paneXs 为空。
[[nodiscard]] std::vector<BlendLitBox> blendLitBoxes(const BlendLitGeometry& g);

/// 背景板必须在自身深度上铺满整屏。铺不满即**抛**（不是判红）：此时"footprint 之外"里
/// 混进了背景板以外的东西，`blend-lit-confined` 测的就不再是"透明段没漏到该漏的地方"。
void requireBackdropCoverage(const BlendLitGeometry& g);

/// 双臂逐像素对账的读数。计数用 long long：整幅 1280×720 = 921600 像素，int 装得下，
/// 但换画幅不该是这条判据的边界。
struct BlendLitArmDiff {
    long long changedInside = 0;  ///< footprint(±pad) 内 changed 像素数
    long long changedOutside = 0; ///< 之外 changed 像素数 —— `blend-lit-confined` 的本体
    int maxLsbInside = 0;         ///< 判别力读数
    int maxLsbOutside = 0;        ///< 之外的最大字节差（与 changedOutside 配成一对读数）
};

/**
 * 两臂逐像素对账，按「是否在任一 footprint 的外扩区内」分两桶。
 *
 * 为什么分桶是这台仪器的**立命处**：只报"图变了"的话，「透明段一条都没画、但两臂因为别的
 * 原因不同」也算过（阴影那条旁路就是这么发现的，见 qa/configs/blend_lit/shadow.lua）。
 * 分桶之后判据是两条独立的：内必须变（贡献）、外必须**逐位不变**（归属）。
 *
 * 尺寸或通道数不一致 = 抛（两臂必须是同一画幅同一格式，否则"逐位相同"这句话没有意义）。
 */
[[nodiscard]] BlendLitArmDiff blendLitCompareArms(const cv::Mat& blendArm, const cv::Mat& opaqueArm,
    const std::vector<BlendLitBox>& boxes, int pad = kBlendLitPadPx);

/// 像素是否落在任一 box 的外扩 pad 区内（独立出来给 selftest 直接钉边界口径）。
[[nodiscard]] bool blendLitInsideAny(
    const std::vector<BlendLitBox>& boxes, int x, int y, int pad = kBlendLitPadPx);

/**
 * 一块区域里的**众数颜色**（出现次数最多的 RGB）。`inside = true` 取 footprint 内，
 * `false` 取 footprint 外（= 背景板）。
 *
 * 为什么是众数而不是均值：本靶要问的是「这块像素**是不是**背景板那个颜色」，那是
 * 「交付图上看不看得见这层透明」的**二值**问题。均值会被边界像素与光照梯度拖走，
 * 而众数在"整块都是背景板"时**恰好等于**背景板的众数（差 0），在"pane 盖在上面"时
 * 差的是整块色差的量级（实测 ≥ 35 LSB）。二值问题要配一个能给出 0 的算子。
 *
 * 区域为空 = **抛**（不是返回黑）：返回黑会让"区域算错了"变成一条看起来正常的读数。
 */
[[nodiscard]] std::array<int, 3> blendLitDominant(const cv::Mat& bgr,
    const std::vector<BlendLitBox>& boxes, bool inside, int pad = kBlendLitPadPx);

/// 两个众数颜色的最大通道字节差（判据 6 的比较算子；与 blendLitCompareArms 同一把尺）。
[[nodiscard]] int blendLitColorDelta(
    const std::array<int, 3>& a, const std::array<int, 3>& b);

}  // namespace RGL::imgqa
