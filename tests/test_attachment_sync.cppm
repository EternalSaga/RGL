module;
/**
 * test_attachment_sync —— B2-4④′：meshlet 标志表 → 图附件**消费者声明** + 图声明面↔附件↔边表
 * 的双源对账
 *
 * ## 被测契约（三句话）
 *
 *  1. `declareBufferConsumptions`：向目标 pass 追加 storageRead 输入；附件必须已在图中
 *     （生产者声明先行）；幂等；试编译 fail-fast（同 pass 读写 feedback / 消费排在生产
 *     之前 = Error 且不落账）；落账前跑双源对账。
 *  2. `reconcileAttachmentSyncEdges`（双源对账）：正向（buffer 附件双声明 ⟹ 表必须有行）
 *     与反向（表行指向的附件出现在图里 ⟹ 两侧声明都必须存在）各配反向用例；
 *     附件不在 desc 里 = 休眠（门关档形态），不报。
 *  3. 附件↔边表（人工半边）：数量锚 + **名字钉死**——表行写的是字面量（offgraph_sync 在
 *     rlgfx，逆向 import 不了 rlscene 的模块常量），本文件是两侧唯一的相遇点。
 *
 * ## 为什么带 EVSSM 探针（实测锚点）
 *
 * 「EVSSM 矩图不迁入图」的裁决挂在一个实测事实：`sampleRead` **进**布局链（产 barrier
 * 条目），`storage*` 不进（plan-only）。本文件把这对不对称钉在**同一张图**里——左半边是
 * 「EVSSM 迁入会让图发布局转换、破 GENERAL 契约与逐位锚」的实证，右半边是
 * 「meshlet 声明命令流中性」的实证。
 *
 * ## 🔴 变异红线声明（哪个变异让我红；答不出 = 摆设）
 *
 *   1. 删附件↔边表行（kAttachmentSyncEdges 清空）→ 阳性锚点的消费者声明红（正向缺边）
 *      + 数量锚用例红（1 → 0）。
 *   2. 删正向规则 → 「缺边红」用例红（fake 附件双声明被静默放行）。
 *   3. 删反向规则 → 「半申报红」用例红（producer-only + 表行被静默放行）。
 *   4. declareBufferConsumptions 去掉落账前对账 → 「缺边红」用例红（声明会落账 + 标脏）。
 *   5. recompile 去掉兜底对账 → 「半申报的 recompile 红」用例红（脏图会编译成功）。
 *   6. 构造去掉对账 → 「构造抛」用例红（不抛了）。
 *   7. 消费者声明写成 storageWrite（档写错）→ 反向规则红（hasWrite 却缺 hasRead）。
 *   8. 消费者声明去掉「附件须在图中」检查 → 「生产者先行」用例红（消息不再指路
 *      declareBufferProductions，而是试编译报一句间接的注册缺失）。
 *   9. storageRead 改 sampleRead（buffer 进布局链）→ 试编译红（validatePassRef 双向规则），
 *      且 EVSSM 探针用例的 buffer 半边红（barrier 计划出现 buffer slot）。
 *  10. 表行名字字面量漂（改一个字母）→ 「名字钉死」用例红（表行 ≠ 模块常量）。
 *
 * ## 本文件**测不到**的（说清楚，别把"全绿"读成"同步对了"）
 *
 * 边在真程序里**有没有发**（那是帧末 `expectedPerFrame` 对账 + 真程序变异的账）；
 * 边 mask 方向与真实先后是否匹配（已知限制，同 hazard 表口径——「方向谎报」由编译
 * fail-fast 抓：读前置 / countRole 两条用例各自红）。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import rlstd 带进来
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

export module test_attachment_sync;

import rlstd;
import rlvk;
import vkerror;
import render_graph;
import dynamic_render_graph;
import offgraph_sync;
import meshlet_cull_service;  // kMeshletVisibleFlagsAttachment（名字钉死的另一侧）

namespace RGL {
namespace {

/** 注册一张 buffer（注册表进程级单例 ⟹ 名字逐用例唯一，先例同 test_attachment_buffer）。 */
void registerBuf(std::string_view name, vk::DeviceSize bytes) {
	registerBufferAttachment(name, [bytes] { return bytes; });
}

