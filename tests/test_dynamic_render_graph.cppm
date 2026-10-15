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
 *
 * 以下 11-20 属批次 B2-5（pass 级动态化）。这一档的失效形态不是「没插进去」，而是「插进去
 * 又撤出来，撤出来的那张图悄悄少了/多了东西」，所以变异集中在**撤回侧**：
 *
 *  11. declarePass 去掉幂等（已在图里还再插一份）→ 用例 4 ② 红（第二次申报变成 Error）。
 *  12. withdrawPass 去掉撤回幂等（删 `if (!record->inserted) return {}`）→ 用例 4 ④ 红
 *      （复合事务重放同一意图就炸 = 开关第二次点下去必崩）。
 *  13. withdrawPass 未登记改成静默成功 → 用例 4 ⑤ 红（「以为关了其实没关」，最坏失效方向）。
 *  14. 撤回把 storeOp 还原成常量 store → 用例 5 红（原值是 dontCare，等于根本没还原）。
 *  15. 撤回时顺手 erase 附件（append-only 破口）→ 六条撤回用例全红：附件都还有别的读者，
 *      于是**试编译当场拒**（withdrawPass 返回 Error）。attNamesOf 是这条的**直接读法**，
 *      两层各守一半：哪天被删的附件恰好没人读，试编译不拦，只剩它出声。
 *  16. 撤回把 storeOp 还原成常量 dontCare → **只有用例 7 红**。为什么只有它：装饰器只在
 *      「前写者不是 store」时发补丁 ⟹ 生产可达的补丁原值恒为 dontCare，常量还原在那一档
 *      **恰好也对** = 等价变异（杀不掉不是判据坏，是区分两种写法的输入在生产里不存在）。
 *      用例 7 用一张生产发不出、图却编译得过的补丁（原值 store → 新值 dontCare 且无人再读）
 *      把这种输入造出来。教训：「两种写法等价」要用**能不能构造出区分输入**来判，别靠读代码觉得像。
 *  17. declarePass 接受「已在册但已撤回」的新 desc（不指路 redeclarePass）→ 用例 9 ③ 红。
 *      这条守的是 A→B→A′ 判据**不被自证**：两边都吃调用方现拼的 desc，则两边都拼错也读成往返一致。
 *  18. adoptPass 只查 pass 在场、不查补丁已生效 → 用例 8 ④ 红（登记一张与现实不符的申报单，
 *      撤回时把图改成谁都没申报过的样子）。
 *  19. pass 级申报去掉试编译 fail-fast（失败也落账）→ 用例 10 红（编译不过的图进了在册图，
 *      拖到下个安全点才出声，且脏位已被污染）。
 *  20. recompile 不计时（lastRecompileNs 恒 0）→ 用例 11 红。这条看着 trivial，但它是
 *      「逐帧 = 测试模态而非 SLA」那条裁决**能被读出来**的唯一前提：读数恒 0 的曲线是编造的。
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

// ═══════════════════════════════════════════════════════════════════════════
// 用例 4-8：pass 级动态插拔（批次 B2-5）
//
// 这一档与 buffer 声明的**根本差别**：它改的是 pass 序列本身，还带跨 pass 补丁。
// 于是判据的重心从「追加位置对不对」挪到「**往返之后图还是不是原来那张**」——
// 半吊子形态不是"没插进去"，而是"插进去又撤出来，撤出来的那张图悄悄少了/多了东西"。
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/** highlight 那条 pass 本体（启动期装饰与动态申报共用同一份形状）。 */
RgPassDesc highlightPass() {
	return RgPassDesc {
		.name = "highlight", .kind = RgPassKind::drawMesh, .state = "highlight",
		.drawSource = "opaque",
		.outputs = {
			RgPassAttachment { .attachment = "scene_hdr", .usage = RgUsage::colorWrite,
				.loadOp = RgLoadOp::load, .storeOp = RgStoreOp::store },
			RgPassAttachment { .attachment = "depth", .usage = RgUsage::depthWrite,
				.loadOp = RgLoadOp::load, .storeOp = RgStoreOp::store }
		}
	};
}

