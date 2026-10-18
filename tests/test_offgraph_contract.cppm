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
 * 以下 7-12 属批次 B2-5（申报**表**：开关要能撤，申报表就得能摘）。新风险不是"锚点解析
 * 对不对"，而是**序**——帧末门位数组按申报的规范序取位，而撤回让"在册四条"与"在场三条"
 * 两种序并存；位序错一格通常还是 true，于是**没人报错**：
 *
 *   7. `withdraw`/`redeclare` 未知名改成静默成功 → 「撤回/恢复幂等」那条红（以为撤了其实
 *      没撤 = 安全点拿旧锚点建表，最坏失效方向）。
 *   8. 撤回**删除条目**而不是改在场位 → 「规范序」那条红（all() 少一项 = 后面所有门集体错位）。
 *   9. `activeDecls()` 忽略在场位（退化成 all()）→ 规范序 / 门位 / 兜底三条同时红。
 *  10. `withdrawAnchoredTo` 对已离场 pass 静默不撤 → 该用例红（把"事务没做全"伪装成装配错）。
 *  11. `redeclareAnchoredTo` 不看撤回原因 → 该用例红（手动关的东西被别人一次开档悄悄抵消）。
 *  12. 锚点解析忽略命中下标（恒用 pass 0）→ 「相位表随插拔重推」那条红（旧相位不被拒 =
 *      插拔后图外工作录在错的 pass 前，且**只有画面**会发现）。
 *  13. 帧末门位**按位**取而不是按名取（变异住在 `Renderer::offGraphGateActive`）→ 「门位必须
 *      按名取」那条红。该用例把两种取位形态**同时编码**：按名段必须绿、按位段必须红且点名
 *      被错位的那条申报——只留其中一半就是半瞎判据（对称判据的每一半边各要一条）。
 *
 * 以下 14-18 属债-128（锚点可缺席 = 申报集不随编译图缩小，forward 图起不来那笔）：
 *
 *  14. **可缺席做成全局容忍**（去掉逐条字段判据 / 恒容忍命中 0）→ 「逐条显式」那条的 ④ 段红
 *      （既有「建表反向②」也红）。这条就是裁决里点名的 M-a：全局容忍把「state 改名 = 装配错」
 *      整个拆掉，红必须落在**不可缺席**的那半，不是缺席的那半。
 *  15. `checkCoverage` 跳过不可调度项（"门开着却整帧没录"对它失效）→ 「三向判据」那条的 ② 段红。
 *  16. `enter` 对不可调度项改成静默记账（返回成功）→ 「三向判据」那条的 ⑥ 段红；若它只记账
 *      不返回成功，③ 段（门关 + called = "门关着却录了"）补刀。
 *  17. `framePrologue + mayBeAbsent` 静默接受（不拒绝无意义组合）→ 「逐条显式」那条的帧首段段红。
 *  18. 容忍写歪成「mayBeAbsent 一律不可调度」（命中 1 也标不可调度）或扩到命中多个
 *      （`hits != 1` 走容忍）→ 「可缺席只管命中 0」那条红（正向半边：deferred 图里可缺席
 *      申报必须正常可调度；歧义半边：命中多个仍 Error）。
 *  19. 把 `mayBeAbsent` 扩成三条申报以外的一切形态（第二刀：fsr_upscale 也标可缺席后）：
 *      标到帧首段项（meshlet_cull）= build 拒「无意义组合」并点名，单元层等价形态由
 *      「锚点可缺席（逐条显式）」的帧首段段钉住，真程序读数 = 启动 throw（M-d → §7.21）；
 *      反向把三条的标记回退成必须命中 = 无 tonemap 形态的图重新起不来，判据在**真程序层**
 *      （生产表不是单测输入；offscreen 内建变体已删〔刀①〕，该形态现仅自定义 blit 图可写，
 *      M-e = 起跑即启动 throw → §7.21）。
 *      未标项命中 0 仍 Error 的半边由 ④ 段常驻守着——「扩」与「缩」各有一把刀，缺一把
 *      「三条」这个口径就只是注释。
 *
 * ## 期望值的出处（不抄被测码）
 *
 * 相位口径来自**接口注释里写死的帧内模型**（帧首段 = 1、第 i 个 pass 之前 = 2+i、
 * pass 循环之后 = 2+N），本文件按该闭式**手算**期望值；被测实现若与注释不一致，
 * 红的是实现，而不是测试跟着改。
 */
