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
import gpu_profiler;  // Phase 9.2：cull CB 时间戳（cull 区域与主 CB gpu.frame 同槽配对）
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
		RGL::DgcCommands* vkDgc, RGL::DgcStaticCommands* vkDgcStatic, uint32_t vkFramesInFlight)
	: device(vkDevice), gpuCull(vkGpuCull), dgc(vkDgc), dgcStatic(vkDgcStatic),
	  framesInFlight(vkFramesInFlight) {
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
	gpuProfilerCullBegin(ccmd);  // Phase 9.2：cull 段复位（本 CB 先执行）+ cull 起点时间戳
	auto cullInput = list.cullInput;  // 值拷贝：sequenceCount 由本处填（render_list 不感知 GPU 表）
	cullInput.sequenceCount = gpuCull->candidateCount();
	// 序列 CPU 侧基值（execSlot + DrawArgs + meshCmd 基值；args 序 = candidate 序
	// 契约校验在 writeSequenceStaging 内——size 不符即抛）
	gpuCull->writeSequenceStaging(frameSlot, list.cullArgs);
	gpuCull->recordCullPass(ccmd, cullInput, sceneStaging, frameSlot);
	gpuProfilerEnd(ccmd);  // Phase 9.2：cull CB 终点时间戳（dispatch+barrier 完整窗口）
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
		RGL::ShaderRegistry& registry, uint32_t frameSlot) {
	if (dgc && dgc->active()) {
		// DGC execute（NVIDIA 路）：GPU 直接消费序列（CPU args + GPU 可见性，
		// 隐式 preprocess）——非紧凑，被剔序列 x=0 no-op。VUID-11053：EXECUTION_SET
		// token 布局要求执行 CB 上先绑 execution set 初始 shader 态
		dgc->bindInitialShaders(cmd);
		dgc->recordExecute(cmd, gpuCull->sequenceAddress(), gpuCull->countTotalAddress(), frameSlot);
	} else if (dgcStatic && dgcStatic->active()) {
		// 静态 shader DGC 路（RADV spike，docs/radv_static_dgc_spike.md）：每管线条目一次
		// Execute（pNext 静态 frag+mesh，无 EXECUTION_SET token → 无需 bindInitialShaders）。
		// 序列 = GpuCull fanout 满槽孔洞流（条目内 candidate 序；孔洞 x=0 = RADV 原生跳过）；
		// sequenceCount 全条目复用 count buffer @4（= candidate 数，GPU 写）
		const VkDeviceAddress streamsBase = gpuCull->streamsAddress();
		const VkDeviceSize entryStreamBytes =
			static_cast<VkDeviceSize>(gpuCull->candidateCount()) * kCullSequenceStride;
		for (uint32_t j = 0; j < dgcStatic->entryCount(); ++j)
			dgcStatic->recordExecute(cmd, j, streamsBase + entryStreamBytes * j,
				gpuCull->countTotalAddress(), frameSlot);
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

namespace {

/** 收集 CPU opaque 段（[0, skyboxStart)）各 draw 的 objectIndex（对账 CPU 侧多重集源）。 */
std::vector<uint32_t> collectCpuObjectSlots(const FrameDrawList& list) {
	std::vector<uint32_t> cpuSlots;
	cpuSlots.reserve(list.skyboxStart);
	for (uint32_t i = 0; i < list.skyboxStart; ++i) {
		const auto* a = reinterpret_cast<const RGL::DrawArgs*>(list.calls[i].pushData.data());
		cpuSlots.push_back(a->objectIndex);
	}
	return cpuSlots;
}

/** cullArgs[i] ↔ entries[i] 同 objectSlot 顺序契约自检（Phase 7b）；首个失配 warn 后即返（等价原 break）。 */
void checkCullAlignment(const FrameDrawList& list, const RGL::GpuCull& gpuCull) {
	for (uint32_t i = 0; i < gpuCull.candidateCount(); ++i) {
		if (list.cullArgs[i].objectIndex == gpuCull.entries()[i].objectSlot)
			continue;
		if (auto logger = RLLogger::getInstance())
			logger->warn("gpu cull alignment VIOLATION at candidate {}: cullArgs.objectIndex={} != entry.objectSlot={} (order contract broken)",
				i, list.cullArgs[i].objectIndex, gpuCull.entries()[i].objectSlot);
		return;
	}
}

/** 收集 GPU 可见 candidate 的 objectSlot（对账 GPU 侧多重集源，candidate 序）。 */
std::vector<uint32_t> collectGpuVisibleSlots(const RGL::GpuCull& gpuCull) {
	std::vector<uint32_t> gpuSlots;
	gpuSlots.reserve(gpuCull.candidateCount());
	for (uint32_t i = 0; i < gpuCull.candidateCount(); ++i)
		if (gpuCull.isVisible(i))
			gpuSlots.push_back(gpuCull.entries()[i].objectSlot);
	return gpuSlots;
}

}  // namespace

void CullDriver::reconcile(const FrameDrawList& list) {
	// CPU opaque 段（[0, skyboxStart)）的 objectSlot 多重集 vs GPU 可见 candidate 的
	// objectSlot 多重集（逐 candidate 固定槽，顺序无关 → 排序后多重集比对）
	auto cpuSlots = collectCpuObjectSlots(list);
	// 对齐自检（Phase 7b 顺序契约）：cullArgs[i] ↔ entries[i] 必须同 objectSlot
	//（render_list 与 main C3 同守卫同序的运行时证明；失配 = args 错位 → 重放画错矩阵）
	checkCullAlignment(list, *gpuCull);
	auto gpuSlots = collectGpuVisibleSlots(*gpuCull);
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
	if (!match) {
		std::string dump;
		for (auto v : gpuSlots | std::views::take(8))
			dump += fmt::format("{} ", v);
		logger->warn("gpu cull reconcile MISMATCH: gpu-visible={}/cpu-visible={} candidates={} gpu[{}]",
			gpuCount, cpuSlots.size(), gpuCull->candidateCount(), dump);
	}
}

}  // namespace RGL
