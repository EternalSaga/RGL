// profiler_impl.cppm — Phase 9 CPU zone profiler 实现（docs/phase9_plan.md 批 9.1）。
// 结构：单例态（环形帧槽，window 定型后不变）→ Zone RAII 读写 → frameBegin 切槽 →
// dump = 聚合 → Perfetto trace + stats JSON + rllogger 摘要三件套（§报告设计）。
// 单线程假设：帧循环全在主线程；9.2 GPU 时间戳 / 9.3 present 记录经 profilerPush 同线程回填。
module;
#include <fmt/format.h>
#include <chrono>            // steady_clock 帧计时 + system_clock dump 命名（GMF 例外清单成员）
#include <filesystem>        // profiles/ 目录创建（GMF 例外清单成员）
module profiler:impl;

import profiler;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import diagnostics;  // diagnostics().profiler（帧窗口配置，main 启动早期 setDiagnostics）

namespace RGL {

namespace {

/** 单帧 zone 容量上限（超出计 drops；正常 ~15 zone/帧，防失控循环刷爆内存）。 */
constexpr uint32_t kMaxZonesPerFrame = 16384;
/** 帧窗口配置钳制上限（防离谱 N 值；最坏 ~4096 帧 × 16384 × 64B ≈ 4GB 仅理论，实际远小）。 */
constexpr uint32_t kMaxWindowFrames = 4096;
/** Perfetto trace pid 常量（process_name 元数据同值）。 */
constexpr int kTracePid = 1;
/** trace 分轨 tid：main（cpu）/ gpu（9.2）/ present（9.3）。 */
constexpr int kTidCpu = 0;
constexpr int kTidGpu = 1;
constexpr int kTidPresent = 2;
/** 摘要日志最多列几个 zone（按 total 降序取前 K）。 */
constexpr size_t kSummaryTopZones = 12;

/** 一个已记录帧的槽（环形窗口按 frame % window 复用；空槽 frame = kEmptyFrame）。 */
struct FrameSlot {
	static constexpr uint64_t kEmptyFrame = ~uint64_t{0};
	uint64_t frame = kEmptyFrame;  // 槽内帧号（kEmpty = 从未占用）
	uint64_t prevFrameDurNs = 0;   // 上一帧墙钟时长（开本帧时算出，归属帧号 = frame-1）
	uint32_t drops = 0;            // 本帧溢出丢弃数
	std::vector<ZoneRecord> zones; // 本帧记录（clear 保留容量，帧内复用）
};

/** profiler 单例态（单线程帧循环；window 定型后不再变化）。 */
struct ProfilerState {
	uint32_t window = 0;           // 帧窗口（0 = 关；首次 frameBegin 按配置定型）
	bool inFrame = false;          // 首帧已开（启动期 zone 不记录）
	uint64_t frame = 0;            // 当前逻辑帧号
	uint32_t depth = 0;            // 嵌套深度游标（帧内 RAII 配对，开帧复位）
	uint64_t prevBeginNs = 0;      // 上次 frameBegin 时刻（帧墙钟差基准）
	bool dumped = false;           // dump 幂等（正常 + 异常路径都可能调）
	uint64_t orphanDrops = 0;      // 回填时目标帧已挤出的丢弃（9.2 GPU 滞后 2 帧）
	std::vector<FrameSlot> slots;  // size = window
};

/** profiler 单例访问器（函数局部静态，避开跨 TU 初始化顺序）。 */
ProfilerState& state() {
	static ProfilerState s;
	return s;
}

/** steady_clock 当前时刻（ns；glibc = CLOCK_MONOTONIC，与 9.2 GPU 校准目标域同源）。 */
uint64_t nowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

/** zone 类别 → trace 分轨 tid（cpu 为默认兜底）。 */
int tidFor(ZoneKind kind) {
	if (kind == ZoneKind::gpu)
		return kTidGpu;
	if (kind == ZoneKind::present)
		return kTidPresent;
	return kTidCpu;
}

/** zone 类别 → trace cat / stats kind 字符串。 */
const char* kindName(ZoneKind kind) {
	if (kind == ZoneKind::gpu)
		return "gpu";
	if (kind == ZoneKind::present)
		return "present";
	return "cpu";
}

/** JSON 字符串转义（label 可能含引号/反斜杠：模板实参、匿名 lambda 名）。 */
std::string jsonEscape(std::string_view in) {
	std::string out;
	out.reserve(in.size() + 8);
	for (char c : in) {
		if (c == '"' || c == '\\')
			out.push_back('\\');
		out.push_back(c);
	}
	return out;
}

/** 时长 ns → ms（double；日志与 JSON 共用）。 */
double toMs(uint64_t ns) { return static_cast<double>(ns) / 1e6; }

/** 百分位（最近秩；durs 须已升序，空集返回 0）。 */
uint64_t percentile(const std::vector<uint64_t>& sorted, double p) {
	if (sorted.empty())
		return 0;
	size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size()));
	if (idx >= sorted.size())
		idx = sorted.size() - 1;
	return sorted[idx];
}

