module;
#include <fmt/format.h>
module dynamic_render_graph:impl;

import dynamic_render_graph;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;
import render_graph;    // compileRenderGraph / bufferAttachmentBytes / RgGraphDesc
import timing_util;     // steadyNowNs（重编译耗时读数；rlbase 叶子，零设备依赖）
import offgraph_sync;   // B2-4④′：attachmentSyncEdges / checkAttachmentSyncEdge（对账的表侧）

namespace RGL {

namespace {

/** 统一错误构造：ErrorCode::RenderGraphInvalid + message。 */
Error dgError(std::string message) {
	return Error{ErrorCode::RenderGraphInvalid, std::move(message)};
}

/**
 * 附件在 desc 里的声明形态（对账两条规则共用的读法；顺带留首命中 pass 名做归因）。
 * 只认 storage 两档（buffer 附件的唯一合法引用方式，validatePassRef 双向规则保证不会
 * 有别的 usage 指向它）。
 */
struct AttachmentDecls {
	bool hasWrite = false;
	bool hasRead = false;
	std::string writePass;  ///< 首个 storageWrite 的 pass 名（报错点名用）
	std::string readPass;   ///< 首个 storageRead 的 pass 名
};

/**
 * 读一个附件在 desc 里的生产/消费声明形态。
 * @param desc 图描述
 * @param name 附件名
 * @return 两侧是否存在 + 首命中的 pass 名
 */
AttachmentDecls declsOf(const RgGraphDesc& desc, std::string_view name) {
	AttachmentDecls out;
	for (const auto& pass : desc.passes) {
		for (const auto& u : pass.outputs)
			if (u.attachment == name && u.usage == RgUsage::storageWrite && !out.hasWrite) {
				out.hasWrite = true;
				out.writePass = pass.name;
			}
		for (const auto& u : pass.inputs)
			if (u.attachment == name && u.usage == RgUsage::storageRead && !out.hasRead) {
				out.hasRead = true;
				out.readPass = pass.name;
			}
	}
	return out;
}

/**
 * 逗号连接附件名（报错归因用；两个 declare 的试编译失败消息共用）。
 * @param names 名字序列
 * @return "a,b,c" 形态
 */
std::string joinNames(const std::vector<std::string>& names) {
	std::string out;
	for (const auto& n : names)
		out += (out.empty() ? "" : ",") + n;
	return out;
}

/**
 * usage 的报错标签（补丁找不到目标 usage 时的归因文案）。
 *
 * 为什么穷尽 switch 不写 default：加一档 RgUsage 而忘了给标签 = 编译期报错
 * （`-Wcovered-switch-default` 已第一方启用；AGENTS 红线四「让错的东西编不过」）。
 */
std::string_view usageLabel(RgUsage usage) {
	switch (usage) {
	case RgUsage::colorWrite: return "colorWrite";
	case RgUsage::depthWrite: return "depthWrite";
	case RgUsage::sampleRead: return "sampleRead";
	case RgUsage::transferRead: return "transferRead";
	case RgUsage::transferWrite: return "transferWrite";
	case RgUsage::storageRead: return "storageRead";
	case RgUsage::storageWrite: return "storageWrite";
	}
	return "unknown";  // 不可达（穷尽），消 -Wreturn-type
}

/**
 * 正向规则（`reconcileAttachmentSyncEdges` 的前半边，拆函数是认知复杂度红线）。
 * @param desc 图描述
 * @return 空值 = 全部双声明附件都有表行；否则 Error（首条缺行，点名附件 + 两侧 pass + 表名）
 */
std::expected<void, Error> checkSharedAttachmentsHaveEdge(const RgGraphDesc& desc) {
	// 这条抓「共享肉眼可见地成立、同步却没人记账」——buffer 档对图 plan-only（零 barrier
	// 产物），图编译绿不代表同步存在。
	for (const auto& att : desc.attachments) {
		if (att.kind != RgAttachmentKind::buffer)
			continue;
		const auto decls = declsOf(desc, att.name);
		if (!decls.hasWrite || !decls.hasRead)
			continue;
		if (auto edge = checkAttachmentSyncEdge(att.name); !edge)
			return std::unexpected(dgError(fmt::format(
				"dynamic graph: 附件↔边对账失败——buffer 附件 '{}' 由 pass '{}' 生产、pass '{}' 消费"
				"（图内共享成立），但{}。真实同步不由图代发（plan-only），这笔账只在这张表里",
				att.name, decls.writePass, decls.readPass, edge.error().message)));
	}
	return {};
}

/**
 * 反向规则（`reconcileAttachmentSyncEdges` 的后半边）。
 * @param desc 图描述
 * @return 空值 = 表行在图里的投影都是完整双声明；否则 Error（首条半申报）
 */
std::expected<void, Error> checkEdgeRowsVisibleToGraph(const RgGraphDesc& desc) {
	// 表行指向的附件出现在 desc 里 ⟹ 两侧声明都必须存在（半申报 = 两个来源各说各话）。
	// 附件不在 desc 里 = 休眠（门关档 / 服务未装配），不报——IZB 的 producer-only + 无表行
	// 形态也不报（它没有表行，正向的双声明条件也不成立）。
	for (const auto& row : attachmentSyncEdges()) {
		const bool inDesc = std::ranges::any_of(desc.attachments,
			[&](const RgAttachmentDesc& a) { return a.name == row.attachment; });
		if (!inDesc)
			continue;
		const auto decls = declsOf(desc, row.attachment);
		if (decls.hasWrite && decls.hasRead)
			continue;
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: 附件↔边对账失败——表行为附件 '{}' 申报了「图内写×图内读」共享"
			"（边 {}），但图声明面{}（附件在 desc 里）。半申报 = 表与图各说各话："
			"要么补上声明（declareBufferProductions/Consumptions），要么删表行",
			row.attachment, syncEdgeDesc(row.edge).name,
			decls.hasWrite ? "缺 storageRead 消费者声明"
			               : decls.hasRead ? "缺 storageWrite 生产者声明"
			                               : "两侧声明都缺")));
	}
	return {};
}

}  // namespace

