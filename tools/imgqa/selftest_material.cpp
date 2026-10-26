// selftest_material.cpp —— **材质场世界不变性**（`material-swim`，债-152）的口径层自检。
//
// 全部闭式、不启引擎（判据 4：算子坏了读数也是假的，先钉算子）。被测对象 = 从
// `rig_shadow_field.cpp` 的 swim 里**提出来共用**的那一层（`ground_field_metric.{h,cpp}`）：
// 重投影、共同格计数、平移搜索。它现在同时是 `swim`（影场）与 `material-swim`（材质场）
// 两台仪器的对拍口径 ⟹ 这里红的两台一起瞎，也正因如此它必须有一条独立判据。
//
// ## 拆成五个测试函数的缝
//
// 工具侧认知复杂度上限 15（warn）/ 30（error），十条断言挤一个函数是 40。缝按**问的东西**切：
//   · `testGroundFieldReprojection`  ① 横移不变性（立命前提）+ ② 转向不是平移
//   · `testGroundFieldUnmeasurable`  ③ 三条「这一像素量不到」支路
//   · `testGroundFieldPairing`       ④ 恒等场 / ⑤ 注入平移回收 / ⑥ 无关场反向
//   · `testGroundFieldSearchGuards`  ⑦ 覆盖率下限 / ⑧ 同分 tie-break / ⑨ 四条挂号
//   · `testGroundFieldCellGrid`      格坐标的确定性 + 细格嵌粗格
// 循环体一律抽成有名 helper（`worstTranslationError` 等）——不是审美：断言的**条件**留在
// 测试函数里才看得见，怎么攒出那个数是实现细节。
//
// ## 十条断言与各自的立命点
//   ① **横移不变性**（🔴 整台仪器的立命前提）：相机沿 Δy=0 的任意横移，「同一像素对应的
//      世界点」恰好平移 Δ ⟹ 两机位的场可以在世界上逐格对拍。这条不成立，J1 的「同一世界点
//      必须同标签」就是一句空话。
//   ② 转向**不**给出常量平移（反向用例）：激励必须是平移，旋转对缺陷结构性失明——这条
//      把「为什么不用转向当激励」从注释变成断言。
//   ③ 视线恰平行地面 / 交点在相机背后 / 超出距离上限 三条不可量支路各一条。
//   ④ 恒等场的零位移失配**恰为 0**（阳性锚点）。
//   ⑤ 注入 (7,−3) 格平移必须被 `searchShift` 原样报回、且该处失配恰为 0（区分力闭式）。
//   ⑥ 无关场的最优失配必须显著高于恒等场（堵「搜索恒返回 0 位移 0 失配」）。
//   ⑦ 覆盖率下限：低覆盖候选不许当最优解（swim 首跑 48 格教训）。
//   ⑧ 同分 tie-break：失配相同取覆盖更高者（口径的一部分，不是实现细节）。
//   ⑨ 四条判决线挂号在场（whyTol > 20 字符、guards 非空由 testRegistry 通用强制，这里只查
//      「四条都在注册表里」——漏 append = 判据名打出来了但 `imgqa check` 里查不到）。
//
// ## 变异红线（哪个变异让我红）
//   · 重投影里 `nx` 的 aspect 漏乘 → ① 红（横移不再是常量平移）。
//   · `t <= 0` 守卫删掉 → ③「相机背后」红（负距离的格被当成有效样本）。
//   · `searchShift` 的覆盖率下限删掉 → ⑦ 红（低覆盖角落平移被当最优）。
//   · `mm == best.mismatch && n > bestN` 改成 `>=` → ⑧ 红（同分不再取覆盖高者）。
//   · `overlapOf` 的平移查表符号写反 → ⑤ 红（报回 (−7, +3)）。
//   · 判据名拼错或漏 append → ⑨ 红。
#include "selftest.h"

#include "ground_field_metric.h"
#include "imgqa.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace RGL::imgqa {