/**
 * 一张能跑 highlight 装饰器的最小图（照 deferred 图缩到装饰器要读的那几个角色）：
 * geometry 写 depth → light 写 scene_hdr → tonemap **采样** scene_hdr（= 首个颜色消费者）。
 *
 * `depthStoreOp` 刻意做成参数：deferred/forward 内建图里 geometry 的 depth 本来就是 `store`
 * （补丁不该发），offscreen 变体的 transparent 段才是 `dontCare`（补丁必须发）。
 * 两种都要有，否则「还原成常量 dontCare」这种写法在其中一种图上**恰好也对** = 判据抓不到。
 *
 * `withHighlight` = 模拟**启动期装饰已经落图**（Renderer 装配期的真实形态）：pass 已在图里，
 * 但还没进动态册——adoptPass 的口径正是"承认这张已在图里的 pass 可撤"，所以它必须能在
 * "在场但未登记"的图上被调用，也必须能在"在场而补丁没生效"的图上报错。
 */
RgGraphDesc highlightHost(RgStoreOp depthStoreOp = RgStoreOp::store, bool withHighlight = false) {
	auto desc = RgGraphDesc{
		.attachments = {
			RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
			RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
			RgAttachmentDesc{.name = "scene_hdr", .kind = RgAttachmentKind::offscreen,
				.format = "rgba16_sfloat"},
		},
		.passes = {
			RgPassDesc{.name = "geometry", .kind = RgPassKind::drawMesh, .state = "gbuffer",
				.drawSource = "opaque", .outputs = {RgPassAttachment{.attachment = "depth",
					.usage = RgUsage::depthWrite, .loadOp = RgLoadOp::clear,
					.storeOp = depthStoreOp}}},
			RgPassDesc{.name = "light", .kind = RgPassKind::drawLight, .state = "light",
				.outputs = {RgPassAttachment{.attachment = "scene_hdr", .usage = RgUsage::colorWrite,
					.loadOp = RgLoadOp::clear, .storeOp = RgStoreOp::store}}},
			RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
				.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite,
					.loadOp = RgLoadOp::clear, .storeOp = RgStoreOp::store}},
				.inputs = {RgPassAttachment{.attachment = "scene_hdr",
					.usage = RgUsage::sampleRead}}},
		},
	};
	if (withHighlight)
		desc.passes.insert(desc.passes.begin() + 2, highlightPass());  // 排在 tonemap 之前
	return desc;
}

/** pass 名序列（往返判据的读法：顺序 + 有无，一次看全）。 */
std::vector<std::string> passNamesOf(const RgGraphDesc& desc) {
	std::vector<std::string> names;
	for (const auto& pass : desc.passes)
		names.push_back(pass.name);
	return names;
}

/** 附件名序列（append-only 红线的读法：撤回只摘 pass，不得顺手删附件）。 */
std::vector<std::string> attNamesOf(const RgGraphDesc& desc) {
	std::vector<std::string> names;
	for (const auto& att : desc.attachments)
		names.push_back(att.name);
	return names;
}

/** 某 pass 某 attachment 的 storeOp（补丁还原断言的读法）。 */
RgStoreOp storeOpOf(const RgGraphDesc& desc, std::string_view passName, std::string_view att) {
	for (const auto& pass : desc.passes) {
		if (pass.name != passName)
			continue;
		for (const auto& out : pass.outputs)
			if (out.attachment == att)
				return out.storeOp;
	}
	return RgStoreOp::dontCare;  // 不可达（用例只查存在的）
}

/** 一张 highlight 申报单（= 启动期装饰的产物；这里手搓，与 planHighlightDecoration 同形态）。 */
RgPassDeclaration highlightDecl(std::string insertBefore = "tonemap",
	std::vector<RgStoreOpPatch> patches = {}) {
	return RgPassDeclaration {
		.pass = highlightPass(),
		.insertBefore = std::move(insertBefore),
		.patches = std::move(patches),
	};
}

}  // namespace

