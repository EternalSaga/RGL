module;
/**
 * test_rpc_capabilities —— `/state` 能力段拼装（rgl/debug_rpc_capabilities.cppm，
 * `docs/能力观测_plan.md` 批 1）
 *
 * ## 被测契约（三句话）
 *
 *  1. 能力段内容**一律从编译产物现算**：附件/pass 的名单与字段只吃 `CompiledRenderGraph`，
 *     不吃 `desc()`（声明）。两者今天逐条相等（编译器 1:1 保序），所以这条只能靠
 *     **脏窗口**（申报后、重编译前）做机器判据——用例 2。
 *  2. `rtBuilt` 的方向：遍历图的附件、逐个问缓存（回调注入），名单唯一来源是图；
 *     非图属 RT 档（swapchain / depth / buffer）不占缓存，**不问、字段缺席**。
 *  3. 尺寸口径与执行器同函数（`resolvedRtExtent`）：独立 extent 优先，跟随档 =
 *     渲染网格 × extentScale（floor、下限 1）。
 *
 * ## 期望值出处（独立于被测实现）
 *  - 名单/顺序类断言的期望 = 测试**自己编译出的那份 CompiledRenderGraph**（契约是
 *    「能力段 == 编译图逐条同序」，被测的是拼装，不是编译器）；
 *  - 具体值类断言（kind 拼写、尺寸、state 名）出处 = rpc_schema.lua 登记的词表与
 *    契约常量（kShadowMapSize / kSsrTraceScale 的闭式半网格 640×360），不抄被测函数；
 *  - 脏窗口用例的「输入确实分歧」有前置守卫（desc 无 highlight + 编译图有），
 *    否则用例 2 的两条主断言对零分歧输入恒真 = 假绿。
 *
 * ## 变异判据（哪个变异让我红；答不出 = 摆设，不配进门）
 *  1. 🔴 passes/attachments 改从 `graph.desc()` 取（= 计划 §6 判据 ①，现状 graphStatus
 *     的错误来源）→ 用例 2 红（脏窗口里 compiled 有 highlight、desc 已没有，能力段必须
 *     跟着编译图报「有」）。
 *  2. 🔴 `rtBuilt` 恒 true（计划 §6 判据 ②）→ 用例 1 的 ao=false 断言红、用例 4 的
 *     未建半边红。
 *  3. 🔴 门关（未做 ssr 装饰）而能力段仍报 ssr_trace（计划 §6 判据 ③，注入一条图里没有
 *     的条目即可复现）→ 用例 3 的「名单逐条相等 + 不含 ssr_*」两条红。
 *  4. rtBuilt 对所有 kind 都问（switch 分支删成无条件）→ 用例 1 的「被问名单 == 图属 RT
 *     名单」与「swapchain/depth 条目无 rtBuilt 键」两条红。
 *  5. 尺寸恒 renderExtent（忽略独立 extent）→ 用例 1 的 shadow_map 2048×2048 红；
 *     忽略 extentScale（跟随档不乘）→ 用例 4 的 640×360 红（floor 口径：721×0.5=360）。
 *  6. order 恒 0 / 用声明下标 → 用例 1 的逐条 order == 下标红。
 *  7. dirty 恒 false → 用例 2 的 dirty==true 红；dirty 恒 true → 用例 1/2 后半红
 *     （对称判据两半各钉）。
 *  8. state 字段改取 drawSource → 用例 1 的 light/tonemap state 断言红（drawLight 档
 *     drawSource 为空串）。
 *  9. 空编译图回 null/缺键而不是空数组 → 用例 5 的 is_array 红。
 *  10. fsr 段（批 2）「尺寸四键无条件出」（usable=false 也出 0）→ 用例 6 的
 *      「未构造半边无 renderWidth 键」红——报 0 把「没构造」与「构造在 0 宽上」塌成同值。
 *  11. fsr 段 temporalEffective / temporalConfig 互换（相邻两键抄错是真实失误形态）→
 *      用例 6 的逐键回显红（两键刻意取**不同**值，同值则换序不可见 = 假绿）。
 *  12. fsr 段某布尔恒 false（gateOpen 漏接线）→ 用例 6 的 true 半边红；恒 true →
 *      用例 7 的 false 半边红（对称判据两半各钉）。
 *  13. 批 3：状态码映射两 case 对调（unresolved→400）→ 用例 8 红；两 case 返回**同一个码**
 *      → 用例 8 的「两半边不许塌成一个」红（那条断言专门防"把能力事实与请求错误混为一谈"，
 *      正是用户 2026-10-19 定原则要禁的东西）。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 🔴 modules 坑：initializer_list 不从 import rlstd 带进来
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

export module test_rpc_capabilities;

import rlstd;
import rlvk;
import vkerror;
import render_graph;
import dynamic_render_graph;
import frame_capture;            // CaptureFailureKind（批 3：类别 → 状态码的入参）
import debug_rpc_capabilities;

namespace RGL {
namespace {

/** 渲染网格基准（用例内闭式换算用：半档 = 640×360）。 */
constexpr vk::Extent2D kExtent {1280u, 720u};

