module;
/**
 * test_attachment_buffer —— 渲染图的 **buffer 档**（批次 B2-3 本体 → docs/compute_调研.md §6 D1 / §7.16）
 *
 * ## 被测契约（三句话）
 *
 *  1. `kind = buffer` 的附件**不持资源**，它只是"这个名字代表一段 buffer"的申报；名字必须在
 *     C++ 侧按名注册（`registerBufferAttachment`）。未注册即拒 = 拒绝面落在 `registerAttachment`，
 *     于是 scene data staging / heap host-write / pushData / 信号量 / fence 那五类**声明不出来**，
 *     不需要黑名单。
 *  2. `storageRead` / `storageWrite` 是 buffer 附件**唯一**合法的引用方式，且反向也成立（image
 *     附件不得用 storage 档）。两条各一条用例 —— 只钉一条 = 半边判据。
 *  3. plan-only 红线做成**类型**：storage 档的 `UsageInfo::layout` 是 `nullopt` ⟹ buffer 既不进
 *     barrier 计划、也不进 beginRendering 条目。这不是"记得写分支"，是写不出来。
 *
 * ## 为什么值得测（而不是"冒烟跑过就行"）
 *
 * 第 1 条的失效形态是**静默**的：拼错一个字母的 buffer 附件名，barrier 照发、对账照过、画面
 * 什么都不缺，直到某个 dispatch 写越界才炸，而那种炸法离根因隔着一整帧。第 3 条的失效形态更坏：
 * `vk::ImageLayout::eUndefined` 作 newLayout 是**合法** Vulkan 值，VVL 不报（§7.10 那条 aspect
 * 回落同族）。所以两条都只能靠编译期判据，而编译期判据不跑就永远"看起来在"。
 *
 * ## 期望值出处（不抄实现返回值）
 *
 * 字节数与写上界来自**被测码之外**：`meshlet_cull.comp.slang` 的 `clusterVisible.Store(tid.x*4u)`
 * 守卫 `tid.x < itemCount`（⟹ 写上界 = itemCount×4）、`meshlet_cull_service_impl.cppm` 的容量
 * 公式、以及 `render_graph.cppm` 里 buffer 档的接口注释。错误消息的期望串 = 消息**本身**应当
 * 说清的事（谁、为什么、去哪儿改），不是"实现现在返回什么"——所以逐字节钉，改文案必红。
 *
 * ## 变异红线（哪个变异让我红；答不出 = 摆设）
 *
 *   1. 删掉 `validateAttachmentBufferName` 的调用（或整个函数直接 `return {}`）→ 组 2 第一条红
 *      （未注册名被放行 = 名字挂在不存在的资源上，且没有任何信号）。
 *   2. 把 `if (att.kind != buffer) return {}` 改成无条件查表 → 组 2 反向那条红（image 档被
 *      注册表绑架，出厂图直接编不过）。
 *   3. 删 `validatePassRef` 里 `storageUsage && !bufferAttachment` 那条 → 组 3 第一条红
 *      （image 声明成 storage = 声明"我不转布局" = 静默漏同步，syncval 实测抓不到）。
 *   4. 删对偶的 `bufferAttachment && !storageUsage` 那条 → 组 3 第二条红（buffer 声明成
 *      colorWrite/sampleRead = dynamic rendering 与 blit 两端都会吃到一个不是 image 的 slot）。
 *   5. **把 `UsageInfo::layout` 的 optional 拆掉**（给 storage 兜一个 `eUndefined`）→ 组 4 红
 *      （buffer 出现在 barrier 计划里）。这条是本文件最重要的一条：它证明"plan-only 做成类型"
 *      不是注释，而拆掉类型判据立刻失效。
 *   6. `planBeginRenderingEntry` 去掉 nullopt 早退 → 组 4 的条目断言红（buffer 挤进
 *      beginRendering 的 color 列表 = 把 buffer 句柄当 image view 交给 dynamic rendering）。
 *   7. `checkBufferWriteBound` 的 `>=` 改成 `>`（边界判错）→ 组 1 的"相等必须绿"红。
 *   8. 把对账方向写反（`*bytes <= writeBound` 才通过）→ 组 1 四条里至少两条红。
 *   9. 注册表 lambda 改成捕获一个可变成员（"注册后还能改"）→ 组 1 的"注册偏大不判红"红。
 *  10. `advanceCursors` 不把 storageWrite 计为写 → 组 5 两条同时红（读前置误拒 / store 完整性失效）。
 *  11. `frameStartDiscards` 的 buffer 档改成 `true` → 组 6 红（buffer 的帧首语义被编成"旧内容
 *      随便丢"，而它的真实对应物是单份 buffer 跨 frames-in-flight 复用）。
 *  12. Lua 词表漏一条（债-122 的原始形态：枚举有、词表没有）→ 组 7 红。反向（未知串被放行）
 *      由组 7 最后一条钉。
 *  13. （B2-4③′）删掉 `validateAttachmentBufferName` 的「注册字节 >0」检查 → 组 8 红
 *      （0 字节附件被放行 = 校验吃 0，「资源还没建就来声明附件」静默进图）。
 *
 * 🔴 **本文件测不到的**：真程序里 meshlet 那两张 buffer 的**实际**字节数与写上界是否对上 ——
 * 那是启动期对账（`uploadItems` 里那次 `checkBufferWriteBound`），判据 = 真程序变异（把注册
 * 字节数改小于写上界 ⟹ 启动 throw 点名两侧字节数），口径见交付报告与 §7.1 的"门开才测"。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import rlstd 带进来
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>

export module test_attachment_buffer;

import rlstd;
import rlvk;
import render_graph;
import render_graph_config;  // loadRenderGraphConfig（Lua 词表那组的入口）
import vkerror;

namespace RGL {
namespace {

/**
 * 临时 lua 配置（构造写盘、析构删除）。形态照 `tests/test_scene_config.cppm::TempLua`，
 * 本文件自带一份而不是共享：那份住在测试文件里而不是 `test_support`，抽公共 = 顺手改别人。
 */
