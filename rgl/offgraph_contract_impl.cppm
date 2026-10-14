module;
#include <fmt/format.h>
module offgraph_contract:impl;

import offgraph_contract;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
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
 * @return 相位；命中 0 个或多个 pass = RenderGraphInvalid
 */
std::expected<uint32_t, Error> resolveAnchor(const RgOffGraphWorkDecl& decl,
	const std::vector<RgPassPlan>& passes, uint32_t passCount) {
	if (decl.anchor.kind == RgOffGraphAnchor::Kind::framePrologue)
		return OffGraphSchedule::kProloguePhase;

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

}  // namespace RGL
