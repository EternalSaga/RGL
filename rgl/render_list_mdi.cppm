module;
#include <fmt/format.h>  // 回落文案拼装（mdiGateMessage；先例 = main:meshlets / shader_registry:impl 的 GMF fmt）
/**
 * render_list:mdi —— ★M-1b 真 MDI 阴影段的**采集机械室**（自 :impl 拆出，文件代码行
 * 红线 500；职责边界 = 「行表怎么建、MDI draw 怎么插」全部住这里，:impl 只留 system 管道
 * 与其余 fill* 函数）。docs/mdi_plan.md §19.2 D4 / §19.5。
 *
 * 内容与拆分前逐条同源：`MdiCascadePlan` / `buildMdiShadowPlans`（预构建 + 采集期预检）/
 * `makeMdiShadowDrawCall`（同源 pair → DrawCall 三件套）/ `emitShadowCasters`（导出接缝的
 * 定义——混合形态主循环 + 段尾补发）。拆出时按 style-check 把 `buildMdiShadowPlans` 收成
 * 「gate 状态码扫描 → 消息渲染 → 逐级联建行 → 形态对账」四段、`emitShadowCasters` 抽出
 * 旧形态支路 `emitLegacyShadowDraw` 与吸收判定 `absorbMdiCandidate`——
 * **纯结构搬运，判据与回落规则一条不加不减**（回落 WARNING 的文案两两不同 = 变异归因的
 * 抓手，改文案必须连 test_mdi_shadow_emit 的判据一起改）。
 */

module render_list:mdi;

import render_list;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import scene_context;            // ShadowVolume + casterInShadowVolume（剔除判据与逐条形态同一条）
import frame_draw_list;          // DrawCall / MdiShadowSegment / ShadowMeshletDrawArgs
import asset_table;              // kInvalidPipelineIndex + numeric_limits 先例（rlstd 已带）
import scene_data;               // kActiveShadowCascades
import mdi_rows;                 // MdiShadowRow/RowLocation/buildMdiShadowRows + 容量算式（★M-0/M-1a）
import shadow_proxy_types;       // pinActive / ShadowProxyTable::Entry（N10 P3 钉段行与逐条支路同一个门）

