module;
#include <fmt/format.h>
module meshlet_ranges:impl;

import meshlet_ranges;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import meshlet_asset;

namespace RGL {
namespace {

/**
 * 四条不变量各自的错误串前缀（🔴 **两两不同**是判据的一部分：装配层把错误串原样打进
 * WARNING，读日志的人要能一眼分清是哪条结构变了，而不是回来读代码）。
 * 与 `meshlet_asset` 读取侧「每条校验各返回不同错误串」同纪律。
 */
constexpr const char* kInv1GroupsContiguous =
	"meshlet ranges: invariant 1 (groups form one contiguous run per prim)";
constexpr const char* kInv2SingleRange =
	"meshlet ranges: invariant 2 (one cluster range per prim)";
constexpr const char* kInv3Tiling =
	"meshlet ranges: invariant 3 (prim ranges tile [0, clusterCount))";
constexpr const char* kInv4L0Prefix =
	"meshlet ranges: invariant 4 (L0 clusters are a prefix of the prim range)";

/** L0 簇判据 = `refined == -1`（"产出本簇的更细 group = 无" = 原始几何，见 DiskCluster 注释）。 */
bool isL0Cluster(const MeshletDiskCluster& c) {
	return c.refined == -1;
}

/**
 * 不变量 2：把**同一个 prim** 的那一段 group 合成单一簇区间。
 *
 * 判据 = 把段内各 group 的 `[clusterOffset, +clusterCount)` 按下起点排序后**首尾相接**：
 * 任何一处 `off != end` 就是「这个 prim 的簇被拆成多段」（或两段重叠）——本表布局
 * （每 prim 一个 `[base, count)`）装不下它，必须报错而不是取 min/max 蒙过去（取 min/max
 * 会把中间不属于本 prim 的簇一起圈进来 = 静默多画）。
 * @param run 同一 prim 的连续 group 区间（调用方按 prim 切好段）
 * @param prim 段所属 prim（只进错误串）
 * @param clusterCount 簇表条数（区间上界）
 */
std::expected<MeshletPrimRange, std::string> mergeRunIntoRange(
	std::span<const MeshletDiskGroup> run, uint32_t prim, uint64_t clusterCount) {
	std::vector<std::pair<uint32_t, uint32_t>> spans;
	spans.reserve(run.size());
	for (const auto& g : run)
		spans.emplace_back(g.clusterOffset, g.clusterCount);
	std::sort(spans.begin(), spans.end());

	const uint64_t base = spans.front().first;
	uint64_t end = base;
	for (const auto& [off, cnt] : spans) {
		if (off != end)
			return std::unexpected(fmt::format(
				"{}: prim {} group cluster ranges are not contiguous (a range starts at {}, "
				"expected {}) — its clusters are split into several ranges",
				kInv2SingleRange, prim, off, end));
		end = off + cnt;  // uint64：off/cnt 来自文件，加法不绕回
	}
	if (end > clusterCount)
		return std::unexpected(fmt::format(
			"{}: prim {} cluster range [{}, {}) is beyond the cluster table ({} clusters)",
			kInv2SingleRange, prim, base, end, clusterCount));
	return MeshletPrimRange{.base = static_cast<uint32_t>(base),
	                        .count = static_cast<uint32_t>(end - base)};
}

/**
 * 不变量 1 + 2：按 group 表序切段，逐段合成区间。
 *
 * 不变量 1 的判据写成「**下一段的 prim 必须恰等于已切出的段数**」——一条比较同时挡住三种坏形态：
 * prim 回退（段被打散）、同 prim 出现第二段（交错）、跳过某个 prim（漏号）。
 * 段数与 primCount 的相等性单独在循环后判（有 prim 一条 group 都没有 = 该 prim 没有区间）。
 */
std::expected<std::vector<MeshletPrimRange>, std::string> scanPrimRuns(
	const MeshletAsset& asset) {
	std::vector<MeshletPrimRange> perPrim;
	perPrim.reserve(asset.header.primCount);
	const auto groups = asset.groups;
	size_t i = 0;
	while (i < groups.size()) {
		const uint32_t prim = groups[i].prim;
		if (prim >= asset.header.primCount)
			return std::unexpected(fmt::format(
				"{}: group {} carries prim {} but primCount is {}", kInv1GroupsContiguous, i,
				prim, asset.header.primCount));
		if (prim != perPrim.size())
			return std::unexpected(fmt::format(
				"{}: group {} starts prim {} but the next expected prim is {} — runs must appear "
				"in table order 0,1,2,…", kInv1GroupsContiguous, i, prim, perPrim.size()));
		size_t j = i;
		while (j < groups.size() && groups[j].prim == prim)
			++j;
		auto merged = mergeRunIntoRange(groups.subspan(i, j - i), prim, asset.clusters.size());
		if (!merged)
			return std::unexpected(merged.error());
		perPrim.push_back(*merged);
		i = j;
	}
	if (perPrim.size() != asset.header.primCount)
		return std::unexpected(fmt::format(
			"{}: {} prim runs but primCount is {} — some prim owns no group",
			kInv1GroupsContiguous, perPrim.size(), asset.header.primCount));
	return perPrim;
}

/**
 * 不变量 3：按 prim 序拼出的区间必须**铺满** `[0, clusterCount)`——起点 0、逐段衔接、末点
 * 恰等于簇表条数。空洞 = 有簇不属于任何 prim（永远画不到）；重叠 = 同一簇被两个 prim 画。
 */
std::optional<std::string> checkTiling(std::span<const MeshletPrimRange> perPrim,
                                       uint64_t clusterCount) {
	if (perPrim.empty()) {
		if (clusterCount != 0)
			return fmt::format(
				"{}: no prim ranges but the cluster table holds {} clusters — nothing owns them",
				kInv3Tiling, clusterCount);
		return std::nullopt;
	}
	uint64_t expect = 0;
	for (size_t p = 0; p < perPrim.size(); ++p) {
		if (perPrim[p].base != expect)
			return fmt::format(
				"{}: prim {} range starts at {}, expected {} — hole or overlap in the "
				"prim-ordered tiling", kInv3Tiling, p, perPrim[p].base, expect);
		expect = perPrim[p].base + perPrim[p].count;
	}
	if (expect != clusterCount)
		return fmt::format(
			"{}: prim-ordered ranges cover [0, {}) but the cluster table holds {} clusters",
			kInv3Tiling, expect, clusterCount);
	return std::nullopt;
}

/**
 * 不变量 4：每 prim 的 L0 簇必须是区间的**前缀**，顺带把 `l0Count` 数出来。
 *
 * 判据 = 先吃掉前缀里的 L0，剩下的区间里**一条 L0 都不许再出现**。这条成立才有 §10.1 的
 * 结论「恒 L0 档只要 `(base, l0Count)`，不必上传 Groups、shader 侧不必选级过滤」。
 * 前置：不变量 3 已过 ⟹ `base + count ≤ clusters.size()`，本函数不再判越界。
 */
std::optional<std::string> fillL0Prefixes(std::vector<MeshletPrimRange>& perPrim,
                                          std::span<const MeshletDiskCluster> clusters) {
	for (size_t p = 0; p < perPrim.size(); ++p) {
		auto& range = perPrim[p];
		const size_t base = range.base;  // 不变量 3 保证 base+count 落在簇表内
		uint32_t l0 = 0;
		while (l0 < range.count && isL0Cluster(clusters[base + l0]))
			++l0;
		for (uint32_t k = l0; k < range.count; ++k)
			if (isL0Cluster(clusters[base + k]))
				return fmt::format(
					"{}: prim {} has an L0 cluster at {} but a non-L0 one already at {} — "
					"L0 is not a prefix of the prim range", kInv4L0Prefix, p, base + k,
					base + l0);
		range.l0Count = l0;
	}
	return std::nullopt;
}

}  // namespace

std::expected<MeshletRangeScan, std::string> scanClusterRanges(const MeshletAsset& asset) {
	// 前置（**不是**第五条不变量，是扫描器自己的生存条件）：视图大小与 header 计数一致。
	// perPrim 按 header.primCount 预留、区间下标按视图取元素，两侧不一致时错的是**静默的
	// 区间**（不是崩溃、不是报错），所以扫描器自己钉一遍，不假设调用方一定过了读取侧。
	if (asset.groups.size() != asset.header.groupCount)
		return std::unexpected(fmt::format(
			"meshlet ranges: groups view holds {} entries but header.groupCount is {} — "
			"asset views are inconsistent with the header", asset.groups.size(),
			asset.header.groupCount));
	if (asset.clusters.size() != asset.header.clusterCount)
		return std::unexpected(fmt::format(
			"meshlet ranges: clusters view holds {} entries but header.clusterCount is {} — "
			"asset views are inconsistent with the header", asset.clusters.size(),
			asset.header.clusterCount));

	auto runs = scanPrimRuns(asset);
	if (!runs)
		return std::unexpected(runs.error());
	if (auto err = checkTiling(*runs, asset.clusters.size()))
		return std::unexpected(*err);
	if (auto err = fillL0Prefixes(*runs, asset.clusters))
		return std::unexpected(*err);

	MeshletRangeScan out;
	out.perPrim = std::move(*runs);
	for (const auto& r : out.perPrim) {
		out.totalClusters += r.count;
		out.totalL0Clusters += r.l0Count;
	}
	return out;
}

MeshletCullItemTable buildClusterCullItems(ClusterRangeTable& table) {
	// 一趟 slot 升序扫描同时产出两件事（cullBase 与 items 的下标关系必须同生共死，见接口注释）。
	// 用 running 而不是先算前缀和再第二趟填表：两趟之间任何对 bySlot 的改动都会让两者错位，
	// 而那种错位是**静默的**（标志读错 = 少画一块，没有错误信号）。
	MeshletCullItemTable out;
	uint32_t running = 0;
	for (size_t slot = 0; slot < table.bySlot.size(); ++slot) {
		ObjectClusterRange& range = table.bySlot[slot];
		range.cullBase = running;
		for (uint32_t local = 0; local < range.l0Count; ++local)
			out.items.push_back(MeshletCullItem{range.base + local, static_cast<uint32_t>(slot)});
		running += range.l0Count;
	}
	return out;
}

}  // namespace RGL
