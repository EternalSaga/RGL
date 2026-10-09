module;
/**
 * test_gate_lint —— 门组合自检（gate_lint 立项批，docs/gate_lint_plan.md）
 *
 * 被测 = rgl/gate_lint.cppm（rlscene 成员）：41 门申报规则表（diagnostics 侧 30 +
 * shadow.lua 侧 11，批 N6b）+ variant 交叉校验。
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
 * 规则表门名集合 == diagnostics 回显表键集合 ∪ shadow 回显表键集合（双向 + 钉 41）。
 * 加门不答「生效前置是什么」→ 红。nullptr 谓词行 = 显式「无前置」，同样占一行被钉住。
 */
#include <doctest/doctest.h>
#include <algorithm>  // std::ranges::sort / unique / count（自由函数不经 BMI 保证传递，GMF 自带）
#include <string>     // std::string 的 operator<（同上：ranges::sort 比较 std::string 时要它）

export module test_gate_lint;

import rlstd;
import gate_lint;
import diagnostics;
import shadow_config;
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

/** 去重后的门数（规则表允许一门多行，覆盖率钉的是门数不是行数）。 */
size_t distinctGateCount(std::vector<std::string> v) {
	std::ranges::sort(v);
	v.erase(std::ranges::unique(v).begin(), v.end());
	return v.size();
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

/**
 * forward 臂的阴影侧配置：出厂形态是 csm + **izb 开**，而 izb 只接 deferred 光照 pass
 * ⟹「全默认 × forward」本身就是规则该喊的 error（不是沉默）。需要"forward 上合法"的
 * 用例一律显式关 izb——测的是「合法世界沉默」，不是「全默认在任何图合法」。
 */
constexpr ShadowConfig kShadowFwd{.izb = false};

/**
 * 调用简写（N6b 后 lint 有七个入参）：两份 defaults 恒取出厂默认（旧用例本来就全传
 * `DiagnosticsConfig{}`），shadow 默认取出厂形态（csm + izb）。
 * @param shadow 阴影侧生效值（forward 臂记得传 kShadowFwd）
 */
std::vector<GateFinding> lint(const DiagnosticsConfig& cfg, std::span<const RgPassPlan> passes,
	RgGraphVariant variant, const GateEnv& env, const ShadowConfig& shadow = {}) {
	return lintDiagnosticsGates(cfg, DiagnosticsConfig{}, shadow, ShadowConfig{},
		passes, variant, env);
}

}  // namespace

TEST_CASE("gate_lint 覆盖率护栏：规则表门名集合 == 两份回显表键集合（双向 + 钉 41）") {
	// 变异红线：kGateRules 删任一行 / 改任一门名拼写 → 本用例红；
	// 任一配置结构体加字段不更新两侧 → 回显表的键数钉先红。
	// ⚠ 钉的是**去重后的门数**：一门可有多条规则（`izb` 两条 = 非 deferred 图 /
	//   mode="none"，两个根因各一条消息），行数只作"表没被整段删空"的下界哨兵。
	const auto rules = gateLintRuleGates();
	const auto echo = diagnosticsEchoKeys();
	const auto shadowEcho = shadowConfigEchoKeys();
	CHECK(rules.size() == 46u);
	CHECK(distinctGateCount(rules) == 43u);
	CHECK(echo.size() == 30u);
	CHECK(shadowEcho.size() == 13u);
	for (const auto& k : echo)
		CHECK(hasGate(rules, k));  // 回显有而规则表没有 = 没回答「生效前置是什么」
	for (const auto& k : shadowEcho)
		CHECK(hasGate(rules, k));  // 同上（shadow.lua 侧，键名是 `表.字段` 形态）
	for (const auto& k : rules) {
		const bool known = hasGate(echo, k) || hasGate(shadowEcho, k);
		CHECK(known);  // 规则表有而两份回显都没有 = 陈旧/拼错门名
	}
}

