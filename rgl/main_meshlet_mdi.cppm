module;
#include <entt/entt.hpp>
#include <fmt/format.h>

// 入口模块 main 的分区实现单元（module main:meshlet_mdi;）——阴影段**真 MDI 行表**的接线。
// 为什么从 main:meshlets 拆出（第四刀）：main:meshlets 是簇化轴的装配层接线（簇 buffer 描述符
// 环 + 逐簇条目烘焙 + 对账生效），本单元是同一根轴上的**另一种绘制形态**——行表住在 GPU 里、
// 阴影段改走 MDI/直派。两者的事实面不重叠：本单元读的是选级表与钉段表的**行数上界**（算容量），
// 摸的是 `MeshletShadowArgsService` 的环与 binding 34 的堆槽，产出是一枚**零消费者**的条目
// 加一个 ctx 生效标记；main:meshlets 摸的是 4 段簇 buffer 的 BDA 与逐簇条目。
// 接缝本来就写在源码里（★M-1a 那条 `// ─── ★M-1a` 分隔线起，到 ★1-a1ii-α 总入口之前止），
// 且四个函数互不引用 main:meshlets 的文件内符号——搬动只新增一条跨分区声明
// （`wireMeshletMdiShadowActivation`，与 `wireMeshletShadowArgs` 同法经 main.cppm）。
// 时序契约（第④步在注册后、第⑤步紧跟其后、整体仍在 assembleBackEnd 之前）**不随搬家改变**：
// 调用点仍在 main:meshlets 的 `wireMeshletPostRegistration` 里按原顺序调两个分区。
module main:meshlet_mdi;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式（MeshletAssembly）
import rlstd;
import rllogger;
import rlvk;  // vk::PhysicalDeviceMeshShaderPropertiesEXT（直派模拟的 grid 容量查询）
import vk_device;  // DeviceContext（受影响平台判据 + 物理设备属性）
import vk_resources;  // Resources（载体领槽与 VMA 分配）
import scene_context;  // entt ctx 类型完整化（find<MeshletShadowLodTable>）
import scene_data;  // kActiveShadowCascades（MDI 级联段数 = 本值 − 1，c0 恒旧路径）
import runtime_config;  // RuntimeConfig + resolveMdiShadowDirectEmu（三态键解析）
import diagnostics;  // diagnostics().shaderSelfcheck（自证与直派互斥的出声）
import meshlet_lod;  // MeshletShadowLodTable（载体的生效事实与「每段行数上界」的来源）
import shadow_proxy_types;  // ShadowProxyTable（N10 P3：钉段行也占 sliceRows 上界的一行）
import meshlet_shadow_args;  // MeshletShadowArgsService + logBakedRowMappingAudit（债-140 对账）
import mdi_rows;  // kMdiShadowRowBinding / mdiDirectEmuGridX / mdiDirectEmuGridFits
import render_list;  // MeshletMdiShadowActivation（第⑤步 emplace 的生效标记，采集侧读它）
import frame_session;  // FrameSession::kFramesInFlight（ring×2 的轮转节奏唯一真相源）
import asset_table;  // AssetTable（回填目标 = 「带 474 条目」的资产）
import shader_registry;  // ShaderRegistry（addShadowMeshletMdiEntry / bindShadowMeshletMdiToAsset）

