module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>  // VMA 分配类型（VmaRaii 不导出 C 层，各 impl 同款自含）
#include <fmt/format.h>  // fmt::format（契约违例报错）；offsetof 经 vulkan.h 的 stddef.h 提供（mip_gen_impl 先例）
module gpu_cull:impl;

import gpu_cull;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import scene_data;  // kSceneDataBytes + SceneDataService（架构债务续账步骤 1）
import frame_draw_list;
import shader;
import VmaRaii;
import material_types_generated;

namespace RGL {

namespace {

/** FanoutPush —— 与 stream_fanout.comp.slang 同名 struct 逐字段一致（uint sequenceCount，
 *  4B push data；recordFanoutDispatch 经 pushDataEXT 推出）。债-12 结构体化：原形态是裸
 *  `uint32_t fanPush[1]`，无类型无断言；推出字节 = sequenceCount 的 4B 小端（改前后逐位
 *  一致：sizeof/alignof/offsetof 三条断言钉死）。 */
struct FanoutPush {
	uint32_t sequenceCount;  // candidate 数（grid.x 上取整 64，尾线程守卫——kernel 头注释契约）
};
static_assert(sizeof(FanoutPush) == 4,
	"FanoutPush 与 stream_fanout.comp.slang FanoutPush {{ uint sequenceCount; }} 漂移（4B push data，pushDataEXT 按 sizeof 推）");
static_assert(offsetof(FanoutPush, sequenceCount) == 0,
	"sequenceCount 必须在 @0（kernel 读 push.sequenceCount = push 区首 4B）");

}  // namespace

// 构造器 / recordCullPass / readCullResult 实现（契约见接口声明）。

GpuCull::GpuCull(RGL::DeviceContext& vkDevice, RGL::Resources& vkResources,
	RGL::SceneDataService& vkSceneData,
	const std::vector<CullEntry>& vkEntries, uint32_t entryStreamCount)
	: resources(vkResources),
	  sceneData(vkSceneData),
	  entryTable(vkEntries),
	  nCandidates(static_cast<uint32_t>(vkEntries.size())),
	  // 静态 DGC 流：候选表空则流无意义（钳 0 = 与未启用同构）
	  nEntryStreams(vkEntries.empty() ? 0u : entryStreamCount) {
	auto& device = vkDevice.getDevice();
	createCullBuffers(device);
	createHostRings();
	allocRuntimeSlots();
	writeHeapDescriptors(device);
	createCullShaders(device);
	visibleFlags.assign(nCandidates, false);

	if (auto logger = RLLogger::getInstance())
		logger->info("gpu cull: {} candidates ({} sequences × {}B) — table slots {}/{}/{}{}",
			nCandidates, nCandidates, kCullSequenceStride, tableSlot, sequenceSlot, countSlot,
			nEntryStreams > 0 ? fmt::format(" + streams {}×{}B @slot {}", nEntryStreams,
				static_cast<uint64_t>(nCandidates) * kCullSequenceStride, streamsSlot) : std::string());
}

void GpuCull::createCullBuffers(vk::raii::Device& device) {
	auto& allocator = resources.getAllocator();

	// 静态表 buffer（host-visible + BDA + eStorageBuffer；启动期 memcpy 一次，
	// 每帧可见性由 recordCullPass 的 heap barrier（eHost→eAllGraphics）覆盖）
	if (nCandidates > 0) {
		vk::BufferCreateInfo ti{};
		ti.size = static_cast<vk::DeviceSize>(nCandidates) * 60;
		ti.usage = vk::BufferUsageFlagBits::eStorageBuffer |
			vk::BufferUsageFlagBits::eShaderDeviceAddress;
		VmaAllocationCreateInfo ta{};
		ta.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		ta.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
			VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		VmaAllocationInfo tadata{};
		auto tableResult = allocator.createBufferWithAddress(device, ti, ta, &tadata);
		if (!tableResult)
			throw std::runtime_error(tableResult.error().describe());
		tableBuffer = std::move(*tableResult);
		std::memcpy(tableBuffer.getMappedData(), entryTable.data(),
			static_cast<size_t>(ti.size));
	}

	// 输出 buffer（device-local + BDA + eTransferSrc——读回拷贝源）
	if (nCandidates > 0) {
		vk::BufferCreateInfo si{};
		si.size = static_cast<vk::DeviceSize>(nCandidates) * kCullSequenceStride;
		si.usage = vk::BufferUsageFlagBits::eStorageBuffer |
			vk::BufferUsageFlagBits::eShaderDeviceAddress |
			vk::BufferUsageFlagBits::eIndirectBuffer |  // DGC execute 消费序列（VUID-12407）
			vk::BufferUsageFlagBits::eTransferSrc |  // 读回拷贝源
			vk::BufferUsageFlagBits::eTransferDst;  // 序列 staging 拷贝目标（CPU 基值入 device）
		VmaAllocationCreateInfo sa{};
		sa.usage = VMA_MEMORY_USAGE_AUTO;
		auto seqResult = allocator.createBufferWithAddress(device, si, sa);
		if (!seqResult)
			throw std::runtime_error(seqResult.error().describe());
		sequenceBuffer = std::move(*seqResult);

		vk::BufferCreateInfo ci{};
		ci.size = kCullCountBytes;
		ci.usage = vk::BufferUsageFlagBits::eStorageBuffer |
			vk::BufferUsageFlagBits::eShaderDeviceAddress |
			vk::BufferUsageFlagBits::eIndirectBuffer |  // DGC sequenceCountAddress 源（VUID-11072）
			vk::BufferUsageFlagBits::eTransferSrc;
		auto cntResult = allocator.createBufferWithAddress(device, ci, sa);
		if (!cntResult)
			throw std::runtime_error(cntResult.error().describe());
		countBuffer = std::move(*cntResult);
	}

	// per-entry 满槽孔洞流（静态 shader DGC 路，docs/archive/radv_static_dgc_spike.md §3.2；
	// 仅启用时建——NV/重放路零开销）
	if (nEntryStreams > 0) {
		vk::BufferCreateInfo fi{};
		fi.size = static_cast<vk::DeviceSize>(nEntryStreams) * nCandidates * kCullSequenceStride;
		fi.usage = vk::BufferUsageFlagBits::eStorageBuffer |  // fanout kernel 写（heap binding）
			vk::BufferUsageFlagBits::eShaderDeviceAddress |
			vk::BufferUsageFlagBits::eIndirectBuffer;  // DGC execute indirectAddress（VUID-12407）
		VmaAllocationCreateInfo fa{};
		fa.usage = VMA_MEMORY_USAGE_AUTO;
		auto streamResult = allocator.createBufferWithAddress(device, fi, fa);
		if (!streamResult)
			throw std::runtime_error(streamResult.error().describe());
		streamsBuffer = std::move(*streamResult);
	}
}

void GpuCull::createHostRings() {
	auto& allocator = resources.getAllocator();

	// 序列 staging ring（×2 host-visible；CPU 每帧写基值 → 拷入 device 序列 buffer）
	for (uint32_t s = 0; s < 2; ++s) {
		vk::BufferCreateInfo si2{};
		si2.size = static_cast<vk::DeviceSize>(nCandidates) * kCullSequenceStride;
		si2.usage = vk::BufferUsageFlagBits::eTransferSrc;
		VmaAllocationCreateInfo sa2{};
		sa2.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		sa2.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
			VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		VmaAllocationInfo sa2data{};
		auto s2Result = allocator.createBuffer(si2, sa2, &sa2data);
		if (!s2Result)
			throw std::runtime_error(s2Result.error().describe());
		seqStaging[s] = std::move(*s2Result);
		seqStagingMapped[s] = sa2data.pMappedData;
	}

	// 读回 ring（×2 host-visible；布局 [0..64)=count 区 [64..)=序列区）
	for (uint32_t s = 0; s < 2; ++s) {
		vk::BufferCreateInfo ri{};
		ri.size = static_cast<vk::DeviceSize>(kCullCountBytes) +
			static_cast<vk::DeviceSize>(nCandidates) * kCullSequenceStride;
		ri.usage = vk::BufferUsageFlagBits::eTransferDst;
		VmaAllocationCreateInfo ra{};
		ra.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
		ra.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo rada{};
		auto rbResult = allocator.createBuffer(ri, ra, &rada);
		if (!rbResult)
			throw std::runtime_error(rbResult.error().describe());
		readback[s] = std::move(*rbResult);
		readbackMapped[s] = rada.pMappedData;
	}
}

void GpuCull::allocRuntimeSlots() {
	// heap 描述符槽（table/sequence/count 各一走运行时段；对象矩阵表子范围槽 =
	// SceneDataService::objectMatrixSlot 幂等返回——main 的 C1 已建，此处只取槽号）
	auto ensureSlot = [&](const char* name) -> uint32_t {
		auto slotResult = resources.allocSlot(RGL::SlotSegment::Runtime, name);
		if (!slotResult)
			throw std::runtime_error(slotResult.error().describe());
		return *slotResult;
	};
	tableSlot = ensureSlot("cull.table");
	sequenceSlot = ensureSlot("cull.sequences");
	countSlot = ensureSlot("cull.count");
	if (nEntryStreams > 0)
		streamsSlot = ensureSlot("cull.streams");
	// 批 T1：cull.comp.slang 的对象矩阵表改走 binding 23 的 StructuredBuffer<ObjectMatrix>
	//（子范围描述符 = {scene data 基址 + 区基址, 区长度含哨兵}）。SceneDataService::objectMatrixSlot
	// 内部幂等 ensure——scene data 全区槽早已被 main C1 固定，此处只取子范围槽号，不漂移。
	objectMatrixSlot = sceneData.objectMatrixSlot();
}

void GpuCull::writeHeapDescriptors(vk::raii::Device& device) {
	// 描述符写入（eStorageBuffer + BDA 全范围——SceneDataService::ensure 同款模式）
	if (nCandidates > 0) {
		const auto writeDesc = [&](RGL::VmaBuffer& buf, vk::DeviceSize size, uint32_t slot) {
			vk::DeviceAddressRangeEXT range{buf.getDeviceAddress(), size};
			vk::ResourceDescriptorInfoEXT desc{};
			desc.type = vk::DescriptorType::eStorageBuffer;
			desc.data.pAddressRange = &range;
			auto slotAddr = resources.getDescriptorHeap().getSlotCpuAddress(slot);
			if (!slotAddr)
				throw std::runtime_error(slotAddr.error().describe());
			vk::HostAddressRangeEXT dst{*slotAddr, resources.getDescriptorHeap().getDescriptorSize()};
			device.writeResourceDescriptorsEXT(
				std::array<vk::ResourceDescriptorInfoEXT, 1>{desc},
				std::array<vk::HostAddressRangeEXT, 1>{dst});
		};
		writeDesc(tableBuffer, static_cast<vk::DeviceSize>(nCandidates) * 60, tableSlot);
		writeDesc(sequenceBuffer, static_cast<vk::DeviceSize>(nCandidates) * kCullSequenceStride, sequenceSlot);
		writeDesc(countBuffer, kCullCountBytes, countSlot);
		if (nEntryStreams > 0)
			writeDesc(streamsBuffer,
				static_cast<vk::DeviceSize>(nEntryStreams) * nCandidates * kCullSequenceStride, streamsSlot);
	}
}

void GpuCull::createCullShaders(vk::raii::Device& device) {
	// compute shader object（stage=COMPUTE + heap bit + binding 映射；
	// shader 模块 makeMappedCompute 单阶段版——push constant 无需映射条目，
	// descriptor heap 下经 vkCmdPushDataEXT 的 push data 区自动供给）
	// 🔴 binding 号 = cull.comp.slang 的显式声明（批 T1 后：0 表 / 2 序列 / 4 计数 / 23 对象矩阵；
	// 旧的 1=scene data 全 buffer 字节读已删，3 号位让给 scene.slang 的 sceneData 声明避免
	// slangc E39001 显式撞号警告）。
	const uint32_t descriptorSize = resources.getDescriptorHeap().getDescriptorSize();

	cullShader = makeMappedCompute(device, "generated_shaders/cull.comp.spv",
		std::vector<std::pair<uint32_t, RGL::HeapSlot>>{
			{0, HeapSlot{tableSlot}}, {2, HeapSlot{sequenceSlot}}, {4, HeapSlot{countSlot}},
			{23, HeapSlot{objectMatrixSlot}}},
		descriptorSize);

	// stream fanout shader object（静态 DGC 路；3 binding：0=table 1=sequences(ro) 2=streams(rw)，
	// heap 映射同款模式——仅启用时建，docs/archive/radv_static_dgc_spike.md §3.3）
	if (nEntryStreams > 0) {
		fanoutShader = makeMappedCompute(device, "generated_shaders/stream_fanout.comp.spv",
			std::vector<std::pair<uint32_t, RGL::HeapSlot>>{
				{0, HeapSlot{tableSlot}}, {1, HeapSlot{sequenceSlot}}, {2, HeapSlot{streamsSlot}}},
			descriptorSize);
	}
}

void GpuCull::writeSequenceStaging(uint32_t slot, const std::vector<RGL::DrawArgs>& args) {
	// 契约：args.size() == nCandidates（candidate 序；render_list 与 main C3 同守卫同序）
	if (args.size() != entryTable.size())
		throw std::runtime_error(fmt::format(
			"GpuCull::writeSequenceStaging: args.size()={} != candidateCount={}",
			args.size(), nCandidates));
	auto* dst = static_cast<std::byte*>(seqStagingMapped[slot]);
	for (uint32_t i = 0; i < nCandidates; ++i) {
		auto* s = dst + static_cast<size_t>(i) * kCullSequenceStride;
		// DGC execution set 下标约定（vk_indirect_commands.cppm）：2j = entry j 的 frag，
		// 2j+1 = mesh；序列 @0 = frag 位（FRAGMENT = shaderStages LSB）、@4 = mesh 位
		const uint32_t execFrag = 2 * entryTable[i].pipelineIndex;
		const uint32_t execMesh = 2 * entryTable[i].pipelineIndex + 1;
		std::memcpy(s + kCullSeqExecSlotFrag, &execFrag, sizeof(uint32_t));
		std::memcpy(s + kCullSeqExecSlotMesh, &execMesh, sizeof(uint32_t));
		std::memcpy(s + kCullSeqDrawArgs, &args[i], sizeof(RGL::DrawArgs));  // DrawArgs @8（4B 仅 objectIndex，Phase 7b+ bindless 化；mvp/材质字段走 scene data 表）
		const uint32_t meshCmd[3] = {entryTable[i].groupCount, 1, 1};
		std::memcpy(s + kCullSeqMeshCmd, meshCmd, sizeof(meshCmd));  // meshCmd @108 基值（GPU 覆盖 x）
	}
}

void GpuCull::recordCullPass(vk::raii::CommandBuffer& cmd, const CullFrameInput& input,
	RGL::VmaBuffer& sceneStaging, uint32_t slot) {
	// 自含数据通路（独立于主 CB 的 ⑥/⑥.5/⑦ 固定段——独立命令缓冲须自带 heap
	// 状态：barrier + 拷贝 + 绑定。scene data 与主 CB 双拷同一内容（幂等，262KB/帧
	// 可忽略）——保持主 CB 路径零改动 = forward diff0 硬约束最小化）

	// heap barrier（host 写 → heap 读 + transfer 读 + shader 读；eHost 不能在 render
	// pass 内，本 CB 全程 pass 外——docs/DESCRIPTOR_HEAP_SPEC.md 坑 8）
	vk::MemoryBarrier2 heapBarrier{};
	heapBarrier.srcStageMask = vk::PipelineStageFlagBits2::eHost;
	heapBarrier.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
	heapBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics;
	heapBarrier.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT |
		vk::AccessFlagBits2::eSamplerHeapReadEXT | vk::AccessFlagBits2::eTransferRead |
		vk::AccessFlagBits2::eShaderRead;
	vk::DependencyInfo heapDep{};
	heapDep.setMemoryBarriers(heapBarrier);
	cmd.pipelineBarrier2(heapDep);

	// scene data 拷贝（staging → device buffer；compute 读对象矩阵表 @128）
	const auto& sceneBuffer = sceneData.getBuffer();
	if (sceneBuffer.buffer != VK_NULL_HANDLE) {
		// 🔴 写前依赖 barrier（与主 CB recordFixedDataPath 的同一条，理由逐字在那边写全，
		// 要点：scene data 单份跨帧共享 + 每帧两个写者（本 CB 与主 CB）+ kFramesInFlight=2
		// ⟹ 本帧 copy 与"上一帧的读 / 另一 CB 的写"之间必须自己建依赖，post-copy 那条
		// dst=AllGraphics/ShaderRead 覆盖不到下一次 copy 的 TransferWrite）。
		vk::BufferMemoryBarrier2 preBarrier{};
		preBarrier.srcStageMask = vk::PipelineStageFlagBits2::eAllGraphics
			| vk::PipelineStageFlagBits2::eComputeShader
			| vk::PipelineStageFlagBits2::eTransfer;
		preBarrier.srcAccessMask = vk::AccessFlagBits2::eShaderRead
			| vk::AccessFlagBits2::eTransferWrite;
		preBarrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
		preBarrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		preBarrier.buffer = sceneBuffer.buffer;
		preBarrier.size = kSceneDataBytes;
		vk::DependencyInfo preDep{};
		preDep.setBufferMemoryBarriers(preBarrier);
		cmd.pipelineBarrier2(preDep);

		VkBufferCopy region{0, 0, static_cast<VkDeviceSize>(kSceneDataBytes)};
		::vkCmdCopyBuffer(*cmd, *sceneStaging, sceneBuffer.buffer, 1, &region);
		vk::BufferMemoryBarrier2 sceneBarrier{};
		sceneBarrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
		sceneBarrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		sceneBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics;
		sceneBarrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
		sceneBarrier.buffer = sceneBuffer.buffer;
		sceneBarrier.size = kSceneDataBytes;
		vk::DependencyInfo sceneDep{};
		sceneDep.setBufferMemoryBarriers(sceneBarrier);
		cmd.pipelineBarrier2(sceneDep);
	}

	// 序列 staging 拷贝（CPU 基值 → device 序列；compute 随后只覆盖 meshCmd.x@104 +
	// count——args 不经 GPU，BIT-IDENTICAL 契约）。staging 的 host 写由上方 heap
	// barrier（eHost→eAllGraphics 含 eTransferRead）覆盖
	VkBufferCopy seqUp{0, 0, static_cast<VkDeviceSize>(nCandidates) * kCullSequenceStride};
	::vkCmdCopyBuffer(*cmd, *seqStaging[slot], *sequenceBuffer, 1, &seqUp);
	vk::BufferMemoryBarrier2 seqUpBarrier{};
	seqUpBarrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
	seqUpBarrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	seqUpBarrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	seqUpBarrier.dstAccessMask = vk::AccessFlagBits2::eShaderWrite;
	seqUpBarrier.buffer = *sequenceBuffer;
	seqUpBarrier.size = VK_WHOLE_SIZE;
	vk::DependencyInfo seqUpDep{};
	seqUpDep.setBufferMemoryBarriers(seqUpBarrier);
	cmd.pipelineBarrier2(seqUpDep);

	// heap 绑定（必须在任何描述符引用前——坑 9：与 descriptor set 状态互斥）
	vk::DeviceAddressRangeEXT heapRange{};
	heapRange.address = resources.getDescriptorHeap().getGpuAddress();
	heapRange.size = resources.getDescriptorHeap().getHeapSize();
	cmd.bindResourceHeapEXT(vk::BindHeapInfoEXT(
		heapRange, resources.getReservedRangeOffset(), resources.getReservedRangeSize()));
	vk::DeviceAddressRangeEXT samplerRange{};
	samplerRange.address = resources.getSamplerHeap().getGpuAddress();
	samplerRange.size = resources.getSamplerHeap().getHeapSize();
	cmd.bindSamplerHeapEXT(vk::BindHeapInfoEXT(
		samplerRange, 0, resources.getSamplerReservedRangeSize()));

	// dispatch（64 线程/group；push constant = CullFrameInput 100B ≤ maxPushDataSize）
	std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
	std::array<vk::ShaderEXT, 1> handles = {*cullShader};
	cmd.bindShadersEXT(stages, handles);
	const auto* pushBytes = reinterpret_cast<const std::byte*>(&input);
	cmd.pushDataEXT(vk::PushDataInfoEXT(
		0, vk::HostAddressRangeConstEXT(pushBytes, sizeof(CullFrameInput))));
	cmd.dispatch((nCandidates + 63) / 64, 1, 1);

	// 静态 DGC 路：per-entry 流扇出（门控 = 构造期是否建流，NV/重放路零开销）
	if (nEntryStreams > 0)
		recordFanoutDispatch(cmd);

	// 输出 buffer：Compute 写 → Transfer 读（序列 + count）
	vk::BufferMemoryBarrier2 seqBarrier{};
	seqBarrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	seqBarrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	seqBarrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
	seqBarrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
	seqBarrier.buffer = *sequenceBuffer;
	seqBarrier.size = VK_WHOLE_SIZE;
	vk::DependencyInfo outDep{};
	outDep.setBufferMemoryBarriers(seqBarrier);
	cmd.pipelineBarrier2(outDep);
	vk::BufferMemoryBarrier2 cntBarrier = seqBarrier;
	cntBarrier.buffer = *countBuffer;
	vk::DependencyInfo cntDep{};
	cntDep.setBufferMemoryBarriers(cntBarrier);
	cmd.pipelineBarrier2(cntDep);

	// 读回拷贝（count 区 + 序列区 → readback[slot]）
	const VkDeviceSize seqBytes = static_cast<VkDeviceSize>(nCandidates) * kCullSequenceStride;
	VkBufferCopy countCopy{0, 0, kCullCountBytes};
	VkBufferCopy seqCopy{0, kCullCountBytes, seqBytes};
	::vkCmdCopyBuffer(*cmd, *countBuffer, *readback[slot], 1, &countCopy);
	::vkCmdCopyBuffer(*cmd, *sequenceBuffer, *readback[slot], 1, &seqCopy);

	// Transfer 写 → Host 读（CPU 在 submit + fence wait 后读 readback[slot]）
	vk::MemoryBarrier2 hostBarrier{};
	hostBarrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
	hostBarrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	hostBarrier.dstStageMask = vk::PipelineStageFlagBits2::eHost;
	hostBarrier.dstAccessMask = vk::AccessFlagBits2::eHostRead;
	vk::DependencyInfo hostDep{};
	hostDep.setMemoryBarriers(hostBarrier);
	cmd.pipelineBarrier2(hostDep);
}

// recordFanoutDispatch 定义（接口注释见 gpu_cull.cppm 私有段）。流写完后由主 CB
// DGC preprocess 消费——同队列提交序 + 同帧 fence 执行依赖，与 NV 路序列消费同款
// 前提，帧内无跨在途帧读写。grid.y = entryCount；写全 (i,j) 行（孔洞 = x=0），
// 无脏行残留（spike §3.3）。
void GpuCull::recordFanoutDispatch(vk::raii::CommandBuffer& cmd) {
	vk::BufferMemoryBarrier2 fanBarrier{};
	fanBarrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	fanBarrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	fanBarrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	fanBarrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	fanBarrier.buffer = *sequenceBuffer;
	fanBarrier.size = VK_WHOLE_SIZE;
	vk::DependencyInfo fanDep{};
	fanDep.setBufferMemoryBarriers(fanBarrier);
	cmd.pipelineBarrier2(fanDep);

	std::array<vk::ShaderStageFlagBits, 1> fanStages = {vk::ShaderStageFlagBits::eCompute};
	std::array<vk::ShaderEXT, 1> fanHandles = {*fanoutShader};
	cmd.bindShadersEXT(fanStages, fanHandles);
	const FanoutPush fanPush{nCandidates};  // 契约 = stream_fanout.comp.slang FanoutPush（断言见本文件匿名段）
	cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(
		reinterpret_cast<const std::byte*>(&fanPush), sizeof(fanPush))));
	cmd.dispatch((nCandidates + 63) / 64, nEntryStreams, 1);
}

uint32_t GpuCull::readCullResult(uint32_t slot) {
	readbackSeqPtr[slot] = static_cast<const std::byte*>(readbackMapped[slot]) + kCullCountBytes;
	// 可见数由 meshCmd.x 位统计（count buffer @0 保留未用——旧 visible atomicAdd
	// 无人清零已删，2026-09-15）
	uint32_t visible = 0;
	for (uint32_t i = 0; i < nCandidates; ++i) {
		const auto* s = readbackSeqPtr[slot] + static_cast<size_t>(i) * kCullSequenceStride;
		const uint32_t x = *reinterpret_cast<const uint32_t*>(s + kCullSeqMeshCmd);  // meshCmd.x@108
		visibleFlags[i] = (x != 0);
		visible += (x != 0);
	}
	lastVisible = visible;
	return lastVisible;
}

}  // namespace RGL
