module;
#include <entt/entt.hpp>
#include <fmt/format.h>

// 入口模块 main 的分区实现单元（module main:meshlets;）——簇化轴的**装配层接线**集中一处。
// 为什么单独成篇（★1-a1 挂号 ①，docs/mdi_plan.md §9）：main_scene.cppm 已 840 行，
// 「装配层接线别再往里塞」是上一批就记下的债；本单元只做簇轴接线——
// ★1-a1i：CPU 侧簇产物扫描结果落到 per-objectSlot 的表（供 scene data 尾区上传）；
// ★1-a1ii-α：簇 buffer 描述符装配（wireMeshletBindings）+ 影子叠加管线条目烘焙
// （bakeMeshletEntries）。
module main:meshlets;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import rlvk;      // vk:: DeviceAddressRangeEXT / writeResourceDescriptorsEXT 描述符装配
import vk_device;     // DeviceContext（描述符写入取 device 句柄）
import vk_resources;  // Resources（allocSlot / descriptor heap 槽地址）
import scene_context;  // MeshRef（asset / objectSlot）+ NextObjectSlot（实例矩阵槽计数）
import meshlet_asset;  // MeshletAsset（只读视图：prims 身份字段）
import meshlet_ranges;  // scanClusterRanges / MeshletRangeScan / ClusterRangeTable
import meshlet_hand;  // traverseClusterHandedness（★0.5-c 真产物统计锚：UV 手性 CPU 参考）
import meshlet_geometry;  // MeshletGeometryStore（4 段 GPU buffer 的 BDA，描述符内容源）
import scene_forward_types_generated;  // ObjectClusterRange（scene data 表条目，反射生成）
import scene_config;   // SceneConfig（resolvePrimitiveSpecName 入参）
import runtime_config;  // RuntimeConfig / MeshletCoplanarMode（★0.5-b2 wireMeshletOverlayDepthCompare 入参）
import vk_renderer;  // Renderer（★0.5-b2 setMeshletOverlayDepthCompareOp 目标；main 已 export import 链上，此处显式）
import scene_data;     // kShadowMapSize（阴影图边长，与模型条目同值）
import render_graph;   // ensureShadowMapSlot（幂等，按名缓存命中同槽）
import gltf_loader;    // GltfModel/GltfPrimitive（逐 primitive 风格派生）
import asset_table;    // AssetTable（纹理槽号 + bindMeshletToAsset 写回）
import shader_registry;  // ShaderRegistry（addMeshletEntry / bindMeshletToAsset）
import sampler_library;      // SamplerLibrary（共享/point sampler 幂等重取同槽）
import render_target_cache;  // RenderTargetCache（阴影图 RT 幂等重取同槽）

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
 * 逐段把簇 buffer 的 BDA 写成 heap 槽 storage buffer 描述符（ensureIzbSlots fallback
 * 同法：`writeResourceDescriptorsEXT` + `vk::DeviceAddressRangeEXT`，装配期 host 直写，
 * 不进命令流）。自 wireMeshletBindings 抽出（stylecheck 认知复杂度预算，纯搬移）。
 * 描述符 range = **allocBytes**（含 16B 补齐区）——meshlet.mesh.slang loadU8x3 的尾读
 * 钳制论证依赖「range 覆盖补齐」。
 * @return false = 某槽地址解析失败（已 WARNING，调用方回落 nullopt）
 */
bool writeMeshletSectionDescriptors(DeviceContext& device, Resources& resources,
	std::span<const MeshletGeometrySection* const> sections, std::span<const uint32_t> slots,
	std::span<const char* const> names) {
	auto* logger = RLLogger::getInstance();
	const vk::DeviceSize descSize = resources.getDescriptorHeap().getDescriptorSize();
	for (size_t i = 0; i < sections.size(); ++i) {
		const auto addr = resources.getDescriptorHeap().getSlotCpuAddress(slots[i]);
		if (!addr) {
			if (logger)
				logger->warn("meshlet: 槽 {} 地址解析失败（{}）：{} —— 叠加停用", slots[i],
					names[i], addr.error().describe());
			return false;
		}
		vk::DeviceAddressRangeEXT range{sections[i]->deviceAddress, sections[i]->allocBytes};
		vk::ResourceDescriptorInfoEXT desc{};
		desc.type = vk::DescriptorType::eStorageBuffer;
		desc.data.pAddressRange = &range;  // 同步 host 写，局部临时量存活到调用返回即够
		device.getDevice().writeResourceDescriptorsEXT(desc,
			vk::HostAddressRangeEXT{*addr, descSize});
	}
	return true;
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
		logger->info("meshlet: ObjectClusterRange 表已装配（scene data 尾区 @722400，消费者 = "
					 "meshlet.mesh.slang）—— prims={} clusters={} l0Clusters={} / objectSlot 填充 {}/{}",
			totals.primRanges, totals.clusters, totals.l0Clusters, filled, slotCount);
}

