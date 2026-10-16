module;
#include <entt/entt.hpp>
#include <fmt/format.h>

// 入口模块 main 的分区实现单元（module main:meshlet_tables;）——簇化轴装配层的**建表**半区。
// 为什么从 main:meshlets 拆出（★18-B2）：阴影远级联 LOD 的选级表必须与簇区间表**同一份扫描、
// 同一个 MeshRef 落表循环**建出（LOD start 是相对区间表 base 的偏移，一致性是构造出来的，
// 不是两处各算再对账），而 main_meshlets.cppm 已在 500 行代码红线。职责边界：
//   本单元 = ★1-a1i 簇区间表（ClusterRangeTable）+ ★18-B2 阴影 LOD 选级表（MeshletShadowLodTable）
//            + 设备发射规格核对（两条簇路径共用的闸门）；
//   main:meshlets = 簇 buffer 描述符 / 管线条目烘焙 / switch 原位取代 / cull 生效。
module main:meshlet_tables;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import rlvk;      // vk::raii::PhysicalDevice / MeshShaderPropertiesEXT（设备发射规格查询）
import vk_device;     // DeviceContext（只读物理设备属性）
import scene_context;  // MeshRef（asset / objectSlot）+ NextObjectSlot（实例矩阵槽计数）
import meshlet_asset;  // MeshletAsset（只读视图：prims 身份字段）
import meshlet_ranges;  // scanClusterRanges / MeshletRangeScan / ClusterRangeTable / MeshletCullItemTable
import meshlet_lod;  // scanMeshletLod / MeshletLodScan / MeshletShadowLodTable（★18-B2）
import meshlet_cull_types_generated;  // kSlotsPerCluster / kTaskTris / kMeshletMax*（规格常量，反射生成）
import meshlet_hand;  // traverseClusterHandedness（★0.5-c 真产物统计锚：UV 手性 CPU 参考）
import scene_forward_types_generated;  // ObjectClusterRange（scene data 表条目，反射生成）
import asset_table;    // AssetTable（auditShadowLod 读 shadowPipelineIndex / shadowMeshletPipelineIndex / doubleSided）
import runtime_config;  // RuntimeConfig（meshletShadowLod / meshletShadowLodFallback / meshletCull）
import meshlet_cull_service;  // MeshletCullService（★1-a2-i 剔除项就地上传）
import gltf_loader;    // GltfModel/GltfPrimitive（逐 primitive 风格派生 + 身份对账）

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
 * 真产物统计锚（★0.5-c 判据 4）：UV 手性 CPU 参考对全表跑 L0，INFO 一条。
 *
 * 两个数**互为探测器**：
 * · `total == header.l0TriangleCount` —— 遍历恰好覆盖 L0、不重不漏（寻址口径 + L0 判据对）；
 * · `negative` 对账 packer 在**源 glb** 上独立量的逐三角读数（bistro = 2,322,319 / 82.00%）——
 *   两个同时对上 = 「寻址 + UV 忠实（池内 uv 是源字节拷贝）+ 公式与 loader 同式」三者同时成立。
 * 任一不符打 WARNING 不出声 = 静默放过；本路径是诊断（门开才走），失败不影响现行渲染。
 */
void reportHandAnchor(const MeshletAsset& product) {
	auto* logger = RLLogger::getInstance();
	auto hand = traverseClusterHandedness(product, 0, product.header.clusterCount,
		MeshletHandFilter::kL0Only, false);
	if (!hand) {
		if (logger)
			logger->warn("meshlet: UV 手性 CPU 参考遍历失败：{} —— 不影响现行路径", hand.error());
		return;
	}
	if (hand->total != product.header.l0TriangleCount) {
		if (logger)
			logger->warn("meshlet: UV 手性遍历未恰好覆盖 L0：total={} vs header.l0TriangleCount={} "
			             "—— 不重不漏判据失效",
				hand->total, product.header.l0TriangleCount);
		return;
	}
	const double pct = hand->total != 0
	                       ? 100.0 * static_cast<double>(hand->negative) /
	                             static_cast<double>(hand->total)
	                       : 0.0;
	if (logger)
		logger->info("meshlet: UV 手性 CPU 参考（L0）total={} negative={} ({:.2f}% negative) "
		             "l0TriangleCount={}",
			hand->total, hand->negative, pct, product.header.l0TriangleCount);
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
		reportHandAnchor(product->second);  // ★0.5-c 统计锚（total/negative 判据 → 函数注释）
	}
}

