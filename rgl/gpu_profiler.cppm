// gpu_profiler.cppm — Phase 9.2 GPU 时间戳 profiler（docs/phase9_plan.md 批 9.2）。
// 数据通路：帧槽 timestamp query pool（kGpuSlots × kQueriesPerFrame，槽内**双段不相交**：
// 主段 [0,kMainQueries) = gpu.frame+pass，cull 段 [kMainQueries,+) = cull）+ 成对
// vkCmdWriteTimestamp2 区域（配对栈嵌套）→ 槽复用时（drawFrame 在 session.beginFrame
// 已等完 fence → 查询必就绪）两段分别读回。查询复位 = CB 内 vkCmdResetQueryPool
//（host 版需 hostQueryReset 特性——spec 可选，未启用即 VVL 报错，CB 内复位零特性依赖）：
// 主段复位记主 CB 头、cull 段复位记 cull CB 头——两段不相交，cull CB 先于主 CB 执行
//（同帧 fence 同步等待）也互不抹除；各自复位先于自身时间戳记录，规避 VVL 跨 CB 记录序。
// VK_EXT_calibrated_timestamps 同次采样设备 tick 与 CLOCK_MONOTONIC + 双读
// steady_clock 求偏移 → 仿射换算到 steady ns → profilerPushRecord 归并（同报告
// gpu 轨 tid=1）。开关 = diagnostics.profiler（0 = 全 no-op）∧ 校准扩展位
//（features_config optional_extensions）∧ 队列 timestampComputeAndGraphics；
// 对象 main 栈持有（声明序在 backEnd 后 = 先于 device 析构，GPU 资源生命周期规则），
// 经 bindGpuProfiler 挂进帧循环钩子。归并滞后槽复用节奏（2 帧）→ 诊断窗口需
// diagnostics.profiler ≥ 3，否则记录计 orphan drops（构造期告警）。
export module gpu_profiler;

import rlstd;
import rlvk;
import vk_device;

namespace RGL {

/**
 * GPU 时间戳会话：timestamp query pool 所有者 + 每帧槽区域状态。main 栈构造
 *（`GpuProfiler gpuProfiler(vkDevice);` + `bindGpuProfiler(&gpuProfiler)`），进程内
 * 单例语义（帧循环全主线程）。构造期按 配置/扩展/队列能力 三重门控定型，任一不过
 * = 不建 pool，之后一切钩子早退（默认路径零 GPU 影响）。
 */
export class GpuProfiler {
  public:
	/**
	 * @param device 设备上下文（逻辑/物理设备、可选扩展位、队列族索引）
	 * 门控：diagnostics().profiler == 0 → 静默不建 pool；calibrated_timestamps 扩展
	 * 缺位 / 队列不支持 timestamp → warn 后不建 pool（9.3 跨机缺位不破坏启动）。
	 */
	explicit GpuProfiler(DeviceContext& device);
	/** 析构：pool 随对象销毁（main 栈序保证 device 仍活）；打 drops/校准偏差观测行。 */
	~GpuProfiler();

	/** 不可拷贝（pool 所有权 + 引用成员）。 */
	GpuProfiler(const GpuProfiler&) = delete;
	/** @see GpuProfiler(const GpuProfiler&) */
	GpuProfiler& operator=(const GpuProfiler&) = delete;
	/** @see GpuProfiler(const GpuProfiler&) */
	GpuProfiler(GpuProfiler&&) = delete;
	/** @see GpuProfiler(const GpuProfiler&) */
	GpuProfiler& operator=(GpuProfiler&&) = delete;

	/**
	 * 帧开始钩子（drawFrame 在 session.beginFrame 等完 fence 后、录制前调用）：
	 * 读回本槽上一轮两段区域并归并（须先于复位）→ 主段复位记入主 CB 头 →
	 * 游标/配对栈复位、登记本轮区域的归属逻辑帧。
	 * @param cmd 本帧主命令缓冲（主段复位记入点）
	 * @param slot 帧槽（session.currentFrameSlot()；≥ kGpuSlots = 防御早退）
	 */
	void frameBegin(vk::raii::CommandBuffer& cmd, uint32_t slot);