/**
 * 簇 buffer 描述符接线（★1-a1ii-α；对外经下方 wireMeshletOverlay 总入口调用，
 * 契约见 main.cppm 声明）。四步：门早退 → 单产物闸门 →
 * 领 4 个 Runtime 段槽 → 逐段写 storage buffer 描述符（ensureIzbSlots fallback 同法：
 * `writeResourceDescriptorsEXT` + `vk::DeviceAddressRangeEXT`，装配期 host 直写）。
 *
 * 描述符 range = 段的 **allocBytes**（含 16B 补齐区）：shader 侧 u8 三元组拼接的尾字
 * 读可能落进补齐区（meshlet.mesh.slang loadU8x3 的钳制论证依赖「range 覆盖补齐」）。
 */
std::optional<MeshletBindingSlots> wireMeshletBindings(Resources& resources,
	DeviceContext& device, const MeshletGeometryStore& meshletGeometry, bool meshletEnable) {
	auto* logger = RLLogger::getInstance();
	if (!meshletEnable)
		return std::nullopt;  // 门关 = 连号都不领（逐位不变构造性成立）
	if (meshletGeometry.byFile.empty())
		return std::nullopt;  // 门开但无产物 / 上传失败（各自已在上游 INFO/WARNING，不重复）
	if (meshletGeometry.byFile.size() != 1) {
		if (logger)
			logger->warn("meshlet: 本批叠加路径只支持**单份**簇产物，现有 {} 份 —— 叠加停用，"
			             "全部模型回落现行路径（多顶点池要 per-entry 顶点 binding，★1-b 面）",
				meshletGeometry.byFile.size());
		return std::nullopt;
	}
	const auto& [file, geom] = *meshletGeometry.byFile.begin();
	// binding 升序 = 字段序（17/18/19/20，addMeshletEntry 的 kClusterBindings 同源）
	const std::array<const MeshletGeometrySection*, 4> sections{
		&geom.vertexPool, &geom.clusters, &geom.tris, &geom.remap};
	static constexpr std::array<const char*, 4> names{"meshlet.gpu.vertexPool",
		"meshlet.gpu.clusters", "meshlet.gpu.tris", "meshlet.gpu.remap"};
	std::array<uint32_t, 4> slotNumbers{};
	for (size_t i = 0; i < sections.size(); ++i) {
		auto slot = resources.allocSlot(SlotSegment::Runtime, names[i]);
		if (!slot) {
			if (logger)
				logger->warn("meshlet: 簇 buffer 领槽失败（{}）：{} —— 叠加停用", names[i],
					slot.error().describe());
			return std::nullopt;
		}
		slotNumbers[i] = *slot;
	}
	if (!writeMeshletSectionDescriptors(device, resources, sections, slotNumbers, names))
		return std::nullopt;
	if (logger)
		logger->info("meshlet: 簇 buffer 描述符已装配 {} —— 槽 {}/{}/{}/{}（binding 17-20，"
					 "顶点池 {}B）",
			file, slotNumbers[0], slotNumbers[1], slotNumbers[2], slotNumbers[3],
			geom.vertexPool.allocBytes);
	return MeshletBindingSlots{.vertexPool = slotNumbers[0], .clusters = slotNumbers[1],
		.tris = slotNumbers[2], .remap = slotNumbers[3]};
}

/**
 * 影子叠加条目烘焙（★1-a1ii-α；对外经下方 wireMeshletOverlay 总入口调用，契约见
 * main.cppm 声明）。逐 primitive 走
 * resolvePrimitiveSpecName（与本体条目同一函数 = 同一 frag），条目去重住在
 * addMeshletEntry（按纹理集，§10.2）；只有**有簇产物的模型**（store 键命中）才建条目
 * ——其余资产 meshletPipelineIndex 保持 kInvalid = render_list 零叠加 draw。
 * @param variant mesh SPV 变体（base/debug/tint，★0.5-b2 起三档；优先级在调用点定）
 */
