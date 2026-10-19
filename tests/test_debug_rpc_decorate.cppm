module;
/**
 * test_debug_rpc_decorate —— `/decorate` 动态能力探针的纯逻辑（rgl/debug_rpc_decorate.cppm，
 * `docs/能力观测_plan.md` 批 4）
 *
 * ## 被测契约（三句话）
 *
 *  1. `refusedAt` 词表每一档都有真实生产者，且**只有**有生产者的档在词表里
 *     （词表覆盖用例：全部可构造场景产出的档集合 == kRefusedVocabulary，双向）。
 *  2. 分类决策顺序是读数口径的一部分：del 先查在册（不在册不发事务）、add 先查在场
 *     （在场 = 幂等成功，门都不问）、再查门、再查在册。
 *  3. 快照差异只吃编译产物名单，出现 / 消失两半边各自独立；回包的条件性（refusedAt
 *     只在 done=false 出现）与键集是 schema 登记的形状。
 *
 * ## 期望值出处（独立于被测实现）
 *  - 词表覆盖用例的期望 = 场景表本身（每个场景自带「它该产哪一档」的注记），
 *    不是从 classifyDecorate 的输出反抄；
 *  - 决策顺序用例的输入刻意做成「顺序错了结果就不同」的形态（在场 + 门关 同时成立），
 *    顺序断言才有内容；
 *  - diff 用例的名单出处 = 测试自己手构的两份 CompiledRenderGraph（构造即期望）。
 *
 * ## 变异判据（哪个变异让我红；答不出 = 摆设，不配进门）
 *  1. 🔴 某个拒绝档改报别的档（例：not-adopted 改成回 gate-off，任务变异 ② 的单元形态）
 *     → 用例 1/2 的 refusedAt 逐条断言红 + 用例 3 词表覆盖红（产出集合变了）。
 *  2. 🔴 探针恒回 done=true（任务变异 ③：假装什么都能做）→ 用例 1/2 的 done==false 半边红
 *     + 用例 3 词表覆盖红（没有任何档被产出）。
 *  3. 🔴 词表加一档没有生产者（任务变异 ④）→ 用例 3 词表覆盖红（该档没有任何场景产出它；
 *     反向「场景产了不在词表的档」同样红）。实机侧的对应判据 = rpc_endpoints_live 的
 *     「refusedAt ∈ schema note 词表」+ rules_rpc 面 7 的三方对账。
 *  4. del 决策顺序翻转（先查在场）→ 用例 1 的「不在册 + 在场」场景会从 not-adopted 变成
 *     发事务（action==attempt 断言红）；add 决策顺序翻转（先查门）→ 用例 2 的
 *     「在场 + 门关」幂等半边红（在场优先于门，读数才不依赖配置档）。
 *  5. diff 的 appeared/disappeared 互换 → 用例 5 两半边全红（对称判据两半各钉，
 *     只钉一半会被读成整条已验）；diff 只算 pass 不算附件 → 用例 5 的附件两条红。
 *  6. 事务定案丢弃引擎原文（reason 不拼 transactionError）→ 用例 4 的「原文逐字在场」红。
 *  7. 回包 done=true 也出 refusedAt → 用例 6 的「成功半边无该键」红；done=false 不出 →
 *     另一半红。幂等格（无事务）diff 出非空 → 用例 6 的空 diff 断言红。
 *  8. 特征表 feature 名不再吃 rpc_value 生成常量（改字面量）→ 用例 7 的「表名 == 生成常量」
 *     逐条红（schema 改名 = 常量变，表没跟上即红；本用例同时钉「表非空」防空表假绿）。
 *  ⚠ 阳性对照（del highlight 必须成）的判据**不在本文件**——它要真图真事务，
 *    实机判据在 tools/rpc_endpoints_live.py（第二实例）与 tools/decorate_matrix.py（量具自检）。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 🔴 modules 坑：initializer_list 不从 import rlstd 带进来
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

export module test_debug_rpc_decorate;

import rlstd;
import rlvk;
import render_graph;
import debug_rpc_decorate;
import rpc_schema_generated;  // rpc_value::decorate::*（特征表名单的单源）

namespace RGL {
namespace {

/** 造一个只关心名单的编译图投影（附件/pass 各按给定名字）。 */
CompiledRenderGraph graphWith(std::vector<std::string> attachmentNames,
	std::vector<std::string> passNames) {
	CompiledRenderGraph g;
	for (const auto& n : attachmentNames)
		g.attachments.push_back(RgCompiledAttachment { .name = n });
	for (const auto& n : passNames)
		g.passes.push_back(RgPassPlan { .name = n });
	return g;
}

