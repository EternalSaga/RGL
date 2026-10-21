module shadow_blackbox:impl;

import shadow_blackbox;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import frame_draw_list;  // MdiShadowSegment 字段（主接口的 import 不 export，这里得自己带）
import rlstd;
import rllogger;
import blackbox;          // 帧记录类型 + 摘要 + 落盘（纯逻辑全在那边）
import mdi_rows;          // kMdiShadowRowStride（按步长切行，不在这里抄 24）
import diagnostics;       // blackbox / dgc_static / shader_selfcheck 三枚门
import scene_forward_types_generated;  // ObjectClusterRange（债-143 审计读区间表镜像的条目）

namespace RGL {

namespace {

/** 把一个 MDI 段的事实抄进记录槽（纯搬运 + 摘要；抽出来是为了让宿主函数留在复杂度红线内）。 */
void fillSegment(BlackboxSegmentFacts& s, const MdiShadowSegment& seg) {
	s.cascadeIndex = seg.cascadeIndex;
	s.frameSlot = seg.frameSlot;
	s.firstRow = seg.firstRow;
	s.rowCount = seg.rowCount;
	s.taskCount = seg.taskCount;
	s.pipeline = seg.pipeline;
	s.argsOffset = seg.loc.argsOffset;
	s.rowBase = seg.loc.rowBase;
	// 🔴 COUNT 与它的来源**各记一份**：执行器读的是 `DrawCall::drawCount`，而它的产地是
	// `seg.rowCount`（`makeMdiShadowDrawCall`）。本案主嫌疑就是这两个数分家，只记一个
	// 就等于没记 —— 分家时日志仍然自洽（同"单一来源不承重，访问路径必须消费那个来源"）。
	s.drawCount = seg.rowCount;
	s.drawCountSource = seg.rowCount;
	const auto dig = blackboxDigestSegment(std::as_bytes(seg.rows), kMdiShadowRowStride);
	s.sliceDigest = dig.digest;
	s.head = dig.head;
	s.tail = dig.tail;
	s.headRowIndex = seg.loc.rowBase;
	s.tailRowIndex = dig.hasRows
	                     ? seg.loc.rowBase + static_cast<uint32_t>(seg.rows.size()) - 1u
	                     : seg.loc.rowBase;
}

/** 抄 heap 槽台账（可空服务 = 留空；🔴 语义是"条目会读的槽"，见 meshlet_shadow_args 接口注释）。 */
void fillHeapSlots(BlackboxFrameRecord& rec, const MeshletShadowArgsService* args) {
	if (args == nullptr)
		return;
	const auto slots = args->entrySlotNumbers();
	for (size_t k = 0u; k < slots.size() && k < kBlackboxMaxHeapSlots; ++k)
		rec.heapSlots[rec.heapSlotCount++] = slots[k];
}

/** 段数超台账容量：宁可少记也要**出声一次**（静默截断 = "三段的现场"被读成"两段的现场"）。 */
void warnIfTruncated(size_t segs, uint32_t recorded) {
	if (segs <= kBlackboxMaxSegments)
		return;
	static bool warned = false;
	if (warned)
		return;
	warned = true;
	if (auto* logger = RLLogger::getInstance())
		logger->warn("blackbox: MDI 段数 {} > 台账容量 {} ⟹ **每帧只记前 {} 段**，"
		             "其余段的现场不在黑匣子里（要全记请抬 kBlackboxMaxSegments）",
			segs, kBlackboxMaxSegments, recorded);
}

}  // namespace

void recordShadowMdiBlackbox(const FrameDrawList& list, uint64_t frame,
	const MeshletShadowArgsService* args) {
	if (!diagnostics().blackbox)
		return;  // 门关 = 一条 if 都不进（出厂锚逐位不变的前提）
	// 🔴 开录必须在判空**之前**：对照臂（mdi_shadow=false → 零段）若走不到开录，
	//    "黑匣子坏了"与"黑匣子没东西可记"就同形了（理由见 blackboxEnsureStarted 注释）。
	blackboxEnsureStarted();
	if (list.mdiSegments.empty())
		return;  // 回落逐条 draw 的帧没有段可记（不是"没录上"，别与 gate_lint 那条 warn 混读）

	BlackboxFrameRecord rec;
	rec.frame = frame;
	rec.timestampNs = blackboxNowNs();
	// DGC 状态 = 静态 shader DGC 路那枚门（对象由 main 按此门 + 设备能力条件构造），
	// 这里记门而不是记指针：指针非空还取决于设备能力，读日志的人要的是"配置上要它开吗"。
	rec.dgcActive = diagnostics().dgcStatic ? 1u : 0u;
	rec.selfCheckOn = diagnostics().shaderSelfcheck ? 1u : 0u;

	uint32_t i = 0u;
	for (const auto& seg : list.mdiSegments) {
		if (i >= kBlackboxMaxSegments)
			break;
		fillSegment(rec.segments[i++], seg);
	}
	rec.segmentCount = i;
	warnIfTruncated(list.mdiSegments.size(), i);
	fillHeapSlots(rec, args);
	blackboxRecord(rec);
}

namespace {

/** 每帧 WARN 明细上限（限流口径 → 接口注释；总数行不受限）。 */
constexpr uint32_t kAuditWarnPerFrame = 5u;

/**
 * 装桩证据（只一次）：通过时不逐帧出声，没有这行，"零 WARN"就分不清"审了没问题"与
 * "根本没接上"——上两批都踩过这条（blackbox 开录行 / heap_slot_audit 首调行，同先例）。
 */
void noteAuditInstalled(RLLogger* logger) {
	static bool installed = false;
	if (installed || logger == nullptr)
		return;
	installed = true;
	logger->info("meshlet: 债-143 行表审计已装桩（常开，不经门）—— 逐帧审 gcX>0 的行："
	             "clusterStart + 最大簇内序 ≤ ObjectClusterRange::count（64 位），"
	             "每帧明细限 {} 条 + 总数行",
		kAuditWarnPerFrame);
}

/**
 * 审一行并出声（明细限流由调用方累计的 totalSoFar 决定）。
 * @return 本行是否违例（1/0，供总数累计——"总量在不在"不许被限掉）
 */
uint32_t auditRow(RLLogger* logger, uint64_t frame, const MdiShadowSegment& seg, size_t i,
	std::span<const ObjectClusterRange> ranges, uint32_t totalSoFar) {
	const MdiShadowRow& row = seg.rows[i];
	// 只审 gcX>0 的行 = GPU 真会读的那些（不可见行根本不启动 task，与 mesh 摄像头同一口径；
	// 黑匣子已逐帧记全行摘要，病态空行逃不过事后复盘）。
	if (row.groupCountX == 0u)
		return 0u;
	const bool entryExists = row.objectIndex < ranges.size();
	// 越界行的 rangeCount 给 0（区间条目不存在）⟹ 要簇的行两条位同响 = "行整个指错"报满。
	const ObjectClusterRange range =
	    entryExists ? ranges[row.objectIndex] : ObjectClusterRange {};
	const uint32_t bits = shadowRowRangeViolations(ShadowRowRangeInput {
		.objectIndex = row.objectIndex,
		.objectCapacity = static_cast<uint32_t>(ranges.size()),
		.clusterStart = row.clusterStart,
		.meshGroupCount = row.meshGroupCount,
		.rangeCount = range.count,
	});
	if (bits == kRowRangeViolNone)
		return 0u;
	if (totalSoFar < kAuditWarnPerFrame && logger != nullptr) {
		// 字段名与 GPU MESH_SELFCHECK 的 printf 逐项同名（obj/cstart/range_base/range_count），
		// 同源对账 = 两侧 grep 同一个词比数值。
		logger->warn("meshlet: DEBT143_AUDIT frame={} cascade={} row={} obj={} cstart={} mgc={} "
		             "range_base={} range_count={} need={} kind={}",
			frame, seg.cascadeIndex, seg.firstRow + static_cast<uint32_t>(i), row.objectIndex,
			row.clusterStart, row.meshGroupCount, range.base, range.count,
			shadowRowRangeNeeded(row.clusterStart, row.meshGroupCount),
			describeShadowRowRangeViolations(bits));
	}
	return 1u;
}

}  // namespace

void auditShadowMdiRows(const FrameDrawList& list, uint64_t frame) {
	if (list.mdiSegments.empty())
		return;  // 门关 / 回落 = 无行可审（静默；"没段"与"审了没问题"的区分靠装桩 INFO）
	auto* logger = RLLogger::getInstance();
	noteAuditInstalled(logger);
	const auto& ranges = list.sceneData.clusterRanges;
	uint32_t total = 0u;
	for (const auto& seg : list.mdiSegments)
		for (size_t i = 0u; i < seg.rows.size(); ++i)
			total += auditRow(logger, frame, seg, i, ranges, total);
	if (total > kAuditWarnPerFrame && logger != nullptr)
		logger->warn("meshlet: DEBT143_AUDIT frame={} 本帧共 {} 条违例，明细只出前 {} 条（限流，"
		             "总数不受限）",
			frame, total, kAuditWarnPerFrame);
}

}  // namespace RGL
