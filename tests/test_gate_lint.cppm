module;
/**
 * test_gate_lint —— 门组合自检（gate_lint 立项批，docs/gate_lint_plan.md）
 *
 * 被测 = rgl/gate_lint.cppm（rlscene 成员）：38 门申报规则表 + variant 交叉校验。
 * 纯函数（一切事实入参），无 GPU 无窗口，`just test` 毫秒级。
 *
 * ## 判据纪律（本文件比别处更严的原因）
 *
 * lint 的失败模式不是「漏喊」（没人知道），而是**常喊**——常错的 lint 等于被静音的
 * lint，还额外训练人不信任日志（防腐条款 2）。所以每条规则必有反向用例
 * （合法组合不得喊），且有一条「全合法世界零 findings」的总基线。
 *
 * ## 覆盖率护栏（同 pass_vocabulary 范式）
 *
 * 规则表门名集合 == diagnostics 回显表键集合（双向 + 钉 38）。加门不答
 * 「生效前置是什么」→ 红。nullptr 谓词行 = 显式「无前置」，同样占一行被钉住。
 */
#include <doctest/doctest.h>

export module test_gate_lint;

import rlstd;
import gate_lint;
import diagnostics;
import render_graph;
import render_graph_config;  // RgGraphVariant

namespace RGL {

namespace {

bool hasGate(const std::vector<std::string>& v, std::string_view k) {
	for (const auto& s : v)
		if (s == k)
			return true;
	return false;
}

/** 找某门的 finding（nullptr = 没有）。 */
const GateFinding* find(const std::vector<GateFinding>& fs, std::string_view gate) {
	for (const auto& f : fs)
		if (f.gate == gate)
			return &f;
	return nullptr;
}

std::vector<RgPassPlan> passesOf(const RgGraphDesc& d) {
	auto g = compileRenderGraph(d);
	REQUIRE(g.has_value());
	return g->passes;
}

/** 全合法世界：非 headless、llm 开、设备 static 能力齐、deferred 图 + deferred 意图。 */
constexpr GateEnv kHappyEnv{.headless = false, .llmCapture = true, .staticGraphicsIndirect = true};

}  // namespace

TEST_CASE("gate_lint 覆盖率护栏：规则表门名集合 == 回显表键集合（双向 + 钉 38）") {
	// 变异红线：kGateRules 删任一行 / 改任一门名拼写 → 本用例红；
	// DiagnosticsConfig 加字段不更新两侧 → diagnosticsEchoKeys 的 38 钉先红。
	const auto rules = gateLintRuleGates();
	const auto echo = diagnosticsEchoKeys();
	CHECK(rules.size() == 38u);
	CHECK(echo.size() == 38u);
	for (const auto& k : echo)
		CHECK(hasGate(rules, k));  // 回显有而规则表没有 = 没回答「生效前置是什么」
	for (const auto& k : rules)
		CHECK(hasGate(echo, k));   // 规则表有而回显没有 = 陈旧/拼错门名
}

TEST_CASE("gate_lint 基线：全默认配置（任何图/任何世界）零 findings") {
	// 反向用例总锚：configs/diagnostics.lua 写满默认值的现实 = lint 必须沉默。
	// 变异红线：任何 nullptr 谓词行被改成恒真 → 本用例红。
	const DiagnosticsConfig d;
	const auto fwd = passesOf(defaultRenderGraphDesc());
	CHECK(lintDiagnosticsGates(d, d, fwd, RgGraphVariant::forward, {}).empty());
	CHECK(lintDiagnosticsGates(d, d, passesOf(deferredRenderGraphDesc()),
		RgGraphVariant::deferred, kHappyEnv).empty());
	// 全默认 + headless/llm关/static关 也零（门全关时环境事实不参与判定）
	const GateEnv dark{.headless = true, .llmCapture = false, .staticGraphicsIndirect = false};
	CHECK(lintDiagnosticsGates(d, d, fwd, RgGraphVariant::forward, dark).empty());
}

TEST_CASE("gate_lint error 组：izb_shadow / shadow_evssm（非 deferred 图，对称收口）") {
	const DiagnosticsConfig d;
	const auto fwd = passesOf(defaultRenderGraphDesc());
	const auto def = passesOf(deferredRenderGraphDesc());
	{  // 正例：IZB × forward = error（此前静默早退）
		DiagnosticsConfig c;
		c.izbShadow = true;
		const auto fs = lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, kHappyEnv);
		REQUIRE(find(fs, "izb_shadow") != nullptr);
		CHECK(find(fs, "izb_shadow")->severity == GateSeverity::error);
	}
	{  // 反向：IZB × deferred 合法组合不喊（且 EVSSM 同世界也不喊）
		DiagnosticsConfig c;
		c.izbShadow = true;
		c.shadowEvssm = true;
		const auto fs = lintDiagnosticsGates(c, d, def, RgGraphVariant::deferred, kHappyEnv);
		CHECK(find(fs, "izb_shadow") == nullptr);
		CHECK(find(fs, "shadow_evssm") == nullptr);
	}
	{  // 正例：EVSSM × forward = error；EVSSM × shadow_pass=false = error
		DiagnosticsConfig c;
		c.shadowEvssm = true;
		const auto fs = lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, kHappyEnv);
		REQUIRE(find(fs, "shadow_evssm") != nullptr);
		CHECK(find(fs, "shadow_evssm")->severity == GateSeverity::error);
		DiagnosticsConfig c2;
		c2.shadowEvssm = true;
		c2.shadowPass = false;
		const auto fs2 = lintDiagnosticsGates(c2, d, def, RgGraphVariant::deferred, kHappyEnv);
		CHECK(find(fs2, "shadow_evssm") != nullptr);  // deferred 图但阴影总开关关了
	}
}