void bakeMeshletEntries(const UploadedAssets& uploaded, const SceneConfig& scene,
	bool deferredMode, ShaderRegistry& pipelines, AssetTable& assets, Resources& resources,
	SamplerLibrary& samplers, RenderTargetCache& rtCache,
	const MeshletGeometryStore& meshletGeometry, const std::optional<MeshletBindingSlots>& slots,
	MeshletMeshVariant variant) {
	if (!slots)
		return;  // 门关 / 无产物 / 多产物回落 = 一条条目都不建（逐位不变）
	// 材质侧三槽幂等重取 = 与模型条目同号（bakePipelineEntries 已领；binding 12/13/15
	// 无条件烘焙纪律同源——光照 frag 声明的 descriptor 缺 mapping 才是 VVL 错误）。
	const uint32_t samplerSlot = samplers.ensureShared();
	const auto shadowSlot = ensureShadowMapSlot(rtCache, kShadowMapSize);
	if (!shadowSlot)
		throw std::runtime_error("meshlet overlay: 阴影图 RT 创建失败: " + shadowSlot.error().describe());
	const uint32_t shadowSamplerSlot = samplers.ensurePointSampler();
	const auto evssmSlot = resources.ensureEvssmSampleSlot();
	if (!evssmSlot)
		throw std::runtime_error("meshlet overlay: EVSSM 矩图槽发号失败: " + evssmSlot.error().describe());
	const std::array<uint32_t, 4> clusterSlots{slots->vertexPool, slots->clusters, slots->tris,
		slots->remap};
	uint32_t baked = 0;
	for (const auto& um : uploaded.models) {
		if (!meshletGeometry.byFile.contains(um.file))
			continue;  // 该模型旁没有簇产物（或扫描/对账失败）= 无叠加条目
		for (size_t i = 0; i < um.assetIds.size(); ++i) {
			const uint32_t id = um.assetIds[i];
			const uint32_t entryIdx = pipelines.addMeshletEntry(
				resolvePrimitiveSpecName(scene, deferredMode, um.model->primitives[i]),
				assets.getTexture(um.textureIds.base[i]).heapSlot,
				assets.getTexture(um.textureIds.normal[i]).heapSlot,
				assets.getTexture(um.textureIds.orm[i]).heapSlot,
				assets.getTexture(um.textureIds.emissive[i]).heapSlot,
				samplerSlot, *shadowSlot, shadowSamplerSlot, *evssmSlot, clusterSlots,
				variant);
			pipelines.bindMeshletToAsset(id, entryIdx);
			++baked;
		}
	}
	if (auto* logger = RLLogger::getInstance())
		logger->info("meshlet: 叠加管线条目已装配 —— prims={}（按纹理集去重）mesh 变体={}"
					 "（0=base 1=debug 2=tint）",
			baked, static_cast<uint32_t>(variant));
}

/**
 * 簇化轴 ★1-a1ii-α 装配总入口（main.cppm 声明）：描述符接线 + 条目烘焙两步的顺序
 * 契约内聚在此（bake 依赖 wire 的槽号），main() 只留一次调用（装配层行数预算）。
 *
 * ★0.5-b2 起 mesh SPV 变体三档（base/debug/tint）：tint 与 debug 同真时 **tint 优先 +
 * warn**（判据 3 只用 tint、判据 4 只用 offset，从不同时用，故不建第 4 个组合变体；
 * warn 只在门开时出——门关 grep meshlet = 0 的护栏）。
 */
void wireMeshletOverlay(Resources& resources, DeviceContext& device,
	const MeshletGeometryStore& meshletGeometry, const UploadedAssets& uploaded,
	const SceneConfig& scene, bool deferredMode, ShaderRegistry& pipelines, AssetTable& assets,
	SamplerLibrary& samplers, RenderTargetCache& rtCache, bool meshletEnable, bool meshletDebug,
	bool meshletTint) {
	const auto slots = wireMeshletBindings(resources, device, meshletGeometry, meshletEnable);
	// mesh SPV 变体选档（门关 = bakeMeshletEntries 首行早退，variant 不被读；warn 也
	// 必须门开才出声——门关 grep meshlet = 0 的护栏）
	MeshletMeshVariant variant = MeshletMeshVariant::base;
	if (meshletTint) {
		variant = MeshletMeshVariant::tint;
		if (meshletEnable && meshletDebug && RLLogger::getInstance())
			RLLogger::getInstance()->warn(
				"meshlet: tint 与 debug 同真 ⟹ tint 优先（不建组合 SPV 变体；判据 3 只用 "
				"tint、判据 4 只用 debug，从不同时用）—— 本跑走 TINT 变体");
	} else if (meshletDebug)
		variant = MeshletMeshVariant::debugOffset;
	bakeMeshletEntries(uploaded, scene, deferredMode, pipelines, assets, resources, samplers,
		rtCache, meshletGeometry, slots, variant);
}

/**
 * 共面双臂深度比较接线（★0.5-b2，main.cppm 声明；调用序契约 = assembleBackEnd 之后）。
 * 三步：门关早退（零日志）→ coplanar 档映射为 optional<vk::CompareOp>（off = nullopt =
 * 执行器零 set）→ 交 setter + INFO 打印**生效**档（四臂假绿的第二道机器护栏）。
 */
void wireMeshletOverlayDepthCompare(Renderer& renderer, const RuntimeConfig& cfg) {
	if (!cfg.meshletEnable)
		return;  // 门关 = 零调用零日志（门关 grep meshlet = 0 护栏；setter 保持默认 nullopt）
	std::optional<vk::CompareOp> op;
	std::string_view modeName = "off";
	std::string_view opName = "eLess（pass 现行，执行器零 set）";
	switch (cfg.meshletCoplanar) {
	case MeshletCoplanarMode::off: break;
	case MeshletCoplanarMode::equal:
		op = vk::CompareOp::eEqual;
		modeName = "equal";
		opName = "eEqual";
		break;
	case MeshletCoplanarMode::ne:
		op = vk::CompareOp::eNotEqual;
		modeName = "ne";
		opName = "eNotEqual";
		break;
	}
	renderer.setMeshletOverlayDepthCompareOp(op);
	if (auto* logger = RLLogger::getInstance())
		logger->info("meshlet: coplanar 双臂生效档 = {}（叠加 draw depthCompare = {}；"
					 "只作用叠加 draw，pass 与旧路径状态不动）",
			modeName, opName);
}

}  // namespace RGL
