module;
#include <nlohmann/json.hpp>  // 协议 JSON（第三方头，不受 GMF STL 白名单约束）

export module debug_rpc_decorate;

import rlstd;
import render_graph;           // CompiledRenderGraph（快照名单的唯一来源 = 编译产物）
import rpc_schema_generated;   // rpc_value::decorate::*（feature / op 词表 = schema 单源）
import debug_rpc;              // RpcResponder（跨帧兑现的句柄，/decorate 与 /shot 同形态）
import debug_rpc_payload;      // RpcOutcome / rpcOk / rpcFail / rpcReject（应答形状闸）
import diagnostics;            // diagnostics().picking（highlight / picking 两个特征的门）
import runtime_config;         // RuntimeConfig（aa / velocityField / ssr.enabled 三枚门）
import vk_renderer;            // Renderer（requestPassToggle 既有事务入口 + 图事实读取）

namespace RGL {

/**
 * `POST /decorate` 动态能力探针的**纯逻辑**（`docs/能力观测_plan.md` §3.6 / §3.7，批 4）。
 *
 * ## 这个端点是什么：一次「尝试 + 报告停在哪」，不是「做成」
 *
 * 请求 `{feature, op}`（op ∈ add/del）完全合法 ⟹ 回包**恒 200**，内容是一次尝试的**结果事实**
 * （done / refusedAt / reason / 尝试前后的能力快照差异）。拒绝不是错误——「渲染能力不得用错误
 * 回包表示」是用户 2026-10-19 定的原则，本模块是它的正面执行（与批 3 的 404 分工见计划 §3.6
 * 那张表：点名不存在的东西 = 引用解析失败走 404；发起变更尝试 = 成与不成都是运行期事实走 200）。
 *
 * ## 🔴 refusedAt 词表：每一档都必须有真实生产者
 *
 * 没有生产者的档位 = 装饰性枚举值（「没有消费者的字段不进 schema」要禁的正是这个形态）。
 * 计划 §3.6 原表五档，本批实机勘察后删掉三档，理由逐条写在文件尾部的「删档记录」。判据：
 * `tests/test_debug_rpc_decorate.cppm` 的**词表覆盖用例**——把全部可构造的分类场景跑一遍，
 * 产出的 refusedAt 集合必须与 `kRefusedVocabulary` 逐条相等（加一档没有场景产它 = 红）。
 *
 * ## 🔴 本模块不碰设备、不碰图：分类吃注入的事实，改图走 Renderer 既有事务
 *
 * 分类（classifyDecorate）只读调用方采集的事实（在册 / 在场 / 门），改图本身仍由
 * `Renderer::requestPassToggle` → 安全点复合事务执行（申报 ≠ 发射权：探针不新开一条写图路径）。
 * 快照与差异只吃 `CompiledRenderGraph`（与批 1 能力段同一来源），本模块自己零第二份记录。
 */

/**
 * `refusedAt` 词表之一：该装饰**没被登记成可撤 / 没有运行期申报单**。
 * 生产者 = 本模块分类函数对 `DynamicRenderGraph::isDynamic` 为假的判定，它镜像
 * `DynamicRenderGraph::withdrawPass` 对不在册名字的明确报错（dynamic_render_graph_impl.cppm
 * 的「启动期 pass 不接受动态撤…拼错的名字也要明确报错」分支）——探针先查在册表再决定要不要
 * 发起事务，所以实际报错点在这条分类里，而 withdrawPass 那条错是同一事实的兜底半边。
 * 实机可达：`del taa / velocity / ao_apply / ssr / picking / shadow` 全部落在这档。
 */
export inline constexpr std::string_view kRefusedNotAdopted = "not-adopted";

/**
 * `refusedAt` 词表之一：配置门本身关着（与「图形态不支持」是两件事，分开才归得了因）。
 * 生产者 = 本模块分类函数对 `DecorateFeatureDesc::gate` 求值（门的事实来源逐条 =
 * `diagnostics()` / `RuntimeConfig`，映射本体 = 本文件的 decorateGateOpen）。
 * 启动不变量（债-129 S3-C：门开而装饰没落图 = 拒启动）⟹ 运行期「不在图」蕴含「门是关的」，
 * 这一档抓的就是这个事实。实机可达：出厂档 `add taa / velocity / ao_apply / ssr`。
 */
export inline constexpr std::string_view kRefusedGateOff = "gate-off";

/**
 * `refusedAt` 词表之一：试编译拒了（`withdrawPass` / `redeclarePass` 各有试编译，这是最硬的
 * 一道正确性闸门；它响说明意图本身不合法，不是能力不足）。
 * 生产者 = `Renderer::applyPassToggles` 复合事务的失败回包（PassToggleOutcome.success = false）。
 * 对**已在册** pass，事务剩下的失败模式就是试编译（不在册的错已被 not-adopted 提前分流）；
 * 图外申报失败也会落这一档，但 reason 带事务原文，归因不靠猜。
 * ⚠ 默认矩阵没有一格走到这里（合法意图的试编译都该过——这本身就是读数）；它的可达性由
 * 单测注入失败事务结果钉住（tests/test_debug_rpc_decorate.cppm 用例「事务定案」+ 词表覆盖）。
 */
export inline constexpr std::string_view kRefusedTrialCompile = "trial-compile";

/**
 * 词表全量（判据 = 词表覆盖用例：每个可构造场景产出的档 ∈ 本表，且本表每档都有场景产它）。
 * 加一档 = 必须同时加一个真实生产者与一条产出场景，否则本用例红。
 */
export inline constexpr std::array<std::string_view, 3> kRefusedVocabulary = {
 	kRefusedNotAdopted, kRefusedGateOff, kRefusedTrialCompile,
};

/**
 * 特征的配置门是哪一枚（穷尽 switch 映射到 bool 的位置 = 本文件的 decorateGateOpen——
 * 加一档不写映射 = 编译不过，不写 default）。
 */
export enum class DecorateGateKind {
	picking,    ///< diagnostics.picking（highlight 与 picking 两个特征同门：装配共用一条门）
	aaTaa,      ///< runtime.lua `aa == "taa"`（RuntimeConfig::aaWantsTaa）
	velocity,   ///< runtime.lua velocityField（由 aa == "fsr" 派生，不再是独立键）
	aaFsr,      ///< runtime.lua `aa == "fsr"`（ao_apply 的门 = FSR 门，见 decorateAoApplyOrThrow）
	ssrEnabled, ///< runtime.lua ssr.enabled
	none,       ///< 无门：启动期无条件装饰（shadow 就是这一档）
};

/** 一个可装饰特征：feature 名 → 它在图里的 pass 名单 + 它的配置门。 */
export struct DecorateFeatureDesc {
	std::string_view name;                 ///< = schema 的 feature oneOf 值（吃生成常量）
	std::vector<std::string_view> passes;  ///< 执行序方向的 pass 名单（del 逆序撤、add 正序插）
	DecorateGateKind gate = DecorateGateKind::none;
};

/**
 * 特征表（feature 名吃 `rpc_value::decorate::feature::*` 生成常量 ⟹ schema 改名当场编译不过）。
 *
 * 🔴 **为什么是这七个而不是计划原稿的四个 `*Decorated`**（用户已同意放宽）：四个 `*Decorated`
 * 特征（taa / velocity / ao_apply / ssr）今天**全部没走 adoptPass 登记**，矩阵里它们的 del 格
 * 是同一堵墙（not-adopted）重复四次，读数没有信息量；而 highlight / picking / shadow 这些
 * 启动期装饰里，highlight 是唯一有运行期往返能力的（阳性对照靠它），picking / shadow 补上
 * 「装饰 ≠ 四个」的覆盖面。放宽后矩阵才分得出「同一堵墙」与「不同的墙」。
 *
 * pass 名单出处 = 各装饰器落图的名字（render_graph_decorate.cppm：highlight / picking /
 * shadow(+shadow1..3) / taa+taa_commit / velocity / ao_apply / ssr_trace+ssr）。
 * shadow 列全部级联（kActiveShadowCascades = 4）：特征「在不在图」= 全部级联都在，
 * 只列 shadow 一级会把「其余三级没了」读成在场。
 */
export inline const std::vector<DecorateFeatureDesc>& decorateFeatures() {
	static const std::vector<DecorateFeatureDesc> features = {
		{ .name = rpc_value::decorate::feature::highlight, .passes = { "highlight" }, .gate = DecorateGateKind::picking },
		{ .name = rpc_value::decorate::feature::picking, .passes = { "picking" }, .gate = DecorateGateKind::picking },
		{ .name = rpc_value::decorate::feature::shadow,
		  .passes = { "shadow", "shadow1", "shadow2", "shadow3" }, .gate = DecorateGateKind::none },
		{ .name = rpc_value::decorate::feature::taa, .passes = { "taa", "taa_commit" }, .gate = DecorateGateKind::aaTaa },
		{ .name = rpc_value::decorate::feature::velocity, .passes = { "velocity" }, .gate = DecorateGateKind::velocity },
		{ .name = rpc_value::decorate::feature::ao_apply, .passes = { "ao_apply" }, .gate = DecorateGateKind::aaFsr },
		{ .name = rpc_value::decorate::feature::ssr, .passes = { "ssr_trace", "ssr" }, .gate = DecorateGateKind::ssrEnabled },
	};
	return features;
}

/** @return 命中的特征描述；nullptr = 表里没有（处理函数据此报 500，oneOf 已拦正常拼错） */
export inline const DecorateFeatureDesc* findDecorateFeature(std::string_view name) {
	for (const auto& f : decorateFeatures())
		if (f.name == name)
			return &f;
	return nullptr;
}

/** 一个 pass 的现状事实（调用点从 DynamicRenderGraph 现取：声明侧 = 事务真正移动的那一侧）。 */
export struct DecoratePassFacts {
	std::string_view passName;
	bool present = false;     ///< hasPass（desc 声明侧）
	bool registered = false;  ///< isDynamic（曾在册：adoptPass / declarePass 过，含已撤回）
};

/** 一个特征的现状事实（分类函数的全部输入）。 */
export struct DecorateFacts {
	bool gateOpen = false;                        ///< 配置门（来源见 DecorateGateKind 注释）
	std::vector<DecoratePassFacts> passes;        ///< 顺序 = 特征表的 pass 顺序
};

/** 分类的下一步：直接回包（respond），还是发起一次真实事务（attempt）。 */
export enum class DecorateAction { respond, attempt };

/** 一次尝试的判定结果（respond = 结论已定；attempt = 交给 Renderer 事务，settle 时定案）。 */
export struct DecorateDecision {
	DecorateAction action = DecorateAction::respond;
	bool done = false;
	std::string_view refusedAt;  ///< 词表内取值；done = true 时为空
	std::string reason;          ///< 一句人话（失败必点名事实来源）
	std::vector<std::string_view> attemptPasses;  ///< attempt：要逐帧移动的 pass（顺序即执行序）
	bool attemptOn = true;                        ///< attempt 的意图（requestPassToggle 的 on）
};

/** 拼「不在册 pass 名单」用的 reason（点名到具体 pass，读者不用回去数表）。 */
inline std::string nameList(std::string_view sep, const std::vector<std::string_view>& names) {
	std::string out;
	for (size_t i = 0; i < names.size(); ++i) {
		if (i)
			out += sep;
		out += names[i];
	}
	return out;
}

/** del 方向的分类（决策顺序 = 在册先于一切；见 classifyDecorate 头注）。 */
inline DecorateDecision classifyDel(const DecorateFacts& facts,
	const std::vector<std::string_view>& notPresent,
	const std::vector<std::string_view>& notRegistered) {
	DecorateDecision d;
	if (!notRegistered.empty()) {
		d.done = false;
		d.refusedAt = kRefusedNotAdopted;
		d.reason = "pass '" + nameList("'、'", notRegistered)
			+ "' 不在动态在册表（装配期没人 adoptPass 登记它可撤；withdrawPass 对不在册名字"
			  "本就明确报错）——这是引擎动态能力的现状事实，不是请求错误";
		return d;
	}
	if (notPresent.size() == facts.passes.size()) {
		d.done = true;
		d.reason = "全部 pass 已在册且已离场（幂等：withdrawPass 对已撤回的名字是空操作）";
		return d;
	}
	d.action = DecorateAction::attempt;
	d.attemptOn = false;
	// 逆序撤（先 taa_commit 后 taa）：正序撤会让后一个 pass 的试编译因「前驱已没」拒掉，
	// 把「撤得掉」误报成 trial-compile。今天只有单 pass 特征可达 attempt，顺序是防御。
	for (auto it = facts.passes.rbegin(); it != facts.passes.rend(); ++it)
		if (it->present)
			d.attemptPasses.push_back(it->passName);
	return d;
}

/** add 方向的分类（决策顺序 = 在场 > 门 > 在册；见 classifyDecorate 头注）。 */
inline DecorateDecision classifyAdd(std::string_view feature, const DecorateFacts& facts,
	const std::vector<std::string_view>& notPresent,
	const std::vector<std::string_view>& notRegistered) {
	DecorateDecision d;
	if (notPresent.empty()) {
		d.done = true;
		d.reason = "全部 pass 已在图里（幂等：意图已满足，本次不改图，diff 为空是诚实读数）";
		return d;
	}
	if (!facts.gateOpen) {
		d.done = false;
		d.refusedAt = kRefusedGateOff;
		d.reason = "配置门关着（门 = 创建期量，改它要重启；启动不变量：门开而装饰没落图 = 拒启动，"
		           "所以运行期「不在图」就等价于「门是关的」）。且该特征没有运行期申报单——"
		           "装饰只在启动期 compileGraphOrThrow 跑过一次，没人把它接进运行期（没实现 ≠ 做不到）";
		return d;
	}
	if (notRegistered.empty()) {
		d.action = DecorateAction::attempt;
		d.attemptOn = true;
		for (const auto& p : facts.passes)
			if (!p.present)
				d.attemptPasses.push_back(p.passName);
		return d;
	}
	// 防御分支（今天的引擎不可达：门开 ⟹ 启动期必已落图 ⟹ 上面 notPresent 空已返回；
	// 能撤的只有 highlight，它必在册）。走到这里 = 引擎形态变了，照实报「没有申报单可插」。
	d.done = false;
	d.refusedAt = kRefusedNotAdopted;
	d.reason = "门开而不在图、且不在册 = 没人给特征 '" + std::string(feature)
		+ "' 接过运行期申报单（当前引擎不可达此分支，走到即新形态）";
	return d;
}

/**
 * 分类一次尝试（纯函数；判据 = tests/test_debug_rpc_decorate.cppm）。
 *
 * 决策顺序是读数口径的一部分（写进 schema 的 refusedAt.note）：
 *  - `del`：先看**在册**（不在册 = 事务必然拒，直接报 not-adopted，不发事务）；在册但全部
 *    已离场 = 幂等成功（withdrawPass 对「在册已撤回」就是空操作返回成功，探针不假装它失败）。
 *  - `add`：先看**在场**（幂等成功——「让 X 在图里」的意图已满足，diff 为空是诚实读数）；
 *    再看门（门关 = gate-off，这里点名：门是创建期量，改它要重启）；再看在册（在册 = 发事务）；
 *    门开 + 不在场 + 不在册 = 今天不可达（启动不变量 ⟹ 门开必已落图），防御分支报
 *    not-adopted 并在 reason 里说清「没人给它接过运行期申报单」——这是**探针的兜底**，
 *    不是引擎的评论。
 * @param feature 特征名（已过 oneOf）
 * @param op      add | del（已过 oneOf）
 * @param facts   现状事实（调用点现取，本函数不持任何状态）
 */
export DecorateDecision classifyDecorate(std::string_view feature, std::string_view op,
	const DecorateFacts& facts) {
	std::vector<std::string_view> notPresent, notRegistered;
	for (const auto& p : facts.passes) {
		if (!p.present)
			notPresent.push_back(p.passName);
		if (!p.registered)
			notRegistered.push_back(p.passName);
	}
	if (op == rpc_value::decorate::op::del)
		return classifyDel(facts, notPresent, notRegistered);
	return classifyAdd(feature, facts, notPresent, notRegistered);
}

/**
 * 事务结果 → 定案（settle 侧；attempt 之后的 done / trial-compile 由它给）。
 * @param success          复合事务是否成功（PassToggleOutcome.success）
 * @param transactionError 失败时的引擎原文（不解析、整段进 reason——归因靠原文，不靠猜）
 */
export DecorateDecision decorateVerdictFromTransaction(bool success,
	std::string_view transactionError) {
	DecorateDecision d;
	if (success) {
		d.done = true;
		d.reason = "复合事务完成（安全点重编译已落图，快照差异见 diff）";
		return d;
	}
	d.done = false;
	d.refusedAt = kRefusedTrialCompile;
	d.reason = std::string("事务失败（对已在册 pass，事务剩下的失败模式 = 试编译拒；原文如下）：")
		+ std::string(transactionError);
	return d;
}

/**
 * 能力快照的**名单投影**（尝试前后各取一次；来源 = 编译产物，与批 1 能力段同一口径——
 * 计划 §5 批 4 条：探针要靠能力段确认「尝试前后图变成什么样」，否则 done 无从验证）。
 * 只取名字：diff 回答的是「哪些附件 / pass 出现或消失」，尺寸格式那些变了也不改名单，
 * 要细粒度读数去读两次完整 `/state.capabilities`。
 */
export struct GraphNameSnapshot {
	std::vector<std::string> attachments;
	std::vector<std::string> passes;
};

/** @return 编译产物的名单快照（附件按 slot 序、pass 按执行序） */
export inline GraphNameSnapshot captureGraphNameSnapshot(const CompiledRenderGraph& compiled) {
	GraphNameSnapshot snap;
	snap.attachments.reserve(compiled.attachments.size());
	for (const auto& att : compiled.attachments)
		snap.attachments.push_back(att.name);
	snap.passes.reserve(compiled.passes.size());
	for (const auto& pass : compiled.passes)
		snap.passes.push_back(pass.name);
	return snap;
}

/** 名单差集（保序：按 after/before 里的出现序），diff 的四个方向共用这一把尺。 */
inline std::vector<std::string> nameDifference(const std::vector<std::string>& from,
	const std::vector<std::string>& notIn) {
	std::vector<std::string> out;
	for (const auto& name : from)
		if (std::find(notIn.begin(), notIn.end(), name) == notIn.end())
			out.push_back(name);
	return out;
}

/**
 * 尝试前后的快照差异（键集 = `rgl/rpc_schema.lua` 的 `decorateDiff`）。
 * 「出现」= 尝试后名单里有、尝试前没有；「消失」反之。两侧都空 = 这次尝试没动图
 * （幂等格 / 被拒格的诚实读数，不是缺数据）。
 */
export nlohmann::json decorateDiffJson(const GraphNameSnapshot& before,
	const GraphNameSnapshot& after) {
	auto toArr = [](const std::vector<std::string>& v) {
		nlohmann::json a = nlohmann::json::array();
		for (const auto& s : v)
			a.push_back(s);
		return a;
	};
	return nlohmann::json{
		{"appearedPasses", toArr(nameDifference(after.passes, before.passes))},
		{"disappearedPasses", toArr(nameDifference(before.passes, after.passes))},
		{"appearedAttachments", toArr(nameDifference(after.attachments, before.attachments))},
		{"disappearedAttachments", toArr(nameDifference(before.attachments, after.attachments))},
	};
}

/**
 * 成功回包拼装（恒 200 的那条路；键集 = schema 的 decorate 端点 response.fields）。
 * `refusedAt` **只在 done = false 时出现**（done = true 没有失败环节可报——出键会把
 * 「没失败」与「失败在空字符串那一档」塌成同值，批 2 硬规则 1 的同一个病）。
 */
export nlohmann::json decorateResponseJson(std::string_view feature, std::string_view op,
	uint32_t frame, const DecorateDecision& d, const nlohmann::json& diff) {
	nlohmann::json out{
		{"ok", true},
		{"feature", std::string(feature)},
		{"op", std::string(op)},
		{"done", d.done},
		{"reason", d.reason},
		{"diff", diff},
		{"frame", frame},
	};
	if (!d.done)
		out["refusedAt"] = std::string(d.refusedAt);
	return out;
}

/**
 * `/decorate` 的在途状态（能力观测批 4；由 RpcWiring 持有一份）。跨帧兑现的理由与 /shot
 * 相同：改图在安全点的复合事务里执行，收单在帧边界，结果要下一帧帧边界才读得到。
 */
export struct DecorateSession {
	std::optional<RpcResponder> pending;  ///< 在途探针（自身互斥；与 /shot 的槽独立，可并存）
	std::string feature;                  ///< 回显：请求 feature
	std::string op;                       ///< 回显：请求 op
	std::vector<std::string> queue;       ///< 待移动的 pass（每帧过一笔事务，pop 一个）
	bool wantOn = true;                   ///< 意图（requestPassToggle 的 on）
	uint32_t frame = 0;                   ///< 收单时刻帧计数（回包回显）
	GraphNameSnapshot before;             ///< 收单时刻能力快照（编译产物口径）
};

/**
 * 特征的配置门求值（探针读的是**现值**——门是创建期量，runtime.lua 注释自陈；热重载改了
 * 配置而图没变 = 这里读到配置现值，该分歧不在本批判据范围，要复现读数就固定配置重启）。
 * 穷尽 switch 不写 default：加一档 DecorateGateKind 不写映射 = 编译不过。
 * @return 门是否开着
 */
export bool decorateGateOpen(const RuntimeConfig& runtime, DecorateGateKind kind) {
	switch (kind) {
	case DecorateGateKind::picking: return diagnostics().picking;
	case DecorateGateKind::aaTaa: return runtime.aaWantsTaa();
	case DecorateGateKind::velocity: return runtime.velocityField;
	case DecorateGateKind::aaFsr: return runtime.aaWantsFsr();
	case DecorateGateKind::ssrEnabled: return runtime.ssrEnabled;
	case DecorateGateKind::none: return true;  // shadow：启动期**无条件**装饰（没有门可查）
	}
	return false;  // 不可达（穷尽）：新档漏分支已被 -Wswitch 拦在编译期，这里保守拒
}

/**
 * POST /decorate（能力观测批 4，计划 §3.6）：动态能力探针。渲染线程帧边界执行（applier 内）。
 *
 * 回包**恒 200**（拒绝不是错误——用户原则「渲染能力不得用错误回包表示」的正面执行）：
 * 请求合法、引擎**试了**、成没成都归档。本函数只做三件事：
 *  ① 会话状态核对（探针在途 / 单意图槽被占 = 409，协议层，与 /shot 的抢槽 409 同族——
 *    两个意图挤一格必然丢一个，丢了还回 200 就是「看起来生效了其实没生效」）；
 *  ② 采集事实（在册 / 在场 = 声明侧，事务真正移动的那一侧；门 = 配置现值）；
 *  ③ 分类 → 直接回包，或发起 Renderer 既有复合事务并跨帧持有句柄（rpcSettleDecorate 定案）。
 */
export void rpcHandleDecorate(Renderer& renderer, const RuntimeConfig& runtime,
	uint32_t frameNow, DecorateSession& s, const nlohmann::json& body, RpcResponder responder) {
	const std::string feature = body.at(rpc_field::decorate::feature).get<std::string>();
	const std::string op = body.at(rpc_field::decorate::op).get<std::string>();
	const auto* desc = findDecorateFeature(feature);
	if (!desc) {
		// oneOf 已拦拼错，走到这里 = schema 加了 feature 而特征表没跟上（引擎自己的洞，
		// 响亮报 500 让矩阵跑不掉；常驻判据 = rules_rpc 面 7 的 feature ↔ 特征表对账）。
		rpcReject(responder, 500,
			"decorate: feature '" + feature + "' 在 schema 里但不在特征表（debug_rpc_decorate.cppm）");
		return;
	}
	if (s.pending) {
		rpcReject(responder, 409, "decorate: 上一发 /decorate 还在途（一帧一个意图）");
		return;
	}
	if (renderer.hasPendingPassToggle()) {
		rpcReject(responder, 409,
			"decorate: 单意图槽已被占用（/pass 或本帧更早的请求收单未过安全点）");
		return;
	}
	DecorateFacts facts { .gateOpen = decorateGateOpen(runtime, desc->gate) };
	for (const auto passName : desc->passes)
		facts.passes.push_back(DecoratePassFacts {
			.passName = passName,
			.present = renderer.dynamicGraphRef().hasPass(passName),
			.registered = renderer.dynamicGraphRef().isDynamic(passName) });
	auto decision = classifyDecorate(feature, op, facts);
	if (decision.action == DecorateAction::respond || decision.attemptPasses.empty()) {
		// 没发起事务：前后快照是同一次 = diff 四数组全空（幂等 / 被拒格的诚实读数）。
		// attemptPasses 为空但 action=attempt 是分类函数的防御死角，同样按无事务回包。
		const auto snap = captureGraphNameSnapshot(renderer.getRenderGraphRef());
		responder.resolve(200, rpcOk(decorateResponseJson(feature, op, frameNow, decision,
			decorateDiffJson(snap, snap))).body.dump());
		return;
	}
	// 发起事务：先取走陈旧结果格（上一笔 /pass 事务写的），结算时读到的才只可能是本探针的。
	renderer.takePassToggleOutcome();
	s.feature = feature;
	s.op = op;
	s.queue.clear();
	for (const auto p : decision.attemptPasses)
		s.queue.push_back(std::string(p));
	s.wantOn = decision.attemptOn;
	s.frame = frameNow;
	s.before = captureGraphNameSnapshot(renderer.getRenderGraphRef());
	renderer.requestPassToggle(s.queue.front(), s.wantOn);
	s.pending = std::move(responder);  // 句柄按值存下（同 /shot：resolve 幂等，兜异常路径）
}

/**
 * /decorate 跨帧结算（帧边界、drain 之前；与 rpcSettleShot 同位置同理：本钩子先于
 * drawFrame ⟹ 读到的结果格来自**上一帧**的安全点事务，快照是重编译后的新图）。
 *
 * 三种情形：
 *  - 结果格是本次的（passName 核对过）：成功 → 队列还有下一个 pass 就继续逐帧移动
 *    （多 pass 特征，今天不可达的防御），否则定案 done=true；失败 → trial-compile + 原文。
 *  - 结果格缺席 / 是别人的，而意图已被并发请求带到位 → done=true（幂等语义，不假装失败）。
 *  - 结果格缺席 / 是别人的，意图也没到 → **409**：意图被单槽覆盖 = 会话状态冲突，
 *    不是能力事实，不进 refusedAt 词表（词表每一档都必须有真实生产者，这里事务根本没跑，
 *    生产者不存在——硬归档就是编造读数）。
 */
export void rpcSettleDecorate(Renderer& renderer, DecorateSession& s) {
	if (!s.pending)
		return;
	auto respond = [&](const RpcOutcome& outcome) {
		auto responder = std::move(*s.pending);
		s.pending.reset();
		s.queue.clear();
		responder.resolve(outcome.status, outcome.body.dump());
	};
	auto respondOk = [&](const DecorateDecision& d) {
		const auto diff = decorateDiffJson(s.before,
			captureGraphNameSnapshot(renderer.getRenderGraphRef()));
		respond(rpcOk(decorateResponseJson(s.feature, s.op, s.frame, d, diff)));
	};
	const std::string front = s.queue.empty() ? std::string {} : s.queue.front();
	const auto outcome = renderer.takePassToggleOutcome();
	if (!front.empty() && outcome && outcome->passName == front) {
		if (!outcome->success) {
			respondOk(decorateVerdictFromTransaction(false, outcome->error));
			return;
		}
		s.queue.erase(s.queue.begin());
		if (!s.queue.empty()) {
			// 多 pass 特征：下一笔事务下一帧过（队列逐帧 pop，今天只有单 pass 的 highlight 可达）
			renderer.requestPassToggle(s.queue.front(), s.wantOn);
			return;
		}
		respondOk(decorateVerdictFromTransaction(true, {}));
		return;
	}
	const bool satisfied = std::ranges::all_of(s.queue, [&](const std::string& p) {
		return renderer.dynamicGraphRef().hasPass(p) == s.wantOn;
	});
	if (satisfied) {
		DecorateDecision d;
		d.done = true;
		d.reason = "意图已由并发请求带到位（本探针未发起自己的事务，diff 如实反映）";
		respondOk(d);
		return;
	}
	respond(rpcFail(409,
		"decorate: 意图被单意图槽覆盖（并发 /pass）或事务未跑——重试；这不是能力读数，不进词表"));
}

/*
 * ── 删档记录（计划 §3.6 原表五档 → 本批三档；每条删除都给了「为什么没有生产者」）──────
 *
 * 1. `attachment-append-only` **删**。附件撤不掉是**真约束**（FrameCapturer / PickingService
 *    构造期缓存 slot），但它**不是探针的拒绝点**：`withdrawPass` 的既有语义就是「附件不跟着撤、
 *    留孤儿 RT」并成功返回——探针从不尝试撤附件，所以没有任何调用会死在「附件撤不掉」上。
 *    留这一档 = 造一个永远不会被产出的枚举值。append-only 这个事实的观测面在 diff：
 *    `del highlight` 成功时 disappearedPasses 有 highlight、disappearedAttachments 恒空。
 * 2. `rt-alloc` **删**，且这是一条**推翻计划预期的新发现**：RT 领建**不是**运行期的墙——
 *    `Renderer::ensureOffscreenAttachments` 每帧遍历**当前编译图**的附件逐个 ensure，
 *    附件经重编译进图后下一帧 RT 自动建好（能力段 rtBuilt 变 true）。计划 §3.6 把 add 的预期
 *    死点写成「rt-alloc 或 entry-wiring」，实测前者根本不成立。
 * 3. `entry-wiring` **删**（作为 refusedAt 档）。装配条目 / 色输入优先级链只在启动时跑一次是
 *    **真实现状**，但探针**够不到它**：add 方向在「没有运行期申报单」那一步就停了（装饰器是
 *    desc 改写函数、不产 RgPassDeclaration，只有 highlight 的 planHighlightDecoration 产），
 *    探针不实现 add 就不该假装量到了装配关。这一档的事实写进 gate-off 的 reason 与台账说明，
 *    不占词表。
 */

}  // namespace RGL
