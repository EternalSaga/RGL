module;
/**
 * test_gate_lint —— 门组合自检（gate_lint 立项批，docs/gate_lint_plan.md）
 *
 * 被测 = rgl/gate_lint.cppm（rlscene 成员）：申报式规则表（三方来源 = diagnostics 回显表 +
 * shadow 回显表 + runtime.lua **meshlet 块**，债-106 整块接管；债-129 S2 把 debug+tint 两枚
 * 布尔门合并成 instrument 一枚、overlay+switch 两枚布尔门合并成 mode 一枚）。
 * 🔴 门数 / 行数不在本注释抄写——真相源 = 本文件「覆盖率护栏」用例里的钉数 CHECK。
 * 纯函数（一切事实入参），无 GPU 无窗口，`just test` 毫秒级。
 *
 * ## 判据纪律（本文件比别处更严的原因）
 *
 * lint 的失败模式不是「漏喊」（没人知道），而是**常喊**——常错的 lint 等于被静音的
 * lint，还额外训练人不信任日志（防腐条款 2）。所以每条规则必有反向用例
 * （合法组合不得喊），且有一条「全合法世界零 findings」的总基线。
 *
 * ## 覆盖率护栏（同 pass_vocabulary 范式，债-129 第二刀 D 换块级口径）
 *
 * 规则表「独立行 ∪ 块条目」门名集合 == diagnostics 回显表键集合 ∪ shadow 回显表键集合 ∪
 * runtimeConfigKnownKeys() 的 meshlet. 前缀键（三方对账；双向 + 钉行数与去重门数）。
 * 命中块前缀的来源键必须是块条目或该块宿主——加门不答「生效前置是什么」→ 红；
 * 「往块里加键不声明宿主」用 nullptr 行也躲不过 → 红。nullptr 谓词行 = 显式「无前置」，
 * 同样占一行被钉住（无前缀的挂号行去留 = 下一刀议题）。
 */
#include <doctest/doctest.h>
#include <algorithm>  // std::ranges::sort / unique / count（自由函数不经 BMI 保证传递，GMF 自带）
#include <cstdlib>    // 金标准 harness：std::getenv（env = 重生成模式；未设 = **校验模式，不是跳过**）
#include <fstream>    // 金标准 harness：重生成模式落盘 + 校验模式读 committed golden
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）
#include <string>     // std::string 的 operator<（同上：ranges::sort 比较 std::string 时要它）

export module test_gate_lint;

import rlstd;
import gate_lint;
import diagnostics;
import shadow_config;
import runtime_config;  // 债-106：meshlet 块八枚键的申报 + runtimeConfigKnownKeys() 覆盖率输入
import render_graph;

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
 * 「非 deferred 图」臂的样本（原出厂 forward 图已随 2026-10-18 图变体收敛刀②.2 删除，
 * `docs/图变体收敛_plan.md` §5）：自建一张**没有 drawLight "light"** 的最小合法图 ——
 * opaque（drawMesh）写 scene_hdr + depth，tonemap（全屏）采 scene_hdr 写 swapchain。
 * 规则表从 pass 序列里只消费 `graphHasDeferredLight` 这一条图事实，故样本取最小形态即可；
 * 仍走 `compileRenderGraph` 而不是手搓 `RgPassPlan`，是为了让样本保持「真编译产物」性质
 * ——状态 / 附件词表改名时本样本先编译失败出声，不会静默漂成一张不合法的图。
 */
std::vector<RgPassPlan> passesOfNoLightGraph() {
	return passesOf(RgGraphDesc{
		.attachments = {
			RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
			RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
			RgAttachmentDesc{.name = std::string{kSceneHdrName},
				.kind = RgAttachmentKind::offscreen, .format = std::string{kSceneHdrFormat}},
		},
		.passes = {
			RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
				.drawSource = "opaque",
				.outputs = {RgPassAttachment{.attachment = std::string{kSceneHdrName},
					                  .usage = RgUsage::colorWrite,
					                  .loadOp = RgLoadOp::clear, .storeOp = RgStoreOp::store},
					RgPassAttachment{.attachment = "depth", .usage = RgUsage::depthWrite,
						.loadOp = RgLoadOp::clear, .storeOp = RgStoreOp::store}}},
			RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
				.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite,
					.loadOp = RgLoadOp::clear, .storeOp = RgStoreOp::store}},
				.inputs = {RgPassAttachment{.attachment = std::string{kSceneHdrName},
					.usage = RgUsage::sampleRead, .loadOp = RgLoadOp::load,
					.storeOp = RgStoreOp::store}}},
		},
	});
}

/**
 * 非 deferred 图臂的阴影侧配置：出厂形态是 csm + **izb 开**，而 izb 只接 deferred 光照
 * pass ⟹「全默认 × 非 deferred 图」本身就是规则该喊的 error（不是沉默）。需要"这张图上
 * 合法"的用例一律显式关 izb——测的是「合法世界沉默」，不是「全默认在任何图合法」。
 */
constexpr ShadowConfig kShadowNoLight{.izb = false};

/**
 * 调用简写（债-106 后 lint 有九个入参）：三份 defaults 恒取出厂默认（旧用例本来就全传
 * `DiagnosticsConfig{}`），shadow 默认取出厂形态（csm + izb），runtime 默认取出厂态
 * （meshlet.assets = true + shadow_lod = 5，其余默认 = meshlet 规则全沉默）。
 * @param shadow 阴影侧生效值（非 deferred 图臂记得传 kShadowNoLight）
 * @param runtime 运行配置生效值（meshlet 用例的变量在这；默认 = 出厂态）
 */
std::vector<GateFinding> lint(const DiagnosticsConfig& cfg, std::span<const RgPassPlan> passes,
	const GateEnv& env, const ShadowConfig& shadow = {},
	const RuntimeConfig& runtime = {}) {
	return lintDiagnosticsGates(cfg, DiagnosticsConfig{}, shadow, ShadowConfig{},
		runtime, RuntimeConfig{}, passes, env);
}

}  // namespace