/** 向量求和 + 均值（ms）。rlstd 未导出 std::accumulate，局部实现避免动 rlstd。 */
double meanMs(const std::vector<uint64_t>& ns) {
	if (ns.empty())
		return 0.0;
	uint64_t sum = 0;
	for (uint64_t x : ns)
		sum += x;
	return toMs(sum) / static_cast<double>(ns.size());
}

/** 单 zone 聚合条目（按 name+file:line 聚合；durs 留存算分位数）。 */
struct ZoneAgg {
	std::string name;
	std::string file;
	uint32_t line = 0;
	ZoneKind kind = ZoneKind::cpu;
	uint64_t totalNs = 0;
	std::vector<uint64_t> durs;  // 升序（聚合末尾排序）
};

/** 聚合结果（zones 按 total 降序 + 帧时长升序分布 + 丢弃/帧数）。 */
struct ReportData {
	std::vector<ZoneAgg> zones;
	std::vector<uint64_t> frameDursNs;
	uint64_t drops = 0;
	uint64_t frames = 0;
};

/** 遍历帧槽聚合（聚合与帧序无关；帧时长同时排序供分位数用）。 */
ReportData aggregate(const std::vector<FrameSlot*>& ordered) {
	ReportData out;
	std::map<std::string, size_t> index;  // key → out.zones 下标
	for (const FrameSlot* slot : ordered) {
		++out.frames;
		out.drops += slot->drops;
		if (slot->prevFrameDurNs > 0)
			out.frameDursNs.push_back(slot->prevFrameDurNs);
		for (const ZoneRecord& r : slot->zones) {
			const std::string key = fmt::format("{}@{}:{}", r.name, r.file, r.line);
			auto [it, inserted] = index.try_emplace(key, out.zones.size());
			if (inserted)
				out.zones.push_back(ZoneAgg{std::string(r.name), std::string(r.file),
					r.line, r.kind, 0, {}});
			ZoneAgg& agg = out.zones[it->second];
			agg.totalNs += r.durNs;
			agg.durs.push_back(r.durNs);
		}
	}
	for (ZoneAgg& a : out.zones)
		std::ranges::sort(a.durs);
	std::ranges::sort(out.frameDursNs);
	std::ranges::sort(out.zones,
		[](const ZoneAgg& a, const ZoneAgg& b) { return a.totalNs > b.totalNs; });
	return out;
}

/** 写文本文件（项目写盘先例 = std::fopen/fwrite，rlstd 导出面、main_impl:61 同款；
 *  无流操作符——格式化归 fmt、日志归 rllogger，栈内不该出现 <<）。 @return 是否成功。 */
bool writeTextFile(const std::filesystem::path& path, const std::string& content) {
	std::FILE* f = std::fopen(path.string().c_str(), "wb");
	if (!f)
		return false;
	const auto written = std::fwrite(content.data(), 1, content.size(), f);
	std::fclose(f);
	return written == content.size();
}

/** 写 Perfetto（Chrome Trace Event JSON）主报告。 @return 是否成功打开并写完。 */
bool writeTrace(const std::vector<FrameSlot*>& ordered, const std::filesystem::path& path) {
	std::string out;
	out.reserve(ordered.size() * 768);
	// 进程/线程元数据（tid 0/1/2 = main/gpu/present；9.2/9.3 数据落位前 gpu/present 为空轨）
	out += R"({"displayTimeUnit":"ms","traceEvents":[)";
	out += fmt::format(R"({{"name":"process_name","ph":"M","pid":{},"args":{{"name":"RGL"}}}},)", kTracePid);
	out += fmt::format(R"({{"name":"thread_name","ph":"M","pid":{},"tid":{},"args":{{"name":"main"}}}},)", kTracePid, kTidCpu);
	out += fmt::format(R"({{"name":"thread_name","ph":"M","pid":{},"tid":{},"args":{{"name":"gpu"}}}},)", kTracePid, kTidGpu);
	out += fmt::format(R"({{"name":"thread_name","ph":"M","pid":{},"tid":{},"args":{{"name":"present"}}}})", kTracePid, kTidPresent);
	for (const FrameSlot* slot : ordered) {
		for (const ZoneRecord& r : slot->zones) {
			// X = Complete 事件（ts/dur，µs，嵌套经包含关系天然成火焰视图）
			fmt::format_to(std::back_inserter(out),
				R"(,{{"name":"{}","cat":"{}","ph":"X","ts":{:.3f},"dur":{:.3f},"pid":{},"tid":{},"args":{{"file":"{}","line":{},"frame":{}}}}})",
				jsonEscape(r.name), kindName(r.kind),
				static_cast<double>(r.startNs) / 1000.0,
				static_cast<double>(r.durNs) / 1000.0,
				kTracePid, tidFor(r.kind), jsonEscape(r.file), r.line, r.frame);
		}
	}
	out += "]}";
	return writeTextFile(path, out);
}

