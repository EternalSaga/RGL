// ground_field_metric.cpp —— 世界地面场的对拍口径（声明与「为什么在这一层」→ 同名 .h）。
//
// 本文件**只有口径**：重投影、共同格计数（二值与连续两套，🔴 同式）、平移搜索。任何
// 「这个逐像素量从哪来」（luma 阈值 / G-buffer 字节阈值 / 掩膜选择）都不在这里——那是各仪器
// 自己的传感部分，住各自的 TU。连续量的 mean/p99 **怎么数**也不在这里，在 `field_diff_metric`。
//
// 🔴 逐式与 `rig_shadow_field.cpp` 改前的 swim 口径**逐字相同**（只把硬编码的 0.05 m 格边长、
// 5000 m 交点上限、±10 格搜索半径提成参数）。为什么必须逐字：`shadow-swim` 的判据是
// 「零位移失配 ≤ max(20, 共同/200)」这种**计数**阈值，重投影挪半格就会改它的读数。
#include "ground_field_metric.h"
#include "field_diff_metric.h"  // meanAbsPermille / p99Abs（与影场共用的连续量口径）

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include <glm/glm.hpp>

namespace RGL::imgqa {

GroundCamera makeGroundCamera(const std::array<double, 3>& position,
    const std::array<double, 3>& target, double fovDegrees, int width, int height)
{
    const glm::vec3 pos(static_cast<float>(position[0]), static_cast<float>(position[1]),
        static_cast<float>(position[2]));
    const glm::vec3 tgt(static_cast<float>(target[0]), static_cast<float>(target[1]),
        static_cast<float>(target[2]));
    const glm::vec3 fw = glm::normalize(tgt - pos);
    const glm::vec3 rt = glm::normalize(glm::cross(fw, glm::vec3(0.f, 1.f, 0.f)));
    GroundCamera cam;
    cam.pos = pos;
    cam.fw = fw;
    cam.rt = rt;
    cam.up = glm::cross(rt, fw);
    cam.tanH = std::tan(fovDegrees * 3.14159265358979 / 360.0);
    cam.width = width;
    cam.height = height;
    return cam;
}

bool groundHitOf(const GroundCamera& cam, int x, int y, double maxDistanceM, glm::dvec2& world)
{
    const double nx = (x + 0.5) / static_cast<double>(cam.width) * 2.0 - 1.0;
    const double ny = 1.0 - (y + 0.5) / static_cast<double>(cam.height) * 2.0;
    const double aspect = static_cast<double>(cam.width) / static_cast<double>(cam.height);
    const glm::vec3 dir = glm::normalize(cam.fw
        + cam.rt * static_cast<float>(nx * cam.tanH * aspect)
        + cam.up * static_cast<float>(ny * cam.tanH));
    if (std::abs(dir.y) < 1e-6)
        return false;  // 视线近平行地面：交点在无穷远，这一像素量不到
    // ⚠ 这条除法**刻意保持 float**（`-cam.pos.y` 与 `dir.y` 都是 float）：swim 改前就是它，
    //  换成 double 会挪动落在格边界上的像素，而 `shadow-swim` 的判据是计数阈值 = 改判据。
    const double t = -cam.pos.y / dir.y;
    if (t <= 0.0 || t > maxDistanceM)
        return false;  // 交点在相机背后 / 远到不可信
    const glm::vec3 w = cam.pos + dir * static_cast<float>(t);
    world = glm::dvec2(w.x, w.z);
    return true;
}

bool groundCellOf(const GroundCamera& cam, int x, int y, double cellM, double maxDistanceM,
    GroundCell& cell)
{
    glm::dvec2 w;
    if (!groundHitOf(cam, x, y, maxDistanceM, w))
        return false;
    cell = std::make_pair(static_cast<int>(std::floor(w.x / cellM)),
        static_cast<int>(std::floor(w.y / cellM)));
    return true;
}

FieldOverlap overlapOf(const GroundField& a, const GroundField& b, int sx, int sz)
{
    FieldOverlap o;
    for (const auto& [c, label] : a) {
        const auto hit = b.find({ c.first + sx, c.second + sz });
        if (hit == b.end())
            continue;
        ++o.common;
        if (hit->second != label)
            ++o.mismatch;
    }
    return o;
}

double RealFieldOverlap::meanPermille() const
{
    // 🔴 不在这里定义千分点口径——`field_diff_metric` 是影场与材质场共用的那一份。
    return meanAbsPermille(sumAbs, static_cast<std::size_t>(common));
}

RealFieldOverlap overlapRealOf(const GroundRealField& a, const GroundRealField& b, int sx,
    int sz)
{
    // 🔴 循环体与 `overlapOf` **逐字同式**（同样只遍历 a、同样按 (x+sx, z+sz) 查 b）：
    // 两条读数必须吃同一批共同格，否则「翻转率绿而连续量红」这个债-154 的结论比的是两批格。
    // 之所以是两份实现而不是一份模板：两者的值类型（bool / double）与统计量都不同，抽成
    // 模板会让「二值那条要不要记 mismatch」这种差异挤进同一个函数体，读的人再也看不出
    // 两条口径是否还同式——而这条同式关系本身就是判据的前提。
    RealFieldOverlap o;
    std::vector<double> diffs;
    diffs.reserve(a.size());
    for (const auto& [c, value] : a) {
        const auto hit = b.find({ c.first + sx, c.second + sz });
        if (hit == b.end())
            continue;
        ++o.common;
        diffs.push_back(std::abs(value - hit->second));
    }
    if (!diffs.empty()) {
        o.sumAbs = std::accumulate(diffs.begin(), diffs.end(), 0.0);
        o.p99Abs = p99Abs(diffs);  // 🔴 分位下标口径的唯一出处（会打乱 diffs 顺序，此后不用它）
    }
    return o;
}

ShiftSearch searchShift(const GroundField& a, const GroundField& b, int common0,
    size_t initialMismatch, int radius)
{
    ShiftSearch best;
    best.mismatch = initialMismatch;
    best.common = overlapOf(a, b, 0, 0).common;
    size_t bestN = best.common;
    for (int sz = -radius; sz <= radius; ++sz)
        for (int sx = -radius; sx <= radius; ++sx) {
            const FieldOverlap o = overlapOf(a, b, sx, sz);
            const size_t mm = static_cast<size_t>(o.mismatch);
            const size_t n = static_cast<size_t>(o.common);
            if (n < static_cast<size_t>(common0) * 8 / 10)
                continue;  // 覆盖率不足，该候选不可信
            if (mm < best.mismatch || (mm == best.mismatch && n > bestN)) {
                best.mismatch = mm;
                best.common = n;
                bestN = n;
                best.x = sx;
                best.z = sz;
            }
        }
    return best;
}

}  // namespace RGL::imgqa