TEST_CASE("gate_lint 基线：全默认配置（任何图/任何世界）零 findings") {
	// 反向用例总锚：configs/diagnostics.lua 与 configs/shadow.lua 写满默认值的现实 =
	// lint 必须沉默。变异红线：任何 nullptr 谓词行被改成恒真 → 本用例红。
	const DiagnosticsConfig d;
	// 结构体默认 = 交付形态（csm + **izb 关**，2026-10-10 换红线）。izb 关之后交付默认在
	// **两种图上都沉默**（此前 izb 开 ⟹ forward 必有一条 izb error，"全默认零 findings"这条
	// 总锚其实一直是靠例外撑着的，现在才是真锚）。
	const auto fwd = passesOf(defaultRenderGraphDesc());
	CHECK(lint(d, fwd, RgGraphVariant::forward, {}, kShadowFwd).empty());
	CHECK(lint(d, passesOf(deferredRenderGraphDesc()), RgGraphVariant::deferred, kHappyEnv).empty());
	// 反向锚点：把 izb 显式开回来 ⟹ forward 上 izb 恰有 **2** 条（一条 error = 非 deferred
	// 图、一条 warn = 交付已关的近场 v1）。这条在，"izb 关"才不是把规则删掉才绿的。
	const ShadowConfig onIzb{.izb = true};
	CHECK(std::ranges::count(lint(d, fwd, RgGraphVariant::forward, {}, onIzb), "izb",
		&GateFinding::gate) == 2);
	// 全默认 + headless/llm关/static关 也零（门全关时环境事实不参与判定）
	const GateEnv dark{.headless = true, .llmCapture = false, .staticGraphicsIndirect = false};
	CHECK(lint(d, fwd, RgGraphVariant::forward, dark, kShadowFwd).empty());
}

TEST_CASE("gate_lint error 组：izb × {非 deferred 图, mode=none} / mode=evssm × 非 deferred") {
	const auto fwd = passesOf(defaultRenderGraphDesc());
	const auto def = passesOf(deferredRenderGraphDesc());
	{  // 正例：IZB × forward = error（此前静默早退）
		const auto fs = lint(DiagnosticsConfig{}, fwd, RgGraphVariant::forward, kHappyEnv,
			ShadowConfig{.izb = true});
		REQUIRE(find(fs, "izb") != nullptr);
		CHECK(find(fs, "izb")->severity == GateSeverity::error);
	}
	{  // 反向：IZB × deferred 合法组合不喊（mode=csm 出厂档也不喊）
		CHECK(find(lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv), "izb")
			== nullptr);
	}
	{  // 正例：mode="evssm" × forward = error（EvssmShadow 不构造 = 该档整体无效）
		const ShadowConfig evssm{.mode = ShadowMode::evssm};
		const auto fs = lint(DiagnosticsConfig{}, fwd, RgGraphVariant::forward, kHappyEnv, evssm);
		REQUIRE(find(fs, "mode") != nullptr);
		CHECK(find(fs, "mode")->severity == GateSeverity::error);
	}
	{  // 反向：mode="evssm" × deferred = 不喊（枚举下没有"总开关"可关，故旧世界
	   // 「EVSSM × shadow_pass=false」那条规则整条消失——mode=none 时 evssm 根本表达不出来）
		const ShadowConfig evssm{.mode = ShadowMode::evssm, .izb = false};
		CHECK(find(lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv, evssm),
			"mode") == nullptr);
	}
	{  // 正例（债-32）：IZB × mode="none" = error。mask 由 shadow0 段重放填充，
	   // 图集不画 ⟹ 段必空 ⟹ mask 无内容 ⟹ IZB 一帧阴影也不产
		const ShadowConfig noneIzb{.mode = ShadowMode::none, .izb = true};
		const auto fs = lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv, noneIzb);
		REQUIRE(find(fs, "izb") != nullptr);
		CHECK(find(fs, "izb")->severity == GateSeverity::error);
	}
	{  // 反向（configs/shadow.lua 现值即此组合）：**交付默认 izb 关** + mode=csm + deferred
	   // ⟹ izb 零 findings（常错的 lint = 被静音的 lint，防腐条款 2）
		CHECK(find(lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv), "izb")
			== nullptr);
	}
	{  // 两条 izb **error** 规则各自独立（非 deferred 图 + mode=none = 两个根因同时成立
	   // ⟹ 两条 error findings，各有自己的消息；合并成一条 = 丢归因）
	   // 变异红线：删掉任一条 izb error 规则 → 本断言只剩 1 条 ⟹ 红
		const ShadowConfig noneIzb{.mode = ShadowMode::none, .izb = true};
		const auto fs = lint(DiagnosticsConfig{}, fwd, RgGraphVariant::forward, kHappyEnv, noneIzb);
		// 两条 **error** 各自独立（第三条 izb finding 是"交付已关"的 warn，与本断言的归因无关，
		// 故按 severity 计数而不是按 gate 计数——按 gate 计会把 warn 混进来变成"3 条 error"的假象）
		CHECK(std::ranges::count_if(fs, [](const GateFinding& f) {
			return f.gate == "izb" && f.severity == GateSeverity::error;
		}) == 2);
	}
}

