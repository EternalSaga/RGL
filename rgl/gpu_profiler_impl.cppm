// gpu_profiler_impl.cppm — Phase 9.2 GPU 时间戳实现（docs/phase9_plan.md 批 9.2）。
// 结构：构造三重门控建 pool → frameBegin 读回两段+校准+归并+主段复位记主 CB →
// begin/cullBegin 成对时间戳（配对栈、双段游标）→ 析构观测日志。读回走裸 C
//（vk::raii 未包装 getQueryPoolResults；每段只读实际写入部分，非 SUCCESS 计 drops 跳段）。
module;
#include <vulkan/vulkan.h>  // vkGetQueryPoolResults C 函数（loader trampoline；vk::raii 未包装）
module gpu_profiler:impl;

import gpu_profiler;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import diagnostics;  // diagnostics().profiler（帧窗口开关，main 启动早期 setDiagnostics）
import profiler;     // profilerCurrentFrame（归属帧）+ profilerPushRecord（归并回填）
import timing_util;  // steadyNowNs（与校准 mono 域同域的取时刻；自本文件收敛）
import vk_device;
import queue_family_manager;  // getFamilyIndex（timestampValidBits 族判定）

namespace RGL {

GpuProfiler::GpuProfiler(DeviceContext& dev) : device(dev) {
	if (diagnostics().profiler == 0)
		return;  // 关：不建 pool——默认路径零记录零 GPU 影响
	if (!device.getOptionalFeatures().calibratedTimestamps) {
		if (auto* lg = RLLogger::getInstance())
			lg->warn("gpu profiler: VK_EXT_calibrated_timestamps unavailable, gpu track disabled");
		return;
	}
	const auto props = device.getPhysicalDevice().getProperties();
	if (!props.limits.timestampComputeAndGraphics) {
		if (auto* lg = RLLogger::getInstance())
			lg->warn("gpu profiler: queue timestamps unsupported, gpu track disabled");
		return;
	}
	timestampPeriodNs = props.limits.timestampPeriod;
	// timestampValidBits 族相关（桌面三厂多为 64；<64 时高位为保留位，查询读值与
	// 校准设备 tick 同掩）。族判定与提交族一致（drawFrame/cull 全走 graphics queue）。
	const auto families = device.getPhysicalDevice().getQueueFamilyProperties();
	const uint32_t family =
		device.getQueueManager().getFamilyIndex();
	const uint32_t validBits = families[family].timestampValidBits;
	validMask = validBits >= 64 ? ~uint64_t{0} : ((uint64_t{1} << validBits) - 1);

	vk::QueryPoolCreateInfo info;
	info.queryType = vk::QueryType::eTimestamp;
	info.queryCount = kGpuSlots * kQueriesPerFrame;
	pool.emplace(device.getDevice(), info);
	ticks.resize(kQueriesPerFrame);
	if (auto* lg = RLLogger::getInstance()) {
		if (diagnostics().profiler < 3)
			lg->warn("gpu profiler: window {} < 3, gpu records will drop (readback lags 2 frames)",
				diagnostics().profiler);
		lg->info("gpu profiler: on ({} queries, {:.3f} ns/tick, validBits {})",
			kGpuSlots * kQueriesPerFrame, timestampPeriodNs, validBits);
	}
}

GpuProfiler::~GpuProfiler() {
	if (!pool.has_value())
		return;
	if (auto* lg = RLLogger::getInstance()) {
		lg->info("gpu profiler: closed ({} drops, {} zones, calibration maxDeviation {} ns)",
			drops, pushedZones, lastMaxDeviationNs);
		// 空轨必须出声（债-87）：窗口够长却一条 gpu zone 都没归并 = query 通路或换算
		// 失效，而 stats JSON 里 kind=gpu 缺席看起来只像"没开 profiler"——静默的判据
		// 比没有判据更坏（本条债因此拖到 per-pass 读数被反复引用后才被发现）。
		if (pushedZones == 0 && diagnostics().profiler >= 3)
			lg->warn("gpu profiler: 0 gpu zones in {} drops — gpu track is EMPTY "
				"(query 未写入 / 读回非就绪 / 换算全被丢弃)，per-pass GPU 读数不可用",
				drops);
	}
}

void GpuProfiler::frameBegin(vk::raii::CommandBuffer& cmd, uint32_t slot) {
	if (!pool.has_value() || slot >= kGpuSlots)
		return;
	SlotState& s = slotStates[slot];
	if (s.regionCount > 0)  // 上一轮占用本槽的帧（fence 已等完 → 查询必就绪）
		readback(slot, s.frame);
	// 主段复位记入主 CB 头（record 序先于本帧主段时间戳；与 cull 段不相交 → cull CB
	// 先执行写 cull 段也互不影响）。不用 host vkResetQueryPool：需 hostQueryReset 特性
	//（spec 可选，本机 RADV 未启用即 VVL 报错）
	cmd.resetQueryPool(*pool, slot * kQueriesPerFrame, kMainQueries);
	s.queryCursor = 0;
	s.cullCursor = 0;
	s.regionCount = 0;
	s.openDepth = 0;
	s.frame = profilerCurrentFrame();  // 本轮区域的归属逻辑帧（读回时带回填）
	s.hostAnchorNs = steadyNowNs();    // 纪元自检的 host 锚点（本轮 GPU 起点必落在其后 ≤1 帧）
	activeSlot = slot;
}

void GpuProfiler::cullBegin(vk::raii::CommandBuffer& cmd) {
	if (!pool.has_value())
		return;
	// cull 段复位记入 cull CB 头（自身先于 cull 时间戳；段与主段不相交 → 与主 CB
	// 的执行序/记录序均无关，规避 VVL 跨 CB 记录序限制）
	cmd.resetQueryPool(*pool, activeSlot * kQueriesPerFrame + kMainQueries, kCullQueries);
	beginIn(cmd, "cull", true);
}

void GpuProfiler::begin(vk::raii::CommandBuffer& cmd, std::string_view name) {
	if (!pool.has_value())
		return;
	beginIn(cmd, name, false);
}

void GpuProfiler::beginIn(vk::raii::CommandBuffer& cmd, std::string_view name, bool cull) {
	SlotState& s = slotStates[activeSlot];
	const uint32_t segBase = activeSlot * kQueriesPerFrame + (cull ? kMainQueries : 0);
	const uint32_t segLimit = cull ? kCullQueries : kMainQueries;
	uint32_t& cursor = cull ? s.cullCursor : s.queryCursor;
	uint32_t mark = kInvalid;
	if (s.regionCount < kMaxRegions && cursor < segLimit) {
		Region& r = s.regions[s.regionCount];
		r.name = name;
		r.depth = s.openDepth;
		r.beginQuery = segBase + cursor++;
		r.endQuery = kInvalid;
		mark = s.regionCount;
		++s.regionCount;
		cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool, r.beginQuery);
	} else {
		++drops;  // 区域表/本段查询预算溢出（正常 ~16 区域/帧，触发即结构异常）
	}
	if (s.openDepth < kMaxDepth)
		s.openStack[s.openDepth++] = mark;
	// 栈深溢出不可达（实际深度 ≤ 2 << kMaxDepth=8）：不压栈，end 侧配对会错位一
	// 层——仅在结构违约时发生，丢弃该帧区域数据不崩溃
}