DynamicRenderGraph::DynamicRenderGraph(RgGraphDesc desc) : graphDesc(std::move(desc)) {
	// 启动期对账（B2-4④′）：初始 desc 里「双声明却缺边」的共享在**构造现场**抛——
	// 图从这里出去就是执行器的真相源，脏图每帧安全点重试刷日志是更坏的出声方式。
	if (auto r = reconcileAttachmentSyncEdges(graphDesc); !r)
		throw std::runtime_error(fmt::format(
			"DynamicRenderGraph: 初始图描述未通过附件↔边对账：{}", r.error().describe()));
}

std::expected<void, Error> DynamicRenderGraph::declareBufferProductions(std::string_view passName,
	std::vector<std::string> attachmentNames) {
	if (attachmentNames.empty())
		return {};
	RgGraphDesc trial = graphDesc;  // 试编译吃副本：失败不落账（变异现场 fail-fast）
	// ① 附件追加在**末尾**（slot 稳定红线）；已在图中的名字 = 幂等跳过。
	std::vector<std::string> added;
	for (const auto& name : attachmentNames) {
		const bool known = std::ranges::any_of(trial.attachments,
			[&](const RgAttachmentDesc& a) { return a.name == name; });
		if (known)
			continue;
		trial.attachments.push_back(RgAttachmentDesc{
			.name = name, .kind = RgAttachmentKind::buffer,
		});
		added.push_back(name);
	}
	if (added.empty())
		return {};  // 全部已声明 = 幂等空操作（不标脏、不重编译）
	// ② 目标 pass 追加 storageWrite 输出（生产者声明；与 B1a 锚点同形态：
	//    storageWrite 不占 colorWrite 名额、不产 barrier / beginRendering 条目）。
	const auto pass = std::ranges::find_if(trial.passes,
		[&](const RgPassDesc& p) { return p.name == passName; });
	if (pass == trial.passes.end())
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: declareBufferProductions 找不到生产者 pass '{}'（图里没有这个名字）",
			passName)));
	for (const auto& name : added) {
		const bool declared = std::ranges::any_of(pass->outputs,
			[&](const RgPassAttachment& u) { return u.attachment == name; });
		if (!declared)
			pass->outputs.push_back(
				RgPassAttachment{.attachment = name, .usage = RgUsage::storageWrite});
	}
	// ③ 试编译 fail-fast：注册缺失 / 0 字节 / pass 名错在**调用现场**炸，而不是把
	//    一个编译不过的 desc 落进真相源、让每帧安全点的重编译反复失败刷日志。
	if (auto compiled = compileRenderGraph(trial); !compiled)
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: declareBufferProductions 试编译失败（生产者 pass '{}' + 附件 {}）：{}",
			passName, joinNames(added), compiled.error().describe())));
	graphDesc = std::move(trial);
	markDirty();
	return {};
}

