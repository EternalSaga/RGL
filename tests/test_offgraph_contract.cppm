module;
/**
 * test_offgraph_contract —— 图外工作申报面（批次 B1 第一刀 → docs/compute_调研.md §7）
 *
 * 被测 = rgl/offgraph_contract.cppm（rlscene 成员）的**纯数据**部分：建表（锚点 → 相位）、
 * 位置契约（enter）、覆盖对账（checkCoverage）。零 `VkDevice` 调用 ⟹ 进 `just test` 子集。
 *
 * ## 为什么值得测
 *
 * 这一层存在的唯一理由是「把注释变成断言」。它自己若不可信，B1 就是**用一份没人验的代码
 * 去验一份本来只是注释的契约**——比原状更糟，因为它会让人以为位置契约已经有人守了。
 *
 * ## 变异红线（哪个变异让我红；答不出 = 摆设）
 *
 *   1. 相位常量漂：`kFirstPassPhase` 2→3、`beforePassPhase` 少乘 2（pass 外/体内并档）、
 *      或 `frameEndPhase()` 少算 ⟹ 建表用例与「推进过头」用例同时红。
 *   2. **位置判据弱化**：`entry.phase != currentPhase` 改成 `entry.phase > currentPhase`
 *      （本地正确的另一半：只抓"提前录"不抓"延后录"）→ 「推进过头」那条红。
 *      这条就是 §7 判据 2 要求的变异本体（把 meshlet_cull 的调用点挪到 pass 循环**之后**）
 *      在单元层的等价形态。**为什么必须有它**：若位置判据写成布尔式的"在不在 pass 循环里"，
 *      循环结束后布尔会重新变 false ⟹ 那条变异全绿。相位单调性就是为它存在的。
 *   2b. **pass 分两档**（之前 / 体内）被并掉：`beforePassPhase(i)` 去掉 `2u *` ⟹
 *      「挂点漂进 pass 体内」那条红。这一档守的是三个 sandwich 的真实契约（它们必须录在
 *      dynamic rendering **之外**），只按 pass 分档时那种漂移完全 invisible。
 *   3. 覆盖对账**两个半边各一条用例**：删 `active && !called` 一半 → 「门开着没录」红；
 *      删 `!active && called` 一半 → 「门关着却录了」红（后者守的是逐位不变红线）。
 *   4. 建表三条 guard 各配同形态反向：删重名 guard → 重名用例红；删 `hits == 0` → 未命中红；
 *      删 `hits > 1` → 歧义红；而三条正向用例保证「guard 没生效」与「guard 坏了」可区分。
 *   5. `beginFrame` 不清 `called` → 跨帧那条红（第二帧门开着没录必须红）。
 *   6. `enter` 不记账（漏 `entry.called = true`）→ 「门关着却录了」那条红。
 *
 * ## 期望值的出处（不抄被测码）
 *
 * 相位口径来自**接口注释里写死的帧内模型**（帧首段 = 1、第 i 个 pass 之前 = 2+i、
 * pass 循环之后 = 2+N），本文件按该闭式**手算**期望值；被测实现若与注释不一致，
 * 红的是实现，而不是测试跟着改。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import rlstd 带进来
#include <string>
#include <vector>

export module test_offgraph_contract;

import rlstd;
import offgraph_contract;
import render_graph;  // RgPassPlan / RgPassKind（建表的输入）
import vkerror;       // Error（expected 的失败侧）

namespace RGL {
namespace {

/** 造一个 pass 计划（只填锚点会读的三个字段，其余默认）。 */
RgPassPlan pass(std::string name, RgPassKind kind, std::string state) {
	return RgPassPlan{.name = std::move(name), .kind = kind, .state = std::move(state)};
}

/**
 * 出厂形态的 pass 序列（照 `render_graph_builtin` 的 deferred 图缩到锚点相关的那几个）：
 * 帧首段之后依次是 shadow / geometry / light / tonemap。
 */
std::vector<RgPassPlan> builtinLikePasses() {
	return {
		pass("shadow", RgPassKind::drawMesh, "shadow0"),
		pass("geometry", RgPassKind::drawMesh, "gbuffer"),
		pass("light", RgPassKind::drawLight, "light"),
		pass("tonemap", RgPassKind::drawLight, "tonemap"),
	};
}

/** 一条帧首段申报（meshlet_cull 的形状）。 */
RgOffGraphWorkDecl prologueWork(std::string name = "meshlet_cull") {
	return RgOffGraphWorkDecl{.name = std::move(name)};
}

