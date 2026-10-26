// selftest_field_diff.cpp —— **连续量差统计口径层**（`field_diff_metric.{h,cpp}`）的自检。
//
// 全部闭式、不启引擎、不读图（判据：算子坏了读数也是假的，先钉算子）。这层是债-154 补位的
// 「配对完之后怎么数」那一份口径，**同时**被 `shadow-swim-flat`（影场 |Δcov|）与
// `material-swim`（材质场 |Δroughness|）消费 ⟹ 这里红的两台仪器一起瞎，也正因如此它必须有
// 一条**独立**判据（不能靠两台仪器各自的绿来反证——两台可以一起错，因为它们吃的是同一份算子）。
//
// ## 为什么这层值得一个独立 TU 的自检（而不是搭在 swim_flat 那边）
// 它只有两枚函数，但两枚都是**跨仪器可比性的唯一支点**：
//   · `meanAbsPermille` 的 ×1000 与「空集回 0」；
//   · `p99Abs` 的**下标口径** `floor(0.99·(n−1))`。
// 两条各自漂一点，两台仪器报出的同名统计量就不可比，而**没有任何一条判据会红**（判据比的是
// 各自的界，不是「两台的 p99 是不是同一个函数」）。这正是 AGENTS 哲学第 1 条点名的形态。
//
// ## 断言与各自的立命点（逐条对应头注的「变异红线」）
//   ① mean 的千分点口径：闭式样本 {1,2,3}/1000 → 恰 2 ‰。
//      🔴 变异「忘了 ×1000」→ 本条红（判据界写的是 ‰，漂成比例口径 = 界被静默放宽 1000 倍，
//      而 0.271 与 0.000271 在打印精度下都像是「远小于 1」= 看不出来）。
//   ② mean 空集 → **0.0 且不是 NaN**（反向用例）。
//      🔴 变异「空集返回 NaN」→ 本条红，且它同时说明为什么必须是 0：NaN 参与 `<=` 恒假
//      ⟹ 判据「mean ≤ 界」变假 ⟹ 但 FAIL 打印会显示 `nan`，看着像数据坏了而不是判据失效；
//      更糟的是有些调用点写 `!(x > 界)`（恒真 = 绿）。分母是彩票这件事归各仪器的规模闸门。
//   ③ p99 的**下标口径**（🔴 本层最要紧的一条）：n=101 且样本 = i/1000（i=0..100）
//      ⟹ 下标 floor(0.99·100) = 99 ⟹ p99 **恰为 0.099**。
//      这条同时钉死三种「换个分位口径」的漂法：取 `ceil(0.99n)`（→ 0.100）、取 `n-1`（→ 0.100）、
//      取 `floor(0.99n)`（→ 0.099 但 n=1 时下标 0 与 0.99·0=0 同，测不出差别，故另配 ④）。
//   ④ p99 在 n=1 / n=2 / n=1000 上的闭式值（小样本半边 + 大样本半边）。
//      n=1 → 只能是那一个样本本身；n=2 → 下标 floor(0.99)=0 = **较小者**（不是较大者！这条
//      是刻意的：本层的 p99 是「保守取低」，与影场那边实测的 p99 数值逐位同源，改这个约定
//      会挪动 `shadow-swim-flat` 四格里所有 p99 读数）。
//   ⑤ 🔴 **反向用例：p99 既不是 max 也不是 median**（偏置样本集上三者必须两两不同）。
//      这条堵两种相反的失效：写成「取最大值」（尾部判据失去「分位」含义，一个离群点就顶穿界）
//      与写成「取中位数」（尾部整个看不见 = 债-154 说的「只看一个统计量会读错结论」的算子版）。
//   ⑥ p99 会打乱入参（文档化的副作用）+ 打乱后**再取一次值不变**（幂等，防止「第二次调拿到
//      另一个数」这种只在打印两次时才暴露的漂）。
//   ⑦ 空集 p99 → 0.0（同 ② 的理由）。
//   ⑧ 挂号：八条判决线在注册表里（漏 append = 判据名打出来了但 `imgqa check` 查不到）。
//
// ## 🔴 变异实跑读数（2026-10-26 本机；工装 = stdlib-only Python，构建带失败重试，
// ##    每条变异还原后重跑基线并断言全绿）
//   · M2 `p99Abs` 下标 0.99 → 0.50（= 取中位数）→ **本层红 4 条**：
//     `p99 下标口径`（读到 0.050000，期望 0.099）、`n=1000`（0.499000，期望 0.989）、
//     `p99 ≠ median`（p99 = 0.001000）、`落在闭式解上`（0.001000，期望 0.500）。
//     🔴 同一条变异在**仪器层两种写法下都还是 PASS**（`material-swim` 八条全绿）——
//     这就是「统计量定义的变异归闭式层、不归仪器层」的实测依据，也是本层必须独立存在的理由。
//   · M3 `meanAbsPermille` 忘乘 1000 → **本层红 2 条**（`千分点口径` 读到 0.002000、
//     `随 Σ|Δ| 线性` 读到 0.004000），且仪器层 J8 同时变红（注入的 mean 掉到界以下）。
//   · 恒等/反向锚点：`p99 ≠ max`、`p99 ≠ median`（偏置样本集上三者两两不同）、
//     空集 mean/p99 均回 **0.0 且非 NaN**（NaN 参与 `<=` 恒假 = 判据静默失效）。
//   · 幂等：`p99Abs` 会打乱入参（nth_element，文档化的副作用），第二次调必须给同一个数。
//
//
// ## 为什么样本集用「千分点整数」而不是随机数
// 全部样本都取 `i/1000` 这种在二进制下**不精确**但差值可闭式核对的形态，断言一律用
// `std::abs(x - 期望) < 1e-12` 而不是逐位相等：本层要钉的是**口径**（哪个下标、乘不乘 1000），
// 不是浮点舍入。写 `==` 会造出一条永远因为 1e-17 而红的线（同 selftest_swim_flat 里
// 「Σ|Δcov| 落在浮点闭合级而不是逐位 0」那条实测教训）。
#include "selftest.h"

