module;
#include <doctest/doctest.h>

/**
 * test_frame_capture_miss —— 帧捕获「这次未命中该重试、还是该判死」的分类契约
 * （被测 = `rgl/frame_capture.cppm` 的 `classifyCaptureMiss`，纯函数，零设备）
 *
 * ## 为什么这一层值得单独钉（2026-10-19 修「一次拼错附件名就把 `/shot` 端点打死」）
 * 缺陷本体是把**永久失败当暂时失败**：请求保持 pending 每帧重试 ⟹ ① 每帧一条 warn（实测
 * 46 秒刷 1505 条，而 `requestAttachment` 的接口注释原本承诺"warn 一次"）② 跨帧响应句柄
 * `pendingShot` 永不复位 ⟹ 之后**每一个**截图请求都吃 409 `another /shot is still pending`，
 * 直到进程重启 ③ 客户端只能等到超时（504），拿不到任何原因。
 *
 * 而**修复方向本身同样危险**：一律判死会让「acquire 超时跳帧」「RT 首帧还没建」这类
 * 真该重试的情况静默丢图。两侧各是一条红线 ⟹ 按 skill `rgl-test-discipline`，
 * **对称判据的每一半边各配用例**，一条"半边"用例会被后来人读成"整条已验"。
 *
 * ## 期望值出处（独立于被测实现）
 * 契约用一句话说清：**名字不在编译好的图里 ⟹ 立刻判死**（图不会自己长出附件）；
 * **在图里 ⟹ 前 `budget-1` 次未命中可重试，第 `budget` 次判死**。用例逐格手算
 * `misses + 1 < budget`（下面闭式扫描里把它作为**第二实现**直接写出来对账），
 * 不抄被测码的表达式。
 *
 * ## 变异红线（哪个变异让我红）
 * 1. 删掉 `if (!inGraph) return CaptureMiss::permanent;`（= 修复前的行为）→ A 组三条全红
 * 2. `missesSoFar + 1u >= budget` 改成 `>`（off-by-one 漂移，本地仍"看起来对"，
 *    正是 skill 说的"弱化变异删除变异看不见"那一类）→ B 组边界格 + D 组闭式扫描红
 * 3. 去掉 `+ 1u`（把"第几次"当成"已经过几次"）→ B 组边界另一格 + D 组红
 * 4. 三元两侧对调（一律判死）→ C 组「预算内仍可重试」两条红
 * 5. 默认实参从 `kCaptureRtRetryBudget` 改成别的数 → E 组双向红（E 专门钉"默认值 = 那个常量"）
 *
 * ⚠ 本文件**测不到**的部分（诚实登记，别把全绿读成"整条修复已验"）：`FrameCapturer` 的
 * 状态机（`request()` 清失败、`takeFailure()` 取后即清）要构造带 `vk::raii` 成员的对象，
 * 无设备编不出来 ⟹ 那半边靠实机 RPC 冒烟验收（拼错附件名 → 400 且 warn 只一条、
 * 之后 `/shot` 仍能用），读数记在 `docs/rpc_单源_plan.md` 落地记录。
 */
export module test_frame_capture_miss;

import rlstd;
import frame_capture;  // 被测：classifyCaptureMiss / kCaptureRtRetryBudget

using namespace RGL;

namespace {

/// 契约的**闭式第二实现**：在图里 ⟺ `misses + 1 < budget` 时可重试。
/// 与被测码各自独立写就（出处 = 上面注释里那句自然语言契约），对账才有意义。
constexpr bool retryableByContract(bool inGraph, uint32_t misses, uint32_t budget) {
	return inGraph && (misses + 1u < budget);
}

}  // namespace

TEST_CASE("捕获未命中分类：名字不在图里 = 永久失败，与已重试几帧无关") {
	// A 组（半边一）：永久失败。misses = 0 这条是**修复本体**——旧行为在这里返回可重试，
	// 于是每帧重演；999 这条钉"重试次数再多也不会把永久翻成暂时"。
	CHECK(classifyCaptureMiss(false, 0) == CaptureMiss::permanent);
	CHECK(classifyCaptureMiss(false, 1) == CaptureMiss::permanent);
	CHECK(classifyCaptureMiss(false, 7) == CaptureMiss::permanent);
	CHECK(classifyCaptureMiss(false, 999) == CaptureMiss::permanent);
	// 反向半边：同一 misses 值，只要名字在图里就不许判死（否则"一律判死"也能让 A 组绿）。
	CHECK(classifyCaptureMiss(true, 0) == CaptureMiss::retryable);
}