TEST_CASE("declarePass：按名字插到中段 + 幂等 + 插入点失效拒落；withdrawPass 摘掉并还原补丁") {
	// ① 正向：插到 tonemap **之前**（不是图末）——中段插入 = 后续 pass 下标全体 +1，
	//    这正是相位表必须重推的形态（对应用例见 test_offgraph_contract）。
	DynamicRenderGraph graph{highlightHost()};
	REQUIRE(errOf(graph.declarePass(highlightDecl())).empty());
	CHECK(graph.isDirty());  // 标脏 = 下个安全点重编译（≤1 帧延迟）
	CHECK(passNamesOf(graph.desc()) == std::vector<std::string> {"geometry", "light", "highlight", "tonemap"});
	CHECK(graph.hasPass("highlight"));
	CHECK(graph.isDynamic("highlight"));
	CHECK(graph.dynamicPassNames() == std::vector<std::string> {"highlight"});

	// ② 幂等（变异 11：去掉幂等 → 这里 desc 变两个 highlight）：再申报一次 = 空操作。
	REQUIRE(errOf(graph.declarePass(highlightDecl())).empty());
	CHECK(graph.desc().passes.size() == 4);

	// ③ 撤回：pass 摘掉 + 标脏；**附件不跟着撤**（append-only 红线）。
	REQUIRE(errOf(graph.withdrawPass("highlight")).empty());
	CHECK(passNamesOf(graph.desc()) == std::vector<std::string> {"geometry", "light", "tonemap"});
	CHECK(graph.desc().attachments.size() == 3);
	CHECK_FALSE(graph.hasPass("highlight"));
	CHECK(graph.isDynamic("highlight"));  // 撤回 ≠ 出册（重插要吃在册那份）

	// ④ 撤回幂等（变异 12：去掉 `if (!record->inserted) return {}` → 第二次撤回变 Error，
	//    复合事务重放同一意图就炸）。
	REQUIRE(errOf(graph.withdrawPass("highlight")).empty());

	// ⑤ 🔴 不在册的两种名字都必须 Error（变异 13：静默成功 = 以为关了其实没关）：
	//    (a) 图里有这个名字但它是启动期 pass；(b) 图里根本没有 = 拼错。
	const auto startup = errOf(graph.withdrawPass("light"));
	REQUIRE_FALSE(startup.empty());
	CHECK(startup.find("light") != std::string::npos);
	CHECK(startup.find("启动期") != std::string::npos);
	const auto typo = errOf(graph.withdrawPass("hilight"));  // 少一个 g
	REQUIRE_FALSE(typo.empty());
	CHECK(typo.find("拼错") != std::string::npos);
}

TEST_CASE("跨 pass 补丁：只在需要时改，撤回还原成**申报的原值**（不是常量 dontCare）") {
	// 场景 = offscreen 变体的 transparent 段：geometry 的 depth 申报 dontCare，
	// highlight 要 load 它 ⟹ 补丁必须把它改成 store；撤 highlight 时必须还原成 **dontCare**。
	// 🔴 这条是「还原成常量 store」与「还原成常量 dontCare」两种写法共同的靶子：
	// 前者在本用例红（没还原），后者在下一个用例（原值本是 store）红（多还原一步）。
	DynamicRenderGraph graph{highlightHost(RgStoreOp::dontCare)};
	const auto decl = highlightDecl("tonemap", {RgStoreOpPatch {
		.passName = "geometry", .attachment = "depth", .usage = RgUsage::depthWrite,
		.newStoreOp = RgStoreOp::store, .originalStoreOp = RgStoreOp::dontCare }});
	REQUIRE(errOf(graph.declarePass(decl)).empty());
	CHECK(storeOpOf(graph.desc(), "geometry", "depth") == RgStoreOp::store);  // 补丁生效

	REQUIRE(errOf(graph.withdrawPass("highlight")).empty());
	CHECK(storeOpOf(graph.desc(), "geometry", "depth") == RgStoreOp::dontCare);  // 还原成原值

	// 重插 → 补丁再生效；再撤 → 再还原。**两次**才叫往返（一次可能是碰巧）。
	REQUIRE(errOf(graph.redeclarePass("highlight")).empty());
	CHECK(storeOpOf(graph.desc(), "geometry", "depth") == RgStoreOp::store);
	REQUIRE(errOf(graph.withdrawPass("highlight")).empty());
	CHECK(storeOpOf(graph.desc(), "geometry", "depth") == RgStoreOp::dontCare);
}