namespace {

/// 机位 A（与 `material_swim.cpp` 的量具常数同形态；这里是合成输入，不 import 那侧的常量：
/// 自检要能在量具挪机位时**仍然**钉住口径，而不是跟着一起挪）。
const std::array<double, 3> kPosA { { 0.0, 1.6, 4.0 } };
const std::array<double, 3> kTgtA { { 0.0, -3.0, -6.0 } };
constexpr int kW = 1280, kH = 720;
constexpr double kFov = 45.0;

/// 确定性伪随机二值场（Knuth MMIX LCG，写死种子——不许用 `std::random_device`：自检必须
/// 跨机器逐位可复现，同 skill rgl-test-discipline 的确定性条）。
GroundField synthField(int w, int h, uint64_t seed)
{
    GroundField f;
    uint64_t s = seed;
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            f[{ x, z }] = ((s >> 33) & 1U) != 0U;
        }
    return f;
}

/// 把整场平移 (sx,sz) 格（= 「图案在世界里滑了 sx·格边长」的合成形态）。
GroundField shiftField(const GroundField& a, int sx, int sz)
{
    GroundField b;
    for (const auto& [c, label] : a)
        b[{ c.first + sx, c.second + sz }] = label;
    return b;
}

bool hit(const GroundCamera& cam, int x, int y, glm::dvec2& w, double maxD = 5000.0)
{
    return groundHitOf(cam, x, y, maxD, w);
}

/// 两枚机位下「同一像素 → 世界点」之差的最大偏离（期望 = 恰为相机平移量 d）。
/// 采样点跨画面四角与中列、跨近远行——只测中心一像素的话，aspect/fov 写错也可能被
/// 「同一处错」掩盖。
double worstTranslationError(const GroundCamera& a, const GroundCamera& b, double d, int& samples)
{
    double worst = 0.0;
    samples = 0;
    for (const int y : { 400, 520, 640, 700 })
        for (const int x : { 0, 213, 640, 1067, 1279 }) {
            glm::dvec2 wa, wb;
            if (!hit(a, x, y, wa) || !hit(b, x, y, wb))
                continue;
            worst = std::max(worst, std::abs((wb.x - wa.x) - d));
            worst = std::max(worst, std::abs(wb.y - wa.y));
            ++samples;
        }
    return worst;
}

/// 只改 target（原地转向）时，「同一像素的世界点之差」在画面两角之间的**极差**。
/// 常量平移 ⟹ 极差 0；转向 ⟹ 随深度变，极差显著非零。
double rotationDeltaSpread(const GroundCamera& a, const GroundCamera& rot, int& samples)
{
    double first = 0.0, last = 0.0;
    samples = 0;
    for (const int y : { 400, 700 })
        for (const int x : { 0, 1279 }) {
            glm::dvec2 wa, wr;
            if (!hit(a, x, y, wa) || !hit(rot, x, y, wr))
                continue;
            const double delta = wr.x - wa.x;
            if (samples == 0)
                first = delta;
            last = delta;
            ++samples;
        }
    return std::abs(last - first);
}

/// 沿中列从上往下第一条能量到的视线，其交点离相机的距离（米）；全量不到 = 0。
double firstHitDistance(const GroundCamera& a, int x)
{
    for (int y = 0; y < kH; ++y) {
        glm::dvec2 w;
        if (hit(a, x, y, w))
            return std::hypot(w.x - kPosA[0], w.y - kPosA[2]);
    }
    return 0.0;
}

/// 判据名是否在注册表里（⑨ 用；容差理由与 guard 的**内容**由 testRegistry 通用强制）。
bool hasJudgment(const char* name)
{
    for (const auto& j : registeredJudgments())
        if (j.name == std::string(name))
            return true;
    return false;
}

/// ⑦ 的输入：A 翻掉若干格 + 贴一小块「与 A 在 +60 格平移处完全吻合」的诱饵。
/// 诱饵处失配恰为 0 但共同只有 9 格 ⟹ 没有覆盖率下限就被它抢走最优解（swim 首跑 48 格
/// 教训的同形）。循环抽出来是为了让测试函数里只剩断言条件。
GroundField fieldWithLowCoverageDecoy(const GroundField& f)
{
    GroundField b;
    int i = 0;
    for (const auto& [cell, label] : f) {
        b[cell] = ((i / 24) % 100 == 0 && (i % 24) == 0) ? !label : label;
        ++i;
    }
    for (int z = 0; z < 3; ++z)
        for (int x = 0; x < 3; ++x)
            b[{ 60 + x, z }] = f.at({ x, z });
    return b;
}

