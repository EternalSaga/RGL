module;
#include <entt/entt.hpp>
#include <fmt/format.h>
#include <glm/glm.hpp>                  // glm::mat4（entityWorldScale 的入参类型）
#include <glm/gtc/matrix_transform.hpp>  // glm::perspective（texel 读数表的规范投影，同 projectionSystem）

// 入口模块 main 的分区实现单元（module main:shadow_proxy;）——N10 P2 阴影代理**钉段表**的装配半区。
// 为什么从 main:meshlet_tables 拆出：那张表本身已在 500 代码行红线（★18-B2 拆它时就是这个理由），
// 而本半区是「又一份逐产物扫描 + 又一个 MeshRef 反查落表」。职责边界：
//   main:meshlet_tables = 簇区间表（ClusterRangeTable）+ 阴影 LOD 选级表 + 设备发射规格闸门；
//   本单元 = 钉段表（ShadowProxyTable）+ texel 门读数表；
//   main:meshlets = 簇 buffer 描述符 / 管线条目烘焙 / switch 原位取代 / cull 生效。
// 🔴 与区间表**共用同一份** `MeshletRangeScan`（由调用方把 main:meshlet_tables 扫出的 scans 传进来）：
// invariant 1 的对照面必须就是区间表实际吃的那份，在这儿重扫 = 造第二个真相源。
module main:shadow_proxy;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import scene_context;  // MeshRef / Transform / NextObjectSlot / ViewProjection / quantizeShadowDistance
import meshlet_asset;  // MeshletAsset（mustL0 段视图）
import meshlet_ranges;  // MeshletRangeScan（invariant 1/2 的对照面）
import shadow_proxy_types;  // ShadowProxyTable / buildShadowProxyTable / pinActive / kShadowProxyNoPrim
import render_list;    // computeCascadeSpheres（texel 读数表与运行期同一套级联几何，N10 P2 抬成接缝）
import render_graph;   // shadowAtlasColumns / shadowTileSize（纹素分母 = 图集单块边长）
import scene_data;     // kActiveShadowCascades
import camera;         // Camera ctx（texel 读数表的 fov / 配置 near）
import ecs;            // RenderExtent ctx（texel 读数表的宽高比，与 projectionSystem 同源）