#include "field_diff_metric.h"
#include "imgqa.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

/// 样本集 i/1000（i = 0 .. n-1），升序。
std::vector<double> ramp(int n)
{
    std::vector<double> v;
    v.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        v.push_back(static_cast<double>(i) / 1000.0);
    return v;
}

/// 判据名是否在注册表里（⑧ 用；容差理由与 guard 的**内容**由 testRegistry 通用强制）。
bool hasJudgment(const char* name)
{
    for (const auto& j : registeredJudgments())
        if (j.name == std::string(name))
            return true;
    return false;
}

constexpr double kEps = 1e-12;

}  // namespace

void testFieldDiffMean(Checker& c)
{
    // ① 千分点口径：{0.001, 0.002, 0.003} → mean = 0.002 → 恰 2 ‰。
    const double got = meanAbsPermille(0.001 + 0.002 + 0.003, 3);
    c.chk("field_diff·mean 是**千分点**口径（Σ|Δ|/n × 1000；忘乘 1000 = 界被放宽 1000 倍）",
        std::abs(got - 2.0) < kEps, "读到 " + std::to_string(got));

    // ①b 分母真的进了除法（不是累加器直接返回）：同样本数 ×2 的和必须给 2 倍均值。
    const double dbl = meanAbsPermille(2.0 * (0.001 + 0.002 + 0.003), 3);
    c.chk("field_diff·mean 随 Σ|Δ| 线性（分母没漏、没被写成样本总数）",
        std::abs(dbl - 4.0) < kEps, "读到 " + std::to_string(dbl));

    // ② 反向用例：空集必须回 0.0，且**不是 NaN**（NaN 让 `<=` 恒假 = 判据静默失效）。
    const double empty = meanAbsPermille(0.0, 0);
    c.chk("field_diff·空集 mean = 0.0 且非 NaN（NaN 参与 <= 恒假 ⟹ 判据假绿/假红不可控）",
        empty == 0.0 && !std::isnan(empty), "读到 " + std::to_string(empty));
}