std::expected<void, Error> DynamicRenderGraph::declareBufferConsumptions(std::string_view passName,
	std::vector<std::string> attachmentNames) {
	if (attachmentNames.empty())
		return {};
	RgGraphDesc trial = graphDesc;  // 试编译吃副本：失败不落账（与生产者声明同口径）
	// ① 附件必须已在图中：生产者声明先行（读前置校验本就要求先写；这里把时序提到 API 面，
	//    错误直接指路，而不是让试编译报一句间接的"读前置"）。
	for (const auto& name : attachmentNames) {
		const bool known = std::ranges::any_of(trial.attachments,
			[&](const RgAttachmentDesc& a) { return a.name == name; });
		if (!known)
			return std::unexpected(dgError(fmt::format(
				"dynamic graph: declareBufferConsumptions 拒绝——附件 '{}' 还不在图里"
				"（消费者声明要求 declareBufferProductions 先行：读前置校验要求先写后读）",
				name)));
	}
	const auto pass = std::ranges::find_if(trial.passes,
		[&](const RgPassDesc& p) { return p.name == passName; });
	if (pass == trial.passes.end())
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: declareBufferConsumptions 找不到消费者 pass '{}'（图里没有这个名字）",
			passName)));
	// ② 目标 pass 追加 storageRead 输入（幂等：该 pass 已声明读同名附件 = 跳过）。
	std::vector<std::string> added;
	for (const auto& name : attachmentNames) {
		const bool declared = std::ranges::any_of(pass->inputs,
			[&](const RgPassAttachment& u) { return u.attachment == name; });
		if (declared)
			continue;
		pass->inputs.push_back(
			RgPassAttachment{.attachment = name, .usage = RgUsage::storageRead});
		added.push_back(name);
	}
	if (added.empty())
		return {};  // 全部已声明 = 幂等空操作（不标脏、不重编译）
	// ③ 试编译 fail-fast：同 pass 读写 feedback（countRole）/ 消费排在生产之前（读前置）
	//    在调用现场炸。storageRead 不产 barrier / beginRendering 条目 ⟹ 命令流零变化。
	if (auto compiled = compileRenderGraph(trial); !compiled)
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: declareBufferConsumptions 试编译失败（消费者 pass '{}' + 附件 {}）：{}",
			passName, joinNames(added), compiled.error().describe())));
	// ④ 双源对账（B2-4④′）：消费者声明是「成对形态完成」那一步——完成后的 desc 必须
	//    过图声明面 ↔ 附件↔边 表的对账，缺边在调用现场红（meshlet 调用方 throw = 启动期）。
	if (auto r = reconcileAttachmentSyncEdges(trial); !r)
		return r;
	graphDesc = std::move(trial);
	markDirty();
	return {};
}

namespace {

/**
 * 按名字找一条申报在 desc 里的插入位置（`insertBefore` 为空 = 图末）。
 *
 * 🔴 按名字而不是按下标：动态插拔会让下标漂移（`RgPassDeclaration` 头注释那条）。
 * @param trial 待查的 desc 副本
 * @param decl  申报单
 * @return 插入迭代器；`insertBefore` 指向的 pass 不在图里时 nullopt
 */
std::optional<std::vector<RgPassDesc>::iterator> insertPosOf(
	std::vector<RgPassDesc>& passes, const RgPassDeclaration& decl) {
	if (decl.insertBefore.empty())
		return passes.end();
	const auto it = std::ranges::find_if(passes,
		[&](const RgPassDesc& p) { return p.name == decl.insertBefore; });
	if (it == passes.end())
		return std::nullopt;
	return it;
}

/**
 * 按名字找一条补丁的目标 usage（补丁的「按名字定位」红线在这里落地）。const/非 const 两份，
 * 免得调用点为了写一个字段去 const_cast（那会把「这里在改图」这件事藏起来）。
 * @param passes 图里的 pass 序列
 * @param patch  补丁
 * @return 可空指针；找不到 = nullptr
 */
RgPassAttachment* findUsage(std::vector<RgPassDesc>& passes, const RgStoreOpPatch& patch) {
	for (auto& pass : passes) {
		if (pass.name != patch.passName)
			continue;
		for (auto& out : pass.outputs)
			if (out.attachment == patch.attachment && out.usage == patch.usage)
				return &out;
	}
	return nullptr;
}

/** 上面那条的只读版（`checkDeclaration` 拿它比对申报的补丁与图上的当前值）。 */
const RgPassAttachment* findUsage(const std::vector<RgPassDesc>& passes,
	const RgStoreOpPatch& patch) {
	for (const auto& pass : passes) {
		if (pass.name != patch.passName)
			continue;
		for (const auto& out : pass.outputs)
			if (out.attachment == patch.attachment && out.usage == patch.usage)
				return &out;
	}
	return nullptr;
}

/** storeOp 的报错标签（穷尽 switch 不写 default：加档忘给标签 = 编译期报错）。 */
std::string_view storeOpLabel(RgStoreOp storeOp) {
	switch (storeOp) {
	case RgStoreOp::store: return "store";
	case RgStoreOp::dontCare: return "dontCare";
	}
	return "unknown";  // 不可达（穷尽），消 -Wreturn-type
}

}  // namespace

