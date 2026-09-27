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
 * 收口 dump（renderSystem 返回后调用一次）：写 profiles/trace_<epoch>.json（Perfetto
 * 主报告）+ profiles/stats_<epoch>.json（每 zone 聚合 + 帧时长分布）+ rllogger 摘要。
 * profiler = 0 时静默 no-op；写盘失败仅 warn——诊断路径不得改变程序结果。
 */
export void profilerDump();

}  // namespace RGL
