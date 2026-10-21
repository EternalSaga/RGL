module shadow_blackbox:impl;

import shadow_blackbox;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import frame_draw_list;  // MdiShadowSegment 字段（主接口的 import 不 export，这里得自己带）
import rlstd;
import rllogger;
import blackbox;          // 帧记录类型 + 摘要 + 落盘（纯逻辑全在那边）
import mdi_rows;          // kMdiShadowRowStride（按步长切行，不在这里抄 24）
import diagnostics;       // blackbox / dgc_static / shader_selfcheck 三枚门

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

}  // namespace RGL
