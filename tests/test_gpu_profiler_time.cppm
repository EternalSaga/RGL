module;
#include <doctest/doctest.h>
#include <cstdint>  // uint64_t/int64_t 字面量与极值（宏/自由函数不经 BMI 保证传递，GMF 自带）

/**
 * test_gpu_profiler_time —— GPU 时间戳换算口径（rgl/gpu_profiler.cppm 的
 * `gpuTicksToNs` / `gpuEpochShiftNs`，债-87 根因回归）。
 *
 * ## 为什么值得钉（债-87：per-pass GPU 读数全灭）
 *
 * 旧实现用**同一条** VK_EXT_calibrated_timestamps 仿射同时定「起点」与「时长」。本机
 * RADV 的 VK_TIME_DOMAIN_DEVICE 是 boottime 域自由跑时钟，而 query 池时间戳另起纪元
 * （2026-10-11 实测恒差 56426.58s = 15.67h，跨 suspend/resume 变，而 maxDeviation 仍
 * 谎报 ~10µs）⟹ 两端被同一常量压掉后再被 `ns > 0` 夹成 0 → 逐区域"非单调"、整帧 GPU
 * 区域全丢（stats 里 kind=gpu 0 zone / 576 drops）。症状离真因三跳：看起来像"query 没
 * 提交"或"驱动不支持"，实际数据通路全程正常（tick 读回来逐条正确）。
 *
 * ## 期望值出处（独立于被测实现）
 *
 * 全部取自**实机日志读数 + 闭式乘积**，不抄被测码：period = 10.019036 ns/tick
 * （`gpu profiler: on` 行），tick 对取自同一次运行的原始读回（gpu.frame 961392 ticks
 * 等），期望 ns = 二者的手算乘积（小数位离整数边界 > 0.05ns ⟹ 截断值确定）。
 * 纪元偏移 56426.58308615544s = 手算 (6861764054875 − 1229826702204) × period。
 * 交叉校验：gpu.frame 9.63ms / shadow0 1.17ms 与还债总账 §2.4 级联成本表 GPU 列
 * （shadow0 1.099ms）同量级——换算口径若漂一个数量级，这条对不上。
 *
 * ## 阳性锚点（变异红线）
 *
 *  - `gpuTicksToNs` 去掉 `× periodNs`（只回 tick 数）⟹ 四条 golden 全红（差 100 倍）；
 *    把 period 换成 `1e9/period` ⟹ 同样全红。
 *  - `gpuEpochShiftNs` 恒返回 0（guard 被"优化"掉）⟹「纪元错乱」两条红，而「纪元一致」
 *    三条**仍绿**——这正是反向用例存在的理由：只有两组合起来才分得开"没触发"与"坏掉了"。
 *  - 阈值比较 `>` 写成 `>=` ⟹ 恰好等于 kGpuEpochSanityNs 那条红。
 *  - 无锚点分支删掉（anchor=0 也去比阈值）⟹「锚点缺失」那条红（会平移出一个巨大常量）。
 *  - 回归组合用例：时长若仍从仿射两端求（旧口径）⟹ 该组输入下两端同为 0、时长归 0 红。
 */
export module test_gpu_profiler_time;

import rlstd;
import gpu_profiler;