/** 一条「某 pass 之前」申报（IZB / EVSSM / FSR 的形状）。 */
RgOffGraphWorkDecl beforePassWork(std::string name, std::string state) {
	return RgOffGraphWorkDecl{
		.name = std::move(name),
		.anchor = RgOffGraphAnchor{
			.kind = RgOffGraphAnchor::Kind::beforePass,
			.passKind = RgPassKind::drawLight,
			.passState = std::move(state),
		},
	};
}

/** 建表并把错误消息取出来（失败时给 INFO 用；成功时返回空串）。 */
template <class Result>
std::string errOf(const Result& result) {
	return result.has_value() ? std::string{} : result.error().message;
}

std::string errOf(const std::expected<void, Error>& result) {
	return result.has_value() ? std::string{} : result.error().message;
}

}  // namespace

TEST_CASE("建表：帧首段 = 相位 1、第 i 个 pass 之前 = 2+2i、帧末 = 2+2N（按闭式手算）") {
	const auto passes = builtinLikePasses();  // N = 4
	const std::vector<RgOffGraphWorkDecl> decls = {
		prologueWork("meshlet_cull"),
		beforePassWork("izb_mask", "light"),
		beforePassWork("fsr_upscale", "tonemap"),
	};

	auto built = OffGraphSchedule::build(decls, passes);
	INFO(errOf(built));
	REQUIRE(built.has_value());

	// 手算期望：light 是第 2 个 pass（下标 2）→ 之前 = 2+4 = 6、体内 = 7；
	// tonemap 下标 3 → 之前 = 8；帧末 = 2+2×4 = 10。
	CHECK(built->prologuePhase() == 1u);
	CHECK(built->beforePassPhase(0) == 2u);
	CHECK(built->beforePassPhase(2) == 6u);
	CHECK(built->inPassPhase(2) == 7u);
	CHECK(built->frameEndPhase() == 10u);
	CHECK(built->workCount() == 3u);

	// 三条申报各自落在哪个相位：靠 enter 成功/失败来验，而不是打开私有表看。
	// 「izb 与 evssm 同相位」是允许的（两个 sandwich 都挂在 light 之前），故这里不查相位互斥。
	built->advanceTo(built->prologuePhase());
	CHECK(built->enter("meshlet_cull").has_value());
	built->advanceTo(built->beforePassPhase(2));
	CHECK(built->enter("izb_mask").has_value());
	built->advanceTo(built->beforePassPhase(3));
	CHECK(built->enter("fsr_upscale").has_value());
}

TEST_CASE("同一 pass 分两档：申报「pass 外」的项在「体内」录制必须红（挂点漂进 pass 内）") {
	const std::vector<RgOffGraphWorkDecl> decls = {beforePassWork("izb_mask", "light")};
	auto built = OffGraphSchedule::build(decls, builtinLikePasses());
	REQUIRE(built.has_value());
	const uint32_t lightIndex = 2;

	// 阳性锚点：在 pass 外录 = 绿。
	built->beginFrame();
	built->advanceTo(built->beforePassPhase(lightIndex));
	CHECK(built->enter("izb_mask").has_value());

	// 反向：同一条申报，挂点漂到 beginRendering **之后**（IZB 自己 beginMaskPass、FSR 发
	// barrier + compute，这些命令都不能出现在 dynamic rendering 实例里）→ 必须红。
	// 若相位只按 pass 分档（不分 pass 外/体内），这条会全绿 = 判据看不见最典型的漂移。
	built->beginFrame();
	built->advanceTo(built->beforePassPhase(lightIndex));
	built->advanceTo(built->inPassPhase(lightIndex));
	auto entered = built->enter("izb_mask");
	REQUIRE_FALSE(entered.has_value());
	CHECK(errOf(entered).find("位置契约违例") != std::string::npos);
}

