module;
#include <fmt/format.h>
module mdi_rows:impl;

import mdi_rows;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;

namespace RGL {
namespace {

/**
 * 四条不变量各自的错误串前缀（🔴 **两两不同**是判据的一部分：★M-1 把错误串打进 WARNING，
 * 读日志的人要能一眼分清是哪条契约破了，而不是回来读代码）。与 `meshlet_ranges` /
 * `meshlet_lod` 「每条校验各返回不同错误串」同纪律。
 */
constexpr const char* kInv1RowCount = "mdi rows: invariant 1 (one row per candidate)";
constexpr const char* kInv2VisibleTasks =
	"mdi rows: invariant 2 (a visible candidate carries tasks)";
constexpr const char* kInv3Anchors =
	"mdi rows: invariant 3 (aggregate anchors agree with the rows)";
constexpr const char* kInv4RowOrder = "mdi rows: invariant 4 (row i is candidate i)";

/**
 * 候选 → 行的**唯一**映射公式（构建器写行、对账器算期望，两边共用这一份）。
 *
 * 两处各写一遍迟早漂（漂成「构建器写 gcX=0 而对账器期望 gcX=taskCount」时，对账恰好失去意义），
 * 所以公式只住这里。两条口径都在这一行上：
 *  · `groupCountX` = 可见 ? taskCount : 0（不可见 = GPU 空转，spike 的 `zero_groupcount_noop`
 *    实测 0 计数**不跳号** ⟹ 不压紧、不 atomic，§19.2 D4）；
 *  · 身份字段（objectIndex / clusterStart）**与可见性无关**，不可见行照写（红了要能定位是哪一行）。
 * 🔴 这里不出现 task 槽数：`taskCount` 是调用方算好的成品数（唯一真相源 = meshletTaskCount）。
 */
MdiShadowRow rowFor(const MdiShadowCandidate& c) {
	MdiShadowRow row;  // Y/Z = 1、保留 word = 0 由默认成员初始化给（接口文件）
	row.groupCountX = c.visible ? c.taskCount : 0u;
	row.objectIndex = c.objectSlot;
	row.clusterStart = c.clusterStart;
	return row;
}

/**
 * 不变量 2：**可见**候选的 `taskCount > 0`（输入契约，只有构建器判——对账器的输入是「已建好的
 * 行表」，那里同样的坏形态由不变量 3 的「空行数 vs 不可见候选数」点名）。
 *
 * 为什么宁可报错也不发一条 `gcX = 0` 的行：可见候选发空行 = 那块几何**该投影却没投**，而阴影图
 * 少一块影子在截图上完全可能没人看出来；报错让上游（LOD 选级表 count 为 0 却仍判可见 /
 * meshletTaskCount 被喂 0）当场现形。不可见候选的 `taskCount` 是 0 还是非 0 都不看（调用方
 * 不必特意清零，`visible = false` 说话）。
 */
std::optional<std::string> checkVisibleTasks(std::span<const MdiShadowCandidate> cands) {
	for (size_t i = 0; i < cands.size(); ++i)
		if (cands[i].visible && cands[i].taskCount == 0u)
			return fmt::format(
				"{}: candidate {} (objectSlot {}) is visible with taskCount 0 — refusing to "
				"emit a row that draws nothing (upstream contract broken)",
				kInv2VisibleTasks, i, cands[i].objectSlot);
	return std::nullopt;
}

/**
 * 不变量 1：行数 == 候选数，且 `rowCount` 锚 == 行数（两条各一条错误串：「数组坏了」与
 * 「锚与数组分家」是两件事，读日志要能分清）。
 *
 * 这条最可能怎么破（不是假想）：有人「顺手」把不可见行压紧（少发几条空行看着像优化），
 * 行序就不再等于候选序 ⟹ §19.1 第三行那条「depth-only ⟹ 逐位不变近乎构造性」的论证失效。
 */
std::optional<std::string> checkRowCount(const MdiShadowRowTable& table,
	size_t candidateCount) {
	if (table.rows.size() != candidateCount)
		return fmt::format(
			"{}: the row table holds {} rows but the candidate list holds {} — row order is "
			"the contract, dropping or compacting rows breaks the bitwise-invariance premise "
			"(§19.1)", kInv1RowCount, table.rows.size(), candidateCount);
	if (table.rowCount != table.rows.size())
		return fmt::format(
			"{}: rowCount says {} but the row table holds {} rows — the anchor and the array "
			"are no longer one fact", kInv1RowCount, table.rowCount, table.rows.size());
	return std::nullopt;
}

/**
 * 不变量 4：行 i 恰是候选 i 的行（**序**与逐字段身份）。
 *
 * 🔴 判据钉的是「逐元素」，不是聚合量：`Σ gcX` / 空行数在整体错位或两行互换时**一位不变**
 * （`test_meshlet_lod` 的 base-shift 教训：对账伙伴模块只钉聚合量 ⟹ 逐元素静默错位还报
 * 「对上了」）。这条也是 ★M-4「GPU 直写行表 vs CPU 构建器逐元素对账」的判据本体。
 * 前置：行数已等于候选数（不变量 1 先跑，否则这里越界读）。
 */
std::optional<std::string> checkRowOrder(
	const MdiShadowRowTable& table, std::span<const MdiShadowCandidate> cands) {
	for (size_t i = 0; i < table.rows.size(); ++i) {
		const MdiShadowRow want = rowFor(cands[i]);
		const MdiShadowRow& got = table.rows[i];
		if (got.groupCountX == want.groupCountX && got.objectIndex == want.objectIndex
			&& got.clusterStart == want.clusterStart)
			continue;
		return fmt::format(
			"{}: row {} holds {{gcX {}, objectIndex {}, clusterStart {}}} but candidate {} maps "
			"to {{gcX {}, objectIndex {}, clusterStart {}}} — rows must follow the candidate "
			"order element by element", kInv4RowOrder, i, got.groupCountX, got.objectIndex,
			got.clusterStart, i, want.groupCountX, want.objectIndex, want.clusterStart);
	}
	return std::nullopt;
}

/**
 * 不变量 3：三个聚合锚与行数组/候选清单自洽（三条各一条错误串）。
 * 期望值在这里**独立重算**（不复用构建器的累加结果），否则「锚与行分家」永远查不出来。
 *
 * 第三条（空行数 vs 不可见候选数）是前两条查不到的那格：`sumGroupCountX` 与 `emptyRows` 各自
 * 与行数组对得上，却仍可能「一个可见候选发出了空行」——那是 §19.2 D5 的 `mdi.rows` 闭式锚
 * 直接读错臂的形态（收益判据 A5 就靠它）。
 */
std::optional<std::string> checkAnchors(
	const MdiShadowRowTable& table, std::span<const MdiShadowCandidate> cands) {
	uint64_t sumX = 0;
	uint64_t emptyRows = 0;
	for (const MdiShadowRow& row : table.rows) {
		sumX += row.groupCountX;
		emptyRows += row.groupCountX == 0u ? 1u : 0u;
	}
	if (table.sumGroupCountX != sumX)
		return fmt::format(
			"{}: sumGroupCountX says {} but the rows sum to {} — the task-count anchor and "
			"the row array diverged", kInv3Anchors, table.sumGroupCountX, sumX);
	if (table.emptyRows != emptyRows)
		return fmt::format(
			"{}: emptyRows says {} but {} rows carry groupCountX 0 — the empty-row anchor and "
			"the row array diverged", kInv3Anchors, table.emptyRows, emptyRows);
	uint64_t invisible = 0;
	for (const MdiShadowCandidate& c : cands)
		invisible += c.visible ? 0u : 1u;
	if (emptyRows != invisible)
		return fmt::format(
			"{}: {} rows carry groupCountX 0 but {} candidates are invisible — a visible "
			"candidate emitted an empty row (or the table was compacted)", kInv3Anchors,
			emptyRows, invisible);
	return std::nullopt;
}

}  // namespace

std::optional<std::string> validateMdiShadowRows(
	const MdiShadowRowTable& table, std::span<const MdiShadowCandidate> cands) {
	// 顺序 = 由外而内：先「行数对得上」（否则逐行判据越界），再逐元素，最后聚合锚。
	if (auto err = checkRowCount(table, cands.size()))
		return err;
	if (auto err = checkRowOrder(table, cands))
		return err;
	return checkAnchors(table, cands);
}

std::expected<MdiShadowRowTable, std::string> buildMdiShadowRows(
	std::span<const MdiShadowCandidate> cands) {
	if (auto err = checkVisibleTasks(cands))
		return std::unexpected(*err);
	MdiShadowRowTable out;
	out.rows.reserve(cands.size());
	for (const MdiShadowCandidate& c : cands) {
		const MdiShadowRow row = rowFor(c);
		out.sumGroupCountX += row.groupCountX;
		out.emptyRows += row.groupCountX == 0u ? 1u : 0u;
		out.rows.push_back(row);
	}
	out.rowCount = out.rows.size();
	// 返回前自检（★M-4 的对账入口 = 同一份判据，不另写一遍）。构建器路径下不变量 1/3/4 的
	// 可达形态只有「本文件被改坏」——它们是**哨兵**，反向用例经导出的 validate 打（理由见
	// tests/test_mdi_rows 文件头）。
	if (auto err = validateMdiShadowRows(out, cands))
		return std::unexpected(*err);
	return out;
}

}  // namespace RGL