/**
 * 配置枚举 → 选级模块枚举（★18-B2；映射住在装配层的**唯一一处**——配置层不 import
 * meshlet_lod 的契约 → MeshletLodFallbackMode 注释；显式 switch，加第三档编译期即点名）。
 */
MeshletLodFallback toLodFallback(MeshletLodFallbackMode mode) {
	switch (mode) {
	case MeshletLodFallbackMode::l0:
		return MeshletLodFallback::l0;
	case MeshletLodFallbackMode::deepest:
		break;
	}
	return MeshletLodFallback::deepest;
}

/** 逐模型选级扫描的结果（★18-B2）：asset→选级 查表 + 打日志/对账要的聚合量（模型视图）。 */
struct ShadowLodScans {
	/** 稳定存储（std::map 节点不搬家 ⟹ perPrim 指针在装配后仍然有效，同 scans 的做法）。 */
	std::map<std::string, MeshletLodScan> byFile;
	/** mesh 资产 id → 该 prim 的选级（与 rangeOfAsset 同一条 asset 反查路径，实例无关）。 */
	std::unordered_map<uint32_t, const MeshletLodPrimRange*> lodOfAsset;
	uint64_t drawnClusters = 0;  ///< Σ scan.drawnClusters（模型视图；单实例场景 == 实例视图）
	uint64_t l0Clusters = 0;     ///< Σ scan.l0Clusters（对账锚：bistro = 27,487）
	uint64_t l0Triangles = 0;    ///< Σ scan.l0Triangles（对账锚：bistro = 2,832,120）
	uint64_t fallbackPrims = 0;  ///< Σ scan.fallbackPrimCount（D 超自身最深级而回落的 prim 数）
};

/** 落表循环的实例视图聚合量（★18-B2；与模型视图 ShadowLodScans 对账 = §18.6 判据 6）。 */
struct ShadowLodFill {
	uint32_t filled = 0;       ///< 落了非零条目的 objectSlot 数
	uint64_t sumClusters = 0;  ///< Σ bySlot.count（实例视图；Σtask = kSlotsPerCluster × 本值）
	uint32_t maxClusters = 0;  ///< max bySlot.count（单 draw task 上限对账用）
};

/**
 * 把 §18.2 成本表打成常驻日志（★18-B2；§18.6 判据 6：一次性读数变启动输出，真产物锚
 * D=5 deepest = 2,566 簇 / 144,448 三角 / 5.100%）。独立成函数只为把嵌套层级从
 * scanShadowLodModels 里挪出去（认知复杂度红线），不做缓存、不产生副作用。
 */
void reportShadowLodCostTable(const std::string& file, const MeshletLodScan& scan,
	uint32_t levelD, const RuntimeConfig& cfg) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	logger->info("meshlet: 阴影 LOD 成本表（★18-B2，§18.2 常驻机器版）← {} D={} fallback={} "
				 "maxLevelInProduct={} l0Clusters={} l0Triangles={}",
		file, levelD,
		cfg.meshletShadowLodFallback == MeshletLodFallbackMode::l0 ? "l0" : "deepest",
		scan.maxLevelInProduct, scan.l0Clusters, scan.l0Triangles);
	for (const auto& row : scan.costTable) {
		const double pct = scan.l0Triangles != 0
		                       ? 100.0 * static_cast<double>(row.triangles)
		                             / static_cast<double>(scan.l0Triangles)
		                       : 0.0;
		logger->info("meshlet:   D={} clusters={} triangles={} 占L0={:.3f}%{}", row.level,
			row.clusters, row.triangles, pct, row.level == levelD ? "  ← 当前生效" : "");
	}
}

