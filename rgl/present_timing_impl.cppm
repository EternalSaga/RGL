// present_timing_impl.cppm — Phase 9.3 VK_EXT_present_timing 测量实现（docs/archive/phase9_plan.md §4.2）。
// 结构：构造期钉 pNext 链与轮询缓冲 → collect 每帧「句柄变化复位 → 时间域 → 刷新属性 →
// 校准 → 读回 → 逐条消费 → 周期摘要」→ attach/onQueueFull 供帧会话调用。
// queue-full 回退语义（wsi.adoc「队列操作仍入队、信号量 wait 仍执行」）：重试 present 由
// frame_session 负责摘掉 wait 信号量（同队列 wait-then-signal = 自死锁），本单元只记账 + 冷却。
module;
#include <fmt/format.h>      // 摘要行格式化（rllogger 内部 fmt，此处直接组串）
module present_timing:impl;

import present_timing;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import profiler;   // profilerCurrentFrame / profilerPushRecord / profilerPushCounter
import timing_util;  // steadyNowNs / toMs / percentile（自本文件与 profiler_impl 收敛）
import vk_device;
import vk_swapchain;

namespace RGL {

namespace {

/** stage 掩码位数（VUID-vkGetPastPresentationTimingEXT-flags-12231：结果项 presentStageCount
	 * 输入容量须 ≥ 挂链查询的 stage 数——本项目掩码恒定，直接按位计数）。 */
uint32_t stageBitCount(vk::PresentStageFlagsEXT mask) {
	static constexpr std::array<vk::PresentStageFlagBitsEXT, 4> kAll{
		vk::PresentStageFlagBitsEXT::eQueueOperationsEnd,
		vk::PresentStageFlagBitsEXT::eRequestDequeued,
		vk::PresentStageFlagBitsEXT::eImageFirstPixelOut,
		vk::PresentStageFlagBitsEXT::eImageFirstPixelVisible};
	uint32_t n = 0;
	for (auto bit : kAll)
		if (mask & bit)
			++n;
	return n;
}

/** 取结果里基准 stage 的时间戳（0 = 驱动未提供该 stage 结果，含未上屏）。 */
uint64_t basisTime(const vk::PastPresentationTimingEXT& res, vk::PresentStageFlagBitsEXT basis) {
	for (uint32_t i = 0; i < res.presentStageCount; ++i)
		if (res.pPresentStages[i].stage == basis)
			return res.pPresentStages[i].time;
	return 0;
}

/** ns → ms 与 percentile 见 `timing_util.cppm`（与 profiler_impl 共用一份口径）。 */

}  // namespace

PresentTiming::PresentTiming(DeviceContext& dev, Swapchain& sw) : device(dev), swapchain(sw) {
	active = sw.isPresentTimingActive();
	if (!active)
		return;
	stageQueries = sw.getPresentStageQueries();
	// 基准 stage 优先级：可见像素 > 首像素输出 > 请求出队 > 队列操作末（越靠后越近显示节奏）
	if (stageQueries & vk::PresentStageFlagBitsEXT::eImageFirstPixelVisible)
		basisStage = vk::PresentStageFlagBitsEXT::eImageFirstPixelVisible;
	else if (stageQueries & vk::PresentStageFlagBitsEXT::eImageFirstPixelOut)
		basisStage = vk::PresentStageFlagBitsEXT::eImageFirstPixelOut;
	else if (stageQueries & vk::PresentStageFlagBitsEXT::eRequestDequeued)
		basisStage = vk::PresentStageFlagBitsEXT::eRequestDequeued;
	else
		basisStage = vk::PresentStageFlagBitsEXT::eQueueOperationsEnd;

	// present pNext 链：presentInfo → idInfo → timingsInfo → （调用方原链）
	timingsInfo.swapchainCount = 1;
	timingsInfo.pTimingInfos = &timingInfo;
	idInfo.pNext = &timingsInfo;
	idInfo.swapchainCount = 1;
	idInfo.pPresentIds = &currentPresentId;  // 成员数组（地址稳定），值每帧 attach 写
	timingInfo.presentStageQueries = stageQueries;
	// flags=0 / targetTime=0 / targetTimeDomainPresentStage=0 = 纯测量不调度；
	// timeDomainId 每帧 attach 时写（解析前不挂链）

	boundHandle = *sw.getSwapchain();
	if (auto* logger = RLLogger::getInstance())
		logger->info("present_timing: active (basis stage {}, mask {:08x})",
			vk::to_string(basisStage), static_cast<uint32_t>(stageQueries));
}

PresentTiming::~PresentTiming() {
	if (!active || stats.attachedPresents == 0)
		return;  // 无数据不打扰（激活但 0 present 的短跑）
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	std::ranges::sort(ipdSorted);
	const double meanMs = stats.ipdSamples
		? toMs(stats.ipdSumNs / stats.ipdSamples) : 0.0;
	logger->info("present_timing summary: presents={} results={} queueFull={} basisMissing={} "
		"incompletePolls={} domainFallbacks={} pending={}",
		stats.attachedPresents, stats.completedResults, stats.queueFullRetries,
		stats.basisMissing, stats.incompletePolls, stats.domainFallbacks, pendingSlots);
	logger->info("present_timing IPD: n={} mean={:.3f}ms min={:.3f} p50={:.3f} p95={:.3f} max={:.3f}",
		stats.ipdSamples, meanMs, toMs(stats.ipdMinNs), toMs(percentile(ipdSorted, 0.50)),
		toMs(percentile(ipdSorted, 0.95)), toMs(stats.ipdMaxNs));
	logger->info("present_timing clock: refreshDuration={:.3f}ms refreshInterval={:.3f}ms "
		"domainsResolved={} propsResolved={} calibrated={} maxDeviation={}ns",
		toMs(stats.refreshDurationNs), toMs(stats.refreshIntervalNs), stats.domainsResolved,
		stats.propsResolved, stats.calibrated, calibMaxDeviationNs);
}

bool PresentTiming::attach(vk::PresentInfoKHR& presentInfo) {
	if (!active)
		return false;
	if (cooldown > 0) {
		--cooldown;  // 回退冷却：跳过挂链让结果队列排空
		return false;
	}
	if (!stats.domainsResolved)
		return false;  // 时间域 id 未知 = presentStageQueries 无法落域（spec 要求 id 有效）
	currentPresentId = nextPresentId++;
	timingInfo.timeDomainId = timeDomainId;
	timingsInfo.pNext = presentInfo.pNext;  // 原链（present fence 等）接到我们之后
	presentInfo.pNext = &idInfo;
	++pendingSlots;
	++stats.attachedPresents;
	return true;
}

void PresentTiming::onQueueFull() {
	if (!active)
		return;
	++stats.queueFullRetries;
	if (pendingSlots > 0)
		--pendingSlots;  // 失败 present 不写结果队列（spec），槽位记账相应减回
	cooldown = kCooldownPresents;
	pushCounter("present.queue_full", stats.queueFullRetries);
}

void PresentTiming::collect() {
	if (!active)
		return;
	if (*swapchain.getSwapchain() != boundHandle) {
		boundHandle = *swapchain.getSwapchain();
		resetForSwapchain();
	}
	if (!stats.domainsResolved)
		resolveTimeDomains();
	if (active && stats.domainsResolved && !stats.propsResolved)
		resolveTimingProperties();
	if (active && stats.domainsResolved && !stats.calibrated && !calibFailed)
		calibrate();
	if (active && stats.domainsResolved)
		consumeResults(pollResults());
	if (active)
		periodicLog();
}

void PresentTiming::resetForSwapchain() {
	stats.domainsResolved = false;
	stats.propsResolved = false;
	stats.calibrated = false;
	calibFailed = false;  // 新交换链重新尝试校准
	domainAttempts = 0;
	pendingSlots = 0;
	cooldown = 0;
	lastBasisNs = 0;
	timingPropsCounter = 0;
	timeDomainsCounter = 0;
}

void PresentTiming::resolveTimeDomains() {
	// 两调 idiom：先探条数（VU-12370：pTimeDomains/pTimeDomainIds 同 NULL 同非 NULL）
	vk::SwapchainTimeDomainPropertiesEXT probe{};
	uint64_t counter = 0;
	auto* logger = RLLogger::getInstance();
	auto r = swapchain.getSwapchain().getTimeDomainPropertiesEXT(&probe, &counter);
	if (r < vk::Result::eSuccess) {
		if (logger)
			logger->warn("present_timing: time domain query failed ({}) -> disabled",
				vk::to_string(r));
		active = false;
		return;
	}
	if (probe.timeDomainCount == 0) {
		noteDomainMiss("empty domain list", {});
		return;
	}
	std::vector<vk::TimeDomainKHR> domains(probe.timeDomainCount);
	std::vector<uint64_t> ids(probe.timeDomainCount);
	vk::SwapchainTimeDomainPropertiesEXT props{};
	props.timeDomainCount = probe.timeDomainCount;
	props.pTimeDomains = domains.data();
	props.pTimeDomainIds = ids.data();
	r = swapchain.getSwapchain().getTimeDomainPropertiesEXT(&props, &counter);
	if (r < vk::Result::eSuccess) {
		if (logger)
			logger->warn("present_timing: time domain fetch failed ({}) -> disabled",
				vk::to_string(r));
		active = false;
		return;
	}
	// 优先 swapchain-local（跨 stage 共域，最省事）；Mesa 26.2 RADV 实测只 advertise
	// stage-local → 取首个该域项：本会话全部时间都落同一域内，IPD 同域相减仍成立
	auto swapIt = std::ranges::find(domains, vk::TimeDomainKHR::eSwapchainLocalEXT);
	auto stageIt = std::ranges::find(domains, vk::TimeDomainKHR::ePresentStageLocalEXT);
	auto chosen = swapIt != domains.end() ? swapIt : stageIt;
	if (chosen == domains.end()) {
		noteDomainMiss("no swapchain-local / stage-local domain", domains);
		return;
	}
	reportDomain = *chosen;
	timeDomainId = ids[static_cast<size_t>(std::ranges::distance(domains.begin(), chosen))];
	timeDomainsCounter = counter;
	stats.domainsResolved = true;
	if (logger)
		logger->info("present_timing: {} domains, report {} id={}, counter={}",
			domains.size(), vk::to_string(reportDomain), timeDomainId, counter);
}

void PresentTiming::noteDomainMiss(std::string_view reason,
	const std::vector<vk::TimeDomainKHR>& domains) {
	++domainAttempts;
	auto* logger = RLLogger::getInstance();
	const bool firstOrLast = domainAttempts == 1 || domainAttempts >= kMaxDomainAttempts;
	if (logger && firstOrLast) {
		std::string list;
		for (auto d : domains)
			list += vk::to_string(d) + ' ';
		logger->info("present_timing: {} (attempt {}/{}; driver list: {})",
			reason, domainAttempts, kMaxDomainAttempts, list);
	}
	if (domainAttempts >= kMaxDomainAttempts) {
		if (logger)
			logger->warn("present_timing: swapchain-local domain unavailable -> disabled");
		active = false;
	}
}

void PresentTiming::resolveTimingProperties() {
	// raii 包装允许 eSuccess/eNotReady（首 present 前 = NOT_READY 常态，不抛）；真正的
	// 错误结果才抛 SystemError，捕获后不置 resolved（下帧重试）
	try {
		auto rv = swapchain.getSwapchain().getTimingPropertiesEXT();
		if (rv.result != vk::Result::eSuccess)
			return;  // NOT_READY：刷新属性尚不可得（等首 present）
		const auto& props = rv.value.first;
		const uint64_t counter = rv.value.second;
		stats.refreshDurationNs = props.refreshDuration;
		stats.refreshIntervalNs = props.refreshInterval;
		stats.propsResolved = true;
		if (counter != timingPropsCounter) {
			timingPropsCounter = counter;
			if (auto* logger = RLLogger::getInstance())
				logger->info("present_timing: refresh props {:.3f}ms (interval {:.3f}ms, counter {})",
					toMs(props.refreshDuration), toMs(props.refreshInterval), counter);
			if (props.refreshDuration > 0)
				pushCounter("present.refresh_ns", props.refreshDuration);
		}
	} catch (const vk::SystemError&) {
		// 驱动异常结果：本轮跳过（常态是 NOT_READY 已在上方短路，此处极少进）
	}
}

void PresentTiming::calibrate() {
	// VU-VkCalibratedTimestampInfoKHR-timeDomain-02354：域须在可校准列表内；12227：
	// swapchain-local 必须链 VkSwapchainCalibratedTimestampInfoEXT（presentStage=0）
	auto* logger = RLLogger::getInstance();
	std::vector<vk::TimeDomainKHR> domains;
	try {
		domains = device.getPhysicalDevice().getCalibrateableTimeDomainsKHR();
	} catch (const vk::SystemError& e) {
		if (logger)
			logger->warn("present_timing: calibrateable domains query failed ({}) -> zones disabled",
				e.what());
		calibFailed = true;
		return;
	}
	auto has = [&](vk::TimeDomainKHR d) {
		return std::ranges::find(domains, d) != domains.end();
	};
	if (!has(vk::TimeDomainKHR::eClockMonotonic) || !has(reportDomain)) {
		if (logger)
			logger->warn("present_timing: calibrateable domains lack mono/{} "
				"-> zones disabled (counters only)", vk::to_string(reportDomain));
		calibFailed = true;
		return;
	}
	vk::SwapchainCalibratedTimestampInfoEXT swInfo{};
	swInfo.swapchain = *swapchain.getSwapchain();
	// VU-12228：stage-local 域必须恰指定一个 present stage（用基准 stage）；swapchain-local 填 0
	swInfo.presentStage = reportDomain == vk::TimeDomainKHR::ePresentStageLocalEXT
		? vk::PresentStageFlagsEXT(basisStage) : vk::PresentStageFlagsEXT{};
	swInfo.timeDomainId = timeDomainId;
	const std::array<vk::CalibratedTimestampInfoKHR, 2> infos{
		vk::CalibratedTimestampInfoKHR{vk::TimeDomainKHR::eClockMonotonic},
		vk::CalibratedTimestampInfoKHR{reportDomain, &swInfo}};
	const uint64_t steadyBefore = steadyNowNs();
	try {
		const auto cal = device.getDevice().getCalibratedTimestampsKHR(infos);
		const uint64_t steadyAfter = steadyNowNs();
		calibMaxDeviationNs = cal.second;
		if (cal.first[1] == 0) {
			// spec：timeDomainId 不再支持时驱动可报 0
			if (logger)
				logger->warn("present_timing: swapchain-local calibration returned 0 -> zones disabled");
			calibFailed = true;
			return;
		}
		steadyBaseNs = static_cast<int64_t>((steadyBefore + steadyAfter) / 2)
			- static_cast<int64_t>(cal.first[1]);
		stats.calibrated = true;
		if (logger)
			logger->info("present_timing: calibrated present-domain -> steady, maxDeviation={}ns",
				calibMaxDeviationNs);
	} catch (const vk::SystemError& e) {
		if (logger)
			logger->warn("present_timing: calibration failed ({}) -> zones disabled", e.what());
		calibFailed = true;
	}
}

uint32_t PresentTiming::pollResults() {
	// 驱动会覆写结果项字段（含 presentStageCount），故每轮重钉 pPresentStages 指针与
	// 容量（VUID-vkGetPastPresentationTimingEXT-flags-12231：容量须 ≥ 该 present 查询的
	// stage 数——本项目掩码恒定 = 同容量）
	const uint32_t stageCapacity = stageBitCount(stageQueries);
	for (uint32_t i = 0; i < kMaxResults; ++i) {
		results[i].sType = vk::StructureType::ePastPresentationTimingEXT;
		results[i].pPresentStages = stageBuffers[i].data();
		results[i].presentStageCount = stageCapacity;
	}
	vk::PastPresentationTimingInfoEXT info{};
	info.flags = {};  // 不带 PARTIAL/OUT_OF_ORDER：全有或无 + 按 present 顺序
	info.swapchain = *swapchain.getSwapchain();
	vk::PastPresentationTimingPropertiesEXT props{};
	props.presentationTimingCount = kMaxResults;
	props.pPresentationTimings = results.data();
	auto r = device.getDevice().getPastPresentationTimingEXT(&info, &props);
	if (r == vk::Result::eIncomplete)
		++stats.incompletePolls;
	// 驱动侧计数变化 = 刷新属性/时间域列表已变，下轮重解析（含重新校准）
	if (props.timingPropertiesCounter != timingPropsCounter)
		stats.propsResolved = false;
	if (props.timeDomainsCounter != timeDomainsCounter) {
		stats.domainsResolved = false;
		stats.calibrated = false;
	}
	if (r < vk::Result::eSuccess)
		return 0;  // NOT_READY / 其它：本轮无结果
	return props.presentationTimingCount;
}

void PresentTiming::consumeResults(uint32_t count) {
	for (uint32_t i = 0; i < count; ++i)
		consumeOne(results[i]);
}

void PresentTiming::consumeOne(const vk::PastPresentationTimingEXT& res) {
	if (!res.reportComplete)
		return;  // 未开 PARTIAL 理论不出现；出现则槽位未释放（pending 不减）
	++stats.completedResults;
	if (pendingSlots > 0)
		--pendingSlots;
	if (stats.completedResults == 1) {
		// 首条样本明细一次性入 log（跨驱动取证：哪些 stage 真给时间、域是否与请求一致；
		// 决定 span zone 能否成立——stage-local 域下他 stage 时间可能填 0）
		std::string dump;
		for (uint32_t k = 0; k < res.presentStageCount; ++k)
			dump += fmt::format("{}={} ", vk::to_string(res.pPresentStages[k].stage),
				res.pPresentStages[k].time);
		if (auto* logger = RLLogger::getInstance())
			logger->info("present_timing: first result: domain={} id={} presentId={} stages: {}",
				vk::to_string(res.timeDomain), res.timeDomainId, res.presentId, dump);
	}
	if (res.timeDomainId != timeDomainId || res.timeDomain != reportDomain) {
		// 引擎回落他域：后续 attach/校准随域改写，本条不落 zone/不产 IPD（跨域不可比）
		++stats.domainFallbacks;
		reportDomain = res.timeDomain;
		timeDomainId = res.timeDomainId;
		stats.calibrated = false;
		lastBasisNs = 0;
		return;
	}
	const uint64_t basisNs = basisTime(res, basisStage);
	if (basisNs == 0) {
		++stats.basisMissing;  // spec：0 = 该 stage 无结果（未上屏 / 被替换）
		pushCounter("present.drop", stats.basisMissing);
		lastBasisNs = 0;       // 断基线：下一条重新起算，免跨丢帧假 IPD
		return;
	}
	if (lastBasisNs != 0 && basisNs > lastBasisNs)
		recordIpd(basisNs - lastBasisNs);
	lastBasisNs = basisNs;
	if (stats.calibrated) {
		// 基准 stage 瞬时落位（plan §5 的 stage 事件形态）。**不做跨 stage 跨度**：Mesa 26.2
		// RADV 实测 stage-local 域下 QueueOperationsEnd 与 ImageFirstPixelOut 历元相差 ~10s
		//（首条结果 log 为证），跨 stage 相减无意义；同 stage 相邻差（IPD）仍精确 = 刷新倍数。
		// 校准的 presentStage 即基准 stage（见 calibrate），故本条 startNs 与 CPU/GPU 轨同源。
		ZoneRecord rec;
		rec.startNs = toSteadyNs(basisNs);
		rec.durNs = 0;
		rec.frame = profilerCurrentFrame();
		rec.kind = ZoneKind::present;
		rec.name = "present.stage";  // 静态字面量（活到 dump）
		profilerPushRecord(rec);
	}
}

void PresentTiming::recordIpd(uint64_t ipdNs) {
	if (stats.ipdSamples == 0 || ipdNs < stats.ipdMinNs)
		stats.ipdMinNs = ipdNs;
	if (ipdNs > stats.ipdMaxNs)
		stats.ipdMaxNs = ipdNs;
	stats.ipdSumNs += ipdNs;
	++stats.ipdSamples;
	if (ipdSorted.size() < kMaxIpdSamples)
		ipdSorted.push_back(ipdNs);
	pushCounter("present.ipd", ipdNs);
}

void PresentTiming::pushCounter(std::string_view name, uint64_t value) {
	CounterRecord rec;
	rec.frame = profilerCurrentFrame();
	rec.tsNs = steadyNowNs();
	rec.value = value;
	rec.name = name;
	profilerPushCounter(rec);
}

void PresentTiming::periodicLog() {
	if (stats.attachedPresents == 0)
		return;
	const uint64_t now = steadyNowNs();
	if (now - lastLogNs < kPeriodicLogNs)
		return;
	lastLogNs = now;
	const double meanMs = stats.ipdSamples
		? toMs(stats.ipdSumNs / stats.ipdSamples) : 0.0;
	const double refreshMs = stats.refreshDurationNs ? toMs(stats.refreshDurationNs) : 0.0;
	if (auto* logger = RLLogger::getInstance())
		logger->info("present_timing: presents={} results={} ipd_mean={:.3f}ms refresh={:.3f}ms "
			"drop={} queueFull={} pending={} cooldown={}",
			stats.attachedPresents, stats.completedResults, meanMs, refreshMs,
			stats.basisMissing, stats.queueFullRetries, pendingSlots, cooldown);
}

}  // namespace RGL