void GpuProfiler::end(vk::raii::CommandBuffer& cmd) {
	if (!pool.has_value())
		return;
	SlotState& s = slotStates[activeSlot];
	if (s.openDepth == 0)
		return;  // 防御：无配对 begin（结构违约）
	const uint32_t mark = s.openStack[--s.openDepth];
	if (mark == kInvalid)
		return;  // begin 已溢出丢弃，配对跳过
	Region& r = s.regions[mark];
	// endQuery 与 beginQuery 同段分配（段由 begin 落点决定，配对栈保证不跨界）
	const uint32_t slotBase = activeSlot * kQueriesPerFrame;
	const bool cull = r.beginQuery >= slotBase + kMainQueries;
	const uint32_t segBase = slotBase + (cull ? kMainQueries : 0);
	const uint32_t segLimit = cull ? kCullQueries : kMainQueries;
	uint32_t& cursor = cull ? s.cullCursor : s.queryCursor;
	if (cursor >= segLimit) {
		++drops;  // endQuery 留空 → 读回时该区域整体丢弃
		return;
	}
	r.endQuery = segBase + cursor++;
	cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool, r.endQuery);
}

void GpuProfiler::readback(uint32_t slot, uint64_t recordedFrame) {
	SlotState& s = slotStates[slot];
	const uint32_t base = slot * kQueriesPerFrame;
	// 两段独立读回：只读各自实际写入部分——复位后未写入的查询为"不可用"，扩读到池
	// 末端必然 VK_NOT_READY（首次真跑踩过）；段不相交 → 复位执行序与跨 CB 记录序均无耦合。
	const std::array<uint32_t, 2> segBase{base, base + kMainQueries};
	const std::array<uint32_t, 2> segCount{s.queryCursor, s.cullCursor};
	for (uint32_t seg = 0; seg < 2; ++seg) {
		if (segCount[seg] == 0)
			continue;  // 该段本帧无写入（cull 未启用时 cull 段恒空）
		const VkResult res = ::vkGetQueryPoolResults(
			static_cast<VkDevice>(*device.getDevice()), static_cast<VkQueryPool>(**pool),
			segBase[seg], segCount[seg], segCount[seg] * sizeof(uint64_t), ticks.data(),
			sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
		if (res == VK_NOT_READY) {
			++drops;  // fence 契约破坏才可达（CB 未完成）——不 reset 越权，本段跳过
			if (auto* lg = RLLogger::getInstance())
				lg->warn("gpu profiler: slot {} segment {} queries not ready, skip readback",
					slot, seg);
			continue;
		}
		if (res != VK_SUCCESS) {
			++drops;
			if (auto* lg = RLLogger::getInstance())
				lg->warn("gpu profiler: getQueryPoolResults failed: {}",
					vk::to_string(vk::Result{res}));
			continue;
		}
		mergeRegions(slot, segBase[seg], segCount[seg], recordedFrame);
	}
}

void GpuProfiler::mergeRegions(uint32_t slot, uint32_t segBase, uint32_t segCount,
	uint64_t recordedFrame) {
	// 校准只管**绝对定位**（债-87）：本机 RADV 的 VK_TIME_DOMAIN_DEVICE 是 boottime 域自由跑
	// 时钟、query 池时间戳另起纪元（实测恒差 15.7h 而 maxDeviation 仍谎报 ~10µs）。旧实现
	// 用同一条仿射同时定起点与时长，两端被同一常量压掉后再被 "ns > 0" 夹成 0 ⟹ 逐区域
	// 非单调、整帧 GPU 区域全丢（债-87 现象）。
	const SteadyCal cal = sampleCalibration();
	SlotState& s = slotStates[slot];
	for (uint32_t i = 0; i < s.regionCount; ++i) {
		const Region& r = s.regions[i];
		if (r.beginQuery < segBase || r.beginQuery >= segBase + segCount)
			continue;  // 非本段区域（begin/end 恒同段分配）
		if (r.endQuery == kInvalid) {
			++drops;  // end 容量丢弃 → 未闭合
			continue;
		}
		// 时长 = 同段同池两 tick 之差（begin 恒先于 end 写入；CB 根本没执行时两端都是
		// 复位值 0 → 差非正被丢），与跨域校准无关 ⟹ 纪元错乱的驱动上读数依旧可信
		const uint64_t beginTick = ticks[r.beginQuery - segBase] & validMask;
		const uint64_t endTick = ticks[r.endQuery - segBase] & validMask;
		if (endTick <= beginTick) {
			++drops;
			continue;
		}
		const int64_t start = cal.toSteady(beginTick, timestampPeriodNs);
		checkEpoch(start, s.hostAnchorNs);  // 整进程一次：纪元不一致则常量平移整条 GPU 轨
		const int64_t anchored = start + epochShiftNs;
		if (anchored <= 0) {
			++drops;  // 平移后仍落不到 host 时间轴（锚点缺失才可达）
			continue;
		}
		ZoneRecord rec;
		rec.startNs = static_cast<uint64_t>(anchored);
		rec.durNs = gpuTicksToNs(endTick - beginTick, timestampPeriodNs);
		rec.frame = recordedFrame;
		rec.depth = r.depth;
		rec.kind = ZoneKind::gpu;
		rec.name = r.name;
		profilerPushRecord(rec);
		++pushedZones;
	}
}

GpuProfiler::SteadyCal GpuProfiler::sampleCalibration() {
	// 设备 tick ↔ CLOCK_MONOTONIC 同次采样（单次调用内近似同刻，偏差受 maxDeviation 约束）；
	// steady_clock = glibc CLOCK_MONOTONIC 同域 → 双读 steady 夹取调用时刻求偏移
	const std::array<vk::CalibratedTimestampInfoKHR, 2> domains{
		vk::CalibratedTimestampInfoKHR{vk::TimeDomainEXT::eDevice},
		vk::CalibratedTimestampInfoKHR{vk::TimeDomainEXT::eClockMonotonic}};
	const uint64_t steadyBefore = steadyNowNs();
	const auto cal = device.getDevice().getCalibratedTimestampsEXT(domains);
	const uint64_t steadyAfter = steadyNowNs();
	lastMaxDeviationNs = cal.second;
	return SteadyCal{
		.devCal = static_cast<int64_t>(cal.first[0] & validMask),
		.monoCal = static_cast<int64_t>(cal.first[1]),
		.steadyOffset = static_cast<int64_t>((steadyBefore + steadyAfter) / 2) -
			static_cast<int64_t>(cal.first[1]),
	};
}

void GpuProfiler::checkEpoch(int64_t calibratedStartNs, uint64_t hostAnchorNs) {
	if (epochChecked)
		return;  // 纪元差是设备级常量 → 整进程判一次，之后所有区域共用同一平移
	epochShiftNs = gpuEpochShiftNs(calibratedStartNs, hostAnchorNs);
	epochChecked = true;
	if (epochShiftNs == 0)
		return;
	if (auto* lg = RLLogger::getInstance())
		lg->warn(
			"gpu profiler: VK_TIME_DOMAIN_DEVICE epoch off host by {:.1f}s (maxDeviation lies "
			"{:.1f}us), gpu track rebased — durations unaffected",
			-static_cast<double>(epochShiftNs) / 1e9,
			static_cast<double>(lastMaxDeviationNs) / 1e3);
}

uint64_t gpuTicksToNs(uint64_t deltaTicks, double periodNs) {
	return static_cast<uint64_t>(static_cast<double>(deltaTicks) * periodNs);
}

int64_t gpuEpochShiftNs(int64_t calibratedStartNs, uint64_t hostAnchorNs) {
	if (hostAnchorNs == 0)
		return 0;  // 无锚点 = 无从判定，宁可不干预也不误平移
	const int64_t delta = calibratedStartNs - static_cast<int64_t>(hostAnchorNs);
	const int64_t magnitude = delta < 0 ? -delta : delta;
	return magnitude > kGpuEpochSanityNs ? -delta : 0;
}

namespace {

/** 当前绑定实例（函数局部静态，POD 指针无析构序问题；main 栈对象不拥有于它）。 */
GpuProfiler*& bound() {
	static GpuProfiler* instance = nullptr;
	return instance;
}

}  // namespace

void bindGpuProfiler(GpuProfiler* profiler) { bound() = profiler; }

void gpuProfilerFrameBegin(vk::raii::CommandBuffer& cmd, uint32_t slot) {
	if (GpuProfiler* g = bound()) {
		auto z = zone("drawFrame.gpuStamp");  // 批 9.5 观测：query 读回归并（滞后 2 帧）+ 复位
		g->frameBegin(cmd, slot);
	}
}

void gpuProfilerCullBegin(vk::raii::CommandBuffer& cmd) {
	if (GpuProfiler* g = bound())
		g->cullBegin(cmd);
}

void gpuProfilerBegin(vk::raii::CommandBuffer& cmd, std::string_view name) {
	if (GpuProfiler* g = bound())
		g->begin(cmd, name);
}

void gpuProfilerEnd(vk::raii::CommandBuffer& cmd) {
	if (GpuProfiler* g = bound())
		g->end(cmd);
}

}  // namespace RGL
