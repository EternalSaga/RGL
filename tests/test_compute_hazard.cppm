module;
/**
 * test_compute_hazard —— compute ↔ compute hazard 的**机器派生**与**双源对账**（批次 B2-2）
 *
 * 被测三面：
 *   ① `compute_access_log`（rlbase）的 hazard 判据与按类去重（纯逻辑、零设备）；
 *   ② `offgraph_sync` 的 hazard 表 + `checkComputeHazard`（rlgfx 里零设备的那半边：查表 + 消息）；
 *   ③ 🔴 **人工半边 ↔ 反射的双向对账**——本文件最重要的一条，缺它整张 hazard 表就是注释。
 *
 * ## 为什么值得测（这一刀买的东西）
 *
 * 「有人给某枚 compute 加一个 RW buffer、或让第二枚 compute 开始读同一资源，却没加同步边」这类错
 * 在本项目**完全静默**（「缺 barrier VVL 会报」实测不成立，docs/compute_调研.md §4）。改前它是
 * 一张人工核对表；本刀把**能算的那一半**（哪些访问对构成 hazard）交给反射 + 工厂绑定表，于是
 * 本文件测的不再是"人抄得对不对"，而是**两条来源是否互锁**。
 *
 * ## 期望值的出处（不抄被测码）
 *
 * 方向（writable / readable）= 已提交生成物 `rgl/generated/compute_bindings_generated.cppm`
 * （真相源 = 各 `.comp.slang` 的声明类型，Slang 反射实查）；边名 = `RgSyncEdgeId`。
 * 而「哪两个 binding 共享一个 heap 槽」= 七个产品调用点的字面量（`izb_shadow_impl.cppm` 等），
 * 本文件里的调用点地图是它的**手抄副本**——这条限制写在「全产品面复算」那条用例的头注释里，
 * 不假装它是机器知道的。
 *
 * ## 🔴 变异红线声明（哪个变异让我红；答不出 = 摆设）
 *
 *   1. **shader 把写侧改成只读**（`izb_create.comp.slang` 的 `head` 从 `RWByteAddressBuffer`
 *      改成 `ByteAddressBuffer`）→ `just reflect` → 「双源对账」里 writer 的 `writable` 断言
 *      **必须红**（人工半边说它是写侧，反射说它不是）。这就是"人工声明被机器消费"的那一格。
 *   2. **shader 改名 / 删 binding**（`izb_prime.comp.slang` 的 `head` 改名 `headBuf`）→ reflect
 *      → 「双源对账」里「两侧都存在于生成物」那条**必须红**（表里还写着旧名）。
 *   3. **偷偷加一条带 reason 的豁免**（hazard 表加第 6 行）→ 「数量锚」红（5 → 6）。
 *      本表刻意**没有** reason / 「不需要边」的字段，加豁免只能显式改条数，改条数就红。
 *   4. **判据少一条**：删 `isComputeHazard` 的 ④（至少一方写）→ 「同槽两读者不报」红；
 *      删 ③（跳过 none）→ 「一侧是 none 不报」红；删 ①（跨实例）→ 「同实例两 param 同槽不报」红。
 *   5. **匹配改成单向**（`checkComputeHazard` 只认 writer↔A / reader↔B 的正向）→ 「两写者且
 *      已申报」那条红（派生出的无序对方向与表里声明相反，正向匹配查不到 = 假阳性抛）。
 *   6. **按类去重坏掉**（`add` 不再比对既有类）→ 「downsample 链只派生一条类」那条红
 *      （N−1 级会派生 N−1 条，产品侧日志刷屏 = "5 行 hazard 日志"这条验收失效）。
 *   7. **hazard 表与产品面脱钩**（表里躺着一条产品面派不出来的声明）→ 「全产品面复算」的
 *      表 ⊆ 派生集合那一半红。⚠ 调用点地图本身是手抄的（限制见那条用例头注释），别把它读成
 *      "调用点也被钉死"——它钉的是**方向**与**表的内聚**。
 *
 * ## 本文件**测不到**的（说清楚，别把"全绿"读成"同步对了"）
 *
 * 边是否**真的发了**、mask 是否与真实先后匹配——那是 `emit` + `expectedPerFrame` 的账
 * （`test_offgraph_sync.cppm`）与本判据的已知限制（只有方向、没有顺序 ⟹ 匹配是对称的）。
 * 以及非 compute 参与者（frag / mesh / 拷贝 / host）根本不在反射面里，hazard 表看不见它们。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import rlstd 带进来
#include <string>
#include <string_view>
#include <vector>

export module test_compute_hazard;

import rlstd;
import compute_access_log;
import compute_bindings_generated;  // 双源对账的**另一侧**：反射方向（已提交生成物）
import offgraph_sync;               // RgComputeHazardEdge / computeHazardEdges / checkComputeHazard

namespace RGL {

namespace {

/** 一个 dispatch 实例的声明形态（derive 的入参元素）。 */
using InstanceCase = std::pair<std::string_view, std::vector<RgComputeBindingDecl>>;