/**
 * 最小两 pass 图（p0 = 生产者宿主 / p1 = 消费者宿主；形态照 test_attachment_buffer
 * 的 bufferWriteGraph：depth 写者 + swapchain 写者，两个 pass 天然满足读前置）。
 */
RgGraphDesc twoPassGraph() {
	return RgGraphDesc{
		.attachments = {
			RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
			RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
		},
		.passes = {
			RgPassDesc{.name = "p0", .kind = RgPassKind::drawMesh, .state = "opaque",
				.drawSource = "opaque",
				.outputs = {RgPassAttachment{.attachment = "depth",
					.usage = RgUsage::depthWrite}}},
			RgPassDesc{.name = "p1", .kind = RgPassKind::drawMesh, .state = "opaque",
				.drawSource = "opaque",
				.outputs = {RgPassAttachment{.attachment = "swapchain",
					.usage = RgUsage::colorWrite}}},
		},
	};
}

std::string errOf(const std::expected<void, Error>& r) {
	return r.has_value() ? std::string{} : r.error().message;
}

bool contains(const std::string& haystack, std::string_view needle) {
	return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 表本体：数量锚 + 名字钉死（变异 1/10 的靶子）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("附件↔边表：数量锚 1 + 名字钉死 kMeshletVisibleFlagsAttachment + 边指向 meshlet 门") {
	// 表 = 「图内写×图内读共享」的全集。多一条 = 有人加了申报，少一条 = 真共享裸奔，
	// 两种都必须在这里出声（数量锚口径同 hazard 表）。
	const auto edges = attachmentSyncEdges();
	REQUIRE(edges.size() == 1);
	// 🔴 两侧唯一相遇点：表行是字面量（rlgfx 逆向 import 不了 rlscene 的常量），
	// 模块常量改名 / 表行手滑，都在这一条红（变异 10）。
	CHECK(edges[0].attachment == kMeshletVisibleFlagsAttachment);
	CHECK(edges[0].edge == RgSyncEdgeId::meshletVisibleWriteToMesh);
	CHECK(syncEdgeDesc(edges[0].edge).name == "meshlet_visible_write_to_mesh");
	CHECK(syncEdgeDesc(edges[0].edge).gate == RgSyncGate::meshlet);

	// 查表正反两半：真名命中同一条边；假名红且点名附件 + 指路表名。
	const auto hit = checkAttachmentSyncEdge("meshletVisibleFlags");
	REQUIRE(hit.has_value());
	CHECK(*hit == RgSyncEdgeId::meshletVisibleWriteToMesh);
	const auto miss = checkAttachmentSyncEdge("nopeNotInAnyTable");
	REQUIRE_FALSE(miss.has_value());
	CHECK(contains(miss.error().message, "nopeNotInAnyTable"));
	CHECK(contains(miss.error().message, "kAttachmentSyncEdges"));
}

// ═══════════════════════════════════════════════════════════════════════════
// 阳性锚点：meshlet 形态全链路（声明 → 对账 → 重编译 → 零 barrier 条目）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("阳性锚点：meshlet 形态（生产者 p0 + 消费者 p1 + 表行）对账/重编译全绿且零 barrier 条目") {
	registerBuf("meshletVisibleFlags", 4096);  // 真名 = 表行指向的名字
	DynamicRenderGraph graph{twoPassGraph()};
	REQUIRE(errOf(graph.declareBufferProductions("p0", {"meshletVisibleFlags"})).empty());
	REQUIRE(errOf(graph.declareBufferConsumptions("p1", {"meshletVisibleFlags"})).empty());
	// desc 形态：附件追加在**末尾**（slot 稳定红线）+ p0 storageWrite + p1 storageRead。
	REQUIRE(graph.desc().attachments.size() == 3);
	CHECK(graph.desc().attachments[2].name == "meshletVisibleFlags");
	CHECK(graph.desc().attachments[2].kind == RgAttachmentKind::buffer);
	REQUIRE_FALSE(graph.desc().passes[0].outputs.empty());
	CHECK(graph.desc().passes[0].outputs.back().usage == RgUsage::storageWrite);
	REQUIRE_FALSE(graph.desc().passes[1].inputs.empty());
	CHECK(graph.desc().passes[1].inputs.back().usage == RgUsage::storageRead);
	// 对账绿（表行在场）+ 重编译成功（安全点兜底对账同样放行）。
	CHECK(errOf(reconcileAttachmentSyncEdges(graph.desc())).empty());
	const auto compiled = graph.recompile();
	REQUIRE_MESSAGE(compiled.has_value(),
		(compiled ? "" : compiled.error().message).c_str());
	CHECK(graph.recompileCount() == 1u);
	// plan-only（消费者侧）：storageRead 与 storageWrite 同样**不进** barrier 计划——
	// 「声明进图」对命令流是中性的，这是五条逐位锚不动的单元面依据。
	const RgAttachmentSlot bufferSlot = 2;
	uint32_t totalBarriers = 0;
	for (const auto& p : compiled->passes) {
		totalBarriers += static_cast<uint32_t>(p.barriers.size());
		for (const auto& b : p.barriers)
			CHECK_MESSAGE(b.attachment != bufferSlot,
				("buffer 附件出现在 barrier 计划里（pass=" + p.name + "）").c_str());
	}
	// 阳性锚点：同图的 depth/swapchain 必须产 barrier，否则上面的"零条目"是空真。
	CHECK(totalBarriers >= 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// 正向规则：双声明缺表行 = 红（变异 2/4 的靶子）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("正向：buffer 附件双声明却缺表行 → 对账红并点名附件 + 两侧 pass + 表名") {
	registerBuf("attSyncFake", 256);
	RgGraphDesc d = twoPassGraph();
	d.attachments.push_back(
		RgAttachmentDesc{.name = "attSyncFake", .kind = RgAttachmentKind::buffer});
	// 🔴 指定初始化（债-118：跨侧布局 struct 不许位置聚合）
	d.passes[0].outputs.push_back(
		RgPassAttachment{.attachment = "attSyncFake", .usage = RgUsage::storageWrite});
	d.passes[1].inputs.push_back(
		RgPassAttachment{.attachment = "attSyncFake", .usage = RgUsage::storageRead});
	const auto msg = errOf(reconcileAttachmentSyncEdges(d));
	REQUIRE_FALSE(msg.empty());
	CHECK(contains(msg, "attSyncFake"));
	CHECK(contains(msg, "p0"));  // 生产者 pass 名
	CHECK(contains(msg, "p1"));  // 消费者 pass 名
	CHECK(contains(msg, "kAttachmentSyncEdges"));
	// IZB 合法形态的反证：producer-only + 无表行 = 绿（正向的双声明条件不成立、
	// 反向无行可查）。删掉这条，"正向规则误伤 producer-only"就没人红。
	DynamicRenderGraph izb{twoPassGraph()};
	REQUIRE(izb.declareBufferProductions("p0", {"attSyncFake"}).has_value());
	CHECK(errOf(reconcileAttachmentSyncEdges(izb.desc())).empty());
}

TEST_CASE("正向（API 面）：消费者声明缺边 → Error 不落账不标脏（变异 4 的靶子）") {
	registerBuf("attSyncFake2", 256);
	DynamicRenderGraph graph{twoPassGraph()};
	// 生产者声明**不**跑对账（producer-only 是合法中间形态，IZB 就住在这里）。
	REQUIRE(graph.declareBufferProductions("p0", {"attSyncFake2"}).has_value());
	const size_t inputsBefore = graph.desc().passes[1].inputs.size();
	const auto bad = errOf(graph.declareBufferConsumptions("p1", {"attSyncFake2"}));
	REQUIRE_FALSE(bad.empty());
	CHECK(contains(bad, "attSyncFake2"));
	// 失败不落账：inputs 未追加（脏位是生产者声明置的，不归本步管）。
	CHECK(graph.desc().passes[1].inputs.size() == inputsBefore);
}

// ═══════════════════════════════════════════════════════════════════════════
// 反向规则：表行在图里却半申报 = 红；不在图里 = 休眠（变异 3/5/7 的靶子）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("反向：表行 + producer-only 半申报 → 对账红、recompile 红且脏位保留；不在图里 = 休眠") {
	registerBuf("meshletVisibleFlags", 2048);
	DynamicRenderGraph graph{twoPassGraph()};
	REQUIRE(graph.declareBufferProductions("p0", {"meshletVisibleFlags"}).has_value());
	// 表行说「图里有一处读」而图看不见它 = 两个来源各说各话。
	const auto msg = errOf(reconcileAttachmentSyncEdges(graph.desc()));
	REQUIRE_FALSE(msg.empty());
	CHECK(contains(msg, "meshletVisibleFlags"));
	CHECK(contains(msg, "storageRead"));  // 点名缺哪一侧
	CHECK(contains(msg, "meshlet_visible_write_to_mesh"));  // 点名申报的边
	// 安全点兜底（变异 5）：半申报的脏图在 recompile 里红，脏位保留（重试的凭据）。
	const auto recompiled = graph.recompile();
	REQUIRE_FALSE(recompiled.has_value());
	CHECK(graph.isDirty());
	CHECK(graph.recompileCount() == 0u);
	// 休眠半边：附件不在 desc 里（门关档 / 服务未装配）= 绿——出厂档判据。
	CHECK(errOf(reconcileAttachmentSyncEdges(twoPassGraph())).empty());
}