TEST_CASE("gate_lint 覆盖率护栏：块条目 ∪ 独立行 == 三方来源键集合（双向 + 钉数 + 块前缀宿主判据）") {
	// 变异红线（债-129 第二刀 D 换口径后）：
	//   · kGateRules 删任一行 / 改任一门名拼写 → 本用例的 `rules.size()` 钉 + 双向 hasGate 红；
	//   · kGateBlocks 删任一枚块条目 → `blockKeys.size()` 钉 + 该键「块条目 ∪ 独立行」双落空红（点名）；
	//   · 任一配置结构体加字段不更新两侧 → 回显表/键表的键数钉先红；
	//   · 把下面的 meshlet 前缀过滤改成扫不到的前缀（如 "meshlett."）→ meshletKeys.size() == 8
	//     的钉先红（对账面缩小不会静默通过）。
	// 🔴 新口径的真收益：每一枚来源键必须被「块条目」或「独立行」至少一个覆盖，两者都没有
	//   = 断言失败并**点名**；且命中块前缀的键必须是块条目或该块宿主——「往块里加一枚键而
	//   不声明宿主」（配一行 nullptr 就能躲的旧形态）绕不过去。
	// ⚠ 钉的是**去重后的门数**：一门可有多条规则（`izb.on` 三条 = 非 deferred 图 / mode="none" /
	//   交付已关 warn，各自一条消息），行数只作"表没被整段删空"的下界哨兵。
	const auto rules = gateLintRuleGates();
	const auto blocks = gateLintBlockCoverage();
	std::vector<std::string> blockKeys;
	for (const auto& b : blocks)
		for (const auto& k : b.keys)
			blockKeys.push_back(k);
	const auto echo = diagnosticsEchoKeys();
	const auto shadowEcho = shadowConfigEchoKeys();
	// 第三个来源 = runtime.lua 的 meshlet 块。前缀过滤**写死**在这里并吃 runtimeConfigKnownKeys()
	// （🔴 不许把这几枚键名抄进测试当清单——抄了键名就等于没抄来源，键表改名会静默漏对）。
	// 为什么目前只有 meshlet 块：见 runtime_config_impl.cppm 的 readSsrBlock 头注释
	// 「为什么**暂**未往 gate_lint 挂号」段（债-106 整块接管，不挑键）。
	// 🔴 2026-10-17 纠正：原在此处写的「其余块没有『子键开而总门关』的孤立形态」是**假的**——
	// `fsr.*` × `aa`、`clip_mode` × `aa` 正是该形态（一直由 smoke.py 的 guard_flags 替引擎守着）。
	// ⟹ 本前缀过滤是**未扩的射程**（债-129 S2），不是裁决结论。扩它**不需要**新真相源：
	// `runtimeConfigKnownKeys()` 已是第三个来源，meshlet 块吃的就是它。
	std::vector<std::string> meshletKeys;
	for (const auto& k : runtimeConfigKnownKeys())
		if (k.starts_with("meshlet."))
			meshletKeys.push_back(k);
	CHECK(rules.size() == 41u);       // 独立行（19 行 nullptr 挂号 + 22 行谓词；债-129 S2：
	                                  // meshlet.debug/tint 三条 → meshlet.instrument 一条，
	                                  // meshlet.switch 两条 → meshlet.mode 一条（互斥行整删））
	CHECK(blocks.size() == 6u);       // pcss / evssm / 三探针 / meshlet
	CHECK(blockKeys.size() == 19u);   // 3 + 4 + 2 + 2 + 2 + 6
	CHECK(echo.size() == 30u);
	CHECK(shadowEcho.size() == 13u);
	CHECK(meshletKeys.size() == 8u);  // 整块 8 枚（债-106 红线：接管整块 = 一枚都不能少；
	                                  // 债-129 S2 起 debug+tint 合并为 instrument、
	                                  // overlay+switch 合并为 mode）
	// 正向：每一枚来源键必须被「块条目」或「独立行」至少一个覆盖，两者都没有 = 红并点名。
	auto coveredBy = [&](std::string_view k) {
		return hasGate(rules, k) ? "独立行" : (hasGate(blockKeys, k) ? "块条目" : "");
	};
	for (const auto& k : echo)
		CHECK_MESSAGE(*coveredBy(k), "来源键既无块条目也无独立行（没回答「生效前置是什么」）: ", k);
	for (const auto& k : shadowEcho)
		CHECK_MESSAGE(*coveredBy(k), "来源键既无块条目也无独立行（没回答「生效前置是什么」）: ", k);
	for (const auto& k : meshletKeys)
		CHECK_MESSAGE(*coveredBy(k), "来源键既无块条目也无独立行（没回答「生效前置是什么」）: ", k);
	// 去重门数钉在「独立行 ∪ 块条目」的并集上（= 三方来源键数之和）。
	std::vector<std::string> unionGates = rules;
	unionGates.insert(unionGates.end(), blockKeys.begin(), blockKeys.end());
	CHECK(distinctGateCount(unionGates) == 51u);
	// 反向：独立行与块条目吃的都必须是已知来源键（陈旧/拼错门名）。
	auto known = [&](std::string_view k) {
		return hasGate(echo, k) || hasGate(shadowEcho, k) || hasGate(meshletKeys, k);
	};
	for (const auto& k : rules)
		CHECK_MESSAGE(known(k), "独立行门名不在三方来源键表（陈旧/拼错）: ", k);
	for (const auto& k : blockKeys)
		CHECK_MESSAGE(known(k), "块条目门名不在三方来源键表（陈旧/拼错）: ", k);
	// 🔴 块前缀宿主判据：命中某块前缀的来源键必须是**该块条目**或**该块宿主**——
	// 「往块里加一枚键而不声明宿主」用 nullptr 独立行也躲不过（旧 19 行零判据挂号的攒法
	// 在块命名空间里从此失效；无前缀的 nullptr 挂号行是下一刀「哪些能变派生物」的议题）。
	// 例外名单 = 全表仅有的两枚「命中前缀但形状不是 owner」的键，各有判据：
	//   · pcss.min_footprint_texels —— 结构性错位键（任何 mode 下都不生效），全表唯一，
	//     🔴 必须留独立行、不许被块吞掉（吞了 = 丢掉这条判据）；
	//   · meshlet.shadow_lod_fallback —— 链式前置（shadow_lod ≥ 0 && assets）且级别
	//     inertNote ≠ meshlet 块的 warn，塞不进块。
	// 新增例外 = 评审裁决点，必须在此写明理由。
	static const std::array<std::string_view, 2> kPrefixExceptions{
		"pcss.min_footprint_texels", "meshlet.shadow_lod_fallback"};
	for (const auto& src : {std::span<const std::string>{echo}, std::span<const std::string>{shadowEcho},
			 std::span<const std::string>{meshletKeys}})
		for (const auto& k : src)
			for (const auto& b : blocks)
				if (k.starts_with(b.prefix)) {
					const bool ok = hasGate(blockKeys, k) || k == b.owner
						|| std::ranges::find(kPrefixExceptions, k) != kPrefixExceptions.end();
					CHECK_MESSAGE(ok, "命中块前缀 ", b.prefix, " 的键既非块条目也非宿主（宿主没声明）: ", k);
				}
}

TEST_CASE("gate_lint 基线：全默认配置（任何图/任何世界）零 findings") {
	// 反向用例总锚：configs/diagnostics.lua 与 configs/shadow.lua 写满默认值的现实 =
	// lint 必须沉默。变异红线：任何 nullptr 谓词行被改成恒真 → 本用例红。
	// 🔴 债-106 起本锚同时是 **meshlet 出厂态**（assets = true + shadow_lod = 5，其余 8 枚
	// 默认）的总锚：lint() 的 runtime 默认参 = `RuntimeConfig{}` = 出厂态 ⟹ meshlet 申报
	// （债-129 D 后 = 独立 10 行 + meshlet 块 8 枚键）谓词必须全部沉默（shadow_lod 那条的
	// 判据是「≥ 0 且 !assets」而不是「偏离默认」，出厂两键同给正是为了让它不成立）。
	// 变异红线：meshlet 任一行/任一枚块条目谓词改成恒真 → 本用例红。
	const DiagnosticsConfig d;
	// 结构体默认 = 交付形态（csm + **izb 关**，2026-10-10 换红线）。izb 关之后交付默认在
	// **两种图上都沉默**（此前 izb 开 ⟹ 非 deferred 图上必有一条 izb error，"全默认零 findings"这条
	// 总锚其实一直是靠例外撑着的，现在才是真锚）。
	const auto noLight = passesOfNoLightGraph();
	CHECK(lint(d, noLight, {}, kShadowNoLight).empty());
	CHECK(lint(d, passesOf(deferredRenderGraphDesc()), kHappyEnv).empty());
	// 出厂 meshlet 态**显式**再钉一次（不靠 lint() 默认参的隐式默认——默认参改了这条也得在）：
	CHECK(lint(d, passesOf(deferredRenderGraphDesc()), kHappyEnv,
		ShadowConfig{}, RuntimeConfig{}).empty());
	// 反向锚点：把 izb 显式开回来 ⟹ 非 deferred 图上 izb 恰有 **2** 条（一条 error = 非 deferred
	// 图、一条 warn = 交付已关的近场 v1）。这条在，"izb 关"才不是把规则删掉才绿的。
	const ShadowConfig onIzb{.izb = true};
	CHECK(std::ranges::count(lint(d, noLight, {}, onIzb), "izb.on",
		&GateFinding::gate) == 2);
	// 全默认 + headless/llm关/static关 也零（门全关时环境事实不参与判定）
	const GateEnv dark{.headless = true, .llmCapture = false, .staticGraphicsIndirect = false};
	CHECK(lint(d, noLight, dark, kShadowNoLight).empty());
}

TEST_CASE("gate_lint error 组：izb × {非 deferred 图, mode=none} / mode=evssm × 非 deferred") {
	const auto noLight = passesOfNoLightGraph();
	const auto def = passesOf(deferredRenderGraphDesc());
	{  // 正例：IZB × 非 deferred 图 = error（此前静默早退）
		const auto fs = lint(DiagnosticsConfig{}, noLight, kHappyEnv,
			ShadowConfig{.izb = true});
		REQUIRE(find(fs, "izb.on") != nullptr);
		CHECK(find(fs, "izb.on")->severity == GateSeverity::error);
	}
	{  // 反向：IZB × deferred 合法组合不喊（mode=csm 出厂档也不喊）
		CHECK(find(lint(DiagnosticsConfig{}, def, kHappyEnv), "izb.on")
			== nullptr);
	}
	{  // 正例：mode="evssm" × 非 deferred 图 = error（EvssmShadow 不构造 = 该档整体无效）
		const ShadowConfig evssm{.mode = ShadowMode::evssm};
		const auto fs = lint(DiagnosticsConfig{}, noLight, kHappyEnv, evssm);
		REQUIRE(find(fs, "mode") != nullptr);
		CHECK(find(fs, "mode")->severity == GateSeverity::error);
	}
	{  // 反向：mode="evssm" × deferred = 不喊（枚举下没有"总开关"可关，故旧世界
	   // 「EVSSM × shadow_pass=false」那条规则整条消失——mode=none 时 evssm 根本表达不出来）
		const ShadowConfig evssm{.mode = ShadowMode::evssm, .izb = false};
		CHECK(find(lint(DiagnosticsConfig{}, def, kHappyEnv, evssm),
			"mode") == nullptr);
	}
	{  // 正例（债-32）：IZB × mode="none" = error。mask 由 shadow0 段重放填充，
	   // 图集不画 ⟹ 段必空 ⟹ mask 无内容 ⟹ IZB 一帧阴影也不产
		const ShadowConfig noneIzb{.mode = ShadowMode::none, .izb = true};
		const auto fs = lint(DiagnosticsConfig{}, def, kHappyEnv, noneIzb);
		REQUIRE(find(fs, "izb.on") != nullptr);
		CHECK(find(fs, "izb.on")->severity == GateSeverity::error);
	}
	{  // 反向（configs/shadow.lua 现值即此组合）：**交付默认 izb 关** + mode=csm + deferred
	   // ⟹ izb 零 findings（常错的 lint = 被静音的 lint，防腐条款 2）
		CHECK(find(lint(DiagnosticsConfig{}, def, kHappyEnv), "izb.on")
			== nullptr);
	}
	{  // 两条 izb **error** 规则各自独立（非 deferred 图 + mode=none = 两个根因同时成立
	   // ⟹ 两条 error findings，各有自己的消息；合并成一条 = 丢归因）
	   // 变异红线：删掉任一条 izb error 规则 → 本断言只剩 1 条 ⟹ 红
		const ShadowConfig noneIzb{.mode = ShadowMode::none, .izb = true};
		const auto fs = lint(DiagnosticsConfig{}, noLight, kHappyEnv, noneIzb);
		// 两条 **error** 各自独立（第三条 izb finding 是"交付已关"的 warn，与本断言的归因无关，
		// 故按 severity 计数而不是按 gate 计数——按 gate 计会把 warn 混进来变成"3 条 error"的假象）
		CHECK(std::ranges::count_if(fs, [](const GateFinding& f) {
			return f.gate == "izb.on" && f.severity == GateSeverity::error;
		}) == 2);
	}
}

