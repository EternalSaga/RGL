module;
#include <doctest/doctest.h>
#include <algorithm>  // std::ranges::min/max_element、is_sorted
#include <vector>

/**
 * test_timing_util —— 时间戳与时间统计叶子（rgl/timing_util.cppm，rlbase）
 *
 * 被测三函数是从 `profiler_impl` / `present_timing_impl` / `gpu_profiler_impl` 里
 * **逐字节相同**的副本收敛来的单点。收敛本身不改行为，改的是"以后只有一份可改"，
 * 所以这里钉的是**口径**而不是"实现正确"：口径一漂，profiler / present_timing 的
 * 摘要行数字就跨次对不上，而那些数字存在的意义正是跨次比。
 *
 * ## 期望值出处（独立于被测实现）
 *  - `toMs`：单位定义 1 ms = 1e6 ns。用例只取**二进制可精确表示**的比值
 *    （1e6/5e5/2.5e5/1.5e6），免得把断言花在浮点舍入上——那测不到换算本身。
 *  - `percentile`：按下标算式 `idx = ⌊p·n⌋` 夹到 `n-1` **手算**每个期望下标，并与
 *    教科书最近秩 `ceil(p·n)`（1-based）对照。两者在 n=4 / p=0.5 处**故意不同**
 *    （本口径 30，教科书 20），那条用例就是钉"本项目用的是前者"。
 *  - `steadyNowNs`：单调性由标准 [time.clock.steady] 保证（永不回退）。
 *
 * ## 阳性锚点
 *  - percentile 恒返回首元素 ⟹「p=1 取到最大值」红；恒返回末元素 ⟹「p=0 取到最小值」
 *    红；越界兜底写成 `idx = 0` ⟹ n=1 两条红。三条合起来堵"返回了某个元素但口径不对"。
 *  - steadyNowNs 若被"优化"成常量（误加 `static const`），单调断言用 `>=` 会**恰好全绿**，
 *    所以另加「1000 次里必须出现多个不同值」堵这个洞。
 */
export module test_timing_util;

import rlstd;
import timing_util;

namespace RGL {

TEST_CASE("toMs 单位换算") {
	CHECK(toMs(0) == 0.0);
	CHECK(toMs(1'000'000) == 1.0);
	CHECK(toMs(500'000) == 0.5);
	CHECK(toMs(250'000) == 0.25);
	CHECK(toMs(1'500'000) == 1.5);
}

TEST_CASE("percentile 口径：⌊p·n⌋ 夹到 n-1，不是教科书最近秩") {
	const std::vector<uint64_t> v{10, 20, 30, 40};  // n = 4，升序

	// 逐点手算：idx = ⌊p·4⌋，夹到 3。
	CHECK(percentile(v, 0.00) == 10);  // idx 0
	CHECK(percentile(v, 0.24) == 10);  // idx ⌊0.96⌋ = 0
	CHECK(percentile(v, 0.25) == 20);  // idx 1
	CHECK(percentile(v, 0.50) == 30);  // idx 2 ← 教科书 ceil(0.5·4)=2（1-based）会给 20
	CHECK(percentile(v, 0.74) == 30);  // idx ⌊2.96⌋ = 2
	CHECK(percentile(v, 0.75) == 40);  // idx 3
	CHECK(percentile(v, 0.95) == 40);  // idx ⌊3.8⌋ = 3
	CHECK(percentile(v, 1.00) == 40);  // idx 4 → 夹到 3

	// 阳性锚点：两端必须真的取到极值（恒首/恒末的实现各会红一条）。
	CHECK(percentile(v, 0.0) == *std::ranges::min_element(v));
	CHECK(percentile(v, 1.0) == *std::ranges::max_element(v));

	// n = 1：任何 p 都只能是那一个值；越界兜底写错在这里暴露。
	const std::vector<uint64_t> one{7};
	CHECK(percentile(one, 0.0) == 7);
	CHECK(percentile(one, 0.5) == 7);
	CHECK(percentile(one, 1.0) == 7);

	// 空集契约：返回 0（"没样本"与"样本是 0"在摘要行里不可区分，是有意的）。
	CHECK(percentile({}, 0.5) == 0);
	CHECK(percentile({}, 1.0) == 0);
}

TEST_CASE("percentile 单调不减（对 p 扫描）") {
	const std::vector<uint64_t> v{1, 2, 3, 5, 8, 13, 21, 34, 55, 89};  // n = 10
	uint64_t prev = 0;
	for (int i = 0; i <= 100; ++i) {
		const double p = static_cast<double>(i) / 100.0;
		const uint64_t got = percentile(v, p);
		CHECK(got >= prev);  // 分位函数对 p 必须不减
		CHECK(got >= v.front());
		CHECK(got <= v.back());
		prev = got;
	}
	CHECK(prev == v.back());  // p 扫到 1 必须落在最大值
}

TEST_CASE("percentile 不排序（契约：调用侧负责升序）") {
	// 被测函数**刻意不排序**（调用侧都在打印路径上、外面已 sort 一次）。这条是
	// 特征化用例：哪天有人"顺手"加个 sort，这里会红，逼他重新看一遍热路径。
	const std::vector<uint64_t> unsorted{30, 10, 40, 20};
	CHECK(percentile(unsorted, 0.50) == 40);  // 按下标取 idx 2，不看大小
}

TEST_CASE("steadyNowNs 单调且真的在走") {
	const uint64_t a = steadyNowNs();
	CHECK(a > 0);

	uint64_t acc = 0;  // 真做点事，别让编译器把两次取值之间掏空
	for (int i = 0; i < 1000; ++i)
		acc += static_cast<uint64_t>(i) * 2654435761ull;
	const uint64_t b = steadyNowNs();
	CHECK(b >= a);
	CHECK(acc != 0);  // 防编译器把上面整个循环当死代码删掉

	// 阳性锚点：若实现被"优化"成常量，上面用 >= 的单调断言会全绿，这条不会。
	std::vector<uint64_t> seen;
	for (int i = 0; i < 1000; ++i) {
		const uint64_t t = steadyNowNs();
		if (seen.empty() || seen.back() != t)
			seen.push_back(t);
	}
	CHECK(seen.size() > 1);
	CHECK(std::ranges::is_sorted(seen));  // 逐次记录本身就是序列，直接验不减
}

}  // namespace RGL
