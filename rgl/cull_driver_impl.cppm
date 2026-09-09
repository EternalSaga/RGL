module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；vkQueueSubmit 裸调用）
#include <ranges>           // std::views（AGENTS 例外：namespace alias 不可 re-export）
#include <fmt/format.h>
module cull_driver:impl;

import cull_driver;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import gpu_cull;
import vk_indirect_commands;
import shader_registry;
import frame_draw_list;
import material_types_generated;  // DrawArgs（对账 objectIndex@96 映射 CPU draw + 重放 push 源）
import VmaRaii;

namespace RGL {

// CullDriver 函数体（2026-09-10 Phase 10 批次 0 手术 3：逐行迁自 vkrenderer_impl
// ④c 段 / opaque 段双路 / reconcileGpuCull / allocateCullCommandBuffers，语义零改动；
// vkDevice→device、graphicsQueue→device.getGraphicsQueue()、graphicsCommandPool→
// device.getGraphicsCommandPool()、MAX_FRAMES_IN_FLIGHT→framesInFlight 成员）。

CullDriver::CullDriver(RGL::DeviceContext& vkDevice, RGL::GpuCull* vkGpuCull,
		RGL::DgcCommands* vkDgc, uint32_t vkFramesInFlight)
	: device(vkDevice), gpuCull(vkGpuCull), dgc(vkDgc), framesInFlight(vkFramesInFlight) {
	allocateCommandBuffers();
}

bool CullDriver::active() const {
	return gpuCull && gpuCull->active();
}

void CullDriver::allocateCommandBuffers() {
	if (!active())
		return;  // inactive：不分配（drawFrame 全路径不碰 culling）
	vk::CommandBufferAllocateInfo allocInfoCmd{
		*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary,
		framesInFlight};
	cullCommandBuffers = device.getDevice().allocateCommandBuffers(allocInfoCmd);
}

std::expected<void, Error> CullDriver::runFrame(uint32_t frameSlot, uint64_t frame,
		const FrameDrawList& list, RGL::VmaBuffer& sceneStaging, vk::Fence inFlightFence) {
	auto& device_ = device.getDevice();  // 原 drawFrame 局部 device 引用
	// ④c GPU culling 数据通路（Phase 7b 批次 5）：独立 cull CB（自含固定段 + dispatch +
	// 读回拷贝）→ 提交 → **同帧同步等待**（读回重放的前提；性能优化 = 后续注记）→
	// 读回对账。fence 与主 CB 共用 inFlightFences[frameSlot]：cull submit 信号后
	// 立即 reset（vkdQueueSubmit 要求 fence 非 signaled 态——主 CB 稍后重新挂同一 fence），
	// 帧 N+2 的 ① 对主 CB submit 信号后的 fence 照常 wait/reset。
	auto& ccmd = cullCommandBuffers[frameSlot];
	ccmd.reset();
	ccmd.begin(vk::CommandBufferBeginInfo{});
	auto cullInput = list.cullInput;  // 值拷贝：sequenceCount 由本处填（render_list 不感知 GPU 表）
	cullInput.sequenceCount = gpuCull->candidateCount();
	// 序列 CPU 侧基值（execSlot + DrawArgs + meshCmd 基值；args 序 = candidate 序
	// 契约校验在 writeSequenceStaging 内——size 不符即抛）
	gpuCull->writeSequenceStaging(frameSlot, list.cullArgs);
	gpuCull->recordCullPass(ccmd, cullInput, sceneStaging, frameSlot);
	// DGC preprocess（NVIDIA 路）：cull dispatch 之后序列 buffer 已含 GPU 可见性——
	// Compute→COMMAND_PREPROCESS 屏障（preprocess 读序列 + 写 preprocess 缓冲）
	if (dgc && dgc->active()) {
		vk::BufferMemoryBarrier2 ppBarrier{};
		ppBarrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
		ppBarrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
		ppBarrier.dstStageMask = vk::PipelineStageFlagBits2::eCommandPreprocessEXT;
		ppBarrier.dstAccessMask = vk::AccessFlagBits2::eCommandPreprocessReadEXT;
		ppBarrier.buffer = gpuCull->sequenceBufferHandle();
		ppBarrier.size = VK_WHOLE_SIZE;
		vk::DependencyInfo ppDep{};
		ppDep.setBufferMemoryBarriers(ppBarrier);
		ccmd.pipelineBarrier2(ppDep);
		dgc->recordPreprocess(ccmd, gpuCull->sequenceAddress());
	}
	ccmd.end();

	VkCommandBuffer ccb = *ccmd;
	VkSubmitInfo cullSubmit{};
	cullSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	cullSubmit.commandBufferCount = 1;
	cullSubmit.pCommandBuffers = &ccb;
	const VkResult cullSubmitResult = ::vkQueueSubmit(device.getGraphicsQueue(), 1,
		&cullSubmit, inFlightFence);
	if (cullSubmitResult != VK_SUCCESS) {
		auto logger = RLLogger::getInstance();
		if (logger)
			logger->error("Renderer: cull submit failed (frame {}): {}", frame,
				vk::to_string(vk::Result{cullSubmitResult}));
		return std::unexpected(Error{ErrorCode::Unknown,
			fmt::format("cull submit failed: {}", vk::to_string(vk::Result{cullSubmitResult}))});
	}
	const vk::Result cullWaitResult = device_.waitForFences(
		inFlightFence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	if (cullWaitResult != vk::Result::eSuccess) {
		auto logger = RLLogger::getInstance();
		if (logger)
			logger->error("Renderer: cull fence wait failed (frame {}): {}", frame,
				vk::to_string(cullWaitResult));
		return std::unexpected(Error{ErrorCode::DeviceLost,
			fmt::format("cull fence wait failed: {}", vk::to_string(cullWaitResult))});
	}
	// fence 已信号 → 重置（主 CB submit 重新挂；已信号 fence 再挂 = VU 违例）
	device_.resetFences(inFlightFence);
	gpuCull->readCullResult(frameSlot);
	reconcile(list);
	return {};
}

void CullDriver::executeOpaqueSegment(vk::raii::CommandBuffer& cmd, const FrameDrawList& list,
		RGL::ShaderRegistry& registry) {
	if (dgc && dgc->active()) {
		// DGC execute（NVIDIA 路）：GPU 直接消费序列（CPU args + GPU 可见性，
		// preprocess 已在 cull CB 完成）——非紧凑，被剔序列 x=0 no-op
		dgc->recordExecute(cmd, gpuCull->sequenceAddress(), gpuCull->countTotalAddress());
	} else {
		// 读回重放（RADV 路）：逐 candidate 按可见位重放——candidate 序 =
		// CPU opaque 段序（顺序契约），args = CPU 位级真相源（list.cullArgs，
		// 与 drawOne 的 pushData 同值）→ 截图 BIT-IDENTICAL
		for (uint32_t i = 0; i < gpuCull->candidateCount(); ++i) {
			if (!gpuCull->isVisible(i))
				continue;
			const auto& e = gpuCull->entries()[i];
			const auto& entry = registry.entry(e.pipelineIndex);
			cmd.pushDataEXT(vk::PushDataInfoEXT(
				entry.pushDataSet, vk::HostAddressRangeConstEXT(
					reinterpret_cast<const std::byte*>(&list.cullArgs[i]),
					sizeof(RGL::DrawArgs))));
			entry.pipeline.bindShaders(cmd);
			cmd.drawMeshTasksEXT(e.groupCount, 1, 1);
		}
	}
}

void CullDriver::reconcile(const FrameDrawList& list) {
	// CPU opaque 段（[0, skyboxStart)）的 objectSlot 多重集 vs GPU 可见 candidate 的
	// objectSlot 多重集（逐 candidate 固定槽，顺序无关 → 排序后多重集比对）
	std::vector<uint32_t> cpuSlots;
	cpuSlots.reserve(list.skyboxStart);
	for (uint32_t i = 0; i < list.skyboxStart; ++i) {
		const auto* a = reinterpret_cast<const RGL::DrawArgs*>(list.calls[i].pushData.data());
		cpuSlots.push_back(a->objectIndex);
	}
	// 对齐自检（Phase 7b 顺序契约）：cullArgs[i] ↔ entries[i] 必须同 objectSlot
	//（render_list 与 main C3 同守卫同序的运行时证明；失配 = args 错位 → 重放画错矩阵）
	for (uint32_t i = 0; i < gpuCull->candidateCount(); ++i) {
		if (list.cullArgs[i].objectIndex != gpuCull->entries()[i].objectSlot) {
			auto logger = RLLogger::getInstance();
			if (logger)
				logger->warn("gpu cull alignment VIOLATION at candidate {}: cullArgs.objectIndex={} != entry.objectSlot={} (order contract broken)",
					i, list.cullArgs[i].objectIndex, gpuCull->entries()[i].objectSlot);
			break;
		}
	}
	std::vector<uint32_t> gpuSlots;
	gpuSlots.reserve(gpuCull->candidateCount());
	for (uint32_t i = 0; i < gpuCull->candidateCount(); ++i)
		if (gpuCull->isVisible(i))
			gpuSlots.push_back(gpuCull->entries()[i].objectSlot);
	bool match = cpuSlots.size() == gpuSlots.size();
	if (match) {
		std::ranges::sort(cpuSlots);
		std::ranges::sort(gpuSlots);
		match = (cpuSlots == gpuSlots);
	}
	const uint32_t gpuCount = static_cast<uint32_t>(gpuSlots.size());
	if (gpuCount == lastReconcileVisible && match == lastReconcileMatch)
		return;  // 节流：静止场景不刷屏（相机移动时计数变化即现）
	lastReconcileVisible = gpuCount;
	lastReconcileMatch = match;
	auto logger = RLLogger::getInstance();
	if (!logger)
		return;
	if (match) {
		logger->info("gpu cull reconcile: gpu-visible={}/cpu-visible={} candidates={} match",
			gpuCount, cpuSlots.size(), gpuCull->candidateCount());
	} else {
		std::string dump;
		for (auto v : gpuSlots | std::views::take(8))
			dump += fmt::format("{} ", v);
		logger->warn("gpu cull reconcile MISMATCH: gpu-visible={}/cpu-visible={} candidates={} gpu[{}]",
			gpuCount, cpuSlots.size(), gpuCull->candidateCount(), dump);
	}
}

}  // namespace RGL