TEST_CASE("gate_lint error 组：dump_gbuf / picking_probe / 输入探针×headless") {
	const auto def = passesOf(deferredRenderGraphDesc());
	{  // dump_gbuf：llm 关 = error；反向 = llm 开 + deferred 不喊
		DiagnosticsConfig c;
		c.dumpGbuf = 2;
		const GateEnv noLlm{.headless = true, .llmCapture = false, .staticGraphicsIndirect = true};
		CHECK(find(lint(c, def, noLlm), "dump_gbuf") != nullptr);
		CHECK(find(lint(c, def, kHappyEnv), "dump_gbuf") == nullptr);
	}
	{  // dump_gbuf 保持默认 -1（关）→ 即使世界全黑也不喊（未偏离默认不参与）
		const GateEnv dark{.headless = true, .llmCapture = false, .staticGraphicsIndirect = false};
		CHECK(lint(DiagnosticsConfig{}, passesOfNoLightGraph(),
			dark, kShadowNoLight).empty());
	}
	{  // picking_probe × picking=false = error；反向：picking 开 + frame 非零不喊
		DiagnosticsConfig c;
		c.pickingProbeFrame = 30;
		CHECK(find(lint(c, passesOfNoLightGraph(), kHappyEnv,
			kShadowNoLight), "picking_probe.frame") != nullptr);
		DiagnosticsConfig c2;
		c2.picking = true;
		c2.pickingProbeFrame = 30;
		CHECK(find(lint(c2, passesOfNoLightGraph(), kHappyEnv,
			kShadowNoLight), "picking_probe.frame") == nullptr);
	}
	{  // 输入探针 × headless = error；反向：窗口模式（非 headless）不喊
		DiagnosticsConfig c;
		c.uiClickProbeFrame = 10;
		c.cameraProbeFrame = 10;
		const GateEnv headless{.headless = true, .llmCapture = true, .staticGraphicsIndirect = true};
		const auto fs = lint(c, passesOfNoLightGraph(),
			headless, kShadowNoLight);
		REQUIRE(find(fs, "ui_click_probe.frame") != nullptr);
		REQUIRE(find(fs, "camera_probe.frame") != nullptr);
		CHECK(find(fs, "ui_click_probe.frame")->severity == GateSeverity::error);
		CHECK(find(lint(c, passesOfNoLightGraph(), kHappyEnv,
			kShadowNoLight), "ui_click_probe.frame") == nullptr);
	}
}

TEST_CASE("gate_lint warn 组：cull_replay 非 gpu_sync × dgc_static（DGC execute 走不到）") {
	// 债-30 方案 C 新增门。lint 的失败模式是常喊，故两条反向用例分别钉住两个假阳性方向：
	// ① gpu_sync（DGC 路真会被走到）不该喊；② 出厂默认（dgc_static 关）绝不该喊。
	const auto noLight = passesOfNoLightGraph();
	for (const auto mode : {CullReplayMode::cpu, CullReplayMode::gpuLagged}) {
		DiagnosticsConfig c;
		c.dgcStatic = true;
		c.cullReplay = mode;
		const auto fs = lint(c, noLight, kHappyEnv, kShadowNoLight);
		REQUIRE(find(fs, "cull_replay") != nullptr);
		CHECK(find(fs, "cull_replay")->severity == GateSeverity::warn);
	}
	{  // 反向①：gpu_sync = opaque 段确实由 GPU 序列驱动 → 不喊
		DiagnosticsConfig c;
		c.dgcStatic = true;
		c.cullReplay = CullReplayMode::gpuSync;
		CHECK(find(lint(c, noLight, kHappyEnv, kShadowNoLight), "cull_replay")
			== nullptr);
	}
	{  // 反向②：dgc_static 关（出厂默认）+ cull_replay=cpu（也是出厂默认）→ 不喊
		DiagnosticsConfig c;
		c.cullReplay = CullReplayMode::cpu;
		CHECK(find(lint(c, noLight, kHappyEnv, kShadowNoLight), "cull_replay")
			== nullptr);
	}
}

TEST_CASE("gate_lint warn 组：dgc_static 能力不符 / imgui×headless / rpc_port 孤儿") {
	const auto noLight = passesOfNoLightGraph();
	{  // dgc_static：设备无 static 能力 = warn；能力齐 = 不喊（判据只看能力，不看厂商）
		DiagnosticsConfig c;
		c.dgcStatic = true;
		CHECK(find(lint(c, noLight, kHappyEnv, kShadowNoLight), "dgc_static")
			== nullptr);  // kHappyEnv static=true → 合法
		const GateEnv noStatic{.headless = false, .llmCapture = true, .staticGraphicsIndirect = false};
		const auto fs = lint(c, noLight, noStatic, kShadowNoLight);
		REQUIRE(find(fs, "dgc_static") != nullptr);
		CHECK(find(fs, "dgc_static")->severity == GateSeverity::warn);
	}
	{  // imgui × headless = warn；窗口模式不喊
		DiagnosticsConfig c;
		c.imgui = true;
		const GateEnv headless{.headless = true, .llmCapture = true, .staticGraphicsIndirect = true};
		CHECK(find(lint(c, noLight, headless, kShadowNoLight), "imgui") != nullptr);
		CHECK(find(lint(c, noLight, kHappyEnv, kShadowNoLight), "imgui") == nullptr);
	}
	{  // rpc_port：偏离默认但 rpc=off = warn；rpc 开 = 不喊；默认端口不喊
		DiagnosticsConfig c;
		c.rpcPort = 9000;
		CHECK(find(lint(c, noLight, kHappyEnv, kShadowNoLight), "rpc_port")
			!= nullptr);
		DiagnosticsConfig c2;
		c2.rpcPort = 9000;
		c2.rpc = true;
		CHECK(find(lint(c2, noLight, kHappyEnv, kShadowNoLight), "rpc_port")
			== nullptr);
		DiagnosticsConfig c3;
		c3.rpc = true;  // 默认端口 + rpc 开 = 完全合法
		CHECK(lint(c3, noLight, kHappyEnv, kShadowNoLight).empty());
	}
}

TEST_CASE("gate_lint warn 组（shadow）：izb 开 = warn（交付默认已关，开回来必须被提醒）") {
	const auto def = passesOf(deferredRenderGraphDesc());
	// 正例：deferred + mode=csm + izb 开 ⟹ 恰一条 izb **warn**（两条 error 规则都不成立，
	// 所以只有这条）。消息里必须带两个实测读数——warn 不带上游为什么关掉的数，就会被当噪声读。
	const ShadowConfig s{.izb = true};
	const auto fs = lint(DiagnosticsConfig{}, def, kHappyEnv, s);
	REQUIRE(find(fs, "izb.on") != nullptr);
	CHECK(find(fs, "izb.on")->severity == GateSeverity::warn);
	CHECK(find(fs, "izb.on")->message.find("405048") != std::string::npos);
	CHECK(find(fs, "izb.on")->message.find("cascade-0") != std::string::npos);
	// 反向：交付默认（izb 关）零 findings —— 变异红线：谓词写成恒真即红
	CHECK(find(lint(DiagnosticsConfig{}, def, kHappyEnv), "izb.on")
		== nullptr);
}

TEST_CASE("gate_lint warn 组（shadow）：izb.dump_frame × izb 关") {
	const auto def = passesOf(deferredRenderGraphDesc());
	{  // 正例：izb 关但写了 dump_frame = warn（有人真打算 dump）
		const ShadowConfig s{.izb = false, .izbDumpFrame = 120};
		const auto fs = lint(DiagnosticsConfig{}, def, kHappyEnv, s);
		REQUIRE(find(fs, "izb.dump_frame") != nullptr);
		CHECK(find(fs, "izb.dump_frame")->severity == GateSeverity::warn);
	}
	{  // 反向：izb 开 + dump_frame 非零 = 不喊（回读的是真存在的 IZB head）
		const ShadowConfig s{.izb = true, .izbDumpFrame = 120};
		CHECK(find(lint(DiagnosticsConfig{}, def, kHappyEnv, s),
			"izb.dump_frame") == nullptr);
	}
	{  // 反向：izb 关但 dump_frame 保持默认 0 = 不喊（未偏离默认不参与判定）
		const ShadowConfig s{.izb = false};
		CHECK(find(lint(DiagnosticsConfig{}, def, kHappyEnv, s),
			"izb.dump_frame") == nullptr);
	}
}