/** pass 名用**字面量**（DecoratePassFacts::passName 是 string_view，指向临时 std::string
 *  会当场悬空——第一版 factsFor 就踩了这个，多 pass 逆序断言读到的全是垃圾内存）。 */
DecorateFacts factsFor(std::vector<std::pair<bool, bool>> presentRegistered, bool gateOpen) {
	static constexpr std::array<std::string_view, 4> kNames { "p0", "p1", "p2", "p3" };
	DecorateFacts f;
	f.gateOpen = gateOpen;
	for (size_t i = 0; i < presentRegistered.size(); ++i)
		f.passes.push_back(DecoratePassFacts {
			.passName = kNames[i],
			.present = presentRegistered[i].first,
			.registered = presentRegistered[i].second });
	return f;
}

/** 词表覆盖用例的场景表：一条 = 一个可构造输入 + 它该产的档（期望出处 = 注记本身）。 */
struct Scenario {
	DecorateFacts facts;
	std::string_view op;
	std::string_view expectRefusedAt;  // 空 = 该成功（done=true）
};

const std::vector<Scenario>& scenarios() {
	static const std::vector<Scenario> s = {
		// del + 不在册 → not-adopted（生产者 = 在册表查询；实机对应 del taa/velocity/…）
		{ factsFor({{true, false}}, true), "del", kRefusedNotAdopted },
		// add + 不在场 + 门关 → gate-off（实机对应出厂档 add taa/velocity/ao_apply/ssr）
		{ factsFor({{false, false}}, false), "add", kRefusedGateOff },
		// add + 门开 + 不在场 + 不在册（防御分支，真机不可达）→ not-adopted
		{ factsFor({{false, false}}, true), "add", kRefusedNotAdopted },
		// 成功场景（done=true，无档）：del 幂等 / add 幂等 / 两条 attempt（档由事务定案给）
		{ factsFor({{false, true}}, true), "del", "" },
		{ factsFor({{true, true}}, false), "add", "" },
		{ factsFor({{false, true}}, true), "add", "" },
		{ factsFor({{true, true}}, true), "del", "" },
	};
	return s;
}

}  // namespace

TEST_CASE("decorate 分类：del 的在册表三态（对称判据逐半边）") {
	// 不在册 + 在场：not-adopted，且**不发事务**（决策顺序：在册先于一切）
	auto d = classifyDecorate("taa", "del", factsFor({{true, false}}, true));
	REQUIRE(d.done == false);
	REQUIRE(d.action == DecorateAction::respond);
	REQUIRE(d.refusedAt == kRefusedNotAdopted);
	REQUIRE(d.reason.find("p0") != std::string::npos);  // 点名到具体 pass

	// 在册 + 在场：发事务（撤向）
	d = classifyDecorate("highlight", "del", factsFor({{true, true}}, true));
	REQUIRE(d.action == DecorateAction::attempt);
	REQUIRE(d.attemptOn == false);
	REQUIRE(d.attemptPasses.size() == 1);

	// 在册 + 已离场：幂等成功（withdrawPass 对已撤回名字就是空操作，探针不假装失败）
	d = classifyDecorate("highlight", "del", factsFor({{false, true}}, true));
	REQUIRE(d.action == DecorateAction::respond);
	REQUIRE(d.done == true);
	REQUIRE(d.refusedAt.empty());

	// 多 pass 混合：只撤在场的，逆序（先 p1 后 p0——正序撤会让后一个的试编译认错前驱）
	d = classifyDecorate("taa", "del", factsFor({{true, true}, {true, true}, {false, true}}, true));
	REQUIRE(d.action == DecorateAction::attempt);
	REQUIRE(d.attemptPasses == std::vector<std::string_view>({ "p1", "p0" }));
}