/** 子串包含判定（报错消息断言用）。不复用任何被测机制。 */
bool contains(const std::string& haystack, std::string_view needle) {
	return haystack.find(needle) != std::string_view::npos;
}

/** 一条 hazard 侧的可读形态（失败信息里定位用）。 */
std::string side(std::string_view shader, std::string_view param) {
	std::string out(shader);
	out += '.';
	out += param;
	return out;
}

/** 取某枚 shader 里某个 binding 的方向（writable, readable）；不在表里返回 nullopt。 */
std::optional<std::pair<bool, bool>> direction(std::string_view shader, std::string_view param) {
	for (const auto& b : computeBindings(shader))
		if (b.name == param)
			return std::pair{b.writable, b.readable};
	return std::nullopt;
}

/**
 * 手搭一个日志：每个 (shader, {binding…}) 一次 add（= 一个 dispatch 实例）。
 * @param cases 实例列表（顺序 = 实例号顺序，判据 ① 依赖它）
 * @return 派生出的全部 hazard 类
 */
std::vector<RgComputeHazardClass> derive(std::initializer_list<InstanceCase> cases) {
	ComputeAccessLog log;
	std::vector<RgComputeHazardClass> all;
	for (const auto& [shader, decls] : cases) {
		const auto fresh = log.add(shader, decls);
		all.insert(all.end(), fresh.begin(), fresh.end());
	}
	return all;
}

}  // namespace

TEST_CASE("阳性锚点：izb_create.head(W) + izb_prime.head(R) 同槽 → 1 条 hazard 且对账通过") {
	// 两个实例（两次 makeMappedCompute = 两个 shader object），同一个 heap 槽 42。
	// 期望值出处：izb_create.comp.slang:30 `RWByteAddressBuffer head`（写）、
	// izb_prime.comp.slang:19 `ByteAddressBuffer head`（读）、izb_shadow_impl.cppm 两处都把
	// head 喂同一个 headSlot。
	const auto fresh = derive({
		{"izb_create.comp.slang", {{"head", 42, true, true}}},
		{"izb_prime.comp.slang", {{"head", 42, false, true}}},
	});
	REQUIRE(fresh.size() == 1);
	CHECK(side(fresh[0].shaderA, fresh[0].paramA) == "izb_create.comp.slang.head");
	CHECK(side(fresh[0].shaderB, fresh[0].paramB) == "izb_prime.comp.slang.head");
	CHECK(fresh[0].slot == 42);

	// 对账：必须找到覆盖它的边，且就是 izb_head_create_to_prime。
	const auto edge = checkComputeHazard(fresh[0]);
	REQUIRE(edge.has_value());
	CHECK(*edge == RgSyncEdgeId::izbHeadCreateToPrime);
	CHECK(syncEdgeDesc(*edge).name == "izb_head_create_to_prime");
}

TEST_CASE("反向用例：未申报的访问对 → 对账失败且消息点名两侧 + 槽号 + 表名") {
	// 自造的 shader/param 名（不在 hazard 表里），形态 = 「阶段 2 预留输出真被接上」：
	// 一枚写、一枚读、同槽。这正是本刀存在要拦的那类错。
	const auto fresh = derive({
		{"fake_writer.comp.slang", {{"outBuf", 7, true, true}}},
		{"fake_reader.comp.slang", {{"inBuf", 7, false, true}}},
	});
	REQUIRE(fresh.size() == 1);
	const auto edge = checkComputeHazard(fresh[0]);
	REQUIRE_FALSE(edge.has_value());
	const std::string msg = edge.error().describe();
	// 🔴 两侧都要点名（shader + param + 槽号），且指路表名——现场只有这一行日志。
	CHECK(contains(msg, "fake_writer.comp.slang"));
	CHECK(contains(msg, "outBuf"));
	CHECK(contains(msg, "fake_reader.comp.slang"));
	CHECK(contains(msg, "inBuf"));
	CHECK(contains(msg, "槽 7"));  // 两侧都点名同一个槽号（同槽 = hazard 的成立条件）
	CHECK(contains(msg, "kComputeHazardEdges"));
	CHECK(contains(msg, "未申报的 compute hazard"));
}