/** 问什么都答「建了」的 rtBuilt（只用于不关心该字段的用例；名单类断言自带录制器）。 */
const std::function<bool(std::string_view)> kAllBuilt = [](std::string_view) { return true; };

/** 录制被问名单的 rtBuilt 回调（判据 4 的读法：谁被问了、问了几次）。 */
struct RtBuiltProbe {
	std::vector<std::string> asked;
	std::set<std::string> built;  // 答 true 的名单，其余答 false
	std::function<bool(std::string_view)> callback() {
		return [this](std::string_view name) {
			asked.push_back(std::string { name });
			return built.contains(std::string { name });
		};
	}
};

/** 内建 deferred 图 + 无条件阴影装饰（= 出厂图的声明形态；装饰序抄 main 的 compileGraphOrThrow 前两步）。 */
RgGraphDesc builtinDeferredDecorated() {
	RgGraphDesc desc = deferredRenderGraphDesc();
	decorateShadowPass(desc, kShadowMapSize);
	return desc;
}

std::vector<std::string> attachmentNames(const nlohmann::json& caps) {
	std::vector<std::string> names;
	for (const auto& e : caps["attachments"])
		names.push_back(e["name"].get<std::string>());
	return names;
}

std::vector<std::string> passNames(const nlohmann::json& caps) {
	std::vector<std::string> names;
	for (const auto& e : caps["passes"])
		names.push_back(e["name"].get<std::string>());
	return names;
}

const nlohmann::json* findEntry(const nlohmann::json& arr, std::string_view name) {
	for (const auto& e : arr)
		if (e.contains("name") && e["name"].get<std::string>() == name)
			return &e;
	return nullptr;
}

const nlohmann::json* findAttachment(const nlohmann::json& caps, std::string_view name) {
	return findEntry(caps["attachments"], name);
}

/** 编译图的图属 RT 名单（offscreen / depthOffscreen 两档 = 占 RT 缓存的那两档）。 */
std::set<std::string> graphRtNames(const CompiledRenderGraph& g) {
	std::set<std::string> names;
	for (const auto& a : g.attachments)
		if (a.kind == RgAttachmentKind::offscreen ||
		    a.kind == RgAttachmentKind::depthOffscreen)
			names.insert(a.name);
	return names;
}

/** 手构最小声明（DynamicRenderGraph 的合法宿主；能力段内容不从它取，只取脏位）。 */
RgGraphDesc minimalDesc() {
	return RgGraphDesc {
		.attachments = {RgAttachmentDesc {
			.name = "swapchain", .kind = RgAttachmentKind::swapchain}},
		.passes = {RgPassDesc {.name = "opaque", .kind = RgPassKind::drawMesh,
			.state = "opaque", .drawSource = "opaque",
			.outputs = {RgPassAttachment {
				.attachment = "swapchain", .usage = RgUsage::colorWrite}}}},
	};
}

