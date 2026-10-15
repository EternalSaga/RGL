module;
/**
 * test_dynamic_render_graph —— **脏驱动的图派生件**（批次 B2-4③′，D5 第一刀）
 *
 * ## 被测契约（三句话）
 *
 *  1. `carryOverPerSlot`：重编译后逐 slot 跨帧表**按名结转**——同名保旧值、新名拿默认值。
 *     「结转改重置」是本批次点名的变异红线：布局追踪表被重置成 Undefined 后，下帧首条
 *     barrier 拿 Undefined 当 oldLayout 转一个**有内容的**图，VVL 报点还在下一次采样处。
 *  2. `PendingEvictions`（债-124）：逐出必须等 in-flight 用户——同名未到期再次 schedule
 *     必须 Error（正反两半都有用例），到期帧按登记序交还并销账。
 *  3. `DynamicRenderGraph`：附件**只准追加在末尾**（幂等）、试编译 fail-fast（失败不落账
 *     不标脏）、recompile 成功才清脏 + 计数（出厂档判据 = 0 的机器面）。
 *
 * ## 为什么整份文件不进 gpu 套件
 *
 * 三块全是**纯逻辑**（零 VkDevice）——这正是 dynamic_render_graph / pending_evictions
 * 住进库层的判据（"这段逻辑需不需要一个 VkDevice 才能跑"）。图编译本身无设备。
 *
 * ## 变异红线（哪个变异让我红；答不出 = 摆设）
 *
 *   1. carryOver 改成"全部填默认值"（结转→重置）→ 用例 1 的同名保旧值断言红。
 *   2. carryOver 改成"按下标搬"（不查名）→ 用例 1 的换序断言红（slot 空间变了还拿旧位）。
 *   3. PendingEvictions 允许同名重复 schedule → 用例 2 的反向断言红（due 吞账 = 泄漏）。
 *   4. popDue 判据 `<=` 改 `<`（差一）→ 用例 2 的"due 帧当帧必须交"红。
 *   5. popDue 交完不销账 → 用例 2 的"第二次收割必须空"红。
 *   6. declareBufferProductions 去掉幂等 → 用例 3 的二次声明后 desc 尺寸断言红。
 *   7. declareBufferProductions 失败也落账（graphDesc = trial 挪到试编译之前）
 *      → 用例 3 的"失败后 desc 未变 / 脏位未置"红。
 *   8. 附件插到头部（insert 而非 push_back）→ 用例 3 的 slot 稳定断言红
 *      （FrameCapturer/PickingService 构造期缓存 slot，这条就是它们的机器守卫）。
 *   9. recompile 无条件清脏（失败也清）→ 用例 3 的"失败后仍脏"红（脏位 = 重试的凭据）。
 *  10. recompile 不计数 → 用例 3 的计数断言红（出厂档 = 0 / IZB 开档 = 1 的读数面）。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 🔴 modules 坑：initializer_list 不从 import rlstd 带进来
#include <string>
#include <vector>

export module test_dynamic_render_graph;

import rlstd;
import rlvk;
import vkerror;
import render_graph;
import dynamic_render_graph;
import pending_evictions;

namespace RGL {
namespace {

/** 注册一张 buffer 并返回名字（注册表进程级单例 ⟹ 名字逐用例唯一，先例同 test_attachment_buffer）。 */
void registerBuf(std::string_view name, vk::DeviceSize bytes) {
	registerBufferAttachment(name, [bytes] { return bytes; });
}

/** 最小合法图：swapchain + drawMesh 写它（declareBufferProductions 的宿主 desc）。 */
RgGraphDesc minimalGraph(std::string_view passName = "opaque") {
	return RgGraphDesc{
		.attachments = {RgAttachmentDesc{
			.name = "swapchain", .kind = RgAttachmentKind::swapchain}},
		.passes = {RgPassDesc{.name = std::string{passName}, .kind = RgPassKind::drawMesh,
			.state = "opaque", .drawSource = "opaque",
			.outputs = {RgPassAttachment{
				.attachment = "swapchain", .usage = RgUsage::colorWrite}}}},
	};
}

std::string errOf(const std::expected<void, Error>& r) {
	return r.has_value() ? std::string{} : r.error().message;
}