class TempLua {
  public:
	TempLua(std::string_view tag, std::string_view body)
	: path_(std::filesystem::temp_directory_path() / ("rgl_bufcfg_" + std::string(tag) + ".lua")) {
		std::ofstream out(path_);
		out << body;
	}
	TempLua(const TempLua&) = delete;
	TempLua& operator=(const TempLua&) = delete;
	~TempLua() {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}
	std::string pathStr() const { return path_.string(); }

  private:
	std::filesystem::path path_;
};

/**
 * 注册一张 buffer 并返回它的名字。
 *
 * 🔴 名字**逐个用例唯一**（本参数不是装饰）：注册表是进程级单例、跨 TEST_CASE 不清空，
 * 而 doctest 的用例顺序可换。两个用例抢同一个名字 = 其中一个的字节数会决定另一个的读数，
 * 那种红随机器与运行次数漂（先例：本仓对"日志次序漂"零容忍的同一条理由）。
 */
std::string registerBuf(std::string_view name, vk::DeviceSize bytes) {
	registerBufferAttachment(name, [bytes] { return bytes; });
	return std::string(name);
}

/** 一条 storageWrite + 一条 colorWrite 的最小合法图（drawMesh 必须至少有一个渲染写附件）。 */
RgGraphDesc bufferWriteGraph(const std::string& bufferName) {
	return RgGraphDesc{
		.attachments = {
			RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
			RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
			RgAttachmentDesc{.name = bufferName, .kind = RgAttachmentKind::buffer},
		},
		.passes = {
			RgPassDesc{.name = "cull", .kind = RgPassKind::drawMesh, .state = "opaque",
				.drawSource = "opaque",
				.outputs = {RgPassAttachment{.attachment = "depth", .usage = RgUsage::depthWrite},
					RgPassAttachment{.attachment = bufferName,
						.usage = RgUsage::storageWrite}}},
			RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
				.drawSource = "opaque", .outputs = {RgPassAttachment{.attachment = "swapchain",
										   .usage = RgUsage::colorWrite}},
				.inputs = {RgPassAttachment{.attachment = bufferName,
					.usage = RgUsage::storageRead}}},
		},
	};
}

