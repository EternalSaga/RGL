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
                      // + kActiveShadowCascades（★M-1a：MDI 级联段数 = 本值 − 1，c0 恒旧路径）
import meshlet_lod;   // MeshletShadowLodTable（★M-1a：载体的生效事实与「每段行数上界」的来源）
import meshlet_shadow_args;  // MeshletShadowArgsService（★M-1a：真 MDI 行表的 GPU 载体，装配层持有）
import render_list;          // MeshletMdiShadowActivation（★M-1b：第⑤步 emplace 的生效标记，采集侧读它）
import frame_session;        // FrameSession::kFramesInFlight（★M-1a：ring×2 的轮转节奏唯一真相源）
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

/**
 * α 影子叠加段的**生效**接线（债-105，`meshlet.overlay` → registry ctx 标记 `MeshletOverlayOn`；
 * 对外经 `wireMeshletPostRegistration` 第三步进入）。三步：门关早退（零 ctx 零日志 = 「门关
 * `grep -ci meshlet` = 0」护栏）→ emplace 生效标记 → 按**事实**出声或打印生效档。
 *
 * 🔴 **生效条件里没有 `meshlet.enable`**（同 `mdi_shadow` 的定性：`meshlet` 块里生效条件不依赖
 * `enable` 的键）——本键的宿主是叠加段自己。但条目的**烘焙**条件挂在 `wireMeshletBindings` 的
 * `slots`（= `enable`）上 ⟹「overlay 开 + enable 关」= 没有条目可采 = **零 draw 而日志一行不出**
 * 的静默空开关（§18.9 第 9 条抓到的同一形态）。所以这里读**事实**（有没有资产带着簇管线条目）
 * 而不是配置意图：无产物 / 多产物回落 / 零模型命中这几条回落路都不写配置，只比 `enable` 会漏。
 * 孤立组合 = **出声但仍 emplace**（标记只代表「本段采集开启」，画不画由 `collectMeshletOverlay`
 * 的判据 1 决定）——别把门改成 `enable && overlay`，那等于把两件事焊回一处，`switch` 的宿主与
 * 叠加段的门重新纠缠。
 * @param registry 场景 registry（emplace 目标 + MeshRef 事实扫描；🔴 **必须在实体注册之后**）
 * @param assets   资产表（读 `meshletPipelineIndex` = 条目宿主在不在）
 * @param cfg      运行配置（读本键）
 */
void wireMeshletOverlayActivation(entt::registry& registry, const AssetTable& assets,
	const RuntimeConfig& cfg) {
	if (!cfg.meshletOverlay)
		return;  // 门关（出厂默认）= 零 ctx 零日志
	registry.ctx().emplace<MeshletOverlayOn>();
	auto* logger = RLLogger::getInstance();
	uint32_t hosts = 0;
	registry.view<MeshRef>().each([&](const MeshRef& meshRef) {
		if (assets.get(meshRef.asset).meshletPipelineIndex != kInvalidPipelineIndex)
			++hosts;
	});
	if (hosts == 0) {
		if (logger)
			logger->warn("meshlet: overlay 开但**没有一条簇管线条目可采**（meshlet.enable 关 / 无簇产物 "
						 "/ 多产物回落 / 零模型命中）—— 叠加段零 draw，本跑画面与 overlay 关逐位相同；"
						 "要它生效请把 meshlet.enable 一并置 true（条目的烘焙条件挂在那枚门上）");
		return;
	}
	if (logger)
		logger->info("meshlet: overlay 生效档 = on —— α 影子叠加段采集 {} 个宿主图元（每 draw 的 task 数 "
					 "= meshletTaskCount(L0 簇数)；正常档共面 + eLess ⟹ 一像素不写 = 验证脚手架）。"
					 "代价与两锚的口径 → docs/mdi_plan.md §18.9 / §18.11",
			hosts);
}