namespace RGL {
namespace {

/**
 * 8 个量化 far 档（计划 §3.2：far 的合法定义域 [0.5, 64] 上的 2 的幂）。
 *
 * `quantizeShadowDistance` 向上取到 2 的幂且**幂等**（恰为 2 的幂时取自身，见其注释）⟹ 任何
 * 落在该定义域内的 far 都必属于这 8 个值之一 —— 这就是「选级结果可以整张枚举」的全部依据，
 * 也是本表能当「跨档跳变的常驻机器版」而不是「某一帧的快照」的理由。
 */
constexpr std::array<float, 8> kShadowFarBands { 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f };

/**
 * 一个实体的世界换算（产物局部单位 → 米）。🔴 式子本体在 `scene_context` 的 `worldScaleOf`
 * （N10 P3 收口：运行期算候选世界包围半径那三行原本与本函数各写一遍）。
 *
 * 为什么取三列最大值而不是 X 列、以及它的保守方向 → 那份注释。
 * 🔴 只在启动期读（texel 读数表的输入）；运行期真值由 `renderListSystem` 从当帧
 * `spin.matrix * tf.world` 取（spin 是纯旋转，不改基向量长度 ⟹ 同一个数）。
 */
float entityWorldScale(const entt::registry& registry, entt::entity entity) {
	const auto* transform = registry.try_get<Transform>(entity);
	if (!transform)
		return 0.0f;  // 拿不到变换 = 不算门（pinActive 在 worldScale=0 下恒 false，偏保守）
	return worldScaleOf(transform->world);
}

/**
 * texel 门在「8 档 × 4 级联」下的活跃条目数表（§3.2 后果 3 的常驻机器版）。
 *
 * ## 三条口径（全部写进日志首行，读日志的人不必回来读代码）
 *  ① **级联几何与运行期同一个函数**：`computeCascadeSpheres`（N10 P2 为这件事抬成导出接缝）。
 *     纹素 = `2·球半径 / 单块边长`，与 `fillShadowData` 写进 scene data 的 `shadowTexelSizes`
 *     同式（也 == `buildShadowVolume` 内部做 texel 吸附用的那枚 —— 它把 `slice.radius` 原样
 *     放进 `ShadowVolume::xyRadius`，所以「运行期的 xyRadius」与「本函数的球半径」是同一个数）。
 *  ② **相机 = 配置机位**（Camera 的 fov + **渲染**网格的宽高比 + 配置 near/far）。运行期的
 *     near/far 是 `projectionSystem` 按 SceneBounds 现算的自适应值，而建表发生在首帧之前
 *     （那里还是空的）⟹ 本表是**量级与跨档跳变点**的读数，不是某一帧的真值。宽高比必须取
 *     `RenderExtent` 而不是窗口尺寸（projectionSystem 那条红线：按窗口比例算投影、按渲染网格
 *     铺像素 = 各向异性拉伸）。
 *  ③ **view = identity**：视锥 8 角共球、球半径只依赖 near/far/fov/aspect，与相机姿态无关
 *     （`frustumBoundingSphere` 闭式解那条注释 —— 那正是阴影不随视角游动的前提）。
 *
 * 判据本体不在这里（`pinActive`，rlbase 纯函数）；本函数只做「喂哪 32 枚 texel」。
 */
void logShadowProxyTexelGate(entt::registry& registry, const ShadowProxyTable& table,
                              const std::vector<float>& worldScaleOfSlot) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	const auto& camera = registry.ctx().get<Camera>();
	const auto& extent = registry.ctx().get<RenderExtent>();
	if (extent.width == 0 || extent.height == 0)
		return;  // 渲染网格未装配（理论到不了：wireRenderExtentContext 早于本路径）
	const float tile = static_cast<float>(shadowTileSize(shadowAtlasColumns(kActiveShadowCascades)));
	ViewProjection canonical;
	canonical.view = glm::mat4(1.0f);  // 口径 ③：球半径与姿态无关
	canonical.proj = glm::perspective(camera.fov,
	    static_cast<float>(extent.width) / static_cast<float>(extent.height), camera.nearPlane,
	    camera.farPlane);
	const float nearQ = quantizeShadowDistance(camera.nearPlane);
	logger->info("shadow proxy: texel 门读数表（§3.2 后果 3；判据 = 被钉段直径 2·radius·worldScale ≥ "
	             "本级纹素，K=1 写死无旋钮）—— 级联几何 = computeCascadeSpheres（与运行期同一函数），"
	             "纹素 = shadowTexelSize 接缝（与 fillShadowData 写进 scene data 的、P3 emit 判钉段用的"
	             "同一条式子）；相机 = 配置机位 "
	             "fov={:.4f}rad aspect={:.4f} 量化near={}m tile={}px；🔴 运行期 near/far 随 SceneBounds "
	             "自适应 ⟹ 本表是量级与跨档跳变点读数，不是某一帧的真值",
		camera.fov, static_cast<double>(extent.width) / static_cast<double>(extent.height), nearQ,
		tile);
	for (const float farBand : kShadowFarBands) {
		const auto spheres = computeCascadeSpheres(canonical, nearQ, farBand);
		std::string line = fmt::format("shadow proxy:   far 档 {:>6.2f} m ⟹ texel", farBand);
		std::string counts = "活跃 objectSlot";
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			const float texel = shadowTexelSize(spheres[c].radius);
			uint32_t active = 0;
			for (size_t s = 0; s < table.bySlot.size(); ++s)
				active += pinActive(table.bySlot[s], worldScaleOfSlot[s], texel) ? 1u : 0u;
			line += fmt::format(" c{}={:.5f}", c, texel);
			counts += fmt::format(" c{}={}", c, active);
		}
		logger->info("{} m ⟺ {}", line, counts);
	}
}

/** 步骤①②的产物：两套反查（口径 → 各自的注释与 wireShadowProxyTable 的函数头）。 */
struct ShadowProxyLookups {
	/** [模型下标][objectSlot] → 该模型的 prim 下标；空 vector = 该模型在本场景零实例。 */
	std::vector<std::vector<uint32_t>> primOfSlotOf;
	/** [objectSlot] → 世界换算（产物局部单位 → 米），texel 读数表的输入。 */
	std::vector<float> worldScaleOfSlot;
};

