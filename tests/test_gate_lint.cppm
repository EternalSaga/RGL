module;
/**
 * test_gate_lint —— 门组合自检（gate_lint 立项批，docs/gate_lint_plan.md）
 *
 * 被测 = rgl/gate_lint.cppm（rlscene 成员）：53 门申报规则表（diagnostics 侧 30 +
 * shadow.lua 侧 13 + runtime.lua **meshlet 块** 10，债-106 整块接管）+ variant 交叉校验。
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
 * 规则表门名集合 == diagnostics 回显表键集合 ∪ shadow 回显表键集合 ∪
 * runtimeConfigKnownKeys() 的 meshlet. 前缀键（债-106 三方对账；双向 + 钉行数与去重门数）。
 * 加门不答「生效前置是什么」→ 红。nullptr 谓词行 = 显式「无前置」，同样占一行被钉住。
 */
#include <doctest/doctest.h>
#include <algorithm>  // std::ranges::sort / unique / count（自由函数不经 BMI 保证传递，GMF 自带）
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）
#include <string>     // std::string 的 operator<（同上：ranges::sort 比较 std::string 时要它）

export module test_gate_lint;

import rlstd;
import gate_lint;
import diagnostics;
import shadow_config;
import runtime_config;  // 债-106：meshlet 块十枚键的申报 + runtimeConfigKnownKeys() 覆盖率输入
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

/** 某门的全部 finding（一门可多行 = 多个根因各一条消息，覆盖率钉的是门不是行）。 */
std::vector<const GateFinding*> gateFindings(const std::vector<GateFinding>& fs,
	std::string_view gate) {
	std::vector<const GateFinding*> out;
	for (const auto& f : fs)
		if (f.gate == gate)
			out.push_back(&f);
	return out;
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
 * 调用简写（债-106 后 lint 有九个入参）：三份 defaults 恒取出厂默认（旧用例本来就全传
 * `DiagnosticsConfig{}`），shadow 默认取出厂形态（csm + izb），runtime 默认取出厂态
 * （meshlet.enable = true + shadow_lod = 5，其余默认 = meshlet 规则全沉默）。
 * @param shadow 阴影侧生效值（forward 臂记得传 kShadowFwd）
 * @param runtime 运行配置生效值（meshlet 用例的变量在这；默认 = 出厂态）
 */
std::vector<GateFinding> lint(const DiagnosticsConfig& cfg, std::span<const RgPassPlan> passes,
	RgGraphVariant variant, const GateEnv& env, const ShadowConfig& shadow = {},
	const RuntimeConfig& runtime = {}) {
	return lintDiagnosticsGates(cfg, DiagnosticsConfig{}, shadow, ShadowConfig{},
		runtime, RuntimeConfig{}, passes, variant, env);
}

}  // namespace

TEST_CASE("gate_lint 覆盖率护栏：规则表门名集合 == 三方来源键集合（双向 + 钉数）") {
	// 变异红线：kGateRules 删任一行 / 改任一门名拼写 → 本用例红（行数钉 + 双向 hasGate）；
	// 任一配置结构体加字段不更新两侧 → 回显表/键表的键数钉先红；
	// 把下面的 meshlet 前缀过滤改成扫不到的前缀（如 "meshlett."）→ meshletKeys.size() == 10
	// 的钉先红（对账面缩小不会静默通过——这是本用例三方化后**新增**的哨兵）。
	// ⚠ 钉的是**去重后的门数**：一门可有多条规则（`izb` 两条 = 非 deferred 图 /
	//   mode="none"，两个根因各一条消息），行数只作"表没被整段删空"的下界哨兵。
	const auto rules = gateLintRuleGates();
	const auto echo = diagnosticsEchoKeys();
	const auto shadowEcho = shadowConfigEchoKeys();
	// 第三个来源 = runtime.lua 的 meshlet 块。前缀过滤**写死**在这里并吃 runtimeConfigKnownKeys()
	// （🔴 不许把 10 枚键名抄进测试当清单——抄了键名就等于没抄来源，键表改名会静默漏对）。
	// 为什么只有 meshlet 块：runtime.lua 其余块（ssr / wet / aa / fsr / render_scale …）
	// 刻意不在射程——没有「子键开而总门关」的孤立形态、前置也不全是纯配置组合，
	// 理由 → runtime_config_impl.cppm 的 readSsrBlock 头注释「为什么不往 gate_lint 挂号」段
	// （债-106 的射程裁决：meshlet 块**整块**接管，不挑键）。
	std::vector<std::string> meshletKeys;
	for (const auto& k : runtimeConfigKnownKeys())
		if (k.starts_with("meshlet."))
			meshletKeys.push_back(k);
	CHECK(rules.size() == 60u);
	CHECK(distinctGateCount(rules) == 53u);
	CHECK(echo.size() == 30u);
	CHECK(shadowEcho.size() == 13u);
	CHECK(meshletKeys.size() == 10u);  // 整块 10 枚（债-106 红线：接管整块 = 一枚都不能少）
	for (const auto& k : echo)
		CHECK(hasGate(rules, k));  // 回显有而规则表没有 = 没回答「生效前置是什么」
	for (const auto& k : shadowEcho)
		CHECK(hasGate(rules, k));  // 同上（shadow.lua 侧，键名是 `表.字段` 形态）
	for (const auto& k : meshletKeys)
		CHECK(hasGate(rules, k));  // 同上（runtime.lua meshlet 侧，门名 = `meshlet.字段` 全名）
	for (const auto& k : rules) {
		const bool known
			= hasGate(echo, k) || hasGate(shadowEcho, k) || hasGate(meshletKeys, k);
		CHECK(known);  // 规则表有而三方都没有 = 陈旧/拼错门名
	}
}

