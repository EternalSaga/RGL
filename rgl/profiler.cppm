// profiler.cppm — Phase 9 CPU zone profiler 核心接口（docs/phase9_plan.md 批 9.1）。
// 零宏设计（D4 翻案 2026-09-27）：调用形态 auto z = RGL::zone("label");——
// source_location 默认实参在调用点求值，捕获 file:line/function（本项目编译规则传
// 绝对源路径，build/build.ninja CXX_COMPILER 行 → file 免费为绝对路径）。
// 开关 = diagnostics.profiler（帧窗口 N，0=关）；关闭时 ctor 首行早退近零开销。
// 存储 = 单进程单线程环形帧槽（录制期全主线程；9.2 GPU/9.3 present 记录同线程回填）。
export module profiler;

import rlstd;

namespace RGL {

/**
 * zone 类别 → Perfetto trace 分轨（cpu → tid main；gpu/present 批 9.2/9.3 消费）。
 */
export enum class ZoneKind { cpu, gpu, present };

/**
 * 单条 zone 记录（值快照）。name/file 为静态字符串或生命周期 ≥ dump 的 string_view
 * （字面量 / FrameSystem.name / RgPassPlan.name 均满足）；时间单位 ns（steady_clock 域，
 * 与 9.2 VK_EXT_calibrated_timestamps 的 CLOCK_MONOTONIC 校准目标同源 → CPU/GPU 同轴）。
 */
export struct ZoneRecord {
	uint64_t startNs = 0;   // 起点（steady_clock since epoch，ns）
	uint64_t durNs = 0;     // 时长（ns）
	uint64_t frame = 0;     // 所属逻辑帧号（profilerFrameBegin 计数）
	uint32_t depth = 0;     // 同线程嵌套深度（0 = 帧顶层相位）
	uint32_t line = 0;      // 调用点行号
	ZoneKind kind = ZoneKind::cpu;
	std::string_view name;  // label 或 source_location::function_name()
	std::string_view file;  // 调用点文件路径
};

/**
 * CPU zone RAII 守卫：ctor 记起点并占一层深度，dtor 写入当前帧槽（深度弹栈）。
 * 单线程假设（帧循环全在主线程）；不可拷贝/移动（栈作用域配对语义，auto z = zone()
 * 经 prvalue 拷贝消除构造，无需移动）。
 */
export class Zone {
  public:
	/**
	 * @param label zone 标签；空 = 回落 loc.function_name()（报告附 file:line）
	 * @param loc 调用点位置（默认实参在**调用处**求值 = 用户代码位置）
	 */
	explicit Zone(std::string_view label = {},
		std::source_location loc = std::source_location::current());
	/** 析构收口：把记录写入当前帧槽（关闭/未开帧时早退）。 */
	~Zone();

	/** 不可拷贝/移动（RAII 栈配对；工厂返回 prvalue 依赖拷贝消除，无需移动）。 */
	Zone(const Zone&) = delete;
	/** @see Zone(const Zone&) */
	Zone& operator=(const Zone&) = delete;
	/** @see Zone(const Zone&) */
	Zone(Zone&&) = delete;
	/** @see Zone(const Zone&) */
	Zone& operator=(Zone&&) = delete;

