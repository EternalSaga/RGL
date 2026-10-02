module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>            // vec2/vec4（与 shader 形态同形；不经 BMI 保证传递，GMF 自带）
#include <cmath>                  // std::exp/std::log/std::sqrt（自由函数同上）
#include <algorithm>              // std::clamp/std::max/std::min（同上）

// EVSSM 消费端公式（rgl/shaders/shadow.slang 的 chebyshevUpperBound / evaluateEvssm /
// zBlocker 解码 / evssm_moments 的矩编码）的数学契约回归——本文件是这些式的 **C++ 侧
// 镜像复刻**（同 test_shadow_forward 的先例）：shader 侧公式任何改动必须同步本文件。
//
// ## 钉住的历史 bug（docs/调试历史.md 2026-10-02）
//
// 转写 Filament chebyshevUpperBound 时漏掉首行快路（depth ≤ moments.x → 1.0）：
// d² 对负 d 同样增长，「亮于均值」与「暗于均值」给出同向 ≈0 = 亮区判暗。实机证据 =
// forward 门开读零矩 fallback 时 176821 px **变暗**（方向与「影子消失」相反）。
// 同 commit 修 fallback：零矩 → far 面最大矩（零矩 = 恒暗，见「零矩全暗」用例）。
// 本测试的「far 最大矩恒全亮」「零矩恒暗」两条即该 bug 的数学正面锚点——快路删除后
// 前者立刻红。
//
// ## 期望值出处（独立于被测复刻式）
//
// · 受光量语义（1=全亮）与快路：Filament surface_shadowing.fs `visibility = shadow(...)`
//   + chebyshevUpperBound 首行快路（源头语义，非本复刻式）；
// · fp16 上限 65504 = (2−2⁻¹⁰)·2¹⁵（IEEE 754 binary16 定义）；
// · 其余端点（0/1/单调）= Chebyshev 不等式与 saturate 的解析结果。
//
// ## c 的单源锚
//
// kC = vk_resources::kEvssmExponent（唯一权威源，消费端经 scene data 常量块取同值）。
// fp16 域用例直接对该常量断言——改常量若破坏 exp(2c) ≤ 65504 立刻红。

export module test_evssm_math;

import rlstd;
import test_support;
import vk_resources;  // kEvssmExponent（唯一权威源；rlgfx 编译期依赖，运行期零设备接触）

