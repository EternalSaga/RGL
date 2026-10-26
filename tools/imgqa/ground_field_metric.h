#ifndef RGL_IMGQA_GROUND_FIELD_METRIC_H
#define RGL_IMGQA_GROUND_FIELD_METRIC_H
// ground_field_metric —— **世界 y=0 平面上的场**与它的对拍口径（重投影 / 平移搜索）。
//
// 🔴 债-154 之后本层有**两种**场（二值 `GroundField` / 连续 `GroundRealField`），但只有
// **一套**格坐标口径。连续场与二值场的共同格判定必须**同式**（二值标签本来就是「同一个中位数
// 过不过阈值」），否则「翻转率绿而连续量红」这个债-154 的核心结论比的是两批格，整个不成立。
// ⟹ 本层的缝 = 「格坐标 + 共同格判定 + 平移搜索」，**不是**「二值 vs 连续」；而「配对完之后
//    mean/p99 怎么数」再往下还有一层，住 `field_diff_metric`（与影场共用，别在这里抄）。
//
// ## 为什么有这一层（而不是每台仪器各写一份）
//
// `swim`（影场世界不变性，`rig_shadow_field.cpp`）与 `material-swim`（材质场世界不变性，
// 债-152）问的是**同一个问题**：把两枚机位下的某个逐像素二值量各自重投影回世界地面、
// 栅格化成场，然后问「同一个世界点在两枚机位下是否同一标签」，FAIL 时再用平移搜索量出
// 「它在世界里滑了多少」。两边共用的是**对拍口径**（重投影公式、格坐标、共同格计数、
// 覆盖率下限、同分 tie-break），不同的只是「这个逐像素二值量怎么来的」（影场吃 luma 的
// p95×0.3 阈值 + objectID 地面掩膜；材质场吃 G-buffer roughness 字节的中位数 + 地面矩形）。
// ⟹ 缝切在「口径 vs 传感」：口径住本文件（唯一出处），传感留在各仪器自己的 TU 里。
//
// 抄第二份口径的代价是本项目反复登记的那一条（AGENTS 哲学第 1 条）：两份实现会漂，
// 而漂了之后 `swim` 绿、`material-swim` 红，读的人以为测的是两件事。
//
// ## 重投影口径（与引擎一致，改动 = 改判据）
//
// 引擎口径：fb 行 0 ↔ ndc.y +1；fov 是**竖直**的，横轴乘 aspect。内部一律 float 运算
// （与 `swim` 改前逐字相同——换成 double 会挪动格归属，而 `shadow-swim` 的阈值是计数，
// 挪动就是改判据）。⚠ 这条精度限制**只在本层的 float32 域内**成立：要跨 |x| = 1e6 工作点做
// 对拍，必须用 double 域的那一份口径（理由 → `shadow_transport_metric.h` 头注 ①），
// 而不是把这里改成 double——改这里 = 改 `swim` 的判据。
// 视线近平行地面（|dir.y| < 1e-6）或交点出界（t ≤ 0、t > maxDistanceM）
// = 这一像素量不到，返回 false 而不是给一个假格。
#include <array>
#include <cstddef>
#include <map>
#include <utility>

#include <glm/glm.hpp>