TEST_CASE("gate_lint inertNote 组（shadow）：izb.grid 与两档调参块按 mode 判死值") {
	const auto def = passesOf(deferredRenderGraphDesc());
	const auto lintS = [&](const ShadowConfig& s) {
		return lint(DiagnosticsConfig{}, def, kHappyEnv, s);
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
	const auto noLight = passesOfNoLightGraph();
	{  // 探针坐标 × frame=0 = inertNote；frame 设了坐标就有效
		DiagnosticsConfig c;
		c.pickingProbeX = 640;
		c.uiClickProbeY = 360;
		c.cameraProbeHold = 20;
		const auto fs = lint(c, noLight, kHappyEnv, kShadowNoLight);
		CHECK(find(fs, "picking_probe.x")->severity == GateSeverity::inertNote);
		CHECK(find(fs, "ui_click_probe.y")->severity == GateSeverity::inertNote);
		CHECK(find(fs, "camera_probe.holdFrames")->severity == GateSeverity::inertNote);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 债-106：runtime.lua 的 meshlet 块（整块挂号不挑键，键数以本文件 meshletKeys 的钉数为准）。
// 判据出处 = runtime_config.cppm 的逐字段注释 + 消费者代码（wireMeshlet* /
// wireClusterRangeTable），级别判据 = 计划 §4。
// 每条规则：正例（该喊的喊了）+ 反向（合法组合不得喊）——防腐条款 2。
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/** meshlet 用例的调用简写：deferred 图 + kHappyEnv + 默认 diagnostics/shadow，只变 runtime。 */
std::vector<GateFinding> lintRt(const RuntimeConfig& runtime) {
	static const std::vector<RgPassPlan> kDef = passesOf(deferredRenderGraphDesc());
	return lint(DiagnosticsConfig{}, kDef, kHappyEnv, ShadowConfig{},
		runtime);
}

/** 门关红线 A 的公共形态：assets = false + shadow_lod = -1（P5 验收态），再叠加传入的改动。 */
RuntimeConfig redlineA(void (*tweak)(RuntimeConfig&) = nullptr) {
	RuntimeConfig r;
	r.meshletAssets = false;
	r.meshletShadowLod = -1;
	if (tweak)
		tweak(r);
	return r;
}

}  // namespace

TEST_CASE("gate_lint meshlet 红线 A：门关态（assets=false + shadow_lod=-1，其余默认）除了该喊的不多喊") {
	// 门关态 = P5 逐位不变的验收态（旧锚 54e40665…），**不是**出厂态（shadow_lod = -1 已偏离
	// 默认 5）——但它是**合法**态：meshlet 八门的期望集合 = 空（shadow_lod 判据要「≥ 0」，
	// -1 = 本档关不是孤立；其余 7 枚未开/未偏离）。逐门钉 nullptr = 「不多喊」的机器形态；
	// 全局零条只按 meshlet 前缀计——非 meshlet 侧本组合也应为零，但那是上面总锚的口径，
	// 本用例的判据对象是 meshlet 块，别把两条红线焊成一条。
	// 变异红线：任何 meshlet 规则改成「与 assets / 偏离态无关的恒真」→ 本用例红。
	const auto fs = lintRt(redlineA());
	for (const char* k : {"meshlet.assets", "meshlet.mode", "meshlet.instrument",
		 "meshlet.coplanar", "meshlet.cull", "meshlet.shadow_lod",
		 "meshlet.shadow_lod_fallback", "meshlet.mdi_shadow"})
		CHECK(find(fs, k) == nullptr);
	// 「门关 grep -ci meshlet = 0」护栏的 lint 侧对等物：meshlet.* 前缀的 finding 一条都不许有
	// （上面逐门钉的是名字表，这条钉的是前缀——新增 meshlet 门忘了进上面的表也会被这条抓住）
	CHECK(std::ranges::count_if(fs, [](const GateFinding& f) {
		return std::string_view{f.gate}.starts_with("meshlet.");
	}) == 0);
}

TEST_CASE("gate_lint meshlet.shadow_lod：≥ 0 而 assets 关 = warn（★18-C 就近 WARNING 的正主）") {
	{  // 正例：assets = false 而 shadow_lod 保持出厂 5 = 描述符槽没人给
		RuntimeConfig r;
		r.meshletAssets = false;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.shadow_lod") != nullptr);
		CHECK(find(fs, "meshlet.shadow_lod")->severity == GateSeverity::warn);
	}
	{  // 反向①：出厂态（assets = true + shadow_lod = 5）不喊——判据是「≥ 0 且 !assets」，
	   // 🔴 不是「偏离默认」（-1 也偏离默认，写成 deviated 红线 A 就红了）
		CHECK(find(lintRt(RuntimeConfig{}), "meshlet.shadow_lod") == nullptr);
	}
	{  // 反向②：门关档（assets = false + shadow_lod = -1）不喊——本档关不是孤立组合
		CHECK(find(lintRt(redlineA()), "meshlet.shadow_lod") == nullptr);
	}
}

TEST_CASE("gate_lint meshlet.mode：按档前置（overlay **不含** assets / switch **需** assets）+ cull 档限制") {
	// 🔴 债-129 S2 O1-1.2 判据本体（拍板 D 约束②「owner 不只有布尔」③「块吞不得」的落地）：
	// overlay + switch 两布尔合并成一枚 mode 后，**两条不同的生效前置必须按档各自成立**——
	//   · `mode = "overlay"` × `assets = false` = **合法组合，不判死值**（overlay 的宿主是叠加段
	//     自己；把它申报进 owner = assets 的块门下 = 假申报。零条目可采的落空事实归装配层
	//     wireMeshletOverlayActivation 按事实出声，lint 沉默）。
	//   · `mode = "switch"` × `assets = false` = **判死值**（簇条目没烘 ⟹ 无 draw 可原位取代，
	//     meshlet 块条目出声）。
	// 原「meshlet.overlay 块条目（overlay && !assets 发射）」**整删**——它的发射条件就是把 overlay
	// 吞进 assets 门下；原「meshlet.switch × meshlet.overlay 互斥」申报行**整删**——三值键下
	// 「同真」不可构造（消灭的是实测可达态：同开截图与 switch 单开逐位同而 warnings 1→2，
	// /tmp/opencode/o3b_premerge_anchors.txt 第四行；合并后该实验永远做不了）。
	// 变异红线声明：
	//   ① mode 块条目 present 弱化成 `meshletMode != off`（把 overlay 档也吞进 assets 门下）→
	//      「overlay × assets 关不判死」断言红（假申报复活 = 本用例的存在理由）。
	//   ② present 改成恒 false（块条目失效）→ 「switch × assets 关判死」红。
	//   ③ cull_replay 残留行谓词漏 `modeIsSwitch()`（对 overlay 档也喊）→ 「overlay × assets 开
	//      零 findings」红。
	//   ④ 残留行门名拼错（meshlet.mode → meshlet.switch）→ 覆盖率护栏双向对账先红。
	static const std::vector<RgPassPlan> kDef = passesOf(deferredRenderGraphDesc());
	{  // 🔴 正例①（判据本体 A）：mode = "switch" + assets 关（红线 A 打底）= 无 draw 可取代
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) {
			r.meshletMode = MeshletMode::inplaceSwitch;
		}));
		REQUIRE(find(fs, "meshlet.mode") != nullptr);
		CHECK(gateFindings(fs, "meshlet.mode").size() == 1u);  // 恰一条（cpu 默认档下 cull 档那条不成立）
		CHECK(find(fs, "meshlet.mode")->severity == GateSeverity::warn);
		CHECK(find(fs, "meshlet.mode")->message.find("wireMeshletOverlayActivation")
			!= std::string::npos);  // 逐键消息自带 overlay 档对照导航（断言不降格）
	}
	{  // 🔴 正例②（判据本体 B）：mode = "overlay" + assets 关（红线 A 打底）= **合法组合不判死**
	   // ——overlay 的生效前置不含 assets；meshlet.mode 一条都不许有（lint 沉默，落空事实
	   // 归装配层按「有没有条目可采」出声）。原 meshlet.overlay 块条目在这格发射 = 假申报，
	   // 随合并删除；本断言就是那条删除的判据。
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) {
			r.meshletMode = MeshletMode::overlay;
		}));
		CHECK(gateFindings(fs, "meshlet.mode").empty());
	}
	{  // 正例③：assets 开 + mode = "switch" + cull_replay = gpu_sync = 段内换管线不在那条路上
		DiagnosticsConfig d;
		d.cullReplay = CullReplayMode::gpuSync;
		RuntimeConfig r;
		r.meshletMode = MeshletMode::inplaceSwitch;
		const auto fs = lint(d, kDef, kHappyEnv, ShadowConfig{}, r);
		REQUIRE(find(fs, "meshlet.mode") != nullptr);
		CHECK(find(fs, "meshlet.mode")->message.find("wireMeshletSwitch") != std::string::npos);
	}
	{  // 半边反向：mode = "overlay" + cull_replay = gpu_sync ⟹ overlay 档不受 cull 档限制
	   // （那条前置只吃 switch 档）⟹ 零 findings。变异红线③的探针。
		DiagnosticsConfig d;
		d.cullReplay = CullReplayMode::gpuSync;
		RuntimeConfig r;
		r.meshletMode = MeshletMode::overlay;
		CHECK(gateFindings(lint(d, kDef, kHappyEnv, ShadowConfig{}, r),
			"meshlet.mode").empty());
	}
	{  // 反向①：mode = "overlay" + assets 开 = 合法 α 形态，meshlet 侧零 findings
		RuntimeConfig r;
		r.meshletMode = MeshletMode::overlay;
		CHECK(lintRt(r).empty());
	}
	{  // 反向②：mode = "switch" + assets 开（cpu 档）= 合法 β 形态，meshlet 侧零 findings
	   // （原「switch + overlay 同开」的互斥用例**整删**——输入不可构造）
		RuntimeConfig r;
		r.meshletMode = MeshletMode::inplaceSwitch;
		CHECK(lintRt(r).empty());
	}
	{  // 反向③：mode 保持出厂 off（含红线 A）任何 assets 态都不喊
		CHECK(find(lintRt(RuntimeConfig{}), "meshlet.mode") == nullptr);
		CHECK(gateFindings(lintRt(redlineA()), "meshlet.mode").empty());
	}
}

