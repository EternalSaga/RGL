module;
#include <fmt/format.h>
module dynamic_render_graph:impl;

import dynamic_render_graph;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;
import render_graph;    // compileRenderGraph / bufferAttachmentBytes / RgGraphDesc
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

std::expected<void, Error> reconcileAttachmentSyncEdges(const RgGraphDesc& desc) {
	// 两条规则各拆一个 helper（认知复杂度红线；规则本体与已知限制的注释在接口声明处）。
	if (auto r = checkSharedAttachmentsHaveEdge(desc); !r)
		return r;
	return checkEdgeRowsVisibleToGraph(desc);
}

std::expected<CompiledRenderGraph, Error> DynamicRenderGraph::recompile() {
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
	++recompiles;
	dirty = false;
	// 机器可观测的日志行：逐 buffer 附件打印**注册字节**（判据 5 的观测面——
	// 「重编译吃到了新尺寸」必须能从日志里读出来，而不是靠人相信注册表）。
	if (auto* logger = RLLogger::getInstance()) {
		std::string bytesDump;
		for (const auto& att : compiled->attachments)
			if (att.kind == RgAttachmentKind::buffer)
				bytesDump += fmt::format(" {}={}B", att.name,
					bufferAttachmentBytes(att.name).value_or(0));
		logger->info("DynamicGraph: recompiled (count={}) buffer attachments:{}",
			recompiles, bytesDump.empty() ? " <none>" : bytesDump);
	}
	return compiled;
}

}  // namespace RGL