TEST_CASE("假阳性守卫 ①：同槽两读者（都只读）→ 不报") {
	// 现场形态 = tableSlot（cull.cullTable 读 / stream_fanout.cullTable 读）与
	// objectMatrixSlot（cull / meshlet_cull 都只读）。两读不需要同步。
	const auto fresh = derive({
		{"cull.comp.slang", {{"cullTable", 11, false, true}}},
		{"stream_fanout.comp.slang", {{"cullTable", 11, false, true}}},
	});
	CHECK(fresh.empty());
}

TEST_CASE("假阳性守卫 ②：同槽但一侧是 none（非资源访问型）→ 不报") {
	// none = CB / push constant 那类参数（生成物里两个 bool 都 false）。判据必须**跳过**它，
	// 既不算读也不算写；否则每个 UBO 槽都会和别人的写槽撞出一条假 hazard。
	const auto fresh = derive({
		{"cull.comp.slang", {{"sceneHeader", 22, false, false}}},
		{"some_writer.comp.slang", {{"thing", 22, true, true}}},
	});
	CHECK(fresh.empty());
	// 反向的一半：none 侧换成真读者就必须报（证明上面那条红在"跳过"，不是"整对没比"）。
	const auto withReader = derive({
		{"cull.comp.slang", {{"sceneHeader", 22, false, true}}},
		{"some_writer.comp.slang", {{"thing", 22, true, true}}},
	});
	CHECK(withReader.size() == 1);
}

TEST_CASE("假阳性守卫 ③：同实例两个 param 落同一槽 → 不报（同实例自别名不是本层射程）") {
	// 判据 ①：instance 相同 ⟹ 不报。一个 dispatch 内部读写同一资源是 shader 自己的事，
	// barrier 帮不上；把它报出来 = 每次 dispatch 都抛，判据直接不可用。
	ComputeAccessLog log;
	const auto fresh = log.add("aliasing.comp.slang",
		std::vector<RgComputeBindingDecl>{
			{"src", 5, false, true}, {"dst", 5, true, false}});
	CHECK(fresh.empty());
	CHECK(log.entries().size() == 2);
	CHECK(log.instanceCount() == 1);
	// 再来一个实例碰同一槽：跨实例了，必须报（证明上面那条不是"这个槽根本没进判据"）。
	// 恰两条类：前一个实例的 src 与 dst **都**在槽 5，各自与这条新记录构成一类
	// （(aliasing.src, other.dst) 与 (aliasing.dst, other.dst)）——按类去重不会把它们并掉，
	// 因为 (shader, param) 无序对不同。
	const auto next = log.add("other.comp.slang",
		std::vector<RgComputeBindingDecl>{{"dst", 5, true, false}});
	CHECK(next.size() == 2);
}

TEST_CASE("假阳性守卫 ④：同槽两写者但已申报 → 对账通过（匹配是对称的）") {
	// izb_prime.head 若哪天变成 RW（变异 ③ 的形态），这一对仍是**同一条**已申报 hazard：
	// 方向变宽不改变"已覆盖"。同时这条钉住"匹配按无序对"——派生侧把**读侧先登记**，
	// 若实现只认 writer↔A 正向就查不到 ⟹ 红（假阳性抛）。
	const auto fresh = derive({
		{"izb_prime.comp.slang", {{"head", 42, true, true}}},
		{"izb_create.comp.slang", {{"head", 42, true, true}}},
	});
	REQUIRE(fresh.size() == 1);
	const auto edge = checkComputeHazard(fresh[0]);
	CHECK(edge.has_value());
	CHECK(*edge == RgSyncEdgeId::izbHeadCreateToPrime);
}

