module;
#include <doctest/doctest.h>
#include <cmath>
#include <cstring>  // memcmp（GMF 真 STL 头——modules 下自由函数不从 import 带来）

/**
 * test_scene_spin —— 帧驱动 spin 的纯函数层（rgl/scene_context.cppm::frameDrivenSpinAngle，rlbase）
 *
 * 被测 = 「angle = anglePerFrame · frameIndex」这一条闭式。它是门 `spin_frame_angle`
 * （诊断激励，默认关）的判据本体：imgqa temporal-motion 的**跨进程 A/B 对齐**完全建立在
 * 「同一 frameIndex 在任何运行给出同一角度」上——函数若掺进累积/时间/隐藏状态，参考图
 * 与候选图量的就不是同一机位序列，整条时域判据作废（dt 累积不可复现的教训写死在
 * tools/imgqa/temporal.cpp 头注释）。
 *
 * ## 期望值出处（独立于被测实现）
 * 全部断言的期望值 = 手算闭式 n·a（右侧就是数学式本身，不抄实现）；
 * 16 取 2 的幂 ⟹ float32 缩放精确，断言写成**恰等**；
 * 10⁴ 帧精度断言 = double 闭式（float32 乘法的舍入界由本用例自己声明并验证）。
 *
 * ## 阳性锚点与反向用例
 *  - angle(0) **恰为 0**：首帧角 = 0 是跨进程对齐的零点约定（反向：`frameIndex−1`
 *    这类 off-by-one 变异当场红）；
 *  - angle(0, a=0) 恰为 0：门关值不造角度；
 *  - 纯函数性 = 同输入两次调用**逐位相同** + 与调用顺序无关（先大后小 vs 先小后大
 *    给出同一张表）——堵「偷偷累积」的变异。
 *
 * ## 变异红线（哪条变异让我红）
 *  `*` 改 `+` → angle(1) = 1+a 红；`frameIndex` 改 `frameIndex−1` → angle(0) 非 0 红；
 *  改成 static 累积 → 纯函数性/闭式两条红；改成 `anglePerFrame²·n` → 闭式全红。
 */
export module test_scene_spin;

import rlstd;
import scene_context;
import test_support;  // testing::expectNear

namespace RGL {

TEST_CASE("scene_spin：帧驱动角 = 每帧角 × 帧号（闭式手算；恰等断言靠 2 的幂）") {
	constexpr float a = 0.00349066f;  // 0.2°/帧（imgqa temporal-motion 默认激励步长）
	// 零点约定：首帧角恰为 0（跨进程对齐的基准，容不得 off-by-one）
	CHECK(frameDrivenSpinAngle(a, 0u) == 0.0f);
	// 2 的幂缩放 float32 精确 ⟹ 闭式右侧手算，断言恰等
	CHECK(frameDrivenSpinAngle(a, 1u) == a);
	CHECK(frameDrivenSpinAngle(a, 2u) == 2.0f * a);
	CHECK(frameDrivenSpinAngle(a, 16u) == 16.0f * a);
	// 一般帧号：期望值 = 数学闭式本身（单次舍入，与实现同式不同源）
	CHECK(frameDrivenSpinAngle(a, 100u) == a * 100.0f);
	// 负步长（反向 yaw）同样成立
	CHECK(frameDrivenSpinAngle(-a, 16u) == -16.0f * a);
	// 门关值不造角度（门本身判 != 0，这里钉函数自身无偏置）
	CHECK(frameDrivenSpinAngle(0.0f, 123u) == 0.0f);
}

TEST_CASE("scene_spin：纯函数性——同输入逐位相同、与调用顺序无关（堵『偷偷累积』）") {
	constexpr float a = 0.00349066f;
	// 先大后小 vs 先小后大：任何「内部状态/累积」实现都会让两组表不一致
	uint32_t idx[8] = {1000u, 300u, 90u, 27u, 9u, 3u, 1u, 0u};
	float forward[8], backward[8];
	for (int i = 0; i < 8; ++i)
		forward[i] = frameDrivenSpinAngle(a, idx[i]);
	for (int i = 7; i >= 0; --i)
		backward[i] = frameDrivenSpinAngle(a, idx[i]);
	for (int i = 0; i < 8; ++i)
		CHECK(memcmp(&forward[i], &backward[i], sizeof(float)) == 0);  // **逐位**相同
	// 重复调用逐位稳定（位比对：浮点 == 对 −0.0/+0.0 会放水）
	const float x = frameDrivenSpinAngle(a, 4096u);
	const float y = frameDrivenSpinAngle(a, 4096u);
	CHECK(memcmp(&x, &y, sizeof(float)) == 0);
	// 单调（正向步长、帧号递增）：跨进程「帧 i ↔ 帧 i+1」的角差方向不得翻转
	for (uint32_t n = 0; n < 10000u; ++n)
		CHECK(frameDrivenSpinAngle(a, n + 1u) > frameDrivenSpinAngle(a, n));
}

TEST_CASE("scene_spin：float32 精度账——10⁴ 帧离 double 闭式 <1e-3 rad（注释承诺的界）") {
	// scene_context.cppm 头注释承诺「ULP ≈4e-6 rad，对应 ≈0.003px」；本用例把承诺
	// 钉成断言：若日后有人把步长/帧号推到需要 double 的量级，这条先红而不是判据静默漂。
	constexpr float a = 0.00349066f;
	const double want = static_cast<double>(a) * 10000.0;
	const double got = static_cast<double>(frameDrivenSpinAngle(a, 10000u));
	CHECK(std::abs(got - want) < 1e-3);
}

}  // namespace RGL