TEST_CASE("gate_lint error 组：dump_gbuf / picking_probe / 输入探针×headless") {
	const auto def = passesOf(deferredRenderGraphDesc());
	{  // dump_gbuf：llm 关 = error；反向 = llm 开 + deferred 不喊
		DiagnosticsConfig c;
		c.dumpGbuf = 2;
		const GateEnv noLlm{.headless = true, .llmCapture = false, .staticGraphicsIndirect = true};
		CHECK(find(lint(c, def, RgGraphVariant::deferred, noLlm), "dump_gbuf") != nullptr);
		CHECK(find(lint(c, def, RgGraphVariant::deferred, kHappyEnv), "dump_gbuf") == nullptr);
	}
	{  // dump_gbuf 保持默认 -1（关）→ 即使世界全黑也不喊（未偏离默认不参与）
		const GateEnv dark{.headless = true, .llmCapture = false, .staticGraphicsIndirect = false};
		CHECK(lint(DiagnosticsConfig{}, passesOf(defaultRenderGraphDesc()),
			RgGraphVariant::forward, dark, kShadowFwd).empty());
	}
	{  // picking_probe × picking=false = error；反向：picking 开 + frame 非零不喊
		DiagnosticsConfig c;
		c.pickingProbeFrame = 30;
		CHECK(find(lint(c, passesOf(defaultRenderGraphDesc()), RgGraphVariant::forward, kHappyEnv,
			kShadowFwd), "picking_probe.frame") != nullptr);
		DiagnosticsConfig c2;
		c2.picking = true;
		c2.pickingProbeFrame = 30;
		CHECK(find(lint(c2, passesOf(defaultRenderGraphDesc()), RgGraphVariant::forward, kHappyEnv,
			kShadowFwd), "picking_probe.frame") == nullptr);
	}
	{  // 输入探针 × headless = error；反向：窗口模式（非 headless）不喊
		DiagnosticsConfig c;
		c.uiClickProbeFrame = 10;
		c.cameraProbeFrame = 10;
		const GateEnv headless{.headless = true, .llmCapture = true, .staticGraphicsIndirect = true};
		const auto fs = lint(c, passesOf(defaultRenderGraphDesc()), RgGraphVariant::forward,
			headless, kShadowFwd);
		REQUIRE(find(fs, "ui_click_probe.frame") != nullptr);
		REQUIRE(find(fs, "camera_probe.frame") != nullptr);
		CHECK(find(fs, "ui_click_probe.frame")->severity == GateSeverity::error);
		CHECK(find(lint(c, passesOf(defaultRenderGraphDesc()), RgGraphVariant::forward, kHappyEnv,
			kShadowFwd), "ui_click_probe.frame") == nullptr);
	}
}

TEST_CASE("gate_lint warn 组：cull_replay 非 gpu_sync × dgc_static（DGC execute 走不到）") {
	// 债-30 方案 C 新增门。lint 的失败模式是常喊，故两条反向用例分别钉住两个假阳性方向：
	// ① gpu_sync（DGC 路真会被走到）不该喊；② 出厂默认（dgc_static 关）绝不该喊。
	const auto fwd = passesOf(defaultRenderGraphDesc());
	for (const auto mode : {CullReplayMode::cpu, CullReplayMode::gpuLagged}) {
		DiagnosticsConfig c;
		c.dgcStatic = true;
		c.cullReplay = mode;
		const auto fs = lint(c, fwd, RgGraphVariant::forward, kHappyEnv, kShadowFwd);
		REQUIRE(find(fs, "cull_replay") != nullptr);
		CHECK(find(fs, "cull_replay")->severity == GateSeverity::warn);
	}
	{  // 反向①：gpu_sync = opaque 段确实由 GPU 序列驱动 → 不喊
		DiagnosticsConfig c;
		c.dgcStatic = true;
		c.cullReplay = CullReplayMode::gpuSync;
		CHECK(find(lint(c, fwd, RgGraphVariant::forward, kHappyEnv, kShadowFwd), "cull_replay")
			== nullptr);
	}
	{  // 反向②：dgc_static 关（出厂默认）+ cull_replay=cpu（也是出厂默认）→ 不喊
		DiagnosticsConfig c;
		c.cullReplay = CullReplayMode::cpu;
		CHECK(find(lint(c, fwd, RgGraphVariant::forward, kHappyEnv, kShadowFwd), "cull_replay")
			== nullptr);
	}
}