namespace RGL {
namespace {

/**
 * ★M-1b：一帧的行表预构建（emit 的 MDI 前置趟）。
 *
 * 产出 = `mdi->segments`（每生效级联一条 `MdiShadowSegment`，行 append 进 `mdi->rows`）+
 * 每条 `MdiCascadePlan`（emit 主循环插 MDI DrawCall 用的读数：同源 pair + 行数 + 条目）。
 * **行只准出自 `buildMdiShadowRows`**（★M-0；任务书 (4) 红线「不许自己拼行」）；本层只
 * 搬运输入与读数，不乘 stride、不加帧槽偏移（pair = `mdiShadowRowLocation` 一次返回）。
 *
 * 回落规则（任何一条不过 = 本帧整体逐条 draw，segments 清空 ⟹ 执行器零新命令）：
 *  · `mdi` 存储缺位（装配层没给，判点在 emitShadowCasters 入口）；
 *  · 可切候选数 < 2（`DrawCall::drawCount > 1` 是 MDI 的唯一判据，1 行无法表达）——**静默**；
 *  · 可切候选的 `meshletMdiPipeline` 不全一致（全场景 1 条目 = 不变式，漂了 = 烘焙分家）；
 *  · 容量闸门：级联段数 × 每段行数 > 载体 `sliceRows`（ring 的 `upload` 会再拦一次，
 *    两道判据同式 = `mdiShadowRowLocation`/`mdiShadowArgsCapacityBytes` 那套闭式）；
 *  · 🔴 采集期预检（雷 3 处置：这些判据**不留到录制期**——录制期 upload 被拒的后果是
 *    "照常提交一张内容陈旧的行表" = 影子用错误可见性画且零信号，故采集期全数拦下、拦下
 *    = 回落出声；录制期同名核对 = 纯 fail-fast，按构造不可达，到达即抛，见
 *    vkrenderer_passrec::prepareMdiShadowRows）：
 *      - `frameSlot < slices`（ring×2 写安全前提 = 只写本帧槽；轮转口径变了 = 接线 bug）；
 *      - pushArgs 已循环前 resize 到级联数（span 悬空红线同族）；
 *      - `totalRows × stride` 的 argsOffset 32 位装得下（DrawCall::argsOffset 按 D3 定为
 *        uint32；真产物远不到）；
 *      - 段数 == 生效级联数（少一段 = 某级联的行表与 draw 分家）；
 *      - 每段 `firstRow + rowCount ≤ sliceRows`（越界 = 踩进下一帧槽，upload 会拒但那时
 *        DrawCall 已发出）。
 *  · 构建器不变量破（错误串原样打进 WARNING）。
 */
struct MdiCascadePlan {
	const RGL::MdiShadowSegment* segment = nullptr;  // 指向 mdi->segments 的元素（预构建后地址稳定）
	bool drawn = false;                              // 本级联的 MDI DrawCall 已插入
};

/** 预构建的行数/段数算式（plans + segments 共用，防两处各乘一遍）。 */
constexpr uint32_t mdiShadowCascades() {
	// c0 恒旧路径（§18.6），写成减法而不是字面 3 = 切级联时不漏校验（wireMeshletShadowArgs 同法）。
	return kActiveShadowCascades - 1u;
}

/** gate 扫描的产出（每级联行数 + 全场景唯一的 MDI 条目下标）。 */
struct MdiShadowGate {
	uint32_t rowsPerCascade = 0;
	uint32_t pipeline = kInvalidPipelineIndex;
};

/** 回落原因（`tooFew` 之外的每一种都要出声；文案表见 `mdiGateMessage`）。 */
enum class MdiGateFail { none, tooFew, entrySplit, capacity, slotDrift, pushArgs, offset32 };

/**
 * 前置扫描：数「可切且带 MDI 宿主」候选 + 条目一致性 + 容量/轮转/存储预检。
 * 只产**状态码**不出声（出声集中在 `buildMdiShadowPlans` 一处，避免 8 条 warn 把
 * 认知复杂度堆过阈值；文案两两不同 = 变异归因的抓手，语义与拆分前逐条相同）。
 */
MdiGateFail mdiScanGate(std::span<const ShadowCandidate> cands,
	const MdiShadowEmitContext& mdi, MdiShadowGate& gate) {
	for (const auto& cd : cands) {
		if (cd.meshletPipeline == kInvalidPipelineIndex || cd.meshletMdiPipeline == kInvalidPipelineIndex)
			continue;  // 不可切 / 无 MDI 宿主 = 不进表（混合形态的另一半，继续逐条旧 draw）
		++gate.rowsPerCascade;
		// N10 P3：钉段行**恒占一行**（只要这候选有钉段），texel 门只决定它的 gcX ——
		// 行表布局按"每级联行数相同"算（`firstRow += rowsPerCascade` + 载体 sliceRows 闭式），
		// 而门是逐级联的，按门加减行数会让每段不等。与载体容量那侧的锚**同一条口径**
		// （`wireMeshletShadowArgs`：选级表 count>0 的槽数 + 钉段表 count>0 的槽数）。
		gate.rowsPerCascade += cd.pinnedCount > 0u ? 1u : 0u;
		if (gate.pipeline == kInvalidPipelineIndex)
			gate.pipeline = cd.meshletMdiPipeline;
		else if (gate.pipeline != cd.meshletMdiPipeline)
			return MdiGateFail::entrySplit;  // 🔴 全场景 1 条目破了：不猜用哪个，整帧回落
	}
	const uint32_t cascades = mdiShadowCascades();
	if (gate.rowsPerCascade < 2u)
		return MdiGateFail::tooFew;  // 静默回落（drawCount=1 无法作 MDI 判据；出生效日志里读）
	if (gate.rowsPerCascade * cascades > mdi.sliceRows)
		return MdiGateFail::capacity;
	if (mdi.frameSlot >= mdi.slices)
		return MdiGateFail::slotDrift;
	if (mdi.pushArgs->size() < kActiveShadowCascades)
		return MdiGateFail::pushArgs;
	if (mdiShadowArgsCapacityBytes(gate.rowsPerCascade * cascades, 1u)
		> std::numeric_limits<uint32_t>::max())
		return MdiGateFail::offset32;
	return MdiGateFail::none;
}

/** 状态码 → 回落 WARNING 文案（nullopt = tooFew 静默档；文案与拆分前逐字同）。 */
std::optional<std::string> mdiGateMessage(MdiGateFail fail, const MdiShadowGate& gate,
	const MdiShadowEmitContext& mdi) {
	switch (fail) {
	case MdiGateFail::none:
	case MdiGateFail::tooFew:
		return std::nullopt;
	case MdiGateFail::entrySplit:
		return std::string("可切候选的 MDI 条目下标不一致（全场景 1 条目的不变式破了 = 烘焙分家）");
	case MdiGateFail::capacity:
		return fmt::format("行表容量不足：每段行数 {} × 级联段 {} > 载体 sliceRows {}（容量上界口径 = "
		                   "选级表 count>0 的 objectSlot 数 + N10 P3 钉段表 count>0 的槽数，再乘段数，"
		                   "见 wireMeshletShadowArgs）",
			gate.rowsPerCascade, mdiShadowCascades(), mdi.sliceRows);
	case MdiGateFail::slotDrift:
		return fmt::format("frameSlot {} ≥ slices {}（ring×2 的写安全前提 = 只写本帧槽；"
		                   "轮转分母口径失配 = 接线 bug）", mdi.frameSlot, mdi.slices);
	case MdiGateFail::pushArgs:
		return fmt::format("pushArgs 未循环前 resize 到级联数（{} < {}，span 悬空红线同族）",
		mdi.pushArgs->size(), kActiveShadowCascades);
	case MdiGateFail::offset32:
		return fmt::format("argsOffset 超出 uint32（totalRows {} × stride {} 装不进 "
		                   "DrawCall::argsOffset）",
			gate.rowsPerCascade * mdiShadowCascades(), kMdiShadowRowStride);
	}
	return std::nullopt;  // 不可达（enum 全覆盖），留编译器兜底
}

/**
 * 一个级联的行表输入打包（候选序；🔴 判据与逐条形态同一条 `casterInShadowVolume`，
 * 不可见 = `gcX=0` 而**行照发**——孔洞流，§19.2 D4）。
 * N10 P3：有钉段的候选**贡献两行**（主行 + 钉段行，见函数体内注释）；钉段行的可见性额外过
 * `pinActive`（与逐条形态 `emitPinnedShadowDraw` **同一个门、同一个 texel 口径**）。
 * @param texel 本级联纹素尺寸（世界米，`shadowTexelSize` 接缝的第三个消费者）
 */
std::vector<MdiShadowCandidate> mdiCascadeInputs(std::span<const ShadowCandidate> cands,
	const ShadowVolume& volume, float texel) {
	std::vector<MdiShadowCandidate> inputs;
	for (const auto& cd : cands) {
		if (cd.meshletPipeline == kInvalidPipelineIndex || cd.meshletMdiPipeline == kInvalidPipelineIndex)
			continue;
		const bool visible = casterInShadowVolume(volume, cd.center, cd.radius);
		MdiShadowCandidate in;
		in.objectSlot = cd.objectSlot;
		in.meshGroupCount = meshletTaskCount(cd.meshletGroups);  // ×2 唯一真相源（喂 word5，非 gcX）
		in.clusterStart = cd.clusterStart;
		in.visible = visible;
		inputs.push_back(in);
		// N10 P3 钉段行（🔴 **行照发，不管本级 texel 门过不过**）：行表布局假设"每级联行数相同"
		// （`firstRow += rowsPerCascade` 与载体 `sliceRows` 的闭式都按它算），texel 门是**逐级联**
		// 的，若按门决定发不发行，每级联行数就不等了。所以门只决定这一行的 `gcX`：过门 = 可见
		// 行（gcX=1），不过门 = 与"本级联看不见"同一形态（gcX=0 = 空转，§19.2 D4 的孔洞流）。
		// 行序 = 候选序**展开**序（钉段行紧跟其主行）；图集 depth-only + eLess ⟹ 行序不进判据。
		if (cd.pinnedCount == 0u)
			continue;
		MdiShadowCandidate pin;
		pin.objectSlot = cd.objectSlot;
		pin.meshGroupCount = meshletTaskCount(cd.pinnedCount);  // 同一个 ×2 真相源
		pin.clusterStart = cd.pinnedStart;
		pin.visible = visible && pinActive(
			ShadowProxyTable::Entry { .start = cd.pinnedStart, .count = cd.pinnedCount,
				.radius = cd.pinnedRadius },
			cd.pinnedWorldScale, texel);
		inputs.push_back(pin);
	}
	return inputs;
}

/**
 * 段形态对账（采集期预检的后半，雷 3）：段数必须恰为生效级联数，每段落点必须在本帧槽
 * slice 内（越界 = 踩进下一帧槽，upload 会拒但那时 DrawCall 已发出）。不过 = 清表 + false。
 * @param mdi 采集上下文（rows/segments 存储 + 容量口径）
 * @param logger 日志器（可空 = 不出声，但**不改变判定**）
 * @return true = 形态自洽
 */
bool mdiShadowSegmentsFit(const MdiShadowEmitContext& mdi, RLLogger* logger) {
	const uint32_t cascades = mdiShadowCascades();
	if (mdi.segments->size() != cascades) {
		if (logger)
			logger->warn("meshlet: mdi 回落逐条 draw —— 段数 {} ≠ 生效级联数 {}（某级联的行表与 draw "
						 "分家 = 接线 bug），本帧阴影段零新命令",
				mdi.segments->size(), cascades);
		mdi.rows->clear();
		mdi.segments->clear();
		return false;
	}
	for (const auto& seg : *mdi.segments) {
		if (seg.firstRow + seg.rowCount > mdi.sliceRows) {
			if (logger)
				logger->warn("meshlet: mdi 回落逐条 draw —— c{} 段落点越界：firstRow {} + rowCount {} > "
							 "sliceRows {}（越界 = 踩进下一帧槽），本帧阴影段零新命令",
					seg.cascadeIndex, seg.firstRow, seg.rowCount, mdi.sliceRows);
			mdi.rows->clear();
			mdi.segments->clear();
			return false;
		}
	}
	return true;
}

/**
 * 逐级联建行 + 段描述入表（`mdi->rows` 首尾相接、`mdi->segments` 每级联一条）。
 * 任一级联构建失败 = 清表返回 false（**不静默半切**）。
 * 🔴 同源 pair 的**唯一**产地（任务书 (3) 红线）：`argsOffset` 与 `rowBase` 取
 * `mdiShadowRowLocation` 同一次返回的两个字段；录制期 upload(frameSlot, firstRow, rows)
 * 在 ring 内部走同一个具名入口、同一组输入（frameSlot 由执行器与采集侧同值对账）。
 */
bool mdiBuildShadowSegments(std::span<const ShadowCandidate> cands,
	std::span<const ShadowVolume> volumes, const MdiShadowEmitContext& mdi,
	const MdiShadowGate& gate) {
	auto* logger = RLLogger::getInstance();
	const uint32_t totalRows = gate.rowsPerCascade * mdiShadowCascades();
	mdi.rows->clear();
	mdi.rows->reserve(totalRows);  // 🔴 精确 reserve：span 发出后**不再有** realloc（resize-before-loop 红线）
	mdi.segments->clear();
	mdi.segments->reserve(mdiShadowCascades());
	uint32_t firstRow = 0;
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c) {
		auto table = buildMdiShadowRows(
		    mdiCascadeInputs(cands, volumes[c], shadowTexelSize(volumes[c].xyRadius)));
		if (!table) {
			if (logger)
				logger->warn("meshlet: mdi 行表构建失败（c{}）—— 本帧回落逐条 draw：{}", c, table.error());
			mdi.rows->clear();
			mdi.segments->clear();
			return false;
		}
		const MdiShadowRowLocation loc = mdiShadowRowLocation(mdi.sliceRows, mdi.frameSlot, firstRow);
		std::span<const MdiShadowRow> rowSpan{mdi.rows->data() + firstRow, table->rows.size()};
		mdi.rows->insert(mdi.rows->end(), table->rows.begin(), table->rows.end());
		mdi.segments->push_back(MdiShadowSegment{
			.cascadeIndex = c,
			.firstRow = firstRow,
			.rowCount = static_cast<uint32_t>(table->rows.size()),
			// 锚是 uint64_t，但不变量 3b 保证它恒等于 `rowCount − emptyRows`（逐行 gcX ≤ 1）
			// ⟹ 上界就是 rowCount（uint32_t）⟹ 这条窄化不会回卷
			.taskCount = static_cast<uint32_t>(table->sumGroupCountX),
			.pipeline = gate.pipeline,
			.frameSlot = mdi.frameSlot,
			.loc = loc,
			.rows = rowSpan,
		});
		firstRow += gate.rowsPerCascade;
	}
	return mdiShadowSegmentsFit(mdi, logger);
}

/** @return true = 生效形态成立（segments 已填）；false = 本帧回落逐条 draw（出声口径见 gate） */
bool buildMdiShadowPlans(std::span<const ShadowCandidate> cands,
	std::span<const ShadowVolume> volumes, const MdiShadowEmitContext& mdi,
	std::array<MdiCascadePlan, kActiveShadowCascades>& plans) {
	MdiShadowGate gate;
	const auto fail = mdiScanGate(cands, mdi, gate);
	if (fail != MdiGateFail::none) {
		const auto msg = mdiGateMessage(fail, gate, mdi);
		if (msg) {
			if (auto* logger = RLLogger::getInstance())
				logger->warn("meshlet: mdi 回落逐条 draw —— {}，本帧阴影段零新命令", *msg);
		}
		return false;
	}
	if (!mdiBuildShadowSegments(cands, volumes, mdi, gate))
		return false;
	for (const auto& seg : *mdi.segments)
		plans[seg.cascadeIndex].segment = &seg;
	return true;
}

/** MDI DrawCall 的构造（行表段 → 一次逻辑 draw 的三件套：条目 / 同源 pair / 行数）。 */
RGL::DrawCall makeMdiShadowDrawCall(const RGL::MdiShadowSegment& seg,
	const MdiShadowEmitContext& mdi) {
	auto& args = (*mdi.pushArgs)[seg.cascadeIndex];
	// 🔴 12B push 在 MDI 变体下的语义（word0=rowBase / word2=reserved）见 shadow_meshlet.mesh.slang
	args.objectIndex = seg.loc.rowBase;     // MDI 语义：行基址（同源 pair 的 rowBase）
	args.cascadeIndex = seg.cascadeIndex;
	args.clusterStart = 0u;                 // reserved（clusterStart 进行表 word[4]）
	return RGL::DrawCall{
		.pipeline = seg.pipeline,
		.pushData = std::as_bytes(std::span<const RGL::ShadowMeshletDrawArgs>{&args, 1}),
		.groupCount = 0u,                   // MDI 下被忽略（GPU 逐行读 gcX；置 0 = 没人误读旧字段）
		.argsOffset = static_cast<uint32_t>(seg.loc.argsOffset),  // 同源 pair 的 argsOffset
		.drawCount = seg.rowCount,          // >1 = 执行器走 vkCmdDrawMeshTasksIndirectEXT 分支
	};
}

/**
 * N10 P3：钉段的那条 draw（c1..c3，**选级恒 0** —— 钉段簇区间本身就是 L0 前缀，
 * 所以 shader 零改动：12B push 里换掉的只有 clusterStart，task 数换成钉段簇数的）。
 *
 * 判据只有一条：`pinActive`（rlbase 纯函数，K=1 写死）。它自带 `count > 0`，所以"没有钉段"
 * 与"有钉段但本级纹素太粗"从同一个门出去 = 不发。宿主（本候选可切 = 有簇管线条目）由调用方
 * 保证 —— 钉段 draw 是**簇** draw，没有条目就无处可发（嵌套理由 → collectShadowCasters，
 * 同一条判据不在这里重述第二遍）。
 * 🔴 task 数走 `meshletTaskCount`（唯一真相源），不在这里乘槽数。
 * @param texel 本级联纹素尺寸（世界米 = `shadowTexelSize(volumes[c].xyRadius)`）
 */
void emitPinnedShadowDraw(const ShadowCandidate& cd, uint32_t c, float texel,
	uint32_t objectSlotCount, std::vector<RGL::ShadowMeshletDrawArgs>& pinnedArgs,
	std::array<std::vector<RGL::DrawCall>, kActiveShadowCascades>& out) {
	const ShadowProxyTable::Entry pin {
		.start = cd.pinnedStart, .count = cd.pinnedCount, .radius = cd.pinnedRadius };
	if (!pinActive(pin, cd.pinnedWorldScale, texel))
		return;
	auto& pargs = pinnedArgs[static_cast<size_t>(c) * objectSlotCount + cd.objectSlot];
	pargs.objectIndex = cd.objectSlot;
	pargs.cascadeIndex = c;
	pargs.clusterStart = cd.pinnedStart;
	out[c].push_back(RGL::DrawCall{cd.meshletPipeline,
		std::as_bytes(std::span<const RGL::ShadowMeshletDrawArgs>{&pargs, 1}),
		meshletTaskCount(cd.pinnedCount)});
}

/**
 * 旧形态的一条阴影 draw（c0 恒此路；★M-1b 生效档里 = 不可切候选的原样逐条支路）。
 * 🔴 **c0 恒旧路径**（§18.6 第一刀形态零改动；IZB mask 重放 replayIzbMaskSegment
 * 只重放 c0 段且按 8B ShadowDrawArgs 读 push，靠的就是这条）。c1..c3 且候选可切
 * → 簇管线 + meshletTaskCount(簇数) + 12B push。
 * ⚠ N10 P3 起「**条数逐条不变**」这句只对**无钉段**的候选成立：含钉段且过 texel 门的候选
 * 在 c1..c3 各多一条 draw（上限 = 被钉 prim 数 × 3 段，bistro = 13×3 = 39 条）。
 * 钉段那条排在常规条**之后**：depth-only 图集 + eLess 下先后无关（同 P1 重排不换锚的理由），
 * 放在后面是为了让"门关 = 那一行根本不执行 = 逐位不变"在 diff 里一眼可见。
 */
void emitLegacyShadowDraw(const ShadowCandidate& cd, uint32_t c, float texel,
	uint32_t objectSlotCount,
	std::vector<RGL::ShadowDrawArgs>& shadowArgs,
	std::vector<RGL::ShadowMeshletDrawArgs>& shadowMeshletArgs,
	std::vector<RGL::ShadowMeshletDrawArgs>& shadowMeshletPinnedArgs,
	std::array<std::vector<RGL::DrawCall>, kActiveShadowCascades>& out) {
	const size_t idx = static_cast<size_t>(c) * objectSlotCount + cd.objectSlot;
	if (c >= 1u && cd.meshletPipeline != kInvalidPipelineIndex) {
		auto& margs = shadowMeshletArgs[idx];
		margs.objectIndex = cd.objectSlot;
		margs.cascadeIndex = c;
		margs.clusterStart = cd.clusterStart;
		out[c].push_back(RGL::DrawCall{cd.meshletPipeline,
			std::as_bytes(std::span<const RGL::ShadowMeshletDrawArgs>{&margs, 1}),
			meshletTaskCount(cd.meshletGroups)});
		emitPinnedShadowDraw(cd, c, texel, objectSlotCount, shadowMeshletPinnedArgs, out);
		return;
	}
	auto& sargs = shadowArgs[idx];
	sargs.objectIndex = cd.objectSlot;
	sargs.cascadeIndex = c;
	out[c].push_back(RGL::DrawCall{cd.pipeline,
		std::as_bytes(std::span<const RGL::ShadowDrawArgs>{&sargs, 1}), cd.groups});
}

/**
 * ★M-1b 生效档的吸收判定：本候选在本级联**是否归行表管**（可切 + 带 MDI 宿主 + 本级联有段）。
 * 吸收 = 不发逐条 draw（吸收关系由行序=候选序担保）；本级联首个「可见且可切」候选的
 * 位置 = 一条 MDI DrawCall 的插入点（`drawn` 幂等：后续可见候选不再插）。
 * @return true = 已吸收（调用方跳过旧形态）；false = 候选不归行表管 = 走逐条
 */
bool absorbMdiCandidate(const ShadowCandidate& cd, uint32_t c, bool visible,
	std::array<MdiCascadePlan, kActiveShadowCascades>& plans, const MdiShadowEmitContext& mdi,
	std::array<std::vector<RGL::DrawCall>, kActiveShadowCascades>& out) {
	if (c < 1u || cd.meshletPipeline == kInvalidPipelineIndex
		|| cd.meshletMdiPipeline == kInvalidPipelineIndex || plans[c].segment == nullptr)
		return false;
	if (visible && !plans[c].drawn) {
		plans[c].drawn = true;
		out[c].push_back(makeMdiShadowDrawCall(*plans[c].segment, mdi));
	}
	return true;
}

/**
 * 段尾补发：本级联有行但**无一可见**（全 gcX=0 的孔洞流合法形态）时，MDI 条目照发在
 * 段尾（depth-only min 与 draw 序无关；不发 = 空转行表段与 draw 分家，读数会骗人）。
 */
void emitMdiTailDrawCalls(const std::array<MdiCascadePlan, kActiveShadowCascades>& plans,
	const MdiShadowEmitContext& mdi,
	std::array<std::vector<RGL::DrawCall>, kActiveShadowCascades>& out) {
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c)
		if (plans[c].segment != nullptr && !plans[c].drawn)
			out[c].push_back(makeMdiShadowDrawCall(*plans[c].segment, mdi));
}

}  // namespace