/** 手构编译产物（kind 矩阵用例：五种附件档 × 三种 pass 档，含真实编译器不给的组合）。 */
CompiledRenderGraph handCompiled() {
	CompiledRenderGraph g;
	g.attachments = {
		RgCompiledAttachment {.name = "swapchain", .kind = RgAttachmentKind::swapchain},
		RgCompiledAttachment {.name = "depth", .kind = RgAttachmentKind::depth},
		RgCompiledAttachment {.name = "follow", .kind = RgAttachmentKind::offscreen,
			.format = "rgba16_sfloat", .extentScale = 0.5f},
		RgCompiledAttachment {.name = "independent", .kind = RgAttachmentKind::offscreen,
			.format = "r8_unorm", .extent = {100u, 50u}, .extentScale = 0.5f},
		RgCompiledAttachment {.name = "shadow_like", .kind = RgAttachmentKind::depthOffscreen,
			.format = "d32_sfloat", .extent = {2048u, 2048u}},
		RgCompiledAttachment {.name = "table_buf", .kind = RgAttachmentKind::buffer},
	};
	g.passes = {
		RgPassPlan {.name = "draw", .kind = RgPassKind::drawMesh, .state = "opaque", .drawSource = "opaque"},
		RgPassPlan {.name = "copy", .kind = RgPassKind::blit},
		RgPassPlan {.name = "light", .kind = RgPassKind::drawLight, .state = "light"},
	};
	return g;
}

}  // namespace

TEST_CASE("能力段：内建 deferred 图逐条 == 编译产物（阳性锚 + 反向半边）") {
	const RgGraphDesc desc = builtinDeferredDecorated();
	auto compiled = compileRenderGraph(desc);
	REQUIRE(compiled.has_value());
	DynamicRenderGraph graph {desc};

	// ao 的 RT「没建」，其余图属 RT「建了」——rtBuilt 两半边的输入。
	RtBuiltProbe probe;
	probe.built = graphRtNames(*compiled);
	probe.built.erase("ao");
	const auto caps = rpcCapabilitiesJson(graph, *compiled, probe.callback(), kExtent, 42);

	// ── 阳性锚：先钉「非空」再遍历（空集合上的 for 恒真）──
	REQUIRE_FALSE(caps["attachments"].empty());
	REQUIRE_FALSE(caps["passes"].empty());
	CHECK(caps["frame"] == 42);
	CHECK(caps["dirty"] == false);  // 干净图（对称半边之一，另一半在用例 2）

	// ── 名单逐条同序 == 编译产物（slot 序 / 执行序）──
	std::vector<std::string> compiledAttNames;
	for (const auto& a : compiled->attachments)
		compiledAttNames.push_back(a.name);
	CHECK(attachmentNames(caps) == compiledAttNames);

	// ── 具体值：kind 拼写（出处 = schema 登记的词表，不是被测函数）──
	const auto* swap = findAttachment(caps, "swapchain");
	REQUIRE(swap != nullptr);
	CHECK((*swap)["kind"] == "swapchain");
	CHECK((*swap)["format"] == "inherit");
	CHECK((*swap)["width"] == 1280);  // swapchain 档 = 渲染网格
	CHECK((*swap)["height"] == 720);
	CHECK_FALSE(swap->contains("rtBuilt"));  // 反向半边：不占缓存的档**不问**
	const auto* depth = findAttachment(caps, "depth");
	REQUIRE(depth != nullptr);
	CHECK((*depth)["kind"] == "depth");
	CHECK_FALSE(depth->contains("rtBuilt"));
	const auto* hdr = findAttachment(caps, "scene_hdr");
	REQUIRE(hdr != nullptr);
	CHECK((*hdr)["kind"] == "offscreen");
	CHECK((*hdr)["format"] == "rgba16_sfloat");
	const auto* shadowMap = findAttachment(caps, "shadow_map");
	REQUIRE(shadowMap != nullptr);
	CHECK((*shadowMap)["kind"] == "depthOffscreen");
	CHECK((*shadowMap)["format"] == "d32_sfloat");
	CHECK((*shadowMap)["width"] == kShadowMapSize);  // 独立 extent 优先，不随窗口
	CHECK((*shadowMap)["height"] == kShadowMapSize);

	// ── rtBuilt 两半边：建了 = true、没建 = **false（不是缺键、不是 true）** ──
	const auto* ao = findAttachment(caps, "ao");
	REQUIRE(ao != nullptr);
	REQUIRE(ao->contains("rtBuilt"));
	CHECK((*ao)["rtBuilt"] == false);  // 变异「恒 true」在这里红（§6 判据 ②）
	CHECK((*hdr)["rtBuilt"] == true);
	CHECK((*shadowMap)["rtBuilt"] == true);

	// ── 方向约束（§3.3）：被问的恰好是图属 RT 名单，一个不多一个不少 ──
	const std::set<std::string> asked {probe.asked.begin(), probe.asked.end()};
	CHECK(asked == graphRtNames(*compiled));
	CHECK(probe.asked.size() == graphRtNames(*compiled).size());  // 每个恰好问一次

	// ── passes：逐条同序 + order == 下标 + kind/state 具体值 ──
	std::vector<std::string> compiledPassNames;
	for (const auto& p : compiled->passes)
		compiledPassNames.push_back(p.name);
	CHECK(passNames(caps) == compiledPassNames);
	REQUIRE(caps["passes"].size() == compiled->passes.size());
	for (size_t i = 0; i < caps["passes"].size(); ++i)
		CHECK(caps["passes"][i]["order"] == i);
	const auto* light = findEntry(caps["passes"], "light");
	REQUIRE(light != nullptr);
	CHECK((*light)["kind"] == "drawLight");
	CHECK((*light)["state"] == "light");  // 变异「state 取 drawSource」红（drawLight 档为空串）
	const auto* geometry = findEntry(caps["passes"], "geometry");
	REQUIRE(geometry != nullptr);
	CHECK((*geometry)["kind"] == "drawMesh");
	CHECK((*geometry)["state"] == "opaque");
}

