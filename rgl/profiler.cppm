// profiler.cppm — Phase 9 CPU zone profiler 核心接口（docs/archive/phase9_plan.md 批 9.1）。
// 零宏设计（D4 翻案 2026-09-27）：调用形态 auto z = RGL::zone("label");——
// source_location 默认实参在调用点求值，捕获 file:line/function（本项目编译规则传
// 绝对源路径，build/build.ninja CXX_COMPILER 行 → file 免费为绝对路径）。
// 开关 = diagnostics.profiler（帧窗口 N，0=关）；关闭时 ctor 首行早退近零开销。
// 存储 = 单进程单线程环形帧槽（录制期全主线程；9.2 GPU/9.3 present 记录同线程回填）。
export module profiler;

import rlstd;
import timing_util;  // steadyNowNs（计数记录回填时间轴；本模块曾另出一份同名实现）

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
 * - `draw.count` = 本帧**逻辑 draw 条数**（★M-1b / §19.2 D5 起含 `gcX=0` 的空行：一次 MDI
 *   调用展开 N 行就计 N，四路全计：普通段 drawOne / GPU culling 读回重放 / drawLight 全屏 /
 *   IZB mask 重放；picking/highlight 克隆同计）。DGC execute 路（NVIDIA）draw 数在 CPU 侧
 *   不可见 → 不计，本机（RADV）不走该路。
 * - `vkcmd.count` = 上述每次 draw 记 3 条（pushDataEXT + bindShadersEXT + drawMeshTasksEXT；
 *   ★M-1b：一次 MDI 调用同样记 **3** 条——drawMeshTasksIndirectEXT 只替换那条 draw，
 *   **不是 3N**，MDI 的收益正是在这里现形）
 *   + barrier / beginRendering / endRendering / blit / applyDynamicState / 固定数据通路段
 *   逐条实计。**未计残项**（量级 O(passes)，非 O(draws)）：cull CB 内命令、IZB/EVSSM 图外
 *   compute dispatch、capturer/picking 回读段、imgui 帧末段。
 * - `mdi.count` = 本帧 `vkCmdDrawMeshTasksIndirectEXT` 的**调用次数**（★M-1b：生效档阴影段
 *   = 生效级联数，关 = 0）。
 * - `mdi.rows` = 经 MDI 提交的**行数**（含 `gcX=0` 空行；与 `mdi.count` 一起给 A5 收益判据
 *   的闭式锚对账口径，容量读上界、实际读可切数，两口径别混 → §19.5）。
 * - `mdi.tasks` = 经 MDI 提交的 **task 数** = Σ 各行 `groupCountX`（★M-3T-c 起 gcX 的单位就是
 *   task）。债-113 立项理由：★M-3T-c 的头条收益（每级联 4724 → 1530）当时**没有计数器**，只能
 *   靠一次性探针取数 ⟹ 收益漂了没人发现。`mdi.rows / mdi.tasks` 之比 = 空行占比。
 */
export void profilerAddDraws(uint32_t n);
/** @see profilerAddDraws 命令口径说明 */
export void profilerAddCommands(uint32_t n);
/**
 * ★M-1b：一次真 MDI 调用的入账（drawOne 的 `drawCount > 1` 分支调用）。
 * 一次调用 = 1 条 mdi.count + `rows` 条 mdi.rows；`draw.count` 的**逻辑**口径修正在这里补
 * `rows − 1`（段级入账 `profilerAddDraws(seg.size())` 已把这条 DrawCall 记作 1 条；
 * 差值补加 = 不改动既有四路的任何数字）；`vkcmd.count` **不补**——MDI 与逐条同为 3 条命令。
 * @param rows 本次调用展开的行数（> 1）
 */
export void profilerAddMdi(uint32_t rows);
/**
 * 债-113：本帧 MDI **task 数**入账（= Σ `groupCountX`，构建侧的聚合锚 `sumGroupCountX`）。
 *
 * 🔴 **入账点 = 录制期的行表落地点（`Renderer::prepareMdiShadowRows`），两处都不是**：
 *  · 不在 `drawOne`：那是**盲执行器**，只认 `drawCount`，让它算 task 等于把行表语义漏进
 *    盲执行器（ECS 规范 4，债-113 原文明令禁止的那条）。
 *  · 也不在采集侧（`render_list:mdi`）：那里算得出同一个锚，但 `mdi.count` / `mdi.rows` 是
 *    **录制期**入账的，把 tasks 放采集期会让四条同源计数跨两个阶段 —— 将来录制期一旦新增
 *    回落分支（今天的容量/轮转预检都在采集期），tasks 就会在"没提交"的帧上报出非零值，
 *    而这条计数器存在的意义正是"收益会不会静默回退"。落地点 = 行表真的写进 ring 之后。
 * @param tasks 本帧全部 MDI 段的 task 数之和（门关 = 不调用 ⟹ 恒 0）
 */
export void profilerAddMdiTasks(uint32_t tasks);
/**
 * 帧末收口（drawFrame 录制完毕处调用一次）：把本帧累加值推成**五**条 CounterRecord
 * （draw/vkcmd/mdi.count/mdi.rows/mdi.tasks）并清零。profiler 关 = profilerPushCounter 自身静默丢，
 * 累加器也每帧被清零（不泄漏、不增长）。
 */
export void profilerFlushFrameCounters();