/**
 * 逐模型跑 scanMeshletLod（★18-B2 步骤③）——与 scanModelRanges **同一份** MeshletRangeScan
 * 输入（一致性构造性成立），成本表逐 D 常驻输出见 reportShadowLodCostTable。
 * 任一模型选级扫描失败 = WARNING + 返回 false（**整表不建** = 阴影全走旧路径；
 * 「半切」挡在生效之前，同 auditMeshletSwitch 纪律）。
 */
bool scanShadowLodModels(const UploadedAssets& assets, const MeshletAssetStore& meshlets,
	const std::map<std::string, MeshletRangeScan>& scans, const RuntimeConfig& cfg,
	ShadowLodScans& out) {
	auto* logger = RLLogger::getInstance();
	const uint32_t levelD = static_cast<uint32_t>(cfg.meshletShadowLod);
	const MeshletLodFallback fallback = toLodFallback(cfg.meshletShadowLodFallback);
	for (const auto& um : assets.models) {
		auto product = meshlets.byFile.find(um.file);
		if (product == meshlets.byFile.end())
			continue;  // 该模型旁没有簇产物（区间扫描同样跳过，不重复出声）
		auto range = scans.find(um.file);
		if (range == scans.end())
			continue;  // 该模型在区间扫描阶段已回落（scanOneModel 已打过 WARNING）
		auto lod = scanMeshletLod(product->second, range->second, levelD, fallback);
		if (!lod) {
			if (logger)
				logger->warn("meshlet: 阴影 LOD 选级扫描失败 {}：{} —— 阴影 LOD 停用（c1..c3 保持旧路径）",
					um.file, lod.error());
			return false;
		}
		const auto [stored, inserted] = out.byFile.emplace(um.file, std::move(*lod));
		(void)inserted;  // 每个 file 只出现一次（assets.models 已按 file 去重）
		for (size_t p = 0; p < stored->second.perPrim.size(); ++p)
			out.lodOfAsset.emplace(um.assetIds[p], &stored->second.perPrim[p]);
		out.drawnClusters += stored->second.drawnClusters;
		out.l0Clusters += stored->second.l0Clusters;
		out.l0Triangles += stored->second.l0Triangles;
		out.fallbackPrims += stored->second.fallbackPrimCount;
		reportShadowLodCostTable(um.file, stored->second, levelD, cfg);
	}
	return true;
}

/** 阴影 LOD 逐资产对账的四个计数（口径 → auditShadowLod）。 */
struct ShadowLodCoverage {
	uint32_t gated = 0;        ///< 会切的（旧路径出阴影 draw 且四条判据全满足）
	uint32_t doubleSided = 0;  ///< 因 doubleSided 不切的（产物无背面副本，§16.2）
	uint32_t noProduct = 0;    ///< 因无簇产物不切的（shadowMeshletPipelineIndex = kInvalid）
	/// 🔴 有 shadowMeshletPipelineIndex 却 count == 0 / 表缺位 / 槽越界 = 烘焙侧与表侧分家
	uint32_t mismatch = 0;
};

/**
 * 阴影 LOD 的**逐资产对账**（★18-B2 步骤④，形态 = auditMeshletSwitch：把「半切」挡在生效
 * 之前，而不是靠人读日志）。对账范围只含「旧路径真的会出阴影 draw」的图元——守卫取
 * collectShadowCasters 的前两条（shadowPipelineIndex 有效 + 非 blend）。第四个计数非 0 =
 * 烘焙侧（bakeShadowMeshletEntries 按产物逐 prim 写回）与表侧（scanMeshletLod）分家 ⟹
 * 那几条 draw 静默走旧管线，画面可能逐位不变而「LOD 生效」的结论是假的。
 * 🔴 doubleSided 判据与 collectShadowCasters / makeOpaqueCall / auditMeshletSwitch **同一条、
 * 四处必须同步改**（§16.2：产物里没有 loader 复制的背面副本）。
 */
