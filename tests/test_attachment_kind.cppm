module;
/**
 * test_attachment_kind —— `RgAttachmentKind` 的分派点必须"加值即必答"
 *
 * ## 被测契约（不是被测函数）
 *
 * `RgAttachmentKind` 现在有五档（第五档 = buffer，批次 B2-3 已落地）。本文件钉的契约只有一条：
 * **再加一个值时，代码里每一个按 kind 分派的位置都必须变成编译错误**，而不是静默
 * 继承某一档的语义。最坏的一种静默是 `aspectOf` 的三元式回落（未知 kind → eColor
 * = 往错的方向走到底，docs/compute_调研.md §7.10 点名条目），本刀把它和同类的
 * `isRtBackedKind` / `isDepthKind` / `frameStartDiscards` 一起改成穷尽 switch，
 * 并把四个 `validateAttachment*` 的 kind 名兜底三元式换成 `kindName`。
 *
 * 🔴 **这条契约今天被机器验了一遍**：buffer 档落地时本文件的数量锚（组 0）先红，
 * 补完全表才绿；`aspect` 那一列也因此从"是否深度面"的布尔改成显式 mask —— 第五档的
 * 正确答案是**空 mask**，布尔表达不出来，而"表达不出来"正是当初没给它留位置的那个洞。
 *
 * ## 可测性结论（先验证后写用例）
 *
 * 三个谓词与 `frameStartDiscards` 住在 **impl partition 的匿名 namespace**
 * （`render_graph_impl.cppm` 的 `namespace { ... }`）：匿名 namespace = 内部链接，
 * 而 `module render_graph:impl;` 是实现分区，外部模块本就不许 import 分区。
 * ⟹ **测试直接调不到它们，也没有为此把 impl 提升成接口**（那是为测试牺牲分层）。
 * 判据改经 `compileRenderGraph` 公开面覆盖——四个量在产物里全部可观测：
 *   - `aspectOf`        → `RgCompiledAttachment::aspect`
 *   - `frameStartDiscards` → `RgCompiledAttachment::frameStartDiscard`
 *   - `isDepthKind`     → `RgAttachmentPlan::clearValue` 取 depth 还是 color 分量
 *                        + `depthWrites non-depth attachment` 拒绝路径
 *   - `isRtBackedKind`  → `validateAttachmentExtent`（独立 extent 只对图属 RT 开放）
 * `kindName` 住在接口（`render_graph.cppm`，导出），既可直接断言，也被四个 validator
 * 的消息消费——两侧都钉：直接断言钉拼写，消息断言钉"归因真的用上了它"。
 *
 * ## 期望值出处（不抄实现返回值）
 *
 * 每条期望值都标了出处，全部来自**被测码之外**：枚举自身的注释
 * （`render_graph.cppm::RgAttachmentKind`）、`RgCompiledAttachment` 的字段注释、
 * Lua 配置词表（`render_graph_config_impl.cppm` 的 `{"swapchain","depth","offscreen"}`）、
 * 以及 validator 消息里已经印了多年的字面量。抄实现返回值 = 同义反复，实现错它也绿。
 *
 * ## 变异红线（哪个变异让我红；答不出 = 摆设）
 *
 *  1. `aspectOf` 的 `depthOffscreen` 分支改成 `eColor` → 组 2 红（aspect 断言）。
 *     这条就是 §7.10 那条静默坑的当前形态：改完**构建全绿、画面不一定错**，
 *     只有本组会红。
 *  2. `kindName` 的 `depth` 分支改成 `"depthOffscreen"` → 组 1 红（直接断言）
 *     **且**组 4 红（extent / format 两条消息里的 kind 名跟着错）。
 *  3. `frameStartDiscards` 的 `offscreen` 分支改成 `true` → 组 3 红。
 *     变异后果 = 持久图帧首 oldLayout 报 eUndefined（内容可丢），TAA 历史隔帧丢失，
 *     而画面第一帧完全正常 ⟹ 截图回归抓不到，只有本组红。
 *  4. `isRtBackedKind` 的 `depthOffscreen` 改成 `false` → 组 5 红（阴影图声明独立
 *     extent 会被误拒）。
 *  5. `isDepthKind` 的 `depth` 改成 `false` → 组 6 红（clearValue 走 color 分量）。
 *  6. 任一 validator 的文案被"顺手改一个字"（含标点/前缀 `render graph:` vs
 *     `render_graph:`）→ 组 4 逐字节断言红。本组存在的**一半理由**就是钉这个。
 *
 * 反向配对（同形态但不该触发）逐条配在组内 —— 否则分不清"这条 guard 没生效"与
 * "这条 guard 坏了"，两者都表现为绿。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import rlstd 带进来
#include <string>
#include <string_view>

export module test_attachment_kind;

import rlstd;
import rlvk;
import render_graph;
import vkerror;  // Error（compileRenderGraph 的 expected 失败侧）

namespace RGL {
namespace {

/**
 * 四种 kind 的**独立描述表**：期望值一列一列写死，出处见各行行尾注释。
 * 写成表而不是散在各 TEST_CASE 里，是为了让"加第五个 kind"这件事在本文件里也只有
 * 一处可改——而组 0 的数量锚会先红，逼着改的人补全整行。
 */