namespace RGL {
namespace {

using testing::expectNear;

// —— 复刻式（与 shadow.slang / evssm_moments.comp.slang 逐式同形，float32 域）——
constexpr float kC = kEvssmExponent;        // 权威源，非本地字面量
constexpr float kLbr = 0.1f;                // shadow.slang kEvssmLbr
constexpr float kEps = 0.002f;              // shadow.slang kEvssmEpsilon

/// evssm_moments.comp.slang 矩编码：z = exp(c·(2d−1))，moments = (z, z², −1/z, 1/z²)。
glm::vec4 encodeMoments(float d)
{
    const float z = std::exp(kC * (d * 2.0f - 1.0f));
    return {z, z * z, -1.0f / z, 1.0f / (z * z)};
}

/// shadow.slang evssmCoverage STEP1 的 zBlocker 解码（encodeMoments 的逆 + clamp）。
float decodeZBlocker(const glm::vec4& moments, float refZ)
{
    const float negMoment = std::max(-moments.z, 1e-8f);
    const float raw = (std::log(negMoment) / -kC + 1.0f) * 0.5f;
    return std::clamp(raw, 0.0f, refZ);
}

/// shadow.slang chebyshevUpperBound（含快路——本文件钉的死条款）。
float chebyshevUpperBound(const glm::vec2& moments, float depth, float minVariance, float lbr)
{
    if (depth <= moments.x)
        return 1.0f;
    const float variance = std::max(moments.y - moments.x * moments.x, minVariance);
    const float d = depth - moments.x;
    const float pMax = variance / (variance + d * d);
    return std::clamp((pMax - lbr) / (1.0f - lbr), 0.0f, 1.0f);
}

/// shadow.slang evaluateEvssm（ELVSM 双侧）。返回受光量（1 = 全亮）。
float evaluateEvssm(const glm::vec4& moments, float zReceiver)
{
    const float depth = zReceiver * 2.0f - 1.0f;
    const float pw = std::exp(kC * depth);
    const float p = chebyshevUpperBound({moments.x, moments.y}, pw, kEps * (pw * pw), kLbr);
    const float nw = -1.0f / pw;
    const float n = chebyshevUpperBound({moments.z, moments.w}, nw, kEps * (nw * nw), kLbr);
    return std::min(p, n);
}

TEST_CASE("EVSSM: c = kEvssmExponent 的 fp16 域契约（常量护栏）") {
	// 期望值出处 = IEEE 754 binary16 有限最大值 (2−2⁻¹⁰)·2¹⁵ = 65504（独立定义，非复刻式）。
	// 阳性锚点：常量若被上调越过 ln(65504)/2 ≈ 5.545，exp(2c) 溢出 fp16 → 本用例红；
	// 反向：把 z 域改错（如忘 2d−1 重映射）→ 端点倒数恒等式红。
	constexpr float kFp16Max = 65504.0f;
	const float zNear = std::exp(kC * (0.0f * 2.0f - 1.0f));  // d=0 → e^−c
	const float zFar = std::exp(kC * (1.0f * 2.0f - 1.0f));   // d=1 → e^c
	CHECK(zFar * zNear == doctest::Approx(1.0f).epsilon(1e-5));  // 域关于 d=0.5 反对称
	CHECK(zFar <= kFp16Max);
	CHECK(zFar * zFar <= kFp16Max);   // 最大矩的 .y 分量不溢出（fallback/空块的正确性前提）
	// 单调：z 随 d 严格增（编码可逆的前提）。
	CHECK(std::exp(kC * (0.7f * 2.0f - 1.0f)) > std::exp(kC * (0.3f * 2.0f - 1.0f)));
}

TEST_CASE("EVSSM: 矩编码↔zBlocker 解码 round-trip（逆式锚点）") {
	// 期望值出处 = 解析逆：decode(encode(d)) = d（log-exp 互逆，fp32 噪声 ~1e-7）。
	// 变异红线：解码漏 +1 或漏 ×0.5 或符号反 → 全表红；编码忘 2d−1 → 全表红。
	for (float d : {0.0f, 0.1f, 0.37f, 0.5f, 0.62f, 0.9f, 1.0f}) {
		const glm::vec4 m = encodeMoments(d);
		// refZ 取 1（不触 clamp 上界）→ 还原 d 本身（absTol 兜 d=0 的期望零值）。
		expectNear(decodeZBlocker(m, 1.0f), d, 1e-4, 1e-6);
	}
	// clamp 语义（反向用例）：refZ 低于遮挡深度 → 截到 refZ（全亮自然成立的机制）。
	const glm::vec4 m = encodeMoments(0.8f);
	CHECK(decodeZBlocker(m, 0.5f) == 0.5f);
	// 零矩解码 = 哨兵侧：neg=max(−0,1e-8)=1e-8 → raw = (ln 1e-8 / −c + 1)/2 = (−18.42/−5.5+1)/2
	// ≈ 2.18 → clamp 到 refZ（遮挡判 0 距离 → 半影 0）。钉「哨兵不产生 NaN」。
	const float decoded = decodeZBlocker(glm::vec4{0.0f, 0.0f, 0.0f, 0.0f}, 1.0f);
	CHECK(decoded == decoded);  // 非 NaN
	CHECK(decoded == 1.0f);     // clamp 上界
}

TEST_CASE("EVSSM: far 面最大矩恒全亮——快路死条款（本 bug 正面锚点）") {
	// 「无遮挡」的数学身份 = far 面最大矩（阴影图集 far clear 的产物）。双侧快路
	// （pw ≤ e^c 且 nw ≤ −e^−c）恒命中 → 受光量 **精确 1.0**（解析语义，非近似）。
	// 变异红线（本文件的核心一条）：复刻式/复刻 shader 任一侧删快路 → p 公式给出
	// ≈1e-9…0.002（d² 对负 d 同样增长），本用例全体红 = 2026-10-02 漏抄事故的护栏。
	const glm::vec4 farMoments = encodeMoments(1.0f);
	for (float z : {0.0f, 0.05f, 0.25f, 0.5f, 0.75f, 0.99f, 1.0f})
		CHECK(evaluateEvssm(farMoments, z) == 1.0f);
}

TEST_CASE("EVSSM: 零矩恒暗——fallback 方向的反向用例") {
	// 零矩 (0,0,0,0)：正边 pw > 0 = moments.x → 公式 p = ε/(ε+1) ≈ 0.002 → LBR 后
	// **精确 0**；负边 nw ≤ 0 = moments.z → 快路 1；min = 0 = 全暗。
	// 语义钉死：「fallback 填零矩 = 全场景变暗」——实机 176821 px 变暗事故（调试历史
	// 2026-10-02）的数学出处；vkresources fallback 的正确性由「值从 kEvssmExponent
	// 现场推导、无手写字面量」构造性保证，本用例钉的是**方向语义**本身。
	// 变异红线：若有人「把 fallback 改回零图更省事」→ 本用例 + 上一条一起说明后果。
	const glm::vec4 zero{};
	for (float z : {0.0f, 0.3f, 0.5f, 0.8f, 1.0f})
		CHECK(evaluateEvssm(zero, z) == 0.0f);
}

TEST_CASE("EVSSM: 单层 blocker——自面亮 / 深影暗 / 深度单调（数学契约）") {
	// blocker 在 d=0.5（z̄ = e^0 = 1，fp32 精确）。
	const glm::vec4 m = encodeMoments(0.5f);
	// ① receiver 与遮挡同深（= 物体自己的表面写进图集）→ depth == moments.x 边界走
	//    快路 → 精确全亮。这是「自遮挡 acne 靠快路+bias 消除」的机制本体。
	CHECK(evaluateEvssm(m, 0.5f) == 1.0f);
	// ② receiver 在 blocker 之前（更近光源）→ 全亮（快路）。
	CHECK(evaluateEvssm(m, 0.0f) == 1.0f);
	// ③ receiver 深在影中：ε-方差上界 → pMax = ε·pw²/(ε·pw²+d²) → LBR 后精确 0。
	CHECK(evaluateEvssm(m, 1.0f) == 0.0f);
	// ④ 单调非增（受光量随光深度不回升；网格确定性，无随机数）。
	float prev = 1.0f;
	for (int i = 0; i <= 64; ++i) {
		const float z = static_cast<float>(i) / 64.0f;
		const float v = evaluateEvssm(m, z);
		CHECK(v <= prev + 1e-6f);
		prev = v;
	}
	// ⑤ 混合双深度（0.45/0.55 各半）→ 真方差（凸性：m.y − m.x² > 0，阳性锚）。
	const glm::vec4 mixed = (encodeMoments(0.45f) + encodeMoments(0.55f)) * 0.5f;
	CHECK(mixed.y - mixed.x * mixed.x > 0.0f);
	// 近缘抬亮 = Chebyshev 软化（light-bleed）的数学本体：同深度下混合方差的受光量
	// **严格高于**零方差单层（单层给 0，混合给 ~0.77）——若公式退化（方差被丢弃/
	// min 拿错侧）此比较红。端点复核：结果恒在 [0,1]。
	const float vMixed = evaluateEvssm(mixed, 0.52f);
	const float vSingle = evaluateEvssm(m, 0.52f);
	CHECK(vMixed > vSingle);
	CHECK(vMixed >= 0.0f);
	CHECK(vMixed <= 1.0f);
}

TEST_CASE("EVSSM: LBR 饱和端点（chebyshevUpperBound 独立契约）") {
	// 构造 pMax 精确值 → 期望 = saturate((pMax−0.1)/0.9) 的解析结果。
	// pMax = var/(var+d²)：var=1, d=3 → 1/10 → (0.1−0.1)/0.9 = **精确 0**（下端点）。
	CHECK(chebyshevUpperBound({1.0f, 2.0f}, 4.0f, 0.0f, kLbr) == 0.0f);
	// pMax = 0.55：d² = 1/0.55 − 1（fp32）→ 期望 0.5（±ulp）。上端点由快路管（下条）。
	const float d = std::sqrt(1.0f / 0.55f - 1.0f);
	expectNear(chebyshevUpperBound({1.0f, 2.0f}, 1.0f + d, 0.0f, kLbr), 0.5, 1e-5);
	// 快路边界**含等号**（depth == moments.x → 1.0，不是公式值）：
	CHECK(chebyshevUpperBound({1.0f, 2.0f}, 1.0f, 0.0f, kLbr) == 1.0f);
	// 反向：略深一点击穿快路 → 严格 < 1（区分「没触发」与「坏掉了」）。
	CHECK(chebyshevUpperBound({1.0f, 2.0f}, 1.0f + 1e-3f, 0.0f, kLbr) < 1.0f);
}

}  // namespace
}  // namespace RGL