TEST_CASE("gate_lint 基线：全默认配置（任何图/任何世界）零 findings") {
	// 反向用例总锚：configs/diagnostics.lua 与 configs/shadow.lua 写满默认值的现实 =
	// lint 必须沉默。变异红线：任何 nullptr 谓词行被改成恒真 → 本用例红。
	// 🔴 债-106 起本锚同时是 **meshlet 出厂态**（enable = true + shadow_lod = 5，其余 8 枚
	// 默认）的总锚：lint() 的 runtime 默认参 = `RuntimeConfig{}` = 出厂态 ⟹ meshlet 14 行
	// 谓词必须全部沉默（shadow_lod 那条的判据是「≥ 0 且 !enable」而不是「偏离默认」，
	// 出厂两键同给正是为了让它不成立）。变异红线：meshlet 任一行谓词改成恒真 → 本用例红。
	const DiagnosticsConfig d;
	// 结构体默认 = 交付形态（csm + **izb 关**，2026-10-10 换红线）。izb 关之后交付默认在
	// **两种图上都沉默**（此前 izb 开 ⟹ forward 必有一条 izb error，"全默认零 findings"这条
	// 总锚其实一直是靠例外撑着的，现在才是真锚）。
	const auto fwd = passesOf(defaultRenderGraphDesc());
	CHECK(lint(d, fwd, RgGraphVariant::forward, {}, kShadowFwd).empty());
	CHECK(lint(d, passesOf(deferredRenderGraphDesc()), RgGraphVariant::deferred, kHappyEnv).empty());
	// 出厂 meshlet 态**显式**再钉一次（不靠 lint() 默认参的隐式默认——默认参改了这条也得在）：
	CHECK(lint(d, passesOf(deferredRenderGraphDesc()), RgGraphVariant::deferred, kHappyEnv,
		ShadowConfig{}, RuntimeConfig{}).empty());
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

// ═══════════════════════════════════════════════════════════════════════════
// 债-106：runtime.lua 的 meshlet 块（整块 10 枚键逐枚挂号）。判据出处 = runtime_config.cppm
// 的逐字段注释 + 消费者代码（wireMeshlet* / wireClusterRangeTable），级别判据 = 计划 §4。
// 每条规则：正例（该喊的喊了）+ 反向（合法组合不得喊）——防腐条款 2。
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/** meshlet 用例的调用简写：deferred 图 + kHappyEnv + 默认 diagnostics/shadow，只变 runtime。 */
std::vector<GateFinding> lintRt(const RuntimeConfig& runtime) {
	static const std::vector<RgPassPlan> kDef = passesOf(deferredRenderGraphDesc());
	return lint(DiagnosticsConfig{}, kDef, RgGraphVariant::deferred, kHappyEnv, ShadowConfig{},
		runtime);
}

/** 门关红线 A 的公共形态：enable = false + shadow_lod = -1（P5 验收态），再叠加传入的改动。 */
RuntimeConfig redlineA(void (*tweak)(RuntimeConfig&) = nullptr) {
	RuntimeConfig r;
	r.meshletEnable = false;
	r.meshletShadowLod = -1;
	if (tweak)
		tweak(r);
	return r;
}

}  // namespace

