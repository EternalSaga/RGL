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
import diagnostics;  // 债-30 方案 C：cull_replay 三态门（docs/cpu_perf_plan.md §4）
import gpu_profiler;  // Phase 9.2：cull CB 时间戳（cull 区域与主 CB gpu.frame 同槽配对）
import profiler;      // 批 9.5：cull 四段 CPU zone（cull.fenceWait = Phase 9 从未量过的那次同步）
import timing_util;   // 债-22：门控读数的采样时刻（CounterRecord::tsNs）
import vk_indirect_commands;
import shader_registry;
import frame_draw_list;
import material_types_generated;  // DrawArgs（对账 objectIndex 映射 CPU draw + 重放 push 源）
import VmaRaii;

namespace RGL {

namespace {

/**
 * 债-22 门控读数计数器名（CounterRecord::name 要求生命周期 ≥ dump ⟹ 静态字面量）。
 * 口径：每轮对账推一条，值 1 = 本轮两侧输入未变被跳过、0 = 本轮真算
 * ⟹ stats JSON `counters` 里 count = 对账轮数、avg = 跳过率、sum = 跳过数。
 */
constexpr std::string_view kCounterReconcileSkip = "cull.reconcileSkip";

/** 收集 CPU opaque 段（[0, skyboxStart)）各 draw 的 objectIndex（对账 CPU 侧多重集源）。
 *  写入调用方复用的缓冲（债-22：clear 不缩容 ⟹ 稳态零堆分配）。 */
void collectCpuObjectSlots(const FrameDrawList& list, std::vector<uint32_t>& out) {
	out.clear();
	out.reserve(list.skyboxStart);
	for (uint32_t i = 0; i < list.skyboxStart; ++i) {
		const auto* a = reinterpret_cast<const RGL::DrawArgs*>(list.calls[i].pushData.data());
		out.push_back(a->objectIndex);
	}
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

/** 收集 GPU 可见 candidate 的 objectSlot（对账 GPU 侧多重集源，candidate 序）。
 *  写入调用方复用的缓冲（债-22：稳态零堆分配）。 */
void collectGpuVisibleSlots(const RGL::GpuCull& gpuCull, std::vector<uint32_t>& out) {
	out.clear();
	out.reserve(gpuCull.candidateCount());
	for (uint32_t i = 0; i < gpuCull.candidateCount(); ++i)
		if (gpuCull.isVisible(i))
			out.push_back(gpuCull.entries()[i].objectSlot);
}

}  // namespace

CullDriver::CullDriver(RGL::DeviceContext& vkDevice, RGL::GpuCull* vkGpuCull,
		RGL::DgcCommands* vkDgc, RGL::DgcStaticCommands* vkDgcStatic, uint32_t vkFramesInFlight)
	: device(vkDevice), gpuCull(vkGpuCull), dgc(vkDgc), dgcStatic(vkDgcStatic),
	  framesInFlight(vkFramesInFlight), replayMode(diagnostics().cullReplay) {
	allocateCommandBuffers();
	allocateFences();
}

bool CullDriver::active() const {
	return gpuCull && gpuCull->active();
}

void CullDriver::allocateCommandBuffers() {
	// 判据是 gpuPassRuns() 而非 active()：cull CB 只被 submitCullPass 摸，而 `cpu` 档根本不
	// 提交 cull CB ⟹ 默认档下本类**一次 Vulkan 分配都没有**（与"整条 GPU 集不上 CB"一致）。
	if (!gpuPassRuns())
		return;
	vk::CommandBufferAllocateInfo allocInfoCmd{
		*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary,
		framesInFlight};
	cullCommandBuffers = device.getDevice().allocateCommandBuffers(allocInfoCmd);
}

void CullDriver::allocateFences() {
	if (!gpuPassRuns())
		return;
	// **必须建成非 signaled**：本类的 fence 只被"提交 → 等待 → 复位"这条链摸，首次提交前
	// 没有任何等待动作，若带 eSignaled 出厂（照抄 FrameSession::inFlightFences 的做法）就是
	// "提交已信号态 fence"——VVL 直接 error（实测两 slot 各一条），RADV 只是默默容忍。
	// FrameSession 能带 eSignaled 是因为 beginFrame 每帧先 wait+reset 再提交，首帧的等待
	// 恰好把它清干净；本类没有那个"先等一次"的前置。
	const vk::FenceCreateInfo fenceInfo{};
	cullFences.reserve(framesInFlight);
	for (uint32_t i = 0; i < framesInFlight; ++i)
		cullFences.emplace_back(device.getDevice(), fenceInfo);
}

std::expected<void, Error> CullDriver::runFrame(uint32_t frameSlot, uint64_t frame,
		const FrameDrawList& list, RGL::VmaBuffer& sceneStaging) {
	// 交付默认档：整条 GPU 集不上 CB（dispatch + 读回拷贝 + 一次提交全省，实测定价见
	// docs/cpu_perf_plan.md §4）。早返回，别在 trace 里留一条 0 时间的假段。
	if (replayMode == CullReplayMode::cpu)
		return {};

	// ① 滞后档先收**上一帧**的读回。为什么这里几乎不等：帧 F-1 的 cull CB 排在帧 F-1 主
	// CB 之前入队，而本帧开场 FrameSession 已等过帧 F-2 的主 CB ⟹ 轮到此处时它通常早已
	// 信号（`gpu_sync` 反过来：等的是本帧 cull CB，排在帧 F-1 整条主 CB 之后 = 债-30 的账）。
	if (replayMode == CullReplayMode::gpuLagged && harvestPending)
		if (auto r = harvest(pendingSlot, list); !r)
			return std::unexpected(std::move(r).error());

	// ② 本帧 cull CB 录 + 提交（提交即把本帧 CPU opaque 表记进待收清单）。
	if (auto r = submitCullPass(frameSlot, frame, list, sceneStaging); !r)
		return std::unexpected(std::move(r).error());

	// ③ 同帧档：本帧等 + 读回 + 对账（现行主路形态，也是将来 DGC 的对照组）。
	if (replayMode == CullReplayMode::gpuSync)
		if (auto r = harvest(frameSlot, list); !r)
			return std::unexpected(std::move(r).error());

	return {};
}

std::expected<void, Error> CullDriver::submitCullPass(uint32_t frameSlot, uint64_t frame,
		const FrameDrawList& list, RGL::VmaBuffer& sceneStaging) {
	auto& ccmd = cullCommandBuffers[frameSlot];
	{
		auto z = zone("cull.record");  // 批 9.5：cull CB 录制 + 序列 CPU 侧 staging 装配
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
	}

	{
		auto z = zone("cull.submit");  // 批 9.5：cull CB 入队（信号本类自持的 cullFences[slot]）
		VkCommandBuffer ccb = *ccmd;
		VkSubmitInfo cullSubmit{};
		cullSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		cullSubmit.commandBufferCount = 1;
		cullSubmit.pCommandBuffers = &ccb;
		const VkResult cullSubmitResult = ::vkQueueSubmit(device.getGraphicsQueue(), 1,
			&cullSubmit, *cullFences[frameSlot]);
		if (cullSubmitResult != VK_SUCCESS) {
			auto logger = RLLogger::getInstance();
			if (logger)
				logger->error("Renderer: cull submit failed (frame {}): {}", frame,
					vk::to_string(vk::Result{cullSubmitResult}));
			return std::unexpected(Error{ErrorCode::Unknown,
				fmt::format("cull submit failed: {}", vk::to_string(vk::Result{cullSubmitResult}))});
		}
	}
	// 本帧 CPU opaque 表挂进待收清单：滞后档在下一帧拿它跟 GPU 结果比（"同一帧的两侧、
	// 只是晚一帧报"，理由见 reconcileLagged 注释）。
	pendingSlot = frameSlot;
	collectCpuObjectSlots(list, pendingCpuSet);
	harvestPending = true;
	return {};
}

std::expected<void, Error> CullDriver::harvest(uint32_t slot, const FrameDrawList& list) {
	auto& device_ = device.getDevice();
	// 批 9.5（观测任务的头号数字）：这次无限超时同步等待自 Phase 9 起一直落在未归因段里、
	// 从未被量过。单独成 zone 才能复核 Phase 9 当年"cull GPU 只 14µs ⟹ 否决读回延后一帧"
	// 那条否决（详见 docs/phase9_plan.md §10）——债-30 的实测把它翻案了：等的不是 cull，
	// 是上一帧整帧排空。
	const vk::Result cullWaitResult = [&] {
		auto z = zone("cull.fenceWait");
		return device_.waitForFences(*cullFences[slot], VK_TRUE,
			std::numeric_limits<uint64_t>::max());
	}();
	if (cullWaitResult != vk::Result::eSuccess) {
		auto logger = RLLogger::getInstance();
		if (logger)
			logger->error("Renderer: cull fence wait failed (slot {}): {}", slot,
				vk::to_string(cullWaitResult));
		return std::unexpected(Error{ErrorCode::DeviceLost,
			fmt::format("cull fence wait failed: {}", vk::to_string(cullWaitResult))});
	}
	{
		// fence 复位（本类自持，复位只为下一次提交）+ mapped 读回 + 与 CPU 剔除对账
		//（纯 CPU 多重集比对，候选 1591 量级——它此前也埋在未归因段里）
		auto z = zone("cull.readback");
		device_.resetFences(*cullFences[slot]);
		harvestPending = false;
		gpuCull->readCullResult(slot);
		{
			// 债-22：对账单独成段——此前它埋在 cull.readback 里与 fence 复位 / mapped 读回
			// 混在一起，门控省掉了多少无从读出（批 9.5 "未归因段"同一病灶）
			auto zr = zone("cull.reconcile");
			if (replayMode == CullReplayMode::gpuLagged)
				reconcileLagged(list);
			else
				reconcile(list);
		}
	}
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
		// 批 9.5：本路 draw 数在 CPU 侧不可知（可见位由 GPU 写），draw.count 记 0；
		// 命令数 = bindInitialShaders + executeGeneratedCommands = 2。本机（RADV）不走此路。
		profilerAddCommands(2);
	} else if (dgcStatic && dgcStatic->active()) {
		// 静态 shader DGC 路（RADV spike，docs/archive/radv_static_dgc_spike.md）：每管线条目一次
		// Execute（pNext 静态 frag+mesh，无 EXECUTION_SET token → 无需 bindInitialShaders）。
		// 序列 = GpuCull fanout 满槽孔洞流（条目内 candidate 序；孔洞 x=0 = RADV 原生跳过）；
		// sequenceCount 全条目复用 count buffer @4（= candidate 数，GPU 写）
		const VkDeviceAddress streamsBase = gpuCull->streamsAddress();
		const VkDeviceSize entryStreamBytes =
			static_cast<VkDeviceSize>(gpuCull->candidateCount()) * kCullSequenceStride;
		for (uint32_t j = 0; j < dgcStatic->entryCount(); ++j)
			dgcStatic->recordExecute(cmd, j, streamsBase + entryStreamBytes * j,
				gpuCull->countTotalAddress(), frameSlot);
		// 批 9.5：每管线条目一次 vkCmdExecuteGeneratedCommandsEXT；draw 数同上不进 draw.count
		profilerAddCommands(dgcStatic->entryCount());
	} else {
		// 读回重放（RADV 路）：逐 candidate 按可见位重放——candidate 序 =
		// CPU opaque 段序（顺序契约），args = CPU 位级真相源（list.cullArgs，
		// 与 drawOne 的 pushData 同值）→ 截图 BIT-IDENTICAL
		uint32_t replayed = 0;  // 批 9.5：可见位重放的 draw 数（循环外一次性入账）
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
			++replayed;
		}
		profilerAddDraws(replayed);
	}
}

