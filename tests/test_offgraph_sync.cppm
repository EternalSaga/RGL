module;
/**
 * test_offgraph_sync —— 图外同步的申报即发射（批次 B1b → docs/compute_调研.md §7.2–§7.6）
 *
 * 被测 = `rgl/offgraph_sync.cppm`（rlgfx 成员）里**零设备**的那三面：usage 词表（掩码物化）、
 * 边表（命令流的唯一来源）、发射账（`recordEmission` / `checkEmission`）。`emit` 本体要
 * `CommandBuffer` ⟹ 不进本文件，由真程序变异覆盖（口径见文末）。
 *
 * ## 为什么值得测
 *
 * 这一层的卖点是「barrier 参数由词表物化，模块不写 mask 字面量」。这句话本身**就是一次语义
 * 改写**——迁移时只要有一个 bit 抄错，画面可能今天不变、换个驱动/换台机器才变。所以本文件的
 * 主要职责不是"测逻辑"，而是**钉死"迁移没改语义"**。
 *
 * ## 期望值的出处（不抄被测码）
 *
 * 全部掩码期望值 = **迁移前**各模块代码里 barrier 调用点的字面量：EVSSM 三处 =
 * `rgl/evssm_shadow_impl.cppm` 的 `stageBarrier(...)`；IZB 十三处 = `rgl/izb_shadow_impl.cppm`
 * 的 `bufferBarrier(...)`（两处 helper 现已删除，git 可查）；GpuCull 八处 =
 * `rgl/gpu_cull_impl.cppm` 的 `recordCullPass` / `recordFanoutDispatch` 里就地手写的八个
 * `vk::MemoryBarrier2` / `vk::BufferMemoryBarrier2` 现场（本就没有 helper，现已删除，git 可查）。
 * 应发次数同理：`mipLevels − 1` 来自 EVSSM ③ 那个循环的圈数（`level = 1; level < mipLevels`）。
 *
 * ## 变异红线（哪个变异让我红；答不出 = 摆设）
 *
 *   1. **词表改一位**：`computeReadWrite` 的 access 去掉 `eShaderWrite`、`anyWriteGlobal` 的
 *      stage 从 `eAllGraphics` 改成 `eComputeShader`、或 `priorAccessWideGfx` 补上 `eComputeShader`
 *      （把两条宽档并成一条）→ 「词表逐位」与「边物化出的掩码对」两条同时红。
 *   2. **边的 src/dst 互换**：`evssm_depth_to_compute` 的 src/dst 对调 → 边表用例红
 *      （对调后与旧代码的 src=AG / dst=CS 不符）。
 *   3. **边挂错门**：`izb_dump_head` 的 gate 从 `izbDump` 改成 `izb` → 边表用例红 +
 *      「子门关着却发」那条红（后者是 §7.5 第 1 条"条件边写两条边各挂各的门"的机器判据）。
 *   4. **对账少半边**：删 `active && got != want` → 「门开少发」红；删 `!active && got != 0`
 *      → 「门关却发」红（后者守逐位不变红线：门关必须零命令）。
 *   5. **只比"有没有"不比次数**：把 `got != want` 弱化成 `got == 0` → 「门开多发」那条红。
 *      它保证 L1 真的是**次数**账，而不是 B1a 那个布尔账的复制品。
 *   6. `beginFrame` 不清账 → 跨帧那条红（第二帧少发必须红）。
 *   7. `expectCount` 不生效（或 `expected` 初值被 `beginFrame` 一起清掉）→ 「构造期申报 7 次」
 *      那组里的两条红（发 7 次反而报违规 / 发 1 次就绿）。
 *   8. 门位表长度不校验 → 「长度不符」那条红（新增门而帧末没跟上 = 静默漏对账）。
 *   9. 词表养闲原子（加了没人用的条目）→ 「每个原子都被某条边用到」那条红。
 *  10. IZB 十三条里漏一条（发射点删掉而边表没删）→ 「IZB 门开着发够」那条红并点名漏的边。
 *  11. GpuCull 八条里漏一条 → 「cull 八条边全发够」那条红并点名漏的边；把 `cull_seq_fanout_chain`
 *      的 gate 从 `cullFanout` 改成 `cull`（子门并父门）→ 边表用例 + 「子门关着却发」那条同时红。
 *  12. 三个 `priorAccess*` 宽档并成一个（让 `priorAccessSceneData` 直接返回 Cs 那对掩码）→
 *      「词表逐位」里那条 **access 不等式**变回等式 ⟹ 红（scene data 的源侧没有 ShaderWrite、
 *      却有另一个写者的 TransferWrite，方向不同 = 不是同一件事）。
 *
 * 🔴 **本文件测不到的**（说清楚，别把"全绿"读成"同步对了"）：`emit` 里有没有忘调
 * `recordEmission`，以及"该发 barrier 的地方根本没写边"。前者由真程序变异覆盖（删一次
 * `sync.emit` ⟹ 帧末必须红，门开配方见 §7.1）；后者是 L2 的活，本层明确不做（§7.3）。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import rlstd 带进来
#include <string_view>

export module test_offgraph_sync;

import rlstd;
import rlvk;  // vk::PipelineStageFlagBits2 / vk::AccessFlagBits2（期望值字面量要用）
import offgraph_sync;
import vkerror;  // Error（expected 的失败侧）

namespace RGL {
namespace {

using Stage = vk::PipelineStageFlagBits2;
using Acc = vk::AccessFlagBits2;

/** 一帧的门位现场（默认全关 = 只有被点名的门开着）。 */
struct Gates {
	bool evssm = false;
	bool izb = false;
	bool izbDump = false;
	bool izbMask = false;
	bool cull = false;
	bool cullFanout = false;
};