TEST_CASE("decorate 分类：add 的四级决策顺序（在场 > 门 > 在册）") {
	// 在场优先于门：全在场 + 门关 → 幂等成功（读数不依赖配置档；翻转顺序本用例红）
	auto d = classifyDecorate("picking", "add", factsFor({{true, true}}, false));
	REQUIRE(d.done == true);
	REQUIRE(d.action == DecorateAction::respond);

	// 不在场 + 门关 → gate-off（不碰事务）
	d = classifyDecorate("taa", "add", factsFor({{false, false}}, false));
	REQUIRE(d.done == false);
	REQUIRE(d.action == DecorateAction::respond);
	REQUIRE(d.refusedAt == kRefusedGateOff);
	// reason 必须带上「没实现 ≠ 做不到」这句——读数会被读成排期还是架构，全看这句在不在
	REQUIRE(d.reason.find("没实现") != std::string::npos);

	// 不在场 + 门开 + 在册 → 发事务（插向）；只插缺席的
	d = classifyDecorate("highlight", "add", factsFor({{false, true}}, true));
	REQUIRE(d.action == DecorateAction::attempt);
	REQUIRE(d.attemptOn == true);
	REQUIRE(d.attemptPasses.size() == 1);

	// 门开 + 不在场 + 不在册（防御分支）→ not-adopted，reason 点名「不可达」
	d = classifyDecorate("shadow", "add", factsFor({{false, false}}, true));
	REQUIRE(d.done == false);
	REQUIRE(d.refusedAt == kRefusedNotAdopted);
	REQUIRE(d.reason.find("不可达") != std::string::npos);
}

TEST_CASE("decorate 词表覆盖：每一档都有场景产出，且只产词表内的档") {
	std::set<std::string_view> produced;
	for (const auto& s : scenarios()) {
		auto d = classifyDecorate("f", s.op, s.facts);
		if (d.action == DecorateAction::attempt) {
			// attempt 的失败档由事务定案给：注入失败事务，它必须产 trial-compile
			produced.insert(decorateVerdictFromTransaction(false, "x").refusedAt);
			continue;
		}
		if (d.done) {
			// 场景表注记说它该成功：成功不许带档
			REQUIRE(s.expectRefusedAt.empty());
			REQUIRE(d.refusedAt.empty());
			continue;
		}
		REQUIRE(d.refusedAt == s.expectRefusedAt);
		produced.insert(d.refusedAt);
	}
	// 事务定案的失败半边也是生产者（trial-compile 的真实来源 = applyPassToggles 结果格）
	produced.insert(decorateVerdictFromTransaction(false, "boom").refusedAt);
	// 双向：词表每一档都有场景产它（加一档没有生产者 = 本断言红，任务变异 ④）；
	//        场景产出的每一档都在词表里（分类函数不许私产野档）。
	std::set<std::string_view> vocab(kRefusedVocabulary.begin(), kRefusedVocabulary.end());
	CHECK(produced == vocab);
	REQUIRE_FALSE(produced.empty());  // 防空集合假绿（空 == 空 会过）
}

TEST_CASE("decorate 事务定案：成败两半 + 引擎原文逐字进 reason") {
	auto ok = decorateVerdictFromTransaction(true, "");
	REQUIRE(ok.done == true);
	REQUIRE(ok.refusedAt.empty());

	const std::string_view engineError = "dynamic graph: 试编译失败（读前置：x 先于 y）";
	auto bad = decorateVerdictFromTransaction(false, engineError);
	REQUIRE(bad.done == false);
	REQUIRE(bad.refusedAt == kRefusedTrialCompile);
	// 原文整段在场（归因靠原文不靠解析；丢弃原文的变异本断言红）
	REQUIRE(bad.reason.find(std::string(engineError)) != std::string::npos);
}

