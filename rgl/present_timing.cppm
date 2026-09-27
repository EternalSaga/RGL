// present_timing.cppm — Phase 9.3 VK_EXT_present_timing 测量通路（docs/phase9_plan.md §4）。
// 只测不调度：present 挂 VkPresentTimingsInfoEXT（presentStageQueries = vkswapchain 探测到的
// surface 支持掩码）+ VkPresentId2KHR（单调 present id，结果按 id 升序回收），每帧 collect
// 读回 past timing → IPD（相邻 present 基准 stage 时间差）/ 基准缺失（spec：stage time 0 =
// 无结果，含被合成器丢弃/替换）/ queue-full 回退 / 刷新周期，三路进报告（Perfetto present 轨
// zone + 进程 counter、stats JSON counters 段、rllogger 周期与终值摘要）。
// 时间域 = 优先 VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT，驱动只给 VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT
// 时退而用之（Mesa 26.2 RADV 实测形态，2026-09-27；vkGetSwapchainTimeDomainPropertiesEXT 解析 id，
// 未解析前不挂链；校准按域类型决定 VkSwapchainCalibratedTimestampInfoEXT.presentStage）；
// absolute/relative 调度两 feature 恒 FALSE（vkdevice 侧钉死）。
// 校准：vkGetCalibratedTimestampsKHR 三域同次采样（DEVICE 不用；CLOCK_MONOTONIC +
// swapchain-local 带 VkSwapchainCalibratedTimestampInfoEXT pNext）→ 仿射 steady =
// base + presentNs（与 9.2 同思路；maxDeviation 入终值摘要）。
// 激活门 = vkswapchain 三层判定（设备三扩展组门 ∧ diagnostics.profiler>0 ∧ surface/模式能力），
// 本类只在 swapchain 已激活时工作；未激活 = 全钩子首行早退（默认路径逐位同旧版）。
// 生命周期 = FrameSession 成员（后于 Swapchain 构造）；交换链重建由 collect 内句柄比对自适应
//（时间域/校准/在飞计数复位），不需要外部通知。
export module present_timing;

import rlstd;
import rlvk;
import vk_device;
import vk_swapchain;

namespace RGL {

/**
 * present_timing 累计统计（终值进 rllogger 摘要行；IPD 分位数另存样本向量）。
 */
export struct PresentTimingStats {
	uint64_t attachedPresents = 0;  // 挂链 present 次数（含随后 queue-full 失败者）
	uint64_t completedResults = 0;  // 收回的 reportComplete 结果数
	uint64_t incompletePolls = 0;   // 轮询返回 VK_INCOMPLETE（缓冲不足）次数
	uint64_t basisMissing = 0;      // 完整但基准 stage 无时间戳（0 值语义：未上屏/不支持）
	uint64_t queueFullRetries = 0;  // VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT 回退次数
	uint64_t domainFallbacks = 0;   // 结果时间域 id ≠ 请求 id（引擎回落域）次数
	uint64_t ipdSamples = 0;        // 有效 IPD 样本数
	uint64_t ipdMinNs = 0;          // IPD 极值/均值在线聚合（min 首样本时置）
	uint64_t ipdMaxNs = 0;
	uint64_t ipdSumNs = 0;
	uint64_t refreshDurationNs = 0; // 最近刷新周期（0 = 尚未取到，首 present 前 VK_NOT_READY）
	uint64_t refreshIntervalNs = 0; // 最近刷新间隔（VRR = UINT64_MAX；0 = 不可用）
	bool domainsResolved = false;   // swapchain-local 时间域 id 已解析
	bool propsResolved = false;     // 刷新周期已解析
	bool calibrated = false;        // present 域 → steady 仿射基已建立（zone 落位前提）
};

/**
 * VK_EXT_present_timing 测量会话（FrameSession 成员；进程内单实例语义，帧循环全主线程）。
 * 未激活（swapchain 探测门不过）时 attach 恒 false、collect/onQueueFull 静默早退。
 */
export class PresentTiming {
  public:
	/**
	 * @param device 设备层（轮询/校准入口）
	 * @param swapchain 交换链（激活态与 stage 掩码来源；句柄变化 = 重建检测）
	 */
	PresentTiming(RGL::DeviceContext& device, RGL::Swapchain& swapchain);
	/** 析构：激活且有样本时打终值摘要行（时间域/校准/IPD 分位数/丢帧/queue-full）。 */
	~PresentTiming();