TEST_CASE("能力段：脏窗口里跟编译产物走，不跟 desc 走（§6 判据 ①）") {
	// 场景 = 生产可达的脏窗口：highlight 启动期装饰落图并 adoptPass 登记，运行期撤回
	// （desc 已摘掉它，执行器当前编译图还有它，脏位已置、还没重编译）。
	RgGraphDesc desc = builtinDeferredDecorated();
	const auto plan = planHighlightDecoration(desc);
	REQUIRE(plan.applicable);
	REQUIRE(applyHighlightPlan(desc, plan));
	auto compiled = compileRenderGraph(desc);  // 「执行器当前持有的那份」
	REQUIRE(compiled.has_value());
	DynamicRenderGraph graph {desc};
	REQUIRE(graph.adoptPass(plan.decl).has_value());
	REQUIRE(graph.withdrawPass("highlight").has_value());

	// 前置守卫：输入**确实分歧**（图的声明侧没有、执行器编译图有），否则下面两条主断言恒真。
	// ⚠ 读 graph.desc()（DynamicRenderGraph 持有的是 move 进去的那份，withdrawPass 改的是
	// 它），不是本地 desc 副本——首版在这里读本地副本，守卫当场红：它验的是「声明侧已摘掉」，
	// 本地副本根本没被 withdrawPass 碰过。
	CHECK(std::ranges::find(graph.desc().passes, "highlight", &RgPassDesc::name) ==
	      graph.desc().passes.end());
	CHECK(std::ranges::find(compiled->passes, "highlight", &RgPassPlan::name) !=
	      compiled->passes.end());

	const auto caps = rpcCapabilitiesJson(graph, *compiled, kAllBuilt, kExtent, 7);
	// 🔴 主断言：能力段 = 这一帧实际在画什么 = 编译图 ⟹ 必须有 highlight。
	// 变异「passes 改从 graph.desc() 取」（= graphStatus 的错误来源）在这里红。
	const auto names = passNames(caps);
	CHECK(std::ranges::find(names, "highlight") != names.end());
	CHECK(caps["dirty"] == true);  // 声明侧确实变了（变异「dirty 恒 false」红）

	// 反向半边：重编译落图后，能力段跟着编译图把它摘掉、dirty 归位。
	auto fresh = graph.recompile();
	REQUIRE(fresh.has_value());
	const auto caps2 = rpcCapabilitiesJson(graph, *fresh, kAllBuilt, kExtent, 8);
	const auto names2 = passNames(caps2);
	CHECK(std::ranges::find(names2, "highlight") == names2.end());
	CHECK(caps2["dirty"] == false);
	CHECK(caps2["frame"] == 8);  // 帧号是入参回显，不是内部计数
}