/** 门位数组（顺序 = `RgSyncGate` 声明序；帧末真实现场用**具名赋值**，见 §7.5）。 */
constexpr std::array<bool, kSyncGateCount> gates(const Gates& g) {
	return {g.evssm, g.izb, g.izbDump, g.izbMask, g.cull, g.cullFanout};
}

/** 失败消息（绿 = 空串）；口径同 `test_offgraph_contract`。 */
std::string messageOf(const std::expected<void, Error>& result) {
	return result.has_value() ? std::string{} : result.error().message;
}

/** 发 N 次某条边（不碰命令缓冲：判据核经 `recordEmission` 驱动，见接口注释）。 */
void emitTimes(OffGraphSync& sync, RgSyncEdgeId edge, uint32_t times = 1) {
	for (uint32_t i = 0; i < times; ++i) sync.recordEmission(edge);
}

/** IZB 全部 13 条边（「IZB 门开着发够」那条按它逐个发；漏一个就是漏一条判据）。 */
constexpr RgSyncEdgeId kIzbEdges[] = {
	RgSyncEdgeId::izbHeadPriorToClear, RgSyncEdgeId::izbHeadClearToCreate,
	RgSyncEdgeId::izbHeadCreateToPrime, RgSyncEdgeId::izbNodesCreateToPrime,
	RgSyncEdgeId::izbMaskPriorToClear, RgSyncEdgeId::izbMaskClearToPass,
	RgSyncEdgeId::izbDumpHead, RgSyncEdgeId::izbDumpNodes, RgSyncEdgeId::izbDumpMask,
	RgSyncEdgeId::izbDumpStagingToHost, RgSyncEdgeId::izbHeadToMaskPass,
	RgSyncEdgeId::izbNodesToMaskPass, RgSyncEdgeId::izbMaskToLight,
};

/**
 * GpuCull 全部 8 条边（含挂在**子门** `cullFanout` 上的那条扇出边）。
 * 「cull 门开着发够」那条按它逐个发；「全发够」的现场门位因此是 `{.cull = true, .cullFanout = true}`。
 */
constexpr RgSyncEdgeId kCullEdges[] = {
	RgSyncEdgeId::cullHeapHostToReads, RgSyncEdgeId::cullScenePriorToCopy,
	RgSyncEdgeId::cullSceneCopyToReads, RgSyncEdgeId::cullSeqUploadToCreate,
	RgSyncEdgeId::cullSeqWriteToReadback, RgSyncEdgeId::cullCountWriteToReadback,
	RgSyncEdgeId::cullReadbackToHost, RgSyncEdgeId::cullSeqFanoutChain,
};

}  // namespace