/**
 * 建两套反查：mesh 资产 id → (模型下标, prim 下标)，再按 `MeshRef` 落成逐模型的 slot → prim。
 *
 * 🔴 第二套的方向是 **slot → prim** 而不是 prim → slot：一个 prim 可以有多个实例（多 objectSlot），
 * 反过来不是函数；`MeshRef` 循环天然就是这个方向，也是建表纯函数入参的口径。
 * 与 `ClusterRangeTable` 同一条资产 id 反查路径（实例无关、不依赖 registerScene 的分配序），
 * 只是值从「区间指针」换成「prim 下标」。
 */
ShadowProxyLookups buildShadowProxyLookups(entt::registry& registry, const UploadedAssets& assets,
                                           uint32_t slotCount) {
	std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> locOfAsset;
	for (size_t m = 0; m < assets.models.size(); ++m)
		for (size_t p = 0; p < assets.models[m].assetIds.size(); ++p)
			locOfAsset.emplace(assets.models[m].assetIds[p],
			    std::make_pair(static_cast<uint32_t>(m), static_cast<uint32_t>(p)));

	ShadowProxyLookups lookups;
	lookups.primOfSlotOf.resize(assets.models.size());
	lookups.worldScaleOfSlot.assign(slotCount, 0.0f);
	registry.view<MeshRef>().each([&](entt::entity entity, const MeshRef& meshRef) {
		auto hit = locOfAsset.find(meshRef.asset);
		if (hit == locOfAsset.end() || meshRef.objectSlot >= slotCount)
			return;  // 无簇产物的实体（区间表那一步也没填它，此处同口径）
		auto& primOfSlot = lookups.primOfSlotOf[hit->second.first];
		if (primOfSlot.empty())
			primOfSlot.assign(slotCount, kShadowProxyNoPrim);
		primOfSlot[meshRef.objectSlot] = hit->second.second;
		lookups.worldScaleOfSlot[meshRef.objectSlot] = entityWorldScale(registry, entity);
	});
	return lookups;
}

/** 步骤③的产物：表本体 + 两个对账读数。 */
struct ShadowProxyBuild {
	ShadowProxyTable table;
	uint64_t sumPinnedClusters = 0;  ///< Σ 被钉簇数（P3 用它估多发的簇数）
	uint32_t pinnedSlots = 0;        ///< 有钉段的 objectSlot 数（P3 用它估多发的 draw）
	uint32_t productEntries = 0;     ///< Σ 产物 MustL0 条目数（对账打包侧 --stats 的受影响 prim 数）
};

/**
 * 把一个产物的钉段并进 `table`（步骤③的内层；拆出来是为了让外层保持「找产物 → 合并」的扁平形）。
 *
 * 同一 objectSlot 被两个产物认领 = 资产 id 反查表分家（装配层 bug）⟹ warn 出声保留先到的，
 * **不静默覆盖**（静默覆盖的失效形态是"少画一段"，画面上看不出来）。
 * @return false = 五条 invariant 之一不过（已 error 出声），调用方必须停用整张表
 */
bool mergeOneProduct(const MeshletAsset& product, const MeshletRangeScan& ranges,
                     std::span<const uint32_t> primOfSlot, ShadowProxyTable& table,
                     uint32_t& productEntries) {
	auto* logger = RLLogger::getInstance();
	auto built = buildShadowProxyTable(product, ranges, primOfSlot);
	if (!built) {
		// 任一 invariant 不过 = 产物与打包侧口径分家。表**不建**（存在即生效）⟹ 与门关
		// 逐位相同；error 而不是 warn：画面是对的但判据没了，必须显眼。
		if (logger)
			logger->error("shadow proxy: 钉段表停用（meshlet.shadow_proxy 开而产物侧不变量不过，"
			              "本跑与门关逐位相同）—— {}",
			    built.error());
		return false;
	}
	productEntries += static_cast<uint32_t>(product.mustL0.size());
	for (size_t s = 0; s < built->bySlot.size(); ++s) {
		if (built->bySlot[s].count == 0)
			continue;
		if (table.bySlot[s].count != 0) {
			if (logger)
				logger->warn("shadow proxy: objectSlot {} 被两个产物同时认领（prim 反查表分家）—— "
				             "保留先到的那条，本条丢弃",
				    s);
			continue;
		}
		table.bySlot[s] = built->bySlot[s];
	}
	return true;
}