TEST_CASE("A→B→A′ 往返：pass 序列与 storeOp 逐字段回到 A（本批的杀手判据在单元层的形态）") {
	// 原值 = store 的档位（deferred / forward 内建图的实际形态）。
	// 这一档抓的是**反向**的写法错误：把撤回写成"还原成 dontCare"，在上一用例（原值
	// dontCare）会假绿，在这里红——两个档位缺一个，判据就是半瞎的。
	DynamicRenderGraph graph{highlightHost(RgStoreOp::store)};
	const std::vector<std::string> passA = passNamesOf(graph.desc());
	const std::vector<std::string> attA = attNamesOf(graph.desc());
	REQUIRE(passA == std::vector<std::string> {"geometry", "light", "tonemap"});

	REQUIRE(errOf(graph.declarePass(highlightDecl())).empty());
	CHECK(passNamesOf(graph.desc()) != passA);  // B ≠ A（开关真的动了图）
	REQUIRE(errOf(graph.withdrawPass("highlight")).empty());

	// 🔴 A′ == A：pass 名序列 + 每条 output 的 storeOp 全量比。
	CHECK(passNamesOf(graph.desc()) == passA);
	CHECK(storeOpOf(graph.desc(), "geometry", "depth") == RgStoreOp::store);
	CHECK(graph.desc().passes[0].outputs.size() == 1);  // 没多出残留 usage
	CHECK(graph.desc().passes.size() == 3);
	// 附件序列原样：highlight 用的 scene_hdr/depth 本来就是启动期附件，插拔都不该动这张表
	// （append-only 红线 = 撤回侧连"顺手清理"都不许做，动了就砸别人正在吃的 slot）
	CHECK(attNamesOf(graph.desc()) == attA);
}

TEST_CASE("撤回还原的是**申报值**而不是任何常量：原值 store → 新值 dontCare 的补丁") {
	// 🔴 这条是变异「撤回还原成常量 dontCare」唯一的杀手，来历要写清：生产**发不出**这种补丁
	// （装饰器只在前写者不是 store 时发补丁 ⟹ 原值恒为 dontCare），于是"还原成常量 dontCare"
	// 在生产可达的每一档上都**恰好也对** = 那条变异在生产形态下不可杀（等价变异，负知识见
	// docs/compute_调研.md §7.20）。但 API 契约写的是"还原成申报单上的 originalStoreOp"，
	// 所以这里手搓一张生产不发得出、图却编译得过的补丁：geometry 的 depth 从 store 改成
	// dontCare，且**没人再读它**（highlight 不输出 depth）⟹ checkPriorWriteStore 不触发。
	DynamicRenderGraph graph{highlightHost(RgStoreOp::store)};
	auto decl = highlightDecl("tonemap", {RgStoreOpPatch {
		.passName = "geometry", .attachment = "depth", .usage = RgUsage::depthWrite,
		.newStoreOp = RgStoreOp::dontCare, .originalStoreOp = RgStoreOp::store }});
	decl.pass.outputs.pop_back();  // 去掉 depth 输出：depth 从此无人读，图才编译得过
	REQUIRE(errOf(graph.declarePass(decl)).empty());
	CHECK(storeOpOf(graph.desc(), "geometry", "depth") == RgStoreOp::dontCare);  // 补丁生效

	REQUIRE(errOf(graph.withdrawPass("highlight")).empty());
	// 🔴 回到 **store**（= 申报的原值）。任何"还原成常量"的实现在这里红。
	CHECK(storeOpOf(graph.desc(), "geometry", "depth") == RgStoreOp::store);
	CHECK(passNamesOf(graph.desc()) == std::vector<std::string> {"geometry", "light", "tonemap"});
}

