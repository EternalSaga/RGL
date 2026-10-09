module;
#include <doctest/doctest.h>
#include <algorithm>  // std::ranges::sort / equal（自由函数不经 BMI 保证传递，GMF 自带）
#include <string>     // std::string / std::to_string（同上：modules 下自由函数要真 STL 头）

/**
 * test_layout_drift —— 图外布局对账护栏的**判定 + 值可读化**（rgl/utils.cppm，rlgfx）
 *
 * 被测 = `RGL::layoutDrift(what, expected, actual)`：布局一致 → `nullopt`；不一致 →
 * 含对象名与两个布局数值的可读消息。它是两处护栏（`izbLightPassPre` 的深度附件、
 * `fsrTonemapPassPre` 的色输入/深度）共用的**唯一有对错的部分**；日志发射与后果说明
 * 刻意留在调用方（两处诊断语义不同）。
 *
 * ## 为什么值得一条毫秒级单测
 *
 * 这条判定在真机上**只能靠故意改坏 barrier 才验得到**（批 14.4b 的护栏变异就是这个），
 * 而它一旦坏掉是静默的：恒 nullopt = 护栏永久失声，而"没报错"会被读成"布局没问题"。
 * 判定本身是纯函数（不碰设备），没有不测的理由。
 *
 * ## 期望值出处（独立于被测实现）
 *  - 「哪两个数该出现在消息里」= `vk::ImageLayout` 在 vulkan.h 里的枚举数值（第三方
 *    规范），不是被测函数怎么拼的串；本文件因此**只从消息里抠数字**、不断言整串文本。
 *  - 「一致 = nullopt / 不一致 = 非空」= 被测函数自己的 `@return` 契约（文档即规格）。
 *  - 「消息里恰好两个数字串」= 从 IZB 那条护栏的**旧缺陷**反推的判据：它原本打三个
 *    占位符而其中两个是同一个变量（恒等），读者会误以为拿到两个独立事实。数字个数
 *    被钉死后，"多塞一个恒等值"这类措辞退化会直接变红。
 *
 * ## 变异红线（哪个变异让我红；答不出 = 摆设）
 *   · M1 判定改成恒返回 nullopt（`if (expected == actual || true)`）→ 用例 ①③④ 红
 *   · M2 判定条件反成 `!=`（不一致才 nullopt）                      → 用例 ①②③④ 全红
 *   · 只打 expected 不打 actual（值可读化坏一半）                    → 用例 ①③ 红（少一个数）
 *   · 两个数都打 expected（旧 IZB 缺陷的形态）                       → 用例 ①③ 红（数值不符）
 *   · 对象名不拼进消息                                               → 用例 ①④ 红
 *   · 边界值 eUndefined 被特判成"恒不算漂移"                         → 用例 ③ 红
 *
 * ## 覆盖面边界（**本测试管什么、不管什么**——项目纪律：一条会被误读成"护栏已验证"的
 * 测试比没有更贵，所以宁可写明）
 *  - **管**：给定三个入参，判定的对错 + 消息里对象名与两个布局数值的在场性。
 *  - **不管**：① **调用点还在不在**——有人把 `izbLightPassPre` / `fsrTonemapPassPre` 里
 *    对 `layoutDrift` 的调用删掉，本测试**照样全绿**；② 传进去的 `expected` / `actual`
 *    是不是**真值**（调用点读错追踪表槽、或 `fsrInputLayout()` 与向 vendor 申报的 state
 *    成对漂移），本测试看不见；③ 日志是否真的落盘、级别对不对；④ 布局漂移本身会不会
 *    造成画面错误。①② 的验证手段 = **进程级护栏变异**（故意把 IZB 还原 barrier 的 `to`
 *    改坏 → `just smoke` 必须 FAIL 且消息语义不变，批 14.4b 步 2 实测过），不在本文件。
 */
export module test_layout_drift;

import rlstd;
import rlvk;           // vk::ImageLayout（被测函数的参数类型）
import vulkan_utils;   // layoutDrift