void CullDriver::reconcile(const FrameDrawList& list) {
	// 对齐自检（Phase 7b 顺序契约）：cullArgs[i] ↔ entries[i] 必须同 objectSlot
	//（render_list 与 main C3 同守卫同序的运行时证明；失配 = args 错位 → 重放画错矩阵）
	// 🔴 不受债-22 门控影响：这是结构性守卫，每帧无条件跑（门控只覆盖下面的多重集比对）。
	checkCullAlignment(list, *gpuCull);
	collectCpuObjectSlots(list, cpuSlotsScratch);
	collectGpuVisibleSlots(*gpuCull, gpuSlotsScratch);
	reportReconcile(cpuSlotsScratch, gpuSlotsScratch);
}

void CullDriver::reconcileLagged(const FrameDrawList& list) {
	// 顺序契约自检用**本帧** list 是合法的：该契约是结构性的（cullArgs 与 entries 同源于
	// 候选表构造顺序），与"GPU 结果晚一帧"无关。可见集合的比对则必须用提交那一帧记下的
	// CPU 表——相机每帧跟着动，拿本帧表比 GPU 的上一帧结果 = 一动就必然失配 = 告警永久化。
	checkCullAlignment(list, *gpuCull);
	cpuSlotsScratch = pendingCpuSet;  // 采集序原样搬进缓冲（容量复用 = 稳态零分配）
	collectGpuVisibleSlots(*gpuCull, gpuSlotsScratch);
	reportReconcile(cpuSlotsScratch, gpuSlotsScratch);
}