TEST_CASE("gate_lint meshlet 红线 A：门关态（enable=false + shadow_lod=-1，其余默认）除了该喊的不多喊") {
	// 门关态 = P5 逐位不变的验收态（旧锚 54e40665…），**不是**出厂态（shadow_lod = -1 已偏离
	// 默认 5）——但它是**合法**态：meshlet 十门的期望集合 = 空（shadow_lod 判据要「≥ 0」，
	// -1 = 本档关不是孤立；其余 9 枚未开/未偏离）。逐门钉 nullptr = 「不多喊」的机器形态；
	// 全局零条只按 meshlet 前缀计——非 meshlet 侧本组合也应为零，但那是上面总锚的口径，
	// 本用例的判据对象是 meshlet 块，别把两条红线焊成一条。
	// 变异红线：任何 meshlet 规则改成「与 enable / 偏离态无关的恒真」→ 本用例红。
	const auto fs = lintRt(redlineA());
	for (const char* k : {"meshlet.enable", "meshlet.overlay", "meshlet.debug", "meshlet.tint",
		 "meshlet.coplanar", "meshlet.switch", "meshlet.cull", "meshlet.shadow_lod",
		 "meshlet.shadow_lod_fallback", "meshlet.mdi_shadow"})
		CHECK(find(fs, k) == nullptr);
	// 「门关 grep -ci meshlet = 0」护栏的 lint 侧对等物：meshlet.* 前缀的 finding 一条都不许有
	// （上面逐门钉的是名字表，这条钉的是前缀——新增 meshlet 门忘了进上面的表也会被这条抓住）
	CHECK(std::ranges::count_if(fs, [](const GateFinding& f) {
		return std::string_view{f.gate}.starts_with("meshlet.");
	}) == 0);
}

TEST_CASE("gate_lint meshlet.shadow_lod：≥ 0 而 enable 关 = warn（★18-C 就近 WARNING 的正主）") {
	{  // 正例：enable = false 而 shadow_lod 保持出厂 5 = 描述符槽没人给
		RuntimeConfig r;
		r.meshletEnable = false;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.shadow_lod") != nullptr);
		CHECK(find(fs, "meshlet.shadow_lod")->severity == GateSeverity::warn);
	}
	{  // 反向①：出厂态（enable = true + shadow_lod = 5）不喊——判据是「≥ 0 且 !enable」，
	   // 🔴 不是「偏离默认」（-1 也偏离默认，写成 deviated 红线 A 就红了）
		CHECK(find(lintRt(RuntimeConfig{}), "meshlet.shadow_lod") == nullptr);
	}
	{  // 反向②：门关档（enable = false + shadow_lod = -1）不喊——本档关不是孤立组合
		CHECK(find(lintRt(redlineA()), "meshlet.shadow_lod") == nullptr);
	}
}

TEST_CASE("gate_lint meshlet.overlay：生效不含 enable，但 overlay && !enable = 没有条目可采") {
	{  // 正例：overlay 开 + enable 关（红线 A 打底，别把 shadow_lod 的归因混进来）
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) { r.meshletOverlay = true; }));
		REQUIRE(find(fs, "meshlet.overlay") != nullptr);
		CHECK(find(fs, "meshlet.overlay")->severity == GateSeverity::warn);
		// 消息点名装配层的事实级出声（同因不同层的读者导航）
		CHECK(find(fs, "meshlet.overlay")->message.find("wireMeshletOverlayActivation")
			!= std::string::npos);
	}
	{  // 反向：overlay 开 + enable 开 = 合法（条目烘焙宿主 = enable 的 slots）
		RuntimeConfig r;
		r.meshletOverlay = true;
		CHECK(find(lintRt(r), "meshlet.overlay") == nullptr);
	}
	{  // 反向：overlay 关（出厂默认）任何 enable 态都不喊
		CHECK(find(lintRt(redlineA()), "meshlet.overlay") == nullptr);
	}
}