TEST_CASE("能力段：ssr 门关则名单里没有 ssr_*，门开则有（§6 判据 ③ 的两半边）") {
	// 门关 = 装饰器不被调用（main 的 decorateSsrOrThrow 在 ssrEnabled=false 时的形态）。
	// 「门关不拢」的注入形态（能力段报了图里没有的条目）由「名单逐条相等」兜住。
	const RgGraphDesc off = builtinDeferredDecorated();
	auto compiledOff = compileRenderGraph(off);
	REQUIRE(compiledOff.has_value());
	DynamicRenderGraph graphOff {off};
	const auto capsOff = rpcCapabilitiesJson(graphOff, *compiledOff, kAllBuilt, kExtent, 1);
	const auto offPassNames = passNames(capsOff);
	for (const auto& name : {"ssr_trace", "ssr_applied", "ssr"}) {
		CHECK(findAttachment(capsOff, name) == nullptr);
		CHECK(std::ranges::find(offPassNames, name) == offPassNames.end());
	}
	CHECK(attachmentNames(capsOff).size() == compiledOff->attachments.size());

	// 门开半边（阳性锚：没有它，「永远不含 ssr_*」的实现也能让上面全绿）。
	RgGraphDesc on = builtinDeferredDecorated();
	REQUIRE(decorateSsrPass(on));
	auto compiledOn = compileRenderGraph(on);
	REQUIRE(compiledOn.has_value());
	DynamicRenderGraph graphOn {on};
	const auto capsOn = rpcCapabilitiesJson(graphOn, *compiledOn, kAllBuilt, kExtent, 2);
	CHECK(attachmentNames(capsOn) == [&] {
		std::vector<std::string> n;
		for (const auto& a : compiledOn->attachments)
			n.push_back(a.name);
		return n;
	}());
	const auto onPassNames = passNames(capsOn);
	CHECK(std::ranges::find(onPassNames, "ssr_trace") != onPassNames.end());
	CHECK(std::ranges::find(onPassNames, "ssr") != onPassNames.end());
	// 尺寸闭式解：trace = 渲染网格 × kSsrTraceScale（0.5）= 640×360；apply = 渲染网格。
	const auto* trace = findAttachment(capsOn, "ssr_trace");
	REQUIRE(trace != nullptr);
	CHECK((*trace)["width"] == 640);
	CHECK((*trace)["height"] == 360);
	const auto* applied = findAttachment(capsOn, "ssr_applied");
	REQUIRE(applied != nullptr);
	CHECK((*applied)["width"] == 1280);
	CHECK((*applied)["height"] == 720);
}