std::expected<void, Error> DynamicRenderGraph::declarePass(RgPassDeclaration decl) {
	// ① 在册 = 幂等 / 指路判定（键 = pass 名）
	if (const auto* record = findRecord(decl.pass.name)) {
		if (record->inserted)
			return {};  // 幂等：已在图里（不标脏、不重编译）
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: declarePass 拒绝——pass '{}' 已在册但已撤回，重插请走 redeclarePass"
			"（它吃在册那份申报；这里若接受新 desc，「两边都拼错」也会被读成往返一致）",
			decl.pass.name)));
	}
	// ② 首次申报：名字不能被启动期 pass 占了（占了 = 图里两个同名 pass，而插入点与补丁都按
	//    名字定位 ⟹ 指错 pass 不报错）
	if (hasPass(decl.pass.name))
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: declarePass 拒绝——pass '{}' 已在图里但不是动态申报的"
			"（启动期内建/装饰 pass 要能撤，请在装配期调 adoptPass 登记，不是同名再插一个）",
			decl.pass.name)));
	if (auto r = checkDeclaration(decl, /*expectPresent =*/ false); !r)
		return r;
	DynamicPassRecord record { .decl = std::move(decl) };
	RgGraphDesc trial = graphDesc;
	if (auto r = applyRecordTo(trial, record); !r)
		return r;
	if (auto compiled = compileRenderGraph(trial); !compiled)
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: declarePass 试编译失败（pass '{}' 排在 '{}' 之前）：{}",
			record.decl.pass.name, record.decl.insertBefore, compiled.error().describe())));
	graphDesc = std::move(trial);
	record.inserted = true;
	dynamicPasses.push_back(std::move(record));
	markDirty();
	return {};
}

std::expected<void, Error> DynamicRenderGraph::redeclarePass(std::string_view passName) {
	auto* record = findRecord(passName);
	if (!record)
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: redeclarePass 拒绝——pass '{}' 不在动态申报册里"
			"（首次进图要走 declarePass 带申报单，这里只重插已在册的那一份）", passName)));
	if (record->inserted)
		return {};  // 幂等：已在场
	if (auto r = checkDeclaration(record->decl, /*expectPresent =*/ false); !r)
		return r;
	RgGraphDesc trial = graphDesc;
	if (auto r = applyRecordTo(trial, *record); !r)
		return r;
	if (auto compiled = compileRenderGraph(trial); !compiled)
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: redeclarePass 试编译失败（重插在册 pass '{}'）：{}",
			passName, compiled.error().describe())));
	graphDesc = std::move(trial);
	record->inserted = true;
	markDirty();
	return {};
}

std::expected<void, Error> DynamicRenderGraph::adoptPass(RgPassDeclaration decl) {
	if (findRecord(decl.pass.name))
		return {};  // 幂等：已登记过
	// expectPresent = true：申报的 pass 必须已在图里，且每条补丁必须**已经生效**
	// （启动期装饰已经把它落图了，登记只是承认它可撤）。
	if (auto r = checkDeclaration(decl, /*expectPresent =*/ true); !r)
		return r;
	dynamicPasses.push_back(DynamicPassRecord { .decl = std::move(decl), .inserted = true });
	return {};
}