/**
 * 叠加 mesh SPV 变体选档（★0.5-b2 的三档 base/debug/tint + **债-105** 的归属裁决）。
 *
 * 🔴 变体位**归叠加段所有**：`overlay = false` 时**一律 base**——这两枚是**条目级**开关（变体经
 * `bakeMeshletEntries` 的 `variant` 烘进**条目**），而那批条目同时是 β-1 原位取代的宿主
 * （`makeOpaqueCall` 读 `asset.meshletPipelineIndex`）⟹ 叠加段关着也能改产品路的几何
 * （DEBUG_OFFSET 把 clip.z 拉向相机 = 取代段整体错位；TINT 把着色输入写死 = 产品路变平色块）。
 * 这不是「仪器没宿主」而是「仪器渗进产品」。孤立组合（debug/tint 开了但被强制归 base、
 * tint 与 debug 同真的让位）的出声自债-106 起在 **gate_lint**（meshlet.debug / meshlet.tint
 * 三条规则）——原就近 warn `warnMeshletVariantInstruments` 的判据是纯配置组合，同一根因
 * 不许多处各喊一条（影子真相源），随迁移删除。
 *
 * 优先级与 ★0.5-b2 一致：tint 与 debug 同真 = **tint 优先**（判据 3 只用 tint、判据 4 只用
 * offset，从不同时用 ⟹ 不建第 4 个组合变体）。
 * @param cfg 运行配置（读 overlay / tint / debug 三字段）
 * @return 本跑该烘的 mesh SPV 变体
 */