TEST_CASE("gate_lint meshlet.debug / meshlet.tint：条目级变体位的死值与让位（三条规则）") {
	{  // debug 正例①：enable 开 + overlay 关 = 强制归 base（债-105 的归属裁决）
		RuntimeConfig r;
		r.meshletDebug = true;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.debug") != nullptr);
		CHECK(find(fs, "meshlet.debug")->severity == GateSeverity::warn);
	}
	{  // debug 正例②：enable 关（overlay 开）= 条目根本不烘，也是死值
	   // 🔴 这条同时钉住「门关 + 仪器开」不再静默——债-106 的挂号原文形态
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) {
			r.meshletOverlay = true;
			r.meshletDebug = true;
		}));
		CHECK(find(fs, "meshlet.debug") != nullptr);
	}
	{  // debug 反向：enable + overlay 都开 = 变体真被烘，不喊
		RuntimeConfig r;
		r.meshletOverlay = true;
		r.meshletDebug = true;
		CHECK(find(lintRt(r), "meshlet.debug") == nullptr);
	}
	{  // tint 正例：overlay 关 = 死值；反向：overlay 开 = 不喊
		RuntimeConfig r;
		r.meshletTint = true;
		REQUIRE(find(lintRt(r), "meshlet.tint") != nullptr);
		r.meshletOverlay = true;
		CHECK(find(lintRt(r), "meshlet.tint") == nullptr);
	}
	{  // 让位正例：enable + overlay 开 + tint 与 debug 同真 ⟹ tint 优先**恰一条**
	   // （overlay 开 ⟹ 两条死值规则都不成立，find 到的只能是让位规则那条）
		RuntimeConfig r;
		r.meshletOverlay = true;
		r.meshletTint = true;
		r.meshletDebug = true;
		const auto fs = lintRt(r);
		const auto hits = gateFindings(fs, "meshlet.tint");
		REQUIRE(hits.size() == 1u);
		CHECK(hits[0]->message.find("tint 优先") != std::string::npos);
		CHECK(find(fs, "meshlet.debug") == nullptr);
	}
	{  // 让位反向：tint 与 debug 同真但 overlay 关 ⟹ 喊的是两条**死值**，让位规则不喊
	   // （变体 = base，没有谁优先这回事）。变异红线：让位谓词删掉 meshletOverlay 条件 → tint
	   // 变 2 条 ⟹ 本断言红。
		RuntimeConfig r;
		r.meshletTint = true;
		r.meshletDebug = true;
		const auto fs = lintRt(r);
		CHECK(gateFindings(fs, "meshlet.tint").size() == 1u);
		CHECK(gateFindings(fs, "meshlet.tint")[0]->message.find("tint 优先") == std::string::npos);
		CHECK(gateFindings(fs, "meshlet.debug").size() == 1u);
	}
}

TEST_CASE("gate_lint meshlet.coplanar：偏离 off 而无叠加 draw = warn；off 与合法组合不喊") {
	{  // 正例①：enable 开 + overlay 关 + coplanar = equal（setter 设了但没有 draw 可作用）
		RuntimeConfig r;
		r.meshletCoplanar = MeshletCoplanarMode::equal;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.coplanar") != nullptr);
		CHECK(find(fs, "meshlet.coplanar")->severity == GateSeverity::warn);
	}
	{  // 正例②：enable 关 + coplanar = ne（红线 A 打底只变 coplanar）
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) {
			r.meshletCoplanar = MeshletCoplanarMode::ne;
		}));
		CHECK(find(fs, "meshlet.coplanar") != nullptr);
	}
	{  // 反向：enable + overlay 都开 + coplanar = equal = 合法四臂档，不喊
		RuntimeConfig r;
		r.meshletOverlay = true;
		r.meshletCoplanar = MeshletCoplanarMode::equal;
		CHECK(find(lintRt(r), "meshlet.coplanar") == nullptr);
	}
	{  // 反向：overlay 关但 coplanar 保持 off（出厂默认）= 未偏离，不喊
		CHECK(find(lintRt(RuntimeConfig{}), "meshlet.coplanar") == nullptr);
	}
}