namespace RGL {
namespace {

/** 抠出消息里所有**连续数字串**（独立于被测的第二判据：只认"消息里有哪几个数"）。
 *  手写累加而不用 std::stoul：少一个自由函数依赖，且不会把 "12ab" 静默截断成 12。 */
std::vector<uint32_t> numbersIn(const std::string& text) {
	std::vector<uint32_t> out;
	uint32_t run = 0;
	bool inRun = false;
	for (const char c : text) {
		if (c >= '0' && c <= '9') {
			run = run * 10u + static_cast<uint32_t>(c - '0');
			inRun = true;
			continue;
		}
		if (inRun) {
			out.push_back(run);
			run = 0;
			inRun = false;
		}
	}
	if (inRun)
		out.push_back(run);
	return out;
}

/** 阳性锚点：漂移必须响，且消息**可排查**——点名是哪个对象、两个布局各是多少。
 *  ⚠ 对象名不得含数字（否则"恰好两个数字串"这条判据被对象名污染）；两处真实调用点
 *  （"izb: 深度附件" / "fsr: 色输入附件"）都不含数字。 */
void expectDrift(std::string_view what, vk::ImageLayout expected, vk::ImageLayout actual) {
	const auto msg = layoutDrift(what, expected, actual);
	REQUIRE(msg.has_value());
	INFO("what = ", what, " msg = ", *msg);
	// ① 对象名原样在场（读者靠它点名是哪张图/哪个输入漂了）。
	CHECK(msg->find(what) != std::string::npos);
	// ② 恰好两个数字串（多一个恒等值 = 旧 IZB 缺陷；少一个 = 值可读化坏了一半）。
	auto nums = numbersIn(*msg);
	CHECK(nums.size() == 2);
	// ③ 那两个数**就是**两个布局的枚举数值（出处 = vulkan.h，不抄被测实现）。
	//    排序后比较 = 只钉"是哪两个数"，不钉措辞与先后（措辞是调用方的事）。
	std::array<uint32_t, 2> want{static_cast<uint32_t>(expected), static_cast<uint32_t>(actual)};
	std::ranges::sort(nums);
	std::ranges::sort(want);
	CHECK(std::ranges::equal(nums, want));
}

}  // namespace

TEST_CASE("layoutDrift ①阳性锚点：布局不同必须响，消息含对象名与两个布局的数值") {
	// 覆盖两处真实调用点的实际组合格径：IZB = 追踪表深度布局 vs recordFrame 返回值；
	// FSR = 向 vendor 申报的布局 vs 追踪表里的色输入/深度布局。
	expectDrift("izb: 深度附件", vk::ImageLayout::eDepthStencilAttachmentOptimal,
		vk::ImageLayout::eColorAttachmentOptimal);
	expectDrift("fsr: 色输入附件", vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::ImageLayout::eColorAttachmentOptimal);
	expectDrift("fsr: 深度附件", vk::ImageLayout::eDepthStencilAttachmentOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal);
	// 边界值 eUndefined（RT 重建后追踪表的初值）必须也算漂移，不得被特判成"没布局=不报"。
	expectDrift("izb: 深度附件", vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral);
	expectDrift("izb: 深度附件", vk::ImageLayout::eGeneral, vk::ImageLayout::eUndefined);
	// 相邻数值（vulkan.h：COLOR_ATTACHMENT_OPTIMAL=2 与 DEPTH_STENCIL_ATTACHMENT_OPTIMAL=3，
	// 正是 IZB 护栏变异实测里那两个真实读数）：若实现把两个数拼成一个、或只打差值，
	// "恰好两个数 + 数值精确"两条一起变红；相邻还顺带堵"数字串边界解析错一格"。
	expectDrift("izb: 深度附件", vk::ImageLayout::eColorAttachmentOptimal,
		vk::ImageLayout::eDepthStencilAttachmentOptimal);
}

TEST_CASE("layoutDrift ②反向：布局相同必须 nullopt（含 eUndefined 等边界，不止一对）") {
	// 相等组合取多对：只测一对无法区分"判据是相等比较"与"恰好特判了那一个值"。
	// eUndefined == eUndefined 尤其关键——它是 RT 重建后追踪表的初值，若被特判成漂移，
	// 每次 resize 后第一帧都会误报（误报的护栏 = 没人看的护栏）。
	const vk::ImageLayout same[] = {
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eGeneral,
		vk::ImageLayout::eColorAttachmentOptimal,
		vk::ImageLayout::eDepthStencilAttachmentOptimal,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::ImageLayout::eAttachmentOptimal,
	};
	for (const auto layout : same) {
		INFO("layout = ", static_cast<uint32_t>(layout));
		CHECK(layoutDrift("izb: 深度附件", layout, layout) == std::nullopt);
	}
	// 对象名不影响判定（同一对布局换个名字、甚至空名字，都不该响）。
	CHECK(layoutDrift("fsr: 色输入附件", vk::ImageLayout::eGeneral,
		vk::ImageLayout::eGeneral) == std::nullopt);
	CHECK(layoutDrift("", vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral) == std::nullopt);
}

TEST_CASE("layoutDrift ③边界值对：eUndefined ↔ 其它布局必须响（不得特判追踪表初值）") {
	// 与 ② 配对：② 钉 eUndefined == eUndefined **不响**，本条钉它与任何别的值相异时
	// **响**。只有 ② 没有本条 = 无法区分"相等才 nullopt"与"eUndefined 恒 nullopt"。
	const vk::ImageLayout others[] = {
		vk::ImageLayout::eGeneral,
		vk::ImageLayout::eColorAttachmentOptimal,
		vk::ImageLayout::eDepthStencilReadOnlyOptimal,
	};
	for (const auto other : others) {
		expectDrift("izb: 深度附件", vk::ImageLayout::eUndefined, other);
		expectDrift("izb: 深度附件", other, vk::ImageLayout::eUndefined);  // 反向也要响
	}
}

TEST_CASE("layoutDrift ④对象名原样进消息（两处调用点靠它点名是哪张图）") {
	// 名字里带冒号/空格/中文都要原样出现：调用点用它区分 izb 与 fsr，被截断或丢掉
	// 就退化成"有个地方布局漂了"这种无法排查的消息。
	const std::string_view names[] = {
		"izb: 深度附件", "fsr: 色输入附件", "fsr: 深度附件", "带冒号: 与空格的名字",
	};
	for (const auto what : names) {
		const auto msg = layoutDrift(what, vk::ImageLayout::eGeneral,
			vk::ImageLayout::eColorAttachmentOptimal);
		REQUIRE(msg.has_value());
		INFO("what = ", what, " msg = ", *msg);
		CHECK(msg->find(what) != std::string::npos);
	}
}

}  // namespace RGL