#include <doctest/doctest.h>
#include <array>             // 帧末门位数组（vector<bool> 构不成 span<const bool>）
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

/** 一条挂在 highlight 上的申报（= 动态 pass 自带图外工作的形状）。 */
RgOffGraphWorkDecl highlightWork(std::string name = "hl_work") {
	return RgOffGraphWorkDecl{
		.name = std::move(name),
		.anchor = RgOffGraphAnchor{
			.kind = RgOffGraphAnchor::Kind::beforePass,
			.passKind = RgPassKind::drawMesh,
			.passState = "highlight",
		},
	};
}

/** 一条**可缺席**的「某 pass 之前」申报（债-128：izb_mask / evssm_moments 的形状）。 */
RgOffGraphWorkDecl absentWork(std::string name, std::string state) {
	return RgOffGraphWorkDecl{
		.name = std::move(name),
		.anchor = RgOffGraphAnchor{
			.kind = RgOffGraphAnchor::Kind::beforePass,
			.passKind = RgPassKind::drawLight,
			.passState = std::move(state),
			.mayBeAbsent = true,
		},
	};
}

/**
 * 非 deferred 图形态的 pass 序列（原出厂 forward 图已随 2026-10-18 刀②.2 删除，本序列是
 * 它那两条锚点相关事实的存活表示）：opaque / transparent / ao / tonemap ——
 * **没有 drawLight "light"**，债-128 的病根。tonemap 保留是判据的一半：fsr_upscale 的锚点
 * 在这种图上也必须命中（不许顺手也标可缺席）。原序列里的 skybox 一段随该绘制机制同刀删除，
 * 它是 drawMesh 段、不参与任何锚点匹配，删掉不改变本文件判据的射程。
 */
std::vector<RgPassPlan> noLightPasses() {
	return {
		pass("opaque", RgPassKind::drawMesh, "opaque"),
		pass("transparent", RgPassKind::drawMesh, "transparent"),
		pass("ao", RgPassKind::drawLight, "ao"),
		pass("tonemap", RgPassKind::drawLight, "tonemap"),
	};
}

/**
 * offscreen 形态的 pass 序列（原内建 offscreen 图的形状：三段渲 scene_rt + composite
 * blit，SDR；该变体已删〔2026-10-18 刀①〕，本函数是该形态在测试里的唯一存活表示）：
 * **没有 tonemap**（也没有 drawLight "light"）——债-128 第二刀里
 * fsr_upscale 的锚点在该图缺席的直接来源。
 */
std::vector<RgPassPlan> offscreenLikePasses() {
	return {
		pass("opaque", RgPassKind::drawMesh, "opaque"),
		pass("transparent", RgPassKind::drawMesh, "transparent"),
		pass("composite", RgPassKind::blit, ""),
	};
}

/**
 * 出厂序列 + highlight（插在 light **之前**）= 动态插拔后的图。
 * 下标：shadow 0 / geometry 1 / highlight 2 / light 3 / tonemap 4。
 */
std::vector<RgPassPlan> withHighlightBeforeLight() {
	auto passes = builtinLikePasses();
	passes.insert(passes.begin() + 2, pass("highlight", RgPassKind::drawMesh, "highlight"));
	return passes;
}

/** 四条申报的表（照生产 offGraphWorkDecls 的规范序，另加一条挂在 highlight 上）。 */
std::vector<RgOffGraphWorkDecl> tableDecls() {
	return {
		prologueWork("meshlet_cull"),
		beforePassWork("izb_mask", "light"),
		highlightWork("hl_work"),
		beforePassWork("fsr_upscale", "tonemap"),
	};
}