ShadowLodCoverage auditShadowLod(entt::registry& registry, const AssetTable& assets,
	const MeshletShadowLodTable* table) {
	ShadowLodCoverage cov;
	registry.view<MeshRef, MaterialRef>().each([&](const MeshRef& meshRef,
		const MaterialRef& matRef) {
		const auto& asset = assets.get(meshRef.asset);
		const auto& mat = assets.getMaterial(matRef.material);
		if (asset.shadowPipelineIndex == kInvalidPipelineIndex || mat.alphaModeBlend)
			return;  // 旧路径本就不出阴影 draw（同 collectShadowCasters 守卫，算进 mismatch = 误杀）
		if (asset.shadowMeshletPipelineIndex == kInvalidPipelineIndex) {
			++cov.noProduct;
			return;  // 无簇产物 / 本档关 / 多产物回落 = 不切
		}
		if (mat.doubleSided) {
			++cov.doubleSided;
			return;  // 🔴 正确性闸门（与 collectShadowCasters 同一条判据）
		}
		const bool switchable = table != nullptr && meshRef.objectSlot < table->bySlot.size()
		                        && table->bySlot[meshRef.objectSlot].count > 0;
		if (switchable)
			++cov.gated;
		else
			++cov.mismatch;
	});
	return cov;
}

/** 单实例的选级条目落表（★18-B2；与 ClusterRangeTable 同一个 MeshRef 循环调用，一致性构造性成立）。 */
void fillShadowLodSlot(const ShadowLodScans& scans, const MeshRef& meshRef,
	MeshletShadowLodTable& table, ShadowLodFill& fill) {
	auto lod = scans.lodOfAsset.find(meshRef.asset);
	if (lod == scans.lodOfAsset.end())
		return;  // 该资产无选级条目（无产物 / 扫描回落）= 全 0 条目 = 不切（合法形态，不是错误）
	auto& e = table.bySlot[meshRef.objectSlot];
	e = MeshletShadowLodTable::Entry{lod->second->start, lod->second->count};
	fill.sumClusters += e.count;
	fill.maxClusters = std::max(fill.maxClusters, e.count);
	if (e.count > 0)
		++fill.filled;
}

/**
 * 阴影 LOD 表生效（★18-B2 步骤④⑤）：逐资产对账 → Σtask 机器对账 → 单 draw task 上限 →
 * emplace + INFO。**对账失败 / task 上限超 / 全零表 = 不 emplace 并出声**（阴影全走旧路径，
 * 与门关逐位相同——「存在即生效」的构造性保证，不靠「建了但没人用」）。
 */