/// ⑧ 的输入：隔一个取一个 ⟹ 零位移处失配恰为 0 而覆盖最大（同分候选里覆盖最高的是它）。
GroundField halfOf(const GroundField& f)
{
    GroundField half;
    int k = 0;
    for (const auto& [cell, label] : f)
        if ((k++ & 1) == 0)
            half[cell] = label;
    return half;
}

}  // namespace

void testGroundFieldReprojection(Checker& c)
{
    const GroundCamera a = makeGroundCamera(kPosA, kTgtA, kFov, kW, kH);
    const double d = 0.5;
    const GroundCamera b = makeGroundCamera({ d, 1.6, 4.0 }, { d, -3.0, -6.0 }, kFov, kW, kH);

    // ① 横移不变性（立命前提）
    int samples = 0;
    const double worst = worstTranslationError(a, b, d, samples);
    c.chk("groundHit·横移不变性：相机沿 +X 平移 d ⟹ 同一像素的世界点恰平移 d（与深度无关）",
        samples >= 15 && worst < 1e-3,
        "样本 " + std::to_string(samples) + "，最大偏差 " + std::to_string(worst));

    // ② 反向：转向不是常量平移
    const GroundCamera rot = makeGroundCamera(kPosA, { 2.0, -3.0, -6.0 }, kFov, kW, kH);
    int rotSamples = 0;
    const double spread = rotationDeltaSpread(a, rot, rotSamples);
    c.chk("groundHit·转向不是常量平移（⟹ 激励必须用平移，旋转对缺陷结构性失明）",
        rotSamples >= 3 && spread > 0.05, "两角之差极差 = " + std::to_string(spread));
}

void testGroundFieldUnmeasurable(Checker& c)
{
    const GroundCamera a = makeGroundCamera(kPosA, kTgtA, kFov, kW, kH);
    glm::dvec2 w;
    // 「恰平行」这条要 dir.y **恰为 0**：ny 恰为 0 要求 (y+0.5)/H·2 = 1，y 是整数 ⟹ 取
    // **H = 1、y = 0**。水平相机（fw.y = rt.y = 0）配 ny = 0 ⟹ dir.y 恰为 0，才真走进
    // |dir.y| < 1e-6 那条支路（拿 720 行的中心行测它，dir.y ≈ 5.8e−4 比 1e−6 大三个数量级
    // = 测的是「这条能量到」，不是守卫本身）。
    const GroundCamera horiz = makeGroundCamera({ 0.0, 1.6, 0.0 }, { 0.0, 1.6, -10.0 }, kFov, 2,
        1);
    c.chk("groundHit·视线恰平行地面（dir.y 恰为 0）→ false", !hit(horiz, 0, 0, w));

    const GroundCamera up = makeGroundCamera({ 0.0, 1.6, 0.0 }, { 0.0, 6.0, -10.0 }, kFov, kW,
        kH);
    c.chk("groundHit·抬头看（交点在相机背后 t≤0）→ false", !hit(up, 640, 100, w));

    // 距离上限这条**自洽**测：同一条视线先量到、按它的实际距离砍一半就量不到（不手算米数——
    // 手算值一旦随重投影口径漂就是假绿）。
    const double farD = firstHitDistance(a, 640);
    glm::dvec2 w2;
    c.chk("groundHit·超出距离上限 → false，放开上限同一条视线必须可量（阳性对照）",
        farD > 0.0 && !groundHitOf(a, 640, 0, farD * 0.5, w2)
            && groundHitOf(a, 640, 0, farD * 2.0, w2),
        "最远可量交点距离 = " + std::to_string(farD));
}

void testGroundFieldPairing(Checker& c)
{
    const GroundField f = synthField(60, 40, 0x2545F491ULL);

    // ④ 恒等场：零位移失配恰为 0（阳性锚点）
    const FieldOverlap same = overlapOf(f, f, 0, 0);
    c.chk("overlapOf·恒等场零位移失配恰为 0（阳性锚点）",
        same.common == static_cast<int>(f.size()) && same.mismatch == 0,
        std::to_string(same.common) + "/" + std::to_string(same.mismatch));

    // ⑤ 注入 (7,−3) 格必须原样报回，且该处失配恰为 0
    const GroundField g = shiftField(f, 7, -3);
    const ShiftSearch got = searchShift(f, g, same.common, 1u << 20, 12);
    c.chk("searchShift·注入 (7,−3) 格平移 → 原样报回且该处失配恰为 0（区分力闭式）",
        got.x == 7 && got.z == -3 && got.mismatch == 0,
        "(" + std::to_string(got.x) + "," + std::to_string(got.z) + ") 失配 "
            + std::to_string(got.mismatch));

    // ⑥ 无关场：最优失配必须显著高于恒等场（堵「搜索恒返回 0 失配」）
    const GroundField u = synthField(60, 40, 0x1234ABCDULL);
    const ShiftSearch noise = searchShift(f, u, same.common, 1u << 20, 12);
    c.chk("searchShift·无关二值场的最优失配 ≥ 共同格的 30%（反向用例：算子不是恒绿）",
        noise.common > 0 && noise.mismatch >= noise.common * 3 / 10,
        std::to_string(noise.mismatch) + "/" + std::to_string(noise.common));
}