/** 申报名序列（规范序判据的读法）。 */
std::vector<std::string> namesOf(const std::vector<RgOffGraphWorkDecl>& decls) {
	std::vector<std::string> out;
	for (const auto& decl : decls)
		out.push_back(decl.name);
	return out;
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

// ═══════════════════════════════════════════════════════════════════════════
// 批次 B2-5：申报**表**（OffGraphWorkTable）——开关要能撤，申报表就得能摘。
//
// 这一档的新风险不是"锚点解析对不对"（上面已验），而是**序**：帧末门位数组按申报的
// 规范序取位，而撤回会把表变成"在册四条 / 在场三条"两种序并存。位序一错，
// 门就集体错位一格——错的那格通常还是 true，于是**没人报错**。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("OffGraphWorkTable：撤回/恢复幂等；未知名字必须 Error（与 pass 级 API 同口径）") {
	OffGraphWorkTable table(tableDecls());
	CHECK(table.size() == 4u);
	CHECK(table.isActive("izb_mask"));

	REQUIRE(errOf(table.withdraw("izb_mask")).empty());
	CHECK_FALSE(table.isActive("izb_mask"));
	// 幂等：复合事务重放同一意图不该炸（pass 撤回 + 申报撤回是一笔事务，可能重放）
	REQUIRE(errOf(table.withdraw("izb_mask")).empty());
	CHECK_FALSE(table.isActive("izb_mask"));

	// 恢复与撤回成对，且也幂等
	REQUIRE(errOf(table.redeclare("izb_mask")).empty());
	CHECK(table.isActive("izb_mask"));
	REQUIRE(errOf(table.redeclare("izb_mask")).empty());
	CHECK(table.isActive("izb_mask"));

	// 🔴 未知名字 = Error 且点名 + 带在册名单：拼错的名字若静默成功 = 以为撤了其实没撤，
	//    下个安全点还会拿旧锚点建表（最坏失效方向 = 看起来关了自己还开着）。
	const auto typo = errOf(table.withdraw("izb_msk"));
	REQUIRE_FALSE(typo.empty());
	CHECK(typo.find("izb_msk") != std::string::npos);
	CHECK(typo.find("izb_mask") != std::string::npos);  // 在册名单在消息里（归因）
	REQUIRE_FALSE(errOf(table.redeclare("izb_msk")).empty());
	CHECK(table.isActive("izb_mask"));  // 拒绝无副作用
}

TEST_CASE("OffGraphWorkTable：规范序 = 帧末门位的位序，撤回只改在场位、绝不重排或删除") {
	OffGraphWorkTable table(tableDecls());
	const std::vector<std::string> canonical
		= {"meshlet_cull", "izb_mask", "hl_work", "fsr_upscale"};
	CHECK(namesOf(table.all()) == canonical);

	REQUIRE(errOf(table.withdraw("izb_mask")).empty());
	REQUIRE(errOf(table.withdraw("meshlet_cull")).empty());

	// 🔴 all() 仍是四条、仍是原序：门位数组按它取位，删项 = 后面所有门集体错位一格。
	//    （"撤回顺手清理"是最容易被写出来的那种错，所以这条要单独钉。）
	CHECK(namesOf(table.all()) == canonical);
	CHECK(table.size() == 4u);
	// activeDecls() = **保序**子集（不是按名排序、不是倒序）
	CHECK(namesOf(table.activeDecls()) == std::vector<std::string> {"hl_work", "fsr_upscale"});
}