TEST_CASE("反向（手搭形态）：只有读 / 两侧都缺 → 消息分别点名缺的那一侧") {
	registerBuf("meshletVisibleFlags", 512);
	// 只有 storageRead（绕开 API 手搭——API 面上消费者要求生产者先行，这种形态进不来，
	// 但 desc 是真相源，对账必须认得它）。
	RgGraphDesc readOnly = twoPassGraph();
	readOnly.attachments.push_back(RgAttachmentDesc{
		.name = "meshletVisibleFlags", .kind = RgAttachmentKind::buffer});
	readOnly.passes[1].inputs.push_back(RgPassAttachment{
		.attachment = "meshletVisibleFlags", .usage = RgUsage::storageRead});
	const auto msgRead = errOf(reconcileAttachmentSyncEdges(readOnly));
	REQUIRE_FALSE(msgRead.empty());
	CHECK(contains(msgRead, "storageWrite"));  // 缺的是生产者侧
	// 两侧都缺（附件躺在 desc 里没人引用）。
	RgGraphDesc orphan = twoPassGraph();
	orphan.attachments.push_back(RgAttachmentDesc{
		.name = "meshletVisibleFlags", .kind = RgAttachmentKind::buffer});
	const auto msgOrphan = errOf(reconcileAttachmentSyncEdges(orphan));
	REQUIRE_FALSE(msgOrphan.empty());
	CHECK(contains(msgOrphan, "两侧声明都缺"));
}