TEST_CASE("捕获未命中分类：在图里 = 预算内可重试、第 budget 次判死（逐格手算）") {
	constexpr uint32_t B = 8;  // 与默认预算无关，本用例自带一个可读的小预算
	// B 组（半边二）：边界两侧各钉一格，off-by-one 的两种漂移方向都跑不掉。
	CHECK(classifyCaptureMiss(true, 0, B) == CaptureMiss::retryable);       // 1 < 8
	CHECK(classifyCaptureMiss(true, B - 2, B) == CaptureMiss::retryable);   // 7 < 8
	CHECK(classifyCaptureMiss(true, B - 1, B) == CaptureMiss::permanent);   // 8 >= 8 ← 边界
	CHECK(classifyCaptureMiss(true, B, B) == CaptureMiss::permanent);       // 9 >= 8
	CHECK(classifyCaptureMiss(true, B + 100, B) == CaptureMiss::permanent);
	// budget = 1 的退化档：一次未命中即判死（"预算 0 帧"不该让首帧也判死以外的形态出现）。
	CHECK(classifyCaptureMiss(true, 0, 1) == CaptureMiss::permanent);
	CHECK(classifyCaptureMiss(true, 0, 2) == CaptureMiss::retryable);
}

TEST_CASE("捕获未命中分类：预算内不许判死（防「一律判死」这种反向修复）") {
	// C 组：把"可重试"这一半边单独钉死。若有人为了消掉日志洪水而一律判死，
	// acquire 跳帧/RT 首帧未建会**静默丢图**——症状比刷日志更难查，所以这条不能省。
	for (uint32_t misses = 0; misses + 1u < kCaptureRtRetryBudget; ++misses)
		CHECK(classifyCaptureMiss(true, misses) == CaptureMiss::retryable);
	CHECK(classifyCaptureMiss(true, kCaptureRtRetryBudget - 1u) == CaptureMiss::permanent);
}

TEST_CASE("捕获未命中分类：与闭式契约逐格对账（budget 1..12 × misses 0..24）") {
	// D 组：全表对账。断言写成"分类 == (闭式契约)"，两个实现同时错的概率远低于各自单独错。
	for (uint32_t budget = 1; budget <= 12; ++budget)
		for (uint32_t misses = 0; misses <= 24; ++misses) {
			const auto got = classifyCaptureMiss(true, misses, budget);
			const auto want = retryableByContract(true, misses, budget) ? CaptureMiss::retryable
			                                                            : CaptureMiss::permanent;
			CHECK(got == want);
		}
	// 不在图里的列同样全表对账（闭式契约：inGraph = false ⟹ 恒 permanent）。
	for (uint32_t budget = 1; budget <= 12; ++budget)
		for (uint32_t misses = 0; misses <= 24; ++misses)
			CHECK(classifyCaptureMiss(false, misses, budget)
			      == (retryableByContract(false, misses, budget) ? CaptureMiss::retryable
			                                                     : CaptureMiss::permanent));
}

TEST_CASE("捕获重试预算：常量可用且默认实参就是它（双向钉，改默认值即红）") {
	// E 组：默认实参 = `kCaptureRtRetryBudget` 这件事本身是契约（impl 调用点不传 budget）。
	// 两条一起才钉得住：默认值调大 ⟹ 第一条红；调小 ⟹ 第二条红。
	CHECK(kCaptureRtRetryBudget >= 2u);  // 预算 < 2 = 首帧未命中即判死 = 重试形同不存在
	CHECK(classifyCaptureMiss(true, kCaptureRtRetryBudget - 2u) == CaptureMiss::retryable);
	CHECK(classifyCaptureMiss(true, kCaptureRtRetryBudget - 1u) == CaptureMiss::permanent);
}
