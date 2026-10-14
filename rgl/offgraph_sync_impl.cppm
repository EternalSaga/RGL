module;
#include <vulkan/vulkan.h>  // VkBuffer / VK_WHOLE_SIZE
#include <fmt/format.h>
module offgraph_sync:impl;

import offgraph_sync;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import vkerror;

namespace RGL {

namespace {

/** 统一错误构造：`ErrorCode::RenderGraphInvalid` + message（同 B1a 的 `offgraph_contract`）。 */
Error syncError(std::string message) {
	return Error{ErrorCode::RenderGraphInvalid, std::move(message)};
}

}  // namespace

RgSyncMasks syncUsageMasks(RgSyncUsage usage) {
	using Stage = vk::PipelineStageFlagBits2;
	using Acc = vk::AccessFlagBits2;
	switch (usage) {
	case RgSyncUsage::computeWrite:
		return {Stage::eComputeShader, Acc::eShaderWrite};
	case RgSyncUsage::computeRead:
		return {Stage::eComputeShader, Acc::eShaderRead};
	case RgSyncUsage::computeReadWrite:
		return {Stage::eComputeShader, Acc::eShaderRead | Acc::eShaderWrite};
	case RgSyncUsage::fragmentRead:
		return {Stage::eFragmentShader, Acc::eShaderRead};
	case RgSyncUsage::graphicsWrite:
		return {Stage::eAllGraphics, Acc::eShaderWrite};
	case RgSyncUsage::graphicsRead:
		return {Stage::eAllGraphics, Acc::eShaderRead};
	case RgSyncUsage::graphicsReadWrite:
		return {Stage::eAllGraphics, Acc::eShaderRead | Acc::eShaderWrite};
	case RgSyncUsage::transferRead:
		return {Stage::eTransfer, Acc::eTransferRead};
	case RgSyncUsage::transferWrite:
		return {Stage::eTransfer, Acc::eTransferWrite};
	case RgSyncUsage::hostRead:
		return {Stage::eHost, Acc::eHostRead};
	case RgSyncUsage::hostWrite:
		// host 侧**写**：descriptor heap 的条目由 CPU 侧 `writeResourceDescriptorsEXT` 填进 heap
		// （`GpuCull::writeHeapDescriptors`）。本层此前只有 hostRead（dump 读 mapped 内存），
		// host 写侧是 cull 那条 heap 序才需要的第一个原子。
		return {Stage::eHost, Acc::eHostWrite};
	case RgSyncUsage::anyWriteGlobal:
		// 宽档：EVSSM 的阴影图**本模块无句柄**（归 RT 缓存），depth-write → compute-read
		// 这条序只能表达成"全局写 → compute 读"，access 用 eMemoryWrite 通配。
		// ⚠ 不许悄悄当默认档用——它比真实访问宽，宽出来的部分是本模块白付的同步成本。
		return {Stage::eAllGraphics, Acc::eMemoryWrite};
	case RgSyncUsage::allCommandsWrite:
		// 宽档：生产者是一条 `vkCmdFillBuffer`（IZB 的 head/mask 每帧清零）。清零是 API 命令、
		// 不是 shader 访问，dst 侧要覆盖"接下来谁都可能先碰它"，所以 stage 给 AllCommands。
		return {Stage::eAllCommands, Acc::eMemoryWrite};
	case RgSyncUsage::priorAccessWideCs:
		// 宽档（跨帧）：head 是**单份跨 frames-in-flight 共享**的 buffer，本帧的 FillBuffer
		// 与上一帧对它的 compute 写 / mask pass frag 读 / dump 拷读之间必须有声明过的执行依赖
		// （债-30 方案 C 暴露）。src 三档全给 = 上一帧最后的访问点取决于 dump 门开没开，
		// 拿不准的一律纳入；代价 = 每帧一条 barrier，换"不靠上一帧恰好排空了"。
		return {Stage::eAllGraphics | Stage::eComputeShader | Stage::eTransfer,
			Acc::eShaderWrite | Acc::eShaderRead | Acc::eTransferRead};
	case RgSyncUsage::priorAccessWideGfx:
		// 同上，但 mask 位图**不经 compute**（frag 写 + 拷出读）⟹ 少 CS 一档。
		// 这两条刻意分开：省掉的那一位正是"这个资源到底被谁碰过"。
		return {Stage::eAllGraphics | Stage::eTransfer,
			Acc::eShaderWrite | Acc::eShaderRead | Acc::eTransferRead};
	case RgSyncUsage::heapReadWide:
		// 宽档（dst 侧）：消费者是 **VK_EXT_descriptor_heap** 的堆对象。它是一个扩展对象、
		// 不是本层能命名句柄的 buffer ⟹ 这条序**表达不成 buffer 档**，只能走无句柄执行序
		// （`RgSyncScope::executionOrder`）。四个 access 各对应 heap 的一种读法：资源堆读 /
		// 采样器堆读 / 把 heap 当拷贝源读 / 着色器经 heap 地址读——少给一位 = 那种读法失去排序。
		return {Stage::eAllGraphics,
			Acc::eResourceHeapReadEXT | Acc::eSamplerHeapReadEXT | Acc::eTransferRead
				| Acc::eShaderRead};
	case RgSyncUsage::priorAccessSceneData:
		// 宽档（跨帧 + 跨命令缓冲）：scene data 是**单份**跨 frames-in-flight 共享的 buffer，
		// 而每帧有**两个写者**（主 CB 的 recordFixedDataPath 与 cull CB 本处）⟹ 本帧 copy 与
		// "上一帧对它的 shader 读 / 另一条 CB 的 TransferWrite"之间必须自建依赖。与
		// `priorAccessWideCs` 的差别在 access 的**方向**：scene data 不被 shader 写、也不被
		// 拷出读，但会被另一条 CB 拷写 ⟹ 少 SW|TR、多 TransferWrite。
		return {Stage::eAllGraphics | Stage::eComputeShader | Stage::eTransfer,
			Acc::eShaderRead | Acc::eTransferWrite};
	default:
		// 只可能是 `count`（遍历哨兵）。写出来而不是留空：新增原子忘了配掩码时，
		// 这里会静默给出 eTopOfPipe/eNone = 一条**什么都不挡**的 barrier，那种错必须靠
		// 单测「词表逐位」那条红抓住，而不是靠编译器（编译器只会给个 -Wswitch 警告）。
		return {Stage::eTopOfPipe, Acc::eNone};
	}
}

namespace {

/**
 * 边表（唯一真相源的数据面；`syncEdgeDesc` 只是它的访问器）。
 *
 * 顺序必须与 `RgSyncEdgeId` 逐行对齐。表长 = 枚举 `count` 是编译期绑定，但**少写一行不会
 * 编译失败**（剩余元素值初始化成空 name）⟹ 真正的护栏是单测「边表逐条钉死」：它按每个
 * 边 id 断言 name 与四条掩码，漏行会红在"name 是空的"上（`REQUIRE` 还钉住了行数）。
 *
 * 为什么住文件作用域而不是函数内 `static`：表有 24 行 × 6 字段，塞进访问函数会让它因为
 * **数据**而不是逻辑撞上函数长度红线；把表拆成两个函数更糟——「顺序与枚举逐行对齐」这条
 * 契约就会跨两个函数体，比一个长函数更难核对。
 */
constexpr std::array<RgSyncEdgeDesc, static_cast<size_t>(RgSyncEdgeId::count)> kEdgeTable = {
	RgSyncEdgeDesc{
		.name = "evssm_depth_to_compute",
		.scope = RgSyncScope::executionOrder,
		.src = RgSyncUsage::anyWriteGlobal,
		.dst = RgSyncUsage::computeRead,
		.gate = RgSyncGate::evssm,
		.expectedPerFrame = 1,
	},
	RgSyncEdgeDesc{
		.name = "evssm_moments_chain",
		.scope = RgSyncScope::executionOrder,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::computeReadWrite,
		.gate = RgSyncGate::evssm,
		// 默认 1；真实次数 = mipLevels−1，由 `EvssmShadow` 构造期 `expectCount` 覆写
		// （构造期而不是录制期：录制期申报 = 自己出题自己答，判据会自证）。
		.expectedPerFrame = 1,
	},
	RgSyncEdgeDesc{
		.name = "evssm_moments_to_light",
		.scope = RgSyncScope::executionOrder,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::fragmentRead,
		.gate = RgSyncGate::evssm,
		.expectedPerFrame = 1,
	},
	// —— IzbShadow（B1b 第二刀）。13 条全是 buffer 档：句柄由发射点给（词表不持资源）。
	// ②③④⑤⑦⑧ 的编号沿用 `recordFrame` 里的注释序号，红了能直接对着那段代码读。
	RgSyncEdgeDesc{
		.name = "izb_head_prior_to_clear",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::priorAccessWideCs,
		.dst = RgSyncUsage::allCommandsWrite,
		.gate = RgSyncGate::izb,
	},
	RgSyncEdgeDesc{
		.name = "izb_head_clear_to_create",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::allCommandsWrite,
		.dst = RgSyncUsage::computeWrite,
		.gate = RgSyncGate::izb,
	},
	RgSyncEdgeDesc{
		.name = "izb_head_create_to_prime",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::computeRead,
		.gate = RgSyncGate::izb,
	},
	// 与上一条 masks **逐位相同**，但资源不同（nodes）。刻意不并成一条 expected=2 的边：
	// 并了以后"漏了 nodes 那条"只会报"少发一次"，而分开两条直接点名是哪个资源。
	RgSyncEdgeDesc{
		.name = "izb_nodes_create_to_prime",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::computeRead,
		.gate = RgSyncGate::izb,
	},
	RgSyncEdgeDesc{
		.name = "izb_mask_prior_to_clear",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::priorAccessWideGfx,
		.dst = RgSyncUsage::allCommandsWrite,
		.gate = RgSyncGate::izb,
	},
	RgSyncEdgeDesc{
		.name = "izb_mask_clear_to_pass",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::allCommandsWrite,
		.dst = RgSyncUsage::graphicsReadWrite,
		.gate = RgSyncGate::izb,
	},
	RgSyncEdgeDesc{
		.name = "izb_dump_head",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::transferRead,
		.gate = RgSyncGate::izbDump,
	},
	RgSyncEdgeDesc{
		.name = "izb_dump_nodes",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::transferRead,
		.gate = RgSyncGate::izbDump,
	},
	RgSyncEdgeDesc{
		.name = "izb_dump_mask",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::graphicsWrite,
		.dst = RgSyncUsage::transferRead,
		.gate = RgSyncGate::izbDump,
	},
	RgSyncEdgeDesc{
		.name = "izb_dump_staging_to_host",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::transferWrite,
		.dst = RgSyncUsage::hostRead,
		.gate = RgSyncGate::izbDump,
	},
	RgSyncEdgeDesc{
		.name = "izb_head_to_mask_pass",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::graphicsRead,
		.gate = RgSyncGate::izbMaskSegment,
	},
	RgSyncEdgeDesc{
		.name = "izb_nodes_to_mask_pass",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::graphicsRead,
		.gate = RgSyncGate::izbMaskSegment,
	},
	RgSyncEdgeDesc{
		.name = "izb_mask_to_light",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::graphicsWrite,
		.dst = RgSyncUsage::graphicsRead,
		.gate = RgSyncGate::izbMaskSegment,
	},
	// —— GpuCull（B1b 第三刀）。6 条 buffer 档 + 2 条无句柄执行序档（heap 与 host 读回
	// 都不是"某个 buffer 的序"）。发射点 = `GpuCull::recordCullPass` / `recordFanoutDispatch`。
	// ① heap：host 写 heap 条目 → 本 CB 之后所有图形阶段对 heap 的读。宽档 `heapReadWide`
	//    的成因见 usage 表；scope 必须是 executionOrder——heap 不是 buffer，交不出句柄。
	RgSyncEdgeDesc{
		.name = "cull_heap_host_to_reads",
		.scope = RgSyncScope::executionOrder,
		.src = RgSyncUsage::hostWrite,
		.dst = RgSyncUsage::heapReadWide,
		.gate = RgSyncGate::cull,
	},
	// ② scene data 拷贝**前**的跨帧 + 跨 CB 依赖（宽档 `priorAccessSceneData` 的成因见
	//    usage 表；现场推理逐字留在 gpu_cull_impl.cppm 的发射点注释里）。
	RgSyncEdgeDesc{
		.name = "cull_scene_prior_to_copy",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::priorAccessSceneData,
		.dst = RgSyncUsage::transferWrite,
		.gate = RgSyncGate::cull,
	},
	// ③ 拷贝落定 → 图形阶段读（compute 经对象矩阵表读，dst 给 AG/ShaderRead）
	RgSyncEdgeDesc{
		.name = "cull_scene_copy_to_reads",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::transferWrite,
		.dst = RgSyncUsage::graphicsRead,
		.gate = RgSyncGate::cull,
	},
	// ④ 序列 CPU 基值拷入 → cull dispatch 覆盖 meshCmd.x（dst 是**写**不是读：kernel
	//    随后写同一行，WAR/WAW 都要挡住）
	RgSyncEdgeDesc{
		.name = "cull_seq_upload_to_create",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::transferWrite,
		.dst = RgSyncUsage::computeWrite,
		.gate = RgSyncGate::cull,
	},
	RgSyncEdgeDesc{
		.name = "cull_seq_write_to_readback",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::transferRead,
		.gate = RgSyncGate::cull,
	},
	// 与上一条 masks **逐位相同**，但资源不同（count）。刻意不并成一条 expected=2 的边：
	// 并了以后"漏了 count 那条"只会报"少发一次"，而分开两条直接点名是哪个资源
	// （先例 = izb_head_create_to_prime / izb_nodes_create_to_prime）。
	RgSyncEdgeDesc{
		.name = "cull_count_write_to_readback",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::transferRead,
		.gate = RgSyncGate::cull,
	},
	// ⑥ 读回拷贝落定 → host 读 mapped 内存。无句柄：host 侧读的是 map 出来的主机内存，
	//    给 buffer 句柄反而把"host 可见性"这件事表达窄了（原代码也是 MemoryBarrier2）。
	RgSyncEdgeDesc{
		.name = "cull_readback_to_host",
		.scope = RgSyncScope::executionOrder,
		.src = RgSyncUsage::transferWrite,
		.dst = RgSyncUsage::hostRead,
		.gate = RgSyncGate::cull,
	},
	// ⑤.5 静态 DGC 流：基础序列 compute 写 → fanout dispatch 既读又写（写全 (i,j) 行）。
	// 🔴 挂**自己的子门** `cullFanout`（= cull 且流已建）：`recordFanoutDispatch` 只在
	//    `nEntryStreams > 0` 时被调，挂在父门上会让"没建流的 cull 档"帧末假阳性。
	RgSyncEdgeDesc{
		.name = "cull_seq_fanout_chain",
		.scope = RgSyncScope::buffer,
		.src = RgSyncUsage::computeWrite,
		.dst = RgSyncUsage::computeReadWrite,
		.gate = RgSyncGate::cullFanout,
	},
};  // kEdgeTable

}  // namespace

const RgSyncEdgeDesc& syncEdgeDesc(RgSyncEdgeId id) {
	return kEdgeTable[syncEdgeIndex(id)];
}

void OffGraphSync::beginFrame() {
	counts.fill(0);
}

void OffGraphSync::expectCount(RgSyncEdgeId edge, uint32_t expected_) {
	expected[syncEdgeIndex(edge)] = expected_;
}

void OffGraphSync::emit(vk::raii::CommandBuffer& cmd, RgSyncEdgeId edge) {
	const auto& desc = syncEdgeDesc(edge);
	const auto src = syncUsageMasks(desc.src);
	const auto dst = syncUsageMasks(desc.dst);
	vk::MemoryBarrier2 barrier{};
	barrier.srcStageMask = src.stage;
	barrier.srcAccessMask = src.access;
	barrier.dstStageMask = dst.stage;
	barrier.dstAccessMask = dst.access;
	vk::DependencyInfo dep{};
	dep.setMemoryBarriers(barrier);
	cmd.pipelineBarrier2(dep);
	recordEmission(edge);  // 记账与发射同一行距离 = 「申报了却没发」写不出来
}

void OffGraphSync::emit(vk::raii::CommandBuffer& cmd, RgSyncEdgeId edge, vk::Buffer buffer) {
	const auto& desc = syncEdgeDesc(edge);
	const auto src = syncUsageMasks(desc.src);
	const auto dst = syncUsageMasks(desc.dst);
	vk::BufferMemoryBarrier2 barrier{};
	barrier.srcStageMask = src.stage;
	barrier.srcAccessMask = src.access;
	barrier.dstStageMask = dst.stage;
	barrier.dstAccessMask = dst.access;
	barrier.buffer = buffer;
	barrier.size = VK_WHOLE_SIZE;  // 整块（口径出处见接口注释）
	vk::DependencyInfo dep{};
	dep.setBufferMemoryBarriers(barrier);
	cmd.pipelineBarrier2(dep);
	recordEmission(edge);  // 同上：buffer 档同样不可分割
}

std::expected<void, Error> OffGraphSync::checkEmission(std::span<const bool> gateActive) const {
	if (gateActive.size() != kSyncGateCount)
		return std::unexpected(syncError(fmt::format(
			"off-graph sync: 对账的 gateActive 长度 {} ≠ 门数 {}（新增门必须同步帧末门位表）",
			gateActive.size(), kSyncGateCount)));
	for (uint32_t i = 0; i < kEdgeCount; ++i) {
		const auto& desc = syncEdgeDesc(static_cast<RgSyncEdgeId>(i));
		const bool active = gateActive[static_cast<size_t>(static_cast<uint8_t>(desc.gate))];
		const uint32_t got = counts[i];
		const uint32_t want = expected[i];
		if (active && got != want)
			return std::unexpected(syncError(fmt::format(
				"off-graph sync: 边 '{}' 门开着却发了 {} 次，应发 {} 次"
				"（少发 = 漏同步 = 未定义行为；多发 = 白付同步成本；两种都要当场红）",
				desc.name, got, want)));
		if (!active && got != 0)
			return std::unexpected(syncError(fmt::format(
				"off-graph sync: 边 '{}' 门关着却发了 {} 次 = 破逐位不变红线（门关必须零命令）",
				desc.name, got)));
	}
	return {};
}

}  // namespace RGL