TEST_CASE("gate_lint error 组：dump_gbuf / picking_probe / 输入探针×headless") {
	const DiagnosticsConfig d;
	const auto def = passesOf(deferredRenderGraphDesc());
	{  // dump_gbuf：llm 关 = error；反向 = llm 开 + deferred 不喊
		DiagnosticsConfig c;
		c.dumpGbuf = 2;
		const GateEnv noLlm{.headless = true, .llmCapture = false, .staticGraphicsIndirect = true};
		CHECK(find(lintDiagnosticsGates(c, d, def, RgGraphVariant::deferred, noLlm), "dump_gbuf")
			!= nullptr);
		CHECK(find(lintDiagnosticsGates(c, d, def, RgGraphVariant::deferred, kHappyEnv),
			"dump_gbuf") == nullptr);
	}
	{  // dump_gbuf 保持默认 -1（关）→ 即使世界全黑也不喊（未偏离默认不参与）
		const GateEnv dark{.headless = true, .llmCapture = false, .staticGraphicsIndirect = false};
		CHECK(lintDiagnosticsGates(d, d, passesOf(defaultRenderGraphDesc()),
			RgGraphVariant::forward, dark).empty());
	}
	{  // picking_probe × picking=false = error；反向：picking 开 + frame 非零不喊
		DiagnosticsConfig c;
		c.pickingProbeFrame = 30;
		CHECK(find(lintDiagnosticsGates(c, d, passesOf(defaultRenderGraphDesc()),
			RgGraphVariant::forward, kHappyEnv), "picking_probe.frame") != nullptr);
		DiagnosticsConfig c2;
		c2.picking = true;
		c2.pickingProbeFrame = 30;
		CHECK(find(lintDiagnosticsGates(c2, d, passesOf(defaultRenderGraphDesc()),
			RgGraphVariant::forward, kHappyEnv), "picking_probe.frame") == nullptr);
	}
	{  // 输入探针 × headless = error；反向：窗口模式（非 headless）不喊
		DiagnosticsConfig c;
		c.uiClickProbeFrame = 10;
		c.cameraProbeFrame = 10;
		const GateEnv headless{.headless = true, .llmCapture = true, .staticGraphicsIndirect = true};
		const auto fs = lintDiagnosticsGates(c, d, passesOf(defaultRenderGraphDesc()),
			RgGraphVariant::forward, headless);
		REQUIRE(find(fs, "ui_click_probe.frame") != nullptr);
		REQUIRE(find(fs, "camera_probe.frame") != nullptr);
		CHECK(find(fs, "ui_click_probe.frame")->severity == GateSeverity::error);
		CHECK(find(lintDiagnosticsGates(c, d, passesOf(defaultRenderGraphDesc()),
			RgGraphVariant::forward, kHappyEnv), "ui_click_probe.frame") == nullptr);
	}
}