std::expected<void, Error> DynamicRenderGraph::withdrawPass(std::string_view passName) {
	auto* record = findRecord(passName);
	if (!record)
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: withdrawPass 拒绝——pass '{}' 不在动态申报册里{}。"
			"启动期 pass 不接受动态撤（要能撤就在装配期 adoptPass 登记），拼错的名字也必须出声",
			passName,
			hasPass(passName) ? "（图里有这个名字 = 它是启动期内建/装饰 pass）"
			                  : "（图里也没有这个名字 = 拼错了）")));
	if (!record->inserted)
		return {};  // 幂等：已经撤过了（复合事务重放同一意图不该炸）
	RgGraphDesc trial = graphDesc;
	if (auto r = revertRecordFrom(trial, *record); !r)
		return r;
	if (auto compiled = compileRenderGraph(trial); !compiled)
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: withdrawPass 试编译失败（撤掉 pass '{}' 后的图编译不过）：{}",
			passName, compiled.error().describe())));
	graphDesc = std::move(trial);
	record->inserted = false;
	markDirty();
	return {};
}

bool DynamicRenderGraph::hasPass(std::string_view passName) const {
	return std::ranges::any_of(graphDesc.passes,
		[&](const RgPassDesc& p) { return p.name == passName; });
}

bool DynamicRenderGraph::isDynamic(std::string_view passName) const {
	return std::ranges::any_of(dynamicPasses,
		[&](const DynamicPassRecord& r) { return r.decl.pass.name == passName; });
}

std::vector<std::string> DynamicRenderGraph::dynamicPassNames() const {
	std::vector<std::string> names;
	names.reserve(dynamicPasses.size());
	for (const auto& record : dynamicPasses)
		names.push_back(record.decl.pass.name);
	return names;
}

DynamicRenderGraph::DynamicPassRecord* DynamicRenderGraph::findRecord(std::string_view passName) {
	const auto it = std::ranges::find_if(dynamicPasses,
		[&](const DynamicPassRecord& r) { return r.decl.pass.name == passName; });
	return it == dynamicPasses.end() ? nullptr : &*it;
}

std::expected<void, Error> DynamicRenderGraph::checkDeclaration(
	const RgPassDeclaration& decl, bool expectPresent) const {
	if (hasPass(decl.pass.name) != expectPresent)
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: 申报单与图不一致——pass '{}' {}（{} 口径要求相反）",
			decl.pass.name, hasPass(decl.pass.name) ? "在图里" : "不在图里",
			expectPresent ? "adoptPass" : "declarePass")));
	if (!decl.insertBefore.empty() && !hasPass(decl.insertBefore))
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: 申报单与图不一致——pass '{}' 要排在 '{}' 之前，而图里没有这个名字"
			"（插入点按名字定位：图被改过就重算申报，不要拿旧单子硬插）",
			decl.pass.name, decl.insertBefore)));
	for (const auto& patch : decl.patches) {
		const auto* usage = findUsage(graphDesc.passes, patch);
		if (!usage)
			return std::unexpected(dgError(fmt::format(
				"dynamic graph: 申报单的补丁找不到目标——pass '{}' 上没有 attachment '{}' 的 {} usage",
				patch.passName, patch.attachment, usageLabel(patch.usage))));
		const auto want = expectPresent ? patch.newStoreOp : patch.originalStoreOp;
		if (usage->storeOp != want)
			return std::unexpected(dgError(fmt::format(
				"dynamic graph: 申报单的补丁与图不一致——pass '{}' 的 {}.{} 现在是 {}，申报的是 {}"
				"（补丁声称的原值对不上 = 图在算申报与落申报之间被人改过，宁可拒落也不静默打错）",
				patch.passName, patch.attachment, usageLabel(patch.usage),
				storeOpLabel(usage->storeOp), storeOpLabel(want))));
	}
	return {};
}

std::expected<void, Error> DynamicRenderGraph::applyRecordTo(
	RgGraphDesc& trial, const DynamicPassRecord& record) const {
	const auto& decl = record.decl;
	const auto pos = insertPosOf(trial.passes, decl);
	if (!pos)
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: 插入失败——pass '{}' 要排在 '{}' 之前，而副本里找不到它"
			"（图在 checkDeclaration 之后被改过？这是内部不一致）",
			decl.pass.name, decl.insertBefore)));
	trial.passes.insert(*pos, decl.pass);
	for (const auto& patch : decl.patches) {
		auto* usage = findUsage(trial.passes, patch);
		if (!usage)
			return std::unexpected(dgError(fmt::format(
				"dynamic graph: 插入失败——补丁找不到目标 pass '{}' 的 attachment '{}'（{} usage）",
				patch.passName, patch.attachment, usageLabel(patch.usage))));
		usage->storeOp = patch.newStoreOp;
	}
	return {};
}