TEST_CASE("gate_lint meshlet.switch：需 enable / 只在 cull_replay=cpu 生效 / 与 overlay 互斥（三条）") {
	static const std::vector<RgPassPlan> kDef = passesOf(deferredRenderGraphDesc());
	{  // 正例①：switch 开 + enable 关 = 无 draw 可取代（恰一条——cpu 默认档下另两条不成立）
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) { r.meshletSwitch = true; }));
		REQUIRE(find(fs, "meshlet.switch") != nullptr);
		CHECK(gateFindings(fs, "meshlet.switch").size() == 1u);
		CHECK(find(fs, "meshlet.switch")->severity == GateSeverity::warn);
	}
	{  // 正例②：enable 开 + switch 开 + cull_replay = gpu_sync = 段内换管线不在那条路上
		DiagnosticsConfig d;
		d.cullReplay = CullReplayMode::gpuSync;
		RuntimeConfig r;
		r.meshletSwitch = true;
		const auto fs = lint(d, kDef, RgGraphVariant::deferred, kHappyEnv, ShadowConfig{}, r);
		REQUIRE(find(fs, "meshlet.switch") != nullptr);
		CHECK(find(fs, "meshlet.switch")->message.find("wireMeshletSwitch") != std::string::npos);
	}
	{  // 正例③：enable + switch + overlay 同开（cpu 档）= 互斥，overlay 会被清空
		RuntimeConfig r;
		r.meshletSwitch = true;
		r.meshletOverlay = true;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.switch") != nullptr);
		CHECK(gateFindings(fs, "meshlet.switch").size() == 1u);  // 只有互斥这一条
	}
	{  // 🔴 半边反向：switch + overlay 同开但 cull_replay = gpu_sync ⟹ switch 不生效 ⟹
	   // overlay **不会**被清空 ⟹ 互斥那条必须不喊（只剩 cull_replay 那条）。
	   // 变异红线：互斥谓词删掉 `cullReplay == cpu` 这个合取 → 本断言红（两条一起喊）。
		DiagnosticsConfig d;
		d.cullReplay = CullReplayMode::gpuSync;
		RuntimeConfig r;
		r.meshletSwitch = true;
		r.meshletOverlay = true;
		const auto fs = lint(d, kDef, RgGraphVariant::deferred, kHappyEnv, ShadowConfig{}, r);
		REQUIRE(gateFindings(fs, "meshlet.switch").size() == 1u);
		CHECK(gateFindings(fs, "meshlet.switch")[0]->message.find("cull_replay")
			!= std::string::npos);
	}
	{  // 反向：enable + switch（cpu 档、overlay 关）= 合法 β 形态，meshlet 侧零 findings
		RuntimeConfig r;
		r.meshletSwitch = true;
		CHECK(lintRt(r).empty());
	}
}

TEST_CASE("gate_lint meshlet.cull：需 enable && switch；同款 cull_replay 限制（两条）") {
	static const std::vector<RgPassPlan> kDef = passesOf(deferredRenderGraphDesc());
	{  // 正例①：cull 开而 switch 关（enable 开）= 标志数组无消费者
		RuntimeConfig r;
		r.meshletCull = true;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.cull") != nullptr);
		CHECK(find(fs, "meshlet.cull")->severity == GateSeverity::warn);
		CHECK(find(fs, "meshlet.cull")->message.find("wireMeshletCull") != std::string::npos);
	}
	{  // 正例②：cull + switch 同开而 **enable 关** = 仍无消费者。
	   // 🔴 这条抓「把 enable && switch 弱化成只查 switch」的变异（a && b → b）：
	   // 弱化后本组合（switch 开）谓词变假 ⟹ 本断言红。
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) {
			r.meshletSwitch = true;
			r.meshletCull = true;
		}));
		CHECK(find(fs, "meshlet.cull") != nullptr);
	}
	{  // 正例③：cull + switch + enable 但 cull_replay = gpuLagged = 同款限制（恰一条，
	   // 宿主缺失那条不叠加——它的前置里 switch 已开）
		DiagnosticsConfig d;
		d.cullReplay = CullReplayMode::gpuLagged;
		RuntimeConfig r;
		r.meshletSwitch = true;
		r.meshletCull = true;
		const auto fs = lint(d, kDef, RgGraphVariant::deferred, kHappyEnv, ShadowConfig{}, r);
		REQUIRE(gateFindings(fs, "meshlet.cull").size() == 1u);
		CHECK(gateFindings(fs, "meshlet.cull")[0]->message.find("cull_replay")
			!= std::string::npos);
	}
	{  // 反向：cull + switch + enable（cpu 档）= 合法 β+cull 形态，cull 不喊
		RuntimeConfig r;
		r.meshletSwitch = true;
		r.meshletCull = true;
		CHECK(find(lintRt(r), "meshlet.cull") == nullptr);
	}
	{  // 反向：cull 关（出厂默认）任何组合都不喊
		CHECK(find(lintRt(redlineA()), "meshlet.cull") == nullptr);
	}
}