TEST_CASE("withdrawAnchoredTo：必须对着**撤 pass 之前**的图调用；反向恢复只恢复那一批") {
	OffGraphWorkTable table(tableDecls());
	const auto passes = withHighlightBeforeLight();

	// 正向：hl_work 挂在 highlight 上 → 撤 highlight 时它一起离场，返回值点名它
	const auto gone = table.withdrawAnchoredTo(passes, "highlight");
	REQUIRE(gone.has_value());
	CHECK(gone.value() == std::vector<std::string> {"hl_work"});
	CHECK_FALSE(table.isActive("hl_work"));
	CHECK(table.isActive("izb_mask"));  // 别人的申报不许被顺手撤掉

	// 幂等：同一意图重放 = 空集（不 Error）
	const auto again = table.withdrawAnchoredTo(passes, "highlight");
	REQUIRE(again.has_value());
	CHECK(again.value().empty());

	// 没有申报挂在这个 pass 上 → 空集（生产里的 highlight 正是这一档：撤它不牵动任何申报）
	const auto none = table.withdrawAnchoredTo(passes, "geometry");
	REQUIRE(none.has_value());
	CHECK(none.value().empty());

	// 反向恢复：只恢复**因 highlight 撤掉**的那批
	const auto back = table.redeclareAnchoredTo("highlight");
	REQUIRE(back.has_value());
	CHECK(back.value() == std::vector<std::string> {"hl_work"});
	CHECK(table.isActive("hl_work"));

	// 手动撤的不该被 anchored 恢复捞回来——否则"我关了它"会被别人的一次开档悄悄抵消
	REQUIRE(errOf(table.withdraw("izb_mask")).empty());
	REQUIRE(table.redeclareAnchoredTo("highlight").has_value());
	CHECK_FALSE(table.isActive("izb_mask"));

	// 🔴 pass 已经不在图里时调用 = Error（静默不撤会把"事务没做全"伪装成装配错）
	std::vector<RgPassPlan> passesAfter = passes;
	passesAfter.erase(std::ranges::find_if(passesAfter,
		[](const RgPassPlan& p) { return p.name == "highlight"; }));
	const auto late = errOf(table.withdrawAnchoredTo(passesAfter, "highlight"));
	REQUIRE_FALSE(late.empty());
	CHECK(late.find("highlight") != std::string::npos);
}

TEST_CASE("相位表随插拔重推：highlight 插到 light 之前 ⟹ izb_mask 的合法相位 6 → 8") {
	const std::vector<RgOffGraphWorkDecl> decls
		= {prologueWork("meshlet_cull"), beforePassWork("izb_mask", "light")};

	// A：出厂序列，light 是下标 2 → 之前 = 2 + 2×2 = 6
	auto sa = OffGraphSchedule::build(decls, builtinLikePasses());
	REQUIRE(sa.has_value());
	sa->beginFrame();
	sa->advanceTo(sa->beforePassPhase(2));
	REQUIRE(sa->enter("izb_mask").has_value());

	// B：highlight 插到 light 之前 → light 下标 3 → 之前 = 8。
	// 🔴 期望值按**新图的闭式**手算，不抄 A 的读数：这条钉的是「build 真按传入的 passes
	// 重推相位」，而不是「模块记得上次算过什么」。Renderer 侧若缓存旧表，红的是 GPU 验收
	// （IZB 开档那条变异）；这里守的是模块这半边的无状态性。
	auto sb = OffGraphSchedule::build(decls, withHighlightBeforeLight());
	REQUIRE(sb.has_value());
	CHECK(sb->beforePassPhase(2) == 6u);  // 相位函数本身与图无关（纯函数）
	CHECK(sb->frameEndPhase() == 12u);    // 帧末 = 2 + 2×5（N 也随插拔变）
	sb->beginFrame();
	sb->advanceTo(sb->beforePassPhase(2));  // A 里合法的那个位置
	const auto stale = errOf(sb->enter("izb_mask"));
	REQUIRE_FALSE(stale.empty());  // 旧位置必须被拒 = 表没跟着挪就会在这里红
	sb->advanceTo(sb->beforePassPhase(3));
	CHECK(sb->enter("izb_mask").has_value());
}