TEST_CASE("offgraph_sync: usage 词表逐位钉死（期望值 = 迁移前各模块 barrier 字面量）") {
	// —— EVSSM（出处：evssm_shadow_impl.cppm 的 stageBarrier 三处调用点）——
	// E1 的 src：旧代码 `stageBarrier(cmd, eAllGraphics, eMemoryWrite, ...)`
	CHECK(syncUsageMasks(RgSyncUsage::anyWriteGlobal).stage == Stage::eAllGraphics);
	CHECK(syncUsageMasks(RgSyncUsage::anyWriteGlobal).access == Acc::eMemoryWrite);
	// E1 的 dst：`eComputeShader, eShaderRead`
	CHECK(syncUsageMasks(RgSyncUsage::computeRead).stage == Stage::eComputeShader);
	CHECK(syncUsageMasks(RgSyncUsage::computeRead).access == Acc::eShaderRead);
	// E2 / E3 的 src：`eComputeShader, eShaderWrite`
	CHECK(syncUsageMasks(RgSyncUsage::computeWrite).stage == Stage::eComputeShader);
	CHECK(syncUsageMasks(RgSyncUsage::computeWrite).access == Acc::eShaderWrite);
	// E2 的 dst：`eComputeShader, eShaderRead | eShaderWrite`（**两个位都要在**）
	CHECK(syncUsageMasks(RgSyncUsage::computeReadWrite).stage == Stage::eComputeShader);
	CHECK(syncUsageMasks(RgSyncUsage::computeReadWrite).access ==
		(Acc::eShaderRead | Acc::eShaderWrite));
	// E3 的 dst：`eFragmentShader, eShaderRead`（**FS 不是 AG**——IZB 的 AG/ShaderRead 是另一个原子）
	CHECK(syncUsageMasks(RgSyncUsage::fragmentRead).stage == Stage::eFragmentShader);
	CHECK(syncUsageMasks(RgSyncUsage::fragmentRead).access == Acc::eShaderRead);

	// —— IZB（出处：izb_shadow_impl.cppm 的 bufferBarrier 十三处调用点）——
	// 图形阶段三个原子：AG/SW（mask frag 写）、AG/SR（frag 读）、AG/SW|SR（清零后两侧都要）
	CHECK(syncUsageMasks(RgSyncUsage::graphicsWrite).stage == Stage::eAllGraphics);
	CHECK(syncUsageMasks(RgSyncUsage::graphicsWrite).access == Acc::eShaderWrite);
	CHECK(syncUsageMasks(RgSyncUsage::graphicsRead).stage == Stage::eAllGraphics);
	CHECK(syncUsageMasks(RgSyncUsage::graphicsRead).access == Acc::eShaderRead);
	CHECK(syncUsageMasks(RgSyncUsage::graphicsReadWrite).stage == Stage::eAllGraphics);
	CHECK(syncUsageMasks(RgSyncUsage::graphicsReadWrite).access ==
		(Acc::eShaderRead | Acc::eShaderWrite));
	// 传输与 host 三原子（dump 链：compute/frag 写 → 拷出读 → host 读 mapped 内存）
	CHECK(syncUsageMasks(RgSyncUsage::transferRead).stage == Stage::eTransfer);
	CHECK(syncUsageMasks(RgSyncUsage::transferRead).access == Acc::eTransferRead);
	CHECK(syncUsageMasks(RgSyncUsage::transferWrite).stage == Stage::eTransfer);
	CHECK(syncUsageMasks(RgSyncUsage::transferWrite).access == Acc::eTransferWrite);
	CHECK(syncUsageMasks(RgSyncUsage::hostRead).stage == Stage::eHost);
	CHECK(syncUsageMasks(RgSyncUsage::hostRead).access == Acc::eHostRead);
	// FillBuffer 那条的 dst：AllCommands / MemoryWrite
	CHECK(syncUsageMasks(RgSyncUsage::allCommandsWrite).stage == Stage::eAllCommands);
	CHECK(syncUsageMasks(RgSyncUsage::allCommandsWrite).access == Acc::eMemoryWrite);
	// 两条跨帧宽档：只差 compute 一档，**这一位就是"资源被谁碰过"这条信息本身**
	CHECK(syncUsageMasks(RgSyncUsage::priorAccessWideCs).stage
		== (Stage::eAllGraphics | Stage::eComputeShader | Stage::eTransfer));
	CHECK(syncUsageMasks(RgSyncUsage::priorAccessWideCs).access
		== (Acc::eShaderWrite | Acc::eShaderRead | Acc::eTransferRead));
	CHECK(syncUsageMasks(RgSyncUsage::priorAccessWideGfx).stage
		== (Stage::eAllGraphics | Stage::eTransfer));
	CHECK(syncUsageMasks(RgSyncUsage::priorAccessWideGfx).access
		== (Acc::eShaderWrite | Acc::eShaderRead | Acc::eTransferRead));
	// 阳性对照：两条宽档**必须**不同（并档变异在这里也红，且红得能看懂为什么）
	CHECK(syncUsageMasks(RgSyncUsage::priorAccessWideCs).stage
		!= syncUsageMasks(RgSyncUsage::priorAccessWideGfx).stage);

	// —— GpuCull（出处：迁移前 gpu_cull_impl.cppm 的 recordCullPass 七处 + recordFanoutDispatch
	// 一处现场）——
	// heapBarrier 的 src：旧代码 `eHost, eHostWrite`（本层唯一 host **写**侧原子）
	CHECK(syncUsageMasks(RgSyncUsage::hostWrite).stage == Stage::eHost);
	CHECK(syncUsageMasks(RgSyncUsage::hostWrite).access == Acc::eHostWrite);
	// heapBarrier 的 dst：四个 access **一个都不能少**——少哪一位，那种读法就失去排序
	CHECK(syncUsageMasks(RgSyncUsage::heapReadWide).stage == Stage::eAllGraphics);
	CHECK(syncUsageMasks(RgSyncUsage::heapReadWide).access
		== (Acc::eResourceHeapReadEXT | Acc::eSamplerHeapReadEXT | Acc::eTransferRead
			| Acc::eShaderRead));
	// preBarrier 的 src：AG|CS|TR / SR|TW（scene data 只被 shader 读、被另一条 CB 拷写）
	CHECK(syncUsageMasks(RgSyncUsage::priorAccessSceneData).stage
		== (Stage::eAllGraphics | Stage::eComputeShader | Stage::eTransfer));
	CHECK(syncUsageMasks(RgSyncUsage::priorAccessSceneData).access
		== (Acc::eShaderRead | Acc::eTransferWrite));
	// 阳性对照（变异 12）：三条宽档**不许并成一个**。sceneData 与 Cs 的 stage 恰好同宽，但
	// access 方向不同（少 SW|TR、多 TW）——那几位之差正是"谁碰过它、往哪个方向碰"本身。
	CHECK(syncUsageMasks(RgSyncUsage::priorAccessSceneData).access
		!= syncUsageMasks(RgSyncUsage::priorAccessWideCs).access);
	CHECK(syncUsageMasks(RgSyncUsage::heapReadWide).access
		!= syncUsageMasks(RgSyncUsage::graphicsRead).access);
	// 词表规模（守的变异 = 「加/删原子却没动本文件」：数字对不上立刻红，逼后来者为新原子补
	// 上面那组逐位断言，而不是只往枚举里塞一行就完事）
	CHECK(static_cast<uint32_t>(RgSyncUsage::count) == 17);

	// 变异 9：词表不养闲原子——每个原子至少被某条边的某一侧用到。加原子而没人用 = 白占一个
	// "看起来已覆盖"的名额，后面铺客户时会被当成现成的档乱用。
	for (uint32_t u = 0; u < static_cast<uint32_t>(RgSyncUsage::count); ++u) {
		const auto usage = static_cast<RgSyncUsage>(u);
		bool used = false;
		for (uint32_t e = 0; e < OffGraphSync::kEdgeCount; ++e) {
			const auto& desc = syncEdgeDesc(static_cast<RgSyncEdgeId>(e));
			used = used || desc.src == usage || desc.dst == usage;
		}
		CHECK_MESSAGE(used, "usage 原子未被任何边使用（要么接上客户，要么删掉这条）");
	}
}