TEST_CASE("adoptPass：启动期装饰的登记口径（在场 + 补丁已生效才收）") {
	// 生产形态 = Renderer 装配期：装饰器已把 highlight 落图（含跨 pass 补丁），adoptPass 只是
	// 承认它"可撤"。所以正向必须从**已在图里但未登记**的宿主起步——若先 declarePass，它自己
	// 就登记了，adoptPass 对已在册者幂等直接返回，判据什么也没测到（这条是写用例时踩过的）。
	const auto decl = RgPassDeclaration {
		.pass = highlightPass(),
		.insertBefore = "tonemap",
		.patches = {RgStoreOpPatch {
			.passName = "geometry", .attachment = "depth", .usage = RgUsage::depthWrite,
			.newStoreOp = RgStoreOp::store, .originalStoreOp = RgStoreOp::dontCare }}
	};
	// 宿主 = 装饰已落图：highlight 在场 + geometry.depth 已被补丁改成 store
	DynamicRenderGraph graph{highlightHost(RgStoreOp::store, /*withHighlight=*/true)};

	// ① 正向：登记成功，且**不改 desc、不标脏**（登记 ≠ 再插一次）。
	REQUIRE(errOf(graph.adoptPass(decl)).empty());
	CHECK(graph.isDynamic("highlight"));
	CHECK_FALSE(graph.isDirty());
	CHECK(passNamesOf(graph.desc())
		== std::vector<std::string> {"geometry", "light", "highlight", "tonemap"});

	// ② 登记之后才谈得上撤：撤回把补丁还原成**申报的原值** dontCare，重插再回 store。
	//    这就是 picking 门关档时 highlight 的真实往返（启动期装饰版，Renderer 走的那条）。
	REQUIRE(errOf(graph.withdrawPass("highlight")).empty());
	CHECK(storeOpOf(graph.desc(), "geometry", "depth") == RgStoreOp::dontCare);
	CHECK(passNamesOf(graph.desc()) == std::vector<std::string> {"geometry", "light", "tonemap"});
	REQUIRE(errOf(graph.redeclarePass("highlight")).empty());
	CHECK(storeOpOf(graph.desc(), "geometry", "depth") == RgStoreOp::store);
	CHECK(errOf(graph.adoptPass(decl)).empty());  // 幂等：已在册 = 空操作

	// ③ 反向：pass 不在图里 → adopt 必须 Error（adopt 的口径是"它已经在了"）。
	DynamicRenderGraph g2{highlightHost()};
	const auto absent = errOf(g2.adoptPass(decl));
	REQUIRE_FALSE(absent.empty());
	CHECK(absent.find("highlight") != std::string::npos);
	CHECK_FALSE(g2.isDynamic("highlight"));  // 拒绝无副作用

	// ④ 反向：pass 在场但补丁**没生效**（图上当前值 ≠ 申报的 newStoreOp）→ Error。
	//    这条抓的是"装饰与登记之间有人改了图"：登记一张与现实不符的申报单，撤回时就会
	//    把图改成一个谁都没申报过的样子。
	DynamicRenderGraph g3{highlightHost(RgStoreOp::dontCare, /*withHighlight=*/true)};
	const auto lying = errOf(g3.adoptPass(decl));
	REQUIRE_FALSE(lying.empty());
	// 消息必须点名"补丁与图不一致"以及现在到底是什么值（归因成本决定它会不会被修）
	CHECK(lying.find("补丁与图不一致") != std::string::npos);
	CHECK(lying.find("现在是 dontCare") != std::string::npos);
	CHECK_FALSE(g3.isDynamic("highlight"));
}

