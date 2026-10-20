module;
#include <fmt/format.h>
module shadow_proxy_types:impl;

import shadow_proxy_types;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import meshlet_asset;
import meshlet_ranges;

namespace RGL {
namespace {

/**
 * 前置：两份输入必须来自**同一份** asset（`meshlet_lod` invariant 1 的同族自保）。
 *
 * 它不占 invariant 号——invariant 1~5 判的是「产物自身与区间表的关系」，而这两条判的是
 * 「调用方给的两个参数是不是同一份数据」。混起来写会让归因变糊：日志里 `invariant 1` 指的
 * 是打包侧漂了，而这里指的是装配层传错了对象。
 */
std::optional<std::string> checkInputsAgree(const MeshletAsset& asset,
                                            const MeshletRangeScan& ranges) {
	if (ranges.perPrim.size() != asset.header.primCount)
		return fmt::format(
		    "shadow proxy: range scan disagrees with the asset header ({} prim ranges vs "
		    "primCount {}) — the two inputs are not from one product",
		    ranges.perPrim.size(), asset.header.primCount);
	if (asset.clusters.size() != asset.header.clusterCount)
		return fmt::format(
		    "shadow proxy: cluster view size {} disagrees with the asset header clusterCount {} "
		    "— the view was not produced by parseMeshletAsset",
		    asset.clusters.size(), asset.header.clusterCount);
	return std::nullopt;
}

/** `primOfSlot` 自己合法吗（越界值 = 装配层反查表建错，**不静默当无钉段放过**）。 */
std::optional<std::string> checkPrimOfSlot(std::span<const uint32_t> primOfSlot,
                                           size_t primCount) {
	for (size_t s = 0; s < primOfSlot.size(); ++s) {
		const uint32_t p = primOfSlot[s];
		if (p != kShadowProxyNoPrim && p >= primCount)
			return fmt::format(
			    "shadow proxy: primOfSlot[{}] = prim {} out of range (primCount {}) — the slot "
			    "lookup was built against a different product",
			    s, p, primCount);
	}
	return std::nullopt;
}

/**
 * 单条 MustL0 条目的四条产物侧不变量（④ 有界 → ③ 升序 → ① 前缀 → ② 不超 L0 段 → ⑤ 不越簇表）。
 *
 * 顺序有理由：**先判下标能不能用**（④），后面三条都要拿 `prim` 去索引 `perPrim`；③ 排在 ① 前
 * 是因为「重复 prim」时 ① 也会跟着红，先报根因（升序破了 = 打包侧写了两条同 prim）。
 */
std::optional<std::string> checkOnePinEntry(const MeshletAsset& asset, const MeshletRangeScan& ranges,
                                            const MeshletDiskMustL0& e, size_t index,
                                            uint32_t prevPrim) {
	// 🔴 ④ 必须**最先**判：后面三条都要拿 `e.prim` 去索引 `perPrim`，先解引用再判越界就是
	// 拿越界读去"检查"越界（UB 不会稳定复现成红，只会复现成假绿或崩）。
	if (e.prim >= ranges.perPrim.size())
		return fmt::format(
		    "shadow proxy: invariant 4 (MustL0 entry references prim out of range) entry {} names "
		    "prim {} but the product has {} prims",
		    index, e.prim, ranges.perPrim.size());
	const auto& range = ranges.perPrim[e.prim];
	if (index > 0 && e.prim <= prevPrim)
		return fmt::format(
		    "shadow proxy: invariant 3 (MustL0 entries are not strictly ascending by prim) entry {} "
		    "names prim {} while entry {} already named {}",
		    index, e.prim, index - 1, prevPrim);
	if (e.clusterOffset != range.base)
		return fmt::format(
		    "shadow proxy: invariant 1 (pinned run is not a prefix of the prim's L0 range) prim {} "
		    "pins from cluster {} but its cluster range starts at {} — the packer's L0 reorder moved "
		    "or the section drifted",
		    e.prim, e.clusterOffset, range.base);
	if (e.clusterCount > range.l0Count)
		return fmt::format(
		    "shadow proxy: invariant 2 (pinned cluster count exceeds the prim's L0 count) prim {} "
		    "pins {} clusters but only owns {} L0 clusters",
		    e.prim, e.clusterCount, range.l0Count);
	if (uint64_t(e.clusterOffset) + e.clusterCount > asset.clusters.size())
		return fmt::format(
		    "shadow proxy: invariant 5 (pinned cluster range out of bounds) prim {} pins clusters "
		    "[{}, {}) of a {}-cluster table",
		    e.prim, e.clusterOffset, uint64_t(e.clusterOffset) + e.clusterCount,
		    asset.clusters.size());
	return std::nullopt;
}

/**
 * 段内簇球的最远外包（球心取**首簇球心**，口径与保守方向 → `ShadowProxyTable::Entry::radius`）。
 *
 * 为什么手写距离而 `import` glm：本模块是 rlbase 的纯扫描层，`meshlet_asset` 的镜像结构存的是
 * `float center[3]`（磁盘布局镜像，不是数学类型）；为一行 `length()` 把 glm 拉进接口依赖不划算。
 * `std::sqrt` / `std::max` 由 `import rlstd;` 提供（rlstd 的 `export namespace std` 补齐面）。
 */
float pinnedRunRadius(const MeshletAsset& asset, const MeshletDiskMustL0& e) {
	const MeshletDiskCluster& first = asset.clusters[e.clusterOffset];
	float radius = 0.0f;
	for (uint32_t i = 0; i < e.clusterCount; ++i) {
		const MeshletDiskCluster& c = asset.clusters[e.clusterOffset + i];
		const float dx = c.center[0] - first.center[0];
		const float dy = c.center[1] - first.center[1];
		const float dz = c.center[2] - first.center[2];
		radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz) + c.radius);
	}
	return radius;
}

}  // namespace