TEST_CASE("offgraph_sync: 边表逐条钉死 = 旧代码 24 处 barrier 调用的等价形态") {
	struct Expect {
		RgSyncEdgeId id;
		std::string_view name;
		RgSyncScope scope;
		RgSyncGate gate;
		vk::PipelineStageFlags2 srcStage;  // 用 Flags<> 而非位枚举：`a | b` 物化出的就是 Flags<>
		vk::AccessFlags2 srcAccess;
		vk::PipelineStageFlags2 dstStage;
		vk::AccessFlags2 dstAccess;
	};
	using enum RgSyncGate;
	using enum RgSyncScope;
	// 逐字段出处：迁移前 evssm_shadow_impl.cppm 的 ①/③/④ 与 izb_shadow_impl.cppm 的 ②③④⑤⑦⑧
	// + beginMaskPass / endMaskPass 三处调用点，以及 gpu_cull_impl.cppm 的 recordCullPass
	// 七处（heap / scene 前后 / 序列上传 / 序列+count 读回 / host 读回）+ recordFanoutDispatch 一处。
	constexpr Expect expects[] = {
		{RgSyncEdgeId::evssmDepthToCompute, "evssm_depth_to_compute", executionOrder, evssm,
			Stage::eAllGraphics, Acc::eMemoryWrite, Stage::eComputeShader, Acc::eShaderRead},
		{RgSyncEdgeId::evssmMomentsChain, "evssm_moments_chain", executionOrder, evssm,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eComputeShader,
			Acc::eShaderRead | Acc::eShaderWrite},
		{RgSyncEdgeId::evssmMomentsToLight, "evssm_moments_to_light", executionOrder, evssm,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eFragmentShader, Acc::eShaderRead},
		{RgSyncEdgeId::izbHeadPriorToClear, "izb_head_prior_to_clear", buffer, izb,
			Stage::eAllGraphics | Stage::eComputeShader | Stage::eTransfer,
			Acc::eShaderWrite | Acc::eShaderRead | Acc::eTransferRead,
			Stage::eAllCommands, Acc::eMemoryWrite},
		{RgSyncEdgeId::izbHeadClearToCreate, "izb_head_clear_to_create", buffer, izb,
			Stage::eAllCommands, Acc::eMemoryWrite, Stage::eComputeShader, Acc::eShaderWrite},
		{RgSyncEdgeId::izbHeadCreateToPrime, "izb_head_create_to_prime", buffer, izb,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eComputeShader, Acc::eShaderRead},
		{RgSyncEdgeId::izbNodesCreateToPrime, "izb_nodes_create_to_prime", buffer, izb,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eComputeShader, Acc::eShaderRead},
		{RgSyncEdgeId::izbMaskPriorToClear, "izb_mask_prior_to_clear", buffer, izb,
			Stage::eAllGraphics | Stage::eTransfer,
			Acc::eShaderWrite | Acc::eShaderRead | Acc::eTransferRead,
			Stage::eAllCommands, Acc::eMemoryWrite},
		{RgSyncEdgeId::izbMaskClearToPass, "izb_mask_clear_to_pass", buffer, izb,
			Stage::eAllCommands, Acc::eMemoryWrite, Stage::eAllGraphics,
			Acc::eShaderWrite | Acc::eShaderRead},
		{RgSyncEdgeId::izbDumpHead, "izb_dump_head", buffer, izbDump,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eTransfer, Acc::eTransferRead},
		{RgSyncEdgeId::izbDumpNodes, "izb_dump_nodes", buffer, izbDump,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eTransfer, Acc::eTransferRead},
		{RgSyncEdgeId::izbDumpMask, "izb_dump_mask", buffer, izbDump,
			Stage::eAllGraphics, Acc::eShaderWrite, Stage::eTransfer, Acc::eTransferRead},
		{RgSyncEdgeId::izbDumpStagingToHost, "izb_dump_staging_to_host", buffer, izbDump,
			Stage::eTransfer, Acc::eTransferWrite, Stage::eHost, Acc::eHostRead},
		{RgSyncEdgeId::izbHeadToMaskPass, "izb_head_to_mask_pass", buffer, izbMaskSegment,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eAllGraphics, Acc::eShaderRead},
		{RgSyncEdgeId::izbNodesToMaskPass, "izb_nodes_to_mask_pass", buffer, izbMaskSegment,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eAllGraphics, Acc::eShaderRead},
		{RgSyncEdgeId::izbMaskToLight, "izb_mask_to_light", buffer, izbMaskSegment,
			Stage::eAllGraphics, Acc::eShaderWrite, Stage::eAllGraphics, Acc::eShaderRead},
		// —— GpuCull 八条（出处：迁移前 gpu_cull_impl.cppm 的 recordCullPass 七处 +
		// recordFanoutDispatch 一处，逐条对应当时的字面量）——
		{RgSyncEdgeId::cullHeapHostToReads, "cull_heap_host_to_reads", executionOrder, cull,
			Stage::eHost, Acc::eHostWrite, Stage::eAllGraphics,
			Acc::eResourceHeapReadEXT | Acc::eSamplerHeapReadEXT | Acc::eTransferRead
				| Acc::eShaderRead},
		{RgSyncEdgeId::cullScenePriorToCopy, "cull_scene_prior_to_copy", buffer, cull,
			Stage::eAllGraphics | Stage::eComputeShader | Stage::eTransfer,
			Acc::eShaderRead | Acc::eTransferWrite, Stage::eTransfer, Acc::eTransferWrite},
		{RgSyncEdgeId::cullSceneCopyToReads, "cull_scene_copy_to_reads", buffer, cull,
			Stage::eTransfer, Acc::eTransferWrite, Stage::eAllGraphics, Acc::eShaderRead},
		{RgSyncEdgeId::cullSeqUploadToCreate, "cull_seq_upload_to_create", buffer, cull,
			Stage::eTransfer, Acc::eTransferWrite, Stage::eComputeShader, Acc::eShaderWrite},
		{RgSyncEdgeId::cullSeqWriteToReadback, "cull_seq_write_to_readback", buffer, cull,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eTransfer, Acc::eTransferRead},
		// 与上一条四条掩码**逐位相同**（旧代码就是 `cntBarrier = seqBarrier` 只换 buffer），
		// 但资源不同 ⟹ 刻意两条边（漏发时点名资源），先例 = izb_nodes_create_to_prime
		{RgSyncEdgeId::cullCountWriteToReadback, "cull_count_write_to_readback", buffer, cull,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eTransfer, Acc::eTransferRead},
		{RgSyncEdgeId::cullReadbackToHost, "cull_readback_to_host", executionOrder, cull,
			Stage::eTransfer, Acc::eTransferWrite, Stage::eHost, Acc::eHostRead},
		// 唯一挂**子门**的一条：`recordFanoutDispatch` 只在 nEntryStreams>0 时被调
		{RgSyncEdgeId::cullSeqFanoutChain, "cull_seq_fanout_chain", buffer, cullFanout,
			Stage::eComputeShader, Acc::eShaderWrite, Stage::eComputeShader,
			Acc::eShaderRead | Acc::eShaderWrite},
	};
	// 边数也要钉：加边不写用例 = 新边没人验（这条红比"某条边写错"更常见）。
	REQUIRE(std::size(expects) == OffGraphSync::kEdgeCount);

	for (const auto& want : expects) {
		INFO("edge #" << static_cast<int>(want.id) << " = " << want.name);
		const auto& desc = syncEdgeDesc(want.id);
		CHECK(desc.name == want.name);
		CHECK(desc.scope == want.scope);
		// 变异 3：挂错门在这里红（EVSSM 三条挂 evssm；IZB 的 recordFrame 六条挂 izb、
		// dump 四条挂 izbDump、mask pass 三条挂 izbMaskSegment）
		CHECK(desc.gate == want.gate);
		// 🔴 判据本体：边**物化出的四条掩码** == 旧代码那四条。不是只钉"边指向哪两个原子"——
		// 那还要再经过一次词表，红了得两处对照才知道哪一处错。
		CHECK(syncUsageMasks(desc.src).stage == want.srcStage);
		CHECK(syncUsageMasks(desc.src).access == want.srcAccess);
		CHECK(syncUsageMasks(desc.dst).stage == want.dstStage);
		CHECK(syncUsageMasks(desc.dst).access == want.dstAccess);
	}
}