namespace RGL::imgqa {

/// 世界 y=0 平面上的格坐标 = floor(世界米 / 格边长)。
using GroundCell = std::pair<int, int>;

/// 二值世界场：格 → 标签（true = 「亮/湿」那一侧，语义由各仪器自己定）。
using GroundField = std::map<GroundCell, bool>;

/// 重投影基向量与视锥参数（一台机位一份；两帧各算各的：与引擎有出入也只会**同样地**
/// 翘曲两帧，平移差不受影响——`swim` 立项时写下的自洽性论证，原样继承）。
struct GroundCamera {
    glm::vec3 pos;  ///< 机位
    glm::vec3 fw;   ///< 前向（target − position 归一化）
    glm::vec3 rt;   ///< 右向
    glm::vec3 up;   ///< 上向
    double tanH = 0.0;  ///< 半高视场的正切（fov 竖直 ⟹ 换算除以 360 不是 2）
    int width = 0;      ///< 采集尺寸（像素 → ndc 的分母，横轴各向异性由它推，不另存 aspect）
    int height = 0;
};

/// 从机位三件套算重投影基向量。`fovDegrees` 是**竖直** fov，`extent` 是采集尺寸。
[[nodiscard]] GroundCamera makeGroundCamera(const std::array<double, 3>& position,
    const std::array<double, 3>& target, double fovDegrees, int width, int height);

/// 像素 (x,y) 的视线与世界 y=0 平面交点（`world` = 交点的 x/z）。量不到（见头注）= false。
/// `maxDistanceM` = 交点距离上限（超出即认为不可信）。
[[nodiscard]] bool groundHitOf(const GroundCamera& cam, int x, int y, double maxDistanceM,
    glm::dvec2& world);

/// 像素 (x,y) 的视线与世界 y=0 平面交点所在的格 = `groundHitOf` + `floor(world / cellM)`。
/// `cellM` = 格边长（米）。
[[nodiscard]] bool groundCellOf(const GroundCamera& cam, int x, int y, double cellM,
    double maxDistanceM, GroundCell& cell);

/// 两场在平移 (sx,sz) 下的共同格数与失配格数；(0,0) 就是零位移读数。
struct FieldOverlap {
    int common = 0;     ///< 共同格数
    int mismatch = 0;   ///< 共同格里标签不一致的格数
};

[[nodiscard]] FieldOverlap overlapOf(const GroundField& a, const GroundField& b, int sx,
    int sz);

/**
 * **连续**世界场：格 → 标量（`material-swim` 用归一化到 [0,1] 的 roughness 中位数）。
 *
 * 为什么和 `GroundField` 住同一个头、同一套格坐标口径：二值标签就是「本场的中位数过不过阈值」，
 * 两条读数必须落在**同一批格**上。分母一旦不同，「翻转率绿而连续量红」这个债-154 的核心结论
 * 就根本不成立（比的会是两批不同的格）。
 */
using GroundRealField = std::map<GroundCell, double>;

/// 连续场在平移 (sx,sz) 下的对拍读数。mean/p99 的**算法**不在这里——那是
/// `field_diff_metric` 的 `meanAbsPermille` / `p99Abs`（与影场共用），本层只管「哪些格算共同格」。
struct RealFieldOverlap {
    int common = 0;       ///< 共同格数（口径与 `FieldOverlap::common` 逐字相同）
    double sumAbs = 0.0;  ///< 共同格上 |Δ| 之和
    double p99Abs = 0.0;  ///< 共同格上 |Δ| 的 p99（同单位，不是千分点）

    /// |Δ| 的均值（千分点）。空集 → 0（分母是彩票由调用侧的规模闸门负责抛）。
    /// ⚠ 名字**不能**叫 `meanAbsPermille`——那会遮蔽同名的共用自由函数（`field_diff_metric`），
    ///   成员体内一句 `meanAbsPermille(sumAbs, common)` 会被解析成「用 2 个实参调本成员」，
    ///   编译期直接红（实测踩过）。口径本身仍只有一份：本成员只是转发。
    [[nodiscard]] double meanPermille() const;
};

/// 连续场的零位移/平移对拍。🔴 共同格的判定必须与 `overlapOf` 同式（同样只遍历 a、同样按
/// (x+sx, z+sz) 查 b），否则两条读数的分母会静默分家。
[[nodiscard]] RealFieldOverlap overlapRealOf(const GroundRealField& a, const GroundRealField& b,
    int sx, int sz);

/// 平移搜索的读数（未触发搜索时保持零位移初值）。
struct ShiftSearch {
    int x = 0;
    int z = 0;
    size_t mismatch = 0;
    size_t common = 0;  ///< 最优候选处的共同格数（覆盖率 = 这个平移有多少证据）
};

/**
 * 对 B 场做 ±`radius` 格的平移搜索——**最优平移 = 被测场在世界里滑了多少**。
 * 候选覆盖率以零位移共同格数为基准（不足 8/10 不可信，防止把低覆盖的角落平移当最优解
 * ——`swim` 首跑 48 格教训），同失配取覆盖更高者。
 * @param common0 零位移共同格数（覆盖率基准；由调用侧给，避免本函数重复数一遍）
 * @param initialMismatch 零位移失配数（= 搜索的下界初值，搜不到更好的就原样还回去）
 */
[[nodiscard]] ShiftSearch searchShift(const GroundField& a, const GroundField& b, int common0,
    size_t initialMismatch, int radius);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_GROUND_FIELD_METRIC_H