TEST_CASE("按类去重：downsample 链 N−1 级只派生一条 hazard 类") {
	// EVSSM 的 8 级链：mipSlots[L] 由第 L 级写、第 L+1 级读，相邻级各派生一次**同一类**。
	// 不去重 ⟹ 日志刷 6 行同样的话（而"5 行 hazard 日志"这条验收就是数行数）。
	ComputeAccessLog log;
	std::vector<RgComputeHazardClass> all;
	const auto append = [&](std::string_view shader, std::vector<RgComputeBindingDecl> decls) {
		const auto fresh = log.add(shader, decls);
		all.insert(all.end(), fresh.begin(), fresh.end());
	};
	append("evssm_moments.comp.slang", {{"moments", 100, true, true}});
	for (uint32_t level = 1; level < 8; ++level)
		append("evssm_downsample.comp.slang", {
			{"mipSrc", 100 + level - 1, true, true},
			{"mipDst", 100 + level, true, true}});
	// 新增类恰两条：(moments ↔ mipSrc) 与 (mipDst ↔ mipSrc)，各只出现一次。
	REQUIRE(all.size() == 2);
	CHECK(log.hazardClasses().size() == 2);
	CHECK(side(all[0].shaderA, all[0].paramA) == "evssm_downsample.comp.slang.mipSrc");
	CHECK(side(all[0].shaderB, all[0].paramB) == "evssm_moments.comp.slang.moments");
	CHECK(side(all[1].shaderA, all[1].paramA) == "evssm_downsample.comp.slang.mipDst");
	CHECK(side(all[1].shaderB, all[1].paramB) == "evssm_downsample.comp.slang.mipSrc");
	// 两条都归 evssm_moments_chain（同一条边覆盖矩链的两跳）。
	CHECK(checkComputeHazard(all[0]).has_value());
	CHECK(checkComputeHazard(all[1]).has_value());
}

TEST_CASE("🔴 双源对账：hazard 表每条声明的两个 (shader,param) 都在生成物里且方向对得上") {
	// 本文件最重要的一条。人工半边（表里写着"谁写谁读"）与机器半边（反射给的方向）在这里互锁：
	//   ① 两侧 (shader, param) 必须存在于 compute_bindings_generated（改名/删 binding ⟹ 红）
	//   ② writer 必须 writable、reader 必须 readable（把写侧改成只读 ⟹ 红）
	// 缺这条，整张表就退化成注释——而"看起来有护栏的注释"比没有更坏。
	const auto edges = computeHazardEdges();
	REQUIRE_FALSE(edges.empty());
	for (const auto& e : edges) {
		const std::string where = side(e.writerShader, e.writerParam)
			+ " ↔ " + side(e.readerShader, e.readerParam);
		const auto w = direction(e.writerShader, e.writerParam);
		INFO("writer 不在反射生成物里（shader 改名或删了 binding？）: " << where);
		REQUIRE(w.has_value());
		INFO("writer 在生成物里不是 writable（方向被改成只读了？）: " << where);
		CHECK(w->first);
		const auto r = direction(e.readerShader, e.readerParam);
		INFO("reader 不在反射生成物里: " << where);
		REQUIRE(r.has_value());
		INFO("reader 在生成物里不是 readable: " << where);
		CHECK(r->second);
		// 顺带钉住"这条 hazard 真的会被派生出来"：用同一个槽号手搭一次两侧，确认判据认它。
		const auto fresh = derive({
			{e.writerShader, {{e.writerParam, 1, w->first, w->second}}},
			{e.readerShader, {{e.readerParam, 1, r->first, r->second}}},
		});
		// 🔴 REQUIRE 不是 CHECK：下一句要下标 fresh[0]，这里若只 CHECK 就继续走 = 越界读 = SIGSEGV，
		//    整条测试网当场断掉（实测变异「写侧改只读」时把 474 个用例打断在 39 个）。红要红得干净。
		INFO("表里这条声明对应的访问对没派生成 hazard: " << where);
		REQUIRE(fresh.size() == 1);
		CHECK(checkComputeHazard(fresh[0]).has_value());
	}
}

TEST_CASE("数量锚：hazard 表恰好 5 条（钉「有人偷偷加一条带 reason 的豁免」）") {
	// 5 = 七个产品调用点与反射方向相交出的共享槽访问对全集（moments→mipSrc / mipDst→mipSrc /
	// izb head / izb nodes / cull sequences）。多一条 = 有人加了申报（要么真 hazard 要么豁免），
	// 少一条 = 有人删了申报（真 hazard 裸奔）。两种都必须在这里出声，而不是留给运行期。
	CHECK(computeHazardEdges().size() == 5);
	// 表里没有"不需要边"的豁免位：每条都必须指向一条真实存在的边、且两侧名字非空。
	for (const auto& e : computeHazardEdges()) {
		INFO(side(e.writerShader, e.writerParam) << " ↔ " << side(e.readerShader, e.readerParam));
		CHECK(e.edge != RgSyncEdgeId::count);
		CHECK_FALSE(syncEdgeDesc(e.edge).name.empty());
		CHECK_FALSE(e.writerShader.empty());
		CHECK_FALSE(e.writerParam.empty());
		CHECK_FALSE(e.readerShader.empty());
		CHECK_FALSE(e.readerParam.empty());
	}
}