TEST_CASE("offgraph_sync: 门开着 ⟺ 发够次数（L1 的两个半边）") {
	const auto on = gates({.evssm = true});
	OffGraphSync sync;

	// 阳性锚点：默认应发次数（边表全是 1）全发够 ⟹ 必须绿
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight);
	CHECK(messageOf(sync.checkEmission(std::span<const bool>{on})).empty());

	// 半边 A：门开着少发 = 漏同步 = 未定义行为，必须当场红并点名边
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight);
	const auto low = messageOf(sync.checkEmission(std::span<const bool>{on}));
	REQUIRE_FALSE(low.empty());
	CHECK(low.find("evssm_moments_chain") != std::string::npos);

	// 变异 5：把"次数"弱化成"有没有"，下面这条是唯一能抓住它的用例
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute, 2);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight);
	CHECK_FALSE(sync.checkEmission(std::span<const bool>{on}).has_value());
}

TEST_CASE("offgraph_sync: IZB 十三条边全发够才绿；漏一条就点名那条") {
	const auto on = gates({.izb = true, .izbDump = true, .izbMask = true});
	OffGraphSync sync;

	// 阳性锚点：IZB 三道门全开、13 条各发一次 ⟹ 绿（IZB 没有循环边，应发次数全是 1）
	sync.beginFrame();
	for (const auto edge : kIzbEdges) emitTimes(sync, edge);
	CHECK(messageOf(sync.checkEmission(std::span<const bool>{on})).empty());
	CHECK(std::size(kIzbEdges) == 13);

	// 变异 10：漏一条（发射点被删而边表还在）。逐条漏一遍 = 每条边都被判据覆盖过，
	// 而不是"有一条被覆盖了所以整表大概没问题"。
	for (const auto skip : kIzbEdges) {
		sync.beginFrame();
		for (const auto edge : kIzbEdges) {
			if (edge != skip) emitTimes(sync, edge);
		}
		const auto msg = messageOf(sync.checkEmission(std::span<const bool>{on}));
		REQUIRE_FALSE(msg.empty());
		INFO("漏掉的边：" << syncEdgeDesc(skip).name);
		CHECK(msg.find(syncEdgeDesc(skip).name) != std::string::npos);
	}
}