	/** 不可拷贝（引用成员 + 链结构地址稳定性）。 */
	PresentTiming(const PresentTiming&) = delete;
	/** @see PresentTiming(const PresentTiming&) */
	PresentTiming& operator=(const PresentTiming&) = delete;
	/** @see PresentTiming(const PresentTiming&) */
	PresentTiming(PresentTiming&&) = delete;
	/** @see PresentTiming(const PresentTiming&) */
	PresentTiming& operator=(PresentTiming&&) = delete;

	/** @return 是否激活（= swapchain 探测结果，构造期定型）。 */
	bool isActive() const { return active; }

	/**
	 * present 前挂 pNext 链：presentInfo → PresentId2KHR → PresentTimingsInfoEXT → 原链
	 *（present fence 等）。未激活 / 冷却中 / 时间域未解析 = 不挂（返回 false）。
	 * @param presentInfo 调用方组装中的 PresentInfoKHR（pNext 已含本帧 fence 结构）
	 * @return true = 已挂（本帧 present 占一个结果队列槽）
	 */
	bool attach(vk::PresentInfoKHR& presentInfo);

	/**
	 * present 返回 VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT 时调用：计数 + 进回退冷却
	 *（spec：失败 present 不写结果队列 → 在飞计数相应减回；后续 K 次 present 不挂链，
	 * 让合成器反馈把队列排空，之后自动恢复测量）。
	 */
	void onQueueFull();

	/**
	 * 每帧轮询（present 前调用）：句柄变化复位 → 解析时间域/刷新周期 → 校准 →
	 * 收结果并分类 → IPD/丢帧/queue-full/刷新进 counter + present 轨 zone → 周期摘要。
	 */
	void collect();

	/** @return 累计统计 const 引用（诊断/测试读取）。 */
	const PresentTimingStats& getStats() const { return stats; }

  private:
	/** 结果缓冲容量（≥ 队列容量 = imageCount×2；本机 imageCount 3 → 6，富余取 32）。 */
	static constexpr uint32_t kMaxResults = 32;
	/** 单结果 stage 数组容量（spec 定义 4 个 present stage，全掩码也 ≤4）。 */
	static constexpr uint32_t kMaxStages = 4;
	/** queue-full 回退冷却帧数（一次满队列后跳过挂链的 present 次数）。 */
	static constexpr uint32_t kCooldownPresents = 8;
	/** 周期摘要最短间隔（ns；2s 一条 info，长跑观测用）。 */
	static constexpr uint64_t kPeriodicLogNs = 2'000'000'000ull;
	/** IPD 样本向量上限（终值分位数用；超出只走在线 min/max/sum 聚合）。 */
	static constexpr uint32_t kMaxIpdSamples = 262144;
	/** 时间域解析重试上限（collect 每帧一次；约 5s 后仍无 swapchain-local = 停用）。 */
	static constexpr uint32_t kMaxDomainAttempts = 300;

	/** 交换链句柄变化 = 重建：解析态/校准/在飞计数复位（结果缓冲无需清，覆写式复用）。 */
	void resetForSwapchain();
	/** 解析结果时间域 id（两调 idiom）：优先 swapchain-local，缺则取首个 present-stage-local
	 * （Mesa 26.2 实测只 advertise stage-local）；两者都无时不立刻判死（可能在首 present 后
	 * 才 advertise），重试 kMaxDomainAttempts 次仍无 = 停用（见 noteDomainMiss）。
	 * 成功置 stats.domainsResolved + reportDomain + timeDomainId。 */
	void resolveTimeDomains();
	/**
	 * 时间域未命中的统一处置：计次 + 首次/末次各一条 log（附驱动实际返回的域列表，跨驱动
	 * 取证）；达 kMaxDomainAttempts = 停用本会话（active = false）。
	 * @param reason 未命中描述
	 * @param domains 本次查询实际返回的域（空列表场景传空向量）
	 */
	void noteDomainMiss(std::string_view reason, const std::vector<vk::TimeDomainKHR>& domains);
	/** 解析刷新周期/间隔 + timing properties 计数（首 present 前 VK_NOT_READY = 下次再来）。 */
	void resolveTimingProperties();
	/** 建立结果域 → steady 仿射基（CLOCK_MONOTONIC 同次采样；域 = reportDomain，stage-local
	 * 时校准结构的 presentStage 须恰为基准 stage 一位——VU-12228）；失败置 calibFailed 不再重试。 */
	void calibrate();
	/** 轮询结果队列（flags=0 = 全有或无 + 按 present 序）；@return 写入条数（0 = 无/失败）。 */
	uint32_t pollResults();
	/** 逐条消费结果：complete/pending 记账、基准 stage 时间提取、IPD 样本、span zone 落位。 */
	void consumeResults(uint32_t count);
	/** 消费单条完整结果（域回落 / 基准缺失 / IPD / span 四分支，各归一处便于复杂度控制）。 */
	void consumeOne(const vk::PastPresentationTimingEXT& res);
	/** 记录一个 IPD 样本（在线 min/max/sum + 样本向量 + counter 落位）。 */
	void recordIpd(uint64_t ipdNs);
	/** 推一条进程 counter（tsNs = 调用时刻 steady；frame = profilerCurrentFrame）。 */
	void pushCounter(std::string_view name, uint64_t value);
	/** 周期摘要 log（距上次 ≥ kPeriodicLogNs 才打，含当前 IPD 均值与队列在飞数）。 */
	void periodicLog();