	/**
	 * cull CB 首钩子（runFrame 在 ccmd.begin 后调用）：cull 段复位记入 cull CB 头
	 *（与主段不相交、自身先于 cull 时间戳 → 执行序/记录序双无关）→ cull 区域开始。
	 * @param cmd 录制中的 cull 命令缓冲
	 */
	void cullBegin(vk::raii::CommandBuffer& cmd);

	/**
	 * 主段区域开始钩子（gpu.frame 跨度 / pass——同槽配对栈嵌套，实际深度 ≤ 2）。
	 * 写 eAllCommands 起点时间戳 + 区域入表；容量溢出 = 丢弃计数 + 栈压无效标记保配对。
	 * @param cmd 正在录制的命令缓冲
	 * @param name 区域名（字面量 / RgPassPlan.name：renderGraph 为 Renderer 成员整进程
	 *   存活，生命周期 ≥ 读回帧）
	 */
	void begin(vk::raii::CommandBuffer& cmd, std::string_view name);

	/**
	 * 区域收口钩子：写终点时间戳并闭合栈顶区域；栈空 = 结构违约防御早退。
	 * @param cmd 与 begin 同一命令缓冲
	 */
	void end(vk::raii::CommandBuffer& cmd);

  private:
	/** 每帧槽位数（槽复用节奏 = kFramesInFlight；分层故不 import frame_session——改值须与其 kFramesInFlight 同步）。 */
	static constexpr uint32_t kGpuSlots = 2;
	/** 主段查询数（gpu.frame + passes ≤ 18 实用；复位记主 CB）。 */
	static constexpr uint32_t kMainQueries = 48;
	/** cull 段查询数（cull 区域 = 2 实用；复位记 cull CB——与主段不相交是双 CB 时序成立的前提）。 */
	static constexpr uint32_t kCullQueries = 16;
	/** 每帧槽宽 = 主段 + cull 段（槽内绝对下标 = slot×本值 + 段内偏移）。 */
	static constexpr uint32_t kQueriesPerFrame = kMainQueries + kCullQueries;
	/** 每帧区域上限（gpu.frame + passes + cull；超出计 drops）。 */
	static constexpr uint32_t kMaxRegions = 24;
	/** 区域配对栈深上限（实际结构深度 ≤ 2；溢出 = 结构违约，防御性丢弃）。 */
	static constexpr uint32_t kMaxDepth = 8;
	/** 查询下标/栈标记无效值（未闭合、容量丢弃；区域下标恒 < kMaxRegions 不混淆）。 */
	static constexpr uint32_t kInvalid = ~uint32_t{0};

	/** 单区域记录（begin/end 查询下标对；endQuery 未闭合 = 读回时丢弃计数）。 */
	struct Region {
		std::string_view name;
		uint32_t beginQuery = kInvalid;
		uint32_t endQuery = kInvalid;
		uint32_t depth = 0;
	};

	/** 每帧槽状态（frameBegin 复位并登记归属帧；readback 消费上一轮两段游标）。 */
	struct SlotState {
		uint64_t frame = 0;       // 区域归属逻辑帧（frameBegin 时 profilerCurrentFrame()）
		uint32_t queryCursor = 0; // 主段查询分配游标
		uint32_t cullCursor = 0;  // cull 段查询分配游标
		uint32_t regionCount = 0; // 有效区域数（读回上一轮后清零）
		uint32_t openDepth = 0;   // 配对栈顶
		std::array<uint32_t, kMaxDepth> openStack{};
		std::array<Region, kMaxRegions> regions{};
	};