namespace {
/**
 * 本帧计数器（债-113 起五条）。🔴 **刻意收成一个结构体**，而不是五个 namespace 作用域全局：
 *  · R-EC4（ECS 规范 3 的静态代理）数的是「列 0 可变全局条数」，五个字段合成**一条**命中
 *    ⟹ 本文件基线 4 → 1（棘轮只准降；加一条计数器不该来基线要额度）。
 *  · 真正的理由在 flush：旧写法要**逐字段手写清零**，而"新增一条计数器"得同时改
 *    取数 / 清零 / 推记录三处 —— 单漏「清零」那处 = 该计数器跨帧累加、读数一路涨且零信号
 *    （观察者自己造出静默漂移，正是债-113 要堵的那类形态）。整体赋值让它在结构上不可能漏。
 */
struct FrameCounters {
	uint64_t draws = 0;     ///< 本帧逻辑 draw 条数（口径见 profilerAddDraws）
	uint64_t commands = 0;  ///< 本帧 vkCmd 条数（同上）
	uint64_t mdiCalls = 0;  ///< ★M-1b：MDI 调用次数（profilerAddMdi 写）
	uint64_t mdiRows = 0;   ///< ★M-1b：经 MDI 提交的行数（含 gcX=0 空行）
	uint64_t mdiTasks = 0;  ///< 债-113：经 MDI 提交的 task 数（= Σ gcX）
};
/** 本帧累加器（写侧 = 采集/录制路径，读侧 = profilerFlushFrameCounters 取数后整体清零）。 */
FrameCounters g_frame;
/** 每次 mesh draw 的命令数（pushDataEXT + bindShadersEXT + drawMeshTasksEXT；
 *  MDI 调用同值 = drawMeshTasksIndirectEXT 只替换那条 draw，见 profilerAddMdi）。 */
constexpr uint64_t kCommandsPerDraw = 3;
/** counter 名（静态字面量 = CounterRecord.name 生命周期要求）。 */
constexpr std::string_view kCounterDraws = "draw.count";
/** @see kCounterDraws */
constexpr std::string_view kCounterCommands = "vkcmd.count";
/** @see kCounterDraws（★M-1b §19.2 D5 新口径） */
constexpr std::string_view kCounterMdiCalls = "mdi.count";
/** @see kCounterMdiCalls */
constexpr std::string_view kCounterMdiRows = "mdi.rows";
/** @see profilerAddMdiTasks 口径说明 */
constexpr std::string_view kCounterMdiTasks = "mdi.tasks";
}  // namespace

/**
 * profilerAddDraws 定义（口径见其声明注释）。三个入口**刻意不加 inline**：定义留在本接口
 * 单元（先例 = shader.cppm 的 loadShaderSpv），importer 只见声明 → 匿名命名空间累加器
 * 不构成 TU-local 暴露（-WTU-local-entity-exposure）。
 * @param n 本次发射的 draw 条数
 */
void profilerAddDraws(uint32_t n) {
	g_frame.draws += n;
	g_frame.commands += kCommandsPerDraw * n;  // 每 draw 的三条命令同处发射
}

/**
 * profilerAddCommands 定义（口径见 profilerAddDraws 声明注释）。
 * @param n 本次发射的 vkCmd 条数
 */
void profilerAddCommands(uint32_t n) { g_frame.commands += n; }

/**
 * profilerAddMdi 定义（口径见其声明注释）：调用数/行数入账 + draw 口径的差值修正。
 * 🔴 `rows − 1` 而非 `rows`：段级 `profilerAddDraws(seg.size())` 已把这条 DrawCall 记作 1
 * （门关形态它就是一条 = 正确数字），MDI 分支只补「一次调用展开多行」多出来的部分；
 * vkcmd 不补（MDI 与逐条同为 kCommandsPerDraw=3 条命令，收益 = 条目数从 N 降到 1 时
 * 段级入账自动少记 3×(N−1)）。
 * @param rows 本次 MDI 调用展开的行数（> 1，调用方守）
 */
void profilerAddMdi(uint32_t rows) {
	g_frame.mdiCalls += 1;
	g_frame.mdiRows += rows;
	g_frame.draws += rows - 1u;
}

/**
 * profilerAddMdiTasks 定义（口径与"为什么不在执行器/采集侧入账"见其声明注释）：纯累加，
 * 与 `mdi.count` / `mdi.rows` 同生命周期（flush 时一并清零）。
 * @param tasks 本帧 MDI task 数（= Σ gcX）
 */
void profilerAddMdiTasks(uint32_t tasks) { g_frame.mdiTasks += tasks; }

/** profilerFlushFrameCounters 定义（口径见其声明注释）：取数 → 整体清零 → 推五条计数。 */
void profilerFlushFrameCounters() {
	const uint64_t frame = profilerCurrentFrame();
	const uint64_t ts = steadyNowNs();
	const FrameCounters c = g_frame;
	g_frame = {};  // 🔴 整体清零 = 新增计数器不需要在这里补行（漏清零的形态结构上不存在）
	profilerPushCounter(
		CounterRecord{.frame = frame, .tsNs = ts, .value = c.draws, .name = kCounterDraws});
	profilerPushCounter(
		CounterRecord{.frame = frame, .tsNs = ts, .value = c.commands, .name = kCounterCommands});
	profilerPushCounter(CounterRecord{
		.frame = frame, .tsNs = ts, .value = c.mdiCalls, .name = kCounterMdiCalls});
	profilerPushCounter(
		CounterRecord{.frame = frame, .tsNs = ts, .value = c.mdiRows, .name = kCounterMdiRows});
	profilerPushCounter(
		CounterRecord{.frame = frame, .tsNs = ts, .value = c.mdiTasks, .name = kCounterMdiTasks});
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