TEST_CASE("decorate 快照差异：出现/消失 × pass/附件 四半边 + 空 diff") {
	const auto before = graphWith({ "swapchain", "scene_hdr" }, { "light", "tonemap" });
	const auto after = graphWith({ "swapchain", "scene_hdr", "extra_rt" },
		{ "light", "extra_pass" });
	auto diff = decorateDiffJson(captureGraphNameSnapshot(before),
		captureGraphNameSnapshot(after));
	CHECK(diff["appearedPasses"] == nlohmann::json::array({ "extra_pass" }));
	CHECK(diff["disappearedPasses"] == nlohmann::json::array({ "tonemap" }));
	CHECK(diff["appearedAttachments"] == nlohmann::json::array({ "extra_rt" }));
	CHECK(diff["disappearedAttachments"] == nlohmann::json::array());

	// 反向半边（before/after 对调 → 出现与消失互换；只钉一半会被读成整条已验）
	auto back = decorateDiffJson(captureGraphNameSnapshot(after),
		captureGraphNameSnapshot(before));
	CHECK(back["appearedPasses"] == nlohmann::json::array({ "tonemap" }));
	CHECK(back["disappearedPasses"] == nlohmann::json::array({ "extra_pass" }));
	CHECK(back["appearedAttachments"] == nlohmann::json::array());
	CHECK(back["disappearedAttachments"] == nlohmann::json::array({ "extra_rt" }));

	// 同图 → 四数组全空（幂等 / 被拒格的诚实读数）。守卫：输入非空，否则「全空」恒真。
	REQUIRE_FALSE(before.passes.empty());
	auto same = decorateDiffJson(captureGraphNameSnapshot(before),
		captureGraphNameSnapshot(before));
	for (const auto& key : { "appearedPasses", "disappearedPasses", "appearedAttachments",
		                     "disappearedAttachments" })
		CHECK(same[key].empty());
}

TEST_CASE("decorate 回包形状：refusedAt 条件性两半 + 键集") {
	DecorateDecision d;
	d.done = true;
	d.reason = "ok";
	auto j = decorateResponseJson("highlight", "del", 7u, d,
		decorateDiffJson(GraphNameSnapshot {}, GraphNameSnapshot {}));
	CHECK(j["ok"] == true);
	CHECK(j["feature"] == "highlight");
	CHECK(j["op"] == "del");
	CHECK(j["done"] == true);
	CHECK(j["frame"] == 7u);
	CHECK_FALSE(j.contains("refusedAt"));  // 成功半边：无失败环节可报
	CHECK(j["diff"].is_object());

	d.done = false;
	d.refusedAt = kRefusedGateOff;
	d.reason = "gate";
	j = decorateResponseJson("taa", "add", 9u, d,
		decorateDiffJson(GraphNameSnapshot {}, GraphNameSnapshot {}));
	REQUIRE(j.contains("refusedAt"));  // 失败半边：必须有档可归档
	CHECK(j["refusedAt"] == "gate-off");
	std::set<std::string_view> vocab(kRefusedVocabulary.begin(), kRefusedVocabulary.end());
	CHECK(vocab.contains(j["refusedAt"].get<std::string_view>()));

	// 顶层键集 = schema 登记的 response.fields（少键/多键都会让实机对拍红，这里先钉单元半边）
	std::set<std::string> keys;
	for (auto it = j.begin(); it != j.end(); ++it)
		keys.insert(it.key());
	CHECK(keys == std::set<std::string>({ "ok", "feature", "op", "done", "refusedAt", "reason",
			"diff", "frame" }));
}

TEST_CASE("decorate 特征表：七个 feature 逐条吃生成常量且可查回") {
	// 表非空 + 条数 = schema oneOf 的 7（schema 加 feature 不补表 = 本条与 rules_rpc 面 7 红）
	REQUIRE(decorateFeatures().size() == 7);
	const std::vector<std::string_view> all = {
		rpc_value::decorate::feature::highlight, rpc_value::decorate::feature::picking,
		rpc_value::decorate::feature::shadow, rpc_value::decorate::feature::taa,
		rpc_value::decorate::feature::velocity, rpc_value::decorate::feature::ao_apply,
		rpc_value::decorate::feature::ssr,
	};
	for (const auto name : all) {
		const auto* f = findDecorateFeature(name);
		REQUIRE_MESSAGE(f != nullptr, name);
		CHECK(f->name == name);
		REQUIRE_FALSE(f->passes.empty());  // 空 pass 名单的分类恒真 = 假绿
	}
	CHECK(findDecorateFeature("zzz") == nullptr);
	// shadow 列全部级联（特征「在图」= 全部级联在图；只列一级会把「其余三级没了」读成在场）
	CHECK(findDecorateFeature(rpc_value::decorate::feature::shadow)->passes.size() == 4);
}

}  // namespace RGL