TEST_CASE("offgraph_sync: cull 八条边全发够才绿；逐条漏发各点名一条") {
	// 现场门位 = 两道门全开（八条里有 1 条挂在子门 cullFanout 上，见边表）
	const auto on = gates({.cull = true, .cullFanout = true});
	OffGraphSync sync;

	// 阳性锚点：cull 两道门全开、8 条各发一次 ⟹ 绿（cull 没有循环边，应发次数全是 1）
	sync.beginFrame();
	for (const auto edge : kCullEdges) emitTimes(sync, edge);
	CHECK(messageOf(sync.checkEmission(std::span<const bool>{on})).empty());
	CHECK(std::size(kCullEdges) == 8);

	// 变异 11：漏一条（发射点被删而边表还在）。逐条漏一遍 = 每条边都被判据覆盖过。
	for (const auto skip : kCullEdges) {
		sync.beginFrame();
		for (const auto edge : kCullEdges) {
			if (edge != skip) emitTimes(sync, edge);
		}
		const auto msg = messageOf(sync.checkEmission(std::span<const bool>{on}));
		REQUIRE_FALSE(msg.empty());
		INFO("漏掉的边：" << syncEdgeDesc(skip).name);
		CHECK(msg.find(syncEdgeDesc(skip).name) != std::string::npos);
	}
}