TEST_CASE("建表反向：重名 / 命中 0 个 / 命中多个各拒绝；三条同形态正向必须通过") {
	const auto passes = builtinLikePasses();

	// ① 重名：同名两项无法分别归因。
	{
		const std::vector<RgOffGraphWorkDecl> decls = {
			prologueWork("meshlet_cull"),
			prologueWork("meshlet_cull"),
		};
		auto built = OffGraphSchedule::build(decls, passes);
		REQUIRE_FALSE(built.has_value());
		CHECK(errOf(built).find("meshlet_cull") != std::string::npos);  // 消息必须点名
	}
	// ① 反向：只差名字 = 通过（否则「guard 没生效」与「guard 坏了」不可区分）。
	{
		const std::vector<RgOffGraphWorkDecl> decls = {
			prologueWork("meshlet_cull"),
			prologueWork("cull_second"),
		};
		auto built = OffGraphSchedule::build(decls, passes);
		INFO(errOf(built));
		CHECK(built.has_value());
	}

	// ② 锚点命中 0 个：state 改名（图里是 "light"，申报写成 "lit"）。
	{
		const std::vector<RgOffGraphWorkDecl> decls = {beforePassWork("izb_mask", "lit")};
		auto built = OffGraphSchedule::build(decls, passes);
		REQUIRE_FALSE(built.has_value());
		CHECK(errOf(built).find("izb_mask") != std::string::npos);
		CHECK(errOf(built).find("0 个 pass") != std::string::npos);
	}
	// ② 反向：state 正确 = 通过。
	{
		const std::vector<RgOffGraphWorkDecl> decls = {beforePassWork("izb_mask", "light")};
		auto built = OffGraphSchedule::build(decls, passes);
		INFO(errOf(built));
		CHECK(built.has_value());
	}

	// ③ 命中多个 = 有歧义：两个 state 同为 "light" 的 drawLight pass。
	{
		auto ambiguous = passes;
		ambiguous.push_back(pass("light2", RgPassKind::drawLight, "light"));
		const std::vector<RgOffGraphWorkDecl> decls = {beforePassWork("izb_mask", "light")};
		auto built = OffGraphSchedule::build(decls, ambiguous);
		REQUIRE_FALSE(built.has_value());
		CHECK(errOf(built).find("歧义") != std::string::npos);
	}
	// ③ 反向：kind 相同但 state 不同 = 不算命中（匹配是两个条件的合取）。
	{
		auto twoLights = passes;
		twoLights.push_back(pass("tonemap2", RgPassKind::drawLight, "tonemap2"));
		const std::vector<RgOffGraphWorkDecl> decls = {beforePassWork("fsr_upscale", "tonemap")};
		auto built = OffGraphSchedule::build(decls, twoLights);
		INFO(errOf(built));
		CHECK(built.has_value());
	}

	// ④ 空 passState = 不按 state 匹配 ⟹ 多个 drawLight 直接歧义（这条 guard 的另一半）。
	{
		const RgOffGraphWorkDecl decl = {
			.name = "any_light",
			.anchor = RgOffGraphAnchor{.kind = RgOffGraphAnchor::Kind::beforePass},
		};
		auto built = OffGraphSchedule::build(std::span<const RgOffGraphWorkDecl>{&decl, 1}, passes);
		CHECK_FALSE(built.has_value());
	}
}

TEST_CASE("位置契约：三种偏离各红——未推进 / 推进过头（判据 2 的变异本体）/ 未申报") {
	const std::vector<RgOffGraphWorkDecl> decls = {
		prologueWork("meshlet_cull"),
		beforePassWork("izb_mask", "light"),
	};
	auto built = OffGraphSchedule::build(decls, builtinLikePasses());
	REQUIRE(built.has_value());

	// ① beginFrame 之后没推进就录（相位 0）：位置不对。
	built->beginFrame();
	CHECK(built->phase() == 0u);
	{
		auto entered = built->enter("meshlet_cull");
		REQUIRE_FALSE(entered.has_value());
		CHECK(errOf(entered).find("位置契约违例") != std::string::npos);
	}

	// ② 🔴 推进过头 = 把调用点挪到 pass 循环**之后**（§7 判据 2 要求的变异）。
	//    这条是整份测试的核心：布尔式的"在不在循环里"在这里会重新变成 false。
	built->advanceTo(built->frameEndPhase());
	{
		auto entered = built->enter("meshlet_cull");
		REQUIRE_FALSE(entered.has_value());
		CHECK(errOf(entered).find("meshlet_cull") != std::string::npos);
		CHECK(errOf(entered).find("位置契约违例") != std::string::npos);
	}
	// ② 的弱化判据探针：把 `!=` 改成 `>`（只抓提前录）时，上面那条会绿、下面这条必须红。
	built->advanceTo(built->prologuePhase());
	CHECK(built->enter("meshlet_cull").has_value());

	// ③ 未申报的工作项（新增图外加载点忘了进申报表）。
	built->advanceTo(built->beforePassPhase(1));
	{
		auto entered = built->enter("undeclared_work");
		REQUIRE_FALSE(entered.has_value());
		CHECK(errOf(entered).find("未申报") != std::string::npos);
	}
}

TEST_CASE("位置契约正向锚点：申报相位录制成功；同帧重复录制红") {
	const std::vector<RgOffGraphWorkDecl> decls = {beforePassWork("evssm_moments", "light")};
	auto built = OffGraphSchedule::build(decls, builtinLikePasses());
	REQUIRE(built.has_value());

	built->beginFrame();
	built->advanceTo(built->beforePassPhase(2));
	CHECK(built->enter("evssm_moments").has_value());

	// 同相位重复录制（一次申报 = 一次录制；两次 = 记账会被后一次掩盖）。
	auto again = built->enter("evssm_moments");
	REQUIRE_FALSE(again.has_value());
	CHECK(errOf(again).find("重复录制") != std::string::npos);
}