TEST_CASE("帧末门位必须**按名**取：按位取 = 把别人的门当成它的门（撤回后位序错位）") {
	// 场景 = highlight 撤档后：在册四条、在场三条，而 fsr_upscale 的门本来就是关的。
	OffGraphWorkTable table(tableDecls());
	REQUIRE(errOf(table.withdraw("hl_work")).empty());
	const auto passes = builtinLikePasses();  // highlight 已离场
	auto built = OffGraphSchedule::build(table.activeDecls(), passes);
	REQUIRE(built.has_value());
	REQUIRE(built->workCount() == 3u);

	// 门的事实（按名登记 = Renderer::offGraphGateActive 的形态；只有 fsr 关着）
	auto gateOf = [](std::string_view name) { return name != "fsr_upscale"; };
	auto recordTwo = [&]() {
		built->beginFrame();
		built->advanceTo(built->prologuePhase());
		REQUIRE(built->enter("meshlet_cull").has_value());
		built->advanceTo(built->beforePassPhase(2));
		REQUIRE(built->enter("izb_mask").has_value());
		built->advanceTo(built->frameEndPhase());
	};

	// ✅ 按名取位：三条在场申报各查各的门
	// （容器必须是 std::array 而不是 vector<bool>：后者是位压缩特化，既拿不到 .data()
	//   也构不成 span<const bool> —— 生产侧的帧末门位数组踩的是同一个坑）
	std::array<bool, 3> byName{};
	size_t bit = 0;
	for (const auto& decl : table.activeDecls())
		byName[bit++] = gateOf(decl.name);
	REQUIRE(bit == 3u);
	recordTwo();
	{
		const auto cov = built->checkCoverage(std::span<const bool>(byName));
		INFO(errOf(cov));
		CHECK(cov.has_value());
	}

	// ❌ 按位取位（从**全表**的门位数组顺序取前三 = 撤回后最常见的错位）：fsr_upscale
	//    拿到的是 hl_work 的门（true）⟹ "门开着却整帧没录"。这条红就是"门位数组必须按名"
	//    的机器判据：没有它，把 offGraphGateActive 改回按位取是**全绿**的。
	recordTwo();
	{
		const std::array<bool, 3> byPosition = {true, true, true};
		const auto cov = built->checkCoverage(std::span<const bool>(byPosition));
		REQUIRE_FALSE(cov.has_value());
		// 点名必须是 fsr_upscale —— 被错位当成它门的那条申报（消息点不出名字 = 归因失败）
		CHECK(errOf(cov).find("fsr_upscale") != std::string::npos);
	}
}