  private:
	std::string_view label;  // 空 = 用 loc.function_name() 兜底
	std::source_location loc;
	uint64_t startNs = 0;
	uint32_t depth = 0;
	bool active = false;
};

/**
 * zone 工厂（惯用形态 `auto z = RGL::zone("staging");` / `auto z = RGL::zone();`）。
 * @param label zone 标签（空 = 回落调用点函数名）
 * @param loc 调用点位置（默认实参）
 * @return RAII Zone，离开作用域自动收口
 */
export Zone zone(std::string_view label = {},
	std::source_location loc = std::source_location::current());

/**
 * 帧边界推进（renderSystem 帧循环顶调用）：切帧槽 + 帧墙钟间隔（上一帧整周期时长）
 * 统计。diagnostics.profiler = 0 时整体早退。
 */
export void profilerFrameBegin();

/**
 * 当前逻辑帧号（profilerFrameBegin 计数；首帧前 = 0）。9.2 GPU 区域登记归属帧用
 *（frameBegin 时点 = drawFrame 录制帧，非读回帧）。
 */
export uint64_t profilerCurrentFrame();

/**
 * 外部记录回填（同线程；9.2 GPU 读回 / 9.3 present 消费）：按 rec.frame 落环形槽。
 * 槽已被挤出 = 计 orphan drops（GPU 归并滞后 2 帧 → 需 diagnostics.profiler ≥ 3，
 * 构造期告警）；window = 0 / 未开帧 = 静默 no-op。
 * @param rec 完整记录（kind 决定 trace 分轨；name/file 须活到 dump）
 */
export void profilerPushRecord(const ZoneRecord& rec);

/**
 * 计数记录（批 9.3 present_timing）：标量采样（IPD ns、丢帧/queue-full 累计、刷新
 * 周期 ns）→ Perfetto `ph:"C"` 进程级 counter 轨 + stats JSON `counters` 段
 *（count/last/min/max/avg）+ rllogger 摘要行。落位与 zone 同帧槽，窗口钳制/溢出
 * drops/orphan drops 三语义一致。
 */
export struct CounterRecord {
	uint64_t frame = 0;      // 归属逻辑帧号（profilerCurrentFrame() 采样时刻取值）
	uint64_t tsNs = 0;       // 采样时刻（steady_clock ns；trace 时间轴定位）
	uint64_t value = 0;      // 计数值（单位由 name 定：ns 或累计次数）
	std::string_view name;   // 计数器名（须静态字面量等 ≥ dump 生命周期）
};

/**
 * 推入一条计数（window = 0 / 未开帧 = 静默 no-op；目标帧已挤出 = 计 orphanDrops）。
 * @param rec 完整计数记录
 */
export void profilerPushCounter(const CounterRecord& rec);

/**
 * 帧命令累加器（批 9.5 观测面，phase9_plan 点名的"下一步头寸"）：录制期在**命令发射点**
 * 累加，帧末 profilerFlushFrameCounters 一次性落成 `draw.count` / `vkcmd.count` 两条
 * CounterRecord（复用上面 CounterRecord 机制，零新存储 / 零新聚合面）。
 *
 * 实现刻意落在**接口单元**：profiler_impl.cppm 已顶到文件代码行红线（500），再加就报。
 * 本模块接口单元有先例 = shader.cppm 的 loadShaderSpv / applyDynamicState。
 *
 * 设计约束（观察者效应）：累加器是**每 pass / 每循环一次**的粗粒度调用（不是每 draw 一次），
 * 单次成本 = 一次整数加，profiler 关时也只是累加到恒被清零的计数器上。
 *
 * 计数口径（vkcmd.count 是**已计数发射点覆盖**的口径，不是驱动侧真值）：
 * - `draw.count` = 本帧 `vkCmdDrawMeshTasksEXT` 发射次数（四路全计：普通段 drawOne /
 *   GPU culling 读回重放 / drawLight 全屏 / IZB mask 重放；picking/highlight 克隆同计）。
 *   DGC execute 路（NVIDIA）draw 数在 CPU 侧不可见 → 不计，本机（RADV）不走该路。
 * - `vkcmd.count` = 上述每次 draw 记 3 条（pushDataEXT + bindShadersEXT + drawMeshTasksEXT）
 *   + barrier / beginRendering / endRendering / blit / applyDynamicState / 固定数据通路段
 *   逐条实计。**未计残项**（量级 O(passes)，非 O(draws)）：cull CB 内命令、IZB/EVSSM 图外
 *   compute dispatch、capturer/picking 回读段、imgui 帧末段。
 */
export void profilerAddDraws(uint32_t n);
/** @see profilerAddDraws 命令口径说明 */
export void profilerAddCommands(uint32_t n);
/**
 * 帧末收口（drawFrame 录制完毕处调用一次）：把本帧累加值推成两条 CounterRecord 并清零。
 * profiler 关 = profilerPushCounter 自身静默丢，累加器也每帧被清零（不泄漏、不增长）。
 */
export void profilerFlushFrameCounters();

/** steady_clock 当前时刻（ns）——外部记录/计数回填时间轴用（批 9.5 帧命令计数）。 */
export uint64_t profilerNowNs();

namespace {
/** 本帧 draw 累加（profilerAddDraws 写、profilerFlushFrameCounters 清零）。 */
uint64_t g_frameDraws = 0;
/** 本帧 vkCmd 累加（口径见 profilerAddDraws 注释）。 */
uint64_t g_frameCommands = 0;
/** 每次 mesh draw 的命令数（pushDataEXT + bindShadersEXT + drawMeshTasksEXT）。 */
constexpr uint64_t kCommandsPerDraw = 3;
/** counter 名（静态字面量 = CounterRecord.name 生命周期要求）。 */
constexpr std::string_view kCounterDraws = "draw.count";
/** @see kCounterDraws */
constexpr std::string_view kCounterCommands = "vkcmd.count";
}  // namespace

/**
 * profilerAddDraws 定义（口径见其声明注释）。三个入口**刻意不加 inline**：定义留在本接口
 * 单元（先例 = shader.cppm 的 loadShaderSpv），importer 只见声明 → 匿名命名空间累加器
 * 不构成 TU-local 暴露（-WTU-local-entity-exposure）。
 * @param n 本次发射的 draw 条数
 */
void profilerAddDraws(uint32_t n) {
	g_frameDraws += n;
	g_frameCommands += kCommandsPerDraw * n;  // 每 draw 的三条命令同处发射
}

/**
 * profilerAddCommands 定义（口径见 profilerAddDraws 声明注释）。
 * @param n 本次发射的 vkCmd 条数
 */
void profilerAddCommands(uint32_t n) { g_frameCommands += n; }

/** profilerFlushFrameCounters 定义（口径见其声明注释）：取数 → 清零 → 推两条计数。 */
void profilerFlushFrameCounters() {
	const uint64_t frame = profilerCurrentFrame();
	const uint64_t ts = profilerNowNs();
	const uint64_t draws = g_frameDraws;
	const uint64_t commands = g_frameCommands;
	g_frameDraws = 0;
	g_frameCommands = 0;
	profilerPushCounter(CounterRecord{.frame = frame, .tsNs = ts, .value = draws,
		.name = kCounterDraws});
	profilerPushCounter(CounterRecord{.frame = frame, .tsNs = ts, .value = commands,
		.name = kCounterCommands});
}

/**
 * 收口 dump（renderSystem 返回后调用一次）：写 profiles/trace_<epoch>.json（Perfetto
 * 主报告）+ profiles/stats_<epoch>.json（每 zone 聚合 + 帧时长分布）+ rllogger 摘要。
 * profiler = 0 时静默 no-op；写盘失败仅 warn——诊断路径不得改变程序结果。
 */
export void profilerDump();

/**
 * 帧窗口活体快照（Phase 10 IMGUI 面板读数用；profilerSnapshotNow 逐帧可读）。
 * profiler = 0 时 enabled = false 其余全 0——面板免判空直接显示。
 * 读环形槽现场值（单线程帧循环假设与本体一致，无锁；O(window)）。
 */
export struct ProfilerSnapshot {
	bool enabled = false;         // profiler 窗口 > 0 且首帧已开
	uint64_t frame = 0;           // 当前逻辑帧号
	double lastFrameMs = 0.0;     // 最近一个完整帧墙钟时长（0 = 无样本）
	double frameAvgMs = 0.0;      // 窗口内有效帧墙钟均值
	double cpuRecordAvgMs = 0.0;  // drawFrame CPU 主段窗口均值（0 = 无记录）
	double gpuFrameAvgMs = 0.0;   // gpu.frame 跨度窗口均值（0 = 9.2 数据缺位）
	uint32_t sampleFrames = 0;    // 窗口内持有墙钟样本的帧数
};

/**
 * 取帧窗口快照（无分配，帧率调用安全）。
 * @return 快照（profiler 关 = enabled false，其余字段 0）
 */
export ProfilerSnapshot profilerSnapshotNow();

}  // namespace RGL