void activateShadowLodTable(entt::registry& registry, MeshletShadowLodTable&& table,
	const ShadowLodScans& scans, const ShadowLodFill& fill, const AssetTable& assetTable,
	const RuntimeConfig& cfg, const DeviceContext& device) {
	auto* logger = RLLogger::getInstance();
	const auto cov = auditShadowLod(registry, assetTable, &table);
	if (cov.mismatch != 0) {
		if (logger)
			logger->warn("meshlet: 阴影 LOD 停用 —— {}/{} 个阴影图元有阴影簇管线条目却选级簇数 = 0（选级表缺位 "
						 "/ 槽越界）：切了会退化成「半切」（那几条 draw 静默走旧管线），画面可能逐位不变而结论是"
						 "假的 —— 本跑保持旧阴影路径",
				cov.mismatch, cov.gated + cov.mismatch);
		return;
	}
	// Σtask == 2 × Σ簇数 的机器对账（§18.6 判据 6）：实例视图（落表循环）vs 扫描视图（逐模型
	// 合计）。×2 取反射规格常量（与 meshletTaskCount 同源），不写字面量。
	const uint64_t sumTasks = static_cast<uint64_t>(kSlotsPerCluster) * fill.sumClusters;
	if (fill.sumClusters != scans.drawnClusters && logger)
		logger->warn("meshlet: 阴影 LOD Σ簇数对账不符 —— 实例视图 {} vs 扫描视图 {}（Σtask = {} = {} × 实例视图）"
					 "—— 落表循环与扫描分家（漏落 / 重落；场景若有 multi-instance，差值属预期，先核实例数）",
			fill.sumClusters, scans.drawnClusters, sumTasks, kSlotsPerCluster);
	// 单 draw task 上限（§18.6 判据 6：per-object 最大 task < 派发上限）。超了 = **停用**而不是
	// 降级：drawMeshTasksEXT 的 X 越 maxMeshWorkGroupCount.x 不是「少画」，是无效调用/UB。
	const uint64_t maxTasks = static_cast<uint64_t>(kSlotsPerCluster) * fill.maxClusters;
	const uint32_t maxGroupsX = checkMeshletDeviceFit(device.getPhysicalDevice()).maxMeshWorkGroupCountX;
	if (maxTasks > maxGroupsX) {
		if (logger)
			logger->warn("meshlet: 阴影 LOD 停用 —— 单 draw 最大簇数 {} × {} = task {} 超设备派发上限 "
						 "maxMeshWorkGroupCount.x = {} —— 本跑保持旧阴影路径",
				fill.maxClusters, kSlotsPerCluster, maxTasks, maxGroupsX);
		return;
	}
	if (fill.filled == 0) {
		// 门开了、产物也在，却零槽可切 = 本档与门关逐位相同（静默空开关是本项目反复踩的形态）。
		if (logger)
			logger->warn("meshlet: 阴影 LOD 生效但**零槽可切**（选级表全零）—— 本档与门关逐位相同，"
						 "阳性对照必须另外跑");
		return;
	}
	registry.ctx().emplace<MeshletShadowLodTable>(std::move(table));
	if (logger)
		logger->info("meshlet: 阴影远级联 LOD 生效（★18-B2，消费者 = shadow_meshlet.mesh.slang，选级偏移走 "
					 "push，零 scene data 变更）—— 🔴 **c0 恒旧路径（零改动）**，c1..c3 切簇路径：每 object 画 "
					 "min(D={}, 自身最深级) 连续子区间；可切图元 {} doubleSided 不切 {} 无簇产物 {}"
					 "（blend 图元不投影，不计入）",
			cfg.meshletShadowLod, cov.gated, cov.doubleSided, cov.noProduct);
	if (logger)
		logger->info("meshlet: 阴影 LOD 启动对账 —— Σ簇数(实例视图)={} Σ簇数(扫描视图)={} Σtask={} "
					 "(= {} × 实例视图，×2 唯一真相源 = meshletTaskCount)；单 draw 最大簇数={} 最大task={} "
					 "vs maxMeshWorkGroupCount.x={}；l0Clusters={} l0Triangles={}（锚 27487 / 2832120）；"
					 "回落 prims={}（D 超自身最深级）",
			fill.sumClusters, scans.drawnClusters, sumTasks, kSlotsPerCluster, fill.maxClusters,
			maxTasks, maxGroupsX, scans.l0Clusters, scans.l0Triangles, scans.fallbackPrims);
	if (logger && cov.gated == 0)
		logger->warn("meshlet: 阴影 LOD 生效但**零图元可切**（可切槽全部撞上 doubleSided？）—— 本档与门关"
					 "逐位相同，阳性对照必须另外跑");
}

}  // namespace