namespace RGL {

namespace {

/// 实机 limits.timestampPeriod（ns/tick，日志 "gpu profiler: on" 行原值）。
constexpr double kPeriodNs = 10.019036;

/// 实机 host 锚点量级（steady/CLOCK_MONOTONIC ns，约开机 10.4h；数值本身不重要，
/// 重要的是它与下面仿射起点**相差一个纪元常量**）。
constexpr uint64_t kHostAnchorNs = 37'712'899'436'481ULL;

/// 实机测得的纪元偏移（ns）：手算 (6861764054875 − 1229826702204) × 10.019036。
/// 符号：query 时钟落后于 VK_TIME_DOMAIN_DEVICE ⟹ 仿射起点落在 host 锚点**之前**。
constexpr int64_t kEpochOffsetNs = -56'426'583'086'155LL;

}  // namespace

TEST_CASE("gpuTicksToNs：时长 = 同段 tick 差 × period（实机 golden，不经跨域换算）") {
	// 手算闭式乘积（截断值离整数边界 > 0.05ns ⟹ 可逐位比）
	CHECK(gpuTicksToNs(961392, kPeriodNs) == 9632221);  // gpu.frame 9.632ms
	CHECK(gpuTicksToNs(117072, kPeriodNs) == 1172948);  // shadow0 1.173ms
	CHECK(gpuTicksToNs(233304, kPeriodNs) == 2337481);  // geometry 2.337ms
	CHECK(gpuTicksToNs(1976, kPeriodNs) == 19797);      // tonemap 0.0198ms

	// 零时长边界：正方向不能凭空长出时间（旧实现里「两端同被夹 0」就是长在这里）
	CHECK(gpuTicksToNs(0, kPeriodNs) == 0);

	// 阳性锚点：四条 golden 之和 ≈ gpu.frame 量级（pass 求和与整帧同向，
	// 换算若整体漂一个数量级，这条与上面的逐条断言一起红）
	const uint64_t passes = gpuTicksToNs(117072, kPeriodNs) + gpuTicksToNs(233304, kPeriodNs) +
		gpuTicksToNs(1976, kPeriodNs);
	CHECK(passes < gpuTicksToNs(961392, kPeriodNs));
}

TEST_CASE("gpuEpochShiftNs：纪元一致时不干预（反向用例，堵「恒平移」）") {
	// 正常管线形态：GPU 起点落在本帧 frameBegin 之后几毫秒（帧槽只有 2 个）
	CHECK(gpuEpochShiftNs(kHostAnchorNs + 5'000'000, kHostAnchorNs) == 0);
	CHECK(gpuEpochShiftNs(kHostAnchorNs - 5'000'000, kHostAnchorNs) == 0);

	// 阈值内侧：恰好不到 1s 仍算一致（阈值取秒量级 ⟹ 毫秒级管线与小时级纪元错乱无灰区）
	CHECK(gpuEpochShiftNs(kHostAnchorNs - (kGpuEpochSanityNs - 1), kHostAnchorNs) == 0);
	CHECK(gpuEpochShiftNs(kHostAnchorNs + (kGpuEpochSanityNs - 1), kHostAnchorNs) == 0);

	// 阈值本身不算越界（`>` 不是 `>=`）
	CHECK(gpuEpochShiftNs(kHostAnchorNs - kGpuEpochSanityNs, kHostAnchorNs) == 0);

	// 锚点缺失（0）= 无从判定 ⟹ 宁可不干预，也不能平移出一个巨大常量
	CHECK(gpuEpochShiftNs(kHostAnchorNs, 0) == 0);
}

TEST_CASE("gpuEpochShiftNs：纪元错乱时平移量精确抵消偏移（阳性锚点，堵「恒 0」）") {
	const int64_t calibrated = static_cast<int64_t>(kHostAnchorNs) + kEpochOffsetNs;
	const int64_t shift = gpuEpochShiftNs(calibrated, kHostAnchorNs);

	// 平移后必须**正好**落回 host 锚点（常量平移不改区域内相对结构）
	CHECK(calibrated + shift == static_cast<int64_t>(kHostAnchorNs));
	// 且平移量就是那个实机测得的纪元差（符号：起点在锚点之前 ⟹ 往正向补回来）
	CHECK(shift == -kEpochOffsetNs);

	// 反方向同形：起点跑到锚点之后 2h 也要拉回来（guard 是双向的，不是只堵历史一侧）
	const int64_t twoHoursNs = 2LL * 3600LL * 1'000'000'000LL;
	const int64_t late = static_cast<int64_t>(kHostAnchorNs) + twoHoursNs;
	CHECK(gpuEpochShiftNs(late, kHostAnchorNs) == -twoHoursNs);

	// 阈值外侧 1ns 即触发（与「阈值本身不算越界」配对钉死边界）
	CHECK(gpuEpochShiftNs(kHostAnchorNs - (kGpuEpochSanityNs + 1), kHostAnchorNs) ==
		kGpuEpochSanityNs + 1);
}

TEST_CASE("债-87 回归：纪元错乱输入下，时长与绝对定位同时成立") {
	// 同一次实机读回的 gpu.frame tick 对 + 实机纪元偏移
	const uint64_t beginTick = 1'229'826'702'204ULL;
	const uint64_t endTick = 1'229'827'663'596ULL;
	const int64_t calibratedStart =
		static_cast<int64_t>(kHostAnchorNs) + kEpochOffsetNs;  // 仿射把起点甩到 15.7h 前

	// ① 时长走同段 tick 差 ⟹ 纪元错乱污染不到（旧口径在这里得 0，区域被整个丢弃）
	CHECK(gpuTicksToNs(endTick - beginTick, kPeriodNs) == 9632221);

	// ② 绝对定位走常量平移 ⟹ 落回 host 轴，Perfetto 的 gpu 轨不再是空轨/飞走
	const int64_t anchored =
		calibratedStart + gpuEpochShiftNs(calibratedStart, kHostAnchorNs);
	CHECK(anchored == static_cast<int64_t>(kHostAnchorNs));
	CHECK(anchored > 0);  // 旧实现的终态是 0（被夹 0 后判非单调丢弃）

	// ③ 同一帧内后续区域共用该平移 ⟹ 区域间相对次序保持（tonemap 在 gpu.frame 内）
	const int64_t later = calibratedStart + 19797;  // tonemap 起点（同段、更晚的 tick）
	CHECK(later + gpuEpochShiftNs(calibratedStart, kHostAnchorNs) > anchored);
}

}  // namespace RGL
