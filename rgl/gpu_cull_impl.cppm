module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>  // VMA 分配类型（VmaRaii 不导出 C 层，各 impl 同款自含）
#include <fmt/format.h>  // fmt::format（契约违例报错）
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
import offgraph_sync;  // 批次 B1b：8 条 barrier 的参数由 usage 词表物化（发射点只写边名）
import VmaRaii;
import material_types_generated;
import compute_push_types_generated;  // FanoutPush（compute-push 反射产物，布局有出处）

namespace RGL {

// FanoutPush **不在本文件**——由 compute-push 反射管线生成
// （generated/compute_push_types_generated.cppm，import 见文件头），与 stream_fanout.comp.slang
// 同名同字段（uint sequenceCount，4B push data；recordFanoutDispatch 经 pushDataEXT 推出）。
//
// 🔴 这里原来是一个手抄 `struct FanoutPush { uint32_t sequenceCount; }` + 两条 static_assert。
// 删掉它们不是"清理冗余"，而是**那两条 assert 是关于 C++ 侧的同义反复**：量的是手抄进 C++ 的
// struct，不是 shader——shader 换序/改类型而 C++ 没跟着动时它们照样全过，而 C++ 写的字节与
// shader 读的字节已经错位，静默。布局现在由反射派生，生成物自带 sizeof/offsetof assert（那份
// 量的才是编译器实查的 shader 布局）。判据与先例 → docs/compute_调研.md §7.13。
// 注：本文件早先的形态是裸 `uint32_t fanPush[1]`（债-12 才结构体化），推出字节 = sequenceCount
// 的 4B 小端，换成本生成物后逐位不变。

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
		auto tableResult = allocator.createBufferWithAddress(device, ti, ta, &tadata, "gpu_cull/candidate_table");
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
		auto seqResult = allocator.createBufferWithAddress(device, si, sa, nullptr, "gpu_cull/sequence");
		if (!seqResult)
			throw std::runtime_error(seqResult.error().describe());
		sequenceBuffer = std::move(*seqResult);

		vk::BufferCreateInfo ci{};
		ci.size = kCullCountBytes;
		ci.usage = vk::BufferUsageFlagBits::eStorageBuffer |
			vk::BufferUsageFlagBits::eShaderDeviceAddress |
			vk::BufferUsageFlagBits::eIndirectBuffer |  // DGC sequenceCountAddress 源（VUID-11072）
			vk::BufferUsageFlagBits::eTransferSrc;
		auto cntResult = allocator.createBufferWithAddress(device, ci, sa, nullptr, "gpu_cull/sequence_count");
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
		auto streamResult = allocator.createBufferWithAddress(device, fi, fa, nullptr, "gpu_cull/entry_streams");
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
		auto s2Result = allocator.createBuffer(si2, sa2, &sa2data, "gpu_cull/sequence_staging");
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
		auto rbResult = allocator.createBuffer(ri, ra, &rada, "gpu_cull/readback");
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
	// 🔴 键 = cull.comp.slang 的 binding **变量名**，号查构建期接口表（批 T1 那次 3→4 挪号
	// 曾要 shader 与这里同步改，现在只改 shader 一处）。只映射本 kernel 真读写的四条：
	// scene.slang 随 import 带进来的其余声明（sceneHeader 等）未被引用、不进 SPV，无需条目。
	const uint32_t descriptorSize = resources.getDescriptorHeap().getDescriptorSize();

	cullShader = makeMappedCompute(device, "generated_shaders/cull.comp.spv",
		std::vector<std::pair<std::string_view, RGL::HeapSlot>>{
			{"cullTable", HeapSlot{tableSlot}}, {"sequences", HeapSlot{sequenceSlot}},
			{"countBuf", HeapSlot{countSlot}}, {"objectMatrixTable", HeapSlot{objectMatrixSlot}}},
		descriptorSize);