// 🔴 `MeshletDeviceFit` 的**定义在 main.cppm**（跨分区共享：main:meshlets 的 switch 闸门与
// 本文件的阴影 LOD 闸门都读它的字段）——本文件只定义下面两个函数，勿在此重复定义结构体。

/**
 * 查询设备是否吃得下簇路径的**发射规格**（★1-a2-i；★18-B2 抬出匿名 namespace =
 * switch 与阴影 LOD 两条装配路径共用，声明 → main.cppm）。
 *
 * ## 为什么必须有这条（schema 与 source_check 都管不到的那一半）
 * `kMeshletMaxOutputVertices = 256` 是**本机实测下限的钉值**，不是设备保证。此前引擎
 * **从不运行期查询** `maxMeshOutputVertices`（全仓只出现在注释里），而 `kTaskTris = 85` 是
 * 硬编码、CPU 侧 task 数也按它算 ⟹ 换台 `maxMeshOutputVertices < 255` 的机器，mesh shader
 * 就是**越界写顶点输出**：不报错、不触发 VVL，只画垃圾或崩。`meshlet_spec_checks` 只能验
 * 「schema 与 shader 数值自洽 + 关系成立」，验不了「这台设备到底给多少」。
 *
 * ## 判据（三条，全部取反射生成的规格常量）
 * 去索引发射（§13.2 路 C）下每任务发射 `3 × kTaskTris` 个顶点、`kTaskTris` 个三角形，
 * 且 `[numthreads(kMeshletMaxWorkGroupInvocations)]` 要 ≤ `maxMeshWorkGroupInvocations`
 * （★M-3T-b 起该规格常量 = 128 = 三机最小值，NVIDIA 的硬上限；本机 256 也照样放行）。
 *
 * @param physicalDevice 待查询的物理设备（只读属性，不碰 device 创建）
 * @return 是否可用 + 上限值（供调用方出声与 LOD task 上限对账）
 */
MeshletDeviceFit checkMeshletDeviceFit(const vk::raii::PhysicalDevice& physicalDevice) {
	const auto props = physicalDevice.getProperties2<vk::PhysicalDeviceProperties2,
		vk::PhysicalDeviceMeshShaderPropertiesEXT>();
	const auto& mesh = props.get<vk::PhysicalDeviceMeshShaderPropertiesEXT>();
	MeshletDeviceFit fit;
	// 直接取属性值：vulkan.hpp 的包装里这三个字段是 **uint32_t**（C 结构 `VkPhysicalDevice...`
	// 里是 int32_t，C++ 包装已归一）⟹ 无需负值钳位，写钳位反而会因类型推导冲突编不过
	fit.maxOutputVertices = mesh.maxMeshOutputVertices;
	fit.maxOutputPrimitives = mesh.maxMeshOutputPrimitives;
	fit.maxWorkGroupInvocations = mesh.maxMeshWorkGroupInvocations;
	fit.maxMeshWorkGroupCountX = mesh.maxMeshWorkGroupCount[0];
	// 🔴 顶点判据是 **3×**：去索引下每个三角形发射它自己的 3 个私有顶点
	fit.ok = 3u * kTaskTris <= fit.maxOutputVertices
		&& kTaskTris <= fit.maxOutputPrimitives
		&& kMeshletMaxWorkGroupInvocations <= fit.maxWorkGroupInvocations;
	return fit;
}

/**
 * 规格不够就 warn 并返回 false（`wireMeshletSwitch` 与 ★18-B2 阴影 LOD 共用的闸门体；
 * 抽出来是为了让调用方保持早返回的扁平形态——内联进来会让它的认知复杂度越过 metrics 红线）。
 *
 * 停用而不是降级：不是「少画一块」，是回到一条已验证可用的路径。静默按不成立的规格发射
 * 是本项目最不能接受的失效形态（不报错、不触发 VVL，只在画面上留下解释不了的垃圾）。
 *
 * @param device 设备上下文（只读物理设备属性）
 * @return 设备吃得下 = true；false = 调用方必须停用簇路径、保持旧路径
 */