// ═══════════════════════════════════════════════════════════════════════════
// 构造期对账：启动即抛（变异 6 的靶子）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("构造对账：初始 desc 双声明缺边 → 构造抛；闭合形态不抛") {
	registerBuf("attSyncCtor", 128);
	RgGraphDesc bad = twoPassGraph();
	bad.attachments.push_back(
		RgAttachmentDesc{.name = "attSyncCtor", .kind = RgAttachmentKind::buffer});
	bad.passes[0].outputs.push_back(
		RgPassAttachment{.attachment = "attSyncCtor", .usage = RgUsage::storageWrite});
	bad.passes[1].inputs.push_back(
		RgPassAttachment{.attachment = "attSyncCtor", .usage = RgUsage::storageRead});
	CHECK_THROWS_AS(DynamicRenderGraph{std::move(bad)}, std::runtime_error);
	// 正向的一半：闭合形态（真名 + 表行）不抛——否则「构造抛」会因为「构造恒抛」假绿。
	registerBuf("meshletVisibleFlags", 256);
	RgGraphDesc good = twoPassGraph();
	good.attachments.push_back(RgAttachmentDesc{
		.name = "meshletVisibleFlags", .kind = RgAttachmentKind::buffer});
	good.passes[0].outputs.push_back(RgPassAttachment{
		.attachment = "meshletVisibleFlags", .usage = RgUsage::storageWrite});
	good.passes[1].inputs.push_back(RgPassAttachment{
		.attachment = "meshletVisibleFlags", .usage = RgUsage::storageRead});
	CHECK_NOTHROW(DynamicRenderGraph{std::move(good)});
}