/** 写聚合 stats JSON（每 zone count/total/mean/p50/p95/max + 帧时长分布）。 @return 是否成功。 */
bool writeStats(const ReportData& data, const std::filesystem::path& path) {
	std::string out;
	out.reserve(data.zones.size() * 256 + 256);
	out += fmt::format(R"({{"frames":{},"drops":{},"zones":[)", data.frames, data.drops);
	bool first = true;
	for (const ZoneAgg& a : data.zones) {
		const uint64_t count = a.durs.size();
		if (!first)
			out += ',';
		first = false;
		out += fmt::format(
			R"({{"name":"{}","file":"{}","line":{},"kind":"{}","count":{},"total_ms":{:.3f},"mean_ms":{:.3f},"p50_ms":{:.3f},"p95_ms":{:.3f},"max_ms":{:.3f}}})",
			jsonEscape(a.name), jsonEscape(a.file), a.line, kindName(a.kind), count,
			toMs(a.totalNs),
			count ? toMs(a.totalNs) / static_cast<double>(count) : 0.0,
			toMs(percentile(a.durs, 0.50)), toMs(percentile(a.durs, 0.95)),
			toMs(count ? a.durs.back() : 0));
	}
	out += "],";
	// 帧墙钟分布（frameBegin 到 frameBegin，含 fence 等待 = 真实帧节奏）
	out += fmt::format(R"("frame_ms":{{"count":{},"avg_ms":{:.3f},"p50_ms":{:.3f},"p95_ms":{:.3f},"max_ms":{:.3f}}})",
		data.frameDursNs.size(), meanMs(data.frameDursNs),
		toMs(percentile(data.frameDursNs, 0.50)), toMs(percentile(data.frameDursNs, 0.95)),
		toMs(data.frameDursNs.empty() ? 0 : data.frameDursNs.back()));
	out += "}";
	return writeTextFile(path, out);
}

/** rllogger 摘要（帧节奏 + top zones 按 total 降序；不开 UI 也能读）。 */
void logSummary(const ReportData& data, uint32_t window) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	if (!data.frameDursNs.empty()) {
		logger->info("profile: {} frames (window {}), frame avg={:.2f}ms p50={:.2f} p95={:.2f} max={:.2f} drops={}",
			data.frames, window, meanMs(data.frameDursNs),
			toMs(percentile(data.frameDursNs, 0.50)),
			toMs(percentile(data.frameDursNs, 0.95)),
			toMs(data.frameDursNs.back()), data.drops);
	}
	const size_t shown = std::min(kSummaryTopZones, data.zones.size());
	for (size_t i = 0; i < shown; ++i) {
		const ZoneAgg& a = data.zones[i];
		const uint64_t count = a.durs.size();
		logger->info("  {:<26} n={:<6} total={:>8.2f}ms mean={:.3f} p95={:.3f} max={:.3f}",
			a.name, count, toMs(a.totalNs),
			count ? toMs(a.totalNs) / static_cast<double>(count) : 0.0,
			toMs(percentile(a.durs, 0.95)), toMs(count ? a.durs.back() : 0));
	}
}

/** 收集有效帧槽并按帧号升序（环形窗口物理无序，dump 侧排序一次）。 */
std::vector<FrameSlot*> collectOrdered(ProfilerState& st) {
	std::vector<FrameSlot*> ordered;
	ordered.reserve(st.slots.size());
	for (FrameSlot& s : st.slots) {
		if (s.frame != FrameSlot::kEmptyFrame)
			ordered.push_back(&s);
	}
	std::ranges::sort(ordered,
		[](const FrameSlot* a, const FrameSlot* b) { return a->frame < b->frame; });
	return ordered;
}

