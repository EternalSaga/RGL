module;
/**
 * test_features_gates —— 能力门纯函数真值表 + 扩展启用清单契约（批 12.3d-A）
 *
 * 为什么值得测：能力门是"这台驱动上这段代码到底会不会跑"的唯一裁决点，形态全是
 * 布尔组合——最适合穷举真值表。且本批就起源于一处**声明与代码分叉**：
 * docs/archive/issue_history.md 拍板 depthClip 门 = eds3 && base 双条件（VUID-07451
 * 查的是旧扩展 VK_EXT_depth_clip_enable 的 feature，只暴露 edS3 的驱动上单条件放行
 * = 调用违 VUID），而 vkrenderer_impl 只传了 depthClipEnable 单 bit。代码就范后
 * 把双条件钉进真值表，防止再次漂移。
 *
 * features 模块在 rlgfx（import rlvk），但三个被测函数都是**纯函数**（吃
 * OptionalFeatureSupport 值 → bool / vector\<const char*\>），不碰任何 VkDevice
 * ——先例 = test_render_graph import rlvk 类型（"import rlvk 是编译期依赖，
 * 需要 VkDevice 才是运行期依赖"）。
 *
 * ## 变异红线声明（哪个变异让我红）
 *   depthClipDynamicSupported ：删 `&& opt.depthClipBase` → (T,F) 组合 expected=false
 *                  实得 true 红；删 `&& opt.depthClipEnable` → (F,T) 红
 *   presentTimingSupported   ：任一 && 改成 || → 对应"只缺一个"组合红（8 组合全举，
 *                  恰有 3 个"缺一"组合覆盖三处 &&）
 *   getDeviceExtensions      ：可选追加项的 if 条件改写（如 depthClipEnable 门控删掉
 *                  → 无条件追加）→ "默认 opt 无 edS3"反向用例红；present_timing 组
 *                  的 all-or-nothing 破坏（移一个出 if）→ "缺一"用例红；
 *                  conservativeRasterizationDynamic 的 || 条件删掉 →
 *                  "保守动态位单独抬 edS3"用例红（EDS3 缺席）
 *   无参 getDeviceExtensions ：9 项必需清单删一项 → size==9 红（换名 → has() 红）
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import 带进来
#include <vulkan/vulkan.h>   // VK_*_EXTENSION_NAME 宏（rlvk 不含宏；features 同款 C 头）

export module test_features_gates;

import rlstd;
import features;  // presentTimingSupported / depthClipDynamicSupported / getDeviceExtensions
                  // （export import features_generated → OptionalFeatureSupport 可见）

namespace RGL {
namespace {

/** 扩展名按**值**比较（const char* 的 == 是指针相等，必须经 string_view）。 */
bool has(const std::vector<const char*>& exts, const char* name) {
	for (const char* e : exts)
		if (std::string_view(e) == name)
			return true;
	return false;
}

/** 必需 9 项（features.cppm kDeviceExtensions 清单——删/换任一都红）。 */
constexpr std::array<const char*, 9> kRequired{{
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	VK_EXT_MESH_SHADER_EXTENSION_NAME,
	VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
	VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
	VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
	VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME,
	VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
	VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
	VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
}};

}  // namespace

TEST_CASE("depthClipDynamicSupported 真值表：eds3 && base 双条件（VUID-07451 裁决）") {
	for (const bool eds3 : {false, true})
		for (const bool base : {false, true}) {
			OptionalFeatureSupport opt;
			opt.depthClipEnable = eds3;
			opt.depthClipBase = base;
			INFO("eds3=" << eds3 << " base=" << base);
			// (T,F) 组合是本批裁决点：驱动只暴露 edS3 而无旧扩展 base → 必须关。
			CHECK(depthClipDynamicSupported(opt) == (eds3 && base));
		}
}

TEST_CASE("presentTimingSupported 真值表：三扩展位 all-or-nothing（8 组合全举）") {
	for (const bool pt : {false, true})
		for (const bool id2 : {false, true})
			for (const bool cal : {false, true}) {
				OptionalFeatureSupport opt;
				opt.presentTiming = pt;
				opt.presentId2 = id2;
				opt.calibratedTimestampsKhr = cal;
				INFO("pt=" << pt << " id2=" << id2 << " calKhr=" << cal);
				CHECK(presentTimingSupported(opt) == (pt && id2 && cal));
			}
}

TEST_CASE("getDeviceExtensions() 无参版 = 必需清单（9 项，与校验同源）") {
	const auto exts = getDeviceExtensions();
	CHECK(exts.size() == kRequired.size());
	for (const char* name : kRequired)
		CHECK(has(exts, name));
}

