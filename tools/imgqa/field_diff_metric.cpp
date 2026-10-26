// field_diff_metric.cpp —— 连续量差的 mean / p99 口径（为什么在这一层、泛化边界 → 同名 .h）。
//
// 本文件只有两枚算子，且**都不设阈值**：判界住各仪器的 judge TU。
#include "field_diff_metric.h"

#include <algorithm>
#include <cmath>

namespace RGL::imgqa {

double meanAbsPermille(double sumAbs, std::size_t n)
{
    // n == 0 → 0.0：见头注「空集必须回 0，不许 NaN」那条（NaN 比不出大小 ⟹ 判据恒假 ⟹ 绿）。
    if (n == 0)
        return 0.0;
    return 1000.0 * sumAbs / static_cast<double>(n);
}

double p99Abs(std::vector<double>& absDiffs)
{
    if (absDiffs.empty())
        return 0.0;
    // 🔴 下标口径的唯一出处（头注）：0.99 落在**最后一个样本**上（n=1 → 下标 0），而不是
    // 「第 99% 个位置」那种会把 n 较小时的尾部整个够不到的写法。nth_element 只要那一个分位，
    // O(n)；sort 是 O(n log n) 而材质场那边每次对拍要跑 4 对 × 3 万格，白付的 log 不值当。
    const std::size_t idx = static_cast<std::size_t>(
        0.99 * static_cast<double>(absDiffs.size() - 1));
    std::nth_element(absDiffs.begin(), absDiffs.begin() + static_cast<long>(idx),
        absDiffs.end());
    return absDiffs[idx];
}

}  // namespace RGL::imgqa