void CullDriver::reportReconcile(std::vector<uint32_t>& cpuSlots, std::vector<uint32_t>& gpuSlots) {
	// ── 债-22 门控（判据与不失明论证见声明注释）──
	// 两侧序列与上一轮逐元素相同 ⟹ 排序后的多重集相同 ⟹ 结论相同 ⟹ 两次 sort 与比对免做。
	// 判据取"被比的那两份数据本身"，不取上游版本号：结论是本函数的纯函数（排序 + ==，
	// 无状态 / 无时钟 / 无 RNG），输入未变就必然结论未变；而任何**新**失配必然伴随至少
	// 一侧序列变化 ⟹ 必然走到下面的比对。首帧无基准 = 必算。
	const bool unchanged = reconcileMemoValid && cpuSlots == memoCpuSlots
		&& gpuSlots == memoGpuSlots;
	// 门控读数（profiler 关时 profilerPushCounter 自身静默丢）：1 = 本轮跳过、0 = 本轮真算
	// ⟹ stats JSON 里 avg = 跳过率、count = 对账轮数、sum = 跳过数。
	profilerPushCounter(CounterRecord{
		.frame = profilerCurrentFrame(), .tsNs = steadyNowNs(),
		.value = unchanged ? 1ULL : 0ULL, .name = kCounterReconcileSkip});
	if (unchanged)
		return;
	reconcileMemoValid = true;
	memoCpuSlots = cpuSlots;  // 存**采集序**（下面就地排序，基准必须是排序前的序）
	memoGpuSlots = gpuSlots;

	// CPU opaque 段（[0, skyboxStart)）的 objectSlot 多重集 vs GPU 可见 candidate 的
	// objectSlot 多重集（逐 candidate 固定槽，顺序无关 → 排序后多重集比对）
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
	if (!logger || match)
		return;
	std::string dump;
	for (auto v : gpuSlots | std::views::take(8))
		dump += fmt::format("{} ", v);
	logger->warn("gpu cull reconcile MISMATCH: gpu-visible={}/cpu-visible={} candidates={} gpu[{}] (mode={})",
		gpuCount, cpuSlots.size(), gpuCull->candidateCount(), dump, cullReplayName(replayMode));
}

}  // namespace RGL