std::expected<ShadowProxyTable, std::string> buildShadowProxyTable(
	const MeshletAsset& asset, const MeshletRangeScan& ranges,
	std::span<const uint32_t> primOfSlot) {
	if (auto err = checkInputsAgree(asset, ranges))
		return std::unexpected(*err);
	if (auto err = checkPrimOfSlot(primOfSlot, ranges.perPrim.size()))
		return std::unexpected(*err);

	// 第一遍：逐条核不变量（🔴 与「本场景有没有实例」无关 —— 产物侧的事实每次启动都全量验一遍，
	// 这就是算在启动期而不是离线换来的东西，计划 §3.6 R5），顺手建 prim → 条目下标 的反查
	// （invariant 3 保证段内 prim 严格升序，但条目稀疏：1591 个 prim 里只有 13 条，
	// 所以用按下标索引的 int32 表而不是二分）。
	std::vector<int32_t> indexOfPrim(ranges.perPrim.size(), -1);
	std::vector<float> radiusOfEntry(asset.mustL0.size(), 0.0f);
	uint32_t prevPrim = 0;
	for (size_t i = 0; i < asset.mustL0.size(); ++i) {
		const MeshletDiskMustL0& e = asset.mustL0[i];
		if (auto err = checkOnePinEntry(asset, ranges, e, i, prevPrim))
			return std::unexpected(*err);
		prevPrim = e.prim;
		indexOfPrim[e.prim] = static_cast<int32_t>(i);
		radiusOfEntry[i] = pinnedRunRadius(asset, e);
	}

	// 第二遍：按 objectSlot 落表。`start` 取**差值**而不是写死 0 —— invariant 1 保证它是 0，
	// 但把「恒 0」写进代码就等于把判据降级成假设（将来段内口径变了，改的是建表，不是消费点）。
	ShadowProxyTable table;
	table.bySlot.resize(primOfSlot.size());
	for (size_t s = 0; s < primOfSlot.size(); ++s) {
		const uint32_t p = primOfSlot[s];
		if (p == kShadowProxyNoPrim)
			continue;  // 该槽不是本产物的图元（无产物资产 / 别的模型的实例）= 全 0 条目，合法常态
		const int32_t idx = indexOfPrim[p];
		if (idx < 0)
			continue;  // 该 prim 没有钉段（bistro：1591 个 prim 里 1578 个如此），常态不是错误
		const MeshletDiskMustL0& e = asset.mustL0[static_cast<size_t>(idx)];
		table.bySlot[s] = ShadowProxyTable::Entry{
		    .start = e.clusterOffset - ranges.perPrim[p].base,
		    .count = e.clusterCount,
		    .radius = radiusOfEntry[static_cast<size_t>(idx)],
		};
	}
	return table;
}

}  // namespace RGL