std::expected<void, Error> DynamicRenderGraph::revertRecordFrom(
	RgGraphDesc& trial, const DynamicPassRecord& record) const {
	const auto pass = std::ranges::find_if(trial.passes,
		[&](const RgPassDesc& p) { return p.name == record.decl.pass.name; });
	if (pass == trial.passes.end())
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: withdrawPass 内部不一致——在册 pass '{}' 在图里找不到"
			"（有人绕过 declare*/withdraw* 直接改了 desc？图与册子已经对不上）",
			record.decl.pass.name)));
	trial.passes.erase(pass);
	// 还原成**申报单上的 originalStoreOp**（不是常量 dontCare；理由见 RgStoreOpPatch）。
	// 目标 pass 已不在图里时跳过 = 别人撤了它，还原无意义（不是错）。
	for (const auto& patch : record.decl.patches) {
		auto* usage = findUsage(trial.passes, patch);
		if (!usage)
			continue;
		usage->storeOp = patch.originalStoreOp;
	}
	return {};
}

std::expected<void, Error> reconcileAttachmentSyncEdges(const RgGraphDesc& desc) {
	// 两条规则各拆一个 helper（认知复杂度红线；规则本体与已知限制的注释在接口声明处）。
	if (auto r = checkSharedAttachmentsHaveEdge(desc); !r)
		return r;
	return checkEdgeRowsVisibleToGraph(desc);
}

std::expected<CompiledRenderGraph, Error> DynamicRenderGraph::recompile() {
	// 重编译耗时从「安全点进来」量到「编译产物到手」（含双源对账 + 编译，不含调用方的
	// 逐 slot 结转与 RT 重装配——那部分成本在 Renderer 侧，口径写在这里是为了读数可复现）。
	const auto compileStartNs = steadyNowNs();
	if (!dirty)
		return std::unexpected(dgError(
			"dynamic graph: recompile 前没有脏位（调用方应先查 isDirty；这不是错误路径）"));
	// 安全点兜底对账（B2-4④′）：声明路径各有现场出声，这里兜住一切绕道进 desc 的形态
	// （半申报 / 双声明缺边）。失败 = 脏位保留（与编译失败同口径），安全点逐帧重试逐帧红。
	if (auto r = reconcileAttachmentSyncEdges(graphDesc); !r)
		return std::unexpected(std::move(r).error());  // expected<void> 转不成 expected<T>，搬 Error
	auto compiled = compileRenderGraph(graphDesc);
	if (!compiled)
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: 重编译失败（脏位保留，下帧安全点重试）：{}",
			compiled.error().describe())));
	const auto compileEndNs = steadyNowNs();
	++recompiles;
	dirty = false;
	lastRecompileDurationNs = compileEndNs > compileStartNs ? compileEndNs - compileStartNs : 0;
	// 机器可观测的日志行：逐 buffer 附件打印**注册字节**（判据 5 的观测面——
	// 「重编译吃到了新尺寸」必须能从日志里读出来，而不是靠人相信注册表）。
	// B2-5 起附带本次耗时（µs）：「逐帧 = 测试模态不是 SLA」这条裁决要有读数才裁决得动，
	// 口径 = 本函数内「对账 + 编译」的墙钟，`just smoke` 的台账按它算中位数。
	if (auto* logger = RLLogger::getInstance()) {
		std::string bytesDump;
		for (const auto& att : compiled->attachments)
			if (att.kind == RgAttachmentKind::buffer)
				bytesDump += fmt::format(" {}={}B", att.name,
					bufferAttachmentBytes(att.name).value_or(0));
		logger->info("DynamicGraph: recompiled (count={}, {:.1f}us) buffer attachments:{}",
			recompiles, static_cast<double>(lastRecompileDurationNs) / 1000.0,
			bytesDump.empty() ? " <none>" : bytesDump);
	}
	return compiled;
}

}  // namespace RGL
