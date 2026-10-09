// timing_util.cppm —— 时间戳与时间统计的公共叶子（Phase 9 三个测量模块的共同前置）。
//
// 收敛前有三份逐字节相同的"取当前时刻"（`gpu_profiler_impl::steadyNs`、
// `present_timing_impl::steadyNs`、`profiler_impl::nowNs`）与两份 `toMs` / `percentile`。
// 三份时钟的注释还互相指认「与校准 mono 域同域」「9.2 同款」「与 9.2 GPU 校准目标域同源」
// —— 既然本就**要求**同域，抄三遍就只是风险而不是隔离。
//
// 为什么住 rlbase（AGENTS 分层判据：这段逻辑需不需要一个 VkDevice 才能跑）：不需要。
// 三个函数都是纯函数，`percentile` 的口径尤其该在毫秒级单测里钉住（见 tests/）。
module;
#include <chrono>  // steady_clock（GMF 例外清单成员；rlstd 不导出 chrono）
export module timing_util;

import rlstd;

namespace RGL {

/**
 * steady_clock 当前时刻（**ns**）。
 *
 * **为什么必须是 `steady_clock`**：`VK_EXT_calibrated_timestamps` 的 mono 域在 glibc 上
 * 就是 `CLOCK_MONOTONIC`，而 libstdc++ 的 `steady_clock` 正是它。换成别的时钟不会编译失败、
 * 也不会运行报错，只会让 CPU 侧 ns 与 GPU 校准域**不同域** —— 批 9.2 的偏差校准当场失效，
 * 表现为"偏差数字看着还行"。这就是它值得单独一个函数 + 一段注释的原因。
 *
 * ⚠ 别和 `SDL_GetTicksNS()` 混用（那是 SDL 自己的单调域，`ecs_frame_diag.cppm` 用的是它）。
 * @return 自 epoch 起的纳秒数，单调不回退
 */
export inline uint64_t steadyNowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

/**
 * ns → ms（摘要行统一以 ms 呈现）。
 * @param ns 纳秒数
 * @return 毫秒数
 */
export inline double toMs(uint64_t ns) { return static_cast<double>(ns) / 1e6; }

/**
 * 百分位（最近秩，**上取整**口径）。
 * @param sorted 须**已升序**——本函数不排序，也不校验（调用侧都在打印路径上，
 *               样本量小、排序一次在外面做）
 * @param p 分位 ∈ [0,1]；0 = 最小值，≥1 = 最大值
 * @return 空集返回 0（"没有样本"与"样本是 0"在摘要行里不可区分，是有意的宽松）
 *
 * ⚠ 口径是 `idx = (size_t)(p·n)` 再夹到 `n-1`，**不是**教科书最近秩 `ceil(p·n)`：
 *   n=4 时 p=0.5 → idx 2（偏上）。两份旧实现此式一字未差，收敛时**刻意保持原样**——
 *   改成教科书口径会让 profiler / present_timing 的历史摘要行数字对不上，而那是要拿来
 *   跨次比对的性能读数。用例 `percentile 口径` 钉的就是这条，不是钉"实现正确"。
 */
export inline uint64_t percentile(const std::vector<uint64_t>& sorted, double p) {
	if (sorted.empty())
		return 0;
	size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size()));
	if (idx >= sorted.size())
		idx = sorted.size() - 1;
	return sorted[idx];
}

}  // namespace RGL