/** 写两件产物（profiles/ 目录创建 + trace/stats）并打落盘结果行。 */
void writeAndLog(const std::vector<FrameSlot*>& ordered, const ReportData& data) {
	const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	std::error_code ec;
	std::filesystem::create_directories("profiles", ec);
	const std::filesystem::path dir = "profiles";
	const std::filesystem::path tracePath = dir / ("trace_" + std::to_string(epoch) + ".json");
	const std::filesystem::path statsPath = dir / ("stats_" + std::to_string(epoch) + ".json");
	const bool traceOk = !ec && writeTrace(ordered, tracePath);
	const bool statsOk = !ec && writeStats(data, statsPath);
	if (auto* lg = RLLogger::getInstance()) {
		if (ec)
			lg->warn("profile: create profiles/ failed: {}", ec.message());
		else
			lg->info("profile: wrote {} ({}), stats {} ({})", tracePath.string(),
				traceOk ? "ok" : "FAILED", statsPath.string(), statsOk ? "ok" : "FAILED");
	}
}

}  // namespace

Zone::Zone(std::string_view label_, std::source_location loc_)
	: label(label_), loc(loc_) {
	auto& st = state();
	if (st.window == 0 || !st.inFrame)
		return;  // 关（或首帧未开）：仅两次可预测分支，近零开销
	active = true;
	startNs = nowNs();
	depth = st.depth++;
}

Zone::~Zone() {
	if (!active)
		return;
	auto& st = state();
	if (st.depth > 0)
		--st.depth;
	FrameSlot& slot = st.slots[st.frame % st.window];
	if (slot.frame != st.frame)
		return;  // 帧已被挤出（单线程帧内配对下理论不可达，防御）
	if (slot.zones.size() >= kMaxZonesPerFrame) {
		++slot.drops;
		return;
	}
	ZoneRecord r;
	r.startNs = startNs;
	r.durNs = nowNs() - startNs;
	r.frame = st.frame;
	r.depth = depth;
	r.line = static_cast<uint32_t>(loc.line());
	r.kind = ZoneKind::cpu;
	const char* fn = loc.function_name();
	const char* file = loc.file_name();
	r.name = label.empty() && fn ? std::string_view{fn} : label;
	r.file = file ? std::string_view{file} : std::string_view{};
	slot.zones.push_back(r);
}

Zone zone(std::string_view label, std::source_location loc) { return Zone{label, loc}; }

void profilerFrameBegin() {
	const uint32_t window = diagnostics().profiler;
	if (window == 0)
		return;
	auto& st = state();
	if (st.slots.empty()) {
		st.window = std::clamp(window, 1u, kMaxWindowFrames);
		st.slots.resize(st.window);
		if (auto* lg = RLLogger::getInstance())
			lg->info("profile: on (window {} frames, dump on exit)", st.window);
	}
	const uint64_t t = nowNs();
	const uint64_t k = st.inFrame ? st.frame + 1 : 0;
	FrameSlot& slot = st.slots[k % st.window];
	// 上一帧墙钟 = 两次 frameBegin 间隔（真实帧节奏，含等 GPU）；归属帧 k-1，
	// 经本槽 prevFrameDurNs 携带（k==0 / 窗口边界语义见 phase9_plan §6）
	slot.prevFrameDurNs = (st.prevBeginNs != 0 && k > 0) ? t - st.prevBeginNs : 0;
	slot.frame = k;
	slot.zones.clear();
	slot.drops = 0;
	st.frame = k;
	st.prevBeginNs = t;
	st.inFrame = true;
	st.depth = 0;
}

uint64_t profilerCurrentFrame() { return state().frame; }

void profilerPushRecord(const ZoneRecord& rec) {
	auto& st = state();
	if (st.window == 0 || !st.inFrame)
		return;  // 关/未开帧（GPU 轨同配置门控，正常不可达）：静默丢
	FrameSlot& slot = st.slots[rec.frame % st.window];
	if (slot.frame != rec.frame) {
		++st.orphanDrops;  // 目标帧已挤出（窗口 < 归并滞后 +1）
		return;
	}
	if (slot.zones.size() >= kMaxZonesPerFrame) {
		++slot.drops;
		return;
	}
	slot.zones.push_back(rec);
}

void profilerDump() {
	auto& st = state();
	if (st.window == 0 || st.slots.empty() || st.dumped)
		return;
	st.dumped = true;  // 幂等：renderSystem 正常路径 + catch 异常路径都可能调
	const std::vector<FrameSlot*> ordered = collectOrdered(st);
	ReportData data = aggregate(ordered);
	data.drops += st.orphanDrops;
	logSummary(data, st.window);
	writeAndLog(ordered, data);
}

}  // namespace RGL