std::string errOf(const std::expected<CompiledRenderGraph, Error>& g) {
	return g.has_value() ? std::string{"<编译竟然通过了>"} : g.error().message;
}

/** 配置加载的失败消息（绿 = 空串）。doctest 消息表达式吃不下三元，统一走这里。 */
std::string errOfConfig(const std::expected<RgGraphDesc, Error>& c) {
	return c.has_value() ? std::string{"<配置竟然加载通过了>"} : c.error().message;
}

/** 编译一张图并取错误消息（期望它**必然**失败；绿 = 空串）。 */
std::string errOfBuild(const RgGraphDesc& d) {
	const auto g = compileRenderGraph(d);
	return g.has_value() ? std::string{} : g.error().message;
}

std::string errOfMessage(const std::expected<void, Error>& r) {
	return r.has_value() ? std::string{} : r.error().message;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 组 0：注册表数据面（阳性锚点 + 未注册的反向）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("buffer 注册表：登记过的名字查得到、没登记的查不到（拒绝面的数据半边）") {
	const auto name = registerBuf("rtmpRegistryBasic", 1024);
	// 阳性锚点：登记 → 可查、字节数就是登记的那个值（不是 0、不是默认值）
	CHECK(isBufferAttachmentRegistered(name));
	REQUIRE(bufferAttachmentBytes(name).has_value());
	CHECK(*bufferAttachmentBytes(name) == 1024u);
	// 反向：没登记的名字两个查询都必须答"没有"。若它们答"有"/答 0，未注册名就会带着一个
	// 编出来的字节数走进对账 —— 对账变成拿 0 比大小，恒红或恒绿取决于方向。
	CHECK_FALSE(isBufferAttachmentRegistered("rtmpNeverRegistered"));
	CHECK_FALSE(bufferAttachmentBytes("rtmpNeverRegistered").has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 1：写上界对账（判据 5 的单元半边；真程序半边 = 启动期 throw）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("checkBufferWriteBound：注册 ≥ 写上界才过；越界点名两侧字节数与差值") {
	const auto ok = registerBuf("rtmpBoundOk", 1024);
	const auto big = registerBuf("rtmpBoundBig", 4096);

	// 阳性锚点（变异 7）：**相等**必须过。写成 `>` 就把"dispatch 恰好写满整块"判成越界，
	// 而那条路径正是 meshlet 标志表满载的形态（itemCount == clusterCapacity）。
	CHECK(errOfMessage(checkBufferWriteBound(ok, 1024)).empty());
	CHECK(errOfMessage(checkBufferWriteBound(ok, 16)).empty());
	// 注册偏大**不判红**（变异 9）：对账是单向的，注册表允许容量给宽。
	// 反过来（给宽也算违规）会逼着每个模块把注册字节数算到逐字节精确，那才是事故源头。
	CHECK(errOfMessage(checkBufferWriteBound(big, 1024)).empty());

	// 越界：必须红，且消息里**两侧数字与差值都在**（这条在启动期抛出，现场只剩一行日志，
	// 要能一眼看出是"容量算小了"还是"写上界申报大了"——两者修复位置不同）。
	const auto bad = errOfMessage(checkBufferWriteBound(ok, 1025));
	REQUIRE_FALSE(bad.empty());
	CHECK(bad.find("rtmpBoundOk") != std::string::npos);
	CHECK(bad.find("1025") != std::string::npos);
	CHECK(bad.find("1024") != std::string::npos);
	CHECK(bad.find("1B") != std::string::npos);  // 差值

	// 未注册名**不能**当成"对账通过"（变异 8 的另一半：把 nullopt 兜成 0 会恒红，
	// 兜成"跳过"会恒绿 —— 两个方向都错，正确答案是出声）。
	const auto unreg = errOfMessage(checkBufferWriteBound("rtmpNeverRegisteredForBound", 1));
	REQUIRE_FALSE(unreg.empty());
	CHECK(unreg.find("rtmpNeverRegisteredForBound") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 2：拒绝面 —— kind=buffer 的名字必须已注册（落点 = registerAttachment）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("kind=buffer 未注册名即拒；消息点名注册表与注册入口") {
	RgGraphDesc d;
	d.attachments = {RgAttachmentDesc{
		.name = "rtmpTypoName", .kind = RgAttachmentKind::buffer}};
	const auto msg = errOf(compileRenderGraph(d));
	REQUIRE(msg.find("<编译竟然通过了>") == std::string::npos);
	// 逐字节钉整条消息：这条是"拼错一个字母"的唯一信号，措辞里"去哪儿注册"是判据的一部分。
	CHECK(msg
		== "render graph: buffer attachment 'rtmpTypoName' is not registered in the C++ buffer "
		   "registry (register it with registerBufferAttachment before compiling the graph; the "
		   "registry is the only place a buffer's byte size may come from — see "
		   "rgl/render_graph_buffers.cppm)");

	// 阳性锚点（变异 1 的反向配对）：同一个名字登记之后**必须**编译得过。
	// 少了这条，上面那条红可能只是"buffer 档整个编不过"而不是"名字没登记"。
	const auto name = registerBuf("rtmpTypoName", 256);
	RgGraphDesc ok;
	ok.attachments = {
		RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
		RgAttachmentDesc{.name = name, .kind = RgAttachmentKind::buffer},
	};
	ok.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
		.drawSource = "opaque",
		.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite}}}};
	CHECK_MESSAGE(compileRenderGraph(ok).has_value(), errOf(compileRenderGraph(ok)).c_str());
}

TEST_CASE("非 buffer 档与注册表无关（变异 2：不许把 image 档也绑上注册表面）") {
	// 现场：一个**已登记**的名字，声明成 offscreen。当前规则是"非 buffer 档不查注册表"，
	// 所以必须编译得过 —— 它钉的是"注册表只管 buffer 档"这条边界：把查表改成无条件，
	// 出厂那张图（附件名与任何注册名都不相干）会直接编不过。
	const auto name = registerBuf("rtmpSharedName", 512);
	RgGraphDesc d;
	d.attachments = {
		RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
		RgAttachmentDesc{.name = name, .kind = RgAttachmentKind::offscreen,
			.format = "rgba8_unorm"},
	};
	d.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
		.drawSource = "opaque",
		.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite}}}};
	CHECK_MESSAGE(compileRenderGraph(d).has_value(), errOf(compileRenderGraph(d)).c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 3：usage × kind 的**双向**规则（两条各一条，少一条就是半边判据）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("image 附件用 storage 档 → 拒（声明'这块 image 我不转布局'）") {
	const auto name = registerBuf("rtmpBidirA", 128);
	RgGraphDesc d;
	d.attachments = {
		RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
		RgAttachmentDesc{.name = "rtmpImg", .kind = RgAttachmentKind::offscreen,
			.format = "rgba8_unorm"},
		RgAttachmentDesc{.name = name, .kind = RgAttachmentKind::buffer},
	};
	// 第一个 pass 把 offscreen 写成 storageWrite：语义 = 不转布局就写它 = 静默漏同步。
	d.passes = {
		RgPassDesc{.name = "w", .kind = RgPassKind::drawMesh, .state = "opaque",
			.drawSource = "opaque",
			.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite},
				RgPassAttachment{.attachment = "rtmpImg", .usage = RgUsage::storageWrite}}},
	};
	const auto msg = errOf(compileRenderGraph(d));
	REQUIRE(msg.find("<编译竟然通过了>") == std::string::npos);
	CHECK(msg
		== "render_graph: pass 'w' uses storage usage on non-buffer attachment 'rtmpImg'");
}