MeshletMeshVariant pickMeshletMeshVariant(const RuntimeConfig& cfg) {
	if (!cfg.meshletOverlay)
		return MeshletMeshVariant::base;
	if (cfg.meshletTint)
		return MeshletMeshVariant::tint;
	if (cfg.meshletDebug)
		return MeshletMeshVariant::debugOffset;
	return MeshletMeshVariant::base;
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
	// 与 α 影子叠加互斥（render_list 侧的抑制 = `collectMeshletOverlay` 的第六枚判据，入参
	// `meshletInPlace`；债-105 把判定点从调用点的 `if` 搬进函数，事实仍是同一个 `MeshletSwitchOn`）。coplanar 双臂只作用**叠加**那批 draw ⟹ 叠加清空后它在本档下没有消费者，
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
	// 🔴 **关 overlay ≠ 关条目**（债-105 的宿主契约，别再往这条门上挂任何东西）：本函数的存在
	// 条件只有 `slots`（= `meshlet.enable`），而**不**看 `meshlet.overlay`。理由是这批条目同时是
	// 两个消费者的宿主——① α 影子叠加段（`collectMeshletOverlay`，由 overlay 门决定采不采 draw）、
	// ② β-1 的原位取代（`makeOpaqueCall` 读 `asset.meshletPipelineIndex`）。把烘焙挪进 overlay 门下
	// ⟹ `overlay = false` 时 `switch` **静默失去宿主**（取代分支整个不成立，画面回到旧路径而日志
	// 里没有任何东西说它没切）。叠加段关 = **不采 draw**，条目照烘。
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

// ─── ★M-1a：阴影段真 MDI 行表 GPU 载体的接线（零消费者，一条 draw 都不接）───────────
/**
 * 载体接线（§19.5 的 ★M-1 接缝）。四步，顺序即代码：
 *  ① 门关早退（`!meshletEnable` 或 `meshletShadowLod < 0`）= **连号都不领**（先例 =
 *     `wireMeshletBindings` 首行；「门关 grep -ci meshlet = 0」的护栏）；
 *  ② 读**生效事实**而不是配置意图：ctx 里没有 `MeshletShadowLodTable` = 阴影簇路径根本没生效
 *     （设备规格不够 / 逐资产对账失败 / 零槽可切 三条回落路都不写配置）⟹ 不建载体，
 *     也不出声——出声归 `wireClusterRangeTable` 自己（它已经点过名，重复出声会让「门关零日志」
 *     的护栏在别的档下失真）；
 *  ③ 闭式算出 `sliceRows`（每帧槽行数）后构造服务；
 *  ④ 所有权交给调用方（🔴 声明序必须**先于 Renderer**——本服务持 GPU buffer，块尾析构 =
 *     帧循环读到已销毁的 VA，见 AGENTS 生命周期红线与 `issue_history` 的 UAF 先例）。
 *
 * 🔴 **★M-1b 的时序接缝（已消解）**：本步在**注册之后**（第④步），而条目烘焙需要 `argSlot()`
 * 的槽号——正解**不是**领槽前移（前移会把其后所有槽号 +1，污染拿槽号做日志对照的批次），
 * 而是把 **MDI 条目烘焙也放到注册之后**（第⑤步 `wireMeshletMdiShadowActivation`，紧跟本步），
 * `argSlot()` 于此刻是真值；同时整个函数仍在 `assembleBackEnd` 之前 ⟹ DGC/GpuCull 的
 * `entries.size()` 快照天然含新条目（雷 1/雷 2 时序论证 → 该函数头注释与 §19.9）。
 * 环层与服务层的分界照旧有效（buffer 与描述符在本步建，条目在第⑤步拿号）。
 *
 * 🔴 生效条件里**不含** `meshlet.mdi_shadow`（§19.6 挂号那条）：载体是「行表住在 GPU 里」，
 * 与「阴影 draw 改用 MDI 形态」（那条才是选 SPV 变体/选条目的门，§19.2 D2）是两件事。
 * ⟹ 出厂默认（`mdi_shadow = false`）也建载体但零消费者，图像逐位不变。
 * ★M-1b 落地形态（本挂号的答复）：binding 34（定案 22 被 sceneHeader 占，见
 * shadow_meshlet.mesh.slang 的 ⚠ 注）的槽号**仍**从宿主条件链取（本服务构造 = enable +
 * shadow_lod + 选级表三判据，不因 mdi_shadow 改动）；「`= false` 时零消费者」升级为
 * 「`= false` 时零条目零 ctx，`= true` 而载体缺位时第⑤步 wireMeshletMdiShadowActivation
 * WARNING 出声」——静默空开关不成立。
 *
 * ## `sliceRows` 的闭式（每帧槽 = 所有级联段的并）
 * `MDI 级联段数 × 每段行数上界`，两个因子各有出处：
 *  · **段数 = `kActiveShadowCascades − 1`**（🔴 c0 恒旧路径，§18.6 第一刀零改动；IZB mask 重放
 *    按 8B push 读 c0 段靠的就是这条）⟹ c1..c3 = 3 段；
 *  · **每段行数上界 = 选级表里 `count > 0` 的 objectSlot 数**：每个可切候选占**自己**一个
 *    objectSlot（下标 = `MeshRef.objectSlot` = shader 的 objectIndex），所以它是「一段能有多少
 *    行」的**上界**而不是精确值——差的那几行正是 doubleSided / blend 这些「有选级条目但不进
 *    MDI」的实体（本机真产物 = 1591 上界 vs §19.5 的 1530 可切）。按上界建 ⟹ 结构上不可能
 *    越界写，而多出来的行恒 0（`gcX = 0` = 空转，§19.2 D4 的孔洞流本来就是合法形态）。
 *    🔴 不在这里重述 `collectShadowCasters` 的四条判据（那是第三处真相源）。
 * @param registry 场景 registry（读 `MeshletShadowLodTable` = 生效事实；🔴 必须注册之后）
 * @param resources 资源层（领槽 + VMA 分配器 + 描述符堆）
 * @param cfg       运行配置（读 `meshletEnable` / `meshletShadowLod` 两枚宿主条件）
 * @return 载体服务；门关 / 阴影簇路径未生效 / 上界为 0 = nullptr（零 buffer 零槽零日志）
 * @throw std::runtime_error 服务构造失败（领槽 / buffer / 描述符；启动期一次性路径）
 */
std::unique_ptr<MeshletShadowArgsService> wireMeshletShadowArgs(entt::registry& registry,
	Resources& resources, DeviceContext& device, const RuntimeConfig& cfg) {
	if (!cfg.meshletEnable || cfg.meshletShadowLod < 0)
		return nullptr;  // 门关 = 连号都不领
	const auto* lod = registry.ctx().find<MeshletShadowLodTable>();
	if (lod == nullptr)
		return nullptr;  // 阴影簇路径没生效（三条回落路都只出声不写配置，读事实不读意图）
	uint32_t rowsPerCascade = 0;
	for (const auto& entry : lod->bySlot)
		rowsPerCascade += entry.count > 0u ? 1u : 0u;
	// 级联段数：c1..cN（🔴 c0 恒旧路径，不进表）。写成减法而不是字面 3 = 切级联时不漏校验
	// （同 frame_draw_list 对 `kActiveShadowCascades` 的那条纪律）。
	const uint32_t mdiCascades = kActiveShadowCascades - 1u;
	const uint32_t sliceRows = rowsPerCascade * mdiCascades;
	if (sliceRows == 0u) {
		// 表在但全零 = 与门关逐位相同（同 `wireMeshletCull` 的「服务缺位而出声」第二道护栏：
		// 「配置说开了而实际没跑」必须在日志里看得见，别让下一位拿旧锚读成生效）。
		if (auto* logger = RLLogger::getInstance())
			logger->warn("meshlet: mdi_shadow 载体未建 —— 选级表全零条目（可切候选 0 个）：本档与"
						 "门关逐位相同，要它生效必须有至少一个资产过四条可切判据");
		return nullptr;
	}
	// 🔴 轮转节奏的唯一真相源（本类不抄字面 2）。main 在分层里位于 frame 之上，import 方向合法。
	return std::make_unique<MeshletShadowArgsService>(device, resources, sliceRows,
		FrameSession::kFramesInFlight);
}

/**
 * 第⑤步的回填子段：把 MDI 条目写进「带 474 条目」的全部 mesh 资产（判据 = 字段本身，
 * 不复述 bakeShadowMeshletEntries 的 byFile 对账 = 第三处真相源）。
 * @return 回填成功的资产数（0 = 载体成立却没有可回填资产，调用方出声）
 */
uint32_t bindMdiShadowEntryToAssets(ShaderRegistry& pipelines, const AssetTable& assets,
	uint32_t mdiIdx) {
	uint32_t bound = 0;
	for (uint32_t id = 0; id < assets.meshSize(); ++id) {
		if (assets.get(id).shadowMeshletPipelineIndex == kInvalidPipelineIndex)
			continue;
		pipelines.bindShadowMeshletMdiToAsset(id, mdiIdx);
		++bound;
	}
	return bound;
}

/**
 * 第⑤步（★M-1b）：真 MDI 阴影段**管线条目烘焙 + 资产回填 + 生效标记**（docs/mdi_plan.md
 * §19.2 D2 / §19.5；消费者 = `renderListSystem` 经 ctx 读 `MeshletMdiShadowActivation`）。
 *
 * ## 时序（雷 1 / 雷 2 的结论，逐条给在这）
 * 本步排在第④步之后（`argSlot()` 此刻是真值——★M-1a 挂号的「领槽前移」接缝被这个形态**消解**：
 * 不需要前移，因为条目烘焙本来就能等到载体之后），而整个 `wireMeshletPostRegistration` 仍在
 * `assembleBackEnd` **之前**（main_impl 调用序），于是：
 *  · DGC `capacity = 2×entries.size()` 与 execution set 逐条目写句柄的循环（vk_indirect_commands_impl
 *    :44/:129）取的是**含本条目**的快照——容量口径自洽，新条目只是两个闲置槽；
 *  · `DgcStaticCommands` 的 `vkEntryCount > entries.size()` 守卫（:280）：vkEntryCount 也在
 *    assembleBackEnd 期取（main_impl entriesView().size()），RHS 含本条目 ⟹ 构造期不可触发；
 *  · `cloneRemap`（addCloneEntries，早于本步）的 remap.size() < 新 entries.size()：读侧
 *    `cloneEntryFor` 有 `entry >= remap.size() → nullopt` 界内守卫（shader_registry.cppm），
 *    且 MDI 条目 modelEntry=false 本就无克隆语义——越界读与陈旧 kNoClone 两条路都到不了；
 *  · shader_hot_reload 的 buildBaselines 在 assembleBackEnd 之后挂（晚于本步）⟹ 索引对条目数组
 *    全程一致；updateShaderPair 又有 execSlotCapacity 界内守卫。
 * 🔴 **纯追加**（§19.7 前置 1）：本条目只能落在旧条目 474 之后；INFO 打机读三元（新下标 /
 * 旧下标 / 旧槽值）供 A8 对账。NVIDIA DGC execute 路本机不可测 → ★M-2（三机）复核。
 *
 * ## 早退与出声（债-106 的孤立空开关形态）
 *  · `!meshletMdiShadow` → **零条目零 INFO 零 ctx**（A2 出厂构造性：那条条目根本不存在）；
 *  · 键开而宿主不在（载体 nullptr = enable 关 / shadow_lod<0 / 选级表全零；或簇槽 nullopt）
 *    → **WARNING 出声**（配置说开了而实际没跑，同 `wireMeshletCull` 的第二道护栏）；
 *  · 有载体但零资产带 474 条目（理论不可达——载体成立要求选级表非零，而建表的资产判据同源）
 *    → 也出声，不静默不激活。
 * @param registry 场景 registry（生效标记 emplace 目标；🔴 与④同点调用 = 注册之后）
 * @param pipelines 管线表（addShadowMeshletMdiEntry / bindShadowMeshletMdiToAsset / 旧条目下标读回）
 * @param assets   资产表（回填目标 = 「带 474 条目」的资产；判据用字段本身，不复述 byFile 对账）
 * @param assembly 服务持有者（读④的 shadowArgs 与 α 的 bindingSlots）
 * @param cfg      运行配置（meshletMdiShadow 门）
 */
void wireMeshletMdiShadowActivation(entt::registry& registry, ShaderRegistry& pipelines,
	const AssetTable& assets, MeshletAssembly& assembly, const RuntimeConfig& cfg) {
	if (!cfg.meshletMdiShadow)
		return;  // 门关 = 零条目零 INFO 零 ctx（A2 构造性）
	auto* logger = RLLogger::getInstance();
	if (!assembly.shadowArgs || !assembly.bindingSlots) {
		if (logger)
			logger->warn("meshlet: mdi_shadow 开但宿主不在（行表载体 {} / 簇 buffer 槽 {}）—— 阴影段"
						 "继续逐条簇 draw，本开关**不生效**。宿主条件 = meshlet.enable + "
						 "shadow_lod ≥ 0 + 选级表非零（§19.6：生效条件不含 mdi_shadow 本身）",
				assembly.shadowArgs ? "已建" : "未建", assembly.bindingSlots ? "已领" : "未领");
		return;
	}
	const auto& slots = *assembly.bindingSlots;
	const std::array<uint32_t, 4> clusterSlots{slots.vertexPool, slots.clusters, slots.tris,
		slots.remap};
	// 条目幂等（同名命中返回既有下标）；argsSlot = ④ 领的行表堆槽，binding 34 与声明同向。
	const uint32_t mdiIdx = pipelines.addShadowMeshletMdiEntry(clusterSlots,
		assembly.shadowArgs->argSlot());
	const uint32_t bound = bindMdiShadowEntryToAssets(pipelines, assets, mdiIdx);
	if (bound == 0) {
		if (logger)
			logger->warn("meshlet: mdi_shadow 条目已烘（下标 {}）但零资产带 474 条目可回填 —— "
						 "不激活（理论不可达：载体成立要求选级表非零），阴影段继续旧路径", mdiIdx);
		return;
	}
	registry.ctx().emplace<MeshletMdiShadowActivation>(MeshletMdiShadowActivation{
		.sliceRows = assembly.shadowArgs->sliceRows(),
		.slices = assembly.shadowArgs->slices(),
	});
	// 🔴 A8 机读三元：新条目下标 / 旧 shadow_meshlet 条目下标（应恒 474 = 纯追加实证）/
	// 旧槽值（8252/8253/8254/8255/8257 那批）与行表槽。条目总数一并打，供 cloneRemap/DGC
	// 快照口径核对（entries.size() 在本步之后、assembleBackEnd 之前不再变）。
	const auto oldIdx = pipelines.pipelineIndex("shadow_meshlet");
	if (logger)
		logger->info("meshlet: mdi_shadow 生效（★M-1b）—— 机读三元：新条目下标={} 旧 shadow_meshlet "
					 "条目下标={}（纯追加应仍=474）entries_total={}；旧簇槽 {}/{}/{}/{} + 可见标志槽 {} "
					 "未动，行表槽 {}（binding 34）；sliceRows={} × slices={}（Σ行上界口径 = 选级表 "
					 "count>0 的 objectSlot 数 × 级联段数，见 wireMeshletShadowArgs）",
			mdiIdx, oldIdx.value_or(0xFFFFFFFFu), pipelines.entriesView().size(), clusterSlots[0],
			clusterSlots[1], clusterSlots[2], clusterSlots[3], slots.clusterVisible,
			assembly.shadowArgs->argSlot(), assembly.shadowArgs->sliceRows(),
			assembly.shadowArgs->slices());
}

/**
 * 簇化轴 ★1-a1ii-α 装配总入口（main.cppm 声明）：描述符接线 + 条目烘焙两步的顺序
 * 契约内聚在此（bake 依赖 wire 的槽号），main() 只留一次调用（装配层行数预算）。
 *
 * ★0.5-b2 起 mesh SPV 变体三档（base/debug/tint）：tint 与 debug 同真时 **tint 优先**
 * （判据 3 只用 tint、判据 4 只用 offset，从不同时用，故不建第 4 个组合变体；让位与孤立
 * 组合的 warn 自债-106 起在 gate_lint 启动期申报，原就近 warnMeshletVariantInstruments 已
 * 随迁移删除——门关 grep meshlet = 0 的护栏由「门关默认档无偏离」保证）。
 *
 * 🔴 债-105 起变体位**归叠加段所有**：`meshlet.overlay` 关 = 变体一律 base（变体烘进条目、
 * 条目是 β-1 取代的宿主 ⟹ 不归位会让关掉叠加段的跑仍然被诊断改掉产品路几何；偏离默认时
 * 由 gate_lint meshlet.debug / meshlet.tint 规则 warn）。
 * 本函数**不**因为 overlay 关而少烘一条条目——见 `bakeMeshletEntries` 的宿主契约注释。
 */
MeshletAssembly wireMeshletOverlay(Resources& resources,
	DeviceContext& device,
	const MeshletGeometryStore& meshletGeometry, const UploadedAssets& uploaded,
	const SceneConfig& scene, bool deferredMode, ShaderRegistry& pipelines, AssetTable& assets,
	SamplerLibrary& samplers, RenderTargetCache& rtCache, const RuntimeConfig& cfg,
	SceneDataService& sceneData) {
	MeshletAssembly assembly;
	const auto slots = wireMeshletBindings(resources, device, meshletGeometry, cfg.meshletEnable,
		sceneData, assembly.cullService);
	// ★M-1b：槽位快照进 assembly（第⑤步 `wireMeshletMdiShadowActivation` 烘 MDI 条目要复用
	// binding 17-20；此刻是本函数局部的 nullopt/值，nullopt 原样传下去 = 与早退判据同一事实）。
	assembly.bindingSlots = slots;
	// mesh SPV 变体选档（门关 = bakeMeshletEntries 首行早退，variant 不被读；孤立组合的 warn
	// 自债-106 起归 gate_lint 启动期申报——门关 grep meshlet = 0 的护栏由「门关默认档无偏离」
	// 保证，见 meshlet.debug / meshlet.tint 规则）。一件事一个函数：选档 = pickMeshletMeshVariant。
	const MeshletMeshVariant variant = pickMeshletMeshVariant(cfg);
	bakeMeshletEntries(uploaded, scene, deferredMode, pipelines, assets, resources, samplers,
		rtCache, meshletGeometry, slots, variant);
	// ★18-B1：阴影段簇路径条目（**只建条目不接 draw**，接线归 B2）。放在 bakeMeshletEntries
	// 之后 = 同一份 slots 复用、且不打乱既有条目序之前的任何一条（本条目是纯追加）。
	bakeShadowMeshletEntries(uploaded, pipelines, meshletGeometry, slots, cfg);
	// ★1-a2-i / ★M-1a：两格服务的所有权交给调用方（装配层里**先于 Renderer** 声明的变量），
	// 理由 → main.cppm 声明。🔴 `shadowArgs` 这一格**本函数不填**：它的每段行数上界要读
	// `registerSceneEntities` 之后才 emplace 的选级表（生效事实），填它 = 第四步。
	return assembly;
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
	// ⚠ 叠加段关（债-105 的出厂默认）时双臂**没有消费者**——叠加零 draw ⟹ 这条 INFO 若不加
	// 说明会被读成"双臂还在跑"（与 wireMeshletSwitch 的互斥告警同一条例）。
	const std::string_view orphan = (!cfg.meshletOverlay && cfg.meshletCoplanar != MeshletCoplanarMode::off)
		                                ? "；🔴 meshlet.overlay 关 ⟹ 叠加段零 draw，本档**没有消费者**"
		                                  "（要跑共面双臂请同时置 meshlet.overlay = true）"
		                                : "";
	if (auto* logger = RLLogger::getInstance())
		logger->info("meshlet: coplanar 双臂生效档 = {}（叠加 draw depthCompare = {}；"
					 "只作用叠加 draw，pass 与旧路径状态不动）{}",
			modeName, opName, orphan);
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
 * 「Renderer 装配后挂」三连（★M-1b 合成一次调用，main() 的 120 行红线；成员三件事的
 * 判据与出声口径各住在自己的函数里，这里只固定**顺序**：coplanar 双臂 → 逐簇剔除 →
 * MDI 行表载体指针。三者互不依赖，门关各自早退零日志）。
 * @param renderer 已装配的后端（assembleBackEnd 之后）
 * @param registry 场景 registry（wireMeshletCull 读取代生效事实）
 * @param cfg 运行配置
 * @param assembly 服务持有者（cullService / shadowArgs 两格的裸指针从这里挂给 Renderer）
 */
void wireMeshletRendererServices(entt::registry& registry, Renderer& renderer,
	const RuntimeConfig& cfg, MeshletAssembly& assembly) {
	wireMeshletOverlayDepthCompare(renderer, cfg);
	wireMeshletCull(renderer, registry, cfg, assembly.cullService.get());
	// ★M-1b：行表载体指针挂载。⚠ 非生效档判据（出厂态 mdi_shadow=false 载体照建、指针照挂，
	// 生效唯一事实 = registry ctx 的 MeshletMdiShadowActivation → 采集段 → 执行器首行判空）。
	renderer.setMeshletShadowArgsService(assembly.shadowArgs.get());
}

/**
 * 接口声明与完整口径 → main.cppm `wireMeshletPostRegistration`。本定义做五步：
 * 把「实体注册之后」的簇轴装配合成一次调用（`main()` 已在 120 行代码红线，
 * `wireSceneSpinContext` / `bakeMeshletEntries` 同法抽 helper，零行为变化）。
 * 顺序契约内聚在此：区间表**先**建（`wireMeshletSwitch` 的对账要读逐槽 l0Count），叠加段生效标记
 * 最后 emplace（债-105：它只读配置 + 资产表，不依赖前两步，但与前两步同属「注册之后」这一段）。
 * 🔴 第四步（★M-1a）必须排在第①步之后：MDI 行表载体的容量读的是**选级表**（存在即生效），
 * 而选级表是①里对账通过才 emplace 的。
 */
void wireMeshletPostRegistration(entt::registry& registry, const UploadedAssets& uploaded,
	const MeshletAssetStore& meshlets, const AssetTable& assetTable, ShaderRegistry& pipelines,
	Resources& resources,
	const RuntimeConfig& cfg, DeviceContext& device, MeshletAssembly& assembly) {
	wireClusterRangeTable(registry, uploaded, meshlets, cfg.meshletEnable, cfg.meshletCull,
		assembly.cullService.get(), assetTable, cfg, device);
	wireMeshletSwitch(registry, assetTable, cfg, device);
	wireMeshletOverlayActivation(registry, assetTable, cfg);
	// ④ ★M-1a：真 MDI 阴影段的行表 GPU 载体（零消费者；门关 = 连号都不领，零 INFO）。
	assembly.shadowArgs = wireMeshletShadowArgs(registry, resources, device, cfg);
	// ⑤ ★M-1b：真 MDI 条目烘焙 + 生效标记（🔴 必须紧跟④：argSlot() 此刻才是真值；而本函数
	// 整体仍在 assembleBackEnd 之前 → DGC/GpuCull 的 entries 快照含新条目，时序论证与三态
	// 结论 → wireMeshletMdiShadowActivation 头注释。门关 = 零条目零 INFO 零 ctx）。
	wireMeshletMdiShadowActivation(registry, pipelines, assetTable, assembly, cfg);
}

}  // namespace RGL
