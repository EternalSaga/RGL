module;
#include <entt/entt.hpp>
#include <fmt/format.h>

// 入口模块 main 的分区实现单元（module main:meshlets;）——簇化轴的**装配层接线**集中一处。
// 为什么单独成篇（★1-a1 挂号 ①，docs/mdi_plan.md §9）：main_scene.cppm 已 840 行，
// 「装配层接线别再往里塞」是上一批就记下的债；本单元只做簇轴接线——
// ★1-a1ii-α：簇 buffer 描述符装配（wireMeshletBindings）+ 影子叠加管线条目烘焙
// （bakeMeshletEntries）+ ★18-B1 阴影簇条目（bakeShadowMeshletEntries）；
// ★1-a1ii-β-1：原位取代对账与生效（wireMeshletSwitch）；★1-a2-i：剔除生效（wireMeshletCull）。
// **建表**半区（簇区间表 + ★18-B2 阴影 LOD 选级表 + 设备发射规格闸门）在 main:meshlet_tables
// （wireClusterRangeTable 定义处，经 main.cppm 接口声明跨分区调用）。
module main:meshlets;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import rlvk;      // vk:: DeviceAddressRangeEXT / writeResourceDescriptorsEXT 描述符装配
import vk_device;     // DeviceContext（描述符写入取 device 句柄）
import vk_resources;  // Resources（allocSlot / descriptor heap 槽地址）
import scene_context;  // MeshRef（asset / objectSlot）+ NextObjectSlot（实例矩阵槽计数）
import meshlet_ranges;  // ClusterRangeTable（wireMeshletSwitch 对账读逐槽 l0Count；建表归 main:meshlet_tables）
import meshlet_geometry;  // MeshletGeometryStore（4 段 GPU buffer 的 BDA，描述符内容源）
import scene_config;   // SceneConfig（resolvePrimitiveSpecName 入参）
import runtime_config;  // RuntimeConfig / MeshletCoplanarMode（★0.5-b2 wireMeshletOverlayDepthCompare 入参）
import diagnostics;  // diagnostics().cullReplay / cullReplayName（★1-a1ii-β-1 wireMeshletSwitch 的 cull 档闸门）
import vk_renderer;  // Renderer（★0.5-b2 setMeshletOverlayDepthCompareOp 目标；main 已 export import 链上，此处显式）
import scene_data;     // kShadowMapSize（阴影图边长，与模型条目同值）+ SceneDataService（★1-a2-i 剔除服务入参）
import meshlet_cull_service;  // MeshletCullService（★1-a2-i 逐簇剔除的 GPU 侧载体，装配层构造）
import meshlet_cull_types_generated;  // kClusterStride（簇头表条数 = 剔除标志下标上界，★1-a2-i）
import render_graph;   // ensureShadowMapSlot（幂等，按名缓存命中同槽）
import gltf_loader;    // GltfModel/GltfPrimitive（逐 primitive 风格派生）
import asset_table;    // AssetTable（纹理槽号 + bindMeshletToAsset 写回）
import shader_registry;  // ShaderRegistry（addMeshletEntry / bindMeshletToAsset）
import sampler_library;      // SamplerLibrary（共享/point sampler 幂等重取同槽）
import render_target_cache;  // RenderTargetCache（阴影图 RT 幂等重取同槽）

