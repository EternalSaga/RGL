module;
#include <fmt/format.h>
module offgraph_contract:impl;

import offgraph_contract;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;  // 可缺席锚点命中 0 的启动期打点（债-128；warn 档 = console sink 可见）
import vkerror;
import render_graph;

namespace RGL {

namespace {

/** 统一错误构造：`ErrorCode::RenderGraphInvalid` + message（同 render_graph_impl 的口径）。 */
Error contractError(std::string message) {
	return Error{ErrorCode::RenderGraphInvalid, std::move(message)};
}

/**
 * pass kind 的名字（只用于错误消息归因；与 Lua 配置词表无关，故不引 pass_vocabulary）。
 * @param kind pass 种类
 * @return 可读名
 */
const char* passKindName(RgPassKind kind) {
	switch (kind) {
	case RgPassKind::drawMesh:
		return "drawMesh";
	case RgPassKind::blit:
		return "blit";
	case RgPassKind::drawLight:
		return "drawLight";
	}
	return "?";
}

/**
 * 锚点的可读描述（错误消息里点名"哪个挂点没命中"，而不是让人回去猜申报写的是什么）。
 * @param anchor 待描述的锚点
 * @return 形如 `framePrologue` / `beforePass(kind=drawLight, state="light")`
 */
std::string describeAnchor(const RgOffGraphAnchor& anchor) {
	if (anchor.kind == RgOffGraphAnchor::Kind::framePrologue)
		return "framePrologue";
	return fmt::format("beforePass(kind={}, state=\"{}\")",
		passKindName(anchor.passKind), anchor.passState);
}

/**
 * 把一个锚点解析成相位（`build` 对每条申报各调一次；拆出来是为了让 build 本体留在
 * 认知复杂度红线内——判据一条没少，只是不再三层嵌套）。
 *
 * @param decl 待解析的申报项（错误消息里要点名它）
 * @param passes 编译图的 pass 计划
 * @param passCount passes.size()（调用方已算，避免这里再取一次 size_t）
 * @return 相位；命中 0 个或多个 pass = RenderGraphInvalid（例外：`mayBeAbsent` 的锚点
 *         命中 0 返回 `kUnschedulablePhase` 并打一条启动期 warn，不报错）
 */
std::expected<uint32_t, Error> resolveAnchor(const RgOffGraphWorkDecl& decl,
	const std::vector<RgPassPlan>& passes, uint32_t passCount) {
	if (decl.anchor.kind == RgOffGraphAnchor::Kind::framePrologue) {
		// 帧首段恒可解析（不查图）⟹「可缺席」对它没有任何可表达的语义。静默忽略会让
		// 「以为声明了可缺席」与「忘了声明」不可区分，所以**拒绝**而不是吞掉（债-128）。
		if (decl.anchor.mayBeAbsent)
			return std::unexpected(contractError(fmt::format(
				"off-graph contract: '{}' 声明了 framePrologue + mayBeAbsent = 无意义组合"
				"（帧首段锚点不查编译图、恒可解析，'可缺席' 只对 beforePass 有意义）",
				decl.name)));
		return OffGraphSchedule::kProloguePhase;
	}

	uint32_t hits = 0;
	uint32_t hitIndex = 0;
	for (uint32_t i = 0; i < passCount; ++i) {
		const auto& pass = passes[i];
		if (pass.kind != decl.anchor.passKind)
			continue;
		if (!decl.anchor.passState.empty() && pass.state != decl.anchor.passState)
			continue;
		++hits;
		hitIndex = i;
	}
	if (hits == 0 && decl.anchor.mayBeAbsent) {
		// 债-128：**逐条显式**的容忍档。同一张申报表服务多张编译图（自定义图可以没有
		// drawLight "light"），命中 0 的含义从「装配错」变成「本图不可调度」：相位给
		// 不可达哨兵 ⟹ enter 打上来必红（挂点错）、帧末门开着必红（静默旁路）。
		// 日志不静默且必须在 warn 档：这条本身就是异常形态（语义上就该是 warn），而
		// `tools/smoke.py` / `tools/regress.py` 的告警计数只数 `[warning]`（`count_severities`），
		// info 不进计数 —— 降到 info 就等于对既有判据隐身。
		// 出厂（deferred）图四条锚点全命中，这行**不该**出现，出现了就是图形态不对。
		if (auto* lg = RLLogger::getInstance())
			lg->warn("off-graph contract: '{}' 的锚点 {} 在本编译图里命中 0 个 pass（共 {} 个）"
				"= 本图不可调度（门开着却整帧没录会每帧点名；门关 = 正常旁路）",
				decl.name, describeAnchor(decl.anchor), passCount);
		return OffGraphSchedule::kUnschedulablePhase;
	}
	if (hits == 0)
		return std::unexpected(contractError(fmt::format(
			"off-graph contract: '{}' 的锚点 {} 在编译图里命中 0 个 pass（共 {} 个 pass；"
			"state 改名或该 pass 不在这张图里 = 装配错，必须启动期出声）",
			decl.name, describeAnchor(decl.anchor), passCount)));
	if (hits > 1)
		return std::unexpected(contractError(fmt::format(
			"off-graph contract: '{}' 的锚点 {} 命中 {} 个 pass = 有歧义"
			"（挂点必须唯一，否则相位无法确定）",
			decl.name, describeAnchor(decl.anchor), hits)));
	return OffGraphSchedule::beforePassPhase(hitIndex);
}

}  // namespace

std::expected<OffGraphSchedule, Error> OffGraphSchedule::build(
	std::span<const RgOffGraphWorkDecl> decls, const std::vector<RgPassPlan>& passes) {
	OffGraphSchedule schedule;
	schedule.passCount = static_cast<uint32_t>(passes.size());
	schedule.entries.reserve(decls.size());

	for (const auto& decl : decls) {
		for (const auto& existing : schedule.entries)
			if (existing.name == decl.name)
				return std::unexpected(contractError(
					fmt::format("off-graph contract: 申报重名 '{}'（同名两项无法分别归因）", decl.name)));
		auto phase = resolveAnchor(decl, passes, schedule.passCount);
		if (!phase)
			return std::unexpected(std::move(phase).error());
		schedule.entries.push_back(Entry{.name = decl.name, .phase = *phase});
	}
	return schedule;
}

void OffGraphSchedule::beginFrame() {
	currentPhase = 0;
	for (auto& entry : entries)
		entry.called = false;
}

std::expected<void, Error> OffGraphSchedule::enter(std::string_view name) {
	for (auto& entry : entries) {
		if (entry.name != name)
			continue;
		// 债-128：本图无锚点的申报**不可录**。不许落到下面的相位比较去报一条
		// 「位置契约违例——申报相位 4294967295」——那是把「挂点错」伪装成「录错时机」，
		// 归因方向整个错掉（修复位置也不同：一个是装配错，一个是调用点错）。
		if (entry.phase == kUnschedulablePhase)
			return std::unexpected(contractError(fmt::format(
				"off-graph contract: '{}' 在本图不可调度（锚点命中 0）却被录制 = 挂点错"
				"（可缺席锚点只在锚点命中的图里可录；这张图里录它 = 装配错）", name)));
		if (entry.phase != currentPhase)
			return std::unexpected(contractError(fmt::format(
				"off-graph contract: '{}' 位置契约违例——申报相位 {}，实际相位 {}"
				"（口径：帧首段 = {}，第 i 个 pass 之前 = {}+2i，其体内 = {}+2i+1，"
				"pass 循环之后 = {}）",
				name, entry.phase, currentPhase, kProloguePhase, kFirstPassPhase, kFirstPassPhase,
				frameEndPhase())));
		if (entry.called)
			return std::unexpected(contractError(fmt::format(
				"off-graph contract: '{}' 本帧重复录制（一次申报 = 一次录制）", name)));
		entry.called = true;
		return {};
	}
	return std::unexpected(contractError(fmt::format(
		"off-graph contract: 未申报的图外工作 '{}'（当前相位 {}）——新增加载点必须先进申报表",
		name, currentPhase)));
}

std::expected<void, Error> OffGraphSchedule::checkCoverage(std::span<const bool> active) const {
	if (active.size() != entries.size())
		return std::unexpected(contractError(fmt::format(
			"off-graph contract: 覆盖对账的 active 长度 {} ≠ 申报项数 {}（两处必须同序同长）",
			active.size(), entries.size())));
	for (uint32_t i = 0; i < static_cast<uint32_t>(entries.size()); ++i) {
		const auto& entry = entries[i];
		if (active[i] && !entry.called)
			return std::unexpected(contractError(fmt::format(
				"off-graph contract: '{}' 门开着却整帧没录 = 静默旁路"
				"（申报的挂点被删了，或门判据与录制判据不一致）", entry.name)));
		if (!active[i] && entry.called)
			return std::unexpected(contractError(fmt::format(
				"off-graph contract: '{}' 门关着却录了 = 破逐位不变红线（门关必须零命令）",
				entry.name)));
	}
	return {};
}

bool OffGraphSchedule::isSchedulable(std::string_view name) const {
	for (const auto& entry : entries)
		if (entry.name == name)
			return entry.phase != kUnschedulablePhase;
	return false;  // 名字不在表 = 不可调度（查询语义，同 OffGraphWorkTable::isActive）
}

// ── 批次 B2-5：申报面的撤回 / 恢复（pass 级动态插拔的配套一半）─────────────────────


/**
 * 在册名单（错误消息里点名"你给的名字不在这些里面"，而不是让人回去翻代码）。
 * 是成员函数而不是自由函数：`Entry` 是私有嵌套类型，自由函数点不到它的字段。
 */
std::string OffGraphWorkTable::knownNames(const std::vector<Entry>& items) {
	std::string out;
	for (const auto& entry : items)
		out += (out.empty() ? "" : ",") + entry.decl.name;
	return out;
}

OffGraphWorkTable::OffGraphWorkTable(std::vector<RgOffGraphWorkDecl> decls) {
	entries.reserve(decls.size());
	for (auto& decl : decls)
		entries.push_back(Entry { .decl = std::move(decl) });
}

std::vector<RgOffGraphWorkDecl> OffGraphWorkTable::all() const {
	std::vector<RgOffGraphWorkDecl> out;
	out.reserve(entries.size());
	for (const auto& entry : entries)
		out.push_back(entry.decl);
	return out;
}

std::vector<RgOffGraphWorkDecl> OffGraphWorkTable::activeDecls() const {
	std::vector<RgOffGraphWorkDecl> out;
	out.reserve(entries.size());
	for (const auto& entry : entries)
		if (entry.active)
			out.push_back(entry.decl);
	return out;
}

bool OffGraphWorkTable::isActive(std::string_view name) const {
	for (const auto& entry : entries)
		if (entry.decl.name == name)
			return entry.active;
	return false;
}

std::expected<void, Error> OffGraphWorkTable::withdraw(std::string_view name) {
	for (auto& entry : entries)
		if (entry.decl.name == name) {
			entry.active = false;  // 幂等：已撤回再撤一次 = 同样的 false
			entry.withdrawnBy.clear();
			return {};
		}
	return std::unexpected(contractError(fmt::format(
		"off-graph contract: withdraw 拒绝——申报 '{}' 不在册（在册：{}）。"
		"拼错的名字静默成功 = 以为撤了其实没撤，安全点会拿旧锚点建表",
		name, knownNames(entries))));
}

std::expected<void, Error> OffGraphWorkTable::redeclare(std::string_view name) {
	for (auto& entry : entries)
		if (entry.decl.name == name) {
			entry.active = true;
			entry.withdrawnBy.clear();
			return {};
		}
	return std::unexpected(contractError(fmt::format(
		"off-graph contract: redeclare 拒绝——申报 '{}' 不在册（在册：{}）",
		name, knownNames(entries))));
}

std::optional<uint32_t> OffGraphWorkTable::anchorPassIndex(
	const RgOffGraphWorkDecl& decl, const std::vector<RgPassPlan>& passes) {
	if (decl.anchor.kind != RgOffGraphAnchor::Kind::beforePass)
		return std::nullopt;  // 帧首段锚点不挂在任何 pass 上（pass 进出与它无关）
	uint32_t hits = 0;
	uint32_t hitIndex = 0;
	for (uint32_t i = 0; i < passes.size(); ++i) {
		if (passes[i].kind != decl.anchor.passKind)
			continue;
		if (!decl.anchor.passState.empty() && passes[i].state != decl.anchor.passState)
			continue;
		++hits;
		hitIndex = i;
	}
	// 命中数不是 1 时**不猜**：build 会用它自己的口径报「命中 0 个 / 有歧义」，
	// 这里若跟着猜一个下标，就会把「申报与图不一致」这件事在批量撤的路径上静默吞掉。
	if (hits != 1)
		return std::nullopt;
	return hitIndex;
}

std::expected<std::vector<std::string>, Error> OffGraphWorkTable::withdrawAnchoredTo(
	const std::vector<RgPassPlan>& passes, std::string_view passName) {
	const auto target = std::ranges::find(passes, passName, &RgPassPlan::name);
	if (target == passes.end())
		return std::unexpected(contractError(fmt::format(
			"off-graph contract: withdrawAnchoredTo 拒绝——编译图里没有 pass '{}'。"
			"本函数必须对着**撤 pass 之前**的图调用：图已经变了再来问锚点挂在哪个 pass 上，"
			"问不出答案（静默不撤 = 下一帧安全点报「命中 0 个 pass」，把事务没做全伪装成装配错）",
			passName)));
	const uint32_t targetIndex = static_cast<uint32_t>(target - passes.begin());
	std::vector<std::string> withdrawn;
	for (auto& entry : entries) {
		if (!entry.active)
			continue;  // 已撤回的不动（也不改它的撤回原因，否则 redeclare 会漏恢复）
		if (anchorPassIndex(entry.decl, passes) != targetIndex)
			continue;
		entry.active = false;
		entry.withdrawnBy = std::string { passName };
		withdrawn.push_back(entry.decl.name);
	}
	return withdrawn;
}

std::expected<std::vector<std::string>, Error>
OffGraphWorkTable::redeclareAnchoredTo(std::string_view passName) {
	std::vector<std::string> restored;
	for (auto& entry : entries) {
		if (entry.active || entry.withdrawnBy != passName)
			continue;
		entry.active = true;
		entry.withdrawnBy.clear();
		restored.push_back(entry.decl.name);
	}
	return restored;
}

}  // namespace RGL