struct KindCase {
	RgAttachmentKind kind;
	std::string_view name;     // kindName 期望值
	std::string_view format;   // 该 kind 的合法格式（避免先撞格式 guard）
	// aspectOf 期望值。**显式写 mask 而不是"是否深度面"的布尔**：第五档（buffer）的正确答案
	// 既不是 eColor 也不是 eDepth 而是**空 mask**（"这不是 image"的记号），布尔表达不出来，
	// 而"表达不出来"正是当初没给它留位置的那个洞——列改成值，加档就被迫当场回答。
	vk::ImageAspectFlags expectAspect;
	bool expectRtBacked;       // isRtBackedKind 期望值
	bool expectDepthKind;      // isDepthKind 期望值
	bool expectFrameStartDiscard;  // frameStartDiscards 期望值
};

const std::vector<KindCase>& kindCases() {
	// 出处（逐列）：
	//   name    ← Lua 词表 {swapchain,depth,offscreen,depthOffscreen,buffer} + validator
	//             消息里已有的字面量 + 枚举成员名（三者互相印证）
	//   format  ← render_graph.cppm::isKnownRgFormat / isKnownDepthRgFormat 的注释词表；
	//             非 RT 两档与 buffer 一律 "inherit"（validateAttachmentFormat 的兜底分支）
	//   aspect  ← RgCompiledAttachment 注释「aspect 按 kind 推导（depth/depthOffscreen →
	//             eDepth，其余 → eColor）」；buffer 档 = 空 mask，出处是它自己的枚举注释
	//             （「没有 aspect —— 那是 image 子资源的概念」）
	//   rtBacked← RgCompiledAttachment::extent 注释「非图属 RT 恒 {0,0}」+ 枚举注释
	//             「offscreen/depthOffscreen = 执行器按名 ensure」；buffer 的资源归**模块**申请
	//   depth   ← RgClearValue 注释「kind=depth 用 depth，其余用 color」+ 枚举注释
	//             「depthOffscreen ② aspect = eDepth」
	//   discard ← RgCompiledAttachment::frameStartDiscard 注释「swapchain/depth/
	//             depthOffscreen = true；offscreen = false 需跨帧追踪布局」；buffer 与
	//             offscreen 同侧（单份 buffer 跨 frames-in-flight 复用，见 MeshletCullService）
	static const std::vector<KindCase> kCases = {
		{RgAttachmentKind::swapchain, "swapchain", "inherit",
			vk::ImageAspectFlagBits::eColor, false, false, true},
		{RgAttachmentKind::depth, "depth", "inherit", vk::ImageAspectFlagBits::eDepth, false,
			true, true},
		{RgAttachmentKind::offscreen, "offscreen", "rgba8_unorm",
			vk::ImageAspectFlagBits::eColor, true, false, false},
		{RgAttachmentKind::depthOffscreen, "depthOffscreen", "d32_sfloat",
			vk::ImageAspectFlagBits::eDepth, true, true, true},
		{RgAttachmentKind::buffer, "buffer", "inherit", vk::ImageAspectFlags{}, false, false,
			false},
	};
	return kCases;
}

/**
 * 把 buffer 档要用到的附件名登记进按名注册表（批次 B2-3 的拒绝面）。
 *
 * 🔴 **必须在编译图之前跑**，且**每个用到的名字都要**：`kind=buffer` 的名字没登记就直接拒，
 * 于是本文件里那些"遍历五档"的用例会红在注册而不是红在它们真正要测的那一列。注册表是进程级
 * 单例，所以这里用 `static` 幂等一次即可；名字 "att"/"x" 是其它用例复用的通用名，登记它们
 * **不影响**那些用例（非 buffer 档根本不查注册表）。
 */