TEST_CASE("offgraph_sync: 子门 cullFanout 关着却发了 = 红（cull 侧的条件边）") {
	// 现场：cull 档在跑，但静态 DGC 流**没建**（`dgcStatic` 关 / NV 路 = 出厂常见形态）
	const auto parentOnly = gates({.cull = true});
	OffGraphSync sync;

	// 阳性锚点：只发 cull 门那 7 条 ⟹ 绿（子门关 = 扇出边必须零条）
	sync.beginFrame();
	for (const auto edge : kCullEdges) {
		if (edge != RgSyncEdgeId::cullSeqFanoutChain) emitTimes(sync, edge);
	}
	CHECK(messageOf(sync.checkEmission(std::span<const bool>{parentOnly})).empty());

	// 子门关着却发了那条 ⟹ 红且点名（= 没建流的 cull 档命令流多了一条 barrier）
	sync.beginFrame();
	for (const auto edge : kCullEdges) emitTimes(sync, edge);
	const auto bad = messageOf(sync.checkEmission(std::span<const bool>{parentOnly}));
	REQUIRE_FALSE(bad.empty());
	CHECK(bad.find("cull_seq_fanout_chain") != std::string::npos);

	// 反向：子门开着却没发 ⟹ 同样红（流建了 = 那条边一条不能少）
	const auto withFanout = gates({.cull = true, .cullFanout = true});
	sync.beginFrame();
	for (const auto edge : kCullEdges) {
		if (edge != RgSyncEdgeId::cullSeqFanoutChain) emitTimes(sync, edge);
	}
	const auto low = messageOf(sync.checkEmission(std::span<const bool>{withFanout}));
	REQUIRE_FALSE(low.empty());
	CHECK(low.find("cull_seq_fanout_chain") != std::string::npos);
}

TEST_CASE("offgraph_sync: 子门（dump / mask 段）关着却发了 = 红（§7.5 条件边的机器判据）") {
	// 现场：IZB 本体开着，但本帧**不是** dump 帧、shadow0 段也空（= 出厂 IZB 档的常态）
	const auto parentOnly = gates({.izb = true});
	OffGraphSync sync;

	// 阳性锚点：只发 izb 门那 6 条 ⟹ 绿（子门关 = 子边必须零条，这正是"条件边写两条边"
	// 的形态：dump 边不靠"次数写 0/4"表达，而是挂在自己的门上）
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::izbHeadPriorToClear);
	emitTimes(sync, RgSyncEdgeId::izbHeadClearToCreate);
	emitTimes(sync, RgSyncEdgeId::izbHeadCreateToPrime);
	emitTimes(sync, RgSyncEdgeId::izbNodesCreateToPrime);
	emitTimes(sync, RgSyncEdgeId::izbMaskPriorToClear);
	emitTimes(sync, RgSyncEdgeId::izbMaskClearToPass);
	CHECK(messageOf(sync.checkEmission(std::span<const bool>{parentOnly})).empty());

	// 子门关着却发了一条 dump 边 ⟹ 红且点名那条边（= 门关时命令流多了一条 barrier）
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::izbHeadPriorToClear);
	emitTimes(sync, RgSyncEdgeId::izbHeadClearToCreate);
	emitTimes(sync, RgSyncEdgeId::izbHeadCreateToPrime);
	emitTimes(sync, RgSyncEdgeId::izbNodesCreateToPrime);
	emitTimes(sync, RgSyncEdgeId::izbMaskPriorToClear);
	emitTimes(sync, RgSyncEdgeId::izbMaskClearToPass);
	emitTimes(sync, RgSyncEdgeId::izbDumpHead);
	const auto bad = messageOf(sync.checkEmission(std::span<const bool>{parentOnly}));
	REQUIRE_FALSE(bad.empty());
	CHECK(bad.find("izb_dump_head") != std::string::npos);

	// 反向：子门开着却没发 ⟹ 同样红（dump 门开 = 四条一条不能少）
	const auto withDump = gates({.izb = true, .izbDump = true});
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::izbHeadPriorToClear);
	emitTimes(sync, RgSyncEdgeId::izbHeadClearToCreate);
	emitTimes(sync, RgSyncEdgeId::izbHeadCreateToPrime);
	emitTimes(sync, RgSyncEdgeId::izbNodesCreateToPrime);
	emitTimes(sync, RgSyncEdgeId::izbMaskPriorToClear);
	emitTimes(sync, RgSyncEdgeId::izbMaskClearToPass);
	CHECK_FALSE(sync.checkEmission(std::span<const bool>{withDump}).has_value());
}