/**
 * 逐产物调建表纯函数，并进同一张 bySlot。
 *
 * 多产物场景下不同模型的产物各建一张，而同一 objectSlot 只可能属于一个模型 ⟹ 并集天然无冲突
 * （冲突分支的归因 → mergeOneProduct）。
 * @return nullopt = 某产物的五条 invariant 不过（已 error 出声，调用方**不得** emplace）
 */
std::optional<ShadowProxyBuild> mergeShadowProxyProducts(const UploadedAssets& assets,
                                                         const MeshletAssetStore& meshlets,
                                                         const std::map<std::string,
                                                             MeshletRangeScan>& scans,
                                                         const ShadowProxyLookups& lookups,
                                                         uint32_t slotCount) {
	ShadowProxyBuild built;
	built.table.bySlot.resize(slotCount);
	const std::vector<uint32_t> noSlots;  // 该模型零实例时的合法入参（空 span，产物侧 invariant 照验）
	for (size_t m = 0; m < assets.models.size(); ++m) {
		auto scan = scans.find(assets.models[m].file);
		if (scan == scans.end())
			continue;  // 该模型没通过区间扫描（那一步已出声）
		auto product = meshlets.byFile.find(assets.models[m].file);
		if (product == meshlets.byFile.end())
			continue;  // 理论到不了：scans 的产地就在这张表里
		const std::vector<uint32_t>& primOfSlot =
		    lookups.primOfSlotOf[m].empty() ? noSlots : lookups.primOfSlotOf[m];
		if (!mergeOneProduct(product->second, scan->second, primOfSlot, built.table,
		        built.productEntries))
			return std::nullopt;
	}
	for (const auto& entry : built.table.bySlot)
		if (entry.count != 0) {
			++built.pinnedSlots;
			built.sumPinnedClusters += entry.count;
		}
	return built;
}

}  // namespace

/**
 * 接口声明与完整口径 → main.cppm `wireShadowProxyTable`。本定义做四步：两套反查 → 逐产物建表
 * 并进同一张 bySlot → emplace 进 ctx + INFO 读数 → texel 门 8 档 × 4 级联读数表。
 */
void wireShadowProxyTable(entt::registry& registry, const UploadedAssets& assets,
                          const MeshletAssetStore& meshlets,
                          const std::map<std::string, MeshletRangeScan>& scans) {
	auto* logger = RLLogger::getInstance();
	const uint32_t slotCount = registry.ctx().get<NextObjectSlot>().next;
	auto lookups = buildShadowProxyLookups(registry, assets, slotCount);
	auto built = mergeShadowProxyProducts(assets, meshlets, scans, lookups, slotCount);
	if (!built)
		return;  // 已 error 出声：不 emplace = 与门关逐位相同
	auto& stored = registry.ctx().emplace<ShadowProxyTable>(std::move(built->table));
	if (logger)
		logger->info("shadow proxy: 钉段表已装配（消费点 = P3 已接：c1..c3 对过 texel 门的候选"
		             "**多发一条选级恒 0 的 draw**，门开态画面会变）—— "
		             "MustL0 条目={}（对账打包侧 --stats 的受影响 prim 数） 被钉 objectSlot={} "
		             "Σ被钉簇数={} 表体积={}B（{} 槽 × 12B）",
		    built->productEntries, built->pinnedSlots, built->sumPinnedClusters,
		    stored.bySlot.size() * 12u, slotCount);
	if (logger && built->productEntries == 0)
		logger->warn("shadow proxy: 门开了但产物**没有 MustL0 段**（v1 产物 / 打包时门槛 0）—— "
		             "本档与门关逐位相同，要读数得先带 `--must-l0-diag` 重打包");
	logShadowProxyTexelGate(registry, stored, lookups.worldScaleOfSlot);
}

}  // namespace RGL