	// stream fanout shader object（静态 DGC 路；cullTable / sequences(ro) / streams(rw)，
	// heap 映射同款模式——仅启用时建，docs/archive/radv_static_dgc_spike.md §3.3）
	if (nEntryStreams > 0) {
		fanoutShader = makeMappedCompute(device, "generated_shaders/stream_fanout.comp.spv",
			std::vector<std::pair<std::string_view, RGL::HeapSlot>>{
				{"cullTable", HeapSlot{tableSlot}}, {"sequences", HeapSlot{sequenceSlot}},
				{"streams", HeapSlot{streamsSlot}}},
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
	RGL::VmaBuffer& sceneStaging, uint32_t slot, OffGraphSync& sync) {
	// 自含数据通路（独立于主 CB 的 ⑥/⑥.5/⑦ 固定段——独立命令缓冲须自带 heap
	// 状态：边 + 拷贝 + 绑定。scene data 与主 CB 双拷同一内容（幂等，每帧全量
	// kSceneDataBytes——**字节数不在注释里抄**，真相源 = scene_data.cppm 的
	// `static_assert(kSceneDataBytes == …)`；双拷的成本账在债-21）——保持主 CB 路径零改动
	// = forward diff0 硬约束最小化）

	// ① heap 边（host 写 → heap 读 + transfer 读 + shader 读；eHost 不能在 render
	// pass 内，本 CB 全程 pass 外——docs/DESCRIPTOR_HEAP_SPEC.md 坑 8）。
	// 无句柄重载：descriptor heap 是扩展对象、不是本层能命名句柄的 buffer ⟹ 只能走执行序
	// （宽档 `heapReadWide` 的成因 → offgraph_sync.cppm 的 usage 表）。
	// 🔴 别再在这里加回一个本地 barrier helper、也别把六个 stage/access 字面量写回来 ——
	// 那等于把"谁都能写 mask 字面量"这件事重新发明，而对账会立刻看不见它（它不在边表里）。
	// 形态与理由 → `docs/compute_调研.md` §7.2 / §7.4（先例：izb_shadow_impl.cppm 同位注释）。
	sync.emit(cmd, RgSyncEdgeId::cullHeapHostToReads);

	// scene data 拷贝（staging → device buffer；compute 读对象矩阵表 @128）
	const auto& sceneBuffer = sceneData.getBuffer();
	if (sceneBuffer.buffer != VK_NULL_HANDLE) {
		// 🔴 写前依赖边（与主 CB recordFixedDataPath 的同一条，理由逐字在那边写全，
		// 要点：scene data 单份跨帧共享 + 每帧两个写者（本 CB 与主 CB）+ kFramesInFlight=2
		// ⟹ 本帧 copy 与"上一帧的读 / 另一 CB 的写"之间必须自己建依赖，post-copy 那条
		// dst=AllGraphics/ShaderRead 覆盖不到下一次 copy 的 TransferWrite）。
		// ⚠ size 等价性：原现场是 `size = kSceneDataBytes` 而非 VK_WHOLE_SIZE，而 emit 的
		// buffer 重载刻意不留 size 参数（接口注释给了理由）。等价性的**出处** =
		// `rgl/scene_data.cppm:1042`——scene data buffer 创建时 `info.size = kSceneDataBytes`，
		// 即 buffer 恰好等于该长度 ⟹ 两者逐字节等价。若哪天 buffer 变大，WHOLE_SIZE 只会
		// **更宽**（过度同步，不是漏同步）。
		sync.emit(cmd, RgSyncEdgeId::cullScenePriorToCopy, sceneBuffer.buffer);

		VkBufferCopy region{0, 0, static_cast<VkDeviceSize>(kSceneDataBytes)};
		::vkCmdCopyBuffer(*cmd, *sceneStaging, sceneBuffer.buffer, 1, &region);
		sync.emit(cmd, RgSyncEdgeId::cullSceneCopyToReads, sceneBuffer.buffer);  // size 同上
	}
	else {
		// 🔴 走到这里 = **已经是 bug**，不是可容忍的降级：cull dispatch 要从 scene data @128
		// 读对象矩阵表，buffer 还不存在 ⟹ 那一读本来就在读垃圾。此前这个分支**静默跳过**
		// 拷贝与两条边，把 bug 一起吞了。
		// 批次 B1b 之后它还多一重坏：帧末同步对账会报「`cull` 门开着却发了 0 次」，而那条
		// 读数**是真的**（确实没发）——但它指不出根因。所以这里先出声，两条读数一起看才成立：
		// 本条说"为什么没发"，帧末那条说"没发这件事本身违了什么约"。**不要**为了消掉帧末
		// 那条而给这两条边另挂一道门——门位口径会变成「scene buffer 就绪没」，那是把 bug
		// 重新藏进护栏里（条件边的正确形态见 `docs/compute_调研.md` §7.5 第 1 条）。
		if (auto logger = RLLogger::getInstance())
			logger->error("GpuCull: scene data buffer 未就绪（ensure 未跑），"
						  "本帧 cull 的 scene data 拷贝与两条同步边一并跳过");
	}

	// 序列 staging 拷贝（CPU 基值 → device 序列；compute 随后只覆盖 meshCmd.x@104 +
	// count——args 不经 GPU，BIT-IDENTICAL 契约）。staging 的 host 写由上方 ① heap
	// 边（eHost→eAllGraphics 含 eTransferRead）覆盖
	VkBufferCopy seqUp{0, 0, static_cast<VkDeviceSize>(nCandidates) * kCullSequenceStride};
	::vkCmdCopyBuffer(*cmd, *seqStaging[slot], *sequenceBuffer, 1, &seqUp);
	sync.emit(cmd, RgSyncEdgeId::cullSeqUploadToCreate, *sequenceBuffer);

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
		recordFanoutDispatch(cmd, sync);

	// 输出 buffer：Compute 写 → Transfer 读（序列 + count）。两条 masks 逐位相同、资源不同，
	// 刻意分成两条边而不是 expected=2 的一条：漏发时消息直接点名是哪个资源
	// （先例 = izb_head_create_to_prime / izb_nodes_create_to_prime）。
	sync.emit(cmd, RgSyncEdgeId::cullSeqWriteToReadback, *sequenceBuffer);
	sync.emit(cmd, RgSyncEdgeId::cullCountWriteToReadback, *countBuffer);

	// 读回拷贝（count 区 + 序列区 → readback[slot]）
	const VkDeviceSize seqBytes = static_cast<VkDeviceSize>(nCandidates) * kCullSequenceStride;
	VkBufferCopy countCopy{0, 0, kCullCountBytes};
	VkBufferCopy seqCopy{0, kCullCountBytes, seqBytes};
	::vkCmdCopyBuffer(*cmd, *countBuffer, *readback[slot], 1, &countCopy);
	::vkCmdCopyBuffer(*cmd, *sequenceBuffer, *readback[slot], 1, &seqCopy);

	// Transfer 写 → Host 读（CPU 在 submit + fence wait 后读 readback[slot]）。无句柄重载：
	// host 侧读的是 map 出来的主机内存，原现场也是 MemoryBarrier2 而非 buffer 档。
	sync.emit(cmd, RgSyncEdgeId::cullReadbackToHost);
}

// recordFanoutDispatch 定义（接口注释见 gpu_cull.cppm 私有段）。流写完后由主 CB
// DGC preprocess 消费——同队列提交序 + 同帧 fence 执行依赖，与 NV 路序列消费同款
// 前提，帧内无跨在途帧读写。grid.y = entryCount；写全 (i,j) 行（孔洞 = x=0），
// 无脏行残留（spike §3.3）。
void GpuCull::recordFanoutDispatch(vk::raii::CommandBuffer& cmd, OffGraphSync& sync) {
	// 序列：compute 写（cull dispatch）→ 本 dispatch 既读又写同一 buffer（dst 给
	// ShaderRead|ShaderWrite = WAW 与 WAR 一并挡住）。挂**子门** cullFanout：本函数只在
	// nEntryStreams>0 时被调，挂父门会让"没建流的 cull 档"帧末假阳性。
	sync.emit(cmd, RgSyncEdgeId::cullSeqFanoutChain, *sequenceBuffer);

	std::array<vk::ShaderStageFlagBits, 1> fanStages = {vk::ShaderStageFlagBits::eCompute};
	std::array<vk::ShaderEXT, 1> fanHandles = {*fanoutShader};
	cmd.bindShadersEXT(fanStages, fanHandles);
	// 🔴 指定初始化器不是风格选择：位置聚合初始化与字段名**无关**，改 shader 里的字段名/换序
	// 都不会红（实测：上一刀换生成物后 `just build` 退出码仍是 0）。C++20 还要求指定初始化
	// 按声明序书写 ⟹ 同类型字段换序也红。两个洞一起堵 = "写错名要编译期知道"的承重墙。
	const FanoutPush fanPush{.sequenceCount = nCandidates};  // 契约 = stream_fanout.comp.slang FanoutPush（生成物，见文件头 import）
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