TEST_CASE("getDeviceExtensions(opt)：默认全 false = 纯必需清单（可选追加的反向用例）") {
	const OptionalFeatureSupport opt;
	const auto exts = getDeviceExtensions(opt);
	CHECK(exts.size() == kRequired.size());
	CHECK_FALSE(has(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME));
	CHECK_FALSE(has(exts, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME));
	CHECK_FALSE(has(exts, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME));
	CHECK_FALSE(has(exts, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME));
	CHECK_FALSE(has(exts, VK_EXT_PRESENT_TIMING_EXTENSION_NAME));
}

TEST_CASE("getDeviceExtensions(opt)：depth clip 两位各自门控各自扩展（启用面 ≠ 调用门）") {
	// 启用清单与**调用门** depthClipDynamicSupported 刻意不同粒度：两 bit 独立追加
	// （各自探测各自启用），双条件裁决发生在调用侧（StateKit）——"enable 了但门没开"
	// 是合法降级态，读源码定此断言，不发明行为。
	{
		OptionalFeatureSupport opt;
		opt.depthClipEnable = true;
		const auto exts = getDeviceExtensions(opt);
		CHECK(has(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME));
		CHECK_FALSE(has(exts, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME));
	}
	{
		OptionalFeatureSupport opt;
		opt.depthClipBase = true;
		const auto exts = getDeviceExtensions(opt);
		CHECK(has(exts, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME));
		CHECK_FALSE(has(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME));
	}
	{
		OptionalFeatureSupport opt;
		opt.depthClipEnable = true;
		opt.depthClipBase = true;
		const auto exts = getDeviceExtensions(opt);
		CHECK(has(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME));
		CHECK(has(exts, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME));
		CHECK(exts.size() == kRequired.size() + 2);
	}
}

TEST_CASE("getDeviceExtensions(opt)：保守光栅动态位单独抬 edS3 底座（批 11.3 1b）") {
	// EDS3 底座从"只有 depthClip 用"变两位共用：删 || 条件 → 本用例红（EDS3 缺席）。
	OptionalFeatureSupport opt;
	opt.conservativeRasterizationDynamic = true;
	const auto exts = getDeviceExtensions(opt);
	CHECK(has(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME));
	CHECK_FALSE(has(exts, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME));  // 不连带旧扩展
	CHECK(exts.size() == kRequired.size() + 1);
}

TEST_CASE("getDeviceExtensions(opt)：robustness2 两 bit 共一条、calibrated EXT 独立追加") {
	for (const bool rba : {false, true})
		for (const bool nullDesc : {false, true}) {
			OptionalFeatureSupport opt;
			opt.robustBufferAccess2 = rba;
			opt.nullDescriptor = nullDesc;
			const auto exts = getDeviceExtensions(opt);
			CHECK(has(exts, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME) == (rba || nullDesc));
		}
	// EXT calibrated_timestamps 是纯扩展位（无 feature 结构），独立于 present_timing 组。
	OptionalFeatureSupport opt;
	opt.calibratedTimestamps = true;
	const auto exts = getDeviceExtensions(opt);
	CHECK(has(exts, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME));
	CHECK_FALSE(has(exts, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME));
}

TEST_CASE("getDeviceExtensions(opt)：present_timing 三扩展整组启用/整组不进") {
	// 三 bit 齐 → 三条扩展全进（KHR calibrated 由组内 calibratedTimestampsKhr 再确认，
	// 组开时该位必为 true）；EXT calibrated 位独立于本组，此组合下刻意**不**追加。
	OptionalFeatureSupport all;
	all.presentTiming = true;
	all.presentId2 = true;
	all.calibratedTimestampsKhr = true;
	const auto exts = getDeviceExtensions(all);
	CHECK(has(exts, VK_EXT_PRESENT_TIMING_EXTENSION_NAME));
	CHECK(has(exts, VK_KHR_PRESENT_ID_2_EXTENSION_NAME));
	CHECK(has(exts, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME));
	CHECK_FALSE(has(exts, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME));
	// 缺一即整组不进（"部分启用 = 启用但永不使用"被验收口径禁止）。
	for (int missing = 0; missing < 3; ++missing) {
		OptionalFeatureSupport opt = all;
		if (missing == 0) opt.presentTiming = false;
		if (missing == 1) opt.presentId2 = false;
		if (missing == 2) opt.calibratedTimestampsKhr = false;
		const auto e = getDeviceExtensions(opt);
		INFO("missing bit #" << missing);
		CHECK_FALSE(has(e, VK_EXT_PRESENT_TIMING_EXTENSION_NAME));
		CHECK_FALSE(has(e, VK_KHR_PRESENT_ID_2_EXTENSION_NAME));
		CHECK_FALSE(has(e, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME));
	}
}

}  // namespace RGL