TEST_CASE("gate_lint warn 组：dgc_static 能力不符 / imgui×headless / rpc_port 孤儿") {
	const auto fwd = passesOf(defaultRenderGraphDesc());
	{  // dgc_static：设备无 static 能力 = warn；能力齐 = 不喊（判据只看能力，不看厂商）
		DiagnosticsConfig c;
		c.dgcStatic = true;
		CHECK(find(lint(c, fwd, RgGraphVariant::forward, kHappyEnv, kShadowFwd), "dgc_static")
			== nullptr);  // kHappyEnv static=true → 合法
		const GateEnv noStatic{.headless = false, .llmCapture = true, .staticGraphicsIndirect = false};
		const auto fs = lint(c, fwd, RgGraphVariant::forward, noStatic, kShadowFwd);
		REQUIRE(find(fs, "dgc_static") != nullptr);
		CHECK(find(fs, "dgc_static")->severity == GateSeverity::warn);
	}
	{  // imgui × headless = warn；窗口模式不喊
		DiagnosticsConfig c;
		c.imgui = true;
		const GateEnv headless{.headless = true, .llmCapture = true, .staticGraphicsIndirect = true};
		CHECK(find(lint(c, fwd, RgGraphVariant::forward, headless, kShadowFwd), "imgui") != nullptr);
		CHECK(find(lint(c, fwd, RgGraphVariant::forward, kHappyEnv, kShadowFwd), "imgui") == nullptr);
	}
	{  // rpc_port：偏离默认但 rpc=off = warn；rpc 开 = 不喊；默认端口不喊
		DiagnosticsConfig c;
		c.rpcPort = 9000;
		CHECK(find(lint(c, fwd, RgGraphVariant::forward, kHappyEnv, kShadowFwd), "rpc_port")
			!= nullptr);
		DiagnosticsConfig c2;
		c2.rpcPort = 9000;
		c2.rpc = true;
		CHECK(find(lint(c2, fwd, RgGraphVariant::forward, kHappyEnv, kShadowFwd), "rpc_port")
			== nullptr);
		DiagnosticsConfig c3;
		c3.rpc = true;  // 默认端口 + rpc 开 = 完全合法
		CHECK(lint(c3, fwd, RgGraphVariant::forward, kHappyEnv, kShadowFwd).empty());
	}
}

TEST_CASE("gate_lint warn 组（shadow）：izb 开 = warn（交付默认已关，开回来必须被提醒）") {
	const auto def = passesOf(deferredRenderGraphDesc());
	// 正例：deferred + mode=csm + izb 开 ⟹ 恰一条 izb **warn**（两条 error 规则都不成立，
	// 所以只有这条）。消息里必须带两个实测读数——warn 不带上游为什么关掉的数，就会被当噪声读。
	const ShadowConfig s{.izb = true};
	const auto fs = lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv, s);
	REQUIRE(find(fs, "izb") != nullptr);
	CHECK(find(fs, "izb")->severity == GateSeverity::warn);
	CHECK(find(fs, "izb")->message.find("405048") != std::string::npos);
	CHECK(find(fs, "izb")->message.find("cascade-0") != std::string::npos);
	// 反向：交付默认（izb 关）零 findings —— 变异红线：谓词写成恒真即红
	CHECK(find(lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv), "izb")
		== nullptr);
}

TEST_CASE("gate_lint warn 组（shadow）：izb.dump_frame × izb 关") {
	const auto def = passesOf(deferredRenderGraphDesc());
	{  // 正例：izb 关但写了 dump_frame = warn（有人真打算 dump）
		const ShadowConfig s{.izb = false, .izbDumpFrame = 120};
		const auto fs = lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv, s);
		REQUIRE(find(fs, "izb.dump_frame") != nullptr);
		CHECK(find(fs, "izb.dump_frame")->severity == GateSeverity::warn);
	}
	{  // 反向：izb 开 + dump_frame 非零 = 不喊（回读的是真存在的 IZB head）
		const ShadowConfig s{.izb = true, .izbDumpFrame = 120};
		CHECK(find(lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv, s),
			"izb.dump_frame") == nullptr);
	}
	{  // 反向：izb 关但 dump_frame 保持默认 0 = 不喊（未偏离默认不参与判定）
		const ShadowConfig s{.izb = false};
		CHECK(find(lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv, s),
			"izb.dump_frame") == nullptr);
	}
}

