module;
#include <fmt/format.h>
#include <chrono>  // 写盘线程的等待节拍（GMF 例外清单成员）
module blackbox:impl;

import blackbox;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;    // 开录/失败/溢出三处出声（"没录上"必须有人红）
import diagnostics; // 按门惰性开录（门与本模块同为 rlbase，方向合规）

namespace RGL {

uint64_t blackboxDigest(std::span<const std::byte> bytes) {
	// FNV-1a 64：偏移基准与素数是公开常量（期望值出处 = FNV 规范，不抄被测码）。
	// 🔴 空 span 返回**偏移基准本身**而不是 0：0 与"一段全零的行表"的摘要必须可区分，
	//    否则"这段没写"与"这段写的全是零"在日志里同形。
	constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
	constexpr uint64_t kFnvPrime = 0x100000001b3ull;
	uint64_t h = kFnvOffset;
	for (const auto b : bytes) {
		h ^= static_cast<uint64_t>(static_cast<uint8_t>(b));
		h *= kFnvPrime;
	}
	return h;
}

uint64_t blackboxNowNs() {
	return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

uint32_t blackboxRowWord(std::span<const std::byte> row, uint32_t i) {
	// 逐字节组小端（与 shader 的 `ByteAddressBuffer.Load` 语义逐位同；未对齐
	// reinterpret_cast 是 UB，这里也不越界读）。
	if (row.size() < static_cast<size_t>(4u) * (i + 1u))
		return 0u;
	const auto* p = reinterpret_cast<const uint8_t*>(row.data()) + 4u * i;
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u)
	     | (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
}

BlackboxSegmentDigest blackboxDigestSegment(std::span<const std::byte> bytes, uint32_t stride) {
	BlackboxSegmentDigest out;
	out.digest = blackboxDigest(bytes);
	if (stride == 0u || bytes.size() < stride)
		return out;  // hasRows 保持 false：调用方不许把两个全零摘要读成"首末行都是零"
	const auto take = [&](size_t rowIdx) {
		BlackboxRowSummary s;
		const size_t begin = rowIdx * stride;
		const size_t end = std::min(bytes.size(), begin + stride);
		std::span<const std::byte> row{bytes.data() + begin, end - begin};
		for (uint32_t w = 0u; w < 6u; ++w)
			s.words[w] = blackboxRowWord(row, w);
		return s;
	};
	out.head = take(0u);
	out.tail = take((bytes.size() - 1u) / stride);  // 末行可能不足一整行：按行起点对齐
	out.hasRows = true;
	return out;
}

std::string encodeBlackboxRowSummary(const BlackboxRowSummary& s) {
	return fmt::format("[{},{},{},{},{},{}]", s.words[0], s.words[1], s.words[2], s.words[3],
		s.words[4], s.words[5]);
}

std::string encodeBlackboxFrameNdjson(const BlackboxFrameRecord& rec) {
	// 键序固定 = 便于 grep / 逐轮 diff；一行一对象 = 引擎半路死掉也只丢最后半行。
	std::string out;
	out.reserve(256u + 160u * rec.segmentCount);
	out += fmt::format("{{\"frame\":{},\"t_ns\":{},\"segs\":{},\"dgc\":{},\"selfcheck\":{}",
		rec.frame, rec.timestampNs, rec.segmentCount, rec.dgcActive, rec.selfCheckOn);
	for (uint32_t i = 0u; i < rec.segmentCount && i < kBlackboxMaxSegments; ++i) {
		const auto& s = rec.segments[i];
		out += fmt::format(
			",\"s{}\":{{\"casc\":{},\"slot\":{},\"first\":{},\"rows\":{},\"tasks\":{},\"pipe\":"
			"{},\"args_off\":{},\"row_base\":{},\"count\":{},\"count_src\":{},\"digest\":\"0x{:016x}\","
			"\"head_row\":{},\"tail_row\":{},\"head\":{},\"tail\":{}}}",
			i, s.cascadeIndex, s.frameSlot, s.firstRow, s.rowCount, s.taskCount, s.pipeline,
			s.argsOffset, s.rowBase, s.drawCount, s.drawCountSource, s.sliceDigest, s.headRowIndex,
			s.tailRowIndex, encodeBlackboxRowSummary(s.head), encodeBlackboxRowSummary(s.tail));
	}
	if (rec.heapSlotCount > 0u) {
		std::string slots;
		for (uint32_t i = 0u; i < rec.heapSlotCount && i < kBlackboxMaxHeapSlots; ++i) {
			if (i)
				slots += ',';
			slots += std::to_string(rec.heapSlots[i]);
		}
		out += fmt::format(",\"heap_slots\":[{}]", slots);
	}
	out += "}\n";
	return out;
}

uint32_t encodeSelfCheckPush(uint32_t rowsWritten, uint32_t objectCapacity) {
	// 🔴 超限**不截断**：截断 = 界变小 = 该报的没报（假阴），而假阴正是本案最怕的失效形态。
	//    返回 0 = 两个界都是"永不违例"，调用方必须出声（见 render_list_mdi 的调用点）。
	constexpr uint32_t kMax16 = 0xFFFFu;
	if (rowsWritten > kMax16 || objectCapacity > kMax16)
		return 0u;
	return (rowsWritten & kMax16) | ((objectCapacity & kMax16) << 16u);
}

SelfCheckPush decodeSelfCheckPush(uint32_t word) {
	return SelfCheckPush {
		.rowsWritten = word & 0xFFFFu,
		.objectCapacity = (word >> 16u) & 0xFFFFu,
	};
}

uint32_t shadowRowViolations(const ShadowRowCheckInput& in) {
	// 🔴 这五条与 shadow_meshlet.task.slang 的 SHADOW_SELFCHECK 分支**逐条同拼写**。
	//    两边各写一遍是没办法的事（shader 与 C++ 无共享头），所以本函数就是那条判据的
	//    **可单测那一份**：shader 侧写错边界时，这里对了、实机读数会与之矛盾而暴露；
	//    反过来若两边一起错，至少 A1 的 COUNT/rows 复盘还能独立看出问题。
	uint32_t bits = kShadowViolNone;
	// 🔴 谓词 1 两边都是**段内相对**口径：drawId 是 SV_DrawID，rowsWritten 是本段写入行数。
	//    拿绝对行号（rowBase + drawID）去比相对计数会几乎恒真 —— 本函数第一版就犯了这个
	//    量纲错，是"判据写出来比不写更糟"的教科书样本。
	if (in.drawId >= in.rowsWritten)
		bits |= kShadowViolRowBeyondWritten;  // 谓词 1：间接计数与已上传行数分家
	if (in.groupCountX != 1u)
		bits |= kShadowViolGcxNotOne;  // 谓词 2：不变量 3b
	if (in.meshGroupCount == 0u)
		bits |= kShadowViolMeshGroupCountZero;  // 谓词 3：不变量 2
	if (in.objectIndex >= in.objectCapacity)
		bits |= kShadowViolObjectIndexOob;  // 谓词 4：mesh 侧越界取址
	if (in.groupCountY != 1u || in.groupCountZ != 1u)
		bits |= kShadowViolRowMisaligned;  // 谓词 5：行读歪
	return bits;
}

std::string describeShadowRowViolations(uint32_t bits) {
	if (bits == kShadowViolNone)
		return "none";
	std::string out;
	const auto add = [&](const char* name, uint32_t bit) {
		if (!(bits & bit))
			return;
		if (!out.empty())
			out += '|';
		out += name;
	};
	add("row_beyond_written", kShadowViolRowBeyondWritten);
	add("gcx_not_one", kShadowViolGcxNotOne);
	add("mesh_group_count_zero", kShadowViolMeshGroupCountZero);
	add("object_index_oob", kShadowViolObjectIndexOob);
	add("row_misaligned", kShadowViolRowMisaligned);
	return out.empty() ? "unknown_bits" : out;  // 有位但没名字 = 位段与描述分家，出声
}

namespace {

/**
 * 环形缓冲槽数（2 的幂，取模变掩码）。
 *
 * 为什么 4096 够用：生产端每帧一条，写盘线程 4ms 醒一次；要溢出得让写盘线程卡住
 * ~20 秒（4096 帧 @200fps）。真溢出也**不静默**：消费端检出并 WARN（只报首次）。
 */
constexpr size_t kRingSlots = 4096u;
constexpr size_t kRingMask = kRingSlots - 1u;
static_assert((kRingSlots & kRingMask) == 0u && kRingSlots > 0u,
	"kRingSlots must be a power of two (the mask trick depends on it)");

/**
 * 黑匣子的全部进程态，收在**函数局部静态**里（`state()`）。
 *
 * 🔴 为什么不是 namespace 作用域全局：style-check 的 ECS 规范 3 护栏正是这么要求的，而这里
 *    还有第二条更硬的理由——`std::thread` 作为 namespace 作用域对象，若进程没走到 shutdown
 *    ledger 就退出（启动中途抛异常），它的析构会直接 `std::terminate`。函数局部静态的析构
 *    在 `state()` 首次被调用**之后**才注册，且 `~State` 自己负责停线程 ⟹ 不存在"线程对象
 *    已析构但还在跑"的窗口。
 */
struct State {
	/** 生产端与写盘线程之间的唯一通道（SPSC：一个渲染线程写、一个写盘线程读）。 */
	std::array<BlackboxFrameRecord, kRingSlots> slots;
	std::atomic<uint64_t> produced {0u};  ///< 已提交条数（写槽之后才发布）
	std::atomic<uint64_t> consumed {0u};  ///< 🔴 只由消费端推进（SPSC 单写者纪律）
	std::thread writer;
	std::atomic<bool> running {false};
	std::mutex ioMu;                      ///< 保护 file / stats / 排空等待
	std::condition_variable cv;           ///< 有新记录或要退出时唤醒写盘线程
	std::ofstream file;
	BlackboxStats stats;
	bool openAttempted = false;           ///< 只按门试开一次（失败不每帧重试刷日志）
	bool overflowWarned = false;          ///< 溢出只 WARN 首次

	/** 停线程 + 排空 + 关文件（幂等；析构与 blackboxClose 共用）。 */
	void shutdown();
	/** 按 `diagnostics().blackbox` 试开一次（含开录/失败两处出声）。 */
	void openFromConfig();
	/** 写盘线程主体。 */
	void writerLoop();
	/** 排空一批（最多 512 条）；返回是否写了东西。 */
	bool drainBatch();

	/** 静态析构：停线程 + 排空 + 关文件（进程退出或异常展开都走这里，幂等）。 */
	~State() { shutdown(); }
};

/** 进程态的唯一产地（函数局部静态 = 懒初始化 + 确定性析构）。 */
State& state() {
	static State s;
	return s;
}

bool State::drainBatch() {
	const uint64_t p = produced.load();
	uint64_t c = consumed.load();
	if (c == p)
		return false;
	// 🔴 溢出检测放在**读之前**：消费端是唯一能改 consumed 的人（SPSC 纪律），所以
	//    "落后超过一圈"必须由它自己判定并丢弃最旧，而不是让生产端跨线程改 consumed。
	if (p - c > kRingSlots) {
		const uint64_t dropped = p - c - kRingSlots;
		c += dropped;
		consumed.store(c);
		{
			std::lock_guard lk(ioMu);
			stats.overwritten += dropped;
			if (!overflowWarned) {
				overflowWarned = true;
				if (auto* logger = RLLogger::getInstance())
					logger->warn("blackbox: 🔴 环形缓冲溢出，已**覆盖最旧 {} 帧** ⟹ 黑匣子里的"
					             "『最后若干帧』并不完整，复盘必须连本行一起读",
						dropped);
			}
		}
	}
	std::string batch;
	batch.reserve(1u << 16);
	uint64_t n = 0u;
	for (; c != p && n < 512u; ++c, ++n)
		batch += encodeBlackboxFrameNdjson(slots[c & kRingMask]);
	consumed.store(c);
	std::lock_guard lk(ioMu);
	if (batch.empty() || !file.good()) {
		cv.notify_all();  // 即便没写成也要放行 blackboxFlush 的等待
		return false;
	}
	file.write(batch.data(), static_cast<std::streamsize>(batch.size()));
	// 🔴 每批必 flush：黑匣子的判据是"最后一条记录 = 案发前一帧"，不落地就等于没记。
	file.flush();
	stats.written += n;
	cv.notify_all();  // 唤醒 blackboxFlush
	return true;
}

void State::writerLoop() {
	while (true) {
		if (drainBatch())
			continue;
		// 🔴 先判停再等：若把 `!running` 只写进 wait 谓词，线程会在"已停录"后每次 wait
		//    立刻返回、无限空转，shutdown() 的 join 永远不回来。
		if (!running.load())
			return;  // 已停录且已排空
		std::unique_lock lk(ioMu);
		cv.wait_for(lk, std::chrono::milliseconds(4), [this] {
			return !running.load() || produced.load() != consumed.load();
		});
	}
}

void State::openFromConfig() {
	if (openAttempted)
		return;
	openAttempted = true;  // 只试一次：开不成就是开不成，每帧重试只会刷日志
	const auto& cfg = diagnostics();
	if (!cfg.blackbox)
		return;  // 门关：连"试开"的痕迹都不留（门关态不刷日志）
	if (cfg.blackboxPath.empty())
		return;  // "门开但没路径"由 gate_lint 出 warn，这里不重复出声
	std::lock_guard lk(ioMu);
	file.open(cfg.blackboxPath, std::ios::out | std::ios::trunc);
	if (!file.good()) {
		++stats.openFailures;
		// 🔴 不抛：黑匣子是诊断设施，它坏了不能把引擎带崩。但必须**红着说**——
		//    一轮实验零产物而日志安静，是本项目最忌的"看起来已完工的半吊子"。
		if (auto* logger = RLLogger::getInstance())
			logger->error("blackbox: 🔴 门开着但**打不开** {} —— 本轮实验没有任何黑匣子产物，"
			              "任何『黑匣子没发现异常』的结论都不成立",
				cfg.blackboxPath);
		return;
	}
	produced.store(0u);
	consumed.store(0u);
	stats.written = stats.overwritten = 0u;
	stats.open = true;
	running.store(true);
	writer = std::thread([this] { writerLoop(); });
	if (auto* logger = RLLogger::getInstance())
		logger->info("blackbox: 开录 → {}（环形缓冲 {} 帧，独立线程异步落盘，每批 flush）",
			cfg.blackboxPath, kRingSlots);
}

void State::shutdown() {
	{
		std::lock_guard lk(ioMu);
		if (!stats.open)
			return;
	}
	// 先排空再停：判据是"最后一条记录 = 案发前一帧"。
	running.store(false);
	cv.notify_all();
	if (writer.joinable())
		writer.join();
	std::lock_guard lk(ioMu);
	if (file.good())
		file.flush();
	file.close();
	stats.open = false;
}

}  // namespace

bool blackboxOpen(std::string_view path) {
	auto& st = state();
	std::lock_guard lk(st.ioMu);
	if (st.stats.open)
		return true;
	st.file.open(std::string(path), std::ios::out | std::ios::trunc);
	if (!st.file.good()) {
		++st.stats.openFailures;
		return false;  // 不抛：黑匣子空转，由 openFailures 明确区分"没录"与"录了没抓到"
	}
	st.produced.store(0u);
	st.consumed.store(0u);
	st.stats.written = st.stats.overwritten = 0u;
	st.stats.open = true;
	st.openAttempted = true;  // 显式开过 = 不再按门试开
	st.running.store(true);
	st.writer = std::thread([&st] { st.writerLoop(); });
	return true;
}

void blackboxEnsureStarted() {
	auto& st = state();
	if (st.running.load())
		return;
	st.openFromConfig();  // 内部自带"只试一次"，门关/无路径时静默返回
}

void blackboxRecord(const BlackboxFrameRecord& rec) {
	auto& st = state();
	if (!st.running.load())
		st.openFromConfig();  // 按门惰性开录（首帧一次，之后是纯原子读）
	if (!st.running.load())
		return;
	// 🔴 生产端只有：一次取号 + 一次 POD 拷贝 + 一次 release 发布。
	//    无锁、无分配、无格式化、无系统调用 —— 观测者效应的下界就在这三行上。
	const uint64_t idx = st.produced.fetch_add(1u);
	st.slots[idx & kRingMask] = rec;
	st.cv.notify_all();
}

void blackboxFlush() {
	auto& st = state();
	std::unique_lock lk(st.ioMu);
	if (!st.stats.open)
		return;
	const uint64_t target = st.produced.load();
	st.cv.notify_all();
	// 等消费端追平。写盘线程每批最多 512 条且每批 notify，故这里必然收敛；
	// 用条件变量而不是自旋（自旋要 this_thread::yield，rlstd 未导出，为一个收尾函数
	// 去动 rlstd 不值，且自旋会在排空期间白占一个核 = 观测者效应）。
	// 5 秒上限是护栏：真排不空（写盘线程已死）宁可带着未落盘返回，也不能把退出路径挂住。
	st.cv.wait_for(lk, std::chrono::seconds(5),
		[&st, target] { return st.consumed.load() >= target; });
	if (st.file.good())
		st.file.flush();
}

void blackboxClose() {
	state().shutdown();
}

BlackboxStats blackboxGetStats() {
	auto& st = state();
	std::lock_guard lk(st.ioMu);
	// 🔴 `recorded` 从 `produced` 派生而不是在生产端累加：生产端在渲染线程上**不持锁**
	//    （那是"GPU 路径零阻塞"的前提），若在那里写 stats.recorded，就和持锁拷贝整个结构体
	//    的 blackboxGetStats() 构成数据竞争。派生 = 零竞争且更准（含被覆盖的那些）。
	BlackboxStats s = st.stats;
	s.recorded = st.produced.load();
	return s;
}

}  // namespace RGL
