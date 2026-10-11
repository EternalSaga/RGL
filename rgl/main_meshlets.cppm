module;
#include <entt/entt.hpp>
#include <fmt/format.h>

// 入口模块 main 的分区实现单元（module main:meshlets;）——簇化轴的**装配层接线**集中一处。
// 为什么单独成篇（★1-a1 挂号 ①，docs/mdi_plan.md §9）：main_scene.cppm 已 840 行，
// 「装配层接线别再往里塞」是上一批就记下的债；本单元只干一件事——把 CPU 侧簇产物扫描结果
// 落到 per-objectSlot 的表上，供 scene data 尾区上传。
module main:meshlets;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import scene_context;  // MeshRef（asset / objectSlot）+ NextObjectSlot（实例矩阵槽计数）
import meshlet_asset;  // MeshletAsset（只读视图：prims 身份字段）
import meshlet_ranges;  // scanClusterRanges / MeshletRangeScan / ClusterRangeTable
import scene_forward_types_generated;  // ObjectClusterRange（scene data 表条目，反射生成）

namespace RGL {
namespace {

/**
 * 扫描期三个**统计锚**的合计（★1-a1i 判据 5：装配日志里的这三个数必须与
 * `docs/mdi_plan.md` §10.1 的真产物实测一致 = 1591 / 61518 / 27487）。
 * 它们是「扫描实现与真产物结构对得上」的唯一常驻证据——合成小产物只验逻辑，验不出真形态。
 */
struct RangeTotals {
	uint64_t primRanges = 0;    ///< Σ 逐 prim 区间条数（= Σ primCount）
	uint64_t clusters = 0;      ///< Σ 全表簇数（= Σ clusterCount）
	uint64_t l0Clusters = 0;    ///< Σ L0 簇数（= Σ 逐 prim L0 前缀长度）
};

/**
 * 产物 prim 表与模型 primitive 的**身份对账**（顺序漂移的探测器）。
 *
 * 两侧都是 tinygltf 的 glb material 下标：写侧 `glb_source.cpp` 取 `prim.material`
 * （无材质 = 0xFFFFFFFF），读侧 `gltf_loader` 取 `GltfPrimitive::materialIndex`
 * （无材质 = -1），且**两侧都是 `meshes[].primitives[]` 展平序**（写侧注释明写
 * 「枚举序 = meshes[].primitives[] 展平，不是节点序」）。区间表按 prim 下标索引，
 * 一旦两侧枚举序分家，画出来的就是「A 的簇配 B 的材质」——**图像静默错、不报错**。
 * 数数（primCount == primitive 数）挡不住「同数不同序」，这条能。
 * @return nullopt = 对得上；有值 = 可直接打进 WARNING 的不符描述
 */
std::optional<std::string> primIdentityMismatch(const MeshletAsset& product,
                                                const UploadedModel& model) {
	for (size_t p = 0; p < product.prims.size(); ++p) {
		const uint32_t packed = product.prims[p].material;
		const int32_t loaded = model.model->primitives[p].materialIndex;
		const uint32_t expected = loaded < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(loaded);
		if (packed != expected)
			return fmt::format("prim {} 的 material 身份不符（产物 {} vs 模型 {}）—— "
			                   "产物与模型的 primitive 枚举序分家",
				p, packed, loaded);
	}
	return std::nullopt;
}

/**
 * 单模型的簇区间扫描 + 两道对账（任何一道不过 → WARNING 出声 + 返回 nullopt 回落）。
 *
 * 三种失败形态各有**不同**的 WARNING（读日志即可归因）：① 四条产物不变量被违反（错误串来自
 * `scanClusterRanges`，自带 `invariant N` 前缀）；② prim 数与模型 primitive 数不符；
 * ③ prim 的 material 身份对不上（同数不同序）。
 * @param um 已上传模型（`assetIds` 与 prim 下标对齐、`model->primitives` 是身份对账的另一侧）
 * @param product 该模型路径旁的簇产物（★0-b2 已加载并校验过字节流）
 * @return 逐 prim 区间 + 统计锚；失败 = nullopt（本函数已打过 WARNING，调用点不再出声）
 */
std::optional<MeshletRangeScan> scanOneModel(const UploadedModel& um,
                                             const MeshletAsset& product) {
	auto* logger = RLLogger::getInstance();
	auto scanned = scanClusterRanges(product);
	if (!scanned) {
		if (logger)
			logger->warn("meshlet: 簇区间扫描失败 {}：{} —— 该模型回落现行路径（不写簇区间表）",
				um.file, scanned.error());
		return std::nullopt;
	}
	if (scanned->perPrim.size() != um.assetIds.size()) {
		if (logger)
			logger->warn("meshlet: 簇区间表与模型 primitive 数不符 {}：产物 {} prim vs 模型 "
			             "{} primitive —— 回落现行路径",
				um.file, scanned->perPrim.size(), um.assetIds.size());
		return std::nullopt;
	}
	if (auto mismatch = primIdentityMismatch(product, um); mismatch) {
		if (logger)
			logger->warn("meshlet: {} —— 回落现行路径", mismatch.value());
		return std::nullopt;
	}
	return std::move(*scanned);
}

/**
 * 逐模型扫簇区间，收「mesh 资产 id → 区间」查表 + 三个统计锚（口径与回落策略 → scanOneModel）。
 *
 * @param scans 稳定存储（std::map 节点不搬家 ⟹ 区间指针在装配后仍然有效）
 * @param rangeOfAsset 输出：mesh 资产 id → 该 prim 的区间（下一步按 MeshRef 落到 objectSlot）
 * @param totals 输出：三个统计锚的合计（只累加**通过**的模型）
 */
void scanModelRanges(const UploadedAssets& assets, const MeshletAssetStore& meshlets,
                     std::map<std::string, MeshletRangeScan>& scans,
                     std::unordered_map<uint32_t, const MeshletPrimRange*>& rangeOfAsset,
                     RangeTotals& totals) {
	auto* logger = RLLogger::getInstance();
	for (const auto& um : assets.models) {
		auto product = meshlets.byFile.find(um.file);
		if (product == meshlets.byFile.end())
			continue;  // 该模型旁没有簇产物（门开但缺产物 = 加载期已打过 INFO，此处不重复）
		auto scanned = scanOneModel(um, product->second);
		if (!scanned)
			continue;
		const auto [stored, inserted] = scans.emplace(um.file, std::move(*scanned));
		(void)inserted;  // 每个 file 只出现一次（assets.models 已按 file 去重）
		for (size_t p = 0; p < stored->second.perPrim.size(); ++p)
			rangeOfAsset.emplace(um.assetIds[p], &stored->second.perPrim[p]);
		totals.primRanges += stored->second.perPrim.size();
		totals.clusters += stored->second.totalClusters;
		totals.l0Clusters += stored->second.totalL0Clusters;
		if (logger)
			logger->info("meshlet: 簇区间扫描 {} ← prims={} clusters={} l0Clusters={}",
				um.file, stored->second.perPrim.size(), stored->second.totalClusters,
				stored->second.totalL0Clusters);
	}
}

}  // namespace

/**
 * 接口声明与完整口径 → main.cppm `wireClusterRangeTable`。本定义只做三步：
 * 门早退 → 逐模型扫描（scanModelRanges）→ 按 `MeshRef{asset → objectSlot}` 落进
 * registry ctx 的 `ClusterRangeTable`（下标 = objectSlot = shader 的 objectIndex）。
 *
 * 落表按 **mesh 资产 id** 反查（不是「注册序 + prim 序」）：同文件多实例各占一个 objectSlot、
 * 共享同一份区间，资产 id 反查天然实例无关，也不依赖 registerScene 的分配序。
 */
void wireClusterRangeTable(entt::registry& registry, const UploadedAssets& assets,
                           const MeshletAssetStore& meshlets, bool meshletEnable) {
	if (!meshletEnable)
		return;  // 门关 = 一行都不走（逐位不变构造性成立；表不建 ⟹ scene data 该区全 0）
	std::map<std::string, MeshletRangeScan> scans;
	std::unordered_map<uint32_t, const MeshletPrimRange*> rangeOfAsset;
	RangeTotals totals;
	scanModelRanges(assets, meshlets, scans, rangeOfAsset, totals);
	if (rangeOfAsset.empty())
		return;  // 没有任何模型通过扫描/对账：ctx 不建条目 ⟹ 执行器把该区写成全 0

	const uint32_t slotCount = registry.ctx().get<NextObjectSlot>().next;
	auto& table = registry.ctx().emplace<ClusterRangeTable>();
	table.bySlot.resize(slotCount);  // value-init = 全 0（无簇产物的实例留 count=0 条目）
	uint32_t filled = 0;
	registry.view<MeshRef>().each([&](const MeshRef& meshRef) {
		auto hit = rangeOfAsset.find(meshRef.asset);
		if (hit == rangeOfAsset.end() || meshRef.objectSlot >= table.bySlot.size())
			return;
		table.bySlot[meshRef.objectSlot] = ObjectClusterRange{
			.base = hit->second->base,
			.count = hit->second->count,
			.l0Count = hit->second->l0Count,
			.reserved = 0,
		};
		++filled;
	});
	if (auto* logger = RLLogger::getInstance())
		logger->info("meshlet: ObjectClusterRange 表已装配（scene data 尾区 @722400，本批零消费者）"
					 "—— prims={} clusters={} l0Clusters={} / objectSlot 填充 {}/{}",
			totals.primRanges, totals.clusters, totals.l0Clusters, filled, slotCount);
}

}  // namespace RGL