TEST_CASE("覆盖对账两个半边各红：门开着没录 / 门关着却录了") {
	const std::vector<RgOffGraphWorkDecl> decls = {
		prologueWork("meshlet_cull"),
		beforePassWork("izb_mask", "light"),
	};
	auto built = OffGraphSchedule::build(decls, builtinLikePasses());
	REQUIRE(built.has_value());

	// 半边 A：门开着（active = {true, true}）却一个都没录 = 静默旁路。
	built->beginFrame();
	{
		const bool active[2] = {true, true};
		auto coverage = built->checkCoverage(std::span<const bool>{active, 2});
		REQUIRE_FALSE(coverage.has_value());
		CHECK(errOf(coverage).find("meshlet_cull") != std::string::npos);
		CHECK(errOf(coverage).find("静默旁路") != std::string::npos);
	}

	// 半边 B：门关着（active = {false, false}）却录了一个 = 破「门关零命令」逐位不变红线。
	built->beginFrame();
	built->advanceTo(built->prologuePhase());
	REQUIRE(built->enter("meshlet_cull").has_value());
	{
		const bool active[2] = {false, false};
		auto coverage = built->checkCoverage(std::span<const bool>{active, 2});
		REQUIRE_FALSE(coverage.has_value());
		CHECK(errOf(coverage).find("meshlet_cull") != std::string::npos);
		CHECK(errOf(coverage).find("门关必须零命令") != std::string::npos);
	}

	// 两条正向（否则无法区分「没触发」与「坏了」）：门开且录了 / 门关且没录，都该绿。
	built->beginFrame();
	built->advanceTo(built->prologuePhase());
	REQUIRE(built->enter("meshlet_cull").has_value());
	built->advanceTo(built->beforePassPhase(2));
	REQUIRE(built->enter("izb_mask").has_value());
	{
		const bool active[2] = {true, true};
		auto coverage = built->checkCoverage(std::span<const bool>{active, 2});
		INFO(errOf(coverage));
		CHECK(coverage.has_value());
	}
	built->beginFrame();
	{
		const bool active[2] = {false, false};
		auto coverage = built->checkCoverage(std::span<const bool>{active, 2});
		INFO(errOf(coverage));
		CHECK(coverage.has_value());
	}
}

TEST_CASE("覆盖对账：active 长度不符拒绝（守卫），长度正确通过（反向）") {
	const std::vector<RgOffGraphWorkDecl> decls = {
		prologueWork("meshlet_cull"),
		beforePassWork("izb_mask", "light"),
	};
	auto built = OffGraphSchedule::build(decls, builtinLikePasses());
	REQUIRE(built.has_value());
	built->beginFrame();

	// 守卫：只报 1 个 active 位（新增申报项忘了同步门表 = 静默漏对账）。
	const bool shortActive[1] = {true};
	auto coverage = built->checkCoverage(std::span<const bool>{shortActive, 1});
	REQUIRE_FALSE(coverage.has_value());
	CHECK(errOf(coverage).find("active 长度") != std::string::npos);

	// 反向：长度 = workCount 时同一条调用必须绿（否则「guard 坏了」会被读成「对账一直有效」）。
	const bool okActive[2] = {false, false};
	auto ok = built->checkCoverage(std::span<const bool>{okActive, 2});
	INFO(errOf(ok));
	CHECK(ok.has_value());
}

TEST_CASE("beginFrame 跨帧清账：第二帧门开着没录必须红（记账不得活过一帧）") {
	const std::vector<RgOffGraphWorkDecl> decls = {prologueWork("meshlet_cull")};
	auto built = OffGraphSchedule::build(decls, builtinLikePasses());
	REQUIRE(built.has_value());
	const bool active[1] = {true};

	// 第 0 帧：正常录制 ⟹ 绿。
	built->beginFrame();
	built->advanceTo(built->prologuePhase());
	REQUIRE(built->enter("meshlet_cull").has_value());
	{
		auto coverage = built->checkCoverage(std::span<const bool>{active, 1});
		INFO(errOf(coverage));
		CHECK(coverage.has_value());
	}

	// 第 1 帧：不调用 ⟹ 必须红。若 beginFrame 漏清 called，这条会假绿。
	built->beginFrame();
	CHECK(built->phase() == 0u);  // 相位也必须归零
	{
		auto coverage = built->checkCoverage(std::span<const bool>{active, 1});
		REQUIRE_FALSE(coverage.has_value());
		CHECK(errOf(coverage).find("meshlet_cull") != std::string::npos);
	}
}

}  // namespace RGL