TEST_CASE("declarePass 反向：撞启动期 pass 名 / 插入点不存在 / 已在册想重插 → 各有指路的 Error") {
	// ① 撞名：图里已有 "light"，再动态插一个同名 pass。
	//    名字是插入点与补丁的**定位键**，撞名 = 两条按名定位的规则指错对象且不报错。
	DynamicRenderGraph graph{highlightHost()};
	DynamicRenderGraph g2{highlightHost()};
	auto clashDecl = highlightDecl();
	clashDecl.pass.name = "light";
	const auto clash2 = errOf(g2.declarePass(clashDecl));
	REQUIRE_FALSE(clash2.empty());
	CHECK(clash2.find("light") != std::string::npos);
	CHECK(g2.desc().passes.size() == 3);  // 拒绝无副作用

	// ② 插入点不存在（图在算申报与落申报之间被改过）。
	const auto gone = errOf(graph.declarePass(highlightDecl("no_such_pass")));
	REQUIRE_FALSE(gone.empty());
	CHECK(gone.find("no_such_pass") != std::string::npos);
	CHECK_FALSE(graph.hasPass("highlight"));  // 失败不落账

	// ③ 已在册但已撤回 → declarePass 拒绝并**指路 redeclarePass**。
	//    🔴 这条是 A→B→A′ 判据不被自证的守卫：若这里接受调用方新拼的 desc，
	//    「两边都拼错」也会被读成往返一致。
	DynamicRenderGraph g3{highlightHost()};
	REQUIRE(errOf(g3.declarePass(highlightDecl())).empty());
	REQUIRE(errOf(g3.withdrawPass("highlight")).empty());
	const auto again = errOf(g3.declarePass(highlightDecl()));
	REQUIRE_FALSE(again.empty());
	CHECK(again.find("redeclarePass") != std::string::npos);

	// ④ redeclarePass 不在册 → Error（首插要走 declarePass 带申报单）。
	DynamicRenderGraph g4{highlightHost()};
	const auto never = errOf(g4.redeclarePass("highlight"));
	REQUIRE_FALSE(never.empty());
	CHECK(never.find("declarePass") != std::string::npos);
}

TEST_CASE("pass 级申报走试编译 fail-fast：插出编译不过的图 = 不落账不标脏") {
	// 构造"能过申报单自检、但编译不过"的形态：output 指向一个**没注册过**的附件名。
	// append-only 注册表那条红线正是靠 trial compile 落地——注册缺失必须在调用现场炸，
	// 而不是把一张编译不过的 desc 留在在册的图里等下个安全点才发现。
	DynamicRenderGraph graph{highlightHost(RgStoreOp::store)};
	auto bogus = highlightDecl();
	bogus.pass.outputs[0].attachment = "scene_hdr_never_registered";
	const auto bad = errOf(graph.declarePass(bogus));
	REQUIRE_FALSE(bad.empty());
	CHECK(bad.find("试编译失败") != std::string::npos);
	// 归因：错误里要能看见是哪个附件名没注册（否则排查要重跑一遍带日志的编译）
	CHECK(bad.find("scene_hdr_never_registered") != std::string::npos);
	CHECK(passNamesOf(graph.desc()) == std::vector<std::string> {"geometry", "light", "tonemap"});
	CHECK_FALSE(graph.isDirty());
	CHECK(graph.recompileCount() == 0u);
	// 且它**没进册**：否则后面想按同名字重插会被"已在册"挡死，变成一个修不好的死账。
	CHECK_FALSE(graph.isDynamic("highlight"));
}

TEST_CASE("recompile 耗时读数：成功重编译后 lastRecompileNs 必须非零（代价可读 = 裁决的前提）") {
	registerBuf("dynBufTiming", 128);
	DynamicRenderGraph graph{minimalGraph()};
	CHECK(graph.lastRecompileNs() == 0u);  // 还没重编译过
	REQUIRE(errOf(graph.declareBufferProductions("opaque", {"dynBufTiming"})).empty());
	REQUIRE(graph.recompile().has_value());
	// 🔴 非零断言：这条看着 trivial，但它是「逐帧 = 测试模态不是 SLA」这条裁决**能被读出来**
	// 的唯一前提——读数恒 0 的话，冒烟台账里的中位数就是一条编造的曲线。
	CHECK(graph.lastRecompileNs() > 0u);
}

}  // namespace RGL