bool warnIfMeshletDeviceUnfit(const DeviceContext& device) {
	const auto fit = checkMeshletDeviceFit(device.getPhysicalDevice());
	if (fit.ok)
		return true;
	if (auto* logger = RLLogger::getInstance())
		logger->warn("meshlet: 簇路径停用（switch 与阴影 LOD 共用本闸门）—— 设备发射规格不够（maxMeshOutputVertices={} "
					 "需 ≥ 3×kTaskTris={}；maxMeshOutputPrimitives={} 需 ≥ {}；"
					 "maxMeshWorkGroupInvocations={} 需 ≥ {}）：去索引发射会越界写顶点输出，"
					 "本跑保持旧路径",
			fit.maxOutputVertices, 3u * kTaskTris, fit.maxOutputPrimitives, kTaskTris,
			fit.maxWorkGroupInvocations, kMeshletMaxWorkGroupInvocations);
	return false;
}

/**
 * 接口声明与完整口径 → main.cppm `wireClusterRangeTable`。本定义做五步：
 * 门早退 → 逐模型扫描（scanModelRanges）→ ★18-B2 阴影 LOD 前置（设备规格闸门 + 同一份扫描
 * 逐模型选级）→ 按 `MeshRef{asset → objectSlot}` **同一个循环**落两张表
 * （`ClusterRangeTable` + 局部的 `MeshletShadowLodTable`）→ ★1-a2-i 剔除项表 → LOD 对账生效。
 *
 * 落表按 **mesh 资产 id** 反查（不是「注册序 + prim 序」）：同文件多实例各占一个 objectSlot、
 * 共享同一份区间，资产 id 反查天然实例无关，也不依赖 registerScene 的分配序。
 * 🔴 两张表同一个 MeshRef 循环、同一份扫描派生（LOD 的 start 是相对区间表 base 的偏移，
 * 一致性是构造出来的，不是两处各算再对账）；LOD 表**对账通过才 emplace**（存在即生效）。
 */