TEST_CASE("兜底 fail-fast：忘了摘申报 → 重推相位表当场 Error 并点名那条申报") {
	// 事务只做了一半的真实形态：pass 撤了，挂在它上面的申报没摘。
	// 判据：既不能静默丢申报，也不能留到运行期由 enter 报"命中 0 个 pass"——建表这一步
	// 就得红，且消息点名是哪条申报、锚在哪个 state 上（这条是**兜底**，不是主防线：
	// 主防线是 withdrawAnchoredTo 在事务里被调用，见上一条用例）。
	OffGraphWorkTable table(tableDecls());
	const auto passes = builtinLikePasses();  // highlight 已离场，但 hl_work 仍在场
	const auto err = errOf(OffGraphSchedule::build(table.activeDecls(), passes));
	REQUIRE_FALSE(err.empty());
	CHECK(err.find("hl_work") != std::string::npos);
	CHECK(err.find("highlight") != std::string::npos);

	// 摘干净之后同一次重推必须绿（否则"guard 坏了"会被读成"建表一直有效"）
	REQUIRE(errOf(table.withdraw("hl_work")).empty());
	auto ok = OffGraphSchedule::build(table.activeDecls(), passes);
	INFO(errOf(ok));
	CHECK(ok.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// 债-128：锚点可缺席（申报集不随编译图缩小，forward 图起不来那笔）。
//
// 失效形态不是"锚点解析对不对"（上面已验），而是**容忍的范围**：全局容忍 0 命中会把
// 「state 改名 = 装配错」这条启动期护栏整个拆掉（B1 第一刀立它的理由）。所以每条用例
// 都把「可缺席」与「不可缺席」两种形态**同图对照**——只测可缺席那半边就是半瞎判据。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("锚点可缺席（逐条显式）：命中 0 → 建表成功 + 该项不可调度；不可缺席同形态仍红；帧首段组合被拒") {
	const auto passes = noLightPasses();  // 没有 drawLight "light"

	// ① 生产申报集（四条，非 deferred 图形态）：build 成功、**申报集不缩小**，
	//    锚 light 的两条不可调度，锚帧首段 / tonemap 的两条正常可调度。
	{
		const std::vector<RgOffGraphWorkDecl> decls = {
			prologueWork("meshlet_cull"),
			absentWork("izb_mask", "light"),
			absentWork("evssm_moments", "light"),
			beforePassWork("fsr_upscale", "tonemap"),
		};
		auto built = OffGraphSchedule::build(decls, passes);
		INFO(errOf(built));
		REQUIRE(built.has_value());
		CHECK(built->workCount() == 4u);  // 申报集不随图缩小（kOffGraphWorkCount 口径不受影响）
		CHECK(built->isSchedulable("meshlet_cull"));
		CHECK_FALSE(built->isSchedulable("izb_mask"));
		CHECK_FALSE(built->isSchedulable("evssm_moments"));
		CHECK(built->isSchedulable("fsr_upscale"));  // 本图有 tonemap ⟹ 必须仍可调度
	}
	// ④ 🔴 正向锚点（变异 M-a「全局容忍」的红点）：同图同锚点，**只把 mayBeAbsent 翻回 false**
	//    ⟹ 仍 Error。做成全局容忍时这段会绿 = 「state 改名 = 装配错」被拆掉。
	{
		const std::vector<RgOffGraphWorkDecl> decls = {beforePassWork("izb_mask", "light")};
		auto built = OffGraphSchedule::build(decls, passes);
		REQUIRE_FALSE(built.has_value());
		CHECK(errOf(built).find("izb_mask") != std::string::npos);
		CHECK(errOf(built).find("0 个 pass") != std::string::npos);
	}
	// 帧首段锚点恒可解析 ⟹「可缺席」对它无意义，build **拒绝**这种组合并点名（静默忽略
	// 会让「以为声明了」与「忘了声明」不可区分）。
	{
		const RgOffGraphWorkDecl decl = {
			.name = "prologue_absent",
			.anchor = RgOffGraphAnchor{
				.kind = RgOffGraphAnchor::Kind::framePrologue,
				.mayBeAbsent = true,
			},
		};
		auto built = OffGraphSchedule::build(std::span<const RgOffGraphWorkDecl>{&decl, 1}, passes);
		REQUIRE_FALSE(built.has_value());
		CHECK(errOf(built).find("prologue_absent") != std::string::npos);
	}
}

TEST_CASE("可缺席只管「命中 0」：命中多个仍歧义红；命中 1 时可缺席申报必须正常可调度") {
	// 歧义半边：非 deferred 图 + 两个 drawLight "light" ⟹ 命中 2，mayBeAbsent 不豁免
	//（歧义与缺席是两件事：容忍缺席不该顺手把"相位无法确定"也吞掉）。
	{
		auto ambiguous = noLightPasses();
		ambiguous.push_back(pass("light", RgPassKind::drawLight, "light"));
		ambiguous.push_back(pass("light2", RgPassKind::drawLight, "light"));
		const std::vector<RgOffGraphWorkDecl> decls = {absentWork("izb_mask", "light")};
		auto built = OffGraphSchedule::build(decls, ambiguous);
		REQUIRE_FALSE(built.has_value());
		CHECK(errOf(built).find("歧义") != std::string::npos);
	}
	// 正向半边：同一条可缺席申报在 deferred 图里必须**正常可调度**（期望值按接口注释的
	// 闭式手算：light 是下标 2 → 之前 = 2 + 2×2 = 6）。容忍写歪成「mayBeAbsent 一律不可
	// 调度」时这段红——出厂档四条锚点全命中必须逐位不变，靠的就是这半边。
	{
		const std::vector<RgOffGraphWorkDecl> decls = {absentWork("izb_mask", "light")};
		auto built = OffGraphSchedule::build(decls, builtinLikePasses());
		REQUIRE(built.has_value());
		CHECK(built->isSchedulable("izb_mask"));
		built->beginFrame();
		built->advanceTo(built->beforePassPhase(2));
		CHECK(built->enter("izb_mask").has_value());
	}
}

TEST_CASE("不可调度项的三向判据：没锚点却录了红 / 门开着没录红 / 门关绿") {
	const std::vector<RgOffGraphWorkDecl> decls = {
		absentWork("izb_mask", "light"),
		prologueWork("meshlet_cull"),
	};
	auto built = OffGraphSchedule::build(decls, noLightPasses());
	REQUIRE(built.has_value());

	// ⑥ 没锚点却录了 = 挂点错。消息点名该项，且**不借**「位置契约违例」那条
	//    （归因方向：装配错 ≠ 录错时机，修复位置也不同）。
	built->beginFrame();
	{
		const auto err = errOf(built->enter("izb_mask"));
		REQUIRE_FALSE(err.empty());
		CHECK(err.find("izb_mask") != std::string::npos);
		CHECK(err.find("不可调度") != std::string::npos);
	}
	// enter 必须**无记账副作用**：若变异成静默记账，这条门关对账会红在
	// 「门关着却录了」（M-c 的第二把刀，不依赖 beginFrame 之前的状态）。
	{
		const bool active[2] = {false, false};
		const auto cov = built->checkCoverage(std::span<const bool>{active, 2});
		INFO(errOf(cov));
		CHECK(cov.has_value());
	}
	// ② 门开着（izb 门 = true）却整帧没录 = 静默旁路（复用既有消息族，不新造判据；
	//    变异 M-b「对账跳过不可调度项」红在这里）。
	built->beginFrame();
	{
		const bool active[2] = {true, false};  // izb 门开、meshlet 门关
		const auto cov = built->checkCoverage(std::span<const bool>{active, 2});
		REQUIRE_FALSE(cov.has_value());
		CHECK(errOf(cov).find("izb_mask") != std::string::npos);
		CHECK(errOf(cov).find("静默旁路") != std::string::npos);
	}
	// ③ 门关 = 不响（正常的旁路形态；缺这条就分不清「护栏没触发」与「护栏坏了」）。
	built->beginFrame();
	{
		const bool active[2] = {false, false};
		const auto cov = built->checkCoverage(std::span<const bool>{active, 2});
		INFO(errOf(cov));
		CHECK(cov.has_value());
	}
}

TEST_CASE("offscreen 形态（无 tonemap）：fsr_upscale 不可调度而 meshlet_cull 仍可；门开着没录仍红") {
	// 生产申报集在 offscreen 图的形状（四条全申报，**申报集不缩小**）：izb/evssm/fsr 三条
	// 可缺席锚点全部命中 0，帧首段的 meshlet_cull 恒可解析。
	const std::vector<RgOffGraphWorkDecl> decls = {
		prologueWork("meshlet_cull"),
		absentWork("izb_mask", "light"),
		absentWork("evssm_moments", "light"),
		absentWork("fsr_upscale", "tonemap"),
	};
	auto built = OffGraphSchedule::build(decls, offscreenLikePasses());
	INFO(errOf(built));
	REQUIRE(built.has_value());
	CHECK(built->workCount() == 4u);
	CHECK(built->isSchedulable("meshlet_cull"));      // 帧首段不查图，恒可解析
	CHECK_FALSE(built->isSchedulable("izb_mask"));
	CHECK_FALSE(built->isSchedulable("evssm_moments"));
	CHECK_FALSE(built->isSchedulable("fsr_upscale")); // offscreen 无 tonemap pass

	// 三向半边（fsr_upscale 版，形状照「不可调度项的三向判据」）：门开（fsr != nullptr 且
	// 输入可解析）却整帧没录 = 静默旁路 —— 残余风险「输入齐备却没有 tonemap pass」的
	// 每帧红就是它的真程序形态；门关（computeFsrUsable 输入不齐 = 不构造 fsr）= 不响。
	built->beginFrame();
	{
		const bool active[4] = {false, false, false, true};  // 只有 fsr 门开
		const auto cov = built->checkCoverage(std::span<const bool>{active, 4});
		REQUIRE_FALSE(cov.has_value());
		CHECK(errOf(cov).find("fsr_upscale") != std::string::npos);
		CHECK(errOf(cov).find("静默旁路") != std::string::npos);
	}
	built->beginFrame();
	{
		const bool active[4] = {false, false, false, false};
		const auto cov = built->checkCoverage(std::span<const bool>{active, 4});
		INFO(errOf(cov));
		CHECK(cov.has_value());
	}
	// 反向半边（不许删）：同一条可缺席申报在**非 deferred 图**里必须仍可调度 —— 「offscreen
	// 标了可缺席」不等于「fsr_upscale 永远不可调度」，非 deferred 图的 FSR 档全靠这半边。
	auto noLight = OffGraphSchedule::build(decls, noLightPasses());
	REQUIRE(noLight.has_value());
	CHECK(noLight->isSchedulable("fsr_upscale"));
	CHECK_FALSE(noLight->isSchedulable("izb_mask"));  // 本图有 tonemap、没有 light：两半各归各
}

}  // namespace RGL
