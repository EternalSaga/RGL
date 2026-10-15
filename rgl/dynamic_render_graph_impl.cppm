module;
#include <fmt/format.h>
module dynamic_render_graph:impl;

import dynamic_render_graph;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;
import render_graph;  // compileRenderGraph / bufferAttachmentBytes / RgGraphDesc

namespace RGL {

namespace {

/** 统一错误构造：ErrorCode::RenderGraphInvalid + message。 */
Error dgError(std::string message) {
	return Error{ErrorCode::RenderGraphInvalid, std::move(message)};
}

}  // namespace

DynamicRenderGraph::DynamicRenderGraph(RgGraphDesc desc) : graphDesc(std::move(desc)) {}

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
	if (auto compiled = compileRenderGraph(trial); !compiled) {
		std::string names;
		for (const auto& n : added)
			names += (names.empty() ? "" : ",") + n;
		return std::unexpected(dgError(fmt::format(
			"dynamic graph: declareBufferProductions 试编译失败（生产者 pass '{}' + 附件 {}）：{}",
			passName, names, compiled.error().describe())));
	}
	graphDesc = std::move(trial);
	markDirty();
	return {};
}

std::expected<CompiledRenderGraph, Error> DynamicRenderGraph::recompile() {
	if (!dirty)
		return std::unexpected(dgError(
			"dynamic graph: recompile 前没有脏位（调用方应先查 isDirty；这不是错误路径）"));
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