namespace RGL {

// ─── ★M-1a：阴影段真 MDI 行表 GPU 载体的接线（零消费者，一条 draw 都不接）───────────
/**
 * 载体接线（§19.5 的 ★M-1 接缝）。四步，顺序即代码：
 *  ① 门关早退（`!meshletAssets` 或 `meshletShadowLod < 0`）= **连号都不领**（先例 =
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
 * shadow_meshlet.mesh.slang 的 ⚠ 注）的槽号**仍**从宿主条件链取（本服务构造 = assets +
 * shadow_lod + 选级表三判据，不因 mdi_shadow 改动）；「`= false` 时零消费者」升级为
 * 「`= false` 时零条目零 ctx，`= true` 而载体缺位时第⑤步 wireMeshletMdiShadowActivation
 * WARNING 出声」——静默空开关不成立。
 *
 * ## `sliceRows` 的闭式（每帧槽 = 所有级联段的并）
 * `MDI 级联段数 × 每段行数上界`，两个因子各有出处：
 *  · **段数 = `kActiveShadowCascades − 1`**（🔴 c0 恒旧路径，§18.6 第一刀零改动；IZB mask 重放
 *    按 8B push 读 c0 段靠的就是这条）⟹ c1..c3 = 3 段；
 *  · **每段行数上界 = 选级表里 `count > 0` 的 objectSlot 数 + N10 P3 钉段表里 `count > 0` 的
 *    objectSlot 数**：每个可切候选占**自己**一个 objectSlot（下标 = `MeshRef.objectSlot` = shader
 *    的 objectIndex），钉段候选再多占**一行**（行照发、texel 门只决定它的 gcX，口径必须与
 *    `mdiScanGate` 同式），所以它是「一段能有多少行」的**上界**而不是精确值——差的那几行正是
 *    doubleSided / blend 这些「有选级条目但不进 MDI」的实体（本机真产物 = 1591 上界 vs
 *    §19.5 的 1530 可切）。按上界建 ⟹ 结构上不可能
 *    越界写，而多出来的行恒 0（`gcX = 0` = 空转，§19.2 D4 的孔洞流本来就是合法形态）。
 *    🔴 不在这里重述 `collectShadowCasters` 的四条判据（那是第三处真相源）。
 * @param registry 场景 registry（读 `MeshletShadowLodTable` = 生效事实；🔴 必须注册之后）
 * @param resources 资源层（领槽 + VMA 分配器 + 描述符堆）
 * @param cfg       运行配置（读 `meshletAssets` / `meshletShadowLod` 两枚宿主条件）
 * @return 载体服务；门关 / 阴影簇路径未生效 / 上界为 0 = nullptr（零 buffer 零槽零日志）
 * @throw std::runtime_error 服务构造失败（领槽 / buffer / 描述符；启动期一次性路径）
 */
std::unique_ptr<MeshletShadowArgsService> wireMeshletShadowArgs(entt::registry& registry,
	Resources& resources, DeviceContext& device, const RuntimeConfig& cfg) {
	if (!cfg.meshletAssets || cfg.meshletShadowLod < 0)
		return nullptr;  // 门关 = 连号都不领
	const auto* lod = registry.ctx().find<MeshletShadowLodTable>();
	if (lod == nullptr)
		return nullptr;  // 阴影簇路径没生效（三条回落路都只出声不写配置，读事实不读意图）
	uint32_t rowsPerCascade = 0;
	for (const auto& entry : lod->bySlot)
		rowsPerCascade += entry.count > 0u ? 1u : 0u;
	// N10 P3 钉段行：有钉段的 objectSlot 每个**多占一行**（行照发、texel 门只决定 gcX，口径 →
	// render_list:mdi 的 mdiScanGate/mdiCascadeInputs，两处必须同式否则容量闸门会误回落）。
	// 门关 = ctx 没这张表 = 加 0 = sliceRows 逐位不变（载体容量本身不进锚，但少算会静默回落）。
	if (const auto* proxy = registry.ctx().find<ShadowProxyTable>(); proxy != nullptr)
		for (const auto& pin : proxy->bySlot)
			rowsPerCascade += pin.count > 0u ? 1u : 0u;
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
 * 债-140 §27 直派模拟的**裁决本体**（gfx1151 workaround，上游 Mesa#16670，判据 → §26/§27）：
 * 两脚 = 三态键 × 受影响设备（`resolveMdiShadowDirectEmu` 纯函数，tests/test_mdi_direct_emu 钉）
 * + 容量回落（grid 判负 = 退回 MDI 并出声，workaround 自己不许把画面搞没）+ 自证冲突出声。
 * 结果只喂一个消费者（`addShadowMeshletMdiEntry` → 条目 directTaskDispatch 位 + task SPV 变体，
 * 同源落位）。从 `wireMeshletMdiShadowActivation` 拆出 = 认知复杂度预算（同文件先例）。
 */
[[nodiscard]] static bool decideMdiShadowDirectEmu(const RuntimeConfig& cfg, bool affectedDevice,
	DeviceContext& device, const MeshletShadowArgsService& shadowArgs,
	RLLogger* logger) {
	bool directEmu = resolveMdiShadowDirectEmu(cfg.meshletMdiShadowDirect, affectedDevice);
	if (!directEmu)
		return false;
	const auto meshLimits = device.getPhysicalDevice()
		.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceMeshShaderPropertiesEXT>()
		.get<vk::PhysicalDeviceMeshShaderPropertiesEXT>();
	// 保守上界 = 整帧槽行数（每段 grid ≤ 本值；算式产地 = mdi_rows，测试钉）
	const uint32_t gridX = mdiDirectEmuGridX(shadowArgs.sliceRows());
	if (!mdiDirectEmuGridFits(gridX, meshLimits.maxTaskWorkGroupCount[0])) {
		if (logger)
			logger->warn("meshlet: mdi_shadow_direct 容量判负（grid X={} > "
						 "maxTaskWorkGroupCount[0] {}，VUID-TaskEXT-07322）—— 回落 MDI 形态",
				gridX, meshLimits.maxTaskWorkGroupCount[0]);
		return false;
	}
	if (diagnostics().shaderSelfcheck && logger)
		logger->warn("meshlet: shader_selfcheck 与 mdi_shadow_direct 同开 —— 自证**失效**"
					 "（task 自证变体只拼在 SV_DrawIndex 那份上，直派选 MDI_DIRECT 变体）；"
					 "要跑自证请把 mdi_shadow_direct 置 \"false\"");
	return true;
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
 *  · 键开而宿主不在（载体 nullptr = assets 关 / shadow_lod<0 / 选级表全零；或簇槽 nullopt）
 *    → **WARNING 出声**（配置说开了而实际没跑，同 `wireMeshletCull` 的第二道护栏）；
 *  · 有载体但零资产带 474 条目（理论不可达——载体成立要求选级表非零，而建表的资产判据同源）
 *    → 也出声，不静默不激活。
 * @param registry 场景 registry（生效标记 emplace 目标；🔴 与④同点调用 = 注册之后）
 * @param pipelines 管线表（addShadowMeshletMdiEntry / bindShadowMeshletMdiToAsset / 旧条目下标读回）
 * @param assets   资产表（回填目标 = 「带 474 条目」的资产；判据用字段本身，不复述 byFile 对账）
 * @param assembly 服务持有者（读④的 shadowArgs 与 α 的 bindingSlots）
 * @param cfg      运行配置（meshletMdiShadow 门 + meshletMdiShadowDirect 三态键）
 * @param device   设备上下文（债-140 §27 直派模拟两脚：受影响平台判据 + 网格容量查询）
 */

void wireMeshletMdiShadowActivation(entt::registry& registry, ShaderRegistry& pipelines,
	const AssetTable& assets, MeshletAssembly& assembly, const RuntimeConfig& cfg,
	DeviceContext& device) {
	if (!cfg.meshletMdiShadow)
		return;  // 门关 = 零条目零 INFO 零 ctx（A2 构造性）
	auto* logger = RLLogger::getInstance();
	if (!assembly.shadowArgs || !assembly.bindingSlots) {
		if (logger)
			logger->warn("meshlet: mdi_shadow 开但宿主不在（行表载体 {} / 簇 buffer 槽 {}）—— 阴影段"
						 "继续逐条簇 draw，本开关**不生效**。宿主条件 = meshlet.assets + "
						 "shadow_lod ≥ 0 + 选级表非零（§19.6：生效条件不含 mdi_shadow 本身）",
				assembly.shadowArgs ? "已建" : "未建", assembly.bindingSlots ? "已领" : "未领");
		return;
	}
	const auto& slots = *assembly.bindingSlots;
	const std::array<uint32_t, 4> clusterSlots{slots.vertexPool, slots.clusters, slots.tris,
		slots.remap};
	// ---- 债-140 §27 直派模拟裁决（gfx1151 workaround，判据 → §26/§27；裁决本体已抽函数）----
	const bool affectedDevice = device.isMdiMeshChainAffected();
	const bool directEmu
		= decideMdiShadowDirectEmu(cfg, affectedDevice, device, *assembly.shadowArgs, logger);
	// 条目幂等（同名命中返回既有下标）；argsSlot = ④ 领的行表堆槽，binding 34 与声明同向。
	const uint32_t mdiIdx = pipelines.addShadowMeshletMdiEntry(clusterSlots,
		assembly.shadowArgs->argSlot(), directEmu);
	// 🔴 债-140 第四击 Ⓐ：**烘入 mapping 对账**（补上 heap_slot_audit 自知的另一半盲区）。
	// 上一击审的是"服务那枚槽里的字节"，但 GPU 读的是**条目烘进去的那枚槽** —— 两条独立
	// 代码路径（服务 writeSlotDescriptor / 条目 bakeBindingMappings）给出各自的槽号，
	// 一旦分家就是"服务往 Y 写、GPU 从 X 读"，而 X 里可能是 0 = fault@0 的直接成因形态，
	// 且上一击**永远看不见**（它只审 Y）。判负不经门（两侧同源于一条代码路径，无假阳性）。
	logBakedRowMappingAudit(pipelines.entry(mdiIdx).pipeline.bakedMappings(),
	    "shadow_meshlet_mdi", kMdiShadowRowBinding, assembly.shadowArgs->argSlot(),
	    assembly.shadowArgs->descriptorBytes());
	// 🔴 夜间批补：Ⓐ 只审了 binding 34 一枚，而条目烘了 8 条 mapping。**任何一枚是零描述符
	// = 基址 0 = fault@0 的直接来源**，而 devcoredump 从不说 fault 是哪条 binding 触发的。
	// 登记下来让"逐帧审计"与"device-lost 现场 dump"两条路都能覆盖全部 8 枚。
	assembly.shadowArgs->registerEntrySlots(pipelines.entry(mdiIdx).pipeline.bakedMappings());
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
					 "count>0 的 objectSlot 数 × 级联段数，见 wireMeshletShadowArgs）；"
					 "direct_emu={}（0=off 1=on；键枚举 0=auto 1=true 2=false；受影响设备 "
					 "0/1；债-140 §27 gfx1151 workaround，Mesa#16670）",
			mdiIdx, oldIdx.value_or(0xFFFFFFFFu), pipelines.entriesView().size(), clusterSlots[0],
			clusterSlots[1], clusterSlots[2], clusterSlots[3], slots.clusterVisible,
			assembly.shadowArgs->argSlot(), assembly.shadowArgs->sliceRows(),
			assembly.shadowArgs->slices(), static_cast<uint32_t>(directEmu),
			static_cast<uint32_t>(cfg.meshletMdiShadowDirect),
			static_cast<uint32_t>(affectedDevice));
}

}  // namespace RGL