	/** present 域 ns → steady ns（校准基；未校准返回值无意义，调用方须先看 stats.calibrated）。 */
	uint64_t toSteadyNs(uint64_t presentNs) const {
		return static_cast<uint64_t>(static_cast<int64_t>(presentNs) + steadyBaseNs);
	}

	RGL::DeviceContext& device;      // 设备层（轮询 + 校准入口）
	RGL::Swapchain& swapchain;       // 交换链（激活态/掩码/句柄）
	bool active = false;             // 构造期自 swapchain.isPresentTimingActive() 定型
	vk::SwapchainKHR boundHandle = nullptr;  // 最近一次解析所绑定的交换链句柄

	// —— present pNext 链成员（地址稳定性 = 成员而非栈临时）——
	vk::PresentId2KHR idInfo{};              // idInfo.pNext = &timingsInfo（构造期钉）
	vk::PresentTimingsInfoEXT timingsInfo{}; // swapchainCount=1, pTimingInfos=&timingInfo
	vk::PresentTimingInfoEXT timingInfo{};   // flags=0 / targetTime=0 = 纯测量
	uint64_t nextPresentId = 1;              // 单调递增 present id
	uint64_t currentPresentId = 0;           // 本帧 id 存储位（idInfo.pPresentIds 指此处）

	// —— 轮询缓冲（结果项 + 每项 stage 数组，成员级地址稳定）——
	std::array<vk::PastPresentationTimingEXT, kMaxResults> results{};
	std::array<std::array<vk::PresentStageTimeEXT, kMaxStages>, kMaxResults> stageBuffers{};

	vk::PresentStageFlagsEXT stageQueries{};    // 挂链用的查询掩码（surface 探测所得）
	vk::PresentStageFlagBitsEXT basisStage = vk::PresentStageFlagBitsEXT::eQueueOperationsEnd;
	uint64_t timeDomainId = 0;                  // 所选时间域 id（swapchain-local 优先，
	                                            //   缺则 stage-local——Mesa 26.2 形态；见 resolveTimeDomains）
	vk::TimeDomainKHR reportDomain = vk::TimeDomainKHR::eSwapchainLocalEXT;  // 所选域类型（校准 presentStage 依此定；结果回读异域时随域改写）
	uint32_t domainAttempts = 0;                // swapchain-local 未命中的解析尝试数（上限后停用）
	uint64_t timingPropsCounter = 0;            // 驱动侧刷新属性计数（变化 = 重解析）
	uint64_t timeDomainsCounter = 0;            // 驱动侧时间域计数（变化 = 重解析）

	bool calibFailed = false;                   // 校准失败（不再重试，zone 落位降级为 counter-only）
	int64_t steadyBaseNs = 0;                   // steady = steadyBaseNs + presentNs
	uint64_t calibMaxDeviationNs = 0;           // 校准散布（终值摘要观测行）

	uint32_t pendingSlots = 0;                  // 已挂链未回收结果的队列占用（观测 + 摘要）
	uint32_t cooldown = 0;                      // queue-full 冷却剩余 present 数
	uint64_t lastBasisNs = 0;                   // 上一条完整结果的基准 stage 时间（0 = 无基线）
	uint64_t lastLogNs = 0;                     // 上次周期摘要时刻（steady ns）

	PresentTimingStats stats;                   // 累计统计（getStats 暴露）
	std::vector<uint64_t> ipdSorted;            // IPD 样本（终值 p50/p95；容量上限见常量）
};

}  // namespace RGL