TEST_CASE("gate_lint warn 组：dgc_static 能力不符 / imgui×headless / pcss 让位 / rpc_port 孤儿") {
	const DiagnosticsConfig d;
	const auto fwd = passesOf(defaultRenderGraphDesc());
	{  // dgc_static：设备无 static 能力 = warn；能力齐 = 不喊（判据只看能力，不看厂商）
		DiagnosticsConfig c;
		c.dgcStatic = true;
		CHECK(find(lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, kHappyEnv),
			"dgc_static") == nullptr);  // kHappyEnv static=true → 合法
		const GateEnv noStatic{.headless = false, .llmCapture = true, .staticGraphicsIndirect = false};
		const auto fs = lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, noStatic);
		REQUIRE(find(fs, "dgc_static") != nullptr);
		CHECK(find(fs, "dgc_static")->severity == GateSeverity::warn);
	}
	{  // imgui × headless = warn；窗口模式不喊
		DiagnosticsConfig c;
		c.imgui = true;
		const GateEnv headless{.headless = true, .llmCapture = true, .staticGraphicsIndirect = true};
		CHECK(find(lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, headless), "imgui")
			!= nullptr);
		CHECK(find(lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, kHappyEnv), "imgui")
			== nullptr);
	}
	{  // shadow_pcss：让位 EVSSM 或 shadow_pass=false = warn；PCSS 独开 + 阴影开 = 不喊
		DiagnosticsConfig c;
		c.shadowPcss = true;
		CHECK(find(lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, kHappyEnv),
			"shadow_pcss") == nullptr);  // 合法：shadow_pass 默认 true
		DiagnosticsConfig c2;
		c2.shadowPcss = true;
		c2.shadowEvssm = true;  // 同开 = EVSSM 优先，PCSS 让位
		CHECK(find(lintDiagnosticsGates(c2, d, passesOf(deferredRenderGraphDesc()),
			RgGraphVariant::deferred, kHappyEnv), "shadow_pcss") != nullptr);
	}
	{  // rpc_port：偏离默认但 rpc=off = warn；rpc 开 = 不喊；默认端口不喊
		DiagnosticsConfig c;
		c.rpcPort = 9000;
		CHECK(find(lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, kHappyEnv), "rpc_port")
			!= nullptr);
		DiagnosticsConfig c2;
		c2.rpcPort = 9000;
		c2.rpc = true;
		CHECK(find(lintDiagnosticsGates(c2, d, fwd, RgGraphVariant::forward, kHappyEnv), "rpc_port")
			== nullptr);
		DiagnosticsConfig c3;
		c3.rpc = true;  // 默认端口 + rpc 开 = 完全合法
		CHECK(lintDiagnosticsGates(c3, d, fwd, RgGraphVariant::forward, kHappyEnv).empty());
	}
}