TEST_CASE("能力段：kind 矩阵（手构编译产物覆盖五档附件 × 三档 pass）") {
	const CompiledRenderGraph compiled = handCompiled();
	DynamicRenderGraph graph {minimalDesc()};
	RtBuiltProbe probe;
	probe.built = {"follow"};  // independent / shadow_like 没建，follow 建了
	const auto caps = rpcCapabilitiesJson(graph, compiled, probe.callback(), kExtent, 9);

	// buffer 档：无图像尺寸恒 0、不占缓存不问。
	const auto* buf = findAttachment(caps, "table_buf");
	REQUIRE(buf != nullptr);
	CHECK((*buf)["kind"] == "buffer");
	CHECK((*buf)["width"] == 0);
	CHECK((*buf)["height"] == 0);
	CHECK_FALSE(buf->contains("rtBuilt"));

	// 跟随档 × extentScale=0.5：floor 口径（1280×0.5=640、720×0.5=360）。
	const auto* follow = findAttachment(caps, "follow");
	REQUIRE(follow != nullptr);
	CHECK((*follow)["width"] == 640);
	CHECK((*follow)["height"] == 360);
	CHECK((*follow)["rtBuilt"] == true);

	// 独立 extent 优先于 extentScale（两者同时给时 extent 赢，见 RgAttachmentDesc 注释）。
	const auto* indep = findAttachment(caps, "independent");
	REQUIRE(indep != nullptr);
	CHECK((*indep)["width"] == 100);
	CHECK((*indep)["height"] == 50);
	CHECK((*indep)["rtBuilt"] == false);  // 没建半边（与用例 1 的 ao 同判据的另一输入形态）

	// 被问名单 = 恰好三个图属 RT（buffer/swapchain/depth 都不该被问）。
	const std::set<std::string> asked {probe.asked.begin(), probe.asked.end()};
	CHECK(asked == std::set<std::string> {"follow", "independent", "shadow_like"});

	// pass 三档拼写 + blit 无动态状态恒空串。
	const auto* blit = findEntry(caps["passes"], "copy");
	REQUIRE(blit != nullptr);
	CHECK((*blit)["kind"] == "blit");
	CHECK((*blit)["state"] == "");
	const auto* draw = findEntry(caps["passes"], "draw");
	REQUIRE(draw != nullptr);
	CHECK((*draw)["kind"] == "drawMesh");
	const auto* lp = findEntry(caps["passes"], "light");
	REQUIRE(lp != nullptr);
	CHECK((*lp)["kind"] == "drawLight");
}

TEST_CASE("能力段：空编译图回空数组而不是缺键/null（退化守卫）") {
	CompiledRenderGraph empty;
	DynamicRenderGraph graph {minimalDesc()};
	const auto caps = rpcCapabilitiesJson(graph, empty, kAllBuilt, kExtent, 0);
	CHECK(caps.is_object());
	CHECK(caps["attachments"].is_array());
	CHECK(caps["attachments"].empty());
	CHECK(caps["passes"].is_array());
	CHECK(caps["passes"].empty());
	CHECK(caps["frame"] == 0);
	CHECK(caps["dirty"] == false);
}

// ── fsr 段（能力观测批 2）─────────────────────────────────────────────────────
// 契约三句话：① 六枚标量键恒在、逐键回显入参（键名抄错 / 相邻键互换在这里红）；
// ② 尺寸四键**只在 usable = true 时出现**——usable = false 时报 0 会把「没构造」与
//   「构造在 0×0 上」塌成同一个值（批 2 要消灭的正是这类塌陷）；③ 本函数没有默认值、
//   不做任何裁决——申报 vs 生效的差是消费者的判据，不是拼装器的内容。
// 期望值出处 = RgFsrFacts 入参本身（回显契约），不抄被测实现。

