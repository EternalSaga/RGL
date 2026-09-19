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
import VmaRaii;
import material_types_generated;

namespace RGL {

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

	// per-entry 满槽孔洞流（静态 shader DGC 路，docs/radv_static_dgc_spike.md §3.2；
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

	// heap 描述符槽（table/sequence/count 各一走运行时段；scene data 槽 = SceneDataService::ensure
	// 幂等返回——main 的 C1 已建，此处只取槽号）
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
		streamsSlot = ensureSlot("cull.streams");  // scene data 槽早已被 main C1 固定，不漂移
	sceneDataSlot = sceneData.ensure();

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

	// compute shader object（stage=COMPUTE + heap bit + 4 binding 映射；
	// RenderPipeline 同款模式，单阶段版——push constant 无需映射条目，
	// descriptor heap 下经 vkCmdPushDataEXT 的 push data 区自动供给）
	auto spvResult = loadShaderSpv("generated_shaders/cull.comp.spv");
	if (!spvResult)
		throw std::runtime_error(spvResult.error().describe());
	const auto& spv = *spvResult;
	const uint32_t descriptorSize = resources.getDescriptorHeap().getDescriptorSize();

	VkDescriptorSetAndBindingMappingEXT mappings[4]{};
	const uint32_t slots[4] = {tableSlot, sceneDataSlot, sequenceSlot, countSlot};
	for (uint32_t n = 0; n < 4; ++n) {
		auto& m = mappings[n];
		m.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
		m.descriptorSet = 0;
		m.firstBinding = n;
		m.bindingCount = 1;
		m.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
		m.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
		m.sourceData.constantOffset.heapOffset = slots[n] * descriptorSize;
		m.sourceData.constantOffset.heapArrayStride = descriptorSize;
	}
	VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
	mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
	mappingInfo.mappingCount = 4;
	mappingInfo.pMappings = mappings;

	VkShaderCreateInfoEXT raw{};
	raw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
	raw.pNext = &mappingInfo;
	raw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
	raw.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	raw.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
	raw.codeSize = spv.size() * sizeof(uint32_t);
	raw.pCode = spv.data();
	raw.pName = "main";
	vk::ShaderCreateInfoEXT createInfo(raw);
	auto shaders = device.createShadersEXT(std::vector<vk::ShaderCreateInfoEXT>{createInfo});
	cullShader = std::move(shaders[0]);

	// stream fanout shader object（静态 DGC 路；3 binding：0=table 1=sequences(ro) 2=streams(rw)，
	// heap 映射同款模式——仅启用时建，docs/radv_static_dgc_spike.md §3.3）
	if (nEntryStreams > 0) {
		auto fanResult = loadShaderSpv("generated_shaders/stream_fanout.comp.spv");
		if (!fanResult)
			throw std::runtime_error(fanResult.error().describe());
		const auto& fanSpv = *fanResult;
		VkDescriptorSetAndBindingMappingEXT fanMappings[3]{};
		const uint32_t fanSlots[3] = {tableSlot, sequenceSlot, streamsSlot};
		for (uint32_t n = 0; n < 3; ++n) {
			auto& m = fanMappings[n];
			m.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
			m.descriptorSet = 0;
			m.firstBinding = n;
			m.bindingCount = 1;
			m.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
			m.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
			m.sourceData.constantOffset.heapOffset = fanSlots[n] * descriptorSize;
			m.sourceData.constantOffset.heapArrayStride = descriptorSize;
		}
		VkShaderDescriptorSetAndBindingMappingInfoEXT fanMappingInfo{};
		fanMappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
		fanMappingInfo.mappingCount = 3;
		fanMappingInfo.pMappings = fanMappings;
		VkShaderCreateInfoEXT fanRaw{};
		fanRaw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
		fanRaw.pNext = &fanMappingInfo;
		fanRaw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
		fanRaw.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		fanRaw.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
		fanRaw.codeSize = fanSpv.size() * sizeof(uint32_t);
		fanRaw.pCode = fanSpv.data();
		fanRaw.pName = "main";
		vk::ShaderCreateInfoEXT fanCreateInfo(fanRaw);
		auto fanShaders = device.createShadersEXT(std::vector<vk::ShaderCreateInfoEXT>{fanCreateInfo});
		fanoutShader = std::move(fanShaders[0]);
	}

	visibleFlags.assign(nCandidates, false);

	if (auto logger = RLLogger::getInstance())
		logger->info("gpu cull: {} candidates ({} sequences × {}B) — table slots {}/{}/{}{}",
			nCandidates, nCandidates, kCullSequenceStride, tableSlot, sequenceSlot, countSlot,
			nEntryStreams > 0 ? fmt::format(" + streams {}×{}B @slot {}", nEntryStreams,
				static_cast<uint64_t>(nCandidates) * kCullSequenceStride, streamsSlot) : std::string());
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
		std::memcpy(s, &execFrag, sizeof(uint32_t));
		std::memcpy(s + 4, &execMesh, sizeof(uint32_t));
		std::memcpy(s + 8, &args[i], sizeof(RGL::DrawArgs));  // DrawArgs @8（36B，Phase 7b+ 去 mvp；mvp 走 scene data 表）
		const uint32_t meshCmd[3] = {entryTable[i].groupCount, 1, 1};
		std::memcpy(s + 108, meshCmd, sizeof(meshCmd));  // meshCmd @108 基值（GPU 覆盖 x）
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

	// dispatch（64 线程/group；push constant = CullFrameInput 164B ≤ maxPushDataSize）
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
	const uint32_t fanPush[1] = {nCandidates};  // FanoutPush.sequenceCount（kernel 头注释契约）
	cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(
		reinterpret_cast<const std::byte*>(fanPush), sizeof(fanPush))));
	cmd.dispatch((nCandidates + 63) / 64, nEntryStreams, 1);
}

uint32_t GpuCull::readCullResult(uint32_t slot) {
	readbackSeqPtr[slot] = static_cast<const std::byte*>(readbackMapped[slot]) + kCullCountBytes;
	// 可见数由 meshCmd.x 位统计（count buffer @0 保留未用——旧 visible atomicAdd
	// 无人清零已删，2026-09-15）
	uint32_t visible = 0;
	for (uint32_t i = 0; i < nCandidates; ++i) {
		const auto* s = readbackSeqPtr[slot] + static_cast<size_t>(i) * kCullSequenceStride;
		const uint32_t x = *reinterpret_cast<const uint32_t*>(s + 108);  // meshCmd.x@108
		visibleFlags[i] = (x != 0);
		visible += (x != 0);
	}
	lastVisible = visible;
	return lastVisible;
}

}  // namespace RGL