/**
 * 定义（接口声明与契约见 render_list.cppm）。候选 × 逐级光空间体 → 阴影 draw 清单
 * （级主序，级内实体迭代序）。一个图元可以落多级（大物体跨越多个切片）——这正是逐级
 * 剔除能把总量压在单级之上的原因。
 *
 * 🔴 ★M-1b `mdi` 生效档的形态改动只有一处：可切候选（c1..c3）不再逐条发簇 draw，而是
 * **每级联一条** MDI DrawCall（位置 = 首个可见可切候选处；该级联无一可见时挂段尾——
 * depth-only 图集的逐纹素 min 与 draw 序无关，§19.1 第三行；行**序**契约在行表内部仍成立）。
 * 不可切候选的旧 draw 逐条原样。构建/回落细节 → `buildMdiShadowPlans` 注释。
 */
void emitShadowCasters(std::span<const ShadowCandidate> cands,
	std::span<const ShadowVolume> volumes, uint32_t objectSlotCount,
	std::vector<RGL::ShadowDrawArgs>& shadowArgs,
	std::vector<RGL::ShadowMeshletDrawArgs>& shadowMeshletArgs,
	std::vector<RGL::ShadowMeshletDrawArgs>& shadowMeshletPinnedArgs,
	std::array<std::vector<RGL::DrawCall>, kActiveShadowCascades>& out,
	const MdiShadowEmitContext* mdi) {
	// N10 P3：逐级联的纹素尺寸（钉段 texel 门的输入）。🔴 与写进 scene data 的
	// `shadowTexelSizes` **同一条式子**（`shadowTexelSize` 接缝，本函数是第三个消费者）——
	// 分两遍写 = 「采样端认为一个纹素多大」与「CPU 决定钉不钉」分家，表现是"该钉的没钉"，
	// 画面上读不出来。4 次/帧，不在候选循环里算。
	std::array<float, kActiveShadowCascades> texels{};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		texels[c] = shadowTexelSize(volumes[c].xyRadius);
	std::array<MdiCascadePlan, kActiveShadowCascades> plans{};
	const bool mdiForm = mdi != nullptr && mdi->rows != nullptr && mdi->segments != nullptr
		&& mdi->pushArgs != nullptr && buildMdiShadowPlans(cands, volumes, *mdi, plans);
	for (const auto& cd : cands)
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			const bool visible = casterInShadowVolume(volumes[c], cd.center, cd.radius);
			if (mdiForm && absorbMdiCandidate(cd, c, visible, plans, *mdi, out))
				continue;  // 钉段也一起被行表吸收（mdiCascadeInputs 那侧多一行），不走逐条支路
			if (!visible)
				continue;
			emitLegacyShadowDraw(cd, c, texels[c], objectSlotCount, shadowArgs, shadowMeshletArgs,
				shadowMeshletPinnedArgs, out);
		}
	if (!mdiForm)
		return;
	// 本级联有行但无一可见（全 gcX=0 的孔洞流合法形态）：MDI 条目照发在**段尾**（helper）。
	emitMdiTailDrawCalls(plans, *mdi, out);
}

}  // namespace RGL