TEST_CASE("fsr 段：usable = true 全键回显（逐键取不同值，换序必红）") {
	const auto fsr = rpcFsrJson(RgFsrFacts {
		.gateOpen = true,
		.usable = true,
		.renderWidth = 1280,
		.renderHeight = 720,
		.upscaleWidth = 1920,
		.upscaleHeight = 1080,
		.temporalEffective = false,  // 与 temporalConfig **刻意不同值**：两键互换的可见性锚
		.temporalConfig = true,
		.jitterWired = true,
		.jitterPhase = 14,
	});
	CHECK(fsr["gateOpen"] == true);
	CHECK(fsr["usable"] == true);
	CHECK(fsr["renderWidth"] == 1280);
	CHECK(fsr["renderHeight"] == 720);
	CHECK(fsr["upscaleWidth"] == 1920);
	CHECK(fsr["upscaleHeight"] == 1080);
	CHECK(fsr["temporalEffective"] == false);  // 变异「与 temporalConfig 互换」红在这里
	CHECK(fsr["temporalConfig"] == true);
	CHECK(fsr["jitterWired"] == true);
	CHECK(fsr["jitterPhase"] == 14);           // 变异「恒 0」红在这里
	CHECK(fsr.size() == 10);                   // 键数钉死：多键 = 悄悄扩契约
}

TEST_CASE("fsr 段：usable = false 时尺寸四键缺席而不是报 0（反向半边）") {
	const auto fsr = rpcFsrJson(RgFsrFacts {
		.gateOpen = true,  // 门开但接不起来（设备轴降级档）= shader16 缺失机器上的真实形态
		.usable = false,
		.renderWidth = 1280,  // 故意给非 0：出键判据必须是「usable 决定」而不是「值决定」
		.renderHeight = 720,
		.upscaleWidth = 1920,
		.upscaleHeight = 1080,
	});
	CHECK(fsr["gateOpen"] == true);
	CHECK(fsr["usable"] == false);
	CHECK_FALSE(fsr.contains("renderWidth"));  // 变异「无条件出尺寸」红（变异判据 ⑩）
	CHECK_FALSE(fsr.contains("renderHeight"));
	CHECK_FALSE(fsr.contains("upscaleWidth"));
	CHECK_FALSE(fsr.contains("upscaleHeight"));
	// 六枚标量键恒在（缺任何一枚 = 消费者侧「缺键是错」被触发，这里先钉住生产者）
	CHECK(fsr.contains("temporalEffective"));
	CHECK(fsr.contains("temporalConfig"));
	CHECK(fsr.contains("jitterWired"));
	CHECK(fsr.contains("jitterPhase"));
	CHECK(fsr.size() == 6);
	// 默认入参（全 false）的 false 半边：变异「某布尔恒 true」红在这里
	const auto off = rpcFsrJson(RgFsrFacts {});
	CHECK(off["gateOpen"] == false);
	CHECK(off["usable"] == false);
	CHECK(off["temporalEffective"] == false);
	CHECK(off["temporalConfig"] == false);
	CHECK(off["jitterWired"] == false);
	CHECK(off["jitterPhase"] == 0);
	CHECK(off.size() == 6);
}

TEST_CASE("捕获失败类别 → 状态码：两半边各钉一格，404 与 400 不许塌成一个") {
	// 契约（docs/能力观测_plan.md §4）：请求**点名**的东西不存在 = 引用解析失败 = 404（与未知
	// 路由同族，引擎没有「尝试」可言）；参数越界 = 请求本身不合协议 = 400。
	// 🔴 两条必须**不同**：塌成一个就等于把「能力事实」与「请求错误」混为一谈——用户 2026-10-19
	// 定的原则（渲染能力不得用错误回包表示）禁止的正是这个混法。
	CHECK(rpcStatusForCaptureFailure(CaptureFailureKind::unresolved) == 404);
	CHECK(rpcStatusForCaptureFailure(CaptureFailureKind::invalidArgument) == 400);
	// 反向半边：写成显式不等，是为了让「两个 case 返回同一个码」这种漂移红——只断言各自等于
	// 某个常数的话，将来有人把两档都改成 400，第一条会红但归因指向错的地方。
	CHECK(rpcStatusForCaptureFailure(CaptureFailureKind::unresolved)
		!= rpcStatusForCaptureFailure(CaptureFailureKind::invalidArgument));
}

}  // namespace RGL
