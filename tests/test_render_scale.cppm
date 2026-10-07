module;
#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>

/**
 * test_render_scale —— 批 14.3 内部渲染分辨率倍率的纯算术层
 * （rgl/runtime_config.cppm，rlbase）
 *
 * 被测 = `computeRenderSize`（渲染网格派生）+ `clampRenderScale`（合法区间钳制）。
 * 两者都不需要 VkDevice（AGENTS 分层判据：不需要 VkDevice 就能跑的逻辑属 rlbase，
 * 就该能在 `just test` 里测），却直接决定两件事：
 *
 *   1. **红线**：`renderScale = 1.0` 时渲染网格必须与窗口**逐像素**相等。这条一旦破，
 *      默认档交付路径就不再是"逐位不变"——而它是整个 Phase 14 的红线，不该靠
 *      每次改动手跑一遍截图回归来守。
 *   2. **非退化**：scale 必须真的在起作用。一个"永远返回 base"的实现能让上面那条红线
 *      用例照样全绿，所以「scale≠1 必须改变尺寸」要单独钉（阳性锚点）。
 *
 * ## 期望值出处（独立于被测实现）
 *  - 960 = 1280 × 0.75、540 = 720 × 0.75、640 = 1280 × 0.5、360 = 720 × 0.5 —— 手算。
 *  - 751 = round(1001 × 0.75) = round(750.75) —— 定义式，**专门用来区分四舍五入与截断**
 *    （把 `lround` 改成隐式转整会正好在这里红）。
 *  - 钳制区间端点取自 `kRenderScaleMin/Max` 本身（那两个常量就是口径定义，不是实现细节）。
 */
export module test_render_scale;

import rlstd;
import runtime_config;

namespace RGL {
namespace {

/** 真实出现过的分辨率集合（含奇数与非常规值，覆盖取整的两种分支）。 */
constexpr uint32_t kResolutions[] = {
    1, 2, 3, 100, 640, 720, 800, 1001, 1080, 1280, 1440, 1600, 1920, 2560, 3840, 4320,
};

}  // namespace

TEST_CASE("红线：renderScale = 1.0 时渲染网格逐像素等于窗口尺寸") {
	// 这条断言就是「默认档交付路径逐位不变」的算术根据。用逐位相等而不是容差比较：
	// 一旦这里允许 ±1，"render 比窗口少一像素"这种错就会以"在容差内"活下来，
	// 而它的症状是整幅画面被拉伸。
	for (uint32_t w : kResolutions) {
		for (uint32_t h : kResolutions) {
			const auto r = computeRenderSize(w, h, 1.0f);
			CHECK(r.width == w);
			CHECK(r.height == h);
		}
	}
}

TEST_CASE("computeRenderSize：0.75 / 0.5 档对上闭式手算值") {
	CHECK(computeRenderSize(1280, 720, 0.75f).width == 960);
	CHECK(computeRenderSize(1280, 720, 0.75f).height == 540);
	CHECK(computeRenderSize(1280, 720, 0.5f).width == 640);
	CHECK(computeRenderSize(1280, 720, 0.5f).height == 360);
	CHECK(computeRenderSize(1920, 1080, 0.75f).width == 1440);
	CHECK(computeRenderSize(1920, 1080, 0.75f).height == 810);
	CHECK(computeRenderSize(3840, 2160, 0.5f).width == 1920);
	CHECK(computeRenderSize(3840, 2160, 0.5f).height == 1080);
}

TEST_CASE("computeRenderSize 四舍五入而非截断（把 lround 改成转整必须在这里红）") {
	// 1001 × 0.75 = 750.75 → 751；截断会得到 750。
	CHECK(computeRenderSize(1001, 1001, 0.75f).width == 751);
	CHECK(computeRenderSize(1001, 1001, 0.75f).height == 751);
	// 100 × 0.25 = 25.0（恰好整数）——确认没有"加半个再取整"之类的偏移。
	CHECK(computeRenderSize(100, 100, kRenderScaleMin).width == 25);
}

TEST_CASE("computeRenderSize 下限 1：极小尺寸不得塌成 0（viewport 为 0 = UB）") {
	CHECK(computeRenderSize(1, 1, kRenderScaleMin).width == 1);
	CHECK(computeRenderSize(1, 1, kRenderScaleMin).height == 1);
	CHECK(computeRenderSize(3, 2, kRenderScaleMin).width == 1);
	CHECK(computeRenderSize(3, 2, kRenderScaleMin).height == 1);
	// 阳性锚点：非极小尺寸必须**没被**下限逻辑误改（下限写错成 max(2,…) 会在这里红）。
	CHECK(computeRenderSize(4, 4, kRenderScaleMin).width == 1);
	CHECK(computeRenderSize(8, 8, kRenderScaleMin).height == 2);
}

TEST_CASE("computeRenderSize 非退化：scale≠1 必须真的改变尺寸（防『恒返回 base』）") {
	// 没有这条，一个把 scale 完全忽略的实现能让上面所有"1.0 档"用例保持绿色。
	const auto full = computeRenderSize(1280, 720, 1.0f);
	const auto half = computeRenderSize(1280, 720, 0.5f);
	CHECK(half.width != full.width);
	CHECK(half.height != full.height);
	// 单调：scale 越小网格越小（对全部测试分辨率）。
	for (uint32_t w : kResolutions) {
		const auto a = computeRenderSize(w, w, 0.75f);
		const auto b = computeRenderSize(w, w, 0.5f);
		CHECK(a.width >= b.width);
		CHECK(a.height >= b.height);
	}
}

TEST_CASE("clampRenderScale：区间内恒等、越界钳端点、非有限值回落") {
	// 区间内恒等（含两个端点本身）
	CHECK(clampRenderScale(1.0f) == 1.0f);
	CHECK(clampRenderScale(kRenderScaleMax) == kRenderScaleMax);
	CHECK(clampRenderScale(kRenderScaleMin) == kRenderScaleMin);
	CHECK(clampRenderScale(0.75f) == 0.75f);
	// 越界 → 钳到端点（不是回落默认——写清区别：0.1 与 0.0 语义不同）
	CHECK(clampRenderScale(0.0f) == kRenderScaleMin);
	CHECK(clampRenderScale(-3.0f) == kRenderScaleMin);
	CHECK(clampRenderScale(1.5f) == kRenderScaleMax);
	// 非有限 → 回落传入的 fallback（配置侧传当前默认，Swapchain 侧传 1.0）
	CHECK(clampRenderScale(std::numeric_limits<float>::infinity(), 0.75f) == 0.75f);
	CHECK(clampRenderScale(-std::numeric_limits<float>::infinity(), 0.75f) == 0.75f);
	CHECK(std::isnan(clampRenderScale(std::numeric_limits<float>::quiet_NaN(), 0.75f)) == false);
	CHECK(clampRenderScale(std::numeric_limits<float>::quiet_NaN(), 0.75f) == 0.75f);
}

TEST_CASE("区间常量自洽：min ≤ 1.0 ≤ max（写反了上面全绿也白搭）") {
	// 若有人把两端写反，clamp 会变成"恒等于 max"，而所有"钳到端点"的用例仍可能通过
	// （它们读的也是常量）。这条是那条链的最后一块。
	CHECK(kRenderScaleMin <= 1.0f);
	CHECK(kRenderScaleMax >= 1.0f);
	CHECK(kRenderScaleMin > 0.0f);
}

}  // namespace RGL