/** 编译图的附件名序列（slot 稳定断言的读法）。 */
std::vector<std::string> namesOf(const CompiledRenderGraph& g) {
	std::vector<std::string> names;
	for (const auto& a : g.attachments)
		names.push_back(a.name);
	return names;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 用例 1：carryOverPerSlot —— 结转不是重置（变异 1/2 的靶子）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("carryOverPerSlot：同名保旧值、新名拿默认、顺序跟新表") {
	const std::vector<std::string> oldNames = {"swapchain", "depth", "scene_hdr"};
	const std::vector<uint32_t> oldValues = {11u, 22u, 33u};

	// 新表 = 旧名全在 + 末尾追加两个新名（B2-4③′ 的常态：附件只准追加在末尾）
	const std::vector<std::string> newNames = {"swapchain", "depth", "scene_hdr",
		"izb_head", "izb_nodes"};
	const auto carried = carryOverPerSlot(oldNames, oldValues, newNames, uint32_t{7});

	// 🔴 判据本体（变异 1）：同名必须等于**旧值**，一个都不能是默认值。
	// 「结转改重置」在这里红 = 交付路径上「下帧首条 barrier 拿错 oldLayout」的红。
	CHECK(carried.size() == 5);
	CHECK(carried[0] == 11u);
	CHECK(carried[1] == 22u);
	CHECK(carried[2] == 33u);
	// 新名 = 默认值（新 RT 首帧布局本来就是 Undefined；默认值不是装饰，是它的语义）。
	CHECK(carried[3] == 7u);
	CHECK(carried[4] == 7u);

	// 变异 2（按下标搬、不查名）的靶子：旧表**换序**后仍按名搬。
	const std::vector<std::string> shuffled = {"scene_hdr", "swapchain", "depth"};
	const auto remap = carryOverPerSlot(oldNames, oldValues, shuffled, uint32_t{0});
	CHECK(remap[0] == 33u);  // scene_hdr
	CHECK(remap[1] == 11u);  // swapchain
	CHECK(remap[2] == 22u);  // depth

	// 旧名在新表消失 = 不搬（新表里没有它的位置，也不许挤进来）
	const std::vector<std::string> shrunk = {"swapchain"};
	const auto dropped = carryOverPerSlot(oldNames, oldValues, shrunk, uint32_t{0});
	REQUIRE(dropped.size() == 1);
	CHECK(dropped[0] == 11u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 用例 2：PendingEvictions —— 「有 in-flight 用户逐出必须炸」（正反两半）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("逐出台账：due 帧交还并销账；同名未到期再次逐出必须 Error") {
	PendingEvictions ledger;
	// 正向：登记 → pending → 未到期不交 → 到期当帧交还（`<=` 判据）→ 销账。
	REQUIRE(ledger.schedule("scene_hdr", 10).has_value());
	CHECK(ledger.pending("scene_hdr"));
	CHECK_FALSE(ledger.pending("depth"));
	CHECK(ledger.popDue(9).empty());  // 差一变异（`<`）在这里红：9 < 10 不该交
	const auto due = ledger.popDue(10);
	REQUIRE(due.size() == 1);
	CHECK(due[0] == "scene_hdr");
	CHECK_FALSE(ledger.pending("scene_hdr"));
	CHECK(ledger.size() == 0);
	// 变异 5（交完不销账）：第二次收割必须空。
	CHECK(ledger.popDue(99).empty());

	// 🔴 反向（变异 3）：同名未到期再次 schedule = Error，且消息点名是谁。
	// 这条不变式抓的是连续 resize：第二次的 due 吞掉第一次的销毁记账 = 旧资源永久泄漏。
	REQUIRE(ledger.schedule("scene_hdr", 20).has_value());
	const auto dup = errOf(ledger.schedule("scene_hdr", 30));
	REQUIRE_FALSE(dup.empty());
	CHECK(dup.find("scene_hdr") != std::string::npos);
	// 拒绝必须**无副作用**：due 仍是 20（30 没把它推后）。
	CHECK(ledger.popDue(19).empty());
	CHECK(ledger.popDue(20).size() == 1);

	// 多项收割按**登记序**（日志/销毁次序可复现，本仓对次序漂零容忍）。
	REQUIRE(ledger.schedule("b", 5).has_value());
	REQUIRE(ledger.schedule("a", 5).has_value());
	const auto both = ledger.popDue(5);
	REQUIRE(both.size() == 2);
	CHECK(both[0] == "b");
	CHECK(both[1] == "a");
}

// ═══════════════════════════════════════════════════════════════════════════
// 用例 3：DynamicRenderGraph —— 追加在末尾 / 幂等 / 试编译 fail-fast / 计数
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("declareBufferProductions：末尾追加 + 幂等 + 失败不落账；recompile 计数与脏位") {
	registerBuf("dynBufA", 128);
	DynamicRenderGraph graph{minimalGraph()};
	CHECK_FALSE(graph.isDirty());
	CHECK(graph.recompileCount() == 0u);

	// 正向：声明 → desc 末尾追加附件 + 目标 pass 追加 storageWrite 输出 + 标脏。
	REQUIRE(errOf(graph.declareBufferProductions("opaque", {"dynBufA"})).empty());
	CHECK(graph.isDirty());
	REQUIRE(graph.desc().attachments.size() == 2);
	// 变异 8（插到头部）的靶子：旧名 slot 必须不动，新名在**末尾**。
	CHECK(graph.desc().attachments[0].name == "swapchain");
	CHECK(graph.desc().attachments[1].name == "dynBufA");
	const auto kind = graph.desc().attachments[1].kind;
	CHECK(kind == RgAttachmentKind::buffer);
	REQUIRE(graph.desc().passes[0].outputs.size() == 2);
	const auto usage = graph.desc().passes[0].outputs[1].usage;
	CHECK(usage == RgUsage::storageWrite);

	// 幂等（变异 6）：再声明一次 = 空操作，desc 不变。
	REQUIRE(errOf(graph.declareBufferProductions("opaque", {"dynBufA"})).empty());
	CHECK(graph.desc().attachments.size() == 2);
	CHECK(graph.desc().passes[0].outputs.size() == 2);

	// recompile 成功：清脏、计数 +1、编译图里确实有那个 buffer 附件（slot 在末尾）。
	const auto compiled = graph.recompile();
	REQUIRE_MESSAGE(compiled.has_value(),
		(compiled ? "" : compiled.error().message).c_str());
	CHECK_FALSE(graph.isDirty());
	CHECK(graph.recompileCount() == 1u);  // 变异 10
	const auto names = namesOf(*compiled);
	REQUIRE(names.size() == 2);
	CHECK(names[0] == "swapchain");
	CHECK(names[1] == "dynBufA");

	// 不脏再 recompile = Error（调用方先查 isDirty；这不是错误路径，出声即可）。
	CHECK_FALSE(graph.recompile().has_value());

	// 🔴 反向一：**未注册**的名字 → Error（试编译 fail-fast），desc 与脏位都不许动。
	// 变异 7（失败也落账）在这里红：落一个编译不过的 desc = 每帧安全点重编译反复失败。
	DynamicRenderGraph g2{minimalGraph()};
	const auto bad = errOf(g2.declareBufferProductions("opaque", {"dynNeverRegistered"}));
	REQUIRE_FALSE(bad.empty());
	CHECK(g2.desc().attachments.size() == 1);
	CHECK_FALSE(g2.isDirty());

	// 反向二：**0 字节**注册（资源还没建就来声明）→ 同样被拒（拒绝面 = 编译期 >0）。
	registerBuf("dynBufZero", 0);
	DynamicRenderGraph g3{minimalGraph()};
	REQUIRE_FALSE(g3.declareBufferProductions("opaque", {"dynBufZero"}).has_value());
	CHECK(g3.desc().attachments.size() == 1);
	CHECK_FALSE(g3.isDirty());

	// 反向三：生产者 pass 名不存在 → Error 且 desc 未变。
	DynamicRenderGraph g4{minimalGraph()};
	REQUIRE_FALSE(g4.declareBufferProductions("no_such_pass", {"dynBufA"}).has_value());
	CHECK(g4.desc().attachments.size() == 1);
}

}  // namespace RGL