void testFieldDiffP99Index(Checker& c)
{
    // ③ 🔴 下标口径的唯一钉法：n=101 的斜坡，floor(0.99·100) = 99 ⟹ 恰 0.099。
    auto r = ramp(101);
    const double p = p99Abs(r);
    c.chk("field_diff·p99 下标口径 = floor(0.99·(n−1))（n=101 斜坡 → 恰 0.099）",
        std::abs(p - 0.099) < kEps, "读到 " + std::to_string(p));

    // ④ 小样本与大样本两半边（只测 n=101 的话，任何「凑巧落在 99」的口径都能过）。
    auto one = ramp(1);
    c.chk("field_diff·n=1 → p99 就是那一个样本（不许回 0，也不许 NaN）",
        std::abs(p99Abs(one)) < kEps, "读到 " + std::to_string(p99Abs(one)));
    auto two = ramp(2);
    const double p2 = p99Abs(two);
    // 下标 floor(0.99·1) = 0 = 较小者。这条**刻意**钉低：改成「取较大者」会挪动影场四格里
    // 每一个 p99 读数（那边实测 3.922 ‰ 就是这么来的），是跨仪器可比性的一部分。
    c.chk("field_diff·n=2 → 下标 0 = 较小者（本层 p99 是保守取低；改约定 = 挪动两台仪器所有 p99）",
        std::abs(p2 - 0.0) < kEps, "读到 " + std::to_string(p2));
    auto big = ramp(1000);
    const double pb = p99Abs(big);
    // 下标 = floor(0.99·999) = floor(989.01) = **989** ⟹ 样本 989/1000 = 0.989（不是 0.990——
    // 这条断言写错过一次，红得正好：它说明「0.99·n 还是 0.99·(n−1)」这个区别在本量具的
    // n（数万格）上就是**一个样本**的差别，而两台仪器共用口径时必须咬死同一个下标式）。
    c.chk("field_diff·n=1000 → 恰 0.989（大样本半边，与 n=101 那条一起排除「固定下标」）",
        std::abs(pb - 0.989) < kEps, "读到 " + std::to_string(pb));

    // ⑦ 空集 p99 → 0.0（同 mean 的理由）。
    std::vector<double> none;
    const double pe = p99Abs(none);
    c.chk("field_diff·空集 p99 = 0.0 且非 NaN", pe == 0.0 && !std::isnan(pe),
        "读到 " + std::to_string(pe));
}

void testFieldDiffP99NotMaxNorMedian(Checker& c)
{
    // ⑤ 🔴 反向用例（本层最要紧的一条）：偏置样本集上 p99 **既不是 max 也不是 median**。
    // 构造 90 个 0.001 + 9 个 0.500 + 1 个 0.900（n=100）：
    //     median = 0.001、max = 0.900、p99（下标 floor(0.99·99) = 98 → 升序第 99 个）= 0.500
    // ⟹ 三者两两不同，一条断言堵两种相反的失效：
    //   · 写成「取最大值」→ 尾部判据失去分位含义，一个离群点就顶穿界；
    //   · 写成「取中位数」→ 尾部整个看不见（= 债-154 那种「只看一个统计量会读错结论」的算子版）。
    std::vector<double> v;
    for (int i = 0; i < 90; ++i)
        v.push_back(0.001);
    for (int i = 0; i < 9; ++i)
        v.push_back(0.500);
    v.push_back(0.900);
    const double p = p99Abs(v);
    c.chk("field_diff·p99 ≠ max（写成取最大值 = 一个离群点顶穿界，尾部判据失去分位含义）",
        std::abs(p - 0.900) > 1e-9, "p99 = " + std::to_string(p));
    c.chk("field_diff·p99 ≠ median（写成取中位数 = 尾部整个看不见，正是债-154 那种失明）",
        std::abs(p - 0.001) > 1e-9, "p99 = " + std::to_string(p));
    c.chk("field_diff·p99 落在闭式解上（n=100 → 下标 98 → 升序第 99 个 = 0.500）",
        std::abs(p - 0.500) < kEps, "读到 " + std::to_string(p));

    // ⑥ 文档化的副作用：`p99Abs` 用 nth_element，**会打乱入参**；但再取一次必须给同一个数。
    // 不钉幂等性的后果很具体：仪器里「先取 p99、再打印一次 p99」会给出两个不同的数，
    // 而排查时第一反应是「数据变了」，不是「算子不幂等」。
    const double again = p99Abs(v);
    c.chk("field_diff·p99 幂等（打乱入参是文档化的副作用，第二次调必须给同一个数）",
        std::abs(again - p) < kEps, "第一次 " + std::to_string(p) + " 第二次 "
            + std::to_string(again));
}

void testFieldDiffRegistration(Checker& c)
{
    // ⑧ 挂号：material-swim 八条 + shadow-swim-flat 五条都在注册表里。
    // 连续量口径被两台仪器共用 ⟹ 本层的自检顺手把**两台**的判据名一起钉住：任何一条被改名
    // 或漏 append，都会在这里红，而不是等到那台命令跑起来才发现「判据打出来了但查不到理由」。
    for (const char* name : {"material-swim-world-invariant", "material-swim-determinism",
             "material-swim-gate-closed-floor", "material-swim-offset-sensitivity",
             "material-swim-continuous-floor", "material-swim-continuous-no-harm",
             "material-swim-continuous-determinism", "material-swim-subthreshold-visible" })
        c.chk("挂号：material-swim 八条判决线在注册表里（连续量四条由本层守口径）",
            hasJudgment(name), name);
}

}  // namespace RGL::imgqa