TEST_CASE("gate_lint meshlet.shadow_lod_fallback：链式前置（lod ≥ 0 且 enable）；死值 = inertNote") {
	{  // 正例①：fallback = l0 而 shadow_lod = -1（enable 开）= 选级整段不跑，调参残留
		RuntimeConfig r;
		r.meshletShadowLod = -1;
		r.meshletShadowLodFallback = MeshletLodFallbackMode::l0;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.shadow_lod_fallback") != nullptr);
		CHECK(find(fs, "meshlet.shadow_lod_fallback")->severity == GateSeverity::inertNote);
	}
	{  // 正例②：fallback = l0 而 enable 关（shadow_lod 保持 5）= 链的第二条件
		RuntimeConfig r;
		r.meshletEnable = false;
		r.meshletShadowLodFallback = MeshletLodFallbackMode::l0;
		CHECK(find(lintRt(r), "meshlet.shadow_lod_fallback") != nullptr);
	}
	{  // 反向：fallback = l0 + enable + shadow_lod = 5 = 有消费者（逐模型选级真读它），不喊
		RuntimeConfig r;
		r.meshletShadowLodFallback = MeshletLodFallbackMode::l0;
		CHECK(find(lintRt(r), "meshlet.shadow_lod_fallback") == nullptr);
	}
	{  // 反向：fallback 保持 deepest（出厂默认）任何组合都不喊——未偏离不参与
		CHECK(find(lintRt(redlineA()), "meshlet.shadow_lod_fallback") == nullptr);
	}
}

TEST_CASE("gate_lint meshlet.mdi_shadow：宿主 = enable + shadow_lod ≥ 0（★M-1b 已接上，非零消费者）") {
	// 🔴 判据核实记录：★M-0 时代的「本批零消费者」已过期——★M-1b 落地后消费者 =
	// wireMeshletMdiShadowActivation（烘条目 + 回填 + emplace）→ renderListSystem 读 ctx。
	// 宿主条件（纯配置子集）= wireMeshletShadowArgs 第①步的 enable + shadow_lod ≥ 0；
	// 第三条（选级表非零）是装配期事实 ⟹ 装配层 warn 保留、lint 消息点名它（不判 error）。
	{  // 正例①：mdi 开 + enable 关（红线 A 打底）= 载体必然不建
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) { r.meshletMdiShadow = true; }));
		REQUIRE(find(fs, "meshlet.mdi_shadow") != nullptr);
		CHECK(find(fs, "meshlet.mdi_shadow")->severity == GateSeverity::warn);
		CHECK(find(fs, "meshlet.mdi_shadow")->message.find("wireMeshletMdiShadowActivation")
			!= std::string::npos);
	}
	{  // 正例②：mdi 开 + enable 开 + shadow_lod = -1 = 宿主条件第二脚空了
		RuntimeConfig r;
		r.meshletShadowLod = -1;
		r.meshletMdiShadow = true;
		CHECK(find(lintRt(r), "meshlet.mdi_shadow") != nullptr);
	}
	{  // 反向：mdi 开 + 出厂两键（enable + shadow_lod = 5）= 宿主条件满足，不喊
	   // （载体建没建 = 选级表事实，归装配层 wireMeshletMdiShadowActivation 出声）
		RuntimeConfig r;
		r.meshletMdiShadow = true;
		CHECK(find(lintRt(r), "meshlet.mdi_shadow") == nullptr);
	}
	{  // 反向：mdi 关（出厂默认）任何组合都不喊——门关 grep 护栏的对等物
		CHECK(find(lintRt(redlineA()), "meshlet.mdi_shadow") == nullptr);
	}
}

}  // namespace RGL