// ═══════════════════════════════════════════════════════════════════════════
// declareBufferConsumptions 语义（变异 8/9 的靶子 + fail-fast 两形态）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("declareBufferConsumptions：生产者先行指路 / pass 名错 / 幂等 / feedback 与读前置拒") {
	registerBuf("attSyncSem", 64);
	// ① 附件不在图中 → Error 且消息指路 declareBufferProductions（变异 8：去掉检查后
	//    消息变成试编译的注册缺失，不再指路）。
	DynamicRenderGraph g0{twoPassGraph()};
	const auto e1 = errOf(g0.declareBufferConsumptions("p1", {"attSyncSem"}));
	REQUIRE_FALSE(e1.empty());
	CHECK(contains(e1, "declareBufferProductions"));
	CHECK_FALSE(g0.isDirty());
	// ② 正常链路（真名，对账放行）：inputs 追加 storageRead + 标脏。
	registerBuf("meshletVisibleFlags", 1024);
	DynamicRenderGraph g{twoPassGraph()};
	REQUIRE(g.declareBufferProductions("p0", {"meshletVisibleFlags"}).has_value());
	REQUIRE(errOf(g.declareBufferConsumptions("no_such_pass", {"meshletVisibleFlags"}))
		.find("no_such_pass") != std::string::npos);  // pass 名错点名传入的名字
	CHECK(g.desc().passes[1].inputs.empty());
	REQUIRE(errOf(g.declareBufferConsumptions("p1", {"meshletVisibleFlags"})).empty());
	CHECK(g.isDirty());
	REQUIRE(g.desc().passes[1].inputs.size() == 1);
	CHECK(g.desc().passes[1].inputs[0].usage == RgUsage::storageRead);
	// ③ 幂等：再声明一次 = 空操作（inputs 不重复追加）。
	REQUIRE(errOf(g.declareBufferConsumptions("p1", {"meshletVisibleFlags"})).empty());
	CHECK(g.desc().passes[1].inputs.size() == 1);
	// ④ 同 pass 读写 feedback：消费者 = 生产者 pass → 试编译拒（countRole），不落账。
	DynamicRenderGraph g2{twoPassGraph()};
	REQUIRE(g2.declareBufferProductions("p0", {"meshletVisibleFlags"}).has_value());
	REQUIRE_FALSE(g2.declareBufferConsumptions("p0", {"meshletVisibleFlags"}).has_value());
	CHECK(g2.desc().passes[0].inputs.empty());
	// ⑤ 消费排在生产之前（「谎报方向」的图侧形态）：生产者 p1、消费者 p0 → 读前置拒。
	DynamicRenderGraph g3{twoPassGraph()};
	REQUIRE(g3.declareBufferProductions("p1", {"meshletVisibleFlags"}).has_value());
	const auto early = errOf(g3.declareBufferConsumptions("p0", {"meshletVisibleFlags"}));
	REQUIRE_FALSE(early.empty());
	CHECK(contains(early, "试编译失败"));
	CHECK(g3.desc().passes[0].inputs.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// EVSSM 裁决实测锚点：sampleRead 进布局链、storage 档不进（同一张图钉两侧）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("EVSSM 裁决实测锚点：sampleRead 产 barrier 条目，storage 档零条目（同图不对称）") {
	// 「EVSSM 矩图不迁入图」不是偏好，是这条实测不对称的后果：矩图是 always-GENERAL 的
	// RT 缓存图，而 sampleRead 会让**图**替它产布局转换条目（破 GENERAL 契约 = VVL
	// mismatch，且逐位锚必动）。左半边钉住这个事实；右半边钉住 meshlet 消费者声明的
	// 命令流中性。同图执行 ⟹ 左边的"有"让右边的"没有"不可能是空真。
	registerBuf("attSyncEvssm", 128);
	RgGraphDesc d{
		.attachments = {
			RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
			RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
			RgAttachmentDesc{.name = "mips_like", .kind = RgAttachmentKind::offscreen,
				.format = std::string{kSceneHdrFormat}},
			RgAttachmentDesc{.name = "attSyncEvssm", .kind = RgAttachmentKind::buffer},
		},
		.passes = {
			RgPassDesc{.name = "writer", .kind = RgPassKind::drawMesh, .state = "opaque",
				.drawSource = "opaque",
				.outputs = {
					RgPassAttachment{.attachment = "depth", .usage = RgUsage::depthWrite},
					RgPassAttachment{.attachment = "mips_like", .usage = RgUsage::colorWrite},
					RgPassAttachment{.attachment = "attSyncEvssm",
						.usage = RgUsage::storageWrite}}},
			RgPassDesc{.name = "reader", .kind = RgPassKind::drawMesh, .state = "opaque",
				.drawSource = "opaque",
				.outputs = {RgPassAttachment{.attachment = "swapchain",
					.usage = RgUsage::colorWrite}},
				.inputs = {
					RgPassAttachment{.attachment = "mips_like", .usage = RgUsage::sampleRead},
					RgPassAttachment{.attachment = "attSyncEvssm",
						.usage = RgUsage::storageRead}}},
		},
	};
	const auto compiled = compileRenderGraph(d);
	REQUIRE_MESSAGE(compiled.has_value(),
		(compiled ? "" : compiled.error().message).c_str());
	const RgAttachmentSlot imageSlot = 2;  // mips_like（offscreen）
	const RgAttachmentSlot bufferSlot = 3;
	// 左半边（EVSSM 的处境）：sampleRead 的 image 附件**必须**出现在 barrier 计划里。
	bool imageInBarriers = false;
	for (const auto& p : compiled->passes)
		for (const auto& b : p.barriers)
			if (b.attachment == imageSlot)
				imageInBarriers = true;
	CHECK_MESSAGE(imageInBarriers,
		"sampleRead 的 image 附件没进 barrier 计划——EVSSM 裁决的实测依据失效，重读 usageInfo");
	// 右半边（meshlet 的处境）：storage 档的 buffer 附件**必须不**出现在任何 barrier 计划。
	for (const auto& p : compiled->passes)
		for (const auto& b : p.barriers)
			CHECK_MESSAGE(b.attachment != bufferSlot,
				("buffer 附件出现在 barrier 计划里（pass=" + p.name + "）——plan-only 结构性护栏被拆").c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
// carryOver「新增附件首现」维度：buffer 新名与 RT 新名同走按名结转
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("carryOver：buffer 附件首现拿默认值（与 RT 新名同一机制，无特判）") {
	// 重编译后 meshletVisibleFlags 是逐 slot 表里的**新名**：布局表按名结转给它默认值。
	// buffer 档结构性不进布局链（EVSSM 探针钉的右半边）⟹ 这个槽的值永远不会被读，
	// 但「新名 = 默认值」不许有 kind 特判——特判 = 下一个 kind 来了没人记得。
	const std::vector<std::string> oldNames = {"swapchain", "depth"};
	const std::vector<uint32_t> oldValues = {11u, 22u};
	const std::vector<std::string> newNames = {"swapchain", "depth", "meshletVisibleFlags"};
	const auto carried = carryOverPerSlot(oldNames, oldValues, newNames, uint32_t{0});
	REQUIRE(carried.size() == 3);
	CHECK(carried[0] == 11u);
	CHECK(carried[1] == 22u);
	CHECK(carried[2] == 0u);  // 新名 = 默认值（重置与结转的差别在旧名，不在新名）
}

}  // namespace RGL