TEST_CASE("offgraph_sync: 门关着必须零命令（逐位不变红线的同步侧）") {
	const auto off = gates({});  // 六道门全关
	OffGraphSync sync;
	// 门关 + 一条没发 = 唯一合法形态
	sync.beginFrame();
	CHECK(sync.checkEmission(std::span<const bool>{off}).has_value());

	// 门关却发了 = 门关时命令流多了一条 barrier = 破逐位不变
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight);
	const auto bad = messageOf(sync.checkEmission(std::span<const bool>{off}));
	REQUIRE_FALSE(bad.empty());
	CHECK(bad.find("evssm_moments_to_light") != std::string::npos);

	// IZB 侧同型（首发客户红过不代表别的客户也红）
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::izbMaskToLight);
	const auto bad2 = messageOf(sync.checkEmission(std::span<const bool>{off}));
	REQUIRE_FALSE(bad2.empty());
	CHECK(bad2.find("izb_mask_to_light") != std::string::npos);

	// cull 侧同型：`cull_replay=cpu` 档（出厂默认）整条 GPU 集不上 CB ⟹ 八条一条都不该发，
	// 发一条就是「门关时命令流多了一条 barrier」= 破逐位不变
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::cullReadbackToHost);
	const auto bad3 = messageOf(sync.checkEmission(std::span<const bool>{off}));
	REQUIRE_FALSE(bad3.empty());
	CHECK(bad3.find("cull_readback_to_host") != std::string::npos);
}

TEST_CASE("offgraph_sync: 构造期申报的次数跨帧生效，本帧计数每帧清零") {
	const auto on = gates({.evssm = true});
	OffGraphSync sync;
	// EVSSM 构造期那句 `expectCount(chain, mipLevels - 1)` 的等价现场（mipLevels = 8）
	sync.expectCount(RgSyncEdgeId::evssmMomentsChain, 7);

	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain, 7);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight);
	CHECK(messageOf(sync.checkEmission(std::span<const bool>{on})).empty());
	CHECK(sync.emitted(RgSyncEdgeId::evssmMomentsChain) == 7);

	// 变异 6：`beginFrame` 若不清账，第二帧会拿上一帧剩下的 7 被读成"够"
	sync.beginFrame();
	CHECK(sync.emitted(RgSyncEdgeId::evssmMomentsChain) == 0);
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain, 6);  // 循环少跑一圈（mip 数被改动）
	CHECK_FALSE(sync.checkEmission(std::span<const bool>{on}).has_value());

	// 变异 7：应发次数是**启动期**事实，若被 beginFrame 一起清回 1，这条会误判成绿
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight);
	CHECK_FALSE(sync.checkEmission(std::span<const bool>{on}).has_value());
}

TEST_CASE("offgraph_sync: 门位表长度不符即错（新增门而帧末没跟上 = 静默漏对账）") {
	const auto on = gates({.evssm = true});
	OffGraphSync sync;
	// 先造一个"次数全发够"的现场，好让下面两条红只可能来自长度这一件事。
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight);
	CHECK(messageOf(sync.checkEmission(std::span<const bool>{on})).empty());

	// 多一位 = 新增了门而帧末门位表没扩。此时次数**是够的**，所以"删掉长度校验"这个变异
	// 会让下面这条变绿（gateActive[0] 仍读到 true）⟹ 它真的在守东西。
	const auto tooMany = [] {
		std::array<bool, kSyncGateCount + 1> a{};
		a[0] = true;
		return a;
	}();
	const auto bad = messageOf(sync.checkEmission(std::span<const bool>{tooMany}));
	REQUIRE_FALSE(bad.empty());
	// 消息必须点名**长度**，不能和"次数不符"共用一句——否则红了还得猜是哪种错
	CHECK(bad.find("gateActive") != std::string::npos);

	// 少一位同理（长度校验被删时这里会读越界 ⟹ 这条守的是"别静默"）
	const std::array<bool, kSyncGateCount - 1> tooFew{};
	CHECK_FALSE(sync.checkEmission(std::span<const bool>{tooFew}).has_value());
}

}  // namespace RGL