TEST_CASE("gate_lint meshlet.instrument：条目级变体位的死值（债-129 S2 三值合并，原三条 → 一条）") {
	// 🔴 原「meshlet.debug / meshlet.tint 三条规则」用例随两布尔合并重写：其中**让位两条**
	// （tint 与 debug 同真 ⟹ tint 优先恰一条 / 让位反向）**整删**——它们断的输入（两枚布尔
	// 同真）在 `MeshletInstrument` 三值键下**不可构造**，让位规则本身已随合并从申报表消失。
	// 「消灭的是一个真实可达态」的实测证据 = 合并前基线第四行（overlay+debug+tint 同真截图
	// 与 tint 单开逐位同 = tint 优先赢下），见 /tmp/opencode/o1_premerge_anchors.txt。
	// 变异红线：instrument 残留行谓词把 `!modeIsOverlay()` 删掉（弱化 = overlay 档也喊）→
	// 「反向」两条红；把 deviated 改成恒真 → 红线 A 用例红。
	{  // 正例①：assets 开 + mode = off（默认）+ offset = 强制归 base（债-105 的归属裁决）
		RuntimeConfig r;
		r.meshletInstrument = MeshletInstrument::offset;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.instrument") != nullptr);
		CHECK(find(fs, "meshlet.instrument")->severity == GateSeverity::warn);
	}
	{  // 正例②：assets 关（mode = "overlay"）+ tint = 条目根本不烘，也是死值（块条目出声）
	   // 🔴 这条同时钉住「门关 + 仪器开」不再静默——债-106 的挂号原文形态
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) {
			r.meshletMode = MeshletMode::overlay;
			r.meshletInstrument = MeshletInstrument::tint;
		}));
		CHECK(find(fs, "meshlet.instrument") != nullptr);
	}
	{  // 反向①：assets + mode = "overlay" 都开 + offset = 变体真被烘，不喊
		RuntimeConfig r;
		r.meshletMode = MeshletMode::overlay;
		r.meshletInstrument = MeshletInstrument::offset;
		CHECK(find(lintRt(r), "meshlet.instrument") == nullptr);
	}
	{  // 反向②：默认 none 任何组合都不喊（出厂态沉默；「同真让位」形态写不出来，无需再测）
		CHECK(find(lintRt(RuntimeConfig{}), "meshlet.instrument") == nullptr);
		CHECK(gateFindings(lintRt(redlineA()), "meshlet.instrument").empty());
	}
}

TEST_CASE("gate_lint meshlet.coplanar：偏离 off 而无叠加 draw（mode ≠ \"overlay\"）= warn；off 与合法组合不喊") {
	{  // 正例①：assets 开 + mode = off（默认）+ coplanar = equal（setter 设了但没有 draw 可作用）
		RuntimeConfig r;
		r.meshletCoplanar = MeshletCoplanarMode::equal;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.coplanar") != nullptr);
		CHECK(find(fs, "meshlet.coplanar")->severity == GateSeverity::warn);
	}
	{  // 正例②：assets 关 + coplanar = ne（红线 A 打底只变 coplanar）
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) {
			r.meshletCoplanar = MeshletCoplanarMode::ne;
		}));
		CHECK(find(fs, "meshlet.coplanar") != nullptr);
	}
	{  // 反向：assets + mode = "overlay" 都开 + coplanar = equal = 合法四臂档，不喊
		RuntimeConfig r;
		r.meshletMode = MeshletMode::overlay;
		r.meshletCoplanar = MeshletCoplanarMode::equal;
		CHECK(find(lintRt(r), "meshlet.coplanar") == nullptr);
	}
	{  // 反向：mode 保持 off（出厂默认）而 coplanar 也 off = 未偏离，不喊
		CHECK(find(lintRt(RuntimeConfig{}), "meshlet.coplanar") == nullptr);
	}
}

TEST_CASE("gate_lint meshlet.cull：需 assets && mode=\"switch\"；同款 cull_replay 限制（两条）") {
	static const std::vector<RgPassPlan> kDef = passesOf(deferredRenderGraphDesc());
	{  // 正例①：cull 开而取代档关（assets 开、mode = off）= 标志数组无消费者
		RuntimeConfig r;
		r.meshletCull = true;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.cull") != nullptr);
		CHECK(find(fs, "meshlet.cull")->severity == GateSeverity::warn);
		CHECK(find(fs, "meshlet.cull")->message.find("wireMeshletCull") != std::string::npos);
	}
	{  // 正例②：cull + mode = "switch" 同开而 **assets 关** = 仍无消费者。
	   // 🔴 这条抓「把 assets && mode=\"switch\" 弱化成只查取代档」的变异（a && b → b）：
	   // 弱化后本组合（取代档开）谓词变假 ⟹ 本断言红。
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) {
			r.meshletMode = MeshletMode::inplaceSwitch;
			r.meshletCull = true;
		}));
		CHECK(find(fs, "meshlet.cull") != nullptr);
	}
	{  // 正例③：cull + 取代档 + assets 但 cull_replay = gpuLagged = 同款限制（恰一条，
	   // 宿主缺失那条不叠加——它的前置里取代档已开）
		DiagnosticsConfig d;
		d.cullReplay = CullReplayMode::gpuLagged;
		RuntimeConfig r;
		r.meshletMode = MeshletMode::inplaceSwitch;
		r.meshletCull = true;
		const auto fs = lint(d, kDef, kHappyEnv, ShadowConfig{}, r);
		REQUIRE(gateFindings(fs, "meshlet.cull").size() == 1u);
		CHECK(gateFindings(fs, "meshlet.cull")[0]->message.find("cull_replay")
			!= std::string::npos);
	}
	{  // 反向：cull + 取代档 + assets（cpu 档）= 合法 β+cull 形态，cull 不喊
		RuntimeConfig r;
		r.meshletMode = MeshletMode::inplaceSwitch;
		r.meshletCull = true;
		CHECK(find(lintRt(r), "meshlet.cull") == nullptr);
	}
	{  // 反向：cull 关（出厂默认）任何组合都不喊
		CHECK(find(lintRt(redlineA()), "meshlet.cull") == nullptr);
	}
}

TEST_CASE("gate_lint meshlet.shadow_lod_fallback：链式前置（lod ≥ 0 且 assets）；死值 = inertNote") {
	{  // 正例①：fallback = l0 而 shadow_lod = -1（assets 开）= 选级整段不跑，调参残留
		RuntimeConfig r;
		r.meshletShadowLod = -1;
		r.meshletShadowLodFallback = MeshletLodFallbackMode::l0;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.shadow_lod_fallback") != nullptr);
		CHECK(find(fs, "meshlet.shadow_lod_fallback")->severity == GateSeverity::inertNote);
	}
	{  // 正例②：fallback = l0 而 assets 关（shadow_lod 保持 5）= 链的第二条件
		RuntimeConfig r;
		r.meshletAssets = false;
		r.meshletShadowLodFallback = MeshletLodFallbackMode::l0;
		CHECK(find(lintRt(r), "meshlet.shadow_lod_fallback") != nullptr);
	}
	{  // 反向：fallback = l0 + assets + shadow_lod = 5 = 有消费者（逐模型选级真读它），不喊
		RuntimeConfig r;
		r.meshletShadowLodFallback = MeshletLodFallbackMode::l0;
		CHECK(find(lintRt(r), "meshlet.shadow_lod_fallback") == nullptr);
	}
	{  // 反向：fallback 保持 deepest（出厂默认）任何组合都不喊——未偏离不参与
		CHECK(find(lintRt(redlineA()), "meshlet.shadow_lod_fallback") == nullptr);
	}
}