TEST_CASE("buffer 附件用非 storage 档 → 拒（colorWrite/sampleRead 对 buffer 无意义）") {
	const auto name = registerBuf("rtmpBidirB", 128);
	RgGraphDesc d;
	d.attachments = {
		RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
		RgAttachmentDesc{.name = name, .kind = RgAttachmentKind::buffer},
	};
	// buffer 先被 storageWrite 写一次（否则先撞"读前置"，红的就不是本条判据了）。
	// 第二个 pass 拿它当 sampleRead —— dynamic rendering 与 blit 两端都只吃 image。
	d.passes = {
		RgPassDesc{.name = "w", .kind = RgPassKind::drawMesh, .state = "opaque",
			.drawSource = "opaque",
			.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite},
				RgPassAttachment{.attachment = name, .usage = RgUsage::storageWrite}}},
		RgPassDesc{.name = "r", .kind = RgPassKind::drawLight, .state = "tonemap",
			.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite}},
			.inputs = {RgPassAttachment{.attachment = name, .usage = RgUsage::sampleRead}}},
	};
	const auto msg = errOf(compileRenderGraph(d));
	REQUIRE(msg.find("<编译竟然通过了>") == std::string::npos);
	CHECK(msg
		== "render_graph: pass 'r' references buffer attachment 'rtmpBidirB' with non-storage usage");
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 4：plan-only 做成类型 —— buffer 既不进 barrier 计划、也不进 beginRendering 条目
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("buffer 档不进 barrier 计划、不进 beginRendering 条目（变异 5/6 的靶子）") {
	const auto name = registerBuf("rtmpNoPlan", 2048);
	const auto graph = bufferWriteGraph(name);
	const auto g = compileRenderGraph(graph);
	REQUIRE_MESSAGE(g.has_value(), errOfBuild(graph).c_str());

	// 附件顺序 = 声明序，slot 即下标（buffer 在 2 号位）。
	REQUIRE(g->attachments.size() == 3);
	const RgAttachmentSlot bufferSlot = 2;
	CHECK(g->attachments[bufferSlot].kind == RgAttachmentKind::buffer);

	// 🔴 阳性锚点（本用例的**一半**价值在这里）：同图里必须**确实有** image 附件产生了 barrier。
	// 少了这个锚点，"零 barrier 提到 buffer"会因为"图根本没产任何 barrier"而恒真 = 假绿。
	uint32_t totalBarriers = 0;
	for (const auto& p : g->passes)
		totalBarriers += static_cast<uint32_t>(p.barriers.size());
	CHECK_MESSAGE(totalBarriers >= 2, "同图里的 depth/swapchain 必须产 barrier，否则下面的断言空真");

	// 判据本体：没有任何一条 barrier / beginRendering 条目落在 buffer slot 上。
	// （doctest 吃不下 `opt.has_value() && opt->x == y`，先落局部量再断言。）
	for (const auto& p : g->passes) {
		for (const auto& b : p.barriers)
			CHECK_MESSAGE(b.attachment != bufferSlot,
				("buffer 附件出现在 barrier 计划里（pass=" + p.name + "）").c_str());
		for (const auto& c : p.colors)
			CHECK(c.slot != bufferSlot);
		const bool depthIsBuffer = p.depth.has_value() && p.depth->slot == bufferSlot;
		const bool blitIsBuffer = p.blitTarget.has_value() && *p.blitTarget == bufferSlot;
		CHECK_FALSE(depthIsBuffer);
		CHECK_FALSE(blitIsBuffer);
	}
	// 帧末 present 转换也不能指向它。⚠ 本图**确实有** present 转换（pass2 写了 swapchain），
	// 所以这里判的是"指向谁"而不是"有没有"——判"没有"会把一条正常产物读成失败。
	const bool presentIsBuffer
		= g->presentTransition.has_value() && g->presentTransition->attachment == bufferSlot;
	CHECK_FALSE(presentIsBuffer);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 5：游标照常推进 —— 读前置与 store 完整性对 buffer 同样生效（变异 10）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("storageWrite 计为写：先写后读放行；没写过就读被拒（读前置不豁免 buffer）") {
	const auto name = registerBuf("rtmpCursor", 512);

	// 阳性锚点：storageWrite(pass1) → storageRead(pass2) 放行 ⟹ 游标把 storageWrite 当写。
	CHECK_MESSAGE(compileRenderGraph(bufferWriteGraph(name)).has_value(),
		errOfBuild(bufferWriteGraph(name)).c_str());

	// 反向：只读不写 ⟹ 必须红在"读前置"，而不是红在别的 guard 上。
	RgGraphDesc d;
	d.attachments = {
		RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
		RgAttachmentDesc{.name = name, .kind = RgAttachmentKind::buffer},
	};
	d.passes = {RgPassDesc{.name = "r", .kind = RgPassKind::drawMesh, .state = "opaque",
		.drawSource = "opaque",
		.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite}},
		.inputs = {RgPassAttachment{.attachment = name, .usage = RgUsage::storageRead}}}};
	const auto msg = errOf(compileRenderGraph(d));
	REQUIRE(msg.find("<编译竟然通过了>") == std::string::npos);
	CHECK(msg.find("reads '" + name + "' before any write in the frame") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 6：buffer 档在"image 专属概念"上的答案（aspect / extent / 帧首丢弃）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("buffer 档：aspect 空 mask、extent 必须留空、frameStartDiscard = false") {
	const auto name = registerBuf("rtmpImageConcepts", 64);
	const auto g = compileRenderGraph(bufferWriteGraph(name));
	REQUIRE_MESSAGE(g.has_value(), errOfBuild(bufferWriteGraph(name)).c_str());

	const auto& att = g->attachments[2];
	// 空 mask = "这不是 image"的显式记号。回落 eColor/eDepth 都会把 buffer 伪装成 image
	// 继续往下走（§7.10 点名的三元式回落形态）。
	CHECK(att.aspect == vk::ImageAspectFlags{});
	// 与 offscreen 同侧：单份 buffer 跨 frames-in-flight 复用（meshlet 标志表正是这一档）。
	// 改成 true = 把"旧内容随便丢"编进它的帧首语义。
	CHECK_FALSE(att.frameStartDiscard);
	CHECK_FALSE(att.history);

	// extent：buffer 只有字节数（在注册表里），像素二维尺寸对它无意义 ⟹ 填了必须被拒。
	// 出处 = isRtBackedKind 的 buffer 分支答 false（"资源归模块申请"），这条断言就是那个
	// false 在公开面上的可见效果 —— 谓词住在 impl 的匿名 namespace，测不到，只能这样测。
	RgGraphDesc bad;
	bad.attachments = {RgAttachmentDesc{
		.name = name, .kind = RgAttachmentKind::buffer, .extent = {4u, 4u}}};
	const auto msg = errOf(compileRenderGraph(bad));
	REQUIRE(msg.find("<编译竟然通过了>") == std::string::npos);
	CHECK(msg.find("buffer attachment '" + name + "' must leave extent {0,0}")
		!= std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 7：Lua 词表（债-122 的原始形态 = 枚举有值、配置词表没挂号）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Lua 词表：kind 认 buffer/depthOffscreen、usage 认 storage 两档；未知串仍拒") {
	// 正向：四档 kind + storage 两档 usage 全部写进一份配置，必须解析成功。
	// depthOffscreen 是**债-122 本体**：它在枚举里住了很久，配置却一直写不出来，
	// 而内建图用同一个 kind 跑得好好的 ⟹ "图支持"与"配置能写"是两条独立的路。
	const auto name = registerBuf("rtmpLuaBuffer", 320);
	TempLua cfg("buffer_kinds",
		"return {\n"
		"  attachments = {\n"
		"    { name = \"swapchain\", kind = \"swapchain\" },\n"
		"    { name = \"depth\", kind = \"depth\" },\n"
		"    { name = \"shadowmap\", kind = \"depthOffscreen\", format = \"d32_sfloat\" },\n"
		"    { name = \""
		+ name
		+ "\", kind = \"buffer\" },\n"
		"  },\n"
		"  passes = {\n"
		"    { name = \"cull\", state = \"opaque\", draws = \"opaque\",\n"
		"      outputs = { { attachment = \"depth\", usage = \"depthWrite\" },\n"
		"                  { attachment = \""
		+ name
		+ "\", usage = \"storageWrite\" } } },\n"
		"    { name = \"opaque\", state = \"opaque\", draws = \"opaque\",\n"
		"      outputs = { { attachment = \"swapchain\", usage = \"colorWrite\" } },\n"
		"      inputs = { { attachment = \""
		+ name
		+ "\", usage = \"storageRead\" } } },\n"
		"  },\n"
		"}\n");
	const auto loaded = loadRenderGraphConfig(cfg.pathStr());
	// doctest 的表达式分解吃不下 `x->a.b[i].c == D`（"Expression Too Complex"），消息表达式
	// 也吃不下带临时量的三元（会打出一个已析构的指针）—— 全部先落成具名局部量再断言。
	const std::string loadErr = errOfConfig(loaded);
	if (!loaded.has_value())
		FAIL("loadRenderGraphConfig 失败：" << loadErr);
	const auto& atts = loaded->attachments;
	const auto& passes = loaded->passes;
	REQUIRE(atts.size() == 4);
	const auto shadowKind = atts[2].kind;
	const auto bufferKind = atts[3].kind;
	CHECK(shadowKind == RgAttachmentKind::depthOffscreen);
	CHECK(bufferKind == RgAttachmentKind::buffer);
	REQUIRE(passes.size() == 2);
	const auto writeUsage = passes[0].outputs[1].usage;
	const auto readUsage = passes[1].inputs[0].usage;
	CHECK(writeUsage == RgUsage::storageWrite);
	CHECK(readUsage == RgUsage::storageRead);

	// 反向（变异 12）：词表**不许**变成来者不拒。kind 与 usage 各钉一条未知串，
	// 期望消息里带合法值清单 —— 只把"写错"变成"报错"还不够，报错要能指路。
	TempLua badKind("buffer_badkind",
		"return { attachments = { { name = \"x\", kind = \"buffers\" } } }\n");
	const auto rk = loadRenderGraphConfig(badKind.pathStr());
	REQUIRE_FALSE(rk.has_value());
	CHECK(rk.error().message.find("buffers") != std::string::npos);

	TempLua badUsage("buffer_badusage",
		"return { attachments = { { name = \"swapchain\", kind = \"swapchain\" } },\n"
		"  passes = { { name = \"p\", state = \"opaque\", draws = \"opaque\",\n"
		"    outputs = { { attachment = \"swapchain\", usage = \"colorRead\" } } } } }\n");
	const auto ru = loadRenderGraphConfig(badUsage.pathStr());
	REQUIRE_FALSE(ru.has_value());
	CHECK(ru.error().message.find("colorRead") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 8（B2-4③′）：拒绝面扩展 —— 注册字节必须 >0；同名替换 = 尺寸漂移的正常路径
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("注册字节 = 0 → 编译期拒；同名替换成 >0 后必须编得过（变异 13 的靶子）") {
	// 现场：模块在资源还没建时就注册（lambda 返回 0 / 尺寸公式算漏）。图对 buffer 档的
	// 全部产物就是校验，校验吃 0 等于没校验 ⟹ 编译期即拒（B2-4③′ 契约④）。
	const auto name = registerBuf("rtmpZeroBytes", 0);
	RgGraphDesc d;
	d.attachments = {
		RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
		RgAttachmentDesc{.name = name, .kind = RgAttachmentKind::buffer},
	};
	d.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
		.drawSource = "opaque",
		.outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::colorWrite}}}};
	const auto msg = errOf(compileRenderGraph(d));
	REQUIRE(msg.find("<编译竟然通过了>") == std::string::npos);
	// 消息必须与"未注册"那条**可区分**（两种坏法的修复位置不同：一个是没注册，
	// 一个是注册了但字节是 0），并点名 >0 这条拒绝面是谁加的。
	CHECK(msg.find("registered with 0 bytes") != std::string::npos);
	CHECK(msg.find(name) != std::string::npos);

	// 阳性锚点（同名替换 = extent 漂移的正常路径，B2-4③′）：同一个名字登记成 >0 之后
	// **必须**编得过。少了这条，上面的红可能只是"buffer 档整个编不过"。
	registerBuf("rtmpZeroBytes", 64);
	CHECK_MESSAGE(compileRenderGraph(d).has_value(), errOf(compileRenderGraph(d)).c_str());
}

}  // namespace RGL