void registerBufferAttachmentsForCases() {
	static const bool done = [] {
		for (const auto& c : kindCases())
			if (c.kind == RgAttachmentKind::buffer)
				registerBufferAttachment(c.name, [] { return vk::DeviceSize{256}; });
		registerBufferAttachment("att", [] { return vk::DeviceSize{256}; });
		registerBufferAttachment("x", [] { return vk::DeviceSize{256}; });
		return true;
	}();
	(void)done;
}

/** 只含一个附件、零 pass 的图：注册期校验最先开火，测 validator 消息不受 pass 规则干扰。 */
RgGraphDesc singleAttGraph(RgAttachmentDesc att) {
	RgGraphDesc d;
	d.attachments = {std::move(att)};
	return d;
}

std::string errOf(const std::expected<CompiledRenderGraph, Error>& g) {
	return g.has_value() ? "<编译竟然通过了>" : g.error().message;
}

/** 编译单附件图并取错误消息（期望它**必然**失败；失败原因由调用方逐字节断言）。 */
std::string errOfSingle(RgAttachmentDesc att) {
	return errOf(compileRenderGraph(singleAttGraph(std::move(att))));
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 组 0：空集合守卫（对空集合遍历 = 恒真假绿，先钉名单非空且恰好四档）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("kind 名单非空且恰好五档（再加一档时本条先红，提醒补全整张表）") {
	registerBufferAttachmentsForCases();
	const auto& cases = kindCases();
	REQUIRE_FALSE(cases.empty());
	// 数量锚：本文件所有遍历都建立在"五档全在"之上。加第六个值而没补这张表时，
	// 后面每一组都会"遍历到五档全绿"= 假绿；这条先红，把假绿堵在门口。
	// （第五档 buffer 就是被这条逼出来的：加它时本文件从 4 改到 5，一处没漏就过不了。）
	CHECK(cases.size() == 5);
	// 名单自身不得重复（重复 = 某一档其实没被覆盖，而 size 仍是 5）
	for (size_t i = 0; i < cases.size(); ++i)
		for (size_t j = i + 1; j < cases.size(); ++j)
			CHECK(cases[i].kind != cases[j].kind);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 1：kindName 阳性锚点（接口导出，直接断言拼写）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("kindName：五档拼写逐个钉死（构造期现场只剩日志，归因错 = 查不到）") {
	registerBufferAttachmentsForCases();
	const auto& cases = kindCases();
	REQUIRE_FALSE(cases.empty());
	for (const auto& c : cases) {
		INFO("kind=" << static_cast<int>(c.kind));
		CHECK(kindName(c.kind) == c.name);
	}
	// 反向配对：四个名字两两不同。若两档同名，上面四条仍可能全绿（表写错时一起错），
	// 而"归因"这件事已经彻底失效——所以唯一性要单独钉。
	for (size_t i = 0; i < cases.size(); ++i)
		for (size_t j = i + 1; j < cases.size(); ++j)
			CHECK(kindName(cases[i].kind) != kindName(cases[j].kind));
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 2：aspectOf（经公开面 RgCompiledAttachment::aspect）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("aspectOf：深度两档 eDepth / 颜色两档 eColor / buffer 空 mask（§7.10 静默回落的本体）") {
	registerBufferAttachmentsForCases();
	// 图形态：五档各一个附件 + 一个写 swapchain 的 pass（否则帧末 present 规则先报错）。
	// 附件顺序 = kindCases 顺序，slot 即下标。
	RgGraphDesc d;
	for (const auto& c : kindCases())
		d.attachments.push_back(RgAttachmentDesc{
			.name = std::string(c.name), .kind = c.kind, .format = std::string(c.format)});
	// depthOffscreen 走独立 extent（阴影图与窗口解耦是它的固有语义，不是本用例的变量）
	d.attachments[3].extent = {1024u, 1024u};
	d.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
		.drawSource = "opaque",
		.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite},
			RgPassAttachment{.attachment = "depth", .usage = RgUsage::depthWrite}}}};

	const auto g = compileRenderGraph(d);
	REQUIRE_MESSAGE(g.has_value(), errOf(g).c_str());
	REQUIRE(g->attachments.size() == 5);
	for (size_t i = 0; i < kindCases().size(); ++i) {
		const auto& c = kindCases()[i];
		INFO("kind=" << c.name);
		// 逐档比**整个 mask**（不是"像不像深度面"）：buffer 的期望值是空 mask，
		// 回落成 eColor 或 eDepth 都在这里红，而不是伪装成 image 继续往下走。
		CHECK(g->attachments[i].aspect == c.expectAspect);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 3：frameStartDiscards（经公开面 RgCompiledAttachment::frameStartDiscard）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("frameStartDiscard：只有 offscreen 与 buffer 是持久档（变异红线 3：改 true 会红）") {
	registerBufferAttachmentsForCases();
	RgGraphDesc d;
	for (const auto& c : kindCases())
		d.attachments.push_back(RgAttachmentDesc{
			.name = std::string(c.name), .kind = c.kind, .format = std::string(c.format)});
	d.attachments[3].extent = {1024u, 1024u};
	d.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
		.drawSource = "opaque",
		.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite},
			RgPassAttachment{.attachment = "depth", .usage = RgUsage::depthWrite}}}};

	const auto g = compileRenderGraph(d);
	REQUIRE(g.has_value());
	for (size_t i = 0; i < kindCases().size(); ++i) {
		const auto& c = kindCases()[i];
		INFO("kind=" << c.name);
		// 变异红线 3：offscreen 改成 true ⟹ 持久图帧首 oldLayout = eUndefined（内容可丢），
		// 首帧画面完全正常、隔帧才丢内容 ⟹ 截图回归抓不到，只有本行红。
		CHECK(g->attachments[i].frameStartDiscard == c.expectFrameStartDiscard);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 4：四个 validator 的拒绝消息**逐字节**断言（含 kind 名）
//   同时钉住两件事：① 不许顺手改文案 ② kind 名归因必须正确
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("validateAttachmentFormat：swapchain/depth 非 inherit 的拒绝消息逐字节（kind 名归因）") {
	// 走的是"两表分立"之后的兜底分支：kind ∉ {offscreen, depthOffscreen} 才进这里，
	// 所以本分支可达的 kind 只有 swapchain / depth —— 两条各钉一次。
	CHECK(errOfSingle(RgAttachmentDesc{.name = "att", .kind = RgAttachmentKind::swapchain,
		        .format = "rgba8_unorm"})
		== "render graph: swapchain attachment 'att' must use format 'inherit' (got 'rgba8_unorm')");
	CHECK(errOfSingle(RgAttachmentDesc{.name = "att", .kind = RgAttachmentKind::depth,
		        .format = "rgba8_unorm"})
		== "render graph: depth attachment 'att' must use format 'inherit' (got 'rgba8_unorm')");

	// 另两档各有自己的分支，一并钉住（它们不经 kindName，改 kindName 不该动到这两条）
	CHECK(errOfSingle(RgAttachmentDesc{.name = "att", .kind = RgAttachmentKind::offscreen,
		        .format = "not_a_format"})
		== "render graph: attachment 'att' has unknown format 'not_a_format'");
	CHECK(errOfSingle(RgAttachmentDesc{.name = "att", .kind = RgAttachmentKind::depthOffscreen,
		        .format = "rgba8_unorm"})
		== "render graph: depth attachment 'att' needs a depth format "
		   "(d32_sfloat / d16_unorm), got 'rgba8_unorm'");
}

TEST_CASE("validateAttachmentExtent：swapchain/depth 填 extent 的拒绝消息逐字节") {
	CHECK(errOfSingle(RgAttachmentDesc{.name = "att", .kind = RgAttachmentKind::swapchain,
		        .extent = {2u, 3u}})
		== "render graph: swapchain attachment 'att' must leave extent {0,0} (follows swapchain)");
	CHECK(errOfSingle(RgAttachmentDesc{.name = "att", .kind = RgAttachmentKind::depth,
		        .extent = {2u, 3u}})
		== "render graph: depth attachment 'att' must leave extent {0,0} (follows swapchain)");
}

TEST_CASE("validateAttachmentExtentScale：三档非 offscreen 的拒绝消息逐字节（旧三元式末档是 depthOffscreen 兜底）") {
	// 旧写法是 swapchain ? … : depth ? … : "depthOffscreen" —— 末档是**兜底**，
	// 第五个 kind 会被叫成 depthOffscreen。改用 kindName 后这里必须逐字节不变，
	// 而第五个 kind 落地时会因穷尽 switch 编译不过（不是静默叫成 depthOffscreen）。
	for (const auto& [kind, want] : {std::pair{RgAttachmentKind::swapchain, "swapchain"},
		     {RgAttachmentKind::depth, "depth"}, {RgAttachmentKind::depthOffscreen, "depthOffscreen"}}) {
		RgAttachmentDesc att;
		att.name = "att";
		att.kind = kind;
		// 格式按各档合法值填，否则先撞格式 guard 测不到 extentScale guard
		att.format = kind == RgAttachmentKind::depthOffscreen ? "d32_sfloat" : "inherit";
		att.extentScale = 0.5f;
		INFO("kind=" << static_cast<int>(kind));
		CHECK(errOfSingle(att)
			== std::string("render graph: extentScale is only allowed on offscreen attachments, but 'att' is ")
			       + want);
	}
	// 反向配对：同一条声明挂 offscreen 上必须通过（否则上面三条可能只是图本身非法）
	RgAttachmentDesc ok;
	ok.name = "att";
	ok.kind = RgAttachmentKind::offscreen;
	ok.format = "rgba8_unorm";
	ok.extentScale = 0.5f;
	CHECK(compileRenderGraph(singleAttGraph(std::move(ok))).has_value());
}

TEST_CASE("validateAttachmentHistory：三档非 offscreen 的拒绝消息逐字节（注意前缀是 render_graph: 带下划线）") {
	// ⚠ 本条消息前缀是 `render_graph:`（下划线），而组 4 其它三条是 `render graph:`（空格）。
	// 逐字节断言把这一点也钉住 —— 这正是"顺手统一文案"会踩的地方。
	for (const auto& [kind, want] : {std::pair{RgAttachmentKind::swapchain, "swapchain"},
		     {RgAttachmentKind::depth, "depth"}, {RgAttachmentKind::depthOffscreen, "depthOffscreen"}}) {
		RgAttachmentDesc att;
		att.name = "att";
		att.kind = kind;
		att.format = kind == RgAttachmentKind::depthOffscreen ? "d32_sfloat" : "inherit";
		if (kind == RgAttachmentKind::depthOffscreen)
			att.extent = {1024u, 1024u};  // 深度 RT 走独立 extent（固有语义）
		att.history = true;
		INFO("kind=" << static_cast<int>(kind));
		CHECK(errOfSingle(att)
			== std::string("render_graph: history=true requires kind=offscreen, but attachment 'att' is ")
			       + want);
	}
	// 反向配对：history 挂 offscreen 上不得报这条（本用例的图没有 writer，会报"never
	// written"那条，所以这里判"不是 history 那条"而不是"编译通过"）。
	RgAttachmentDesc hist;
	hist.name = "att";
	hist.kind = RgAttachmentKind::offscreen;
	hist.format = "rgba8_unorm";
	hist.history = true;
	const auto g = compileRenderGraph(singleAttGraph(std::move(hist)));
	REQUIRE_FALSE(g.has_value());
	CHECK(errOf(g).find("history=true requires kind=offscreen") == std::string::npos);
	CHECK(errOf(g).find("never written") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 5：isRtBackedKind（经公开面：独立 extent 只对图属 RT 开放）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("isRtBackedKind：图属两档可声明独立 extent，非图属三档填了必被拒") {
	registerBufferAttachmentsForCases();
	const auto& cases = kindCases();
	REQUIRE_FALSE(cases.empty());
	size_t rejected = 0;  // 空集合守卫：非图属档必须**真的覆盖到**，否则 else 分支空真
	for (const auto& c : cases) {
		RgAttachmentDesc att;
		att.name = "att";
		att.kind = c.kind;
		att.format = std::string(c.format);
		att.extent = {64u, 64u};  // 唯一变量：填了独立 extent
		INFO("kind=" << c.name << " err=" << errOfSingle(att));
		if (c.expectRtBacked)
			CHECK(compileRenderGraph(singleAttGraph(att)).has_value());
		else {
			CHECK_FALSE(compileRenderGraph(singleAttGraph(att)).has_value());
			++rejected;
		}
	}
	// swapchain + depth + buffer 三档都要被拒。buffer 这一档的意义在"它不是因为没注册
	// 才被拒"——注册已在上面做过，所以红的原因必须是 extent guard（同文件 buffer 专测里
	// 逐字节钉着那条消息）。
	CHECK(rejected == 3);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 6：isDepthKind（经公开面：clearValue 取哪个分量 + depthWrite 合法性）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("isDepthKind：写角色的 clearValue 走 depth 分量的恰好是深度两档") {
	// 图形态：一个 pass 同时写「待测深度档」与 offscreen 颜色档，两档各给一组互不相同的
	// clear 值；断言产物里深度条目取的是 depth 分量、颜色条目取的是 color 分量。
	// 出处 = RgClearValue 注释「kind=depth 用 depth，其余用 color（执行器按 attachment
	// kind 择一）」。图里刻意**不放 swapchain 附件**：放了就得写它，否则先撞帧末
	// 「swapchain attachment is never written」那条 guard，测不到 clearValue 分派。
	auto build = [](std::string_view depthName, RgAttachmentKind depthKind) {
		RgGraphDesc d;
		d.attachments = {
			RgAttachmentDesc{.name = std::string(depthName), .kind = depthKind,
				.format = depthKind == RgAttachmentKind::depth ? "inherit" : "d32_sfloat"},
			RgAttachmentDesc{.name = "rt", .kind = RgAttachmentKind::offscreen,
				.format = "rgba8_unorm"},
		};
		d.passes = {RgPassDesc{.name = "p", .kind = RgPassKind::drawMesh, .state = "opaque",
			.drawSource = "opaque",
			.outputs = {RgPassAttachment{.attachment = std::string(depthName),
					     .usage = RgUsage::depthWrite,
					     .clear = RgClearValue{{1.f, 2.f, 3.f, 4.f}, 0.25f}},
				RgPassAttachment{.attachment = "rt", .usage = RgUsage::colorWrite,
					.clear = RgClearValue{{5.f, 6.f, 7.f, 8.f}, 0.75f}}}}};
		return d;
	};

	const auto& cases = kindCases();
	registerBufferAttachmentsForCases();
	REQUIRE_FALSE(cases.empty());
	size_t covered = 0;  // 空集合守卫的第二半：确认深度档确实覆盖到了两档
	for (const auto& c : cases) {
		if (!c.expectDepthKind)
			continue;  // 非深度档不能作 depthWrite 目标（下一条用例专门钉这条）
		++covered;
		const auto g = compileRenderGraph(build(c.name, c.kind));
		INFO("kind=" << c.name << " err=" << errOf(g));
		REQUIRE(g.has_value());
		REQUIRE(g->passes.size() == 1);
		REQUIRE(g->passes[0].depth.has_value());
		// 深度条目：取 clear.depth（color 那组值故意留在这里当反向对照——它必须没被采用）
		CHECK(g->passes[0].depth->clearValue.depthStencil.depth == 0.25f);
		CHECK(g->passes[0].depth->clearValue.depthStencil.stencil == 0u);
		// 颜色条目（offscreen，非深度档）：取 clear.color 四个分量，不是 depth 的 0.75
		REQUIRE(g->passes[0].colors.size() == 1);
		const auto& col = g->passes[0].colors[0].clearValue.color;
		CHECK(col.float32[0] == 5.f);
		CHECK(col.float32[1] == 6.f);
		CHECK(col.float32[2] == 7.f);
		CHECK(col.float32[3] == 8.f);
	}
	CHECK(covered == 2);  // depth + depthOffscreen 两档都覆盖到了
}

TEST_CASE("isDepthKind 反向：非深度档被 depthWrite 时拒绝消息逐字节点名该附件") {
	const auto& cases = kindCases();
	registerBufferAttachmentsForCases();
	REQUIRE_FALSE(cases.empty());
	size_t covered = 0;
	for (const auto& c : cases) {
		if (c.expectDepthKind)
			continue;
		++covered;
		// 单附件 + 单 depthWrite：不写 swapchain 就不触发 present 那条 guard，
		// 报错必然来自 usage×kind 匹配这一条（否则本断言会被别的 guard 抢先满足）。
		RgGraphDesc d;
		d.attachments = {RgAttachmentDesc{
			.name = "x", .kind = c.kind, .format = std::string(c.format)}};
		d.passes = {RgPassDesc{.name = "p", .kind = RgPassKind::drawMesh, .state = "opaque",
			.drawSource = "opaque",
			.outputs = {RgPassAttachment{.attachment = "x", .usage = RgUsage::depthWrite}}}};
		const auto g = compileRenderGraph(d);
		INFO("kind=" << c.name << " err=" << errOf(g));
		REQUIRE_FALSE(g.has_value());
		CHECK(errOf(g) == "render_graph: pass 'p' depthWrites non-depth attachment 'x'");
	}
	CHECK(covered == 3);  // swapchain + offscreen + buffer 三档都覆盖到了
}

}  // namespace RGL
