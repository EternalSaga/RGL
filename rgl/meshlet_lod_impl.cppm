module;
#include <fmt/format.h>
module meshlet_lod:impl;

import meshlet_lod;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import meshlet_asset;
import meshlet_ranges;

namespace RGL {
namespace {

/**
 * 五条不变量各自的错误串前缀（🔴 **两两不同**是判据的一部分：批 B 把错误串原样打进
 * WARNING，读日志的人要能一眼分清是哪条结构变了，而不是回来读代码）。
 * 与 `meshlet_ranges` / `meshlet_asset` 「每条校验各返回不同错误串」同纪律。
 */
constexpr const char* kInv1Views =
	"meshlet lod: invariant 1 (asset views consistent with the header)";
constexpr const char* kInv2LevelContiguous =
	"meshlet lod: invariant 2 (cluster levels non-decreasing within each prim range)";
constexpr const char* kInv3L0CrossCheck =
	"meshlet lod: invariant 3 (level-0 run equals the scanned L0 cluster count)";
constexpr const char* kInv4L0Domain =
	"meshlet lod: invariant 4 (every prim with clusters owns at least one L0 cluster)";
constexpr const char* kInv5SelectionConsistency =
	"meshlet lod: invariant 5 (selected level non-empty iff the prim has clusters)";

/** prim 内一个**级段** = 该 prim 簇区间内同一级的连续子区间（§18.2 排序口径的机器形态）。 */
struct LevelSegment {
	/** DAG 级（= groups[cluster.group].depth，已验落在 [0, levelCount)）。 */
	uint32_t depth = 0;
	/** 相对 prim 区间起点的偏移（**不是**全局簇下标——接口契约的源头）。 */
	uint32_t start = 0;
	/** 段长（簇条数）。 */
	uint32_t count = 0;
	/** 段内 Σ triCount（uint64 累加，落进 MeshletLodPrimRange 时收窄，见使用点注释）。 */
	uint64_t triangles = 0;
};

/** 一个 prim 的级段表：depth 严格递增（不变量 2 保证「不减」，合并同段后严格递增）。 */
using PrimSegments = std::vector<LevelSegment>;

/**
 * 选级（纯函数，主选级与 costTable 共用同一实现——两处各写一遍迟早漂，
 * 契约 `costTable[levelD] == (drawnClusters, drawnTriangles)` 全靠这唯一一份）。
 *
 * - `deepest`：段表按 depth 升序 ⟹ 「depth ≤ levelD 的最后一段」一条规则同时覆盖
 *   「D 存在 → 用它」与「D 不存在 → ≤ D 的最深存在级」两种情形（§18.2 回落语义）。
 * - `l0`：D 存在 → 用它；否则级 0 段；级 0 段不存在 → nullptr（由不变量 5 点名，
 *   检查顺序理由见接口文件头）。
 * @return 选中的级段；nullptr = 无可选段
 */
const LevelSegment* selectSegment(
	const PrimSegments& segs, uint32_t levelD, MeshletLodFallback fallback) {
	if (fallback == MeshletLodFallback::l0) {
		for (const LevelSegment& s : segs)
			if (s.depth == levelD)
				return &s;
		for (const LevelSegment& s : segs)
			if (s.depth == 0)
				return &s;
		return nullptr;
	}
	const LevelSegment* best = nullptr;
	for (const LevelSegment& s : segs) {
		if (s.depth > levelD)
			break;
		best = &s;
	}
	return best;
}

/**
 * 某簇的级 = `groups[cluster.group].depth`（§18.2 口径），带两条前置（**不是**不变量，是
 * 「不假设调用方过了读取侧」的自保，各带点名错误串）：`cluster.group` 有界（本扫描器**要**
 * 经它跳 group 读 depth——`meshlet_ranges` 不跳所以不必防，这里必须防）、depth 落在
 * `[0, levelCount)`（histogram 按 levelCount 分配，越界 = 静默写歪或越界）。
 */
std::expected<uint32_t, std::string> clusterLevel(const MeshletAsset& asset, uint32_t global) {
	const MeshletDiskCluster& c = asset.clusters[global];
	if (c.group >= asset.groups.size())
		return std::unexpected(fmt::format(
			"meshlet lod: cluster {} references group {} out of range ({} groups) — asset "
			"views are inconsistent", global, c.group, asset.groups.size()));
	const int32_t depth = asset.groups[c.group].depth;
	if (depth < 0 || uint64_t(depth) >= asset.header.levelCount)
		return std::unexpected(fmt::format(
			"meshlet lod: cluster {} has level {} outside [0, levelCount {}) — declared DAG "
			"levels disagree with cluster depths", global, depth, asset.header.levelCount));
	return uint32_t(depth);
}

/**
 * 把一簇并进级段表：升级开新段、同级并段；**降级 = 不变量 2 破**（§18.2「每级是同一 prim
 * 内的连续子区间」不再成立 = 打包工具排序口径变了）。
 * @param global 该簇的全局簇下标（只进错误串）
 */
std::optional<std::string> appendClusterToSegments(
	PrimSegments& segs, uint32_t depth, uint32_t triCount, uint32_t start, size_t prim,
	uint32_t global) {
	if (segs.empty() || depth > segs.back().depth) {
		segs.push_back(LevelSegment { depth, start, 1, triCount });
		return std::nullopt;
	}
	if (depth == segs.back().depth) {
		segs.back().count += 1;
		segs.back().triangles += triCount;
		return std::nullopt;
	}
	return fmt::format(
		"{}: prim {} cluster {} has level {} but the previous cluster had {} — levels are no "
		"longer contiguous ascending sub-ranges (packer output order changed?)",
		kInv2LevelContiguous, prim, global, depth, segs.back().depth);
}

/**
 * 逐簇扫一个 prim 的簇区间，切成级段表（不变量 2 在 appendClusterToSegments 里逐簇判）。
 *
 * 前置（**不是**不变量）：区间落在簇表内——ranges 与 asset 必须同源，区间坏了错的是
 * **静默的选级**（越界读还是 UB），所以在这里点名。
 */
std::expected<PrimSegments, std::string> buildPrimSegments(
	const MeshletAsset& asset, const MeshletPrimRange& range, size_t prim) {
	PrimSegments segs;
	if (range.count == 0)
		return segs;
	if (uint64_t(range.base) + range.count > asset.clusters.size())
		return std::unexpected(fmt::format(
			"meshlet lod: prim {} cluster range [{}, {}) is beyond the cluster table ({} "
			"clusters) — ranges must come from scanClusterRanges on this asset",
			prim, range.base, uint64_t(range.base) + range.count, asset.clusters.size()));
	for (uint32_t k = 0; k < range.count; ++k) {
		const uint32_t global = range.base + k;
		auto depth = clusterLevel(asset, global);
		if (!depth)
			return std::unexpected(depth.error());
		if (auto err = appendClusterToSegments(segs, *depth, asset.clusters[global].triCount, k,
		         prim, global))
			return std::unexpected(*err);
	}
	return segs;
}

/** 单 prim 的扫描中间量（主循环只聚合，判据全在 scanOnePrim，两边的认知复杂度都低）。 */
struct PrimScan {
	PrimSegments segs;            ///< 级段表（costTable 重算用）
	MeshletLodPrimRange entry;    ///< 选级结果（无簇 = 全 0）
	uint32_t depth0Len = 0;       ///< `depth==0` 连续段长度（不变量 3 的左操作数）
	uint64_t l0Triangles = 0;     ///< 级 0 段的 Σ triCount
	bool selected = false;        ///< 是否选中了级段（无簇 prim = false，不计数不聚合）
	bool fellBack = false;        ///< 选中级 ≠ levelD（fallbackPrimCount 的判据）
};

/**
 * 单 prim：切段（含不变量 2 与前置）→ 不变量 3 → 选级 → 不变量 5 → 不变量 4。
 *
 * 🔴 5 排在 4 前是刻意的：`l0` 策略碰上「无级 0 段」的 prim 交出空选，先由 5 点名
 * 「选级与区间矛盾」；「prim 缺 L0」的根因在其余形态（选级非空但 l0Count==0）由 4 点名。
 * 这个顺序让两条各有一条**经真读取路径可达**的反向用例（tests/test_meshlet_lod）。
 */
std::expected<PrimScan, std::string> scanOnePrim(
	const MeshletAsset& asset, const MeshletPrimRange& range, size_t prim,
	uint32_t levelD, MeshletLodFallback fallback) {
	PrimScan out;
	auto built = buildPrimSegments(asset, range, prim);
	if (!built)
		return std::unexpected(built.error());
	out.segs = std::move(*built);
	const PrimSegments& segs = out.segs;

	// 不变量 3：`depth==0` 连续段长度 == 区间扫描的 l0Count（`refined == -1` 口径）。
	// 两条独立口径分家 = 工具语义变了；本条是 §18.2「真产物上二者相等」的机器护栏。
	if (!segs.empty() && segs.front().depth == 0) {
		out.depth0Len = segs.front().count;
		out.l0Triangles = segs.front().triangles;
	}
	if (out.depth0Len != range.l0Count)
		return std::unexpected(fmt::format(
			"{}: prim {} has {} clusters at level 0 but the range scan counted {} L0 clusters "
			"(refined == -1) — the two independent L0 criteria diverged", kInv3L0CrossCheck,
			prim, out.depth0Len, range.l0Count));

	const LevelSegment* sel = selectSegment(segs, levelD, fallback);

	// 不变量 5：选中级非空 ⟺ 该 prim 有簇（选级逻辑与区间扫描的自洽哨兵；
	// count==0 ⟹ segs 空 ⟹ sel 恒空，坏形态只剩「有簇却选空」一种，条件仍写双向以自洽）。
	if ((sel != nullptr) != (range.count > 0))
		return std::unexpected(fmt::format(
			"{}: prim {} selected {} clusters but its range holds {} — selection contradicts "
			"the range scan", kInv5SelectionConsistency, prim, sel ? sel->count : 0u,
			range.count));

	// 不变量 4：有簇的 prim 至少一个 L0 簇（`l0` 回落策略的定义域）。
	if (range.count > 0 && range.l0Count == 0)
		return std::unexpected(fmt::format(
			"{}: prim {} has {} clusters but no L0 one (l0Count 0) — the l0 fallback policy "
			"has no domain", kInv4L0Domain, prim, range.count));

	if (sel == nullptr)  // 无簇 prim：entry 保持全 0（不是错误，接口契约）
		return out;
	out.selected = true;
	out.fellBack = sel->depth != levelD;
	// triangles 收窄 uint64→uint32：单 prim 本级三角 ≤ 该 prim 全部三角 ≤ 产物总三角 < 2^32
	// （triCount ≤ 128 的 8bit 契约 × 簇表大小界；真产物单 prim 万级）。
	out.entry = MeshletLodPrimRange { sel->start, sel->count, sel->depth,
		static_cast<uint32_t>(sel->triangles) };
	return out;
}

/** 把一个 prim 的扫描结果聚合进产物（无簇 prim 只贡献零条目：不计数、不进 histogram）。 */
void mergePrimScan(MeshletLodScan& out, const PrimScan& s) {
	if (s.selected) {
		out.drawnClusters += s.entry.count;
		out.drawnTriangles += s.entry.triangles;
		out.maxLevelUsed = std::max(out.maxLevelUsed, s.entry.level);
		out.levelHistogram[s.entry.level] += 1;  // depth < levelCount 由前置保证
		// 🔴 这里**不乘** task 槽数：task 数唯一真相源 = meshletTaskCount（接口文件头）。
		out.maxClustersPerDraw = std::max(out.maxClustersPerDraw, s.entry.count);
		if (s.fellBack)
			out.fallbackPrimCount += 1;
	}
	out.perPrim.push_back(s.entry);
	if (!s.segs.empty())
		out.maxLevelInProduct = std::max(out.maxLevelInProduct, s.segs.back().depth);
	out.l0Clusters += s.depth0Len;
	out.l0Triangles += s.l0Triangles;
}

/**
 * costTable：对每个候选 D 按**当前** fallback 策略重算全表（§18.2 表的常驻机器版；
 * 与主选级共用 selectSegment ⟹ 契约 costTable[levelD] == (drawnClusters, drawnTriangles)）。
 */
std::vector<MeshletLodCostRow> buildCostTable(
	const std::vector<PrimScan>& scans, uint32_t maxLevelInProduct,
	MeshletLodFallback fallback) {
	std::vector<MeshletLodCostRow> table;
	table.reserve(maxLevelInProduct + 1);
	for (uint32_t d = 0; d <= maxLevelInProduct; ++d) {
		MeshletLodCostRow row;
		row.level = d;
		for (const PrimScan& s : scans)
			if (const LevelSegment* sel = selectSegment(s.segs, d, fallback)) {
				row.clusters += sel->count;
				row.triangles += sel->triangles;
			}
		table.push_back(row);
	}
	return table;
}

}  // namespace

std::expected<MeshletLodScan, std::string> scanMeshletLod(
	const MeshletAsset& asset, const MeshletRangeScan& ranges,
	uint32_t levelD, MeshletLodFallback fallback) {
	// 不变量 1（前置，照 scanClusterRanges 的做法）：视图大小 == header 计数。
	// 不重复读取侧的段表/范围校验，但两侧不一致时错的是**静默的选级**，自己钉一遍。
	if (asset.groups.size() != asset.header.groupCount)
		return std::unexpected(fmt::format(
			"{}: groups view holds {} entries but header.groupCount is {} — asset views are "
			"inconsistent with the header", kInv1Views, asset.groups.size(),
			asset.header.groupCount));
	if (asset.clusters.size() != asset.header.clusterCount)
		return std::unexpected(fmt::format(
			"{}: clusters view holds {} entries but header.clusterCount is {} — asset views "
			"are inconsistent with the header", kInv1Views, asset.clusters.size(),
			asset.header.clusterCount));
	// 前置（histogram 按 levelCount 分配）：levelCount 是 header 裸字段、不被任何段大小
	// 界住，超过簇数 + 1 = 分配面被 header 字段牵着走，拒。真产物 bistro：12 ≪ 61,519。
	if (asset.header.levelCount > asset.clusters.size() + 1)
		return std::unexpected(fmt::format(
			"meshlet lod: header.levelCount {} exceeds cluster count {} + 1 — the level "
			"histogram allocation would not be bounded by the product", asset.header.levelCount,
			asset.clusters.size()));

	MeshletLodScan out;
	out.requestedLevel = levelD;
	out.fallback = fallback;
	out.levelHistogram.assign(asset.header.levelCount, 0);

	std::vector<PrimScan> scans;
	scans.reserve(ranges.perPrim.size());
	for (size_t p = 0; p < ranges.perPrim.size(); ++p) {
		auto prim = scanOnePrim(asset, ranges.perPrim[p], p, levelD, fallback);
		if (!prim)
			return std::unexpected(prim.error());
		mergePrimScan(out, *prim);
		scans.push_back(std::move(*prim));
	}
	out.costTable = buildCostTable(scans, out.maxLevelInProduct, fallback);
	return out;
}

}  // namespace RGL