namespace RGL {
namespace {

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

/** 原位取代对账的两个计数（口径 → auditMeshletSwitch）。 */
struct SwitchCoverage {
	uint32_t gated = 0;       ///< 旧路径出 draw 且带簇条目的图元数（= 被取代的 draw 数）
	uint32_t mismatch = 0;    ///< 其中 L0 簇数 = 0 / 表缺位 / 槽越界的图元数（不可取代）
	/// 🔴 doubleSided 材质覆盖的图元数：**不计入 gated**（产物里没有 loader 复制出来的背面副本，
	/// 取代 = 背面消失）。口径与实测数字 → makeOpaqueCall 的闸门注释与 docs/mdi_plan.md §16。
	uint32_t doubleSided = 0;
};

/**
 * 原位取代的**逐资产对账**（★1-a1ii-β-1：把任务书的停止条件「门开资产 l0Count == 0 但旧路径
 * 有 draw」变成装配期机器检查，而不是靠人读日志）。
 *
 * 为什么必须在装配期查、而不是渲染期兜着：不匹配时 `makeOpaqueCall` 会**静默退回旧管线**——
 * 画面完全可能逐位不变（于是所有像素判据全绿），但「簇路径已取代旧路径」这句话从此是假的。
 * 装配期一次遍历（万级图元，微秒级）把这种「半切」状态挡在生效之前。
 *
 * 对账范围只含「旧路径真的会出 draw」的图元：`pipelineIndex` 无效或 `groupCount == 0` 的图元在
 * render_list 构造 draw **之前**就 return，取代与否都与它无关（把它们算进 mismatch = 误杀）。
 * @param registry 场景 registry（读 MeshRef 实例）
 * @param assets   资产表（读 pipelineIndex / groupCount / meshletPipelineIndex）
 * @param table    簇区间表（nullptr = 门关 / 全部模型扫描失败）
 * @return gated = 会被取代的图元数；mismatch = 有簇条目却无簇可画的图元数
 */
SwitchCoverage auditMeshletSwitch(entt::registry& registry, const AssetTable& assets,
	const ClusterRangeTable* table) {
	SwitchCoverage cov;
	registry.view<MeshRef, MaterialRef>().each([&](const MeshRef& meshRef,
		const MaterialRef& matRef) {
		const auto& asset = assets.get(meshRef.asset);
		if (asset.pipelineIndex == kInvalidPipelineIndex || asset.groupCount == 0)
			return;  // 旧路径本就不出 draw（见函数注释）
		if (asset.meshletPipelineIndex == kInvalidPipelineIndex)
			return;  // 无簇产物 / 多产物回落 = 不取代（α 起的回落契约，β-1 不改）
		if (assets.getMaterial(matRef.material).doubleSided) {
			++cov.doubleSided;
			return;  // 🔴 正确性闸门，与 makeOpaqueCall 同一条判据（两处必须同步改）
		}
		++cov.gated;
		const bool hasClusters = table != nullptr && meshRef.objectSlot < table->bySlot.size()
		                         && table->bySlot[meshRef.objectSlot].l0Count > 0;
		if (!hasClusters)
			++cov.mismatch;
	});
	return cov;
}

/**
 * 簇路径「opaque 段原位取代」接线（★1-a1ii-β-1；对外经 wireMeshletPostRegistration 第二步
 * 进入，完整口径 → main.cppm 声明）。四步：
 * ① 门关 / 关档早退（零 ctx 零日志 = 门关 grep meshlet = 0 护栏）→ ② cull 档闸门 →
 * ③ 逐资产对账（停止条件的机器版）→ ④ emplace 生效标记 + INFO 打印**生效**档。
 *
 * 生效标记走 registry ctx（`MeshletSwitchOn`，存在即生效）而不是把三枚配置传进 render_list：
 * 「生效」这件事必须**只判定一次**（消费者重述条件 = 两处真相源迟早漂），且 ctx 是本项目
 * 装配层 → system 的既有唯一通道（`ClusterRangeTable` / `SsrTuning` / `WetTuning` 同法）。
 */
void wireMeshletSwitch(entt::registry& registry, const AssetTable& assets,
	const RuntimeConfig& cfg, const DeviceContext& device) {
	auto* logger = RLLogger::getInstance();
	if (!cfg.meshletEnable || !cfg.meshletSwitch)
		return;  // 门关 / 不切 = 零 ctx 零日志（门关 grep meshlet = 0 护栏）
	// ①.5 设备发射规格核对（口径 → checkMeshletDeviceFit）。放在门关之后、其余判据之前：
	// 规格不成立时后面每一步都毫无意义，且这条**与配置无关**——配置一样、换机器就会变。
	if (!warnIfMeshletDeviceUnfit(device))
		return;
	// ② cull 档闸门。只有 cpu 档的 opaque 段是「逐 draw 盲执行」（executePassDraws 通用分支），
	// 段内换管线条目才落到命令流上。gpu_sync 档那段改由 cullDriver.executeOpaqueSegment 按
	// **候选表**重放（DGC 的 push 与 task 数按旧 mesh 管线烘），换管线不在那条路上 ⟹ 不切、
	// 出声（静默切一半 = 判据全绿而结论是假的）。
	if (diagnostics().cullReplay != CullReplayMode::cpu) {
		if (logger)
			logger->warn("meshlet: switch 在非默认 cull 档不生效（cull_replay = {}，只有 \"cpu\" 档的 "
						 "opaque 段是逐 draw 盲执行、才认段内换管线条目）—— 本跑保持旧路径 + 影子叠加",
				cullReplayName(diagnostics().cullReplay));
		return;
	}
	// ③ 逐资产对账（口径 → auditMeshletSwitch）
	const auto cov = auditMeshletSwitch(registry, assets, registry.ctx().find<ClusterRangeTable>());
	if (cov.mismatch != 0) {
		if (logger)
			logger->warn("meshlet: switch 停用 —— {}/{} 个门开图元有簇管线条目却没有 L0 簇（区间表缺位 "
						 "/ 槽越界 / l0Count = 0）：取代会退化成「半切」（那几条 draw 静默走旧管线），画面"
						 "可能逐位不变而结论是假的 —— 本跑保持旧路径",
				cov.mismatch, cov.gated);
		return;
	}
	// ④ 生效
	registry.ctx().emplace<MeshletSwitchOn>();
	if (logger)
		logger->info("meshlet: switch 生效档 = in_place —— opaque 段内簇路径 1:1 取代旧 mesh draw，"
					 "取代图元 {} 个（同 objectIndex / 同 pushData / 同段内位置，只换管线条目与 task 数 "
					 "= 2×L0 簇数）；doubleSided 材质 {} 个**不接管**（产物无背面副本，见 §16）",
			cov.gated, cov.doubleSided);
	if (logger && cov.gated == 0)
		logger->warn("meshlet: switch 生效但**零图元可取代**（没有任何簇产物通过扫描/对账）—— 本档"
					 "与门关逐位相同，阳性对照必须另外跑");
	// 与 α 影子叠加互斥（render_list 侧的抑制 = `if (!meshletInPlace)` 包住 collectMeshletOverlay
	// 调用点）。coplanar 双臂只作用**叠加**那批 draw ⟹ 叠加清空后它在本档下没有消费者，
	// 而 wireMeshletOverlayDepthCompare 的 INFO 照打不误 —— 不点出来会被读成"双臂还在跑"。
	if (logger)
		logger->warn("meshlet: switch 生效 ⟹ α 影子叠加段（meshletCalls / executeMeshletOverlay）整段"
					 "清空（同一批几何不画两遍）{}",
			cfg.meshletCoplanar == MeshletCoplanarMode::off ? ""
								: "；且 coplanar 双臂只作用叠加 draw，本档下它无消费者");
}

}  // namespace

/**
 * 簇 buffer 描述符接线（★1-a1ii-α；对外经下方 wireMeshletOverlay 总入口调用，
 * 契约见 main.cppm 声明）。五步：门早退 → 单产物闸门 →
 * 领 4 个 Runtime 段槽 → 逐段写 storage buffer 描述符（ensureIzbSlots fallback 同法：
 * `writeResourceDescriptorsEXT` + `vk::DeviceAddressRangeEXT`，装配期 host 直写）→
 * 构造逐簇剔除服务（★1-a2-i）。
 *
 * 描述符 range = 段的 **allocBytes**（含 16B 补齐区）：shader 侧 u8 三元组拼接的尾字
 * 读可能落进补齐区（meshlet.mesh.slang loadU8x3 的钳制论证依赖「range 覆盖补齐」）。
 *
 * 🔴 第⑤步的判据是「走到这里」而不是「`meshlet.cull` 开」：mesh SPV 只有一份、binding 21
 * 无条件声明 ⟹ 标志 buffer 与它的描述符在门关时也必须存在（内容 = 全 1 = 全可见）。
 * 门关的「零新命令」由 `wireMeshletCull` 不把服务交给 Renderer 来保证。
 */
std::optional<MeshletBindingSlots> wireMeshletBindings(Resources& resources,
	DeviceContext& device, const MeshletGeometryStore& meshletGeometry, bool meshletEnable,
	SceneDataService& sceneData, std::unique_ptr<MeshletCullService>& cullService) {
	auto* logger = RLLogger::getInstance();
	if (!meshletEnable)
		return std::nullopt;  // 门关 = 连号都不领（逐位不变构造性成立）
	cullService.reset();  // 下面任何一条回落路径都必须让服务为空（Renderer 据此零命令）
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
	// ⑤ 逐簇剔除服务（★1-a2-i）。容量 = 簇头表条数（= 标志下标 `cullBase + 簇内序` 的上界，
	// 与 itemCount 无关，口径 → MeshletCullService 类注释）；簇头槽**复用** binding 18 那一槽
	// （同一张表两份描述符没有任何收益，descriptor 内容不变、只是多一个 binding 指它）。
	const uint32_t clusterCapacity =
		static_cast<uint32_t>(geom.clusters.bytes / static_cast<vk::DeviceSize>(kClusterStride));
	cullService = std::make_unique<MeshletCullService>(device, resources, sceneData,
		slotNumbers[1], clusterCapacity);
	if (logger)
		logger->info("meshlet: 簇 buffer 描述符已装配 {} —— 槽 {}/{}/{}/{}（binding 17-20，"
					 "顶点池 {}B）；可见标志槽 {}（binding 21）",
			file, slotNumbers[0], slotNumbers[1], slotNumbers[2], slotNumbers[3],
			geom.vertexPool.allocBytes, cullService->visibleSlot());
	return MeshletBindingSlots{.vertexPool = slotNumbers[0], .clusters = slotNumbers[1],
		.tris = slotNumbers[2], .remap = slotNumbers[3],
		.clusterVisible = cullService->visibleSlot()};
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
	const std::array<uint32_t, 5> clusterSlots{slots->vertexPool, slots->clusters, slots->tris,
		slots->remap, slots->clusterVisible};
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
 * 阴影段簇路径条目烘焙（★18-B1；对外经下方 wireMeshletOverlay 总入口调用）。
 *
 * 🔴 **本批只建条目，不接任何 draw**（接线 = 批 B2）。于是门开的形态 = 「条目建了、资产字段
 * 写了、没有任何一条 draw 引用它」⟹ 截图必须仍 = 基线锚（本批主判据：基础设施本身不改像素）。
 *
 * 早退两条（任一成立 = 一条条目都不建、**一行日志都不打**，门关 `grep -ci meshlet` = 0 的护栏）：
 *  · `!slots` —— 门关 / 无产物 / 多产物回落（wireMeshletBindings 没建服务，簇 buffer 描述符
 *    根本不存在，条目烘出来就是「声明了 binding 却无 mapping」= VVL 错误）；
 *  · `cfg.meshletShadowLod < 0` —— 本档关（-1 = 出厂默认）。
 *
 * 第三条路径是**出声**不是早退：`slots` 有值（= 门开且产物在）却零资产命中 ⟹ warn。
 * 「配置说开了而实际一条没建」静默通过 = 判据全绿而结论是假的（同 wireMeshletCull 的
 * 「服务缺位而出声」第二道护栏）。
 *
 * 🔴 为什么**逐 prim 调** addShadowMeshletEntry 而不是只调一次：条目去重住在被调方（全场景
 * 1 条目，同一次调用返回同一个下标），但**写回是逐资产的**。逐 prim 走一遍让「没有簇产物的
 * 资产」天然保持 `kInvalidPipelineIndex`（它们的循环根本走不到）——批 B2 靠这个字段判
 * 「这个资产能不能走簇路径」，调一次再统一回填就得在这里重造一遍"哪些资产有产物"的判据。
 * @param slots wireMeshletBindings 的产物（4 个簇 buffer 堆槽 + 可见标志槽；nullopt = 早退）
 * @param cfg   运行配置（读 meshletShadowLod 本档开关）
 */
void bakeShadowMeshletEntries(const UploadedAssets& uploaded, ShaderRegistry& pipelines,
	const MeshletGeometryStore& meshletGeometry,
	const std::optional<MeshletBindingSlots>& slots, const RuntimeConfig& cfg) {
	if (!slots || cfg.meshletShadowLod < 0)
		return;  // 门关 / 无产物 / 多产物回落 / 本档关 = 零条目零日志（逐位不变 + grep 护栏）
	const std::array<uint32_t, 4> clusterSlots{slots->vertexPool, slots->clusters, slots->tris,
		slots->remap};
	uint32_t baked = 0;
	// 🔴 条目下标用 optional 而不是 0 初值：0 是**合法条目下标**（内建几何就在低位），零覆盖时
	// 打进日志的"条目下标 0"会被读成"建在了 0"——那是谎报。
	std::optional<uint32_t> entryIdx;
	for (const auto& um : uploaded.models) {
		if (!meshletGeometry.byFile.contains(um.file))
			continue;  // 该模型旁没有簇产物（或扫描/对账失败）= 无阴影簇条目
		for (const uint32_t id : um.assetIds) {
			entryIdx = pipelines.addShadowMeshletEntry(clusterSlots);
			pipelines.bindShadowMeshletToAsset(id, *entryIdx);
			++baked;
		}
	}
	auto* logger = RLLogger::getInstance();
	if (baked == 0) {
		// 门开了、产物也在（slots 有值就要求 byFile 恰一份），却没有任何资产的文件名命中
		// ⟹ 本批的"生效"证据不能是 INFO（什么都没生效）。出声，别静默（同 wireMeshletCull
		// 「服务缺位而出声」的第二道护栏纪律）。
		if (logger)
			logger->warn("meshlet: 阴影簇管线条目**未装配** —— meshlet.enable 开且簇产物存在，"
						 "但没有一个已上传模型的产物文件命中（shadow_lod={} 无消费者）",
				cfg.meshletShadowLod);
		return;
	}
	if (logger)
		logger->info("meshlet: 阴影簇管线条目已装配（★18-B1，**只建条目不接 draw**，接线归 B2）"
					 " —— 条目下标 {} 覆盖 prims={}（全场景 1 条目，簇 buffer 全局单例）"
					 " 簇 buffer 槽 {}/{}/{}/{}（binding 17-20，无 21）；shadow_lod={} fallback={}",
			*entryIdx, baked, clusterSlots[0], clusterSlots[1], clusterSlots[2], clusterSlots[3],
			cfg.meshletShadowLod,
			cfg.meshletShadowLodFallback == MeshletLodFallbackMode::l0 ? "l0" : "deepest");
}

/**
 * 簇化轴 ★1-a1ii-α 装配总入口（main.cppm 声明）：描述符接线 + 条目烘焙两步的顺序
 * 契约内聚在此（bake 依赖 wire 的槽号），main() 只留一次调用（装配层行数预算）。
 *
 * ★0.5-b2 起 mesh SPV 变体三档（base/debug/tint）：tint 与 debug 同真时 **tint 优先 +
 * warn**（判据 3 只用 tint、判据 4 只用 offset，从不同时用，故不建第 4 个组合变体；
 * warn 只在门开时出——门关 grep meshlet = 0 的护栏）。
 */
std::unique_ptr<MeshletCullService> wireMeshletOverlay(Resources& resources,
	DeviceContext& device,
	const MeshletGeometryStore& meshletGeometry, const UploadedAssets& uploaded,
	const SceneConfig& scene, bool deferredMode, ShaderRegistry& pipelines, AssetTable& assets,
	SamplerLibrary& samplers, RenderTargetCache& rtCache, const RuntimeConfig& cfg,
	SceneDataService& sceneData) {
	std::unique_ptr<MeshletCullService> cullService;
	const auto slots = wireMeshletBindings(resources, device, meshletGeometry, cfg.meshletEnable,
		sceneData, cullService);
	// mesh SPV 变体选档（门关 = bakeMeshletEntries 首行早退，variant 不被读；warn 也
	// 必须门开才出声——门关 grep meshlet = 0 的护栏）
	MeshletMeshVariant variant = MeshletMeshVariant::base;
	if (cfg.meshletTint) {
		variant = MeshletMeshVariant::tint;
		if (cfg.meshletEnable && cfg.meshletDebug && RLLogger::getInstance())
			RLLogger::getInstance()->warn(
				"meshlet: tint 与 debug 同真 ⟹ tint 优先（不建组合 SPV 变体；判据 3 只用 "
				"tint、判据 4 只用 debug，从不同时用）—— 本跑走 TINT 变体");
	} else if (cfg.meshletDebug)
		variant = MeshletMeshVariant::debugOffset;
	bakeMeshletEntries(uploaded, scene, deferredMode, pipelines, assets, resources, samplers,
		rtCache, meshletGeometry, slots, variant);
	// ★18-B1：阴影段簇路径条目（**只建条目不接 draw**，接线归 B2）。放在 bakeMeshletEntries
	// 之后 = 同一份 slots 复用、且不打乱既有条目序之前的任何一条（本条目是纯追加）。
	bakeShadowMeshletEntries(uploaded, pipelines, meshletGeometry, slots, cfg);
	// ★1-a2-i：服务所有权交给调用方（装配层里先于 Renderer 声明的变量），理由 → main.cppm 声明
	return cullService;
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

/**
 * 逐簇视锥剔除生效接线（★1-a2-i，main.cppm 声明；调用序契约 = assembleBackEnd 之后）。
 * 四步：门关早退（零调用零日志——门关「帧首零新命令」的构造性保证就在这第一行）→ 服务缺位
 * 而出声（配置说开了而实际没跑 = 判据全绿而结论是假的，同 switch/coplanar 的第二道护栏）→
 * 无消费者（switch 关）出声且不 dispatch → setter + INFO 打印生效档。
 */
void wireMeshletCull(Renderer& renderer, entt::registry& registry, const RuntimeConfig& cfg,
	MeshletCullService* cullService) {
	if (!cfg.meshletEnable || !cfg.meshletCull)
		return;  // 门关 = 零调用零日志（Renderer 成员恒 nullptr ⟹ 帧首一条命令都不多）
	auto* logger = RLLogger::getInstance();
	if (cullService == nullptr) {
		if (logger)
			logger->warn("meshlet: meshlet.cull 开但剔除服务未装配（多产物回落 / 无产物 / 扫描全数"
						 "失败）—— 逐簇剔除**不运行**，本跑画面与门关逐位相同");
		return;
	}
	// 🔴 消费者 = **已生效**的取代档（ctx 里有没有 `MeshletSwitchOn`），读生效事实而不是配置意图：
	// switch 的三条回落路（设备规格 / 非 cpu cull 档 / 对账失败）都不写配置，读配置会让剔除在
	// 那些档下照跑而取代根本没发生。switch 关时簇 draw 只存在于 α 影子叠加段，而叠加默认
	// coplanar="off" = 共面全拒零像素写入 ⟹ 剔它纯属白干；更坏的是 debug 档（DEBUG_OFFSET 变体
	// 把 clip.z 拉向相机）下几何已被位移，用**未位移**位置做的视锥测试会剔掉"位移后其实在屏内"
	// 的簇 = 让诊断档的图少画东西。故这里**不 dispatch**（标志表恒为启动期填的 1，叠加段照画），
	// 只出声。承诺出处 = runtime_config.cppm meshletCull 注释「单独开 = 无消费者，装配层 warn
	// 出声（静默空开关是本项目反复踩的形态）」。
	if (!registry.ctx().contains<MeshletSwitchOn>()) {
		if (logger)
			logger->warn("meshlet: meshlet.cull 开但取代档未生效（meshlet.switch 关 / 设备规格不够 / "
						 "非 cpu cull 档 / 逐资产对账失败）—— 标志数组没有消费者，逐簇剔除**不运行**，"
						 "本跑画面与 meshlet.cull 关逐位相同");
		return;
	}
	renderer.setMeshletCullService(cullService);
	if (logger)
		logger->info("meshlet: 逐簇视锥剔除生效档 = gpu —— 帧首（recordFixedDataPath 之后、pass "
					 "循环之前）dispatch meshlet_cull.comp，标志表 {} 条；mesh 侧按 cullBase+簇内序"
					 "早退（SetMeshOutputCounts(0,0)）",
			cullService->itemCount());
}

/**
 * 接口声明与完整口径 → main.cppm `wireMeshletPostRegistration`。本定义只做一件事：
 * 把「实体注册之后」的两步簇轴装配合成一次调用（`main()` 已在 120 行代码红线，
 * `wireSceneSpinContext` / `bakeMeshletEntries` 同法抽 helper，零行为变化）。
 * 顺序契约内聚在此：区间表**先**建（`wireMeshletSwitch` 的对账要读逐槽 l0Count）。
 */
void wireMeshletPostRegistration(entt::registry& registry, const UploadedAssets& uploaded,
	const MeshletAssetStore& meshlets, const AssetTable& assetTable, const RuntimeConfig& cfg,
	const DeviceContext& device, MeshletCullService* cullService) {
	wireClusterRangeTable(registry, uploaded, meshlets, cfg.meshletEnable, cfg.meshletCull,
		cullService, assetTable, cfg, device);
	wireMeshletSwitch(registry, assetTable, cfg, device);
}

}  // namespace RGL