void testGroundFieldSearchGuards(Checker& c)
{
    const GroundField f = synthField(60, 40, 0x2545F491ULL);

    // ⑦ 覆盖率下限（输入构造见 fieldWithLowCoverageDecoy）
    const GroundField b = fieldWithLowCoverageDecoy(f);
    const int common0 = overlapOf(f, b, 0, 0).common;
    const int decoy = overlapOf(f, b, 60, 0).common;
    const ShiftSearch cov = searchShift(f, b, common0, 1u << 20, 64);
    c.chk("searchShift·覆盖率不足 8/10 的候选不当最优解（低覆盖角落平移不是证据）",
        common0 > 0 && decoy > 0 && decoy < common0 / 10 && cov.x == 0 && cov.z == 0,
        "零位移共同 " + std::to_string(common0) + "，诱饵共同 " + std::to_string(decoy)
            + "，最优 (" + std::to_string(cov.x) + "," + std::to_string(cov.z) + ")");

    // ⑧ 同分 tie-break：B 是 A 的一半（零位移处失配 0 且覆盖最大）⟹ 最优必须是零位移。
    const GroundField half = halfOf(f);
    const int halfCommon = overlapOf(f, half, 0, 0).common;
    const ShiftSearch tie = searchShift(f, half, halfCommon, 1u << 20, 6);
    c.chk("searchShift·子集场的最优平移是零位移且失配恰为 0（同分取覆盖高者）",
        tie.x == 0 && tie.z == 0 && tie.mismatch == 0,
        "(" + std::to_string(tie.x) + "," + std::to_string(tie.z) + ") 失配 "
            + std::to_string(tie.mismatch));

    // ⑨ 四条判决线挂号在场（容差理由与 guard 的**内容**由 testRegistry 通用强制）
    for (const char* name : { "material-swim-world-invariant", "material-swim-determinism",
            "material-swim-gate-closed-floor", "material-swim-offset-sensitivity" })
        c.chk("挂号：material-swim 四条判决线在注册表里", hasJudgment(name), name);
}

void testGroundFieldCellGrid(Checker& c)
{
    // 格坐标 = floor(世界 / 格边长)：同一世界点在固定格边长下必须落进同一格（确定性），
    // 而细格必须**嵌**在粗格里（floor 不与缩放交换，所以断言是包含而不是 50 倍）。
    // `material-swim` 的两机位对拍**完全**建立在「同一个世界点 → 同一个格」上（横移不变性
    // 保证两机位看到的世界点集是平移关系）。
    const GroundCamera a = makeGroundCamera(kPosA, kTgtA, kFov, kW, kH);
    GroundCell c1 {}, c2 {};
    const bool ok1 = groundCellOf(a, 640, 500, 0.05, 5000.0, c1);
    const bool ok2 = groundCellOf(a, 640, 500, 0.05, 5000.0, c2);
    c.chk("groundCellOf·同像素同格边长 → 同一格（确定性）",
        ok1 && ok2 && c1 == c2, ok1 ? "ok" : "量不到");
    GroundCell wide {};
    const bool ok3 = groundCellOf(a, 640, 500, 0.001, 5000.0, wide);
    c.chk("groundCellOf·格边长细化 50 倍 → 细格嵌进同一枚粗格（刻度真的进了分母）",
        ok1 && ok3 && wide.first / 50 == c1.first && wide.second / 50 == c1.second,
        std::to_string(wide.first) + "/50 vs " + std::to_string(c1.first));
}

}  // namespace RGL::imgqa