TEST_CASE("gate_lint meshlet.mdi_shadow：宿主 = assets + shadow_lod ≥ 0（★M-1b 已接上，非零消费者）") {
	// 🔴 判据核实记录：★M-0 时代的「本批零消费者」已过期——★M-1b 落地后消费者 =
	// wireMeshletMdiShadowActivation（烘条目 + 回填 + emplace）→ renderListSystem 读 ctx。
	// 宿主条件（纯配置子集）= wireMeshletShadowArgs 第①步的 assets + shadow_lod ≥ 0；
	// 第三条（选级表非零）是装配期事实 ⟹ 装配层 warn 保留、lint 消息点名它（不判 error）。
	{  // 正例①：mdi 开 + assets 关（红线 A 打底）= 载体必然不建（meshlet 块出声，键级消息
	   // 覆盖块级缺省——逐键归因自带 wireMeshletMdiShadowActivation 全名，断言不降格）
		const auto fs = lintRt(redlineA([](RuntimeConfig& r) { r.meshletMdiShadow = true; }));
		REQUIRE(find(fs, "meshlet.mdi_shadow") != nullptr);
		CHECK(find(fs, "meshlet.mdi_shadow")->severity == GateSeverity::warn);
		CHECK(find(fs, "meshlet.mdi_shadow")->message.find("wireMeshletMdiShadowActivation")
			!= std::string::npos);
	}
	{  // 正例②：mdi 开 + assets 开 + shadow_lod = -1 = 宿主条件第二脚空了（独立残留行出声，
	   // 消息保留逐键归因：点名装配层 wireMeshletMdiShadowActivation 的事实级 warn）
		RuntimeConfig r;
		r.meshletShadowLod = -1;
		r.meshletMdiShadow = true;
		const auto fs = lintRt(r);
		REQUIRE(find(fs, "meshlet.mdi_shadow") != nullptr);
		CHECK(find(fs, "meshlet.mdi_shadow")->message.find("wireMeshletMdiShadowActivation")
			!= std::string::npos);
	}
	{  // 反向：mdi 开 + 出厂两键（assets + shadow_lod = 5）= 宿主条件满足，不喊
	   // （载体建没建 = 选级表事实，归装配层 wireMeshletMdiShadowActivation 出声）
		RuntimeConfig r;
		r.meshletMdiShadow = true;
		CHECK(find(lintRt(r), "meshlet.mdi_shadow") == nullptr);
	}
	{  // 反向：mdi 关（出厂默认）任何组合都不喊——门关 grep 护栏的对等物
		CHECK(find(lintRt(redlineA()), "meshlet.mdi_shadow") == nullptr);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 金标准 harness（债-129 第二刀 D 收尾修正随刀重建，**常驻树**）
//
// 用途：gate_lint 的任何重构（塌表 / 换装配 / 改措辞 / 换发射顺序）都要能在**不重跑 GPU、
// 不截图**的前提下判定「触发面与归因面有没有变」。上面的行为用例逐条钉逐门语义；本 harness
// 钉的是 88 档全组合扫描下的**输出面**，两张基线随仓库走（/tmp 下的历史副本不是基线）：
//   · `tests/golden/gate_lint_findings.txt`     = 每档 `(门名, 级别枚举值)` 行 —— 触发面
//   · `tests/golden/gate_lint_findings_msg.txt` = 每档 `(门名, 消息)` 行 —— 归因面
// 行序 = 档内字典序（**集合语义**：块装配的 finding 发射顺序与独立行时代不同是允许的，
// 内容不许变；级别枚举值 = GateSeverity 的 0/1/2 = inertNote/warn/error）。
//
// 怎么跑（🔴 **默认（无 env）= 校验模式**：现算一份与 committed 的比对，不一致即 FAIL 并点名
// 漂的文件与行号；`RGL_GATE_LINT_GOLDEN=<dir>` = 重生成模式，写盘后自己 `git diff` 看）：
//   RGL_GATE_LINT_GOLDEN=$PWD/tests/golden just test && git diff --exit-code tests/golden/
// 极性为什么反过来（2026-10-17 实测咬人，不是假想）：旧形态「env 未设 = no-op」+ `tools/checks/`
// 里没有任何东西比对 committed golden ⟹ 那份「已提交的基线」其实**没人验**；d5cb02b 把门名
// `izb` 改成 `izb.on` 没重生成，committed golden 就这么静默陈旧了一整刀。判据同「设备级用例
// 无设备必须 FAIL 不 SKIP」那条：**静默跳过 = 假绿**。代价实测整个 harness 0.79s，进 `just test` 可接受。
// 重构后 diff 非空 = 输出面变了。评审确认是「等价改写」才准把基线更新与重构放进**同一刀**
// （diff 本身就是评审材料）；diff 点名「消息被并成块消息 / 某档少一条」= 本 harness 要拦的
// 正是这个形态——上一刀靠它抓出逐键归因丢失与断言降格。
//
// 变异红线：① kGateRules / kGateBlocks / kMeshletKeys 任一条目的触发或消息变 → 校验模式直接红
// （committed 没跟着改）；重生成基线的 git diff 点名改了什么；② 扫描表删档 → 用例数钉 90 先红
// （回归网不许静默缩小）；③ 把本用例改成无条件写文件 → `just test` 长出副作用（禁止）；
// ④ 档位定义改动而输出没变 = 允许（档位是覆盖面不是判据本体），但覆盖面缩小时 ② 拦不住逐档
// 语义——新增键时**先补档再挂号**；⑤ 🔴 把校验模式改回「env 未设就 return」= 把 d5cb02b 那记
// 假绿原样装回来（禁止；定位宏缺失同样必须红，不许回落成跳过）。
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/** harness 单档：三份配置 + 图/env（缺省 = 出厂态 × deferred 图 × 全合法世界）。 */
struct GoldenCase {
	std::string_view name;
	DiagnosticsConfig cfg{};
	ShadowConfig shadow{};
	RuntimeConfig runtime{};
	bool noLight = false;  ///< true = 非 deferred 图（缺省 = deferred 图）
	GateEnv world = kHappyEnv;
};

/** 世界三档（kHappyEnv 之外：全黑 / 仅 headless / 缺 static 能力 / 缺 llm 消费方）。 */
constexpr GateEnv kGoldenDark{.headless = true, .llmCapture = false, .staticGraphicsIndirect = false};
constexpr GateEnv kGoldenHeadless{.headless = true, .llmCapture = true, .staticGraphicsIndirect = true};
constexpr GateEnv kGoldenNoStatic{.llmCapture = true, .staticGraphicsIndirect = false};
constexpr GateEnv kGoldenNoLlm{.staticGraphicsIndirect = true};

/** pcss / evssm 调参块的「全偏离」形态（四字段都 ≠ 出厂值；8.0 = 偏离默认 16.0）。 */
constexpr ShadowTuning kTuningAll{2.0, 40.0, 48.0, 8.0};

/** 扫描表 88 档（90 − 2：variant 交叉校验两档随 RgGraphVariant 枚举删除，2026-10-18 刀②；档序 = 基线文件档序；每档的期望输出以基线文件为准，行为语义见上方用例）。 */
const std::vector<GoldenCase>& goldenCases() {
	static const std::vector<GoldenCase> kCases{
		// ———— factory：出厂配置 × 图/env 四世界 ————
		{.name = "factory/def-happy"},
		{.name = "factory/no-light-dark", .noLight = true, .world = kGoldenDark},
		{.name = "factory/no-light-happy", .noLight = true},
		{.name = "factory/def-headless", .world = kGoldenDark},
		// ———— pcss 块：宿主 = mode=="pcss"（含结构性错位键 min_footprint_texels）————
		{.name = "pcss/owner-on", .shadow = {.mode = ShadowMode::pcss, .pcss = kTuningAll}},
		{.name = "pcss/owner-on-mft-default",
			.shadow = {.mode = ShadowMode::pcss, .pcss{2.0, 40.0, 48.0}}},
		{.name = "pcss/off-csm-all", .shadow = {.pcss = kTuningAll}},
		{.name = "pcss/off-csm-sun-only", .shadow = {.pcss{2.0}}},
		{.name = "pcss/off-csm-mft-only", .shadow = {.pcss{.minFootprintTexels = 8.0}}},
		{.name = "pcss/off-none-all", .shadow = {.mode = ShadowMode::none, .pcss = kTuningAll}},
		{.name = "pcss/off-evssm-all", .shadow = {.mode = ShadowMode::evssm, .pcss = kTuningAll}},
		// ———— evssm 块：与 pcss 侧对称（宿主 = mode=="evssm"）————
		{.name = "evssm/owner-on", .shadow = {.mode = ShadowMode::evssm, .evssm = kTuningAll}},
		{.name = "evssm/off-csm-all", .shadow = {.evssm = kTuningAll}},
		{.name = "evssm/off-csm-mft-only", .shadow = {.evssm{.minFootprintTexels = 8.0}}},
		{.name = "evssm/off-pcss-all", .shadow = {.mode = ShadowMode::pcss, .evssm = kTuningAll}},
		{.name = "evssm/both-blocks-csm", .shadow = {.pcss = kTuningAll, .evssm = kTuningAll}},
		{.name = "evssm/evssm-mode-pcss-block",
			.shadow = {.mode = ShadowMode::evssm, .pcss = kTuningAll}},
		// ———— izb：error（非 deferred 图 / mode=none）× warn（交付已关）× 子键 ————
		{.name = "izb/on-def", .shadow = {.izb = true}},
		{.name = "izb/on-no-light", .shadow = {.izb = true}, .noLight = true},
		{.name = "izb/on-none", .shadow = {.mode = ShadowMode::none, .izb = true}},
		{.name = "izb/on-none-no-light", .shadow = {.mode = ShadowMode::none, .izb = true}, .noLight = true},
		{.name = "izb/dump-off", .shadow = {.izbDumpFrame = 120}},
		{.name = "izb/dump-on", .shadow = {.izb = true, .izbDumpFrame = 120}},
		{.name = "izb/grid-off", .shadow = {.izbGrid = 1024}},
		{.name = "izb/grid-on", .shadow = {.izb = true, .izbGrid = 1024}},
		{.name = "izb/evssm-no-light", .shadow = {.mode = ShadowMode::evssm}, .noLight = true},
		// ———— picking_probe：宿主 picking（error）× 块宿主 frame（inertNote）————
		{.name = "pp/frame-off-nopicking", .cfg = {.pickingProbeFrame = 30}},
		{.name = "pp/frame-on-picking-on", .cfg = {.picking = true, .pickingProbeFrame = 30}},
		{.name = "pp/xy-frame-off", .cfg = {.pickingProbeX = 640, .pickingProbeY = 360}},
		{.name = "pp/x-only-frame-off", .cfg = {.pickingProbeX = 640}},
		{.name = "pp/xy-frame-on-picking-on",
			.cfg = {.picking = true, .pickingProbeFrame = 30, .pickingProbeX = 640,
				.pickingProbeY = 360}},
		{.name = "pp/xy-frame-on-nopicking",
			.cfg = {.pickingProbeFrame = 30, .pickingProbeX = 640, .pickingProbeY = 360}},
		{.name = "pp/xy-frame-off-nopicking", .cfg = {.pickingProbeX = 640, .pickingProbeY = 360}},
		// ———— ui_click_probe / camera_probe：headless 死路（error）× frame 块（inertNote）————
		{.name = "uic/xy-frame-off", .cfg = {.uiClickProbeX = 100, .uiClickProbeY = 200}},
		{.name = "uic/xy-frame-on-window",
			.cfg = {.uiClickProbeFrame = 30, .uiClickProbeX = 100, .uiClickProbeY = 200}},
		{.name = "uic/frame-headless", .cfg = {.uiClickProbeFrame = 30}, .world = kGoldenHeadless},
		{.name = "cam/keyhold-frame-off", .cfg = {.cameraProbeKey = 'W', .cameraProbeHold = 20}},
		{.name = "cam/key-only-frame-off", .cfg = {.cameraProbeKey = 'W'}},
		{.name = "cam/hold-only-frame-off", .cfg = {.cameraProbeHold = 20}},
		{.name = "cam/keyhold-frame-on-window",
			.cfg = {.cameraProbeFrame = 30, .cameraProbeKey = 'W', .cameraProbeHold = 20}},
		{.name = "cam/frame-headless", .cfg = {.cameraProbeFrame = 30}, .world = kGoldenHeadless},
		// ———— rpc / dgc / imgui / dump_gbuf ————
		{.name = "rpc/port-off", .cfg = {.rpcPort = 9000}},
		{.name = "rpc/port-on", .cfg = {.rpc = true, .rpcPort = 9000}},
		{.name = "dgc/nocap", .cfg = {.dgcStatic = true}, .world = kGoldenNoStatic},
		{.name = "dgc/replay-cpu", .cfg = {.dgcStatic = true}},
		{.name = "dgc/replay-lagged", .cfg = {.dgcStatic = true, .cullReplay = CullReplayMode::gpuLagged}},
		{.name = "dgc/replay-sync", .cfg = {.dgcStatic = true, .cullReplay = CullReplayMode::gpuSync}},
		{.name = "imgui/headless", .cfg = {.imgui = true}, .world = kGoldenHeadless},
		{.name = "imgui/window", .cfg = {.imgui = true}},
		{.name = "gbuf/nollm", .cfg = {.dumpGbuf = 2}, .world = kGoldenNoLlm},
		{.name = "gbuf/llm-no-light", .cfg = {.dumpGbuf = 2}, .noLight = true},
		{.name = "gbuf/ok", .cfg = {.dumpGbuf = 2}},
		// ———— rt：runtime.lua 的 meshlet 块（红线 A 打底 = redlineA；其余指定初始化）————
		{.name = "rt/factory"},
		{.name = "rt/off-redline", .runtime = redlineA()},
		{.name = "rt/off-lod5", .runtime = {.meshletAssets = false}},
		// 🔴 债-129 S2（第二件）：rt/overlay-* / rt/switch-* 五档随两布尔合并改写成 rt/mode-*；
		// 原 rt/switch-overlay-cpu / rt/switch-overlay-sync 两档断的「同真互斥」输入**不可构造**
		// （一枚三值键），改立两档**跨档前置**对照（mode-switch-coplanar / mode-overlay-cull-sync：
		// coplanar 宿主 = overlay 档、cull 宿主 = switch 档，两档各自成立 = 按档分立的判据面）。
		{.name = "rt/mode-overlay", .runtime = {.meshletMode = MeshletMode::overlay}},
		{.name = "rt/mode-overlay-offenable",
			.runtime = redlineA([](RuntimeConfig& r) {
				r.meshletMode = MeshletMode::overlay;
			})},  // 🔴 期望 = 空：overlay 档生效前置**不含** assets（合法组合不判死值；原
			     // rt/overlay-offenable 的 meshlet.overlay 块条目随合并删除——假申报）
		{.name = "rt/instrument-offset", .runtime = {.meshletInstrument = MeshletInstrument::offset}},
		{.name = "rt/instrument-offset-offenable",
			.runtime = redlineA([](RuntimeConfig& r) {
				r.meshletMode = MeshletMode::overlay;
				r.meshletInstrument = MeshletInstrument::offset;
			})},
		{.name = "rt/instrument-tint", .runtime = {.meshletInstrument = MeshletInstrument::tint}},
		{.name = "rt/instrument-tint-offenable",
			.runtime = redlineA([](RuntimeConfig& r) {
				r.meshletMode = MeshletMode::overlay;
				r.meshletInstrument = MeshletInstrument::tint;
			})},
		// 🔴 债-129 S2：原 rt/tint-debug / rt/tint-debug-overlay / rt/tint-debug-offenable 三档
		// （断「两布尔同真 ⟹ tint 优先让位」）随合并**整删**——输入不可构造；让位规则已不在
		// 申报表里。补两档 instrument 的「overlay 档开 = 合法沉默」反向（扫描面保持 90 档，
		// 删档 = 回归网静默缩小的形态用补档堵住，档位哨兵 CHECK(size==90) 不降格）。
		{.name = "rt/instrument-offset-overlay-ok",
			.runtime = {.meshletMode = MeshletMode::overlay, .meshletInstrument = MeshletInstrument::offset}},
		{.name = "rt/instrument-tint-overlay-ok",
			.runtime = {.meshletMode = MeshletMode::overlay, .meshletInstrument = MeshletInstrument::tint}},
		{.name = "rt/coplanar-eq", .runtime = {.meshletCoplanar = MeshletCoplanarMode::equal}},
		{.name = "rt/coplanar-ne-offenable",
			.runtime = redlineA([](RuntimeConfig& r) {
				r.meshletCoplanar = MeshletCoplanarMode::ne;
			})},
		{.name = "rt/coplanar-ok",
			.runtime = {.meshletMode = MeshletMode::overlay, .meshletCoplanar = MeshletCoplanarMode::equal}},
		{.name = "rt/mode-switch-offenable",
			.runtime = redlineA([](RuntimeConfig& r) { r.meshletMode = MeshletMode::inplaceSwitch; })},
		{.name = "rt/mode-switch-sync",
			.cfg = {.cullReplay = CullReplayMode::gpuSync},
			.runtime = {.meshletMode = MeshletMode::inplaceSwitch}},
		{.name = "rt/mode-switch-coplanar",
			.runtime = {.meshletMode = MeshletMode::inplaceSwitch,
				.meshletCoplanar = MeshletCoplanarMode::equal}},  // 跨档：取代档下双臂无消费者
		{.name = "rt/mode-overlay-cull-sync",
			.cfg = {.cullReplay = CullReplayMode::gpuSync},
			.runtime = {.meshletMode = MeshletMode::overlay,
				.meshletCull = true}},  // 跨档：cull 的宿主 = 取代档（overlay 档 + gpuSync 下
		                                // 只喊「取代档关」一条，cull_replay 那条不成立）
		{.name = "rt/mode-switch-ok", .runtime = {.meshletMode = MeshletMode::inplaceSwitch}},
		{.name = "rt/cull-only", .runtime = {.meshletCull = true}},
		{.name = "rt/cull-offenable",
			.runtime = redlineA([](RuntimeConfig& r) { r.meshletCull = true; })},
		{.name = "rt/cull-switch-offenable",
			.runtime = redlineA([](RuntimeConfig& r) {
				r.meshletMode = MeshletMode::inplaceSwitch;
				r.meshletCull = true;
			})},
		{.name = "rt/cull-switch-sync",
			.cfg = {.cullReplay = CullReplayMode::gpuSync},
			.runtime = {.meshletMode = MeshletMode::inplaceSwitch, .meshletCull = true}},
		{.name = "rt/cull-switch-lagged",
			.cfg = {.cullReplay = CullReplayMode::gpuLagged},
			.runtime = {.meshletMode = MeshletMode::inplaceSwitch, .meshletCull = true}},
		{.name = "rt/cull-ok", .runtime = {.meshletMode = MeshletMode::inplaceSwitch, .meshletCull = true}},
		{.name = "rt/lod-neg-enable", .runtime = {.meshletShadowLod = -1}},
		{.name = "rt/fallback-l0-lodneg",
			.runtime = {.meshletShadowLod = -1, .meshletShadowLodFallback = MeshletLodFallbackMode::l0}},
		{.name = "rt/fallback-l0-offenable",
			.runtime = {.meshletAssets = false, .meshletShadowLodFallback = MeshletLodFallbackMode::l0}},
		{.name = "rt/fallback-l0-ok",
			.runtime = {.meshletShadowLodFallback = MeshletLodFallbackMode::l0}},
		{.name = "rt/mdi-offenable",
			.runtime = redlineA([](RuntimeConfig& r) { r.meshletMdiShadow = true; })},
		{.name = "rt/mdi-lodneg",
			.runtime = {.meshletShadowLod = -1, .meshletMdiShadow = true}},
		{.name = "rt/mdi-both",
			.runtime = redlineA([](RuntimeConfig& r) { r.meshletMdiShadow = true; })},
		{.name = "rt/mdi-ok", .runtime = {.meshletMdiShadow = true}},
		{.name = "rt/kitchen-sink-off",
			.runtime = redlineA([](RuntimeConfig& r) {
				r.meshletMode = MeshletMode::inplaceSwitch;
				r.meshletInstrument = MeshletInstrument::tint;
				r.meshletCoplanar = MeshletCoplanarMode::ne;
				r.meshletCull = true;
				r.meshletShadowLodFallback = MeshletLodFallbackMode::l0;
				r.meshletMdiShadow = true;
			})},  // 债-129 S2：原「overlay + switch 同真」全开形态写不出来，取取代档全开
		{.name = "rt/kitchen-sink-on",
			.cfg = {.cullReplay = CullReplayMode::gpuSync},
			.runtime = {.meshletMode = MeshletMode::inplaceSwitch,
				.meshletInstrument = MeshletInstrument::tint,
				.meshletCoplanar = MeshletCoplanarMode::ne,
				.meshletCull = true,
				.meshletShadowLodFallback = MeshletLodFallbackMode::l0,
				.meshletMdiShadow = true}},
		// ———— combo：五块宿主同时不成立的「全灭」世界（块装配的聚合形态就藏在这类档里）————
		{.name = "combo/all-blocks-off",
			.cfg = {.pickingProbeX = 640, .uiClickProbeY = 200, .cameraProbeKey = 'W'},
			.shadow = {.pcss = kTuningAll, .evssm = kTuningAll},
			.runtime = {.meshletAssets = false, .meshletMode = MeshletMode::overlay,
				.meshletInstrument = MeshletInstrument::offset,
				.meshletMdiShadow = true}},  // 🔴 mode = "overlay" 在 assets 关的「全灭」世界里
		                                     // **不**出 meshlet.mode finding（按档分立：overlay
		                                     // 档不吃 assets 门；原 meshlet.overlay 块条目的正主）
	};
	return kCases;
}

/** 88 档全组合扫描 → 两份基线文本（触发面 keep / 归因面 msg）。校验与重生成**共用这一份**
 *  装配代码：两条路各写一遍 = 两个真相源，改口径时必漂（本 harness 要拦的正是这个形态）。 */
void buildGoldenBlobs(std::string& keep, std::string& msg) {
	static const std::vector<RgPassPlan> kNoLightGraph = passesOfNoLightGraph();
	static const std::vector<RgPassPlan> kDef = passesOf(deferredRenderGraphDesc());
	for (const auto& c : goldenCases()) {
		const std::span<const RgPassPlan> passes = c.noLight ? std::span{kNoLightGraph} : std::span{kDef};
		const auto fs = lintDiagnosticsGates(c.cfg, DiagnosticsConfig{}, c.shadow, ShadowConfig{},
			c.runtime, RuntimeConfig{}, passes, c.world);
		std::vector<std::string> keepLines, msgLines;
		for (const auto& f : fs) {
			keepLines.push_back(std::string{f.gate} + '\t' + std::to_string(static_cast<int>(f.severity)));
			msgLines.push_back(std::string{f.gate} + '\t' + f.message);
		}
		std::ranges::sort(keepLines);  // 集合语义：发射顺序不进基线（段头注释）
		std::ranges::sort(msgLines);
		keep += "### " + std::string{c.name} + "\n";
		msg += "### " + std::string{c.name} + "\n";
		for (const auto& l : keepLines)
			keep += l + '\n';
		for (const auto& l : msgLines)
			msg += l + '\n';
	}
}

/** 文本按行切开（末尾换行不产生空尾行 ⟹ 行号与 committed 文件的行号逐一对齐）。 */
std::vector<std::string> splitLines(const std::string& text) {
	std::vector<std::string> out;
	std::size_t pos = 0;
	while (pos < text.size()) {
		const std::size_t nl = text.find('\n', pos);
		if (nl == std::string::npos) {
			out.push_back(text.substr(pos));
			break;
		}
		out.push_back(text.substr(pos, nl - pos));
		pos = nl + 1;
	}
	return out;
}

/// 差异报告最多点名的行数（整段位移时首处即根因位置，其余只报总数，防刷屏）。
constexpr int kMaxDiffLines = 5;
/// 校验模式实际比对过的基线份数（阳性锚点：用例断言读到 2 = 两份都真验了；
/// 「env 未设就 return」的旧形态读到 0 —— 这条断言本身就是「没跳过」的机器判据）。
int goldenVerifiedFiles = 0;

/** committed golden 逐行读入（`ok = false` = 打不开，与「空文件」区分开——读不进来必须出声）。 */
std::vector<std::string> readGoldenLines(const std::string& file, bool& ok) {
	std::ifstream in{file};
	ok = in.is_open();
	std::vector<std::string> out;
	for (std::string line; ok && std::getline(in, line);)
		out.push_back(line);
	return out;
}

/** 一份基线 vs 现算文本 → 差异报告（空串 = 一致）。逐行归因点名「哪个文件哪一行漂了」+
 *  两侧实际内容，报错即地图（只说「golden 陈旧」= 让人自己 diff 找）。 */
std::string diffGoldenFile(const std::string& file, const std::string& current) {
	bool ok = false;
	const std::vector<std::string> golden = readGoldenLines(file, ok);
	if (!ok)
		return file + " 打不开（golden 缺失 / RGL_GATE_LINT_GOLDEN_DIR 指向不存在的目录）\n";
	++goldenVerifiedFiles;  // 走到这里 = 这份真的比过了
	const std::vector<std::string> now = splitLines(current);
	const std::size_t shared = std::min(golden.size(), now.size());
	std::string report;
	int shown = 0;
	for (std::size_t i = 0; i < shared; ++i) {
		if (golden[i] == now[i])
			continue;
		if (++shown <= kMaxDiffLines)  // 整段位移时前 kMaxDiffLines 行足够定位，别刷屏
			report += file + ":" + std::to_string(i + 1) + " 漂了 ⟵ golden=`" + golden[i]
				+ "` 现算=`" + now[i] + "`\n";
	}
	if (golden.size() != now.size())
		report += file + ": 行数不同（golden " + std::to_string(golden.size()) + " 行 / 现算 "
			+ std::to_string(now.size()) + " 行），首处差异在第 "
			+ std::to_string(shared + 1) + " 行\n";
	if (shown > kMaxDiffLines)
		report += file + ": 差异行共 " + std::to_string(shown) + " 条（只列前 "
			+ std::to_string(kMaxDiffLines) + " 条）\n";
	return report;
}

}  // namespace

TEST_CASE("gate_lint 金标准 harness：88 档全组合扫描（默认 = 校验 committed golden；RGL_GATE_LINT_GOLDEN=<dir> = 重生成）") {
	CHECK(goldenCases().size() == 88u);  // 扫描面哨兵：删档 = 回归网静默缩小（变异红线 ②）
	std::string keep, msg;
	buildGoldenBlobs(keep, msg);
	const char* const outDir = std::getenv("RGL_GATE_LINT_GOLDEN");
	if (outDir != nullptr && *outDir != '\0') {  // 重生成模式（唯一会写文件的一条路）
		const std::string dir{outDir};
		std::ofstream keepFile{dir + "/gate_lint_findings.txt"};
		std::ofstream msgFile{dir + "/gate_lint_findings_msg.txt"};
		REQUIRE(keepFile.is_open());  // env 指了个写不进/不存在的目录 = 红，不静默吞
		REQUIRE(msgFile.is_open());
		keepFile << keep;
		msgFile << msg;
		return;
	}
	// 🔴 默认路 = **校验模式，不需要任何 env**（无 env 就 return = 假绿，变异红线 ⑤）。
#ifndef RGL_GATE_LINT_GOLDEN_DIR
	FAIL("RGL_GATE_LINT_GOLDEN_DIR 编译期宏没传（应由 tests/CMakeLists.txt 指向源树 tests/golden/）"
		 "—— 定位不到基线目录必须红，不许静默跳过（先例：d5cb02b 改门名没重生成，committed golden 陈旧一整刀）");
#else
	std::string report = diffGoldenFile(RGL_GATE_LINT_GOLDEN_DIR "/gate_lint_findings.txt", keep)
		+ diffGoldenFile(RGL_GATE_LINT_GOLDEN_DIR "/gate_lint_findings_msg.txt", msg);
	if (!report.empty()) {
		report += "重生成基线：RGL_GATE_LINT_GOLDEN=" RGL_GATE_LINT_GOLDEN_DIR " just test && git diff --exit-code tests/golden/\n";
		FAIL(report);  // 红 = committed golden 与代码不一致，且消息里点名到文件:行号
	}
	// 阳性锚点：这条断言**只在默认路（无 env）执行** ⟹ 用例输出里 assertions ≥ 1 = 校验真跑了；
	// 「env 未设就 return」的旧形态是 0 assertions（变异红线 ⑤ 的机器判据）。
	CHECK_MESSAGE(goldenVerifiedFiles == 2, "校验模式必须逐份比对两份基线（份数 ≠ 2 = 校验没跑全）");
#endif
}

}  // namespace RGL