TEST_CASE("gate_lint inertNote 组（shadow）：izb.grid 与两档调参块按 mode 判死值") {
	const auto def = passesOf(deferredRenderGraphDesc());
	const auto lintS = [&](const ShadowConfig& s) {
		return lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv, s);
	};
	{  // izb.grid × izb 关 = inertNote（调参残留，不是 warn）
		const ShadowConfig s{.izb = false, .izbGrid = 1024};
		const auto fs = lintS(s);
		REQUIRE(find(fs, "izb.grid") != nullptr);
		CHECK(find(fs, "izb.grid")->severity == GateSeverity::inertNote);
	}
	{  // 反向：izb 开 + grid 偏离 = 不喊（网格真按它构造）
		const ShadowConfig s{.izb = true, .izbGrid = 1024};
		CHECK(find(lintS(s), "izb.grid") == nullptr);
	}
	{  // pcss.* 三键：mode=csm（出厂档）时是死值；mode=pcss 时有效
		const ShadowConfig s{.pcss{2.0, 40.0, 48.0}};
		const auto fs = lintS(s);
		for (const char* k : {"pcss.sun_degrees", "pcss.search_radius", "pcss.max_penumbra"}) {
			REQUIRE(find(fs, k) != nullptr);
			CHECK(find(fs, k)->severity == GateSeverity::inertNote);
		}
		const ShadowConfig on{.mode = ShadowMode::pcss, .pcss{2.0, 40.0, 48.0}};
		const auto fs2 = lintS(on);
		for (const char* k : {"pcss.sun_degrees", "pcss.search_radius", "pcss.max_penumbra"})
			CHECK(find(fs2, k) == nullptr);
	}
	{  // evssm.* 三键：与 pcss 侧**对称**（N6b 按档分立的核心 = 旧门做不到这条，
	   // 旧世界两档共用一套 pcss_* 键，"EVSSM 的半影上限"没有自己的名字）
		const ShadowConfig s{.mode = ShadowMode::pcss, .evssm{2.0, 40.0, 48.0}};
		const auto fs = lintS(s);
		for (const char* k : {"evssm.sun_degrees", "evssm.search_radius", "evssm.max_penumbra"}) {
			REQUIRE(find(fs, k) != nullptr);
			CHECK(find(fs, k)->severity == GateSeverity::inertNote);
		}
		const ShadowConfig on{.mode = ShadowMode::evssm, .evssm{2.0, 40.0, 48.0}};
		const auto fs2 = lintS(on);
		for (const char* k : {"evssm.sun_degrees", "evssm.search_radius", "evssm.max_penumbra"})
			CHECK(find(fs2, k) == nullptr);
	}
	{  // 交叉：mode=evssm 时**反过来**是 pcss.* 死、evssm.* 活（变异红线：把两条规则的
	   // mode 判据写成同一个 ⟹ 本断言红）
		const ShadowConfig s{.mode = ShadowMode::evssm, .pcss{2.0, 40.0, 48.0}};
		const auto fs = lintS(s);
		CHECK(find(fs, "pcss.sun_degrees") != nullptr);
		CHECK(find(fs, "evssm.sun_degrees") == nullptr);
	}
}

TEST_CASE("gate_lint inertNote 组：主门关时的死值（探针坐标）") {
	const auto fwd = passesOf(defaultRenderGraphDesc());
	{  // 探针坐标 × frame=0 = inertNote；frame 设了坐标就有效
		DiagnosticsConfig c;
		c.pickingProbeX = 640;
		c.uiClickProbeY = 360;
		c.cameraProbeHold = 20;
		const auto fs = lint(c, fwd, RgGraphVariant::forward, kHappyEnv, kShadowFwd);
		CHECK(find(fs, "picking_probe.x")->severity == GateSeverity::inertNote);
		CHECK(find(fs, "ui_click_probe.y")->severity == GateSeverity::inertNote);
		CHECK(find(fs, "camera_probe.holdFrames")->severity == GateSeverity::inertNote);
	}
}

TEST_CASE("gate_lint variant 交叉校验：意图与图事实的分歧两个方向都喊") {
	const auto fwd = passesOf(defaultRenderGraphDesc());
	const auto def = passesOf(deferredRenderGraphDesc());
	{  // variant=deferred 但图没有 light pass（内建图被改坏的形态）= error
		const auto fs = lint(DiagnosticsConfig{}, fwd, RgGraphVariant::deferred, kHappyEnv,
			kShadowFwd);
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
		const auto fs = lint(DiagnosticsConfig{}, passesOf(deferredRenderGraphDesc()),
			RgGraphVariant::forward, kHappyEnv, kShadowFwd);
		REQUIRE(find(fs, "variant") != nullptr);
		CHECK(find(fs, "variant")->severity == GateSeverity::warn);
	}
	{  // 反向：两个一致组合都不喊
		CHECK(find(lint(DiagnosticsConfig{}, def, RgGraphVariant::deferred, kHappyEnv), "variant")
			== nullptr);
		CHECK(find(lint(DiagnosticsConfig{}, fwd, RgGraphVariant::forward, kHappyEnv, kShadowFwd),
			"variant") == nullptr);
	}
}

}  // namespace RGL