	/**
	 * 区域开始共用体（begin 主段 / cullBegin cull 段的分派核心）：段内游标分配
	 * 绝对下标 + 区域入表 + 起点时间戳 + 配对栈压入。
	 * @param cmd 正在录制的命令缓冲 @param name 区域名 @param cull 落 cull 段还是主段
	 */
	void beginIn(vk::raii::CommandBuffer& cmd, std::string_view name, bool cull);

	/**
	 * 读回并归并本槽上一轮两段区域：每段单独 vkGetQueryPoolResults（只读实际写入
	 * 部分——复位后未写入查询不可用，扩读必然 VK_NOT_READY；非 SUCCESS 计 drops 跳段）→
	 * 逐段 mergeRegions 换算归并（frame = recordedFrame；滞后 2 帧需窗口 ≥ 3）。
	 * @param slot 槽号
	 * @param recordedFrame 区域记录时的逻辑帧（frameBegin 登记值）
	 */
	void readback(uint32_t slot, uint64_t recordedFrame);

	/**
	 * 单段区域换算与归并：段内校准（设备 tick ↔ CLOCK_MONOTONIC 同次采样 + 双读
	 * steady_clock 求偏移，仿射换算 steady ns——校准为单次 ext 调用，每段各采一次
	 * 成本可忽略且更贴近读回时刻）→ 有效区域逐条 profilerPushRecord。
	 * @param slot 槽号 @param segBase 段起始绝对查询下标 @param segCount 段内已写入数
	 * @param recordedFrame 区域归属逻辑帧（frameBegin 登记值）
	 */
	void mergeRegions(uint32_t slot, uint32_t segBase, uint32_t segCount,
		uint64_t recordedFrame);

	DeviceContext& device;                    // main 装配层持有，生命周期覆盖本对象
	std::optional<vk::raii::QueryPool> pool;  // nullopt = 门控未过（关）
	std::vector<uint64_t> ticks;              // 读回复用缓冲（构造一次分配）
	float timestampPeriodNs = 0.0f;           // limits.timestampPeriod（ns/tick）
	uint64_t validMask = ~uint64_t{0};        // timestampValidBits 掩码（<64 位机型高位保留）
	uint32_t activeSlot = 0;                  // frameBegin 登记（begin/end 槽归属）
	uint64_t drops = 0;                       // 容量/未闭合/未就绪丢弃累计（析构日志）
	uint64_t lastMaxDeviationNs = 0;          // 最近一次校准 maxDeviation（观测）
	std::array<SlotState, kGpuSlots> slotStates{};
};

/**
 * 绑定帧循环钩子的当前 GpuProfiler 实例（main 栈对象；nullptr = 解绑）。
 * 单例语义：帧循环单线程读写，绑定后全帧钩子经它转发。
 * @param profiler 实例指针（不持有所有权）
 */
export void bindGpuProfiler(GpuProfiler* profiler);

/**
 * 帧开始钩子转发（未绑定 = no-op）。drawFrame 在 beginFrame 等完 fence 后调用。
 * @param cmd 本帧主命令缓冲（主段复位记入点）@param slot 帧槽（session.currentFrameSlot()）
 */
export void gpuProfilerFrameBegin(vk::raii::CommandBuffer& cmd, uint32_t slot);

/**
 * cull CB 首钩子转发（cull 段复位 + cull 区域开始；未绑定 = no-op）。
 * @param cmd 录制中的 cull 命令缓冲
 */
export void gpuProfilerCullBegin(vk::raii::CommandBuffer& cmd);

/**
 * 区域开始钩子转发（未绑定 = no-op）。 @param cmd 录制中命令缓冲 @param name 区域名
 */
export void gpuProfilerBegin(vk::raii::CommandBuffer& cmd, std::string_view name);

/**
 * 区域收口钩子转发（与最近一次 gpuProfilerBegin 配对；未绑定 = no-op）。
 * @param cmd 与 begin 同一命令缓冲
 */
export void gpuProfilerEnd(vk::raii::CommandBuffer& cmd);

}  // namespace RGL