TEST_CASE("gate_lint inertNote 组：主门关时的死值（izb_grid / pcss_* / 探针坐标）") {
	const DiagnosticsConfig d;
	const auto fwd = passesOf(defaultRenderGraphDesc());
	{  // pcss_* 数值：软阴影门全关 = inertNote（不是 warn 更不是 error——configs 本就写默认值）
		DiagnosticsConfig c;
		c.pcssSunDegrees = 2.0;
		const auto fs = lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, kHappyEnv);
		REQUIRE(find(fs, "pcss_sun_degrees") != nullptr);
		CHECK(find(fs, "pcss_sun_degrees")->severity == GateSeverity::inertNote);
		DiagnosticsConfig c2;
		c2.pcssSunDegrees = 2.0;
		c2.shadowPcss = true;  // 门开了数值就有效
		CHECK(find(lintDiagnosticsGates(c2, d, fwd, RgGraphVariant::forward, kHappyEnv),
			"pcss_sun_degrees") == nullptr);
	}
	{  // izb_dump_frame × izb off = warn（有人真打算 dump）；izb_grid × izb off = note（调参残留）
		DiagnosticsConfig c;
		c.izbDumpFrame = 120;
		c.izbGrid = 1024;
		const auto fs = lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, kHappyEnv);
		REQUIRE(find(fs, "izb_dump_frame") != nullptr);
		CHECK(find(fs, "izb_dump_frame")->severity == GateSeverity::warn);
		REQUIRE(find(fs, "izb_grid") != nullptr);
		CHECK(find(fs, "izb_grid")->severity == GateSeverity::inertNote);
		DiagnosticsConfig c2;
		c2.izbShadow = true;
		c2.izbDumpFrame = 120;
		c2.izbGrid = 1024;
		// deferred 图 + izb 开 → 都不喊（warn 组里 izb_grid 只在主门关时喊）
		const auto fs2 = lintDiagnosticsGates(c2, d, passesOf(deferredRenderGraphDesc()),
			RgGraphVariant::deferred, kHappyEnv);
		CHECK(find(fs2, "izb_dump_frame") == nullptr);
		CHECK(find(fs2, "izb_grid") == nullptr);
	}
	{  // 探针坐标 × frame=0 = inertNote；frame 设了坐标就有效
		DiagnosticsConfig c;
		c.pickingProbeX = 640;
		c.uiClickProbeY = 360;
		c.cameraProbeHold = 20;
		const auto fs = lintDiagnosticsGates(c, d, fwd, RgGraphVariant::forward, kHappyEnv);
		CHECK(find(fs, "picking_probe.x")->severity == GateSeverity::inertNote);
		CHECK(find(fs, "ui_click_probe.y")->severity == GateSeverity::inertNote);
		CHECK(find(fs, "camera_probe.holdFrames")->severity == GateSeverity::inertNote);
	}
}

TEST_CASE("gate_lint variant 交叉校验：意图与图事实的分歧两个方向都喊") {
	const DiagnosticsConfig d;
	const auto fwd = passesOf(defaultRenderGraphDesc());
	const auto def = passesOf(deferredRenderGraphDesc());
	{  // variant=deferred 但图没有 light pass（内建图被改坏的形态）= error
		const auto fs = lintDiagnosticsGates(d, d, fwd, RgGraphVariant::deferred, kHappyEnv);
		REQUIRE(find(fs, "variant") != nullptr);
		CHECK(find(fs, "variant")->severity == GateSeverity::error);
	}
	{  // variant=forward 但图有 light pass（自定义图手写 light = 合法但错位）= warn
		RgGraphDesc g = defaultRenderGraphDesc();
		bool hasLight = false;
		for (const auto& p : passesOf(g))
			hasLight = hasLight || (p.kind == RgPassKind::drawLight && p.state == "light");
		REQUIRE_FALSE(hasLight);
		// 用内建 deferred 的 pass 序列冒充 forward 变体的图（模拟 Lua 自定义）
		const auto fs = lintDiagnosticsGates(d, d, passesOf(deferredRenderGraphDesc()),
			RgGraphVariant::forward, kHappyEnv);
		REQUIRE(find(fs, "variant") != nullptr);
		CHECK(find(fs, "variant")->severity == GateSeverity::warn);
	}
	{  // 反向：两个一致组合都不喊
		CHECK(find(lintDiagnosticsGates(d, d, def, RgGraphVariant::deferred, kHappyEnv), "variant")
			== nullptr);
		CHECK(find(lintDiagnosticsGates(d, d, fwd, RgGraphVariant::forward, kHappyEnv), "variant")
			== nullptr);
	}
}

}  // namespace RGL