void wireClusterRangeTable(entt::registry& registry, const UploadedAssets& assets,
                           const MeshletAssetStore& meshlets, bool meshletEnable, bool meshletCull,
                           MeshletCullService* cullService, const AssetTable& assetTable,
                           const RuntimeConfig& cfg, const DeviceContext& device) {
	if (!meshletEnable)
		return;  // 门关 = 一行都不走（逐位不变构造性成立；表不建 ⟹ scene data 该区全 0）。
		         // 「shadow_lod ≥ 0 而 enable 关」的孤立组合自债-106 起由 gate_lint 启动期申报
		         // （meshlet.shadow_lod 规则）——原就近 warnIfShadowLodOrphaned 的判据是纯配置
		         // 组合，同一根因不许多处各喊一条（影子真相源），故随迁移删除。
	std::map<std::string, MeshletRangeScan> scans;
	std::unordered_map<uint32_t, const MeshletPrimRange*> rangeOfAsset;
	RangeTotals totals;
	scanModelRanges(assets, meshlets, scans, rangeOfAsset, totals);
	if (rangeOfAsset.empty())
		return;  // 没有任何模型通过扫描/对账：ctx 不建条目 ⟹ 执行器把该区写成全 0

	// ★18-B2 阴影远级联 LOD 前置（步骤①门关 = 零 ctx 零日志；步骤②设备发射规格——理由同
	// β-1：去索引发射会越界写顶点输出；步骤③同一份扫描逐模型选级 + 成本表常驻日志）。
	// lodAlive 为假只关 LOD 这一张表，ClusterRangeTable 照建（β-1 自己的设备闸门在
	// wireMeshletSwitch，两条路径互不连坐）。
	ShadowLodScans lodScans;
	bool lodAlive = cfg.meshletShadowLod >= 0;
	if (lodAlive && !warnIfMeshletDeviceUnfit(device))
		lodAlive = false;  // 已出声（保持旧路径）
	if (lodAlive)
		lodAlive = scanShadowLodModels(assets, meshlets, scans, cfg, lodScans);  // 失败已出声

	const uint32_t slotCount = registry.ctx().get<NextObjectSlot>().next;
	auto& table = registry.ctx().emplace<ClusterRangeTable>();
	table.bySlot.resize(slotCount);  // value-init = 全 0（无簇产物的实例留 count=0 条目）
	// 🔴 LOD 表先留**局部**，逐资产对账通过才 emplace（「存在即生效」→ MeshletShadowLodTable
	// 注释）；与区间表同一个落表循环、同一条 asset 反查路径。
	MeshletShadowLodTable lodTable;
	if (lodAlive)
		lodTable.bySlot.resize(slotCount);
	uint32_t filled = 0;
	ShadowLodFill lodFill;
	registry.view<MeshRef>().each([&](const MeshRef& meshRef) {
		auto hit = rangeOfAsset.find(meshRef.asset);
		if (hit == rangeOfAsset.end() || meshRef.objectSlot >= table.bySlot.size())
			return;
		table.bySlot[meshRef.objectSlot] = ObjectClusterRange{
			.base = hit->second->base,
			.count = hit->second->count,
			.l0Count = hit->second->l0Count,
			.cullBase = 0,  // ★1-a2-i：门开时由 buildClusterCullItems 就地改写，门关恒 0
		};
		++filled;
		if (lodAlive)
			fillShadowLodSlot(lodScans, meshRef, lodTable, lodFill);
	});
	// ★1-a2-i 逐簇剔除：建剔除项表 + 就地写 cullBase。门 `meshlet.cull` 关 = **根本不调**，
	// 于是 scene data 该区与 β-1 逐位同（cullBase 保持 0，mesh shader 也不读标志）——
	// 门关逐位不变的构造性保证，不靠"读了但结果一样"。
	if (meshletCull) {
		auto& items = registry.ctx().emplace<MeshletCullItemTable>();
		items = buildClusterCullItems(table);
		// 就地上传给 GPU 侧（cullBase 与 items 同生共死，见 main.cppm 声明）。服务缺位 =
		// 多产物回落（`wireMeshletBindings` 没建服务）⟹ 没有簇管线条目、mesh shader 根本不跑，
		// 表白扫但无害；出声是为了让「配置说开了而实际没跑」在日志里看得见。
		if (cullService)
			cullService->uploadItems(items.items);
		else if (auto* logger = RLLogger::getInstance())
			logger->warn("meshlet: meshlet.cull 开但剔除服务未装配（多产物回落 / 无产物）—— "
						 "逐簇剔除不运行，本跑只有 CPU 侧实例级剔除");
		if (auto* logger = RLLogger::getInstance())
			logger->info("meshlet: 逐簇剔除项表已装配（★1-a2-i，消费者 = meshlet_cull.comp.slang）"
						 "—— cullItems={}（应 == l0Clusters={}）",
					items.items.size(), totals.l0Clusters);
	}
	if (auto* logger = RLLogger::getInstance())
		logger->info("meshlet: ObjectClusterRange 表已装配（scene data 尾区 @724224，消费者 = "
					 "meshlet.mesh.slang）—— prims={} clusters={} l0Clusters={} / objectSlot 填充 {}/{}",
			totals.primRanges, totals.clusters, totals.l0Clusters, filled, slotCount);
	// ★18-B2 步骤④⑤：逐资产对账 → Σtask 机器对账 → 单 draw task 上限 → emplace + INFO
	//（任一不过 = 不 emplace = 阴影全走旧路径，与门关逐位相同）。
	if (lodAlive)
		activateShadowLodTable(registry, std::move(lodTable), lodScans, lodFill, assetTable, cfg,
			device);
}

}  // namespace RGL