TEST_CASE("全产品面复算：七个调用点派生出的 hazard 类集合 == hazard 表（双向）") {
	// ⚠ **调用点地图是手抄副本**（真身在 izb_shadow_impl / evssm_shadow_impl / gpu_cull_impl /
	// meshlet_cull_service_impl 四个文件）。它的价值 = 方向来自反射生成物（不是手抄），所以
	// shader 侧改方向会在这里翻脸；它**不**保证调用点本身不被改（那条限制别读丢）。
	// 槽号：跨模块互不相同（各模块各自 allocSlot / ensure* 领号），同模块共享的用同一个数。
	ComputeAccessLog log;
	std::vector<RgComputeHazardClass> derived;
	const auto add = [&](std::string_view shader, std::vector<RgComputeBindingDecl> decls) {
		const auto fresh = log.add(shader, decls);
		derived.insert(derived.end(), fresh.begin(), fresh.end());
	};
	// EVSSM（8 级矩链）：mip 槽 100..107，阴影图采样槽 200
	add("evssm_moments.comp.slang",
		{{"shadowDepth", 200, false, true}, {"moments", 100, true, true}});
	for (uint32_t level = 1; level < 8; ++level)
		add("evssm_downsample.comp.slang", {
			{"mipSrc", 100 + level - 1, true, true}, {"mipDst", 100 + level, true, true}});
	// IZB：head 300 / nodes 301 / primed 302 / depth 303
	add("izb_create.comp.slang", {
		{"head", 300, true, true}, {"nodes", 301, true, true}, {"depthTex", 303, false, true}});
	add("izb_prime.comp.slang", {
		{"head", 300, false, true}, {"nodes", 301, false, true}, {"primed", 302, true, true}});
	// cull：table 400 / sequences 401 / count 402 / objectMatrix 403
	add("cull.comp.slang", {
		{"cullTable", 400, false, true}, {"sequences", 401, true, true},
		{"countBuf", 402, true, true}, {"objectMatrixTable", 403, false, true}});
	// fanout：table 400 / sequences 401 / streams 404
	add("stream_fanout.comp.slang", {
		{"cullTable", 400, false, true}, {"sequences", 401, false, true},
		{"streams", 404, true, true}});
	// meshlet cull：items 500 / clusters 501 / clusterVisible 502 / objectMatrix 403
	add("meshlet_cull.comp.slang", {
		{"cullItems", 500, false, true}, {"clusters", 501, false, true},
		{"clusterVisible", 502, true, true}, {"objectMatrixTable", 403, false, true}});

	// 派生集合 ⊆ 表：每条派生出的 hazard 都能查到边（未申报 ⟹ 这里红并点名两侧）。
	for (const auto& h : derived) {
		INFO("未覆盖的 hazard: " << side(h.shaderA, h.paramA) << " ↔ "
			<< side(h.shaderB, h.paramB));
		CHECK(checkComputeHazard(h).has_value());
	}
	// 表 ⊆ 派生集合：表里每条声明都必须真被产品面派生出来（躺着一条产品面没有的声明
	// = 白申报，也会让"数量锚"变成唯一防线）。
	CHECK(derived.size() == computeHazardEdges().size());
	for (const auto& e : computeHazardEdges()) {
		bool found = false;
		for (const auto& h : derived) {
			const bool samePair = (e.writerShader == h.shaderA && e.readerShader == h.shaderB)
				|| (e.writerShader == h.shaderB && e.readerShader == h.shaderA);
			const bool sameParams = (e.writerParam == h.paramA && e.readerParam == h.paramB)
				|| (e.writerParam == h.paramB && e.readerParam == h.paramA);
			found = found || (samePair && sameParams);
		}
		INFO("hazard 表里这条声明在产品面派生不出来: "
			<< side(e.writerShader, e.writerParam) << " ↔ " << side(e.readerShader, e.readerParam));
		CHECK(found);
	}
}

}  // namespace RGL
